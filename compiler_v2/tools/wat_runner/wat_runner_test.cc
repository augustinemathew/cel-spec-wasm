// Run every WAT file under doc/implementation-plan/rewrite/wat/
// through the harness, with stubbed cel_host impls where the
// milestone hasn't landed real ones yet.  Each test decodes the
// CelValue at $eval's return offset and asserts on its shape —
// proving the ABI round-trips before any codegen C++ lands.
//
// Per CLAUDE.md's "WAT-first" rule: these tests are the milestone
// gate.  A codegen arm that stops producing shape-matching wasm
// is caught HERE before it contaminates e2e tests downstream.

#include "compiler_v2/tools/wat_runner/wat_runner.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/runtime/cel_data.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// ─────────────────────────────────────────────────────────
// Decode helpers — read a 24-byte CelValue out of a snapshot.
// ─────────────────────────────────────────────────────────

CelValue DecodeCelValue(const std::vector<uint8_t>& mem, uint32_t offset) {
  CelValue cv;
  std::memcpy(&cv, mem.data() + offset, sizeof(cv));
  return cv;
}

// Decode the arena cursor/limit pair `cel_reset` writes.  Offsets
// match `runtime/cel_runtime.c::cel_reset`: cursor at byte 8, limit
// at byte 12.  Used to prove our real runtime's `cel_reset`
// executed — if the harness were bypassing the runtime (e.g. using
// a mock that no-ops), these u32s would stay zero.
struct ArenaHeader {
  uint32_t cursor;
  uint32_t limit;
};
ArenaHeader DecodeArenaHeader(const std::vector<uint8_t>& mem) {
  ArenaHeader h{};
  std::memcpy(&h.cursor, mem.data() + 8, sizeof(uint32_t));
  std::memcpy(&h.limit, mem.data() + 12, sizeof(uint32_t));
  return h;
}

std::string ReadSpan(const std::vector<uint8_t>& mem, CelSpan s) {
  return std::string(reinterpret_cast<const char*>(mem.data() + s.ptr), s.len);
}

// Builds a 24-byte CelValue{CEL_INT, payload.i=value} for use as a
// pre-write.
std::vector<uint8_t> EncodeIntCelValue(int64_t value) {
  CelValue cv{};
  cv.kind = CEL_INT;
  cv.payload.i = value;
  std::vector<uint8_t> out(sizeof(cv));
  std::memcpy(out.data(), &cv, sizeof(cv));
  return out;
}

std::vector<uint8_t> EncodeMessageCelValue(uint32_t msg_slot) {
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = msg_slot;
  std::vector<uint8_t> out(sizeof(cv));
  std::memcpy(out.data(), &cv, sizeof(cv));
  return out;
}

// ─────────────────────────────────────────────────────────
// Expression 1 — `42` literal.
// ─────────────────────────────────────────────────────────

constexpr absl::string_view kLiteral42Wat = R"WAT(
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\2a\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")
  (func $eval (result i32)
    (call $cel_reset (i32.const 40) (i32.const 131072))
    (i32.const 16))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
)WAT";

TEST(WatRunnerTest, LiteralFortyTwoReturnsCelIntFortyTwo) {
  WatRunInput in;
  in.wat = kLiteral42Wat;
  auto out = RunWat(in);
  ASSERT_THAT(out, IsOk());
  EXPECT_EQ(out->eval_return, 16u);
  CelValue cv = DecodeCelValue(out->memory_after, out->eval_return);
  EXPECT_EQ(cv.kind, CEL_INT);
  EXPECT_EQ(cv.payload.i, 42);

  // Prove our real cel_runtime.wasm ran: cel_reset must have
  // written (arena_base=40, limit=131072) into bytes [8, 16).  If
  // the harness were silently bypassing the runtime (e.g. via a
  // mock that no-ops), both u32s would stay zero.
  ArenaHeader ah = DecodeArenaHeader(out->memory_after);
  EXPECT_EQ(ah.cursor, 40u);
  EXPECT_EQ(ah.limit, 131072u);
}

// ─────────────────────────────────────────────────────────
// Expression 2 — ident `x` with x:int.  Harness pre-writes
// the bound CelValue into the variable's slot.
// ─────────────────────────────────────────────────────────

constexpr absl::string_view kIdentXWat = R"WAT(
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (func $eval (result i32)
    (local $x_off i32)
    (local.set $x_off (i32.const 16))
    (call $cel_reset (i32.const 40) (i32.const 131072))
    (local.get $x_off))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
)WAT";

