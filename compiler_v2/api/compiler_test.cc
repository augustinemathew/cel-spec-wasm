// M1 — Compiler / Builder lifecycle + Compile producing Program
// bytes.  Per the (revised) Plan §5.2 / §5.3 split.  The Compiler
// is pure compile-time and never touches wasmtime; tests here
// reflect that — no wasm engine setup, no wasmtime deps.

#include "compiler_v2/api/compiler.h"

#include <utility>

#include "absl/status/status.h"
#include "compiler_v2/api/program.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

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

}  // namespace
}  // namespace cel
