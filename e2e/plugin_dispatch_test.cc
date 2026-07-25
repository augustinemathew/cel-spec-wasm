// e2e: end-to-end dispatch through `Engine::AddPlugin` for a
// kPlugin decl — Compile → Plan → plugin instantiation →
// Eval → result.  Uses inline component-model WAT assembled by
// `wasmtime_wat2wasm` (confirmed accepted by the vendored wasmtime),
// so the test is self-contained: no wit-bindgen, no wasi-sdk, no
// out-of-band build.
//
// Coverage focus: pin the dispatch wiring (marshaling layer round-trip
// + per-Plan instantiation + call) for the load-bearing CEL types
// (int, string, bool) in both directions.  The broader §6 / §10
// matrix from `e2e/foreign_fn_type_matrix_test.cc` is a sibling and
// remains SKIP'd against per-decl fixtures (lands with the celfnc
// generator).  This file proves the C.1-C.3 path is wired correctly;
// the matrix file is the exhaustive type sweep.
//
// Link-mode coverage: built twice via `link_mode_e2e_cc_test`
// (`_dynamic` / `_static` — see
// doc/implementation-plan/rewrite/m28-configurable-linking.md §5.5),
// so plugin dispatch is exercised against both dynamically-linked
// and merged-runtime Programs.  Each test keeps its own Engine
// (plugin registrations are per-Engine state), so link mode is
// routed through `e2e::DefaultOpts()` on each Compile call rather
// than through `e2e::CompilePlan` (which would share GlobalEngine).

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Assemble component-model WAT into bytes via wasmtime_wat2wasm.
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

FunctionLibrary OneFnLib(absl::string_view fn_name, CelfnType return_type,
                         std::vector<CelfnParam> params) {
  auto lib_or = FunctionLibrary::Builder()
                    .AddPlugin(fn_name, std::move(return_type),
                                         std::move(params))
                    .Build();
  ABSL_CHECK(lib_or.ok()) << lib_or.status();
  return *std::move(lib_or);
}

// A component exporting `add-int-int : (s64,s64) -> s64`.  Component-
// Model exports must be kebab-case (snake_case fails parse with
// "not a valid extern name").  The engine converts the CEL
// overload-id `add_int_int` to `add-int-int` at the
// `wasmtime_component_instance_get_export_index` call site, so the
// component's authoring side stays idiomatic WIT.
constexpr absl::string_view kAddIntIntComponentWat = R"WAT(
(component
  (core module $m
    (func (export "add") (param i64 i64) (result i64)
      local.get 0 local.get 1 i64.add))
  (core instance $i (instantiate $m))
  (func (export "add-int-int") (param "a" s64) (param "b" s64) (result s64)
    (canon lift (core func $i "add"))))
)WAT";

