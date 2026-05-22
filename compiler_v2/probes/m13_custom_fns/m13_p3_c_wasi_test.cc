// M13 Probe 3 (WASI C) — wasi-libc-linked C foreign wasm module
// honours the cross-language ABI.
//
// Sibling to m13_p2_test.cc / m13_p3_c_test.cc / m13_p3_rust_test.cc.
// Same caller WAT.  Same assertions.
//
// What this probe adds beyond bare-C (m13_p3_c_test.cc):
//
//   * Linked against wasi-libc + compiler-rt.  This is the
//     toolchain a real production C user would reach for —
//     stable, has a full libc, and supports threads (sort of).
//   * Built with `-mexec-model=reactor`, which makes wasm-ld
//     emit `_initialize` (analogous to TinyGo).  The engine
//     policy of "call `_initialize` when exported" gets exercised
//     here on a non-Go toolchain.
//
// What stays the same:
//
//   * The `allow_string_string` export.
//   * The 24-byte CelValue ABI on both the read (args) and write
//     (out) sides.
//   * The host harness shape — only the wasm path + the
//     `_initialize` call differs from the bare-C probe.

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

TEST(M13Probe3CWasi, WasiCForeignWasmCallReturnsBool) {
  WasmtimeState s;

  s.engine = wasm_engine_new();
  ASSERT_NE(s.engine, nullptr);
  s.store = wasmtime_store_new(s.engine, nullptr, nullptr);
  ASSERT_NE(s.store, nullptr);
  wasmtime_context_t* ctx = wasmtime_store_context(s.store);

  const std::string rules_bytes = ReadWorkspaceFile(
      "compiler_v2/probes/m13_custom_fns/rules_c_wasi/rules.wasm");
  ASSERT_FALSE(rules_bytes.empty())
      << "could not read rules_c_wasi/rules.wasm — did rules_c_wasi/build_rules.sh run?";

  ASSERT_EQ(wasmtime_module_new(
                s.engine, reinterpret_cast<const uint8_t*>(rules_bytes.data()),
                rules_bytes.size(), &s.rules_module),
            nullptr);

  // wasi-libc-linked + reactor mode: no WASI imports needed by this
  // minimal source.  Empty linker.
  s.rules_linker = wasmtime_linker_new(s.engine);
  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        s.rules_linker, ctx, s.rules_module, &s.rules_instance, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("instantiate(rules)", err);
    if (trap != nullptr) FAIL() << TrapStatus("instantiate(rules)", trap);
  }

  // wasi-libc reactor mode REQUIRES `_initialize` before exports.
  // Sanity-check that it's present, then call it.  This exercises
  // the engine's "call `_initialize` when exported" policy on a
  // non-Go toolchain.
  {
    wasmtime_extern_t init_ext;
    ASSERT_TRUE(wasmtime_instance_export_get(
        ctx, &s.rules_instance, "_initialize", 11, &init_ext))
        << "wasi-libc reactor-mode rules.wasm must export _initialize";
    ASSERT_EQ(init_ext.kind, WASMTIME_EXTERN_FUNC);
    wasmtime_func_t init_fn = init_ext.of.func;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_func_call(
        ctx, &init_fn, /*args=*/nullptr, /*nargs=*/0, /*results=*/nullptr,
        /*nresults=*/0, &trap);
    ASSERT_EQ(err, nullptr) << WasmtimeErrorStatus("_initialize", err);
    if (trap != nullptr) FAIL() << TrapStatus("_initialize", trap);
  }

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

  const std::string caller_wat = ReadWorkspaceFile(
      "doc/implementation-plan/rewrite/wat/m13_p1_caller.wat");
  ASSERT_FALSE(caller_wat.empty()) << "could not read m13_p1_caller.wat";

  auto caller_bytes_or = WatToWasm(caller_wat);
  ASSERT_TRUE(caller_bytes_or.ok()) << caller_bytes_or.status();
  ASSERT_EQ(wasmtime_module_new(s.engine, caller_bytes_or->data(),
                                caller_bytes_or->size(), &s.caller_module),
            nullptr);

  s.caller_linker = wasmtime_linker_new(s.engine);
  ASSERT_EQ(wasmtime_linker_define(s.caller_linker, ctx, "cel", 3, "memory", 6,
                                   &rules_memory_ext),
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
    if (trap != nullptr) FAIL() << TrapStatus("instantiate(caller)", trap);
  }

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

  const uint32_t out_offset = static_cast<uint32_t>(result.of.i32);
  EXPECT_EQ(out_offset, 64u);

  const uint8_t* mem =
      wasmtime_memory_data(ctx, &rules_memory_ext.of.memory);
  CelValue out{};
  std::memcpy(&out, mem + out_offset, sizeof(out));

  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_BOOL))
      << "wasi-C `allow` should have written a CEL_BOOL CelValue";
  EXPECT_EQ(out.payload.b, 1)
      << "wasi-C `allow` should have returned true for two CEL_STRING args";
}

}  // namespace
}  // namespace celwasm
