// Trampoline-level e2e for the `WasmtimeMemoryView::ReadSpan`
// bounds check — closes cleanup-backlog #42 / coverage gap 1.
//
// INVARIANT under test (the contract this file defends):
//
//   Every host-fn trampoline that lifts a wasm-supplied
//   (ptr, len) pair into an `absl::string_view` MUST route the
//   read through `WasmtimeMemoryView::ReadSpan`, which
//   bounds-checks against `MemoryView::Size()`.  When wasm
//   supplies an out-of-bounds (ptr, len), the lift returns an
//   EMPTY view — not host bytes adjacent to the wasm
//   reservation, not a SIGSEGV on the guard page.
//
// Anchors the invariant guards (file:line for future-refactor
// reference):
//   - `eval/internal/cel_host_wasmtime.h:127`
//     (`WasmtimeMemoryView::ReadSpan` — the load-bearing bounds
//     check).
//   - `eval/host_call_context.cc:368`
//     (`HostCallContext::ArgString` — the typed accessor every
//     `@host` callback uses; this is the line the trampoline lifts
//     through).
//   - `eval/engine.cc:455`
//     (`HostCallbackTrampoline` — the function that builds the
//     `WasmtimeMemoryView mem(ctx, he->memory)` over the live
//     wasmtime shared memory and hands it to the callback).
//
// Why hand-authored wasm: a naturally compiled CEL expression can't
// synthesise an adversarial (ptr, len) pair — codegen + the
// activation marshal go through `WasmtimeArenaAllocator`, which only
// hands out in-bounds offsets.  The full attack surface is only
// reachable via wasm that stages the bad CelValue directly in linear
// memory and then calls a host trampoline with that slot.  Hence the
// `.wat` fixture under `doc/implementation-plan/rewrite/wat/`.
//
// Failure modes this test catches (each is a real way a future
// refactor could regress the contract):
//
//   1. A new trampoline reads via `wasmtime_sharedmemory_data(mem)
//      + ptr` directly without the IsInBounds guard.  Outcome:
//      SIGSEGV on the guard page (test crashes loudly) OR host
//      bytes leaked into the callback's `string_view` (test fails
//      with non-empty captured.size).
//   2. A trampoline lifts via an unguarded helper that bypasses
//      `MemoryView::ReadSpan` (e.g. takes a raw `uint8_t*`).
//      Same outcome — captured view is not empty.
//   3. `WasmtimeMemoryView::ReadSpan`'s `IsInBounds(ptr, len)`
//      check is removed or weakened (e.g. an additive `ptr + len >
//      Size()` form that u32-wraps).  Captured view fills with host
//      bytes; the test fails.
//
// Test cases probe THREE adversarial inputs (each lifts through the
// same trampoline path, but stresses a different shape of the
// bounds check):
//
//   - `ptr = 0xFFFFFFFF, len = 64` — the exact audit case from
//     cleanup-backlog #36.  Reads 4 GiB past mem base.
//   - `ptr = Size() - 1, len = 2` — straddles the boundary.  An
//     additive check that overflows at the boundary would miss it;
//     `IsInBounds`'s subtraction form catches it.
//   - `ptr = 0x80000000, len = 0x80000000` — u32 wrap to a small
//     in-range address.  Additive `ptr + len > Size()` would
//     evaluate to `0 > Size()` = false; the subtraction form
//     `len > Size() - ptr` catches it.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/program.h"
#include "eval/engine.h"
#include "eval/host_call_context.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// Captured state from the host callback.  A pointer into this
// struct is plumbed through the callback's lambda closure so the
// test can assert post-Eval what the trampoline actually lifted.
struct CapturedLift {
  // Non-OK iff `ArgString` failed for a reason OTHER than a
  // legitimate empty string view (e.g. wrong kind, dangling slot).
  // For the OOB cases below, the lift returns OK with an EMPTY
  // string_view — that is the documented contract.
  absl::Status status;
  // Length of the lifted string_view.  MUST be 0 for the OOB
  // probes — a non-zero length proves either ReadSpan didn't
  // bounds-check (returning host bytes) or a new code path
  // bypassed MemoryView entirely.
  size_t lifted_len = 0;
  // Snapshot of the lifted bytes (copied — the source is wasm
  // linear memory which doesn't outlive the callback).  For the
  // OOB probes this MUST be empty; if a regression leaks host
  // bytes into the lift, the bytes themselves are useful
  // diagnostic evidence in the failure message.
  std::string lifted_bytes;
  bool callback_fired = false;
};

