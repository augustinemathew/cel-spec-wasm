// End-to-end test for the `cel_wasm_plugin` Starlark macro
// (//bazel:cel_wasm_plugin.bzl, m26 §6).
//
// Loads the `demo_plugin.wasm` byte stream the macro produces
// from `fns.idl` + `user_fns.cc` via bazel runfiles, registers it
// with `Engine::AddPlugin(plugin_bytes, lib)`, and exercises
// both declared fns through the Compile → Plan → Eval pipeline.
// This is the *integration* gate the macro previously lacked: the
// codec/stub/skeleton emitters are unit-tested elsewhere; this file
// proves the bytes the macro emits are a functional plugin, not
// just a well-formed one.
//
// Coverage:
//   - `greet(string, int) -> string`: string + scalar in, string
//     out.  Pins the wit-bindgen string carrier round-trip via the
//     emitted codec.h (`customfn_string_t` ↔ `std::string_view` /
//     `std::string`).
//   - `add(int, int) -> int`: scalar pass-through.  Pins the codec's
//     scalar arm (no codec lift/lower; native s64 passing).
//   - the macro output is self-describing: `demo_plugin.wasm` carries
//     the verbatim `fns.idl` bytes in its `cel.fns` custom section
//     (embedded by the macro's `cel embed-decls` step), and
//     `Plugin::Load` round-trips it — decls, derived WIT interface,
//     and declaration text all come from the artifact itself.
//
// Out of scope (other slices):
//   - Proto args/returns — covered by `demo_plugin_proto`, which is
//     a manual-tagged target because libprotobuf-cpp under wasm32-wasip2
//     is a multi-minute cold-cache build.
//
// Link-mode coverage: built twice via `link_mode_e2e_cc_test`
// (`_dynamic` / `_static` — see
// doc/implementation-plan/rewrite/m28-configurable-linking.md §5.5);
// link mode is routed through `e2e::DefaultOpts()` because each test
// owns its Engine (AddPlugin registrations are per-Engine state).

#include <cstdint>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "abi/plugin.h"
#include "abi/wasm_binary.h"
#include "absl/log/absl_check.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "e2e/plugin_fixtures/cel_wasm_plugin_demo/user.pb.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gmock/gmock.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::bazel::tools::cpp::runfiles::Runfiles;

