#include "tools/wat_runner/wat_runner.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "eval/host/cel_log.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_runtime_wasm_bytes.h"
#include "wasi.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

namespace {

// Names of every helper exported by `cel_runtime.wasm` that the
// harness's expr modules may import.  Bound onto the linker by
// `InstantiateRuntime` after instantiation — mirrors the
// "always link the runtime fully" rule from CLAUDE.md
// (api/engine.cc::Engine::Plan does the same).  Append-only as
// the runtime grows; dropping a name silently breaks WATs that
// rely on it, which is the point.
constexpr std::array<absl::string_view, 115> kRuntimeExports = {
    // M1 baseline.
    "arena_reset",
    "arena_alloc",
    // M3: map runtime helpers.
    "cel_map_create",
    "cel_map_insert",
    "cel_map_insert_at",
    "cel_map_insert_at_if_bool",
    "cel_map_lookup_arena",
    "cel_map_lookup",
    // List runtime helpers.
    "cel_list_create",
    "cel_list_append_at",
    "cel_list_append_at_if_bool",
    "cel_list_at_arena",
    "cel_list_at",
    // arithmetic helpers.
    "cel_int_add_at_vv",
    "cel_int_sub_at_vv",
    "cel_int_mul_at_vv",
    "cel_int_div_at_vv",
    "cel_int_mod_at_vv",
    "cel_int_neg_at_v",
    "cel_uint_add_at_vv",
    "cel_uint_sub_at_vv",
    "cel_uint_mul_at_vv",
    "cel_uint_div_at_vv",
    "cel_uint_mod_at_vv",
    "cel_double_add_at_vv",
    "cel_double_sub_at_vv",
    "cel_double_mul_at_vv",
    "cel_double_div_at_vv",
    "cel_double_neg_at_v",
    // comparison helpers.
    "cel_int_eq_at_vv",
    "cel_int_ne_at_vv",
    "cel_int_lt_at_vv",
    "cel_int_le_at_vv",
    "cel_int_gt_at_vv",
    "cel_int_ge_at_vv",
    "cel_uint_eq_at_vv",
    "cel_uint_ne_at_vv",
    "cel_uint_lt_at_vv",
    "cel_uint_le_at_vv",
    "cel_uint_gt_at_vv",
    "cel_uint_ge_at_vv",
    "cel_double_eq_at_vv",
    "cel_double_ne_at_vv",
    "cel_double_lt_at_vv",
    "cel_double_le_at_vv",
    "cel_double_gt_at_vv",
    "cel_double_ge_at_vv",
    "cel_bool_eq_at_vv",
    "cel_bool_ne_at_vv",
    "cel_bool_lt_at_vv",
    "cel_bool_le_at_vv",
    "cel_bool_gt_at_vv",
    "cel_bool_ge_at_vv",
    "cel_null_eq_at_vv",
    // cross-type numeric ladder.
    "cel_numeric_eq_at_vv",
    "cel_numeric_ne_at_vv",
    "cel_numeric_lt_at_vv",
    "cel_numeric_le_at_vv",
    "cel_numeric_gt_at_vv",
    "cel_numeric_ge_at_vv",
    // string + bytes ops.
    "cel_string_concat_at_vv",
    "cel_string_size_at_v",
    "cel_string_eq_at_vv",
    "cel_string_lt_at_vv",
    "cel_string_le_at_vv",
    "cel_string_gt_at_vv",
    "cel_string_ge_at_vv",
    "cel_string_contains_at_vv",
    "cel_string_starts_with_at_vv",
    "cel_string_ends_with_at_vv",
    "cel_bytes_concat_at_vv",
    "cel_bytes_size_at_v",
    "cel_bytes_eq_at_vv",
    "cel_bytes_lt_at_vv",
    "cel_bytes_le_at_vv",
    "cel_bytes_gt_at_vv",
    "cel_bytes_ge_at_vv",
    // aggregate arena fast paths.
    "cel_list_size_arena",
    "cel_list_in_arena",
    "cel_list_eq_arena",
    "cel_list_concat_arena",
    "cel_map_size_arena",
    "cel_map_in_arena",
    "cel_map_eq_arena",
    // aggregate kDynamic dispatchers.
    "cel_list_size",
    "cel_list_in",
    "cel_list_eq",
    "cel_list_concat",
    "cel_map_size",
    "cel_map_in",
    "cel_map_eq",
    // map-key iteration helpers (used by WAT
    // `64_comprehension_exists_map.wat` and downstream slice-F /
    // slice-G map-source comprehensions).
    "cel_map_iter_init",
    "cel_map_iter_next",
    "cel_map_iter_key_at",
    "cel_map_iter_value_at",
    // polymorphic equality.
    "cel_equals_at_vv",
    "cel_not_equals_at_vv",
    // 3VL / control-flow helpers.
    "cel_and",
    "cel_or",
    "cel_not",
    "cel_unknown_merge",
    "cel_copy_slot",
    // optional<T> runtime kernels — exported from cel_runtime.wasm
    // (see runtime/BUILD.bazel's optional --export lines).
    "cel_optional_none_at",
    "cel_optional_of_at_v",
    "cel_optional_of_non_zero_at_v",
    "cel_optional_has_value_at_v",
    "cel_optional_value_at_v",
    "cel_optional_or_at_vv",
    "cel_optional_or_value_at_vv",
    "cel_select_optional_field_at_vv",
    "cel_map_insert_at_if_present",
    "cel_list_append_at_if_present",
    "cel_set_field_at_if_present",
    // M17 encoders (base64) kernels — exported from cel_runtime.wasm
    // (see runtime/BUILD.bazel's cel_base64_* --export lines).  Bound
    // to the real wasm exports (not stubs); arity comes from the
    // export signature.  See wat/m17_base64_{encode,decode}.wat.
    "cel_base64_encode_at_v",
    "cel_base64_decode_at_v",
};

// ── Status helpers — mirror celwasm::Engine's shape ─────────────

absl::Status WasmtimeErrorToStatus(absl::string_view context,
                                   wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string text(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return absl::FailedPreconditionError(absl::StrCat(context, ": ", text));
}

absl::Status WasmTrapToStatus(absl::string_view context, wasm_trap_t* trap) {
  wasm_byte_vec_t msg;
  wasm_trap_message(trap, &msg);
  std::string text(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return absl::InternalError(absl::StrCat(context, ": ", text));
}

// ── Trampoline state: holds a CelHostStub + a way to reach the
// caller's memory so the stub can write into `out_slot`. ─────

struct StubEnv {
  CelHostStub stub;
};

wasm_trap_t* StubTrampoline(void* env, wasmtime_caller_t* caller,
                            const wasmtime_val_t* args, size_t nargs,
                            wasmtime_val_t* /*results*/, size_t /*nresults*/) {
  if (nargs != 4 || args[0].kind != WASMTIME_I32 ||
      args[1].kind != WASMTIME_I32 || args[2].kind != WASMTIME_I32 ||
      args[3].kind != WASMTIME_I32) {
    // Stub trampoline expects the 4-arg shape exactly.  Any
    // mismatch is a programming error in the calling WAT, not a
    // runtime failure we should paper over.
    wasm_byte_vec_t msg;
    const char kMsg[] = "wat_runner stub: expected 4 × i32 args";
    wasm_byte_vec_new(&msg, sizeof(kMsg) - 1, kMsg);
    wasm_trap_t* t = wasm_trap_new(nullptr, &msg);
    wasm_byte_vec_delete(&msg);
    return t;
  }
  const auto out_slot = static_cast<uint32_t>(args[0].of.i32);
  const auto msg_slot = static_cast<uint32_t>(args[1].of.i32);
  const auto field_ref_id = static_cast<uint32_t>(args[2].of.i32);
  const auto attribute_id = static_cast<uint32_t>(args[3].of.i32);

  // Reach the caller's (shared) memory through its `memory` export.
  wasmtime_extern_t ext;
  const char kName[] = "memory";
  if (!wasmtime_caller_export_get(caller, kName, sizeof(kName) - 1, &ext)) {
    wasm_byte_vec_t msg;
    const char kMsg[] = "wat_runner stub: caller has no memory export";
    wasm_byte_vec_new(&msg, sizeof(kMsg) - 1, kMsg);
    wasm_trap_t* t = wasm_trap_new(nullptr, &msg);
    wasm_byte_vec_delete(&msg);
    return t;
  }
  uint8_t* data = wasmtime_sharedmemory_data(ext.of.sharedmemory);
  const size_t size = wasmtime_sharedmemory_data_size(ext.of.sharedmemory);

  auto* s = static_cast<StubEnv*>(env);
  s->stub(out_slot, msg_slot, field_ref_id, attribute_id, data, size);
  return nullptr;
}

void DeleteStubEnv(void* env) {
  delete static_cast<StubEnv*>(env);
}

// No-op 3-i32-in, void-out trampoline.  cel_runtime.wasm has
// `return_call $cel_host.cel_map_lookup` and
// `cel_host.cel_list_at` arms; the runtime won't instantiate
// without these bound.  WAT fixtures don't exercise the kHost
// paths, so a no-op suffices.
wasm_trap_t* NoopCelHostThreeArg(void*, wasmtime_caller_t*,
                                 const wasmtime_val_t*, size_t, wasmtime_val_t*,
                                 size_t) {
  return nullptr;
}

wasm_functype_t* HostThreeArgTrampolineType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* param_arr[3];
  for (auto& p : param_arr) {
    p = wasm_valtype_new(WASM_I32);
  }
  wasm_valtype_vec_new(&params, 3, param_arr);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

wasm_functype_t* HostTwoArgTrampolineType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* param_arr[2];
  for (auto& p : param_arr) {
    p = wasm_valtype_new(WASM_I32);
  }
  wasm_valtype_vec_new(&params, 2, param_arr);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

absl::Status RegisterCelHostThreeArgNoop(wasmtime_linker_t* linker,
                                         absl::string_view name) {
  wasm_functype_t* type = HostThreeArgTrampolineType();
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, "cel_host", 8, name.data(), name.size(), type,
      NoopCelHostThreeArg, nullptr, nullptr);
  wasm_functype_delete(type);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(
        absl::StrCat("linker.define(cel_host.", name, ")"), err);
  }
  return absl::OkStatus();
}

absl::Status RegisterCelHostTwoArgNoop(wasmtime_linker_t* linker,
                                       absl::string_view name) {
  wasm_functype_t* type = HostTwoArgTrampolineType();
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, "cel_host", 8, name.data(), name.size(), type,
      NoopCelHostThreeArg, nullptr, nullptr);
  wasm_functype_delete(type);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(
        absl::StrCat("linker.define(cel_host.", name, ")"), err);
  }
  return absl::OkStatus();
}