absl::StatusOr<std::vector<uint8_t>> Wat2Wasm(absl::string_view wat) {
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
  if (err != nullptr) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string text(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return absl::InvalidArgumentError(absl::StrCat("wat2wasm: ", text));
  }
  std::vector<uint8_t> bytes(out.data, out.data + out.size);
  wasm_byte_vec_delete(&out);
  return bytes;
}

// Load a WAT fixture from runfiles.  Mirrors the loader in
// `tools/wat_runner/wat_runner_test.cc`.
absl::StatusOr<std::string> LoadWat(absl::string_view filename) {
  std::string path =
      absl::StrCat("doc/implementation-plan/rewrite/wat/", filename);
  std::ifstream f{path};
  if (!f) {
    return absl::NotFoundError(absl::StrCat("wat file: ", path));
  }
  std::stringstream buf;
  buf << f.rdbuf();
  return buf.str();
}

// Build a WAT module of the same shape as
// `68_ReadSpanOobInTrampoline.wat`, parameterized by the (ptr, len)
// pair staged in the arg0 CelValue at offset 16.  We could just
// load the .wat once with the audit ptr; instead we generate the
// three probe variants here so a single source of truth (this
// helper) drives the assertion matrix.  The .wat in
// `doc/implementation-plan/rewrite/wat/` is the canonical
// human-readable trace + walkthrough; the test exercises three
// concretizations of the same shape.
std::string MakeOobProbeWat(uint32_t bad_ptr, uint32_t bad_len) {
  // CelValue at offset 16:
  //   u32 kind          = 0x05 0x00 0x00 0x00   (CEL_STRING)
  //   u32 _pad          = 0x00 0x00 0x00 0x00
  //   u32 payload.s.ptr = <bad_ptr> LE
  //   u32 payload.s.len = <bad_len> LE
  //   u64 tail          = 0
  auto le_bytes = [](uint32_t v) {
    char buf[5 * 4];  // enough for 4 escape sequences
    int n = std::snprintf(buf, sizeof(buf), R"(\%02x\%02x\%02x\%02x)",
                          static_cast<unsigned>(v & 0xff),
                          static_cast<unsigned>((v >> 8) & 0xff),
                          static_cast<unsigned>((v >> 16) & 0xff),
                          static_cast<unsigned>((v >> 24) & 0xff));
    return std::string(buf, n);
  };
  return absl::StrCat(
      "(module\n"
      "  (import \"cel\" \"memory\" (memory 2 1024 shared))\n"
      "  (import \"cel\" \"arena_reset\" (func $arena_reset))\n"
      "  (import \"cel_fn\" \"probe_string\"\n"
      "    (func $probe_string (param i32 i32)))\n"
      "  (data (i32.const 16)\n"
      "        \"\\05\\00\\00\\00\"\n"
      "        \"\\00\\00\\00\\00\"\n"
      "        \"",
      le_bytes(bad_ptr),
      "\"\n"
      "        \"",
      le_bytes(bad_len),
      "\"\n"
      "        \"\\00\\00\\00\\00\"\n"
      "        \"\\00\\00\\00\\00\")\n"
      "  (func $eval (result i32)\n"
      "    (call $arena_reset)\n"
      "    (call $probe_string (i32.const 40) (i32.const 16))\n"
      "    (i32.const 40))\n"
      "  (export \"eval\" (func $eval))\n"
      "  (export \"memory\" (memory 0)))\n");
}

// Build the host callback for the `cel_fn.probe_string` import.
// Captures the lifted `string_view` (length + bytes copy + status)
// into `*out`, then writes an INT CelValue to the out_slot so
// `Instance::Eval()` returns cleanly and the test can fail with a
// specific assertion rather than a wasm trap.
HostCallback MakeProbeCallback(CapturedLift* out) {
  return [out](HostCallContext& ctx) -> absl::Status {
    out->callback_fired = true;
    auto sv_or = ctx.ArgString(0);
    if (!sv_or.ok()) {
      out->status = sv_or.status();
      // Still write a CelValue to the out_slot so Eval() doesn't
      // see an undecodable result.  Sentinel: kind=CEL_INT,
      // payload.i = -1 so an accidental success-path mis-read
      // looks visibly wrong.
      return ctx.ReturnInt(-1);
    }
    const absl::string_view sv = *sv_or;
    out->lifted_len = sv.size();
    out->lifted_bytes.assign(sv.data(), sv.size());
    out->status = absl::OkStatus();
    return ctx.ReturnInt(static_cast<int64_t>(sv.size()));
  };
}

