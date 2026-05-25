// network_ext extension — AST-shape probe.
//
// Unlike string/math/optionals/encoders, cel-cpp ships NO network
// extension checker library.  So this probe PROVES we can self-declare
// the two abstract types (`net.IP`, `net.CIDR`) and the ~16 functions
// directly on a `TypeCheckerBuilder` — the same MakeFunctionDecl /
// AddOverload / AddFunction / AddVariable pattern cel-cpp's own
// `checker/optional.cc` uses — then parses + type-checks a
// representative battery pulled from
// `tests/simple/testdata/network_ext.textproto` and dumps the
// resulting CheckedExpr (DebugString) + reference_map overload ids.
//
// It answers, for every distinct corpus shape:
//   - does it type-check against the self-declared decls?
//   - is `cidr(...).containsIP(...)` a receiver kCallExpr (target set)?
//   - is `ip(...)` a global call?  what does `net.IP` reach as?
//   - what overload id(s) + result type does the checker assign?
//
// Research only — nothing here is production code.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/syntax.pb.h"
#include "checker/standard_library.h"
#include "checker/type_checker.h"
#include "checker/type_checker_builder.h"
#include "checker/type_checker_builder_factory.h"
#include "checker/validation_result.h"
#include "common/ast.h"
#include "common/ast_proto.h"
#include "common/decl.h"
#include "common/source.h"
#include "common/type.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "parser/macro_registry.h"
#include "parser/options.h"
#include "parser/parser.h"
#include "parser/standard_macros.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"

