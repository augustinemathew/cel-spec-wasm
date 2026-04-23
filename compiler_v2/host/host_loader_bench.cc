// Micro-benchmarks that attribute wall time to each sub-step of
// `EvalInstance::Create` + `CallEval`.  Used to pick the sharing
// boundary for the upcoming `EvalEngine` abstraction (see
// `doc/implementation-plan/` — or the in-tree plan that spawned this
// commit): we only promote a step to engine-level ownership if the
// numbers here say it's worth sharing.
//
// Run:
//   bazel run -c opt //compiler_v2/host:host_loader_bench -- \
//       --benchmark_min_time=1s
//
// Each per-step bench uses the same trivially-compiled expr (`"42"`)
// so expr compile cost is negligible and the wasmtime stages dominate
// the numbers.  Per-step benches reuse the linker helpers inline
// rather than calling production code in an anonymous namespace;
// keeping them local makes this file a self-contained measurement
// tool.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "benchmark/benchmark.h"
#include "compiler_v2/compile.h"
#include "compiler_v2/host/cel_log.h"
#include "compiler_v2/host/host_loader.h"
#include "compiler_v2/runtime/cel_runtime_wasm_bytes.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// Pre-compile a stable expr once at process start so the benches
// aren't dominated by the compile pipeline.
const std::vector<uint8_t>& ExprBytes() {
  static const std::vector<uint8_t>* bytes = []() {
    auto artifact = Compile("42");
    ABSL_CHECK(artifact.ok()) << artifact.status();
    return new std::vector<uint8_t>(std::move(artifact->wasm_bytes));
  }();
  return *bytes;
}

absl::Span<const uint8_t> RuntimeBytes() {
  return absl::MakeConstSpan(
      reinterpret_cast<const uint8_t*>(kCelRuntimeWasmBytes),
      kCelRuntimeWasmBytesSize);
}

// No-op trampolines just so the linker registrations have the right
// signatures.  The step benches that exercise these don't actually
// invoke $eval; they measure setup cost only.
wasm_trap_t* NoopTrampoline(void*, wasmtime_caller_t*, const wasmtime_val_t*,
                            size_t, wasmtime_val_t*, size_t) {
  return nullptr;
}

wasm_functype_t* I32I32ToVoid() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* ps[2] = {wasm_valtype_new(WASM_I32),
                           wasm_valtype_new(WASM_I32)};
  wasm_valtype_vec_new(&params, 2, ps);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

wasm_functype_t* I32ToI32() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* ps[1] = {wasm_valtype_new(WASM_I32)};
  wasm_valtype_t* rs[1] = {wasm_valtype_new(WASM_I32)};
  wasm_valtype_vec_new(&params, 1, ps);
  wasm_valtype_vec_new(&results, 1, rs);
  return wasm_functype_new(&params, &results);
}

// Mirrors `host_loader.cc::InstallHostImports` byte-for-byte on
// signatures/names so the bench measures representative work, not a
// cheaper version of it.
void InstallHostImports(wasmtime_linker_t* linker) {
  {
    wasm_functype_t* t = I32I32ToVoid();
    wasmtime_error_t* err = wasmtime_linker_define_func(
        linker, "cel", 3, "cel_reset", 9, t, NoopTrampoline, nullptr, nullptr);
    wasm_functype_delete(t);
    ABSL_CHECK(err == nullptr);
  }
  {
    wasm_functype_t* t = I32ToI32();
    wasmtime_error_t* err = wasmtime_linker_define_func(
        linker, "cel", 3, "cel_alloc", 9, t, NoopTrampoline, nullptr, nullptr);
    wasm_functype_delete(t);
    ABSL_CHECK(err == nullptr);
  }
  ABSL_CHECK(RegisterCelLog(linker).ok());
}

// ———— End-to-end baselines ————

void BM_FullCreate(benchmark::State& state) {
  const auto& bytes = ExprBytes();
  for (auto _ : state) {
    auto inst = EvalInstance::Create(bytes);
    ABSL_CHECK(inst.ok()) << inst.status();
    benchmark::DoNotOptimize(inst);
  }
}
BENCHMARK(BM_FullCreate);

void BM_CallEval_Only(benchmark::State& state) {
  auto inst_or = EvalInstance::Create(ExprBytes());
  ABSL_CHECK(inst_or.ok()) << inst_or.status();
  auto inst = std::move(*inst_or);
  for (auto _ : state) {
    auto off = inst.CallEval();
    ABSL_CHECK(off.ok()) << off.status();
    benchmark::DoNotOptimize(off);
  }
}
BENCHMARK(BM_CallEval_Only);

// ———— Per-step benches ————

void BM_Step_EngineNew(benchmark::State& state) {
  for (auto _ : state) {
    wasm_engine_t* e = wasm_engine_new();
    benchmark::DoNotOptimize(e);
    wasm_engine_delete(e);
  }
}
BENCHMARK(BM_Step_EngineNew);

