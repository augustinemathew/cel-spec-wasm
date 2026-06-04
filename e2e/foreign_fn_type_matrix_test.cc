// e2e: exhaustive type-matrix coverage for the m24 foreign-component
// custom-fn boundary — the `kForeignComponent` backend dispatched via
// `Engine::AddComponent(component_bytes, lib)`.
//
// Sibling to `e2e/host_fn_type_matrix_test.cc` (the in-process host-fn
// matrix), structurally identical: one section per CEL type from §6 of
// `doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md`,
// each section covers BOTH directions (arg + return) plus the §10
// boundary inputs, plus the §11 negative-coverage rows (wrong arity,
// missing export at AddComponent, fn-returning-eval-error, component
// trap, 3VL absorption).
//
// ── Status ─────────────────────────────────────────────────────────────
//
// Every test in this file is `GTEST_SKIP()`'d today with a verified
// blocker citation, per the CLAUDE.md "Verified-dead / not-yet-supported
// → GTEST_SKIP() with a named blocker" discipline.  The test bodies
// below carry the exact assertion they will make once the impl lands —
// closing the milestone is "delete the SKIP line and confirm green."
//
// The three blockers, by layer, today (2026-06-03):
//
//   B0 — eval/engine.{h,cc} `Engine::AddComponent` returns
//        `Unimplemented`.  Until it instantiates a Component-Model
//        component and binds typed host callbacks for each kForeignComponent
//        decl in the lib (m24 §3.5), no foreign-component fn is reachable
//        from `Instance::Eval`.  This is the dominant blocker — almost
//        every test here cites B0.
//   B1 — `compiler/celfn/function_library.h` has `kType` and `kOptional`
//        in `CelfnType::Kind` only as forward-declared identifiers; the
//        celfn IDL parser still has no `type` / `option(...)` keyword
//        (`function_library.cc:251` `ExtractType`).  Cells that depend on
//        building decls via the *celfn-source* path SKIP on B1.
//        Programmatic-Builder paths (which most tests here use) sidestep
//        B1.
//   B2 — `shared/type.h` exposes `CelType::Type()` but neither
//        `CelType::Optional(T)` nor `Value::Optional(T)` exists; the
//        Activation cannot bind an `optional<T>` value, and the type
//        checker cannot type-check an `optional<T>`-typed variable.
//        Cells that bind / type-check an optional<T> on the embedder
//        side SKIP on B2 even after B0 lands.
//
// ── How this file maps to the §6 type table (m24) ──────────────────────
//
//   bool          — Bool section, arg + return + boundary.
//   int           — Int section, arg + return + INT64_MIN / INT64_MAX.
//   uint          — Uint section, arg + return + UINT64_MAX.
//   double        — Double section, arg + return + ±0 / ±Inf / NaN / DBL boundaries.
//   string        — String section, arg + return + empty / NUL / UTF-8 / long.
//   bytes         — Bytes section, arg + return + empty / 0xFF / long.
//   null          — Null section, arg (as `optional<unit>` per m24 §6) + return.
//   duration      — Duration section, min / max seconds, nanos boundary.
//   timestamp     — Timestamp section, min / max seconds, nanos boundary.
//   type          — Type section, arg + return (cites B2 on bind path).
//   optional<T>   — Optional section (covers each T from §6), cites B2.
//   list<T>       — List section for each scalar T + nested `list<list>` + `list<map>`.
//   map<K,V>      — Map section for each valid key kind + nested + missing-key.
//   proto(fqn)    — Proto section (the m24 §8 "crosses as bytes" path).
//
// ── How this file maps to the §10 boundary matrix ──────────────────────
//
//   The §10 row "* covered by e2e/foreign_component_fixtures/stub_demo/
//   driver_main.cc today (17
//   cases)" identifies the cells already proven at the component level
//   (no eval-side dispatch yet).  This file is the *pipeline* matrix
//   — compile → plan → AddComponent → eval → assert — and goes wider
//   than the stub-demo (e.g. each map key kind, each list element kind).
//
// ── How this file maps to the §11 negative coverage ────────────────────
//
//   The "Negative" section at the bottom of this file holds:
//     - wrong-arity registration (declared 2-arg, component exports 3-arg)
//     - missing export at AddComponent
//     - fn returning eval-error → CEL_ERROR at the call site
//     - component trap → host absl::Status from Eval
//     - 3VL absorption — error/unknown arg short-circuits before marshal
//
// ── Component bytes ────────────────────────────────────────────────────
//
// Until the celfnc generator (m24 §5 + §9 + §13) emits per-test
// components, this file passes an empty `component_bytes` span; every
// AddComponent call short-circuits on the B0 Unimplemented stub before
// looking at the bytes.  When B0 lands, the un-skip commit replaces the
// span with bytes loaded via `LoadComponent("<section>")` — a helper
// pointing into a bazel-built per-section `.wasm` artifact under
// `e2e/foreign_component_fixtures/`.

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/internal/cel_host.h"  // HostListBacking / HostMapBacking Size()
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::celwasm::testdata::Customer;

