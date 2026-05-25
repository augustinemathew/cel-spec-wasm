// Coverage for the four Phase C parse / format kernels in
// `cel_time_parse.{h,cc}`:
//
//   cel_timestamp_parse_at_v   — RFC3339 string -> CEL_TIMESTAMP
//   cel_duration_parse_at_v    — absl-grammar string -> CEL_DURATION
//   cel_timestamp_format_at_v  — CEL_TIMESTAMP -> RFC3339 string (UTC, Z)
//   cel_duration_format_at_v   — CEL_DURATION -> proto-Duration JSON
//
// Test discipline mirrors `cel_convert_test.cc` /
// `cel_time_test.cc`: each kernel gets 3VL absorb + kind-mismatch
// negatives as focused TEST_F, then a parameterised matrix
// exhaustively covering the spec-mandated admit / reject sets.
//
// Spec citations: timestamp / duration acceptance envelope is pinned
// by the existing host trampolines (`CelTimestampParseImpl` /
// `CelDurationParseImpl` in `compiler_v2/api/internal/cel_host.cc`),
// which the runtime kernels mirror verbatim per `phase-c-plan.md`
// §4.1-4.4.  Langdef bounds: years 0001..9999 (RFC3339 lexical),
// duration |seconds| ≤ 315_576_000_000 (proto JSON envelope).

#include "compiler_v2/runtime/cel_time_parse.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_layout.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

constexpr int64_t kTsMinSeconds = -62135596800LL;   // 0001-01-01T00:00:00Z
constexpr int64_t kTsMaxSeconds = 253402300799LL;   // 9999-12-31T23:59:59Z
constexpr int64_t kDurMaxSeconds = 315576000000LL;  // proto JSON envelope

class TimeParseFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }

  uint32_t MakeOut() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  uint32_t MakeStr(const char* s) {
    return cel_make_string(s, static_cast<uint32_t>(std::strlen(s)));
  }

  uint32_t MakeError() {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_ERROR;
    v->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
    return slot;
  }

  uint32_t MakeUnknown() {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_UNKNOWN;
    v->payload.unk = 0u;
    return slot;
  }

  uint32_t MakeInt(int64_t i) {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_INT;
    v->payload.i = i;
    return slot;
  }

  uint32_t MakeTs(int64_t s, int32_t ns) {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_TIMESTAMP;
    v->payload.ts = CelDurTs{.seconds = s, .nanos = ns, ._pad = 0};
    return slot;
  }

  uint32_t MakeDur(int64_t s, int32_t ns) {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_DURATION;
    v->payload.dur = CelDurTs{.seconds = s, .nanos = ns, ._pad = 0};
    return slot;
  }

  const CelValue* At(uint32_t slot) {
    return cel_value_at(slot);
  }

  void ExpectError(uint32_t slot, uint32_t err) {
    const CelValue* v = At(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
    EXPECT_EQ(v->payload.err, err);
  }

  std::string StringAt(uint32_t slot) {
    const CelValue* v = At(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
    const char* base = reinterpret_cast<const char*>(cel_mem_base());
    return {base + v->payload.s.ptr, v->payload.s.len};
  }
};

// ───────────────────────────────────────────────────────────────
// 3VL absorb + kind-mismatch (negative coverage, one per kernel).
// ───────────────────────────────────────────────────────────────

TEST_F(TimeParseFixture, TimestampParseAbsorbsError) {
  uint32_t out = MakeOut();
  cel_timestamp_parse_at_v(out, MakeError());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, CEL_ERR_DIVIDE_BY_ZERO);  // passthrough
}

