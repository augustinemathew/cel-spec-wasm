# `benchmark/compiler/` — Compile + Plan benches

**Status: active — Compile/Plan benches live here.**  The
compiler-phase benches (compile latency, Plan/Cranelift cost, and the
cold-start composites) were ported out of the legacy `bench/`
directory.  Eval steady-state numbers live in the corpus-driven
harness under [`../eval/`](../eval/).

## How to run

Always `bazel run -c opt` — debug builds blow timing numbers up by
~10x and are useless as a baseline.  All targets are `manual`-tagged
so they never run in CI by default.

```bash
bazel run -c opt //benchmark/compiler:pipeline_bench
bazel run -c opt //benchmark/compiler:stage_bench
bazel run -c opt //benchmark/compiler:in_operator_compile_bench
bazel run -c opt //benchmark/compiler:program_size_main
```

Or run the three timed benches in one pass with JSON output to
`/tmp/benchmark_compiler_<name>.json`:

```bash
benchmark/compiler/run.sh          # full run (min_time=0.5s)
benchmark/compiler/run.sh smoke    # faster turnaround (min_time=0.1s)
```

## What each binary measures

### `pipeline_bench` — compile latency per expression shape

One representative expression per AST kind, timed at the Compile and
Plan boundaries with proper caching discipline (Compiler built once
outside the loop; Program pre-compiled for Plan-only benches; one
shared Engine per process):

  - `BM_Compile_*` — `Compiler::Compile` only: literal, 3-term
    arithmetic, 20-term comparison chain, `type(x) == int`,
    `int(string(123))`, proto struct literal.
  - `BM_Plan_*` — `Engine::Plan` only (literal, 3-term arithmetic).
  - `BM_Compile_*_Opt2` — the same Compile shapes at Binaryen
    `optimize_level = 2`, paired with the unoptimized counterparts so
    the optimization cost reads in side-by-side columns.

### `stage_bench` — per-stage decomposition + cold-start composites

Decomposes the user-facing pipeline stage-by-stage over five
scalar-literal inputs (`42`, `true`, `3.14`, `"hello"`, `null`):

  - `BM_Compiler_Build` / `BM_Engine_Build` — one-time builder costs
    (Engine::Build includes parsing `cel_runtime.wasm`).
  - `BM_Compile` — per-source Compile, Compiler reused.
  - `BM_Plan_Hot` — per-Plan (Cranelift translate + link), Engine and
    Program reused.  Should be roughly flat across inputs — Plan is
    shape-agnostic.

The composite rows are the truly-cold / warm-builder / warm-program
partition — the operator's actual "how long until I can serve this
new policy" number:

| BM name | what's pre-built | what's timed |
|---|---|---|
| `BM_Pipeline_Cold` | nothing | Compiler::Builder + Engine::Builder + Compile + Plan + Eval |
| `BM_Pipeline_WarmEngine` | Compiler + Engine | Compile + Plan + Eval |
| `BM_Pipeline_WarmProgram` | Compiler + Engine + Program | Plan + Eval |

(Each composite row includes one Eval so it reports time-to-first-
result; eval-only steady state is `../eval/`'s job.)

### `in_operator_compile_bench` — large-literal compile scaling

Compile and Plan cost as a source-inline list literal grows:

  - `BM_Compile_In_IntList_Literal` / `BM_Plan_In_IntList_Literal` —
    `x in [0..N-1]` at N = 100 / 1000 / 10000.
  - `BM_Compile_In_IamPermissions_Literal` /
    `BM_Plan_In_IamPermissions_Literal` — `perm in [...]` over
    realistic 50-byte GCP IAM permission strings at N = 10 / 100 /
    1000.

The literal sizes cap where the cel-cpp parser's 100 k-codepoint
source limit binds (a 10 k int list is ~98 kB of source).

### `program_size_main` — size reporter (not a timed bench)

Prints, for a representative expression matrix: the serialized
`cel.expr.CheckedExpr` AST-proto size, the expr-module wasm byte size
at `optimize_level` 0 and 2, and the opt2/ast ratio — plus `sizeof`
for the in-memory C++ API objects (`Value`, `Activation`, `Program`,
`Compiler`, the wire `CelValue`).  Used to populate the "Program
size" table in the repo README.

## Future comparative methodology

cel-cpp's "compile" is parse + check producing a tree; ours is parse
+ check + codegen-to-wasm.  cel-cpp has no Plan step (the tree IS the
plan); ours runs Cranelift on the wasm bytes.  The shapes don't
compare element-wise, so the head-to-head comparison remains future
work: the fair triple to time against our Compile + Plan is cel-cpp's
`Parse(source) → Check(parsed) → Runtime::CreateProgram(checked)` —
the honest column is "time-to-ready in each implementation".

When that comparison opens, the corpus should cover:

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
| **`@host` / `@plugin` fn count** | 0 / 1 / 10 / 100 | overload table population |

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

### Open questions

1. **Does cel-cpp's "compile" amortise differently?**  If parse cost
   dominates Compile in cel-cpp, the comparison should split parse vs
   check vs CreateProgram into separate columns so an honest reader
   can locate where the time goes.
2. **Should we include the wasi-sdk runtime instantiation cost?**
   `cel_runtime.wasm` gets instantiated per Engine.  cel-cpp has no
   analog.  Probably belongs in a "warm-builder" row only so it
   doesn't confuse the steady-state Compile number.
3. **Cold-start with file I/O?**  Loading `cel_runtime.wasm` from
   disk vs from `runtime/cel_runtime_wasm_bytes.h` (embedded).  The
   embedded path is what real embedders ship; the bench should
   measure the embedded path.

The eval-side harness in `../eval/` defines the corpus schema and the
comparator-runner contract.  The compiler-side comparison, when it
opens, reuses that schema with a `phase: compile | plan | both` field
and a `pre_built: nothing | builder | builder+program` field for the
truly-cold / warm-builder / warm-program partition.
