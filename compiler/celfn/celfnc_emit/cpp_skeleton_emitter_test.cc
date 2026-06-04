// user_fns.h skeleton emission tests — tests-first per CLAUDE.md.

#include "compiler/celfn/celfnc_emit/cpp_skeleton_emitter.h"

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
  auto lib_or = FunctionLibrary::Builder()
                    .AddForeignComponent(fn_name, std::move(ret),
                                         std::move(params))
                    .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

constexpr absl::string_view kMod = "rules";

// ── Preamble ──────────────────────────────────────────────────────

TEST(EmitUserFnsH, PragmaOnceAndStandardIncludes) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitUserFnsH(*lib_or, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("#pragma once"));
  EXPECT_THAT(*t, HasSubstr("#include <cstdint>"));
  EXPECT_THAT(*t, HasSubstr("#include <string>"));
  EXPECT_THAT(*t, HasSubstr("#include <string_view>"));
  EXPECT_THAT(*t, HasSubstr("#include <vector>"));
  EXPECT_THAT(*t, HasSubstr("namespace rules {"));
  EXPECT_THAT(*t, HasSubstr("}  // namespace rules"));
}

TEST(EmitUserFnsH, ExtraIncludesEmitted) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitUserFnsH(*lib_or, kMod, {"acme/user.pb.h"});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("#include \"acme/user.pb.h\""));
}

TEST(EmitUserFnsH, EmptyNamespaceGlobalScope) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitUserFnsH(*lib_or, "", {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, Not(HasSubstr("namespace  {")));
}

// ── Scalar signatures ─────────────────────────────────────────────

TEST(EmitUserFnsH, IntIntReturnInt) {
  auto lib = OneFn("add", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                    CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("int64_t Add(int64_t a, int64_t b);"));
}

TEST(EmitUserFnsH, BoolReturnsBool) {
  auto lib = OneFn("allow", Prim(CelfnType::Kind::kBool),
                   {CelfnParam{false, Prim(CelfnType::Kind::kBool), "x"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("bool Allow(bool x);"));
}

TEST(EmitUserFnsH, DoubleSignature) {
  auto lib = OneFn("scale", Prim(CelfnType::Kind::kDouble),
                   {CelfnParam{false, Prim(CelfnType::Kind::kDouble), "x"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("double Scale(double x);"));
}

// ── String / bytes ───────────────────────────────────────────────

TEST(EmitUserFnsH, StringInStringViewOutString) {
  auto lib = OneFn("greet", Prim(CelfnType::Kind::kString),
                   {CelfnParam{false, Prim(CelfnType::Kind::kString), "name"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("std::string Greet(std::string_view name);"));
}

TEST(EmitUserFnsH, BytesIn) {
  auto lib = OneFn("size", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kBytes), "b"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("int64_t Size(const std::vector<uint8_t>& b);"));
}

// ── Records / time ────────────────────────────────────────────────

TEST(EmitUserFnsH, DurationParam) {
  auto lib = OneFn("ms", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kDuration), "d"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("int64_t Ms(absl::Duration d);"));
  EXPECT_THAT(*t, HasSubstr("#include \"absl/time/time.h\""));
}

TEST(EmitUserFnsH, TimestampReturn) {
  auto lib = OneFn("now", Prim(CelfnType::Kind::kTimestamp), {});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("absl::Time Now();"));
}

// ── Aggregates ────────────────────────────────────────────────────

TEST(EmitUserFnsH, ListIntParam) {
  auto lib = OneFn(
      "sum", Prim(CelfnType::Kind::kInt),
      {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kInt)), "xs"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr(
                      "int64_t Sum(const std::vector<int64_t>& xs);"));
}

TEST(EmitUserFnsH, ListStringReturn) {
  auto lib = OneFn("words", ListOf(Prim(CelfnType::Kind::kString)),
                   {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("std::vector<std::string> Words("
                        "std::string_view s);"));
}

TEST(EmitUserFnsH, MapStringIntParam) {
  auto lib = OneFn(
      "lookup", Prim(CelfnType::Kind::kInt),
      {CelfnParam{false,
                  MapOf(Prim(CelfnType::Kind::kString),
                        Prim(CelfnType::Kind::kInt)),
                  "m"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("int64_t Lookup(const std::map<std::string, int64_t>"
                        "& m);"));
}

TEST(EmitUserFnsH, NestedListOfMapStringListInt) {
  CelfnType nested =
      ListOf(MapOf(Prim(CelfnType::Kind::kString),
                   ListOf(Prim(CelfnType::Kind::kInt))));
  auto lib = OneFn("agg", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, nested, "xs"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(
      *t,
      HasSubstr("int64_t Agg(const std::vector<std::map<std::string, "
                "std::vector<int64_t>>>& xs);"));
}

// ── Proto ─────────────────────────────────────────────────────────

TEST(EmitUserFnsH, ProtoParamUsesNamespaceQualified) {
  auto lib = OneFn("check", Prim(CelfnType::Kind::kBool),
                   {CelfnParam{false, ProtoOf("acme.User"), "u"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("bool Check(const acme::User& u);"));
}

TEST(EmitUserFnsH, ProtoReturnByValue) {
  auto lib = OneFn("fetch", ProtoOf("acme.User"),
                   {CelfnParam{false, Prim(CelfnType::Kind::kInt), "id"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("acme::User Fetch(int64_t id);"));
}

// ── Output stability ─────────────────────────────────────────────

TEST(EmitUserFnsH, ByteForByteDeterministic) {
  auto lib = OneFn(
      "sum", Prim(CelfnType::Kind::kInt),
      {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kInt)), "xs"}});
  auto a = EmitUserFnsH(lib, kMod, {});
  auto b = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(*a, *b);
}

}  // namespace
}  // namespace celwasm::celfnc_emit
