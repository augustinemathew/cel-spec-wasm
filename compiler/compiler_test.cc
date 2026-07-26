// M1 — Compiler / Builder lifecycle + Compile producing Program
// bytes.  Per the (revised) Plan §5.2 / §5.3 split.  The Compiler
// is pure compile-time and never touches wasmtime; tests here
// reflect that — no wasm engine setup, no wasmtime deps.

#include "compiler/compiler.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "abi/plugin.h"
#include "abi/wasm_binary.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "compiler/program.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"
#include "google/protobuf/message.h"
#include "bazel/link_mode_test_helpers.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Returns `CompilerOptions` with `link_mode` set to the per-binary
// `kTestLinkMode` — picked at build time by `link_mode_cc_test`.
inline CompilerOptions LinkModeOpts() {
  CompilerOptions opts;
  opts.link_mode = kTestLinkMode;
  return opts;
}

using ::absl_testing::StatusIs;

// Force generated-pool registration of descriptors referenced by
// tests below.  Runs once at static init per test binary.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      return 0;
    }();

TEST(CompilerBuilderTest, BuildSucceedsWithDefaults) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
}

TEST(CompilerCompileTest, CompileScalarLiteralReturnsProgramWithBytes) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  Compiler compiler = *std::move(compiler_or);

  auto prog_or = compiler.Compile("42", LinkModeOpts());
  ASSERT_TRUE(prog_or.ok()) << prog_or.status();
  auto bytes = prog_or->wasm_bytes();
  // Wasm magic = 00 61 73 6d.
  ASSERT_GE(bytes.size(), 4u);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6d);
}

TEST(CompilerCompileTest, CompileBadSourceReturnsInvalidArgument) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  Compiler compiler = *std::move(compiler_or);

  auto prog_or = compiler.Compile("this is not a CEL expression !!", LinkModeOpts());
  EXPECT_EQ(prog_or.status().code(), absl::StatusCode::kInvalidArgument);
}

// m28 — link_mode field on CompilerOptions.
//
// kDynamic (default) emits a Program that imports the runtime helpers
// from the `"cel"` module.  kStatic merges the wrapper-stripped runtime
// into the Program so it carries the helpers internally.  The byte-shape
// invariant (no `cel.*` imports in kStatic mode) is asserted at the
// pipeline-internal layer (`compiler/internal/compile_test.cc`) where
// the Binaryen handle is reachable; this layer asserts both modes
// produce a valid Program with wasm-magic bytes.

// m28 — both link modes produce a valid Program with wasm-magic
// preamble.  No assertion about which mode is the default — the
// default is a release-engineering choice, not a contract.
TEST(CompilerLinkModeTest, DynamicProducesValidWasmProgram) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  CompilerOptions opts;
  opts.link_mode = CompilerOptions::LinkMode::kDynamic;
  auto prog_or = compiler_or->Compile("42", opts);
  ASSERT_TRUE(prog_or.ok()) << prog_or.status();
  auto bytes = prog_or->wasm_bytes();
  ASSERT_GE(bytes.size(), 4u);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6d);
}

TEST(CompilerLinkModeTest, StaticProducesValidWasmProgram) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  CompilerOptions opts;
  opts.link_mode = CompilerOptions::LinkMode::kStatic;
  auto prog_or = compiler_or->Compile("42", opts);
  ASSERT_TRUE(prog_or.ok()) << prog_or.status();
  auto bytes = prog_or->wasm_bytes();
  ASSERT_GE(bytes.size(), 4u);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6d);
}

// The two modes produce materially different artifact sizes.  kStatic
// bundles ~800 KB of runtime; kDynamic is ~10 KB.  Order-of-magnitude
// assertion, not byte-equal.
TEST(CompilerLinkModeTest, StaticIsMaterallyLargerThanDynamic) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  CompilerOptions dyn;
  dyn.link_mode = CompilerOptions::LinkMode::kDynamic;
  CompilerOptions stc;
  stc.link_mode = CompilerOptions::LinkMode::kStatic;
  auto d = compiler_or->Compile("42", dyn);
  auto s = compiler_or->Compile("42", stc);
  ASSERT_TRUE(d.ok()) << d.status();
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_GT(s->wasm_bytes().size(), d->wasm_bytes().size() * 10);
}

