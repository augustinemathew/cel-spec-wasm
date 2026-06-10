// E2E pins for the conformance rows
// `proto2/extensions_get/package_scoped_repeated_test_all_types` and
// `proto2/extensions_get/message_scoped_repeated_test_all_types`
// (spec/tests/simple/testdata/proto2.textproto): a repeated-message
// extension read (CEL_LIST_HOST over ProtoList) compared for
// equality against a literal-constructed list of messages
// (CEL_LIST_ARENA of CEL_MESSAGE elements).  The cross-origin walk
// in `CelListEqImpl` resolves message elements to their underlying
// protos and compares via the same Any-peel + MessageDifferencer
// core as `cel_host.cel_message_eq`.
//
// Companion negative/boundary coverage (value mismatch, length
// mismatch, both directions) rides along so the e2e surface matches
// the unit matrix in eval/internal/cel_list_eq_impl_test.cc.

#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto2/test_all_types_extensions.pb.h"
#include "compiler/compiler.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

using ::cel::expr::conformance::proto2::TestAllTypes;
using ::celwasm::e2e::CompilePlan;
using ::celwasm::e2e::EvalOk;

// Force generated-pool registration of the proto2 conformance
// descriptors (including the package- and message-scoped extensions)
// so `Reflection::FindKnownExtensionByName` can resolve the backtick
// field names at eval time.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<TestAllTypes>();
      return 0;
    }();

constexpr absl::string_view kProto2TestAllTypes =
    "cel.expr.conformance.proto2.TestAllTypes";

// The conformance rows' literal:
// `[TestAllTypes{single_int64: 1}, TestAllTypes{single_bool: true}]`.
constexpr absl::string_view kLiteralList =
    "[cel.expr.conformance.proto2.TestAllTypes{single_int64: 1}, "
    "cel.expr.conformance.proto2.TestAllTypes{single_bool: true}]";

Compiler CompilerWithMsgVar() {
  Compiler::Builder b;
  b.DeclareVariable("msg", CelType::Message(std::string(kProto2TestAllTypes)));
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  return *std::move(compiler);
}

// `msg` carrying the package-scoped repeated extension with the
// conformance rows' element values.
TestAllTypes MsgWithPackageScopedExtension() {
  TestAllTypes m;
  m.AddExtension(::cel::expr::conformance::proto2::repeated_test_all_types)
      ->set_single_int64(1);
  m.AddExtension(::cel::expr::conformance::proto2::repeated_test_all_types)
      ->set_single_bool(true);
  return m;
}

TestAllTypes MsgWithMessageScopedExtension() {
  using ScopedMessage =
      ::cel::expr::conformance::proto2::Proto2ExtensionScopedMessage;
  TestAllTypes m;
  m.AddExtension(ScopedMessage::message_scoped_repeated_test_all_types)
      ->set_single_int64(1);
  m.AddExtension(ScopedMessage::message_scoped_repeated_test_all_types)
      ->set_single_bool(true);
  return m;
}

bool EvalBool(absl::string_view source, const TestAllTypes& msg) {
  Compiler compiler = CompilerWithMsgVar();
  auto instance = CompilePlan(compiler, source);
  Activation a;
  a.Bind("msg", Value::Message(msg));
  auto result = EvalOk(instance, a).AsBool();
  ABSL_CHECK_OK(result) << source;
  return *result;
}

class Proto2ExtensionListEqE2ETest : public ::testing::Test {};

// — The two conformance rows, verbatim —

TEST_F(Proto2ExtensionListEqE2ETest, PackageScopedRepeatedTestAllTypes) {
  const std::string source = absl::StrCat(
      "msg.`cel.expr.conformance.proto2.repeated_test_all_types` == ",
      kLiteralList);
  EXPECT_TRUE(EvalBool(source, MsgWithPackageScopedExtension()));
}

TEST_F(Proto2ExtensionListEqE2ETest, MessageScopedRepeatedTestAllTypes) {
  const std::string source = absl::StrCat(
      "msg.`cel.expr.conformance.proto2.Proto2ExtensionScopedMessage."
      "message_scoped_repeated_test_all_types` == ",
      kLiteralList);
  EXPECT_TRUE(EvalBool(source, MsgWithMessageScopedExtension()));
}

// — Reverse direction: literal on the left, extension read on the
//   right (arena==host instead of host==arena) —

TEST_F(Proto2ExtensionListEqE2ETest, LiteralOnLeftComparesEqual) {
  const std::string source = absl::StrCat(
      kLiteralList,
      " == msg.`cel.expr.conformance.proto2.repeated_test_all_types`");
  EXPECT_TRUE(EvalBool(source, MsgWithPackageScopedExtension()));
}

// — Negative / boundary companions —

TEST_F(Proto2ExtensionListEqE2ETest, ElementValueMismatchComparesFalse) {
  TestAllTypes m;
  m.AddExtension(::cel::expr::conformance::proto2::repeated_test_all_types)
      ->set_single_int64(1);
  m.AddExtension(::cel::expr::conformance::proto2::repeated_test_all_types)
      ->set_single_int64(99);  // second element differs from the literal
  const std::string source = absl::StrCat(
      "msg.`cel.expr.conformance.proto2.repeated_test_all_types` == ",
      kLiteralList);
  EXPECT_FALSE(EvalBool(source, m));
}

TEST_F(Proto2ExtensionListEqE2ETest, LengthMismatchComparesFalse) {
  TestAllTypes m;
  m.AddExtension(::cel::expr::conformance::proto2::repeated_test_all_types)
      ->set_single_int64(1);  // one element vs the literal's two
  const std::string source = absl::StrCat(
      "msg.`cel.expr.conformance.proto2.repeated_test_all_types` == ",
      kLiteralList);
  EXPECT_FALSE(EvalBool(source, m));
}

TEST_F(Proto2ExtensionListEqE2ETest, UnsetExtensionVsNonEmptyComparesFalse) {
  // Unset repeated extension reads as an empty HostList; compared
  // against the two-element literal it must be unequal, not error.
  const std::string source = absl::StrCat(
      "msg.`cel.expr.conformance.proto2.repeated_test_all_types` == ",
      kLiteralList);
  EXPECT_FALSE(EvalBool(source, TestAllTypes()));
}

}  // namespace
}  // namespace celwasm
