// M13 Probe 1 — validates the cross-module wasm-link contract for
// foreign-wasm-backed CEL custom functions.
//
// Two hand-rolled WAT modules sit at:
//   * doc/implementation-plan/rewrite/wat/m13_p1_rules_stub.wat
//       — stand-in for what TinyGo / Rust / AS produces from a
//       `bool rules.<fn>(...);` declaration in `.celfn`.
//   * doc/implementation-plan/rewrite/wat/m13_p1_caller.wat
//       — stand-in for what celwasmc emits for an expression
//       calling into a `rules.<fn>` declared fn.
//
// Both modules share a host-owned 2-page wasm memory.  The probe
// drives them end-to-end:
//
//   1. Allocates `cel.memory` (host-owned, 2 pages).
//   2. Instantiates the stub against a linker carrying `cel.memory`.
//   3. Extracts the stub's `allow_string_string` export.
//   4. Binds that export as `rules.allow_string_string`
//      on a second linker that also carries `cel.memory`.
//   5. Instantiates the caller against that linker.
//   6. Calls `eval()`.
//   7. Decodes the CelValue at the returned offset.
//   8. Asserts kind=CEL_BOOL and payload.b=1.
//
// This proves four facts the M13 design depends on:
//
//   * The wasmtime Linker can wire one module's export as another
//     module's import under an arbitrary `<alias>.<name>` namespace
//     (here: `rules.allow_string_string`).
//   * Two user wasms can share a host-owned `cel.memory`.
//   * The caller can pre-stage CelValue args and an out_slot in
//     memory, pass slot offsets to an import, and decode the result
//     by reading the out_slot bytes after the call returns.
//   * The 24-byte CelValue wire layout from cel_data.h is the
//     right ABI surface to publish for cross-language toolchains
//     (Probe 2 will compile a TinyGo module to the same shape).

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
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// Probe-private RAII for the wasm objects we own.  Intentionally not
// shared with `wat_runner` — the probe is a parallel implementation
// that validates the wiring without coupling to the production
// harness's invariants.  If the harness's invariants ever drift,
// the probe still proves the underlying ABI surface independently.
struct WasmtimeState {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_module_t* stub_module = nullptr;
  wasmtime_module_t* caller_module = nullptr;
  wasmtime_linker_t* stub_linker = nullptr;
  wasmtime_linker_t* caller_linker = nullptr;
  wasmtime_memory_t memory{};
  wasmtime_instance_t stub_instance{};
  wasmtime_instance_t caller_instance{};

  ~WasmtimeState() {
    if (caller_linker != nullptr) wasmtime_linker_delete(caller_linker);
    if (stub_linker != nullptr) wasmtime_linker_delete(stub_linker);
    if (caller_module != nullptr) wasmtime_module_delete(caller_module);
    if (stub_module != nullptr) wasmtime_module_delete(stub_module);
    if (store != nullptr) wasmtime_store_delete(store);
    if (engine != nullptr) wasm_engine_delete(engine);
  }
};

std::string ReadWatFile(absl::string_view filename) {
  // Bazel test runfiles land workspace files under the test CWD.
  const std::string path =
      absl::StrCat("doc/implementation-plan/rewrite/wat/", filename);
  std::ifstream in(path);
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

TEST(M13Probe1, ForeignWasmCallReturnsBool) {
  WasmtimeState s;

  // ── Engine + store ────────────────────────────────────────────────
  s.engine = wasm_engine_new();
  ASSERT_NE(s.engine, nullptr);
  s.store = wasmtime_store_new(s.engine, nullptr, nullptr);
  ASSERT_NE(s.store, nullptr);
  wasmtime_context_t* ctx = wasmtime_store_context(s.store);

  // ── Host-owned 2-page memory ─────────────────────────────────────
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

  // ── Load + assemble both WAT files ───────────────────────────────
  const std::string caller_wat = ReadWatFile("m13_p1_caller.wat");
  ASSERT_FALSE(caller_wat.empty()) << "could not read m13_p1_caller.wat";
  const std::string stub_wat = ReadWatFile("m13_p1_rules_stub.wat");
  ASSERT_FALSE(stub_wat.empty()) << "could not read m13_p1_rules_stub.wat";

  auto caller_bytes_or = WatToWasm(caller_wat);
  ASSERT_TRUE(caller_bytes_or.ok()) << caller_bytes_or.status();
  auto stub_bytes_or = WatToWasm(stub_wat);
  ASSERT_TRUE(stub_bytes_or.ok()) << stub_bytes_or.status();

  ASSERT_EQ(wasmtime_module_new(s.engine, caller_bytes_or->data(),
                                caller_bytes_or->size(), &s.caller_module),
            nullptr);
  ASSERT_EQ(wasmtime_module_new(s.engine, stub_bytes_or->data(),
                                stub_bytes_or->size(), &s.stub_module),
            nullptr);

  // ── Stub linker + instantiation ──────────────────────────────────
  // The stub imports only `cel.memory`.
  s.stub_linker = wasmtime_linker_new(s.engine);
  ASSERT_EQ(wasmtime_linker_define(s.stub_linker, ctx, "cel", 3, "memory", 6,
                                   &mem_ext),
            nullptr);

  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        s.stub_linker, ctx, s.stub_module, &s.stub_instance, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("instantiate(stub)", err);
    if (trap != nullptr) {
      FAIL() << TrapStatus("instantiate(stub)", trap);
    }
  }

  // ── Extract the stub's allow_* export ────────────────────────────
  constexpr absl::string_view kExportName = "allow_string_string";
  wasmtime_extern_t allow_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(
      ctx, &s.stub_instance, kExportName.data(), kExportName.size(),
      &allow_ext))
      << "stub does not export " << kExportName;
  ASSERT_EQ(allow_ext.kind, WASMTIME_EXTERN_FUNC);

  // ── Caller linker + instantiation ────────────────────────────────
  // The caller imports `cel.memory` AND `rules.allow_*` — that's
  // exactly the namespace-routing the .celfn IDL's `<alias>.<fn>(...)`
  // form generates.
  s.caller_linker = wasmtime_linker_new(s.engine);
  ASSERT_EQ(wasmtime_linker_define(s.caller_linker, ctx, "cel", 3, "memory", 6,
                                   &mem_ext),
            nullptr);
  ASSERT_EQ(wasmtime_linker_define(s.caller_linker, ctx, "rules", 5,
                                   kExportName.data(), kExportName.size(),
                                   &allow_ext),
            nullptr);

  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        s.caller_linker, ctx, s.caller_module, &s.caller_instance, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("instantiate(caller)", err);
    if (trap != nullptr) {
      FAIL() << TrapStatus("instantiate(caller)", trap);
    }
  }

  // ── Call eval() ──────────────────────────────────────────────────
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
    if (trap != nullptr) {
      FAIL() << TrapStatus("eval", trap);
    }
  }
  ASSERT_EQ(result.kind, WASMTIME_I32);

  // ── Decode the bool CelValue ────────────────────────────────────
  const uint32_t out_offset = static_cast<uint32_t>(result.of.i32);
  EXPECT_EQ(out_offset, 64u)
      << "caller is documented to return out_slot=64";

  const uint8_t* mem = wasmtime_memory_data(ctx, &s.memory);
  CelValue out{};
  std::memcpy(&out, mem + out_offset, sizeof(out));

  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_BOOL))
      << "stub should have written a CEL_BOOL CelValue";
  EXPECT_EQ(out.payload.b, 1)
      << "stub should have written `true` into payload.b";
}

}  // namespace
}  // namespace celwasm