TEST(CompilerCompileTest, OneCompilerProducesManyPrograms) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  Compiler compiler = *std::move(compiler_or);

  auto a = compiler.Compile("42", LinkModeOpts());
  auto b = compiler.Compile("true", LinkModeOpts());
  auto c = compiler.Compile("\"hello\"");
  ASSERT_TRUE(a.ok()) << a.status();
  ASSERT_TRUE(b.ok()) << b.status();
  ASSERT_TRUE(c.ok()) << c.status();
  // Each Compile produces independent bytes.
  EXPECT_NE(a->wasm_bytes().size(), 0u);
  EXPECT_NE(b->wasm_bytes().size(), 0u);
  EXPECT_NE(c->wasm_bytes().size(), 0u);
}

// ————————— DeclareVariable / RegisterMessageType (M2) —————————

TEST(CompilerBuilderDeclareVariableTest, AcceptsScalarTypes) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int())
      .DeclareVariable("s", CelType::String())
      .DeclareVariable("b", CelType::Bool());
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  ASSERT_EQ(c->declared_variables().size(), 3u);
  EXPECT_EQ(c->declared_variables()[0].name, "x");
  EXPECT_EQ(c->declared_variables()[0].type.kind(), CelType::Kind::kInt);
}

TEST(CompilerBuilderDeclareVariableTest, RejectsDuplicateName) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int())
      .DeclareVariable("x", CelType::String());
  EXPECT_THAT(std::move(b).Build(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(CompilerBuilderDeclareVariableTest, RejectsEmptyName) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("", CelType::Int());
  EXPECT_THAT(std::move(b).Build(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(CompilerBuilderDeclareVariableTest, RejectsUnknownType) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType{});  // default-constructed
  EXPECT_THAT(std::move(b).Build(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// Compile with a declared variable — M2.B.1 lights the kIdent arm
// of expr_lower.  The declaration flows through the checker, the
// resolver assigns the variable a slot, expr_lower emits the
// `$eval` prelude + local.get, and the module serialises.
// Running the module requires Instance::Eval(Activation), which
// lands in M2.B.3 — this test only goes as far as Compile().
TEST(CompilerCompileDeclaredVariableTest, DeclaredIdentCompilesToValidModule) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int());
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  auto prog_or = c->Compile("x", LinkModeOpts());
  ASSERT_THAT(prog_or, IsOk());
  auto bytes = prog_or->wasm_bytes();
  // Wasm magic = 00 61 73 6d.
  ASSERT_GE(bytes.size(), 4u);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6d);
}

// Compile with a declared Message variable — the checker resolves
// "celwasm.testdata.Customer" via the process-wide generated
// descriptor pool (the cc_proto_library fixture is linked into the
// test binary).  M2.C select lowering takes `c.name` through to a
// valid module.  Touching `Customer::descriptor()` once is enough
// to force the generated pool registration — no explicit
// registration API is needed.
TEST(CompilerCompileDeclaredVariableTest,
     MessageTypeDeclarationResolvesThroughGeneratedPool) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  auto compiler = std::move(b).Build();
  ASSERT_THAT(compiler, IsOk());
  EXPECT_THAT(compiler->Compile("c.name", LinkModeOpts()), IsOk());
}

// Undeclared variable in the source → InvalidArgument at the
// checker level (not Unimplemented).
TEST(CompilerCompileDeclaredVariableTest, UndeclaredVariableFailsAtChecker) {
  auto c = Compiler::NewBuilder().Build();
  ASSERT_THAT(c, IsOk());
  auto prog_or = c->Compile("x", LinkModeOpts());
  EXPECT_THAT(prog_or, StatusIs(absl::StatusCode::kInvalidArgument));
}

// ─── M13 Slice C.2 — Compiler::Builder::DeclareFunctions / AddFunction ───

TEST(CompilerBuilderDeclareFunctionsTest, EmptyLibraryBuildsOk) {
  auto lib_or = celwasm::FunctionLibrary::Builder().Build();
  ASSERT_THAT(lib_or, IsOk());
  auto b = Compiler::NewBuilder();
  b.DeclareFunctions(*std::move(lib_or));
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  EXPECT_EQ(c->function_libraries().size(), 1u);
  EXPECT_EQ(c->function_libraries()[0].decls().size(), 0u);
}

TEST(CompilerBuilderDeclareFunctionsTest, LibraryWithHostFnPropagatesToCompiler) {
  celwasm::CelfnType ret;
  ret.kind = celwasm::CelfnType::Kind::kBool;
  celwasm::CelfnType arg;
  arg.kind = celwasm::CelfnType::Kind::kString;
  std::vector<celwasm::CelfnParam> params{
      celwasm::CelfnParam{/*is_receiver=*/true, arg, "s"}};
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddHost("is_number", ret, std::move(params))
                    .Build();
  ASSERT_THAT(lib_or, IsOk());
  auto b = Compiler::NewBuilder();
  b.DeclareFunctions(*std::move(lib_or));
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  ASSERT_EQ(c->function_libraries().size(), 1u);
  ASSERT_EQ(c->function_libraries()[0].decls().size(), 1u);
  EXPECT_EQ(c->function_libraries()[0].decls()[0].overload_id,
            "is_number_string");
}

TEST(CompilerBuilderAddFunctionTest, SingleHostDeclString) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("string @host.upper(this string s);");
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  ASSERT_EQ(c->function_libraries().size(), 1u);
  EXPECT_EQ(c->function_libraries()[0].decls()[0].overload_id, "upper_string");
}

