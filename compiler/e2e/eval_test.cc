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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/abi.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/host/attribute.h"
#include "compiler/host/host_loader.h"
#include "compiler/ir/typed_ast.h"
#include "compiler/runtime/cel_runtime.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "google/protobuf/message.h"
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
  auto abi = BuildCelAbi(*typed, cel_source);
  if (!abi.ok()) return abi.status();
  if (auto s = AttachCelAbiSection(mod, *abi); !s.ok()) return s;
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
    absl::string_view cel_source, std::vector<std::string> variable_specs,
    const std::vector<wasmtime_val_t>& args) {
  CheckOptions opts;
  opts.variable_specs = std::move(variable_specs);
  auto typed = ParseAndCheck(cel_source, opts);
  if (!typed.ok()) return typed.status();

  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  auto abi = BuildCelAbi(*typed, cel_source);
  if (!abi.ok()) return abi.status();
  if (auto s = AttachCelAbiSection(mod, *abi); !s.ok()) return s;
  if (auto s = mod.Validate(); !s.ok()) return s;

  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();

  auto loaded = LoadEval(*bytes);
  if (!loaded.ok()) return loaded.status();
  return loaded->CallEval(args);
}

wasmtime_val_t I64(int64_t v) {
  return wasmtime_val_t{WASMTIME_I64, {.i64 = v}};
}
wasmtime_val_t I32(int32_t v) {
  return wasmtime_val_t{WASMTIME_I32, {.i32 = v}};
}
wasmtime_val_t F64(double v) {
  return wasmtime_val_t{WASMTIME_F64, {.f64 = v}};
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
  EXPECT_DOUBLE_EQ(r->of.f64, 1.5 + (2.25 * 4.0));
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

// ---- Checked arithmetic (M4 Slice B) -----------------------------------
//
// Happy-path regression — the codegen retrofit reshaped arithmetic
// into a helper call + write-to-sret-on-error block (M4 Slice C/3b1
// switched from trap to observable CelValue ERROR).  The existing
// arithmetic / precedence tests exercise the happy path; these add
// the overflow / div-by-zero side.  CallEval's DecodeSlot surfaces
// CEL_ERROR as `absl::InternalError("CallEval: result is ERROR")`,
// so that's the assertion we make here.

MATCHER(CelErrorStatus, "is an InternalError carrying a CelValue ERROR") {
  return !arg.ok() && arg.code() == absl::StatusCode::kInternal &&
         absl::StrContains(arg.message(), "result is ERROR");
}

TEST(EvalE2ETest, IntAddOverflowIsCelError) {
  // 9223372036854775807 + 1 = INT64_MAX + 1 — overflow.
  auto r = Evaluate("9223372036854775807 + 1");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, IntSubOverflowIsCelError) {
  // INT64_MIN - 1 underflows.
  auto r = Evaluate("-9223372036854775808 - 1");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, IntMulOverflowIsCelError) {
  // 2^62 * 8 = 2^65, overflow.  The hi/lo-split helper must detect this
  // without pulling in __multi3 on wasm32.
  auto r = Evaluate("4611686018427387904 * 8");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, IntDivMinByNegOneIsCelError) {
  // INT64_MIN / -1 is the classic signed overflow case.
  auto r = Evaluate("-9223372036854775808 / -1");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, IntDivByZeroIsCelError) {
  auto r = Evaluate("5 / 0");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, IntModByZeroIsCelError) {
  auto r = Evaluate("5 % 0");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, IntModMinByNegOneIsZeroNotTrap) {
  // INT64_MIN % -1 is mathematically 0; C reserves UB, so the runtime
  // special-cases it.  A trap here would mean the special case leaked.
  auto r = Evaluate("-9223372036854775808 % -1");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 0);
}

TEST(EvalE2ETest, UintAddOverflowIsCelError) {
  // UINT64_MAX + 1 wraps arithmetically but CEL defines it as error.
  auto r = Evaluate("18446744073709551615u + 1u");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, UintSubUnderflowIsCelError) {
  auto r = Evaluate("0u - 1u");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, UintMulOverflowIsCelError) {
  // (2^32) * (2^32) = 2^64, overflow.  Exercises the hi/lo-split path.
  auto r = Evaluate("4294967296u * 4294967296u");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, UintDivByZeroIsCelError) {
  auto r = Evaluate("5u / 0u");
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, IntMulHappyPathStillWorksAfterRetrofit) {
  // 2^32 * 2^30 = 2^62 — fits in signed 64.  Regression guard: the hi/lo
  // split must reconstruct the low 64 bits correctly on in-range mul.
  auto r = Evaluate("4294967296 * 1073741824");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, int64_t{4611686018427387904});
}

TEST(EvalE2ETest, SignedMulNegativeResultRoundTrips) {
  // -3 * 7 = -21 — ensures the sign-reconstruction branch in
  // s64_mul_overflow returns the right sign.
  auto r = Evaluate("-3 * 7");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, -21);
}

TEST(EvalE2ETest, SignedMulIntMinByOneRoundTrips) {
  // INT64_MIN * 1 must produce INT64_MIN, not overflow — this is the
  // one edge case where |a| == 2^63 but the result is representable.
  auto r = Evaluate("-9223372036854775808 * 1");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, INT64_MIN);
}

// ---- NaN-unordered compares (M4 Slice D) -------------------------------
//
// CEL §langdef specifies that NaN in an ordered compare (`<`, `<=`,
// `>`, `>=`) produces ERROR, not a bogus `false`.  IEEE 754 says
// `NaN op x` is always false for ordered compares — using that
// directly would violate CEL semantics, so codegen must insert an
// explicit NaN guard.  NaN flows in via host-provided `double`
// params (CEL has no NaN literal) or via `0.0 / 0.0`.  For `==` /
// `!=`, IEEE 754's NaN behavior is what CEL wants (`NaN == NaN` is
// false, `NaN != NaN` is true), so no guard is needed there.
//
// Under Slice C/3b1, NaN-in-ordered-compare writes CEL_ERR_TYPE_MISMATCH
// into the sret slot and returns; the host decodes it as an observable
// CelValue ERROR.

TEST(EvalE2ETest, DoubleLessNaNOnLeftIsCelError) {
  auto r = EvaluateWithVars("x < 1.0", {"x:double"},
                            {F64(std::numeric_limits<double>::quiet_NaN())});
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, DoubleLessNaNOnRightIsCelError) {
  auto r = EvaluateWithVars("1.0 < x", {"x:double"},
                            {F64(std::numeric_limits<double>::quiet_NaN())});
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, DoubleLessEqNaNIsCelError) {
  auto r = EvaluateWithVars("x <= 1.0", {"x:double"},
                            {F64(std::numeric_limits<double>::quiet_NaN())});
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, DoubleGreaterNaNIsCelError) {
  auto r = EvaluateWithVars("x > 1.0", {"x:double"},
                            {F64(std::numeric_limits<double>::quiet_NaN())});
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, DoubleGreaterEqNaNIsCelError) {
  auto r = EvaluateWithVars("x >= 1.0", {"x:double"},
                            {F64(std::numeric_limits<double>::quiet_NaN())});
  EXPECT_THAT(r.status(), CelErrorStatus());
}

