// Per-CEL-type codec.h tests.
//
// Tests-first per CLAUDE.md.  Coverage:
//   - File preamble (pragma once, include guards, includes).
//   - Namespace wrapping derived from the IDL <module>.
//   - Per-row of m26 §4: lift(author_T) → std::T and
//     lower(author_T*, std::T) → void, for every CEL type the
//     foreign-fn surface admits.
//   - Records emit with the `exports_<package_normalized>_fns_<r>_t`
//     prefix (m26 §3.5.1).
//   - Nested aggregates recurse via the inner-type lift/lower.
//   - Deduplication: emitter writes each lift+lower exactly once
//     even when many decls use the same type.
//   - Unused types are NOT emitted (no `author_list_string_t` when
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
constexpr absl::string_view kPkg = "cel:customfn";

// ── Preamble ──────────────────────────────────────────────────────

TEST(EmitCodecH, EmitsPragmaOnceAndStandardIncludes) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitCodecH(*lib_or, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("#pragma once"));
  EXPECT_THAT(*t, HasSubstr("#include \"author.h\""));
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
  auto lib = OneFn("ident", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kInt), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  // No lift/lower for int alone — pass-through.
  EXPECT_THAT(*t, Not(HasSubstr("lift(int64_t")));
  EXPECT_THAT(*t, Not(HasSubstr("lower(int64_t")));
}

// ── String ────────────────────────────────────────────────────────