namespace celwasm::probes {
namespace {

using ::cel::expr::CheckedExpr;
using ::cel::expr::Expr;

// ---- the self-declared net decls -------------------------------------
//
// Types are named abstract (opaque) types with no parameters — exactly
// how OptionalType is an OpaqueType under the hood.  The arena must
// outlive the checker; we leak a static one (probe only).

cel::Type NetIpType(google::protobuf::Arena* arena) {
  return cel::Type(cel::OpaqueType(arena, "net.IP", /*parameters=*/{}));
}

cel::Type NetCidrType(google::protobuf::Arena* arena) {
  return cel::Type(cel::OpaqueType(arena, "net.CIDR", /*parameters=*/{}));
}

// Register the full network_ext surface on the builder.  Mirrors
// checker/optional.cc's RegisterOptionalDecls: one MakeFunctionDecl per
// name with one-or-more MakeOverloadDecl / MakeMemberOverloadDecl, then
// AddFunction; plus AddVariable for the bare type literals.
absl::Status RegisterNetDecls(cel::TypeCheckerBuilder& builder,
                              google::protobuf::Arena* arena) {
  const cel::Type ip = NetIpType(arena);
  const cel::Type cidr = NetCidrType(arena);
  const cel::Type str = cel::StringType();
  const cel::Type bln = cel::BoolType();
  const cel::Type i64 = cel::IntType();

  // --- global constructors / validators ---
  {
    auto d = cel::MakeFunctionDecl(
        "ip", cel::MakeOverloadDecl("net_ip_string", ip, str));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }
  {
    auto d = cel::MakeFunctionDecl(
        "cidr", cel::MakeOverloadDecl("net_cidr_string", cidr, str));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }
  {
    // isIP(string)->bool and isIP(string,int)->bool (2-arg version probe).
    auto d = cel::MakeFunctionDecl(
        "isIP", cel::MakeOverloadDecl("net_isIP_string", bln, str),
        cel::MakeOverloadDecl("net_isIP_string_int", bln, str, i64));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }
  {
    // ip.isCanonical(string)->bool — declared as a *global* function
    // whose name is the dotted "ip.isCanonical" (namespace form), NOT a
    // receiver on `ip`.  This is the disambiguation probe.
    auto d = cel::MakeFunctionDecl(
        "ip.isCanonical",
        cel::MakeOverloadDecl("net_ip_isCanonical_string", bln, str));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }

  // --- string() / type() additions over the new kinds ---
  // string() and type() are standard-library functions; extend them via
  // MergeFunction so the new overloads join the existing decl.
  {
    auto d = cel::MakeFunctionDecl(
        "string", cel::MakeOverloadDecl("net_string_ip", str, ip),
        cel::MakeOverloadDecl("net_string_cidr", str, cidr));
    if (!d.ok()) return d.status();
    if (auto s = builder.MergeFunction(*d); !s.ok()) return s;
  }

  // --- IP receiver predicates ---
  {
    auto fam = cel::MakeFunctionDecl(
        "family", cel::MakeMemberOverloadDecl("net_ip_family", i64, ip));
    if (!fam.ok()) return fam.status();
    if (auto s = builder.AddFunction(*fam); !s.ok()) return s;
  }
  struct Pred {
    absl::string_view fn;
    absl::string_view id;
  };
  for (const Pred& p : {
           Pred{"isLoopback", "net_ip_isLoopback"},
           Pred{"isUnspecified", "net_ip_isUnspecified"},
           Pred{"isGlobalUnicast", "net_ip_isGlobalUnicast"},
           Pred{"isLinkLocalUnicast", "net_ip_isLinkLocalUnicast"},
           Pred{"isLinkLocalMulticast", "net_ip_isLinkLocalMulticast"},
       }) {
    auto d = cel::MakeFunctionDecl(
        std::string(p.fn), cel::MakeMemberOverloadDecl(p.id, bln, ip));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }

  // --- CIDR receiver methods (overloaded contains*) ---
  {
    auto d = cel::MakeFunctionDecl(
        "containsIP",
        cel::MakeMemberOverloadDecl("net_cidr_containsIP_ip", bln, cidr, ip),
        cel::MakeMemberOverloadDecl("net_cidr_containsIP_string", bln, cidr,
                                    str));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }
  {
    auto d = cel::MakeFunctionDecl(
        "containsCIDR",
        cel::MakeMemberOverloadDecl("net_cidr_containsCIDR_cidr", bln, cidr,
                                    cidr),
        cel::MakeMemberOverloadDecl("net_cidr_containsCIDR_string", bln, cidr,
                                    str));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }
  {
    // <cidr>.ip() — receiver method named "ip" with a cidr target.
    // Note: name collides with the global ip(string) constructor; the
    // member-vs-global distinction is what disambiguates.  Probe whether
    // AddFunction accepts a member overload on the same name as the
    // existing global "ip" (it may need MergeFunction).
    auto d = cel::MakeFunctionDecl(
        "ip", cel::MakeMemberOverloadDecl("net_cidr_ip", ip, cidr));
    if (!d.ok()) return d.status();
    if (auto s = builder.MergeFunction(*d); !s.ok()) return s;
  }
  {
    auto d = cel::MakeFunctionDecl(
        "masked", cel::MakeMemberOverloadDecl("net_cidr_masked", cidr, cidr));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }
  {
    auto d = cel::MakeFunctionDecl(
        "prefixLength",
        cel::MakeMemberOverloadDecl("net_cidr_prefixLength", i64, cidr));
    if (!d.ok()) return d.status();
    if (auto s = builder.AddFunction(*d); !s.ok()) return s;
  }

  // --- the bare type literals: `net.IP` / `net.CIDR` as idents whose
  // type is type(net.IP) / type(net.CIDR).  Mirrors how optional.cc
  // registers `optional_type` as a VariableDecl of TypeType.
  if (auto s = builder.AddVariable(cel::MakeVariableDecl(
          "net.IP", cel::Type(cel::TypeType(arena, ip))));
      !s.ok()) {
    return s;
  }
  if (auto s = builder.AddVariable(cel::MakeVariableDecl(
          "net.CIDR", cel::Type(cel::TypeType(arena, cidr))));
      !s.ok()) {
    return s;
  }
  return absl::OkStatus();
}

// A single static arena that outlives every checker built in this TU.
google::protobuf::Arena* ProbeArena() {
  static auto* arena = new google::protobuf::Arena();
  return arena;
}

absl::StatusOr<CheckedExpr> ParseAndCheck(absl::string_view expression) {
  cel::ParserOptions parser_opts;
  cel::MacroRegistry registry;
  if (auto s = cel::RegisterStandardMacros(registry, parser_opts); !s.ok()) {
    return s;
  }

  auto source = cel::NewSource(expression, "<probe>");
  if (!source.ok()) return source.status();
  auto parsed =
      google::api::expr::parser::Parse(**source, registry, parser_opts);
  if (!parsed.ok()) return parsed.status();

  auto builder = cel::CreateTypeCheckerBuilder(
      google::protobuf::DescriptorPool::generated_pool());
  if (!builder.ok()) return builder.status();
  if (auto s = (*builder)->AddLibrary(cel::StandardCheckerLibrary()); !s.ok()) {
    return s;
  }
  if (auto s = RegisterNetDecls(**builder, ProbeArena()); !s.ok()) return s;

  auto checker = std::move(**builder).Build();
  if (!checker.ok()) return checker.status();

  auto ast = cel::CreateAstFromParsedExpr(*parsed);
  if (!ast.ok()) return ast.status();
  auto result = (*checker)->Check(std::move(*ast));
  if (!result.ok()) return result.status();
  if (!result->IsValid()) {
    return absl::InvalidArgumentError(
        absl::StrCat("type check failed: ", result->FormatError()));
  }
  auto checked_ast = result->ReleaseAst();
  if (!checked_ast.ok()) return checked_ast.status();
  CheckedExpr out;
  if (auto s = cel::AstToCheckedExpr(**checked_ast, &out); !s.ok()) return s;
  return out;
}

const Expr* FindCall(const Expr& root, absl::string_view fn) {
  if (root.expr_kind_case() == Expr::kCallExpr &&
      root.call_expr().function() == fn) {
    return &root;
  }
  switch (root.expr_kind_case()) {
    case Expr::kCallExpr:
      if (root.call_expr().has_target()) {
        if (const auto* x = FindCall(root.call_expr().target(), fn);
            x != nullptr) {
          return x;
        }
      }
      for (const auto& a : root.call_expr().args()) {
        if (const auto* x = FindCall(a, fn); x != nullptr) return x;
      }
      break;
    case Expr::kListExpr:
      for (const auto& e : root.list_expr().elements()) {
        if (const auto* x = FindCall(e, fn); x != nullptr) return x;
      }
      break;
    case Expr::kSelectExpr:
      if (const auto* x = FindCall(root.select_expr().operand(), fn);
          x != nullptr) {
        return x;
      }
      break;
    default:
      break;
  }
  return nullptr;
}

std::vector<std::string> OverloadIds(const CheckedExpr& ce, int64_t expr_id) {
  std::vector<std::string> out;
  auto it = ce.reference_map().find(expr_id);
  if (it == ce.reference_map().end()) return out;
  for (const auto& o : it->second.overload_id()) out.push_back(o);
  return out;
}

void Probe(absl::string_view expr) {
  std::cerr << "\n========================================================\n"
            << "EXPR: " << expr << "\n"
            << "--------------------------------------------------------\n";
  auto ce_or = ParseAndCheck(expr);
  if (!ce_or.ok()) {
    std::cerr << "RESULT: FAILED\n  status: " << ce_or.status() << "\n";
    return;
  }
  const auto& ce = ce_or.value();
  std::cerr << "RESULT: OK\n";

  std::vector<const Expr*> stack = {&ce.expr()};
  while (!stack.empty()) {
    const Expr* e = stack.back();
    stack.pop_back();
    if (e->expr_kind_case() == Expr::kCallExpr) {
      const auto& c = e->call_expr();
      auto ids = OverloadIds(ce, e->id());
      std::cerr << "  CALL fn=\"" << c.function() << "\""
                << " has_target=" << (c.has_target() ? "true" : "false")
                << " args=" << c.args_size() << " overload_ids=[";
      for (size_t i = 0; i < ids.size(); ++i) {
        std::cerr << (i ? ", " : "") << ids[i];
      }
      std::cerr << "]\n";
      if (c.has_target()) stack.push_back(&c.target());
      for (const auto& a : c.args()) stack.push_back(&a);
    } else if (e->expr_kind_case() == Expr::kSelectExpr) {
      const auto& s = e->select_expr();
      std::cerr << "  SELECT field=\"" << s.field() << "\" test_only="
                << (s.test_only() ? "true" : "false") << "\n";
      stack.push_back(&s.operand());
    } else if (e->expr_kind_case() == Expr::kIdentExpr) {
      std::cerr << "  IDENT name=\"" << e->ident_expr().name() << "\"";
      auto it = ce.reference_map().find(e->id());
      if (it != ce.reference_map().end()) {
        std::cerr << " ref.name=\"" << it->second.name() << "\"";
      }
      std::cerr << "\n";
    } else if (e->expr_kind_case() == Expr::kListExpr) {
      for (const auto& el : e->list_expr().elements()) stack.push_back(&el);
    }
  }
  auto it = ce.type_map().find(ce.expr().id());
  if (it != ce.type_map().end()) {
    std::cerr << "  ROOT TYPE: " << it->second.ShortDebugString() << "\n";
  }
  std::cerr << "--- CheckedExpr ---\n" << ce.DebugString() << "\n";
}

// ---- the corpus battery ----------------------------------------------

TEST(NetAstShape, FullCorpusBattery) {
  for (absl::string_view e : {
           // ip_type section
           "string(ip('192.168.0.1'))",
           "ip('192.168.0.1.0')",
           "isIP('192.168.0.1')",
           "isIP('192.168.0.1.0')",
           "ip.isCanonical('127.0.0.1')",
           "ip.isCanonical('2001:DB8::68')",
           "type(ip('192.168.0.1')) == net.IP",
           "isIP(cidr('192.168.0.0/24'))",  // expected COMPILE ERROR row
           // ipv4 / ipv6 receiver predicates
           "ip('192.168.0.1').family()",
           "ip('0.0.0.0').isUnspecified()",
           "ip('127.0.0.1').isLoopback()",
           "ip('192.168.0.1').isGlobalUnicast()",
           "ip('224.0.0.1').isLinkLocalMulticast()",
           "ip('169.254.169.254').isLinkLocalUnicast()",
           "ip('127.0.0.1') == ip('127.0.0.1')",
           "ip('::ffff:c0a8:1') == ip('192.168.0.1')",
           // cidr section
           "type(cidr('192.168.0.0/24')) == net.CIDR",
           "cidr('127.0.0.1/24') == cidr('127.0.0.1/24')",
           "cidr('192.168.0.0/24').containsIP(ip('192.168.0.1'))",
           "cidr('192.168.0.0/24').containsIP('192.168.0.1')",
           "cidr('192.168.0.0/24').containsCIDR(cidr('192.168.0.0/25'))",
           "cidr('192.168.0.0/24').containsCIDR('192.168.0.0/25')",
           "cidr('192.168.0.0/24').ip() == ip('192.168.0.0')",
           "cidr('192.168.0.1/24').masked() == cidr('192.168.0.0/24')",
           "cidr('192.168.0.0/24').prefixLength()",
           "string(cidr('2001:db8::/32'))",
           // bare type literals on their own
           "net.IP",
           "net.CIDR",
       }) {
    Probe(e);
  }
}

// ---- green/red assertions capturing the load-bearing answers --------

TEST(NetAstShape, IpIsGlobalCallWithResultNetIp) {
  auto ce_or = ParseAndCheck("ip('192.168.0.1')");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const Expr* call = FindCall(ce_or->expr(), "ip");
  ASSERT_NE(call, nullptr);
  EXPECT_FALSE(call->call_expr().has_target()) << "ip() must be a global call";
  EXPECT_THAT(OverloadIds(*ce_or, call->id()),
              ::testing::ElementsAre("net_ip_string"));
  auto it = ce_or->type_map().find(call->id());
  ASSERT_NE(it, ce_or->type_map().end());
  EXPECT_TRUE(it->second.has_abstract_type())
      << it->second.ShortDebugString();
  EXPECT_EQ(it->second.abstract_type().name(), "net.IP");
}

TEST(NetAstShape, ContainsIpIsReceiverCallTargetSet) {
  auto ce_or =
      ParseAndCheck("cidr('192.168.0.0/24').containsIP(ip('192.168.0.1'))");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const Expr* call = FindCall(ce_or->expr(), "containsIP");
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(call->call_expr().has_target())
      << "containsIP must reach codegen as a receiver call with target";
  EXPECT_EQ(call->call_expr().args_size(), 1);
  EXPECT_THAT(OverloadIds(*ce_or, call->id()),
              ::testing::ElementsAre("net_cidr_containsIP_ip"));
}

TEST(NetAstShape, ContainsIpStringOverloadResolves) {
  auto ce_or = ParseAndCheck("cidr('192.168.0.0/24').containsIP('192.168.0.1')");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const Expr* call = FindCall(ce_or->expr(), "containsIP");
  ASSERT_NE(call, nullptr);
  EXPECT_THAT(OverloadIds(*ce_or, call->id()),
              ::testing::ElementsAre("net_cidr_containsIP_string"));
}

TEST(NetAstShape, IpCanonicalNamespaceForm) {
  // Probe: how does the parser/checker shape `ip.isCanonical('x')`?
  // Global call named "ip.isCanonical", or a select/receiver?
  auto ce_or = ParseAndCheck("ip.isCanonical('127.0.0.1')");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const Expr* call = FindCall(ce_or->expr(), "ip.isCanonical");
  ASSERT_NE(call, nullptr)
      << "ip.isCanonical did not resolve as a global 'ip.isCanonical' call";
  EXPECT_FALSE(call->call_expr().has_target());
  EXPECT_THAT(OverloadIds(*ce_or, call->id()),
              ::testing::ElementsAre("net_ip_isCanonical_string"));
}

TEST(NetAstShape, BareNetIpTypeLiteralResolves) {
  auto ce_or = ParseAndCheck("net.IP");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  // Whatever shape (ident vs select) — just confirm it type-checks and
  // the root type is type(net.IP).
  auto it = ce_or->type_map().find(ce_or->expr().id());
  ASSERT_NE(it, ce_or->type_map().end());
  EXPECT_TRUE(it->second.has_type()) << it->second.ShortDebugString();
}

TEST(NetAstShape, TypeOfIpEqualsNetIp) {
  auto ce_or = ParseAndCheck("type(ip('192.168.0.1')) == net.IP");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  auto it = ce_or->type_map().find(ce_or->expr().id());
  ASSERT_NE(it, ce_or->type_map().end());
  EXPECT_TRUE(it->second.has_primitive()) << it->second.ShortDebugString();
}

TEST(NetAstShape, StringAndEqualityFallOutOfStandardOverloads) {
  // string(net.IP) via MergeFunction; == on net.IP via standard equals.
  EXPECT_TRUE(ParseAndCheck("string(ip('1.2.3.4'))").ok());
  EXPECT_TRUE(ParseAndCheck("string(cidr('1.2.3.0/24'))").ok());
  EXPECT_TRUE(ParseAndCheck("ip('1.2.3.4') == ip('1.2.3.4')").ok());
  EXPECT_TRUE(ParseAndCheck("cidr('1.2.3.0/24') == cidr('1.2.3.0/24')").ok());
}

TEST(NetAstShape, IsIpCidrArgIsCompileError) {
  // Corpus row `is_ip_cidr_compile_error` expects a no-matching-overload
  // failure for isIP(cidr(...)).  Confirm the checker rejects it.
  auto ce_or = ParseAndCheck("isIP(cidr('192.168.0.0/24'))");
  EXPECT_FALSE(ce_or.ok())
      << "isIP(net.CIDR) should fail to type-check (no matching overload)";
}

}  // namespace
}  // namespace celwasm::probes