// Four-i32-in, zero-out — the M2 cel_host trampoline signature.
wasm_functype_t* CelHostTrampolineType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* param_arr[4];
  for (auto& p : param_arr) {
    p = wasm_valtype_new(WASM_I32);
  }
  wasm_valtype_vec_new(&params, 4, param_arr);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

absl::Status RegisterCelHostStub(wasmtime_linker_t* linker,
                                 absl::string_view name, CelHostStub stub) {
  auto* env = new StubEnv{std::move(stub)};
  wasm_functype_t* type = CelHostTrampolineType();
  const char kModule[] = "cel_host";
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, kModule, sizeof(kModule) - 1, name.data(), name.size(), type,
      StubTrampoline, env, DeleteStubEnv);
  wasm_functype_delete(type);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(
        absl::StrCat("linker.define(cel_host.", name, ")"), err);
  }
  return absl::OkStatus();
}

// 3-arg stub trampoline state — mirrors StubEnv for the 4-arg case.
struct ThreeArgStubEnv {
  CelHostThreeArgStub stub;
};

void DeleteThreeArgStubEnv(void* p) {
  delete static_cast<ThreeArgStubEnv*>(p);
}

wasm_trap_t* ThreeArgStubTrampoline(void* env, wasmtime_caller_t* caller,
                                    const wasmtime_val_t* args, size_t nargs,
                                    wasmtime_val_t* /*results*/,
                                    size_t /*nresults*/) {
  if (nargs != 3 || args[0].kind != WASMTIME_I32 ||
      args[1].kind != WASMTIME_I32 || args[2].kind != WASMTIME_I32) {
    wasm_byte_vec_t msg;
    const char kMsg[] = "wat_runner 3-arg stub: expected 3 × i32 args";
    wasm_byte_vec_new(&msg, sizeof(kMsg) - 1, kMsg);
    wasm_trap_t* t = wasm_trap_new(nullptr, &msg);
    wasm_byte_vec_delete(&msg);
    return t;
  }
  const auto out_slot = static_cast<uint32_t>(args[0].of.i32);
  const auto operand_slot = static_cast<uint32_t>(args[1].of.i32);
  const auto key_slot = static_cast<uint32_t>(args[2].of.i32);

  wasmtime_extern_t ext;
  const char kName[] = "memory";
  if (!wasmtime_caller_export_get(caller, kName, sizeof(kName) - 1, &ext)) {
    wasm_byte_vec_t msg;
    const char kMsg[] = "wat_runner 3-arg stub: caller has no memory export";
    wasm_byte_vec_new(&msg, sizeof(kMsg) - 1, kMsg);
    wasm_trap_t* t = wasm_trap_new(nullptr, &msg);
    wasm_byte_vec_delete(&msg);
    return t;
  }
  uint8_t* base = wasmtime_sharedmemory_data(ext.of.sharedmemory);
  size_t size = wasmtime_sharedmemory_data_size(ext.of.sharedmemory);

  static_cast<ThreeArgStubEnv*>(env)->stub(out_slot, operand_slot, key_slot,
                                           base, size);
  return nullptr;
}

