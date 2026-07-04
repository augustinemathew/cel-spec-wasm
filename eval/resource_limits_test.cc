// Unit coverage for `celwasm::ResourceLimits` — the value semantics of
// the public bound struct.  Behavioral enforcement (a component that
// loops forever trapping at the deadline, the memory cap, the
// Unlimited() opt-out) is exercised end-to-end in
// `e2e/component_resource_limits_test.cc`.

#include "eval/resource_limits.h"

#include "absl/time/time.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(ResourceLimitsTest, DefaultIsSafeAndBounded) {
  ResourceLimits l = ResourceLimits::Default();
  EXPECT_EQ(l.max_eval_time, absl::Seconds(1));
  EXPECT_EQ(l.max_memory_bytes, uint64_t{64} << 20);
  EXPECT_GT(l.max_eval_time, absl::ZeroDuration());
  EXPECT_GT(l.max_memory_bytes, 0u);
}

TEST(ResourceLimitsTest, DefaultConstructedEqualsDefaultPreset) {
  ResourceLimits def;
  ResourceLimits preset = ResourceLimits::Default();
  EXPECT_EQ(def.max_eval_time, preset.max_eval_time);
  EXPECT_EQ(def.max_memory_bytes, preset.max_memory_bytes);
}

TEST(ResourceLimitsTest, UnlimitedDisablesBothBounds) {
  ResourceLimits l = ResourceLimits::Unlimited();
  // Non-positive deadline is the "no deadline / no timer thread"
  // sentinel the engine keys on.
  EXPECT_LE(l.max_eval_time, absl::ZeroDuration());
  // Zero memory cap is the "unlimited" sentinel.
  EXPECT_EQ(l.max_memory_bytes, 0u);
}

TEST(ResourceLimitsTest, FieldsAreIndependentlyOverridable) {
  ResourceLimits l;
  l.max_eval_time = absl::Milliseconds(250);
  EXPECT_EQ(l.max_eval_time, absl::Milliseconds(250));
  // Overriding the deadline leaves the memory cap at its default.
  EXPECT_EQ(l.max_memory_bytes, uint64_t{64} << 20);

  l.max_memory_bytes = uint64_t{1} << 20;
  EXPECT_EQ(l.max_memory_bytes, uint64_t{1} << 20);
  EXPECT_EQ(l.max_eval_time, absl::Milliseconds(250));
}

}  // namespace
}  // namespace celwasm
