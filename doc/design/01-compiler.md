# Compiler design — the pass pipeline

Status: current — authored 2026-06-10, rewritten for clarity 2026-06-11.
System context (the four roles, the link-mode fork, threading) is
[`00-architecture.md`](00-architecture.md); the byte-level wire format
is [`03-abi-and-memory.md`](03-abi-and-memory.md). This doc is the
compiler itself: how a CEL string becomes wasm.

## 1. The shape of the thing

The compiler takes a CEL string and produces a `.wasm` module plus a
`cel.abi` descriptor. That's it — one function, `Compiler::Compile`,
in and bytes out.

Two ideas make the whole pipeline easy to hold in your head:

**The cel-cpp AST *is* our IR.** We don't lower CEL into some custom
intermediate form. We parse and type-check with cel-cpp, get back its
`cel::Ast`, and walk *that* tree the rest of the way. There is no
optimizer, no SSA, no basic blocks — just a typed tree.

**Everything we learn lives in a side-table, not the tree.** The AST
belongs to cel-cpp; we can't bolt fields onto its nodes. So every fact
a later pass needs — "this node's value is a string," "this variable
is wasm local 3," "this result lives at byte offset 240" — gets written
into one `flat_hash_map<node_id, NodeAnnotation>`. A pass is therefore
always the same move: *read the AST plus what earlier passes annotated,
write more annotations.* The tree is mutated exactly twice, and only to
fold two kinds of constant (§3); otherwise it is read-only from parse
to codegen.

Hold those two ideas and the pipeline is just a sequence of annotators
followed by an emitter:

![The compile→eval pipeline](diagrams/pipeline.svg)

| Stage | What it adds | In one sentence |
|---|---|---|
| **Frontend** (§3) | a typed `cel::Ast` | parse + type-check with cel-cpp, then reject anything we can't compile |
| **Annotations** (§4) | `repr`, `field_number` | stamp each node with its wire kind |
| **ResolvePass** (§5) | names, scopes, overloads | turn identifiers into indices and pick the runtime helper for each call |
| **LayoutPass** (§6) | a memory home per value | give every CelValue a byte offset in linear memory |
| **Lowering** (§7) | the wasm | walk the annotated tree, emit Binaryen IR for `$eval` |
| **Finalize** (§8) | bytes + `cel.abi` | bootstrap the module for the chosen link mode, validate, serialize |