TEST(PluginDispatch, IntAddRoundTripsBoundaryValues) {
  // The dispatch path: AddPlugin → AddPlugin →
  // Compile("add(a,b)") → Plan → Eval(activation{a,b}) → result.
  // Pins that arg lift + component call + result lower work for
  // the integer scalar arm.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneFnLib("add", Prim(CelfnType::Kind::kInt),
                      {CelfnParam{false, Prim(CelfnType::Kind::kInt), "a"},
                       CelfnParam{false, Prim(CelfnType::Kind::kInt), "b"}});
  const std::vector<uint8_t> plugin_bytes =
      WatToWasm(kAddIntIntComponentWat);
  ASSERT_THAT(engine_or->AddPlugin(plugin_bytes, lib), IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("a", CelType::Int())
      .DeclareVariable("b", CelType::Int())
      .AddLibrary(lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("add(a, b)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();

  // Boundary inputs covered: 0 + 0, INT64_MIN + 0 (no overflow), and
  // a small positive sum.
  struct Case {
    int64_t a;
    int64_t b;
    int64_t expected;
  };
  for (auto c : std::vector<Case>{{0, 0, 0},
                                  {1, 2, 3},
                                  {-7, 7, 0},
                                  {std::numeric_limits<int64_t>::min(), 0,
                                   std::numeric_limits<int64_t>::min()}}) {
    Activation act;
    act.Bind("a", Value::Int(c.a));
    act.Bind("b", Value::Int(c.b));
    auto v_or = inst_or->Eval(act);
    ASSERT_THAT(v_or, IsOk()) << "a=" << c.a << " b=" << c.b;
    EXPECT_EQ(*v_or->AsInt(), c.expected) << "a=" << c.a << " b=" << c.b;
  }
}

constexpr absl::string_view kPassthroughBoolBoolComponentWat = R"WAT(
(component
  (core module $m
    (func (export "id") (param i32) (result i32) local.get 0))
  (core instance $i (instantiate $m))
  (func (export "ident-bool") (param "x" bool) (result bool)
    (canon lift (core func $i "id"))))
)WAT";

TEST(PluginDispatch, BoolPassthroughRoundTrips) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneFnLib("ident", Prim(CelfnType::Kind::kBool),
                      {CelfnParam{false, Prim(CelfnType::Kind::kBool), "x"}});
  ASSERT_THAT(
      engine_or->AddPlugin(WatToWasm(kPassthroughBoolBoolComponentWat), lib),
      IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("x", CelType::Bool()).AddLibrary(lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("ident(x)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();

  for (bool b : {false, true}) {
    Activation act;
    act.Bind("x", Value::Bool(b));
    auto v_or = inst_or->Eval(act);
    ASSERT_THAT(v_or, IsOk()) << "b=" << b;
    EXPECT_EQ(*v_or->AsBool(), b);
  }
}

// ── Large-payload dispatch at MiB / 10⁵-element scale ──────────────
//
// The marshaling-layer test (`eval/internal/cel_plugin_test.cc::
// Large…`) proves Lift / Lower handle large CelValues; this proves
// the full pipeline does — every layer the dispatch path crosses:
//
//   eval-side memcpy →  HostCallContext::ArgString / ArgList → cel
//   Lift to component val → wasmtime canonical-ABI copy into the
//   component's memory (the part we can't unit-test in isolation —
//   needs wasmtime executing a real component) → core fn → canonical-
//   ABI lift of the s64 return → cel Lower → HostCallContext::Return
//   → eval-side write.
//
// We pick result types that don't need component-side allocation
// for the return (s64), so the WAT can stay terse: a single core
// function + a bump realloc for the incoming aggregate.  The
// boundary case caught here is *transport size*, not value range.

// Component: `len-string : string -> s64`.  16-page memory (= 1 MiB)
// fits a 256 KiB string with headroom; the bump-realloc is sufficient
// because each call is a fresh component instantiation via Plan.
constexpr absl::string_view kStringLenComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      global.get $next
      local.set $ret
      global.get $next
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
  (func (export "len-string") (param "s" string) (result s64)
    (canon lift (core func $len_fn) (memory $mem) (realloc $realloc))))
)WAT";

TEST(PluginDispatch, LargeStringTransportsAtMiBScale) {
  // 256 KiB string crosses the dispatch boundary; the component
  // returns the byte-length so the assertion is byte-exact and
  // tiny — the *transport* is what's under test.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneFnLib("len", Prim(CelfnType::Kind::kInt),
                      {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kStringLenComponentWat), lib),
              IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("s", CelType::String()).AddLibrary(lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("len(s)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();

  for (size_t n : {size_t{0}, size_t{1}, size_t{1 << 10}, size_t{256} * 1024}) {
    // ASCII-only payload: the canonical ABI validates strings as UTF-8.
    std::string payload(n, 'x');
    Activation act;
    act.Bind("s", Value::String(payload));
    auto v_or = inst_or->Eval(act);
    ASSERT_THAT(v_or, IsOk()) << "n=" << n;
    EXPECT_EQ(*v_or->AsInt(), static_cast<int64_t>(n)) << "n=" << n;
  }
}

// Component: `sum-list-int : list<s64> -> s64`.  Reads each i64 from
// the flat element array the canonical ABI builds in memory, sums.
// 16-page memory (1 MiB) fits 128 K i64s with margin; the test
// scales to 100 000 elements (800 KB).
constexpr absl::string_view kListSumComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      global.get $next
      local.set $ret
      global.get $next
      local.get $new_sz
      i32.add
      global.set $next
      local.get $ret)
    (func (export "sum_fn") (param $ptr i32) (param $len i32) (result i64)
      (local $i i32) (local $end i32) (local $acc i64)
      local.get $ptr
      local.set $i
      local.get $ptr
      local.get $len
      i32.const 8
      i32.mul
      i32.add
      local.set $end
      (block $done
        (loop $loop
          local.get $i
          local.get $end
          i32.eq
          br_if $done
          local.get $acc
          local.get $i
          i64.load
          i64.add
          local.set $acc
          local.get $i
          i32.const 8
          i32.add
          local.set $i
          br $loop))
      local.get $acc))
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (alias core export $i "sum_fn" (core func $sum_fn))
  (func (export "sum-list-int") (param "xs" (list s64)) (result s64)
    (canon lift (core func $sum_fn) (memory $mem) (realloc $realloc))))
)WAT";

