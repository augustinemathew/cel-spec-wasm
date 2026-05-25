// CEL `network_ext` extension kernels — self-hosted in cel_runtime.wasm.
//
// Public ABI for the `net.IP` surface of the network extension.  Like
// every other runtime helper, the kernels follow the out-slot
// convention: each parameter is a `uint32_t` byte offset into the
// shared linear memory naming a 24-byte CelValue, and the result lands
// in `out_slot` (the kernel returns void).  Suffix `_at_v` = out + 1
// value (2 params).
//
// `net.IP` (CelKind CEL_IP) and `net.CIDR` (CelKind CEL_CIDR) are
// first-class abstract types whose payload (`CelValue.payload.net_ref`)
// is the arena byte offset of a `NetIp` / `NetCidr` struct (see
// cel_data.h).  The parser normalizes IPv4 (both dotted-decimal and hex
// IPv4-mapped) to `family = 4` so the two forms compare equal; IPv6
// keeps all 16 bytes.
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

// ── net.IP classification predicates (receiver methods) ───────────
// Each takes the receiver CEL_IP in `ip_slot` and writes the result
// into `out_slot`.  ERROR / UNKNOWN operands are absorbed; a non-IP
// operand poisons with CEL_ERR_TYPE_MISMATCH.  Semantics mirror Go's
// `net/netip` `Addr` methods (cel-go wraps net/netip).

// `<ip>.family()` → CEL_INT: 4 (IPv4) or 6 (IPv6).
void cel_ip_family_at_v(uint32_t out_slot, uint32_t ip_slot);
// `<ip>.isLoopback()` → CEL_BOOL: v4 127.0.0.0/8 or v6 ::1.
void cel_ip_is_loopback_at_v(uint32_t out_slot, uint32_t ip_slot);
// `<ip>.isUnspecified()` → CEL_BOOL: 0.0.0.0 or ::.
void cel_ip_is_unspecified_at_v(uint32_t out_slot, uint32_t ip_slot);
// `<ip>.isGlobalUnicast()` → CEL_BOOL: Go's IsGlobalUnicast — true for
// any addr that is not unspecified, not loopback, not multicast, not
// link-local-unicast, and not interface-local-multicast.
void cel_ip_is_global_unicast_at_v(uint32_t out_slot, uint32_t ip_slot);
// `<ip>.isLinkLocalUnicast()` → CEL_BOOL: v4 169.254.0.0/16 or v6
// fe80::/10.
void cel_ip_is_link_local_unicast_at_v(uint32_t out_slot, uint32_t ip_slot);
// `<ip>.isLinkLocalMulticast()` → CEL_BOOL: v4 224.0.0.0/24 or v6
// ff02::/16.
void cel_ip_is_link_local_multicast_at_v(uint32_t out_slot, uint32_t ip_slot);

// ── net.CIDR type + operations ────────────────────────────────────

// `cidr(s)` — parse a CEL_STRING into a CEL_CIDR.  Splits on the LAST
// '/', parses the address via the net.IP parse path and the prefix as
// a decimal int (validated against the family's max).  The address is
// stored as given (not masked).  Parse failure / zone id / bad mask →
// CEL_ERR_INVALID_ARGUMENT; wrong operand kind → CEL_ERR_TYPE_MISMATCH.
void cel_cidr_parse_at_v(uint32_t out_slot, uint32_t str_slot);

// `string(cidrVal)` — canonical text "<addr>/<prefix>" into a fresh
// arena CEL_STRING.  Wrong operand kind → CEL_ERR_TYPE_MISMATCH.
void cel_cidr_to_string_at_v(uint32_t out_slot, uint32_t cidr_slot);

// `<cidr>.containsIP(arg)` — CEL_BOOL.  Handles BOTH overloads: `arg`
// may be a CEL_IP (used directly) or a CEL_STRING (parsed to an IP
// first; parse failure poisons CEL_ERR_INVALID_ARGUMENT).  A
// cross-family arg yields false (not an error); otherwise compares the
// first `prefix` bits of arg.addr against cidr.addr.
void cel_cidr_contains_ip_at_vv(uint32_t out_slot, uint32_t cidr_slot,
                                uint32_t arg_slot);

// `<cidr>.containsCIDR(arg)` — CEL_BOOL.  `arg` may be a CEL_CIDR or a
// CEL_STRING (parsed first).  A contains B iff same family AND
// A.prefix <= B.prefix AND the first A.prefix bits of B.addr match
// A.addr.
void cel_cidr_contains_cidr_at_vv(uint32_t out_slot, uint32_t cidr_slot,
                                  uint32_t arg_slot);

// `<cidr>.ip()` — the stored network address as a fresh CEL_IP.
void cel_cidr_ip_at_v(uint32_t out_slot, uint32_t cidr_slot);

// `<cidr>.masked()` — a fresh CEL_CIDR with all host bits beyond
// `prefix` zeroed.
void cel_cidr_masked_at_v(uint32_t out_slot, uint32_t cidr_slot);

// `<cidr>.prefixLength()` — CEL_INT: the mask length.
void cel_cidr_prefix_length_at_v(uint32_t out_slot, uint32_t cidr_slot);

// Structural equality of two CEL_CIDR values: same family, same
// prefix, and equal addr bytes.  Used by the runtime equality dispatch
// (cel_runtime.c) for the `==` codegen path and element-wise list/map
// equality.  Caller guarantees both operands are CEL_CIDR.
int net_cidr_eq(const CelValue* a, const CelValue* b);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_NET_EXT_H_
