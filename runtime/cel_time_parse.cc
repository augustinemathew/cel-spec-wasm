#include "runtime/cel_time_parse.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_memory.h"

namespace {

// langdef-pinned bounds (see `rewrite/m7b-duration-timestamp.md` §3.2,
// mirrored in cel_time.c).  0001-01-01T00:00:00Z .. 9999-12-31T23:59:59Z.
constexpr int64_t kTimestampMinSeconds = -62135596800LL;
constexpr int64_t kTimestampMaxSeconds = 253402300799LL;

// proto Duration JSON envelope — `EncodeDurationToJson` rejects
// outside ±315B seconds.
constexpr int64_t kDurationMaxSeconds = 315576000000LL;

// CEL_ERROR stamp.
inline void Poison(CelValue* out, uint32_t err_code) {
  out->kind = CEL_ERROR;
  out->payload.err = err_code;
}

// 3VL passthrough: ERROR / UNKNOWN.
inline bool Absorb3vl(CelValue* out, const CelValue* in) {
  if (in->kind == CEL_ERROR || in->kind == CEL_UNKNOWN) {
    *out = *in;
    return true;
  }
  return false;
}

// Borrow input bytes from linear memory as a string_view.  Span offsets
// are relative to `cel_mem_base()` — that resolves to 0 on wasm
// (linear-memory base) and to the real backing-buffer base on the
// native build.  Treating the offset directly as a pointer only works
// on wasm and silently segfaults on host tests.
inline absl::string_view BorrowSpan(const CelSpan& s) {
  return absl::string_view(
      reinterpret_cast<const char*>(cel_mem_base()) + s.ptr, s.len);
}

// Decompose an absl::Duration into the proto-style (seconds, nanos)
// pair the CelDurTs payload holds.  Mirrors `DecomposeAbslDuration`
// in `eval/internal/cel_host.h`.
void DecomposeAbslDuration(absl::Duration d, CelDurTs* out) {
  out->seconds = absl::IDivDuration(d, absl::Seconds(1), &d);
  out->nanos =
      static_cast<int32_t>(absl::IDivDuration(d, absl::Nanoseconds(1), &d));
  out->_pad = 0;
}

// Post-validation: absl is laxer than CEL on lowercase `z`,
// leap-second `:60`, and two-digit year inputs.  Called only after
// `absl::ParseTime` succeeds.  See `rewrite/phase-c-plan.md` §4.1
// "spec-strict timestamp validation".
bool RejectsAsTimestampPerCEL(absl::string_view input) {
  if (!input.empty() && input.back() == 'z') return true;
  if (input.find(":60") != absl::string_view::npos) return true;
  const size_t dash = input.find('-');
  return dash == absl::string_view::npos || dash < 4;
}

// Unit ranks for `RejectsAsDurationPerCEL`: h > m > s > ms > us > ns.
int UnitRank(char first, char second_or_zero) {
  if (first == 'h') return 5;
  if (first == 'm' && second_or_zero == 0) return 4;
  if (first == 's' && second_or_zero == 0) return 3;
  if (first == 'm' && second_or_zero == 's') return 2;
  if (first == 'u' && second_or_zero == 's') return 1;
  if (first == 'n' && second_or_zero == 's') return 0;
  return -1;
}

// Post-validation: absl admits `1s2h` (as 2h1s); CEL rejects.
// Asserts unit order is strictly decreasing.
bool RejectsAsDurationPerCEL(absl::string_view input) {
  int prev_rank = INT32_MAX;
  for (size_t i = 0; i < input.size();) {
    const char c = input[i];
    if (c == '-' || (c >= '0' && c <= '9') || c == '.') {
      ++i;
      continue;
    }
    const char next = i + 1 < input.size() ? input[i + 1] : 0;
    const int rank = UnitRank(c, next == 's' ? 's' : 0);
    if (rank < 0) return true;
    if (rank >= prev_rank) return true;
    prev_rank = rank;
    i += (rank == 0 || rank == 1 || rank == 2) ? 2 : 1;
  }
  return false;
}

// Allocate `len` bytes in the per-Eval arena and copy `s` in.
// Returns 0 on OOM, else the offset relative to `cel_mem_base()`.
// Zero-length strings still allocate a valid (offset, 0) span.
uint32_t ArenaCopyString(absl::string_view s, uint32_t* out_len) {
  *out_len = static_cast<uint32_t>(s.size());
  uint32_t off = arena_alloc(static_cast<uint32_t>(s.size()));
  if (off == 0 && !s.empty()) return 0;
  if (!s.empty()) {
    std::memcpy(cel_mem_base() + off, s.data(), s.size());
  }
  return off;
}

// proto Duration JSON encoding: `[-]<seconds>[.<frac>]s` where the
// fractional part is 3, 6 or 9 digits (multiple of 3, trailing zero
// triples trimmed).  Mirrors `FormatProtoDuration` in cel_host.cc.
std::string FormatProtoDuration(int64_t seconds, int32_t nanos) {
  std::string out;
  const bool negative = seconds < 0 || nanos < 0;
  if (negative) out.push_back('-');
  const uint64_t abs_s = seconds < 0 ? static_cast<uint64_t>(-(seconds + 1)) + 1
                                     : static_cast<uint64_t>(seconds);
  const uint32_t abs_n =
      nanos < 0 ? static_cast<uint32_t>(-nanos) : static_cast<uint32_t>(nanos);
  absl::StrAppend(&out, abs_s);
  if (abs_n != 0) {
    out.push_back('.');
    char buf[10];
    std::snprintf(buf, sizeof(buf), "%09u", abs_n);
    size_t frac_len = 9;
    while (frac_len > 3 && buf[frac_len - 1] == '0' &&
           buf[frac_len - 2] == '0' && buf[frac_len - 3] == '0') {
      frac_len -= 3;
    }
    out.append(buf, frac_len);
  }
  out.push_back('s');
  return out;
}

// Write a CEL_STRING that points at the just-allocated arena bytes.
// `s` has already been copied via `ArenaCopyString` (caller passes
// the resulting offset + len here).
inline void WriteStringResult(CelValue* out, uint32_t offset, uint32_t len) {
  out->kind = CEL_STRING;
  out->payload.s.ptr = offset;
  out->payload.s.len = len;
}

}  // namespace

