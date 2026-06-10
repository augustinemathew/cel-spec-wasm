// M18 e2e acceptance spec — the `network_ext` extension functions
// (`net.IP` / `net.CIDR`) end-to-end through Compiler::Compile →
// Engine::Plan → Instance::Eval.  Source expressions and expected
// results mirror conformance rows from
// `tests/simple/testdata/network_ext.textproto` (69 rows) so a
// regression in any family surfaces before the conformance run.
//
// `net.IP` / `net.CIDR` are first-class abstract runtime kinds with no
// host-side Value::Kind — so every row here observes them indirectly,
// exactly as the corpus does: through `string(...)` (→ string),
// `type(...) == net.IP/net.CIDR` (→ bool), `==` (→ bool), the integer
// accessors `family()` / `prefixLength()` (→ int), and the boolean
// classification / containment predicates.  No expression returns a
// bare IP/CIDR value to the host.
//
// Families:
//   - IpParseE2ETest   ip() parse + string() round-trip (v4/v6),
//                      isIP, ip.isCanonical, type()
//   - IpPredicateE2ETest   family + the five classification predicates
//                          (v4 + v6), and == (incl. v4-mapped equality)
//   - CidrE2ETest      cidr() parse, string(), type(), ==, containsIP
//                      (object + string overloads), containsCIDR
//                      (object + string overloads), ip(), masked(),
//                      prefixLength()
//   - ErrorE2ETest     parse-failure / zone / v4-mapped eval-error rows
//   - CompileErrorE2ETest  the single checker-rejected row
//                          (isIP applied to net.CIDR)

#include <cstdint>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "compiler/program.h"
#include "eval/value.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

using ::celwasm::e2e::GlobalEngine;

Compiler NetCompiler() {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  return *std::move(compiler);
}

using ::celwasm::e2e::CompilePlan;

Value EvalOk(absl::string_view source) {
  auto compiler = NetCompiler();
  auto instance = CompilePlan(compiler, source);
  Activation a;
  auto v = instance.Eval(a);
  ABSL_CHECK_OK(v) << source;
  return *std::move(v);
}

bool EvalBool(absl::string_view source) {
  Value v = EvalOk(source);
  ABSL_CHECK(v.kind() == Value::Kind::kBool)
      << source << " kind=" << static_cast<int>(v.kind());
  return *v.AsBool();
}

int64_t EvalInt(absl::string_view source) {
  Value v = EvalOk(source);
  ABSL_CHECK(v.kind() == Value::Kind::kInt)
      << source << " kind=" << static_cast<int>(v.kind());
  return *v.AsInt();
}

std::string EvalString(absl::string_view source) {
  Value v = EvalOk(source);
  ABSL_CHECK(v.kind() == Value::Kind::kString)
      << source << " kind=" << static_cast<int>(v.kind());
  return std::string(*v.AsString());
}

// A parse / canonicalisation failure type-checks but evaluates to a CEL
// error Value (the harness compares error kind only; messages are not
// reproduced — see cel_net_ext.h).
void ExpectEvalError(absl::string_view source, absl::string_view why) {
  auto compiler = NetCompiler();
  auto instance = CompilePlan(compiler, source);
  Activation a;
  auto v = instance.Eval(a);
  ASSERT_THAT(v, IsOk()) << source;
  EXPECT_TRUE(v->IsError())
      << "expected `" << source << "` to surface a CEL error Value (" << why
      << "); got kind=" << static_cast<int>(v->kind());
}

// ──────────────────────────────────────────────────────────────
// IpParseE2ETest — ip() parse + string() round-trip, isIP,
// ip.isCanonical, type().  Both IPv4 and IPv6.
// ──────────────────────────────────────────────────────────────

class IpParseE2ETest : public ::testing::Test {};

TEST_F(IpParseE2ETest, ParseAndStringIpv4) {
  EXPECT_EQ(EvalString("string(ip('192.168.0.1'))"), "192.168.0.1");
}

TEST_F(IpParseE2ETest, ParseAndStringIpv6) {
  EXPECT_EQ(EvalString("string(ip('2001:db8::68'))"), "2001:db8::68");
}

