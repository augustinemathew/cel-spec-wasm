// M18 network_ext — Slice A (net.IP) kernel coverage.
//
// Native unit tests for the IPv4 / IPv6 parser + canonicaliser + the
// ip() / isIP() / ip.isCanonical() / string(ip) kernels and the
// CEL_IP equality path.  Semantics are pinned to cel-go's network
// extension and the conformance fixture
// `tests/simple/testdata/network_ext.textproto`; the parse matrix
// (every valid/invalid v4+v6 form the corpus exercises) is the
// load-bearing half.

#include "compiler_v2/runtime/cel_net_ext.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_internal.h"
#include "compiler_v2/runtime/cel_layout.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class NetExtTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }
  uint32_t Slot() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }
  uint32_t Str(const std::string& s) {
    return cel_make_string(s.data(), static_cast<uint32_t>(s.size()));
  }
  uint32_t Int(int64_t v) {
    uint32_t s = Slot();
    CelValue* c = cel_value_at(s);
    c->kind = CEL_INT;
    c->payload.i = v;
    return s;
  }
  uint32_t Err() {
    uint32_t s = Slot();
    CelValue* c = cel_value_at(s);
    c->kind = CEL_ERROR;
    c->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
    return s;
  }
  uint32_t Unknown() {
    uint32_t s = Slot();
    cel_value_at(s)->kind = CEL_UNKNOWN;
    return s;
  }
  const CelValue* At(uint32_t s) {
    return cel_value_at(s);
  }
  std::string StringAt(uint32_t s) {
    const CelValue* v = At(s);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
    if (v->payload.s.len == 0) return {};
    return {reinterpret_cast<const char*>(cel_mem_base() + v->payload.s.ptr),
            v->payload.s.len};
  }
  // Parse `s` into a fresh CEL_IP slot via the production kernel.
  uint32_t Ip(const std::string& s) {
    uint32_t o = Slot();
    cel_ip_parse_at_v(o, Str(s));
    return o;
  }
  bool IsIp(const std::string& s) {
    uint32_t o = Slot();
    cel_isip_at_v(o, Str(s));
    EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_BOOL));
    return At(o)->payload.b != 0;
  }
  std::string Canonical(const std::string& s) {
    uint32_t o = Slot();
    cel_ip_to_string_at_v(o, Ip(s));
    return StringAt(o);
  }
  bool Equal(const std::string& a, const std::string& b) {
    return net_ip_eq(At(Ip(a)), At(Ip(b)));
  }
  // Parse `s` into a fresh CEL_CIDR slot via the production kernel.
  uint32_t Cidr(const std::string& s) {
    uint32_t o = Slot();
    cel_cidr_parse_at_v(o, Str(s));
    return o;
  }
  // Run a unary CEL_IP receiver predicate kernel; returns the bool.
  bool IpPred(void (*fn)(uint32_t, uint32_t), const std::string& s) {
    uint32_t o = Slot();
    fn(o, Ip(s));
    EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_BOOL)) << s;
    return At(o)->payload.b != 0;
  }
  bool CidrEqual(const std::string& a, const std::string& b) {
    return net_cidr_eq(At(Cidr(a)), At(Cidr(b)));
  }
};

// ════════════════════════════════════════════════════════════════
// Slice B — net.IP classification predicates.
// ════════════════════════════════════════════════════════════════

TEST_F(NetExtTest, Family) {
  uint32_t o = Slot();
  cel_ip_family_at_v(o, Ip("192.168.0.1"));  // corpus: ipv4_family
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(At(o)->payload.i, 4);
  cel_ip_family_at_v(o, Ip("2001:db8::68"));  // corpus: ipv6 family
  EXPECT_EQ(At(o)->payload.i, 6);
}

TEST_F(NetExtTest, IsUnspecified) {
  EXPECT_TRUE(IpPred(cel_ip_is_unspecified_at_v, "0.0.0.0"));     // v4 true
  EXPECT_FALSE(IpPred(cel_ip_is_unspecified_at_v, "127.0.0.1"));  // v4 false
  EXPECT_TRUE(IpPred(cel_ip_is_unspecified_at_v, "::"));          // v6 true
  EXPECT_FALSE(IpPred(cel_ip_is_unspecified_at_v, "2001:db8::68"));
}

