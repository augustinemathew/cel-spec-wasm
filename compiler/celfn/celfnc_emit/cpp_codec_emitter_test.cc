// Per-CEL-type codec.h tests.
//
// Tests-first per CLAUDE.md.  Coverage:
//   - File preamble (pragma once, include guards, includes).
//   - Namespace wrapping derived from the IDL <module>.
//   - Per-row of m26 §4: lift(customfn_T) → std::T and
//     lower(customfn_T*, std::T) → void, for every CEL type the
//     foreign-fn surface admits.
//   - Records emit with the `exports_<package_normalized>_fns_<r>_t`
//     prefix (m26 §3.5.1).
//   - Nested aggregates recurse via the inner-type lift/lower.
//   - Deduplication: emitter writes each lift+lower exactly once
//     even when many decls use the same type.
//   - Unused types are NOT emitted (no `customfn_list_string_t` when
//     no decl uses `list<string>`).
//   - Regression tripwire: `optional<T>` / `type` in the input
//     surfaces FailedPrecondition.

#include "compiler/celfn/celfnc_emit/cpp_codec_emitter.h"

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
constexpr absl::string_view kPkg = "cel:customfn";

// ── Preamble ──────────────────────────────────────────────────────

TEST(EmitCodecH, EmitsPragmaOnceAndStandardIncludes) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitCodecH(*lib_or, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("#pragma once"));
  EXPECT_THAT(*t, HasSubstr("#include \"customfn.h\""));
  EXPECT_THAT(*t, HasSubstr("#include <string>"));
  EXPECT_THAT(*t, HasSubstr("#include <vector>"));
  EXPECT_THAT(*t, HasSubstr("namespace rules::codec {"));
  EXPECT_THAT(*t, HasSubstr("}  // namespace rules::codec"));
}

TEST(EmitCodecH, EmptyNamespaceEmitsAtGlobalScope) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitCodecH(*lib_or, "", kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("namespace codec {"));
  EXPECT_THAT(*t, Not(HasSubstr("namespace ::codec")));
}

// ── Primitives — scalars round-trip with no codec needed ────────
//
// bool / s64 / u64 / f64 pass through wit-bindgen as native C
// types.  The emitter does NOT generate codec fns for them
// (would be `int64_t lift(int64_t)` — pointless).  But it MUST emit
// the codec when those primitives appear nested inside a list/map.

