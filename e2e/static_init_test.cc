// E2E tests focused on code paths that depend on C++ global static
// initialization — specifically:
//
//   - **cctz** — timezone tables, civil-time conversion lookups, the
//     IANA timezone-name interner.  Runtime code touches cctz via
//     `cel_host.cel_timestamp_tz_accessor` plus the in-runtime
//     `cel_time.c` arms that parse / format / arithmetic-on
//     timestamps and durations.
//   - **absl `str_format`** — used by `string(<double>)` to render
//     `double` values; pulls in absl's static format-spec registry
//     and pointer-conversion tables.
//   - **double-arithmetic that flows through cel_host** — comparison
//     against int, equality involving polymorphism, etc.
//
// **Why these matter for m28 (Configurable Linking).** In `kDynamic`
// mode every cross-module call into the runtime goes through wasi-
// libc's `.command_export` wrappers, which call `__wasm_call_ctors`
// before each body.  In `kStatic` mode the strip tool removed those
// wrappers — codegen calls bare bodies — and the prototype does NOT
// explicitly invoke `__wasm_call_ctors` at instantiate.  The first
// access to a cctz / absl global from the static-mode runtime
// silently reads zero-initialised memory.
//
// These tests are the load-bearing forcing function for the P1
// `CallInit` follow-up.  They are expected to pass in `kDynamic`
// (today's behaviour) and to expose the gap in `kStatic` if it
// hasn't been fixed — running the file twice via
// `link_mode_e2e_cc_test` gives us both signals from one source.
//
// **Coverage targets.**
//   - timestamp parse + accessor + arithmetic.
//   - duration parse + scalar accessor + arithmetic.
//   - mixed timestamp + duration ops (`t + d`, `t - t`).
//   - `string(<double>)` for representative values (zero, small,
//     large, negative, fractional, NaN-adjacent).
//   - `double(<string>)` parse round-trips.
//   - `double` arithmetic that crosses CelType boundaries.

#include "absl/log/absl_check.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::e2e::CompilePlan;
using ::celwasm::e2e::EvalOk;

// ──────────────────────────────────────────────────────────────────
// Cctz — timestamp parse + accessor.
// ──────────────────────────────────────────────────────────────────

class CctzE2ETest : public ::testing::Test {};

TEST_F(CctzE2ETest, TimestampParseRoundTripUtcEpoch) {
  // `timestamp(string)` is the headline cctz-touching path: the
  // runtime parses the RFC3339 string via cctz's ParseAbsoluteTime,
  // which dereferences a cctz-global timezone table on cold-start.
  // Under kStatic without __wasm_call_ctors, that table is zero —
  // null-deref or a wrong epoch.
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance =
      CompilePlan(*compiler_or, "timestamp(\"1970-01-01T00:00:00Z\")");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kTimestamp);
  // Epoch is absl::UnixEpoch() — a sanity floor that anything cctz
  // parsed correctly will reproduce.
  EXPECT_EQ(*v.AsTimestamp(), absl::UnixEpoch());
}

TEST_F(CctzE2ETest, TimestampParseNonEpochUtc) {
  // A non-zero offset proves cctz computed seconds-since-epoch
  // correctly, not just that it returned the zero-init default.
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance =
      CompilePlan(*compiler_or, "timestamp(\"2026-06-08T12:00:00Z\")");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kTimestamp);
  // Reconstruct from absl::FromCivil to avoid hand-counting epoch
  // seconds.  The point isn't the exact integer; it's that cctz
  // parsed the RFC3339 string correctly under both link modes.
  const absl::Time expected = absl::FromCivil(
      absl::CivilSecond(2026, 6, 8, 12, 0, 0), absl::UTCTimeZone());
  EXPECT_EQ(*v.AsTimestamp(), expected);
}

TEST_F(CctzE2ETest, TimestampGetSecondsAccessor) {
  // `t.getSeconds()` routes through `cel_host.cel_timestamp_tz_accessor`
  // — the cctz-using host trampoline.  Both the parse AND the accessor
  // depend on cctz init having run.
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or,
                              "timestamp(\"2026-01-01T00:00:00Z\").getSeconds()");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 0);
}

// ──────────────────────────────────────────────────────────────────
// Duration parse + arithmetic.
// ──────────────────────────────────────────────────────────────────

class DurationE2ETest : public ::testing::Test {};

TEST_F(DurationE2ETest, ParseSeconds) {
  // `duration(string)` parses via the runtime's `cel_duration_parse`
  // which uses cctz's `ParseDuration`.
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "duration(\"3600s\")");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kDuration);
  EXPECT_EQ(*v.AsDuration(), absl::Seconds(3600));
}