absl::Status RegisterCelHostThreeArgStub(wasmtime_linker_t* linker,
                                         absl::string_view name,
                                         CelHostThreeArgStub stub) {
  auto* env = new ThreeArgStubEnv{std::move(stub)};
  wasm_functype_t* type = HostThreeArgTrampolineType();
  const char kModule[] = "cel_host";
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, kModule, sizeof(kModule) - 1, name.data(), name.size(), type,
      ThreeArgStubTrampoline, env, DeleteThreeArgStubEnv);
  wasm_functype_delete(type);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(
        absl::StrCat("linker.define(cel_host.", name, ")"), err);
  }
  return absl::OkStatus();
}

// ── WAT → wasm bytes ─────────────────────────────────────

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

// ── Store + memory + linker wiring ──────────────────────

struct RunState {
  wasm_engine_t* engine = nullptr;
  wasmtime_module_t* runtime_module = nullptr;
  wasmtime_module_t* expr_module = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_linker_t* linker = nullptr;
  // Phase C: the runtime defines + exports a SHARED memory; the
  // harness clones the handle in `InstantiateRuntime`.  Owned here.
  wasmtime_sharedmemory_t* memory = nullptr;
  wasmtime_instance_t runtime_instance{};
  wasmtime_instance_t expr_instance{};
  wasmtime_func_t eval_fn{};