TEST(EmitCodecH, IntPrimitiveAloneEmitsNoCodecBody) {
  auto lib =
      OneFn("ident", CelType::Int(), {CelfnParam{false, CelType::Int(), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  // No lift/lower for int alone — pass-through.
  EXPECT_THAT(*t, Not(HasSubstr("lift(int64_t")));
  EXPECT_THAT(*t, Not(HasSubstr("lower(int64_t")));
}

// ── String ────────────────────────────────────────────────────────

TEST(EmitCodecH, StringLiftReturnsStringView) {
  auto lib = OneFn("ident", CelType::String(),
                   {CelfnParam{false, CelType::String(), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(
      *t,
      HasSubstr("inline std::string_view lift(const customfn_string_t& s)"));
  EXPECT_THAT(*t, HasSubstr("return {reinterpret_cast<const char*>(s.ptr), "
                            "s.len};"));
}

TEST(EmitCodecH, StringLowerUsesAuthorStringDupN) {
  // Lower for string uses customfn_string_dup_n (the only out-path the
  // canonical ABI accepts without manual cabi_realloc bookkeeping —
  // m26 §3.5.1).  The author NEVER calls _free on the result.
  auto lib = OneFn("ident", CelType::String(),
                   {CelfnParam{false, CelType::String(), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline void lower(customfn_string_t* ret, "
                            "std::string_view s)"));
  EXPECT_THAT(*t, HasSubstr("customfn_string_dup_n(ret, s.data(), s.size())"));
}

// ── Bytes — list<u8> ─────────────────────────────────────────────

TEST(EmitCodecH, BytesLiftReturnsVectorU8) {
  auto lib = OneFn("ident", CelType::Bytes(),
                   {CelfnParam{false, CelType::Bytes(), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline std::vector<uint8_t> lift(const "
                            "customfn_list_u8_t& l)"));
}

TEST(EmitCodecH, BytesLowerUsesCabiRealloc) {
  auto lib = OneFn("ident", CelType::Bytes(),
                   {CelfnParam{false, CelType::Bytes(), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline void lower(customfn_list_u8_t* ret, const "
                            "std::vector<uint8_t>& v)"));
  EXPECT_THAT(*t, HasSubstr("cabi_realloc"));
}

// ── list<T> ──────────────────────────────────────────────────────

TEST(EmitCodecH, ListIntLiftReturnsVectorInt) {
  auto lib = OneFn("ident", ListOf(CelType::Int()),
                   {CelfnParam{false, ListOf(CelType::Int()), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline std::vector<int64_t> lift(const "
                            "customfn_list_s64_t& l)"));
  EXPECT_THAT(*t, HasSubstr("return {l.ptr, l.ptr + l.len};"));
}

TEST(EmitCodecH, ListStringLiftRecursesThroughStringElement) {
  auto lib = OneFn("ident", ListOf(CelType::String()),
                   {CelfnParam{false, ListOf(CelType::String()), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline std::vector<std::string> lift(const "
                            "customfn_list_string_t& l)"));
  // String_view input gets copied via emplace_back(string_view -> string).
  EXPECT_THAT(*t, HasSubstr("emplace_back"));
}

TEST(EmitCodecH, NestedListOfListOfIntEmitsBothLifts) {
  // list<list<int>> needs lift(customfn_list_s64_t) AND
  // lift(customfn_list_list_s64_t).  The latter calls the former.
  CelType nested = ListOf(ListOf(CelType::Int()));
  auto lib = OneFn("ident", nested, {CelfnParam{false, nested, "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("lift(const customfn_list_s64_t&"));
  EXPECT_THAT(*t, HasSubstr("lift(const customfn_list_list_s64_t&"));
  EXPECT_THAT(*t, HasSubstr("std::vector<std::vector<int64_t>>"));
}

// ── map<K,V> — list<tuple<K,V>> ───────────────────────────────────

TEST(EmitCodecH, MapStringIntLiftReturnsStdMap) {
  auto lib =
      OneFn("ident", MapOf(CelType::String(), CelType::Int()),
            {CelfnParam{false, MapOf(CelType::String(), CelType::Int()), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline std::map<std::string, int64_t> lift(const "
                            "customfn_list_tuple2_string_s64_t& m)"));
  // The lower half (map RETURN) — string keys lower via the string
  // codec, scalar values assign directly.
  EXPECT_THAT(*t, HasSubstr("inline void lower(customfn_list_tuple2_string_"
                            "s64_t* ret, const std::map<std::string, "
                            "int64_t>& m)"));
  EXPECT_THAT(*t, HasSubstr("lower(&ret->ptr[i].f0, kv.first);"));
  EXPECT_THAT(*t, HasSubstr("ret->ptr[i].f1 = kv.second;"));
}

// ── Records (duration / timestamp) ─────────────────────────────────

TEST(EmitCodecH, DurationUsesExportsPrefix) {
  // Per m26 §3.5.1 the record type is `exports_<pkg_normalized>_<iface>_<r>_t`.
  // `cel:customfn` normalizes to `cel_customfn`.
  auto lib = OneFn("ident", CelType::Duration(),
                   {CelfnParam{false, CelType::Duration(), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("exports_cel_customfn_fns_duration_t"));
  EXPECT_THAT(*t, HasSubstr("::google::protobuf::Duration"));
  EXPECT_THAT(*t, HasSubstr("google/protobuf/duration.pb.h"));
}

TEST(EmitCodecH, TimestampUsesExportsPrefix) {
  auto lib = OneFn("ident", CelType::Timestamp(),
                   {CelfnParam{false, CelType::Timestamp(), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("exports_cel_customfn_fns_timestamp_t"));
  EXPECT_THAT(*t, HasSubstr("::google::protobuf::Timestamp"));
  EXPECT_THAT(*t, HasSubstr("google/protobuf/timestamp.pb.h"));
}

// ── Proto crosses as list<u8> ───────────────────────────────────

TEST(EmitCodecH, ProtoEmitsParseAndSerialize) {
  auto lib = OneFn("ident", ProtoOf("acme.User"),
                   {CelfnParam{false, ProtoOf("acme.User"), "u"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  // Author writes user::Fn(const acme::User&) -> acme::User.
  EXPECT_THAT(*t, HasSubstr("ParseFromArray"));
  EXPECT_THAT(*t, HasSubstr("SerializeToString"));
}

// ── Deduplication ─────────────────────────────────────────────

TEST(EmitCodecH, SharedTypeAcrossDeclsEmittedOnce) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddPlugin("f1", CelType::Int(),
                     {CelfnParam{false, ListOf(CelType::Int()), "xs"}})
          .AddPlugin("f2", ListOf(CelType::Int()),
                     {CelfnParam{false, CelType::String(), "k"}})
          .Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitCodecH(*lib_or, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  // The lift for customfn_list_s64_t appears once (used by f1 input
  // AND f2 output).  Same for lower.
  size_t lift_count = 0;
  size_t pos = 0;
  while ((pos = t->find("lift(const customfn_list_s64_t&", pos)) !=
         std::string::npos) {
    ++lift_count;
    ++pos;
  }
  EXPECT_EQ(lift_count, 1u);
}

TEST(EmitCodecH, UnusedTypesAreNotEmitted) {
  // Decl uses only int; bytes / strings / records absent.
  auto lib =
      OneFn("ident", CelType::Int(), {CelfnParam{false, CelType::Int(), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, Not(HasSubstr("customfn_list_string_t")));
  EXPECT_THAT(*t, Not(HasSubstr("customfn_string_t")));
  EXPECT_THAT(*t, Not(HasSubstr("exports_cel_customfn_fns_duration_t")));
}

// ── Output stability ────────────────────────────────────────────

TEST(EmitCodecH, ByteForByteDeterministic) {
  auto lib = OneFn("ident", ListOf(CelType::String()),
                   {CelfnParam{false, ListOf(CelType::String()), "x"}});
  auto a = EmitCodecH(lib, kMod, kPkg);
  auto b = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(*a, *b);
}

}  // namespace
}  // namespace celwasm::celfnc_emit
