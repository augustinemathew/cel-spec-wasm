// E2E pins for two kernel-correctness fixes in `cel_runtime.c`:
//
// 1. Arena+arena aggregate equality with non-scalar elements
//    (cleanup-backlog #40 latent-gap note, resolved): both operands
//    literal-constructed, i.e. CEL_LIST_ARENA / CEL_MAP_ARENA values
//    holding CEL_MESSAGE elements or nested aggregates.  The kernel's
//    scalar-only arena fast path used to return a silent `false` for
//    `[TestAllTypes{single_int64: 1}] == [TestAllTypes{single_int64:
//    1}]`; it now routes such element pairs through the polymorphic
//    equality kernel and the `cel_host.cel_message_eq` trampoline —
//    the same Any-peel + MessageDifferencer path a direct
//    `msg1 == msg2` takes (equivalence asserted below).
//
// 2. Comprehension over a poisoned source: `[1/0].exists(x, x == 2)`
//    used to silently evaluate `false` (the kernel degraded the
//    poisoned iter_range to an empty walk); it now propagates the
//    construction error into the comprehension result.
//
// Kernel-level matrices live in runtime/cel_deep_eq_test.cc and
// runtime/cel_oom_poison_test.cc; this file pins the user-visible
// behavior through the full compile → plan → eval pipeline in BOTH
// link modes (see e2e/BUILD.bazel `link_mode_e2e_cc_test`).

#include <string>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
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
// descriptors so the checker resolves the TestAllTypes message
// literals (nothing else in this file references the generated
// class, so without this the linker drops the registration).
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<TestAllTypes>();
      return 0;
    }();

constexpr absl::string_view kMsg = "cel.expr.conformance.proto2.TestAllTypes";

// Declaring a variable of the message type pulls the conformance
// descriptors into the checker's scope so the message literals
// resolve; no binding is needed for the literal-only expressions.
Compiler MakeCompiler() {
  Compiler::Builder b;
  b.DeclareVariable("msg", CelType::Message(std::string(kMsg)));
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  return *std::move(compiler);
}

Value Eval(absl::string_view source) {
  Compiler compiler = MakeCompiler();
  auto instance = CompilePlan(compiler, source);
  Activation a;
  return EvalOk(instance, a);
}

bool EvalBool(absl::string_view source) {
  auto result = Eval(source).AsBool();
  ABSL_CHECK_OK(result) << source;
  return *result;
}

// Shorthand: `M{...}` → fully-qualified TestAllTypes literal.
std::string T(absl::string_view fields) {
  return std::string(kMsg) + "{" + std::string(fields) + "}";
}

class ArenaMessageAggregateEqE2ETest : public ::testing::Test {};

// ── arena+arena lists of messages ──────────────────────────────────

