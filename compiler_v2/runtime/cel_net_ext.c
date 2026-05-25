// CEL `network_ext` extension kernels — self-hosted in cel_runtime.wasm.
//
// `net.IP` surface: parse / validate / canonical / to-string + the
// structural-equality helper the runtime equality dispatch calls.
// Semantics are pinned to cel-go's `ext` network extension and the
// conformance fixture `tests/simple/testdata/network_ext.textproto`.
// See `doc/implementation-plan/rewrite/m18-network-ext.md`.
//
// The IPv4 + IPv6 string<->bytes conversion is delegated to the C
// standard library's `inet_pton` / `inet_ntop` (musl-derived in
// wasi-libc, native on the host); this file owns only the thin
// CEL-policy wrapper around them (operand copying, zone-id /
// dotted-v4-mapped rejection, v4-mapped folding, 3VL/poison plumbing).
// Everything operates on the shared linear memory via cel_internal.h
// helpers.

#include "compiler_v2/runtime/cel_net_ext.h"

#include <arpa/inet.h>
#include <stdint.h>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_internal.h"
#include "compiler_v2/runtime/cel_make.h"

// Longest IP literal we accept: a full uncompressed IPv6 address
// ("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff") is 39 bytes.  Anything
// longer cannot be a valid IP, so a fixed stack buffer with room for a
// NUL terminator suffices.
enum { kNetIpMaxText = 39 };

// ════════════════════════════════════════════════════════════════
// Parser primitives.  Inputs are (ptr, len) spans into linear memory;
// the parser fills a `NetIp` (family + 16 bytes) and never allocates.
// ════════════════════════════════════════════════════════════════

// Does buf[off..off+len) contain byte `c`?
static int span_has(const uint8_t* buf, uint32_t off, uint32_t len, uint8_t c) {
  for (uint32_t k = off; k < off + len; ++k) {
    if (buf[k] == c) return 1;
  }
  return 0;
}

// Detect the IPv4-mapped IPv6 form (::ffff:a.b.c.d) in raw bytes: the
// first ten bytes are zero and bytes 10,11 are 0xff.  cel-go folds this
// to a family=4 address so `::ffff:c0a8:1 == 192.168.0.1`.
static int is_v4_mapped(const uint8_t* b16) {
  for (int k = 0; k < 10; ++k) {
    if (b16[k] != 0) return 0;
  }
  return b16[10] == 0xff && b16[11] == 0xff;
}

// True iff a dotted-decimal IPv4 octet has a leading zero ("01", "00",
// "012").  cel-go rejects these even though some platform inet_pton
// (macOS) accepts them, so the wrapper screens for them: a '0' that
// starts an octet (preceded by start-of-string or '.') and is followed
// by another digit.
static int has_leading_zero_octet(const uint8_t* buf, uint32_t off,
                                  uint32_t len) {
  for (uint32_t k = off; k < off + len; ++k) {
    int at_octet_start = (k == off) || (buf[k - 1] == '.');
    if (at_octet_start && buf[k] == '0' && k + 1 < off + len &&
        buf[k + 1] >= '0' && buf[k + 1] <= '9') {
      return 1;
    }
  }
  return 0;
}

// Fill *ip as a family=4 address from 4 octets (addr zeroed, low 4 set).
static void set_ipv4(NetIp* ip, const uint8_t* v4) {
  ip->family = 4;
  for (int k = 0; k < 16; ++k) {
    ip->addr[k] = 0;
  }
  for (int k = 0; k < 4; ++k) {
    ip->addr[k] = v4[k];
  }
}

// Try the dotted-decimal IPv4 path on NUL-terminated `s` (spanning
// buf[off..off+len)).  Returns 1 and fills *ip on success; returns -1 if
// the input isn't IPv4 at all (caller falls through to IPv6); returns 0
// on a CEL-policy rejection (leading-zero octet).
static int try_parse_v4(const char* s, const uint8_t* buf, uint32_t off,
                        uint32_t len, NetIp* ip) {
  uint8_t v4[4];
  if (inet_pton(AF_INET, s, v4) != 1) return -1;
  if (has_leading_zero_octet(buf, off, len)) return 0;
  set_ipv4(ip, v4);
  return 1;
}

// Try the IPv6 path on NUL-terminated `s` (spanning buf[off..off+len)).
// Returns 1 and fills *ip on success, 0 on malformed input or a
// CEL-policy rejection (dotted-decimal v4-mapped form).
static int try_parse_v6(const char* s, const uint8_t* buf, uint32_t off,
                        uint32_t len, NetIp* ip) {
  if (!span_has(buf, off, len, ':')) return 0;  // not v6 either
  // Dotted-decimal v4-mapped (`::ffff:192.168.0.1`) is rejected by
  // cel-go even though the hex form is accepted; only the hex form is
  // in the corpus.
  if (span_has(buf, off, len, '.')) return 0;
  uint8_t b16[16];
  if (inet_pton(AF_INET6, s, b16) != 1) return 0;
  if (is_v4_mapped(b16)) {
    set_ipv4(ip, b16 + 12);
    return 1;
  }
  ip->family = 6;
  for (int k = 0; k < 16; ++k) {
    ip->addr[k] = b16[k];
  }
  return 1;
}

