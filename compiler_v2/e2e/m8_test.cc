// M8 e2e test suite — the spec of "done" for wrapper types
// (`google.protobuf.{Bool,Int32,Int64,UInt32,UInt64,Float,Double,
// String,Bytes}Value`).  Mirrors the m7b_test shape: every test
// asserts a capability `m8-wrapper-types.md` says M8 must light up.
// Running this binary today should SKIP every case below (each
// `TEST_F` opens with `GTEST_SKIP() << "M8.<arm> ships here ..."`);
// the skips drop slice-by-slice as M8.B → M8.C → M8.A → M8.D close
// per the as-shipped sequencing in `m8-wrapper-types.md` §5.
//
// Wrapper-types are the M7 follow-on slice: M7 admits the explicit
// recursive `kStructExpr` form (`Foo{w: Int32Value{value: 5}}`);
// M8 adds (A) write-side auto-wrap from scalar (`Foo{w: 5}` +
// `Activation::Bind("w", Value::Int(5))`), (B) read-side auto-peel
// + Any-chain (`TestAllTypes{}.single_int32_wrapper == null`,
// `Any{IntValue{value:1}} == 1`), and (C) kStructExpr tail-unwrap
// so a bare wrapper literal surfaces as the inner scalar
// (`Int32Value{value: 5} == 5`).
//
// Fixtures grouped by capability (one section per arm + cross-
// cutting matrices):
//
//   - WrapperLiteralUnwrapE2ETest      M8.C — kStructExpr tail-
//                                             unwrap; bare wrapper
//                                             literal peels to inner
//                                             scalar at eq dispatch.
//   - WrapperFieldReadE2ETest          M8.B — read-side auto-peel
//                                             on `single_X_wrapper`
//                                             across proto2 + proto3.
//   - WrapperAnyChainE2ETest           M8.B — Any-of-wrapper chains
//                                             through `UnpackAnyToValue`
//                                             to the inner scalar.
//   - WrapperConstructionE2ETest       M8.A — write-side auto-wrap
//                                             when scalar value is
//                                             assigned to a wrapper-
//                                             typed field literal.
//   - WrapperActivationBindE2ETest     M8.A — activation auto-wrap
//                                             when bound `Value` is
//                                             a scalar against a
//                                             wrapper-declared var.
//   - WrapperRoundTripE2ETest          M8.A+B+C — the 16 dynamic.textproto
//                                             round-trip rows: construct
//                                             with scalar, read back
//                                             scalar.
//   - WrapperRejectE2ETest             §6.3 — checker + runtime
//                                             rejection matrix.
//
// Conformance unlock estimate per arm is logged on each test section;
// aggregate target is +151 PASS per `m8-wrapper-types.md` §1 (A=89,
// B=24 standalone, C=38).

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/any.pb.h"
#include "google/protobuf/message.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"