TEST(PluginDispatch, LargeListIntTransportsAt100kElements) {
  // 100 000 element list crosses the boundary; the component sums
  // and returns one s64.  Boundary-style payload: sum_{0..N-1} = N(N-1)/2.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib =
      OneFnLib("sum", Prim(CelfnType::Kind::kInt),
               {CelfnParam{false, ListOf(Prim(CelfnType::Kind::kInt)), "xs"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kListSumComponentWat), lib),
              IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("xs", CelType::List(CelType::Int())).AddLibrary(lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("sum(xs)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();

  for (size_t n : {size_t{0}, size_t{1}, size_t{1024}, size_t{100'000}}) {
    std::vector<Value> elems;
    elems.reserve(n);
    int64_t expected = 0;
    for (size_t i = 0; i < n; ++i) {
      elems.push_back(Value::Int(static_cast<int64_t>(i)));
      expected += static_cast<int64_t>(i);
    }
    Activation act;
    act.Bind("xs", Value::List(std::move(elems)));
    auto v_or = inst_or->Eval(act);
    ASSERT_THAT(v_or, IsOk()) << "n=" << n;
    EXPECT_EQ(*v_or->AsInt(), expected) << "n=" << n;
  }
}

// ── String return path ─────────────────────────────────────────────
//
// `echo-string : string -> string`.  A string RESULT exercises the
// canonical-ABI return-pointer arm (a lifted result that flattens to
// more than one core value crosses via an i32 pointer to a {ptr,len}
// pair in component memory) plus the eval-side string Lower — neither
// of which the arg-only `len-string` case touches.  The core fn
// writes the realloc'd arg's {ptr,len} to a scratch slot at address 8
// (realloc starts handing out memory at 1024, so 8 is free) and
// returns that slot's address.
constexpr absl::string_view kStringEchoComponentWat = R"WAT(
(component
  (core module $m
    (memory (export "memory") 16)
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc")
        (param $orig i32) (param $orig_sz i32) (param $align i32)
        (param $new_sz i32) (result i32)
      (local $ret i32)
      global.get $next
      local.set $ret
      global.get $next
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
  (func (export "echo-string") (param "s" string) (result string)
    (canon lift (core func $echo) (memory $mem) (realloc $realloc))))
)WAT";

TEST(PluginDispatch, StringEchoReturnsStringResult) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneFnLib("echo", Prim(CelfnType::Kind::kString),
                      {CelfnParam{false, Prim(CelfnType::Kind::kString), "s"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kStringEchoComponentWat), lib),
              IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("s", CelType::String()).AddLibrary(lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("echo(s)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();

  // Empty, ASCII, and multi-byte UTF-8 payloads (the canonical ABI
  // validates strings as UTF-8; "héllo→😀" covers 1- to 4-byte
  // encodings).
  for (const std::string& payload :
       {std::string(), std::string("hello"), std::string("héllo→😀")}) {
    Activation act;
    act.Bind("s", Value::String(payload));
    auto v_or = inst_or->Eval(act);
    ASSERT_THAT(v_or, IsOk()) << "payload=" << payload;
    EXPECT_EQ(std::string(*v_or->AsString()), payload);
  }
}

// ── Error paths ─────────────────────────────────────────────────────

TEST(PluginDispatch, MissingExportFailsAtPlanNotAddPlugin) {
  // The embedder declares `frob` but registers a plugin that only
  // exports `add-int-int`.  Pins WHERE the mismatch surfaces:
  // `AddPlugin` only conflict-checks overload-ids and parses the
  // bytes; the export ↔ decl lookup happens at per-Plan component
  // instantiation, so the failure is a FailedPrecondition from
  // `Engine::Plan`, naming the kebab-case export it looked for.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneFnLib("frob", Prim(CelfnType::Kind::kInt),
                      {CelfnParam{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kAddIntIntComponentWat), lib),
              IsOk())
      << "AddPlugin validates bytes + overload-id conflicts only; "
         "the export lookup is deferred to Plan";

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("x", CelType::Int()).AddLibrary(lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("frob(x)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition)
      << inst_or.status();
  EXPECT_THAT(std::string(inst_or.status().message()),
              ::testing::HasSubstr("frob-int"));
}

// `boom-int : s64 -> s64` whose core body traps (`unreachable`).
constexpr absl::string_view kTrappingComponentWat = R"WAT(
(component
  (core module $m
    (func (export "boom") (param i64) (result i64) unreachable))
  (core instance $i (instantiate $m))
  (func (export "boom-int") (param "x" s64) (result s64)
    (canon lift (core func $i "boom"))))
)WAT";

TEST(PluginDispatch, TrappingPluginFnFailsEvalCleanly) {
  // A plugin fn that traps mid-call must surface as a clean
  // non-OK status from `Instance::Eval` — not a crash, not a
  // plausible-looking value.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto lib = OneFnLib("boom", Prim(CelfnType::Kind::kInt),
                      {CelfnParam{false, Prim(CelfnType::Kind::kInt), "x"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kTrappingComponentWat), lib),
              IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("x", CelType::Int()).AddLibrary(lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("boom(x)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();

  Activation act;
  act.Bind("x", Value::Int(1));
  auto v_or = inst_or->Eval(act);
  ASSERT_FALSE(v_or.ok()) << "trapping plugin fn produced a value";
}

}  // namespace
}  // namespace celwasm
