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
// NOTE.  Our public API now lives in `celwasm::api` (no symbols in
// `namespace cel`), so it no longer collides at link time with the
// `cel::` symbols cel-cpp pulls in through the oracle — the
// our-pipeline-and-oracle differential links cleanly in one binary.
// It stays consolidated here for cohesion, not out of necessity.

#include "compiler_v2/testdata/cel_cpp_oracle.h"

#include <cstdint>
#include <limits>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/conformance/runner.h"
#include "google/protobuf/generated_message_reflection.h"
#include "gtest/gtest.h"

namespace celwasm::api {
namespace {

using ::absl_testing::IsOk;

constexpr absl::string_view kP2 = "cel.expr.conformance.proto2";
constexpr absl::string_view kP3 = "cel.expr.conformance.proto3";

// Force the proto2/proto3 conformance descriptors into the generated
// pool so container-qualified names resolve in OUR pipeline (the oracle
// links its own copy in cel_cpp_oracle.cc).
[[maybe_unused]] const int kDescriptorsLinked = [] {
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
TEST(CelCppOracle, StringConcatAgrees) {
  ExpectAgree("'foo' + 'bar'", kP3);
}
TEST(CelCppOracle, BoolComparisonAgrees) {
  ExpectAgree("2 < 3", kP3);
}
TEST(CelCppOracle, ProtoFieldSelectAgrees) {
  ExpectAgree("TestAllTypes{single_int32: 7}.single_int32", kP3);
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

}  // namespace
}  // namespace celwasm::api
