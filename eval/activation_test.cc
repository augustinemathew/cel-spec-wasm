#include "eval/activation.h"

#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "eval/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Resolve() and unwrap to an int, asserting each step.  Most cases care
// about the bound integer, not the pointer plumbing.
int ResolvedInt(const Activation& act, absl::string_view name) {
  auto found = act.Resolve(name);
  EXPECT_THAT(found, IsOk());
  EXPECT_NE(*found, nullptr);
  auto n = (*found)->AsInt();
  EXPECT_THAT(n, IsOk());
  return *n;
}

// ---------- Eager binding ---------------------------------------------------

TEST(ActivationTest, UnboundNameResolvesToNullptr) {
  Activation act;
  auto found = act.Resolve("x");
  ASSERT_THAT(found, IsOk());
  EXPECT_EQ(*found, nullptr);
}

TEST(ActivationTest, BindRoundTrips) {
  Activation act;
  act.Bind("x", Value::Int(42));
  EXPECT_EQ(ResolvedInt(act, "x"), 42);
}

TEST(ActivationTest, BindOverwritesPrior) {
  Activation act;
  act.Bind("x", Value::Int(1));
  act.Bind("x", Value::Int(2));
  EXPECT_EQ(ResolvedInt(act, "x"), 2);
}

TEST(ActivationTest, BindReturnsReferenceForFluency) {
  Activation act;
  act.Bind("x", Value::Int(1))
      .Bind("y", Value::String("hi"))
      .Bind("z", Value::Bool(true));
  for (absl::string_view name : {"x", "y", "z"}) {
    auto found = act.Resolve(name);
    ASSERT_THAT(found, IsOk());
    EXPECT_NE(*found, nullptr) << name;
  }
}

// ---------- Lazy binding ----------------------------------------------------

TEST(ActivationTest, BindLazyRoundTrips) {
  Activation act;
  act.BindLazy("x", []() -> absl::StatusOr<Value> { return Value::Int(7); });
  EXPECT_EQ(ResolvedInt(act, "x"), 7);
}

TEST(ActivationTest, BindLazyIsNotInvokedUntilResolved) {
  int calls = 0;
  Activation act;
  act.BindLazy("x", [&calls]() -> absl::StatusOr<Value> {
    ++calls;
    return Value::Int(1);
  });
  EXPECT_EQ(calls, 0) << "binding alone must not invoke the binder";
  (void)act.Resolve("x");
  EXPECT_EQ(calls, 1);
}

// The memoization contract: repeated reads within one evaluation see one
// invocation.  Instance reads a variable twice (a sizing pre-pass and the
// marshal itself), so without this a lazy string variable's binder would
// run twice per Eval.
TEST(ActivationTest, BindLazyIsInvokedAtMostOncePerEvaluation) {
  int calls = 0;
  Activation act;
  act.BindLazy("x", [&calls]() -> absl::StatusOr<Value> {
    ++calls;
    return Value::Int(5);
  });
  EXPECT_EQ(ResolvedInt(act, "x"), 5);
  EXPECT_EQ(ResolvedInt(act, "x"), 5);
  EXPECT_EQ(ResolvedInt(act, "x"), 5);
  EXPECT_EQ(calls, 1);
}

TEST(ActivationTest, ClearLazyCacheCausesReinvocation) {
  int calls = 0;
  Activation act;
  act.BindLazy("x", [&calls]() -> absl::StatusOr<Value> {
    ++calls;
    return Value::Int(calls);
  });
  EXPECT_EQ(ResolvedInt(act, "x"), 1);
  act.ClearLazyCache();
  EXPECT_EQ(ResolvedInt(act, "x"), 2) << "next evaluation re-invokes";
  EXPECT_EQ(calls, 2);
}

TEST(ActivationTest, ClearLazyCacheOnUncachedActivationIsANoop) {
  Activation act;
  act.ClearLazyCache();
  act.Bind("x", Value::Int(1));
  act.ClearLazyCache();
  EXPECT_EQ(ResolvedInt(act, "x"), 1) << "eager bindings survive a clear";
}