TEST_F(TimeParseFixture, TimestampParseAbsorbsUnknown) {
  uint32_t out = MakeOut();
  cel_timestamp_parse_at_v(out, MakeUnknown());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

TEST_F(TimeParseFixture, TimestampParseKindMismatch) {
  uint32_t out = MakeOut();
  cel_timestamp_parse_at_v(out, MakeInt(0));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(TimeParseFixture, DurationParseAbsorbsError) {
  uint32_t out = MakeOut();
  cel_duration_parse_at_v(out, MakeError());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
}

TEST_F(TimeParseFixture, DurationParseAbsorbsUnknown) {
  uint32_t out = MakeOut();
  cel_duration_parse_at_v(out, MakeUnknown());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

TEST_F(TimeParseFixture, DurationParseKindMismatch) {
  uint32_t out = MakeOut();
  cel_duration_parse_at_v(out, MakeInt(0));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(TimeParseFixture, TimestampFormatAbsorbsError) {
  uint32_t out = MakeOut();
  cel_timestamp_format_at_v(out, MakeError());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
}

TEST_F(TimeParseFixture, TimestampFormatAbsorbsUnknown) {
  uint32_t out = MakeOut();
  cel_timestamp_format_at_v(out, MakeUnknown());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

TEST_F(TimeParseFixture, TimestampFormatKindMismatch) {
  uint32_t out = MakeOut();
  cel_timestamp_format_at_v(out, MakeInt(0));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(TimeParseFixture, DurationFormatAbsorbsError) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeError());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
}

TEST_F(TimeParseFixture, DurationFormatAbsorbsUnknown) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeUnknown());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

TEST_F(TimeParseFixture, DurationFormatKindMismatch) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeInt(0));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

// ───────────────────────────────────────────────────────────────
// Timestamp parse — admit / reject matrix.
//
// Admitted rows assert (seconds, nanos) exactly so a future absl
// behaviour shift gets caught.  Rejected rows assert the resulting
// CEL_ERROR err code is INVALID_ARGUMENT (parse / langdef strict)
// or OVERFLOW (out-of-range second after a successful parse).
// ───────────────────────────────────────────────────────────────

struct TsParseRow {
  const char* in;
  // expect == kAccept: assert (seconds, nanos) match.
  // expect == kRejectInvalidArg / kRejectOverflow: assert error code.
  enum { kAccept, kRejectInvalidArg, kRejectOverflow } expect;
  int64_t seconds;
  int32_t nanos;
};

class TimestampParseTable : public TimeParseFixture,
                            public ::testing::WithParamInterface<TsParseRow> {};

TEST_P(TimestampParseTable, Matrix) {
  const TsParseRow& row = GetParam();
  uint32_t out = MakeOut();
  cel_timestamp_parse_at_v(out, MakeStr(row.in));
  switch (row.expect) {
    case TsParseRow::kAccept: {
      const CelValue* v = At(out);
      ASSERT_EQ(v->kind, static_cast<uint32_t>(CEL_TIMESTAMP))
          << "input=\"" << row.in << "\"";
      EXPECT_EQ(v->payload.ts.seconds, row.seconds);
      EXPECT_EQ(v->payload.ts.nanos, row.nanos);
      break;
    }
    case TsParseRow::kRejectInvalidArg:
      ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
      break;
    case TsParseRow::kRejectOverflow:
      ExpectError(out, CEL_ERR_OVERFLOW);
      break;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Admit, TimestampParseTable,
    ::testing::Values(
        // Unix epoch, integral seconds.
        TsParseRow{"1970-01-01T00:00:00Z", TsParseRow::kAccept, 0, 0},
        // Common-era reference.
        TsParseRow{"2026-05-18T10:00:00Z", TsParseRow::kAccept, 1779098400, 0},
        // Fractional seconds — 1, 3, 6, 9 digits exercise the decoder.
        TsParseRow{"1970-01-01T00:00:00.1Z", TsParseRow::kAccept, 0, 100000000},
        TsParseRow{"1970-01-01T00:00:00.123Z", TsParseRow::kAccept, 0, 123000000},
        TsParseRow{"1970-01-01T00:00:00.123456Z", TsParseRow::kAccept, 0,
                   123456000},
        TsParseRow{"1970-01-01T00:00:00.123456789Z", TsParseRow::kAccept, 0,
                   123456789},
        // Pre-epoch (negative seconds).
        TsParseRow{"1969-12-31T23:59:59Z", TsParseRow::kAccept, -1, 0},
        // TZ offset other than Z — admitted; result is normalised UTC.
        TsParseRow{"1970-01-01T05:00:00+05:00", TsParseRow::kAccept, 0, 0},
        TsParseRow{"1970-01-01T00:00:00-05:00", TsParseRow::kAccept, 18000, 0},
        // Langdef boundary: year 0001 lower edge.
        TsParseRow{"0001-01-01T00:00:00Z", TsParseRow::kAccept, kTsMinSeconds,
                   0},
        // Langdef boundary: year 9999 upper edge.
        TsParseRow{"9999-12-31T23:59:59Z", TsParseRow::kAccept, kTsMaxSeconds,
                   0}));

INSTANTIATE_TEST_SUITE_P(
    RejectSyntax, TimestampParseTable,
    ::testing::Values(
        // Empty + obvious garbage.
        TsParseRow{"", TsParseRow::kRejectInvalidArg, 0, 0},
        TsParseRow{"not-a-time", TsParseRow::kRejectInvalidArg, 0, 0},
        // Missing Z and offset.
        TsParseRow{"1970-01-01T00:00:00", TsParseRow::kRejectInvalidArg, 0, 0},
        // Wrong separator — date-only with no T.
        TsParseRow{"1970-01-01", TsParseRow::kRejectInvalidArg, 0, 0},
        // Two-digit year — `RejectsAsTimestampPerCEL` requires the
        // first dash at byte ≥4 (i.e. year padded to 4 digits).
        TsParseRow{"70-01-01T00:00:00Z", TsParseRow::kRejectInvalidArg, 0, 0},
        // Lowercase `z` — absl admits, CEL rejects per langdef §6
        // RFC3339 lexical rule.
        TsParseRow{"1970-01-01T00:00:00z", TsParseRow::kRejectInvalidArg, 0, 0},
        // Leap second `:60` — proto Timestamp rejects.
        TsParseRow{"1972-06-30T23:59:60Z", TsParseRow::kRejectInvalidArg, 0, 0},
        // Impossible month / day.
        TsParseRow{"1970-13-01T00:00:00Z", TsParseRow::kRejectInvalidArg, 0, 0},
        TsParseRow{"1970-02-30T00:00:00Z", TsParseRow::kRejectInvalidArg, 0,
                   0}));

INSTANTIATE_TEST_SUITE_P(
    RejectOverflow, TimestampParseTable,
    ::testing::Values(
        // Year 0 — one second below `0001-01-01T00:00:00Z`; the
        // absl parser admits, the langdef-strict range check rejects
        // (overflow).  Use the well-formed RFC3339 spelling so we
        // exercise the post-parse range gate.
        TsParseRow{"0000-12-31T23:59:59Z", TsParseRow::kRejectOverflow,
                   kTsMinSeconds - 1, 0}));

// ───────────────────────────────────────────────────────────────
// Duration parse — admit / reject matrix.
// ───────────────────────────────────────────────────────────────

struct DurParseRow {
  const char* in;
  enum { kAccept, kRejectInvalidArg } expect;
  int64_t seconds;
  int32_t nanos;
};

class DurationParseTable : public TimeParseFixture,
                           public ::testing::WithParamInterface<DurParseRow> {};

TEST_P(DurationParseTable, Matrix) {
  const DurParseRow& row = GetParam();
  uint32_t out = MakeOut();
  cel_duration_parse_at_v(out, MakeStr(row.in));
  if (row.expect == DurParseRow::kAccept) {
    const CelValue* v = At(out);
    ASSERT_EQ(v->kind, static_cast<uint32_t>(CEL_DURATION))
        << "input=\"" << row.in << "\"";
    EXPECT_EQ(v->payload.dur.seconds, row.seconds);
    EXPECT_EQ(v->payload.dur.nanos, row.nanos);
  } else {
    ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Admit, DurationParseTable,
    ::testing::Values(
        // Zero.
        DurParseRow{"0s", DurParseRow::kAccept, 0, 0},
        // Each unit at rank 5..0.
        DurParseRow{"1h", DurParseRow::kAccept, 3600, 0},
        DurParseRow{"1m", DurParseRow::kAccept, 60, 0},
        DurParseRow{"1s", DurParseRow::kAccept, 1, 0},
        DurParseRow{"1ms", DurParseRow::kAccept, 0, 1000000},
        DurParseRow{"1us", DurParseRow::kAccept, 0, 1000},
        DurParseRow{"1ns", DurParseRow::kAccept, 0, 1},
        // Strictly-decreasing mixed units.
        DurParseRow{"1h30m", DurParseRow::kAccept, 5400, 0},
        DurParseRow{"2h3m4s", DurParseRow::kAccept, 2 * 3600 + 3 * 60 + 4, 0},
        DurParseRow{"1s500ms", DurParseRow::kAccept, 1, 500000000},
        // Negative.
        DurParseRow{"-1s", DurParseRow::kAccept, -1, 0},
        DurParseRow{"-1h30m", DurParseRow::kAccept, -5400, 0},
        // Fractional seconds (decimal notation).
        DurParseRow{"1.5s", DurParseRow::kAccept, 1, 500000000}));

INSTANTIATE_TEST_SUITE_P(
    Reject, DurationParseTable,
    ::testing::Values(
        // Empty.
        DurParseRow{"", DurParseRow::kRejectInvalidArg, 0, 0},
        // Bare number (no unit).
        DurParseRow{"1", DurParseRow::kRejectInvalidArg, 0, 0},
        // Unknown unit.
        DurParseRow{"1d", DurParseRow::kRejectInvalidArg, 0, 0},
        DurParseRow{"1y", DurParseRow::kRejectInvalidArg, 0, 0},
        // Non-decreasing unit order — `RejectsAsDurationPerCEL` gates.
        DurParseRow{"1s2h", DurParseRow::kRejectInvalidArg, 0, 0},
        DurParseRow{"1ms1s", DurParseRow::kRejectInvalidArg, 0, 0},
        DurParseRow{"1ns1us", DurParseRow::kRejectInvalidArg, 0, 0},
        // Repeated unit at same rank.
        DurParseRow{"1h2h", DurParseRow::kRejectInvalidArg, 0, 0},
        // Beyond proto-Duration JSON envelope (>315B s).
        DurParseRow{"315576000001s", DurParseRow::kRejectInvalidArg, 0, 0},
        DurParseRow{"-315576000001s", DurParseRow::kRejectInvalidArg, 0, 0}));

// Boundary at exactly ±315B seconds — admitted.
TEST_F(TimeParseFixture, DurationParseAcceptsMaxBoundary) {
  uint32_t out = MakeOut();
  cel_duration_parse_at_v(out, MakeStr("315576000000s"));
  ASSERT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_DURATION));
  EXPECT_EQ(At(out)->payload.dur.seconds, kDurMaxSeconds);
}

TEST_F(TimeParseFixture, DurationParseAcceptsMinBoundary) {
  uint32_t out = MakeOut();
  cel_duration_parse_at_v(out, MakeStr("-315576000000s"));
  ASSERT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_DURATION));
  EXPECT_EQ(At(out)->payload.dur.seconds, -kDurMaxSeconds);
}