// ─────────────────────────────────────────────────────────────────────
// SKIP blocker citations — referenced by every test until B0/B1/B2
// each land.
// ─────────────────────────────────────────────────────────────────────

constexpr absl::string_view kBlockerB0 =
    "blocked on m24 §3.5 — Engine::AddComponent currently returns "
    "Unimplemented (eval/engine.cc); the Component-Model instantiation + "
    "typed-export validation + per-fn host-callback binding is not yet "
    "wired. Un-skip when AddComponent stops returning Unimplemented and "
    "binds the component's typed exports to the cel_fn dispatch path.";

constexpr absl::string_view kBlockerB1 =
    "blocked on m24 §6 + the celfn IDL parser — CelfnType::Kind has "
    "kType / kOptional declared but the .celfn grammar "
    "(compiler/celfn/function_library.cc ExtractType, ~line 251) has no "
    "`type` / `option(...)` rules. Programmatic-Builder paths sidestep "
    "this; tests SKIPped on B1 build decls from celfn source.";

constexpr absl::string_view kBlockerB2 =
    "blocked on shared/eval support for optional<T> — neither "
    "CelType::Optional(T) nor Value::Optional(T) exists today, so the "
    "embedder cannot bind an optional<T> arg through Activation, and the "
    "checker cannot type-check an optional<T>-typed variable. Un-skip "
    "once Optional lands at both layers.";

// ─────────────────────────────────────────────────────────────────────
// Plumbing — build (compiler, program), Plan an engine, register the
// component-backed lib, evaluate.  Mirrors host_fn_type_matrix_test.cc's
// shape; the *single* surface difference is `Engine::AddComponent` in
// place of `AddFunction` / `AddTypedFunction`.
// ─────────────────────────────────────────────────────────────────────

CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}

CelfnType ListOf(CelfnType elem) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  t.list_element.push_back(std::move(elem));
  return t;
}

CelfnType MapOf(CelfnType key, CelfnType value) {
  CelfnType t;
  t.kind = CelfnType::Kind::kMap;
  t.map_kv.push_back(std::move(key));
  t.map_kv.push_back(std::move(value));
  return t;
}

CelfnType ProtoOf(absl::string_view fqn) {
  CelfnType t;
  t.kind = CelfnType::Kind::kProto;
  t.proto_fqn = std::string(fqn);
  return t;
}

CelfnType OptionalOf(CelfnType elem) {
  CelfnType t;
  t.kind = CelfnType::Kind::kOptional;
  t.optional_element.push_back(std::move(elem));
  return t;
}

CelfnType TypeOfTypes() {
  CelfnType t;
  t.kind = CelfnType::Kind::kType;
  return t;
}

struct DeclVar {
  std::string name;
  CelType type;
};

// Build a FunctionLibrary with one foreign-component fn.
absl::StatusOr<FunctionLibrary> ForeignLibOne(absl::string_view fn_name,
                                              CelfnType return_type,
                                              std::vector<CelfnParam> params) {
  return FunctionLibrary::Builder()
      .AddForeignComponent(fn_name, std::move(return_type), std::move(params))
      .Build();
}

