// e2e: exhaustive type-matrix coverage for the m24 plugin
// custom-fn boundary — the `kPlugin` backend dispatched via
// `Engine::AddPlugin(plugin_bytes, lib)`.
//
// Sibling to `e2e/host_fn_type_matrix_test.cc` (the in-process host-fn
// matrix), structurally identical: one section per CEL type from §6 of
// `doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md`,
// each section covers BOTH directions (arg + return) plus the §10
// boundary inputs, plus the §11 negative-coverage rows (wrong arity,
// missing export, fn-returning-eval-error, component trap, 3VL
// absorption).
//
// ── Status (re-verified 2026-06-10) ────────────────────────────────────
//
// The original B0 blocker ("Engine::AddPlugin returns Unimplemented")
// is RESOLVED — m24 shipped 2026-06-04 (`eval/engine.cc:1519`), and the
// dispatch wiring is proven in `e2e/plugin_dispatch_test.cc`.
// Every formerly-B0 test below now runs against a REAL Component-Model
// component: inline component WAT assembled at test runtime via
// `wasmtime_wat2wasm` (no wit-bindgen, no out-of-band build).
//
// Remaining skips, each with a freshly-verified citation at the skip
// site:
//
//   - optional<T> embedder bindings (kBlockerB2): `shared/type.h` has
//     neither `CelType::Optional(T)` nor `Value::Optional(T)`
//     (verified 2026-06-10), AND m24 §14 permanently rejects
//     `optional<T>` as a plugin declarable shape
//     (`function_library.cc:291` CheckPluginDeclShape) —
//     pinned positive by OptionalArgRejectedAtLibraryBuild below.
//   - `type` / `option(...)` celfn keywords: the grammar's `type` rule
//     (`compiler/celfn/Celfn.g4`, "Types" section) has no alternative
//     for either, and m24 §14 closes both permanently for foreign
//     components — so the keyword work will never land for this
//     backend.  Note the `@plugin.` decl prefix DID land
//     (`Celfn.g4` pluginFnDecl) and is now exercised by every
//     RunWithPlugin test plus CelfnSourceAdmitsPluginDecl.
//   - eval-error returns from a component: the m24 wire has no error
//     result variant — `CallPluginAndLowerResult`
//     (`eval/engine.cc:835`) lowers exactly one plain value, so a
//     component cannot surface a CEL error through its return channel.
//   - TinyGo forcing fixture: AddPlugin itself shipped; the TinyGo
//     build target under `e2e/plugin_fixtures/tinygo/` does
//     not exist.
//
// ── How this file maps to the §6 type table (m24) ──────────────────────
//
//   bool          — Bool section, arg + return + boundary.
//   int           — Int section, arg + return + INT64_MIN / INT64_MAX.
//   uint          — Uint section, arg + return + UINT64_MAX.
//   double        — Double section, arg + return + ±0 / ±Inf / NaN / DBL boundaries.
//   string        — String section, arg + return + empty / NUL / UTF-8 / long.
//   bytes         — Bytes section, arg + return + empty / 0xFF / long.
//   null          — Null section, arg (as `option<unit>` per m24 §6) + return.
//   duration      — Duration section, min / max seconds, nanos boundary.
//   timestamp     — Timestamp section, min / max seconds, nanos boundary.
//   type          — PERMANENTLY REJECTED (m24 §14) — pinned as Build()-time
//                   negative coverage.
//   optional<T>   — PERMANENTLY REJECTED (m24 §14) — Build()-time negative
//                   pin + B2 skips for the embedder-side binding cells.
//   list<T>       — List section for each scalar T + nested `list<list>`.
//   map<K,V>      — Map section for each valid key kind + nested.
//   proto(fqn)    — Proto section (the m24 §8 "crosses as bytes" path).
//
// ── Component bytes ────────────────────────────────────────────────────
//
// Each section authors the component inline as Component-Model WAT and
// assembles it with `wasmtime_wat2wasm` (same recipe as
// `plugin_dispatch_test.cc`).  Gotchas inherited from that
// file: component export names must be kebab-case — the engine
// converts the snake_case CEL overload-id at the
// `wasmtime_component_instance_get_export_index` site
// (`eval/engine.cc` OverloadIdToKebab), so e.g. fn `echo_bool(bool)` →
// overload-id `echo_bool_bool` → export "echo-bool-bool".  Results
// whose canonical-ABI flattening exceeds one core value (string,
// bytes, list, record, option) cross via an i32 return pointer into
// the component's exported memory; list / string ARGUMENTS additionally
// need a `realloc` export for the canonical-ABI lowering.

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/attribute.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/internal/cel_host.h"  // HostListBacking / HostMapBacking Size()
#include "eval/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::celwasm::testdata::Customer;
using ::testing::HasSubstr;

// ─────────────────────────────────────────────────────────────────────
// SKIP blocker citations — only the genuinely-still-blocked cells
// reference these; everything else runs against a real component.
// ─────────────────────────────────────────────────────────────────────

constexpr absl::string_view kBlockerB2 =
    "blocked on shared/eval support for optional<T> — verified 2026-06-10 "
    "that neither CelType::Optional(T) nor Value::Optional(T) exists "
    "(shared/type.h has no Optional factory), so the embedder cannot bind "
    "an optional<T> arg through Activation. Additionally m24 §14 "
    "PERMANENTLY rejects optional<T> as a plugin declarable "
    "shape (function_library.cc:291 CheckPluginDeclShape — "
    "pinned by OptionalArgRejectedAtLibraryBuild), so even with embedder "
    "Optional support these cells would need a scope reversal first.";

constexpr absl::string_view kNoErrorResultVariant =
    "the m24 wire has no eval-error result variant — "
    "CallPluginAndLowerResult (eval/engine.cc:835) lowers exactly one "
    "plain value per the decl's return CelfnType; a component has no "
    "channel to surface a CEL error Value (verified 2026-06-10 against "
    "eval/internal/cel_plugin.cc LowerComponentToCel, which has no "
    "result/error arm). Un-skip if a result<T, cel-error> wire variant "
    "ever ships.";

// ─────────────────────────────────────────────────────────────────────
// Component assembly — inline component-model WAT → bytes, via
// wasmtime_wat2wasm at test runtime (same helper as
// plugin_dispatch_test.cc).
// ─────────────────────────────────────────────────────────────────────

std::vector<uint8_t> WatToWasm(absl::string_view wat) {
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
  if (err != nullptr) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string err_str(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    ABSL_CHECK(false) << "wat2wasm failed: " << err_str;
  }
  std::vector<uint8_t> bytes(
      reinterpret_cast<const uint8_t*>(out.data),
      reinterpret_cast<const uint8_t*>(out.data) + out.size);
  wasm_byte_vec_delete(&out);
  return bytes;
}