namespace celwasm::api {
namespace {

using ::absl_testing::IsOk;

// Force generated-pool registration of descriptors referenced by the
// field-read / construction / Any-chain tests below.  Runs once at
// static init.  Mirrors the m7b shape; we link the proto2 +
// proto3 conformance `TestAllTypes` plus the 9 wrapper protos and
// `google.protobuf.Any`.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<
          ::cel::expr::conformance::proto2::TestAllTypes>();
      google::protobuf::LinkMessageReflection<
          ::cel::expr::conformance::proto3::TestAllTypes>();
      google::protobuf::LinkMessageReflection<::google::protobuf::BoolValue>();
      google::protobuf::LinkMessageReflection<::google::protobuf::Int32Value>();
      google::protobuf::LinkMessageReflection<::google::protobuf::Int64Value>();
      google::protobuf::LinkMessageReflection<
          ::google::protobuf::UInt32Value>();
      google::protobuf::LinkMessageReflection<
          ::google::protobuf::UInt64Value>();
      google::protobuf::LinkMessageReflection<::google::protobuf::FloatValue>();
      google::protobuf::LinkMessageReflection<
          ::google::protobuf::DoubleValue>();
      google::protobuf::LinkMessageReflection<
          ::google::protobuf::StringValue>();
      google::protobuf::LinkMessageReflection<::google::protobuf::BytesValue>();
      google::protobuf::LinkMessageReflection<::google::protobuf::Any>();
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

using ConfigureFn = std::function<void(Compiler::Builder&)>;
absl::StatusOr<Compiler> BuildCompiler(const ConfigureFn& configure) {
  Compiler::Builder b;
  configure(b);
  return std::move(b).Build();
}

[[maybe_unused]] absl::StatusOr<Compiler> CompilerEmpty() {
  return BuildCompiler([](Compiler::Builder& /*b*/) {});
}

// All e2e helpers below are unused while every test SKIPs.  Once the
// first arm ships and a test body uses them, the `[[maybe_unused]]`
// is dropped.  This file is the spec-of-done; the helpers stand
// ready for slice-by-slice migration (mirrors m7b_test.cc).

[[maybe_unused]] Instance CompilePlan(const Compiler& compiler,
                                      absl::string_view source) {
  auto program = compiler.Compile(source);
  ABSL_CHECK_OK(program) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance) << source;
  return *std::move(instance);
}

[[maybe_unused]] Value EvalOk(Instance& instance,
                              const Activation& activation) {
  auto v = instance.Eval(activation);
  ABSL_CHECK_OK(v);
  return *std::move(v);
}

[[maybe_unused]] void ExpectCompileFails(const Compiler& compiler,
                                         absl::string_view source,
                                         absl::string_view why) {
  auto program_or = compiler.Compile(source);
  EXPECT_FALSE(program_or.ok())
      << "expected `" << source << "` to fail at compile (" << why << ")";
}

// Build a Compiler that declares a single variable of the given type.
// Mirrors `CompilerWithVar` in m7b_test.cc.
[[maybe_unused]] Compiler CompilerWithVar(absl::string_view name,
                                          const CelType& type) {
  auto compiler_or = BuildCompiler([&](Compiler::Builder& b) {
    b.DeclareVariable(std::string(name), type);
  });
  ABSL_CHECK_OK(compiler_or);
  return *std::move(compiler_or);
}

// FQN string shorthand for the 9 wrapper types.  Used as type-id
// for `CelType::Message(...)` declarations and as the leading
// segment of kStructExpr source strings.
constexpr absl::string_view kFqnBoolValue = "google.protobuf.BoolValue";
constexpr absl::string_view kFqnInt32Value = "google.protobuf.Int32Value";
constexpr absl::string_view kFqnInt64Value = "google.protobuf.Int64Value";
constexpr absl::string_view kFqnUInt32Value = "google.protobuf.UInt32Value";
constexpr absl::string_view kFqnUInt64Value = "google.protobuf.UInt64Value";
constexpr absl::string_view kFqnFloatValue = "google.protobuf.FloatValue";
constexpr absl::string_view kFqnDoubleValue = "google.protobuf.DoubleValue";
constexpr absl::string_view kFqnStringValue = "google.protobuf.StringValue";
constexpr absl::string_view kFqnBytesValue = "google.protobuf.BytesValue";

[[maybe_unused]] constexpr absl::string_view kProto2TestAllTypes =
    "cel.expr.conformance.proto2.TestAllTypes";
[[maybe_unused]] constexpr absl::string_view kProto3TestAllTypes =
    "cel.expr.conformance.proto3.TestAllTypes";

// ──────────────────────────────────────────────────────────────
// 1. WrapperLiteralUnwrapE2ETest  (M8.C — kStructExpr tail-unwrap)
//
//    A bare wrapper literal at the tail of an expression must peel
//    to the inner scalar before equality dispatch.  Today every row
//    fails at compare-time with `want-kind=<scalar> got-kind=message`
//    because kStructExpr emits CEL_MESSAGE into a scalar slot and
//    `cel_equals_at_vv` rejects the kind mismatch.  After M8.C the
//    new `cel_host.cel_wkt_unwrap_wrapper` tail-call surfaces the
//    inner CEL_INT / CEL_BOOL / CEL_STRING / CEL_BYTES / CEL_DOUBLE /
//    CEL_UINT and the comparison dispatches scalar-vs-scalar.
//
//    Conformance unlock: +38 rows (comparisons.eq_X × 9 +
//    eq_X_empty × 9 + dynamic literal* × 20).
// ──────────────────────────────────────────────────────────────

class WrapperLiteralUnwrapE2ETest : public ::testing::Test {};

// — Per-kind set-to-value peel: `XValue{value: v} == v` → true —

TEST_F(WrapperLiteralUnwrapE2ETest, BoolValueSetToTruePeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.BoolValue{value: true} == true");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, Int32ValueSetToFivePeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.Int32Value{value: 5} == 5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, Int64ValueSetToBoundaryMaxPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "google.protobuf.Int64Value{value: 9223372036854775807} == "
                  "9223372036854775807");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, UInt32ValueSetToBoundaryMaxPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "google.protobuf.UInt32Value{value: 4294967295u} == 4294967295u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, UInt64ValueSetToBoundaryMaxPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "google.protobuf.UInt64Value{value: 18446744073709551615u} == "
      "18446744073709551615u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, FloatValueSetToValuePeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // Floats peel to double in CEL (no float kind); cel-cpp widens.
  auto instance =
      CompilePlan(*compiler, "google.protobuf.FloatValue{value: 1.5} == 1.5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, DoubleValueSetToValuePeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "google.protobuf.DoubleValue{value: 3.14159} == 3.14159");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, StringValueSetToUnicodePeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // dynamic.textproto literal_unicode row: multi-byte UTF-8.
  auto instance = CompilePlan(
      *compiler, R"(google.protobuf.StringValue{value: "flambé"} == "flambé")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, BytesValueSetToValuePeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(google.protobuf.BytesValue{value: b"abc"} == b"abc")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Per-kind set-to-zero peel: `XValue{value: 0} == 0` → true.
//   Distinct from `XValue{}` (empty-construct) below — the wrapper is
//   present and value-set; the inner field happens to be the
//   protobuf scalar zero default. —

TEST_F(WrapperLiteralUnwrapE2ETest, BoolValueSetToFalsePeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "google.protobuf.BoolValue{value: false} == false");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, Int32ValueSetToZeroPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.Int32Value{value: 0} == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Empty-construct peels to default scalar: `XValue{} == <default>` → true —

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyBoolValuePeelsToFalse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.BoolValue{} == false");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyInt32ValuePeelsToZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "google.protobuf.Int32Value{} == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyInt64ValuePeelsToZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "google.protobuf.Int64Value{} == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyUInt32ValuePeelsToZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "google.protobuf.UInt32Value{} == 0u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyUInt64ValuePeelsToZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "google.protobuf.UInt64Value{} == 0u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyFloatValuePeelsToZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "google.protobuf.FloatValue{} == 0.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyDoubleValuePeelsToZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.DoubleValue{} == 0.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyStringValuePeelsToEmptyString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"(google.protobuf.StringValue{} == "")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyBytesValuePeelsToEmptyBytes) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"(google.protobuf.BytesValue{} == b"")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Cross-form: scalar on either side peels equivalently —

TEST_F(WrapperLiteralUnwrapE2ETest, Int32ValueOnRhsPeelsEqually) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "5 == google.protobuf.Int32Value{value: 5}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, StringValueOnRhsPeelsEqually) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"("hello" == google.protobuf.StringValue{value: "hello"})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Empty-wrapper-literal is NOT null (langdef §3.1 §"Wrapper Types":
//   present wrapper message peels to inner default scalar, not null) —

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyInt32ValueIsNotNull) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.Int32Value{} == null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyInt32ValueNotEqualNull) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.Int32Value{} != null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyBoolValueIsNotNull) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "google.protobuf.BoolValue{} == null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyStringValueIsNotNull) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.StringValue{} == null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(WrapperLiteralUnwrapE2ETest, EmptyBytesValueIsNotNull) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.BytesValue{} == null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// — Wrapper-vs-wrapper after peel: both operands tail-unwrap, the
//   equality kernel sees scalar-vs-scalar (M5.B's cel_message_eq is
//   NOT reached because both LHS and RHS are peeled). —

TEST_F(WrapperLiteralUnwrapE2ETest, Int32ValueEqualsItselfAfterPeel) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "google.protobuf.Int32Value{value: 1} == "
                              "google.protobuf.Int32Value{value: 1}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperLiteralUnwrapE2ETest, Int32ValueDiffersFromOtherValue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "google.protobuf.Int32Value{value: 1} != "
                              "google.protobuf.Int32Value{value: 2}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 2. WrapperFieldReadE2ETest  (M8.B — read-side auto-peel)
//
//    Reading `single_X_wrapper` on a proto2 / proto3 `TestAllTypes`:
//
//      - unset → CEL_NULL (langdef line 484-486; cel-cpp option
//        `enable_empty_wrapper_null_unboxing=true` mandates this
//        for both syntaxes).
//      - set (even to scalar zero) → inner scalar (auto-peel).
//      - has() reflects presence, NOT value-vs-default.
//
//    Today proto3 unset wrappers incidentally pass (the field's
//    default message has no value), but proto2 unset wrappers fail
//    — Arm B unifies treatment.
//
//    Conformance unlock: +24 standalone (9 comparisons.eq_X_proto2_null
//    + 9 wrappers/to_any + 5 dynamic field_read_proto2_unset + 1
//    proto2 empty_field/wkt); also gates the read-half of the 16
//    round-trip rows attributed to M8.A.
// ──────────────────────────────────────────────────────────────

class WrapperFieldReadE2ETest : public ::testing::Test {};

// Build a Compiler that declares `m` as proto3 TestAllTypes; the
// expression body produces the message literal so no Activation
// binding is required.  Used by both `set` and `unset` rows below
// because expressions like `M{}.single_int32_wrapper` exercise the
// wrapper read path without dragging in M8.A's auto-wrap.  However
// `set` rows DO drag in M8.A — they're marked under "round-trip"
// below (M8.A + M8.B both required to execute).  These tests use
// proto2 / proto3 TestAllTypes by FQN exclusively.

// — Unset field reads as null (the load-bearing M8.B row) —

TEST_F(WrapperFieldReadE2ETest, UnsetInt32WrapperReadsAsNullProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{}.single_int32_wrapper == "
      "null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperFieldReadE2ETest, UnsetInt32WrapperReadsAsNullProto2) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto2.TestAllTypes{}.single_int32_wrapper == "
      "null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperFieldReadE2ETest, UnsetBoolWrapperReadsAsNullProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{}.single_bool_wrapper == "
      "null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperFieldReadE2ETest, UnsetStringWrapperReadsAsNullProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{}.single_string_wrapper == "
      "null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperFieldReadE2ETest, UnsetBytesWrapperReadsAsNullProto2) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto2.TestAllTypes{}.single_bytes_wrapper == "
      "null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — `has()` on unset wrapper field is false — independent of the
//   value-vs-null peel; presence is descriptor-level. —

TEST_F(WrapperFieldReadE2ETest, HasUnsetWrapperIsFalseProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "has(cel.expr.conformance.proto3.TestAllTypes{}.single_int32_wrapper)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(WrapperFieldReadE2ETest, HasUnsetWrapperIsFalseProto2) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "has(cel.expr.conformance.proto2.TestAllTypes{}.single_int32_wrapper)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// — `has()` on set wrapper field is true even when set-to-default
//   (presence reflects the descriptor-level "field set", not the
//   inner value).  These rows depend on M8.A landing first to make
//   construction succeed; the assertion is M8.B's. —

TEST_F(WrapperFieldReadE2ETest, HasSetToZeroWrapperIsTrueProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "has(cel.expr.conformance.proto3.TestAllTypes{"
                              "single_int32_wrapper: 0}.single_int32_wrapper)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperFieldReadE2ETest, HasSetWrapperIsTrueProto2) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "has(cel.expr.conformance.proto2.TestAllTypes{"
                              "single_int32_wrapper: 5}.single_int32_wrapper)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Set-to-default still reads as scalar, NOT null.  The "absent
//   reads as null" rule applies only to *unset* fields. —

TEST_F(WrapperFieldReadE2ETest, SetToZeroReadsAsScalarNotNullProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_int32_wrapper: 0}.single_int32_wrapper == null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(WrapperFieldReadE2ETest, SetToEmptyStringReadsAsScalarNotNullProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(cel.expr.conformance.proto3.TestAllTypes{)"
                 R"(single_string_wrapper: ""}.single_string_wrapper == null)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// ──────────────────────────────────────────────────────────────
// 3. WrapperAnyChainE2ETest  (M8.B — Any-of-wrapper peel chain)
//
//    Bind an `Any` field containing a wrapper proto; read the field;
//    expect the inner scalar.  `UnpackAnyToValue` (M7-A.B) parses
//    the Any envelope into a temp message; M8.B chains a wrapper-
//    peel after that so the Any-of-wrapper surfaces as scalar.
//
//    Conformance unlock: +9 rows (wrappers/<w>/to_any × 9).
//
//    The construction side of these tests requires the Any pack arm
//    (M7-A.A, shipped) and the wrapper write-side (M8.A) — the test
//    fixtures bind a pre-populated TestAllTypes with the Any field
//    already set, so M8.A is not on the critical path for the *read*
//    half.
// ──────────────────────────────────────────────────────────────

class WrapperAnyChainE2ETest : public ::testing::Test {};

TEST_F(WrapperAnyChainE2ETest, AnyOfInt32ValuePeelsToInt) {
  // Construct TestAllTypes{single_any: Int32Value{value: 7}} on the
  // host side so the read-side Any-of-wrapper chain is exercised in
  // isolation from M8.A's construction path.
  ::cel::expr::conformance::proto3::TestAllTypes m;
  ::google::protobuf::Int32Value inner;
  inner.set_value(7);
  ABSL_CHECK(m.mutable_single_any()->PackFrom(inner));
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "m", CelType::Message("cel.expr.conformance.proto3.TestAllTypes"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "m.single_any == 7");
  Activation a;
  a.Bind("m", Value::Message(m));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperAnyChainE2ETest, AnyOfBoolValuePeelsToBool) {
  ::cel::expr::conformance::proto3::TestAllTypes m;
  ::google::protobuf::BoolValue inner;
  inner.set_value(true);
  ABSL_CHECK(m.mutable_single_any()->PackFrom(inner));
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "m", CelType::Message("cel.expr.conformance.proto3.TestAllTypes"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "m.single_any == true");
  Activation a;
  a.Bind("m", Value::Message(m));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperAnyChainE2ETest, AnyOfStringValuePeelsToString) {
  ::cel::expr::conformance::proto3::TestAllTypes m;
  ::google::protobuf::StringValue inner;
  inner.set_value("hello");
  ABSL_CHECK(m.mutable_single_any()->PackFrom(inner));
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "m", CelType::Message("cel.expr.conformance.proto3.TestAllTypes"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(m.single_any == "hello")");
  Activation a;
  a.Bind("m", Value::Message(m));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperAnyChainE2ETest, AnyOfBytesValuePeelsToBytes) {
  ::cel::expr::conformance::proto3::TestAllTypes m;
  ::google::protobuf::BytesValue inner;
  inner.set_value("abc");
  ABSL_CHECK(m.mutable_single_any()->PackFrom(inner));
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "m", CelType::Message("cel.expr.conformance.proto3.TestAllTypes"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(m.single_any == b"abc")");
  Activation a;
  a.Bind("m", Value::Message(m));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperAnyChainE2ETest, AnyOfDoubleValuePeelsToDouble) {
  ::cel::expr::conformance::proto3::TestAllTypes m;
  ::google::protobuf::DoubleValue inner;
  inner.set_value(2.5);
  ABSL_CHECK(m.mutable_single_any()->PackFrom(inner));
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "m", CelType::Message("cel.expr.conformance.proto3.TestAllTypes"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "m.single_any == 2.5");
  Activation a;
  a.Bind("m", Value::Message(m));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperAnyChainE2ETest, AnyOfUInt64ValuePeelsToUint) {
  ::cel::expr::conformance::proto3::TestAllTypes m;
  ::google::protobuf::UInt64Value inner;
  inner.set_value(42);
  ABSL_CHECK(m.mutable_single_any()->PackFrom(inner));
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "m", CelType::Message("cel.expr.conformance.proto3.TestAllTypes"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "m.single_any == 42u");
  Activation a;
  a.Bind("m", Value::Message(m));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// Negative regression: an Any of a NON-wrapper proto must NOT be
// erroneously peeled.  M8.B's chain only fires when the inner
// descriptor is in the wrapper-FQN set.
TEST_F(WrapperAnyChainE2ETest, AnyOfNonWrapperMessageStaysMessage) {
  // Inner is a TestAllTypes (NOT a wrapper); the outer field's read
  // should return a non-wrapper message that compares equal to the
  // inner via cel_message_eq (M5.B).  Tests that B's wrapper-peel
  // chain is descriptor-gated.
  ::cel::expr::conformance::proto3::TestAllTypes outer;
  ::cel::expr::conformance::proto3::TestAllTypes inner;
  inner.set_single_int32(99);
  ABSL_CHECK(outer.mutable_single_any()->PackFrom(inner));
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "m", CelType::Message("cel.expr.conformance.proto3.TestAllTypes"));
  });
  ASSERT_THAT(compiler, IsOk());
  // Read m.single_any (the non-wrapper inner) and compare to a
  // freshly-constructed equivalent.  No peel should fire.
  auto instance =
      CompilePlan(*compiler,
                  "m.single_any == cel.expr.conformance.proto3.TestAllTypes{"
                  "single_int32: 99}");
  Activation a;
  a.Bind("m", Value::Message(outer));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 4. WrapperConstructionE2ETest  (M8.A — write-side auto-wrap)
//
//    `Foo{single_X_wrapper: scalar}` synthesises an `XValue{value:
//    scalar}` and assigns to the field.  The byte-image must be
//    identical to constructing the same shape via explicit wrapper
//    literal (`Foo{single_X_wrapper: XValue{value: scalar}}`,
//    already shipped at M7.E).
//
//    Conformance unlock: +89 rows (54 dynamic field_assign + 16
//    dynamic round-trip + 9 proto2 literal_wellknown + 9 proto3
//    literal_wellknown + 1 parse repeat/message_literal).
// ──────────────────────────────────────────────────────────────

class WrapperConstructionE2ETest : public ::testing::Test {};

// — Auto-wrap-equals-explicit-wrap (per kind, the canonical row) —

TEST_F(WrapperConstructionE2ETest, AutoWrapInt32EqualsExplicitWrap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_int32_wrapper: 5} == "
      "cel.expr.conformance.proto3.TestAllTypes{"
      "single_int32_wrapper: google.protobuf.Int32Value{value: 5}}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapBoolEqualsExplicitWrap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_bool_wrapper: true} == "
      "cel.expr.conformance.proto3.TestAllTypes{"
      "single_bool_wrapper: google.protobuf.BoolValue{value: true}}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapStringEqualsExplicitWrap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(cel.expr.conformance.proto3.TestAllTypes{single_string_wrapper: "hi"} == )"
      R"(cel.expr.conformance.proto3.TestAllTypes{)"
      R"(single_string_wrapper: google.protobuf.StringValue{value: "hi"}})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapBytesEqualsExplicitWrap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(cel.expr.conformance.proto3.TestAllTypes{single_bytes_wrapper: b"x"} == )"
      R"(cel.expr.conformance.proto3.TestAllTypes{)"
      R"(single_bytes_wrapper: google.protobuf.BytesValue{value: b"x"}})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapInt64BoundaryMaxEqualsExplicit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_int64_wrapper: 9223372036854775807} == "
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_int64_wrapper: google.protobuf.Int64Value{"
                  "value: 9223372036854775807}}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapUInt64BoundaryMaxEqualsExplicit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_uint64_wrapper: 18446744073709551615u} == "
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_uint64_wrapper: google.protobuf.UInt64Value{"
                  "value: 18446744073709551615u}}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapDoubleEqualsExplicit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_double_wrapper: 3.14} "
      "== "
      "cel.expr.conformance.proto3.TestAllTypes{"
      "single_double_wrapper: google.protobuf.DoubleValue{value: 3.14}}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapFloatEqualsExplicit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_float_wrapper: 2.5} == "
      "cel.expr.conformance.proto3.TestAllTypes{"
      "single_float_wrapper: google.protobuf.FloatValue{value: 2.5}}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapUInt32EqualsExplicit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_uint32_wrapper: 7u} == "
      "cel.expr.conformance.proto3.TestAllTypes{"
      "single_uint32_wrapper: google.protobuf.UInt32Value{value: 7u}}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Boundary values per kind (§6.2): empty string/bytes,
//   embedded NUL, multi-byte UTF-8 —

TEST_F(WrapperConstructionE2ETest, AutoWrapEmptyStringEqualsExplicit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(cel.expr.conformance.proto3.TestAllTypes{single_string_wrapper: ""} == )"
      R"(cel.expr.conformance.proto3.TestAllTypes{)"
      R"(single_string_wrapper: google.protobuf.StringValue{value: ""}})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, AutoWrapMultiByteUTF8) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(cel.expr.conformance.proto3.TestAllTypes{single_string_wrapper: "flambé"} == )"
      R"(cel.expr.conformance.proto3.TestAllTypes{)"
      R"(single_string_wrapper: google.protobuf.StringValue{value: "flambé"}})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Null-into-wrapper-field clears the field (langdef §"Wrapper Types":
//   wrapper(null) ≡ field-unset).  Equivalent to constructing the
//   empty message. —

TEST_F(WrapperConstructionE2ETest, NullIntoInt32WrapperClearsField) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_int32_wrapper: null} == "
      "cel.expr.conformance.proto3.TestAllTypes{}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, NullIntoBoolWrapperClearsField) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_bool_wrapper: null} == "
      "cel.expr.conformance.proto3.TestAllTypes{}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperConstructionE2ETest, NullIntoStringWrapperClearsField) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "cel.expr.conformance.proto3.TestAllTypes{single_"
                              "string_wrapper: null} == "
                              "cel.expr.conformance.proto3.TestAllTypes{}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 5. WrapperActivationBindE2ETest  (M8.A — activation auto-wrap)
//
//    `Activation::Bind("w", Value::<Scalar>(...))` against a wrapper-
//    typed declared variable.  `Instance::EncodeMessage` synthesises
//    the wrapper proto on the way in; the expression then reads it
//    back as a scalar via Arm B / Arm C.  Tests the cross-arm round-
//    trip on the bind path.
//
//    Conformance unlock: counted under M8.A's +89 (entry-point
//    expansion of the construction matrix).
// ──────────────────────────────────────────────────────────────

class WrapperActivationBindE2ETest : public ::testing::Test {};

TEST_F(WrapperActivationBindE2ETest, BindIntAgainstInt32WrapperVar) {
  // Declare `w` as Int32Value (wrapper-typed); bind a CEL_INT;
  // expect the expression `w == 5` to read the bound int through
  // the synthesised wrapper.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnInt32Value)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == 5");
  Activation a;
  a.Bind("w", Value::Int(5));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperActivationBindE2ETest, BindBoolAgainstBoolWrapperVar) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnBoolValue)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == true");
  Activation a;
  a.Bind("w", Value::Bool(true));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperActivationBindE2ETest, BindStringAgainstStringWrapperVar) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnStringValue)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(w == "hi")");
  Activation a;
  a.Bind("w", Value::String("hi"));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperActivationBindE2ETest, BindBytesAgainstBytesWrapperVar) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnBytesValue)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(w == b"x")");
  Activation a;
  a.Bind("w", Value::Bytes("x"));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperActivationBindE2ETest, BindDoubleAgainstDoubleWrapperVar) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnDoubleValue)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == 2.5");
  Activation a;
  a.Bind("w", Value::Double(2.5));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperActivationBindE2ETest, BindDoubleAgainstFloatWrapperVar) {
  // Floats narrow on synthesis; no CEL Value::Float kind.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnFloatValue)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == 2.5");
  Activation a;
  a.Bind("w", Value::Double(2.5));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperActivationBindE2ETest, BindUIntAgainstUInt32WrapperVar) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnUInt32Value)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == 7u");
  Activation a;
  a.Bind("w", Value::Uint(7));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperActivationBindE2ETest, BindUIntAgainstUInt64WrapperVar) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnUInt64Value)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == 7u");
  Activation a;
  a.Bind("w", Value::Uint(7));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperActivationBindE2ETest, BindIntAgainstInt64WrapperVar) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnInt64Value)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == 5");
  Activation a;
  a.Bind("w", Value::Int(5));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Null-bind against a wrapper-typed var: encodes as unset wrapper
