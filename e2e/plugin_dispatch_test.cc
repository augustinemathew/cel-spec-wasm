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
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "abi/plugin.h"
#include "abi/wasm_binary.h"
#include "absl/log/absl_check.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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
#include "tools/cel/run_embed_decls.h"
#include "tools/cpp/runfiles/runfiles.h"
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

CelType ListOf(CelType elem) {
  return CelType::List(std::move(elem));
}

FunctionLibrary OneFnLib(absl::string_view fn_name, CelType return_type,
                         std::vector<CelfnParam> params) {
  auto lib_or =
      FunctionLibrary::Builder()
          .AddPlugin(fn_name, std::move(return_type), std::move(params))
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
  auto lib = OneFnLib("add", CelType::Int(),
                      {CelfnParam{false, CelType::Int(), "a"},
                       CelfnParam{false, CelType::Int(), "b"}});
  const std::vector<uint8_t> plugin_bytes = WatToWasm(kAddIntIntComponentWat);
  ASSERT_THAT(engine_or->AddPlugin(plugin_bytes, lib), IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("a", CelType::Int())
      .DeclareVariable("b", CelType::Int())
      .DeclareFunctions(lib);
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
  auto lib = OneFnLib("ident", CelType::Bool(),
                      {CelfnParam{false, CelType::Bool(), "x"}});
  ASSERT_THAT(
      engine_or->AddPlugin(WatToWasm(kPassthroughBoolBoolComponentWat), lib),
      IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("x", CelType::Bool()).DeclareFunctions(lib);
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
  auto lib = OneFnLib("len", CelType::Int(),
                      {CelfnParam{false, CelType::String(), "s"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kStringLenComponentWat), lib),
              IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("s", CelType::String()).DeclareFunctions(lib);
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
  auto lib = OneFnLib("sum", CelType::Int(),
                      {CelfnParam{false, ListOf(CelType::Int()), "xs"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kListSumComponentWat), lib),
              IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("xs", CelType::List(CelType::Int()))
      .DeclareFunctions(lib);
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
  auto lib = OneFnLib("echo", CelType::String(),
                      {CelfnParam{false, CelType::String(), "s"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kStringEchoComponentWat), lib),
              IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("s", CelType::String()).DeclareFunctions(lib);
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
  auto lib = OneFnLib("frob", CelType::Int(),
                      {CelfnParam{false, CelType::Int(), "x"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kAddIntIntComponentWat), lib),
              IsOk())
      << "AddPlugin validates bytes + overload-id conflicts only; "
         "the export lookup is deferred to Plan";

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("x", CelType::Int()).DeclareFunctions(lib);
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
  auto lib = OneFnLib("boom", CelType::Int(),
                      {CelfnParam{false, CelType::Int(), "x"}});
  ASSERT_THAT(engine_or->AddPlugin(WatToWasm(kTrappingComponentWat), lib),
              IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("x", CelType::Int()).DeclareFunctions(lib);
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

// ── Plan-time required-function verification (m35 §2/§5.3) ─────────
//
// The Plan-time check unit matrix lives in
// eval/internal/required_fn_check_test.cc (hand-built rows + registry
// states, every signature axis).  The cases below pin the two §2
// headline shapes and the two §5.3 host shapes through the FULL
// pipeline: Compile (required_functions emission) → Plan (check).

using ::bazel::tools::cpp::runfiles::Runfiles;

// Loads the macro-built `demo_plugin.wasm` (self-describing: carries
// `cel.fns` with Module customfn; @plugin.{greet,add,len}).
std::vector<uint8_t> LoadDemoPluginBytes() {
  std::string error;
  auto runfiles = absl::WrapUnique(Runfiles::CreateForTest(&error));
  ABSL_CHECK(runfiles != nullptr) << "runfiles init failed: " << error;
  const std::string path = runfiles->Rlocation(
      "_main/e2e/plugin_fixtures/cel_wasm_plugin_demo/demo_plugin.wasm");
  ABSL_CHECK(!path.empty()) << "demo_plugin.wasm not in runfiles";
  std::ifstream f(path, std::ios::binary);
  ABSL_CHECK(f.is_open()) << "failed to open " << path;
  return {(std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>()};
}

// Returns `bytes` minus the top-level custom section named `name`
// (which must exist).  Works on both wasm layers — the id/size
// framing is identical; LEB decoding comes from //abi:wasm_binary.
std::vector<uint8_t> RemoveTopLevelCustomSection(
    absl::Span<const uint8_t> bytes, absl::string_view name) {
  std::vector<uint8_t> out(bytes.begin(), bytes.begin() + 8);
  size_t pos = 8;
  bool removed = false;
  while (pos < bytes.size()) {
    const size_t section_start = pos;
    const uint8_t section_id = bytes[pos++];
    uint32_t size = 0;
    ABSL_CHECK(ReadLeb128U32(bytes, &pos, &size));
    const size_t body_end = pos + size;
    ABSL_CHECK_LE(body_end, bytes.size());
    bool is_target = false;
    if (section_id == 0) {
      size_t p = pos;
      uint32_t name_len = 0;
      ABSL_CHECK(ReadLeb128U32(bytes, &p, &name_len));
      const absl::string_view section_name(
          reinterpret_cast<const char*>(bytes.data() + p), name_len);
      is_target = section_name == name;
    }
    if (is_target) {
      removed = true;
    } else {
      out.insert(out.end(), bytes.begin() + section_start,
                 bytes.begin() + body_end);
    }
    pos = body_end;
  }
  ABSL_CHECK(removed) << "no `" << name << "` custom section to remove";
  return out;
}

// Rebuilds a Program's `cel.abi` WITHOUT field 8 — the legacy
// (pre-required_functions) wire shape, which cannot be produced by
// the in-tree compiler anymore, so it is crafted by section surgery:
// decode the payload, clear the field, re-frame the section.
std::vector<uint8_t> StripRequiredFunctions(
    absl::Span<const uint8_t> program_bytes) {
  auto payload_or = FindCustomSection(program_bytes, "cel.abi");
  ABSL_CHECK_OK(payload_or.status());
  celwasm::abi::CelAbi abi;
  ABSL_CHECK(abi.ParseFromArray(payload_or->data(),
                                static_cast<int>(payload_or->size())));
  ABSL_CHECK_GT(abi.required_functions_size(), 0)
      << "fixture Program must carry required_functions to strip";
  abi.clear_required_functions();
  const std::string new_payload = abi.SerializeAsString();
  const std::vector<uint8_t> without =
      RemoveTopLevelCustomSection(program_bytes, "cel.abi");
  auto with_or =
      AppendCustomSection(without, "cel.abi",
                          {reinterpret_cast<const uint8_t*>(new_payload.data()),
                           new_payload.size()});
  ABSL_CHECK_OK(with_or.status());
  return *std::move(with_or);
}

// A minimal self-describing plugin: `mul-int-int` under the derived
// interface `cel:mymath/fns@0.1.0`, with the matching `cel.fns`
// declaration text appended with the production framing writer.
constexpr absl::string_view kMulIfaceComponentWat = R"WAT(
(component
  (core module $m
    (func (export "mul") (param i64 i64) (result i64)
      (i64.mul (local.get 0) (local.get 1))))
  (core instance $ci (instantiate $m))
  (func $mul (param "a" s64) (param "b" s64) (result s64)
    (canon lift (core func $ci "mul")))
  (instance $fns (export "mul-int-int" (func $mul)))
  (export "cel:mymath/fns@0.1.0" (instance $fns)))
)WAT";

constexpr absl::string_view kMulIdl =
    "Module mymath;\nint @plugin.mul(int a, int b);\n";

Plugin LoadMulPlugin() {
  const std::vector<uint8_t> component = WatToWasm(kMulIfaceComponentWat);
  auto with_fns = AppendCustomSection(
      component, "cel.fns",
      {reinterpret_cast<const uint8_t*>(kMulIdl.data()), kMulIdl.size()});
  ABSL_CHECK_OK(with_fns.status());
  auto plugin_or = Plugin::Load(*with_fns);
  ABSL_CHECK_OK(plugin_or.status());
  return *std::move(plugin_or);
}

TEST(RequiredFnPlanCheck, MissingPluginFnFailsAtPlanWithFrozenMessage) {
  // Compile with `Use` (one-noun flow), Plan on an engine that was
  // never given the plugin: the §2 missing-plugin shape, verbatim.
  const Plugin mul_plugin = LoadMulPlugin();
  auto builder = Compiler::NewBuilder();
  builder.Use(mul_plugin);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("mul(2, 3)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition)
      << inst_or.status();
  EXPECT_EQ(inst_or.status().message(),
            "Engine::Plan: program requires plugin function `mul_int_int` "
            "(`int mul(int, int)`) but no registered plugin declares it; "
            "register the providing plugin with Engine::Use before Plan");
}

TEST(RequiredFnPlanCheck, SignatureMismatchFailsAtPlanWithFrozenMessage) {
  // The plugin-update-drift scenario: the engine holds a rebuilt
  // demo plugin whose `add` changed its return type (int → string)
  // out from under a Program compiled against the pristine decls.
  // The rebuilt artifact is crafted by real section surgery: strip
  // the embedded `cel.fns`, re-embed a modified idl with the
  // production `cel embed-decls` core.  Overload-ids (and therefore
  // the kebab exports) are unchanged, so `Engine::Use`'s static
  // export check still passes — only Plan's signature compare can
  // catch the drift, with the §2 mismatch shape verbatim (including
  // the plugin hash rendered as its first 12 lowercase hex chars).
  const std::vector<uint8_t> demo_bytes = LoadDemoPluginBytes();
  auto pristine_or = Plugin::Load(demo_bytes);
  ASSERT_THAT(pristine_or, IsOk());

  const std::vector<uint8_t> stripped =
      RemoveTopLevelCustomSection(demo_bytes, "cel.fns");
  constexpr absl::string_view kModifiedIdl =
      "Module customfn;\n"
      "string @plugin.greet(string name, int age);\n"
      "string @plugin.add(int a, int b);\n"
      "int    @plugin.len(string s);\n";
  auto modified_bytes_or = tools::cel::EmbedDecls(stripped, kModifiedIdl);
  ASSERT_THAT(modified_bytes_or, IsOk());
  auto modified_or = Plugin::Load(*modified_bytes_or);
  ASSERT_THAT(modified_or, IsOk());

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  ASSERT_THAT(engine_or->Use(*modified_or), IsOk())
      << "Use checks export existence only; the kebab exports are "
         "unchanged, so the drift must survive to Plan";

  auto builder = Compiler::NewBuilder();
  builder.Use(*pristine_or);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("add(1, 2)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition)
      << inst_or.status();
  EXPECT_EQ(inst_or.status().message(),
            absl::StrCat(
                "Engine::Plan: program requires plugin function `add_int_int` "
                "with signature `int add(int, int)` but the registered plugin "
                "(hash ",
                modified_or->hash_hex().substr(0, 12),
                ") declares `string add(int, int)`; signatures must match "
                "exactly — recompile the program or rebuild the plugin"));
}

TEST(RequiredFnPlanCheck, ProtoFqnMismatchEndToEnd) {
  GTEST_SKIP() << R"CELSKIP(CELSKIP v1
reason: harness-limit
why-not-a-bug: blocked on the demo_plugin_proto FIXTURE, not on the product
  path under test.  That fixture cross-compiles libprotobuf under the WASI
  sysroot, where absl synchronization does not build, so nothing may depend
  on its bytes.  The proto-FQN axis itself is covered at unit level by
  required_fn_check_test.ProtoFqnMismatchExactFrozenMessage; this row would
  only add the end-to-end wiring.  Un-skip when the wasip2 build is fixed.
citation: e2e/plugin_fixtures/cel_wasm_plugin_demo (demo_plugin_proto target)
)CELSKIP";
  // Intended assertion: compile against `bool is_adult(proto(acme.User))`,
  // register a rebuilt plugin declaring `bool is_adult(proto(acme.Person))`
  // (same overload-id via re-embedded idl), and expect the §2 mismatch
  // message naming both proto FQNs.
}

TEST(RequiredFnPlanCheck, MissingHostFnFailsAtPlanWithFrozenMessage) {
  // The §5.3 missing-host shape, verbatim: a forgotten host
  // registration fails Plan cleanly instead of surfacing as an
  // opaque `unknown import cel_fn.<id>` wasmtime link error.
  auto builder = Compiler::NewBuilder();
  builder.AddFunction("int @host.discount_pct(string tier);");
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or =
      compiler_or->Compile("discount_pct('gold')", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition)
      << inst_or.status();
  EXPECT_EQ(inst_or.status().message(),
            "Engine::Plan: program requires host function "
            "`discount_pct_string` (`int discount_pct(string)`) but none is "
            "registered; call Engine::BindFunction (or AddFunction) before "
            "Plan");
}

TEST(RequiredFnPlanCheck, HostArityMismatchFailsAtPlanWithFrozenMessage) {
  // The §5.3 arity shape, verbatim.  `AddFunction` is arity-only
  // (no decl to compare), but a wrong arity is still caught.
  auto builder = Compiler::NewBuilder();
  builder.AddFunction("int @host.discount_pct(string tier);");
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or =
      compiler_or->Compile("discount_pct('gold')", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  ASSERT_THAT(engine_or->AddFunction("discount_pct_string", /*num_args=*/3,
                                     [](HostCallContext&) {
                                       return absl::OkStatus();
                                     }),
              IsOk());
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_FALSE(inst_or.ok());
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition)
      << inst_or.status();
  EXPECT_EQ(inst_or.status().message(),
            "Engine::Plan: program requires host function "
            "`discount_pct_string` with wasm arity 2 but it was registered "
            "with arity 3");
}

TEST(RequiredFnPlanCheck, TwoPluginsOnePlanBothDispatch) {
  // The multi-plugin positive: two self-describing plugins Used on
  // one engine, one Program calling a function from each — Plan
  // verifies both rows, instantiates both, and Eval dispatches into
  // both sandboxes.
  const std::vector<uint8_t> demo_bytes = LoadDemoPluginBytes();
  auto demo_or = Plugin::Load(demo_bytes);
  ASSERT_THAT(demo_or, IsOk());
  const Plugin mul_plugin = LoadMulPlugin();

  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  ASSERT_THAT(engine_or->Use(*demo_or), IsOk());
  ASSERT_THAT(engine_or->Use(mul_plugin), IsOk());

  auto builder = Compiler::NewBuilder();
  builder.Use(*demo_or).Use(mul_plugin);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or =
      compiler_or->Compile("add(1, 2) + mul(2, 3)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();
  Activation act;
  auto v_or = inst_or->Eval(act);
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 9);
}

// ── Selective instantiation (m35 §6.4) ─────────────────────────────
//
// A component whose core module traps in its `start` — parsing
// succeeds (so AddPlugin admits it) but ANY instantiation fails.
// The observable for "did Plan instantiate this plugin?".
constexpr absl::string_view kBrokenAtInstantiateComponentWat = R"WAT(
(component
  (core module $m
    (func $boom unreachable)
    (start $boom)
    (func (export "f") (param i64) (result i64) local.get 0))
  (core instance $i (instantiate $m))
  (func (export "broken-int") (param "x" s64) (result s64)
    (canon lift (core func $i "f"))))
)WAT";

// Engine with the broken plugin registered FIRST (so instantiate-all
// would always hit it before any healthy plugin) and the healthy
// add plugin second.  Returns the add library for the compile side.
struct BrokenPlusHealthy {
  Engine engine;
  FunctionLibrary add_lib;
};

BrokenPlusHealthy MakeBrokenPlusHealthyEngine() {
  auto engine_or = Engine::NewBuilder().Build();
  ABSL_CHECK_OK(engine_or.status());
  auto broken_lib = OneFnLib("broken", CelType::Int(),
                             {CelfnParam{false, CelType::Int(), "x"}});
  ABSL_CHECK_OK(engine_or->AddPlugin(
      WatToWasm(kBrokenAtInstantiateComponentWat), broken_lib));
  auto add_lib = OneFnLib("add", CelType::Int(),
                          {CelfnParam{false, CelType::Int(), "a"},
                           CelfnParam{false, CelType::Int(), "b"}});
  ABSL_CHECK_OK(
      engine_or->AddPlugin(WatToWasm(kAddIntIntComponentWat), add_lib));
  return {*std::move(engine_or), std::move(add_lib)};
}

TEST(SelectiveInstantiation, ProgramCallingOnlyHealthyPluginPlansGreen) {
  // §6.4 behavioral pin (1): two plugins registered, one broken at
  // instantiation; a new-format Program calling only the healthy one
  // must Plan + Eval green — Plan instantiates ONLY the plugins
  // owning a required PLUGIN row.  (Before selective instantiation
  // this failed: instantiate-all hit the broken plugin.)
  BrokenPlusHealthy fx = MakeBrokenPlusHealthyEngine();
  auto builder = Compiler::NewBuilder();
  builder.DeclareFunctions(fx.add_lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("add(1, 2)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto inst_or = fx.engine.Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();
  Activation act;
  auto v_or = inst_or->Eval(act);
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 3);
}

TEST(SelectiveInstantiation, LegacyProgramWithoutField8KeepsInstantiateAll) {
  // §6.4 behavioral pin (2): a legacy-format Program (cel.abi
  // rebuilt WITHOUT field 8 by section surgery — the in-tree
  // compiler can no longer emit that shape) gives the engine no
  // required-function table, so compat instantiate-all holds and the
  // broken plugin keeps failing the Plan.
  BrokenPlusHealthy fx = MakeBrokenPlusHealthyEngine();
  auto builder = Compiler::NewBuilder();
  builder.DeclareFunctions(fx.add_lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("add(1, 2)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  std::vector<uint8_t> legacy_bytes =
      StripRequiredFunctions(prog_or->wasm_bytes());
  auto inst_or = fx.engine.Plan(Program(std::move(legacy_bytes)));
  ASSERT_FALSE(inst_or.ok())
      << "legacy Program must keep instantiate-all and hit the broken plugin";
  EXPECT_THAT(std::string(inst_or.status().message()),
              ::testing::HasSubstr("instantiate(plugin)"));
}

TEST(SelectiveInstantiation, ProgramRequiringNoPluginsInstantiatesZero) {
  // §6.4 behavioral pin (3): a Program calling no plugin functions
  // (its required table carries a HOST row only) instantiates ZERO
  // plugins — observable because the broken plugin is registered and
  // Plan still goes green.
  BrokenPlusHealthy fx = MakeBrokenPlusHealthyEngine();
  ASSERT_THAT(fx.engine.BindFunction(
                  "int @host.discount_pct(string tier);",
                  [](absl::string_view tier) -> absl::StatusOr<int64_t> {
                    return tier == "gold" ? 20 : 5;
                  }),
              IsOk());
  auto builder = Compiler::NewBuilder();
  builder.AddFunction("int @host.discount_pct(string tier);");
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or =
      compiler_or->Compile("discount_pct('gold')", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  auto inst_or = fx.engine.Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();
  Activation act;
  auto v_or = inst_or->Eval(act);
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 20);
}

TEST(SelectiveInstantiation, EmptyRequiredTableKeepsInstantiateAll) {
  // The honest compat boundary: a Program with NO custom-fn call
  // sites carries an EMPTY required_functions table — on the wire
  // that is indistinguishable from a legacy pre-field-8 Program, so
  // the engine must keep instantiate-all (a legacy Program that DOES
  // call plugin fns depends on it) and the broken plugin still fails
  // this Plan.
  BrokenPlusHealthy fx = MakeBrokenPlusHealthyEngine();
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("42", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  auto inst_or = fx.engine.Plan(*prog_or);
  ASSERT_FALSE(inst_or.ok())
      << "empty required table must keep legacy instantiate-all";
  EXPECT_THAT(std::string(inst_or.status().message()),
              ::testing::HasSubstr("instantiate(plugin)"));
}

}  // namespace
}  // namespace celwasm