// Plan + Eval the OOB-probe wasm against an engine that has
// `probe_string` registered.  Returns the trapped status from any
// stage (Build / AddFunction / Plan / Eval) so the test can assert
// against it.
absl::Status RunProbe(uint32_t bad_ptr, uint32_t bad_len,
                      CapturedLift* captured, Value* result_out) {
  auto engine_or = Engine::NewBuilder().Build();
  if (!engine_or.ok()) return engine_or.status();
  Engine engine = *std::move(engine_or);
  if (auto s = engine.AddFunction("probe_string", /*num_args=*/2,
                                  MakeProbeCallback(captured));
      !s.ok()) {
    return s;
  }
  auto wasm_or = Wat2Wasm(MakeOobProbeWat(bad_ptr, bad_len));
  if (!wasm_or.ok()) return wasm_or.status();
  Program program(*std::move(wasm_or));
  auto inst_or = engine.Plan(program);
  if (!inst_or.ok()) return inst_or.status();
  auto val_or = inst_or->Eval();
  if (!val_or.ok()) return val_or.status();
  *result_out = *std::move(val_or);
  return absl::OkStatus();
}

// ── canonical .wat assembles cleanly ─────────────────────────────
//
// Sanity: the human-readable trace under
// `doc/implementation-plan/rewrite/wat/` must round-trip through
// the production wasm assembler.  Catches a regression where the
// `.wat` drifts from a valid module shape (e.g. the threads /
// shared-memory feature bit is dropped from wasmtime's default
// config and the import surface stops parsing).
TEST(HostTrampolineBoundsTest, CanonicalWatFixtureAssembles) {
  auto wat_or = LoadWat("68_ReadSpanOobInTrampoline.wat");
  ASSERT_TRUE(wat_or.ok()) << wat_or.status();
  auto bytes_or = Wat2Wasm(*wat_or);
  ASSERT_TRUE(bytes_or.ok()) << bytes_or.status();
  EXPECT_GT(bytes_or->size(), 0u);
}

// ── the three probe shapes ──────────────────────────────────────

TEST(HostTrampolineBoundsTest, AdversarialMaxPtrLiftsToEmptyString) {
  CapturedLift captured;
  Value result;
  // The exact audit case from cleanup-backlog #36.
  absl::Status s =
      RunProbe(/*bad_ptr=*/0xFFFFFFFFu, /*bad_len=*/64u, &captured, &result);
  ASSERT_TRUE(s.ok()) << s;
  EXPECT_TRUE(captured.callback_fired) << "trampoline did not fire callback";
  EXPECT_TRUE(captured.status.ok()) << captured.status;
  // The load-bearing assertion: an OOB lift returns ZERO bytes.
  // A future trampoline that skips MemoryView::ReadSpan and
  // dereferences directly will either SIGSEGV (this test crashes
  // loudly) or fill `lifted_bytes` with host memory (this test
  // fails with the leaked bytes visible in the diagnostic).
  EXPECT_EQ(captured.lifted_len, 0u)
      << "ReadSpan bypass: lifted " << captured.lifted_len
      << " bytes for ptr=0xFFFFFFFF; first byte=0x"
      << (captured.lifted_bytes.empty()
              ? 0
              : static_cast<unsigned>(
                    static_cast<uint8_t>(captured.lifted_bytes[0])));
  EXPECT_TRUE(captured.lifted_bytes.empty());
  // Returned Value mirrors the lifted length — sanity-check the
  // trampoline's out_slot write went through.
  ASSERT_EQ(result.kind(), Value::Kind::kInt);
  EXPECT_EQ(*result.AsInt(), 0);
}