//   field; read path (Arm B) surfaces it as null. —

TEST_F(WrapperActivationBindE2ETest, NullBindAgainstInt32WrapperVarReadsNull) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnInt32Value)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == null");
  Activation a;
  a.Bind("w", Value::Null());
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Negative: wrong-kind bind (Value::String against an Int32Value-
//   typed binding) → encoder rejects at Eval time. —

TEST_F(WrapperActivationBindE2ETest, WrongKindBindFailsAtEval) {
  GTEST_SKIP() << "M8.A ships the kind-mismatch reject at the encoder";
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnInt32Value)));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "w == 5");
  Activation a;
  a.Bind("w", Value::String("not-an-int"));
  auto val_or = instance.Eval(a);
  EXPECT_FALSE(val_or.ok())
      << "expected string/int32-wrapper bind-mismatch to fail at Eval";
}

// ──────────────────────────────────────────────────────────────
// 6. WrapperRoundTripE2ETest  (M8.A + M8.B + M8.C — end-to-end)
//
//    The 16 dynamic.textproto round-trip rows: construct with scalar,
//    read back through `.single_X_wrapper`.  Requires A (construction
//    auto-wrap) AND B (read auto-peel) to both ship — A alone makes
//    the construction succeed but the read still returns CEL_MESSAGE.
//
//    Conformance unlock: 16 dynamic.textproto field_read* rows
//    (covered by the A+B cumulative total).
// ──────────────────────────────────────────────────────────────