// A failing binder propagates verbatim — Instance surfaces it as the
// Eval status rather than remapping it.
TEST(ActivationTest, BindLazyErrorPropagatesVerbatim) {
  Activation act;
  act.BindLazy("x", []() -> absl::StatusOr<Value> {
    return absl::InvalidArgumentError("host value did not convert");
  });
  EXPECT_THAT(act.Resolve("x"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "host value did not convert"));
}

TEST(ActivationTest, FailingBinderIsRetriedRatherThanCachingTheFailure) {
  int calls = 0;
  Activation act;
  act.BindLazy("x", [&calls]() -> absl::StatusOr<Value> {
    ++calls;
    if (calls == 1) return absl::UnavailableError("transient");
    return Value::Int(99);
  });
  EXPECT_THAT(act.Resolve("x"), StatusIs(absl::StatusCode::kUnavailable));
  EXPECT_EQ(ResolvedInt(act, "x"), 99);
}

// ---------- Interaction between the two binding kinds -----------------------

TEST(ActivationTest, BindOverwritesLazyBinding) {
  int calls = 0;
  Activation act;
  act.BindLazy("x", [&calls]() -> absl::StatusOr<Value> {
    ++calls;
    return Value::Int(1);
  });
  act.Bind("x", Value::Int(2));
  EXPECT_EQ(ResolvedInt(act, "x"), 2);
  EXPECT_EQ(calls, 0) << "the replaced binder must never run";
}

TEST(ActivationTest, BindLazyOverwritesEagerBinding) {
  Activation act;
  act.Bind("x", Value::Int(1));
  act.BindLazy("x", []() -> absl::StatusOr<Value> { return Value::Int(2); });
  EXPECT_EQ(ResolvedInt(act, "x"), 2);
}

TEST(ActivationTest, BindOverwritesAnAlreadyMemoizedLazyValue) {
  Activation act;
  act.BindLazy("x", []() -> absl::StatusOr<Value> { return Value::Int(1); });
  EXPECT_EQ(ResolvedInt(act, "x"), 1);  // memoize it
  act.Bind("x", Value::Int(2));
  EXPECT_EQ(ResolvedInt(act, "x"), 2) << "stale memo must be dropped";
}

TEST(ActivationTest, BindLazyOverwritesAnAlreadyMemoizedLazyValue) {
  Activation act;
  act.BindLazy("x", []() -> absl::StatusOr<Value> { return Value::Int(1); });
  EXPECT_EQ(ResolvedInt(act, "x"), 1);
  act.BindLazy("x", []() -> absl::StatusOr<Value> { return Value::Int(2); });
  EXPECT_EQ(ResolvedInt(act, "x"), 2);
}

TEST(ActivationTest, LazyBindersAreIndependentPerName) {
  Activation act;
  int x_calls = 0;
  int y_calls = 0;
  act.BindLazy("x", [&x_calls]() -> absl::StatusOr<Value> {
    ++x_calls;
    return Value::Int(1);
  });
  act.BindLazy("y", [&y_calls]() -> absl::StatusOr<Value> {
    ++y_calls;
    return Value::Int(2);
  });
  EXPECT_EQ(ResolvedInt(act, "x"), 1);
  EXPECT_EQ(x_calls, 1);
  EXPECT_EQ(y_calls, 0) << "resolving one name must not force the other";
}

// ---------- Value semantics -------------------------------------------------

TEST(ActivationTest, IsMovable) {
  Activation act;
  act.Bind("x", Value::Int(1));
  act.BindLazy("y", []() -> absl::StatusOr<Value> { return Value::Int(2); });
  Activation moved = std::move(act);
  EXPECT_EQ(ResolvedInt(moved, "x"), 1);
  EXPECT_EQ(ResolvedInt(moved, "y"), 2);
}

static_assert(!std::is_copy_constructible_v<Activation>,
              "a lazy binder is move-only, so Activation must be too");

}  // namespace
}  // namespace celwasm
