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
#include "absl/strings/str_cat.h"
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
  auto r = ParseCelfnSource("bool rules.allow(this string user, string r);");
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
      "bool @native.is_number(this string s) = s.matches(\"^[0-9]+$\");");
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
  auto r = ParseCelfnSource("bool rules.is_admin(proto(acme.User) u);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("foreign-backed"));
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("proto"));
}

TEST(FunctionLibrary, RejectsForeignDeclWithProtoReturn) {
  auto r = ParseCelfnSource("proto(acme.User) rules.fetch(string id);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("proto"));
}

TEST(FunctionLibrary, RejectsForeignDeclWithProtoInList) {
  auto r =
      ParseCelfnSource("bool rules.any_admin(list<proto(acme.User)> users);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("proto"));
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
      "bool @native.is_adult(this proto(acme.User) u) = u.age >= 18;");
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->decls().size(), 1u);
  EXPECT_EQ(r->decls()[0].overload_id, "is_adult_message_acme_User");
}

// ─── Other semantic validations ────────────────────────────────────

TEST(FunctionLibrary, RejectsBareHostDecl) {
  // `host.foo(...)` (without the `@`) — user likely meant `@host.foo`.
  auto r = ParseCelfnSource("bool host.allow(string r);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("reserved alias"));
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("@host."));
}

TEST(FunctionLibrary, RejectsModuleDirectiveCollidingWithForeignAlias) {
  auto r = ParseCelfnSource(
      "Module rules;\n"
      "bool rules.allow(string r);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("collides"));
}

TEST(FunctionLibrary, RejectsCelDefinedFnWithoutModuleDirective) {
  auto r = ParseCelfnSource(
      "bool @native.is_number(this string s) = s.matches(\"a\");");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("no module name"));
}

TEST(FunctionLibrary, RejectsDuplicateOverloadId) {
  auto r = ParseCelfnSource(
      "bool @host.f(int x);"
      "bool @host.f(int y);");  // same sig
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("duplicate"));
}

TEST(FunctionLibrary, RejectsThisModifierOnNonFirstParam) {
  auto r = ParseCelfnSource("bool @host.f(int x, this string s);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("first parameter"));
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
  auto lib_or =
      FunctionLibrary::Builder()
          .AddCelDefined("isNum", Prim(CelfnType::Kind::kBool),
                         {{true, Prim(CelfnType::Kind::kString), "s"}},
                         "s.matches(\"a\")")
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()),
              HasSubstr("no module name"));
}

TEST(FunctionLibraryBuilder, ProgrammaticCelDefinedWithModuleName) {
  auto lib_or =
      FunctionLibrary::Builder()
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
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeign("rules", "is_admin", Prim(CelfnType::Kind::kBool),
                      {{false, proto_user, "u"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("proto"));
}

TEST(FunctionLibraryBuilder, ProgrammaticDuplicateOverloadIdIsRejected) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddHost("f", Prim(CelfnType::Kind::kBool),
                             {{false, Prim(CelfnType::Kind::kInt), "x"}})
                    .AddHost("f", Prim(CelfnType::Kind::kBool),
                             {{false, Prim(CelfnType::Kind::kInt), "y"}})
                    .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("duplicate"));
}

// ─── kForeignComponent v1: optional<T> rejection ───────────────────

// optional<T> as a declarable foreign-component argument or return
// shape was dropped from v1 (user direction 2026-06-03; the marshaling
// layer in eval/internal/cel_component.cc refuses kOptional outright).
// Build() catches the violation here, naming the offending decl, so
// the surprise isn't a runtime InvalidArgument deep inside a host
// callback or — pre-fix — an ABSL_CHECK(false) crash in
// parse_and_check.cc::CelfnTypeToCelType's stub arm.

CelfnType OptOfPrim(CelfnType::Kind inner) {
  CelfnType t;
  t.kind = CelfnType::Kind::kOptional;
  CelfnType i;
  i.kind = inner;
  t.optional_element.push_back(i);
  return t;
}

CelfnType ListOfPrim(CelfnType::Kind inner) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  CelfnType i;
  i.kind = inner;
  t.list_element.push_back(i);
  return t;
}

CelfnType ListOf(CelfnType inner) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  t.list_element.push_back(std::move(inner));
  return t;
}