// Overload-id → component export name (mirrors the engine's
// OverloadIdToKebab so the WAT builders below can derive export names
// from the same source of truth the lookup uses).
std::string Kebab(absl::string_view overload_id) {
  return absl::StrReplaceAll(overload_id, {{"_", "-"}});
}

// ── Reusable WAT builders ───────────────────────────────────────────
//
// Scalar echo: param + result are the same single-flat WIT type, so
// the core fn is an identity over the matching core type.
std::string EchoScalarWat(absl::string_view export_name,
                          absl::string_view wit_type,
                          absl::string_view core_type) {
  return absl::StrCat(R"WAT(
(component
  (core module $m
    (func (export "id") (param )WAT",
                      core_type, R"WAT() (result )WAT", core_type,
                      R"WAT() local.get 0))
  (core instance $i (instantiate $m))
  (func (export ")WAT",
                      export_name, R"WAT(") (param "x" )WAT", wit_type,
                      R"WAT() (result )WAT", wit_type, R"WAT()
    (canon lift (core func $i "id"))))
)WAT");
}

// Nullary fn returning one single-flat scalar produced by
// `const_instr` (e.g. "i64.const -1").
std::string ConstScalarWat(absl::string_view export_name,
                           absl::string_view wit_type,
                           absl::string_view core_type,
                           absl::string_view const_instr) {
  return absl::StrCat(R"WAT(
(component
  (core module $m
    (func (export "f") (result )WAT",
                      core_type, R"WAT() )WAT", const_instr, R"WAT())
  (core instance $i (instantiate $m))
  (func (export ")WAT",
                      export_name, R"WAT(") (result )WAT", wit_type, R"WAT()
    (canon lift (core func $i "f"))))
)WAT");
}

// ptr/len echo: param + result are the same {ptr,len}-shaped WIT type
// (string, or list<u8> for bytes).  The arg is realloc'd into the
// component's memory by the canonical ABI; the core fn writes the
// arg's {ptr,len} to a scratch retptr slot at address 8 (the bump
// realloc starts at 1024, so 8 is free) and returns the slot address —
// the canonical ABI lifts the result from there.
std::string EchoPtrLenWat(absl::string_view export_name,
                          absl::string_view wit_type) {
  return absl::StrCat(R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      ;; ret = (next + align - 1) & -align — the canonical ABI validates
      ;; that realloc results honor the requested alignment (an
      ;; unaligned result fails the call with "result not aligned").
      global.get $next
      local.get $align
      i32.add
      i32.const 1
      i32.sub
      i32.const 0
      local.get $align
      i32.sub
      i32.and
      local.set $ret
      local.get $ret
      local.get $new_sz
      i32.add
      global.set $next
      local.get $ret)
    (func (export "echo") (param $ptr i32) (param $len i32) (result i32)
      i32.const 8
      local.get $ptr
      i32.store
      i32.const 12
      local.get $len
      i32.store
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "echo" (core func $echo))
  (func (export ")WAT",
                      export_name, R"WAT(") (param "x" )WAT", wit_type,
                      R"WAT() (result )WAT", wit_type, R"WAT()
    (canon lift (core func $echo) (memory $mem) (realloc $realloc))))
)WAT");
}

// Nullary fn returning an EMPTY {ptr,len}-shaped value (empty string /
// empty bytes): retptr slot at 8 carries {ptr=16, len=0}.
std::string EmptyPtrLenWat(absl::string_view export_name,
                           absl::string_view wit_type) {
  return absl::StrCat(R"WAT(
(component
  (core module $m
    (memory (export "memory") 1)
    (func (export "f") (result i32)
      i32.const 8
      i32.const 16
      i32.store
      i32.const 12
      i32.const 0
      i32.store
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (func (export ")WAT",
                      export_name, R"WAT(") (result )WAT", wit_type, R"WAT()
    (canon lift (core func $i "f") (memory $mem))))
)WAT");
}

// Length-of-aggregate: param is any list-shaped WIT type (list<T>, or
// the map encoding list<tuple<K,V>>), which lowers as (ptr, len); the
// core fn returns len as s64.  Covers every "size is observable" cell.
std::string LenOfListArgWat(absl::string_view export_name,
                            absl::string_view wit_param_type) {
  return absl::StrCat(R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      ;; ret = (next + align - 1) & -align — the canonical ABI validates
      ;; that realloc results honor the requested alignment (an
      ;; unaligned result fails the call with "result not aligned").
      global.get $next
      local.get $align
      i32.add
      i32.const 1
      i32.sub
      i32.const 0
      local.get $align
      i32.sub
      i32.and
      local.set $ret
      local.get $ret
      local.get $new_sz
      i32.add
      global.set $next
      local.get $ret)
    (func (export "len_fn") (param $ptr i32) (param $len i32) (result i64)
      local.get $len
      i64.extend_i32_u))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "len_fn" (core func $len_fn))
  (func (export ")WAT",
                      export_name, R"WAT(") (param "xs" )WAT", wit_param_type,
                      R"WAT() (result s64)
    (canon lift (core func $len_fn) (memory $mem) (realloc $realloc))))
)WAT");
}

