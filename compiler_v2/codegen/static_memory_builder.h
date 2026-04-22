#ifndef CELWASM_COMPILER_V2_CODEGEN_STATIC_MEMORY_BUILDER_H_
#define CELWASM_COMPILER_V2_CODEGEN_STATIC_MEMORY_BUILDER_H_

// Packs compile-time-known CelValues into a byte buffer destined for
// the wasm module's `.rodata` data segment.  LayoutPass calls
// `AppendScalar` / `AppendSpan` for every `kConst` literal in the AST;
// each returns the linear-memory offset at which the emitted CelValue
// header lives.  The result is move-returned via `Finalize() &&` and
// stamped into the data segment by codegen.
//
// CelValue is 24 bytes, 8-byte aligned.  Span payload bytes (the
// string / bytes body that the CelValue header's CelSpan points at)
// are emitted immediately after the header; the next append pads
// back to 8-byte alignment, keeping every CelValue header on an
// 8-byte boundary.
//
// No cap, no fallback, no `absl::Status` on the happy path: by design
// §6.2.1 every literal lands in rodata unconditionally.  AppendList
// and AppendMap are declared now so M5/M6 wiring lands as a data-only
// change; both return Unimplemented at M1.

#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/runtime/cel_runtime.h"

namespace celwasm {

// A compile-time scalar literal: kind + its matching union payload.
// `kind` must be one of CEL_NULL / CEL_BOOL / CEL_INT / CEL_UINT /
// CEL_DOUBLE — spans use `AppendSpan`, not `AppendScalar`.
struct CelScalarPayload {
  CelKind kind;
  union {
    int32_t b;
    int64_t i;
    uint64_t u;
    double d;
  } value;
};

class StaticMemoryBuilder {
 public:
  // `base_offset` is the linear-memory offset at which this rodata
  // segment will start.  Returned offsets are absolute linear-memory
  // offsets (`base_offset` + buffer-local offset) so callers can drop
  // them into CelSpan / LocalGet without an extra add.
  explicit StaticMemoryBuilder(uint32_t base_offset);

  // Emits a 24-byte CelValue header (8-byte aligned) and returns its
  // linear-memory offset.  Bytes after the payload inside the 24-byte
  // frame are zero-filled.  CHECKs if `p.kind` is not a scalar kind.
  uint32_t AppendScalar(const CelScalarPayload& p);

  // Emits a 24-byte CelValue header whose CelSpan points at the
  // payload bytes that follow immediately after the header, then
  // pads the next write-cursor back to 8-byte alignment.  Returns
  // the header's linear-memory offset.  `kind` must be CEL_STRING or
  // CEL_BYTES.
  uint32_t AppendSpan(CelKind kind, absl::string_view bytes);

  // Future work.  M1 stub returns Unimplemented so any codegen path
  // that tries to AppendList / AppendMap a literal surfaces as a
  // clean compile error, not a silent miscodegen.
  ABSL_MUST_USE_RESULT absl::StatusOr<uint32_t> AppendList(
      absl::Span<const uint32_t> element_offsets);
  ABSL_MUST_USE_RESULT absl::StatusOr<uint32_t> AppendMap(
      absl::Span<const uint32_t> key_offsets,
      absl::Span<const uint32_t> value_offsets);

  // Move-return the packed buffer.  After Finalize the builder is
  // consumed.  Use `size_bytes()` beforehand to learn the final size
  // for cel.abi / data-segment bookkeeping.
  std::vector<uint8_t> Finalize() &&;

  uint32_t size_bytes() const {
    return static_cast<uint32_t>(buf_.size());
  }
  uint32_t base_offset() const {
    return base_offset_;
  }

 private:
  std::vector<uint8_t> buf_;
  uint32_t base_offset_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_STATIC_MEMORY_BUILDER_H_