  ~RunState() {
    if (memory != nullptr) wasmtime_sharedmemory_delete(memory);
    if (linker != nullptr) wasmtime_linker_delete(linker);
    if (store != nullptr) wasmtime_store_delete(store);
    if (expr_module != nullptr) wasmtime_module_delete(expr_module);
    if (runtime_module != nullptr) wasmtime_module_delete(runtime_module);
    if (engine != nullptr) wasm_engine_delete(engine);
  }
};

absl::Status InitEngineAndModules(RunState& s,
                                  absl::Span<const uint8_t> expr_bytes) {
  // cel_runtime.wasm's aggregate-op dispatchers use `return_call`;
  // the engine must opt in to the wasm tail-call feature to
  // instantiate it.  Mirrors api/engine.cc.
  wasm_config_t* config = wasm_config_new();
  if (config == nullptr) {
    return absl::InternalError("wasm_config_new returned null");
  }
  wasmtime_config_wasm_tail_call_set(config, true);
  // Phase C: cel_runtime.wasm is built against wasm32-wasi-threads —
  // it declares its memory as shared and may import threading
  // primitives.  Both require the threads proposal + shared-memory
  // support enabled here (mirrors api/engine.cc::InitWasmtime).
  wasmtime_config_wasm_threads_set(config, true);
  wasmtime_config_shared_memory_set(config, true);
  s.engine = wasm_engine_new_with_config(config);
  if (s.engine == nullptr) {
    return absl::InternalError("wasm_engine_new_with_config returned null");
  }
  wasmtime_error_t* err =
      wasmtime_module_new(s.engine, kCelRuntimeWasmBytes,
                          kCelRuntimeWasmBytesSize, &s.runtime_module);
  if (err != nullptr) return WasmtimeErrorToStatus("module_new(runtime)", err);
  err = wasmtime_module_new(s.engine, expr_bytes.data(), expr_bytes.size(),
                            &s.expr_module);
  if (err != nullptr) return WasmtimeErrorToStatus("module_new(expr)", err);
  return absl::OkStatus();
}

// Phase C: the runtime (cel_runtime.wasm) defines and exports its own
// shared memory `(memory 4 1024 shared)`.  The harness adopts that
// memory as `cel.memory` after instantiating the runtime (see
// `InstantiateRuntime`), so this only has to create the store.
// (Pre-Phase-C the harness created a private non-shared memory here
// and the runtime imported it; that produced two disjoint memories
// once the runtime started owning its own.)
absl::Status InitStore(RunState& s) {
  s.store = wasmtime_store_new(s.engine, nullptr, nullptr);
  if (s.store == nullptr) {
    return absl::InternalError("wasmtime_store_new returned null");
  }
  // Phase C: cel_runtime.wasm links absl + cctz, which pull in
  // wasi-libc functions referencing env / stdio.  Hand wasmtime a
  // minimal wasi_config so the imports defined via
  // `wasmtime_linker_define_wasi` resolve into a sandboxed impl
  // (mirrors api/engine.cc::InitStore).
  wasi_config_t* wasi = wasi_config_new();
  if (wasi == nullptr) {
    return absl::InternalError("wasi_config_new returned null");
  }
  wasmtime_context_t* ctx = wasmtime_store_context(s.store);
  // `wasmtime_context_set_wasi` takes ownership of `wasi`.
  if (wasmtime_error_t* err = wasmtime_context_set_wasi(ctx, wasi);
      err != nullptr) {
    return WasmtimeErrorToStatus("context_set_wasi", err);
  }
  return absl::OkStatus();
}

