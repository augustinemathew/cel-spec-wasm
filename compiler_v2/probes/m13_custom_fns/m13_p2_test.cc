// M13 Probe 2 — TinyGo-built `rules.wasm` validates the cross-language ABI.
//
// Same harness as `m13_p1_test.cc` but the stub WAT is replaced by a
// real wasm module produced by:
//
//   tinygo build -target=wasm-unknown -no-debug -o rules.wasm \
//     compiler_v2/probes/m13_custom_fns/rules/
//
// (`compiler_v2/probes/m13_custom_fns/rules/build_rules.sh` runs that.)
//
// What changes vs Probe 1:
//
//   * The "foreign module" is now compiled from Go (TinyGo 0.41.1
//     with the `wasm-unknown` target produces a 827-byte, no-import
//     wasm with the canonical `allow_string_string` export).
//   * The TinyGo runtime defines memory itself (vs Probe 1 where the
//     host pre-allocated a `cel.memory`).  So the wiring inverts:
//     instantiate rules.wasm first, extract ITS exported memory, and
//     bind that as `cel.memory` for the caller.  The 24-byte CelValue
//     ABI is unchanged; this is purely about who owns the bytes.
//   * TinyGo emits a `_initialize` export that must be called once
//     to set the `runtime.initialized` flag at memory[65536].  Without
//     it the Go-side runtime traps on the first invocation.
//
// What stays the same:
//
//   * The caller WAT (`m13_p1_caller.wat`) is BYTE-IDENTICAL to Probe 1.
//     It imports `cel.memory` and `rules.allow_string_string`,
//     stages args + bytes via `(data …)` segments, calls the import.
//   * The assertion is unchanged: returned out_slot at offset 64,
//     CelValue{kind=CEL_BOOL, payload.b=1}.  TinyGo's body
//     intentionally returns `true` when both args have the expected
//     CelKind tags (CEL_MESSAGE for arg0, CEL_STRING for arg1).
//
// Win condition: Probe 2 passes iff Probe 1 passes AND the TinyGo
// wasm honoured the cross-language ABI without help.

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

struct WasmtimeState {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_module_t* rules_module = nullptr;
  wasmtime_module_t* caller_module = nullptr;
  wasmtime_linker_t* rules_linker = nullptr;
  wasmtime_linker_t* caller_linker = nullptr;
  wasmtime_instance_t rules_instance{};
  wasmtime_instance_t caller_instance{};

  ~WasmtimeState() {
    if (caller_linker != nullptr) wasmtime_linker_delete(caller_linker);
    if (rules_linker != nullptr) wasmtime_linker_delete(rules_linker);
    if (caller_module != nullptr) wasmtime_module_delete(caller_module);
    if (rules_module != nullptr) wasmtime_module_delete(rules_module);
    if (store != nullptr) wasmtime_store_delete(store);
    if (engine != nullptr) wasm_engine_delete(engine);
  }
};