TEST_F(DurationE2ETest, DurationAddsToTimestamp) {
  // `timestamp + duration` exercises BOTH parsers + the arithmetic
  // arm.  Multiple cctz globals touched.
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance =
      CompilePlan(*compiler_or,
                  "timestamp(\"1970-01-01T00:00:00Z\") + duration(\"3600s\")");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kTimestamp);
  EXPECT_EQ(*v.AsTimestamp(), absl::FromUnixSeconds(3600));
}

TEST_F(DurationE2ETest, TimestampDifference) {
  // `t - t` returns a Duration; cctz subtract-and-normalise path.
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance =
      CompilePlan(*compiler_or,
                  "timestamp(\"1970-01-01T01:00:00Z\") - "
                  "timestamp(\"1970-01-01T00:00:00Z\")");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kDuration);
  EXPECT_EQ(*v.AsDuration(), absl::Seconds(3600));
}

// ──────────────────────────────────────────────────────────────────
// Double formatting — `string(<double>)`.
// ──────────────────────────────────────────────────────────────────
//
// `string(<double>)` uses absl's `StrFormat` family which pulls in
// statically-initialised conversion-spec registries.  Without
// `__wasm_call_ctors`, the registry is null-init → trap or "%g"
// returning garbage.

class DoubleStringifyE2ETest : public ::testing::Test {};

TEST_F(DoubleStringifyE2ETest, Zero) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "string(0.0)");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "0");
}

TEST_F(DoubleStringifyE2ETest, Integer) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "string(42.0)");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "42");
}

// The runtime emits full-precision %.17g output for double-to-string
// (`"-3.14000000000000012"` etc.).  We don't pin the exact format —
// we round-trip the produced string back to a double and check the
// numeric value.  Either way the path exercises absl's StrFormat
// machinery + its statically-initialised conversion-spec registry.

TEST_F(DoubleStringifyE2ETest, Negative) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "string(-3.14)");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  double parsed = 0.0;
  ASSERT_TRUE(absl::SimpleAtod(*v.AsString(), &parsed)) << *v.AsString();
  EXPECT_DOUBLE_EQ(parsed, -3.14);
}

TEST_F(DoubleStringifyE2ETest, Fractional) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "string(3.14)");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  double parsed = 0.0;
  ASSERT_TRUE(absl::SimpleAtod(*v.AsString(), &parsed)) << *v.AsString();
  EXPECT_DOUBLE_EQ(parsed, 3.14);
}

// ──────────────────────────────────────────────────────────────────
// Double parsing — `double(<string>)`.
// ──────────────────────────────────────────────────────────────────

class DoubleParseE2ETest : public ::testing::Test {};

TEST_F(DoubleParseE2ETest, ParseInteger) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "double(\"42\")");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kDouble);
  EXPECT_EQ(*v.AsDouble(), 42.0);
}

TEST_F(DoubleParseE2ETest, ParseFractional) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "double(\"3.14\")");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kDouble);
  EXPECT_DOUBLE_EQ(*v.AsDouble(), 3.14);
}

TEST_F(DoubleParseE2ETest, ParseNegative) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "double(\"-2.5\")");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kDouble);
  EXPECT_DOUBLE_EQ(*v.AsDouble(), -2.5);
}

// ──────────────────────────────────────────────────────────────────
// Double arithmetic edge cases.
// ──────────────────────────────────────────────────────────────────

class DoubleArithmeticE2ETest : public ::testing::Test {};

TEST_F(DoubleArithmeticE2ETest, FractionalAddition) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "1.5 + 2.25");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kDouble);
  EXPECT_DOUBLE_EQ(*v.AsDouble(), 3.75);
}

TEST_F(DoubleArithmeticE2ETest, DivisionResultingInRecurring) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "1.0 / 3.0");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kDouble);
  EXPECT_DOUBLE_EQ(*v.AsDouble(), 1.0 / 3.0);
}

TEST_F(DoubleArithmeticE2ETest, IntCoercedToDoubleViaMultiplication) {
  // `2 * 0.5` — checker resolves to the double overload; double
  // arithmetic path is taken (cel_double_mul_at_vv).
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto instance = CompilePlan(*compiler_or, "double(2) * 0.5");
  Activation a;
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kDouble);
  EXPECT_DOUBLE_EQ(*v.AsDouble(), 1.0);
}

}  // namespace
}  // namespace celwasm