TEST(HostTrampolineBoundsTest, HugeLenAcrossOverflowsLiftsToEmptyString) {
  CapturedLift captured;
  Value result;
  // ptr is well inside memory, but len is large enough that
  // ptr + len overflows past UINT32_MAX.  Catches a straddler
  // bypass on the additive form `ptr + len > Size()` — the sum
  // wraps and may compare false against `Size()`.  The
  // subtraction form `len > Size() - ptr` sees a huge `len` and
  // rejects.  Memory-size-agnostic so it's robust against the
  // runtime growing the wasm memory over time.
  absl::Status s =
      RunProbe(/*bad_ptr=*/0x100u, /*bad_len=*/0xFFFFFFFEu, &captured, &result);
  ASSERT_TRUE(s.ok()) << s;
  EXPECT_TRUE(captured.callback_fired);
  EXPECT_TRUE(captured.status.ok()) << captured.status;
  EXPECT_EQ(captured.lifted_len, 0u);
  EXPECT_TRUE(captured.lifted_bytes.empty());
  ASSERT_EQ(result.kind(), Value::Kind::kInt);
  EXPECT_EQ(*result.AsInt(), 0);
}

TEST(HostTrampolineBoundsTest, U32WrapLiftsToEmptyString) {
  CapturedLift captured;
  Value result;
  // ptr + len wraps to 0.  An additive check `ptr + len > Size()`
  // evaluates `0 > Size()` = false (BYPASS).  The subtraction form
  // sees `len > Size() - ptr` = `0x80000000 > <small>` = true and
  // rejects.  This case is the load-bearing reason for ReadSpan's
  // subtraction-form check.
  absl::Status s = RunProbe(/*bad_ptr=*/0x80000000u, /*bad_len=*/0x80000000u,
                            &captured, &result);
  ASSERT_TRUE(s.ok()) << s;
  EXPECT_TRUE(captured.callback_fired);
  EXPECT_TRUE(captured.status.ok()) << captured.status;
  EXPECT_EQ(captured.lifted_len, 0u);
  EXPECT_TRUE(captured.lifted_bytes.empty());
  ASSERT_EQ(result.kind(), Value::Kind::kInt);
  EXPECT_EQ(*result.AsInt(), 0);
}

// ── positive control ────────────────────────────────────────────
//
// An in-bounds (ptr, len) lifts the literal bytes — proves the
// callback path is wired correctly and the empty-on-OOB result
// above is genuinely the bounds-check firing, not a no-op
// callback or a broken decode.
TEST(HostTrampolineBoundsTest, InBoundsLiftReturnsStagedBytes) {
  CapturedLift captured;
  Value result;
  // Stage "hello" at byte offset 80 by piggy-backing on the
  // generated module shape (a small extra data segment).  Easier:
  // synthesize a fresh WAT here with the bytes inline.
  constexpr char kWat[] = R"WAT(
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel_fn" "probe_string"
    (func $probe_string (param i32 i32)))
  (data (i32.const 16)
        "\05\00\00\00"
        "\00\00\00\00"
        "\50\00\00\00"
        "\05\00\00\00"
        "\00\00\00\00"
        "\00\00\00\00")
  (data (i32.const 80) "hello")
  (func $eval (result i32)
    (call $arena_reset)
    (call $probe_string (i32.const 40) (i32.const 16))
    (i32.const 40))
  (export "eval" (func $eval))
  (export "memory" (memory 0)))
)WAT";
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);
  ASSERT_TRUE(engine
                  .AddFunction("probe_string", /*num_args=*/2,
                               MakeProbeCallback(&captured))
                  .ok());
  auto wasm_or = Wat2Wasm(kWat);
  ASSERT_TRUE(wasm_or.ok()) << wasm_or.status();
  Program program(*std::move(wasm_or));
  auto inst_or = engine.Plan(program);
  ASSERT_TRUE(inst_or.ok()) << inst_or.status();
  auto val_or = inst_or->Eval();
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  result = *std::move(val_or);

  EXPECT_TRUE(captured.callback_fired);
  EXPECT_TRUE(captured.status.ok()) << captured.status;
  EXPECT_EQ(captured.lifted_len, 5u);
  EXPECT_EQ(captured.lifted_bytes, "hello");
  ASSERT_EQ(result.kind(), Value::Kind::kInt);
  EXPECT_EQ(*result.AsInt(), 5);
}

}  // namespace
}  // namespace celwasm
