// Shared benchmark fixture helpers for `compiler/bench/...`.
//
// Mirrors the pipeline driver used by `compiler/e2e/eval_test.cc`, but
// shaped for Google Benchmark: the compile step (ParseAndCheck + Lower
// + Serialize + LoadEval) is factored out so the caller can hoist it
// outside the measurement loop.  The eval-path benchmarks own a
// `PrecompiledEval` per-fixture and only time `CallEval`; the compile-
// path benchmarks time `Precompile` itself.

#ifndef CELWASM_COMPILER_BENCH_BENCH_FIXTURE_H_
#define CELWASM_COMPILER_BENCH_BENCH_FIXTURE_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/host/host_loader.h"
#include "google/protobuf/message.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm::bench {

// Wraps a compiled + instantiated `eval` module alongside any args the
// caller plans to pass on every invocation.  The externref args in
// `args` reference messages owned outside this struct (the caller keeps
// the messages alive for the lifetime of the fixture).
struct PrecompiledEval {
  LoadedEval loaded;
  std::vector<wasmtime_val_t> args;
};

// Runs ParseAndCheck + Lower + Serialize + LoadEval for `cel_source`
// under `variable_specs`.  Does NOT call eval.  This is the unit of
// work the compile-path benchmarks time.
absl::StatusOr<LoadedEval> Precompile(absl::string_view cel_source,
                                      std::vector<std::string> variable_specs);

// Decoded-string payload extracted from the runtime's linear memory by
// following a CelValue offset returned from `eval`.
struct DecodedString {
  uint32_t kind = 0;
  std::string payload;
};

// Reads a CelValue at the runtime-relative offset `offset`, asserting
// the slot lives inside the runtime's g_memory region.  Same code as
// `eval_test.cc`'s `DecodeStringAt`; lifted here so string benchmarks
// can assert that the emitted result is a well-formed string (and not
// e.g. silently a CEL_ERROR CelValue).
absl::StatusOr<DecodedString> DecodeStringAt(LoadedEval& loaded,
                                             int32_t offset);

// Wraps `msg` in a fresh externref backed by `loaded`'s wasmtime store.
// The caller owns `msg` and must keep it alive for every CallEval that
// consumes the returned `wasmtime_val_t`.
wasmtime_val_t MessageAsExternref(LoadedEval& loaded,
                                  google::protobuf::Message& msg);

// Small typed constructors mirroring those in `eval_test.cc`.  Kept
// here so bench files don't need to repeat the initializer shape.
inline wasmtime_val_t I64(int64_t v) {
  return wasmtime_val_t{WASMTIME_I64, {.i64 = v}};
}
inline wasmtime_val_t I32(int32_t v) {
  return wasmtime_val_t{WASMTIME_I32, {.i32 = v}};
}
inline wasmtime_val_t F64(double v) {
  return wasmtime_val_t{WASMTIME_F64, {.f64 = v}};
}

}  // namespace celwasm::bench

#endif  // CELWASM_COMPILER_BENCH_BENCH_FIXTURE_H_