class WrapperRoundTripE2ETest : public ::testing::Test {};

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackInt32Proto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_int32_wrapper: 5}.single_int32_wrapper == 5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackInt32Proto2) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto2.TestAllTypes{"
                  "single_int32_wrapper: 5}.single_int32_wrapper == 5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackBoolProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_bool_wrapper: true}.single_bool_wrapper == true");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackStringProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(cel.expr.conformance.proto3.TestAllTypes{)"
      R"(single_string_wrapper: "hello"}.single_string_wrapper == "hello")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackBytesProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(cel.expr.conformance.proto3.TestAllTypes{)"
      R"(single_bytes_wrapper: b"abc"}.single_bytes_wrapper == b"abc")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackDoubleProto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_double_wrapper: 3.14}.single_double_wrapper == 3.14");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackUInt64Proto3) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_uint64_wrapper: 42u}.single_uint64_wrapper == 42u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackInt64BoundaryProto2) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto2.TestAllTypes{"
      "single_int64_wrapper: 9223372036854775807}.single_int64_wrapper == "
      "9223372036854775807");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperRoundTripE2ETest, ConstructAndReadBackSetToZeroIsScalar) {
  // Set-to-zero is "present" and reads as scalar 0, NOT null
  // (langdef §"Wrapper Types"; cel-cpp option
  // `enable_empty_wrapper_null_unboxing=true` mandates this).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_int32_wrapper: 0}.single_int32_wrapper == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Cross-form: explicit-wrapper construction in the field-set still
//   round-trips identically (the read-side peel doesn't care which
//   syntactic form authored the wrapper). —

TEST_F(WrapperRoundTripE2ETest, ExplicitWrapperConstructionReadsScalar) {
  GTEST_SKIP() << "M8.B ships the read-half; explicit construction is M7.E";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "cel.expr.conformance.proto3.TestAllTypes{"
                  "single_int32_wrapper: google.protobuf.Int32Value{value: 5}}"
                  ".single_int32_wrapper == 5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 7. WrapperRejectE2ETest  (§6.3 — negative matrix)
//
//    Compile-time checker rejection for mixed-type / wrong-kind
//    wrapper interactions; the static-subset guard on `dyn(...)`-
//    erased wrappers (currently rejected at the frontend gate).
// ──────────────────────────────────────────────────────────────

class WrapperRejectE2ETest : public ::testing::Test {};

TEST_F(WrapperRejectE2ETest, CrossKindEqRejectedByChecker) {
  GTEST_SKIP() << "M8 leaves the cross-kind == checker-reject untouched";
  // `Int32Value{value:1} == "1"` is a type error in the static
  // subset: mixed-kind equality is rejected at compile.  This is
  // unrelated to M8 specifically — checker behaviour is independent
  // of the wrapper peel — but the row pins the guarantee.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler,
                     R"(google.protobuf.Int32Value{value: 1} == "1")",
                     "cross-kind == (int vs string after wrapper peel)");
}

TEST_F(WrapperRejectE2ETest, WrongScalarKindIntoStringWrapperRejected) {
  GTEST_SKIP() << "M8.A ships the wrong-kind reject at the checker";
  // `TestAllTypes{single_string_wrapper: 5}` must reject at compile:
  // wrong scalar kind for a string-wrapper field.  Checker should
  // catch this before codegen.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_string_wrapper: 5}",
      "wrong scalar kind for StringValue field");
}