TEST_F(ArenaMessageAggregateEqE2ETest, MessageListsEqual) {
  EXPECT_TRUE(EvalBool("[" + T("single_int64: 1") + "] == [" +
                       T("single_int64: 1") + "]"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, MessageListsUnequal) {
  EXPECT_FALSE(EvalBool("[" + T("single_int64: 1") + "] == [" +
                        T("single_int64: 2") + "]"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, MessageListsLengthMismatch) {
  EXPECT_FALSE(EvalBool("[" + T("single_int64: 1") + ", " +
                        T("single_bool: true") + "] == [" +
                        T("single_int64: 1") + "]"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, MessageListEmptyVsNonEmptyViaNotEquals) {
  EXPECT_TRUE(EvalBool("[" + T("single_int64: 1") + "] != [" +
                       T("single_int64: 2") + "]"));
  EXPECT_FALSE(EvalBool("[" + T("single_int64: 1") + "] != [" +
                        T("single_int64: 1") + "]"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, MultiElementMessageListsEqual) {
  const std::string lhs =
      "[" + T("single_int64: 1") + ", " + T("single_bool: true") + "]";
  EXPECT_TRUE(EvalBool(lhs + " == " + lhs));
}

// ── equivalence with direct msg == msg ─────────────────────────────

TEST_F(ArenaMessageAggregateEqE2ETest, ListVerdictMatchesDirectMessageEq) {
  // The list walk must produce exactly the verdict the direct
  // message comparison (CelMessageEqImpl: Any-peel + descriptor +
  // MessageDifferencer) produces on the same operands.
  const bool direct_eq =
      EvalBool(T("single_int64: 1") + " == " + T("single_int64: 1"));
  const bool direct_ne =
      EvalBool(T("single_int64: 1") + " == " + T("single_int64: 2"));
  EXPECT_EQ(EvalBool("[" + T("single_int64: 1") + "] == [" +
                     T("single_int64: 1") + "]"),
            direct_eq);
  EXPECT_EQ(EvalBool("[" + T("single_int64: 1") + "] == [" +
                     T("single_int64: 2") + "]"),
            direct_ne);
  EXPECT_TRUE(direct_eq);
  EXPECT_FALSE(direct_ne);
}

// ── arena+arena maps with message values ───────────────────────────

TEST_F(ArenaMessageAggregateEqE2ETest, MessageValuedMapsEqual) {
  EXPECT_TRUE(EvalBool("{'a': " + T("single_int64: 1") +
                       "} == {'a': " + T("single_int64: 1") + "}"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, MessageValuedMapsUnequal) {
  EXPECT_FALSE(EvalBool("{'a': " + T("single_int64: 1") +
                        "} == {'a': " + T("single_int64: 2") + "}"));
}

// ── nested aggregate-in-aggregate ──────────────────────────────────

TEST_F(ArenaMessageAggregateEqE2ETest, NestedListsEqual) {
  EXPECT_TRUE(EvalBool("[[1, 2], [3]] == [[1, 2], [3]]"));
  EXPECT_FALSE(EvalBool("[[1]] == [[2]]"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, MapsInListsEqual) {
  EXPECT_TRUE(EvalBool("[{1: 2}] == [{1: 2}]"));
  EXPECT_FALSE(EvalBool("[{1: 2}] == [{1: 3}]"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, ListsInMapValuesEqual) {
  EXPECT_TRUE(EvalBool("{1: [1]} == {1: [1]}"));
  EXPECT_FALSE(EvalBool("{1: [1]} == {1: [2]}"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, MessageListNestedInListEqual) {
  EXPECT_TRUE(EvalBool("[[" + T("single_int64: 1") + "]] == [[" +
                       T("single_int64: 1") + "]]"));
}

// ── `in` with non-scalar needles ───────────────────────────────────

TEST_F(ArenaMessageAggregateEqE2ETest, MessageInList) {
  EXPECT_TRUE(EvalBool(T("single_int64: 1") + " in [" + T("single_bool: true") +
                       ", " + T("single_int64: 1") + "]"));
  EXPECT_FALSE(
      EvalBool(T("single_int64: 1") + " in [" + T("single_int64: 2") + "]"));
}

TEST_F(ArenaMessageAggregateEqE2ETest, ListInListOfLists) {
  EXPECT_TRUE(EvalBool("[1] in [[2], [1]]"));
  EXPECT_FALSE(EvalBool("[1] in [[2]]"));
}

// ── scalar control: fast path semantics unchanged ──────────────────

TEST_F(ArenaMessageAggregateEqE2ETest, ScalarListsControl) {
  EXPECT_TRUE(EvalBool("[1, 2, 3] == [1, 2, 3]"));
  EXPECT_FALSE(EvalBool("[1, 2, 3] == [1, 2, 4]"));
  EXPECT_TRUE(EvalBool("{'k': 'v'} == {'k': 'v'}"));
}

// ── comprehension over a poisoned source ───────────────────────────
// (item 2 of the same kernel-correctness batch: the OOM/poisoned-
// source empty-walk degrade in `cel_list_arena_view` /
// `cel_map_iter_init`.)

class PoisonedComprehensionSourceE2ETest : public ::testing::Test {};

TEST_F(PoisonedComprehensionSourceE2ETest, ErrorListSourceMapPropagates) {
  EXPECT_TRUE(Eval("[1 / 0].map(x, x)").IsError());
}

TEST_F(PoisonedComprehensionSourceE2ETest, ErrorListSourceExistsPropagates) {
  // Used to silently evaluate `false`.
  EXPECT_TRUE(Eval("[1 / 0].exists(x, x == 2)").IsError());
}

TEST_F(PoisonedComprehensionSourceE2ETest, ErrorElementFilterPropagates) {
  EXPECT_TRUE(Eval("[1, 2, 1 / 0].filter(x, x > 1)").IsError());
}

TEST_F(PoisonedComprehensionSourceE2ETest, ErrorMapSourceExistsPropagates) {
  // The map literal poisons at construction ({'a': 1/0} is a strict
  // create); the comprehension over it must surface that error.
  EXPECT_TRUE(Eval("{'a': 1 / 0}.exists(k, k == 'b')").IsError());
}

TEST_F(PoisonedComprehensionSourceE2ETest, ErrorValuedMapLiteralIsError) {
  EXPECT_TRUE(Eval("{'a': 1 / 0}").IsError());
  EXPECT_TRUE(Eval("size({'a': 1 / 0})").IsError());
}

TEST_F(PoisonedComprehensionSourceE2ETest, ErrorKeyedMapLiteralIsError) {
  EXPECT_TRUE(Eval("{1 / 0: 'a'}").IsError());
}

TEST_F(PoisonedComprehensionSourceE2ETest, HealthyComprehensionControl) {
  // Controls: untouched comprehension semantics.
  EXPECT_TRUE(EvalBool("[1, 2, 3].map(x, x * 2) == [2, 4, 6]"));
  EXPECT_FALSE(EvalBool("[].exists(x, x == 2)"));
  EXPECT_TRUE(EvalBool("{'a': 1}.exists(k, k == 'a')"));
}

}  // namespace
}  // namespace celwasm
