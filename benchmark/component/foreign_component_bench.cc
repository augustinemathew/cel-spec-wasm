// Foreign-component vs native host-fn dispatch cost — m24 D.3,
// m26 D.3 rewrite.
//
// Two shapes, two backends, four BMs:
//
//   Shape A — `add(a, b) : int x int -> int`.  The smallest meaningful
//   dispatch unit: arg pull + return write, no aggregate copy.  Measures
//   per-call overhead.
//   Shape B — `len(s) : string -> int`.  A 256 KiB string argument
//   crosses the boundary; the return is a single s64.  Measures
//   per-call overhead + the canonical-ABI string-copy throughput.
//
// Two backends per shape:
//
//   - Foreign-component (m24 path) — the wasm32-wasip2 Component-Model
//     component the cel_wasm_component macro produces at
//     //e2e/foreign_component_fixtures/cel_wasm_component_demo:demo_component,
//     loaded via the bazel runfiles library and registered through
//     `Engine::AddComponent`.  Dispatched per call through
//     wasmtime_component_func_call.
//   - Native AddTypedFunction (the host-callback baseline) — same CEL
//     decl, same `cel_fn.<helper>` import, but the body is a C++ lambda
//     called directly, no canonical-ABI hop.
//
// Both backends route through `kCelFn` import dispatch (m24 §2 contract:
// component-ness is invisible to the compiler).  The delta between the
// two BMs *is* the component-call overhead; it isolates exactly what
// the m24 backend costs over m13's host path.
//
// Production config per CLAUDE.md "Benchmark configuration":
// `kBenchOptimizeLevel = 2` on every expr-module compile; LTO + -O3 on
// `cel_runtime.wasm`; bench itself under `bazel run -c opt`.
//
// `manual`-tagged — invoke explicitly:
//   bazel run -c opt //benchmark/component:foreign_component_bench
//   bazel run -c opt //benchmark/component:foreign_component_bench -- \
//       --benchmark_filter=BM_Eval_ForeignComponent \
//       --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/memory/memory.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "benchmark/benchmark.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
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

// Load the macro-built demo component's bytes once.  Returned by value
// to keep the BM-local `lib` + `bytes` lifetimes independent.
const std::vector<uint8_t>& DemoComponentBytes() {
  static const std::vector<uint8_t>* const bytes = []() {
    std::string error;
    auto runfiles = absl::WrapUnique(Runfiles::Create(Argv0(), &error));
    ABSL_CHECK(runfiles != nullptr) << "runfiles init failed: " << error;
    const std::string path = runfiles->Rlocation(
        "_main/e2e/foreign_component_fixtures/cel_wasm_component_demo/"
        "demo_component.wasm");
    ABSL_CHECK(!path.empty()) << "demo_component.wasm not in runfiles";
    std::ifstream f(path, std::ios::binary);
    ABSL_CHECK(f.is_open()) << "open " << path;
    return new std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
  }();
  return *bytes;
}

CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}

// Component-side libraries — the demo's fns.idl declares add + len + greet
// inside `cel:customfn/fns@0.1.0`.  The bench builds the smallest library
// it needs for each shape; the engine's two-level lookup uses the WIT
// interface name to find each decl's export within the demo component.

constexpr absl::string_view kDemoWitInterface = "cel:customfn/fns@0.1.0";

FunctionLibrary BuildAddLib() {
  auto lib_or = FunctionLibrary::Builder()
                    .SetWitInterface(kDemoWitInterface)
                    .AddForeignComponent(
                        "add", Prim(CelfnType::Kind::kInt),
                        {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                         CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}})
                    .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

FunctionLibrary BuildLenLib() {
  auto lib_or =
      FunctionLibrary::Builder()
          .SetWitInterface(kDemoWitInterface)
          .AddForeignComponent(
              "len", Prim(CelfnType::Kind::kInt),
              {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}})
          .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

// Host-side baselines — same overload-id shape, body is a C++ lambda.
FunctionLibrary BuildAddLibAsHost() {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("add", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                    CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}})
          .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

FunctionLibrary BuildLenLibAsHost() {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("len", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}})
          .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

Program CompileOrDie(
    absl::string_view src, const FunctionLibrary& lib,
    absl::Span<const std::pair<absl::string_view, CelType>> vars) {
  Compiler::Builder b;
  for (const auto& [name, ty] : vars) {
    b.DeclareVariable(std::string(name), ty);
  }
  b.AddLibrary(lib);
  auto c_or = std::move(b).Build();
  ABSL_CHECK_OK(c_or);
  CompilerOptions opts;
  opts.optimize_level = kBenchOptimizeLevel;
  auto p_or = c_or->Compile(src, opts);
  ABSL_CHECK_OK(p_or);
  return *std::move(p_or);
}