// ───────────────────────────────────────────────────────────────
// Timestamp format — direct + round-trip.
// ───────────────────────────────────────────────────────────────

TEST_F(TimeParseFixture, TimestampFormatEpoch) {
  uint32_t out = MakeOut();
  cel_timestamp_format_at_v(out, MakeTs(0, 0));
  EXPECT_EQ(StringAt(out), "1970-01-01T00:00:00Z");
}

TEST_F(TimeParseFixture, TimestampFormatTrailingZNotOffset) {
  // absl's RFC3339_full emits `+00:00`; the kernel rewrites to `Z`.
  uint32_t out = MakeOut();
  cel_timestamp_format_at_v(out, MakeTs(0, 0));
  const std::string s = StringAt(out);
  EXPECT_FALSE(s.empty());
  EXPECT_EQ(s.back(), 'Z');
  EXPECT_EQ(s.find("+00:00"), std::string::npos);
}

TEST_F(TimeParseFixture, TimestampFormatPreservesNanos) {
  uint32_t out = MakeOut();
  cel_timestamp_format_at_v(out, MakeTs(0, 123456789));
  EXPECT_EQ(StringAt(out), "1970-01-01T00:00:00.123456789Z");
}

TEST_F(TimeParseFixture, TimestampFormatHalfSecond) {
  // Documents current behaviour: kernel formats via
  // `absl::FormatTime(RFC3339_full, ...)` and then swaps `+00:00` for
  // `Z`, with no post-processing of fractional digits.  Proto Timestamp
  // JSON spec (proto3 JSON mapping) requires the fractional component
  // to be 0, 3, 6, or 9 digits — never 1.  `0.500000000s` would be
  // proto-compliant; `.5Z` is what absl emits and what this kernel
  // currently produces.  Tracked as a separate Phase C conformance
  // gap; do NOT change this assertion without fixing the kernel.
  uint32_t out = MakeOut();
  cel_timestamp_format_at_v(out, MakeTs(0, 500000000));
  EXPECT_EQ(StringAt(out), "1970-01-01T00:00:00.5Z");
}