TEST_F(NetExtTest, IsLoopback) {
  EXPECT_TRUE(IpPred(cel_ip_is_loopback_at_v, "127.0.0.1"));  // v4 true
  EXPECT_TRUE(IpPred(cel_ip_is_loopback_at_v, "127.5.5.5"));  // 127/8
  EXPECT_FALSE(IpPred(cel_ip_is_loopback_at_v, "1.2.3.4"));
  EXPECT_TRUE(IpPred(cel_ip_is_loopback_at_v, "::1"));  // v6 true
  EXPECT_FALSE(IpPred(cel_ip_is_loopback_at_v, "2001:db8::68"));
}

TEST_F(NetExtTest, IsGlobalUnicast) {
  EXPECT_TRUE(IpPred(cel_ip_is_global_unicast_at_v, "192.168.0.1"));
  // corpus: ipv4_is_global_unicast_false — broadcast is NOT global.
  EXPECT_FALSE(IpPred(cel_ip_is_global_unicast_at_v, "255.255.255.255"));
  EXPECT_FALSE(IpPred(cel_ip_is_global_unicast_at_v, "0.0.0.0"));
  EXPECT_TRUE(IpPred(cel_ip_is_global_unicast_at_v, "2001:db8::abcd"));
  EXPECT_FALSE(IpPred(cel_ip_is_global_unicast_at_v, "ff00::1"));  // multicast
  EXPECT_FALSE(IpPred(cel_ip_is_global_unicast_at_v, "::"));
  EXPECT_FALSE(IpPred(cel_ip_is_global_unicast_at_v, "127.0.0.1"));
  EXPECT_FALSE(IpPred(cel_ip_is_global_unicast_at_v, "fe80::1"));  // link-local
  EXPECT_TRUE(IpPred(cel_ip_is_global_unicast_at_v, "fd00::1"));   // ULA: true
}

TEST_F(NetExtTest, IsLinkLocalMulticast) {
  EXPECT_TRUE(IpPred(cel_ip_is_link_local_multicast_at_v, "224.0.0.1"));
  EXPECT_FALSE(IpPred(cel_ip_is_link_local_multicast_at_v, "224.0.1.1"));
  EXPECT_TRUE(IpPred(cel_ip_is_link_local_multicast_at_v, "ff02::1"));
  EXPECT_FALSE(IpPred(cel_ip_is_link_local_multicast_at_v, "fd00::1"));
}

TEST_F(NetExtTest, IsLinkLocalUnicast) {
  EXPECT_TRUE(IpPred(cel_ip_is_link_local_unicast_at_v, "169.254.169.254"));
  EXPECT_FALSE(IpPred(cel_ip_is_link_local_unicast_at_v, "192.168.0.1"));
  EXPECT_TRUE(IpPred(cel_ip_is_link_local_unicast_at_v, "fe80::1"));
  EXPECT_FALSE(IpPred(cel_ip_is_link_local_unicast_at_v, "fd80::1"));
}