extern "C" void cel_timestamp_parse_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(in_slot);
  if (Absorb3vl(out, in)) return;
  if (in->kind != CEL_STRING) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  const absl::string_view s = BorrowSpan(in->payload.s);
  absl::Time t;
  std::string err;
  if (!absl::ParseTime(absl::RFC3339_full, s, &t, &err) ||
      RejectsAsTimestampPerCEL(s)) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  CelDurTs payload{};
  DecomposeAbslDuration(t - absl::UnixEpoch(), &payload);
  // Langdef range check + boundary refinement (sign-correlated):
  // at MIN with negative nanos and at MAX with positive nanos
  // overflow.  Mirrors `payload_in_range` in cel_time.c.
  if (payload.seconds < kTimestampMinSeconds ||
      payload.seconds > kTimestampMaxSeconds ||
      (payload.seconds == kTimestampMaxSeconds && payload.nanos > 0) ||
      (payload.seconds == kTimestampMinSeconds && payload.nanos < 0)) {
    Poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = CEL_TIMESTAMP;
  out->payload.ts = payload;
}

extern "C" void cel_duration_parse_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(in_slot);
  if (Absorb3vl(out, in)) return;
  if (in->kind != CEL_STRING) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  const absl::string_view s = BorrowSpan(in->payload.s);
  absl::Duration d;
  if (!absl::ParseDuration(s, &d) || RejectsAsDurationPerCEL(s)) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  CelDurTs payload{};
  DecomposeAbslDuration(d, &payload);
  if (payload.seconds < -kDurationMaxSeconds ||
      payload.seconds > kDurationMaxSeconds) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  out->kind = CEL_DURATION;
  out->payload.dur = payload;
}

extern "C" void cel_timestamp_format_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(in_slot);
  if (Absorb3vl(out, in)) return;
  if (in->kind != CEL_TIMESTAMP) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  const absl::Time t = absl::UnixEpoch() +
                       absl::Seconds(in->payload.ts.seconds) +
                       absl::Nanoseconds(in->payload.ts.nanos);
  std::string s = absl::FormatTime(absl::RFC3339_full, t, absl::UTCTimeZone());
  // absl emits `+00:00`; cel-cpp / proto Timestamp want trailing `Z`.
  constexpr absl::string_view kUtcOffset = "+00:00";
  if (s.size() > kUtcOffset.size() && absl::EndsWith(s, kUtcOffset)) {
    s.resize(s.size() - kUtcOffset.size());
    s.push_back('Z');
  }
  uint32_t len = 0;
  const uint32_t off = ArenaCopyString(s, &len);
  if (off == 0 && !s.empty()) {
    Poison(out, CEL_ERR_OVERFLOW);  // arena OOM surfaces as overflow.
    return;
  }
  WriteStringResult(out, off, len);
}

extern "C" void cel_duration_format_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(in_slot);
  if (Absorb3vl(out, in)) return;
  if (in->kind != CEL_DURATION) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  const std::string s =
      FormatProtoDuration(in->payload.dur.seconds, in->payload.dur.nanos);
  uint32_t len = 0;
  const uint32_t off = ArenaCopyString(s, &len);
  if (off == 0 && !s.empty()) {
    Poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  WriteStringResult(out, off, len);
}
