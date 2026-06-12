// Wasm↔host call-boundary microbenches — the experiment behind the
// "typed/unchecked wasmtime calls" optimization candidate
// (benchmark/ANALYSIS.md, candidate P0/P5).
//
// Measures the SAME logical call four ways, isolating wasmtime's
// calling-convention cost from everything celwasm adds on top:
//
//   wasm→host (the trampoline exit path every cel_host.* call takes):
//     - BM_WasmToHost_Boxed      — wasmtime_linker_define_func: each
//                                  call boxes 4 i32 params + 1 result
//                                  through wasmtime_val_t (today's
//                                  production path, eval/internal/
//                                  cel_host_wasmtime.cc::DefineHostFunc).
//     - BM_WasmToHost_Unchecked  — wasmtime_linker_define_func_unchecked:
//                                  params/results live in a raw
//                                  wasmtime_val_raw_t array, no boxing.
//
//   host→wasm (the per-Eval entry path Instance::Eval takes):
//     - BM_HostToWasm_Checked    — wasmtime_func_call (today's path:
//                                  per-call type/arity validation).
//     - BM_HostToWasm_Unchecked  — wasmtime_func_call_unchecked.
//
// Each wasm→host cell drives a wasm loop that makes kInnerCalls
// imported-function calls per outer invocation, so the reported
// number divided by kInnerCalls is the per-call boundary cost; the
// `ns/call` figure is stamped on the result label.
//
// The delta between Boxed and Unchecked is the upper bound on what
// retargeting DefineHostFunc at the unchecked ABI can save per host
// call; Checked vs Unchecked bounds the Instance::Eval entry saving.
//
// `manual`-tagged; run explicitly, always -c opt:
//   bazel run -c opt //benchmark/boundary:wasmtime_call_bench

#include <cstdint>
#include <cstring>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "benchmark/benchmark.h"
#include "wasm.h"
#include "wasmtime.h"

namespace {

// Inner wasm-loop trip count per timed outer call.  Large enough that
// the outer host→wasm entry cost amortises to noise in the wasm→host
// cells.
constexpr int kInnerCalls = 1000;

// (func $f (import "host" "f") (param i32 i32 i32 i32) (result i32))
// mirrors the dominant cel_host trampoline shape (cel_get_field:
// four i32 scalars in, one i32 out).
constexpr char kWat[] = R"WAT(
(module
  (import "host" "f" (func $f (param i32 i32 i32 i32) (result i32)))
  (func (export "loop") (param $n i32) (result i32)
    (local $acc i32)
    (block $done
      (loop $top
        (br_if $done (i32.eqz (local.get $n)))
        (local.set $acc
          (i32.add (local.get $acc)
                   (call $f (local.get $n) (i32.const 2)
                            (i32.const 3) (i32.const 4))))
        (local.set $n (i32.sub (local.get $n) (i32.const 1)))
        (br $top)))
    (local.get $acc))
  (func (export "add") (param i32 i32) (result i32)
    (i32.add (local.get 0) (local.get 1))))
)WAT";

// Host implementations of "host.f" — identical arithmetic, two ABIs.
wasm_trap_t* BoxedHostF(void* /*env*/, wasmtime_caller_t* /*caller*/,
                        const wasmtime_val_t* args, size_t /*nargs*/,
                        wasmtime_val_t* results, size_t /*nresults*/) {
  results[0].kind = WASMTIME_I32;
  results[0].of.i32 =
      args[0].of.i32 + args[1].of.i32 + args[2].of.i32 + args[3].of.i32;
  return nullptr;
}

wasm_trap_t* UncheckedHostF(void* /*env*/, wasmtime_caller_t* /*caller*/,
                            wasmtime_val_raw_t* args_and_results,
                            size_t /*num_args_and_results*/) {
  args_and_results[0].i32 = args_and_results[0].i32 + args_and_results[1].i32 +
                            args_and_results[2].i32 + args_and_results[3].i32;
  return nullptr;
}

// One self-contained engine+store+instance per registration flavor.
struct Fixture {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_context_t* ctx = nullptr;
  wasmtime_func_t loop_fn{};
  wasmtime_func_t add_fn{};
};

wasm_functype_t* FourI32sToI32() {
  wasm_valtype_t* params[4];
  for (auto*& p : params) p = wasm_valtype_new(WASM_I32);
  wasm_valtype_t* results[1] = {wasm_valtype_new(WASM_I32)};
  wasm_valtype_vec_t params_vec;
  wasm_valtype_vec_t results_vec;
  wasm_valtype_vec_new(&params_vec, 4, params);
  wasm_valtype_vec_new(&results_vec, 1, results);
  return wasm_functype_new(&params_vec, &results_vec);
}

void DefineHostF(wasmtime_linker_t* linker, bool unchecked) {
  wasm_functype_t* ty = FourI32sToI32();
  wasmtime_error_t* err = nullptr;
  if (unchecked) {
    err = wasmtime_linker_define_func_unchecked(
        linker, "host", 4, "f", 1, ty, &UncheckedHostF, /*data=*/nullptr,
        /*finalizer=*/nullptr);
  } else {
    err = wasmtime_linker_define_func(linker, "host", 4, "f", 1, ty,
                                      &BoxedHostF, /*data=*/nullptr,
                                      /*finalizer=*/nullptr);
  }
  wasm_functype_delete(ty);
  ABSL_CHECK(err == nullptr) << "define host.f failed";
}

