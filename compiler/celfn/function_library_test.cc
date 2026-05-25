// M13 Slice B — semantic-validation tests for `ParseCelfnSource`.
//
// Companion to `celfn_parser_probe_test.cc` (which exercises the
// grammar layer only).  These tests cover the visitor + semantic
// validations on top — overload-id synthesis, proto-on-foreign
// rejection (§4.5.1), alias collisions, etc.

#include "compiler/celfn/function_library.h"

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::testing::HasSubstr;

TEST(FunctionLibrary, EmptyFileProducesEmptyResult) {
  auto r = ParseCelfnSource("");
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->module_name(), "");
  EXPECT_EQ(r->decls().size(), 0u);
  EXPECT_EQ(r->foreign_aliases().size(), 0u);
}

TEST(FunctionLibrary, ExtractsHostDecl) {
  auto r = ParseCelfnSource("string @host.upper(this string s);");
  ASSERT_TRUE(r.ok()) << r.status();
  ASSERT_EQ(r->decls().size(), 1u);
  const auto& d = r->decls()[0];
  EXPECT_EQ(d.backend, CelfnDecl::Backend::kHost);
  EXPECT_EQ(d.fn_name, "upper");
  EXPECT_EQ(d.module_name, "cel_fn");
  EXPECT_EQ(d.overload_id, "upper_string");
  EXPECT_TRUE(d.is_receiver);
  EXPECT_EQ(d.num_args, 2u);  // out_slot + 1 arg
  EXPECT_EQ(d.return_type.kind, CelfnType::Kind::kString);
  ASSERT_EQ(d.params.size(), 1u);
  EXPECT_EQ(d.params[0].name, "s");
  EXPECT_EQ(d.params[0].type.kind, CelfnType::Kind::kString);
  EXPECT_TRUE(d.params[0].is_receiver);
}

TEST(FunctionLibrary, ExtractsForeignDecl) {
  auto r = ParseCelfnSource(
      "bool rules.allow(this string user, string r);");
  ASSERT_TRUE(r.ok()) << r.status();
  ASSERT_EQ(r->decls().size(), 1u);
  const auto& d = r->decls()[0];
  EXPECT_EQ(d.backend, CelfnDecl::Backend::kForeign);
  EXPECT_EQ(d.fn_name, "allow");
  EXPECT_EQ(d.module_name, "rules");
  EXPECT_EQ(d.overload_id, "allow_string_string");
  EXPECT_EQ(d.num_args, 3u);  // out + 2 args
  ASSERT_EQ(r->foreign_aliases().size(), 1u);
  EXPECT_EQ(r->foreign_aliases()[0], "rules");
}

TEST(FunctionLibrary, ExtractsCelDefinedFn) {
  auto r = ParseCelfnSource(
      "Module foo;\n"
      "bool is_number(this string s) = s.matches(\"^[0-9]+$\");");
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->module_name(), "foo");
  ASSERT_EQ(r->decls().size(), 1u);
  const auto& d = r->decls()[0];
  EXPECT_EQ(d.backend, CelfnDecl::Backend::kCelDefined);
  EXPECT_EQ(d.fn_name, "is_number");
  EXPECT_EQ(d.module_name, "foo");
  EXPECT_EQ(d.overload_id, "is_number_string");
  EXPECT_THAT(d.body, HasSubstr("s.matches"));
}

TEST(FunctionLibrary, MultipleForeignAliasesPreservedInFirstUseOrder) {
  auto r = ParseCelfnSource(
      "bool rules.allow(string r);"
      "int policy.score(string u);"
      "bool rules.deny(string r);");
  ASSERT_TRUE(r.ok()) << r.status();
  ASSERT_EQ(r->foreign_aliases().size(), 2u);
  EXPECT_EQ(r->foreign_aliases()[0], "rules");
  EXPECT_EQ(r->foreign_aliases()[1], "policy");
}