TEST_F(NetExtTest, ClassificationWrongKindPoisons) {
  uint32_t o = Slot();
  cel_ip_family_at_v(o, Int(5));
  EXPECT_EQ(At(o)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
  cel_ip_is_loopback_at_v(o, Int(5));
  EXPECT_EQ(At(o)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(NetExtTest, ClassificationAbsorbs3vl) {
  uint32_t o = Slot();
  cel_ip_family_at_v(o, Err());
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  cel_ip_is_loopback_at_v(o, Unknown());
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

// ════════════════════════════════════════════════════════════════
// Slice C — net.CIDR type + operations.
// ════════════════════════════════════════════════════════════════

TEST_F(NetExtTest, ParseValidCidr) {
  EXPECT_EQ(At(Cidr("192.168.0.0/24"))->kind, static_cast<uint32_t>(CEL_CIDR));
  EXPECT_EQ(At(Cidr("2001:db8::/32"))->kind, static_cast<uint32_t>(CEL_CIDR));
  EXPECT_EQ(At(Cidr("0.0.0.0/0"))->kind, static_cast<uint32_t>(CEL_CIDR));
  EXPECT_EQ(At(Cidr("1.2.3.4/32"))->kind, static_cast<uint32_t>(CEL_CIDR));
  EXPECT_EQ(At(Cidr("::/128"))->kind, static_cast<uint32_t>(CEL_CIDR));
}

TEST_F(NetExtTest, ParseInvalidCidr) {
  // corpus: parse_invalid_cidr_ipv4 — empty mask.
  EXPECT_EQ(At(Cidr("192.168.0.0/"))->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(Cidr("192.168.0.0/"))->payload.err,
            static_cast<uint32_t>(CEL_ERR_INVALID_ARGUMENT));
  // corpus: parse_invalid_cidr_with_zone.
  EXPECT_EQ(At(Cidr("fe80::1%en0/24"))->kind, static_cast<uint32_t>(CEL_ERROR));
  // corpus: parse_invalid_cidr_ipv4_in_ipv6 — dotted v4-mapped rejected.
  EXPECT_EQ(At(Cidr("::ffff:192.168.0.1/24"))->kind,
            static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(Cidr("192.168.0.0"))->kind,
            static_cast<uint32_t>(CEL_ERROR));  // no slash
}

TEST_F(NetExtTest, ParseCidrPrefixBounds) {
  EXPECT_EQ(At(Cidr("192.168.0.0/33"))->kind,
            static_cast<uint32_t>(CEL_ERROR));  // v4 max 32
  EXPECT_EQ(At(Cidr("2001:db8::/129"))->kind,
            static_cast<uint32_t>(CEL_ERROR));  // v6 max 128
  EXPECT_EQ(At(Cidr("192.168.0.0/abc"))->kind,
            static_cast<uint32_t>(CEL_ERROR));  // non-numeric mask
  EXPECT_EQ(At(Cidr("192.168.0.0/24"))->kind, static_cast<uint32_t>(CEL_CIDR));
}

TEST_F(NetExtTest, ParseCidrKeepsHostBits) {
  // Go's ParsePrefix keeps the address as given (not masked); pinned by
  // corpus cidr_masked_ipv4 (`.../24` keeps `.1`, masked → `.0`).
  uint32_t c = Cidr("192.168.0.1/24");
  const auto* nc =
      reinterpret_cast<const NetCidr*>(cel_mem_base() + At(c)->payload.net_ref);
  EXPECT_EQ(nc->addr[3], 1);
  EXPECT_EQ(nc->prefix, 24u);
}

TEST_F(NetExtTest, CidrParseWrongKindPoisons) {
  uint32_t o = Slot();
  cel_cidr_parse_at_v(o, Int(5));
  EXPECT_EQ(At(o)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(NetExtTest, CidrToString) {
  uint32_t o = Slot();
  cel_cidr_to_string_at_v(o, Cidr("192.168.0.0/24"));  // corpus: cidr_to_string
  EXPECT_EQ(StringAt(o), "192.168.0.0/24");
  cel_cidr_to_string_at_v(o, Cidr("2001:db8::/32"));  // corpus: parse_cidr_ipv6
  EXPECT_EQ(StringAt(o), "2001:db8::/32");
  cel_cidr_to_string_at_v(o, Cidr("0.0.0.0/0"));
  EXPECT_EQ(StringAt(o), "0.0.0.0/0");
}

TEST_F(NetExtTest, CidrToStringWrongKindPoisons) {
  uint32_t o = Slot();
  cel_cidr_to_string_at_v(o, Int(5));
  EXPECT_EQ(At(o)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(NetExtTest, ContainsIpObject) {
  uint32_t o = Slot();
  // corpus: cidr_contains_ip_ipv4_object.
  cel_cidr_contains_ip_at_vv(o, Cidr("192.168.0.0/24"), Ip("192.168.0.1"));
  EXPECT_TRUE(At(o)->payload.b);
  // corpus: cidr_does_not_contain_ip_ipv4_object.
  cel_cidr_contains_ip_at_vv(o, Cidr("192.168.0.0/24"), Ip("192.168.1.1"));
  EXPECT_FALSE(At(o)->payload.b);
  // corpus: cidr_contains_ip_ipv6_object.
  cel_cidr_contains_ip_at_vv(o, Cidr("2001:db8::/32"), Ip("2001:db8::1"));
  EXPECT_TRUE(At(o)->payload.b);
}

TEST_F(NetExtTest, ContainsIpString) {
  uint32_t o = Slot();
  // corpus: cidr_contains_ip_ipv4_string.
  cel_cidr_contains_ip_at_vv(o, Cidr("192.168.0.0/24"), Str("192.168.0.1"));
  EXPECT_TRUE(At(o)->payload.b);
  // corpus: cidr_does_not_contain_ip_ipv4_string.
  cel_cidr_contains_ip_at_vv(o, Cidr("192.168.0.0/24"), Str("192.168.1.1"));
  EXPECT_FALSE(At(o)->payload.b);
}

TEST_F(NetExtTest, ContainsIpCrossFamilyFalse) {
  uint32_t o = Slot();
  // corpus: cidr_ipv6_not_contains_ip_ipv4_object.
  cel_cidr_contains_ip_at_vv(o, Cidr("2001:db8::/32"), Ip("192.168.1.1"));
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_BOOL));  // false, not error
  EXPECT_FALSE(At(o)->payload.b);
  // corpus: cidr_ipv4_not_contains_ip_ipv6_object.
  cel_cidr_contains_ip_at_vv(o, Cidr("192.168.1.1/32"), Ip("2001:db8::1"));
  EXPECT_FALSE(At(o)->payload.b);
}

TEST_F(NetExtTest, ContainsIpBadStringPoisons) {
  uint32_t o = Slot();
  cel_cidr_contains_ip_at_vv(o, Cidr("192.168.0.0/24"), Str("not-an-ip"));
  EXPECT_EQ(At(o)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INVALID_ARGUMENT));
}

TEST_F(NetExtTest, ContainsCidrObject) {
  uint32_t o = Slot();
  // corpus: cidr_contains_cidr_ipv4_object (sub-net /25 inside /24).
  cel_cidr_contains_cidr_at_vv(o, Cidr("192.168.0.0/24"),
                               Cidr("192.168.0.0/25"));
  EXPECT_TRUE(At(o)->payload.b);
  // corpus: cidr_contains_cidr_ipv4_object_32.
  cel_cidr_contains_cidr_at_vv(o, Cidr("192.168.0.0/24"),
                               Cidr("192.168.0.1/32"));
  EXPECT_TRUE(At(o)->payload.b);
  // corpus: cidr_not_contains_cidr_ipv4_object (super-net /23 NOT in /24).
  cel_cidr_contains_cidr_at_vv(o, Cidr("192.168.0.0/24"),
                               Cidr("192.168.0.0/23"));
  EXPECT_FALSE(At(o)->payload.b);
  // corpus: cidr_contains_cidr (equal).
  cel_cidr_contains_cidr_at_vv(o, Cidr("10.0.0.0/8"), Cidr("10.0.0.0/8"));
  EXPECT_TRUE(At(o)->payload.b);
  // corpus: cidr_contains_cidr_ipv6_object.
  cel_cidr_contains_cidr_at_vv(o, Cidr("2001:db8::/32"), Cidr("2001:db8::/33"));
  EXPECT_TRUE(At(o)->payload.b);
}

TEST_F(NetExtTest, ContainsCidrString) {
  uint32_t o = Slot();
  // corpus: cidr_contains_cidr_ipv4_string.
  cel_cidr_contains_cidr_at_vv(o, Cidr("192.168.0.0/24"),
                               Str("192.168.0.0/25"));
  EXPECT_TRUE(At(o)->payload.b);
  // corpus: cidr_contains_cidr_ipv4_exact.
  cel_cidr_contains_cidr_at_vv(o, Cidr("10.0.0.0/8"), Str("10.0.0.0/8"));
  EXPECT_TRUE(At(o)->payload.b);
}

TEST_F(NetExtTest, ContainsCidrCrossFamilyFalse) {
  uint32_t o = Slot();
  cel_cidr_contains_cidr_at_vv(o, Cidr("2001:db8::/32"), Cidr("10.0.0.0/8"));
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_FALSE(At(o)->payload.b);
}

TEST_F(NetExtTest, CidrIp) {
  // corpus: cidr_get_ip_ipv4 — cidr('192.168.0.0/24').ip() ==
  // ip('192.168.0.0').
  uint32_t o = Slot();
  cel_cidr_ip_at_v(o, Cidr("192.168.0.0/24"));
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_IP));
  EXPECT_TRUE(net_ip_eq(At(o), At(Ip("192.168.0.0"))));
  // corpus: cidr_get_ip_ipv6.
  cel_cidr_ip_at_v(o, Cidr("2001:db8::/32"));
  EXPECT_TRUE(net_ip_eq(At(o), At(Ip("2001:db8::"))));
}

TEST_F(NetExtTest, CidrMasked) {
  // corpus: cidr_masked_ipv4 — masked('192.168.0.1/24') ==
  // cidr('192.168.0.0/24').
  uint32_t o = Slot();
  cel_cidr_masked_at_v(o, Cidr("192.168.0.1/24"));
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_CIDR));
  EXPECT_TRUE(net_cidr_eq(At(o), At(Cidr("192.168.0.0/24"))));
}

TEST_F(NetExtTest, CidrPrefixLength) {
  uint32_t o = Slot();
  cel_cidr_prefix_length_at_v(o, Cidr("192.168.0.0/24"));  // corpus row
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(At(o)->payload.i, 24);
  cel_cidr_prefix_length_at_v(o, Cidr("2001:db8::/32"));
  EXPECT_EQ(At(o)->payload.i, 32);
}

TEST_F(NetExtTest, CidrEquality) {
  // corpus: cidr_equals.
  EXPECT_TRUE(CidrEqual("127.0.0.1/24", "127.0.0.1/24"));
  // corpus: cidr_not_equals (different addr + prefix).
  EXPECT_FALSE(CidrEqual("192.0.0.1/32", "10.0.0.1/8"));
  // corpus: cidr_not_equals_ipv4_ipv6.
  EXPECT_FALSE(CidrEqual("2001:db8::/32", "10.0.0.1/32"));
  // same addr, different prefix → false.
  EXPECT_FALSE(CidrEqual("192.168.0.0/24", "192.168.0.0/25"));
}

TEST_F(NetExtTest, CidrOpsAbsorb3vl) {
  uint32_t o = Slot();
  cel_cidr_to_string_at_v(o, Err());
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  cel_cidr_prefix_length_at_v(o, Unknown());
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  cel_cidr_contains_ip_at_vv(o, Err(), Ip("1.2.3.4"));
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
}

// ── ip() parse: valid v4 / v6 ─────────────────────────────────────

TEST_F(NetExtTest, ParseValidIpv4) {
  uint32_t o = Ip("192.168.0.1");
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_IP));
}