TEST_F(WrapperRejectE2ETest, WrongScalarKindIntoInt32WrapperRejected) {
  GTEST_SKIP() << "M8.A ships the wrong-kind reject at the checker";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(
      *compiler,
      R"(cel.expr.conformance.proto3.TestAllTypes{single_int32_wrapper: "x"})",
      "wrong scalar kind for Int32Value field");
}

TEST_F(WrapperRejectE2ETest, DynOfWrapperVarRejectedByStaticSubset) {
  GTEST_SKIP()
      << "static-subset rejects dyn(<wrapper-typed>); M8 keeps this guard";
  // `dyn(w)` against a wrapper-typed var should reject at frontend
  // per the M1.5 static-subset gate.  Confirms M8 doesn't broaden
  // admissibility of dyn-erased wrappers (§4.R4).
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("w", CelType::Message(std::string(kFqnInt32Value)));
  });
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "dyn(w) == 1",
                     "static-subset rejects dyn-of-wrapper");
}

// ──────────────────────────────────────────────────────────────
// 8. WrapperArithmeticE2ETest  (incidental coverage — no new code)
//
//    cel-cpp peels wrappers at the call-dispatch boundary for
//    arithmetic / ordering / size / `in` / scalar-constructor /
//    string-concat (probe `throwaway/m8-wrapper-probe` PR #4
//    matrix-confirmed against the vendored cel-cpp).  Our M8
//    architecture (typed_ast.cc:56 wrapper→scalar Repr + M8.B
//    read-side peel + M8.C kStructExpr tail-unwrap) means every
//    such expression flows through the existing scalar-arithmetic
//    codegen by the time codegen sees the operand — no
//    wrapper-arithmetic-specific machinery needed.  These tests
//    pin that property so a future refactor that tries to
//    "simplify" the typed_ast wrapper-Repr mapping doesn't
//    silently regress wrapper arithmetic.
//
//    Originally listed as M8 §9 "Future work — Wrapper coercion
//    in arithmetic" before the probe empirically resolved it as
//    already-working.
// ──────────────────────────────────────────────────────────────

