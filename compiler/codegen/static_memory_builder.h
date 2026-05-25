#ifndef CELWASM_COMPILER_V2_CODEGEN_STATIC_MEMORY_BUILDER_H_
#define CELWASM_COMPILER_V2_CODEGEN_STATIC_MEMORY_BUILDER_H_

// Packs compile-time-known CelValues into a byte buffer destined
// for the wasm module's `.rodata` data segment.  LayoutPass calls
// one `Allocate*` per `kConst` literal in the AST; each returns
// the frame's byte offset inside the compiled module's linear
// memory.  The buffer is move-returned via `Finalize() &&` and
// stamped into the data segment by codegen.
//
// Return type.  All `Allocate*` methods return `uint32_t` — an
// absolute wasm32 linear-memory byte offset (buffer-local offset
// plus `base_offset`), ready to drop into an `i32.const` or a
// CelSpan without further arithmetic.  Not a host C++ pointer;
// the value is only meaningful inside the emitted wasm module's
// memory.  If we ever target wasm64 this becomes `uint64_t`.
//
// Infallible.  Rodata packing has no failure mode by design
// (§6.2.1): no cap, no fallback, no runtime-initialised-literal
// variant — every literal lands in rodata and the buffer grows
// as needed.  Hence no `absl::StatusOr` on any Allocate return
// path.
//
// CelValue is 24 bytes, 8-byte aligned.  For string / bytes the
// payload bytes follow the 24-byte frame directly and the cursor
// is padded back to 8-byte alignment before the next Allocate,
// so every frame lands on an 8-byte boundary.
//
// Out of scope: messages and non-all-literal lists / maps.
// Their runtime value is a host-side handle (externref /
// arena pointer), not bytes — they're constructed at eval
// time via runtime / host calls (§4.7), not packed here.
// `AllocateList` / `AllocateMap` are reserved for the narrow
// all-literal optimization case (§12 open question 6) and are
// signature-final stubs; their M1 body `ABSL_CHECK(false)`s
// so any accidental early caller gets a loud crash, not a
// silent miscodegen.  M5/M6 fills the body without API churn.

#include <cstdint>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/runtime/cel_runtime.h"

namespace celwasm {

class StaticMemoryBuilder {
 public:
  // `base_offset` is the linear-memory offset at which this rodata
  // segment will start.  Returned offsets are absolute linear-memory
  // offsets (`base_offset` + buffer-local offset) so callers can
  // drop them into CelSpan / i32.const without an extra add.
  explicit StaticMemoryBuilder(uint32_t base_offset);

  // Allocate a 24-byte CelValue frame in rodata holding one
  // compile-time-known scalar and return its absolute linear-memory
  // offset.  Bytes within the frame beyond the scalar payload are
  // zero-filled so the whole frame is bit-for-bit deterministic.
  uint32_t AllocateNull();
  uint32_t AllocateBool(bool v);
  uint32_t AllocateInt(int64_t v);
  uint32_t AllocateUint(uint64_t v);
  uint32_t AllocateDouble(double v);

  // Allocate a 24-byte CelValue frame whose CelSpan points at the
  // payload bytes that follow immediately after the frame, then pad
  // the next write cursor back to 8-byte alignment.  Returns the
  // frame's absolute linear-memory offset.
  uint32_t AllocateString(absl::string_view s);
  uint32_t AllocateBytes(absl::string_view b);
  // Type-of-types value.  Same payload shape as a string (CelSpan
  // into rodata-resident bytes) but `kind = CEL_TYPE`.  Used by
  // `LayoutPass::ConstLayoutVisitor` when the kConstant's
  // checker-assigned `Repr` is `kType` — see
  // `rewrite/m9-type-subsystem.md`.
  uint32_t AllocateType(absl::string_view name);

  // Stubs.  Signature is final; body is `ABSL_CHECK(false)` so any
  // caller that reaches here crashes with the method name, not a
  // silent miscodegen.
  uint32_t AllocateList(absl::Span<const uint32_t> element_offsets);
  uint32_t AllocateMap(absl::Span<const uint32_t> key_offsets,
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
  // Writes the 8-byte (kind, pad) header prefix at the current
  // cursor and returns the local offset of the frame's start.
  // Caller must then write exactly 16 bytes of payload to complete
  // the 24-byte frame.  CHECKs the cursor is 8-byte aligned.
  uint32_t OpenFrame(CelKind kind);

  // Shared implementation for AllocateString / AllocateBytes.
  uint32_t AllocateSpan(CelKind kind, absl::string_view bytes);

  std::vector<uint8_t> buf_;
  uint32_t base_offset_ = 0;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_STATIC_MEMORY_BUILDER_H_
