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
//
// Format mini-language (matching the docstring in
// `runtime/cel_runtime.h`):
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

#ifndef CELWASM_EVAL_HOST_CEL_LOG_H_
#define CELWASM_EVAL_HOST_CEL_LOG_H_

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

  virtual void Emit(absl::string_view file, absl::string_view fn,
                    uint32_t line_no, absl::string_view message) = 0;
};

ABSL_MUST_USE_RESULT CelLogSink* absl_nonnull DefaultCelLogSink();

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

CelLogSink* absl_nullable SetCelLogSink(CelLogSink* absl_nullable sink);

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

void DecodeCelLog(absl::Span<const uint8_t> linear_memory,
                  const CelLogWireArgs& args, CelLogSink* absl_nonnull sink);

ABSL_MUST_USE_RESULT absl::Status RegisterCelLog(
    wasmtime_linker_t* absl_nonnull linker);

}  // namespace celwasm

#endif  // CELWASM_EVAL_HOST_CEL_LOG_H_