TEST(FunctionLibraryBuilder, ForeignComponentOptionalReturnRejectedAtBuild) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "lookup", OptOfPrim(CelfnType::Kind::kInt),
              {CelfnParam{false, Prim(CelfnType::Kind::kString), "k"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("optional"));
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("lookup"));
}

TEST(FunctionLibraryBuilder, ForeignComponentOptionalParamRejectedAtBuild) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "f", Prim(CelfnType::Kind::kBool),
              {CelfnParam{false, OptOfPrim(CelfnType::Kind::kString), "u"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("optional"));
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("u"));
}

TEST(FunctionLibraryBuilder,
     ForeignComponentOptionalNestedInListRejectedAtBuild) {
  // list<optional<int>> — nested optional must be rejected too.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "g", Prim(CelfnType::Kind::kBool),
              {CelfnParam{false, ListOf(OptOfPrim(CelfnType::Kind::kInt)),
                          "xs"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("optional"));
}

TEST(FunctionLibraryBuilder, ForeignComponentTypeReturnRejectedAtBuild) {
  // kType (the CEL type-of-types) is permanently out of scope as a
  // foreign-component declarable shape (user direction; m24 §14).
  // The Lift/Lower for kType stays implemented in
  // eval/internal/cel_component.cc because other kCelFn / kHost
  // paths can still use it; only the foreign-component decl surface
  // is closed.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "type_of", Prim(CelfnType::Kind::kType),
              {CelfnParam{false, Prim(CelfnType::Kind::kString), "x"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("type"));
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("type_of"));
}

TEST(FunctionLibraryBuilder, ForeignComponentTypeParamRejectedAtBuild) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent(
                        "f", Prim(CelfnType::Kind::kBool),
                        {CelfnParam{false, Prim(CelfnType::Kind::kType), "t"}})
                    .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("type"));
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("t"));
}

TEST(FunctionLibraryBuilder, ForeignComponentTypeNestedInListRejectedAtBuild) {
  // list<type> — nested type must be rejected too, mirroring the
  // optional<T> nested rejection.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "g", Prim(CelfnType::Kind::kBool),
              {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kType)), "ts"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("type"));
}

