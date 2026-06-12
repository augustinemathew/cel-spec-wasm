// Per-stage cost across the user-facing compile pipeline:
//
//   celwasm::Compiler::Builder::Build      compile-time setup (near-zero)
//   celwasm::Engine::Builder::Build        wasm engine + parse runtime
//   celwasm::Compiler::Compile             source → Program bytes (per-source)
//   celwasm::Engine::Plan                  Program → Instance (per-Plan)
//
// Benches against the production user-facing surface, not the raw
// wasmtime API — what users actually call.  Eval-only steady-state
// numbers live in the corpus-driven benches under `benchmark/eval/`;
// the composite rows here include one Eval per iteration because the
// question they answer is "how long until this policy serves its
// first result".
//
// Run:
//   bazel run -c opt //benchmark/compiler:stage_bench -- \
//       --benchmark_min_time=1s
//
// Inputs.  Five representative scalar-literal CEL kinds are exercised
// so the per-Compile / per-Plan numbers are visible per kind:
//   L = "42"          int
//   B = "true"        bool
//   D = "3.14"        double
//   S = "\"hello\""   string  (rodata + variable-length payload)
//   N = "null"        null
//
// What the numbers should confirm (architectural invariants):
//   1. BM_Plan_Hot is roughly flat across all inputs — Plan is shape-
//      agnostic.  If it varies wildly by input the input is doing
//      per-Plan work it shouldn't.
//   2. BM_Compile ≫ BM_Plan_Hot by ≥10x — per-source cost is paid at
//      Compile, not at Plan.
//   3. BM_Pipeline_Cold ≫ BM_Pipeline_WarmProgram — engine + compile
//      caching is essential.
//
// No CI gate on absolute numbers; bench is a planning + regression
// investigation tool.

#include <utility>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/engine.h"
#include "eval/instance.h"

namespace celwasm {
namespace {

// ——— Inputs ———

constexpr absl::string_view kInputs[] = {
    "42",          // L
    "true",        // B
    "3.14",        // D
    R"("hello")",  // S
    "null",        // N
};
constexpr const char* kInputNames[] = {"L_int", "B_bool", "D_double",
                                       "S_string", "N_null"};
constexpr int kNumInputs = sizeof(kInputs) / sizeof(kInputs[0]);

absl::string_view InputForRange(int range) {
  ABSL_CHECK(range >= 0 && range < kNumInputs);
  return kInputs[range];
}

// ——— Shared fixtures (process-static so benches don't double-pay
//     for Compiler / Engine / Program construction) ———

const Compiler& SharedCompiler() {
  static const Compiler* c = []() {
    auto compiler_or = Compiler::NewBuilder().Build();
    ABSL_CHECK_OK(compiler_or);
    return new Compiler(*std::move(compiler_or));
  }();
  return *c;
}

const Engine& SharedEngine() {
  static const Engine* e = []() {
    auto engine_or = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(engine_or);
    return new Engine(*std::move(engine_or));
  }();
  return *e;
}

const Program& SharedProgram(int range) {
  static Program* progs[kNumInputs] = {nullptr, nullptr, nullptr, nullptr,
                                       nullptr};
  if (progs[range] == nullptr) {
    auto p_or = SharedCompiler().Compile(kInputs[range]);
    ABSL_CHECK_OK(p_or);
    progs[range] = new Program(*std::move(p_or));
  }
  return *progs[range];
}

// Helpers to label the per-input rows in benchmark output.
void LabelInput(benchmark::State& state) {
  state.SetLabel(kInputNames[state.range(0)]);
}

// ——— Per-stage micro-benches ———

// One-time Compiler::Builder::Build cost.  Should be near-zero —
// Compiler is pure compile-time (no wasmtime).
void BM_Compiler_Build(benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    auto compiler_or = Compiler::NewBuilder().Build();
    ABSL_CHECK_OK(compiler_or);
    benchmark::DoNotOptimize(compiler_or);
  }
}
BENCHMARK(BM_Compiler_Build);

// One-time Engine::Builder::Build cost.  Engine_new + parse
// cel_runtime.wasm.
void BM_Engine_Build(benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    auto engine_or = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(engine_or);
    benchmark::DoNotOptimize(engine_or);
  }
}
BENCHMARK(BM_Engine_Build);

