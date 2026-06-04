// Foreign-component vs native host-fn dispatch cost — m24 D.3.
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
//   - Foreign-component (m24 path) — a tiny component is registered via
//     `Engine::AddComponent` and dispatched per call through
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
//   bazel run -c opt //bench:foreign_component_bench
//   bazel run -c opt //bench:foreign_component_bench -- \
//       --benchmark_filter=BM_Eval_ForeignComponent \
//       --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
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
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

constexpr int kBenchOptimizeLevel = 2;

// Per-shape WATs — both use the explicit `alias core export` form for
// canon-options (the inline `(core func $i "name")` is rejected by
// wasmtime_wat2wasm; settled at e2e/foreign_component_dispatch_test).

constexpr absl::string_view kAddIntIntComponentWat = R"WAT(
(component
  (core module $m
    (func (export "add") (param i64 i64) (result i64)
      local.get 0 local.get 1 i64.add))
  (core instance $i (instantiate $m))
  (func (export "add-int-int") (param "a" s64) (param "b" s64) (result s64)
    (canon lift (core func $i "add"))))
)WAT";

constexpr absl::string_view kLenStringComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      global.get $next
      local.set $ret
      global.get $next
      local.get $new_sz
      i32.add
      global.set $next
      local.get $ret)
    (func (export "len_fn") (param $ptr i32) (param $len i32) (result i64)
      local.get $len
      i64.extend_i32_u))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "len_fn" (core func $len_fn))
  (func (export "len-string") (param "s" string) (result s64)
    (canon lift (core func $len_fn) (memory $mem) (realloc $realloc))))
)WAT";

std::vector<uint8_t> WatToWasm(absl::string_view wat) {
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
  ABSL_CHECK_EQ(err, nullptr) << "wat2wasm failed";
  std::vector<uint8_t> bytes(
      reinterpret_cast<const uint8_t*>(out.data),
      reinterpret_cast<const uint8_t*>(out.data) + out.size);
  wasm_byte_vec_delete(&out);
  return bytes;
}

CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}

// Builds a single-decl library for the named fn.
FunctionLibrary BuildAddLib() {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent("add", Prim(CelfnType::Kind::kInt),
                               {CelfnParam{false, Prim(CelfnType::Kind::kInt),
                                           "a"},
                                CelfnParam{false, Prim(CelfnType::Kind::kInt),
                                           "b"}})
          .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

FunctionLibrary BuildLenLib() {
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent(
                        "len", Prim(CelfnType::Kind::kInt),
                        {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}})
                    .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

// Same shape as BuildAddLib but registered as a host fn — the dispatch
// still flows through `kCelFn`, so the native lambda is the only delta.
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

Program CompileOrDie(absl::string_view src, const FunctionLibrary& lib,
                     absl::Span<const std::pair<absl::string_view, CelType>>
                         vars) {
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
  Program prog = CompileOrDie(
      "add(a, b)", lib,
      {{"a", CelType::Int()}, {"b", CelType::Int()}});
  auto engine = NewEngine();
  ABSL_CHECK_OK(
      engine->AddComponent(WatToWasm(kAddIntIntComponentWat), lib));
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
  Program prog = CompileOrDie(
      "add(a, b)", lib,
      {{"a", CelType::Int()}, {"b", CelType::Int()}});
  auto engine = NewEngine();
  ABSL_CHECK_OK(engine->AddTypedFunction(
      "add_int_int",
      [](int64_t a, int64_t b) -> absl::StatusOr<int64_t> { return a + b; }));
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

void BM_Eval_ForeignComponent_LenString_256KiB(benchmark::State& state) {
  auto lib = BuildLenLib();
  Program prog = CompileOrDie("len(s)", lib, {{"s", CelType::String()}});
  auto engine = NewEngine();
  ABSL_CHECK_OK(
      engine->AddComponent(WatToWasm(kLenStringComponentWat), lib));
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
BENCHMARK(BM_Eval_ForeignComponent_LenString_256KiB);

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

BENCHMARK_MAIN();