`Compiler::Compile` (`compiler/compiler.h`) maps the public options onto
an internal `CompileOptions` and calls the facade `celwasm::Compile`
(`compiler/internal/compile.cc`), which dispatches on `link_mode` (§8)
and runs the chain. The deep contract — exactly what each pass consumes,
produces, and would break if reordered — is the reference table in
[Appendix A](#appendix-a--pass-contracts); the prose below is the way in.

## 2. The one invariant that explains the order

Most of the pass ordering follows from a single rule:

> A pass may only read facts that an earlier pass has already written.

ResolvePass needs every node typed, so it runs after the checker.
LayoutPass needs to know which nodes hold values, so it runs after
ResolvePass interns the variables. Lowering needs a memory address for
every value, so it runs last. The "breaks if reordered" column in
Appendix A is just this rule applied case by case — you rarely need to
memorize it; you can re-derive it from "what does this pass read?"

The other ordering constraint is the two AST rewrites (§3.2): they must
run *before* anything that reads the tree's final shape, because they
are the only steps that change it.

## 3. Frontend — parse, check, and the static-subset gate

`compiler/frontend/parse_and_check.{h,cc}`. Entry point:
`ParseAndCheck(expression, CheckOptions) -> absl::StatusOr<TypedAst>`.

We don't reimplement CEL. cel-cpp parses and type-checks; our job is to
drive it, fold two constant cases, and then **refuse anything outside
the static subset** — the slice of CEL we can compile to wasm.

### 3.1 Driving cel-cpp

The checker is built once with the standard library plus the extensions
we support: ComprehensionsV2, strings, encoders, math, optionals, and
hand-built `net.IP` / `net.CIDR` decls. Per call we add the embedder's
`container`, variable declarations, and any custom-function decls.
Message types resolve against the process-wide descriptor pool; an
embedder schema *overlays* it (merges over, never replaces).

When the check fails, the error is an `InvalidArgument` carrying a
machine-readable payload (`status_tags.h`). Consumers branch on the
payload, never on the message text — error strings are not a stable API.

### 3.2 Two constant folds (the only tree mutations)

cel-cpp hands us two shapes as *identifiers* that are really
*constants*, and carrying them as idents would force every downstream
pass to special-case them. So we fold them up front, idempotently:

1. **Enum constants** — an ident whose reference resolves to a value
   (e.g. an enum member) becomes a `kConstant`.
2. **Type literals** — an ident that names a *type* (its checker type
   is `type(T)`) becomes a `kConstant` carrying the type name. This must
   run *after* (1), because it keys off "the reference has no value."

After this, codegen never sees an identifier that's secretly a
constant. These are the only two places the AST changes.

### 3.3 RejectDyn — the gate

cel-wasm compiles a *static* subset of CEL. There is no `dyn` runtime
kind in our value representation, no heterogeneous container, no
runtime type errors-as-values. So we reject, at compile time, anything
that would need them.

A node is rejected if it's untyped or its type is `dyn`, `error`, a
function, a type parameter, or unset — and this recurses through list
element types and map key/value types. That's the important part:
implicit dyn is caught too. A bare `[]`, a heterogeneous `[1, "two"]`,
an `optional<dyn>` — all rejected, not just an explicit `dyn(...)`.

There are **five carve-outs** — narrow, shape-matched exceptions where
an otherwise-dyn node is admitted because we know exactly how to lower
it:

1. **`dyn(x)` passthrough.** `dyn` is the identity function — there's
   no runtime conversion. If its argument is a primitive/null/type (or
   another `dyn`), we admit it and lower the argument directly (§7.1).
2. **Select-through-Any.** A `dyn`-typed select whose operand is a
   `google.protobuf.Any` is admitted (the Any unpacks at runtime).
3. **`math.@min` / `math.@max`.** The macro expansions produce a
   dyn-typed result over a mixed-numeric list; we admit the result and
   skip the macro-built list arg.
4. **`x.format([...])`.** A list *literal* argument to `format` is
   admitted (the renderer dispatches per element); a `list<dyn>`
   *variable* still rejects.
5. **`cel.bind` shape.** The macro's degenerate empty-range loop is
   admitted without checking its unreachable dyn-typed parts.

Rejections accumulate — you get every violation at once, tagged and
with the offending node ids, not just the first. (The public header's
one-line gate summary omits the carve-outs; this section is the
authoritative list.)

## 4. Annotations — the side-table

`compiler/ir/typed_ast.{h,cc}` + `annotations.h`.

`TypedAst` is deliberately thin: a `unique_ptr<cel::Ast>`, the
annotations map, and the variable list. There's no heavier IR because
there doesn't need to be — downstream passes read cel-cpp's `type_map`
and `reference_map` *alongside* our annotations.

The annotations map is **one fact table, one entry per node**. No
parallel tables, no sentinels for "not applicable" — a field is either
meaningful for that node or ignored. Here's the schema and, crucially,
*who fills each field in*:

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

The frontend stamps only the first two fields; everything else is
filled by the codegen-side passes. The single most useful thing to
notice: **`repr` is the spine.** It's how the frontend's knowledge of a
node's *type* reaches codegen's decision about how to *lower* it, and
almost every dispatch downstream (§7) is a switch on `repr`.

## 5. ResolvePass — names, scopes, overloads

`compiler/codegen/resolve_pass.{h,cc}`.

The checker gave us types; ResolvePass gives us everything else codegen
needs that *isn't* a memory address: it turns identifiers into dense
indices, works out comprehension scoping, interns the tables the ABI
will carry, and records which runtime helper each call resolves to.

It's eight visitors in a fixed order. Only the last one is
order-sensitive; the rest are independent. The two worth understanding
in depth:

**Identifier resolution and comprehension scope.** Every identifier is
interned into a dense `variables` table (first occurrence wins its
`local_index`). The subtlety is comprehensions: `iter_range` and
`accu_init` resolve in the *outer* scope, while `loop_condition` and
`loop_step` run in an inner frame that binds the iteration variable(s)
and the accumulator, and `result` sees only the accumulator. Because
cel-cpp names every accumulator `@result` regardless of nesting depth,
these bindings are stamped *by node id, never by name* — that's the
only way nested comprehensions stay distinct.

**Origin inference.** `MapOriginVisitor` / `ListOriginVisitor` tag each
aggregate with where it came from: a literal `[...]` / `{...}` is
`kArena` (built in our arena), a map/list-typed free variable or select
is `kHost` (lives in the embedder's memory), and everything else stays
`kDynamic`. This tag is what lets lowering pick a fast path later (§7.3).
The key safety property: **a missing or conservative tag is never a
miscompile.** `kDynamic` is the correct-but-slower default — it routes
through a runtime kind-check — so the worst a too-cautious origin does
is cost a branch.

The remaining visitors audit constant reprs, intern attribute paths for
partial eval, intern struct-literal type names, copy overload ids off
cel-cpp's reference map, and forward `dyn(x)`'s annotations onto the
call node. The output is `ResolveOutput`: the annotations, the dense
variable table (whose size is the number of i32 locals `$eval`
declares), the intern tables, and the max comprehension depth.

## 6. LayoutPass — a home in memory for every value

`compiler/codegen/layout_pass.{h,cc}` and friends.
`LayoutPass(TypedAst, ResolveOutput, …) -> absl::StatusOr<StaticLayout>`
— note the `StatusOr`: **layout can fail**, and that's a feature (§6.4).

wasm has one flat array of bytes. Every CelValue the expression touches
— constants, variables, intermediate results — needs a byte offset in
it. LayoutPass assigns those offsets. To understand it you first need
the map of the memory it's assigning into.

### 6.1 The memory map, and the line you must not cross

![Linear memory](diagrams/memory-map.svg)

The expression module and the runtime kernel **share one linear
memory**. The runtime's own world — wasi-libc's static data, its stack,
the dlmalloc heap (which holds the per-Instance arena) — is pinned
*above* byte 8192, because the runtime is linked with
`--global-base=8192`. Below that line is a small window the expression
owns: a null sentinel, then read-only constant data (rodata), then
workspace scratch slots, then a guard band.

The thing to internalize: **the first 8 KiB is the only memory the
expression may write.** There is no hardware fence at byte 8192 — a
stray write just past it silently corrupts libc state, and the process
doesn't trap *there*; it traps minutes later inside some unrelated
helper with an inscrutable message. That failure mode is exactly why
LayoutPass can fail and why the gates in §6.4 exist. (`memory_layout.h`
is the single source of truth for these constants, cross-checked
against the runtime's header by `static_assert` — a drifted constant
fails the build, not the eval.)

### 6.2 What it assigns, in five sub-passes

Conceptually there are two kinds of home: **rodata** (constants, packed
once at compile time) and **workspace** (scratch cells for results,
reused at runtime). The five sub-passes fill them in order:

- **A — rodata.** Pack every constant into a contiguous block as
  24-byte CelValue frames, returning absolute offsets (no relocation
  math in the emitted wasm). Aggregates are *not* packed here — they're
  built at eval time in the arena.
- **B — variable slots.** One 32-byte cell per referenced variable.
- **C — variable storage.** Stamp each identifier with `{kLocal,
  index}`; the `$eval` prelude loads each local with its slot's offset.
- **D — scratch slots.** Walk the tree and hand out a workspace cell to
  each node that produces a result, via the `SlotAllocator` (§6.3).
- **E — comprehension locals.** Reserve the auxiliary wasm locals each
  comprehension needs (iteration cursor, end pointer, index counter).

### 6.3 The SlotAllocator — why a chain doesn't blow the budget

The scratch slots are where the cleverness is. Naïvely, every
intermediate result needs its own cell, so `a + b + c + … ` (N terms)
would need N cells, and a long chain would march workspace writes
straight past the 8 KiB line.

The fix is a **free list**. A node `Acquire`s a cell for its result and
`Release`s it once every reader is done. Because backing helpers read
their operand slots *before* writing their result slot, a parent can
safely reuse a just-released operand cell as its own result. So a
left-associative chain peaks at ~**one** live cell no matter how long it
is; a balanced tree peaks at its depth. (Pinned: a 2000-term chain
peaks at one slot and both compiles and evaluates.)

Two details that are load-bearing, not incidental:

- **Cells are 32 bytes even though a CelValue is 24.** The runtime's
  helpers use atomic memory ops, which trap on a misaligned address; a
  24-byte stride from a 16-aligned base lands every other cell on an
  8-aligned address and traps. The extra 8 bytes are alignment padding.
- **Aggregates pin their slot across their whole subtree.** A list/map
  literal writes its parent cell *first* (`cel_list_create`) and then
  fills it element by element, so the parent's cell must not alias any
  descendant's. They acquire on the way down and release on the way up;
  scalar ops do the opposite. This split is the entire "release
  discipline," and the `slot_aliasing_test` battery is what keeps it
  honest.

This free-list reuse is also what fixed the original P0: the old no-op
`Release` let a few hundred result-producing nodes push workspace past
8192 into libc, which is what *actually* caused both the "unaligned
atomic" trap and the 10K-literal-list panic — bugs that had been
misfiled against the runtime.

### 6.4 The gates — fail at compile, not at eval

Because crossing byte 8192 is silent and catastrophic, LayoutPass
refuses to emit a layout that would. Three gates, all derived from the
memory map:

1. **Slot-exhaustion gate.** If workspace would push past the window
   (minus rodata, minus the guard band), return `ResourceExhausted`
   with a remediation hint. The guard band is sized so the gate trips on
   the *next* allocation before anything spills.
2. **`ValidateExprStaticRegion`.** A whole-region check, run in *both*
   link modes: if rodata + workspace ends past 8192, reject. It covers
   dynamic mode too, because there the expression imports the runtime's
   memory — oversized rodata corrupts it at instantiate time exactly as
   in static mode. This is a *status*, never a `CHECK`: region size
   depends on embedder input, and embedder input must never crash the
   process.
3. **A static-mode install tripwire.** `CHECK(rodata_end ≤ 8192)` at the
   point we attach the rodata segment — reaching it means a gate
   upstream regressed.

The eval side adds a matching Plan-time check, so a Program whose ABI
*claims* an out-of-window slot is rejected before any write happens.
The exact cliffs (`x in [0..N]` fits at N=327, overflows at 328) move if
the stride or framing changes — the tests are the pin, not these
numbers.

## 7. Lowering — emit the wasm

`compiler/codegen/expr_lower.{h,cc}` + the comprehension TU.

Now every node is typed, resolved, and has a memory home, so lowering is
a straight tree-walk that emits Binaryen IR. `LowerToEvalFunction` adds
one nullary function `$eval` whose body is *load each free variable's
local, reset the arena, evaluate the root* and whose return value is the
root result's byte offset. Every node lowers to an i32-valued wasm
expression: the offset of its CelValue.

> **House rule: WAT first.** Every lowering arm was designed as an
> executable `.wat` file under `rewrite/wat/` *before* any C++ was
> written, and is re-run on every build. This doc cites those files
> rather than pasting wasm listings; they are the maintained reference
> for the emitted shape.

The one address primitive everything routes through is
`EmitSlotBaseAddress(Storage)`: a rodata or scratch slot is a literal
`i32.const offset`, but a *local* holds the offset, so it needs a
`local.get`. Reading `storage.payload` directly — treating a local's
*index* as an *offset* — was a real, since-fixed bug; the primitive
exists so no arm makes that mistake.

### 7.1 The kCall dispatch ladder

`Emit`'s call arm tries four things in order:

1. **`dyn(x)`** — identity; emit the argument.
2. **`_[_]`** (indexing) — origin-aware dispatch (§7.3); optional
   operands route to the optional-index kernel.
3. **`_?_:_`** (ternary) — the one operator where laziness matters (§7.4).
4. **Everything else** — look up the overload id in the OverloadTable,
   flatten a receiver into `args[0]`, and emit one uniform call shape:
   `(out_slot, arg_slot…) -> void`.

`&&`, `||`, and `!` take that last arm — they evaluate *both* operands
eagerly and let the kernel do non-strict 3VL absorption. That's
spec-equivalent because CEL is side-effect-free, and it's simpler and
faster than branching. (The original plan called for explicit branching
here; it was wrong, and the eager shape is the as-shipped fact.)

### 7.2 kSelect — three branches on the operand's repr

A `select` (`x.field`) means three different things depending on what
`x` is, so `EmitKSelect` switches on the operand's `repr`:

- **optional** → the optional-field-select kernel.
- **map** → map field-selection sugar (`m.field` ≡ `m['field']`),
  lowered to a map lookup with the field name from rodata. This always
  uses the *dynamic* dispatcher, even though ResolvePass tags map
  selects `kHost`: a nested select like `{'c':{...}}.c.d` yields an
  arena map the host trampoline can't read, and the dynamic dispatcher's
  runtime kind-branch routes correctly at any depth.
- **otherwise (proto message)** → emit a host `cel_get_field` call,
  recording the field name and number in a side row for the ABI.

### 7.3 Three-path aggregate dispatch

This is the payoff of the origin tags from §5. A map/list operation
picks its call target from its operand's origin: `kArena` → a pure-wasm
fast path, `kHost` → a host trampoline, `kDynamic` → a runtime
dispatcher that kind-branches once and tail-calls the right arm. The
OverloadTable points aggregate ops at the dynamic dispatcher by default,
so it stays a flat id→name map; only the hand-tuned `_[_]` and select
arms exploit compile-time origin.

### 7.4 Ternary

`EmitConditional` is the only place we nest evaluation: each arm's code
lives *inside* its `if`-branch, so only the chosen arm runs. The outer
check is "is the cond a `CEL_BOOL`?" — anything else (an UNKNOWN or
ERROR cond) is copied through verbatim — and the inner check selects on
the bool payload. Arms copy from their emitted value through the
storage-aware copy helper, so a cond or arm that lives in a local works
correctly.

### 7.5 Comprehensions

`expr_lower_comprehension.cc`. `LowerComprehension` emits a prologue, a
`(block (loop …))`, and a result expression. The shape that makes it
tractable: **the loop step is classified once, by AST structure, into a
closed set** — list-append, map-insert, map-merge (each with an
optional filter), or a generic fold — because the macro names are gone
by the time we see the tree. Collection accumulators are *pre-sized*
(capacity = range count × per-iteration count) and the runtime traps on
overflow, so a sizing bug is a loud trap, not a silent corruption.
Similarly, the loop-condition is matched against a closed set of four
peephole shapes; anything else fails compile loudly rather than emitting
a wrong loop.

### 7.6 The OverloadTable

A flat map from cel-cpp overload-id strings (copied *verbatim*, typos
included, so the lookup stays byte-equal with the checker) to
`(import module, helper name)`. 271 built-in seeds, plus a row per
custom-function decl. A coverage tripwire partitions every standard
overload id between "seeded" and "explicitly unimplemented" and rejects
any overlap or gap, so a new cel-cpp overload can't slip through
unhandled — it fails at `Build()` naming the id.

## 8. Finalization and link modes

`celwasm::Compile` dispatches on `link_mode`. Both arms share
`RunFrontAndLayout` at the front and `LowerExportAndFinalise` at the
back — **one codegen path, two bootstraps** — so the modes can't
silently diverge. The difference is only *how the module is assembled
around the same `$eval`*:

- **Dynamic** — a fresh module that *imports* `cel.memory` and the full
  runtime surface (`arena_reset`, every host trampoline, the map/list
  kernels). The runtime is a separate `.wasm` linked at Plan time.
- **Static (the default)** — adopt the wrapper-stripped runtime bytes as
  the base module, attach rodata on its memory, and install the host
  imports under codegen's canonical names. Every `cel.*` call is now a
  *defined* function in the adopted module, so no `cel.*` imports remain.

Why static is the default, and the full link-mode reasoning, is
[`00-architecture.md` §3](00-architecture.md). The standing rule that
matters here: **we install the entire runtime import surface regardless
of what the AST uses.** Unused imports are harmless; AST-gated imports
are a silent-breakage vector, so we don't do them.

The shared tail, `LowerExportAndFinalise`: build the OverloadTable,
install the import/export surface (self-skipping names already defined
in the adopted runtime), lower and export `$eval`, attach the serialized
`cel.abi` section, then **validate first, optimize only if asked,
serialize.** Validating before optimizing matters — optimizing an
unvalidated module mutates unproven IR.

A kStatic Program has zero `cel`-module imports, keeps its `cel_host.*`
imports, exports `eval`, and is >10× the size of its dynamic twin —
all pinned by `compile_test.cc`.

## 9. Public surface and options

A `Compiler` is built through `Compiler::Builder` (which consumes itself
on `Build()`): declare variables, add function libraries, add `.celfn`
source. Validation is deferred to `Build()` so you get every problem at
once. One `Compiler` mints many `Program`s; a `Program` is pure bytes.

The options are few, and the honest version of what each one *actually*
does — which sometimes differs from what the header historically
claimed — is worth stating plainly:

| Knob | What it really does |
|---|---|
| `mem_size_bytes` (default 128 KiB) | Dynamic mode: the initial page count of the imported memory. **Static mode: no effect** (the adopted runtime owns its memory). The old "raise for a bigger arena" advice is wrong — the arena is dlmalloc-sized at runtime. |
| `container` | Forwarded to cel-cpp's name-resolution container. Checker-only; no codegen effect. |
| `optimize_level` (default 0) | Gates Binaryen optimization. Level 0 is a byte-identical no-op. Negative levels silently behave as 0; only `> 3` is rejected. Binaryen's optimizer is process-global state, so serialize concurrent Compiles when this is `> 0`. |
| `link_mode` (default static) | Picks the bootstrap (§8). |

Internal-only knobs (export names, validate/serialize toggles) stay on
the internal `CompileOptions`; the public struct carries only what tunes
an expression's lowering.

## 10. Rejected alternatives

Recorded so they aren't re-proposed without new evidence:

- **Sethi–Ullman / Strahler slot pre-assignment** — the LIFO free list
  (§6.3) gets the same peak-≈-depth result more simply.
- **A `MessagePattern` table for proto literals** — shipped shape is
  type-id interning + empty-then-populate calls.
- **Always-host dispatch / always-materialise** — both lost to the
  three-path origin split (§7.3).
- **Explicit branching for `&&` / `||`** — eager slot-out + kernel 3VL
  absorption is spec-equivalent and simpler (§7.1).
- **Resolve-time cross-numeric overload pick** — non-viable; cel-cpp's
  reference map carries no candidate list to pick from, so the
  cross-numeric id must be synthesized at codegen from operand reprs.
- **Interned uint32 overload ids** — replaced by borrowed `string_view`s
  (a future `TypedAst` serialization would need to re-own them).
- **Inlining the runtime conditionally / "don't inline the runtime"** —
  static-by-default won on measurement; the old reasoning still governs
  the dynamic mode.
- **Rodata caps / dedup / runtime-initialized literals** — deliberate
  non-features; the budget lives at the §6.4 gates.
- **AST-gated ("lazy") imports** — the full runtime surface installs
  regardless of AST shape (§8).

## 11. Known gaps and future work

The honest state. A few are *pinned, open bugs*; the rest are planned
work or unverified questions.

**Pinned bugs (open):**
- An ERROR accumulator trips `exists`'s early-exit peephole because it
  reads the bool payload without a kind check, so
  `[0,2].exists(x, 2/x == 1)` returns the division error instead of
  `true` (`KnownBugs.ExistsAbsorbsErrorAccumulator`). Fix: a kind check
  ahead of the payload probe.
- A `transformMapEntry` whose entry isn't a map *literal* hits
  `ABSL_CHECK(false)` and aborts the compiler
  (`KnownBugs.TransformMapEntryComputedEntryCrash`) — should be a
  status error.

**Planned:**
- Tie the hand-copied CelValue wire constants in codegen to
  `runtime/cel_data.h` (today a layout change would compile green and
  fail only at e2e).
- Grow origin inference (`kCall → kHost`, comprehension-fold → arena) —
  measure before building.
- A relocatable / growable static region, lifting the rodata-bound
  ceilings (the `in`-list cliff at ~327 ints); the boundary tests flip
  back to value checks when it lands.
- The `@native` library-module fork — its producer
  (`CompileLibraryBodies` / `rodata_base_override`) is declared but
  unbuilt; it gets a producer or gets deleted
  ([`05-custom-functions.md`](05-custom-functions.md)).

**Unverified questions** (the `V…` tracking items — repr edge cases,
option-contract pins, the dyn-cond ternary behavior, short-circuit
`orValue`, single-walk Pass-D) are catalogued in
[`design/notes/`](notes/) rather than carried inline here, so they don't
clutter the design.

<!-- diagram-wanted: the pass-contract chain — one box per pass with
     consumes/produces edges and the three gate diamonds -->
<!-- diagram-wanted: the kSelect / aggregate dispatch decision tree —
     operand repr → origin → forced-dynamic overrides → call target -->

---

## Appendix A — pass contracts

The precise consume/produce/reorder contract for each pass, for when
you're changing the pipeline and need the invariants exact. The prose in
§3–§8 is the explanation; this is the lookup table.

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
