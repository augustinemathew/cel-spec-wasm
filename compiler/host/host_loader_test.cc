#include "compiler/host/host_loader.h"

#include <cstdint>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "absl/types/span.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Compiles `expr` through the full codegen pipeline and returns the
// serialized eval module.  Shared by every test below.
std::vector<uint8_t> EmitEval(absl::string_view expr) {
  auto typed = ParseAndCheck(expr, CheckOptions{});
  CHECK_OK(typed.status()) << "ParseAndCheck failed for: " << expr;
  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  CHECK_OK(fn.status()) << "LowerToEvalFunction failed for: " << expr;
  mod.ExportFunction("eval", "eval");
  CHECK_OK(mod.Validate());
  auto bytes = mod.Serialize();
  CHECK_OK(bytes.status());
  return *bytes;
}

TEST(HostLoaderTest, LoadsAndCallsNullaryEval) {
  auto bytes = EmitEval("42");
  auto loaded = LoadEval(bytes);
  ASSERT_THAT(loaded.status(), IsOk());
  auto r = loaded->CallNullaryEval();
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 42);
}

TEST(HostLoaderTest, ResolvesRuntimeImports) {
  // A string expression forces the eval module to actually exercise
  // the runtime imports (cel_alloc, cel_make_string_view, memory); a
  // misconfigured linker would fail at instantiation time.
  auto bytes = EmitEval("'hello'");
  auto loaded = LoadEval(bytes);
  ASSERT_THAT(loaded.status(), IsOk());
  auto r = loaded->CallNullaryEval();
  ASSERT_THAT(r.status(), IsOk());
  // The eval export returns an i32 — the offset of a CelValue in the
  // runtime's arena.  We only assert non-zero; the runtime-level
  // semantics (that the bytes at that offset match "hello") are
  // covered by unit tests in compiler/runtime/.
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_GT(r->of.i32, 0);
}

TEST(HostLoaderTest, CanBeCalledRepeatedly) {
  // Per-call cel_reset() should give a clean arena each time; the same
  // string literal must yield the same offset on every invocation.
  auto bytes = EmitEval("'reuse'");
  auto loaded = LoadEval(bytes);
  ASSERT_THAT(loaded.status(), IsOk());
  auto r1 = loaded->CallNullaryEval();
  auto r2 = loaded->CallNullaryEval();
  ASSERT_THAT(r1.status(), IsOk());
  ASSERT_THAT(r2.status(), IsOk());
  EXPECT_EQ(r1->of.i32, r2->of.i32);
}

TEST(HostLoaderTest, ReturnsScalarReprsUnchanged) {
  {
    auto bytes = EmitEval("true");
    auto loaded = LoadEval(bytes);
    ASSERT_THAT(loaded.status(), IsOk());
    auto r = loaded->CallNullaryEval();
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->kind, WASMTIME_I32);
    EXPECT_EQ(r->of.i32, 1);
  }
  {
    auto bytes = EmitEval("3.14");
    auto loaded = LoadEval(bytes);
    ASSERT_THAT(loaded.status(), IsOk());
    auto r = loaded->CallNullaryEval();
    ASSERT_THAT(r.status(), IsOk());
    EXPECT_EQ(r->kind, WASMTIME_F64);
    EXPECT_DOUBLE_EQ(r->of.f64, 3.14);
  }
}

TEST(HostLoaderTest, RejectsNonWasmBytes) {
  std::vector<uint8_t> garbage = {0x00, 0x00, 0x00, 0x00};
  auto loaded = LoadEval(garbage);
  EXPECT_THAT(loaded.status(), StatusIs(absl::StatusCode::kInternal));
}

TEST(HostLoaderTest, RejectsEvalModuleWithUnsatisfiedImports) {
  // A module importing from a module namespace we don't register must
  // be rejected by the linker, not silently accepted.
  WasmModule mod;
  // Imports from "unknown"/"foo" — not "cel"/… — so the linker can't
  // resolve it.
  BinaryenType p = BinaryenTypeInt32();
  mod.AddFunctionImport("foo", "unknown", "foo",
                        absl::Span<const BinaryenType>(&p, 1),
                        BinaryenTypeInt32());
  BinaryenExpressionRef body = BinaryenConst(mod.raw(), BinaryenLiteralInt32(0));
  mod.AddFunction("eval", /*params=*/{}, BinaryenTypeInt32(),
                  /*local_types=*/{}, body);
  mod.ExportFunction("eval", "eval");
  ASSERT_THAT(mod.Validate(), IsOk());
  auto bytes = mod.Serialize();
  ASSERT_THAT(bytes.status(), IsOk());
  auto loaded = LoadEval(*bytes);
  EXPECT_THAT(loaded.status(), StatusIs(absl::StatusCode::kInternal));
}

}  // namespace
}  // namespace celwasm
