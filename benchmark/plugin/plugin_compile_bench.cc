// Compile-path plugin costs — the compile-side complement to the
// eval-dispatch benches in plugin_bench.cc next door.
//
// Three questions, all answered against the real macro-built demo
// plugin (//e2e/plugin_fixtures/cel_wasm_plugin_demo:demo_plugin — a
// wasm32-wasip2 Component-Model component carrying its `.celfn`
// declarations in an embedded `cel.fns` custom section):
//
//   1. BM_PluginLoad — the cost of `Plugin::Load`: CM-preamble
//      classification + top-level section walk to `cel.fns` + celfn
//      parse + SHA-256 over (bytes ‖ declarations).  This is the
//      per-artifact fixed cost an embedder pays once per plugin.
//   2. BM_CompilerBuild_* / BM_Compile_* — decl-registration cost:
//      the same expression compiled by a plain Compiler vs one with
//      a `Builder::Use`'d plugin.  The Build pair isolates the
//      builder-time registration; the Compile pair isolates the
//      per-compile checker-env + emission cost of carrying the decls
//      (uncalled), and the `add(a, b)` row adds a live plugin call
//      site (a `cel_fn` import + one required-functions row).
//   3. BM_Compile_RequiredFnEmission_* — required-functions emission
//      overhead as declared-fn count grows (1 vs 8 declared, 1
//      called).  The table is derived from the POST-optimize import
//      surface (m35-plugin-ergonomics.md §5.2), so the Opt0/Opt2
//      pair is load-bearing: at optimize_level 0 every declared fn's
//      import survives and the emitted table carries all N rows; at
//      level 2 Binaryen drops the uncalled imports and the table
//      carries exactly one.
//
// Production config per CLAUDE.md "Benchmark configuration":
// `kBenchOptimizeLevel = 2` on every timed compile (the Opt0 rows of
// the emission table are the explicit comparison-pair deviation, per
// the pipeline_bench `_Opt2` precedent); bench itself under
// `bazel run -c opt`.
//
// `manual`-tagged — invoke explicitly:
//   bazel run -c opt //benchmark/plugin:plugin_compile_bench
//   bazel run -c opt //benchmark/plugin:plugin_compile_bench -- \
//       --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

#include <array>
#include <cstdint>
#include <fstream>
#include <ios>
#include <string>
#include <utility>
#include <vector>

#include "abi/plugin.h"
#include "absl/log/absl_check.h"
#include "absl/memory/memory.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "benchmark/benchmark.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "shared/type.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace celwasm {
namespace {

constexpr int kBenchOptimizeLevel = 2;

using ::bazel::tools::cpp::runfiles::Runfiles;

// argv[0] captured in main() so Runfiles::Create can find this
// binary's runfiles tree.  benchmark::RunSpecifiedBenchmarks() does
// not forward argv to the BM fns, so we route through a global.
std::string& Argv0() {
  static std::string* const v = new std::string();
  return *v;
}

// Load the macro-built demo plugin's bytes once.
const std::vector<uint8_t>& DemoPluginBytes() {
  static const std::vector<uint8_t>* const bytes = []() {
    std::string error;
    auto runfiles = absl::WrapUnique(Runfiles::Create(Argv0(), &error));
    ABSL_CHECK(runfiles != nullptr) << "runfiles init failed: " << error;
    const std::string path = runfiles->Rlocation(
        "_main/e2e/plugin_fixtures/cel_wasm_plugin_demo/"
        "demo_plugin.wasm");
    ABSL_CHECK(!path.empty()) << "demo_plugin.wasm not in runfiles";
    std::ifstream f(path, std::ios::binary);
    ABSL_CHECK(f.is_open()) << "open " << path;
    return new std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
  }();
  return *bytes;
}

Plugin LoadDemoPluginOrDie() {
  auto p_or = Plugin::Load(DemoPluginBytes());
  ABSL_CHECK_OK(p_or);
  return *std::move(p_or);
}

CompilerOptions BenchOpts(int optimize_level = kBenchOptimizeLevel) {
  CompilerOptions opts;
  opts.optimize_level = optimize_level;
  return opts;
}

Compiler BuildCompilerOrDie(Compiler::Builder b) {
  auto c_or = std::move(b).Build();
  ABSL_CHECK_OK(c_or);
  return *std::move(c_or);
}

// Two-int-variable builder shared by the decl-registration pairs so
// the plain and Use'd compilers differ ONLY in the plugin decls.
Compiler::Builder TwoIntBuilder() {
  Compiler::Builder b;
  b.DeclareVariable("a", CelType::Int()).DeclareVariable("b", CelType::Int());
  return b;
}

// ══════════════════════════════════════════════════════════════════════
// 1. Plugin::Load — the per-artifact fixed cost.
// ══════════════════════════════════════════════════════════════════════

void BM_PluginLoad(benchmark::State& state) {
  const std::vector<uint8_t>& bytes = DemoPluginBytes();
  for (auto _ : state) {
    auto p_or = Plugin::Load(bytes);
    ABSL_CHECK_OK(p_or);
    std::array<uint8_t, 32> hash = p_or->hash();
    benchmark::DoNotOptimize(hash);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(bytes.size()));
}
BENCHMARK(BM_PluginLoad);

// ══════════════════════════════════════════════════════════════════════
// 2. Decl-registration cost — plain builder vs Use'd plugin.
// ══════════════════════════════════════════════════════════════════════

void BM_CompilerBuild_NoLibrary(benchmark::State& state) {
  for (auto _ : state) {
    auto c_or = TwoIntBuilder().Build();
    ABSL_CHECK_OK(c_or);
    benchmark::DoNotOptimize(*c_or);
  }
}
BENCHMARK(BM_CompilerBuild_NoLibrary);

void BM_CompilerBuild_UsePlugin(benchmark::State& state) {
  const Plugin plugin = LoadDemoPluginOrDie();
  for (auto _ : state) {
    Compiler::Builder b = TwoIntBuilder();
    b.Use(plugin);
    auto c_or = std::move(b).Build();
    ABSL_CHECK_OK(c_or);
    benchmark::DoNotOptimize(*c_or);
  }
}
BENCHMARK(BM_CompilerBuild_UsePlugin);

// Same expression (`a + b` — no plugin call) through both compilers:
// the delta is the per-compile cost of carrying the plugin's decls in
// the checker environment + emission with zero surviving imports.
void BM_Compile_NoLibrary(benchmark::State& state) {
  const Compiler c = BuildCompilerOrDie(TwoIntBuilder());
  for (auto _ : state) {
    auto p_or = c.Compile("a + b", BenchOpts());
    ABSL_CHECK_OK(p_or);
    benchmark::DoNotOptimize(*p_or);
  }
}
BENCHMARK(BM_Compile_NoLibrary);

void BM_Compile_PluginDeclsUncalled(benchmark::State& state) {
  const Plugin plugin = LoadDemoPluginOrDie();
  Compiler::Builder b = TwoIntBuilder();
  b.Use(plugin);
  const Compiler c = BuildCompilerOrDie(std::move(b));
  for (auto _ : state) {
    auto p_or = c.Compile("a + b", BenchOpts());
    ABSL_CHECK_OK(p_or);
    benchmark::DoNotOptimize(*p_or);
  }
}
BENCHMARK(BM_Compile_PluginDeclsUncalled);

// A live plugin call site: overload resolution against the plugin
// decl, a surviving `cel_fn.add_int_int` import, and one PLUGIN row
// in the emitted required-functions table.
void BM_Compile_PluginCallAdd(benchmark::State& state) {
  const Plugin plugin = LoadDemoPluginOrDie();
  Compiler::Builder b = TwoIntBuilder();
  b.Use(plugin);
  const Compiler c = BuildCompilerOrDie(std::move(b));
  for (auto _ : state) {
    auto p_or = c.Compile("add(a, b)", BenchOpts());
    ABSL_CHECK_OK(p_or);
    benchmark::DoNotOptimize(*p_or);
  }
}
BENCHMARK(BM_Compile_PluginCallAdd);

// ══════════════════════════════════════════════════════════════════════
// 3. Required-functions emission overhead — N declared, 1 called.
// ══════════════════════════════════════════════════════════════════════

CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}

