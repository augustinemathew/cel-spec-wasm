// generated_stub.cc emission tests — tests-first per CLAUDE.md.
//
// Each test pins one shape of export-body, walking the m24 §6 type
// matrix at top level + a handful of nested cases.  Compile-check
// (whether the emitter's output is valid C++) is the separate
// `stub_compile_check` cc_library that depends on the
// genrule-emitted text — see m26 §8.0.  These unit tests pin the
// *textual contract* (no compiler needed); the compile-check pins
// the *semantic contract*.

#include "compiler/celfn/celfnc_emit/cpp_stub_emitter.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "compiler/celfn/function_library.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm::celfnc_emit {
namespace {

using ::absl_testing::IsOk;
using ::testing::HasSubstr;
using ::testing::Not;

CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}
CelfnType ListOf(CelfnType e) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  t.list_element.push_back(std::move(e));
  return t;
}
CelfnType MapOf(CelfnType k, CelfnType v) {
  CelfnType t;
  t.kind = CelfnType::Kind::kMap;
  t.map_kv.push_back(std::move(k));
  t.map_kv.push_back(std::move(v));
  return t;
}
CelfnType ProtoOf(std::string fqn) {
  CelfnType t;
  t.kind = CelfnType::Kind::kProto;
  t.proto_fqn = std::move(fqn);
  return t;
}

FunctionLibrary OneFn(absl::string_view fn_name, CelfnType ret,
                      std::vector<CelfnParam> params) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(fn_name, std::move(ret), std::move(params))
          .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

constexpr absl::string_view kMod = "rules";
constexpr absl::string_view kPkg = "cel:customfn";

// ── snake → camel translation ─────────────────────────────────────

TEST(SnakeToCamel, BasicWord) {
  EXPECT_EQ(SnakeToCamel("allow"), "Allow");
  EXPECT_EQ(SnakeToCamel("allow_user"), "AllowUser");
  EXPECT_EQ(SnakeToCamel("sum_list_of_ints"), "SumListOfInts");
}

TEST(SnakeToCamel, LeadingUnderscoreOk) {
  // Leading `_` is unusual but should not crash; treat the next
  // character as the new word start.
  EXPECT_EQ(SnakeToCamel("_priv"), "Priv");
}

TEST(SnakeToCamel, EmptyStringEmpty) {
  EXPECT_EQ(SnakeToCamel(""), "");
}

// ── Preamble ──────────────────────────────────────────────────────

TEST(EmitStubCc, EmitsStandardIncludes) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitStubCc(*lib_or, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("#include \"author.h\""));
  EXPECT_THAT(*t, HasSubstr("#include \"codec.h\""));
  EXPECT_THAT(*t, HasSubstr("#include \"user_fns.h\""));
}

TEST(EmitStubCc, EmitsExtraIncludesBeforeCodecH) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t =
      EmitStubCc(*lib_or, kMod, kPkg, {"acme/user.pb.h", "shared/types.h"});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("#include \"acme/user.pb.h\""));
  EXPECT_THAT(*t, HasSubstr("#include \"shared/types.h\""));
}

// ── Scalar return ─────────────────────────────────────────────────

TEST(EmitStubCc, ScalarReturnUsesNativeCType) {
  // int @rules.add(int a, int b);
  auto lib = OneFn("add", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                    CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("int64_t exports_cel_customfn_fns_add_int_int("
                            "int64_t a, int64_t b)"));
  EXPECT_THAT(*t, HasSubstr("return rules::Add(a, b);"));
}

