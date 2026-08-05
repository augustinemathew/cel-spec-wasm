// The wasm binary-format layer: core-module preamble check, LEB128
// codec, and top-level custom-section read/write over raw bytes.
//
// This is the ONLY first-party code allowed to know wasm binary
// framing.  A magic constant or LEB decoder anywhere else is a
// review finding (doc/implementation-plan/rewrite/
// feature-pipeline-checklist.md §2.7).  Deliberately absl-only —
// no Binaryen, no wasmtime, no proto — so it sits below both
// `compiler/` and `eval/`.  The layering line: byte-level framing
// here; module semantics → Binaryen; runtime linking → wasmtime.

#ifndef CELWASM_ABI_WASM_BINARY_H_
#define CELWASM_ABI_WASM_BINARY_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

// Preamble check: true iff `bytes` starts with the 8-byte core-module
// preamble — `\0asm` magic + version word 0x00000001.  False on a
// truncated preamble, wrong magic, or any other version word (e.g. a
// Component-Model component's 0x0001000d).
bool IsCoreModule(absl::Span<const uint8_t> bytes);

// LEB128 (unsigned 32-bit) read/append — shared by walk and build.
//
// ReadLeb128U32 decodes at `*pos`, advancing it past the encoding.
// Returns false on truncated input or an encoding longer than five
// bytes; `*pos` is unspecified (mid-encoding) on failure.
bool ReadLeb128U32(absl::Span<const uint8_t> bytes, size_t* pos, uint32_t* out);
void AppendLeb128U32(std::vector<uint8_t>& out, uint32_t value);

// Finds the top-level custom section named `name` in a core module.
// Non-custom sections are skipped opaquely — the walk never descends
// into section payloads.  Returns:
//   - OK: zero-copy span into `wasm_bytes` (the payload after the
//     name field; valid only while the input outlives it).
//   - NotFound: no top-level custom section named `name`.
//   - InvalidArgument: not a core module, framing overrun (truncated
//     LEB / section size or name length past EOF), or more than one
//     top-level custom section named `name`.
ABSL_MUST_USE_RESULT absl::StatusOr<absl::Span<const uint8_t>>
FindCustomSection(absl::Span<const uint8_t> wasm_bytes, absl::string_view name);

// Returns a copy of `wasm_bytes` with a custom section appended at
// top level.  InvalidArgument on bad preamble or existing `name`.
ABSL_MUST_USE_RESULT absl::StatusOr<std::vector<uint8_t>> AppendCustomSection(
    absl::Span<const uint8_t> wasm_bytes, absl::string_view name,
    absl::Span<const uint8_t> payload);

// Test/build helper: frame {name, payload} as custom-section bytes
// (id 0x00, LEB128 section size, LEB128 name length, name, payload).
std::vector<uint8_t> BuildCustomSection(absl::string_view name,
                                        absl::Span<const uint8_t> payload);

}  // namespace celwasm

#endif  // CELWASM_ABI_WASM_BINARY_H_