TEST(CompilerBuilderAddFunctionTest, ParseErrorSurfacesAtBuild) {
  auto b = Compiler::NewBuilder();
  b.AddFunction("string @host.upper(this string s)");  // missing `;`
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("AddFunction"));
}

TEST(CompilerBuilderAddFunctionTest, FirstParseErrorWins) {
  // Two failures, first one wins.  Subsequent ok call still safe.
  auto b = Compiler::NewBuilder();
  b.AddFunction("garbage");
  b.AddFunction("more garbage");
  b.AddFunction("string @host.upper(this string s);");
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(CompilerBuilderDeclareFunctionsTest, CrossLibraryDuplicateOverloadIdRejected) {
  celwasm::CelfnType ret;
  ret.kind = celwasm::CelfnType::Kind::kString;
  celwasm::CelfnType arg;
  arg.kind = celwasm::CelfnType::Kind::kString;
  auto make_lib = [&]() {
    return *celwasm::FunctionLibrary::Builder()
                .AddHost("upper", ret, {celwasm::CelfnParam{true, arg, "s"}})
                .Build();
  };
  auto b = Compiler::NewBuilder();
  b.DeclareFunctions(make_lib());
  b.DeclareFunctions(make_lib());
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("upper_string"));
}

// M13 Slice C.3 — call-sites resolve through to a valid wasm
// module.  This is the slice's acceptance test: `name.is_number()`
// compiles cleanly given the host-backed `is_number(string)` decl,
// because (a) the checker has the OverloadDecl from
// `RegisterCustomFunctionsOnChecker` and (b) codegen emits a
// `cel_fn.is_number_string` import via the OverloadTable's
// custom-row.  Engine-side binding is exercised end-to-end in
// engine_test.cc / instance_test.cc.
TEST(CompilerBuilderAddFunctionTest, CompileReceiverHostFnResolvesAndLowers) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("name", CelType::String());
  b.AddFunction("bool @host.is_number(this string s);");
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  ASSERT_EQ(c->function_libraries().size(), 1u);
  auto prog_or = c->Compile("name.is_number()", LinkModeOpts());
  ASSERT_THAT(prog_or, IsOk());
  // Sanity: emitted bytes start with the wasm magic.
  auto bytes = prog_or->wasm_bytes();
  ASSERT_GE(bytes.size(), 4u);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6d);
}

// kPlugin decls are dispatched via the host-callback path
// (cel_fn) per m24 §2-§3 — a Component-Model-backed fn is invisible
// to the compiler / checker / overload table at the call site (the
// emitted wasm import is `(import "cel_fn" "<helper>" …)`, identical
// to a kHost decl).  These tests pin that contract.
TEST(CompilerBuilderDeclareFunctionsTest, PluginDeclRoutesViaCelFn) {
  celwasm::CelfnType ret;
  ret.kind = celwasm::CelfnType::Kind::kBool;
  celwasm::CelfnType arg;
  arg.kind = celwasm::CelfnType::Kind::kString;
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddPlugin(
                        "allow", ret,
                        {celwasm::CelfnParam{/*is_receiver=*/false, arg, "u"}})
                    .Build();
  ASSERT_THAT(lib_or, IsOk()) << lib_or.status();
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("u", CelType::String());
  b.DeclareFunctions(*std::move(lib_or));
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  auto prog_or = c->Compile("allow(u)");
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  const auto bytes = prog_or->wasm_bytes();
  // The emitted module must carry an import of the form
  // `(import "cel_fn" "allow_string" …)`.  Search for the byte
  // sequence — the wasm import section encodes module + name as
  // length-prefixed strings, so the two adjacent literals appear
  // verbatim in the module bytes.
  const std::string blob(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());
  EXPECT_NE(blob.find("cel_fn"), std::string::npos)
      << "kPlugin decl did not produce a `cel_fn` import — "
         "routing regressed";
  EXPECT_NE(blob.find("allow_string"), std::string::npos)
      << "helper name not present in emitted imports";
}