TEST(FunctionLibrary, SynthesisesOverloadIdsForAllTypes) {
  auto r = ParseCelfnSource(
      "int @host.f1(int x);"
      "double @host.f2(double a, double b);"
      "bytes @host.f3(string s);"
      "Duration @host.f4(Timestamp t);"
      "list<int> @host.f5(list<int> xs);"
      "bool @host.f6(map<string, int> m);"
      "bool @host.f7(proto(acme.User) u);");
  ASSERT_TRUE(r.ok()) << r.status();
  ASSERT_EQ(r->decls().size(), 7u);
  EXPECT_EQ(r->decls()[0].overload_id, "f1_int");
  EXPECT_EQ(r->decls()[1].overload_id, "f2_double_double");
  EXPECT_EQ(r->decls()[2].overload_id, "f3_string");
  EXPECT_EQ(r->decls()[3].overload_id, "f4_timestamp");
  EXPECT_EQ(r->decls()[4].overload_id, "f5_list_int");
  EXPECT_EQ(r->decls()[5].overload_id, "f6_map_string_int");
  EXPECT_EQ(r->decls()[6].overload_id, "f7_message_acme_User");
}

// ─── §4.5.1 v1 cross-foreign-boundary constraint ───────────────────

TEST(FunctionLibrary, RejectsForeignDeclWithProtoArg) {
  auto r = ParseCelfnSource(
      "bool rules.is_admin(proto(acme.User) u);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("foreign-backed"));
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("proto"));
}

TEST(FunctionLibrary, RejectsForeignDeclWithProtoReturn) {
  auto r = ParseCelfnSource(
      "proto(acme.User) rules.fetch(string id);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("proto"));
}

TEST(FunctionLibrary, RejectsForeignDeclWithProtoInList) {
  auto r = ParseCelfnSource(
      "bool rules.any_admin(list<proto(acme.User)> users);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("proto"));
}

TEST(FunctionLibrary, HostDeclAllowedToCarryProtos) {
  // The constraint applies to FOREIGN decls only.  Host trampolines
  // routinely carry proto messages (externref → cel_host adapter).
  auto r = ParseCelfnSource(
      "bool @host.is_admin(proto(acme.User) u);"
      "proto(acme.User) @host.fetch_user(string id);");
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->decls().size(), 2u);
}

TEST(FunctionLibrary, CelDefinedAllowedToCarryProtos) {
  auto r = ParseCelfnSource(
      "Module foo;\n"
      "bool is_adult(this proto(acme.User) u) = u.age >= 18;");
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->decls().size(), 1u);
  EXPECT_EQ(r->decls()[0].overload_id, "is_adult_message_acme_User");
}

// ─── Other semantic validations ────────────────────────────────────

TEST(FunctionLibrary, RejectsBareHostDecl) {
  // `host.foo(...)` (without the `@`) — user likely meant `@host.foo`.
  auto r = ParseCelfnSource("bool host.allow(string r);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("reserved alias"));
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("@host."));
}

TEST(FunctionLibrary, RejectsModuleDirectiveCollidingWithForeignAlias) {
  auto r = ParseCelfnSource(
      "Module rules;\n"
      "bool rules.allow(string r);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("collides"));
}

TEST(FunctionLibrary, RejectsCelDefinedFnWithoutModuleDirective) {
  auto r = ParseCelfnSource(
      "bool is_number(this string s) = s.matches(\"a\");");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("no module name"));
}

TEST(FunctionLibrary, RejectsDuplicateOverloadId) {
  auto r = ParseCelfnSource(
      "bool @host.f(int x);"
      "bool @host.f(int y);");  // same sig
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("duplicate"));
}

TEST(FunctionLibrary, RejectsThisModifierOnNonFirstParam) {
  auto r = ParseCelfnSource(
      "bool @host.f(int x, this string s);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("first parameter"));
}

// ─── Programmatic Builder API tests ────────────────────────────────

// Helper: construct a CelfnType for a primitive kind.
CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}