TEST_F(TimeParseFixture, TimestampParseFormatRoundTrip) {
  // Parse then re-format; assert byte-identical output for the
  // canonical UTC spelling.  Excludes inputs that exercise the proto
  // JSON gaps documented in `TimestampFormatHalfSecond`:
  //   - sub-1000 years: kernel emits unpadded year (`0001` → `1`).
  //   - non-9-digit fractional seconds: kernel emits minimal-digit
  //     fraction (`.500000000` → `.5`).
  // Those are conformance follow-ups; this round-trip locks in the
  // behaviour for inputs the kernel handles losslessly today.
  const char* kInputs[] = {
      "1970-01-01T00:00:00Z",
      "2026-05-18T10:00:00Z",
      "1969-12-31T23:59:59Z",
      "9999-12-31T23:59:59Z",
      "1970-01-01T00:00:00.123456789Z",
  };
  for (const char* in : kInputs) {
    arena_reset();
    uint32_t parsed = MakeOut();
    cel_timestamp_parse_at_v(parsed, MakeStr(in));
    ASSERT_EQ(At(parsed)->kind, static_cast<uint32_t>(CEL_TIMESTAMP)) << in;
    uint32_t formatted = MakeOut();
    cel_timestamp_format_at_v(formatted, parsed);
    EXPECT_EQ(StringAt(formatted), in) << "round-trip drift for " << in;
  }
}