TEST(CompilerBuilderDeclareFunctionsTest,
     PluginDeclAdmitsProtoAndRoutesViaCelFn) {
  // m24 §8 admits proto(...) on kPlugin (cross as bytes).
  celwasm::CelfnType ret;
  ret.kind = celwasm::CelfnType::Kind::kBool;
  celwasm::CelfnType arg;
  arg.kind = celwasm::CelfnType::Kind::kProto;
  arg.proto_fqn = "celwasm.testdata.Customer";
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddPlugin(
                        "is_premium", ret,
                        {celwasm::CelfnParam{/*is_receiver=*/false, arg, "c"}})
                    .Build();
  ASSERT_THAT(lib_or, IsOk()) << lib_or.status();
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  b.DeclareFunctions(*std::move(lib_or));
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  auto prog_or = c->Compile("is_premium(c)");
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  const std::string blob(
      reinterpret_cast<const char*>(prog_or->wasm_bytes().data()),
      prog_or->wasm_bytes().size());
  EXPECT_NE(blob.find("cel_fn"), std::string::npos);
  EXPECT_NE(blob.find("is_premium_message_celwasm_testdata_Customer"),
            std::string::npos);
}

TEST(CompilerBuilderDeclareFunctionsTest,
     PluginAndHostCoexistAndShareCelFnNamespace) {
  // Two decls with distinct overload-ids, one kHost + one kPlugin,
  // both routing via cel_fn — must coexist in one library.
  celwasm::CelfnType b_t;
  b_t.kind = celwasm::CelfnType::Kind::kBool;
  celwasm::CelfnType s_t;
  s_t.kind = celwasm::CelfnType::Kind::kString;
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddHost("upper", s_t, {celwasm::CelfnParam{true, s_t, "s"}})
                    .AddPlugin(
                        "allow", b_t,
                        {celwasm::CelfnParam{false, s_t, "u"}})
                    .Build();
  ASSERT_THAT(lib_or, IsOk()) << lib_or.status();
  ASSERT_EQ(lib_or->decls().size(), 2u);
  EXPECT_EQ(lib_or->decls()[0].backend,
            celwasm::CelfnDecl::Backend::kHost);
  EXPECT_EQ(lib_or->decls()[1].backend,
            celwasm::CelfnDecl::Backend::kPlugin);
  // Both decls should claim module_name=="cel_fn" (the call-site
  // import-module is the same for kHost and kPlugin).
  EXPECT_EQ(lib_or->decls()[0].module_name, "cel_fn");
  EXPECT_EQ(lib_or->decls()[1].module_name, "cel_fn");
}

TEST(CompilerBuilderDeclareFunctionsTest,
     PluginDuplicateOverloadIdRejected) {
  // Same overload-id collision detection as @host — registering the
  // same helper twice (one @host + one kPlugin) must fail.
  celwasm::CelfnType b_t;
  b_t.kind = celwasm::CelfnType::Kind::kBool;
  celwasm::CelfnType s_t;
  s_t.kind = celwasm::CelfnType::Kind::kString;
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddHost("clash", b_t, {celwasm::CelfnParam{false, s_t, "x"}})
                    .AddPlugin(
                        "clash", b_t,
                        {celwasm::CelfnParam{false, s_t, "x"}})
                    .Build();
  EXPECT_THAT(lib_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(lib_or.status().message()),
              testing::HasSubstr("duplicate"));
}

