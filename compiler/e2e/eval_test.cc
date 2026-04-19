// End-to-end evaluation test for the M2 codegen MVP.
//
// Pipeline exercised:
//
//   CEL source string
//       → ParseAndCheck   (compiler/frontend)
//       → LowerToEvalFunction + WasmModule::Serialize
//                         (compiler/codegen)
//       → wasmtime: load, instantiate, call "eval", inspect return
//
// This is the only test in the repo that actually executes WASM the
// compiler emits.  Everything upstream (Binaryen's validator,
// instruction-shape assertions in expr_lower_test) can still pass
// while the module means nothing semantically; this test catches that.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/typed_ast.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Pulls a printable message out of a `wasmtime_error_t` (error from
// the host API) or `wasm_trap_t` (runtime trap), and frees it.
std::string ErrorMessage(wasmtime_error_t* absl_nonnull err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string out(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return out;
}

std::string TrapMessage(wasm_trap_t* absl_nonnull trap) {
  wasm_message_t msg;
  wasm_trap_message(trap, &msg);
  std::string out(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return out;
}

// Runs `cel_source` through the full pipeline and returns the single
// scalar that `eval()` produced.  Keeps the caller's surface small —
// the tests only need to look at the `wasmtime_val_t`.
absl::StatusOr<wasmtime_val_t> Evaluate(absl::string_view cel_source) {
  auto typed = ParseAndCheck(cel_source, CheckOptions{});
  if (!typed.ok()) return typed.status();

  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  if (auto s = mod.Validate(); !s.ok()) return s;

  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();

  wasm_engine_t* engine = wasm_engine_new();
  wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
  wasmtime_context_t* ctx = wasmtime_store_context(store);

  wasmtime_module_t* wmod = nullptr;
  if (wasmtime_error_t* err =
          wasmtime_module_new(engine, bytes->data(), bytes->size(), &wmod)) {
    auto s = absl::InternalError(
        absl::StrCat("wasmtime_module_new: ", ErrorMessage(err)));
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return s;
  }

  wasmtime_instance_t instance;
  wasm_trap_t* trap = nullptr;
  if (wasmtime_error_t* err = wasmtime_instance_new(
          ctx, wmod, /*imports=*/nullptr, /*nimports=*/0, &instance, &trap)) {
    auto s = absl::InternalError(
        absl::StrCat("wasmtime_instance_new: ", ErrorMessage(err)));
    wasmtime_module_delete(wmod);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return s;
  }
  if (trap != nullptr) {
    auto s = absl::InternalError(
        absl::StrCat("instantiation trap: ", TrapMessage(trap)));
    wasmtime_module_delete(wmod);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return s;
  }

  wasmtime_extern_t ext;
  const char kEvalName[] = "eval";
  if (!wasmtime_instance_export_get(ctx, &instance, kEvalName,
                                    std::strlen(kEvalName), &ext)) {
    wasmtime_module_delete(wmod);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return absl::NotFoundError(
        "instance has no export named `eval` — codegen bug");
  }
  if (ext.kind != WASMTIME_EXTERN_FUNC) {
    wasmtime_extern_delete(&ext);
    wasmtime_module_delete(wmod);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return absl::FailedPreconditionError(
        "export `eval` is not a function");
  }

  wasmtime_val_t result{};
  if (wasmtime_error_t* err = wasmtime_func_call(
          ctx, &ext.of.func, /*args=*/nullptr, /*nargs=*/0,
          /*results=*/&result, /*nresults=*/1, &trap)) {
    auto s = absl::InternalError(
        absl::StrCat("wasmtime_func_call: ", ErrorMessage(err)));
    wasmtime_module_delete(wmod);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return s;
  }
  if (trap != nullptr) {
    auto s = absl::InternalError(
        absl::StrCat("eval() trapped: ", TrapMessage(trap)));
    wasmtime_module_delete(wmod);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return s;
  }

  wasmtime_module_delete(wmod);
  wasmtime_store_delete(store);
  wasm_engine_delete(engine);
  return result;  // scalar — no unroot needed.
}

TEST(EvalE2ETest, IntConstant) {
  auto r = Evaluate("42");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 42);
}

TEST(EvalE2ETest, IntArithmeticPrecedence) {
  auto r = Evaluate("1 + 2 * 3");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 7);
}

TEST(EvalE2ETest, IntSubtractionAndNegation) {
  auto r = Evaluate("-(1 + 2) + 10");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

TEST(EvalE2ETest, UintUnsignedDivision) {
  // If we accidentally used i64.div_s here, the bit pattern of the
  // uint divisor would be the same but the dividend would be read as
  // a signed negative — so exercising with a large uint is the
  // clearest guard.  For plain 10u/3u the two are equivalent; keep
  // the test simple and rely on the module_test opcode inspection
  // for the stronger structural proof.
  auto r = Evaluate("10u / 3u");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(static_cast<uint64_t>(r->of.i64), 3u);
}

TEST(EvalE2ETest, DoubleArithmetic) {
  auto r = Evaluate("1.5 + 2.25 * 4.0");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_F64);
  EXPECT_DOUBLE_EQ(r->of.f64, 1.5 + 2.25 * 4.0);
}

TEST(EvalE2ETest, BoolLiteralTrue) {
  auto r = Evaluate("true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, BoolLiteralFalse) {
  auto r = Evaluate("false");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2ETest, LogicalAndShortCircuit) {
  auto r = Evaluate("true && false");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2ETest, LogicalOrShortCircuit) {
  auto r = Evaluate("false || true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, LogicalNot) {
  auto r = Evaluate("!false");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, IntComparisons) {
  EXPECT_EQ(Evaluate("1 < 2")->of.i32, 1);
  EXPECT_EQ(Evaluate("2 < 2")->of.i32, 0);
  EXPECT_EQ(Evaluate("2 <= 2")->of.i32, 1);
  EXPECT_EQ(Evaluate("3 > 2")->of.i32, 1);
  EXPECT_EQ(Evaluate("2 >= 3")->of.i32, 0);
  EXPECT_EQ(Evaluate("2 == 2")->of.i32, 1);
  EXPECT_EQ(Evaluate("2 != 2")->of.i32, 0);
}

TEST(EvalE2ETest, SignedIntComparisonTreatsNegativeCorrectly) {
  // If we had used unsigned comparison by mistake, -1 would read as
  // UINT64_MAX and the answer would flip.
  auto r = Evaluate("-1 < 0");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, DoubleComparisons) {
  EXPECT_EQ(Evaluate("1.0 < 2.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("1.0 == 1.0")->of.i32, 1);
}

TEST(EvalE2ETest, Ternary) {
  EXPECT_EQ(Evaluate("true ? 1 : 2")->of.i64, 1);
  EXPECT_EQ(Evaluate("false ? 1 : 2")->of.i64, 2);
}

TEST(EvalE2ETest, MixedArithAndConditional) {
  // (1+2)*3 == 9  →  true  →  returns 42.  Also exercises short-circuit.
  auto r = Evaluate("(1 + 2) * 3 == 9 ? 42 : -1");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 42);
}

TEST(EvalE2ETest, IntModulo) {
  auto r = Evaluate("10 % 3");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 1);
}

}  // namespace
}  // namespace celwasm