TEST_F(IpParseE2ETest, IsIpValidIpv4) {
  EXPECT_TRUE(EvalBool("isIP('192.168.0.1')"));
}

TEST_F(IpParseE2ETest, IsIpInvalidIpv4) {
  // isIP never errors — an unparseable string returns false.
  EXPECT_FALSE(EvalBool("isIP('192.168.0.1.0')"));
}

TEST_F(IpParseE2ETest, IsCanonicalValidIpv4) {
  EXPECT_TRUE(EvalBool("ip.isCanonical('127.0.0.1')"));
}

TEST_F(IpParseE2ETest, IsCanonicalNonCanonicalIpv6) {
  // Uppercase hex is non-canonical.
  EXPECT_FALSE(EvalBool("ip.isCanonical('2001:DB8::68')"));
}

TEST_F(IpParseE2ETest, TypeIsNetIp) {
  EXPECT_TRUE(EvalBool("type(ip('192.168.0.1')) == net.IP"));
}

// ──────────────────────────────────────────────────────────────
// IpPredicateE2ETest — family + classification predicates + ==.
// ──────────────────────────────────────────────────────────────

class IpPredicateE2ETest : public ::testing::Test {};

TEST_F(IpPredicateE2ETest, FamilyIpv4) {
  EXPECT_EQ(EvalInt("ip('192.168.0.1').family()"), 4);
}

TEST_F(IpPredicateE2ETest, FamilyIpv6) {
  EXPECT_EQ(EvalInt("ip('2001:db8::68').family()"), 6);
}

TEST_F(IpPredicateE2ETest, IsUnspecifiedIpv4) {
  EXPECT_TRUE(EvalBool("ip('0.0.0.0').isUnspecified()"));
  EXPECT_FALSE(EvalBool("ip('127.0.0.1').isUnspecified()"));
}

TEST_F(IpPredicateE2ETest, IsUnspecifiedIpv6) {
  EXPECT_TRUE(EvalBool("ip('::').isUnspecified()"));
}

TEST_F(IpPredicateE2ETest, IsLoopbackIpv4) {
  EXPECT_TRUE(EvalBool("ip('127.0.0.1').isLoopback()"));
  EXPECT_FALSE(EvalBool("ip('1.2.3.4').isLoopback()"));
}

TEST_F(IpPredicateE2ETest, IsLoopbackIpv6) {
  EXPECT_TRUE(EvalBool("ip('::1').isLoopback()"));
}

TEST_F(IpPredicateE2ETest, IsGlobalUnicastIpv4) {
  EXPECT_TRUE(EvalBool("ip('192.168.0.1').isGlobalUnicast()"));
  EXPECT_FALSE(EvalBool("ip('255.255.255.255').isGlobalUnicast()"));
}

TEST_F(IpPredicateE2ETest, IsGlobalUnicastIpv6) {
  EXPECT_TRUE(EvalBool("ip('2001:db8::abcd').isGlobalUnicast()"));
  EXPECT_FALSE(EvalBool("ip('ff00::1').isGlobalUnicast()"));
}

TEST_F(IpPredicateE2ETest, IsLinkLocalMulticastIpv4) {
  EXPECT_TRUE(EvalBool("ip('224.0.0.1').isLinkLocalMulticast()"));
  EXPECT_FALSE(EvalBool("ip('224.0.1.1').isLinkLocalMulticast()"));
}

TEST_F(IpPredicateE2ETest, IsLinkLocalMulticastIpv6) {
  EXPECT_TRUE(EvalBool("ip('ff02::1').isLinkLocalMulticast()"));
  EXPECT_FALSE(EvalBool("ip('fd00::1').isLinkLocalMulticast()"));
}

TEST_F(IpPredicateE2ETest, IsLinkLocalUnicastIpv4) {
  EXPECT_TRUE(EvalBool("ip('169.254.169.254').isLinkLocalUnicast()"));
  EXPECT_FALSE(EvalBool("ip('192.168.0.1').isLinkLocalUnicast()"));
}

TEST_F(IpPredicateE2ETest, IsLinkLocalUnicastIpv6) {
  EXPECT_TRUE(EvalBool("ip('fe80::1').isLinkLocalUnicast()"));
  EXPECT_FALSE(EvalBool("ip('fd80::1').isLinkLocalUnicast()"));
}