// ─── m35 B2 — Compiler::Builder::Use(Plugin) ─────────────────────
//
// `Use(plugin)` is exactly `DeclareFunctions(plugin.library())` —
// the compile side needs no wasm engine, so the fixtures below are
// hand-framed plugin bytes (Component-Model preamble + a `cel.fns`
// custom section built with the production framing writer), the
// same shape abi/plugin_test.cc pins.

// `\0asm` + version/layer word 0x0001000d — a minimal CM component.
constexpr uint8_t kComponentPreamble[] = {0x00, 0x61, 0x73, 0x6d,
                                          0x0d, 0x00, 0x01, 0x00};

Plugin MakePlugin(absl::string_view celfn_text) {
  std::vector<uint8_t> bytes(kComponentPreamble,
                             kComponentPreamble + sizeof(kComponentPreamble));
  const std::vector<uint8_t> section = BuildCustomSection(
      "cel.fns", {reinterpret_cast<const uint8_t*>(celfn_text.data()),
                  celfn_text.size()});
  bytes.insert(bytes.end(), section.begin(), section.end());
  auto plugin_or = Plugin::Load(bytes);
  EXPECT_THAT(plugin_or, IsOk()) << plugin_or.status();
  return *std::move(plugin_or);
}

TEST(CompilerBuilderUseTest, UseRegistersDeclsAndCallSiteTypeChecks) {
  const Plugin plugin = MakePlugin("int @plugin.add(int a, int b);\n");
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("x", CelType::Int())
      .DeclareVariable("y", CelType::Int())
      .Use(plugin);
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  ASSERT_EQ(c->function_libraries().size(), 1u);
  EXPECT_EQ(c->function_libraries()[0].decls()[0].overload_id, "add_int_int");
  // The call site type-checks against the plugin's declaration and
  // lowers to a `cel_fn.add_int_int` import.
  auto prog_or = c->Compile("add(x, y)", LinkModeOpts());
  ASSERT_THAT(prog_or, IsOk()) << prog_or.status();
  const std::string blob(
      reinterpret_cast<const char*>(prog_or->wasm_bytes().data()),
      prog_or->wasm_bytes().size());
  EXPECT_NE(blob.find("add_int_int"), std::string::npos);
}

TEST(CompilerBuilderUseTest, CallSiteTypeMismatchFailsAtCompile) {
  // The declared signature is enforced at the call site — the wrong
  // argument type fails Compile (checker InvalidArgument), which is
  // the whole point of declarations flowing from the artifact.
  const Plugin plugin = MakePlugin("int @plugin.add(int a, int b);\n");
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String()).Use(plugin);
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  EXPECT_THAT(c->Compile("add(s, s)", LinkModeOpts()),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(CompilerBuilderUseTest, DuplicateOverloadIdAcrossTwoUsesFailsAtBuild) {
  const Plugin p1 = MakePlugin("int @plugin.add(int a, int b);\n");
  const Plugin p2 = MakePlugin("int @plugin.add(int a, int b);\n");
  auto b = Compiler::NewBuilder();
  b.Use(p1).Use(p2);
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("add_int_int"));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("more than one library"));
}

TEST(CompilerBuilderUseTest, DuplicateOverloadIdAcrossUseAndAddFunction) {
  // A Use'd plugin decl and a text-path AddFunction decl that
  // synthesise the same overload-id collide at Build(), same as any
  // cross-library duplicate.
  const Plugin plugin = MakePlugin("int @plugin.add(int a, int b);\n");
  auto b = Compiler::NewBuilder();
  b.Use(plugin);
  b.AddFunction("int @host.add(int a, int b);");
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("add_int_int"));
}

// ─── m35 B2 — unmappable decl types rejected at Build() ──────────
//
// `FunctionLibrary::Builder` admits `type` / `optional<T>` on
// programmatically-built kHost decls, but the checker-side mapping
// (`CelfnTypeToCelType`) has no `cel::Type` for either
// (cleanup-backlog #44).  Build() must reject with a clean
// InvalidArgument naming the decl and the type — never crash.

CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}

TEST(CompilerBuilderUnmappableTypeTest, TypeReturnRejectedNamingDeclAndType) {
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddHost("weird", Prim(CelfnType::Kind::kType), {})
                    .Build();
  ASSERT_THAT(lib_or, IsOk()) << lib_or.status();
  auto b = Compiler::NewBuilder();
  b.DeclareFunctions(*std::move(lib_or));
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("`weird`"));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("`type`"));
}

