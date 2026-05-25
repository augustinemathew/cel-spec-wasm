// Phase C: `cel_matches_at_vv` runtime kernel — RE2-backed regex
// PartialMatch with a per-Instance single-slot most-recent-pattern
// cache.  Replaces what would otherwise be a `cel_host.cel_matches`
// trampoline by self-hosting RE2 inside cel_runtime.wasm.
//
// ABI: `(out_slot, text_slot, pat_slot)` of linear-memory offsets —
// canonical `_at_vv` shape.  Implementation in `cel_matches.cc`
// (C++ TU; the runtime cc_binary links it alongside the C-only TUs
// and `cel_time_parse.cc`).  Cache state is module-static so each
// Instance gets its own copy at instantiation time.

#ifndef CELWASM_RUNTIME_CEL_MATCHES_H_
#define CELWASM_RUNTIME_CEL_MATCHES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns CEL_BOOL{true} iff `text` matches `pat` per RE2's
// PartialMatch (regex matches some substring of text).  Compile
// failure on `pat` poisons `out` with CEL_ERR_INVALID_ARGUMENT;
// kind-mismatch on either operand likewise.  3VL absorbs ERROR /
// UNKNOWN from either operand.
void cel_matches_at_vv(uint32_t out_slot, uint32_t text_slot,
                       uint32_t pat_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_MATCHES_H_