class WrapperArithmeticE2ETest : public ::testing::Test {};

// — Wrapper-vs-scalar arithmetic across the four numeric kinds —

TEST_F(WrapperArithmeticE2ETest, Int32WrapperPlusScalarPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.Int32Value{value: 1} + 2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

TEST_F(WrapperArithmeticE2ETest, Int64WrapperMinusScalarPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.Int64Value{value: 10} - 3");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 7);
}

TEST_F(WrapperArithmeticE2ETest, UInt32WrapperTimesScalarPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.UInt32Value{value: 7u} * 3u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 21u);
}

TEST_F(WrapperArithmeticE2ETest, DoubleWrapperPlusScalarPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.DoubleValue{value: 1.5} + 2.5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsDouble(), 4.0);
}

// — Wrapper-vs-wrapper arithmetic (both operands peel) —

TEST_F(WrapperArithmeticE2ETest, Int32WrapperMinusInt32WrapperPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "google.protobuf.Int32Value{value: 5} - "
                              "google.protobuf.Int32Value{value: 2}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

// — String concat: wrapper-vs-scalar peels to string-string concat —

TEST_F(WrapperArithmeticE2ETest, StringWrapperConcatScalarPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(google.protobuf.StringValue{value: "ab"} + "c")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "abc");
}