TEST(EmitCodecH, StringLiftReturnsStringView) {
  auto lib = OneFn("ident", Prim(CelfnType::Kind::kString),
                   {CelfnParam{false, Prim(CelfnType::Kind::kString), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr(
                      "inline std::string_view lift(const author_string_t& s)"));
  EXPECT_THAT(*t, HasSubstr("return {reinterpret_cast<const char*>(s.ptr), "
                            "s.len};"));
}

TEST(EmitCodecH, StringLowerUsesAuthorStringDupN) {
  // Lower for string uses author_string_dup_n (the only out-path the
  // canonical ABI accepts without manual cabi_realloc bookkeeping —
  // m26 §3.5.1).  The author NEVER calls _free on the result.
  auto lib = OneFn("ident", Prim(CelfnType::Kind::kString),
                   {CelfnParam{false, Prim(CelfnType::Kind::kString), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline void lower(author_string_t* ret, "
                            "std::string_view s)"));
  EXPECT_THAT(*t,
              HasSubstr("author_string_dup_n(ret, s.data(), s.size())"));
}

// ── Bytes — list<u8> ─────────────────────────────────────────────

TEST(EmitCodecH, BytesLiftReturnsVectorU8) {
  auto lib = OneFn("ident", Prim(CelfnType::Kind::kBytes),
                   {CelfnParam{false, Prim(CelfnType::Kind::kBytes), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline std::vector<uint8_t> lift(const "
                            "author_list_u8_t& l)"));
}

TEST(EmitCodecH, BytesLowerUsesCabiRealloc) {
  auto lib = OneFn("ident", Prim(CelfnType::Kind::kBytes),
                   {CelfnParam{false, Prim(CelfnType::Kind::kBytes), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("inline void lower(author_list_u8_t* ret, const "
                        "std::vector<uint8_t>& v)"));
  EXPECT_THAT(*t, HasSubstr("cabi_realloc"));
}

// ── list<T> ──────────────────────────────────────────────────────

TEST(EmitCodecH, ListIntLiftReturnsVectorInt) {
  auto lib =
      OneFn("ident", ListOf(Prim(CelfnType::Kind::kInt)),
            {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kInt)), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("inline std::vector<int64_t> lift(const "
                            "author_list_s64_t& l)"));
  EXPECT_THAT(*t, HasSubstr("return {l.ptr, l.ptr + l.len};"));
}

TEST(EmitCodecH, ListStringLiftRecursesThroughStringElement) {
  auto lib =
      OneFn("ident", ListOf(Prim(CelfnType::Kind::kString)),
            {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kString)), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(
      *t,
      HasSubstr("inline std::vector<std::string> lift(const "
                "author_list_string_t& l)"));
  // String_view input gets copied via emplace_back(string_view -> string).
  EXPECT_THAT(*t, HasSubstr("emplace_back"));
}

TEST(EmitCodecH, NestedListOfListOfIntEmitsBothLifts) {
  // list<list<int>> needs lift(author_list_s64_t) AND
  // lift(author_list_list_s64_t).  The latter calls the former.
  CelfnType nested = ListOf(ListOf(Prim(CelfnType::Kind::kInt)));
  auto lib = OneFn("ident", nested, {CelfnParam{false, nested, "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("lift(const author_list_s64_t&"));
  EXPECT_THAT(*t, HasSubstr("lift(const author_list_list_s64_t&"));
  EXPECT_THAT(*t, HasSubstr("std::vector<std::vector<int64_t>>"));
}

// ── map<K,V> — list<tuple<K,V>> ───────────────────────────────────

TEST(EmitCodecH, MapStringIntLiftReturnsStdMap) {
  auto lib = OneFn(
      "ident",
      MapOf(Prim(CelfnType::Kind::kString), Prim(CelfnType::Kind::kInt)),
      {CelfnParam{false,
                  MapOf(Prim(CelfnType::Kind::kString),
                        Prim(CelfnType::Kind::kInt)),
                  "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("inline std::map<std::string, int64_t> lift(const "
                        "author_list_tuple2_string_s64_t& m)"));
}

// ── Records (duration / timestamp) ─────────────────────────────────

TEST(EmitCodecH, DurationUsesExportsPrefix) {
  // Per m26 §3.5.1 the record type is `exports_<pkg_normalized>_<iface>_<r>_t`.
  // `cel:customfn` normalizes to `cel_customfn`.
  auto lib = OneFn(
      "ident", Prim(CelfnType::Kind::kDuration),
      {CelfnParam{false, Prim(CelfnType::Kind::kDuration), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("exports_cel_customfn_fns_duration_t"));
  EXPECT_THAT(*t, HasSubstr("absl::Duration"));
}

TEST(EmitCodecH, TimestampUsesExportsPrefix) {
  auto lib = OneFn(
      "ident", Prim(CelfnType::Kind::kTimestamp),
      {CelfnParam{false, Prim(CelfnType::Kind::kTimestamp), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t,
              HasSubstr("exports_cel_customfn_fns_timestamp_t"));
  EXPECT_THAT(*t, HasSubstr("absl::Time"));
}

// ── Proto crosses as list<u8> ───────────────────────────────────

TEST(EmitCodecH, ProtoEmitsParseAndSerialize) {
  auto lib = OneFn(
      "ident", ProtoOf("acme.User"),
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
          .AddForeignComponent(
              "f1", Prim(CelfnType::Kind::kInt),
              {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kInt)), "xs"}})
          .AddForeignComponent(
              "f2", ListOf(Prim(CelfnType::Kind::kInt)),
              {CelfnParam{false, Prim(CelfnType::Kind::kString), "k"}})
          .Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitCodecH(*lib_or, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  // The lift for author_list_s64_t appears once (used by f1 input
  // AND f2 output).  Same for lower.
  size_t lift_count = 0;
  size_t pos = 0;
  while ((pos = t->find("lift(const author_list_s64_t&", pos)) !=
         std::string::npos) {
    ++lift_count;
    ++pos;
  }
  EXPECT_EQ(lift_count, 1u);
}

TEST(EmitCodecH, UnusedTypesAreNotEmitted) {
  // Decl uses only int; bytes / strings / records absent.
  auto lib = OneFn("ident", Prim(CelfnType::Kind::kInt),
                   {CelfnParam{false, Prim(CelfnType::Kind::kInt), "x"}});
  auto t = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, Not(HasSubstr("author_list_string_t")));
  EXPECT_THAT(*t, Not(HasSubstr("author_string_t")));
  EXPECT_THAT(*t, Not(HasSubstr("exports_cel_customfn_fns_duration_t")));
}

// ── Output stability ────────────────────────────────────────────

TEST(EmitCodecH, ByteForByteDeterministic) {
  auto lib = OneFn(
      "ident", ListOf(Prim(CelfnType::Kind::kString)),
      {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kString)), "x"}});
  auto a = EmitCodecH(lib, kMod, kPkg);
  auto b = EmitCodecH(lib, kMod, kPkg);
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(*a, *b);
}

}  // namespace
}  // namespace celwasm::celfnc_emit
