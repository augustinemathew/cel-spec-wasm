// M17 — `encoders` extension runtime kernels (self-hosted in
// `cel_runtime.wasm`).  Public ABI for the two cel-cpp `encoders`
// extension functions: `base64.encode(bytes) -> string` and
// `base64.decode(string) -> bytes`.
//
// Both kernels follow the canonical unary slot-out ABI used
// throughout the runtime: `(out_slot, arg_slot) -> void`, where each
// slot is a u32 byte offset into the shared linear memory.  3VL
// absorb (ERROR / UNKNOWN) and kind-mismatch envelopes match the
// string_ext kernels (`cel_string_ext.h`) — they reuse the same
// `cel_string_ext_internal.h` helpers.
//
// **Output allocation.**  Both kernels arena_alloc their output
// bytes; the resulting CelValue's payload span points into the
// per-Eval arena and is valid until the next `arena_reset`.  Empty
// output is encoded as the canonical `{ptr=0, len=0}` sentinel.
//
// **Semantics — cel-cpp parity.**  Mirrors
// `third_party/cel-cpp/extensions/encoders.cc` verbatim (confirmed
// 2026-05-24, see `rewrite/m17-encoders-ext.md` §2):
//   - encode → `absl::Base64Escape` (standard RFC 4648 alphabet,
//     WITH padding).
//   - decode → `absl::Base64Unescape` (standard alphabet, padding
//     optional — unpadded input like `'aGVsbG8'` decodes fine);
//     invalid input → CEL_ERROR(CEL_ERR_INVALID_ARGUMENT), matching
//     cel-cpp's "invalid base64 data".
//
// See `doc/implementation-plan/rewrite/m17-encoders-ext.md` for the
// test matrix and slice plan, and `wat/m17_base64_{encode,decode}.wat`
// for the locked call shapes.

#ifndef CELWASM_RUNTIME_CEL_BASE64_EXT_H_
#define CELWASM_RUNTIME_CEL_BASE64_EXT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// `base64.encode(b)`.  Reads `bytes_slot` as CEL_BYTES, base64-encodes
// it (standard alphabet, padded), and writes a CEL_STRING result whose
// payload span names freshly arena-allocated ASCII bytes.  Wrong kind
// (non-bytes) → CEL_ERROR/CEL_ERR_TYPE_MISMATCH.  3VL absorbs ERROR /
// UNKNOWN.  Arena OOM → CEL_ERROR/CEL_ERR_OVERFLOW.  Output is always
// ASCII text, hence CEL_STRING regardless of the input bytes.
// cel:codegen-export
void cel_base64_encode_at_v(uint32_t out_slot, uint32_t bytes_slot);

// `base64.decode(s)`.  Reads `str_slot` as CEL_STRING, base64-decodes
// it (standard alphabet, padding optional), and writes a CEL_BYTES
// result whose payload span names freshly arena-allocated bytes (may
// be non-UTF-8).  Wrong kind (non-string) →
// CEL_ERROR/CEL_ERR_TYPE_MISMATCH.  Invalid base64 →
// CEL_ERROR/CEL_ERR_INVALID_ARGUMENT (cel-cpp "invalid base64 data").
// 3VL absorbs ERROR / UNKNOWN.  Arena OOM → CEL_ERROR/CEL_ERR_OVERFLOW.
// cel:codegen-export
void cel_base64_decode_at_v(uint32_t out_slot, uint32_t str_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_BASE64_EXT_H_
