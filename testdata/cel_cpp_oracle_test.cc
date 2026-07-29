// Differential test suite: evaluate each expression through BOTH
// cel-cpp (the oracle) and OUR pipeline and assert they agree —
// "agree" meaning cel-cpp errors <=> we error, and on a value verdict
// our decoded value matches the oracle's under the conformance
// comparator (the same equality the conformance gate uses).
//
// This both smoke-tests the oracle itself and pins the M20 contract —
// out-of-range enum / int32 / uint32 field assignment is a CEL error
// VALUE, in-range assignment round-trips — directly against the
// reference implementation rather than against pre-baked corpus
// literals.  The M20 expressions mirror the corpus rows the milestone
// flips:
//   - enums.textproto  legacy_proto{2,3}/assign_standalone_int_too_{big,neg}
//   - dynamic.textproto int32/uint32 field_assign_proto{2,3}_range
// plus an INT32 boundary matrix (MIN, MAX, ±1 past each, 0).
//
// NOTE.  Our public API now lives in `celwasm` (no symbols in
// `namespace cel`), so it no longer collides at link time with the
// `cel::` symbols cel-cpp pulls in through the oracle — the
// our-pipeline-and-oracle differential links cleanly in one binary.
// It stays consolidated here for cohesion, not out of necessity.

#include "testdata/cel_cpp_oracle.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "conformance/runner.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "google/protobuf/generated_message_reflection.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

constexpr absl::string_view kP2 = "cel.expr.conformance.proto2";
constexpr absl::string_view kP3 = "cel.expr.conformance.proto3";

// Force the proto2/proto3 conformance descriptors into the generated
// pool so container-qualified names resolve in OUR pipeline (the oracle
// links its own copy in cel_cpp_oracle.cc).
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<
          ::cel::expr::conformance::proto2::TestAllTypes>();
      google::protobuf::LinkMessageReflection<
          ::cel::expr::conformance::proto3::TestAllTypes>();
      return 0;
    }();

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Evaluate `source` through OUR pipeline under `container`.  Returns a
// non-OK status only on a HOST TRAP — the poison contract means an
// out-of-range field assignment evaluates OK to a CEL error value, not
// a trap, so a non-OK here is a genuine regression.
absl::StatusOr<Value> EvalOurs(absl::string_view source,
                               absl::string_view container) {
  CompilerOptions opts;
  opts.container = std::string(container);
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  if (!compiler.ok()) return compiler.status();
  auto program = compiler->Compile(source, opts);
  if (!program.ok()) return program.status();
  auto instance = GlobalEngine().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a;
  return instance->Eval(a);
}

// The core differential assertion.  Both engines must reach the same
// verdict on `source`; on a value verdict, the values must match under
// the conformance comparator.
void ExpectAgree(absl::string_view source, absl::string_view container) {
  auto oracle = testdata::EvalWithCelCpp(source, container);
  ASSERT_THAT(oracle.status(), IsOk()) << source;

  auto ours = EvalOurs(source, container);
  ASSERT_THAT(ours.status(), IsOk())
      << source << " — our pipeline trapped instead of producing a value";

  if (oracle->is_error) {
    EXPECT_TRUE(ours->IsError())
        << source << ": cel-cpp errored (\"" << oracle->error_message
        << "\") but we yielded kind " << static_cast<int>(ours->kind());
  } else {
    ASSERT_FALSE(ours->IsError())
        << source << ": cel-cpp produced a value but we errored";
    EXPECT_THAT(conformance::CompareValue(*ours, oracle->value), IsOk())
        << source;
  }
}

// ── Oracle smoke cases (non-proto + a proto field select) ──

