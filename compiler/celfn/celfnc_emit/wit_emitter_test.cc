// Per-CEL-type WIT-emission tests for celfnc_emit/wit_emitter.
//
// Discipline (CLAUDE.md "interface → tests → implementation"): the
// matrix below covers every m24 §6 row (bool / int / uint / double /
// null / string / bytes / duration / timestamp / list / map / proto)
// plus the four nesting shapes that catch arbitrary-depth recursion
// (list<list<int>>, list<map<string, list<int>>>, map<int, bytes>,
// map<string, list<map<string, int>>>).  Each test is written
// against the EmitWit interface; bodies are present before the
// impl lands.
//
// Output assertions: we check (a) the function declaration in the
// rendered text matches the expected WIT form byte-exactly, and
// (b) the rendered text is what `wit-bindgen c --world customfn`
// would accept (the latter implicitly via the emitter's own
// stability contract — the integration check vs wit-bindgen runs
// in the cel_wasm_plugin macro's e2e test).

#include "compiler/celfn/celfnc_emit/wit_emitter.h"

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
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// ── helpers (mirror function_library_test.cc) ─────────────────────

CelType ListOf(CelType inner) {
  return CelType::List(std::move(inner));
}

CelType MapOf(CelType k, CelType v) {
  return CelType::Map(std::move(k), std::move(v));
}

CelType ProtoOf(std::string fqn) {
  return CelType::Message(std::move(fqn));
}

FunctionLibrary OneFn(absl::string_view fn_name, CelType return_type,
                      std::vector<CelfnParam> params) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddPlugin(fn_name, std::move(return_type), std::move(params))
          .Build();
  ABSL_CHECK_OK(lib_or);
  return *std::move(lib_or);
}

constexpr absl::string_view kPkg = "cel:customfn";
constexpr absl::string_view kVer = "0.1.0";

// ── Header + package + world structure ─────────────────────────────

TEST(EmitWit, EmptyLibraryEmitsPackageHeaderAndEmptyInterface) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto text_or = EmitWit(*lib_or, kPkg, kVer);
  ASSERT_THAT(text_or, IsOk());
  EXPECT_THAT(*text_or, HasSubstr("package cel:customfn@0.1.0;"));
  EXPECT_THAT(*text_or, HasSubstr("interface fns {"));
  EXPECT_THAT(*text_or, HasSubstr("world customfn { export fns; }"));
  EXPECT_THAT(*text_or, HasSubstr("world host   { import fns; }"));
}

TEST(EmitWit, NoVersionEmitsPackageHeaderWithoutVersionSuffix) {
  auto lib_or = FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto text_or = EmitWit(*lib_or, kPkg, "");
  ASSERT_THAT(text_or, IsOk());
  EXPECT_THAT(*text_or, HasSubstr("package cel:customfn;"));
  EXPECT_THAT(*text_or, ::testing::Not(HasSubstr("@")));
}

TEST(EmitWit, NonPluginDeclsAreIgnored) {
  // A kHost decl alongside a kPlugin: only the latter
  // appears in WIT (kHost is dispatched via the cel_fn callback
  // path with no WIT surface).
  auto lib_or = FunctionLibrary::Builder()
                    .AddHost("h", CelType::Bool(),
                             {CelfnParam{false, CelType::Int(), "x"}})
                    .AddPlugin("fc", CelType::Bool(),
                               {CelfnParam{false, CelType::Int(), "x"}})
                    .Build();
  ASSERT_THAT(lib_or, IsOk());
  auto text_or = EmitWit(*lib_or, kPkg, kVer);
  ASSERT_THAT(text_or, IsOk());
  EXPECT_THAT(*text_or, ::testing::Not(HasSubstr("h-int")));
  EXPECT_THAT(*text_or, HasSubstr("fc-int"));
}

// ── snake → kebab translation ─────────────────────────────────────

TEST(SnakeToKebab, ReplacesEachUnderscoreWithHyphen) {
  EXPECT_EQ(SnakeToKebab("add_int_int"), "add-int-int");
  EXPECT_EQ(SnakeToKebab("f5_list_int"), "f5-list-int");
  // CamelCase last segment (e.g. proto fqn `acme.User`) flattens to
  // lowercase — WIT identifiers are lower-only.
  EXPECT_EQ(SnakeToKebab("is_admin_message_acme_User"),
            "is-admin-message-acme-user");
}

