#include "runtime/cel_time_canonical.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

namespace celwasm {

std::string FormatTimestampRfc3339(int64_t seconds, int32_t nanos) {
  const absl::Time t =
      absl::UnixEpoch() + absl::Seconds(seconds) + absl::Nanoseconds(nanos);
  std::string s = absl::FormatTime(absl::RFC3339_full, t, absl::UTCTimeZone());
  // absl emits `+00:00`; cel-cpp / proto Timestamp want trailing `Z`.
  constexpr absl::string_view kUtcOffset = "+00:00";
  if (s.size() > kUtcOffset.size() && absl::EndsWith(s, kUtcOffset)) {
    s.resize(s.size() - kUtcOffset.size());
    s.push_back('Z');
  }
  return s;
}

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

}  // namespace celwasm
