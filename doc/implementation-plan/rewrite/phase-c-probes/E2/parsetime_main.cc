// Mirror of doc/implementation-plan/rewrite/wasi/experiments/exp_e_absl_parsetime.cc
// but built via a bazel genrule that drives wasi-sdk clang++.
//
// Probes whether bazel can produce a wasm artifact byte-identical (or
// nearly so) to the standalone CMake-built baseline (E1), using:
//   - @wasi_sdk//:clang  (already in MODULE.bazel)
//   - pre-built static archives under the dev's exp1_re2/absl-install/
//     dir (referenced via absolute path inside the genrule cmd, since
//     they live outside the bazel workspace and we're not committing
//     them to the repo for this probe).
//
// The interesting question for Path C: does bazel's sandbox + genrule
// flag-passing reach feature parity with the standalone build?

#include <cstdint>
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

extern "C" {

int32_t parse(const char* buf, int32_t len,
              int64_t* out_seconds, int32_t* out_nanos) {
  absl::Time t;
  std::string err;
  if (!absl::ParseTime(absl::RFC3339_full,
                       absl::string_view(buf, static_cast<size_t>(len)),
                       &t, &err)) {
    return -1;
  }
  const int64_t ns_since_epoch = absl::ToUnixNanos(t);
  *out_seconds = ns_since_epoch / 1000000000;
  *out_nanos = static_cast<int32_t>(ns_since_epoch % 1000000000);
  return 0;
}

}  // extern "C"
