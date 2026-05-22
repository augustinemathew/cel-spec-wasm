// M13 Probe 5 — host-backed custom function via C++ callback.
//
// Symmetric to Probe 2 (TinyGo) / Probe 3 (Rust, C) but for the
// `@host.X` backend.  A C++ callback is registered with wasmtime
// as the impl for `cel_fn.length_string`; the caller WAT imports
// that name and calls it; the callback reads the input CelValue
// from shared memory, computes the result, writes it back.
//
// What this proves:
//
//   * The `cel_fn.X` wasm import name is the canonical host-backend
//     namespace (§4.5 of m13-custom-fns.md).
//   * A `wasmtime_func_callback_t` registered via
//     `wasmtime_linker_define_func` is a workable shape for the
//     `FunctionImpl` callback type the design doc sketches.
//   * The CelValue ABI (24-byte struct, kind tag + payload) works
//     identically when the "foreign module" is a C++ callback
//     instead of a separately-instantiated wasm module.
//   * `wasmtime_caller_export_get(caller, "memory", …)` gives the
//     host callback access to the caller's shared memory — same
//     mechanism the existing `wat_runner` uses for cel_host stubs.
//
// What this deliberately doesn't prove:
//
//   * Higher-level `Value` ↔ `CelValue` typed coercion.  That's a
//     library on top of the raw byte-shuffling validated here;
//     belongs to Slice C.
//   * Outbound string allocation via `arena_alloc`.  Probe-5
//     returns an int (fits inline in payload); a follow-up probe
//     exercises the arena path when a host fn needs to return a
//     string.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/runtime/cel_data.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// ──────────────────────────────────────────────────────────────────
// The host callback for `cel_fn.length_string`.  Signature in
// wasmtime terms: `(out_slot: i32, s_slot: i32) → ()`.  Reads the
// CelValue at `s_slot`, expects CEL_STRING kind, writes the string
// length as a CEL_INT to `out_slot`.
//
// Two pieces of API leverage:
//   * `wasmtime_caller_context()` — the store context inside the
//     callback (so we can poke at the caller's memory).
//   * `wasmtime_caller_export_get(caller, "memory", …)` — pulls
//     the caller's exported memory.  In production, the engine
//     would tie this to the canonical `cel.memory` resolved at
//     Plan time; for the probe the caller exports memory directly.
wasm_trap_t* HostLengthCallback(void* /*env*/, wasmtime_caller_t* caller,
                                const wasmtime_val_t* args, size_t nargs,
                                wasmtime_val_t* /*results*/,
                                size_t /*nresults*/) {
  if (nargs != 2 || args[0].kind != WASMTIME_I32 ||
      args[1].kind != WASMTIME_I32) {
    constexpr char kMsg[] =
        "HostLengthCallback: expected (i32, i32) args";
    wasm_byte_vec_t m;
    wasm_byte_vec_new(&m, sizeof(kMsg) - 1, kMsg);
    wasm_trap_t* t = wasm_trap_new(nullptr, &m);
    wasm_byte_vec_delete(&m);
    return t;
  }
  const uint32_t out_slot = static_cast<uint32_t>(args[0].of.i32);
  const uint32_t str_slot = static_cast<uint32_t>(args[1].of.i32);

  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  wasmtime_extern_t mem_ext;
  if (!wasmtime_caller_export_get(caller, "memory", 6, &mem_ext) ||
      mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
    constexpr char kMsg[] = "HostLengthCallback: caller lacks `memory` export";
    wasm_byte_vec_t m;
    wasm_byte_vec_new(&m, sizeof(kMsg) - 1, kMsg);
    wasm_trap_t* t = wasm_trap_new(nullptr, &m);
    wasm_byte_vec_delete(&m);
    return t;
  }
  uint8_t* mem = wasmtime_memory_data(ctx, &mem_ext.of.memory);

  CelValue input{};
  std::memcpy(&input, mem + str_slot, sizeof(input));
  if (input.kind != static_cast<uint32_t>(CEL_STRING)) {
    constexpr char kMsg[] =
        "HostLengthCallback: arg 0 was not a CEL_STRING";
    wasm_byte_vec_t m;
    wasm_byte_vec_new(&m, sizeof(kMsg) - 1, kMsg);
    wasm_trap_t* t = wasm_trap_new(nullptr, &m);
    wasm_byte_vec_delete(&m);
    return t;
  }

  // Compute + write the result.  No allocation: int fits inline in
  // the 24-byte CelValue's payload.
  CelValue output{};
  output.kind = static_cast<uint32_t>(CEL_INT);
  output.payload.i = static_cast<int64_t>(input.payload.s.len);
  std::memcpy(mem + out_slot, &output, sizeof(output));
  return nullptr;  // no trap
}

// Construct the (i32, i32) → () functype for the callback.  Mirrors
// wat_runner's `HostThreeArgTrampolineType` pattern.
wasm_functype_t* MakeLengthCallbackType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_new_uninitialized(&params, 2);
  params.data[0] = wasm_valtype_new(WASM_I32);
  params.data[1] = wasm_valtype_new(WASM_I32);
  wasm_valtype_vec_t results;
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

// ──────────────────────────────────────────────────────────────────
// Test fixture state.