TEST(CompilerBuilderUnmappableTypeTest,
     OptionalParamRejectedNamingDeclAndType) {
  CelfnType opt = Prim(CelfnType::Kind::kOptional);
  opt.optional_element.push_back(Prim(CelfnType::Kind::kInt));
  auto lib_or =
      celwasm::FunctionLibrary::Builder()
          .AddHost("maybe", Prim(CelfnType::Kind::kInt),
                   {celwasm::CelfnParam{/*is_receiver=*/false, opt, "x"}})
          .Build();
  ASSERT_THAT(lib_or, IsOk()) << lib_or.status();
  auto b = Compiler::NewBuilder();
  b.DeclareFunctions(*std::move(lib_or));
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("`maybe`"));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("`optional<int>`"));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("parameter `x`"));
}

TEST(CompilerBuilderUnmappableTypeTest, NestedUnmappableInsideListRejected) {
  // The check is structural: `list<optional<string>>` is just as
  // unmappable as a bare `optional<...>`.
  CelfnType opt = Prim(CelfnType::Kind::kOptional);
  opt.optional_element.push_back(Prim(CelfnType::Kind::kString));
  CelfnType lst = Prim(CelfnType::Kind::kList);
  lst.list_element.push_back(opt);
  auto lib_or = celwasm::FunctionLibrary::Builder()
                    .AddHost("nested", lst, {})
                    .Build();
  ASSERT_THAT(lib_or, IsOk()) << lib_or.status();
  auto b = Compiler::NewBuilder();
  b.DeclareFunctions(*std::move(lib_or));
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("`optional<string>`"));
}

TEST(CompilerBuilderUnmappableTypeTest,
     DiagnosticUsesCelfnGrammarSpelling) {
  // The rejected type is rendered via THE `.celfn` grammar renderer
  // (abi/celfn_wire.h::RenderType) — `Duration` capitalised,
  // `map<K, V>` with a space — the same spellings Plan messages and
  // emit tests pin, not an ad-hoc per-call-site respelling.
  CelfnType m = Prim(CelfnType::Kind::kMap);
  m.map_kv.push_back(Prim(CelfnType::Kind::kString));
  m.map_kv.push_back(Prim(CelfnType::Kind::kDuration));
  CelfnType opt = Prim(CelfnType::Kind::kOptional);
  opt.optional_element.push_back(m);
  auto lib_or =
      celwasm::FunctionLibrary::Builder()
          .AddHost("later", Prim(CelfnType::Kind::kInt),
                   {celwasm::CelfnParam{/*is_receiver=*/false, opt, "x"}})
          .Build();
  ASSERT_THAT(lib_or, IsOk()) << lib_or.status();
  auto b = Compiler::NewBuilder();
  b.DeclareFunctions(*std::move(lib_or));
  auto build_or = std::move(b).Build();
  EXPECT_THAT(build_or, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(build_or.status().message()),
              testing::HasSubstr("`optional<map<string, Duration>>`"));
}

TEST(CompilerBuilderUnmappableTypeTest, MappableDeclStillBuildsAndCompiles) {
  // Positive twin: every mappable kind passes the gate untouched.
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String());
  b.AddFunction("bool @host.is_number(this string s);");
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  EXPECT_THAT(c->Compile("s.is_number()", LinkModeOpts()), IsOk());
}

TEST(CompilerBuilderDeclareFunctionsTest, MultipleLibrariesWithDistinctOverloadsOk) {
  celwasm::CelfnType ret;
  ret.kind = celwasm::CelfnType::Kind::kString;
  celwasm::CelfnType arg;
  arg.kind = celwasm::CelfnType::Kind::kString;
  auto lib1 = *celwasm::FunctionLibrary::Builder()
                   .AddHost("upper", ret, {celwasm::CelfnParam{true, arg, "s"}})
                   .Build();
  auto lib2 = *celwasm::FunctionLibrary::Builder()
                   .AddHost("lower", ret, {celwasm::CelfnParam{true, arg, "s"}})
                   .Build();
  auto b = Compiler::NewBuilder();
  b.DeclareFunctions(std::move(lib1));
  b.DeclareFunctions(std::move(lib2));
  auto c = std::move(b).Build();
  ASSERT_THAT(c, IsOk());
  EXPECT_EQ(c->function_libraries().size(), 2u);
}

}  // namespace
}  // namespace celwasm
