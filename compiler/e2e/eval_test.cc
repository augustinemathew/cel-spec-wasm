// End-to-end evaluation test for the codegen MVP.
//
// Pipeline exercised:
//
//   CEL source string
//       → ParseAndCheck   (compiler/frontend)
//       → LowerToEvalFunction + WasmModule::Serialize
//                         (compiler/codegen)
//       → LoadEval        (compiler/host)  — instantiates runtime
//                           + eval under a shared wasmtime linker
//                           (two-module architecture, design §7.0)
//       → CallNullaryEval — invokes the `eval` export and returns
//                           the scalar result
//
// This is the only test in the repo that actually executes WASM the
// compiler emits against a real runtime.  Everything upstream
// (Binaryen's validator, instruction-shape assertions in
// expr_lower_test) can still pass while the module means nothing
// semantically; this test catches that.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/host/host_loader.h"
#include "compiler/ir/typed_ast.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Runs `cel_source` through the full pipeline and returns the single
// scalar that `eval()` produced.
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

  auto loaded = LoadEval(*bytes);
  if (!loaded.ok()) return loaded.status();
  return loaded->CallNullaryEval();
}

// Same as Evaluate, but declares variables via `variable_specs` and
// passes `args` through to the eval function.  The caller is
// responsible for packing args in the same order as specs and using a
// `wasmtime_val_t` whose `kind` matches each variable's ABI (i64 for
// int/uint, i32 for bool, f64 for double, etc.).
absl::StatusOr<wasmtime_val_t> EvaluateWithVars(
    absl::string_view cel_source,
    std::vector<std::string> variable_specs,
    std::vector<wasmtime_val_t> args) {
  CheckOptions opts;
  opts.variable_specs = std::move(variable_specs);
  auto typed = ParseAndCheck(cel_source, opts);
  if (!typed.ok()) return typed.status();

  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  if (auto s = mod.Validate(); !s.ok()) return s;

  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();

  auto loaded = LoadEval(*bytes);
  if (!loaded.ok()) return loaded.status();
  return loaded->CallEval(args);
}

wasmtime_val_t I64(int64_t v) { return wasmtime_val_t{WASMTIME_I64, {.i64 = v}}; }
wasmtime_val_t I32(int32_t v) { return wasmtime_val_t{WASMTIME_I32, {.i32 = v}}; }
wasmtime_val_t F64(double v)  { return wasmtime_val_t{WASMTIME_F64, {.f64 = v}}; }

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
  // Per-op coverage mirroring IntComparisons.  A regression that used
  // int opcodes on f64 operands would produce either validator failure
  // or numerically wrong results on the ordered ops.
  EXPECT_EQ(Evaluate("1.0 < 2.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("2.0 < 2.0")->of.i32, 0);
  EXPECT_EQ(Evaluate("2.0 <= 2.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("3.0 > 2.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("2.0 >= 3.0")->of.i32, 0);
  EXPECT_EQ(Evaluate("1.0 == 1.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("2.0 != 2.0")->of.i32, 0);
}

TEST(EvalE2ETest, UintComparisons) {
  // The important row is the ordered compares: if codegen used the signed
  // opcode, the answer for a very large uint would flip.  Using a value
  // above `INT64_MAX` makes that lurking bug visible.
  EXPECT_EQ(Evaluate("1u < 2u")->of.i32, 1);
  EXPECT_EQ(Evaluate("2u < 2u")->of.i32, 0);
  EXPECT_EQ(Evaluate("2u <= 2u")->of.i32, 1);
  EXPECT_EQ(Evaluate("3u > 2u")->of.i32, 1);
  EXPECT_EQ(Evaluate("2u >= 3u")->of.i32, 0);
  EXPECT_EQ(Evaluate("2u == 2u")->of.i32, 1);
  EXPECT_EQ(Evaluate("2u != 2u")->of.i32, 0);
  // 18446744073709551615u == 2^64 - 1; would be read as -1 if signed
  // compares leaked in.
  EXPECT_EQ(Evaluate("18446744073709551615u > 1u")->of.i32, 1);
}

TEST(EvalE2ETest, DoubleNegate) {
  // `-(x + y)` forces a non-constant negate so constant folding in the
  // parser can't short-circuit the codegen path.
  auto r = Evaluate("-(1.5 + 2.25)");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_F64);
  EXPECT_DOUBLE_EQ(r->of.f64, -(1.5 + 2.25));
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

TEST(EvalE2ETest, IntVariableIsReadFromFirstParam) {
  // `x + 1` with x=41 exercises the full ident path: parse+check assigns
  // `x:int` an i64 Repr, codegen lays it out as param 0, and the runtime
  // call hands `41` in.  If the param-index assignment or the
  // `local.get` type ever drift, this test catches it.
  auto r = EvaluateWithVars("x + 1", {"x:int"}, {I64(41)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 42);
}

TEST(EvalE2ETest, BoolVariableInTernary) {
  auto r = EvaluateWithVars("flag ? 7 : 9", {"flag:bool"}, {I32(1)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
  auto r2 = EvaluateWithVars("flag ? 7 : 9", {"flag:bool"}, {I32(0)});
  ASSERT_THAT(r2.status(), IsOk());
  EXPECT_EQ(r2->of.i64, 9);
}

TEST(EvalE2ETest, DoubleVariableArithmetic) {
  auto r = EvaluateWithVars("x * 2.0", {"x:double"}, {F64(1.5)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_F64);
  EXPECT_DOUBLE_EQ(r->of.f64, 3.0);
}

TEST(EvalE2ETest, UintVariableUnsignedComparison) {
  // Signed-vs-unsigned matters here: if the compare opcode leaks in
  // signed, 2^63 would read as negative and this comparison would
  // flip.
  uint64_t big = 1ULL << 63;
  auto r = EvaluateWithVars("x > 1u", {"x:uint"},
                            {I64(static_cast<int64_t>(big))});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, TwoVariablesReadInDeclarationOrder) {
  // `x:int, y:int` then expr `x - y`.  With x=10, y=3 the result is 7.
  // If the codegen ever swapped the param indices, this would return
  // -7 (because `y - x`), which is a very visible regression.
  auto r = EvaluateWithVars("x - y", {"x:int", "y:int"},
                            {I64(10), I64(3)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

TEST(EvalE2ETest, UnreferencedVariableStillOccupiesParamSlot) {
  // Declaring `y` but referring only to `x` must still produce a
  // 2-param signature so the host ABI is deterministic.  Passing a
  // dummy value for `y` exercises the slot; the expression value
  // shouldn't depend on it.
  auto r = EvaluateWithVars("x + 1", {"x:int", "y:int"},
                            {I64(5), I64(999)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 6);
}

TEST(EvalE2ETest, StringLiteralReturnsCelValueOffset) {
  // Strings travel as i32 offsets of CelValues in the runtime's arena.
  // The runtime instantiation populates the static singletons below
  // offset kStaticStart+4*sizeof(CelValue); our string's CelValue will
  // land beyond that.  Just assert positivity here — the
  // runtime-level semantics (offsets point to a CEL_STRING CelValue
  // whose bytes are "hello") are covered by compiler/runtime tests.
  auto r = Evaluate("'hello'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_GT(r->of.i32, 0);
}

}  // namespace
}  // namespace celwasm
