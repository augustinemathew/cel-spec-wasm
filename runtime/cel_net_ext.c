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

#include "runtime/cel_net_ext.h"

#include <arpa/inet.h>
#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_make.h"

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

// NOLINTNEXTLINE(misc-use-internal-linkage)
int net_cidr_eq(const CelValue* a, const CelValue* b) {
  const NetCidr* ca = (const NetCidr*)(cel_memory_base_() + a->payload.net_ref);
  const NetCidr* cb = (const NetCidr*)(cel_memory_base_() + b->payload.net_ref);
  if (ca->family != cb->family || ca->prefix != cb->prefix) return 0;
  for (int k = 0; k < 16; ++k) {
    if (ca->addr[k] != cb->addr[k]) return 0;
  }
  return 1;
}

// ════════════════════════════════════════════════════════════════
// net.IP classification (Go `net/netip` Addr predicates).  Each works
// on the parsed {family, addr} bytes; v4 keeps the four octets in
// addr[0..4].
// ════════════════════════════════════════════════════════════════

static int ip_is_unspecified(const NetIp* ip) {
  int n = (ip->family == 4) ? 4 : 16;
  for (int k = 0; k < n; ++k) {
    if (ip->addr[k] != 0) return 0;
  }
  return 1;
}

static int ip_is_loopback(const NetIp* ip) {
  if (ip->family == 4) return ip->addr[0] == 127;  // 127.0.0.0/8
  for (int k = 0; k < 15; ++k) {                   // ::1
    if (ip->addr[k] != 0) return 0;
  }
  return ip->addr[15] == 1;
}

static int ip_is_multicast(const NetIp* ip) {
  if (ip->family == 4) return (ip->addr[0] & 0xf0) == 0xe0;  // 224.0.0.0/4
  return ip->addr[0] == 0xff;                                // ff00::/8
}

static int ip_is_link_local_unicast(const NetIp* ip) {
  if (ip->family == 4) {  // 169.254.0.0/16
    return ip->addr[0] == 169 && ip->addr[1] == 254;
  }
  return ip->addr[0] == 0xfe && (ip->addr[1] & 0xc0) == 0x80;  // fe80::/10
}

static int ip_is_link_local_multicast(const NetIp* ip) {
  if (ip->family == 4) {  // 224.0.0.0/24
    return ip->addr[0] == 224 && ip->addr[1] == 0 && ip->addr[2] == 0;
  }
  return ip->addr[0] == 0xff && ip->addr[1] == 0x02;  // ff02::/16
}

// Go's IsGlobalUnicast: false for the v4 unspecified + v4 broadcast
// (255.255.255.255); otherwise not v6-unspecified, not loopback, not
// multicast, not link-local-unicast (multicast subsumes both
// link-local- and interface-local-multicast).
static int ip_is_global_unicast(const NetIp* ip) {
  if (ip->family == 4) {
    int bcast = 1;
    for (int k = 0; k < 4; ++k) {
      if (ip->addr[k] != 0xff) bcast = 0;
    }
    if (ip_is_unspecified(ip) || bcast) return 0;
  }
  if (ip_is_unspecified(ip)) return 0;  // v6 ::
  return !ip_is_loopback(ip) && !ip_is_multicast(ip) &&
         !ip_is_link_local_unicast(ip);
}

// Shared body for the bool classification predicates: 3VL-absorb,
// type-check CEL_IP, then run `pred` on the parsed bytes.
typedef int (*IpPredicate)(const NetIp*);
static void ip_classify(uint32_t out_slot, uint32_t ip_slot, IpPredicate pred) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(ip_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind != CEL_IP) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const NetIp* ip = (const NetIp*)(cel_memory_base_() + in->payload.net_ref);
  write_bool(out, pred(ip));
}

void cel_ip_family_at_v(uint32_t out_slot, uint32_t ip_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(ip_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind != CEL_IP) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const NetIp* ip = (const NetIp*)(cel_memory_base_() + in->payload.net_ref);
  write_int(out, (int64_t)ip->family);
}

void cel_ip_is_loopback_at_v(uint32_t out_slot, uint32_t ip_slot) {
  ip_classify(out_slot, ip_slot, ip_is_loopback);
}

void cel_ip_is_unspecified_at_v(uint32_t out_slot, uint32_t ip_slot) {
  ip_classify(out_slot, ip_slot, ip_is_unspecified);
}

void cel_ip_is_global_unicast_at_v(uint32_t out_slot, uint32_t ip_slot) {
  ip_classify(out_slot, ip_slot, ip_is_global_unicast);
}

void cel_ip_is_link_local_unicast_at_v(uint32_t out_slot, uint32_t ip_slot) {
  ip_classify(out_slot, ip_slot, ip_is_link_local_unicast);
}

