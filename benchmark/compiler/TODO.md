# `benchmark/compiler/` — Compile + Plan benches (deferred)

**Status: not started.**  Common cases listed here so the day the
compiler-side methodology becomes load-bearing, the test matrix is
already specified.

## Why deferred

cel-cpp's "compile" is parse + check producing a tree; ours is parse
+ check + codegen-to-wasm.  cel-cpp has no Plan step (the tree IS the
plan); ours runs Cranelift on the wasm bytes.  The shapes don't
compare element-wise — a "Compile" cell on each side measures
fundamentally different work.

Compile + Plan numbers DO matter for:
  - one-shot policy compile latency (e.g. K8s admission webhook
    cold-start)
  - hot-reload scenarios (policy change → recompile)
  - bulk-compile workloads (compile 10K policies at startup)

When those scenarios become the load-bearing question, this dir
fills in.  Until then: empty.

## What the corpus should cover when this opens

### Compile latency

| dimension | values | notes |
|---|---|---|
| **source length** | 1 / 10 / 100 / 1000 chars | linear vs the source size |
| **AST depth** | 1 / 5 / 20 / 100 nodes | recursive descent worst case |
| **variable count** | 0 / 1 / 10 / 100 declared | Compiler builder cost |
| **surface coverage** | every kind once + a "kitchen sink" | catches per-arm cost drift |
| **map / list literal size** | 1 / 10 / 100 / 1000 entries | rodata layout cost scales |
| **proto type complexity** | flat / nested / recursive | descriptor walk cost |
| **macro density** | 0 / 1 / 5 / 10 nested comprehensions | expansion cost |
| **`@host` / `@component` fn count** | 0 / 1 / 10 / 100 | overload table population |

Per cell: time taken by `Compiler::Compile(source)` from a
pre-`Build()`-ed Compiler.

### Plan latency (Cranelift translate-to-native)

| dimension | values | notes |
|---|---|---|
| **wasm body size** | bytes after Binaryen optimize_level=0 / 2 | the Cranelift input size |
| **function count** | 1 (root) / 5 (with helpers) / 50 (heavy comprehensions) | wasm fn-count drives Plan cost ~linearly |
| **import count** | 0 / 10 / 50 (full kernel) | resolution-table cost |
| **memory size** | min pages 1 / 16 / 256 | mmap cost in wasmtime |

Per cell: time taken by `Engine::Plan(program)` from a pre-loaded
`Program`.

### Compile + Plan composite (cold-start hot path)

Cold-start latency is the headline number for "policy change →
serve":

| scenario | what's pre-built | what's timed |
|---|---|---|
| **truly-cold** | nothing | Compiler::Builder + Engine::Builder + Compile + Plan |
| **warm-builder** | Compiler + Engine | Compile + Plan |
| **warm-program** | Compiler + Engine + Program | Plan only |

These three rows for every interesting expression shape — that's the
operator's actual "how long until I can serve this new policy" number.

### What the comparator looks like for cel-cpp

cel-cpp's analog of Compile + Plan is `Parse(source) → Check(parsed)
→ Runtime::CreateProgram(checked)`.  That's a fair triple to time
against our Compile + Plan, even though the work done is different
(tree vs wasm + Cranelift).  Honest column: "what's the time-to-
ready in each implementation."

When this dir opens, the harness extends to wrap that triple for
cel-cpp alongside ours, same shape as the eval-side wrapping.

## Open questions for when this opens

1. **Does cel-cpp's "compile" amortise differently?**  If parse cost
   dominates Compile in cel-cpp, the comparison should split parse vs
   check vs CreateProgram into separate columns so an honest reader
   can locate where the time goes.

2. **Should we include the wasi-sdk runtime instantiation cost?**
   `cel_runtime.wasm` (~241 KB) gets instantiated per Engine.  cel-
   cpp has no analog.  Probably belongs in a "warm-builder" row only
   so it doesn't confuse the steady-state Compile number.

3. **Cold-start with file I/O?**  Loading `cel_runtime.wasm` from
   disk vs from `runtime/cel_runtime_wasm_bytes.h` (embedded).  The
   embedded path is what real embedders ship.  Bench should measure
   the embedded path.

## Cross-link to eval

The eval-side harness in `../eval/HARNESS.md` defines the corpus
schema and the comparator-runner contract.  The compiler-side
harness, when it opens, REUSES that schema with three additions:

  - `phase: compile | plan | both` field on each entry.
  - `pre_built: nothing | builder | builder+program` field for the
    truly-cold / warm-builder / warm-program partition.
  - The output table groups by `phase` and `pre_built` instead of
    `surface`.

Nothing more — the methodology is the same; the columns just shift.