TEST_F(IpPredicateE2ETest, EqualsIpv4) {
  EXPECT_TRUE(EvalBool("ip('127.0.0.1') == ip('127.0.0.1')"));
  EXPECT_FALSE(EvalBool("ip('127.0.0.1') == ip('10.0.0.1')"));
}

TEST_F(IpPredicateE2ETest, EqualsIpv6CaseInsensitive) {
  EXPECT_TRUE(EvalBool("ip('2001:db8::1') == ip('2001:DB8::1')"));
  EXPECT_FALSE(EvalBool("ip('::') == ip('::ffff')"));
}

// The hex v4-mapped form `::ffff:c0a8:1` normalises to family=4 and
// compares EQUAL to the dotted-decimal v4 (corpus `ipv4_equals_ipv6`);
// the dotted-decimal v4-mapped form is rejected at parse (ErrorE2ETest).
TEST_F(IpPredicateE2ETest, V4MappedEqualsV4) {
  EXPECT_TRUE(EvalBool("ip('::ffff:c0a8:1') == ip('192.168.0.1')"));
  EXPECT_FALSE(EvalBool("ip('::ffff:c0a8:1') == ip('192.168.10.1')"));
}

// ──────────────────────────────────────────────────────────────
// CidrE2ETest — cidr() parse, string(), type(), ==, containsIP /
// containsCIDR (object + string overloads), ip(), masked(),
// prefixLength().
// ──────────────────────────────────────────────────────────────

class CidrE2ETest : public ::testing::Test {};

TEST_F(CidrE2ETest, TypeIsNetCidrIpv4) {
  EXPECT_TRUE(EvalBool("type(cidr('192.168.0.0/24')) == net.CIDR"));
}

TEST_F(CidrE2ETest, StringIpv4) {
  EXPECT_EQ(EvalString("string(cidr('192.168.0.0/24'))"), "192.168.0.0/24");
}

TEST_F(CidrE2ETest, StringIpv6) {
  EXPECT_EQ(EvalString("string(cidr('2001:db8::/32'))"), "2001:db8::/32");
}

TEST_F(CidrE2ETest, Equals) {
  EXPECT_TRUE(EvalBool("cidr('127.0.0.1/24') == cidr('127.0.0.1/24')"));
  EXPECT_FALSE(EvalBool("cidr('192.0.0.1/32') == cidr('10.0.0.1/8')"));
  EXPECT_FALSE(EvalBool("cidr('2001:db8::/32') == cidr('10.0.0.1/32')"));
}

TEST_F(CidrE2ETest, ContainsIpObjectIpv4) {
  EXPECT_TRUE(EvalBool("cidr('192.168.0.0/24').containsIP(ip('192.168.0.1'))"));
  EXPECT_FALSE(
      EvalBool("cidr('192.168.0.0/24').containsIP(ip('192.168.1.1'))"));
}

TEST_F(CidrE2ETest, ContainsIpStringIpv4) {
  EXPECT_TRUE(EvalBool("cidr('192.168.0.0/24').containsIP('192.168.0.1')"));
  EXPECT_FALSE(EvalBool("cidr('192.168.0.0/24').containsIP('192.168.1.1')"));
}

TEST_F(CidrE2ETest, ContainsIpObjectIpv6) {
  EXPECT_TRUE(EvalBool("cidr('2001:db8::/32').containsIP(ip('2001:db8::1'))"));
}

TEST_F(CidrE2ETest, ContainsIpCrossFamilyIsFalse) {
  EXPECT_FALSE(EvalBool("cidr('2001:db8::/32').containsIP(ip('192.168.1.1'))"));
  EXPECT_FALSE(
      EvalBool("cidr('192.168.1.1/32').containsIP(ip('2001:db8::1'))"));
}

TEST_F(CidrE2ETest, ContainsCidrObjectIpv4) {
  EXPECT_TRUE(
      EvalBool("cidr('192.168.0.0/24').containsCIDR(cidr('192.168.0.0/25'))"));
  EXPECT_TRUE(
      EvalBool("cidr('192.168.0.0/24').containsCIDR(cidr('192.168.0.1/32'))"));
  EXPECT_FALSE(
      EvalBool("cidr('192.168.0.0/24').containsCIDR(cidr('192.168.0.0/23'))"));
}

