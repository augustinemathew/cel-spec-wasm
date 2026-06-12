# Profiling cel-wasm

How to find where time goes — in the JIT'd wasm *and* in the C++
evaluator. This is the practical companion to
[`design/07-benchmarking.md`](design/07-benchmarking.md) (which is about
*measuring* numbers): this doc is about *explaining* them.

## 1. Know which quadrant you're in first

Don't reach for a profiler until you know what you're profiling. cel-wasm
has two cost phases and two execution domains, and they need different
tools:

|  | **wasm domain** (JIT'd native code) | **host domain** (C++) |
|---|---|---|
| **Plan** (once per Program) | Cranelift compiling the module | ABI decode, descriptor resolution, instantiation |
| **Eval** (per call, the hot path) | the `$eval` body + runtime kernel | `cel_host.*` trampolines, marshal, decode |

Two facts that decide where to look:

- **Plan is amortized; Eval is the hot path.** Plan JITs the module once
  and you `Eval` it many times. A number that looks bad is almost always
  an Eval number — but if you're benchmarking compile-heavy workloads
  (many distinct expressions, each Planned once), Plan-time Cranelift cost
  is the thing, and it's a different profile entirely.
- **Arithmetic stays in wasm; strings/maps/lists/protos cross into C++.**
  A `a + b * c` expression runs entirely as JIT'd native code. A
  `msg.field`, a map lookup, a string concat, or an `in` over a host list
  calls *out* through a `cel_host.*` trampoline into the C++ evaluator.
  When cel-wasm loses to cel-cpp (the honest losses in the benchmark
  table), the time is almost always at that boundary — so the first
  question is "does this expression cross into the host, and how often?"

So: identify the phase and the domain, *then* pick the tool below.

## 2. Drive the workload with the existing harnesses

Never profile a hand-rolled loop — the build config alone will mislead
you (a debug build is ~10× off and useless as a baseline). Use the
harnesses, always under `-c opt`:

- **`benchmark/eval/`** — the three-way comparison. `run.sh` builds both
  benches `-c opt`, runs `celwasm_bench` in *both* link modes (static +
  dynamic) and `celcpp_bench`, and `report.sh` prints the
  mode-vs-cel-cpp tables. This is the harness for "is this slower than
  cel-cpp, and by how much."

  ```bash
  benchmark/eval/run.sh          # full run + comparison report
  benchmark/eval/run.sh smoke    # min_time=0.1s, fast turnaround
  ```

- **`bench/`** — Google Benchmark microbenches for "how fast is this one
  thing." `kernel_bench` times the runtime-kernel ops in isolation
  (arith / compare / convert / string / 3VL / aggregates); `pipeline_bench`
  times `Compile` / `Plan` / `Eval` separately — and crucially it
  **pre-stages the compiler, program, and instance outside the timed
  loop**, so the Eval numbers are steady-state Eval cost with no compile
  or plan bleeding in. That pre-staging is the pattern to copy when you
  write your own bench: stage everything you can outside `for (auto _ :
  state)` so you measure only the phase you care about.

  ```bash
  bazel run -c opt //bench:kernel_bench
  bazel run -c opt //bench:pipeline_bench
  ```

**Production config is not optional for a real number** (see
`CLAUDE.md` §Benchmark configuration). `-c opt`, the runtime built
`-O3 -flto`, and the expression module compiled at
`optimize_level = kBenchOptimizeLevel` (2). Anything less is a
pessimized baseline that doesn't represent what an embedder ships, and
the LTO in particular is load-bearing — it inlines kernel helpers
(`cel_int_eq` into `cel_list_in`'s scan loop) that otherwise show up as
non-inlined calls and mislead the profile.

## 3. Profiling the JIT'd wasm

The hot Eval path runs as native machine code Cranelift produced from the
wasm. A sampling profiler will show those frames as anonymous JIT
addresses (`[JIT] 0x7f…`) unless you give it a symbol map — which is
exactly what the engine's perfmap knob does.

**Step 1 — emit the perfmap.** `Engine::Builder::EnableJitPerfMap(true)`
sets wasmtime's `PERFMAP` profiling strategy, which writes
`/tmp/perf-<pid>.map` describing every JIT'd function (the runtime kernel
*and* every Planned expr module). The eval bench already wires this
behind an env knob:

```cpp
auto engine = celwasm::Engine::NewBuilder()
                  .EnableJitPerfMap(true)   // off by default
                  .Build()
                  .value();
```

```bash
# benchmark/eval/celwasm_bench honors this directly:
CELWASM_BENCH_PERFMAP=1 bazel run -c opt //benchmark/eval:celwasm_bench
```

**Step 2 — sample it.** Two profilers understand the perfmap format:

- **`samply`** (recommended — cross-platform, works on macOS *and*
  Linux; `cargo install samply`). It records, reads `/tmp/perf-<pid>.map`
  to symbolicate the JIT frames, and opens an interactive flamegraph in
  the browser.

  ```bash
  CELWASM_BENCH_PERFMAP=1 samply record -- \
      bazel-bin/benchmark/eval/celwasm_bench --benchmark_filter=YourCase
  ```

- **Linux `perf`** — `perf record -g -- <bench>` then `perf report`; it
  reads the same `/tmp/perf-<pid>.map` automatically.

With the map loaded you'll see real names — `$eval`, `cel_int_add`,
`cel_list_in`, `cel_map_lookup` — and the flamegraph tells you whether
the time is in the expression body, a kernel helper, or a `cel_host.*`
call back into C++ (which is your cue to switch to §4).

## 4. Profiling the evaluator (C++)

The host side — Plan-time Cranelift, the activation marshal, the result
decode, and every `cel_host.*` trampoline — is ordinary native C++, so
ordinary native tools apply. Build `-c opt` but keep frame pointers for
readable stacks:

```bash
bazel build -c opt --copt=-fno-omit-frame-pointer //bench:pipeline_bench
```

- **macOS** — `samply record -- <bench>` (same tool as §3, so one
  flamegraph spans both domains), or Instruments' Time Profiler, or
  `sample <pid>` against a running bench.
- **Linux** — `perf record -g` / `perf report`, or `valgrind
  --tool=callgrind` for instruction-level attribution when sampling is
  too coarse.
- **Allocation behavior** — the arena and the activation buffer are the
  things to watch. Instruments' Allocations, `heaptrack` (Linux), or a
  run under `--config=asan` (which also catches the memory bugs the
  pitch claims it doesn't have) will show arena churn and per-Eval
  allocation.

**Isolating Plan from Eval** is the whole game on the C++ side. Use the
`pipeline_bench` pattern: Plan once outside the loop, Eval inside it, so
the profile attributes time to the phase you mean. If Plan-time Cranelift
dominates a compile-heavy workload, that's a different investigation
(module size, the per-Plan re-parse seam noted in
[`design/02-evaluator.md`](design/02-evaluator.md) §10) than an Eval
hotspot.

## 5. Reading the codegen, not just the clock

Sometimes the profiler tells you *where* but not *why* — and the answer
is in the wasm the compiler emitted. The CLI dumps it:

```bash
# Compile an expression to a standalone .wasm you can inspect.
bazel run //tools/cel:cel -- compile 'a + b + c' \
    --var a:int --var b:int --var c:int --output /tmp/expr.wasm

wasm-dis  /tmp/expr.wasm | less     # Binaryen — readable WAT
wasm2wat  /tmp/expr.wasm | less     # wabt — alternative WAT
wasm-objdump -d /tmp/expr.wasm      # wabt — bytecode disassembly
```

What to look for: the number of locals and workspace slots (slot reuse —
`01-compiler.md` §6.3 — should keep a long chain flat, not N-deep), the
call sequence (is this expression crossing into `cel_host.*`, and how
many times?), and the overall shape (a flat statement sequence vs deep
operand nesting). For the lowering-level reference, the assembled WAT
traces under `doc/implementation-plan/rewrite/wat/` show the intended
shape of every codegen arm.

## 6. A worked example

Say `perm in perms` (an `in` over a bound `list<string>`) profiles
slower than cel-cpp. The quadrant check (§1) says: Eval phase, and `in`
over a *host* list crosses into C++. Confirm with the harness:

```bash
CELWASM_BENCH_PERFMAP=1 samply record -- \
    bazel-bin/benchmark/eval/celwasm_bench --benchmark_filter=in_
```

The flamegraph shows the time in `cel_host.cel_list_in`'s scan, calling
the C++ comparison per element — the host-boundary cost from §1, not a
codegen bug. Then `wasm-dis` of the compiled expression confirms the
single `cel_host.cel_list_in` call (host-origin operand, not the
pure-wasm `cel.*_arena` fast path). The conclusion writes itself: this is
the architectural cost of a host-backed list, and the fix is the
materialize-into-arena path on the roadmap — exactly the honest,
cause-attributed framing the benchmark table uses, now backed by a
profile instead of a guess.

## 7. Gotchas

- **Profile `-c opt`, never fastbuild.** Debug timing is ~10× inflated
  and the flamegraph is dominated by un-inlined helpers that don't exist
  in a shipping build.
- **Keep the sandbox config.** The engine *requires* wasm tail calls,
  threads, and shared memory (the runtime is `wasm32-wasi-threads` with
  `musttail` dispatchers); don't strip them to "simplify" a profiling
  build — you'd be profiling a module that can't run.
- **Don't conflate Plan and Eval.** Pre-stage Plan outside the timed loop
  (or filter it out of the flamegraph) — otherwise a one-time JIT cost
  smears across your per-call numbers.
- **The host boundary is the usual suspect, and it's often not a bug.**
  Strings, maps, lists, and protos cost a C++ round-trip by design. A
  profile that lands in `cel_host.*` is telling you the architecture, not
  pointing at a defect — weigh it against the two-sided benchmark story
  before "optimizing."
- **`perf` is Linux-only.** On macOS use `samply` (it reads the same
  perfmap) or Instruments; the perfmap knob works on both — only the
  consumer differs.