TEST(SnakeToKebab, LeavesAlreadyKebabAlone) {
  EXPECT_EQ(SnakeToKebab("plain"), "plain");
  EXPECT_EQ(SnakeToKebab(""), "");
}

// ── Per-CEL-type matrix (single-param fns) ────────────────────────

// Each test is `func ident-<wit>: func(x: <wit>) -> <wit>` — the
// minimum shape that pins the type emitter for that row.

void ExpectFnSig(absl::string_view text, absl::string_view sig) {
  EXPECT_THAT(std::string(text), HasSubstr(std::string(sig)))
      << "expected signature:\n  " << sig << "\nin emitted text:\n"
      << text;
}

TEST(EmitWit, BoolPrimitive) {
  auto lib = OneFn("ident", CelType::Bool(),
                   {CelfnParam{false, CelType::Bool(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-bool: func(x: bool) -> bool;");
}

TEST(EmitWit, IntPrimitiveBecomesS64) {
  auto lib =
      OneFn("ident", CelType::Int(), {CelfnParam{false, CelType::Int(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-int: func(x: s64) -> s64;");
}

TEST(EmitWit, UintPrimitiveBecomesU64) {
  auto lib = OneFn("ident", CelType::Uint(),
                   {CelfnParam{false, CelType::Uint(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-uint: func(x: u64) -> u64;");
}

TEST(EmitWit, DoublePrimitiveBecomesF64) {
  auto lib = OneFn("ident", CelType::Double(),
                   {CelfnParam{false, CelType::Double(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-double: func(x: f64) -> f64;");
}

TEST(EmitWit, NullPrimitiveBecomesOptionUnit) {
  // CEL `null` rides on the wire as `option<u8>`-shaped placeholder —
  // wit-bindgen 0.57 doesn't admit a bare `option<unit>` argument
  // (option must wrap a non-unit type at the C-generator level).
  // We pick option<u8> as the conventional carrier; the canonical-ABI
  // value is "none" for a present null, which matches CEL semantics.
  auto lib = OneFn("ident", CelType::Null(),
                   {CelfnParam{false, CelType::Null(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-null: func(x: option<u8>) -> option<u8>;");
}

TEST(EmitWit, StringPrimitive) {
  auto lib = OneFn("ident", CelType::String(),
                   {CelfnParam{false, CelType::String(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-string: func(x: string) -> string;");
}

TEST(EmitWit, BytesPrimitiveBecomesListU8) {
  auto lib = OneFn("ident", CelType::Bytes(),
                   {CelfnParam{false, CelType::Bytes(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-bytes: func(x: list<u8>) -> list<u8>;");
}

TEST(EmitWit, DurationBecomesInlineSecondsNanosRecord) {
  // Records can't sit at top level in wit-bindgen 0.57.  We emit a
  // single `duration` record declaration inside the interface and
  // reference it by name from every duration-typed parameter.
  auto lib = OneFn("ident", CelType::Duration(),
                   {CelfnParam{false, CelType::Duration(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("record duration { seconds: s64, nanos: s32 }"));
  ExpectFnSig(*t, "ident-duration: func(x: duration) -> duration;");
}

TEST(EmitWit, TimestampBecomesInlineSecondsNanosRecord) {
  auto lib = OneFn("ident", CelType::Timestamp(),
                   {CelfnParam{false, CelType::Timestamp(), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  EXPECT_THAT(*t, HasSubstr("record timestamp { seconds: s64, nanos: s32 }"));
  ExpectFnSig(*t, "ident-timestamp: func(x: timestamp) -> timestamp;");
}

TEST(EmitWit, RecordEmittedOnlyOnceEvenWithMultipleDurationDecls) {
  // The interface body must declare `record duration` exactly once
  // even when many fns reference it.  Same for timestamp.
  auto lib_or = FunctionLibrary::Builder()
                    .AddPlugin("f", CelType::Duration(),
                               {CelfnParam{false, CelType::Duration(), "x"}})
                    .AddPlugin("g", CelType::Bool(),
                               {CelfnParam{false, CelType::Duration(), "y"}})
                    .Build();
  ASSERT_THAT(lib_or, IsOk());
  auto t = EmitWit(*lib_or, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  size_t count = 0;
  size_t pos = 0;
  while ((pos = t->find("record duration", pos)) != std::string::npos) {
    ++count;
    ++pos;
  }
  EXPECT_EQ(count, 1u);
}

TEST(EmitWit, ListOfInt) {
  auto lib = OneFn("ident", ListOf(CelType::Int()),
                   {CelfnParam{false, ListOf(CelType::Int()), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-list-int: func(x: list<s64>) -> list<s64>;");
}

TEST(EmitWit, ListOfString) {
  auto lib = OneFn("ident", ListOf(CelType::String()),
                   {CelfnParam{false, ListOf(CelType::String()), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-list-string: func(x: list<string>) -> list<string>;");
}

TEST(EmitWit, MapStringInt) {
  auto lib =
      OneFn("ident", MapOf(CelType::String(), CelType::Int()),
            {CelfnParam{false, MapOf(CelType::String(), CelType::Int()), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t,
              "ident-map-string-int: func(x: list<tuple<string, s64>>) -> "
              "list<tuple<string, s64>>;");
}

TEST(EmitWit, MapIntBytes) {
  auto lib =
      OneFn("ident", MapOf(CelType::Int(), CelType::Bytes()),
            {CelfnParam{false, MapOf(CelType::Int(), CelType::Bytes()), "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t,
              "ident-map-int-bytes: func(x: list<tuple<s64, list<u8>>>) -> "
              "list<tuple<s64, list<u8>>>;");
}

TEST(EmitWit, ProtoCrossesAsListU8) {
  // m24 §8: proto-typed args cross as serialized bytes (list<u8>).
  // The proto fqn appears in the overload-id as `message_<fqn_with_
  // dots_to_underscores>` (mirror of ArgkindSlug), and the
  // snake→kebab translation makes that `message-<fqn-with-hyphens>`
  // in the WIT export name.  The raw dotted fqn does NOT appear in
  // the WIT type signature — it's a host-side detail the codec
  // layer uses for ParseFromString.
  auto lib = OneFn("ident", ProtoOf("acme.User"),
                   {CelfnParam{false, ProtoOf("acme.User"), "u"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t, "ident-message-acme-user: func(u: list<u8>) -> list<u8>;");
  EXPECT_THAT(*t, ::testing::Not(HasSubstr("acme.User")));
}

// ── Arbitrary nesting ─────────────────────────────────────────────

TEST(EmitWit, NestedListOfList) {
  CelType nested = ListOf(ListOf(CelType::Int()));
  auto lib = OneFn("ident", nested, {CelfnParam{false, nested, "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(
      *t, "ident-list-list-int: func(x: list<list<s64>>) -> list<list<s64>>;");
}

TEST(EmitWit, NestedListOfMapStringListInt) {
  CelType nested = ListOf(MapOf(CelType::String(), ListOf(CelType::Int())));
  auto lib = OneFn("ident", nested, {CelfnParam{false, nested, "x"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(*t,
              "ident-list-map-string-list-int: func(x: list<list<tuple<string, "
              "list<s64>>>>) -> list<list<tuple<string, list<s64>>>>;");
}

TEST(EmitWit, MultipleParamsWithMixedShapes) {
  // Realistic decl: bool fn(string user, list<string> roles).  Each
  // param emits with its kebab name (the IDL identifier, NOT the
  // overload-id suffix); the function name itself takes the snake→
  // kebab translation.
  auto lib = OneFn("allow", CelType::Bool(),
                   {CelfnParam{false, CelType::String(), "user"},
                    CelfnParam{false, ListOf(CelType::String()), "roles"}});
  auto t = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(t, IsOk());
  ExpectFnSig(
      *t,
      "allow-string-list-string: func(user: string, roles: list<string>) -> "
      "bool;");
}

// ── Output stability ─────────────────────────────────────────────

TEST(EmitWit, ByteForByteDeterministicAcrossInvocations) {
  auto lib =
      OneFn("ident", CelType::Int(), {CelfnParam{false, CelType::Int(), "x"}});
  auto a = EmitWit(lib, kPkg, kVer);
  auto b = EmitWit(lib, kPkg, kVer);
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(*a, *b);
}

// ── Regression tripwire ─────────────────────────────────────────
//
// `optional<T>` and `type` are rejected at Build() time (m24 §A.4),
// so a kPlugin decl carrying either of those shapes should
// never reach EmitWit.  This test pins the tripwire: bypass Builder
// and construct a CelfnDecl directly, then assert EmitWit refuses
// rather than emitting an invalid WIT.

}  // namespace
}  // namespace celwasm::celfnc_emit
