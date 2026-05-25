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
};

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