TEST(EvalE2ETest, DoubleEqualityWithNaNReturnsFalseNotTrap) {
  // Per CEL §langdef, `==` / `!=` on NaN follow IEEE 754: NaN == NaN
  // is false; NaN != NaN is true.  These must NOT trap — that would
  // mean the NaN guard leaked into the equality path.
  auto nan = F64(std::numeric_limits<double>::quiet_NaN());
  auto r_eq = EvaluateWithVars("x == 1.0", {"x:double"}, {nan});
  ASSERT_THAT(r_eq.status(), IsOk());
  EXPECT_EQ(r_eq->kind, WASMTIME_I32);
  EXPECT_EQ(r_eq->of.i32, 0);
  auto r_ne = EvaluateWithVars("x != 1.0", {"x:double"}, {nan});
  ASSERT_THAT(r_ne.status(), IsOk());
  EXPECT_EQ(r_ne->of.i32, 1);
}

TEST(EvalE2ETest, DoubleOrderedCompareNonNaNStillWorks) {
  // Regression guard: the NaN guard must not alter ordinary double
  // ordered-compare semantics.
  auto r = EvaluateWithVars("x < 2.0", {"x:double"}, {F64(1.5)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, DoubleDivZeroProducesInfNotTrap) {
  // IEEE 754 defines double `/ 0.0` as +/- Infinity, and CEL
  // inherits that — it's only integer div-by-zero that's ERROR.
  auto r = Evaluate("1.0 / 0.0");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_F64);
  EXPECT_TRUE(std::isinf(r->of.f64));
}

// ---- 3VL absorption gap (Slice F) -----------------------------------
//
// Every test below DISABLED_ asserts the CEL-spec answer for an
// expression where an ERROR-producing subtree flows through a
// non-absorbing intermediate op (arithmetic, comparison, NaN-compare)
// into a 3VL absorber (`&&` / `||`).  Per `doc/langdef.md`
// §partial-evaluation, the absorber is supposed to see the ERROR as a
// value and short-circuit past it (`ERROR || OK(true) → OK(true)`).
// Our pipeline today early-returns from `$eval` on the first ERROR
// (Slice C/3b1 arithmetic + Slice D NaN-compare + Slice E1 ternary
// cond), so the host sees `CelErrorStatus` instead of the spec result.
//
// Keep these tests in lock-step with the checklist in
// `doc/implementation-plan/m4-slice-f-3vl-absorption.md`.  When Slice
// F lands (3VL-aware comparisons and arithmetic: operands and results
// ride the CelValue-offset ABI so the ERROR can flow upward), drop
// the DISABLED_ prefix and they should pass.

TEST(EvalE2ETest, ThreeValuedAbsorptionErrorEqAbsorbedByOr) {
  // Row 1: (1/0 == 0) || true → true.
  auto r = Evaluate("(1 / 0 == 0) || true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, ThreeValuedAbsorptionErrorEqAbsorbedByAnd) {
  // Row 2: (1/0 == 0) && false → false.
  auto r = Evaluate("(1 / 0 == 0) && false");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2ETest, ThreeValuedAbsorptionErrorOrderedCompareAbsorbed) {
  // Row 3: (1/0 > 5) || true → true.
  auto r = Evaluate("(1 / 0 > 5) || true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, ThreeValuedAbsorptionErrorArithThenCompareAbsorbed) {
  // Row 4: ((1/0) + 1) == 0 || true → true.  ERROR through a second
  // arithmetic hop before the comparison.
  auto r = Evaluate("((1 / 0) + 1) == 0 || true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, ThreeValuedAbsorptionOverflowAbsorbed) {
  // Row 5: (INT64_MAX + 1 == 0) || true → true.  Overflow-flavoured
  // ERROR (vs. div-by-zero) absorbed the same way.
  auto r = Evaluate("(9223372036854775807 + 1 == 0) || true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, ThreeValuedAbsorptionNaNCompareAbsorbed) {
  // Row 6: (NaN < 1.0) || true → true.  NaN-in-ordered-compare is
  // surfaced as CEL_ERROR by `cel_cmp_double_lt` in the runtime, and
  // the wrapping `||` absorbs past the literal `true`.  Uniform
  // boxed path — no `HasNonOkProducer` gate.
  auto r = EvaluateWithVars("(x < 1.0) || true", {"x:double"},
                            {F64(std::numeric_limits<double>::quiet_NaN())});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, ThreeValuedAbsorptionTernaryResultAbsorbedByOr) {
  // Row 8: ((1/0 == 0) ? true : false) || true → true.  Slice F step 6
  // routes ternary operands of a 3VL absorber through
  // `LowerConditionalBoxed`, which returns the cond's CelValue offset
  // when the cond is UNKNOWN / ERROR instead of bailing out of `$eval`
  // via `EmitSretEarlyReturnIfNonOk`.  The wrapping `||` then sees the
  // ERROR as a value and absorbs it against the OK(true) rhs.
  auto r = Evaluate("((1 / 0 == 0) ? true : false) || true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
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
  auto r =
      EvaluateWithVars("x > 1u", {"x:uint"}, {I64(static_cast<int64_t>(big))});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, TwoVariablesReadInDeclarationOrder) {
  // `x:int, y:int` then expr `x - y`.  With x=10, y=3 the result is 7.
  // If the codegen ever swapped the param indices, this would return
  // -7 (because `y - x`), which is a very visible regression.
  auto r = EvaluateWithVars("x - y", {"x:int", "y:int"}, {I64(10), I64(3)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

TEST(EvalE2ETest, UnreferencedVariableStillOccupiesParamSlot) {
  // Declaring `y` but referring only to `x` must still produce a
  // 2-param signature so the host ABI is deterministic.  Passing a
  // dummy value for `y` exercises the slot; the expression value
  // shouldn't depend on it.
  auto r = EvaluateWithVars("x + 1", {"x:int", "y:int"}, {I64(5), I64(999)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 6);
}

// Reads a string `CelValue` back out of the runtime's linear memory.
// The eval function returns an i32 offset into the shared memory; we
// need to pull the 24-byte CelValue header and the span's bytes from
// wasmtime to assert on the semantic result, not just the offset.
//
// Keeps the test coupled to the on-wire layout (the struct defined in
// cel_runtime.h) on purpose: a layout bump would break this read in a
// visible way, which is what we want — a layout change that silently
// works on the host but not in the runtime would be much worse.
struct DecodedString {
  uint32_t kind = 0;
  std::string payload;
};
absl::StatusOr<DecodedString> DecodeStringAt(LoadedEval& loaded,
                                             int32_t offset) {
  wasmtime_context_t* ctx = loaded.context();
  wasmtime_extern_t mem_ext;
  if (!wasmtime_instance_export_get(ctx, &loaded.runtime_instance(), "memory",
                                    6, &mem_ext)) {
    return absl::InternalError("runtime does not export `memory`");
  }
  if (mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
    return absl::InternalError("`memory` export is not a memory");
  }
  const uint8_t* base = wasmtime_memory_data(ctx, &mem_ext.of.memory);
  size_t size = wasmtime_memory_data_size(ctx, &mem_ext.of.memory);

  // CelValue offsets returned by the runtime are relative to the
  // runtime's `g_memory` array, not to wasm linear memory.  Ask the
  // runtime where g_memory lives and translate.
  wasmtime_extern_t mem_base_ext;
  if (!wasmtime_instance_export_get(ctx, &loaded.runtime_instance(),
                                    "cel_mem_base", std::strlen("cel_mem_base"),
                                    &mem_base_ext)) {
    return absl::InternalError("runtime does not export `cel_mem_base`");
  }
  wasmtime_val_t base_off{};
  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_func_call(
        ctx, &mem_base_ext.of.func, /*args=*/nullptr, /*nargs=*/0, &base_off,
        /*nresults=*/1, &trap);
    if (err != nullptr || trap != nullptr) {
      return absl::InternalError("cel_mem_base call failed");
    }
  }
  const auto mem_base = static_cast<uint32_t>(base_off.of.i32);
  const uint64_t abs_cv =
      static_cast<uint64_t>(mem_base) + static_cast<uint64_t>(offset);
  if (offset <= 0 || abs_cv + sizeof(CelValue) > size) {
    return absl::OutOfRangeError("CelValue offset is outside memory bounds");
  }
  CelValue v;
  std::memcpy(&v, base + abs_cv, sizeof(v));
  DecodedString out;
  out.kind = v.kind;
  const uint32_t ptr = v.payload.s.ptr;
  const uint32_t len = v.payload.s.len;
  if (len > 0) {
    const uint64_t abs_bytes =
        static_cast<uint64_t>(mem_base) + static_cast<uint64_t>(ptr);
    if (abs_bytes + len > size) {
      return absl::OutOfRangeError(
          "string payload span falls outside runtime memory");
    }
    out.payload.assign(reinterpret_cast<const char*>(base + abs_bytes), len);
  }
  return out;
}

// Runs `cel_source` through the full pipeline, invokes `eval`, decodes
// the resulting i32 offset as a CEL string, and hands the decoded
// {kind, bytes} pair back.  The `kind` lives on the struct so callers
// can assert `CEL_STRING` (a codegen regression that returned a bool
// or an int offset would otherwise surface as surprising but passing
// bytes comparisons — better to pin the kind explicitly).
absl::StatusOr<DecodedString> EvaluateToString(absl::string_view cel_source) {
  auto typed = ParseAndCheck(cel_source, CheckOptions{});
  if (!typed.ok()) return typed.status();
  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  auto abi = BuildCelAbi(*typed, cel_source);
  if (!abi.ok()) return abi.status();
  if (auto s = AttachCelAbiSection(mod, *abi); !s.ok()) return s;
  if (auto s = mod.Validate(); !s.ok()) return s;
  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();
  auto loaded = LoadEval(*bytes);
  if (!loaded.ok()) return loaded.status();
  auto result = loaded->CallNullaryEval();
  if (!result.ok()) return result.status();
  if (result->kind != WASMTIME_I32) {
    return absl::InternalError(
        "expected eval to return an i32 CelValue offset");
  }
  return DecodeStringAt(*loaded, result->of.i32);
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

TEST(EvalE2ETest, StringLiteralRoundTripsThroughMemory) {
  // The strongest literal-path assertion: a string constant survives
  // the `cel_alloc` + `i32.store8` + `cel_make_string_view` chain and
  // can be read back byte-identical via the runtime's shared memory.
  auto r = EvaluateToString("'hello'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(r->payload, "hello");
}

TEST(EvalE2ETest, EmptyStringRoundTrips) {
  // cel_alloc(0), zero store8s, cel_make_string_view(ptr, 0).  Must
  // produce a valid CelValue whose span is len=0.  Regressions where
  // the empty-path returns 0 (null offset) or the wrong kind would
  // cause DecodeStringAt to fail cleanly rather than silently pass.
  auto r = EvaluateToString("''");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(r->payload, "");
}

TEST(EvalE2ETest, StringConcatenationProducesJoinedBytes) {
  auto r = EvaluateToString("'hi' + 'there'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(r->payload, "hithere");
}

TEST(EvalE2ETest, StringConcatenationEmptyLhs) {
  // `span_concat`'s la=0 branch is the one that would regress if
  // someone reordered the memcpy guards.  Exercise it explicitly.
  auto r = EvaluateToString("'' + 'xy'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->payload, "xy");
}

TEST(EvalE2ETest, StringEqualityPositiveAndNegative) {
  EXPECT_EQ(Evaluate("'hi' == 'hi'")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hi' == 'bye'")->of.i32, 0);
  // Length-different operands hit the early-exit branch in span_eq;
  // same-length-different-bytes hits the memcmp branch.  Cover both.
  EXPECT_EQ(Evaluate("'ab' == 'abc'")->of.i32, 0);
  EXPECT_EQ(Evaluate("'ab' == 'ac'")->of.i32, 0);
  EXPECT_EQ(Evaluate("'' == ''")->of.i32, 1);
}

TEST(EvalE2ETest, StringInequalityInvertsEquality) {
  EXPECT_EQ(Evaluate("'hi' != 'hi'")->of.i32, 0);
  EXPECT_EQ(Evaluate("'hi' != 'bye'")->of.i32, 1);
}

TEST(EvalE2ETest, SizeOfAsciiStringIsByteCount) {
  // For ASCII, code-point count == byte count, so the observable
  // answer is the same.  The UTF-8 test below is where the codepoint
  // semantics actually matter.
  auto r = Evaluate("size('hello')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 5);
}

TEST(EvalE2ETest, SizeOfEmptyStringIsZero) {
  EXPECT_EQ(Evaluate("size('')")->of.i64, 0);
}

TEST(EvalE2ETest, SizeOfUtf8StringCountsCodepointsNotBytes) {
  // "héllo" is 6 bytes (`é` encodes as C3 A9) but 5 code points.
  // cel_string_size is defined to count code points (CEL §1110); a
  // regression that counted bytes instead would return 6 here.
  auto r = Evaluate("size('h\\u00e9llo')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 5);
}

TEST(EvalE2ETest, SizeOfConcatenatedString) {
  // End-to-end compose: concat(...) must produce a CEL_STRING whose
  // size the runtime can subsequently compute.  Exercises the path
  // that consumes a freshly-allocated concat result — a regression
  // that stashed the concat result in the wrong arena slot would
  // surface here as a wrong count or a trap.
  auto r = Evaluate("size('hi' + 'there')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

// M3 slice E: string member calls.  All three helpers are i32-returning
// runtime imports; these tests confirm the eval module lowers the CEL
// method syntax into a runtime call that honors the spec's edge cases
// (empty needle is true, longer needle than haystack is false).

TEST(EvalE2ETest, StartsWithPositiveAndNegative) {
  EXPECT_EQ(Evaluate("'hello'.startsWith('he')")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hello'.startsWith('lo')")->of.i32, 0);
}

TEST(EvalE2ETest, StartsWithEmptyPrefixIsTrue) {
  // CEL §9: every string starts with the empty string.  The runtime's
  // la=0 early-return guards this; a regression that dropped the guard
  // would memcmp zero bytes and still return 1, but if the zero-length
  // guard moved around after the allocation check the wrong branch
  // could return 0.
  EXPECT_EQ(Evaluate("'hello'.startsWith('')")->of.i32, 1);
}

TEST(EvalE2ETest, StartsWithLongerPrefixIsFalse) {
  EXPECT_EQ(Evaluate("'hi'.startsWith('hello')")->of.i32, 0);
}

TEST(EvalE2ETest, EndsWithPositiveAndNegative) {
  EXPECT_EQ(Evaluate("'hello'.endsWith('lo')")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hello'.endsWith('he')")->of.i32, 0);
}

TEST(EvalE2ETest, EndsWithEmptySuffixIsTrue) {
  EXPECT_EQ(Evaluate("'hello'.endsWith('')")->of.i32, 1);
}

TEST(EvalE2ETest, ContainsPositivePrefixMiddleSuffix) {
  // Three cases exercise the three search positions so a regression in
  // the search-loop bounds (off-by-one on the `last` computation)
  // surfaces in at least one assertion.
  EXPECT_EQ(Evaluate("'hello'.contains('he')")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hello'.contains('ell')")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hello'.contains('lo')")->of.i32, 1);
}

TEST(EvalE2ETest, ContainsNegative) {
  EXPECT_EQ(Evaluate("'hello'.contains('xyz')")->of.i32, 0);
}

TEST(EvalE2ETest, ContainsEmptyNeedleIsTrue) {
  EXPECT_EQ(Evaluate("'hello'.contains('')")->of.i32, 1);
}

TEST(EvalE2ETest, ContainsLongerNeedleIsFalse) {
  EXPECT_EQ(Evaluate("'hi'.contains('hello')")->of.i32, 0);
}

// M3 slice F: bytes literals and operators.  The `DecodedString` helper
// reads out the CelValue regardless of kind, so bytes tests reuse the
// same decode path and assert `kind == CEL_BYTES` explicitly so a
// regression that swapped the span constructor would surface as a
// mismatched kind rather than silently-passing byte comparisons.  CEL
// bytes literals use the `b''` prefix; concatenation uses `+` like
// strings, and `size()` is a byte count (not a codepoint count).

TEST(EvalE2ETest, BytesLiteralRoundTripsThroughMemory) {
  auto r = EvaluateToString("b'hi'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(r->payload, "hi");
}

TEST(EvalE2ETest, EmptyBytesRoundTrips) {
  auto r = EvaluateToString("b''");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(r->payload, "");
}

TEST(EvalE2ETest, BytesLiteralPreservesHighBits) {
  // `b'\xff\x00\xfe'` — non-ASCII + an embedded NUL.  The store-per-byte
  // loop's `i32.const byte` must survive the sign-extension dance in
  // Binaryen; a regression that let a u8 get sign-extended through i32
  // would surface as a mismatched byte here (0xff would become -1 and
  // then zero-extend differently on the other side).
  auto r = EvaluateToString(R"(b'\xff\x00\xfe')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  ASSERT_EQ(r->payload.size(), 3u);
  EXPECT_EQ(static_cast<uint8_t>(r->payload.at(0)), 0xffu);
  EXPECT_EQ(static_cast<uint8_t>(r->payload.at(1)), 0x00u);
  EXPECT_EQ(static_cast<uint8_t>(r->payload.at(2)), 0xfeu);
}

TEST(EvalE2ETest, BytesConcatenationProducesJoinedBytes) {
  auto r = EvaluateToString("b'ab' + b'cd'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(r->payload, "abcd");
}

TEST(EvalE2ETest, BytesConcatenationEmptyLhs) {
  auto r = EvaluateToString("b'' + b'xy'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(r->payload, "xy");
}

TEST(EvalE2ETest, BytesEqualityPositiveAndNegative) {
  EXPECT_EQ(Evaluate("b'hi' == b'hi'")->of.i32, 1);
  EXPECT_EQ(Evaluate("b'hi' == b'bye'")->of.i32, 0);
  EXPECT_EQ(Evaluate("b'ab' == b'abc'")->of.i32, 0);
  EXPECT_EQ(Evaluate("b'' == b''")->of.i32, 1);
}

TEST(EvalE2ETest, BytesInequalityInvertsEquality) {
  EXPECT_EQ(Evaluate("b'hi' != b'hi'")->of.i32, 0);
  EXPECT_EQ(Evaluate("b'hi' != b'bye'")->of.i32, 1);
}

TEST(EvalE2ETest, SizeOfBytesIsByteCount) {
  // The same 4-byte UTF-8 sequence whose string size is 1 codepoint
  // must report 4 here — `size(bytes)` is byte count per CEL §1110.
  // This is the semantic difference between `cel_string_size` and
  // `cel_bytes_size` that the codegen dispatch must honour.
  auto r = Evaluate(R"(size(b'\xf0\x9f\x98\x80'))");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 4);
}

TEST(EvalE2ETest, SizeOfEmptyBytesIsZero) {
  EXPECT_EQ(Evaluate("size(b'')")->of.i64, 0);
}

TEST(EvalE2ETest, SizeOfConcatenatedBytes) {
  // Mirror SizeOfConcatenatedString: the concat result's CelValue must
  // be usable as an input to size() in the same eval.  A regression
  // where the concat output landed in an arena slot that `cel_bytes_size`
  // couldn't read (stale `g_memory` pointer, for instance) would trap
  // here rather than return the wrong count.
  auto r = Evaluate("size(b'hi' + b'there')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

// ---- Proto field reads (M3 Slice G2) ---------------------------------------
//
// Exercises `kSelectExpr` → `cel_host.get_field` through the full
// pipeline: the CEL source references a message-typed variable, the
// test builds a `celwasm.testdata.Customer` (see
// `compiler/testdata/e2e_fixture.proto` — one scalar per CEL-relevant
// wire type, so every payload-load dispatch in `LoadSelectPayload` has
// at least one test), hands it to the module as an externref, and
// checks the evaluator's result against the in-process protobuf value.
//
// The indirection through `LoadCompiled` (vs. `EvaluateWithVars`) is
// because `wasmtime_externref_new` needs a live `wasmtime_context_t*`
// — which only exists after `LoadEval` succeeds.

absl::StatusOr<LoadedEval> LoadCompiled(absl::string_view cel_source,
                                        std::vector<std::string> specs) {
  CheckOptions opts;
  opts.variable_specs = std::move(specs);
  auto typed = ParseAndCheck(cel_source, opts);
  if (!typed.ok()) return typed.status();
  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  auto abi = BuildCelAbi(*typed, cel_source);
  if (!abi.ok()) return abi.status();
  if (auto s = AttachCelAbiSection(mod, *abi); !s.ok()) return s;
  if (auto s = mod.Validate(); !s.ok()) return s;
  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();
  return LoadEval(*bytes);
}

// Wraps `msg` in a fresh externref against `loaded`'s store.  The
// caller must keep `msg` alive until `CallEval` returns — the test
// passes a stack-allocated message, which is fine because evaluation
// is synchronous.
wasmtime_val_t MessageAsExternref(LoadedEval& loaded,
                                  google::protobuf::Message& msg) {
  wasmtime_val_t v{};
  v.kind = WASMTIME_EXTERNREF;
  EXPECT_TRUE(wasmtime_externref_new(loaded.context(), &msg,
                                     /*finalizer=*/nullptr, &v.of.externref));
  return v;
}

constexpr absl::string_view kCustomerSpec = "c:celwasm.testdata.Customer";

TEST(EvalE2ETest, SelectProtoStringFieldEq) {
  auto loaded = LoadCompiled("c.name == \"Ada\"", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, SelectProtoStringFieldNeq) {
  auto loaded = LoadCompiled("c.name == \"Ada\"", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_name("Grace");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2ETest, SelectProtoStringFieldDefaultIsEmpty) {
  // Unset singular string must come back as the empty string (proto3
  // default) — a regression where `cel_host::get_field` returned the
  // zero CelValue (kind=0) would surface as a traped memcmp or a
  // silent true.  The comparison below is the cleanest positive
  // assertion of the default.
  auto loaded = LoadCompiled("c.name == \"\"", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;  // default-constructed: name is unset.
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, SelectProtoStringFieldPassThrough) {
  // Directly returns the string payload — exercises the kString result
  // path that short-circuits the payload-load branch and passes the
  // scratch CelValue* straight through as the eval result.
  auto loaded = LoadCompiled("c.name", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_name("Hello");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  // The returned i32 is an arena-relative CelValue offset; dereference
  // through the runtime's linear memory and check kind + bytes.
  auto* ctx = loaded->context();
  wasmtime_extern_t base_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(ctx, &loaded->runtime_instance(),
                                           "cel_mem_base",
                                           strlen("cel_mem_base"), &base_ext));
  wasmtime_val_t base{};
  wasm_trap_t* trap = nullptr;
  ASSERT_EQ(
      wasmtime_func_call(ctx, &base_ext.of.func, nullptr, 0, &base, 1, &trap),
      nullptr);
  wasmtime_extern_t mem_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(
      ctx, &loaded->runtime_instance(), "memory", strlen("memory"), &mem_ext));
  uint8_t* mem = wasmtime_memory_data(ctx, &mem_ext.of.memory);
  auto* cv = reinterpret_cast<CelValue*>(mem + base.of.i32 + r->of.i32);
  EXPECT_EQ(cv->kind, CEL_STRING);
  std::string got(
      reinterpret_cast<const char*>(mem + base.of.i32 + cv->payload.s.ptr),
      cv->payload.s.len);
  EXPECT_EQ(got, "Hello");
}

TEST(EvalE2ETest, SelectProtoInt32FieldIsCelInt) {
  // proto int32 widens to CEL int (i64).  `c.age == 30` exercises the
  // kInt payload-load (i64.load, signed) with a small positive value.
  auto loaded = LoadCompiled("c.age == 30", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_age(30);
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, SelectProtoInt64FieldCarriesLargeValue) {
  // int64 that doesn't fit in int32 — guards against a regression that
  // narrowed the host-side memcpy or the module-side i64.load.
  constexpr int64_t kLarge = 1ll << 40;
  auto loaded = LoadCompiled("c.user_id", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_user_id(kLarge);
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, kLarge);
}

TEST(EvalE2ETest, SelectProtoUint64FieldIsUnsigned) {
  // A value above INT64_MAX must round-trip without sign-flipping.
  // If the payload-load branch used `i64.load` with a `signed` cast
  // somewhere in the comparison, this test would fail.
  constexpr uint64_t kHuge = (1ull << 63) + 42ull;
  auto loaded =
      LoadCompiled("c.balance_cents > 1u", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_balance_cents(kHuge);
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, SelectProtoUint32FieldIsCelUint) {
  auto loaded = LoadCompiled("c.priority == 5u", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_priority(5);
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, SelectProtoDoubleField) {
  // f64 payload-load at offset 8 — a regression that used i64.load on
  // the double branch would compare wrong bit patterns and fail.
  auto loaded =
      LoadCompiled("c.credit_score > 700.0", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_credit_score(742.5);
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, SelectProtoBoolField) {
  // bool payload-load at offset 8 — a 4-byte i32 load.  Covers true
  // and false on the same expression so an inverted branch would
  // flip both assertions.
  auto loaded = LoadCompiled("c.is_premium", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  {
    celwasm::testdata::Customer msg;
    msg.set_is_premium(true);
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->kind, WASMTIME_I32);
    EXPECT_EQ(r->of.i32, 1);
  }
  {
    celwasm::testdata::Customer msg;
    msg.set_is_premium(false);
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 0);
  }
}

TEST(EvalE2ETest, SelectProtoBytesFieldRoundTrips) {
  // bytes share the pass-through path with strings but must come back
  // with kind = CEL_BYTES (not CEL_STRING) — `cel_host::ReadField` is
  // responsible for that split.  A regression that produced the wrong
  // kind would be caught here even though the bytes bytes match.
  auto loaded = LoadCompiled("c.session_token", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_session_token(std::string("\xff\x00\x01", 3));
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  auto* ctx = loaded->context();
  wasmtime_extern_t base_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(ctx, &loaded->runtime_instance(),
                                           "cel_mem_base",
                                           strlen("cel_mem_base"), &base_ext));
  wasmtime_val_t base{};
  wasm_trap_t* trap = nullptr;
  ASSERT_EQ(
      wasmtime_func_call(ctx, &base_ext.of.func, nullptr, 0, &base, 1, &trap),
      nullptr);
  wasmtime_extern_t mem_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(
      ctx, &loaded->runtime_instance(), "memory", strlen("memory"), &mem_ext));
  uint8_t* mem = wasmtime_memory_data(ctx, &mem_ext.of.memory);
  auto* cv = reinterpret_cast<CelValue*>(mem + base.of.i32 + r->of.i32);
  EXPECT_EQ(cv->kind, CEL_BYTES);
  ASSERT_EQ(cv->payload.s.len, 3u);
  const uint8_t* bytes = mem + base.of.i32 + cv->payload.s.ptr;
  EXPECT_EQ(bytes[0], 0xffu);
  EXPECT_EQ(bytes[1], 0x00u);
  EXPECT_EQ(bytes[2], 0x01u);
}

// ---- has(msg.field) — M3 Slice G3 ------------------------------------------
//
// `has(x.f)` lowers to a `SelectExpr` with `test_only=true` and must
// compile to a single `cel_host.has_field(msg, field_number) → i32`
// call (no cel_alloc, no scratch CelValue).  The CEL semantics of
// proto3 default that these tests pin are:
//
//   - singular scalar set to a non-default value    → has() is true
//   - singular scalar at its default (0, "", false) → has() is false
//   - singular submessage unset                      → has() is false
//   - singular submessage set (even if all fields at
//     their default)                                → has() is true
//
// The runtime equivalent is `FieldDescriptor::HasField()` on the
// operand; the per-wire-type behaviour is an implementation detail of
// `cel_host::HasField` and is also covered by `cel_host_test`.  The
// tests below are the end-of-slice e2e pin: the full codegen +
// wasmtime pipeline hands back the right 0/1 for each scenario.

TEST(EvalE2ETest, HasProtoStringFieldSetAndUnset) {
  auto loaded = LoadCompiled("has(c.name)", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  {
    celwasm::testdata::Customer msg;
    msg.set_name("Ada");
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->kind, WASMTIME_I32);
    EXPECT_EQ(r->of.i32, 1);
  }
  {
    celwasm::testdata::Customer msg;  // default: name is unset.
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 0);
  }
}

TEST(EvalE2ETest, HasProtoInt32FieldSetAndUnset) {
  // Proto3 scalar presence: any non-default value reads as has()=true;
  // the zero default reads as has()=false.  A regression that used the
  // CelValue kind as the has() signal (instead of asking the host
  // `FieldDescriptor::HasField`) would flip this assertion.
  auto loaded = LoadCompiled("has(c.age)", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  {
    celwasm::testdata::Customer msg;
    msg.set_age(30);
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 1);
  }
  {
    celwasm::testdata::Customer msg;
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 0);
  }
}

TEST(EvalE2ETest, HasProtoBoolFieldSetToFalseIsFalse) {
  // Singular bool at its zero-default is unset in proto3, regardless
  // of whether the user explicitly called set_is_premium(false).  This
  // is the single scalar case most likely to surprise callers — pin
  // it.
  auto loaded = LoadCompiled("has(c.is_premium)", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  msg.set_is_premium(false);
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2ETest, HasProtoBytesFieldEmptyIsFalse) {
  auto loaded =
      LoadCompiled("has(c.session_token)", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  {
    celwasm::testdata::Customer msg;
    msg.set_session_token("\x01");
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 1);
  }
  {
    celwasm::testdata::Customer msg;  // empty default.
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 0);
  }
}

TEST(EvalE2ETest, HasProtoMessageFieldRespectsExplicitPresence) {
  // Singular message has explicit presence in proto3 — set/unset is
  // observable even if the submessage is at its own default.  The host
  // call path goes through `Reflection::HasField` on a message field
  // descriptor, which is a distinct code path from the scalar branch.
  auto loaded =
      LoadCompiled("has(c.billing_address)", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  {
    celwasm::testdata::Customer msg;
    msg.mutable_billing_address();  // explicit default-construct.
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 1);
  }
  {
    celwasm::testdata::Customer msg;  // submessage unset.
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 0);
  }
}

TEST(EvalE2ETest, HasComposesWithLogicalNot) {
  // `!has(x.f)` exercises the G3 `has_field` path through the existing
  // bool-result wiring (`!_` lowers to `i32.eqz`).  A regression that
  // returned a non-i32 from has_field would either fail validation or
  // produce the wrong answer here.
  auto loaded = LoadCompiled("!has(c.name)", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;  // name unset.
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, HasAndFieldCompareTernary) {
  // End-to-end: composition of G2 (field read) + G3 (has()) through
  // the ternary lowering — `has(c.name) ? c.name == "Ada" : false`.
  // Pins that the two slices coexist in a single eval body without
  // local-index aliasing (both use scratch locals allocated from the
  // same LoweringContext).
  auto loaded = LoadCompiled("has(c.name) ? c.name == \"Ada\" : false",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  {
    celwasm::testdata::Customer msg;
    msg.set_name("Ada");
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 1);
  }
  {
    celwasm::testdata::Customer
        msg;  // name unset → has() false → ternary false.
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 0);
  }
}

// ---- G4: nested message select + message equality --------------------------
//
// Every test below forces codegen to pair `LowerSelectField` with
// `LoadSelectPayload(Repr::kMessage)` or `LowerComparison(Repr::kMessage)`
// — both landed in G4.  Failures localise cleanly because G2/G3 scalar
// coverage above remains intact on the same fixture.

TEST(EvalE2ETest, NestedSelectReadsInnerField) {
  auto loaded = LoadCompiled("c.billing_address.city == \"Seattle\"",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  {
    celwasm::testdata::Customer msg;
    msg.mutable_billing_address()->set_city("Seattle");
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 1);
  }
  {
    celwasm::testdata::Customer msg;
    msg.mutable_billing_address()->set_city("Boston");
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->of.i32, 0);
  }
}

TEST(EvalE2ETest, NestedSelectThroughUnsetSubmessageReadsDefault) {
  // Proto3 reads through an unset submessage as the submessage's
  // default, so `c.billing_address.city` is `""` when billing_address
  // was never set.  Pins that G4 follows proto3 reflection semantics
  // rather than treating unset as an error / unknown.
  auto loaded = LoadCompiled("c.billing_address.city == \"\"",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, MessageEqualityTrueForStructurallyEqual) {
  auto loaded = LoadCompiled(
      "a == b", {"a:celwasm.testdata.Customer", "b:celwasm.testdata.Customer"});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer a;
  a.set_name("Ada");
  a.set_age(30);
  celwasm::testdata::Customer b;
  b.set_name("Ada");
  b.set_age(30);
  wasmtime_val_t arg_a = MessageAsExternref(*loaded, a);
  wasmtime_val_t arg_b = MessageAsExternref(*loaded, b);
  auto r = loaded->CallEval({arg_a, arg_b});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, MessageEqualityFalseForDifferentFields) {
  auto loaded = LoadCompiled(
      "a == b", {"a:celwasm.testdata.Customer", "b:celwasm.testdata.Customer"});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer a;
  a.set_name("Ada");
  celwasm::testdata::Customer b;
  b.set_name("Bob");
  wasmtime_val_t arg_a = MessageAsExternref(*loaded, a);
  wasmtime_val_t arg_b = MessageAsExternref(*loaded, b);
  auto r = loaded->CallEval({arg_a, arg_b});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2ETest, MessageInequalityInvertsMessageEq) {
  auto loaded = LoadCompiled(
      "a != b", {"a:celwasm.testdata.Customer", "b:celwasm.testdata.Customer"});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer a;
  a.set_age(1);
  celwasm::testdata::Customer b;
  b.set_age(2);
  wasmtime_val_t arg_a = MessageAsExternref(*loaded, a);
  wasmtime_val_t arg_b = MessageAsExternref(*loaded, b);
  auto r = loaded->CallEval({arg_a, arg_b});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, NestedMessageEqualityComposesSelectAndEq) {
  // Composition: two G4 `LowerSelectField(kMessage)` feeding into a G4
  // `LowerComparison(kMessage) → message_eq`.  Catches bugs where the
  // externref returned by LoadSelectPayload isn't a valid argument to
  // message_eq, or where wrap/unwrap loses identity across a round-trip
  // through $cel_refs.
  auto loaded = LoadCompiled(
      "a.billing_address == b.billing_address",
      {"a:celwasm.testdata.Customer", "b:celwasm.testdata.Customer"});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer a;
  a.mutable_billing_address()->set_city("NY");
  celwasm::testdata::Customer b;
  b.mutable_billing_address()->set_city("NY");
  wasmtime_val_t arg_a = MessageAsExternref(*loaded, a);
  wasmtime_val_t arg_b = MessageAsExternref(*loaded, b);
  auto r = loaded->CallEval({arg_a, arg_b});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

// ---- Multi-param + proto-field composition (M3 polish, #40) ----------------
//
// Task #40 scope: expressions that mix several param kinds or compose a
// proto field read with a CEL operator/macro.  Each case is the minimum
// that exercises one composition path — no wall-of-scenarios coverage;
// the per-type payload branches are already pinned above.
//
// The composition surface we care about here is the interaction between
// `cel_host.get_field` (which writes a `CelValue` into scratch) and the
// CEL operator that reads its payload.  A regression in e.g. `size()`
// not understanding a proto-sourced string would still pass the
// `LoadSelectPayload` string pass-through test because that only checks
// the payload load, not the downstream operator.

TEST(EvalE2ETest, MultiParamTwoMessagesConcatenateNames) {
  // Two message params composing a string concat on proto string
  // fields.  Breaks if the eval module's param ordering or the
  // externref plumbing gets either slot wrong.
  auto loaded = LoadCompiled(
      "a.name + \" & \" + b.name",
      {"a:celwasm.testdata.Customer", "b:celwasm.testdata.Customer"});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer a;
  a.set_name("Ada");
  celwasm::testdata::Customer b;
  b.set_name("Grace");
  wasmtime_val_t arg_a = MessageAsExternref(*loaded, a);
  wasmtime_val_t arg_b = MessageAsExternref(*loaded, b);
  auto r = loaded->CallEval({arg_a, arg_b});
  ASSERT_THAT(r.status(), IsOk());
  ASSERT_EQ(r->kind, WASMTIME_I32);
  auto s = DecodeStringAt(*loaded, r->of.i32);
  ASSERT_THAT(s.status(), IsOk());
  EXPECT_EQ(s->payload, "Ada & Grace");
}

TEST(EvalE2ETest, MultiParamMessagePlusScalarUintComparison) {
  // Mixes a message param with a scalar uint param.  The uint slot
  // has to be wired after the externref, and the comparison must see
  // CEL `uint` semantics (unsigned) despite both operands living in
  // i64 wasm slots.
  auto loaded = LoadCompiled("c.balance_cents > threshold",
                             {std::string(kCustomerSpec), "threshold:uint"});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer c;
  c.set_balance_cents(5'000);
  wasmtime_val_t msg_arg = MessageAsExternref(*loaded, c);
  auto r = loaded->CallEval({msg_arg, I64(1'000)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);

  auto r2 = loaded->CallEval({msg_arg, I64(10'000)});
  ASSERT_THAT(r2.status(), IsOk());
  EXPECT_EQ(r2->of.i32, 0);
}

TEST(EvalE2ETest, MultiParamTwoMessagesConditional) {
  // Two message params feeding a ternary that picks one or the other.
  // Exercises both the bool-from-field path (`is_premium`) and a
  // message-field pass-through on either branch — the scratch slot
  // has to be per-invocation, not per-select.
  auto loaded = LoadCompiled(
      "a.is_premium ? a.name : b.name",
      {"a:celwasm.testdata.Customer", "b:celwasm.testdata.Customer"});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer a;
  a.set_is_premium(true);
  a.set_name("Alpha");
  celwasm::testdata::Customer b;
  b.set_name("Beta");
  {
    wasmtime_val_t arg_a = MessageAsExternref(*loaded, a);
    wasmtime_val_t arg_b = MessageAsExternref(*loaded, b);
    auto r = loaded->CallEval({arg_a, arg_b});
    ASSERT_THAT(r.status(), IsOk());
    auto s = DecodeStringAt(*loaded, r->of.i32);
    ASSERT_THAT(s.status(), IsOk());
    EXPECT_EQ(s->payload, "Alpha");
  }
  a.set_is_premium(false);
  {
    wasmtime_val_t arg_a = MessageAsExternref(*loaded, a);
    wasmtime_val_t arg_b = MessageAsExternref(*loaded, b);
    auto r = loaded->CallEval({arg_a, arg_b});
    ASSERT_THAT(r.status(), IsOk());
    auto s = DecodeStringAt(*loaded, r->of.i32);
    ASSERT_THAT(s.status(), IsOk());
    EXPECT_EQ(s->payload, "Beta");
  }
}

TEST(EvalE2ETest, SizeOfProtoStringField) {
  // `size()` over a proto-sourced string — exercises the payload load
  // for kString (pass-through scratch offset) composing with the
  // `cel_size` runtime import.
  auto loaded = LoadCompiled("size(c.name)", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer c;
  c.set_name("héllo");  // 5 codepoints, 6 bytes; CEL size() on string is
                        // codepoint count.
  wasmtime_val_t arg = MessageAsExternref(*loaded, c);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 5);
}

TEST(EvalE2ETest, ProtoStringFieldStartsWith) {
  // Member-call `startsWith` on a proto-sourced string.  Breaks if
  // member dispatch assumes a literal/scratch-local operand shape and
  // can't accept the CelValue that `cel_host.get_field` writes.
  auto loaded =
      LoadCompiled("c.name.startsWith(\"Ad\")", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer c;
  c.set_name("Ada");
  wasmtime_val_t arg = MessageAsExternref(*loaded, c);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, ProtoIntFieldArithmetic) {
  // Proto int64 field + literal.  Result must be an i64 CEL int, and
  // the proto value (which goes through `LoadSelectPayload` kInt) has
  // to feed cleanly into the `+` lowering.
  auto loaded = LoadCompiled("c.user_id + 100", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer c;
  c.set_user_id(42);
  wasmtime_val_t arg = MessageAsExternref(*loaded, c);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 142);
}

TEST(EvalE2ETest, NestedProtoStringFieldConcatWithLiteral) {
  // Two nested-select reads of the same submessage, composed via `+`.
  // Pins that repeated `cel_host.get_field` calls from the same eval
  // function don't alias each other through the scratch slot, and
  // that nested-string loads work the same as flat ones.
  auto loaded = LoadCompiled(
      "c.billing_address.city + \", \" + c.billing_address.country",
      {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  celwasm::testdata::Customer c;
  c.mutable_billing_address()->set_city("Seattle");
  c.mutable_billing_address()->set_country("USA");
  wasmtime_val_t arg = MessageAsExternref(*loaded, c);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  ASSERT_EQ(r->kind, WASMTIME_I32);
  auto s = DecodeStringAt(*loaded, r->of.i32);
  ASSERT_THAT(s.status(), IsOk());
  EXPECT_EQ(s->payload, "Seattle, USA");
}

// ---- Partial-eval UNKNOWN propagation (M4 Slice E2a.1) --------------------
//
// Scope of this slice: the host `cel_host.get_field` trampoline is the
// single new UNKNOWN *producer*.  When a call site's rooted attribute
// path FULL-matches a host-configured `AttributePattern`, the slot is
// written with `CelValue{CEL_UNKNOWN}` and the field read is skipped.
// Downstream 3VL absorption through logical operators is a separate
// slice (M4 Slice F) — the tests here verify the producer only:
// direct UNKNOWN at the top of the expression, pattern match modes
// (FULL / PARTIAL / NONE), wildcards, and the empty-pattern path.
// End-user surface for an UNKNOWN result is
// `absl::InternalError("CallEval: result is UNKNOWN")` today —
// `DecodeSlot` elevates every non-OK kind to InternalError.

AttributePattern ParsePatternOrDie(absl::string_view text) {
  auto p = ParseUnknownAttributePattern(text);
  CHECK_OK(p);
  return *std::move(p);
}

bool ResultIsUnknown(const absl::StatusOr<wasmtime_val_t>& r) {
  return !r.ok() && absl::StrContains(r.status().message(), "UNKNOWN");
}

TEST(EvalE2EUnknownTest, FullMatchOnFlatSelectProducesUnknownTopLevel) {
  // Canonical happy path: pattern `c.name` exactly matches the only
  // select site, slot gets CEL_UNKNOWN, CallEval surfaces it.
  auto loaded = LoadCompiled("c.name", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.name"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  EXPECT_TRUE(ResultIsUnknown(r)) << r.status();
}

TEST(EvalE2EUnknownTest, EmptyPatternSetResolvesNormally) {
  // With zero patterns configured, the trampoline must never
  // short-circuit — proves `AttributeIsFullyUnknown` returns false
  // for every attribute when `unknown_patterns_` is empty.
  auto loaded = LoadCompiled("c.name", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  ASSERT_THAT(loaded->SetUnknownPatterns({}), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Grace");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk()) << "empty pattern set must not UNKNOWN";
  EXPECT_EQ(r->kind, WASMTIME_I32);
}

TEST(EvalE2EUnknownTest, PatternMismatchOnDifferentVariableResolvesNormally) {
  // Pattern roots on a different variable (`d` vs `c`) — match is
  // NONE, normal field read path.
  auto loaded = LoadCompiled("c.name == \"Ada\"", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("d.name"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1) << "field read must happen when no pattern matches";
}

TEST(EvalE2EUnknownTest, PatternMismatchOnDifferentQualifierResolvesNormally) {
  // Same variable but a different qualifier — pattern `c.age`
  // against attribute `c.name` is NONE (qualifier mismatch, neither
  // wildcard).
  auto loaded = LoadCompiled("c.name", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.age"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Grace");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
}

TEST(EvalE2EUnknownTest, WildcardFullMatchProducesUnknown) {
  // `c.*` is a single-wildcard pattern; it FULL-matches any
  // `c.<qualifier>` attribute (pattern_len = attr_len = 1, wildcard
  // accepts any concrete qualifier).
  auto loaded = LoadCompiled("c.name", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.*"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  EXPECT_TRUE(ResultIsUnknown(r)) << r.status();
}

TEST(EvalE2EUnknownTest, BareVariablePatternFullMatchesAnyField) {
  // A pattern with no qualifiers (`c`) FULL-matches every attribute
  // rooted at `c`, regardless of qualifier path length.
  auto loaded = LoadCompiled("c.name", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  EXPECT_TRUE(ResultIsUnknown(r)) << r.status();
}

TEST(EvalE2EUnknownTest, PartialMatchDoesNotShortCircuit) {
  // Pattern `c.name.surname` is longer than the select path `c.name`,
  // so `IsMatch` returns PARTIAL — the trampoline must fall through
  // to the real field read, not produce UNKNOWN.
  auto loaded = LoadCompiled("c.name == \"Ada\"", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.name.surname"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1) << "PARTIAL must not trigger UNKNOWN";
}

TEST(EvalE2EUnknownTest, MultiplePatternsAnyFullMatchWins) {
  // Two patterns; only the second FULL-matches.  Checks that the
  // trampoline iterates the full set, not just index 0.
  auto loaded = LoadCompiled("c.name", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("d.foo"));   // NONE
  patterns.push_back(ParsePatternOrDie("c.name"));  // FULL
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  EXPECT_TRUE(ResultIsUnknown(r)) << r.status();
}

// ---- UNKNOWN-absorption gap (Slice F, rows 9-21) --------------------------
//
// E2a.1 ships the UNKNOWN *producer* only.  Every expression below
// produces an UNKNOWN value on one side of a non-absorbing op
// (equality, ordered compare, arithmetic, string op) and feeds the
// result into a 3VL absorber (`&&` / `||`).  Per `doc/langdef.md`
// §partial-evaluation, the spec answer is the absorber's short-circuit
// result — but today the non-absorbing ops consume the UNKNOWN as if
// it were an OK value, so the absorber never sees it.  These assert
// the spec answers and will go green when Slice F
// (doc/implementation-plan/m4-slice-f-3vl-absorption.md) lands.
//
// Row numbers below refer to that doc's UNKNOWN-source table.

TEST(EvalE2EUnknownTest,
     UnknownThroughEqualityAbsorbedByAnd) {  // Slice F row 10
  auto loaded =
      LoadCompiled("c.age == 0 && false", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.age"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2EUnknownTest,
     UnknownThroughOrderedCompareAbsorbedByOr) {  // Slice F row 9
  auto loaded =
      LoadCompiled("c.age > 10 || true", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.age"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownThroughStringEqAbsorbedByOr) {  // Slice F row 15
  auto loaded =
      LoadCompiled("c.name == \"foo\" || true", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.name"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownThroughStartsWithAbsorbedByAndFalse) {  // Slice F row 16
  // `UNKNOWN && OK(false) → OK(false)` per langdef.md §11.1 — the
  // startsWith call's UNKNOWN return must be absorbed by `&& false`.
  auto loaded = LoadCompiled("c.name.startsWith(\"x\") && false",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.name"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2EUnknownTest,
     UnknownThroughBytesEqAbsorbedByOr) {  // Slice F row 17
  auto loaded = LoadCompiled("c.session_token == b\"foo\" || true",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.session_token"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownThroughSizeThenCompareAbsorbed) {  // Slice F row 21
  // size(UNKNOWN) must propagate UNKNOWN (not collapse to 0); then
  // `== 0` preserves UNKNOWN; then `|| true` absorbs.
  auto loaded =
      LoadCompiled("size(c.name) == 0 || true", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.name"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownThroughMessageEqAbsorbedByOr) {  // Slice F row 14
  // Message equality where one operand is an UNKNOWN sub-message: the
  // host's descriptor-aware `message_eq` never runs because
  // `cel_message_eq_prologue_v` short-circuits to the UNKNOWN offset,
  // which the wrapping `|| true` then absorbs.  Proves the boxed
  // message-eq path (Step 5) wires through to the 3VL absorber.
  auto loaded = LoadCompiled(
      "a.billing_address == b.billing_address || true",
      {"a:celwasm.testdata.Customer", "b:celwasm.testdata.Customer"});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("a.billing_address"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer a;
  a.mutable_billing_address()->set_city("NY");
  celwasm::testdata::Customer b;
  b.mutable_billing_address()->set_city("NY");
  wasmtime_val_t arg_a = MessageAsExternref(*loaded, a);
  wasmtime_val_t arg_b = MessageAsExternref(*loaded, b);
  auto r = loaded->CallEval({arg_a, arg_b});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownEqualityBothOperandsUnknownAbsorbedByOr) {  // Slice F row 11
  auto loaded =
      LoadCompiled("c.age == c.user_id || true", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.age"));
  patterns.push_back(ParsePatternOrDie("c.user_id"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownUintOrderedCompareAbsorbedByOr) {  // Slice F row 12
  auto loaded =
      LoadCompiled("c.priority >= 0u || true", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.priority"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownDoubleOrderedCompareAbsorbedByOr) {  // Slice F row 13
  auto loaded = LoadCompiled("c.credit_score < 1.0 || true",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.credit_score"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownBoolEqualityPropagatesThroughOrFalse) {  // Slice F row 20
  // Spec: `UNKNOWN || OK(false) → UNKNOWN`.  `|| false` does NOT
  // short-circuit past UNKNOWN (only `|| true` absorbs), so the
  // bool-equality UNKNOWN must ride through to the host.
  auto loaded = LoadCompiled("(c.is_premium == true) || false",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.is_premium"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  EXPECT_TRUE(ResultIsUnknown(r)) << r.status();
}

TEST(EvalE2EUnknownTest,
     UnknownThroughArithThenCompareAbsorbed) {  // Slice F row 18
  auto loaded =
      LoadCompiled("(c.age + 1) == 0 || true", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.age"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownInTernaryCondAbsorbedByOr) {  // Slice F row 19
  // Row 19: `(msg.int_field > 0 ? 1 : 2) == 1 || true` → true.
  // UNKNOWN propagates through the ternary cond's `>` compare, Slice F
  // step 6 routes the ternary through `LowerConditionalBoxed` from the
  // boxed outer `==`, so the UNKNOWN CelValue reaches the wrapping
  // `||` as a value instead of the ternary short-circuiting `$eval`.
  auto loaded = LoadCompiled("(c.age > 0 ? 1 : 2) == 1 || true",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.age"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest,
     UnknownAndErrorInArithSubtreeErrorDominates) {  // Slice F row 22
  // UNKNOWN and ERROR in the same arith subtree: cel_status_either picks
  // ERROR over UNKNOWN (ERROR > UNKNOWN per langdef.md §11.1); the `||
  // true` absorber then short-circuits, so the whole expression is true.
  auto loaded = LoadCompiled("(c.age + (1 / 0)) == 0 || true",
                             {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());
  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.age"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());
  celwasm::testdata::Customer msg;
  wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
  auto r = loaded->CallEval({arg});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2EUnknownTest, SetUnknownPatternsIsIdempotentAcrossCalls) {
  // SetUnknownPatterns replaces; calling again with an empty set
  // restores the normal field read path.  Prevents per-call state
  // leaking between invocations on the same LoadedEval.
  auto loaded = LoadCompiled("c.name", {std::string(kCustomerSpec)});
  ASSERT_THAT(loaded.status(), IsOk());

  std::vector<AttributePattern> patterns;
  patterns.push_back(ParsePatternOrDie("c.name"));
  ASSERT_THAT(loaded->SetUnknownPatterns(std::move(patterns)), IsOk());

  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  {
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    EXPECT_TRUE(ResultIsUnknown(r)) << r.status();
  }
  // Now clear; the next call must resolve normally.
  ASSERT_THAT(loaded->SetUnknownPatterns({}), IsOk());
  {
    wasmtime_val_t arg = MessageAsExternref(*loaded, msg);
    auto r = loaded->CallEval({arg});
    ASSERT_THAT(r.status(), IsOk()) << "cleared pattern set must not UNKNOWN";
  }
}

}  // namespace
}  // namespace celwasm