TEST_F(NetExtTest, ParseValidIpv6) {
  uint32_t o = Ip("2001:db8::68");
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_IP));
}

TEST_F(NetExtTest, ParseVariousValidForms) {
  for (const char* s : {"0.0.0.0", "255.255.255.255", "127.0.0.1", "::", "::1",
                        "::ffff", "ff02::1", "fe80::1", "2001:db8::abcd"}) {
    uint32_t o = Ip(s);
    EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_IP)) << s;
  }
}

// ── ip() parse: invalid forms poison INVALID_ARGUMENT ─────────────

TEST_F(NetExtTest, ParseInvalidIpv4TrailingOctet) {
  uint32_t o = Ip("192.168.0.1.0");  // corpus: parse_invalid_ipv4
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(o)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INVALID_ARGUMENT));
}

TEST_F(NetExtTest, ParseInvalidIpv6TripleColon) {
  uint32_t o = Ip("2001:db8:::68");  // corpus: parse_invalid_ipv6
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
}

TEST_F(NetExtTest, ParseRejectsZoneId) {
  uint32_t o = Ip("fe80::1%en0");  // corpus: parse_invalid_ipv6_with_zone
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
}

TEST_F(NetExtTest, ParseRejectsDottedV4Mapped) {
  // corpus: parse_invalid_ipv4_in_ipv6 — dotted-decimal v4-mapped is
  // rejected even though hex v4-mapped is accepted.
  uint32_t o = Ip("::ffff:192.168.0.1");
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
}

