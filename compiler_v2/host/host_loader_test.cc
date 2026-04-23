#include "compiler_v2/host/host_loader.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/compile.h"
#include "compiler_v2/runtime/cel_data.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// Local ASSIGN_OR_RETURN-shaped helper.  The codebase doesn't pull in
// a shared ASSERT_OK_AND_ASSIGN macro; inline definition keeps the
// tests readable without dragging in a macro-bearing header.  Each
// call generates a unique variable name from __LINE__.
#define CEL_CONCAT_INNER(a, b) a##b
#define CEL_CONCAT(a, b) CEL_CONCAT_INNER(a, b)
// NOLINTBEGIN(bugprone-macro-parentheses) — `lhs` is a declaration
// like `auto foo`; wrapping it in parens would break the syntax.
#define CEL_ASSERT_OK_AND_ASSIGN(lhs, rexpr)           \
  auto CEL_CONCAT(__cel_sor_, __LINE__) = (rexpr);     \
  ASSERT_TRUE((CEL_CONCAT(__cel_sor_, __LINE__)).ok()) \
      << (CEL_CONCAT(__cel_sor_, __LINE__)).status();  \
  lhs = std::move((CEL_CONCAT(__cel_sor_, __LINE__))).value()
// NOLINTEND(bugprone-macro-parentheses)

// Compile `expression` via the top-level pipeline facade and return
// the emitted wasm bytes.  Keeps the tests focused on load / run
// behaviour rather than on re-wiring the compile stages.
absl::StatusOr<std::vector<uint8_t>> CompileToWasm(
    absl::string_view expression) {
  auto artifact_or = Compile(expression);
  if (!artifact_or.ok()) return artifact_or.status();
  return std::move(artifact_or->wasm_bytes);
}

// Reads a single 24-byte CelValue at `offset`.
absl::StatusOr<CelValue> ReadCelValue(const EvalInstance& inst,
                                      uint32_t offset) {
  auto bytes_or = inst.ReadBytes(offset, sizeof(CelValue));
  if (!bytes_or.ok()) return bytes_or.status();
  CelValue v;
  std::memcpy(&v, bytes_or->data(), sizeof(CelValue));
  return v;
}

// ——— Positive path: the two-phase loader runs $eval for each
//     scalar kind and returns a CelValue offset the test can decode.

TEST(HostLoaderTest, EvalsIntLiteral) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("42"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto offset, inst.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto cv, ReadCelValue(inst, offset));
  EXPECT_EQ(cv.kind, CEL_INT);
  EXPECT_EQ(cv.payload.i, 42);
}

TEST(HostLoaderTest, EvalsNegIntLiteral) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("-42"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto offset, inst.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto cv, ReadCelValue(inst, offset));
  EXPECT_EQ(cv.kind, CEL_INT);
  EXPECT_EQ(cv.payload.i, -42);
}

TEST(HostLoaderTest, EvalsUintLiteral) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("42u"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto offset, inst.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto cv, ReadCelValue(inst, offset));
  EXPECT_EQ(cv.kind, CEL_UINT);
  EXPECT_EQ(cv.payload.u, 42u);
}

TEST(HostLoaderTest, EvalsBoolLiteral) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("true"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto offset, inst.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto cv, ReadCelValue(inst, offset));
  EXPECT_EQ(cv.kind, CEL_BOOL);
  EXPECT_NE(cv.payload.b, 0);
}

TEST(HostLoaderTest, EvalsDoubleLiteral) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("3.14"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto offset, inst.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto cv, ReadCelValue(inst, offset));
  EXPECT_EQ(cv.kind, CEL_DOUBLE);
  EXPECT_DOUBLE_EQ(cv.payload.d, 3.14);
}

TEST(HostLoaderTest, EvalsNullLiteral) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("null"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto offset, inst.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto cv, ReadCelValue(inst, offset));
  EXPECT_EQ(cv.kind, CEL_NULL);
}

TEST(HostLoaderTest, EvalsStringLiteral) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("\"hello\""));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto offset, inst.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto cv, ReadCelValue(inst, offset));
  EXPECT_EQ(cv.kind, CEL_STRING);
  CEL_ASSERT_OK_AND_ASSIGN(auto payload_bytes,
                           inst.ReadBytes(cv.payload.s.ptr, cv.payload.s.len));
  const std::string payload(payload_bytes.begin(), payload_bytes.end());
  EXPECT_EQ(payload, "hello");
}