wasmtime_func_t LookupFunc(wasmtime_context_t* ctx,
                           wasmtime_instance_t* instance, const char* name) {
  wasmtime_extern_t ext;
  ABSL_CHECK(wasmtime_instance_export_get(ctx, instance, name,
                                          std::strlen(name), &ext))
      << "export " << name << " not found";
  ABSL_CHECK(ext.kind == WASMTIME_EXTERN_FUNC);
  return ext.of.func;
}

// Builds the engine/store/instance with host.f registered via the
// requested ABI and the two exports resolved.  Leaks on purpose: one
// fixture per registration flavor for the process lifetime.
Fixture* MakeFixture(bool unchecked) {
  auto* fx = new Fixture();
  fx->engine = wasm_engine_new();
  fx->store = wasmtime_store_new(fx->engine, nullptr, nullptr);
  fx->ctx = wasmtime_store_context(fx->store);

  wasm_byte_vec_t wasm_bytes;
  wasmtime_error_t* err =
      wasmtime_wat2wasm(kWat, sizeof(kWat) - 1, &wasm_bytes);
  ABSL_CHECK(err == nullptr) << "wat2wasm failed";

  wasmtime_module_t* module = nullptr;
  err = wasmtime_module_new(fx->engine,
                            reinterpret_cast<uint8_t*>(wasm_bytes.data),
                            wasm_bytes.size, &module);
  wasm_byte_vec_delete(&wasm_bytes);
  ABSL_CHECK(err == nullptr && module != nullptr) << "module_new failed";

  wasmtime_linker_t* linker = wasmtime_linker_new(fx->engine);
  DefineHostF(linker, unchecked);

  wasmtime_instance_t instance;
  wasm_trap_t* trap = nullptr;
  err = wasmtime_linker_instantiate(linker, fx->ctx, module, &instance, &trap);
  ABSL_CHECK(err == nullptr && trap == nullptr) << "instantiate failed";

  fx->loop_fn = LookupFunc(fx->ctx, &instance, "loop");
  fx->add_fn = LookupFunc(fx->ctx, &instance, "add");
  wasmtime_linker_delete(linker);
  wasmtime_module_delete(module);
  return fx;
}

Fixture& BoxedFixture() {
  static Fixture* fx = MakeFixture(/*unchecked=*/false);
  return *fx;
}

Fixture& UncheckedFixture() {
  static Fixture* fx = MakeFixture(/*unchecked=*/true);
  return *fx;
}

// ─── wasm→host: per-imported-call cost under each registration ABI ───

int32_t CallLoop(Fixture& fx) {
  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = kInnerCalls;
  wasmtime_val_t result;
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(fx.ctx, &fx.loop_fn, &arg, 1, &result, 1, &trap);
  ABSL_CHECK(err == nullptr && trap == nullptr) << "loop call failed";
  return result.of.i32;
}

void RunWasmToHost(benchmark::State& state, Fixture& fx) {
  int32_t sink = 0;
  for ([[maybe_unused]] auto _ : state) {
    sink = CallLoop(fx);
    benchmark::DoNotOptimize(sink);
  }
  state.SetLabel(absl::StrCat("result=", sink, "; divide time by ",
                              kInnerCalls, " for ns/call"));
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(kInnerCalls));
}

void BM_WasmToHost_Boxed(benchmark::State& state) {
  RunWasmToHost(state, BoxedFixture());
}
BENCHMARK(BM_WasmToHost_Boxed)->Unit(benchmark::kMicrosecond);

void BM_WasmToHost_Unchecked(benchmark::State& state) {
  RunWasmToHost(state, UncheckedFixture());
}
BENCHMARK(BM_WasmToHost_Unchecked)->Unit(benchmark::kMicrosecond);

// ─── host→wasm: per-entry cost, checked vs unchecked invocation ───

void BM_HostToWasm_Checked(benchmark::State& state) {
  Fixture& fx = BoxedFixture();
  wasmtime_val_t args[2];
  args[0].kind = WASMTIME_I32;
  args[0].of.i32 = 20;
  args[1].kind = WASMTIME_I32;
  args[1].of.i32 = 22;
  wasmtime_val_t result;
  for ([[maybe_unused]] auto _ : state) {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_func_call(fx.ctx, &fx.add_fn, args, 2, &result, 1, &trap);
    ABSL_CHECK(err == nullptr && trap == nullptr);
    benchmark::DoNotOptimize(result.of.i32);
  }
  state.SetLabel(absl::StrCat("result=", result.of.i32));
}
BENCHMARK(BM_HostToWasm_Checked)->Unit(benchmark::kNanosecond);

void BM_HostToWasm_Unchecked(benchmark::State& state) {
  Fixture& fx = BoxedFixture();
  wasmtime_val_raw_t raw[2];
  int32_t out = 0;
  for ([[maybe_unused]] auto _ : state) {
    raw[0].i32 = 20;
    raw[1].i32 = 22;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_func_call_unchecked(fx.ctx, &fx.add_fn, raw, 2, &trap);
    ABSL_CHECK(err == nullptr && trap == nullptr);
    out = raw[0].i32;
    benchmark::DoNotOptimize(out);
  }
  state.SetLabel(absl::StrCat("result=", out));
}
BENCHMARK(BM_HostToWasm_Unchecked)->Unit(benchmark::kNanosecond);

}  // namespace

BENCHMARK_MAIN();