TEST(FunctionLibraryBuilder,
     ForeignComponentTypeNestedInsideOptionalIsRejected) {
  // optional<type> — the optional check fires first (it's an outer
  // optional), but the test pins that nesting type inside any
  // rejected carrier is still rejected.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "h", Prim(CelfnType::Kind::kBool),
              {CelfnParam{false, OptOfPrim(CelfnType::Kind::kType), "ot"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  // Either "optional" or "type" naming the violation is acceptable;
  // the order of MentionsOptional vs MentionsType determines which
  // message wins.  The current order returns the optional error first.
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("optional"));
}

TEST(FunctionLibraryBuilder, ForeignComponentNullParamIsAccepted) {
  // CEL `null` (kNull) is a distinct kind from kOptional and stays
  // supported as a foreign-component declarable shape.  Wire-level
  // null encodes as `option<unit>` (a canonical-ABI detail), but
  // the IDL/decl-level kind is kNull, not kOptional, and
  // MentionsOptional must not flag it.
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent(
                        "accepts_null", Prim(CelfnType::Kind::kBool),
                        {CelfnParam{false, Prim(CelfnType::Kind::kNull), "n"}})
                    .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

TEST(FunctionLibraryBuilder, ForeignComponentNullReturnIsAccepted) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent(
                        "returns_null", Prim(CelfnType::Kind::kNull),
                        {CelfnParam{false, Prim(CelfnType::Kind::kInt), "x"}})
                    .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

// ─── kForeignComponent: nested aggregates ──────────────────────────
//
// Recursion at the foreign-fn surface is by **concrete expansion**,
// not a recursive type (m24 §6).  Every layer — IDL parse (ExtractType
// recurses on list_element and map_kv), Builder gates
// (MentionsOptional / MentionsType / FirstIllegalMapKey all recurse),
// and marshaling (Lift/Lower dispatch recurses on the same fields) —
// handles arbitrary nesting uniformly.  The matrix-layer cases at
// eval/internal/cel_component_test.cc::{ListListIntRaggedNesting,
// LargeNestedListLiftsAt10kLeafCells, LargeMapStringListIntLifts…}
// pin the *marshaling*; these pin the **decl-time acceptance**.

// Shared with the legal-map-key block further down.
CelfnType MapOf(CelfnType k, CelfnType v) {
  CelfnType t;
  t.kind = CelfnType::Kind::kMap;
  t.map_kv.push_back(std::move(k));
  t.map_kv.push_back(std::move(v));
  return t;
}

TEST(FunctionLibraryBuilder, ForeignComponentListOfListOfIntAccepted) {
  // `list<list<int>>` — the simplest nested aggregate; pins that
  // ExtractType + Build() recurse cleanly without an arity gate.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "flatten", Prim(CelfnType::Kind::kInt),
              {CelfnParam{false, ListOf(ListOfPrim(CelfnType::Kind::kInt)),
                          "rows"}})
          .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

TEST(FunctionLibraryBuilder, ForeignComponentListOfMapStringListOfIntAccepted) {
  // `list<map<string, list<int>>>` — the user-requested shape.  Three
  // layers deep, mixed aggregate kinds.  Pins that the Builder gates
  // don't reject a legal nested decl, and that the legal map-key
  // check (FirstIllegalMapKey, which also recurses) accepts a
  // nested-inside-list map.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "merge",
              ListOf(MapOf(Prim(CelfnType::Kind::kString),
                           ListOfPrim(CelfnType::Kind::kInt))),
              {CelfnParam{false,
                          ListOf(MapOf(Prim(CelfnType::Kind::kString),
                                       ListOfPrim(CelfnType::Kind::kInt))),
                          "groups"}})
          .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  // Decl came through with the nested shape preserved (sanity:
  // not collapsed to a flat list by some accidental visitor).
  const auto& decl = lib_or->decls().front();
  ASSERT_EQ(decl.return_type.kind, CelfnType::Kind::kList);
  ASSERT_EQ(decl.return_type.list_element.size(), 1u);
  EXPECT_EQ(decl.return_type.list_element[0].kind, CelfnType::Kind::kMap);
  ASSERT_EQ(decl.return_type.list_element[0].map_kv.size(), 2u);
  EXPECT_EQ(decl.return_type.list_element[0].map_kv[0].kind,
            CelfnType::Kind::kString);
  EXPECT_EQ(decl.return_type.list_element[0].map_kv[1].kind,
            CelfnType::Kind::kList);
}

TEST(FunctionLibraryBuilder,
     ForeignComponentNestedDeclWithIllegalMapKeyRejected) {
  // `list<map<double, int>>` — the legal-map-key check (m24 §A.5)
  // recurses through list_element; a buggy gate that only looked
  // at top-level map shape would let this through.
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent(
                        "f", Prim(CelfnType::Kind::kBool),
                        {CelfnParam{false,
                                    ListOf(MapOf(Prim(CelfnType::Kind::kDouble),
                                                 Prim(CelfnType::Kind::kInt))),
                                    "xs"}})
                    .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("double"));
}

TEST(FunctionLibraryBuilder,
     ForeignComponentNestedDeclWithOptionalDeepInsideRejected) {
  // `list<map<string, list<optional<int>>>>` — optional buried at
  // depth-4 still gets caught by MentionsOptional's recursion.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "f", Prim(CelfnType::Kind::kBool),
              {CelfnParam{
                  false,
                  ListOf(MapOf(Prim(CelfnType::Kind::kString),
                               ListOf(OptOfPrim(CelfnType::Kind::kInt)))),
                  "groups"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("optional"));
}

TEST(FunctionLibraryBuilder, ForeignComponentListOfIntAccepted) {
  // Sanity counterpart: non-optional aggregates pass cleanly.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "sum", Prim(CelfnType::Kind::kInt),
              {CelfnParam{false, ListOfPrim(CelfnType::Kind::kInt), "xs"}})
          .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

// ─── langdef: map key kinds must be {bool,int,uint,string} ─────────
//
// Source-driven .celfn decls are already rejected at two earlier
// layers — the grammar (Celfn.g4:108 restricts mapKeyType to the four
// legal tokens; anything else is a parse error) and the visitor
// (ExtractType's mapType arm at function_library.cc:429 falls through
// to InvalidArgument on any other key text).  The Build()-time check
// covers the programmatic Builder path, where an embedder constructs
// a CelfnType in C++ and bypasses the grammar.  Same shape as the
// MentionsProto check that catches proto-on-foreign-decls bypassing
// the grammar.

TEST(FunctionLibraryBuilder, RejectsForeignComponentMapWithDoubleKeyParam) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent("f", Prim(CelfnType::Kind::kBool),
                               {CelfnParam{false,
                                           MapOf(Prim(CelfnType::Kind::kDouble),
                                                 Prim(CelfnType::Kind::kInt)),
                                           "m"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("double"));
  EXPECT_THAT(std::string(lib_or.status().message()),
              HasSubstr("bool|int|uint|string"));
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("m"));
}

TEST(FunctionLibraryBuilder, RejectsForeignComponentMapWithBytesKeyReturn) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "lookup",
              MapOf(Prim(CelfnType::Kind::kBytes), Prim(CelfnType::Kind::kInt)),
              {CelfnParam{false, Prim(CelfnType::Kind::kString), "q"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("bytes"));
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("lookup"));
}