TEST(WatRunnerTest, IdentXReturnsPreWrittenCelValueSlot) {
  WatRunInput in;
  in.wat = kIdentXWat;
  // Simulate Activation::Bind("x", Value::Int(7)) — write the
  // CelValue into x's workspace slot at offset 16.
  in.pre_writes = {{16u, EncodeIntCelValue(7)}};
  auto out = RunWat(in);
  ASSERT_THAT(out, IsOk());
  EXPECT_EQ(out->eval_return, 16u);
  CelValue cv = DecodeCelValue(out->memory_after, out->eval_return);
  EXPECT_EQ(cv.kind, CEL_INT);
  EXPECT_EQ(cv.payload.i, 7);
  // Real runtime's cel_reset ran: arena cursor/limit at bytes 8/12.
  ArenaHeader ah = DecodeArenaHeader(out->memory_after);
  EXPECT_EQ(ah.cursor, 40u);
  EXPECT_EQ(ah.limit, 131072u);
}

TEST(WatRunnerTest, IdentXCelResetDoesNotClobberWorkspace) {
  // Regression: cel_reset writes only cursor/limit (bytes 8..16).
  // The workspace slot at 16 must survive the reset call that runs
  // at the top of $eval.
  WatRunInput in;
  in.wat = kIdentXWat;
  in.pre_writes = {{16u, EncodeIntCelValue(-12345)}};
  auto out = RunWat(in);
  ASSERT_THAT(out, IsOk());
  CelValue cv = DecodeCelValue(out->memory_after, out->eval_return);
  EXPECT_EQ(cv.kind, CEL_INT);
  EXPECT_EQ(cv.payload.i, -12345);
}

// ─────────────────────────────────────────────────────────
// Runtime exercise — cel_alloc through the real runtime.
// cel_alloc bumps the arena cursor (stored in bytes [8,12) by
// cel_reset) and returns the pre-bump offset.  Two successive
// allocs of size 24 should return arena_base and arena_base+24
// if the runtime is wired correctly.
// ─────────────────────────────────────────────────────────

constexpr absl::string_view kTwoAllocsWat = R"WAT(
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (func $eval (result i32)
    (local $first i32)
    (local $second i32)
    (call $cel_reset (i32.const 16) (i32.const 131072))
    (local.set $first  (call $cel_alloc (i32.const 24)))
    (local.set $second (call $cel_alloc (i32.const 24)))
    ;; Write both offsets as u32s into bytes [200, 208) so the
    ;; test can inspect them.  $eval returns the first offset.
    (i32.store offset=200 (i32.const 0) (local.get $first))
    (i32.store offset=204 (i32.const 0) (local.get $second))
    (local.get $first))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
)WAT";

TEST(WatRunnerTest, CelAllocThroughRealRuntimeBumpsCursor) {
  WatRunInput in;
  in.wat = kTwoAllocsWat;
  auto out = RunWat(in);
  ASSERT_THAT(out, IsOk());

  // $eval returned the first alloc's offset — cel_alloc's contract
  // says it returns the pre-bump cursor, which was arena_base=16
  // at the start.
  EXPECT_EQ(out->eval_return, 16u);

  // Inspect the saved offsets.  Second alloc should sit 24 bytes
  // past the first.
  uint32_t first = 0, second = 0;
  std::memcpy(&first, out->memory_after.data() + 200, sizeof(first));
  std::memcpy(&second, out->memory_after.data() + 204, sizeof(second));
  EXPECT_EQ(first, 16u);
  EXPECT_EQ(second, 40u);

  // After two 24-byte allocs, the cursor should be arena_base + 48.
  ArenaHeader ah = DecodeArenaHeader(out->memory_after);
  EXPECT_EQ(ah.cursor, 16u + 48u);
  EXPECT_EQ(ah.limit, 131072u);
}

// ─────────────────────────────────────────────────────────
// Expression 4 — select `c.name` via stubbed cel_host.
// Prototypes the four-arg ABI end-to-end BEFORE the real
// Layer-2/3 trampoline is implemented.  The stub:
//   - inspects msg_slot (proves the caller passed the right
//     variable slot — not just any i32).
//   - writes a CelValue{CEL_STRING, payload.s=<static span>} into
//     out_slot (proves the caller's slot is where the result lands).
//   - rejects unexpected field_ref_id / attribute_id (proves the
//     caller passed the ABI ids verbatim).
// ─────────────────────────────────────────────────────────

