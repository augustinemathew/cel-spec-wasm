// Probe E7 — link the Phase C kernels for which absl is the backing
// library, so bazel's cc_library transitive-dep resolution surfaces
// the minimum absl_* set needed.
//
//   timestamp(string) -> timestamp   uses absl::ParseTime
//   duration(string)  -> duration    uses absl::ParseDuration
//   timestamp.string() -> string     uses absl::FormatTime
//   duration.string()  -> string     uses absl::FormatDuration
//   strings.format(string, list)     uses absl::StrFormat
//
// Plus RE2 for matches() — but that's a separate http_archive (E8).

#include <cstdint>
#include <cstring>
#include <string>

#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

extern "C" {

// Stub arena to keep the link self-contained.
static char arena_buf[8192];
static int arena_cursor = 0;

__attribute__((export_name("arena_reset")))
void arena_reset(void) { arena_cursor = 0; }

__attribute__((export_name("arena_alloc")))
void* arena_alloc(int n) {
  arena_cursor = (arena_cursor + 7) & ~7;
  if (arena_cursor + n > 8192) return nullptr;
  void* p = &arena_buf[arena_cursor];
  arena_cursor += n;
  return p;
}

__attribute__((export_name("parse_timestamp")))
int32_t parse_timestamp(const char* buf, int32_t len,
                        int64_t* out_seconds, int32_t* out_nanos) {
  absl::Time t;
  std::string err;
  if (!absl::ParseTime(absl::RFC3339_full,
                       absl::string_view(buf, static_cast<size_t>(len)),
                       &t, &err)) {
    return -1;
  }
  const int64_t ns = absl::ToUnixNanos(t);
  *out_seconds = ns / 1000000000;
  *out_nanos = static_cast<int32_t>(ns % 1000000000);
  return 0;
}

__attribute__((export_name("parse_duration")))
int32_t parse_duration(const char* buf, int32_t len,
                       int64_t* out_seconds, int32_t* out_nanos) {
  absl::Duration d;
  if (!absl::ParseDuration(absl::string_view(buf, static_cast<size_t>(len)),
                            &d)) {
    return -1;
  }
  *out_seconds = absl::ToInt64Seconds(d);
  *out_nanos = static_cast<int32_t>(absl::ToInt64Nanoseconds(d) % 1000000000);
  return 0;
}

__attribute__((export_name("format_timestamp")))
int32_t format_timestamp(int64_t seconds, int32_t nanos,
                         char* out_buf, int32_t out_cap) {
  absl::Time t = absl::FromUnixNanos(seconds * 1000000000LL + nanos);
  std::string formatted = absl::FormatTime(absl::RFC3339_full, t,
                                           absl::UTCTimeZone());
  if (static_cast<int32_t>(formatted.size()) > out_cap) return -1;
  std::memcpy(out_buf, formatted.data(), formatted.size());
  return static_cast<int32_t>(formatted.size());
}

__attribute__((export_name("format_duration")))
int32_t format_duration(int64_t seconds, int32_t nanos,
                        char* out_buf, int32_t out_cap) {
  absl::Duration d = absl::Seconds(seconds) + absl::Nanoseconds(nanos);
  std::string formatted = absl::FormatDuration(d);
  if (static_cast<int32_t>(formatted.size()) > out_cap) return -1;
  std::memcpy(out_buf, formatted.data(), formatted.size());
  return static_cast<int32_t>(formatted.size());
}

__attribute__((export_name("strings_format_int")))
int32_t strings_format_int(const char* /*fmt*/, int32_t /*fmt_len*/,
                           int64_t value, char* out_buf, int32_t out_cap) {
  // For E7 we just need to pull absl::StrFormat machinery into the
  // link.  The real cel strings.format kernel takes a runtime format
  // string via FormatUntyped — for the probe a constexpr fmt is fine.
  std::string formatted = absl::StrFormat("%d", value);
  if (static_cast<int32_t>(formatted.size()) > out_cap) return -1;
  std::memcpy(out_buf, formatted.data(), formatted.size());
  return static_cast<int32_t>(formatted.size());
}

}  // extern "C"