// Each BM owns its own Engine on the heap: AddComponent is per-Plan
// and Engine state interferes if shared.  Setup happens once before
// the timing loop starts so the per-iteration cost is just Eval().
std::unique_ptr<Engine> NewEngine() {
  auto e_or = Engine::NewBuilder().Build();
  ABSL_CHECK_OK(e_or);
  return std::make_unique<Engine>(*std::move(e_or));
}

Instance PlanOrDie(Engine& engine, const Program& prog) {
  auto i_or = engine.Plan(prog);
  ABSL_CHECK_OK(i_or);
  return *std::move(i_or);
}

// ══════════════════════════════════════════════════════════════════════
// Shape A — `add(int, int) -> int`.  Minimum-overhead dispatch.
// ══════════════════════════════════════════════════════════════════════

void BM_Eval_ForeignComponent_AddIntInt(benchmark::State& state) {
  auto lib = BuildAddLib();
  Program prog = CompileOrDie("add(a, b)", lib,
                              {{"a", CelType::Int()}, {"b", CelType::Int()}});
  auto engine = NewEngine();
  ABSL_CHECK_OK(engine->AddComponent(DemoComponentBytes(), lib));
  Instance inst = PlanOrDie(*engine, prog);
  Activation act;
  act.Bind("a", Value::Int(7));
  act.Bind("b", Value::Int(35));
  for (auto _ : state) {
    auto v_or = inst.Eval(act);
    ABSL_CHECK_OK(v_or);
    benchmark::DoNotOptimize(v_or->AsInt());
  }
}
BENCHMARK(BM_Eval_ForeignComponent_AddIntInt);

void BM_Eval_HostFn_AddIntInt(benchmark::State& state) {
  auto lib = BuildAddLibAsHost();
  Program prog = CompileOrDie("add(a, b)", lib,
                              {{"a", CelType::Int()}, {"b", CelType::Int()}});
  auto engine = NewEngine();
  ABSL_CHECK_OK(engine->AddTypedFunction(
      "add_int_int", [](int64_t a, int64_t b) -> absl::StatusOr<int64_t> {
        return a + b;
      }));
  Instance inst = PlanOrDie(*engine, prog);
  Activation act;
  act.Bind("a", Value::Int(7));
  act.Bind("b", Value::Int(35));
  for (auto _ : state) {
    auto v_or = inst.Eval(act);
    ABSL_CHECK_OK(v_or);
    benchmark::DoNotOptimize(v_or->AsInt());
  }
}
BENCHMARK(BM_Eval_HostFn_AddIntInt);

// ══════════════════════════════════════════════════════════════════════
// Shape B — `len(string) -> int`.  Adds the canonical-ABI string copy
// to the dispatch.  256 KiB payload picked to be in the same regime as
// the F.1 large-payload marshaling tests.
// ══════════════════════════════════════════════════════════════════════

constexpr size_t kBenchStringBytes = 256 * 1024;

// BM_Eval_ForeignComponent_LenString_256KiB stays disabled.  Even after
// engine.cc started stubbing wasi:random/random.get-random-bytes with
// a deterministic host fn (m26 #44 partial mitigation; lets the
// AddIntInt scalar shape pass cleanly), passing a 256 KiB string
// through the canonical-ABI copy still trips
// `wasm trap: cannot leave component instance` somewhere inside
// libc++'s post-RNG-init machinery.  See the matching SKIP reason on
// CelWasmComponentDemo.GreetRoundTripsString.  Un-skip when wasmtime
// exposes a real wasi-preview2 store context — or rebuild the demo
// against wasm32-wasi + the preview1 adapter so the existing
// `wasi.hh` WasiConfig satisfies libc++.

void BM_Eval_HostFn_LenString_256KiB(benchmark::State& state) {
  auto lib = BuildLenLibAsHost();
  Program prog = CompileOrDie("len(s)", lib, {{"s", CelType::String()}});
  auto engine = NewEngine();
  ABSL_CHECK_OK(engine->AddTypedFunction(
      "len_string", [](absl::string_view s) -> absl::StatusOr<int64_t> {
        return static_cast<int64_t>(s.size());
      }));
  Instance inst = PlanOrDie(*engine, prog);
  const std::string payload(kBenchStringBytes, 'x');
  Activation act;
  act.Bind("s", Value::String(payload));
  for (auto _ : state) {
    auto v_or = inst.Eval(act);
    ABSL_CHECK_OK(v_or);
    benchmark::DoNotOptimize(v_or->AsInt());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kBenchStringBytes));
}
BENCHMARK(BM_Eval_HostFn_LenString_256KiB);

}  // namespace
}  // namespace celwasm

int main(int argc, char** argv) {
  // Capture argv[0] for the runfiles loader before Google Benchmark
  // consumes it.  bazel run sets argv[0] to the absolute path of the
  // binary, which is what Runfiles::Create needs to locate the
  // `<binary>.runfiles/` tree.
  if (argc > 0) celwasm::Argv0() = argv[0];
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