constexpr absl::string_view kSelectCNameWat = R"WAT(
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel_host" "cel_get_field"
          (func $cel_get_field (param i32 i32 i32 i32)))
  ;; Static string bytes at [200, 203) — used as the stub's fake
  ;; return span so we don't have to call cel_alloc from the stub.
  (data (i32.const 200) "Ada")
  (func $eval (result i32)
    (local $c_off i32)
    (local.set $c_off (i32.const 16))
    (call $cel_reset (i32.const 64) (i32.const 131072))
    (call $cel_get_field
          (i32.const 40)
          (local.get $c_off)
          (i32.const 1)
          (i32.const 1))
    (i32.const 40))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
)WAT";

TEST(WatRunnerTest, SelectCNameStubEndToEndRoundTripsFourArgAbi) {
  // Track stub invocations so assertions can check the ABI contract.
  struct Capture {
    int calls = 0;
    uint32_t out_slot = 0;
    uint32_t msg_slot = 0;
    uint32_t field_ref_id = 0;
    uint32_t attribute_id = 0;
  };
  auto capture = std::make_shared<Capture>();

  WatRunInput in;
  in.wat = kSelectCNameWat;
  // Pre-write the message CelValue into c's slot.  msg_slot value is
  // arbitrary for this prototype — 0x1234 is a recognisable sentinel.
  in.pre_writes = {{16u, EncodeMessageCelValue(0x1234)}};
  in.cel_get_field_stub =
      [capture](uint32_t out_slot, uint32_t msg_slot, uint32_t field_ref_id,
                uint32_t attribute_id, uint8_t* memory,
                size_t /*mem_size*/) {
        ++capture->calls;
        capture->out_slot = out_slot;
        capture->msg_slot = msg_slot;
        capture->field_ref_id = field_ref_id;
        capture->attribute_id = attribute_id;

        // Prove the stub can read msg_slot back out.
        CelValue incoming;
        std::memcpy(&incoming, memory + msg_slot, sizeof(incoming));

        // Write CelValue{CEL_STRING, span(ptr=200, len=3)} to out_slot.
        CelValue out{};
        out.kind = CEL_STRING;
        out.payload.s.ptr = 200;
        out.payload.s.len = 3;
        std::memcpy(memory + out_slot, &out, sizeof(out));
      };

  auto result = RunWat(in);
  ASSERT_THAT(result, IsOk());

  // ABI assertions — the four i32 args the wasm passed should
  // arrive at the stub verbatim.
  EXPECT_EQ(capture->calls, 1);
  EXPECT_EQ(capture->out_slot, 40u);
  EXPECT_EQ(capture->msg_slot, 16u);
  EXPECT_EQ(capture->field_ref_id, 1u);
  EXPECT_EQ(capture->attribute_id, 1u);

  // $eval returned out_slot, which now holds the stub-written string.
  EXPECT_EQ(result->eval_return, 40u);
  CelValue cv = DecodeCelValue(result->memory_after, result->eval_return);
  EXPECT_EQ(cv.kind, CEL_STRING);
  EXPECT_EQ(ReadSpan(result->memory_after, cv.payload.s), "Ada");
}

// Stub that simulates an UNKNOWN branch — writes CelValue{CEL_UNKNOWN,
// unk=<attribute_id>} to out_slot.  Pre-flight for M2.E:
// PartialEval's trampoline absorption semantics work the same way.
TEST(WatRunnerTest, SelectWithStubWritingUnknownCelValue) {
  WatRunInput in;
  in.wat = kSelectCNameWat;
  in.pre_writes = {{16u, EncodeMessageCelValue(0xdead)}};
  in.cel_get_field_stub = [](uint32_t out_slot, uint32_t /*msg_slot*/,
                             uint32_t /*field_ref_id*/,
                             uint32_t attribute_id, uint8_t* memory,
                             size_t /*mem_size*/) {
    CelValue out{};
    out.kind = CEL_UNKNOWN;
    out.payload.unk = attribute_id;
    std::memcpy(memory + out_slot, &out, sizeof(out));
  };
  auto result = RunWat(in);
  ASSERT_THAT(result, IsOk());
  CelValue cv = DecodeCelValue(result->memory_after, result->eval_return);
  EXPECT_EQ(cv.kind, CEL_UNKNOWN);
  EXPECT_EQ(cv.payload.unk, 1u);  // attribute_id from the wat.
}

// Missing stub when the WAT imports cel_host.cel_get_field → module
// instantiation fails with FailedPrecondition.  Locks the contract
// that future codegen arms cannot sneak an unimplemented host import
// past the harness.
TEST(WatRunnerTest, SelectWithoutStubFailsInstantiation) {
  WatRunInput in;
  in.wat = kSelectCNameWat;
  // No stub supplied.
  auto result = RunWat(in);
  EXPECT_FALSE(result.ok());
}

}  // namespace
}  // namespace celwasm
