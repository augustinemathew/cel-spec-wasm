// M17 — `encoders` extension kernels: base64.encode / base64.decode.
//
// Both kernels are thin wrappers over absl's base64 codec
// (`absl/strings/escaping.h`), bridged to the runtime's slot-out ABI
// via the shared `cel_string_ext_internal.h` envelope helpers.  The
// only base64-specific piece is `WriteBytesFromBytes` (the CEL_BYTES
// analog of `WriteStringFromBytes`, which only produces CEL_STRING).
//
// Semantics track `third_party/cel-cpp/extensions/encoders.cc`
// line-for-line so `tests/simple/testdata/encoders_ext.textproto`
// matches byte-for-byte:
//   - Base64Encode → absl::Base64Escape  (standard alphabet, padded).
//   - Base64Decode → absl::Base64Unescape; on failure returns
//     InvalidArgumentError("invalid base64 data") (encoders.cc:51).
//     absl::Base64Unescape accepts missing padding, so the corpus's
//     unpadded `'aGVsbG8'` row decodes without a manual re-pad.

#include "compiler_v2/runtime/cel_base64_ext.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "compiler_v2/runtime/cel_string_ext_internal.h"

namespace {

using celwasm::string_ext_internal::Absorb3vlUnary;
using celwasm::string_ext_internal::BorrowSpan;
using celwasm::string_ext_internal::Poison;
using celwasm::string_ext_internal::WriteStringFromBytes;

// CEL_BYTES analog of `WriteStringFromBytes`: copies `len` bytes into
// the per-Eval arena and writes a CEL_BYTES CelValue naming them.
// Empty output uses the canonical `{ptr=0, len=0}` sentinel.  Returns
// false on arena OOM (caller already poisoned).
bool WriteBytesFromBytes(CelValue* out, const char* data, size_t len) {
  if (len == 0) {
    out->kind = CEL_BYTES;
    out->payload.bytes.ptr = 0;
    out->payload.bytes.len = 0;
    return true;
  }
  const uint32_t off = arena_alloc(static_cast<uint32_t>(len));
  if (off == 0) {
    Poison(out, CEL_ERR_OVERFLOW);
    return false;
  }
  std::memcpy(cel_mem_base() + off, data, len);
  out->kind = CEL_BYTES;
  out->payload.bytes.ptr = off;
  out->payload.bytes.len = static_cast<uint32_t>(len);
  return true;
}

}  // namespace

extern "C" void cel_base64_encode_at_v(uint32_t out_slot, uint32_t bytes_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(bytes_slot);
  if (Absorb3vlUnary(out, in)) return;
  if (in->kind != CEL_BYTES) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const absl::string_view src = BorrowSpan(in->payload.bytes);
  std::string encoded;
  absl::Base64Escape(src, &encoded);
  WriteStringFromBytes(out, encoded.data(), encoded.size());
}

extern "C" void cel_base64_decode_at_v(uint32_t out_slot, uint32_t str_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(str_slot);
  if (Absorb3vlUnary(out, in)) return;
  if (in->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const absl::string_view src = BorrowSpan(in->payload.s);
  std::string decoded;
  if (!absl::Base64Unescape(src, &decoded)) {
    // cel-cpp: InvalidArgumentError("invalid base64 data").
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  WriteBytesFromBytes(out, decoded.data(), decoded.size());
}