void BM_Step_StoreNew(benchmark::State& state) {
  wasm_engine_t* engine = wasm_engine_new();
  for (auto _ : state) {
    wasmtime_store_t* s = wasmtime_store_new(engine, nullptr, nullptr);
    benchmark::DoNotOptimize(s);
    wasmtime_store_delete(s);
  }
  wasm_engine_delete(engine);
}
BENCHMARK(BM_Step_StoreNew);

void BM_Step_LinkerNew_WithTrampolines(benchmark::State& state) {
  wasm_engine_t* engine = wasm_engine_new();
  for (auto _ : state) {
    wasmtime_linker_t* l = wasmtime_linker_new(engine);
    InstallHostImports(l);
    benchmark::DoNotOptimize(l);
    wasmtime_linker_delete(l);
  }
  wasm_engine_delete(engine);
}
BENCHMARK(BM_Step_LinkerNew_WithTrampolines);

void BM_Step_ModuleNew_Expr(benchmark::State& state) {
  wasm_engine_t* engine = wasm_engine_new();
  const auto& bytes = ExprBytes();
  for (auto _ : state) {
    wasmtime_module_t* m = nullptr;
    wasmtime_error_t* err =
        wasmtime_module_new(engine, bytes.data(), bytes.size(), &m);
    ABSL_CHECK(err == nullptr);
    benchmark::DoNotOptimize(m);
    wasmtime_module_delete(m);
  }
  wasm_engine_delete(engine);
}
BENCHMARK(BM_Step_ModuleNew_Expr);

void BM_Step_ModuleNew_Runtime(benchmark::State& state) {
  wasm_engine_t* engine = wasm_engine_new();
  const auto bytes = RuntimeBytes();
  for (auto _ : state) {
    wasmtime_module_t* m = nullptr;
    wasmtime_error_t* err =
        wasmtime_module_new(engine, bytes.data(), bytes.size(), &m);
    ABSL_CHECK(err == nullptr);
    benchmark::DoNotOptimize(m);
    wasmtime_module_delete(m);
  }
  wasm_engine_delete(engine);
}
BENCHMARK(BM_Step_ModuleNew_Runtime);

// Instantiate-expr bench: shares engine + expr module + linker
// (with trampolines already installed) across iterations; fresh
// store per iteration.  Mirrors what we'd pay per CreateInstance if
// those three were engine-owned.
void BM_Step_InstantiateExpr(benchmark::State& state) {
  wasm_engine_t* engine = wasm_engine_new();
  wasmtime_linker_t* linker = wasmtime_linker_new(engine);
  InstallHostImports(linker);
  wasmtime_module_t* module = nullptr;
  const auto& bytes = ExprBytes();
  ABSL_CHECK(wasmtime_module_new(engine, bytes.data(), bytes.size(), &module) ==
             nullptr);
  for (auto _ : state) {
    wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasmtime_instance_t inst;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_linker_instantiate(linker, ctx, module, &inst, &trap);
    ABSL_CHECK(err == nullptr && trap == nullptr);
    benchmark::DoNotOptimize(inst);
    wasmtime_store_delete(store);
  }
  wasmtime_module_delete(module);
  wasmtime_linker_delete(linker);
  wasm_engine_delete(engine);
}
BENCHMARK(BM_Step_InstantiateExpr);

// Instantiate-runtime bench: must first instantiate the expr module
// (to get an exported `memory` to bind into the linker) before
// instantiating the runtime.  Per iteration we build the whole pair
// in a fresh store so the bench reflects what production does
// minus the engine/linker/module creation we already measured.
void BM_Step_InstantiateRuntime(benchmark::State& state) {
  wasm_engine_t* engine = wasm_engine_new();
  wasmtime_linker_t* linker = wasmtime_linker_new(engine);
  InstallHostImports(linker);
  wasmtime_module_t* expr_mod = nullptr;
  wasmtime_module_t* rt_mod = nullptr;
  {
    const auto& bytes = ExprBytes();
    ABSL_CHECK(wasmtime_module_new(engine, bytes.data(), bytes.size(),
                                   &expr_mod) == nullptr);
    const auto rbytes = RuntimeBytes();
    ABSL_CHECK(wasmtime_module_new(engine, rbytes.data(), rbytes.size(),
                                   &rt_mod) == nullptr);
  }
  for (auto _ : state) {
    wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasmtime_instance_t expr_inst;
    wasm_trap_t* trap = nullptr;
    ABSL_CHECK(wasmtime_linker_instantiate(linker, ctx, expr_mod, &expr_inst,
                                           &trap) == nullptr &&
               trap == nullptr);
    // Bind expr's memory under cel.memory so the runtime instantiates.
    wasmtime_extern_t mem_ext;
    ABSL_CHECK(
        wasmtime_instance_export_get(ctx, &expr_inst, "memory", 6, &mem_ext));
    ABSL_CHECK(wasmtime_linker_define(linker, ctx, "cel", 3, "memory", 6,
                                      &mem_ext) == nullptr);
    wasmtime_instance_t rt_inst;
    ABSL_CHECK(wasmtime_linker_instantiate(linker, ctx, rt_mod, &rt_inst,
                                           &trap) == nullptr &&
               trap == nullptr);
    benchmark::DoNotOptimize(rt_inst);
    wasmtime_store_delete(store);
  }
  wasmtime_module_delete(rt_mod);
  wasmtime_module_delete(expr_mod);
  wasmtime_linker_delete(linker);
  wasm_engine_delete(engine);
}
BENCHMARK(BM_Step_InstantiateRuntime);