// — Ordering: wrapper-vs-scalar comparison peels —

TEST_F(WrapperArithmeticE2ETest, Int32WrapperLessThanScalarPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "google.protobuf.Int32Value{value: 1} < 2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(WrapperArithmeticE2ETest, Int32WrapperGreaterEqualsWrapperPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "google.protobuf.Int32Value{value: 5} >= "
                              "google.protobuf.Int32Value{value: 5}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — `size()` on string/bytes wrappers peels through —

TEST_F(WrapperArithmeticE2ETest, SizeStringWrapperPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(size(google.protobuf.StringValue{value: "hello"}))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 5);
}

TEST_F(WrapperArithmeticE2ETest, SizeBytesWrapperPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(size(google.protobuf.BytesValue{value: b"foo"}))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

// — `in` operator: wrapper element vs scalar list peels —

TEST_F(WrapperArithmeticE2ETest, WrapperInScalarListPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "google.protobuf.Int32Value{value: 2} in [1, 2, 3]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — Scalar constructor (string()) on a wrapper peels through —

TEST_F(WrapperArithmeticE2ETest, StringConstructorOnIntWrapperPeels) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "string(google.protobuf.Int32Value{value: 42})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "42");
}

// — Field-read wrapper in arithmetic (M8.B-peel then scalar add) —

TEST_F(WrapperArithmeticE2ETest, FieldReadWrapperInArithmetic) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "cel.expr.conformance.proto3.TestAllTypes{single_int32_wrapper: 5}."
      "single_int32_wrapper + 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 12);
}

// — Empty-wrapper arithmetic: empty literal peels to default scalar —

TEST_F(WrapperArithmeticE2ETest, EmptyInt32WrapperPlusScalarUsesDefault) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // Int32Value{} peels to scalar default 0; 0 + 2 = 2.  Confirms
  // the M8.C tail-unwrap path treats an unset value field as the
  // proto default (langdef §"Wrapper Types" — wrapper literals
  // are present messages whose field defaults to zero).
  auto instance = CompilePlan(*compiler, "google.protobuf.Int32Value{} + 2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

}  // namespace
}  // namespace celwasm::api
