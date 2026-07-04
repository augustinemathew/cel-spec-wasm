// e2e: `ResourceLimits` enforcement on the `@component` path — the
// wall-clock eval deadline and the linear-memory cap that make loading
// an untrusted component safe (see `doc/user-guide/security-model.md`
// and `eval/resource_limits.h`).
//
// These are the behavioral pins for the security-model promise that a
// `@component` "cannot hang the host": a component is arbitrary guest
// wasm — unlike a total CEL expression it CAN loop forever or grow
// memory without bound — so the tests drive exactly those two attacks
// through the public `Engine::AddComponent` path and assert they fail
// cleanly (a `ResourceExhausted` status / a failed allocation) rather
// than hanging or OOMing the process.
//
// Self-contained: inline component-model WAT assembled by
// `wasmtime_wat2wasm`, no wit-bindgen / wasi-sdk.  Uses the dynamic
// link mode (default `Compile`) since resource limits are engine-level
// and link-mode-independent.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/resource_limits.h"
#include "eval/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

std::vector<uint8_t> WatToWasm(absl::string_view wat) {
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
  if (err != nullptr) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string err_str(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    ABSL_CHECK(false) << "wat2wasm failed: " << err_str;
  }
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

FunctionLibrary OneIntFnLib(absl::string_view fn_name) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent(
                        fn_name, Prim(CelfnType::Kind::kInt),
                        {CelfnParam{false, Prim(CelfnType::Kind::kInt), "x"}})
                    .Build();
  ABSL_CHECK(lib_or.ok()) << lib_or.status();
  return *std::move(lib_or);
}

// `spin-int : s64 -> s64` whose core body loops forever — the epoch
// deadline check at the loop back-edge is what must trap it.
constexpr absl::string_view kSpinComponentWat = R"WAT(
(component
  (core module $m
    (func (export "spin") (param i64) (result i64)
      (loop $l br $l)
      unreachable))
  (core instance $i (instantiate $m))
  (func (export "spin-int") (param "x" s64) (result s64)
    (canon lift (core func $i "spin"))))
)WAT";

// `grow-int : s64 -> s64` — grows the component's own linear memory by
// 2000 pages (~128 MiB) and returns the `memory.grow` result (the
// previous size in pages, or -1 if the growth was refused by the store
// limiter).  Lets a test observe the memory cap deterministically.
constexpr absl::string_view kGrowComponentWat = R"WAT(
(component
  (core module $m
    (memory 1)
    (func (export "grow") (param i64) (result i64)
      (i64.extend_i32_s (memory.grow (i32.const 2000)))))
  (core instance $i (instantiate $m))
  (func (export "grow-int") (param "x" s64) (result s64)
    (canon lift (core func $i "grow"))))
)WAT";

// A fast, well-behaved `add-one-int : s64 -> s64` — proves the limits
// do not false-trip on a normal call.
constexpr absl::string_view kAddOneComponentWat = R"WAT(
(component
  (core module $m
    (func (export "addone") (param i64) (result i64)
      local.get 0 (i64.const 1) i64.add))
  (core instance $i (instantiate $m))
  (func (export "addone-int") (param "x" s64) (result s64)
    (canon lift (core func $i "addone"))))
)WAT";

// Compile `<fn>(x)` against `lib`, Plan on `engine`, Eval with x=1.
absl::StatusOr<Value> EvalOneArg(Engine& engine, const FunctionLibrary& lib,
                                 absl::string_view call_expr) {
  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("x", CelType::Int()).AddLibrary(lib);
  auto compiler_or = std::move(builder).Build();
  if (!compiler_or.ok()) return compiler_or.status();
  auto prog_or = compiler_or->Compile(call_expr);
  if (!prog_or.ok()) return prog_or.status();
  auto inst_or = engine.Plan(*prog_or);
  if (!inst_or.ok()) return inst_or.status();
  Activation act;
  act.Bind("x", Value::Int(1));
  return inst_or->Eval(act);
}

// ——— deadline ———

TEST(ComponentResourceLimits, InfiniteLoopComponentTrapsAtDeadline) {
  ResourceLimits limits;
  limits.max_eval_time = absl::Milliseconds(100);
  auto engine_or = Engine::NewBuilder().WithResourceLimits(limits).Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneIntFnLib("spin");
  ASSERT_THAT(engine_or->AddComponent(WatToWasm(kSpinComponentWat), lib),
              IsOk());

  // The load-bearing assertion is that this call RETURNS at all (the
  // deadline fires ~100ms in); a regression that dropped the deadline
  // would hang here forever.
  auto v_or = EvalOneArg(*engine_or, lib, "spin(x)");
  ASSERT_FALSE(v_or.ok()) << "infinite-loop component produced a value";
  EXPECT_EQ(v_or.status().code(), absl::StatusCode::kResourceExhausted)
      << v_or.status();
}

TEST(ComponentResourceLimits, DefaultDeadlineDoesNotTripFastComponent) {
  // Default limits (1s deadline) must not false-trip a normal call.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneIntFnLib("addone");
  ASSERT_THAT(engine_or->AddComponent(WatToWasm(kAddOneComponentWat), lib),
              IsOk());

  auto v_or = EvalOneArg(*engine_or, lib, "addone(x)");
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v_or->AsInt(), 2);
}

TEST(ComponentResourceLimits, UnlimitedOptOutStillEvaluatesNormally) {
  // Unlimited() disables the deadline (no timer thread) and the memory
  // cap; a well-behaved component still works.  (We cannot assert the
  // infinite-loop case here — with no deadline it would hang.)
  auto engine_or = Engine::NewBuilder()
                       .WithResourceLimits(ResourceLimits::Unlimited())
                       .Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneIntFnLib("addone");
  ASSERT_THAT(engine_or->AddComponent(WatToWasm(kAddOneComponentWat), lib),
              IsOk());

  auto v_or = EvalOneArg(*engine_or, lib, "addone(x)");
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 2);
}

// ——— memory cap ———

TEST(ComponentResourceLimits, MemoryCapRefusesOversizedComponentGrowth) {
  // A 4 MiB per-memory cap must refuse the component's 128 MiB
  // memory.grow: the guest sees the -1 failure sentinel rather than
  // the host allocating 128 MiB.
  ResourceLimits limits;
  limits.max_memory_bytes = uint64_t{4} << 20;  // 4 MiB
  auto engine_or = Engine::NewBuilder().WithResourceLimits(limits).Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneIntFnLib("grow");
  ASSERT_THAT(engine_or->AddComponent(WatToWasm(kGrowComponentWat), lib),
              IsOk());

  auto v_or = EvalOneArg(*engine_or, lib, "grow(x)");
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v_or->AsInt(), -1) << "growth past the cap should be refused";
}

TEST(ComponentResourceLimits, UnlimitedMemoryAllowsLargeComponentGrowth) {
  // With no memory cap the same 128 MiB growth succeeds (memory.grow
  // returns the previous page count, >= 0).
  auto engine_or = Engine::NewBuilder()
                       .WithResourceLimits(ResourceLimits::Unlimited())
                       .Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneIntFnLib("grow");
  ASSERT_THAT(engine_or->AddComponent(WatToWasm(kGrowComponentWat), lib),
              IsOk());

  auto v_or = EvalOneArg(*engine_or, lib, "grow(x)");
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kInt);
  EXPECT_GE(*v_or->AsInt(), 0) << "growth under no cap should succeed";
}

}  // namespace
}  // namespace celwasm