struct WasmtimeState {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_module_t* caller_module = nullptr;
  wasmtime_linker_t* linker = nullptr;
  wasmtime_memory_t memory{};
  wasmtime_instance_t caller_instance{};

  ~WasmtimeState() {
    if (linker != nullptr) wasmtime_linker_delete(linker);
    if (caller_module != nullptr) wasmtime_module_delete(caller_module);
    if (store != nullptr) wasmtime_store_delete(store);
    if (engine != nullptr) wasm_engine_delete(engine);
  }
};

std::string ReadFile(absl::string_view path) {
  std::ifstream in(std::string{path}, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

absl::Status WasmtimeErrorStatus(absl::string_view ctx,
                                 wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  absl::Status s = absl::InternalError(
      absl::StrCat(ctx, ": ", std::string(msg.data, msg.size)));
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return s;
}

absl::Status TrapStatus(absl::string_view ctx, wasm_trap_t* trap) {
  wasm_byte_vec_t msg;
  wasm_trap_message(trap, &msg);
  absl::Status s = absl::InternalError(
      absl::StrCat(ctx, ": ", std::string(msg.data, msg.size)));
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return s;
}

absl::StatusOr<std::vector<uint8_t>> WatToWasm(absl::string_view wat) {
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
  if (err != nullptr) return WasmtimeErrorStatus("wat2wasm", err);
  std::vector<uint8_t> bytes(out.data, out.data + out.size);
  wasm_byte_vec_delete(&out);
  return bytes;
}

// ──────────────────────────────────────────────────────────────────
// Tests.

TEST(M13Probe5Host, HostCallbackReadsStringWritesIntLength) {
  WasmtimeState s;

  s.engine = wasm_engine_new();
  ASSERT_NE(s.engine, nullptr);
  s.store = wasmtime_store_new(s.engine, nullptr, nullptr);
  ASSERT_NE(s.store, nullptr);
  wasmtime_context_t* ctx = wasmtime_store_context(s.store);

  // Host-owned 2-page memory shared with the caller.
  wasm_memorytype_t* mty = nullptr;
  ASSERT_EQ(
      wasmtime_memorytype_new(/*min=*/2, /*max_present=*/true, /*max=*/2,
                              /*is_64=*/false, /*shared=*/false,
                              /*page_size_log2=*/16, &mty),
      nullptr);
  ASSERT_EQ(wasmtime_memory_new(ctx, mty, &s.memory), nullptr);
  wasm_memorytype_delete(mty);

  wasmtime_extern_t mem_ext;
  mem_ext.kind = WASMTIME_EXTERN_MEMORY;
  mem_ext.of.memory = s.memory;

  // Load + assemble caller WAT.
  const std::string caller_wat = ReadFile(
      "doc/implementation-plan/rewrite/wat/m13_p5_caller.wat");
  ASSERT_FALSE(caller_wat.empty()) << "could not read m13_p5_caller.wat";
  auto caller_bytes_or = WatToWasm(caller_wat);
  ASSERT_TRUE(caller_bytes_or.ok()) << caller_bytes_or.status();
  ASSERT_EQ(wasmtime_module_new(s.engine, caller_bytes_or->data(),
                                caller_bytes_or->size(), &s.caller_module),
            nullptr);

  // Linker provides cel.memory + the host callback under
  // `cel_fn.length_string`.
  s.linker = wasmtime_linker_new(s.engine);
  ASSERT_EQ(wasmtime_linker_define(s.linker, ctx, "cel", 3, "memory", 6,
                                   &mem_ext),
            nullptr);

  wasm_functype_t* ftype = MakeLengthCallbackType();
  ASSERT_EQ(wasmtime_linker_define_func(
                s.linker, "cel_fn", 6, "length_string", 13, ftype,
                HostLengthCallback, /*data=*/nullptr, /*finalizer=*/nullptr),
            nullptr);
  wasm_functype_delete(ftype);

  // Instantiate caller.
  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        s.linker, ctx, s.caller_module, &s.caller_instance, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("instantiate", err);
    if (trap != nullptr) FAIL() << TrapStatus("instantiate", trap);
  }

  // Call eval().
  wasmtime_extern_t eval_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(ctx, &s.caller_instance, "eval", 4,
                                           &eval_ext));
  ASSERT_EQ(eval_ext.kind, WASMTIME_EXTERN_FUNC);

  wasmtime_val_t result{};
  {
    wasmtime_func_t eval_fn = eval_ext.of.func;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_func_call(ctx, &eval_fn, /*args=*/nullptr, /*nargs=*/0,
                           &result, /*nresults=*/1, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("eval", err);
    if (trap != nullptr) FAIL() << TrapStatus("eval", trap);
  }
  ASSERT_EQ(result.kind, WASMTIME_I32);

  // Decode the int CelValue at out_slot.
  const uint32_t out_offset = static_cast<uint32_t>(result.of.i32);
  EXPECT_EQ(out_offset, 40u);

  const uint8_t* mem = wasmtime_memory_data(ctx, &s.memory);
  CelValue out{};
  std::memcpy(&out, mem + out_offset, sizeof(out));

  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_INT));
  // "hello world" is 11 chars.
  EXPECT_EQ(out.payload.i, 11);
}

}  // namespace
}  // namespace celwasm