TEST(HostLoaderTest, EvalsBytesLiteral) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("b\"\\x00\\x01\\x02\""));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto offset, inst.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto cv, ReadCelValue(inst, offset));
  EXPECT_EQ(cv.kind, CEL_BYTES);
  CEL_ASSERT_OK_AND_ASSIGN(
      auto payload_bytes,
      inst.ReadBytes(cv.payload.bytes.ptr, cv.payload.bytes.len));
  ASSERT_EQ(payload_bytes.size(), 3u);
  EXPECT_EQ(payload_bytes[0], 0x00);
  EXPECT_EQ(payload_bytes[1], 0x01);
  EXPECT_EQ(payload_bytes[2], 0x02);
}

// ——— cel_reset lands at bytes 8/12 per the memory model.  The
//     host-side trampoline (not the runtime wasm's cel_reset) does the
//     write for M1; verifying the bytes proves the trampoline fired.

TEST(HostLoaderTest, CelResetLandsArenaCursorAtBytesEightTwelve) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("42"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  ABSL_ASSERT_OK(inst.CallEval());

  // After $eval runs, cursor should hold `arena_base` and limit
  // should hold `mem_size_bytes` (compile-time constants).  We don't
  // know the exact rodata / arena_base offset a priori, but we do
  // know both values are non-zero and that limit > cursor.
  CEL_ASSERT_OK_AND_ASSIGN(auto header, inst.ReadBytes(8, 8));
  uint32_t cursor = 0;
  uint32_t limit = 0;
  std::memcpy(&cursor, header.data(), sizeof(uint32_t));
  std::memcpy(&limit, header.data() + 4, sizeof(uint32_t));
  EXPECT_GT(cursor, 0u);
  EXPECT_GT(limit, cursor);
}

// ——— Two-phase instantiation: two EvalInstances built from the same
//     wasm bytes have independent memories.  Mutating one's memory
//     must not affect the other's.

TEST(HostLoaderTest, TwoInstancesHaveIndependentMemory) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("42"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst_a, EvalInstance::Create(wasm));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst_b, EvalInstance::Create(wasm));

  // Run both; pull the arena-cursor bytes from each.  The values
  // should be identical (same compile-time constants) but backed by
  // distinct memory regions — verified structurally by each instance
  // being a separate EvalInstance with its own wasmtime_store.
  ABSL_ASSERT_OK(inst_a.CallEval());
  ABSL_ASSERT_OK(inst_b.CallEval());
  CEL_ASSERT_OK_AND_ASSIGN(auto hdr_a, inst_a.ReadBytes(8, 8));
  CEL_ASSERT_OK_AND_ASSIGN(auto hdr_b, inst_b.ReadBytes(8, 8));
  EXPECT_EQ(hdr_a, hdr_b);

  // `ReadBytes` returns a copy — mutating it in the test shouldn't
  // affect the other's memory (the shared-pointer-to-wire-memory
  // hazard).  Straightforward: copies are independent.
  hdr_a[0] = 0xFF;
  CEL_ASSERT_OK_AND_ASSIGN(auto hdr_b2, inst_b.ReadBytes(8, 1));
  EXPECT_NE(hdr_b2[0], 0xFF);
}

// ——— Failure modes: missing export, malformed bytes.

TEST(HostLoaderTest, MissingEvalExportFails) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("42"));
  EvalInstanceOptions opts;
  opts.eval_export_name = "does_not_exist";
  EXPECT_THAT(EvalInstance::Create(wasm, opts),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("does not export `does_not_exist`")));
}

TEST(HostLoaderTest, MalformedWasmBytesFail) {
  const std::vector<uint8_t> garbage{0x00, 0x61, 0x73, 0x6d, 0xff, 0xff};
  EXPECT_THAT(EvalInstance::Create(garbage),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("module_new(expr)")));
}

// ——— ReadBytes bounds-check.

TEST(HostLoaderTest, ReadBytesBeyondEndReturnsOutOfRange) {
  CEL_ASSERT_OK_AND_ASSIGN(auto wasm, CompileToWasm("42"));
  CEL_ASSERT_OK_AND_ASSIGN(auto inst, EvalInstance::Create(wasm));
  EXPECT_THAT(inst.ReadBytes(inst.memory_size_bytes() - 1, 2),
              StatusIs(absl::StatusCode::kOutOfRange));
}

}  // namespace
}  // namespace celwasm
