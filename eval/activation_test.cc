#include "eval/activation.h"

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "eval/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(ActivationTest, UnboundNameReturnsNullptr) {
  Activation act;
  EXPECT_EQ(act.Find("x"), nullptr);
}

TEST(ActivationTest, BindRoundTrips) {
  Activation act;
  act.Bind("x", Value::Int(42));
  const Value* v = act.Find("x");
  ASSERT_NE(v, nullptr);
  auto n = v->AsInt();
  ASSERT_TRUE(n.ok());
  EXPECT_EQ(*n, 42);
}

TEST(ActivationTest, BindOverwritesPrior) {
  Activation act;
  act.Bind("x", Value::Int(1));
  act.Bind("x", Value::Int(2));
  const Value* v = act.Find("x");
  ASSERT_NE(v, nullptr);
  auto n = v->AsInt();
  ASSERT_TRUE(n.ok());
  EXPECT_EQ(*n, 2);
}

TEST(ActivationTest, BindReturnsReferenceForFluency) {
  Activation act;
  act.Bind("x", Value::Int(1))
      .Bind("y", Value::String("hi"))
      .Bind("z", Value::Bool(true));
  EXPECT_NE(act.Find("x"), nullptr);
  EXPECT_NE(act.Find("y"), nullptr);
  EXPECT_NE(act.Find("z"), nullptr);
}

TEST(ActivationDeathTest, BindLazyIsUnimplementedAndLoud) {
  Activation act;
  EXPECT_DEATH(
      {
        act.BindLazy("x", []() -> absl::StatusOr<Value> {
          return Value::Int(1);
        });
      },
      "BindLazy is unimplemented");
}

TEST(ActivationDeathTest, OverrideFunctionIsUnimplementedAndLoud) {
  Activation act;
  EXPECT_DEATH(
      {
        act.OverrideFunction("foo", [](auto) -> Value {
          return Value::Null();
        });
      },
      "OverrideFunction is unimplemented");
}

}  // namespace
}  // namespace celwasm