TEST(FunctionLibraryBuilder, ProgrammaticHostDecl) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddHost("upper", Prim(CelfnType::Kind::kString),
                             {{true, Prim(CelfnType::Kind::kString), "s"}})
                    .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  ASSERT_EQ(lib_or->decls().size(), 1u);
  const auto& d = lib_or->decls()[0];
  EXPECT_EQ(d.backend, CelfnDecl::Backend::kHost);
  EXPECT_EQ(d.overload_id, "upper_string");
  EXPECT_EQ(d.module_name, "cel_fn");
  EXPECT_TRUE(d.is_receiver);
}

TEST(FunctionLibraryBuilder, ProgrammaticForeignDecl) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeign("rules", "allow", Prim(CelfnType::Kind::kBool),
                                {{true, Prim(CelfnType::Kind::kString), "u"},
                                 {false, Prim(CelfnType::Kind::kString), "r"}})
                    .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  ASSERT_EQ(lib_or->decls().size(), 1u);
  EXPECT_EQ(lib_or->decls()[0].overload_id, "allow_string_string");
  EXPECT_EQ(lib_or->decls()[0].module_name, "rules");
  ASSERT_EQ(lib_or->foreign_aliases().size(), 1u);
  EXPECT_EQ(lib_or->foreign_aliases()[0], "rules");
}

TEST(FunctionLibraryBuilder, ProgrammaticCelDefinedNeedsModuleName) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddCelDefined("isNum", Prim(CelfnType::Kind::kBool),
                                   {{true, Prim(CelfnType::Kind::kString), "s"}},
                                   "s.matches(\"a\")")
                    .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()),
              HasSubstr("no module name"));
}

TEST(FunctionLibraryBuilder, ProgrammaticCelDefinedWithModuleName) {
  auto lib_or = FunctionLibrary::Builder()
                    .SetModuleName("foo")
                    .AddCelDefined("isNum", Prim(CelfnType::Kind::kBool),
                                   {{true, Prim(CelfnType::Kind::kString), "s"}},
                                   "s.matches(\"a\")")
                    .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  EXPECT_EQ(lib_or->module_name(), "foo");
  ASSERT_EQ(lib_or->decls().size(), 1u);
  EXPECT_EQ(lib_or->decls()[0].overload_id, "isNum_string");
  EXPECT_EQ(lib_or->decls()[0].module_name, "foo");
  EXPECT_EQ(lib_or->decls()[0].body, "s.matches(\"a\")");
}

TEST(FunctionLibraryBuilder, ProgrammaticForeignWithProtoIsRejected) {
  CelfnType proto_user;
  proto_user.kind = CelfnType::Kind::kProto;
  proto_user.proto_fqn = "acme.User";
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeign("rules", "is_admin",
                                Prim(CelfnType::Kind::kBool),
                                {{false, proto_user, "u"}})
                    .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()),
              HasSubstr("proto"));
}

TEST(FunctionLibraryBuilder, ProgrammaticDuplicateOverloadIdIsRejected) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("f", Prim(CelfnType::Kind::kBool),
                   {{false, Prim(CelfnType::Kind::kInt), "x"}})
          .AddHost("f", Prim(CelfnType::Kind::kBool),
                   {{false, Prim(CelfnType::Kind::kInt), "y"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("duplicate"));
}

TEST(FunctionLibrary, AcceptsFullFileShape) {
  // Mirrors the canonical example from §3.4 of the design doc,
  // adjusted for the v1 cross-foreign-boundary constraint.
  const std::string source = R"(
Module foo;

bool is_number(this string s) = s.matches("^[0-9]+$");
bool is_adult(this proto(acme.User) u) = u.age >= 18;

string @host.upper(this string s);
bool @host.is_admin(proto(acme.User) user);
proto(acme.User) @host.lookup_user(string user_id);

bool rules.allow(this string user_id, string resource);
bool rules.deny(this string user_id, string resource);
int  policy.score(this string user_id);
)";
  auto r = ParseCelfnSource(source);
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->module_name(), "foo");
  EXPECT_EQ(r->decls().size(), 8u);
  ASSERT_EQ(r->foreign_aliases().size(), 2u);
  EXPECT_EQ(r->foreign_aliases()[0], "rules");
  EXPECT_EQ(r->foreign_aliases()[1], "policy");
}

}  // namespace
}  // namespace celwasm