// ───────────────────────────────────────────────────────────────
// Duration format — direct + round-trip.
//
// Output shape is `[-]<seconds>[.<frac>]s` per proto Duration JSON;
// fractional part trims trailing zero-triples (3 / 6 / 9 digits).
// ───────────────────────────────────────────────────────────────

TEST_F(TimeParseFixture, DurationFormatZero) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeDur(0, 0));
  EXPECT_EQ(StringAt(out), "0s");
}

TEST_F(TimeParseFixture, DurationFormatPositiveSeconds) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeDur(1, 0));
  EXPECT_EQ(StringAt(out), "1s");
}

TEST_F(TimeParseFixture, DurationFormatNegativeSeconds) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeDur(-1, 0));
  EXPECT_EQ(StringAt(out), "-1s");
}

TEST_F(TimeParseFixture, DurationFormatThreeDigitFraction) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeDur(0, 500000000));
  EXPECT_EQ(StringAt(out), "0.500s");
}

TEST_F(TimeParseFixture, DurationFormatSixDigitFraction) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeDur(0, 123000));
  EXPECT_EQ(StringAt(out), "0.000123s");
}

TEST_F(TimeParseFixture, DurationFormatNineDigitFraction) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeDur(0, 1));
  EXPECT_EQ(StringAt(out), "0.000000001s");
}

TEST_F(TimeParseFixture, DurationFormatNegativeNanosOnly) {
  // Sign of the encoded value is taken from EITHER component
  // being negative; `(0, -1ns)` formats as `-0.000000001s`.
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeDur(0, -1));
  EXPECT_EQ(StringAt(out), "-0.000000001s");
}

TEST_F(TimeParseFixture, DurationFormatMaxBoundary) {
  uint32_t out = MakeOut();
  cel_duration_format_at_v(out, MakeDur(kDurMaxSeconds, 0));
  EXPECT_EQ(StringAt(out), "315576000000s");
}

}  // namespace
}  // namespace celwasm