std::string ReadWorkspaceFile(absl::string_view path) {
  // Bazel test runfiles land workspace files under the test CWD.
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

TEST(M13Probe2, TinyGoForeignWasmCallReturnsBool) {
  WasmtimeState s;

  // ── Engine + store ────────────────────────────────────────────────
  s.engine = wasm_engine_new();
  ASSERT_NE(s.engine, nullptr);
  s.store = wasmtime_store_new(s.engine, nullptr, nullptr);
  ASSERT_NE(s.store, nullptr);
  wasmtime_context_t* ctx = wasmtime_store_context(s.store);

  // ── Load the TinyGo-built rules.wasm ─────────────────────────────
  // Checked into the tree at the location rules/build_rules.sh
  // produces.  Rebuild via that script when rules.go changes.
  const std::string rules_bytes = ReadWorkspaceFile(
      "compiler_v2/probes/m13_custom_fns/rules/rules.wasm");
  ASSERT_FALSE(rules_bytes.empty())
      << "could not read rules.wasm — did rules/build_rules.sh run?";

  ASSERT_EQ(wasmtime_module_new(
                s.engine, reinterpret_cast<const uint8_t*>(rules_bytes.data()),
                rules_bytes.size(), &s.rules_module),
            nullptr);

  // ── Instantiate rules.wasm against an empty linker ───────────────
  // TinyGo's `wasm-unknown` target produces a fully self-contained
  // module (no imports).  An empty linker is sufficient.
  s.rules_linker = wasmtime_linker_new(s.engine);
  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        s.rules_linker, ctx, s.rules_module, &s.rules_instance, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("instantiate(rules)", err);
    if (trap != nullptr) FAIL() << TrapStatus("instantiate(rules)", trap);
  }

  // ── Call TinyGo's `_initialize` (sets runtime.initialized flag) ──
  // Without this the allow() body short-circuits and traps on the
  // first call (see disassembly: load@65536, eqz, br_if 0 → unreachable).
  {
    wasmtime_extern_t init_ext;
    ASSERT_TRUE(wasmtime_instance_export_get(
        ctx, &s.rules_instance, "_initialize", 11, &init_ext));
    ASSERT_EQ(init_ext.kind, WASMTIME_EXTERN_FUNC);
    wasmtime_func_t init_fn = init_ext.of.func;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_func_call(
        ctx, &init_fn, /*args=*/nullptr, /*nargs=*/0, /*results=*/nullptr,
        /*nresults=*/0, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("_initialize", err);
    if (trap != nullptr) FAIL() << TrapStatus("_initialize", trap);
  }

  // ── Extract rules.wasm's exports we'll bind into the caller ──────
  wasmtime_extern_t rules_memory_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(ctx, &s.rules_instance, "memory", 6,
                                           &rules_memory_ext));
  ASSERT_EQ(rules_memory_ext.kind, WASMTIME_EXTERN_MEMORY);

  constexpr absl::string_view kExportName = "allow_string_string";
  wasmtime_extern_t allow_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(
      ctx, &s.rules_instance, kExportName.data(), kExportName.size(),
      &allow_ext))
      << "rules.wasm does not export " << kExportName;
  ASSERT_EQ(allow_ext.kind, WASMTIME_EXTERN_FUNC);

  // ── Caller WAT (byte-identical to Probe 1) ───────────────────────
  const std::string caller_wat = ReadWorkspaceFile(
      "doc/implementation-plan/rewrite/wat/m13_p1_caller.wat");
  ASSERT_FALSE(caller_wat.empty()) << "could not read m13_p1_caller.wat";

  auto caller_bytes_or = WatToWasm(caller_wat);
  ASSERT_TRUE(caller_bytes_or.ok()) << caller_bytes_or.status();
  ASSERT_EQ(wasmtime_module_new(s.engine, caller_bytes_or->data(),
                                caller_bytes_or->size(), &s.caller_module),
            nullptr);

  // ── Caller linker ───────────────────────────────────────────────
  // Bind rules.wasm's memory as `cel.memory` (foreign owns the bytes
  // in this configuration — see file header) and rules.wasm's allow_*
  // export as the `rules.allow_*` import.
  s.caller_linker = wasmtime_linker_new(s.engine);
  ASSERT_EQ(wasmtime_linker_define(s.caller_linker, ctx, "cel", 3, "memory", 6,
                                   &rules_memory_ext),
            nullptr);
  ASSERT_EQ(wasmtime_linker_define(s.caller_linker, ctx, "rules", 5,
                                   kExportName.data(), kExportName.size(),
                                   &allow_ext),
            nullptr);

  // ── Instantiate caller (data segments stage args into shared memory) ─
  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        s.caller_linker, ctx, s.caller_module, &s.caller_instance, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("instantiate(caller)", err);
    if (trap != nullptr) FAIL() << TrapStatus("instantiate(caller)", trap);
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
    if (trap != nullptr) FAIL() << TrapStatus("eval", trap);
  }
  ASSERT_EQ(result.kind, WASMTIME_I32);

  // ── Decode the bool CelValue ────────────────────────────────────
  const uint32_t out_offset = static_cast<uint32_t>(result.of.i32);
  EXPECT_EQ(out_offset, 64u);

  // Read from rules.wasm's exported memory (which is also the
  // caller's imported memory — both names refer to the same bytes).
  const uint8_t* mem =
      wasmtime_memory_data(ctx, &rules_memory_ext.of.memory);
  CelValue out{};
  std::memcpy(&out, mem + out_offset, sizeof(out));

  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_BOOL))
      << "TinyGo `allow` should have written a CEL_BOOL CelValue";
  EXPECT_EQ(out.payload.b, 1)
      << "TinyGo `allow` should have returned true for "
         "(CEL_MESSAGE arg, CEL_STRING arg)";
}

}  // namespace
}  // namespace celwasm