// Register the optional 4-arg cel_host stubs (cel_get_field /
// cel_has_field).  Pulling these out of InitLinker to keep its
// per-function footprint under the lint gate.
absl::Status RegisterCelHostFourArgStubs(wasmtime_linker_t* linker,
                                         const WatRunInput& input) {
  if (input.cel_get_field_stub) {
    if (auto st = RegisterCelHostStub(linker, "cel_get_field",
                                      input.cel_get_field_stub);
        !st.ok()) {
      return st;
    }
  }
  if (input.cel_has_field_stub) {
    if (auto st = RegisterCelHostStub(linker, "cel_has_field",
                                      input.cel_has_field_stub);
        !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

// Register one optional 3-arg cel_host trampoline by name: if the
// caller supplied a stub, route the trampoline through it; otherwise
// bind a no-op so the WAT instantiates even if it never calls the
// surface.  Pulled out of `RegisterCelHostThreeArgTrampolines` so
// adding a new optional trampoline doesn't push the parent function
// past the readability-function-size threshold.
absl::Status RegisterOptionalThreeArg(wasmtime_linker_t* linker,
                                      absl::string_view name,
                                      const CelHostThreeArgStub& stub) {
  if (stub) {
    return RegisterCelHostThreeArgStub(linker, name, stub);
  }
  return RegisterCelHostThreeArgNoop(linker, name);
}

// Bulk no-op binds for cel_host imports that WATs reference but
// don't exercise — the aggregate-op kHost dispatchers,
// `cel_set_field`, `cel_make_message`, and
// `resolve_message_type_name`.  Each surface ships with a
// real production trampoline; tests that need real semantics route
// through the full Compiler/Engine pipeline, not this harness.
// Factored out of `RegisterCelHostThreeArgTrampolines` to keep the
// parent function under the readability-function-size threshold.
absl::Status RegisterCelHostBulkNoopImports(wasmtime_linker_t* linker) {
  // Aggregate-op kHost imports.  Tests link the dispatchers but
  // don't exercise the host arms; no-op stubs suffice.
  static constexpr absl::string_view kThreeArg[] = {
      "cel_list_in", "cel_list_eq", "cel_list_concat",
      "cel_map_in",  "cel_map_eq",  "cel_message_eq",
  };
  for (absl::string_view name : kThreeArg) {
    if (auto st = RegisterCelHostThreeArgNoop(linker, name); !st.ok()) {
      return st;
    }
  }
  static constexpr absl::string_view kTwoArg[] = {
      "cel_list_size",
      "cel_map_size",
      // `cel_make_message(type_id, out_slot)`.
      "cel_make_message",
      // `resolve_message_type_name(out_slot, in_slot)`.
      "resolve_message_type_name",
      // Comprehension-source iter-snapshot trampolines (m5b §CCF-8):
      // `cel_{list,map}_iter_open(out_slot, src_slot)`.  Tests that
      // link the dispatchers but iterate only arena sources never
      // call the host arm; no-op stubs let the runtime instantiate.
      "cel_list_iter_open",
      "cel_map_iter_open",
  };
  for (absl::string_view name : kTwoArg) {
    if (auto st = RegisterCelHostTwoArgNoop(linker, name); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

// Register the 3-arg cel_host trampolines (cel_map_lookup /
// cel_list_at / cel_wkt_unwrap_wrapper).  Caller may supply a stub
// to simulate host-table dispatch (kHost-path tests) / wrapper
// peel; otherwise a no-op binds so kArena WATs that link
// the cel_host imports but never call them still instantiate.
absl::Status RegisterCelHostThreeArgTrampolines(wasmtime_linker_t* linker,
                                                const WatRunInput& input) {
  if (auto st = RegisterOptionalThreeArg(linker, "cel_map_lookup",
                                         input.cel_host_cel_map_lookup_stub);
      !st.ok()) {
    return st;
  }
  if (auto st = RegisterOptionalThreeArg(linker, "cel_list_at",
                                         input.cel_host_cel_list_at_stub);
      !st.ok()) {
    return st;
  }
  // `cel_host.cel_wkt_unwrap_wrapper(out_slot, msg_slot,
  // wrapper_kind)`.  Same 3-i32-in / void-out shape as the
  // cel_map_lookup / cel_list_at trampolines above.  Stubs interpret the third arg as
  // `wrapper_kind` (CelKind tag: 1=BOOL, 2=INT, 3=UINT, 4=DOUBLE,
  // 5=STRING, 6=BYTES), NOT the CelHostThreeArgStub-default
  // "key_or_index_slot".
  if (auto st =
          RegisterOptionalThreeArg(linker, "cel_wkt_unwrap_wrapper",
                                   input.cel_host_cel_wkt_unwrap_wrapper_stub);
      !st.ok()) {
    return st;
  }
  // `cel_host.cel_set_field(msg_slot, field_ref_id, value_slot)` — the
  // Some-path delegation of `cel_set_field_at_if_present`.  Overridable
  // so the proto-set-field-if-present WAT can assert the kernel wrote
  // the field (and verify the None-path short-circuit leaves it unset).
  if (auto st = RegisterOptionalThreeArg(linker, "cel_set_field",
                                         input.cel_host_cel_set_field_stub);
      !st.ok()) {
    return st;
  }
  return RegisterCelHostBulkNoopImports(linker);
}

// Bind no-op fallbacks for M7B (duration/timestamp) imports that
// the WAT traces under doc/.../wat/50-55 declare but whose real
// impls land in M7B.B–E.  Kernel kernels live in module "cel"
// (alongside cel_int_add_at_vv etc.); host trampolines in module
// "cel_host".  As each kernel/trampoline ships, the corresponding
// entry should move from this no-op table into either
// `kRuntimeExports` (for cel.* kernels exported by
// cel_runtime.wasm) or a real stub-registration call (for cel_host.*
// trampolines).
//
// Without these, the M7B WATs fail to instantiate via wat_runner
// (the imports resolve to nothing).  With them, the WATs round-trip
// through `wasm-as` AND through wasmtime instantiation; callers that
// want end-to-end execution against a meaningful result wire a stub
// at the call site.
// Bind a list of named no-op trampolines under the `cel` module on
// the given linker, using `functype_factory` to build the per-name
// wasm_functype_t.  Factored out of `RegisterPendingM7BImports` to
// keep that parent function under the readability-function-size
// threshold.
absl::Status RegisterCelModuleNoopImports(
    wasmtime_linker_t* linker, absl::Span<const absl::string_view> names,
    wasm_functype_t* (*functype_factory)()) {
  for (absl::string_view name : names) {
    wasm_functype_t* type = functype_factory();
    wasmtime_error_t* err = wasmtime_linker_define_func(
        linker, "cel", 3, name.data(), name.size(), type, NoopCelHostThreeArg,
        nullptr, nullptr);
    wasm_functype_delete(type);
    if (err != nullptr) {
      return WasmtimeErrorToStatus(
          absl::StrCat("linker.define(cel.", name, ")"), err);
    }
  }
  return absl::OkStatus();
}

absl::Status RegisterPendingM7BImports(wasmtime_linker_t* linker) {
  // 3-arg `cel.cel_*_at_vv` kernels (M7B.B arithmetic + ordering).
  static constexpr absl::string_view kCelThreeArg[] = {
      "cel_dur_add_at_vv",
      "cel_ts_ts_sub_at_vv",
  };
  if (auto st = RegisterCelModuleNoopImports(linker, kCelThreeArg,
                                             HostThreeArgTrampolineType);
      !st.ok()) {
    return st;
  }
  // 2-arg `cel.cel_ts_*_utc` accessor kernels (M7B.C).  This is the
  // representative for all 14 accessor helpers — see
  // `wat/51_timestamp_year_utc.wat` header for the matrix.
  static constexpr absl::string_view kCelTwoArg[] = {
      "cel_ts_year_utc",
  };
  if (auto st = RegisterCelModuleNoopImports(linker, kCelTwoArg,
                                             HostTwoArgTrampolineType);
      !st.ok()) {
    return st;
  }
  // 2-arg `cel_host.*` parse/format trampolines (M7B.D).  Each WAT
  // assumes `(out_slot, in_slot) -> ()`.  Production Layer-2 impls
  // land in `eval/internal/cel_host.cc`.
  static constexpr absl::string_view kCelHostTwoArg[] = {
      "cel_timestamp_parse",
      "cel_duration_parse",
      "cel_timestamp_format",
      "cel_duration_format",
  };
  for (absl::string_view name : kCelHostTwoArg) {
    if (auto st = RegisterCelHostTwoArgNoop(linker, name); !st.ok()) {
      return st;
    }
  }
  // 4-arg `cel_host.cel_timestamp_tz_accessor` dispatch trampoline
  // (M7B.E).  Same wire shape as the M2 `cel_host.cel_get_field`
  // trampoline (`(i32, i32, i32, i32) -> ()`).  Binding a no-op
  // here keeps WATs that import this name instantiable; production
  // Layer-2 impl lives in cel_host.cc once M7B.E ships.
  {
    wasm_functype_t* type = CelHostTrampolineType();
    wasmtime_error_t* err = wasmtime_linker_define_func(
        linker, "cel_host", 8, "cel_timestamp_tz_accessor",
        sizeof("cel_timestamp_tz_accessor") - 1, type, NoopCelHostThreeArg,
        nullptr, nullptr);
    wasm_functype_delete(type);
    if (err != nullptr) {
      return WasmtimeErrorToStatus(
          "linker.define(cel_host.cel_timestamp_tz_accessor)", err);
    }
  }
  return absl::OkStatus();
}

absl::Status InitLinker(RunState& s, const WatRunInput& input) {
  s.linker = wasmtime_linker_new(s.engine);
  if (s.linker == nullptr) {
    return absl::InternalError("wasmtime_linker_new returned null");
  }
  // Phase C: cel_runtime.wasm imports wasi_snapshot_preview1.* (absl +
  // cctz pull in ~10 wasi-libc imports).  Define wasmtime's built-in
  // WASI preview1 impl so the runtime instantiates (mirrors
  // api/engine.cc::RegisterWasiStubs).
  if (wasmtime_error_t* err = wasmtime_linker_define_wasi(s.linker);
      err != nullptr) {
    return WasmtimeErrorToStatus("linker.define_wasi", err);
  }
  if (auto st = RegisterCelLog(s.linker); !st.ok()) return st;
  if (auto st = RegisterCelHostThreeArgTrampolines(s.linker, input); !st.ok()) {
    return st;
  }
  if (auto st = RegisterPendingM7BImports(s.linker); !st.ok()) return st;
  // `cel.memory` is NOT bound here: the runtime defines + exports it,
  // and `InstantiateRuntime` adopts that export onto the linker.
  return RegisterCelHostFourArgStubs(s.linker, input);
}

absl::Status BindExport(wasmtime_linker_t* linker, wasmtime_context_t* ctx,
                        const wasmtime_instance_t& inst,
                        absl::string_view name) {
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &inst, name.data(), name.size(),
                                    &ext)) {
    return absl::FailedPreconditionError(
        absl::StrCat("runtime has no export `", name, "`"));
  }
  wasmtime_error_t* err = wasmtime_linker_define(
      linker, ctx, "cel", 3, name.data(), name.size(), &ext);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(absl::StrCat("linker.define(cel.", name, ")"),
                                 err);
  }
  return absl::OkStatus();
}

// Phase C: adopt the runtime's exported SHARED memory as `cel.memory`
// so the expr module imports the SAME memory the arena helpers operate
// on, and so pre-writes / snapshots observe the live arena.  The
// runtime defines `(memory 4 1024 shared)` and exports it under
// "memory".  Mirrors api/engine.cc::BindRuntimeMemory.
absl::Status AdoptRuntimeMemory(RunState& s, wasmtime_context_t* ctx) {
  wasmtime_extern_t mem_ext;
  if (!wasmtime_instance_export_get(ctx, &s.runtime_instance, "memory", 6,
                                    &mem_ext)) {
    return absl::FailedPreconditionError("runtime has no `memory` export");
  }
  if (mem_ext.kind != WASMTIME_EXTERN_SHAREDMEMORY) {
    return absl::FailedPreconditionError(
        "runtime `memory` export is not a shared memory");
  }
  // Clone so this RunState owns a refcounted handle (deleted in dtor);
  // the export's handle is owned by the store.
  s.memory = wasmtime_sharedmemory_clone(mem_ext.of.sharedmemory);
  if (wasmtime_error_t* err = wasmtime_linker_define(s.linker, ctx, "cel", 3,
                                                     "memory", 6, &mem_ext);
      err != nullptr) {
    return WasmtimeErrorToStatus("linker.define(cel.memory)", err);
  }
  return absl::OkStatus();
}

// Phase C: the arena is malloc-backed and seeded once per Instance via
// `arena_init(cap_bytes)` (mirrors api/engine.cc::SeedRuntimeArena).
// WAT `$eval` bodies call the zero-arg `arena_reset()` to rewind the
// cursor but no longer set up the arena base/size themselves.
absl::Status SeedArena(RunState& s, wasmtime_context_t* ctx) {
  wasmtime_extern_t init_ext;
  if (!wasmtime_instance_export_get(ctx, &s.runtime_instance, "arena_init", 10,
                                    &init_ext) ||
      init_ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError("runtime has no `arena_init` func");
  }
  wasmtime_val_t cap;
  cap.kind = WASMTIME_I32;
  cap.of.i32 = static_cast<int32_t>(CELWASM_ARENA_CAPACITY_BYTES);
  wasm_trap_t* init_trap = nullptr;
  if (wasmtime_error_t* ierr = wasmtime_func_call(
          ctx, &init_ext.of.func, &cap, /*nargs=*/1, /*results=*/nullptr,
          /*nresults=*/0, &init_trap);
      ierr != nullptr) {
    return WasmtimeErrorToStatus("arena_init", ierr);
  }
  if (init_trap != nullptr) {
    return WasmTrapToStatus("arena_init trapped", init_trap);
  }
  return absl::OkStatus();
}

absl::Status InstantiateRuntime(RunState& s) {
  wasmtime_context_t* ctx = wasmtime_store_context(s.store);
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_linker_instantiate(
      s.linker, ctx, s.runtime_module, &s.runtime_instance, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("instantiate(runtime)", err);
  if (trap != nullptr) {
    return WasmTrapToStatus("instantiate(runtime) trapped", trap);
  }
  if (auto st = AdoptRuntimeMemory(s, ctx); !st.ok()) return st;
  // Bind every export so any WAT that imports a runtime helper resolves
  // at instantiate time — mirrors the "always link the runtime fully"
  // rule from CLAUDE.md (api/engine.cc::Engine::Plan does the same).
  for (absl::string_view name : kRuntimeExports) {
    if (auto st = BindExport(s.linker, ctx, s.runtime_instance, name);
        !st.ok()) {
      return st;
    }
  }
  return SeedArena(s, ctx);
}

absl::Status InstantiateExpr(RunState& s) {
  wasmtime_context_t* ctx = wasmtime_store_context(s.store);
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_linker_instantiate(
      s.linker, ctx, s.expr_module, &s.expr_instance, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("instantiate(expr)", err);
  if (trap != nullptr) {
    return WasmTrapToStatus("instantiate(expr) trapped", trap);
  }
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &s.expr_instance, "eval", 4, &ext)) {
    return absl::FailedPreconditionError("expr module does not export `eval`");
  }
  if (ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError("`eval` export is not a function");
  }
  s.eval_fn = ext.of.func;
  return absl::OkStatus();
}

// Write the pre-write payloads into the live memory.  Applied AFTER
// instantiation (so the data segments from the expr module have
// already been laid down) but BEFORE $eval is called (so the body's
// arena_reset runs on top of whatever we wrote).
absl::Status ApplyPreWrites(RunState& s, const WatRunInput& input) {
  uint8_t* data = wasmtime_sharedmemory_data(s.memory);
  const size_t size = wasmtime_sharedmemory_data_size(s.memory);
  for (const auto& [offset, bytes] : input.pre_writes) {
    if (static_cast<std::uint64_t>(offset) + bytes.size() > size) {
      return absl::OutOfRangeError(
          absl::StrCat("pre_write [", offset, ", ", offset + bytes.size(),
                       ") exceeds memory size ", size));
    }
    std::memcpy(data + offset, bytes.data(), bytes.size());
  }
  return absl::OkStatus();
}

absl::StatusOr<uint32_t> CallEval(RunState& s) {
  wasmtime_context_t* ctx = wasmtime_store_context(s.store);
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  wasmtime_func_t fn = s.eval_fn;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &fn, /*args=*/nullptr, /*nargs=*/0, &result,
                         /*nresults=*/1, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("eval", err);
  if (trap != nullptr) return WasmTrapToStatus("eval trapped", trap);
  if (result.kind != WASMTIME_I32) {
    return absl::FailedPreconditionError("eval returned non-i32");
  }
  return static_cast<uint32_t>(result.of.i32);
}

std::vector<uint8_t> SnapshotMemory(RunState& s) {
  const uint8_t* data = wasmtime_sharedmemory_data(s.memory);
  const size_t size = wasmtime_sharedmemory_data_size(s.memory);
  return {data, data + size};
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage) — public API, declared in the header.
absl::StatusOr<WatRunOutput> RunWat(const WatRunInput& input) {
  auto expr_bytes_or = Wat2Wasm(input.wat);
  if (!expr_bytes_or.ok()) return expr_bytes_or.status();

  RunState s;
  if (auto st = InitEngineAndModules(s, *expr_bytes_or); !st.ok()) return st;
  if (auto st = InitStore(s); !st.ok()) return st;
  if (auto st = InitLinker(s, input); !st.ok()) return st;
  if (auto st = InstantiateRuntime(s); !st.ok()) return st;
  if (auto st = InstantiateExpr(s); !st.ok()) return st;
  if (auto st = ApplyPreWrites(s, input); !st.ok()) return st;
  auto ret_or = CallEval(s);
  if (!ret_or.ok()) return ret_or.status();

  return WatRunOutput{*ret_or, SnapshotMemory(s)};
}

}  // namespace celwasm
