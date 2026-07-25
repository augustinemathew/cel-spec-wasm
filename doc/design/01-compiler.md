# Compiler design — the pass pipeline

How a CEL string becomes wasm. System context is [`00-architecture.md`](00-architecture.md); the byte-level wire format is [`03-abi-and-memory.md`](03-abi-and-memory.md).

## 1. The shape of the thing

`Compiler::Compile` takes a CEL string and produces a `.wasm` module plus a `cel.abi` descriptor. Two ideas organize the pipeline:

**The cel-cpp AST *is* the IR.** We parse and type-check with cel-cpp, get back its `cel::Ast`, and walk that tree the rest of the way. No optimizer, no SSA, no basic blocks — a typed tree.

**Every derived fact lives in a side-table, not the tree.** The AST belongs to cel-cpp, so every fact a later pass needs goes into one `flat_hash_map<node_id, NodeAnnotation>`. A pass always makes the same move: read the AST plus earlier annotations, write more annotations. The tree is mutated exactly twice — the two constant folds of §3.2 — and is otherwise read-only from parse to codegen.

![The compile→eval pipeline](../img/pipeline-light.svg#only-light)
![The compile→eval pipeline](../img/pipeline-dark.svg#only-dark)

| Stage | What it adds | In one sentence |
|---|---|---|
| **Frontend** (§3) | a typed `cel::Ast` | parse + type-check with cel-cpp, then reject anything we can't compile |
| **Annotations** (§4) | `repr`, `field_number` | stamp each node with its wire kind |
| **ResolvePass** (§5) | names, scopes, overloads | turn identifiers into indices and pick the runtime helper for each call |
| **LayoutPass** (§6) | a memory home per value | give every CelValue a byte offset in linear memory |
| **Lowering** (§7) | the wasm | walk the annotated tree, emit Binaryen IR for `$eval` |
| **Finalize** (§8) | bytes + `cel.abi` | bootstrap the module for the chosen link mode, validate, serialize |

`Compiler::Compile` (`compiler/compiler.h`) maps public options onto an internal `CompileOptions` and calls the facade `celwasm::Compile` (`compiler/internal/compile.cc`), which dispatches on `link_mode` ([09 §2](09-lowering.md#2-finalization-and-link-modes)) and runs the chain. Exact per-pass contracts: [Appendix A](#appendix-a-pass-contracts).

## 2. Why the passes run in this order

One rule generates the ordering:

> A pass may only read facts that an earlier pass has already written.

ResolvePass needs every node typed, so it follows the checker. LayoutPass needs the interned variables, so it follows ResolvePass. Lowering needs a memory address for every value, so it runs last. The other constraint: the two AST rewrites (§3.2) must precede anything that reads the tree's final shape.

## 3. Frontend — parse, check, and the static-subset gate

`compiler/frontend/parse_and_check.{h,cc}`. Entry point: `ParseAndCheck(expression, CheckOptions) -> absl::StatusOr<TypedAst>`.

### 3.1 Driving cel-cpp

The checker is built once with the standard library plus supported extensions: ComprehensionsV2, strings, encoders, math, optionals, and hand-built `net.IP` / `net.CIDR` decls. Per call we add the embedder's `container`, variable declarations, and custom-function decls. Message types resolve against the process-wide descriptor pool; an embedder schema *overlays* it (merges over, never replaces).

A failed check returns `InvalidArgument` with a machine-readable payload (`status_tags.h`). Consumers branch on the payload, never the message text — error strings are not a stable API.

### 3.2 Two constant folds (the only tree mutations)

cel-cpp hands us two shapes as *identifiers* that are really *constants*. We fold them up front, idempotently:

1. **Enum constants** — an ident whose reference resolves to a value becomes a `kConstant`.
2. **Type literals** — an ident naming a *type* (checker type `type(T)`) becomes a `kConstant` carrying the type name. Must run after (1): it keys off "the reference has no value."

After this, codegen never sees an identifier that is secretly a constant.

### 3.3 RejectDyn — the gate

cel-wasm compiles a *static* subset of CEL: no `dyn` runtime kind, no heterogeneous containers, no runtime type errors-as-values. A node is rejected if it is untyped or its type is `dyn`, `error`, a function, a type parameter, or unset — recursing through list element and map key/value types, so implicit dyn is caught too (a bare `[]`, a heterogeneous `[1, "two"]`, an `optional<dyn>`).

Five carve-outs admit an otherwise-dyn node whose lowering is known:

1. **`dyn(x)` passthrough** — `dyn` is the identity function; a primitive/null/type (or nested `dyn`) argument is admitted and lowered directly (§7.1).
2. **Select-through-Any** — a `dyn`-typed select whose operand is a `google.protobuf.Any` (the Any unpacks at runtime).
3. **`math.@min` / `math.@max`** — the macro expansions produce a dyn-typed result over a mixed-numeric list; the result is admitted, the macro-built list arg skipped.
4. **`x.format([...])`** — a list *literal* argument to `format` (the renderer dispatches per element); a `list<dyn>` *variable* still rejects.
5. **`cel.bind` shape** — the macro's degenerate empty-range loop is admitted without checking its unreachable dyn-typed parts.

Rejections accumulate: every violation is reported at once, tagged with the offending node ids. This section, not the public header's one-line summary, is the authoritative carve-out list.

## 4. Annotations — the side-table

`compiler/ir/typed_ast.{h,cc}` + `annotations.h`.

`TypedAst` is thin: a `unique_ptr<cel::Ast>`, the annotations map, and the variable list. Downstream passes read cel-cpp's `type_map` and `reference_map` alongside our annotations. The map is **one fact table, one entry per node** — a field is either meaningful for that node's kind or ignored:

| Field | Written by | Meaning |
|---|---|---|
| `repr` | frontend | the node's wire kind, derived from its checker type (`bool`, `int`, `string`, `message`, `optional`, …) |
| `field_number` | frontend | proto field number for a select; 0 = resolve by name |
| `overload_id` | ResolvePass | cel-cpp's resolved overload string for a call (e.g. `add_int64`) |
| `local_index` | ResolvePass | which wasm local an identifier maps to |
| `scope_id` | ResolvePass | comprehension nesting depth for a scope-bound ident; 0 = free |
| `attribute_id` | ResolvePass | interned attribute path, for partial evaluation |
| `message_type_id` | ResolvePass | index of a struct literal's type in `cel.abi` |
| `storage` | LayoutPass | the value's home: `{kind, payload}` — rodata offset, local index, or scratch slot |
| `map_origin` / `list_origin` | ResolvePass | where an aggregate came from — drives the three-path dispatch (§7.3) |
| comprehension locals | Resolve/Layout | per-comprehension iter/accu bindings, by node id (cel-cpp reuses the name `@result` at every depth, so names are useless here) |

**`repr` is the spine**: it carries the frontend's type knowledge to codegen, and almost every downstream dispatch (§7) switches on it.

## 5. ResolvePass — names, scopes, overloads

`compiler/codegen/resolve_pass.{h,cc}`. Eight visitors in a fixed order; only the last is order-sensitive. Two matter most:

**Identifier resolution and comprehension scope.** Every identifier is interned into a dense `variables` table (first occurrence wins its `local_index`). Comprehension subtlety: `iter_range` and `accu_init` resolve in the *outer* scope; `loop_condition` and `loop_step` run in an inner frame binding the iteration variable(s) and accumulator; `result` sees only the accumulator. cel-cpp names every accumulator `@result` regardless of depth, so bindings are stamped *by node id, never by name* — the only way nested comprehensions stay distinct.

**Origin inference.** `MapOriginVisitor` / `ListOriginVisitor` tag each aggregate: a literal `[...]` / `{...}` is `kArena`, a map/list free variable or select is `kHost`, everything else stays `kDynamic`. The safety property: **a conservative tag is never a miscompile** — `kDynamic` routes through a runtime kind-check, so a too-cautious origin costs a branch, not correctness.

The remaining visitors audit constant reprs, intern attribute paths and struct-literal type names, copy overload ids off cel-cpp's reference map, and forward `dyn(x)`'s annotations onto the call node. Output: `ResolveOutput` — annotations, the dense variable table (whose size is the number of i32 locals `$eval` declares), the intern tables, and the max comprehension depth.

## 6. LayoutPass — a memory home for every value

`compiler/codegen/layout_pass.{h,cc}`. `LayoutPass(TypedAst, ResolveOutput, …) -> absl::StatusOr<StaticLayout>` — layout can fail, and that's a feature (§6.4). Every CelValue the expression touches — constants, variables, intermediates — gets a byte offset in linear memory.

### 6.1 The memory map, and the line you must not cross

![Linear memory](diagrams/memory-map-light.svg#only-light)
![Linear memory](diagrams/memory-map-dark.svg#only-dark)

The expression module and the runtime kernel share one linear memory. The runtime's own world — wasi-libc statics, its stack, the dlmalloc heap holding the per-Instance arena — is pinned above byte 262144 (`--global-base=262144`, `runtime/BUILD.bazel`). Below is the window the expression owns: a null sentinel, rodata, workspace scratch slots, a guard band.

**The first 256 KiB is the only memory the expression may write.** There is no hardware fence at that boundary: a stray write silently corrupts libc state and traps later, elsewhere, with an inscrutable message. That failure mode is why the §6.4 gates exist. `CELWASM_RESERVED_LOW_MEMORY_BYTES` (`runtime/cel_layout.h`) is the single source of truth, tied to the compiler's `memory_layout.h` by `static_assert` — a drifted constant fails the build.

### 6.2 Five sub-passes

Two kinds of home: **rodata** (constants, packed at compile time) and **workspace** (scratch cells, reused at runtime).

- **A — rodata.** Pack every constant as 24-byte CelValue frames at absolute offsets (no relocation math in the emitted wasm). Aggregates are not packed here — they're built at eval time in the arena.
- **B — variable slots.** One 32-byte cell per referenced variable.
- **C — variable storage.** Stamp each identifier `{kLocal, index}`; the `$eval` prelude loads each local with its slot's offset.
- **D — scratch slots.** Hand each result-producing node a workspace cell via the `SlotAllocator` (§6.3).
- **E — comprehension locals.** Reserve the auxiliary wasm locals per comprehension (iteration cursor, end pointer, index counter).

### 6.3 The SlotAllocator — why a chain doesn't blow the budget

Naïvely every intermediate needs its own cell, so an N-term chain would march workspace past the 8 KiB line. The fix is a **LIFO free list**: a node `Acquire`s a cell and `Release`s it once every reader is done. Helpers read operand slots *before* writing their result slot, so a parent can reuse a just-released operand cell. A left-associative chain peaks at ~**one** live cell; a balanced tree peaks at its depth. (Pinned: a 2000-term chain peaks at one slot and both compiles and evaluates.)

Two load-bearing details:

- **Cells are 32 bytes though a CelValue is 24.** The runtime's helpers use atomic memory ops that trap on misalignment; a 24-byte stride from a 16-aligned base lands every other cell 8-aligned. The extra 8 bytes are alignment padding.
- **Aggregates pin their slot across their subtree.** A list/map literal writes its parent cell first (`cel_list_create`), then fills element by element — the parent's cell must not alias any descendant's. Aggregates acquire on the way down and release on the way up; scalars do the opposite. The `slot_aliasing_test` battery keeps this discipline honest.

### 6.4 The gates — fail at compile, not at eval

Crossing byte 8192 is silent and catastrophic, so LayoutPass refuses to emit a layout that would:

1. **Slot-exhaustion gate.** Workspace past the window (minus rodata, minus the guard band) → `ResourceExhausted` with a remediation hint. The guard band ensures the gate trips on the *next* allocation before anything spills.
2. **`ValidateExprStaticRegion`.** Whole-region check in *both* link modes: rodata + workspace past 8192 → reject. Dynamic mode is covered too — there the expression imports the runtime's memory, and oversized rodata corrupts it at instantiate time. A *status*, never a `CHECK`: embedder input must never crash the process.
3. **A static-mode install tripwire.** `CHECK(rodata_end ≤ 8192)` where the rodata segment attaches — reaching it means an upstream gate regressed.

The eval side adds a matching Plan-time check, so a Program whose ABI claims an out-of-window slot is rejected before any write. The exact cliffs (`x in [0..N]` fits at N=327, overflows at 328) move if the stride or framing changes — the tests are the pin, not these numbers.

## Lowering & link modes (moved)

The per-node lowering arms (kCall ladder, kSelect, aggregates, ternary, comprehensions, the OverloadTable) and the two link-mode bootstraps are [`09-lowering.md`](09-lowering.md).

## 7. Public surface and options

A `Compiler` is built through `Compiler::Builder` (consumes itself on `Build()`): declare variables, add function libraries, add `.celfn` source. Validation is deferred to `Build()` so every problem surfaces at once. One `Compiler` mints many `Program`s; a `Program` is pure bytes.

| Knob | What it actually does |
|---|---|
| `container` | Forwarded to cel-cpp's name-resolution container. Checker-only; no codegen effect. |
| `optimize_level` (default 0) | Gates Binaryen optimization. Level 0 is a byte-identical no-op. Negative levels behave as 0; only `> 3` is rejected. Binaryen's optimizer is process-global state — serialize concurrent Compiles when `> 0`. |
| `link_mode` (default static) | Picks the bootstrap ([09 §2](09-lowering.md#2-finalization-and-link-modes)). |

Internal-only knobs (export names, validate/serialize toggles) stay on the internal `CompileOptions`.

## 8. Rejected alternatives

Recorded so they aren't re-proposed without new evidence:

- **Sethi–Ullman / Strahler slot pre-assignment** — the LIFO free list (§6.3) gets the same peak-≈-depth result more simply.
- **A `MessagePattern` table for proto literals** — shipped shape is type-id interning + empty-then-populate calls.
- **Always-host dispatch / always-materialise** — both lost to the three-path origin split ([09 §1.3](09-lowering.md#13-three-path-aggregate-dispatch)).
- **Explicit branching for `&&` / `||`** — eager slot-out + kernel 3VL absorption is spec-equivalent and simpler ([09 §1.1](09-lowering.md#11-the-kcall-dispatch-ladder)).
- **Resolve-time cross-numeric overload pick** — non-viable; cel-cpp's reference map carries no candidate list, so the cross-numeric id is synthesized at codegen from operand reprs.
- **Interned uint32 overload ids** — replaced by borrowed `string_view`s.
- **Conditional runtime inlining / "don't inline the runtime"** — static-by-default won on measurement; the old reasoning still governs the dynamic mode.
- **Rodata caps / dedup / runtime-initialized literals** — deliberate non-features; the budget lives at the §6.4 gates.
- **AST-gated ("lazy") imports** — the full runtime surface installs regardless of AST shape ([09 §2](09-lowering.md#2-finalization-and-link-modes)).

## 9. Known gaps and future work

**Pinned bugs (open):**

- An ERROR accumulator trips `exists`'s early-exit peephole (it reads the bool payload without a kind check), so `[0,2].exists(x, 2/x == 1)` returns the division error instead of `true` (`KnownBugs.ExistsAbsorbsErrorAccumulator`). Fix: kind check ahead of the payload probe.
- A `transformMapEntry` whose entry isn't a map *literal* hits `ABSL_CHECK(false)` and aborts the compiler (`KnownBugs.TransformMapEntryComputedEntryCrash`) — should be a status error.

**Planned:**

- Tie the hand-copied CelValue wire constants in codegen to `runtime/cel_data.h` (today a layout change compiles green and fails only at e2e).
- Grow origin inference (`kCall → kHost`, comprehension-fold → arena) — measure before building.
- A relocatable / growable static region, lifting the rodata-bound ceilings (the `in`-list cliff at ~327 ints).
- The `@native` library-module fork — its producer (`CompileLibraryBodies` / `rodata_base_override`) is declared but unbuilt; it gets a producer or gets deleted ([`05-custom-functions.md`](05-custom-functions.md)).

Unverified questions (repr edge cases, option-contract pins, the dyn-cond ternary behavior, short-circuit `orValue`, single-walk Pass-D) are catalogued in [`design/notes/`](https://github.com/augustinemathew/cel-wasm/tree/master/doc/design/notes).

---

## Appendix A — pass contracts

| Pass | Consumes | Produces | Invariant established | Breaks if reordered |
|---|---|---|---|---|
| Parse + check | source, `CheckOptions` | checked `cel::Ast` (`type_map`, `reference_map`) | every node typed; calls carry overload ids | everything downstream reads `type_map` |
| AST rewrites (×2) | checked AST | enum/type-literal idents → `kConstant` | no kIdent for a resolved constant survives | rewrite 2 keys off "Reference has no value" — must follow rewrite 1 |
| RejectDyn | rewritten AST | pass/fail | survivors are in the static subset (5 carve-outs) | must precede annotation so no dyn repr survives |
| Annotations | AST + pool | `repr`, `field_number` → `TypedAst` | every node has a repr | ResolvePass CHECKs `repr != kUnknown` on idents |
| ResolvePass | `TypedAst` | `ResolveOutput` | locals/scopes/attributes/types/origins/overloads populated | LayoutPass derefs `local_index`; lowering derefs `overload_id` |
| LayoutPass | `TypedAst` + `ResolveOutput` | `StatusOr<StaticLayout>` | every value has a memory home; region fits the window | lowering CHECKs `storage.kind` |
| `ValidateExprStaticRegion` | `StaticLayout` | pass/fail | region end ≤ 8192, both modes | static install CHECK assumes it ran |
| Module bootstrap | link mode | `WasmModule` (imports installed / runtime adopted) | every call target resolves | lowering needs imports installed first |
| `LowerExportAndFinalise` | all of the above | bytes + `cel.abi` | one shared tail; validate→optimize→serialize | optimizing an unvalidated module mutates unproven IR |