// Parse buf[off..off+len) into *ip via inet_pton.  Returns 1 on success,
// 0 on any malformed input or on a CEL-policy rejection (zone id,
// dotted-decimal v4-mapped, over-long literal).
static int parse_address(const uint8_t* buf, uint32_t off, uint32_t len,
                         NetIp* ip) {
  if (len == 0 || len > kNetIpMaxText) return 0;
  if (span_has(buf, off, len, '%')) return 0;  // zone id not allowed
  // inet_pton needs a NUL-terminated C string.
  char s[kNetIpMaxText + 1];
  for (uint32_t k = 0; k < len; ++k) {
    s[k] = (char)buf[off + k];
  }
  s[len] = '\0';
  int v4 = try_parse_v4(s, buf, off, len, ip);
  if (v4 >= 0) return v4;  // matched IPv4 syntax (success or policy reject)
  return try_parse_v6(s, buf, off, len, ip);
}

// Render *ip into dst (must hold kNetIpMaxText+1 bytes) via inet_ntop;
// returns the text length (inet_ntop yields canonical RFC-5952 form).
static uint32_t render_address(const NetIp* ip, char* dst) {
  if (ip->family == 4) {
    inet_ntop(AF_INET, ip->addr, dst, kNetIpMaxText + 1);
  } else {
    inet_ntop(AF_INET6, ip->addr, dst, kNetIpMaxText + 1);
  }
  uint32_t n = 0;
  while (dst[n] != '\0') {
    ++n;
  }
  return n;
}

// ════════════════════════════════════════════════════════════════
// Equality (called by the runtime equality dispatch).
// ════════════════════════════════════════════════════════════════

// NOLINTNEXTLINE(misc-use-internal-linkage)
int net_ip_eq(const CelValue* a, const CelValue* b) {
  const NetIp* ia = (const NetIp*)(cel_memory_base_() + a->payload.net_ref);
  const NetIp* ib = (const NetIp*)(cel_memory_base_() + b->payload.net_ref);
  if (ia->family != ib->family) return 0;
  for (int k = 0; k < 16; ++k) {
    if (ia->addr[k] != ib->addr[k]) return 0;
  }
  return 1;
}

// ════════════════════════════════════════════════════════════════
// Kernels.
// ════════════════════════════════════════════════════════════════

void cel_ip_parse_at_v(uint32_t out_slot, uint32_t str_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* str = cel_value_at(str_slot);
  if (absorb_3vl_unary(out, str)) return;
  if (str->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  NetIp ip;
  const uint8_t* base = cel_memory_base_();
  if (!parse_address(base, str->payload.s.ptr, str->payload.s.len, &ip)) {
    poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  uint32_t off = arena_alloc((uint32_t)sizeof(NetIp));
  if (off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  NetIp* dst = (NetIp*)(cel_memory_base_() + off);
  dst->family = ip.family;
  for (int k = 0; k < 16; ++k) {
    dst->addr[k] = ip.addr[k];
  }
  out->kind = CEL_IP;
  out->_pad = 0;
  out->payload.net_ref = off;
}

void cel_ip_to_string_at_v(uint32_t out_slot, uint32_t ip_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(ip_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind != CEL_IP) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const NetIp* ip = (const NetIp*)(cel_memory_base_() + in->payload.net_ref);
  char buf[kNetIpMaxText + 1];
  uint32_t n = render_address(ip, buf);
  uint32_t s = cel_make_string(buf, n);
  if (s == 0 && n > 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  *out = *cel_value_at(s);
}

void cel_isip_at_v(uint32_t out_slot, uint32_t str_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* str = cel_value_at(str_slot);
  if (absorb_3vl_unary(out, str)) return;
  if (str->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  NetIp ip;
  const uint8_t* base = cel_memory_base_();
  int ok = parse_address(base, str->payload.s.ptr, str->payload.s.len, &ip);
  write_bool(out, ok);
}

void cel_ip_is_canonical_at_v(uint32_t out_slot, uint32_t str_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* str = cel_value_at(str_slot);
  if (absorb_3vl_unary(out, str)) return;
  if (str->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  NetIp ip;
  const uint8_t* base = cel_memory_base_();
  uint32_t ptr = str->payload.s.ptr;
  uint32_t len = str->payload.s.len;
  if (!parse_address(base, ptr, len, &ip)) {
    poison(out, CEL_ERR_INVALID_ARGUMENT);  // errors on unparseable input
    return;
  }
  // Canonical iff re-rendering reproduces the input byte-for-byte
  // (catches uppercase hex, non-minimal `::`, leading zeros, …).
  char buf[kNetIpMaxText + 1];
  uint32_t n = render_address(&ip, buf);
  int canon = (n == len);
  for (uint32_t k = 0; canon && k < n; ++k) {
    if ((uint8_t)buf[k] != base[ptr + k]) canon = 0;
  }
  write_bool(out, canon);
}