TEST_F(CidrE2ETest, ContainsCidrExact) {
  EXPECT_TRUE(EvalBool("cidr('10.0.0.0/8').containsCIDR(cidr('10.0.0.0/8'))"));
}

TEST_F(CidrE2ETest, ContainsCidrStringIpv4) {
  EXPECT_TRUE(
      EvalBool("cidr('192.168.0.0/24').containsCIDR('192.168.0.0/25')"));
  EXPECT_TRUE(EvalBool("cidr('10.0.0.0/8').containsCIDR('10.0.0.0/8')"));
}

TEST_F(CidrE2ETest, ContainsCidrObjectIpv6) {
  EXPECT_TRUE(
      EvalBool("cidr('2001:db8::/32').containsCIDR(cidr('2001:db8::/33'))"));
}

TEST_F(CidrE2ETest, IpAccessor) {
  EXPECT_TRUE(EvalBool("cidr('192.168.0.0/24').ip() == ip('192.168.0.0')"));
  EXPECT_TRUE(EvalBool("cidr('2001:db8::/32').ip() == ip('2001:db8::')"));
}

TEST_F(CidrE2ETest, Masked) {
  EXPECT_TRUE(
      EvalBool("cidr('192.168.0.1/24').masked() == cidr('192.168.0.0/24')"));
}

TEST_F(CidrE2ETest, PrefixLength) {
  EXPECT_EQ(EvalInt("cidr('192.168.0.0/24').prefixLength()"), 24);
  EXPECT_EQ(EvalInt("cidr('2001:db8::/32').prefixLength()"), 32);
}

// ──────────────────────────────────────────────────────────────
// ErrorE2ETest — rows that type-check but evaluate to a CEL error
// Value: invalid v4/v6 forms, zone IDs, dotted-decimal v4-mapped,
// bad masks.  (The corpus's rich error strings are not reproduced —
// the harness compares error kind only.)
// ──────────────────────────────────────────────────────────────

class ErrorE2ETest : public ::testing::Test {};

TEST_F(ErrorE2ETest, ParseInvalidIpv4) {
  ExpectEvalError("ip('192.168.0.1.0')", "too many octets");
}

TEST_F(ErrorE2ETest, IsCanonicalInvalidIpv4) {
  ExpectEvalError("ip.isCanonical('127.0.0.1.0')", "unparseable arg to canon");
}

TEST_F(ErrorE2ETest, ParseInvalidIpv6) {
  ExpectEvalError("ip('2001:db8:::68')", "triple colon");
}

TEST_F(ErrorE2ETest, ParseInvalidIpv6WithZone) {
  ExpectEvalError("ip('fe80::1%en0')", "zone id not allowed");
}

TEST_F(ErrorE2ETest, ParseDottedV4Mapped) {
  ExpectEvalError("ip('::ffff:192.168.0.1')", "dotted v4-mapped not allowed");
}

TEST_F(ErrorE2ETest, ParseInvalidCidrIpv4) {
  ExpectEvalError("cidr('192.168.0.0/')", "missing prefix");
}

TEST_F(ErrorE2ETest, ParseInvalidCidrWithZone) {
  ExpectEvalError("cidr('fe80::1%en0/24')", "zone id not allowed");
}

TEST_F(ErrorE2ETest, ParseInvalidCidrV4Mapped) {
  ExpectEvalError("cidr('::ffff:192.168.0.1/24')", "dotted v4-mapped");
}

// ──────────────────────────────────────────────────────────────
// CompileErrorE2ETest — the single corpus row that is a checker
// error (`disable_check: false`): isIP has no overload over net.CIDR.
// ──────────────────────────────────────────────────────────────

class CompileErrorE2ETest : public ::testing::Test {};

TEST_F(CompileErrorE2ETest, IsIpAppliedToCidrRejected) {
  auto compiler = NetCompiler();
  auto program = compiler.Compile("isIP(cidr('192.168.0.0/24'))");
  EXPECT_FALSE(program.ok())
      << "isIP(net.CIDR) must fail type-checking (no matching overload)";
}

}  // namespace
}  // namespace celwasm
