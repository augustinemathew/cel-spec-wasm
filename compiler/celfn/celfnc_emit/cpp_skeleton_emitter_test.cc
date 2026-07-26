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

CelType ListOf(CelType e) {
  return CelType::List(std::move(e));
}
CelType MapOf(CelType k, CelType v) {
  return CelType::Map(std::move(k), std::move(v));
}
CelType ProtoOf(std::string fqn) {
  return CelType::Message(std::move(fqn));
}

FunctionLibrary OneFn(absl::string_view fn_name, CelType ret,
                      std::vector<CelfnParam> params) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddPlugin(fn_name, std::move(ret), std::move(params))
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
  auto lib = OneFn("add", CelType::Int(),
                   {CelfnParam{false, CelType::Int(), "a"},
                    CelfnParam{false, CelType::Int(), "b"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("int64_t Add(int64_t a, int64_t b);"));
}

TEST(EmitUserFnsH, BoolReturnsBool) {
  auto lib = OneFn("allow", CelType::Bool(),
                   {CelfnParam{false, CelType::Bool(), "x"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("bool Allow(bool x);"));
}

TEST(EmitUserFnsH, DoubleSignature) {
  auto lib = OneFn("scale", CelType::Double(),
                   {CelfnParam{false, CelType::Double(), "x"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("double Scale(double x);"));
}

// ── String / bytes ───────────────────────────────────────────────

TEST(EmitUserFnsH, StringInStringViewOutString) {
  auto lib = OneFn("greet", CelType::String(),
                   {CelfnParam{false, CelType::String(), "name"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("std::string Greet(std::string_view name);"));
}

TEST(EmitUserFnsH, BytesIn) {
  auto lib =
      OneFn("size", CelType::Int(), {CelfnParam{false, CelType::Bytes(), "b"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("int64_t Size(const std::vector<uint8_t>& b);"));
}

// ── Records / time ────────────────────────────────────────────────

TEST(EmitUserFnsH, DurationParam) {
  auto lib = OneFn("ms", CelType::Int(),
                   {CelfnParam{false, CelType::Duration(), "d"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("int64_t Ms(::google::protobuf::Duration d);"));
  EXPECT_THAT(*t, HasSubstr("#include \"google/protobuf/duration.pb.h\""));
}

TEST(EmitUserFnsH, TimestampReturn) {
  auto lib = OneFn("now", CelType::Timestamp(), {});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("::google::protobuf::Timestamp Now();"));
  EXPECT_THAT(*t, HasSubstr("#include \"google/protobuf/timestamp.pb.h\""));
}

// ── Aggregates ────────────────────────────────────────────────────

TEST(EmitUserFnsH, ListIntParam) {
  auto lib = OneFn("sum", CelType::Int(),
                   {CelfnParam{false, ListOf(CelType::Int()), "xs"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("int64_t Sum(const std::vector<int64_t>& xs);"));
}

TEST(EmitUserFnsH, ListStringReturn) {
  auto lib = OneFn("words", ListOf(CelType::String()),
                   {CelfnParam{false, CelType::String(), "s"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("std::vector<std::string> Words("
                            "std::string_view s);"));
}

TEST(EmitUserFnsH, MapStringIntParam) {
  auto lib =
      OneFn("lookup", CelType::Int(),
            {CelfnParam{false, MapOf(CelType::String(), CelType::Int()), "m"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("int64_t Lookup(const std::map<std::string, int64_t>"
                        "& m);"));
}

TEST(EmitUserFnsH, NestedListOfMapStringListInt) {
  CelType nested = ListOf(MapOf(CelType::String(), ListOf(CelType::Int())));
  auto lib = OneFn("agg", CelType::Int(), {CelfnParam{false, nested, "xs"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("int64_t Agg(const std::vector<std::map<std::string, "
                        "std::vector<int64_t>>>& xs);"));
}

// ── Proto ─────────────────────────────────────────────────────────

TEST(EmitUserFnsH, ProtoParamUsesNamespaceQualified) {
  auto lib = OneFn("check", CelType::Bool(),
                   {CelfnParam{false, ProtoOf("acme.User"), "u"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("bool Check(const acme::User& u);"));
}

TEST(EmitUserFnsH, ProtoReturnByValue) {
  auto lib = OneFn("fetch", ProtoOf("acme.User"),
                   {CelfnParam{false, CelType::Int(), "id"}});
  auto t = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("acme::User Fetch(int64_t id);"));
}

// ── Output stability ─────────────────────────────────────────────

TEST(EmitUserFnsH, ByteForByteDeterministic) {
  auto lib = OneFn("sum", CelType::Int(),
                   {CelfnParam{false, ListOf(CelType::Int()), "xs"}});
  auto a = EmitUserFnsH(lib, kMod, {});
  auto b = EmitUserFnsH(lib, kMod, {});
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(*a, *b);
}

}  // namespace
}  // namespace celwasm::celfnc_emit