// seconds/nanos record echo (duration + timestamp share the wire shape
// per eval/internal/cel_plugin.cc EmitSecondsNanosRecord).  Param
// flattens to (i64, i32); the result record flattens to two core
// values, so it crosses via retptr: seconds at retptr+0, nanos at
// retptr+8 (record layout: s64 @0, s32 @8, align 8).
std::string EchoSecondsNanosWat(absl::string_view export_name) {
  return absl::StrCat(R"WAT(
(component
  (core module $m
    (memory (export "memory") 1)
    (func (export "f") (param i64 i32) (result i32)
      i32.const 8
      local.get 0
      i64.store
      i32.const 16
      local.get 1
      i32.store
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  ;; Named types used in exported function signatures must themselves
  ;; be exported (component-model validation: "func not valid to be
  ;; used as export" otherwise) — hence the `(export ... (type ...))`
  ;; indirection.
  (type $rec (record (field "seconds" s64) (field "nanos" s32)))
  (export $recx "rec" (type $rec))
  (func (export ")WAT",
                      export_name, R"WAT(") (param "x" $recx) (result $recx)
    (canon lift (core func $i "f") (memory $mem))))
)WAT");
}

// A component whose core body traps (`unreachable`) if ever invoked —
// the 3VL-absorption tests register this and assert it never runs.
std::string TrapIfInvokedWat(absl::string_view export_name) {
  return absl::StrCat(R"WAT(
(component
  (core module $m
    (func (export "boom") (param i64) (result i64) unreachable))
  (core instance $i (instantiate $m))
  (func (export ")WAT",
                      export_name, R"WAT(") (param "x" s64) (result s64)
    (canon lift (core func $i "boom"))))
)WAT");
}

// ─────────────────────────────────────────────────────────────────────
// Decl plumbing
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

// Build a FunctionLibrary with one plugin fn.
absl::StatusOr<FunctionLibrary> ForeignLibOne(absl::string_view fn_name,
                                              CelfnType return_type,
                                              std::vector<CelfnParam> params) {
  return FunctionLibrary::Builder()
      .AddPlugin(fn_name, std::move(return_type), std::move(params))
      .Build();
}

// Common pipeline: declare the fn to the checker via celfn SOURCE (so
// the `@plugin.` IDL path is exercised on every test), compile,
// register the component, Plan, Eval.  The programmatic `lib` and the
// parsed decl synthesize the same overload-id (both go through
// SynthesiseOverloadId), which is what ties the compiled call site to
// the component export.
absl::StatusOr<Value> RunWithPlugin(
    absl::string_view fn_decl_celfn, absl::string_view expr,
    const std::vector<DeclVar>& vars, const FunctionLibrary& lib,
    absl::Span<const uint8_t> plugin_bytes, const Activation& act) {
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
  if (auto st = engine->AddPlugin(plugin_bytes, lib); !st.ok()) {
    return st;
  }
  auto instance = engine->Plan(*program);
  if (!instance.ok()) return instance.status();
  return instance->Eval(act);
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — bool
// ═════════════════════════════════════════════════════════════════════

TEST(PluginTypeMatrix, BoolArgPluginSeesBoundValue) {
  auto lib = ForeignLibOne("echo_bool", Prim(CelfnType::Kind::kBool),
                           {{false, Prim(CelfnType::Kind::kBool), "b"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("b", Value::Bool(true));
  auto v = RunWithPlugin(
      "bool @plugin.echo_bool(bool b);", "echo_bool(b)",
      {{"b", CelType::Bool()}}, *lib,
      WatToWasm(EchoScalarWat("echo-bool-bool", "bool", "i32")), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

TEST(PluginTypeMatrix, BoolReturnPluginEmitsValue) {
  auto lib = ForeignLibOne("always_false", Prim(CelfnType::Kind::kBool), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin(
      "bool @plugin.always_false();", "always_false()", {}, *lib,
      WatToWasm(ConstScalarWat("always-false", "bool", "i32", "i32.const 0")),
      act);
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
  auto lib = ForeignLibOne("echo_int", Prim(CelfnType::Kind::kInt),
                           {{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("x", Value::Int(GetParam().in));
  auto v = RunWithPlugin(
      "int @plugin.echo_int(int x);", "echo_int(x)", {{"x", CelType::Int()}},
      *lib, WatToWasm(EchoScalarWat("echo-int-int", "s64", "i64")), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), GetParam().in);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, IntBoundary,
    testing::Values(Int64Case{"zero", 0}, Int64Case{"neg_one", -1},
                    Int64Case{"int64_min", std::numeric_limits<int64_t>::min()},
                    Int64Case{"int64_max",
                              std::numeric_limits<int64_t>::max()}),
    [](const testing::TestParamInfo<Int64Case>& info) {
      return info.param.label;
    });

TEST(PluginTypeMatrix, IntReturnPluginEmitsInt64Min) {
  auto lib = ForeignLibOne("int_min", Prim(CelfnType::Kind::kInt), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin(
      "int @plugin.int_min();", "int_min()", {}, *lib,
      WatToWasm(ConstScalarWat("int-min", "s64", "i64",
                               "i64.const -9223372036854775808")),
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
  auto lib = ForeignLibOne("echo_uint", Prim(CelfnType::Kind::kUint),
                           {{false, Prim(CelfnType::Kind::kUint), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("x", Value::Uint(GetParam().in));
  auto v = RunWithPlugin(
      "uint @plugin.echo_uint(uint x);", "echo_uint(x)",
      {{"x", CelType::Uint()}}, *lib,
      WatToWasm(EchoScalarWat("echo-uint-uint", "u64", "i64")), act);
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

TEST(PluginTypeMatrix, UintReturnPluginEmitsUintMax) {
  auto lib = ForeignLibOne("uint_max", Prim(CelfnType::Kind::kUint), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  // i64.const -1 is the all-ones bit pattern == UINT64_MAX under u64.
  auto v = RunWithPlugin(
      "uint @plugin.uint_max();", "uint_max()", {}, *lib,
      WatToWasm(ConstScalarWat("uint-max", "u64", "i64", "i64.const -1")), act);
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
  auto lib = ForeignLibOne("echo_double", Prim(CelfnType::Kind::kDouble),
                           {{false, Prim(CelfnType::Kind::kDouble), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("x", Value::Double(GetParam().in));
  auto v = RunWithPlugin(
      "double @plugin.echo_double(double x);", "echo_double(x)",
      {{"x", CelType::Double()}}, *lib,
      WatToWasm(EchoScalarWat("echo-double-double", "f64", "f64")), act);
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

TEST(PluginTypeMatrix, DoubleReturnPluginEmitsNan) {
  auto lib = ForeignLibOne("nanval", Prim(CelfnType::Kind::kDouble), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin(
      "double @plugin.nanval();", "nanval()", {}, *lib,
      WatToWasm(ConstScalarWat("nanval", "f64", "f64", "f64.const nan")), act);
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
  auto lib = ForeignLibOne("echo_string", Prim(CelfnType::Kind::kString),
                           {{false, Prim(CelfnType::Kind::kString), "s"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("s", Value::String(GetParam().in));
  auto v = RunWithPlugin(
      "string @plugin.echo_string(string s);", "echo_string(s)",
      {{"s", CelType::String()}}, *lib,
      WatToWasm(EchoPtrLenWat("echo-string-string", "string")), act);
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

TEST(PluginTypeMatrix, StringReturnPluginEmitsEmpty) {
  auto lib = ForeignLibOne("empty_str", Prim(CelfnType::Kind::kString), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin(
      "string @plugin.empty_str();", "empty_str()", {}, *lib,
      WatToWasm(EmptyPtrLenWat("empty-str", "string")), act);
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
  auto lib = ForeignLibOne("echo_bytes", Prim(CelfnType::Kind::kBytes),
                           {{false, Prim(CelfnType::Kind::kBytes), "b"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("b", Value::Bytes(GetParam().in));
  auto v = RunWithPlugin(
      "bytes @plugin.echo_bytes(bytes b);", "echo_bytes(b)",
      {{"b", CelType::Bytes()}}, *lib,
      WatToWasm(EchoPtrLenWat("echo-bytes-bytes", "(list u8)")), act);
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

TEST(PluginTypeMatrix, BytesReturnPluginEmitsEmpty) {
  auto lib = ForeignLibOne("empty_bytes", Prim(CelfnType::Kind::kBytes), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin(
      "bytes @plugin.empty_bytes();", "empty_bytes()", {}, *lib,
      WatToWasm(EmptyPtrLenWat("empty-bytes", "(list u8)")), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->AsBytes()->empty());
}

// ═════════════════════════════════════════════════════════════════════
// SCALAR — null (m24 §6: crosses as a WIT `option`, always `none`)
// ═════════════════════════════════════════════════════════════════════
//
// Lift emits kind=OPTION / option=nullptr (none) for CEL null
// (cel_plugin.cc kNull arm); a `none` option type-checks against
// any option<T>, so the WAT declares `option<bool>`.  option<bool>
// flattens to (i32 disc, i32 payload) as a param; as a RESULT it
// flattens to two core values and therefore crosses via retptr
// (disc byte at retptr+0).

constexpr absl::string_view kIsNullComponentWat = R"WAT(
(component
  (core module $m
    (func (export "f") (param i32 i32) (result i32)
      local.get 0
      i32.eqz))
  (core instance $i (instantiate $m))
  (func (export "is-null-null") (param "x" (option bool)) (result bool)
    (canon lift (core func $i "f"))))
)WAT";

TEST(PluginTypeMatrix, NullArgPluginObservesAbsence) {
  auto lib = ForeignLibOne("is_null", Prim(CelfnType::Kind::kBool),
                           {{false, Prim(CelfnType::Kind::kNull), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin("bool @plugin.is_null(null x);", "is_null(null)",
                            {}, *lib, WatToWasm(kIsNullComponentWat), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v->AsBool());
}

constexpr absl::string_view kMakeNullComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 1)
    (func (export "f") (result i32)
      i32.const 8
      i32.const 0
      i32.store8
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (func (export "make-null") (result (option bool))
    (canon lift (core func $i "f") (memory $mem))))
)WAT";

TEST(PluginTypeMatrix, NullReturnPluginEmitsNull) {
  auto lib = ForeignLibOne("make_null", Prim(CelfnType::Kind::kNull), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin("null @plugin.make_null();", "make_null()", {},
                            *lib, WatToWasm(kMakeNullComponentWat), act);
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
  auto lib = ForeignLibOne("echo_dur", Prim(CelfnType::Kind::kDuration),
                           {{false, Prim(CelfnType::Kind::kDuration), "d"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("d", Value::Duration(GetParam().in));
  auto v = RunWithPlugin("Duration @plugin.echo_dur(Duration d);",
                            "echo_dur(d)", {{"d", CelType::Duration()}}, *lib,
                            WatToWasm(EchoSecondsNanosWat("echo-dur-duration")),
                            act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsDuration(), GetParam().in);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, DurationBoundary,
    testing::Values(DurationCase{"zero", absl::ZeroDuration()},
                    DurationCase{"neg", -absl::Seconds(1)},
                    DurationCase{"max_seconds", absl::Seconds(315576000000)},
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
  auto lib = ForeignLibOne("echo_ts", Prim(CelfnType::Kind::kTimestamp),
                           {{false, Prim(CelfnType::Kind::kTimestamp), "t"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("t", Value::Timestamp(GetParam().in));
  auto v = RunWithPlugin("Timestamp @plugin.echo_ts(Timestamp t);",
                            "echo_ts(t)", {{"t", CelType::Timestamp()}}, *lib,
                            WatToWasm(EchoSecondsNanosWat("echo-ts-timestamp")),
                            act);
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
// META — `type` (PERMANENTLY out of scope for plugins,
// m24 §14).  These are Build()-time rejection pins, not skips: the
// decl surface refuses kType anywhere in a plugin shape
// (function_library.cc CheckPluginDeclShape), so the
// negative IS the contract.  The kType Lift arm stays implemented in
// cel_plugin.cc for the kHost path; the Lower (return) arm is
// Unimplemented (cleanup-backlog #44) — both unreachable through this
// backend because the decl never builds.
// ═════════════════════════════════════════════════════════════════════

TEST(PluginTypeMatrix, TypeArgRejectedAtLibraryBuild) {
  auto lib = ForeignLibOne("type_name", Prim(CelfnType::Kind::kString),
                           {{false, TypeOfTypes(), "t"}});
  ASSERT_FALSE(lib.ok());
  EXPECT_EQ(lib.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(lib.status().message()),
              HasSubstr("`type` is not supported as a plugin "
                        "argument or return shape"));
}

TEST(PluginTypeMatrix, TypeReturnRejectedAtLibraryBuild) {
  auto lib = ForeignLibOne("ret_type", TypeOfTypes(), {});
  ASSERT_FALSE(lib.ok());
  EXPECT_EQ(lib.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(lib.status().message()),
              HasSubstr("has a `type` return"));
}

// ═════════════════════════════════════════════════════════════════════
// OPTIONAL<T> — PERMANENTLY out of scope as a plugin decl
// shape (m24 §14); embedder-side optional<T> bindings additionally
// blocked on B2.
// ═════════════════════════════════════════════════════════════════════

// The Build()-time rejection pin — the positive statement of the
// m24 §14 contract, runs today.
TEST(PluginTypeMatrix, OptionalArgRejectedAtLibraryBuild) {
  auto lib =
      ForeignLibOne("opt_arg", Prim(CelfnType::Kind::kInt),
                    {{false, OptionalOf(Prim(CelfnType::Kind::kInt)), "x"}});
  ASSERT_FALSE(lib.ok());
  EXPECT_EQ(lib.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(lib.status().message()),
              HasSubstr("optional<T> is not supported as a plugin "
                        "argument or return shape"));
}

TEST(PluginTypeMatrix, OptionalIntArgPresent) {
  GTEST_SKIP() << kBlockerB2;
}

TEST(PluginTypeMatrix, OptionalIntArgAbsent) {
  GTEST_SKIP() << kBlockerB2;
}

TEST(PluginTypeMatrix, OptionalStringReturn) {
  GTEST_SKIP() << kBlockerB2;
}

TEST(PluginTypeMatrix, OptionalListIntNested) {
  GTEST_SKIP() << kBlockerB2;
}

TEST(PluginTypeMatrix, OptionalDeclarableViaCelfnSource) {
  GTEST_SKIP() << "the celfn grammar (compiler/celfn/Celfn.g4 `type` rule) "
                  "has no `option(...)` alternative — verified 2026-06-10 — "
                  "and m24 §14 PERMANENTLY rejects optional<T> as a "
                  "plugin shape (pinned by "
                  "OptionalArgRejectedAtLibraryBuild), so the grammar "
                  "keyword will not land for this backend.";
}

// ═════════════════════════════════════════════════════════════════════
// AGGREGATES — list<T> for each scalar T + nested + RETURN
// ═════════════════════════════════════════════════════════════════════

struct ListElemCase {
  std::string label;
  std::string elem_celfn;
  std::string elem_wit;  // WIT spelling for the component's param type
  CelfnType elem_celfn_type;
  CelType cel_elem;
  std::vector<Value> elems;
  int64_t expected_size;
};

class ListByElemKind : public testing::TestWithParam<ListElemCase> {};

TEST_P(ListByElemKind, ArgSizeIsObservable) {
  const ListElemCase& c = GetParam();
  auto lib = ForeignLibOne("list_size", Prim(CelfnType::Kind::kInt),
                           {{false, ListOf(c.elem_celfn_type), "xs"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("xs", Value::List(c.elems));
  const std::string decl =
      absl::StrCat("int @plugin.list_size(list<", c.elem_celfn, "> xs);");
  const std::string export_name = Kebab(lib->decls()[0].overload_id);
  const std::string wat =
      LenOfListArgWat(export_name, absl::StrCat("(list ", c.elem_wit, ")"));
  auto v = RunWithPlugin(decl, "list_size(xs)",
                            {{"xs", CelType::List(c.cel_elem)}}, *lib,
                            WatToWasm(wat), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), c.expected_size);
}

INSTANTIATE_TEST_SUITE_P(
    EachScalarElem, ListByElemKind,
    testing::Values(
        ListElemCase{"bool",
                     "bool",
                     "bool",
                     Prim(CelfnType::Kind::kBool),
                     CelType::Bool(),
                     {Value::Bool(true), Value::Bool(false)},
                     2},
        ListElemCase{"int",
                     "int",
                     "s64",
                     Prim(CelfnType::Kind::kInt),
                     CelType::Int(),
                     {Value::Int(1), Value::Int(2),
                      Value::Int(std::numeric_limits<int64_t>::min())},
                     3},
        ListElemCase{"uint",
                     "uint",
                     "u64",
                     Prim(CelfnType::Kind::kUint),
                     CelType::Uint(),
                     {Value::Uint(uint64_t{0}),
                      Value::Uint(std::numeric_limits<uint64_t>::max())},
                     2},
        ListElemCase{"double",
                     "double",
                     "f64",
                     Prim(CelfnType::Kind::kDouble),
                     CelType::Double(),
                     {Value::Double(1.5), Value::Double(2.5)},
                     2},
        ListElemCase{
            "string",
            "string",
            "string",
            Prim(CelfnType::Kind::kString),
            CelType::String(),
            {Value::String(""), Value::String("a"), Value::String("b")},
            3},
        ListElemCase{"bytes",
                     "bytes",
                     "(list u8)",
                     Prim(CelfnType::Kind::kBytes),
                     CelType::Bytes(),
                     {Value::Bytes("\x01"), Value::Bytes("")},
                     2},
        ListElemCase{"empty_int",
                     "int",
                     "s64",
                     Prim(CelfnType::Kind::kInt),
                     CelType::Int(),
                     {},
                     0}),
    [](const testing::TestParamInfo<ListElemCase>& info) {
      return info.param.label;
    });

TEST(PluginTypeMatrix, ListNestedListIntOuterSize) {
  auto lib = ForeignLibOne(
      "outer_size", Prim(CelfnType::Kind::kInt),
      {{false, ListOf(ListOf(Prim(CelfnType::Kind::kInt))), "xs"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("xs", Value::List({Value::List({Value::Int(1), Value::Int(2)}),
                              Value::List({})}));
  auto v = RunWithPlugin(
      "int @plugin.outer_size(list<list<int>> xs);", "outer_size(xs)",
      {{"xs", CelType::List(CelType::List(CelType::Int()))}}, *lib,
      WatToWasm(
          LenOfListArgWat("outer-size-list-list-int", "(list (list s64))")),
      act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 2);
}

// `three-ints : () -> list<s64>`.  The result list crosses via retptr:
// three i64 elements at 16/24/32, then {ptr=16, len=3} at the retptr
// slot 8.
constexpr absl::string_view kThreeIntsComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 1)
    (func (export "f") (result i32)
      i32.const 16
      i64.const 7
      i64.store
      i32.const 24
      i64.const 8
      i64.store
      i32.const 32
      i64.const 9
      i64.store
      i32.const 8
      i32.const 16
      i32.store
      i32.const 12
      i32.const 3
      i32.store
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (func (export "three-ints") (result (list s64))
    (canon lift (core func $i "f") (memory $mem))))
)WAT";

TEST(PluginTypeMatrix, ListReturnPluginEmitsThreeInts) {
  auto lib =
      ForeignLibOne("three_ints", ListOf(Prim(CelfnType::Kind::kInt)), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v =
      RunWithPlugin("list<int> @plugin.three_ints();", "three_ints()", {},
                       *lib, WatToWasm(kThreeIntsComponentWat), act);
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
// m24 §6 maps map<K,V> → WIT `list<tuple<wit K, wit V>>`, which lowers
// as a (ptr, len) pair like any list — so the size-observation core fn
// is the same len-returning shape as the list section.

struct MapKeyKindCase {
  std::string label;
  std::string key_celfn;
  std::string key_wit;  // WIT spelling for the tuple's key slot
  CelfnType key_celfn_type;
  CelType cel_key;
  std::vector<std::pair<Value, Value>> entries;
  int64_t expected_size;
};

class MapByKeyKind : public testing::TestWithParam<MapKeyKindCase> {};

TEST_P(MapByKeyKind, ArgSizeIsObservable) {
  const MapKeyKindCase& c = GetParam();
  auto lib = ForeignLibOne(
      "map_size", Prim(CelfnType::Kind::kInt),
      {{false, MapOf(c.key_celfn_type, Prim(CelfnType::Kind::kInt)), "m"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("m", Value::Map(c.entries));
  const std::string decl =
      absl::StrCat("int @plugin.map_size(map<", c.key_celfn, ", int> m);");
  const std::string export_name = Kebab(lib->decls()[0].overload_id);
  const std::string wat = LenOfListArgWat(
      export_name, absl::StrCat("(list (tuple ", c.key_wit, " s64))"));
  auto v = RunWithPlugin(decl, "map_size(m)",
                            {{"m", CelType::Map(c.cel_key, CelType::Int())}},
                            *lib, WatToWasm(wat), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), c.expected_size);
}

INSTANTIATE_TEST_SUITE_P(
    EachKeyKind, MapByKeyKind,
    testing::Values(MapKeyKindCase{"bool",
                                   "bool",
                                   "bool",
                                   Prim(CelfnType::Kind::kBool),
                                   CelType::Bool(),
                                   {{Value::Bool(true), Value::Int(1)},
                                    {Value::Bool(false), Value::Int(0)}},
                                   2},
                    MapKeyKindCase{"int",
                                   "int",
                                   "s64",
                                   Prim(CelfnType::Kind::kInt),
                                   CelType::Int(),
                                   {{Value::Int(-1), Value::Int(100)},
                                    {Value::Int(2), Value::Int(7)}},
                                   2},
                    MapKeyKindCase{
                        "uint",
                        "uint",
                        "u64",
                        Prim(CelfnType::Kind::kUint),
                        CelType::Uint(),
                        {{Value::Uint(uint64_t{5}), Value::Int(50)},
                         {Value::Uint(uint64_t{10}), Value::Int(100)}},
                        2},
                    MapKeyKindCase{"string",
                                   "string",
                                   "string",
                                   Prim(CelfnType::Kind::kString),
                                   CelType::String(),
                                   {{Value::String("a"), Value::Int(1)},
                                    {Value::String("b"), Value::Int(2)}},
                                   2},
                    MapKeyKindCase{"empty_string_keyed",
                                   "string",
                                   "string",
                                   Prim(CelfnType::Kind::kString),
                                   CelType::String(),
                                   {},
                                   0}),
    [](const testing::TestParamInfo<MapKeyKindCase>& info) {
      return info.param.label;
    });

// `size-at : map<string, list<s64>> -> s64`, where the component sums
// the inner-list sizes.  Map entry order on the wire is whatever
// HostMapBacking::ForEach yields (unspecified), so the component
// computes an ORDER-INDEPENDENT aggregate; with the fixture
// {"a":[1,2,3], "b":[]} the sum (3+0) equals the original "size at key
// a" expectation of 3 while keeping the WAT free of string compares.
// Entry layout: tuple<string, list<s64>> = {str_ptr, str_len, list_ptr,
// list_len} (4×i32, size 16) — the inner length lives at entry+12.
constexpr absl::string_view kSumInnerSizesComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      ;; ret = (next + align - 1) & -align — the canonical ABI validates
      ;; that realloc results honor the requested alignment (an
      ;; unaligned result fails the call with "result not aligned").
      global.get $next
      local.get $align
      i32.add
      i32.const 1
      i32.sub
      i32.const 0
      local.get $align
      i32.sub
      i32.and
      local.set $ret
      local.get $ret
      local.get $new_sz
      i32.add
      global.set $next
      local.get $ret)
    (func (export "f") (param $ptr i32) (param $len i32) (result i64)
      (local $idx i32) (local $acc i64)
      (block $done
        (loop $loop
          local.get $idx
          local.get $len
          i32.eq
          br_if $done
          local.get $acc
          local.get $ptr
          local.get $idx
          i32.const 16
          i32.mul
          i32.add
          i32.load offset=12
          i64.extend_i32_u
          i64.add
          local.set $acc
          local.get $idx
          i32.const 1
          i32.add
          local.set $idx
          br $loop))
      local.get $acc))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "f" (core func $f))
  (func (export "size-at-map-string-list-int")
      (param "m" (list (tuple string (list s64)))) (result s64)
    (canon lift (core func $f) (memory $mem) (realloc $realloc))))
)WAT";

TEST(PluginTypeMatrix, MapValueListNestedShapeRoundTrips) {
  auto lib = ForeignLibOne("size_at", Prim(CelfnType::Kind::kInt),
                           {{false,
                             MapOf(Prim(CelfnType::Kind::kString),
                                   ListOf(Prim(CelfnType::Kind::kInt))),
                             "m"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind(
      "m",
      Value::Map({{Value::String("a"),
                   Value::List({Value::Int(1), Value::Int(2), Value::Int(3)})},
                  {Value::String("b"), Value::List({})}}));
  auto v = RunWithPlugin(
      "int @plugin.size_at(map<string, list<int>> m);", "size_at(m)",
      {{"m", CelType::Map(CelType::String(), CelType::List(CelType::Int()))}},
      *lib, WatToWasm(kSumInnerSizesComponentWat), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsInt(), 3);
}

// `ages : () -> map<string, s64>` with two entries.  The result
// list<tuple<string, s64>> crosses via retptr; the entry tuples
// (string{ptr,len} @0, pad, s64 @8 — size 16, align 8) sit at 128,
// the key bytes come from a data segment at 64.
constexpr absl::string_view kAgesComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 1)
    (data (i32.const 64) "adabob")
    (func (export "f") (result i32)
      i32.const 128
      i32.const 64
      i32.store
      i32.const 132
      i32.const 3
      i32.store
      i32.const 136
      i64.const 36
      i64.store
      i32.const 144
      i32.const 67
      i32.store
      i32.const 148
      i32.const 3
      i32.store
      i32.const 152
      i64.const 41
      i64.store
      i32.const 8
      i32.const 128
      i32.store
      i32.const 12
      i32.const 2
      i32.store
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (func (export "ages") (result (list (tuple string s64)))
    (canon lift (core func $i "f") (memory $mem))))
)WAT";

TEST(PluginTypeMatrix, MapReturnPluginEmitsStringInt) {
  auto lib = ForeignLibOne(
      "ages",
      MapOf(Prim(CelfnType::Kind::kString), Prim(CelfnType::Kind::kInt)), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin("map<string, int> @plugin.ages();", "ages()", {},
                            *lib, WatToWasm(kAgesComponentWat), act);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kMap);
  auto mb = v->MapBacking();
  ASSERT_TRUE(mb.ok()) << mb.status();
  EXPECT_EQ((*mb)->Size(), 2u);
}

TEST(PluginTypeMatrix, MapMissingKeyAtPluginSurfacesNoSuchKey) {
  GTEST_SKIP() << "this cell needs the component to surface a CEL no-such-key "
                  "error through its return channel, but "
               << kNoErrorResultVariant;
}

// ═════════════════════════════════════════════════════════════════════
// PROTO — the m24 §8 "crosses as bytes" path (the m13 §4.5.1 ban lifted)
// ═════════════════════════════════════════════════════════════════════
//
// kPlugin admits proto(...) — the Lift side serializes the
// message to wire bytes (list<u8>); the Lower side parses list<u8>
// back through the generated descriptor pool.

// `first-letter-...-customer : list<u8> -> string`.  The arg is the
// proto wire encoding of Customer{name:"Ada", age:36}: proto3 C++
// serializes fields in field-number order and `name` is field 1
// (testdata/e2e_fixture.proto:46), so the bytes start
// 0x0A <len> 'A' 'd' 'a' … — the first letter of `name` is at arg
// offset 2 (valid while the fixture keeps name=field 1 and the name
// stays shorter than 128 bytes, i.e. a one-byte length varint).
constexpr absl::string_view kFirstLetterComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      ;; ret = (next + align - 1) & -align — the canonical ABI validates
      ;; that realloc results honor the requested alignment (an
      ;; unaligned result fails the call with "result not aligned").
      global.get $next
      local.get $align
      i32.add
      i32.const 1
      i32.sub
      i32.const 0
      local.get $align
      i32.sub
      i32.and
      local.set $ret
      local.get $ret
      local.get $new_sz
      i32.add
      global.set $next
      local.get $ret)
    (func (export "f") (param $ptr i32) (param $len i32) (result i32)
      i32.const 8
      local.get $ptr
      i32.const 2
      i32.add
      i32.store
      i32.const 12
      i32.const 1
      i32.store
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "f" (core func $f))
  (func (export "first-letter-message-celwasm-testdata-customer")
      (param "c" (list u8)) (result string)
    (canon lift (core func $f) (memory $mem) (realloc $realloc))))
)WAT";

TEST(PluginTypeMatrix, ProtoArgPluginReadsField) {
  Customer c;
  c.set_name("Ada");
  c.set_age(36);
  auto lib =
      ForeignLibOne("first_letter", Prim(CelfnType::Kind::kString),
                    {{false, ProtoOf("celwasm.testdata.Customer"), "c"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("c", Value::Message(c));
  auto v = RunWithPlugin(
      "string @plugin.first_letter(proto(celwasm.testdata.Customer) c);",
      "first_letter(c)", {{"c", CelType::Message("celwasm.testdata.Customer")}},
      *lib, WatToWasm(kFirstLetterComponentWat), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(std::string(*v->AsString()), "A");
}

// `build-customer-string : string -> list<u8>` — emits the proto wire
// encoding of Customer{name:<arg>} by hand: tag 0x0A (field 1,
// length-delimited), one-byte length varint (valid while len < 128),
// then the name bytes copied via memory.copy.  Output buffer at 2048
// (the bump realloc hands out from 1024; the test arg is 3 bytes, so
// no collision).
constexpr absl::string_view kBuildCustomerComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      ;; ret = (next + align - 1) & -align — the canonical ABI validates
      ;; that realloc results honor the requested alignment (an
      ;; unaligned result fails the call with "result not aligned").
      global.get $next
      local.get $align
      i32.add
      i32.const 1
      i32.sub
      i32.const 0
      local.get $align
      i32.sub
      i32.and
      local.set $ret
      local.get $ret
      local.get $new_sz
      i32.add
      global.set $next
      local.get $ret)
    (func (export "f") (param $ptr i32) (param $len i32) (result i32)
      i32.const 2048
      i32.const 10
      i32.store8
      i32.const 2049
      local.get $len
      i32.store8
      i32.const 2050
      local.get $ptr
      local.get $len
      memory.copy
      i32.const 8
      i32.const 2048
      i32.store
      i32.const 12
      local.get $len
      i32.const 2
      i32.add
      i32.store
      i32.const 8))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "f" (core func $f))
  (func (export "build-customer-string") (param "n" string)
      (result (list u8))
    (canon lift (core func $f) (memory $mem) (realloc $realloc))))
)WAT";

TEST(PluginTypeMatrix, ProtoReturnPluginEmitsCustomer) {
  auto lib =
      ForeignLibOne("build_customer", ProtoOf("celwasm.testdata.Customer"),
                    {{false, Prim(CelfnType::Kind::kString), "n"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("n", Value::String("Ada"));
  auto v = RunWithPlugin(
      "proto(celwasm.testdata.Customer) "
      "@plugin.build_customer(string n);",
      "build_customer(n).name", {{"n", CelType::String()}}, *lib,
      WatToWasm(kBuildCustomerComponentWat), act);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(std::string(*v->AsString()), "Ada");
}

// One pin at the interface layer — Builder admits proto(...) for
// kPlugin and synthesizes the message_<fqn> overload-id.
TEST(PluginTypeMatrix, BuilderAdmitsProtoForPlugin) {
  auto lib =
      FunctionLibrary::Builder()
          .AddPlugin("first_letter", Prim(CelfnType::Kind::kString),
                               {{false, ProtoOf("acme.User"), "u"}})
          .Build();
  ASSERT_TRUE(lib.ok()) << lib.status();
  ASSERT_EQ(lib->decls().size(), 1u);
  EXPECT_EQ(lib->decls()[0].backend, CelfnDecl::Backend::kPlugin);
  EXPECT_EQ(lib->decls()[0].overload_id, "first_letter_message_acme_User");
  EXPECT_EQ(lib->decls()[0].module_name, "cel_fn");
}

// Argkind synthesis for the kType / kOptional CelfnType kinds (the
// kinds themselves exist for the kHost surface even though the
// plugin decl surface rejects them).
TEST(PluginTypeMatrix, ArgkindForNewKinds) {
  EXPECT_EQ(TypeOfTypes().Argkind(), "type");
  EXPECT_EQ(OptionalOf(Prim(CelfnType::Kind::kInt)).Argkind(), "optional_int");
  EXPECT_EQ(OptionalOf(ListOf(Prim(CelfnType::Kind::kString))).Argkind(),
            "optional_list_string");
}

// ═════════════════════════════════════════════════════════════════════
// NEGATIVE — m24 §11 forcing rows
// ═════════════════════════════════════════════════════════════════════

// Decl says one CEL param; the component's export takes two.  Neither
// AddPlugin (parses bytes + conflict-checks overload-ids only) nor
// Plan (resolves the export by name and checks it is a function —
// BindOnePluginDecl, eval/engine.cc:1083 — with no signature
// introspection) catches the mismatch; it surfaces at Eval when
// wasmtime_component_func_call rejects the arg-count.
constexpr absl::string_view kTwoArgComponentWat = R"WAT(
(component
  (core module $m
    (func (export "f") (param i64 i64) (result i64) local.get 0))
  (core instance $i (instantiate $m))
  (func (export "two-arg-int") (param "a" s64) (param "b" s64) (result s64)
    (canon lift (core func $i "f"))))
)WAT";

TEST(PluginTypeMatrix, NegativeWrongArityFailsAtEval) {
  auto lib = ForeignLibOne("two_arg", Prim(CelfnType::Kind::kInt),
                           {{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = RunWithPlugin("int @plugin.two_arg(int x);", "two_arg(x)",
                            {{"x", CelType::Int()}}, *lib,
                            WatToWasm(kTwoArgComponentWat), act);
  ASSERT_FALSE(v.ok()) << "arity-mismatched plugin call produced a value";
  EXPECT_THAT(std::string(v.status().message()),
              HasSubstr("plugin func call"));
}

TEST(PluginTypeMatrix, NegativeMissingExportFailsAtPlan) {
  // Decl names a fn `not_exported` not present in the component.  The
  // export ↔ decl lookup happens at per-Plan component instantiation
  // (see plugin_dispatch_test.cc
  // MissingExportFailsAtPlanNotAddPlugin), so the failure is a
  // FailedPrecondition from Engine::Plan naming the kebab-case export.
  auto lib = ForeignLibOne("not_exported", Prim(CelfnType::Kind::kInt), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v = RunWithPlugin("int @plugin.not_exported();", "not_exported()",
                            {}, *lib, WatToWasm(kTwoArgComponentWat), act);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(v.status().code(), absl::StatusCode::kFailedPrecondition)
      << v.status();
  EXPECT_THAT(std::string(v.status().message()), HasSubstr("not-exported"));
}

TEST(PluginTypeMatrix,
     NegativeMalformedPluginBytesAtAddPlugin) {
  auto lib = ForeignLibOne("any_fn", Prim(CelfnType::Kind::kInt), {});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  const std::vector<uint8_t> garbage = {0xde, 0xad, 0xbe, 0xef};
  auto st = engine->AddPlugin(garbage, *lib);
  ASSERT_FALSE(st.ok());
  // As shipped, every wasmtime_error_t funnels through
  // WasmtimeErrorToStatus (eval/engine.cc:44), which maps to
  // kFailedPrecondition — including the component-parse failure here.
  EXPECT_EQ(st.code(), absl::StatusCode::kFailedPrecondition) << st;
  EXPECT_THAT(std::string(st.message()), HasSubstr("parse plugin"));
}

TEST(PluginTypeMatrix, NegativeFnReturnsEvalErrorBecomesCelError) {
  GTEST_SKIP() << kNoErrorResultVariant;
}

constexpr absl::string_view kTrappingComponentWat = R"WAT(
(component
  (core module $m
    (func (export "boom") (param i64) (result i64) unreachable))
  (core instance $i (instantiate $m))
  (func (export "boom-int") (param "x" s64) (result s64)
    (canon lift (core func $i "boom"))))
)WAT";

TEST(PluginTypeMatrix, NegativePluginTrapBecomesHostStatus) {
  // Component traps (`unreachable`); Instance::Eval returns a non-OK
  // absl::Status (NOT a Value::Error).
  auto lib = ForeignLibOne("boom", Prim(CelfnType::Kind::kInt),
                           {{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  act.Bind("x", Value::Int(1));
  auto v = RunWithPlugin("int @plugin.boom(int x);", "boom(x)",
                            {{"x", CelType::Int()}}, *lib,
                            WatToWasm(kTrappingComponentWat), act);
  ASSERT_FALSE(v.ok()) << "trapping plugin fn produced a value";
}

TEST(PluginTypeMatrix,
     NegativeThreeValuedLogicErrorArgShortCircuits) {
  // 3VL: an error arg short-circuits BEFORE any marshaling
  // (AbsorbUnknownOrErrorArg in PluginCallbackTrampoline,
  // eval/engine.cc:857) and the component is never invoked — the
  // component body traps if it runs, so a clean Value::Error result
  // proves the absorption.  `1 / 0` produces the CEL divide-by-zero
  // error (langdef "Arithmetic errors").
  auto lib = ForeignLibOne("use_int", Prim(CelfnType::Kind::kInt),
                           {{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  Activation act;
  auto v =
      RunWithPlugin("int @plugin.use_int(int x);", "use_int(1 / 0)", {},
                       *lib, WatToWasm(TrapIfInvokedWat("use-int-int")), act);
  ASSERT_TRUE(v.ok()) << v.status();  // a trap would surface as non-OK
  EXPECT_TRUE(v->IsError());
}

TEST(PluginTypeMatrix,
     NegativeThreeValuedLogicUnknownArgShortCircuits) {
  // Same absorption contract for unknowns: PartialEval with an
  // AttributePattern over the fn's arg yields an unknown result and
  // the (trap-if-invoked) component never runs.
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  b.AddFunction("int @plugin.use_unk(int x);");
  auto compiler = std::move(b).Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto program = compiler->Compile("use_unk(x)");
  ASSERT_TRUE(program.ok()) << program.status();
  auto lib = ForeignLibOne("use_unk", Prim(CelfnType::Kind::kInt),
                           {{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  ASSERT_TRUE(
      engine->AddPlugin(WatToWasm(TrapIfInvokedWat("use-unk-int")), *lib)
          .ok());
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  Activation act;
  act.Bind("x", Value::Int(7));
  auto pat = AttributePattern::Parse("x");
  ASSERT_TRUE(pat.ok()) << pat.status();
  std::vector<AttributePattern> pats;
  pats.push_back(*std::move(pat));
  auto v = instance->PartialEval(act, pats);
  ASSERT_TRUE(v.ok()) << v.status();  // a trap would surface as non-OK
  EXPECT_TRUE(v->IsUnknown());
}

// ═════════════════════════════════════════════════════════════════════
// FORCING — m24 §11 forcing function (TinyGo component implementing the
// same fns.wit, proving the contract is language-agnostic)
// ═════════════════════════════════════════════════════════════════════

TEST(PluginTypeMatrix, TinyGoBackedPluginProducesSameResult) {
  GTEST_SKIP() << "Engine::AddPlugin itself shipped with m24 "
                  "(eval/engine.cc:1519; dispatch proven in "
                  "plugin_dispatch_test.cc) — this row is blocked "
                  "solely on the TinyGo build fixture: no target exists under "
                  "e2e/plugin_fixtures/tinygo/ (verified "
                  "2026-06-10).";
}

// ═════════════════════════════════════════════════════════════════════
// IDL — m24 §6 declarations via .celfn source
// ═════════════════════════════════════════════════════════════════════
//
// The `@plugin.` decl prefix landed in the grammar
// (compiler/celfn/Celfn.g4 pluginFnDecl) — pinned positive below
// and exercised implicitly by every RunWithPlugin test above.  The
// `type` / `option(...)` keywords never will (m24 §14 permanent
// closure), so those rows stay skipped with the closure cited.

TEST(PluginTypeMatrix, CelfnSourceAdmitsPluginDecl) {
  auto lib = ParseCelfnSource("string @plugin.first_letter(string s);");
  ASSERT_TRUE(lib.ok()) << lib.status();
  ASSERT_EQ(lib->decls().size(), 1u);
  EXPECT_EQ(lib->decls()[0].backend, CelfnDecl::Backend::kPlugin);
  EXPECT_EQ(lib->decls()[0].overload_id, "first_letter_string");
}

TEST(PluginTypeMatrix, CelfnSourceAdmitsTypeKeyword) {
  GTEST_SKIP() << "the celfn grammar (compiler/celfn/Celfn.g4 `type` rule) "
                  "has no `type` keyword alternative — verified 2026-06-10 — "
                  "and m24 §14 PERMANENTLY rejects `type` as a "
                  "plugin shape (pinned by "
                  "TypeArgRejectedAtLibraryBuild / "
                  "TypeReturnRejectedAtLibraryBuild), so the keyword will "
                  "not land for this backend.";
}

TEST(PluginTypeMatrix, CelfnSourceAdmitsOptionalKeyword) {
  GTEST_SKIP() << "the celfn grammar (compiler/celfn/Celfn.g4 `type` rule) "
                  "has no `option(...)` alternative — verified 2026-06-10 — "
                  "and m24 §14 PERMANENTLY rejects optional<T> as a "
                  "plugin shape (pinned by "
                  "OptionalArgRejectedAtLibraryBuild), so the keyword will "
                  "not land for this backend.";
}

}  // namespace
}  // namespace celwasm