TEST_F(NetExtTest, ParseRejectsBadOctetRange) {
  EXPECT_FALSE(IsIp("256.0.0.1"));
  EXPECT_FALSE(IsIp("192.168.0"));     // too few groups
  EXPECT_FALSE(IsIp("192.168.0.01"));  // leading zero
  EXPECT_FALSE(IsIp(""));
}

TEST_F(NetExtTest, ParseRejectsBadIpv6) {
  EXPECT_FALSE(IsIp(":1"));                 // single leading colon
  EXPECT_FALSE(IsIp("1:2:3:4:5:6:7:8:9"));  // too many groups
  EXPECT_FALSE(IsIp("12345::"));            // group > 4 hex digits
  EXPECT_FALSE(IsIp("2001::db8::1"));       // two `::` runs
}

// ── ip.isCanonical: hex v4-mapped accepted + normalised to v4 ─────

TEST_F(NetExtTest, ParseHexV4MappedNormalisesToV4) {
  // corpus: ipv4_equals_ipv6 — `::ffff:c0a8:1` == `192.168.0.1`.
  EXPECT_EQ(At(Ip("::ffff:c0a8:1"))->kind, static_cast<uint32_t>(CEL_IP));
  EXPECT_TRUE(Equal("::ffff:c0a8:1", "192.168.0.1"));
  EXPECT_FALSE(Equal("::ffff:c0a8:1", "192.168.10.1"));
}

