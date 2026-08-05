// Semantic-validation tests for `ParseCelfnSource` and the
// programmatic `FunctionLibrary::Builder`.
//
// Companion to `celfn_parser_probe_test.cc` (which exercises the
// grammar layer only).  These tests cover the visitor + semantic
// validations on top — overload-id synthesis, alias collisions,
// map-key legality, etc.

#include "compiler/celfn/function_library.h"

#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::testing::HasSubstr;

CelType ListOf(CelType inner) {
  return CelType::List(std::move(inner));
}

CelType MapOf(CelType k, CelType v) {
  return CelType::Map(std::move(k), std::move(v));
}

TEST(FunctionLibrary, EmptyFileProducesEmptyResult) {
  auto r = ParseCelfnSource("");
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->module_name(), "");
  EXPECT_EQ(r->decls().size(), 0u);
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
  EXPECT_EQ(d.return_type.kind(), CelType::Kind::kString);
  ASSERT_EQ(d.params.size(), 1u);
  EXPECT_EQ(d.params[0].name, "s");
  EXPECT_EQ(d.params[0].type.kind(), CelType::Kind::kString);
  EXPECT_TRUE(d.params[0].is_receiver);
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

TEST(ArgkindSlugTest, ArgkindForTypeAndOptionalKinds) {
  // kType / kOptional have no `.celfn` grammar spelling, so the
  // overload-id path for them is only reachable programmatically —
  // pin the slugs directly.
  EXPECT_EQ(ArgkindSlug(CelType::Type()), "type");
  EXPECT_EQ(ArgkindSlug(CelType::Optional(CelType::Int())), "optional_int");
  EXPECT_EQ(ArgkindSlug(CelType::Optional(ListOf(CelType::String()))),
            "optional_list_string");
}

TEST(FunctionLibrary, HostDeclAllowedToCarryProtos) {
  // Host trampolines routinely carry proto messages
  // (externref → cel_host adapter).
  auto r = ParseCelfnSource(
      "bool @host.is_admin(proto(acme.User) u);"
      "proto(acme.User) @host.fetch_user(string id);");
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->decls().size(), 2u);
}

// ─── Other semantic validations ────────────────────────────────────

TEST(FunctionLibrary, RejectsBareHostDecl) {
  // `host.foo(...)` (without the `@`) — user likely meant `@host.foo`.
  auto r = ParseCelfnSource("bool host.allow(string r);");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("reserved alias"));
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("@host."));
}

TEST(FunctionLibrary, RejectsRemovedPluginPrefixAtParse) {
  // `@plugin.` was the removed sandboxed-wasm backend's spelling; the
  // grammar no longer has a production for it, so it fails as a plain
  // parse error.
  auto r = ParseCelfnSource("bool @plugin.allow(this string u, string r);");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("parse error"));
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("mismatched input 'plugin' expecting 'host'"));
}

TEST(FunctionLibrary, RejectsRemovedNativePrefixAtParse) {
  // `@native.` (CEL-defined bodies) was removed with the plugin
  // backend; the grammar no longer has a production for it.
  auto r = ParseCelfnSource(
      "bool @native.is_number(this string s) = s.matches(\"^[0-9]+$\");");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(r.status().message()), HasSubstr("parse error"));
  EXPECT_THAT(std::string(r.status().message()),
              HasSubstr("mismatched input 'native' expecting 'host'"));
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

TEST(FunctionLibraryBuilder, ProgrammaticHostDecl) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("upper", CelType::String(), {{true, CelType::String(), "s"}})
          .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  ASSERT_EQ(lib_or->decls().size(), 1u);
  const auto& d = lib_or->decls()[0];
  EXPECT_EQ(d.backend, CelfnDecl::Backend::kHost);
  EXPECT_EQ(d.overload_id, "upper_string");
  EXPECT_EQ(d.module_name, "cel_fn");
  EXPECT_TRUE(d.is_receiver);
}

TEST(FunctionLibraryBuilder, ProgrammaticDuplicateOverloadIdIsRejected) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("f", CelType::Bool(), {{false, CelType::Int(), "x"}})
          .AddHost("f", CelType::Bool(), {{false, CelType::Int(), "y"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("duplicate"));
}

// ─── Nested aggregates ─────────────────────────────────────────────
//
// Recursion at the decl surface is by **concrete expansion**, not a
// recursive type.  Every layer — IDL parse (ExtractType recurses on
// list_element and map_kv) and the Builder gate (FirstIllegalMapKey
// recurses) — handles arbitrary nesting uniformly.

TEST(FunctionLibraryBuilder, HostListOfListOfIntAccepted) {
  // `list<list<int>>` — the simplest nested aggregate; pins that
  // ExtractType + Build() recurse cleanly without an arity gate.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("flatten", CelType::Int(),
                   {CelfnParam{false, ListOf(CelType::List(CelType::Int())),
                               "rows"}})
          .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

TEST(FunctionLibraryBuilder, HostListOfMapStringListOfIntAccepted) {
  // `list<map<string, list<int>>>` — three layers deep, mixed
  // aggregate kinds.  Pins that the Builder gates don't reject a
  // legal nested decl, and that the legal map-key check
  // (FirstIllegalMapKey, which also recurses) accepts a
  // nested-inside-list map.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost(
              "merge",
              ListOf(MapOf(CelType::String(), CelType::List(CelType::Int()))),
              {CelfnParam{false,
                          ListOf(MapOf(CelType::String(),
                                       CelType::List(CelType::Int()))),
                          "groups"}})
          .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
  // Decl came through with the nested shape preserved (sanity:
  // not collapsed to a flat list by some accidental visitor).
  const auto& decl = lib_or->decls().front();
  ASSERT_EQ(decl.return_type.kind(), CelType::Kind::kList);
  EXPECT_EQ(decl.return_type.list_element().kind(), CelType::Kind::kMap);
  EXPECT_EQ(decl.return_type.list_element().map_key().kind(),
            CelType::Kind::kString);
  EXPECT_EQ(decl.return_type.list_element().map_value().kind(),
            CelType::Kind::kList);
}