TEST(CelCppOracle, IntArithmeticAgrees) {
  ExpectAgree("1 + 1", kP3);
}
// Empty-needle `replace` / empty-separator `split` take dedicated
// interleave/explode code paths (runtime/cel_string_ext_search.cc /
// cel_string_ext_list.cc) whose semantics were transcribed from
// cel-cpp — these differentials keep the transcription honest.
// Relational operators on string / bytes / bool, the uint arithmetic
// arms, and the `%e` / `%o` format verbs each have a dedicated runtime
// kernel that no e2e row exercised; these differentials fix the
// expected values against cel-cpp before the e2e rows assert them.
TEST(CelCppOracle, StringRelationalAgrees) {
  for (const char* src : {R"("b" > "a")", R"("a" > "b")", R"("b" >= "b")",
                          R"("a" >= "b")", R"("a" <= "a")", R"("b" <= "a")"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, BytesRelationalAgrees) {
  for (const char* src : {R"(b"b" > b"a")", R"(b"a" < b"b")",
                          R"(b"b" >= b"b")", R"(b"a" <= b"b")"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, BoolRelationalAgrees) {
  for (const char* src :
       {"true > false", "true >= true", "false <= true", "false >= true"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, UintArithmeticAgrees) {
  for (const char* src : {"3u - 1u", "6u / 2u", "7u % 3u"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, DoubleNegAndSubAgree) {
  for (const char* src : {"-1.5", "1.5 - 0.5"}) ExpectAgree(src, kP3);
}
TEST(CelCppOracle, ScientificAndOctalFormatAgree) {
  ExpectAgree(R"("%e".format([1.5]))", kP3);
  ExpectAgree(R"("%o".format([8]))", kP3);
}
TEST(CelCppOracle, LastIndexOfAgrees) {
  ExpectAgree(R"("abcb".lastIndexOf("b"))", kP3);
  ExpectAgree(R"("abcb".lastIndexOf("b", 1))", kP3);
}
// The empty-needle branch of indexOf / lastIndexOf walks code points
// to honour `pos`, and its multi-byte path plus the past-the-end
// return had no non-corpus workload.  Substring at exactly the
// code-point count is the empty tail; one past it is an error.
TEST(CelCppOracle, EmptyNeedleAndSubstringEdgesAgree) {
  for (const char* src : {R"("héllo".indexOf("", 2))",
                          R"("héllo".lastIndexOf("", 2))",
                          R"("abc".substring(3))",
                          R"("héllo".substring(5))",
                          R"("héllo".substring(6))"}) {
    ExpectAgree(src, kP3);
  }
}

// Index-by-non-int, duplicate map keys, cross-kind element equality,
// and the multi-byte / negative-position search paths — each has its
// own runtime arm and only the conformance corpus was reaching them.
TEST(CelCppOracle, DynIndexKindsAgree) {
  for (const char* src : {"[1,2][dyn(1u)]", "[1,2][dyn(1.0)]",
                          "[1,2][dyn(1.5)]", "[1,2][dyn(\"x\")]"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, DuplicateMapKeyAgrees) {
  ExpectAgree(R"({1: "a", 1: "b"})", kP3);
}
TEST(CelCppOracle, ElementEqualityAcrossKindsAgrees) {
  for (const char* src :
       {R"([b"a"] == [b"a"])", "[null] == [null]",
        R"([duration("1s")] == [duration("1s")])",
        "[timestamp(0)] == [timestamp(0)]",
        R"([b"a"] == [b"b"])"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, SearchPositionEdgesAgree) {
  for (const char* src : {R"("héllo".indexOf("l", 1))",
                          R"("héllo".lastIndexOf("l", 3))",
                          R"("abc".indexOf("a", -1))",
                          R"("abc".lastIndexOf("a", -1))"}) {
    ExpectAgree(src, kP3);
  }
}

// The math extension's kernels guard their operand kind at runtime.
// A `dyn()`-wrapped operand of the wrong kind clears the static-subset
// gate at compile time and then trips the guard — the same route the
// conformance corpus takes to reach these arms.
TEST(CelCppOracle, MathExtOperandKindGuardsAgree) {
  for (const char* src : {"math.ceil(dyn(1))", "math.floor(dyn(1))",
                          "math.round(dyn(1))", "math.trunc(dyn(1))",
                          "math.isInf(dyn(1))", "math.isNaN(dyn(1))",
                          "math.isFinite(dyn(1))"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, MathExtBitwiseKindGuardsAgree) {
  for (const char* src : {"math.bitAnd(dyn(1), 2u)", "math.bitOr(dyn(1), 2u)",
                          "math.bitXor(dyn(1), 2u)"}) {
    ExpectAgree(src, kP3);
  }
}

// Every format verb dispatches per operand kind, and the suite only
// drove the int / double / string arms; uint, bool and bytes operands
// take separate branches, as do the NaN / +-Infinity special cases.
TEST(CelCppOracle, FormatVerbOperandKindsAgree) {
  for (const char* src : {
           R"("%s".format([true]))", R"("%s".format([1u]))",
           R"("%s".format([b"ab"]))", R"("%d".format([1u]))",
           R"("%d".format([1.7]))", R"("%b".format([5u]))",
           R"("%b".format([true]))", R"("%o".format([8u]))",
           R"("%x".format([255u]))", R"("%x".format([b"ab"]))",
           R"("%X".format([255u]))"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, FormatNonFiniteDoublesAgree) {
  for (const char* src : {R"("%f".format([double("inf")]))",
                          R"("%f".format([double("nan")]))",
                          R"("%e".format([double("-inf")]))",
                          R"("%s".format([double("inf")]))"}) {
    ExpectAgree(src, kP3);
  }
}

// Arithmetic overflow / divide / modulus errors: each kernel has its
// own poison arm, and only `1 / 0` had any e2e row.
TEST(CelCppOracle, IntArithmeticErrorsAgree) {
  for (const char* src : {"9223372036854775807 + 1", "9223372036854775807 * 2",
                          "-9223372036854775807 - 2", "1 % 0"}) {
    ExpectAgree(src, kP3);
  }
}
TEST(CelCppOracle, UintArithmeticErrorsAgree) {
  for (const char* src : {"18446744073709551615u + 1u", "0u - 1u",
                          "18446744073709551615u * 2u", "1u / 0u",
                          "1u % 0u"}) {
    ExpectAgree(src, kP3);
  }
}
// `strings.quote` escapes seven C-style control characters plus the
// double quote; each is a separate arm of the escape switch.
TEST(CelCppOracle, StringsQuoteEscapesAgree) {
  for (const char* src :
       {R"(strings.quote("a\tb"))", R"(strings.quote("a\nb"))",
        R"(strings.quote("a\rb"))", R"(strings.quote("a\bb"))",
        R"(strings.quote("a\fb"))", R"(strings.quote("a\vb"))",
        R"(strings.quote("a\ab"))", R"(strings.quote("q\"z"))"}) {
    ExpectAgree(src, kP3);
  }
}

TEST(CelCppOracle, TimestampAndTypeFormatAgree) {
  ExpectAgree(R"("%s".format([timestamp(0)]))", kP3);
  ExpectAgree(R"("%s".format([type(1)]))", kP3);
}
TEST(CelCppOracle, StringExtRangeErrorsAgree) {
  for (const char* src : {R"("abc".charAt(9))", R"("abc".substring(9))",
                          R"("abc".substring(2, 1))"}) {
    ExpectAgree(src, kP3);
  }
}

TEST(CelCppOracle, MultiByteUtf8Agrees) {
  ExpectAgree(R"(size("héllo"))", kP3);
  ExpectAgree(R"("héllo".charAt(1))", kP3);
}

// Duration `%s` rendering has three fractional-digit widths (3 / 6 / 9)
// plus a zero and a negative form, and the widths are cel-cpp's, not a
// generic float format — these differentials pin every branch of
// `AppendDurationCanonical` against the reference implementation.
TEST(CelCppOracle, DurationFormatZeroAgrees) {
  ExpectAgree(R"("%s".format([duration("0s")]))", kP3);
}
TEST(CelCppOracle, DurationFormatNegativeAgrees) {
  ExpectAgree(R"("%s".format([duration("-1.5s")]))", kP3);
}
TEST(CelCppOracle, DurationFormatMillisAgrees) {
  ExpectAgree(R"("%s".format([duration("1.5s")]))", kP3);
}
TEST(CelCppOracle, DurationFormatMicrosAgrees) {
  ExpectAgree(R"("%s".format([duration("1.000001s")]))", kP3);
}
TEST(CelCppOracle, DurationFormatNanosAgrees) {
  ExpectAgree(R"("%s".format([duration("1.000000001s")]))", kP3);
}
TEST(CelCppOracle, DurationFormatNegativeSubSecondAgrees) {
  ExpectAgree(R"("%s".format([duration("-0.25s")]))", kP3);
}

TEST(CelCppOracle, StringReplaceEmptyNeedleAgrees) {
  ExpectAgree(R"("abc".replace("", "-"))", kP3);
}
TEST(CelCppOracle, StringReplaceEmptyNeedleLimitAgrees) {
  ExpectAgree(R"("abc".replace("", "-", 2))", kP3);
}
TEST(CelCppOracle, StringSplitEmptySepAgrees) {
  ExpectAgree(R"("abc".split("") == ["a", "b", "c"])", kP3);
}
TEST(CelCppOracle, StringSplitEmptySepLimitAgrees) {
  ExpectAgree(R"("abc".split("", 2))", kP3);
}
TEST(CelCppOracle, StringConcatAgrees) {
  ExpectAgree("'foo' + 'bar'", kP3);
}
TEST(CelCppOracle, BoolComparisonAgrees) {
  ExpectAgree("2 < 3", kP3);
}
TEST(CelCppOracle, ProtoFieldSelectAgrees) {
  ExpectAgree("TestAllTypes{single_int32: 7}.single_int32", kP3);
}

// Conformance rows `lists/index/zero_based_double` and
// `lists/index/zero_based_uint`: per the corpus, indexing a list with
// `dyn(0.0)` or `dyn(0u)` returns the int element at offset 0.
// Pins our runtime's heterogeneous list-index admission against the
// reference implementation.
TEST(CelCppOracle, ListIndexDoubleAgrees) {
  ExpectAgree("[7, 8, 9][dyn(0.0)]", kP3);
}
TEST(CelCppOracle, ListIndexUintAgrees) {
  ExpectAgree("[7, 8, 9][dyn(0u)]", kP3);
}
TEST(CelCppOracle, ListIndexNonIntegerDoubleAgrees) {
  // The corpus row `zero_based_double_error` expects an error here.
  ExpectAgree("[7, 8, 9][dyn(0.1)]", kP3);
}

// The oracle surfaces a CEL eval error as `is_error`, not as an
// `absl::Status` failure (which is reserved for harness/setup failure).
TEST(CelCppOracle, DivByZeroSurfacesAsCelError) {
  auto oracle = testdata::EvalWithCelCpp("1 / 0", kP3);
  ASSERT_THAT(oracle.status(), IsOk());
  EXPECT_TRUE(oracle->is_error) << "expected a CEL error value for 1/0";
}

// ── M20 enum standalone-field range rows (enums.textproto) ──

TEST(M20EnumRange, StandaloneEnumTooBigProto2) {
  ExpectAgree("TestAllTypes{standalone_enum: 5000000000}", kP2);
}
TEST(M20EnumRange, StandaloneEnumTooNegProto2) {
  ExpectAgree("TestAllTypes{standalone_enum: -7000000000}", kP2);
}
TEST(M20EnumRange, StandaloneEnumTooBigProto3) {
  ExpectAgree("TestAllTypes{standalone_enum: 5000000000}", kP3);
}
TEST(M20EnumRange, StandaloneEnumTooNegProto3) {
  ExpectAgree("TestAllTypes{standalone_enum: -7000000000}", kP3);
}

// ── M20 int32 / uint32 wrapper range rows (dynamic.textproto) ──

TEST(M20WrapperRange, Int32WrapperTooBigProto2) {
  ExpectAgree("TestAllTypes{single_int32_wrapper: 12345678900}", kP2);
}
TEST(M20WrapperRange, Int32WrapperTooNegProto3) {
  ExpectAgree("TestAllTypes{single_int32_wrapper: -998877665544332211}", kP3);
}
TEST(M20WrapperRange, Uint32WrapperTooBigProto2) {
  ExpectAgree("TestAllTypes{single_uint32_wrapper: 6111222333u}", kP2);
}
TEST(M20WrapperRange, Uint32WrapperTooBigProto3) {
  ExpectAgree("TestAllTypes{single_uint32_wrapper: 6111222333u}", kP3);
}

// ── M20 INT32 boundary matrix on the enum field.  In-range cases read
//    back the scalar so value verdicts compare as ints; out-of-range
//    cases construct (and error). ──

TEST(M20EnumBoundary, Zero) {
  ExpectAgree("TestAllTypes{standalone_enum: 0}.standalone_enum", kP3);
}
TEST(M20EnumBoundary, Int32Max) {
  ExpectAgree(
      absl::StrCat("TestAllTypes{standalone_enum: ",
                   std::numeric_limits<int32_t>::max(), "}.standalone_enum"),
      kP3);
}
TEST(M20EnumBoundary, Int32Min) {
  ExpectAgree(
      absl::StrCat("TestAllTypes{standalone_enum: ",
                   std::numeric_limits<int32_t>::min(), "}.standalone_enum"),
      kP3);
}
TEST(M20EnumBoundary, Int32MaxPlusOne) {
  ExpectAgree(
      absl::StrCat(
          "TestAllTypes{standalone_enum: ",
          static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1, "}"),
      kP3);
}
TEST(M20EnumBoundary, Int32MinMinusOne) {
  ExpectAgree(
      absl::StrCat(
          "TestAllTypes{standalone_enum: ",
          static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1, "}"),
      kP3);
}

// ── M20 int32 wrapper boundary: in-range round-trips on both engines. ──

TEST(M20WrapperBoundary, Int32WrapperMax) {
  ExpectAgree(
      "TestAllTypes{single_int32_wrapper: 2147483647}.single_int32_wrapper",
      kP3);
}
TEST(M20WrapperBoundary, Int32WrapperMin) {
  ExpectAgree(
      "TestAllTypes{single_int32_wrapper: -2147483648}.single_int32_wrapper",
      kP3);
}

// ── Tier-1 conversion bug fixes, validated against the real cel-cpp
//    oracle (known_bugs_test guards the same behaviors as standalone
//    regressions; here we pin them to cel-cpp directly). ──

// cel-cpp int(string)/uint(string) use absl::SimpleAtoi, which accepts a
// leading '+' (type_conversion_functions.cc:140,295).
TEST(Tier1Conversions, IntFromStringLeadingPlusAgrees) {
  ExpectAgree("int('+5')", kP3);
}
TEST(Tier1Conversions, UintFromStringLeadingPlusAgrees) {
  ExpectAgree("uint('+5')", kP3);
}
// cel-cpp rejects int(-2^63.0) as a range error (the double -2^63 is not a
// valid int conversion); our pipeline must error identically.
TEST(Tier1Conversions, IntFromDoubleMinIsRangeError) {
  ExpectAgree("int(-9223372036854775808.0)", kP3);
}

// ── `optional.ofNonZeroValue(<message>)` zero-value pins
//    (cleanup-backlog #10).  cel-cpp's message zero-predicate is
//    `ParsedMessageValue::IsZeroValue()` (third_party/cel-cpp/common/
//    values/parsed_message_value.cc:78): a message is zero iff its
//    unknown-field set is empty AND `Reflection::ListFields` returns
//    no set fields.  The direct EvalWithCelCpp pins lock the expected
//    verdicts empirically; the ExpectAgree variants additionally run
//    OUR pipeline (which used to `__builtin_trap()` in
//    `runtime/cel_optional.c::is_zero_value` on CEL_MESSAGE). ──

// The exact conformance row `optionals/optional_ofNonZeroValue_struct_
// optional_ofNonZeroValue_map_optindex_field`: the inner
// ofNonZeroValue(0.0) is None, so the struct is a zero TestAllTypes,
// so the outer ofNonZeroValue is None → hasValue() is false.
constexpr absl::string_view kOfNonZeroValueConformanceRowExpr =
    "optional.ofNonZeroValue(TestAllTypes{?single_double_wrapper: "
    "optional.ofNonZeroValue(0.0)}).hasValue()";

TEST(OptionalOfNonZeroValueMessage, ConformanceRowOracleIsFalse) {
  auto oracle =
      testdata::EvalWithCelCpp(kOfNonZeroValueConformanceRowExpr, kP2);
  ASSERT_THAT(oracle.status(), IsOk());
  ASSERT_FALSE(oracle->is_error) << oracle->error_message;
  ASSERT_TRUE(oracle->value.has_bool_value());
  EXPECT_FALSE(oracle->value.bool_value());
}

TEST(OptionalOfNonZeroValueMessage, ZeroMessageOracleIsFalse) {
  auto oracle = testdata::EvalWithCelCpp(
      "optional.ofNonZeroValue(TestAllTypes{}).hasValue()", kP2);
  ASSERT_THAT(oracle.status(), IsOk());
  ASSERT_FALSE(oracle->is_error) << oracle->error_message;
  ASSERT_TRUE(oracle->value.has_bool_value());
  EXPECT_FALSE(oracle->value.bool_value());
}

TEST(OptionalOfNonZeroValueMessage, NonZeroMessageOracleIsTrue) {
  auto oracle = testdata::EvalWithCelCpp(
      "optional.ofNonZeroValue(TestAllTypes{single_int32: 1}).hasValue()", kP2);
  ASSERT_THAT(oracle.status(), IsOk());
  ASSERT_FALSE(oracle->is_error) << oracle->error_message;
  ASSERT_TRUE(oracle->value.has_bool_value());
  EXPECT_TRUE(oracle->value.bool_value());
}

// Differential: cel-cpp and OUR pipeline must agree on all three.
TEST(OptionalOfNonZeroValueMessage, ConformanceRowAgrees) {
  ExpectAgree(kOfNonZeroValueConformanceRowExpr, kP2);
}
TEST(OptionalOfNonZeroValueMessage, ZeroMessageAgrees) {
  ExpectAgree("optional.ofNonZeroValue(TestAllTypes{}).hasValue()", kP2);
}
TEST(OptionalOfNonZeroValueMessage, NonZeroMessageAgrees) {
  ExpectAgree(
      "optional.ofNonZeroValue(TestAllTypes{single_int32: 1}).hasValue()", kP2);
}

// ── Partial-eval oracle: pins cel-cpp's unknown-attribute semantics,
//    the empirical reference that e2e/partial_eval_test.cc asserts
//    OUR pipeline against.  (Reading cel-cpp source is not enough —
//    these RUN cel-cpp with unknown processing on.) ──

cel::expr::Value OracleInt(int64_t x) {
  cel::expr::Value v;
  v.set_int64_value(x);
  return v;
}

cel::expr::Value OracleListOfInts(const std::vector<int64_t>& xs) {
  cel::expr::Value v;
  for (int64_t x : xs) {
    v.mutable_list_value()->add_values()->set_int64_value(x);
  }
  return v;
}

testdata::OracleResult PartialOracleOk(
    absl::string_view source, const std::vector<testdata::OracleVar>& vars,
    const std::vector<std::string>& patterns) {
  auto r = testdata::PartialEvalWithCelCpp(source, kP3, vars, patterns);
  ABSL_CHECK_OK(r) << source;
  return *std::move(r);
}

// `x + 1` with `x` unknown → unknown.  cel-cpp's own
// unknowns_end_to_end_test.cc:169 pins the equivalent (`var1 > 3` with
// `var1` an unknown, unbound attribute → UnknownSet); confirmed here
// through our oracle harness.
TEST(PartialEvalOracle, WholeScalarVarUnknownThroughArithmetic) {
  auto r = PartialOracleOk("x + 1", {{"x", std::nullopt}}, {"x"});
  EXPECT_TRUE(r.is_unknown) << "x+1 with x unknown must be unknown";
}

// A bound value does NOT defeat a matching unknown pattern.
TEST(PartialEvalOracle, BoundButUnknownIgnoresBinding) {
  auto r = PartialOracleOk("x", {{"x", OracleInt(99)}}, {"x"});
  EXPECT_TRUE(r.is_unknown);
}

// THE cleanup-backlog #14 reference: a comprehension over an UNKNOWN
// range is unknown (not the empty-range identity our pipeline currently
// returns).  cel-cpp comprehension_step.cc:165-169 routes a kUnknown
// range to `result = range`; this confirms that verdict empirically.
TEST(PartialEvalOracle, ComprehensionOverUnknownRangeIsUnknown) {
  auto r =
      PartialOracleOk("xs.exists(e, e > 0)", {{"xs", std::nullopt}}, {"xs"});
  EXPECT_TRUE(r.is_unknown)
      << "exists over an unknown range must be unknown (backlog #14)";
}

// Error-vs-unknown precedence for a strict binary op
// (doc/design/03-abi-and-memory.md §8.3): cel-cpp's NoOverloadResult
// (eval/eval/function_step.cc:202-223) scans the args for an
// ErrorValue BEFORE merging unknowns, so ERROR dominates UNKNOWN
// regardless of operand order.  Both orders pinned here.
TEST(PartialEvalOracle, UnknownPlusErrorIsError) {
  auto r = PartialOracleOk("x + (1 / 0)", {{"x", std::nullopt}}, {"x"});
  EXPECT_FALSE(r.is_unknown)
      << "unknown(a) + error(b) must propagate the ERROR, not the unknown";
  EXPECT_TRUE(r.is_error);
}

TEST(PartialEvalOracle, ErrorPlusUnknownIsError) {
  auto r = PartialOracleOk("(1 / 0) + x", {{"x", std::nullopt}}, {"x"});
  EXPECT_FALSE(r.is_unknown)
      << "error(a) + unknown(b) must propagate the ERROR";
  EXPECT_TRUE(r.is_error);
}

// A pattern targeting the LOOP variable name is a no-op: the iteration
// variable is not an activation attribute root, so the pattern matches
// nothing and the comprehension runs concretely.
TEST(PartialEvalOracle, PatternOnLoopVarIsNoOp) {
  auto r = PartialOracleOk("xs.exists(e, e > 10)",
                           {{"xs", OracleListOfInts({1, 2, 30})}}, {"e"});
  ASSERT_FALSE(r.is_unknown) << "loop var `e` cannot be patterned";
  ASSERT_FALSE(r.is_error);
  EXPECT_TRUE(r.value.bool_value()) << "30 > 10 → exists is true";
}

// ── m32 SwissTable map-index numeric-key canonicalization gate
//    (m32-swisstable-map-index.md §5, §5.1, decision §14 #4).
//
//    The SwissTable invariant is: keys cel-cpp considers EQUAL must hash
//    identically.  The hash kernel canonicalizes `int N`, `uint N`, and
//    integral `double N.0` to one token so they collide.  The P0 merge
//    gate is the `≥ 2^53` boundary: above it an integral double can
//    compare-equal to a RANGE of int64s (the double has only 52 mantissa
//    bits), so truncation-to-token can pick a different int than the one
//    a linear scan would compare-equal.  These cases oracle-confirm where
//    cel-cpp's cross-type numeric equality is exact vs lossy, freezing
//    the `≥ 2^53 → linear-scan fallback` threshold before the kernel is
//    locked.  EvalWithCelCpp takes only (source, container) — every case
//    is a literal expression.
//
//    2^53     = 9007199254740992
//    2^53 + 1 = 9007199254740993  (NOT representable as a double; the
//               double literal 9007199254740993.0 rounds to 2^53)
//    2^53 + 2 = 9007199254740994  (representable)

// Helper: assert a literal expression evaluates to a non-error bool and
// return its value.
bool OracleBool(absl::string_view source) {
  auto r = testdata::EvalWithCelCpp(source, kP3);
  ABSL_CHECK_OK(r.status()) << source;
  ABSL_CHECK(!r->is_error) << source << ": " << r->error_message;
  ABSL_CHECK(r->value.has_bool_value()) << source << ": not a bool result";
  return r->value.bool_value();
}

// ---- Cross-type map-key membership (the membership shape the index
//      must preserve: a uint/double lookup key finding an int stored
//      key).
//
//      KEY FINDING: cel-cpp's CHECKER rejects a homogeneously-typed
//      cross-type membership expression — `1u in {1:'a'}` is a
//      compile/type-check error because the map literal's key type is
//      `int` and the `in` overload requires the element type to match.
//      Cross-type numeric membership only reaches the RUNTIME (where the
//      numeric-aware key compare lives) when the lookup key is `dyn(...)`
//      (or the map is `dyn`).  The hash kernel's cross-type
//      canonicalization is therefore exercised exactly on the
//      `dyn`-wrapped lookup path; the homogeneous literal never gets
//      there.  Both shapes are pinned below. ----

TEST(MapKeyNumericCrossType, UintLiteralKeyRejectedByChecker) {
  // No dyn(): the checker rejects uint-in-map<int,...>.
  auto r = testdata::EvalWithCelCpp("1u in {1:'a'}", kP3);
  EXPECT_FALSE(r.status().ok())
      << "expected a type-check failure for cross-type membership";
}
TEST(MapKeyNumericCrossType, UintKeyFindsIntEntryViaDyn) {
  EXPECT_TRUE(OracleBool("dyn(1u) in {1:'a'}"));
}
TEST(MapKeyNumericCrossType, DoubleKeyFindsIntEntryViaDyn) {
  EXPECT_TRUE(OracleBool("dyn(1.0) in {1:'a'}"));
}
TEST(MapKeyNumericCrossType, DoubleKeyFindsUintEntryViaDyn) {
  EXPECT_TRUE(OracleBool("dyn(2.0) in {1:'a', 2u:'b'}"));
}
TEST(MapKeyNumericCrossType, NonIntegralDoubleKeyMissesViaDyn) {
  // A non-integral double cannot compare-equal to any integer key.
  EXPECT_FALSE(OracleBool("dyn(1.5) in {1:'a', 2:'b'}"));
}

// ---- Cross-type equality at / around 2^53.  These pin where int↔double
//      equality is EXACT and where rounding makes a double equal a
//      neighbor int.
//
//      As with membership, heterogeneous `==` is a RUNTIME feature: the
//      checker has no `int == double` overload, so a homogeneous literal
//      comparison fails type-check.  `dyn(...)` on one side routes to the
//      runtime cross-type equality kernel (the same numeric compare the
//      map key path uses).  This is itself a finding — cross-type numeric
//      equality is reachable only through `dyn`. ----

TEST(MapKeyNumericCrossType, IntEqDoubleHomogeneousRejectedByChecker) {
  auto r =
      testdata::EvalWithCelCpp("9007199254740992 == 9007199254740992.0", kP3);
  EXPECT_FALSE(r.status().ok())
      << "expected a type-check failure for homogeneous int==double";
}
TEST(MapKeyNumericCrossType, IntEqDoubleExactAt2Pow53) {
  // 2^53 is exactly representable; equality is exact.
  EXPECT_TRUE(OracleBool("dyn(9007199254740992) == 9007199254740992.0"));
}
TEST(MapKeyNumericCrossType, IntPlus1EqDoubleAt2Pow53) {
  // OBSERVED: 2^53+1 (int) == 2^53.0 (double) is TRUE.  cel-cpp's
  // cross-type `==` does NOT round the int to double (which would make
  // them unequal — (double)(2^53+1) == 2^53 only by coincidence here);
  // it compares MATHEMATICALLY, and because the double 2^53.0 sits within
  // rounding range of 2^53+1 cel-cpp reports equal.  In fact cel-cpp's
  // numeric cross-compare (`cel::internal::Number`) treats an integral
  // double as the integer it rounds to under `==`: 2^53.0 widens to the
  // int 2^53, and... see the asymmetry probe below — `==` and map-`in`
  // DISAGREE here.  The raw observed verdict is pinned:
  EXPECT_TRUE(OracleBool("dyn(9007199254740993) == 9007199254740992.0"));
}
TEST(MapKeyNumericCrossType, IntEqDoubleLiteralRoundsDown) {
  // The double literal 9007199254740993.0 is NOT representable; it rounds
  // to 2^53 (9007199254740992.0).  OBSERVED: 2^53+1 (int) == that double
  // is TRUE — same lossy verdict as the 2^53.0-literal case above.
  EXPECT_TRUE(OracleBool("dyn(9007199254740993) == 9007199254740993.0"));
}
TEST(MapKeyNumericCrossType, IntEqRoundedDoubleNeighbor) {
  // The double literal 9007199254740993.0 rounds to 2^53; so 2^53 (int)
  // compares equal to it.
  EXPECT_TRUE(OracleBool("dyn(9007199254740992) == 9007199254740993.0"));
}
TEST(MapKeyNumericCrossType, IntPlus2EqDoubleExact) {
  // 2^53+2 is representable as a double; equality is exact.
  EXPECT_TRUE(OracleBool("dyn(9007199254740994) == 9007199254740994.0"));
}
TEST(MapKeyNumericCrossType, UintEqDoubleAt2Pow53Lossy) {
  // uint side: OBSERVED 2^53+1 (uint) == 2^53.0 (double) is TRUE — same
  // lossy verdict as the int side.
  EXPECT_TRUE(OracleBool("dyn(9007199254740993u) == 9007199254740992.0"));
}
TEST(MapKeyNumericCrossType, UintEqDoubleExactAt2Pow53) {
  EXPECT_TRUE(OracleBool("dyn(9007199254740992u) == 9007199254740992.0"));
}
TEST(MapKeyNumericCrossType, IntEqDoubleExactBelow2Pow53) {
  // Just below 2^53 every int64 is exactly representable; equality exact.
  EXPECT_TRUE(OracleBool("dyn(9007199254740991) == 9007199254740991.0"));
}

// ---- Large-int double map lookup: the actual index probe shape — a
//      double lookup key probing for a large-int stored key. ----

TEST(MapKeyNumericCrossType, DoubleAt2Pow53FindsIntEntry) {
  EXPECT_TRUE(OracleBool("dyn(9007199254740992.0) in {9007199254740992: 'a'}"));
}
TEST(MapKeyNumericCrossType, DoubleLiteralRoundsToStoredIntKey) {
  // The lookup-key double 9007199254740993.0 rounds to 2^53; the stored
  // int key is 2^53.  Does the double find it?  (This is exactly the
  // §5.1 hazard: truncation-to-token would token-ize the double as
  // 2^53+1 and MISS, but cel-cpp's linear scan compares double-equal and
  // HITS.)
  EXPECT_TRUE(OracleBool("dyn(9007199254740993.0) in {9007199254740992: 'a'}"));
}
TEST(MapKeyNumericCrossType, DoubleAt2Pow53MissesNeighborIntKey) {
  // OBSERVED: FALSE — and this is THE load-bearing asymmetry.  The
  // lookup-key double 9007199254740992.0 (== 2^53) probing a map whose
  // only key is the int 2^53+1 returns NO MATCH, even though the `==`
  // operator reports `2^53+1 == 2^53.0` is TRUE (see IntPlus1Eq...
  // above).  cel-cpp's map key membership therefore does NOT use the same
  // lossy `==` comparison: map-key matching is EXACT (the double must
  // equal the int's exact mathematical value), so 2^53.0 ≠ 2^53+1 as map
  // keys.  This means a map-index hash kernel canonicalizing a double to
  // its TRUNCATED integer value matches cel-cpp's map semantics — and the
  // §5.1 hazard ("a double near a rounding boundary equals a RANGE of
  // ints") does NOT apply to MAP KEYS, only to the bare `==` operator.
  EXPECT_FALSE(
      OracleBool("dyn(9007199254740992.0) in {9007199254740993: 'a'}"));
}
TEST(MapKeyNumericCrossType, DoubleAt2Pow53Plus2FindsIntEntry) {
  // Both representable; exact hit.
  EXPECT_TRUE(OracleBool("dyn(9007199254740994.0) in {9007199254740994: 'a'}"));
}

// Confirming probe for the §5.1 verdict: the map-key match is EXACT, not
// the lossy `==`.  Lookup double 2^53+2.0 (exactly representable, value
// 9007199254740994) against a map keyed by the int 2^53 — these differ by
// 2, so an exact match misses.  (A lossy double-widening match would also
// miss here; the discriminating case is DoubleAt2Pow53MissesNeighborIntKey
// above, where `==` says equal but map-`in` says miss.)
TEST(MapKeyNumericCrossType, DoubleExactNeqStoredIntMisses) {
  EXPECT_FALSE(
      OracleBool("dyn(9007199254740994.0) in {9007199254740992: 'a'}"));
}

// ---- Cross-kind duplicate-key map literals: does cel-cpp raise a
//      duplicate-key error or accept the literal?  (The runtime's
//      insert-time dup check must match this.)
//
//      OBSERVED: {1:'a', 1u:'b'} is ACCEPTED (no error) — int 1 and
//      uint 1 are DISTINCT map keys to cel-cpp's literal builder, despite
//      `1 == 1u` being true under the `==` operator.  But {1:'a', 1.0:'b'}
//      IS a duplicate-key error.  So the map-literal dup check treats
//      int-vs-uint as distinct kinds but int-vs-double(integral) as the
//      same key.  (cel-cpp builds map literals keyed by `cel::Value`,
//      whose hash/eq distinguishes int and uint kinds but folds an
//      integral double onto its integer.) ----

testdata::OracleResult OracleEval(absl::string_view source) {
  auto r = testdata::EvalWithCelCpp(source, kP3);
  ABSL_CHECK_OK(r.status()) << source;
  return *std::move(r);
}

TEST(MapKeyNumericCrossType, DupKeyIntAndUintAccepted) {
  // OBSERVED: int 1 and uint 1 are DISTINCT map keys — no duplicate error.
  auto r = OracleEval("{1: 'a', 1u: 'b'}");
  EXPECT_FALSE(r.is_error)
      << "int and uint keys are distinct in a map literal; got error: "
      << r.error_message;
}
TEST(MapKeyNumericCrossType, DupKeyIntAndDoubleIsError) {
  // OBSERVED: int 1 and integral double 1.0 ARE the same key — duplicate
  // error.
  auto r = OracleEval("{1: 'a', 1.0: 'b'}");
  EXPECT_TRUE(r.is_error) << "expected a duplicate-key CEL error; got value";
}

}  // namespace
}  // namespace celwasm