void cel_ip_is_link_local_multicast_at_v(uint32_t out_slot, uint32_t ip_slot) {
  ip_classify(out_slot, ip_slot, ip_is_link_local_multicast);
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

// ════════════════════════════════════════════════════════════════
// net.CIDR — bit helpers (shared by contains / masked) + kernels.
// ════════════════════════════════════════════════════════════════

// True iff the first `nbits` bits of a16 and b16 are identical (whole
// leading bytes by memcmp-style loop, then the partial trailing byte
// masked to its high `nbits % 8` bits).
static int first_n_bits_match(const uint8_t* a16, const uint8_t* b16,
                              uint32_t nbits) {
  uint32_t full = nbits / 8;
  for (uint32_t k = 0; k < full; ++k) {
    if (a16[k] != b16[k]) return 0;
  }
  uint32_t rem = nbits % 8;
  if (rem != 0) {
    uint8_t mask = (uint8_t)(0xff << (8 - rem));
    if ((a16[full] & mask) != (b16[full] & mask)) return 0;
  }
  return 1;
}

// Zero every bit of addr16 at or beyond bit position `nbits` (in place).
static void mask_bits(uint8_t* addr16, uint32_t nbits) {
  for (uint32_t k = 0; k < 16; ++k) {
    if (k * 8 >= nbits) {
      addr16[k] = 0;
    } else if (nbits < (k + 1) * 8) {
      uint32_t rem = nbits - (k * 8);
      addr16[k] = (uint8_t)(addr16[k] & (0xff << (8 - rem)));
    }
  }
}

// Parse the decimal prefix in buf[ptr+from .. ptr+len) into *out_prefix,
// bounded to `max`.  Returns 1 on success; 0 on a non-digit char, an
// empty run, or an out-of-range value.
static int parse_prefix(const uint8_t* buf, uint32_t ptr, uint32_t from,
                        uint32_t len, uint32_t max, uint32_t* out_prefix) {
  if (from >= len) return 0;  // empty mask
  uint32_t prefix = 0;
  for (uint32_t k = from; k < len; ++k) {
    uint8_t c = buf[ptr + k];
    if (c < '0' || c > '9') return 0;
    prefix = (prefix * 10) + (uint32_t)(c - '0');
    if (prefix > 128) return 0;  // bounds-guard against overflow too
  }
  if (prefix > max) return 0;
  *out_prefix = prefix;
  return 1;
}

// Parse `<addr>/<prefix>` (split on the LAST '/') into *out.  Returns 1
// on success; 0 on any malformed input or CEL-policy rejection (zone
// id, missing/empty prefix, prefix out of [0, max], dotted-decimal
// v4-mapped — all inherited from parse_address).
static int parse_cidr(const uint8_t* buf, uint32_t ptr, uint32_t len,
                      NetCidr* out) {
  if (len == 0) return 0;
  uint32_t slash = len;  // index of the last '/', relative to ptr
  for (uint32_t k = 0; k < len; ++k) {
    if (buf[ptr + k] == '/') slash = k;
  }
  if (slash == len) return 0;  // no '/'
  NetIp ip;
  if (!parse_address(buf, ptr, slash, &ip)) return 0;
  uint32_t max = (ip.family == 4) ? 32 : 128;
  uint32_t prefix = 0;
  if (!parse_prefix(buf, ptr, slash + 1, len, max, &prefix)) return 0;
  out->family = ip.family;
  out->prefix = prefix;
  for (int k = 0; k < 16; ++k) {
    out->addr[k] = ip.addr[k];
  }
  return 1;
}

// Allocate a NetCidr in the arena and store *src; returns the byte
// offset, or 0 on arena exhaustion.
static uint32_t alloc_cidr(const NetCidr* src) {
  uint32_t off = arena_alloc((uint32_t)sizeof(NetCidr));
  if (off == 0) return 0;
  NetCidr* dst = (NetCidr*)(cel_memory_base_() + off);
  *dst = *src;
  return off;
}

void cel_cidr_parse_at_v(uint32_t out_slot, uint32_t str_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* str = cel_value_at(str_slot);
  if (absorb_3vl_unary(out, str)) return;
  if (str->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  NetCidr c;
  const uint8_t* base = cel_memory_base_();
  if (!parse_cidr(base, str->payload.s.ptr, str->payload.s.len, &c)) {
    poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  uint32_t off = alloc_cidr(&c);
  if (off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = CEL_CIDR;
  out->_pad = 0;
  out->payload.net_ref = off;
}

void cel_cidr_to_string_at_v(uint32_t out_slot, uint32_t cidr_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(cidr_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind != CEL_CIDR) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const NetCidr* c = (const NetCidr*)(cel_memory_base_() + in->payload.net_ref);
  NetIp ip;
  ip.family = c->family;
  for (int k = 0; k < 16; ++k) {
    ip.addr[k] = c->addr[k];
  }
  char buf[kNetIpMaxText + 1 + 4];  // "/nnn" tail (max prefix 128)
  uint32_t n = render_address(&ip, buf);
  buf[n++] = '/';
  uint32_t p = c->prefix;
  char digits[3];
  int nd = 0;
  do {
    digits[nd++] = (char)('0' + (p % 10));
    p /= 10;
  } while (p != 0);
  for (int k = nd - 1; k >= 0; --k) {
    buf[n++] = digits[k];
  }
  uint32_t s = cel_make_string(buf, n);
  if (s == 0 && n > 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  *out = *cel_value_at(s);
}

// Resolve `arg` (CEL_IP directly, or CEL_STRING parsed) into *ip.
// Returns 1 on success; 0 on a parse failure (caller poisons
// CEL_ERR_INVALID_ARGUMENT).  Caller has already 3VL-absorbed.
static int resolve_arg_ip(const CelValue* arg, NetIp* ip) {
  if (arg->kind == CEL_IP) {
    const NetIp* src =
        (const NetIp*)(cel_memory_base_() + arg->payload.net_ref);
    *ip = *src;
    return 1;
  }
  const uint8_t* base = cel_memory_base_();
  return parse_address(base, arg->payload.s.ptr, arg->payload.s.len, ip);
}

void cel_cidr_contains_ip_at_vv(uint32_t out_slot, uint32_t cidr_slot,
                                uint32_t arg_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* cv = cel_value_at(cidr_slot);
  const CelValue* arg = cel_value_at(arg_slot);
  if (absorb_3vl_binary(out, cv, arg)) return;
  if (cv->kind != CEL_CIDR ||
      (arg->kind != CEL_IP && arg->kind != CEL_STRING)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  NetIp ip;
  if (!resolve_arg_ip(arg, &ip)) {
    poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  const NetCidr* c = (const NetCidr*)(cel_memory_base_() + cv->payload.net_ref);
  if (c->family != ip.family) {  // cross-family → false, not error
    write_bool(out, 0);
    return;
  }
  write_bool(out, first_n_bits_match(c->addr, ip.addr, c->prefix));
}

// Resolve `arg` (CEL_CIDR directly, or CEL_STRING parsed) into *c.
static int resolve_arg_cidr(const CelValue* arg, NetCidr* c) {
  if (arg->kind == CEL_CIDR) {
    const NetCidr* src =
        (const NetCidr*)(cel_memory_base_() + arg->payload.net_ref);
    *c = *src;
    return 1;
  }
  const uint8_t* base = cel_memory_base_();
  return parse_cidr(base, arg->payload.s.ptr, arg->payload.s.len, c);
}

void cel_cidr_contains_cidr_at_vv(uint32_t out_slot, uint32_t cidr_slot,
                                  uint32_t arg_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* cv = cel_value_at(cidr_slot);
  const CelValue* arg = cel_value_at(arg_slot);
  if (absorb_3vl_binary(out, cv, arg)) return;
  if (cv->kind != CEL_CIDR ||
      (arg->kind != CEL_CIDR && arg->kind != CEL_STRING)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  NetCidr b;
  if (!resolve_arg_cidr(arg, &b)) {
    poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  const NetCidr* a = (const NetCidr*)(cel_memory_base_() + cv->payload.net_ref);
  if (a->family != b.family || a->prefix > b.prefix) {
    write_bool(out, 0);
    return;
  }
  write_bool(out, first_n_bits_match(a->addr, b.addr, a->prefix));
}

void cel_cidr_ip_at_v(uint32_t out_slot, uint32_t cidr_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(cidr_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind != CEL_CIDR) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const NetCidr* c = (const NetCidr*)(cel_memory_base_() + in->payload.net_ref);
  uint32_t off = arena_alloc((uint32_t)sizeof(NetIp));
  if (off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  NetIp* dst = (NetIp*)(cel_memory_base_() + off);
  dst->family = c->family;
  for (int k = 0; k < 16; ++k) {
    dst->addr[k] = c->addr[k];
  }
  out->kind = CEL_IP;
  out->_pad = 0;
  out->payload.net_ref = off;
}

void cel_cidr_masked_at_v(uint32_t out_slot, uint32_t cidr_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(cidr_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind != CEL_CIDR) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const NetCidr* c = (const NetCidr*)(cel_memory_base_() + in->payload.net_ref);
  NetCidr m = *c;
  mask_bits(m.addr, m.prefix);
  uint32_t off = alloc_cidr(&m);
  if (off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = CEL_CIDR;
  out->_pad = 0;
  out->payload.net_ref = off;
}

void cel_cidr_prefix_length_at_v(uint32_t out_slot, uint32_t cidr_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(cidr_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind != CEL_CIDR) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const NetCidr* c = (const NetCidr*)(cel_memory_base_() + in->payload.net_ref);
  write_int(out, (int64_t)c->prefix);
}
