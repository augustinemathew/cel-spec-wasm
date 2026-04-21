// Host-side implementation of the `cel_env.cel_log` import emitted by
// the runtime module and (declaratively) by the eval module.  Splits
// into three layers:
//
//   - `CelLogSink` — pluggable consumer of formatted log lines.  The
//     default writes to stderr; tests install a capture sink that
//     accumulates every line in a `std::vector<std::string>`.
//   - `DecodeCelLog` — pure function that parses the 9-arg wire ABI
//     against a linear-memory byte span and emits one formatted line
//     per call.  Runtime-agnostic: no wasmtime types in the signature.
//   - `RegisterCelLog` — wasmtime glue that wires the trampoline onto
//     a `wasmtime_linker_t` under the `cel_env` module namespace.
//     Pairs with `CelHostEnv::Register` in `cel_host_wasmtime.{h,cc}`.
//
// The format mini-language (matching the docstring in
// `compiler/runtime/cel_runtime.h`):
//
//   %s   string span           (u32 ptr, u32 len) packed into payload
//   %d   signed i64
//   %u   unsigned u64
//   %f   f64
//   %b   bool i32              prints "true" / "false"
//   %v   CelValue offset u32   pretty-printed kind + payload
//   %%   literal percent
//
// Any other byte (including a bare `%x`) is emitted verbatim — logging
// must never trap.

#ifndef CELWASM_COMPILER_HOST_CEL_LOG_H_
#define CELWASM_COMPILER_HOST_CEL_LOG_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "wasmtime.h"

namespace celwasm {

// Abstract sink for formatted log lines.  Thread-unsafe by design —
// the wasm runtime is single-threaded per store and the sink's Emit
// runs on the wasm call stack.
class CelLogSink {
 public:
  virtual ~CelLogSink() = default;

  // Called once per `cel_log(…)` invocation.  `line` is the
  // fully-formatted message body (no trailing newline); the
  // call-site metadata in `file`, `fn`, `line_no` lets a prefix-
  // printing sink stamp `[file:line fn] message\n`.
  virtual void Emit(absl::string_view file, absl::string_view fn,
                    uint32_t line_no, absl::string_view message) = 0;
};

// Default sink: writes `[file:line fn] message\n` to stderr on every
// call.  Shared singleton — no per-instance state.  Global, so
// embedders that don't install a sink still see log output.
ABSL_MUST_USE_RESULT CelLogSink* absl_nonnull DefaultCelLogSink();

// Sink that accumulates every emitted line in memory.  Each Emit
// pushes `"[file:line fn] message"` (no trailing newline) onto
// `lines()`.  Intended for tests.  Not thread-safe.
class CapturingCelLogSink : public CelLogSink {
 public:
  void Emit(absl::string_view file, absl::string_view fn, uint32_t line_no,
            absl::string_view message) override;

  const std::vector<std::string>& lines() const {
    return lines_;
  }
  void Clear() {
    lines_.clear();
  }

 private:
  std::vector<std::string> lines_;
};

// Install `sink` as the process-wide consumer of cel_log output.
// Passing nullptr restores the default stderr sink.  Ownership: the
// sink must outlive every subsequent `DecodeCelLog` / registered
// trampoline that reads it.  Returns the previously installed sink so
// test fixtures can restore the prior state in their tear-down.
CelLogSink* absl_nullable SetCelLogSink(CelLogSink* absl_nullable sink);

// Decoder input: a byte span standing in for the wasm module's linear
// memory, plus the nine i32 arguments the import carries.  Runtime-
// agnostic so unit tests can exercise every format directive without
// spinning up wasmtime.
//
// All `*_ptr` values are byte offsets into `linear_memory`; `argv_ptr`
// points at `argc` contiguous 16-byte slots laid out as two
// consecutive `uint64_t` words — the shape the `CEL_LOG_*` call-site
// macros in `cel_runtime.h` expand to.  The first word's low 32 bits
// carry the `CEL_LOG_TAG_*` discriminator (high 32 reserved); the
// second word is the per-tag payload (packed u32 ptr+len for `%s`,
// bit-cast `double` for `%f`, a CelValue arena offset for `%v`,
// etc.).  `%v` reads CelValue offsets from the same memory span;
// out-of-range offsets print as `<oob>` rather than trapping.
struct CelLogWireArgs {
  uint32_t file_ptr = 0;
  uint32_t file_len = 0;
  uint32_t fn_ptr = 0;
  uint32_t fn_len = 0;
  uint32_t line = 0;
  uint32_t fmt_ptr = 0;
  uint32_t fmt_len = 0;
  uint32_t argv_ptr = 0;
  uint32_t argc = 0;
};

// Decodes one cel_log call and forwards the formatted line to `sink`.
// Graceful on every ill-formed input: OOB pointers emit `<oob>`,
// unknown directives copy the `%` and the following byte verbatim,
// argc mismatches print the remainder of the format as a literal.
// Never fails, never traps.
void DecodeCelLog(absl::Span<const uint8_t> linear_memory,
                  const CelLogWireArgs& args, CelLogSink* absl_nonnull sink);

// Registers the `cel_env.cel_log` trampoline on `linker`.  Must be
// called before `wasmtime_linker_instantiate` for any module that
// imports `cel_log`.  The trampoline reads `caller`'s memory export
// (named "memory") and decodes against it; a missing memory export
// causes the trampoline to silently no-op — logging is diagnostic,
// not load-bearing.  `sink` may be null, in which case the
// process-wide default (set via SetCelLogSink) is used at call time.
ABSL_MUST_USE_RESULT absl::Status RegisterCelLog(
    wasmtime_linker_t* absl_nonnull linker);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_HOST_CEL_LOG_H_
