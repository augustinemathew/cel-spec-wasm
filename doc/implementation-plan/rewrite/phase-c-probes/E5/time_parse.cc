// Probe E5 — C++ TU that calls absl::ParseTime and writes the result
// into the arena allocated by the C TU.  Demonstrates that
// arena-from-C + libc++ + abseil all link together under bazel's
// wasm32-wasi-threads toolchain.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

extern "C" {

// From runtime.c.
void* arena_alloc(size_t n);

// parse_timestamp(buf, len, out_seconds, out_nanos) -> int32
// 0 on success, -1 on parse failure.
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
  // Touch the arena to prove C + C++ linkage.  Write the iso string
  // for the timestamp; the wat driver doesn't read it but the link
  // pulls in arena_alloc.
  void* scratch = arena_alloc(32);
  if (scratch) {
    std::memset(scratch, 0, 32);
  }
  const int64_t ns_since_epoch = absl::ToUnixNanos(t);
  *out_seconds = ns_since_epoch / 1000000000;
  *out_nanos = static_cast<int32_t>(ns_since_epoch % 1000000000);
  return 0;
}

}  // extern "C"