// N plugin decls `int fn<i>(int x)` under one synthetic interface.
FunctionLibrary BuildSyntheticPluginLib(int n_decls) {
  FunctionLibrary::Builder b;
  b.SetWitInterface("cel:bench/fns@0.1.0");
  for (int i = 1; i <= n_decls; ++i) {
    b.AddPlugin(absl::StrCat("fn", i), Prim(CelfnType::Kind::kInt),
                {CelfnParam{false, Prim(CelfnType::Kind::kInt), "x"}});
  }
  auto lib_or = b.Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

Compiler MakeCompilerWithSyntheticLib(int n_decls) {
  Compiler::Builder b;
  b.DeclareVariable("x", CelType::Int());
  b.DeclareFunctions(BuildSyntheticPluginLib(n_decls));
  return BuildCompilerOrDie(std::move(b));
}

void RunRequiredFnEmission(benchmark::State& state, int optimize_level) {
  const Compiler c =
      MakeCompilerWithSyntheticLib(static_cast<int>(state.range(0)));
  const CompilerOptions opts = BenchOpts(optimize_level);
  for (auto _ : state) {
    auto p_or = c.Compile("fn1(x)", opts);
    ABSL_CHECK_OK(p_or);
    benchmark::DoNotOptimize(*p_or);
  }
}

// At level 2 the uncalled decls' imports are optimized away, so the
// emitted required-functions table has one row regardless of N —
// the Declared1→Declared8 delta is checker-env + import-install cost.
void BM_Compile_RequiredFnEmission_Opt2(benchmark::State& state) {
  RunRequiredFnEmission(state, /*optimize_level=*/2);
}
BENCHMARK(BM_Compile_RequiredFnEmission_Opt2)->Arg(1)->Arg(8);

// At level 0 every declared fn's import survives and the table
// carries all N rows — the pair reads the table-size-scaling cost.
void BM_Compile_RequiredFnEmission_Opt0(benchmark::State& state) {
  RunRequiredFnEmission(state, /*optimize_level=*/0);
}
BENCHMARK(BM_Compile_RequiredFnEmission_Opt0)->Arg(1)->Arg(8);

}  // namespace
}  // namespace celwasm

int main(int argc, char** argv) {
  // Capture argv[0] for the runfiles loader before Google Benchmark
  // consumes it.
  if (argc > 0) celwasm::Argv0() = argv[0];
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