// Common pre-AddComponent pipeline.  Tests today SKIP before invoking
// this; once B0 lands the bodies call it.
absl::StatusOr<Value> RunWithComponent(
    absl::string_view fn_decl_celfn, absl::string_view expr,
    const std::vector<DeclVar>& vars, const FunctionLibrary& lib,
    absl::Span<const uint8_t> component_bytes, const Activation& act) {
  auto b = Compiler::NewBuilder();
  for (const auto& v : vars) {
    b.DeclareVariable(v.name, v.type);
  }
  b.AddFunction(fn_decl_celfn);  // declares the overload to the checker
  auto compiler = std::move(b).Build();
  if (!compiler.ok()) return compiler.status();
  auto program = compiler->Compile(expr);
  if (!program.ok()) return program.status();
  auto engine = Engine::NewBuilder().Build();
  if (!engine.ok()) return engine.status();
  if (auto st = engine->AddComponent(component_bytes, lib); !st.ok()) {
    return st;
  }
  auto instance = engine->Plan(*program);
  if (!instance.ok()) return instance.status();
  return instance->Eval(act);
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — bool
// ═════════════════════════════════════════════════════════════════════

TEST(ForeignComponentTypeMatrix, BoolArgComponentSeesBoundValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("echo_bool", Prim(CelfnType::Kind::kBool),
                           {{false, Prim(CelfnType::Kind::kBool), "b"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("b", Value::Bool(true));
  auto v = RunWithComponent("bool rules.echo_bool(bool b);", "echo_bool(b)",
                            {{"b", CelType::Bool()}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

TEST(ForeignComponentTypeMatrix, BoolReturnComponentEmitsValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("always_false", Prim(CelfnType::Kind::kBool), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("bool rules.always_false();", "always_false()", {},
                            *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_FALSE(*v->AsBool());
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — int (with INT64_MIN / INT64_MAX from §10)
// ═════════════════════════════════════════════════════════════════════

struct Int64Case {
  std::string label;
  int64_t in;
};
class IntBoundary : public testing::TestWithParam<Int64Case> {};

TEST_P(IntBoundary, ArgRoundTripsBoundaryValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("echo_int", Prim(CelfnType::Kind::kInt),
                           {{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("x", Value::Int(GetParam().in));
  auto v = RunWithComponent("int rules.echo_int(int x);", "echo_int(x)",
                            {{"x", CelType::Int()}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), GetParam().in);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, IntBoundary,
    testing::Values(Int64Case{"zero", 0}, Int64Case{"neg_one", -1},
                    Int64Case{"int64_min", std::numeric_limits<int64_t>::min()},
                    Int64Case{"int64_max", std::numeric_limits<int64_t>::max()}),
    [](const testing::TestParamInfo<Int64Case>& info) {
      return info.param.label;
    });

TEST(ForeignComponentTypeMatrix, IntReturnComponentEmitsInt64Min) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("int_min", Prim(CelfnType::Kind::kInt), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("int rules.int_min();", "int_min()", {}, *lib, {},
                            act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), std::numeric_limits<int64_t>::min());
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — uint (with UINT64_MAX from §10)
// ═════════════════════════════════════════════════════════════════════

struct Uint64Case {
  std::string label;
  uint64_t in;
};
class UintBoundary : public testing::TestWithParam<Uint64Case> {};

TEST_P(UintBoundary, ArgRoundTripsBoundaryValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("echo_uint", Prim(CelfnType::Kind::kUint),
                           {{false, Prim(CelfnType::Kind::kUint), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("x", Value::Uint(GetParam().in));
  auto v = RunWithComponent("uint rules.echo_uint(uint x);", "echo_uint(x)",
                            {{"x", CelType::Uint()}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsUint(), GetParam().in);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, UintBoundary,
    testing::Values(Uint64Case{"zero", uint64_t{0}}, Uint64Case{"one", 1},
                    Uint64Case{"uint64_max",
                               std::numeric_limits<uint64_t>::max()}),
    [](const testing::TestParamInfo<Uint64Case>& info) {
      return info.param.label;
    });

TEST(ForeignComponentTypeMatrix, UintReturnComponentEmitsUintMax) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("uint_max", Prim(CelfnType::Kind::kUint), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v =
      RunWithComponent("uint rules.uint_max();", "uint_max()", {}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsUint(), std::numeric_limits<uint64_t>::max());
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — double (with ±0 / ±Inf / NaN / DBL boundaries from §10)
// ═════════════════════════════════════════════════════════════════════

struct DoubleCase {
  std::string label;
  double in;
};
class DoubleBoundary : public testing::TestWithParam<DoubleCase> {};

TEST_P(DoubleBoundary, ArgRoundTripsBoundaryValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("echo_double", Prim(CelfnType::Kind::kDouble),
                           {{false, Prim(CelfnType::Kind::kDouble), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("x", Value::Double(GetParam().in));
  auto v = RunWithComponent("double rules.echo_double(double x);",
                            "echo_double(x)", {{"x", CelType::Double()}}, *lib,
                            {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  const double in = GetParam().in;
  const double got = *v->AsDouble();
  if (in != in) {
    EXPECT_NE(got, got);
  } else {
    EXPECT_EQ(got, in);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, DoubleBoundary,
    testing::Values(DoubleCase{"zero", 0.0}, DoubleCase{"negzero", -0.0},
                    DoubleCase{"posinf", 1.0 / 0.0},
                    DoubleCase{"neginf", -1.0 / 0.0},
                    DoubleCase{"nan", 0.0 / 0.0},
                    DoubleCase{"dbl_max", std::numeric_limits<double>::max()},
                    DoubleCase{"dbl_min_pos",
                               std::numeric_limits<double>::denorm_min()}),
    [](const testing::TestParamInfo<DoubleCase>& info) {
      return info.param.label;
    });

TEST(ForeignComponentTypeMatrix, DoubleReturnComponentEmitsNan) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("nanval", Prim(CelfnType::Kind::kDouble), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v =
      RunWithComponent("double rules.nanval();", "nanval()", {}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  const double got = *v->AsDouble();
  EXPECT_NE(got, got);
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — string (empty / single / NUL / UTF-8 / long, from §10)
// ═════════════════════════════════════════════════════════════════════

struct StringCase {
  std::string label;
  std::string in;
};
class StringBoundary : public testing::TestWithParam<StringCase> {};

TEST_P(StringBoundary, ArgRoundTripsBoundaryValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("echo_string", Prim(CelfnType::Kind::kString),
                           {{false, Prim(CelfnType::Kind::kString), "s"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("s", Value::String(GetParam().in));
  auto v = RunWithComponent("string rules.echo_string(string s);",
                            "echo_string(s)", {{"s", CelType::String()}}, *lib,
                            {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(std::string(*v->AsString()), GetParam().in);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, StringBoundary,
    testing::Values(StringCase{"empty", ""}, StringCase{"single", "a"},
                    StringCase{"utf8_latin1", "héllo"},
                    StringCase{"utf8_cjk", "日本語"},
                    StringCase{"embedded_nul", std::string("hi\0there", 8)},
                    StringCase{"long", std::string(4096, 'x')}),
    [](const testing::TestParamInfo<StringCase>& info) {
      return info.param.label;
    });

TEST(ForeignComponentTypeMatrix, StringReturnComponentEmitsEmpty) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("empty_str", Prim(CelfnType::Kind::kString), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("string rules.empty_str();", "empty_str()", {},
                            *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->AsString()->empty());
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — bytes (empty / NUL run / 0xFF run / long, from §10)
// ═════════════════════════════════════════════════════════════════════

struct BytesCase {
  std::string label;
  std::string in;
};
class BytesBoundary : public testing::TestWithParam<BytesCase> {};

TEST_P(BytesBoundary, ArgRoundTripsBoundaryValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("echo_bytes", Prim(CelfnType::Kind::kBytes),
                           {{false, Prim(CelfnType::Kind::kBytes), "b"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("b", Value::Bytes(GetParam().in));
  auto v = RunWithComponent("bytes rules.echo_bytes(bytes b);", "echo_bytes(b)",
                            {{"b", CelType::Bytes()}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(std::string(*v->AsBytes()), GetParam().in);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, BytesBoundary,
    testing::Values(BytesCase{"empty", ""},
                    BytesCase{"single_nul", std::string(1, '\0')},
                    BytesCase{"ff_run", std::string(8, '\xff')},
                    BytesCase{"long", std::string(4096, '\x01')}),
    [](const testing::TestParamInfo<BytesCase>& info) {
      return info.param.label;
    });

TEST(ForeignComponentTypeMatrix, BytesReturnComponentEmitsEmpty) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("empty_bytes", Prim(CelfnType::Kind::kBytes), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("bytes rules.empty_bytes();", "empty_bytes()", {},
                            *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->AsBytes()->empty());
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — null (m24 §6: `option<unit>` / `std::monostate` — rarely a
// declared param; the realistic shape is the catch-all `value` resource
// on the dynamic path)
// ═════════════════════════════════════════════════════════════════════

TEST(ForeignComponentTypeMatrix, NullArgComponentObservesAbsence) {
  GTEST_SKIP() << kBlockerB0;
  // The CEL `null` literal is admitted by the celfn IDL (the host-fn
  // matrix demonstrates this); the foreign-component path forwards it
  // as `option<unit> = none` per §6.
  auto lib = ForeignLibOne("is_null", Prim(CelfnType::Kind::kBool),
                           {{false, Prim(CelfnType::Kind::kNull), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("bool rules.is_null(null x);", "is_null(null)", {},
                            *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

TEST(ForeignComponentTypeMatrix, NullReturnComponentEmitsNull) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("make_null", Prim(CelfnType::Kind::kNull), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("null rules.make_null();", "make_null()", {}, *lib,
                            {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsNull());
}

// ═════════════════════════════════════════════════════════════════════
// TEMPORAL — duration (min/max seconds, sub-second boundary, from §10)
// ═════════════════════════════════════════════════════════════════════

struct DurationCase {
  std::string label;
  absl::Duration in;
};
class DurationBoundary : public testing::TestWithParam<DurationCase> {};

TEST_P(DurationBoundary, ArgRoundTripsBoundaryValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("echo_dur", Prim(CelfnType::Kind::kDuration),
                           {{false, Prim(CelfnType::Kind::kDuration), "d"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("d", Value::Duration(GetParam().in));
  auto v = RunWithComponent("Duration rules.echo_dur(Duration d);",
                            "echo_dur(d)", {{"d", CelType::Duration()}}, *lib,
                            {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsDuration(), GetParam().in);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, DurationBoundary,
    testing::Values(DurationCase{"zero", absl::ZeroDuration()},
                    DurationCase{"neg", -absl::Seconds(1)},
                    DurationCase{"max_seconds",
                                 absl::Seconds(315576000000)},
                    DurationCase{"sub_nano", absl::Nanoseconds(1)}));

// ═════════════════════════════════════════════════════════════════════
// TEMPORAL — timestamp (min / y2000 / y9999 / pre-epoch, from §10)
// ═════════════════════════════════════════════════════════════════════

struct TimestampCase {
  std::string label;
  absl::Time in;
};
class TimestampBoundary : public testing::TestWithParam<TimestampCase> {};

TEST_P(TimestampBoundary, ArgRoundTripsBoundaryValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("echo_ts", Prim(CelfnType::Kind::kTimestamp),
                           {{false, Prim(CelfnType::Kind::kTimestamp), "t"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("t", Value::Timestamp(GetParam().in));
  auto v = RunWithComponent("Timestamp rules.echo_ts(Timestamp t);",
                            "echo_ts(t)", {{"t", CelType::Timestamp()}}, *lib,
                            {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsTimestamp(), GetParam().in);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, TimestampBoundary,
    testing::Values(TimestampCase{"epoch", absl::UnixEpoch()},
                    TimestampCase{"y2000", absl::FromUnixSeconds(946684800)},
                    TimestampCase{"y9999", absl::FromUnixSeconds(253402300799)},
                    TimestampCase{"pre_epoch",
                                  absl::FromUnixSeconds(-62135596800)}));

// ═════════════════════════════════════════════════════════════════════
// META — `type` (m24 §6: WIT `string`, C++ `std::string` — type name)
// ═════════════════════════════════════════════════════════════════════

TEST(ForeignComponentTypeMatrix, TypeArgComponentSeesTypeName) {
  GTEST_SKIP() << kBlockerB0;
  // CEL Value::Type carries the type-name string ("int", "list", …).
  // m24 §6 maps `type` → WIT `string`, so a foreign component arg of
  // declared type `type` receives the name as a string.
  auto lib = ForeignLibOne("type_name", Prim(CelfnType::Kind::kString),
                           {{false, TypeOfTypes(), "t"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("t", Value::Type("int"));
  auto v = RunWithComponent("string rules.type_name(type t);", "type_name(t)",
                            {{"t", CelType::Type()}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(std::string(*v->AsString()), "int");
}

TEST(ForeignComponentTypeMatrix, TypeReturnComponentEmitsTypeValue) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("ret_type", TypeOfTypes(), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("type rules.ret_type();", "ret_type()", {}, *lib,
                            {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kType);
}

// ═════════════════════════════════════════════════════════════════════
// OPTIONAL<T> — one cell per scalar T from §6 + nested
// ═════════════════════════════════════════════════════════════════════
//
// Per m24 §6, optional<T> → WIT `option<wit T>` → C++ `std::optional<C++
// T>`.  Activation/CelType cannot express optional<T> yet (blocker B2),
// so these SKIP on B2 (NOT B0 — even when AddComponent lands, the
// embedder-side surface is still missing).

TEST(ForeignComponentTypeMatrix, OptionalIntArgPresent) {
  GTEST_SKIP() << kBlockerB2;
}

TEST(ForeignComponentTypeMatrix, OptionalIntArgAbsent) {
  GTEST_SKIP() << kBlockerB2;
}

TEST(ForeignComponentTypeMatrix, OptionalStringReturn) {
  GTEST_SKIP() << kBlockerB2;
}

TEST(ForeignComponentTypeMatrix, OptionalListIntNested) {
  GTEST_SKIP() << kBlockerB2;
}

TEST(ForeignComponentTypeMatrix, OptionalDeclarableViaCelfnSource) {
  GTEST_SKIP() << kBlockerB1;
  // When B1 lands (.celfn grammar admits `option(int)`), this proves
  // ParseCelfnSource emits a CelfnDecl with kOptional + correct
  // optional_element[0].kind.
}

// ═════════════════════════════════════════════════════════════════════
// AGGREGATES — list<T> for each scalar T + nested + RETURN
// ═════════════════════════════════════════════════════════════════════

struct ListElemCase {
  std::string label;
  std::string elem_celfn;
  CelfnType elem_celfn_type;
  CelType cel_elem;
  std::vector<Value> elems;
  int64_t expected_size;
};

class ListByElemKind : public testing::TestWithParam<ListElemCase> {};

TEST_P(ListByElemKind, ArgSizeIsObservable) {
  GTEST_SKIP() << kBlockerB0;
  const ListElemCase& c = GetParam();
  auto lib = ForeignLibOne(
      "list_size", Prim(CelfnType::Kind::kInt),
      {{false, ListOf(c.elem_celfn_type), "xs"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("xs", Value::List(c.elems));
  const std::string decl =
      absl::StrCat("int rules.list_size(list<", c.elem_celfn, "> xs);");
  auto v = RunWithComponent(decl, "list_size(xs)",
                            {{"xs", CelType::List(c.cel_elem)}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), c.expected_size);
}

INSTANTIATE_TEST_SUITE_P(
    EachScalarElem, ListByElemKind,
    testing::Values(
        ListElemCase{"bool",
                     "bool",
                     Prim(CelfnType::Kind::kBool),
                     CelType::Bool(),
                     {Value::Bool(true), Value::Bool(false)},
                     2},
        ListElemCase{"int",
                     "int",
                     Prim(CelfnType::Kind::kInt),
                     CelType::Int(),
                     {Value::Int(1), Value::Int(2),
                      Value::Int(std::numeric_limits<int64_t>::min())},
                     3},
        ListElemCase{
            "uint",
            "uint",
            Prim(CelfnType::Kind::kUint),
            CelType::Uint(),
            {Value::Uint(uint64_t{0}),
             Value::Uint(std::numeric_limits<uint64_t>::max())},
            2},
        ListElemCase{"double",
                     "double",
                     Prim(CelfnType::Kind::kDouble),
                     CelType::Double(),
                     {Value::Double(1.5), Value::Double(2.5)},
                     2},
        ListElemCase{
            "string",
            "string",
            Prim(CelfnType::Kind::kString),
            CelType::String(),
            {Value::String(""), Value::String("a"), Value::String("b")},
            3},
        ListElemCase{"bytes",
                     "bytes",
                     Prim(CelfnType::Kind::kBytes),
                     CelType::Bytes(),
                     {Value::Bytes("\x01"), Value::Bytes("")},
                     2},
        ListElemCase{"empty_int", "int", Prim(CelfnType::Kind::kInt),
                     CelType::Int(), {}, 0}),
    [](const testing::TestParamInfo<ListElemCase>& info) {
      return info.param.label;
    });

TEST(ForeignComponentTypeMatrix, ListNestedListIntOuterSize) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne(
      "outer_size", Prim(CelfnType::Kind::kInt),
      {{false, ListOf(ListOf(Prim(CelfnType::Kind::kInt))), "xs"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("xs", Value::List({Value::List({Value::Int(1), Value::Int(2)}),
                              Value::List({})}));
  auto v = RunWithComponent(
      "int rules.outer_size(list<list<int>> xs);", "outer_size(xs)",
      {{"xs", CelType::List(CelType::List(CelType::Int()))}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 2);
}

TEST(ForeignComponentTypeMatrix, ListReturnComponentEmitsThreeInts) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("three_ints",
                           ListOf(Prim(CelfnType::Kind::kInt)), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("list<int> rules.three_ints();", "three_ints()", {},
                            *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kList);
  auto lb = v->ListBacking();
  ASSERT_TRUE(lb.ok()) << lb.status();
  EXPECT_EQ((*lb)->Size(), 3u);
}

// ═════════════════════════════════════════════════════════════════════
// AGGREGATES — map<K, V> for each valid key kind (bool/int/uint/string)
// ═════════════════════════════════════════════════════════════════════
//
// langdef.md §"Map literals": valid key kinds are bool, int, uint,
// string.  Iterating every one per the "spell out the closed set" rule.
// m24 §6 maps map<K,V> → WIT `list<tuple<wit K, wit V>>` → C++
// `std::map<C++ K, C++ V>`.

struct MapKeyKindCase {
  std::string label;
  std::string key_celfn;
  CelfnType key_celfn_type;
  CelType cel_key;
  std::vector<std::pair<Value, Value>> entries;
  int64_t expected_size;
};

class MapByKeyKind : public testing::TestWithParam<MapKeyKindCase> {};

TEST_P(MapByKeyKind, ArgSizeIsObservable) {
  GTEST_SKIP() << kBlockerB0;
  const MapKeyKindCase& c = GetParam();
  auto lib = ForeignLibOne(
      "map_size", Prim(CelfnType::Kind::kInt),
      {{false, MapOf(c.key_celfn_type, Prim(CelfnType::Kind::kInt)), "m"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("m", Value::Map(c.entries));
  const std::string decl =
      absl::StrCat("int rules.map_size(map<", c.key_celfn, ", int> m);");
  auto v = RunWithComponent(
      decl, "map_size(m)",
      {{"m", CelType::Map(c.cel_key, CelType::Int())}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), c.expected_size);
}

INSTANTIATE_TEST_SUITE_P(
    EachKeyKind, MapByKeyKind,
    testing::Values(
        MapKeyKindCase{"bool",
                       "bool",
                       Prim(CelfnType::Kind::kBool),
                       CelType::Bool(),
                       {{Value::Bool(true), Value::Int(1)},
                        {Value::Bool(false), Value::Int(0)}},
                       2},
        MapKeyKindCase{"int",
                       "int",
                       Prim(CelfnType::Kind::kInt),
                       CelType::Int(),
                       {{Value::Int(-1), Value::Int(100)},
                        {Value::Int(2), Value::Int(7)}},
                       2},
        MapKeyKindCase{"uint",
                       "uint",
                       Prim(CelfnType::Kind::kUint),
                       CelType::Uint(),
                       {{Value::Uint(uint64_t{5}), Value::Int(50)},
                        {Value::Uint(uint64_t{10}), Value::Int(100)}},
                       2},
        MapKeyKindCase{"string",
                       "string",
                       Prim(CelfnType::Kind::kString),
                       CelType::String(),
                       {{Value::String("a"), Value::Int(1)},
                        {Value::String("b"), Value::Int(2)}},
                       2},
        MapKeyKindCase{"empty_string_keyed", "string",
                       Prim(CelfnType::Kind::kString), CelType::String(), {},
                       0}),
    [](const testing::TestParamInfo<MapKeyKindCase>& info) {
      return info.param.label;
    });

TEST(ForeignComponentTypeMatrix, MapValueListNestedShapeRoundTrips) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne(
      "size_at", Prim(CelfnType::Kind::kInt),
      {{false,
        MapOf(Prim(CelfnType::Kind::kString),
              ListOf(Prim(CelfnType::Kind::kInt))),
        "m"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind(
      "m", Value::Map({{Value::String("a"),
                        Value::List({Value::Int(1), Value::Int(2),
                                     Value::Int(3)})},
                       {Value::String("b"), Value::List({})}}));
  auto v = RunWithComponent(
      "int rules.size_at(map<string, list<int>> m);", "size_at(m)",
      {{"m", CelType::Map(CelType::String(), CelType::List(CelType::Int()))}},
      *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 3);
}

TEST(ForeignComponentTypeMatrix, MapReturnComponentEmitsStringInt) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("ages",
                           MapOf(Prim(CelfnType::Kind::kString),
                                 Prim(CelfnType::Kind::kInt)),
                           {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithComponent("map<string, int> rules.ages();", "ages()", {},
                            *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kMap);
  auto mb = v->MapBacking();
  ASSERT_TRUE(mb.ok()) << mb.status();
  EXPECT_EQ((*mb)->Size(), 2u);
}

TEST(ForeignComponentTypeMatrix, MapMissingKeyAtComponentSurfacesNoSuchKey) {
  GTEST_SKIP() << kBlockerB0;
  // §10 cell: map<.,list>, missing-key — the component's body looks up
  // a key not in the map and reports through the Value error channel
  // (m24 §11 negative coverage).
}

// ═════════════════════════════════════════════════════════════════════
// PROTO — the m24 §8 "crosses as bytes" path (the m13 §4.5.1 ban lifted)
// ═════════════════════════════════════════════════════════════════════
//
// kForeignComponent admits proto(...) — codec serializes one side and
// deserializes the other.  The cell below uses the existing Customer
// e2e fixture to keep parity with host_fn_type_matrix_test.cc's proto
// section.

TEST(ForeignComponentTypeMatrix, ProtoArgComponentReadsField) {
  GTEST_SKIP() << kBlockerB0;
  Customer c;
  c.set_name("Ada");
  c.set_age(36);
  auto lib = ForeignLibOne(
      "first_letter", Prim(CelfnType::Kind::kString),
      {{false, ProtoOf("celwasm.testdata.Customer"), "c"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("c", Value::Message(c));
  auto v = RunWithComponent(
      "string rules.first_letter(proto(celwasm.testdata.Customer) c);",
      "first_letter(c)",
      {{"c", CelType::Message("celwasm.testdata.Customer")}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(std::string(*v->AsString()), "A");
}

TEST(ForeignComponentTypeMatrix, ProtoReturnComponentEmitsCustomer) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne(
      "build_customer", ProtoOf("celwasm.testdata.Customer"),
      {{false, Prim(CelfnType::Kind::kString), "n"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("n", Value::String("Ada"));
  auto v = RunWithComponent(
      "proto(celwasm.testdata.Customer) "
      "rules.build_customer(string n);",
      "build_customer(n).name", {{"n", CelType::String()}}, *lib, {}, act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(std::string(*v->AsString()), "Ada");
}

// One pin that DOES run today — Builder admits proto(...) for
// kForeignComponent (the §4.5.1 ban only applies to kForeign).  This is
// the smallest interface-layer assertion and validates the deltas this
// commit ships against function_library.{h,cc}.
TEST(ForeignComponentTypeMatrix, BuilderAdmitsProtoForForeignComponent) {
  auto lib = FunctionLibrary::Builder()
                 .AddForeignComponent(
                     "first_letter", Prim(CelfnType::Kind::kString),
                     {{false, ProtoOf("acme.User"), "u"}})
                 .Build();
  ASSERT_TRUE(lib.ok()) << lib.status();
  ASSERT_EQ(lib->decls().size(), 1u);
  EXPECT_EQ(lib->decls()[0].backend,
            CelfnDecl::Backend::kForeignComponent);
  EXPECT_EQ(lib->decls()[0].overload_id, "first_letter_message_acme_User");
  EXPECT_EQ(lib->decls()[0].module_name, "cel_fn");
}

// Sibling: kForeign STILL rejects proto(...) — the §4.5.1 ban is
// scoped to the shared-memory backend, not lifted globally.
TEST(ForeignComponentTypeMatrix, BuilderRejectsProtoForKForeign) {
  auto lib =
      FunctionLibrary::Builder()
          .AddForeign("rules", "first_letter", Prim(CelfnType::Kind::kString),
                      {{false, ProtoOf("acme.User"), "u"}})
          .Build();
  ASSERT_FALSE(lib.ok());
  EXPECT_EQ(lib.status().code(), absl::StatusCode::kInvalidArgument);
}

// Argkind synthesis for the new CelfnType kinds — exercises the
// extension in CelfnType::Argkind() that this commit adds.
TEST(ForeignComponentTypeMatrix, ArgkindForNewKinds) {
  EXPECT_EQ(TypeOfTypes().Argkind(), "type");
  EXPECT_EQ(OptionalOf(Prim(CelfnType::Kind::kInt)).Argkind(), "optional_int");
  EXPECT_EQ(OptionalOf(ListOf(Prim(CelfnType::Kind::kString))).Argkind(),
            "optional_list_string");
}

// ═════════════════════════════════════════════════════════════════════
// NEGATIVE — m24 §11 forcing rows
// ═════════════════════════════════════════════════════════════════════

TEST(ForeignComponentTypeMatrix, NegativeWrongArityAtAddComponent) {
  GTEST_SKIP() << kBlockerB0;
  // Decl says 2-arg (one param + out_slot); the component exports a
  // 3-arg func.  AddComponent must FailedPrecondition with both
  // arities named in the message.
  auto lib = ForeignLibOne("two_arg", Prim(CelfnType::Kind::kInt),
                           {{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  auto st = engine->AddComponent({}, *lib);
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(ForeignComponentTypeMatrix, NegativeMissingExportAtAddComponent) {
  GTEST_SKIP() << kBlockerB0;
  // Decl names a fn `not_exported` not present in the component.
  // AddComponent must FailedPrecondition citing the fn_name.
  auto lib =
      ForeignLibOne("not_exported", Prim(CelfnType::Kind::kInt), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  auto st = engine->AddComponent({}, *lib);
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(ForeignComponentTypeMatrix, NegativeMalformedComponentBytesAtAddComponent) {
  GTEST_SKIP() << kBlockerB0;
  auto lib = ForeignLibOne("any_fn", Prim(CelfnType::Kind::kInt), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  const std::vector<uint8_t> garbage = {0xde, 0xad, 0xbe, 0xef};
  auto st = engine->AddComponent(garbage, *lib);
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.code(), absl::StatusCode::kInvalidArgument);
}

TEST(ForeignComponentTypeMatrix, NegativeFnReturnsEvalErrorBecomesCelError) {
  GTEST_SKIP() << kBlockerB0;
  // Component body returns the eval-error result variant; the host
  // bridge surfaces it as Value::Error at the call site (not a
  // wasm trap).
}

TEST(ForeignComponentTypeMatrix, NegativeComponentTrapBecomesHostStatus) {
  GTEST_SKIP() << kBlockerB0;
  // Component traps (out-of-bounds memory access, unreachable);
  // Instance::Eval returns absl::Status (NOT a Value::Error).
}

TEST(ForeignComponentTypeMatrix, NegativeThreeValuedLogicErrorArgShortCircuits) {
  GTEST_SKIP() << kBlockerB0;
  // 3VL: passing an error arg short-circuits and the component is NEVER
  // invoked.  Mirrors the m13/m21 absorption rule (m24 §2 line: "3VL
  // absorption unchanged").
}

TEST(ForeignComponentTypeMatrix,
     NegativeThreeValuedLogicUnknownArgShortCircuits) {
  GTEST_SKIP() << kBlockerB0;
}

// ═════════════════════════════════════════════════════════════════════
// FORCING — m24 §11 forcing function (TinyGo component implementing the
// same fns.wit, proving the contract is language-agnostic)
// ═════════════════════════════════════════════════════════════════════

TEST(ForeignComponentTypeMatrix, TinyGoBackedComponentProducesSameResult) {
  GTEST_SKIP() << kBlockerB0
               << " (also needs the TinyGo build target under "
                  "e2e/foreign_component_fixtures/tinygo/)";
}

// ═════════════════════════════════════════════════════════════════════
// IDL — m24 §6 declarations via .celfn source (B1 grammar work)
// ═════════════════════════════════════════════════════════════════════
//
// When the celfn IDL grammar is extended for foreign-component
// keywords (m24 §9), ParseCelfnSource will need to admit `type` and
// `option(...)`.  Tests pinned on B1 today.

TEST(ForeignComponentTypeMatrix, CelfnSourceAdmitsForeignComponentDecl) {
  GTEST_SKIP() << kBlockerB1;
  // Once the grammar grows a `@component` prefix (or analogous),
  // ParseCelfnSource("string @component.rules.first_letter(...);")
  // produces a CelfnDecl with backend == kForeignComponent.
}

TEST(ForeignComponentTypeMatrix, CelfnSourceAdmitsTypeKeyword) {
  GTEST_SKIP() << kBlockerB1;
}

TEST(ForeignComponentTypeMatrix, CelfnSourceAdmitsOptionalKeyword) {
  GTEST_SKIP() << kBlockerB1;
}

}  // namespace
}  // namespace celwasm