// ── isIP() ────────────────────────────────────────────────────────

TEST_F(NetExtTest, IsIpTrueFalse) {
  EXPECT_TRUE(IsIp("192.168.0.1"));     // corpus: is_ip_valid_ipv4
  EXPECT_FALSE(IsIp("192.168.0.1.0"));  // corpus: is_ip_invalid_ipv4
  EXPECT_TRUE(IsIp("2001:db8::68"));
  EXPECT_FALSE(IsIp("fe80::1%en0"));  // does NOT error, just false
}

TEST_F(NetExtTest, IsIpWrongKindPoisons) {
  uint32_t o = Slot();
  cel_isip_at_v(o, Int(5));
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(o)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// ── ip.isCanonical() ──────────────────────────────────────────────

TEST_F(NetExtTest, IsCanonicalTrue) {
  uint32_t o = Slot();
  cel_ip_is_canonical_at_v(o, Str("127.0.0.1"));  // corpus: valid_ipv4
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_TRUE(At(o)->payload.b);
}

TEST_F(NetExtTest, IsCanonicalFalseUppercaseIpv6) {
  uint32_t o = Slot();
  cel_ip_is_canonical_at_v(o, Str("2001:DB8::68"));  // corpus: non_canonical
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_FALSE(At(o)->payload.b);
}

TEST_F(NetExtTest, IsCanonicalErrorsOnUnparseable) {
  uint32_t o = Slot();
  cel_ip_is_canonical_at_v(o, Str("127.0.0.1.0"));  // corpus: invalid_ipv4
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(o)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INVALID_ARGUMENT));
}

// ── string(ip) round-trips ────────────────────────────────────────

TEST_F(NetExtTest, ToStringV4) {
  EXPECT_EQ(Canonical("192.168.0.1"), "192.168.0.1");  // corpus: ip_to_string
  EXPECT_EQ(Canonical("0.0.0.0"), "0.0.0.0");
  EXPECT_EQ(Canonical("255.255.255.255"), "255.255.255.255");
}

TEST_F(NetExtTest, ToStringV6Compression) {
  EXPECT_EQ(Canonical("2001:db8::68"), "2001:db8::68");  // corpus: parse_ipv6
  EXPECT_EQ(Canonical("::"), "::");
  EXPECT_EQ(Canonical("::1"), "::1");
  EXPECT_EQ(Canonical("2001:DB8::68"), "2001:db8::68");  // lowercased
}

TEST_F(NetExtTest, ToStringHexV4MappedRendersAsV4) {
  EXPECT_EQ(Canonical("::ffff:c0a8:1"), "192.168.0.1");
}

TEST_F(NetExtTest, ToStringWrongKindPoisons) {
  uint32_t o = Slot();
  cel_ip_to_string_at_v(o, Int(5));
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(o)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// ── equality ──────────────────────────────────────────────────────

TEST_F(NetExtTest, EqualityV4) {
  EXPECT_TRUE(Equal("127.0.0.1", "127.0.0.1"));  // corpus: ipv4_equals
  EXPECT_FALSE(Equal("127.0.0.1", "10.0.0.1"));  // corpus: ipv4_not_equals
}

TEST_F(NetExtTest, EqualityV6CaseInsensitiveViaNormalisation) {
  // corpus: ipv6_equals — `2001:db8::1` == `2001:DB8::1` (parser keeps
  // bytes; case folds at parse, so equal).
  EXPECT_TRUE(Equal("2001:db8::1", "2001:DB8::1"));
  EXPECT_FALSE(Equal("::", "::ffff"));  // corpus: ipv6_not_equals
}

TEST_F(NetExtTest, EqualityCrossFamilyFalse) {
  EXPECT_FALSE(Equal("192.168.0.1", "2001:db8::1"));
}

// ── 3VL absorption ────────────────────────────────────────────────

TEST_F(NetExtTest, AbsorbErrorAndUnknown) {
  uint32_t o = Slot();
  cel_ip_parse_at_v(o, Err());
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  cel_isip_at_v(o, Unknown());
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  cel_ip_to_string_at_v(o, Err());
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  cel_ip_is_canonical_at_v(o, Unknown());
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

TEST_F(NetExtTest, ParseWrongKindPoisons) {
  uint32_t o = Slot();
  cel_ip_parse_at_v(o, Int(5));
  EXPECT_EQ(At(o)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(o)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

}  // namespace
}  // namespace celwasm