// Load `demo_plugin.wasm` from the runfiles tree.  The bazel
// `data = [":demo_plugin"]` on the cc_test makes the file
// available; runfiles resolves the path on both macOS and Linux.
std::vector<uint8_t> LoadRunfileBytes(absl::string_view basename) {
  std::string error;
  auto runfiles = absl::WrapUnique(Runfiles::CreateForTest(&error));
  ABSL_CHECK(runfiles != nullptr) << "runfiles init failed: " << error;
  const std::string path = runfiles->Rlocation(absl::StrCat(
      "_main/e2e/plugin_fixtures/cel_wasm_plugin_demo/", basename));
  ABSL_CHECK(!path.empty()) << basename << " not in runfiles";

  std::ifstream f(path, std::ios::binary);
  ABSL_CHECK(f.is_open()) << "failed to open " << path;
  return {(std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> LoadDemoPluginBytes() {
  return LoadRunfileBytes("demo_plugin.wasm");
}

// Build a FunctionLibrary that mirrors fns.idl's two decls.  The
// embedder declares the same shape the .idl declared; otherwise
// AddPlugin would refuse the export ↔ decl mismatch (m24 §3.5
// validation gate).
FunctionLibrary BuildDemoLibrary() {
  auto lib_or =
      FunctionLibrary::Builder()
          // The macro's default package is `cel:<module>`, derived from
          // the IDL's `Module customfn;` directive; the wit-bindgen
          // emitter wraps the fns in interface `fns` at version `0.1.0`.
          // The embedder mirrors the matching WIT interface name so
          // Engine::AddPlugin does the two-level export lookup.
          .SetWitInterface("cel:customfn/fns@0.1.0")
          .AddPlugin("greet", CelType::String(),
                     {CelfnParam{false, CelType::String(), "name"},
                      CelfnParam{false, CelType::Int(), "age"}})
          .AddPlugin("add", CelType::Int(),
                     {CelfnParam{false, CelType::Int(), "a"},
                      CelfnParam{false, CelType::Int(), "b"}})
          .Build();
  ABSL_CHECK_OK(lib_or) << lib_or.status();
  return *std::move(lib_or);
}

// Compile + Plan + Eval `source` against `plugin`.  Each call builds
// its own Compiler and Engine — plugin registrations are per-Engine
// state.
absl::StatusOr<Value> EvalPluginSource(const Plugin& plugin,
                                       absl::string_view source) {
  auto builder = Compiler::NewBuilder();
  builder.Use(plugin);
  auto compiler = std::move(builder).Build();
  if (!compiler.ok()) return compiler.status();
  auto program = compiler->Compile(source, e2e::DefaultOpts());
  if (!program.ok()) return program.status();

  auto engine = Engine::NewBuilder().Build();
  if (!engine.ok()) return engine.status();
  if (auto s = engine->Use(plugin); !s.ok()) return s;
  auto instance = engine->Plan(*program);
  if (!instance.ok()) return instance.status();

  Activation act;
  return instance->Eval(act);
}

// Assert `source` evaluates to boolean true through `plugin`.
void ExpectPluginBoolTrue(const Plugin& plugin, absl::string_view source) {
  auto v = EvalPluginSource(plugin, source);
  ASSERT_TRUE(v.ok()) << source << ": " << v.status();
  auto b = v->AsBool();
  ASSERT_TRUE(b.ok()) << source << " kind=" << static_cast<int>(v->kind());
  EXPECT_TRUE(*b) << source;
}

// The macro-built artifact is self-describing: it CARRIES the
// `cel.fns` section (verbatim fns.idl bytes, embedded by the macro's
// `cel embed-decls` step) and `Plugin::Load` round-trips it.
TEST(CelWasmPluginDemo, MacroOutputCarriesCelFnsAndPluginLoadRoundTrips) {
  const std::vector<uint8_t> bytes = LoadDemoPluginBytes();
  const std::vector<uint8_t> idl = LoadRunfileBytes("fns.idl");
  const absl::string_view idl_text(reinterpret_cast<const char*>(idl.data()),
                                   idl.size());

  // The section is present and its payload is the exact fns.idl bytes.
  auto section = FindCustomSection(bytes, "cel.fns");
  ASSERT_THAT(section, IsOk()) << section.status();
  EXPECT_EQ(absl::string_view(reinterpret_cast<const char*>(section->data()),
                              section->size()),
            idl_text);

  // Plugin::Load round-trips it: decls match the idl, and the WIT
  // interface derives from the idl's `Module customfn;` directive.
  auto plugin = Plugin::Load(bytes);
  ASSERT_THAT(plugin, IsOk()) << plugin.status();
  EXPECT_EQ(plugin->celfn_source(), idl_text);
  EXPECT_EQ(plugin->wit_interface(), "cel:customfn/fns@0.1.0");
  // Names in declaration order.  Listed rather than indexed one by one
  // so adding a decl to fns.idl does not silently renumber the
  // assertions below it — the previous form broke that way when
  // echo_uint / echo_string were added.
  std::vector<std::string> names;
  names.reserve(plugin->decls().size());
  for (const auto& d : plugin->decls())
    names.push_back(d.fn_name);
  EXPECT_THAT(names, ::testing::ElementsAre(
                         "greet", "add", "len", "echo_double", "echo_uint",
                         "echo_string", "negate", "rev_bytes", "echo_list",
                         "echo_map", "sum_list", "iota", "echo_uints",
                         "echo_doubles", "echo_uint_bool_map", "echo_strings",
                         "echo_int_map", "echo_nested"));
  // Overload ids carry the arg-kind slugs; spot-check the three shapes.
  EXPECT_EQ(plugin->decls()[0].overload_id, "greet_string_int");
  EXPECT_EQ(plugin->decls()[1].overload_id, "add_int_int");
  EXPECT_EQ(plugin->decls()[2].overload_id, "len_string");
}

// ─── kind-matrix dispatch: the carriers that work today ──────────
//
// The scalar carriers are the supported envelope (doc/design/
// 05-custom-functions.md §5.4).  Each row is echo-shaped, so one
// call drives BOTH directions of the host codec
// (LiftCelToComponent for the arg, LowerComponentToCel for the
// result).  Every NON-scalar carrier is pinned below.
TEST(CelWasmPluginDemo, KindMatrixEchoRoundTrips) {
  // One echo-shaped call per canonical-ABI carrier: each row drives
  // BOTH directions of the host codec (LiftCelToComponent for the
  // arg, LowerComponentToCel for the result) for its kind.  The
  // aggregate rows used to trap ("cannot leave component instance")
  // until the guest stopped importing wasi:random — see
  // bazel/plugin_rng_stub.c for why that import was fatal here.
  auto plugin_or = Plugin::Load(LoadDemoPluginBytes());
  ASSERT_THAT(plugin_or, IsOk()) << plugin_or.status();
  const absl::string_view kTrueSources[] = {
      // Every primitive CEL type, so no carrier's lift/lower arm is
      // left unexecuted.  `int` is covered by add/sum_list/iota below;
      // uint and bare string had NO declaration in the fixture at all
      // until this row set, so their arms never ran.
      "echo_double(1.5) == 1.5",
      "negate(true) == false",
      "echo_uint(7u) == 7u",
      "echo_uint(18446744073709551615u) == 18446744073709551615u",
      "echo_string('hi') == 'hi'",
      "echo_string('') == ''",
      // bytes (list<u8> on the wire)
      "rev_bytes(b'ab') == b'ba'",
      // list<int>, both directions and the empty case
      "echo_list([1, 2])[1] == 2",
      "size(echo_list([])) == 0",
      "sum_list([1, 2, 3]) == 6",
      "iota(3)[2] == 2",
      // map<string,int> (list<tuple2> on the wire), incl. empty —
      // the lower half of the map codec only exists as of the
      // kMapTpl change that landed with this fixture.
      "echo_map({'a': 1, 'b': 2})['b'] == 2",
      "size(echo_map({})) == 0",
      // Nested / non-string-key carriers: the recursive list template,
      // the map lower's non-string-key branch, and a list whose
      // element is itself an aggregate.
      // Element-suffix carriers: `SuffixFor` in cpp_codec_emitter.cc is
      // reached only through a container, so uint / double / bool need
      // to appear as a list element or map key/value — a bare argument
      // passes through unwrapped and never touches those arms.
      "echo_uints([1u, 2u])[1] == 2u",
      "echo_doubles([1.5, 2.5])[0] == 1.5",
      "echo_uint_bool_map({1u: true, 2u: false})[2u] == false",
      "size(echo_uint_bool_map({})) == 0",
      "echo_strings(['a', 'b'])[1] == 'b'",
      "size(echo_strings([])) == 0",
      "echo_int_map({1: 2, 3: 4})[3] == 4",
      "echo_nested([[1, 2], [3]])[0][1] == 2",
      "size(echo_nested([])) == 0",
  };
  for (const absl::string_view source : kTrueSources) {
    ExpectPluginBoolTrue(*plugin_or, source);
  }
}

TEST(CelWasmPluginDemo, OneNounFlowLoadUseCompileUsePlanEval) {
  auto plugin_or = Plugin::Load(LoadDemoPluginBytes());
  ASSERT_THAT(plugin_or, IsOk()) << plugin_or.status();
  const Plugin& plugin = *plugin_or;

  // Compile side — declarations flow from the artifact.
  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("a", CelType::Int())
      .DeclareVariable("b", CelType::Int())
      .Use(plugin);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("add(a, b)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();

  // Eval side — same noun; registration statically verifies the
  // component exports every declared fn before any Plan.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  ASSERT_THAT(engine_or->Use(plugin), IsOk());

  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();
  Activation act;
  act.Bind("a", Value::Int(40));
  act.Bind("b", Value::Int(2));
  auto v_or = inst_or->Eval(act);
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 42);
}

TEST(CelWasmPluginDemo, OneNounFlowProtoArg) {
  GTEST_SKIP()
      << "demo_plugin_proto cannot build without patching absl to compile "
         "threadless (cctz's time-zone mutex, absl::Mutex's yield hook, the "
         "stdcpp waiter + spinlock lock_guard).  Those patches were tried and "
         "REVERTED deliberately on 2026-07-28: the direction chosen instead is "
         "to keep absl out of the wasm side entirely — see "
         "doc/design/10-plugin-wit-pipeline.md §4.  Un-skip when the guest "
         "gets a proto runtime that does not drag absl (upb, or an IDL "
         "surface that passes fields rather than messages).  Everything else "
         "in the proto path is FIXED and unit-pinned: the export-symbol "
         "lowercasing (cpp_stub_emitter_test "
         "ProtoDeclExportSymbolIsLowercased) "
         "and the -pthread strip (wasm_clang.sh).";
  // Intended body: Plugin::Load(demo_plugin_proto bytes) ->
  // Compiler::Builder::Use -> Compile("is_adult(u)") -> Engine::Use ->
  // Plan -> Eval with a bound acme.User, both directions.  Verified
  // GREEN on 2026-07-28 with the absl patches applied, so the only
  // blocker is the absl-in-guest dependency itself.
}

TEST(CelWasmPluginDemo, AddRoundTrips) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  const auto lib = BuildDemoLibrary();
  ASSERT_THAT(engine_or->AddPlugin(LoadDemoPluginBytes(), lib), IsOk());

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

  Activation act;
  act.Bind("a", Value::Int(40));
  act.Bind("b", Value::Int(2));
  auto v_or = inst_or->Eval(act);
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  EXPECT_EQ(*v_or->AsInt(), 42);
}

TEST(CelWasmPluginDemo, GreetRoundTripsString) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_THAT(engine_or, IsOk());
  const auto lib = BuildDemoLibrary();
  ASSERT_THAT(engine_or->AddPlugin(LoadDemoPluginBytes(), lib), IsOk());

  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("name", CelType::String())
      .DeclareVariable("age", CelType::Int())
      .DeclareFunctions(lib);
  auto compiler_or = std::move(builder).Build();
  ASSERT_THAT(compiler_or, IsOk());
  auto prog_or = compiler_or->Compile("greet(name, age)", e2e::DefaultOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  auto inst_or = engine_or->Plan(*prog_or);
  ASSERT_THAT(inst_or, IsOk()) << inst_or.status();

  Activation act;
  act.Bind("name", Value::String("Ada"));
  act.Bind("age", Value::Int(30));
  auto v_or = inst_or->Eval(act);
  ASSERT_THAT(v_or, IsOk()) << v_or.status();
  EXPECT_EQ(std::string(*v_or->AsString()), "Hello, Ada (age 30)");
}

}  // namespace
}  // namespace celwasm