// Per-source Compile cost (Compiler reused).  Includes parse →
// check → resolve → layout → module → lower → assemble.  Output is
// bytes; no wasmtime touched.
void BM_Compile(benchmark::State& state) {
  LabelInput(state);
  const auto src = InputForRange(state.range(0));
  for ([[maybe_unused]] auto _ : state) {
    auto p_or = SharedCompiler().Compile(src);
    ABSL_CHECK_OK(p_or);
    benchmark::DoNotOptimize(p_or);
  }
}
BENCHMARK(BM_Compile)->DenseRange(0, kNumInputs - 1);

// Per-Plan cost (Engine + Program reused).  Hot path: store +
// memory + linker + bind cel.memory + instantiate runtime + bind
// runtime exports + parse expr bytes via wasmtime_module_new +
// instantiate expr + lookup eval.  Plan re-parses the expr bytes
// per call — this is what we pay until an Engine-side expr-module
// cache lands.
void BM_Plan_Hot(benchmark::State& state) {
  LabelInput(state);
  const Program& program = SharedProgram(state.range(0));
  for ([[maybe_unused]] auto _ : state) {
    auto inst_or = SharedEngine().Plan(program);
    ABSL_CHECK_OK(inst_or);
    benchmark::DoNotOptimize(inst_or);
  }
}
BENCHMARK(BM_Plan_Hot)->DenseRange(0, kNumInputs - 1);

// ——— Composite end-to-end benches ———

// One cold-pipeline iteration.  Pulled into a helper to keep
// BM_Pipeline_Cold under the function-size lint.
void OneColdIteration(absl::string_view src) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ABSL_CHECK_OK(compiler_or);
  auto engine_or = Engine::NewBuilder().Build();
  ABSL_CHECK_OK(engine_or);
  auto p_or = compiler_or->Compile(src);
  ABSL_CHECK_OK(p_or);
  auto inst_or = engine_or->Plan(*p_or);
  ABSL_CHECK_OK(inst_or);
  auto v_or = inst_or->Eval();
  ABSL_CHECK_OK(v_or);
  benchmark::DoNotOptimize(v_or);
}

// Cold: fresh Engine + fresh Compile + Plan + Eval per iteration.
// Worst case: brand-new process per eval.  Upper bound on what
// engine + compile caching can save.
void BM_Pipeline_Cold(benchmark::State& state) {
  LabelInput(state);
  const auto src = InputForRange(state.range(0));
  for ([[maybe_unused]] auto _ : state) {
    OneColdIteration(src);
  }
}
BENCHMARK(BM_Pipeline_Cold)->DenseRange(0, kNumInputs - 1);

// WarmEngine: Compiler + Engine reused; Compile + Plan + Eval per
// iteration.  Realistic "user submits a never-before-seen policy
// for the first time" cost.
void BM_Pipeline_WarmEngine(benchmark::State& state) {
  LabelInput(state);
  const auto src = InputForRange(state.range(0));
  for ([[maybe_unused]] auto _ : state) {
    auto p_or = SharedCompiler().Compile(src);
    ABSL_CHECK_OK(p_or);
    auto inst_or = SharedEngine().Plan(*p_or);
    ABSL_CHECK_OK(inst_or);
    auto v_or = inst_or->Eval();
    ABSL_CHECK_OK(v_or);
    benchmark::DoNotOptimize(v_or);
  }
}
BENCHMARK(BM_Pipeline_WarmEngine)->DenseRange(0, kNumInputs - 1);

// WarmProgram: Program (and everything before it) reused; just
// Plan + Eval per iteration.  Realistic "second-and-later eval of
// the same policy".
void BM_Pipeline_WarmProgram(benchmark::State& state) {
  LabelInput(state);
  const Program& program = SharedProgram(state.range(0));
  for ([[maybe_unused]] auto _ : state) {
    auto inst_or = SharedEngine().Plan(program);
    ABSL_CHECK_OK(inst_or);
    auto v_or = inst_or->Eval();
    ABSL_CHECK_OK(v_or);
    benchmark::DoNotOptimize(v_or);
  }
}
BENCHMARK(BM_Pipeline_WarmProgram)->DenseRange(0, kNumInputs - 1);

}  // namespace
}  // namespace celwasm

BENCHMARK_MAIN();