TEST(EmitStubCc, BoolReturnUsesBool) {
  auto lib = OneFn("allow", Prim(CelfnType::Kind::kBool),
                   {CelfnParam{false, Prim(CelfnType::Kind::kBool), "x"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("bool exports_cel_customfn_fns_allow_bool(bool x)"));
  EXPECT_THAT(*t, HasSubstr("return rules::Allow(x);"));
}

TEST(EmitStubCc, DoubleReturnUsesDouble) {
  auto lib = OneFn("scale", Prim(CelfnType::Kind::kDouble),
                   {CelfnParam{false, Prim(CelfnType::Kind::kDouble), "x"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("double exports_cel_customfn_fns_scale_double(double x"
                        ")"));
}

// ── String args / returns ─────────────────────────────────────────

TEST(EmitStubCc, StringReturnUsesOutParam) {
  // string @rules.greet(string name);
  auto lib = OneFn("greet", Prim(CelfnType::Kind::kString),
                   {CelfnParam{false, Prim(CelfnType::Kind::kString), "name"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("void exports_cel_customfn_fns_greet_string("
                            "author_string_t* name, author_string_t* ret)"));
  EXPECT_THAT(*t, HasSubstr("rules::codec::lower(ret, rules::Greet("
                            "rules::codec::lift(*name)));"));
}

TEST(EmitStubCc, StringArgScalarReturnLiftsButNoLower) {
  // int @rules.len(string s);
  auto lib = OneFn("len", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("int64_t exports_cel_customfn_fns_len_string("
                            "author_string_t* s)"));
  EXPECT_THAT(*t, HasSubstr("return rules::Len(rules::codec::lift(*s));"));
}

// ── Bytes args / returns ───────────────────────────────────────────

TEST(EmitStubCc, BytesArgScalarReturn) {
  // int @rules.size(bytes b);
  auto lib = OneFn("size", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kBytes), "b"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("author_list_u8_t* b"));
  EXPECT_THAT(*t, HasSubstr("rules::codec::lift(*b)"));
}

// ── List args / returns ────────────────────────────────────────────

TEST(EmitStubCc, ListIntArgScalarReturn) {
  auto lib =
      OneFn("sum", Prim(CelfnType::Kind::kInt),
            {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kInt)), "xs"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("author_list_s64_t* xs"));
  EXPECT_THAT(*t, HasSubstr("rules::codec::lift(*xs)"));
}

TEST(EmitStubCc, ListStringReturn) {
  auto lib = OneFn("words", ListOf(Prim(CelfnType::Kind::kString)),
                   {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("void exports_cel_customfn_fns_words_string("
                            "author_string_t* s, author_list_string_t* ret)"));
}

// ── Duration / Timestamp ──────────────────────────────────────────

TEST(EmitStubCc, DurationArgUsesExportsPrefixType) {
  auto lib = OneFn("ms", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kDuration), "d"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("exports_cel_customfn_fns_duration_t* d"));
}

TEST(EmitStubCc, TimestampReturnUsesExportsPrefixType) {
  auto lib = OneFn("now", Prim(CelfnType::Kind::kTimestamp), {});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("void exports_cel_customfn_fns_now("
                            "exports_cel_customfn_fns_timestamp_t* ret)"));
}

// ── Map args ──────────────────────────────────────────────────────

TEST(EmitStubCc, MapStringIntArgScalarReturn) {
  auto lib = OneFn("lookup", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false,
                               MapOf(Prim(CelfnType::Kind::kString),
                                     Prim(CelfnType::Kind::kInt)),
                               "m"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("author_list_tuple2_string_s64_t* m"));
  EXPECT_THAT(*t, HasSubstr("rules::codec::lift(*m)"));
}

// ── Proto args / returns ──────────────────────────────────────────

TEST(EmitStubCc, ProtoArgUsesLiftProtoTemplate) {
  // bool @rules.check(proto(acme.User) u);
  auto lib = OneFn("check", Prim(CelfnType::Kind::kBool),
                   {CelfnParam{false, ProtoOf("acme.User"), "u"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("author_list_u8_t* u"));
  EXPECT_THAT(*t, HasSubstr("rules::codec::lift_proto<acme::User>(*u)"));
}

TEST(EmitStubCc, ProtoReturnUsesLowerProtoTemplate) {
  auto lib = OneFn("fetch", ProtoOf("acme.User"),
                   {CelfnParam{false, Prim(CelfnType::Kind::kInt), "id"}});
  auto t = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("void exports_cel_customfn_fns_fetch_int("
                            "int64_t id, author_list_u8_t* ret)"));
  EXPECT_THAT(*t, HasSubstr("rules::codec::lower_proto<acme::User>(ret, "
                            "rules::Fetch(id));"));
}

// ── Multiple decls — one export each ─────────────────────────────

TEST(EmitStubCc, MultipleDeclsEmitMultipleExports) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddForeignComponent(
              "f1", Prim(CelfnType::Kind::kInt),
              {CelfnParam{false, Prim(CelfnType::Kind::kInt), "x"}})
          .AddForeignComponent(
              "f2", Prim(CelfnType::Kind::kBool),
              {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}})
          .Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitStubCc(*lib_or, kMod, kPkg, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("exports_cel_customfn_fns_f1_int"));
  EXPECT_THAT(*t, HasSubstr("exports_cel_customfn_fns_f2_string"));
  EXPECT_THAT(*t, HasSubstr("rules::F1"));
  EXPECT_THAT(*t, HasSubstr("rules::F2"));
}

// ── Output stability ─────────────────────────────────────────────

TEST(EmitStubCc, ByteForByteDeterministic) {
  auto lib =
      OneFn("sum", Prim(CelfnType::Kind::kInt),
            {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kInt)), "xs"}});
  auto a = EmitStubCc(lib, kMod, kPkg, {});
  auto b = EmitStubCc(lib, kMod, kPkg, {});
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(*a, *b);
}

}  // namespace
}  // namespace celwasm::celfnc_emit
