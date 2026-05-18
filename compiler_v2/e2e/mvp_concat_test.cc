// MVP end-to-end test for the WASI migration: `"foo" + "bar"`
// compiles, plans, evaluates, and decodes to `"foobar"`.
//
// This is the canonical "did the migration not break anything"
// vertical-slice test referenced by
// `doc/implementation-plan/wasi/DESIGN.md` §2.
//
// The expression exercises the full pipeline:
//   - Frontend (cel-cpp parser + checker).
//   - Codegen kConst (rodata-packed string literals).
//   - Codegen kCall arm for `_+_` on strings (resolves to
//     `cel_string_concat_at_vv` overload).
//   - Runtime kernel calling `arena_alloc` (via compat shim
//     `cel_alloc` until Phase B) for the concat payload.
//   - Host decoder reading the result CelValue's payload.s
//     span and resolving its bytes via wasmtime memory.

#include <string>

#include "absl/status/statusor.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

TEST(MvpConcatE2ETest, FooBar) {
  auto compiler = Compiler::NewBuilder().Build();
  ASSERT_TRUE(compiler.ok()) << compiler.status();
  auto engine = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine.ok()) << engine.status();
  auto program = compiler->Compile(R"("foo" + "bar")");
  ASSERT_TRUE(program.ok()) << program.status();
  auto instance = engine->Plan(*program);
  ASSERT_TRUE(instance.ok()) << instance.status();
  auto value = instance->Eval();
  ASSERT_TRUE(value.ok()) << value.status();
  ASSERT_EQ(value->kind(), Value::Kind::kString);
  auto s = value->AsString();
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(*s, "foobar");
}

}  // namespace
}  // namespace cel
