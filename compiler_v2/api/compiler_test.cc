// M1 — Compiler / Builder lifecycle + Compile producing Program
// bytes.  Per the (revised) Plan §5.2 / §5.3 split.  The Compiler
// is pure compile-time and never touches wasmtime; tests here
// reflect that — no wasm engine setup, no wasmtime deps.

#include "compiler_v2/api/compiler.h"

#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

using ::absl_testing::IsOk;
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

  auto prog_or = compiler.Compile("42");
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

  auto prog_or = compiler.Compile("this is not a CEL expression !!");
  EXPECT_EQ(prog_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(CompilerCompileTest, OneCompilerProducesManyPrograms) {
  auto compiler_or = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler_or.ok()) << compiler_or.status();
  Compiler compiler = *std::move(compiler_or);

  auto a = compiler.Compile("42");
  auto b = compiler.Compile("true");
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
  auto prog_or = c->Compile("x");
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
  EXPECT_THAT(compiler->Compile("c.name"), IsOk());
}

// Undeclared variable in the source → InvalidArgument at the
// checker level (not Unimplemented).
TEST(CompilerCompileDeclaredVariableTest, UndeclaredVariableFailsAtChecker) {
  auto c = Compiler::NewBuilder().Build();
  ASSERT_THAT(c, IsOk());
  auto prog_or = c->Compile("x");
  EXPECT_THAT(prog_or, StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace cel
