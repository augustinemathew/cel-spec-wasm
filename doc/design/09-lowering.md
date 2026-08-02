# Lowering & link modes — emitting the wasm

How the typed, laid-out AST becomes wasm bytes: the per-node lowering arms and the two link-mode bootstraps. The passes that feed this stage are [`01-compiler.md`](01-compiler.md).

## 1. Lowering — emit the wasm

`compiler/codegen/expr_lower.{h,cc}` + the comprehension TU.

`LowerToEvalFunction` adds one nullary function `$eval`: load each free variable's local, reset the arena, evaluate the root, return the root result's byte offset. Every node lowers to an i32-valued wasm expression — the offset of its CelValue.

!!! note "House rule: WAT first"
    Every lowering arm was designed as an executable `.wat` file under `rewrite/wat/` before any C++, and is re-run on every build. Those files, not pasted listings, are the maintained reference for the emitted shape.

The one address primitive everything routes through is `EmitSlotBaseAddress(Storage)`: rodata/scratch slots are a literal `i32.const offset`, but a *local* holds the offset and needs a `local.get`. Treating a local's index as an offset was a real, since-fixed bug; the primitive exists so no arm repeats it.

### 1.1 The kCall dispatch ladder

`Emit`'s call arm tries four things in order:

1. **`dyn(x)`** — identity; emit the argument.
2. **`_[_]`** (indexing) — origin-aware dispatch (§1.3); optional operands route to the optional-index kernel.
3. **`_?_:_`** (ternary) — the one operator where laziness matters (§1.4).
4. **Everything else** — look up the overload id in the OverloadTable, flatten a receiver into `args[0]`, emit one uniform call shape: `(out_slot, arg_slot…) -> void`.

`&&`, `||`, and `!` take the last arm: both operands evaluate eagerly and the kernel does non-strict 3VL absorption. Spec-equivalent because CEL is side-effect-free, and simpler and faster than branching.

### 1.2 kSelect — three branches on the operand's repr

`EmitKSelect` switches on the operand's `repr`:

- **optional** → the optional-field-select kernel.
- **map** → map field-selection sugar (`m.field` ≡ `m['field']`), lowered to a map lookup with the field name from rodata. Always the *dynamic* dispatcher, even though ResolvePass tags map selects `kHost`: a nested select like `{'c':{...}}.c.d` yields an arena map the host trampoline can't read; the dynamic dispatcher's runtime kind-branch routes correctly at any depth.
- **otherwise (proto message)** → a host `cel_get_field` call, recording the field name and number in a side row for the ABI.

### 1.3 Three-path aggregate dispatch

The payoff of the origin tags from [01 §5](01-compiler.md#5-resolvepass-names-scopes-overloads). A map/list operation picks its call target from its operand's origin: `kArena` → a pure-wasm fast path, `kHost` → a host trampoline, `kDynamic` → a runtime dispatcher that kind-branches once and tail-calls the right arm. The OverloadTable points aggregate ops at the dynamic dispatcher by default; only the hand-tuned `_[_]` and select arms exploit compile-time origin.

### 1.4 Ternary

`EmitConditional` is the only nested evaluation: each arm's code lives inside its `if`-branch, so only the chosen arm runs. The outer check is "is the cond a `CEL_BOOL`?" — an UNKNOWN or ERROR cond is copied through verbatim — and the inner check selects on the bool payload. Arms copy through the storage-aware copy helper, so a cond or arm living in a local works correctly.

### 1.5 Comprehensions

`expr_lower_comprehension.cc`. `LowerComprehension` emits a prologue, a `(block (loop …))`, and a result expression. **The loop step is classified once, by AST structure, into a closed set** — list-append, map-insert, map-merge (each with an optional filter), or a generic fold — because the macro names are gone by the time we see the tree. Collection accumulators are pre-sized (capacity = range count × per-iteration count); the runtime traps on overflow, so a sizing bug is a loud trap, not silent corruption. The one exception is a `transformMapEntry` whose entry expression is computed rather than a literal: its key count is unknown until eval, so the runtime merge helper grows the accumulator instead. The loop-condition is matched against a closed set of four peephole shapes; anything else fails compile loudly. The two accumulator shapes test the accumulator's *kind* as well as its payload, so an error or unknown mid-fold keeps iterating and a later element can still absorb it.

### 1.6 The OverloadTable

A flat map from cel-cpp overload-id strings (copied *verbatim*, typos included, so lookup stays byte-equal with the checker) to `(import module, helper name)`. 271 built-in seeds plus a row per custom-function decl. A coverage tripwire partitions every standard overload id between "seeded" and "explicitly unimplemented" and rejects any overlap or gap — a new cel-cpp overload fails at `Build()` naming the id.

## 2. Finalization and link modes

`celwasm::Compile` dispatches on `link_mode`. Both arms share `RunFrontAndLayout` at the front and `LowerExportAndFinalise` at the back — **one codegen path, two bootstraps**. The difference is only how the module is assembled around the same `$eval`:

- **Dynamic** — a fresh module importing `cel.memory` and the full runtime surface (`arena_reset`, every host trampoline, the map/list kernels). The runtime is a separate `.wasm` linked at Plan time.
- **Static (default)** — adopt the wrapper-stripped runtime bytes as the base module, attach rodata on its memory, install the host imports under codegen's canonical names. Every `cel.*` call is a *defined* function; no `cel.*` imports remain.

The link-mode rationale is [`00-architecture.md` §3](00-architecture.md). Standing rule: **the entire runtime import surface installs regardless of what the AST uses.** Unused imports are harmless; AST-gated imports are a silent-breakage vector.

The shared tail, `LowerExportAndFinalise`: build the OverloadTable, install the import/export surface (self-skipping names already defined in an adopted runtime), lower and export `$eval`, attach the serialized `cel.abi`, then **validate first, optimize only if asked, serialize** — optimizing an unvalidated module mutates unproven IR.

A kStatic Program has zero `cel`-module imports, keeps its `cel_host.*` imports, exports `eval`, and is >10× the size of its dynamic twin — all pinned by `compile_test.cc`.

