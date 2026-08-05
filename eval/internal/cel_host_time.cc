#include "eval/internal/cel_host_time.h"

#include <cctype>
#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "eval/internal/cel_host_error.h"
#include "runtime/cel_data.h"

namespace celwasm {

// With-TZ accessor dispatch trampoline.
// ══════════════════════════════════════════════════════════════════
//
// Single host import absorbs all 10 with-TZ accessor surfaces; the
// per-accessor shims in cel_time.c supply the `accessor_kind`
// constant.  Wire enum lives in cel_time.h (`CelTzAccessorKind`)
// and is mirrored here — keep them in lockstep.  Rationale ("1
// dispatch trampoline vs 10 named trampolines": ABI surface count
// savings > switch-branch cost) lives in
// `doc/implementation-plan/rewrite/m7b-duration-timestamp.md`.

namespace {

// Mirrors `CelTzAccessorKind` in cel_time.h.  Append-only.
enum class TzAccessorKind : uint8_t {
  kYear = 0,
  kMonth = 1,
  kDayOfMonth1 = 2,
  kDayOfMonth = 3,
  kDayOfYear = 4,
  kDayOfWeek = 5,
  kHours = 6,
  kMinutes = 7,
  kSeconds = 8,
  kMilliseconds = 9,
};

int64_t ProjectCivilField(const absl::CivilSecond& cs, absl::Weekday weekday,
                          int day_of_year, int64_t ns_in_second,
                          TzAccessorKind kind) {
  switch (kind) {
    case TzAccessorKind::kYear:
      return cs.year();
    case TzAccessorKind::kMonth:
      return cs.month() - 1;  // cel-cpp 0-based
    case TzAccessorKind::kDayOfMonth1:
      return cs.day();  // 1-based
    case TzAccessorKind::kDayOfMonth:
      return cs.day() - 1;  // 0-based
    case TzAccessorKind::kDayOfYear:
      return day_of_year - 1;  // absl 1-based → cel-cpp 0-based
    case TzAccessorKind::kDayOfWeek:
      // absl::Weekday: monday=0..sunday=6.  cel-cpp: sunday=0..saturday=6.
      return (static_cast<int>(weekday) + 1) % 7;
    case TzAccessorKind::kHours:
      return cs.hour();
    case TzAccessorKind::kMinutes:
      return cs.minute();
    case TzAccessorKind::kSeconds:
      return cs.second();
    case TzAccessorKind::kMilliseconds: {
      // Sub-second within the civil second.  Sign-correlated nanos
      // get unix-floor-shifted to match cel-cpp's
      // `ToInt64Milliseconds(t - FloorToSecond(t))`.
      int64_t n = ns_in_second;
      if (n < 0) n += 1'000'000'000;
      return n / 1'000'000;
    }
  }
  return 0;  // unreachable; codegen never emits unknown kinds.
}

}  // namespace

namespace {

// Resolve a TZ string to an `absl::TimeZone`.  Three shapes:
//   - "UTC" / "Z" → UTC.
//   - "+HH:MM" / "-HH:MM" → fixed offset; absl::LoadTimeZone doesn't
//     parse these inline so we do it ourselves (plan §4.3).
//   - IANA name → absl::LoadTimeZone walks the host tzdata.
// Returns false on parse failure or unknown IANA name.
bool ResolveTimeZone(absl::string_view name, absl::TimeZone* out) {
  if (name == "UTC" || name == "Z") {
    *out = absl::UTCTimeZone();
    return true;
  }
  // Fixed offset: `+HH:MM` / `-HH:MM` / `HH:MM` (no sign = +).
  // cel-cpp admits the unsigned form per
  // `runtime/standard/time_functions.cc`.  Trim the sign prefix
  // if present, then validate HH:MM digit layout.
  int sign = 1;
  absl::string_view rest = name;
  if (!rest.empty() && (rest[0] == '+' || rest[0] == '-')) {
    sign = rest[0] == '+' ? 1 : -1;
    rest.remove_prefix(1);
  }
  if (rest.size() == 5 && rest[2] == ':' &&
      std::isdigit(static_cast<unsigned char>(rest[0])) &&
      std::isdigit(static_cast<unsigned char>(rest[1])) &&
      std::isdigit(static_cast<unsigned char>(rest[3])) &&
      std::isdigit(static_cast<unsigned char>(rest[4]))) {
    const int hours = ((rest[0] - '0') * 10) + (rest[1] - '0');
    const int minutes = ((rest[3] - '0') * 10) + (rest[4] - '0');
    if (hours > 23 || minutes > 59) return false;
    *out = absl::FixedTimeZone(sign * ((hours * 3600) + (minutes * 60)));
    return true;
  }
  return absl::LoadTimeZone(std::string(name), out);
}

// 3VL absorb + operand kind guards for the TZ-accessor trampoline.
// Returns true (and writes the result CelValue) if the call has
// already been short-circuited; false to continue.
bool TzAccessorPrelude(uint32_t out_slot, const CelValue& ts_cv,
                       const CelValue& tz_cv, uint32_t accessor_kind,
                       MemoryView& mem) {
  if (ts_cv.kind == CEL_ERROR || ts_cv.kind == CEL_UNKNOWN) {
    mem.WriteCelValue(out_slot, ts_cv);
    return true;
  }
  if (tz_cv.kind == CEL_ERROR || tz_cv.kind == CEL_UNKNOWN) {
    mem.WriteCelValue(out_slot, tz_cv);
    return true;
  }
  if (ts_cv.kind != CEL_TIMESTAMP || tz_cv.kind != CEL_STRING ||
      accessor_kind > static_cast<uint32_t>(TzAccessorKind::kMilliseconds)) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    mem.WriteCelValue(out_slot, err);
    return true;
  }
  return false;
}

}  // namespace

absl::Status CelTimestampTzAccessorImpl(uint32_t out_slot, uint32_t ts_slot,
                                        uint32_t tz_slot,
                                        uint32_t accessor_kind,
                                        const TrampolineContext& ctx) {
  CelValue ts_cv = ctx.mem.ReadCelValue(ts_slot);
  CelValue tz_cv = ctx.mem.ReadCelValue(tz_slot);
  if (TzAccessorPrelude(out_slot, ts_cv, tz_cv, accessor_kind, ctx.mem)) {
    return absl::OkStatus();
  }
  const absl::string_view tz_name =
      ctx.mem.ReadSpan(tz_cv.payload.s.ptr, tz_cv.payload.s.len);
  absl::TimeZone tz;
  if (!ResolveTimeZone(tz_name, &tz)) {
    WriteInvalidArgumentError(out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const absl::Time t = absl::UnixEpoch() +
                       absl::Seconds(ts_cv.payload.ts.seconds) +
                       absl::Nanoseconds(ts_cv.payload.ts.nanos);
  const absl::TimeZone::CivilInfo info = tz.At(t);
  const int day_of_year = absl::GetYearDay(absl::CivilDay(info.cs));
  const absl::Weekday weekday = absl::GetWeekday(absl::CivilDay(info.cs));
  const int64_t result =
      ProjectCivilField(info.cs, weekday, day_of_year, ts_cv.payload.ts.nanos,
                        static_cast<TzAccessorKind>(accessor_kind));
  CelValue out{};
  out.kind = CEL_INT;
  out.payload.i = result;
  ctx.mem.WriteCelValue(out_slot, out);
  return absl::OkStatus();
}

}  // namespace celwasm