// ———— Composite lower-bounds ————

// Share just the engine; rebuild everything else per iteration.
void BM_WithSharedEngine(benchmark::State& state) {
  wasm_engine_t* engine = wasm_engine_new();
  const auto& expr_bytes = ExprBytes();
  const auto rt_bytes = RuntimeBytes();
  for (auto _ : state) {
    wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasmtime_linker_t* linker = wasmtime_linker_new(engine);
    InstallHostImports(linker);
    wasmtime_module_t* expr_mod = nullptr;
    ABSL_CHECK(wasmtime_module_new(engine, expr_bytes.data(),
                                   expr_bytes.size(), &expr_mod) == nullptr);
    wasmtime_instance_t expr_inst;
    wasm_trap_t* trap = nullptr;
    ABSL_CHECK(wasmtime_linker_instantiate(linker, ctx, expr_mod, &expr_inst,
                                           &trap) == nullptr &&
               trap == nullptr);
    wasmtime_extern_t mem_ext;
    ABSL_CHECK(
        wasmtime_instance_export_get(ctx, &expr_inst, "memory", 6, &mem_ext));
    ABSL_CHECK(wasmtime_linker_define(linker, ctx, "cel", 3, "memory", 6,
                                      &mem_ext) == nullptr);
    wasmtime_module_t* rt_mod = nullptr;
    ABSL_CHECK(wasmtime_module_new(engine, rt_bytes.data(), rt_bytes.size(),
                                   &rt_mod) == nullptr);
    wasmtime_instance_t rt_inst;
    ABSL_CHECK(wasmtime_linker_instantiate(linker, ctx, rt_mod, &rt_inst,
                                           &trap) == nullptr &&
               trap == nullptr);
    benchmark::DoNotOptimize(rt_inst);
    wasmtime_module_delete(rt_mod);
    wasmtime_module_delete(expr_mod);
    wasmtime_linker_delete(linker);
    wasmtime_store_delete(store);
  }
  wasm_engine_delete(engine);
}
BENCHMARK(BM_WithSharedEngine);

// Share engine AND parsed runtime module; everything else
// per iteration.  This is the lower bound if Phase 2 decides to
// cache the parsed runtime module on the engine.
void BM_WithSharedEngineAndRuntimeModule(benchmark::State& state) {
  wasm_engine_t* engine = wasm_engine_new();
  const auto& expr_bytes = ExprBytes();
  const auto rt_bytes = RuntimeBytes();
  wasmtime_module_t* shared_rt_mod = nullptr;
  ABSL_CHECK(wasmtime_module_new(engine, rt_bytes.data(), rt_bytes.size(),
                                 &shared_rt_mod) == nullptr);
  for (auto _ : state) {
    wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasmtime_linker_t* linker = wasmtime_linker_new(engine);
    InstallHostImports(linker);
    wasmtime_module_t* expr_mod = nullptr;
    ABSL_CHECK(wasmtime_module_new(engine, expr_bytes.data(),
                                   expr_bytes.size(), &expr_mod) == nullptr);
    wasmtime_instance_t expr_inst;
    wasm_trap_t* trap = nullptr;
    ABSL_CHECK(wasmtime_linker_instantiate(linker, ctx, expr_mod, &expr_inst,
                                           &trap) == nullptr &&
               trap == nullptr);
    wasmtime_extern_t mem_ext;
    ABSL_CHECK(
        wasmtime_instance_export_get(ctx, &expr_inst, "memory", 6, &mem_ext));
    ABSL_CHECK(wasmtime_linker_define(linker, ctx, "cel", 3, "memory", 6,
                                      &mem_ext) == nullptr);
    wasmtime_instance_t rt_inst;
    ABSL_CHECK(wasmtime_linker_instantiate(linker, ctx, shared_rt_mod, &rt_inst,
                                           &trap) == nullptr &&
               trap == nullptr);
    benchmark::DoNotOptimize(rt_inst);
    wasmtime_module_delete(expr_mod);
    wasmtime_linker_delete(linker);
    wasmtime_store_delete(store);
  }
  wasmtime_module_delete(shared_rt_mod);
  wasm_engine_delete(engine);
}
BENCHMARK(BM_WithSharedEngineAndRuntimeModule);

}  // namespace
}  // namespace celwasm

BENCHMARK_MAIN();