TEST(FunctionLibraryBuilder, HostListOfIntAccepted) {
  // Sanity counterpart: aggregates pass cleanly.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("sum", CelType::Int(),
                   {CelfnParam{false, CelType::List(CelType::Int()), "xs"}})
          .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

TEST(FunctionLibraryBuilder, HostNullParamIsAccepted) {
  // CEL `null` (kNull) is a distinct kind and a legal declarable
  // shape.
  auto lib_or = FunctionLibrary::Builder()
                    .AddHost("accepts_null", CelType::Bool(),
                             {CelfnParam{false, CelType::Null(), "n"}})
                    .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

TEST(FunctionLibraryBuilder, HostNullReturnIsAccepted) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddHost("returns_null", CelType::Null(),
                             {CelfnParam{false, CelType::Int(), "x"}})
                    .Build();
  ASSERT_TRUE(lib_or.ok()) << lib_or.status();
}

// ─── langdef: map key kinds must be {bool,int,uint,string} ─────────
//
// Source-driven .celfn decls are already rejected at two earlier
// layers — the grammar (Celfn.g4 restricts mapKeyType to the four
// legal tokens; anything else is a parse error) and the visitor
// (ExtractType's mapType arm falls through to InvalidArgument on any
// other key text).  The Build()-time check covers the programmatic
// Builder path, where an embedder constructs a CelType in C++ and
// bypasses the grammar.

TEST(FunctionLibraryBuilder, RejectsHostMapWithDoubleKeyParam) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("f", CelType::Bool(),
                   {CelfnParam{false, MapOf(CelType::Double(), CelType::Int()),
                               "m"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("double"));
  EXPECT_THAT(std::string(lib_or.status().message()),
              HasSubstr("bool|int|uint|string"));
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("m"));
}

TEST(FunctionLibraryBuilder, RejectsHostMapWithBytesKeyReturn) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("lookup", MapOf(CelType::Bytes(), CelType::Int()),
                   {CelfnParam{false, CelType::String(), "q"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("bytes"));
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("lookup"));
}

TEST(FunctionLibraryBuilder, RejectsMapWithDoubleKeyNestedInList) {
  // list<map<double, int>> — illegal key buried inside a list.
  const CelType nested =
      CelType::List(MapOf(CelType::Double(), CelType::Int()));
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost("g", CelType::Bool(), {CelfnParam{false, nested, "xs"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("double"));
}

TEST(FunctionLibraryBuilder, RejectsMapWithDoubleKeyNestedAsMapValue) {
  // map<string, map<double, int>> — illegal key inside the value.
  auto lib_or =
      FunctionLibrary::Builder()
          .AddHost(
              "h", CelType::Bool(),
              {CelfnParam{false,
                          MapOf(CelType::String(),
                                MapOf(CelType::Double(), CelType::Int())),
                          "m"}})
          .Build();
  ASSERT_FALSE(lib_or.ok());
  EXPECT_THAT(std::string(lib_or.status().message()), HasSubstr("double"));
}

TEST(FunctionLibraryBuilder, AcceptsLegalMapKeyKinds) {
  // Sanity: every legal key kind passes through cleanly.
  for (const CelType& key :
       {CelType::Bool(), CelType::Int(), CelType::Uint(), CelType::String()}) {
    auto lib_or =
        FunctionLibrary::Builder()
            .AddHost(
                absl::StrCat("f_legal_key_", static_cast<int>(key.kind())),
                CelType::Bool(),
                {CelfnParam{false, MapOf(key, CelType::Int()), "m"}})
            .Build();
    EXPECT_TRUE(lib_or.ok()) << "key kind " << static_cast<int>(key.kind())
                             << ": " << lib_or.status();
  }
}

TEST(FunctionLibrary, AcceptsFullFileShape) {
  const std::string source = R"(
Module foo;

string @host.upper(this string s);
bool @host.is_admin(proto(acme.User) user);
proto(acme.User) @host.lookup_user(string user_id);
)";
  auto r = ParseCelfnSource(source);
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->module_name(), "foo");
  EXPECT_EQ(r->decls().size(), 3u);
}

// ── Backend spelling ────────────────────────────────────────────────

TEST(BackendPrefixTest, EveryBackendSpelledWithAtAndTrailingDot) {
  // The closed backend set, spelled exactly as `.celfn` source writes
  // it — the shared diagnostic vocabulary for Engine::BindFunction
  // messages.
  EXPECT_EQ(BackendPrefix(CelfnDecl::Backend::kHost), "@host.");
}

}  // namespace
}  // namespace celwasm