TEST(FunctionLibraryBuilder, RejectsMapWithDoubleKeyNestedInList) {
  // list<map<double, int>> — illegal key buried inside a list.
  CelfnType nested;
  nested.kind = CelfnType::Kind::kList;
  nested.list_element.push_back(
      MapOf(Prim(CelfnType::Kind::kDouble), Prim(CelfnType::Kind::kInt)));
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent("g", Prim(CelfnType::Kind::kBool),
                                         {CelfnParam{false, nested, "xs"}})
                    .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("double"));
}

TEST(FunctionLibraryBuilder, RejectsMapWithDoubleKeyNestedAsMapValue) {
  // map<string, map<double, int>> — illegal key inside the value.
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent(
                        "h", Prim(CelfnType::Kind::kBool),
                        {CelfnParam{false,
                                    MapOf(Prim(CelfnType::Kind::kString),
                                          MapOf(Prim(CelfnType::Kind::kDouble),
                                                Prim(CelfnType::Kind::kInt))),
                                    "m"}})
                    .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("double"));
}

TEST(FunctionLibraryBuilder, RejectsHostDeclWithDoubleMapKey) {
  // Backend-independent.  kHost trampolines are equally bound by the
  // langdef restriction; programmatic AddHost bypasses the grammar
  // just like AddForeignComponent does.
  auto lib_or = FunctionLibrary::Builder()
                    .AddHost("count", Prim(CelfnType::Kind::kInt),
                             {CelfnParam{false,
                                         MapOf(Prim(CelfnType::Kind::kDouble),
                                               Prim(CelfnType::Kind::kInt)),
                                         "m"}})
                    .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("double"));
}

TEST(FunctionLibraryBuilder, AcceptsLegalMapKeyKinds) {
  // Sanity: every legal key kind passes through cleanly.
  for (CelfnType::Kind k : {CelfnType::Kind::kBool, CelfnType::Kind::kInt,
                            CelfnType::Kind::kUint, CelfnType::Kind::kString}) {
    auto lib_or =
        FunctionLibrary::Builder()
            .AddForeignComponent(
                absl::StrCat("f_legal_key_", static_cast<int>(k)),
                Prim(CelfnType::Kind::kBool),
                {CelfnParam{false, MapOf(Prim(k), Prim(CelfnType::Kind::kInt)),
                            "m"}})
            .Build();
    EXPECT_TRUE(lib_or.ok())
        << "key kind " << static_cast<int>(k) << ": " << lib_or.status();
  }
}

TEST(FunctionLibraryBuilder, ForeignComponentProtoAdmitted) {
  // m24 §8: kForeignComponent admits proto(...) (crosses as bytes).
  // Counterpart to RejectsForeignDeclWithProtoArg for kForeign.
  CelfnType proto_user;
  proto_user.kind = CelfnType::Kind::kProto;
  proto_user.proto_fqn = "acme.User";
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent("is_admin", Prim(CelfnType::Kind::kBool),
                               {CelfnParam{false, proto_user, "u"}})
          .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

TEST(FunctionLibrary, AcceptsFullFileShape) {
  // Mirrors the canonical example from §3.4 of the design doc,
  // adjusted for the v1 cross-foreign-boundary constraint.
  const std::string source = R"(
Module foo;

bool @native.is_number(this string s) = s.matches("^[0-9]+$");
bool @native.is_adult(this proto(acme.User) u) = u.age >= 18;

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
