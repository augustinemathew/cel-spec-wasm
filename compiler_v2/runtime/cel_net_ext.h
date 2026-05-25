// CEL `network_ext` extension kernels — self-hosted in cel_runtime.wasm.
//
// Public ABI for the `net.IP` surface of the network extension.  Like
// every other runtime helper, the kernels follow the out-slot
// convention: each parameter is a `uint32_t` byte offset into the
// shared linear memory naming a 24-byte CelValue, and the result lands
// in `out_slot` (the kernel returns void).  Suffix `_at_v` = out + 1
// value (2 params).
//
// `net.IP` (CelKind CEL_IP) is a first-class abstract type whose
// payload (`CelValue.payload.net_ref`) is the arena byte offset of a
// `NetIp` struct (see cel_data.h).  The parser normalizes IPv4 (both
// dotted-decimal and hex IPv4-mapped) to `family = 4` so the two forms
// compare equal; IPv6 keeps all 16 bytes.
//
// 3VL / error handling mirrors the other extension kernels: ERROR /
// UNKNOWN operands are absorbed verbatim into `out_slot`; a wrong-kind
// operand poisons with CEL_ERR_TYPE_MISMATCH; a string that fails to
// parse as an IP poisons with CEL_ERR_INVALID_ARGUMENT.  The
// conformance harness compares error *kind* only (runner.cc::
// CompareEvalError), so the corpus's rich message text is not
// reproduced — a numeric code is sufficient.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_NET_EXT_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_NET_EXT_H_

#include <stdint.h>

#include "compiler_v2/runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// `ip(s)` — parse a CEL_STRING into a CEL_IP.  Parse failure →
// CEL_ERR_INVALID_ARGUMENT; wrong operand kind → CEL_ERR_TYPE_MISMATCH.
void cel_ip_parse_at_v(uint32_t out_slot, uint32_t str_slot);

// `string(ipVal)` — canonical text of a CEL_IP into a fresh arena
// CEL_STRING.  v4 → dotted-decimal; v6 → lowercase hex with RFC-5952
// `::` compression.  Wrong operand kind → CEL_ERR_TYPE_MISMATCH.
void cel_ip_to_string_at_v(uint32_t out_slot, uint32_t ip_slot);

// `isIP(s)` — CEL_BOOL true iff the CEL_STRING parses as a valid IP.
// Does NOT error on an invalid string (returns false).  Wrong operand
// kind → CEL_ERR_TYPE_MISMATCH.
void cel_isip_at_v(uint32_t out_slot, uint32_t str_slot);

// `ip.isCanonical(s)` — CEL_BOOL.  Errors (CEL_ERR_INVALID_ARGUMENT) if
// the CEL_STRING doesn't parse at all; otherwise true iff the string is
// already in canonical form (lowercase hex, minimal `::` placement).
void cel_ip_is_canonical_at_v(uint32_t out_slot, uint32_t str_slot);

// Structural equality of two CEL_IP values: memcmp over
// {family, addr}.  Used by the runtime equality dispatch
// (cel_runtime.c) for both the `==` codegen path and element-wise
// list/map equality.  Caller guarantees both operands are CEL_IP.
int net_ip_eq(const CelValue* a, const CelValue* b);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_NET_EXT_H_
