// Probe: can we call absl::ParseTime from inside a wasm runtime
// module built with wasi-sdk?
//
// The exp1_re2 work already proved abseil-cpp cross-compiles under
// wasi-sdk (RE2 + full absl, 388 KB stripped).  This experiment
// narrows to the specific function we care about for Phase C of
// the WASI migration: absl::ParseTime for `timestamp(string)`.
//
// Build (after exp1_re2 set up the absl-install dir):
//
//   ./wasi-sdk/bin/clang++ \
//     --target=wasm32-wasi-threads -Oz -fno-rtti -pthread \
//     -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
//     -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID \
//     -I ../../../../wasm_compilation_experiments/exp1_re2/absl-install/include \
//     -Wl,--shared-memory -Wl,--max-memory=67108864 \
//     -Wl,--export=parse -Wl,--strip-all -Wl,--gc-sections \
//     -nostartfiles -Wl,--no-entry \
//     ../../../../wasm_compilation_experiments/exp1_re2/cxa_stubs.o \
//     exp_e_absl_parsetime.cc \
//     $(ls ../../../../wasm_compilation_experiments/exp1_re2/absl-install/lib/*.a | tr '\n' ' ') \
//     -lwasi-emulated-signal -lwasi-emulated-process-clocks \
//     -lwasi-emulated-mman -lwasi-emulated-getpid \
//     -o exp_e_absl_parsetime.wasm

#include <cstdint>
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

extern "C" {

// parse(buf, len, out_seconds, out_nanos) -> int32
//
//   buf, len      — pointer + length of an RFC3339 timestamp string
//                   in linear memory.
//   out_seconds   — i32 offset where the parsed Unix-seconds value
//                   gets stored (8 bytes, little-endian).
//   out_nanos     — i32 offset where the parsed nanoseconds-since-
//                   epoch fractional component gets stored.
//
// Returns 0 on success, -1 on parse failure.
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
