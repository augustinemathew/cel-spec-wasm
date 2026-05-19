// Locks the runtime null-pointer-elision bug closed.
//
// `cel_memory_base_()` returns address 0 on wasm32 (the imported
// memory is mapped from offset 0).  Without an opacity barrier
// in the C source, clang treats `*(uint32_t*)(0+off) = v` as
// undefined behaviour and elides the store; cel_reset compiled
// to a no-op + cel_log call, arena_alloc compiled to `unreachable`.
// See compiler_v2/runtime/cel_runtime.c::cel_memory_base_().
//
// This test stands up a minimal wasmtime harness that:
//   1. Allocates a host-owned 2-page wasmtime_memory_t.
//   2. Binds it as `cel.memory` on a linker.
//   3. Registers a no-op `cel_env.cel_log`.
//   4. Instantiates cel_runtime.wasm against that linker.
//   5. Calls cel_reset(arena_base, arena_limit) and verifies the
//      bump cursor + limit land at bytes 8 and 12.
//   6. Calls arena_alloc(24) and verifies the returned offset is the
//      pre-call bump value AND the cursor advanced by 24 (rounded
//      up to 8-byte alignment, which 24 already satisfies).
//   7. Calls arena_alloc again to verify the cursor keeps moving.
//
// If any of these regress (e.g. a future clang upgrade re-discovers
// the null-UB elision), this test fails immediately rather than the
// bug staying latent until a downstream caller actually invokes
// cel_reset/arena_alloc on the runtime wasm path.

#include <cstdint>
#include <cstring>
#include <string>

#include "compiler_v2/runtime/cel_runtime_wasm_bytes.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

constexpr uint32_t kArenaBase = 64;
constexpr uint32_t kArenaLimit = 65536;
constexpr uint32_t kBumpOffset = 8;
constexpr uint32_t kLimitOffset = 12;

std::string WasmtimeErrorMsg(wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string text(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return text;
}

std::string WasmTrapMsg(wasm_trap_t* trap) {
  wasm_byte_vec_t msg;
  wasm_trap_message(trap, &msg);
  std::string text(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return text;
}

// No-op cel_env.cel_log trampoline.  cel_runtime.c calls
// CEL_LOG("enter") at the top of cel_reset / arena_alloc; we don't
// care about the output here, just that the import resolves.
wasm_trap_t* NoopCelLog(void*, wasmtime_caller_t*, const wasmtime_val_t*,
                        size_t, wasmtime_val_t*, size_t) {
  return nullptr;
}

// No-op cel_host.cel_map_lookup / cel_host.cel_list_at trampolines.
// M3.C added the kDynamic dispatcher with a `return_call` into the
// map import; M4.C added the list one.  The runtime module won't
// instantiate without these bound, even though these tests don't
// exercise the kHost paths — a no-op suffices.
wasm_trap_t* NoopCelHostThreeArg(void*, wasmtime_caller_t*,
                                 const wasmtime_val_t*, size_t, wasmtime_val_t*,
                                 size_t) {
  return nullptr;
}

wasm_functype_t* CelLogFuncType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* param_arr[9];
  for (auto& p : param_arr) {
    p = wasm_valtype_new(WASM_I32);
  }
  wasm_valtype_vec_new(&params, 9, param_arr);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

wasm_functype_t* HostThreeArgFuncType() {
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

wasm_functype_t* HostTwoArgFuncType() {
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

// Owns the wasmtime state for one instantiated cel_runtime.wasm.
// Public fields so the test can drive cel_reset / arena_alloc and
// peek at memory bytes directly.
struct RuntimeHarness {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_linker_t* linker = nullptr;
  wasmtime_module_t* module = nullptr;
  wasmtime_memory_t memory{};
  wasmtime_instance_t instance{};
  wasmtime_func_t cel_reset_fn{};
  wasmtime_func_t arena_alloc_fn{};

  ~RuntimeHarness() {
    if (module != nullptr) wasmtime_module_delete(module);
    if (linker != nullptr) wasmtime_linker_delete(linker);
    if (store != nullptr) wasmtime_store_delete(store);
    if (engine != nullptr) wasm_engine_delete(engine);
  }
};

// Pulls one i32-returning or void-returning func export off an
// instance and stores its handle.  Reused for cel_reset (void) and
// arena_alloc (i32 result).
::testing::AssertionResult LookupFunc(wasmtime_context_t* ctx,
                                      const wasmtime_instance_t& inst,
                                      const char* name, size_t name_len,
                                      wasmtime_func_t* out) {
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &inst, name, name_len, &ext)) {
    return ::testing::AssertionFailure()
           << "export not found: " << std::string(name, name_len);
  }
  if (ext.kind != WASMTIME_EXTERN_FUNC) {
    return ::testing::AssertionFailure()
           << "export is not a func: " << std::string(name, name_len);
  }
  *out = ext.of.func;
  return ::testing::AssertionSuccess();
}

// Allocates a 2-page host-owned memory in the harness's store.  Two
// pages matches cel_runtime.wasm's `--import-memory` min=2 (per
// runtime/BUILD.bazel).
::testing::AssertionResult InitEngineStoreMemory(RuntimeHarness* h) {
  // M3.C: enable wasm tail-call so the runtime's `cel_map_lookup`
  // dispatcher can `return_call` into the kArena / kHost arms.
  // wasmtime defaults this off; without it the module won't even
  // compile.
  wasm_config_t* config = wasm_config_new();
  if (config == nullptr) {
    return ::testing::AssertionFailure() << "wasm_config_new";
  }
  wasmtime_config_wasm_tail_call_set(config, true);
  h->engine = wasm_engine_new_with_config(config);
  if (h->engine == nullptr) {
    return ::testing::AssertionFailure() << "wasm_engine_new_with_config";
  }
  h->store = wasmtime_store_new(h->engine, nullptr, nullptr);
  if (h->store == nullptr) {
    return ::testing::AssertionFailure() << "wasmtime_store_new";
  }
  wasmtime_context_t* ctx = wasmtime_store_context(h->store);
  wasm_memorytype_t* mty = nullptr;
  if (wasmtime_error_t* err = wasmtime_memorytype_new(
          /*min=*/2, /*max_present=*/true, /*max=*/2, /*is_64=*/false,
          /*shared=*/false, /*page_size_log2=*/16, &mty);
      err != nullptr) {
    return ::testing::AssertionFailure()
           << "memorytype_new: " << WasmtimeErrorMsg(err);
  }
  wasmtime_error_t* err = wasmtime_memory_new(ctx, mty, &h->memory);
  wasm_memorytype_delete(mty);
  if (err != nullptr) {
    return ::testing::AssertionFailure()
           << "memory_new: " << WasmtimeErrorMsg(err);
  }
  return ::testing::AssertionSuccess();
}

// Wires cel_env.cel_log (no-op) and cel.memory (host-owned) onto a
// fresh linker.  Sets `h->linker`.
::testing::AssertionResult InitLinker(RuntimeHarness* h) {
  wasmtime_context_t* ctx = wasmtime_store_context(h->store);
  h->linker = wasmtime_linker_new(h->engine);
  if (h->linker == nullptr) {
    return ::testing::AssertionFailure() << "wasmtime_linker_new";
  }
  wasm_functype_t* ft = CelLogFuncType();
  wasmtime_error_t* err = wasmtime_linker_define_func(
      h->linker, "cel_env", 7, "cel_log", 7, ft, NoopCelLog,
      /*data=*/nullptr, /*finalizer=*/nullptr);
  wasm_functype_delete(ft);
  if (err != nullptr) {
    return ::testing::AssertionFailure()
           << "define cel_env.cel_log: " << WasmtimeErrorMsg(err);
  }
  wasm_functype_t* mlft = HostThreeArgFuncType();
  err = wasmtime_linker_define_func(h->linker, "cel_host", 8, "cel_map_lookup",
                                    14, mlft, NoopCelHostThreeArg,
                                    /*data=*/nullptr, /*finalizer=*/nullptr);
  wasm_functype_delete(mlft);
  if (err != nullptr) {
    return ::testing::AssertionFailure()
           << "define cel_host.cel_map_lookup: " << WasmtimeErrorMsg(err);
  }
  // M4.C: same shape for `cel_host.cel_list_at`.
  wasm_functype_t* llft = HostThreeArgFuncType();
  err = wasmtime_linker_define_func(h->linker, "cel_host", 8, "cel_list_at", 11,
                                    llft, NoopCelHostThreeArg,
                                    /*data=*/nullptr, /*finalizer=*/nullptr);
  wasm_functype_delete(llft);
  if (err != nullptr) {
    return ::testing::AssertionFailure()
           << "define cel_host.cel_list_at: " << WasmtimeErrorMsg(err);
  }
  // M5.D step 2: aggregate-op kHost imports.  size helpers are
  // 2-arg, in/eq/concat (and cel_message_eq) are 3-arg.  Tests
  // here don't exercise the kHost path; no-op stubs suffice.
  struct Entry {
    const char* name;
    size_t name_len;
    int arity;
  };
  static const Entry kEntries[] = {
      {"cel_list_size", 13, 2},
      {"cel_list_in", 11, 3},
      {"cel_list_eq", 11, 3},
      {"cel_list_concat", 15, 3},
      {"cel_map_size", 12, 2},
      {"cel_map_in", 10, 3},
      {"cel_map_eq", 10, 3},
      {"cel_message_eq", 14, 3},
      // M9.B: `type(message)` descriptor-FQN resolver.  Tests here
      // don't exercise the CEL_MESSAGE arm of `cel_type_of_at_v`,
      // but the wasm module imports the symbol unconditionally so
      // the stub still has to land.
      {"resolve_message_type_name", 25, 2},
  };
  for (const auto& e : kEntries) {
    wasm_functype_t* ft =
        e.arity == 2 ? HostTwoArgFuncType() : HostThreeArgFuncType();
    err = wasmtime_linker_define_func(h->linker, "cel_host", 8, e.name,
                                      e.name_len, ft, NoopCelHostThreeArg,
                                      /*data=*/nullptr, /*finalizer=*/nullptr);
    wasm_functype_delete(ft);
    if (err != nullptr) {
      return ::testing::AssertionFailure()
             << "define cel_host." << e.name << ": " << WasmtimeErrorMsg(err);
    }
  }
  wasmtime_extern_t ext;
  ext.kind = WASMTIME_EXTERN_MEMORY;
  ext.of.memory = h->memory;
  err = wasmtime_linker_define(h->linker, ctx, "cel", 3, "memory", 6, &ext);
  if (err != nullptr) {
    return ::testing::AssertionFailure()
           << "define cel.memory: " << WasmtimeErrorMsg(err);
  }
  return ::testing::AssertionSuccess();
}

// Compiles + instantiates cel_runtime.wasm and pulls the
// cel_reset / arena_alloc func handles.  Linker must already have
// cel_env.cel_log + cel.memory bound (InitLinker did that).
::testing::AssertionResult InstantiateRuntime(RuntimeHarness* h) {
  wasmtime_context_t* ctx = wasmtime_store_context(h->store);
  if (wasmtime_error_t* err =
          wasmtime_module_new(h->engine, kCelRuntimeWasmBytes,
                              kCelRuntimeWasmBytesSize, &h->module);
      err != nullptr) {
    return ::testing::AssertionFailure()
           << "module_new(runtime): " << WasmtimeErrorMsg(err);
  }
  wasm_trap_t* trap = nullptr;
  if (wasmtime_error_t* err = wasmtime_linker_instantiate(
          h->linker, ctx, h->module, &h->instance, &trap);
      err != nullptr) {
    return ::testing::AssertionFailure()
           << "instantiate: " << WasmtimeErrorMsg(err);
  }
  if (trap != nullptr) {
    return ::testing::AssertionFailure()
           << "instantiate trapped: " << WasmTrapMsg(trap);
  }
  if (auto r = LookupFunc(ctx, h->instance, "cel_reset", 9, &h->cel_reset_fn);
      !r) {
    return r;
  }
  return LookupFunc(ctx, h->instance, "arena_alloc", 9, &h->arena_alloc_fn);
}

// Top-level harness builder.  Three steps, each its own helper to
// stay under the 60-line function-size lint.
::testing::AssertionResult BuildHarness(RuntimeHarness* h) {
  if (auto r = InitEngineStoreMemory(h); !r) return r;
  if (auto r = InitLinker(h); !r) return r;
  return InstantiateRuntime(h);
}

uint32_t CelAllocCall(const RuntimeHarness& h, uint32_t n) {
  wasmtime_context_t* ctx = wasmtime_store_context(h.store);
  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = static_cast<int32_t>(n);
  wasmtime_val_t result;
  wasm_trap_t* trap = nullptr;
  wasmtime_func_t fn = h.arena_alloc_fn;
  wasmtime_error_t* err = wasmtime_func_call(ctx, &fn, &arg, /*nargs=*/1,
                                             &result, /*nresults=*/1, &trap);
  if (err != nullptr) {
    ADD_FAILURE() << "arena_alloc trampoline error: " << WasmtimeErrorMsg(err);
    return 0;
  }
  if (trap != nullptr) {
    ADD_FAILURE() << "arena_alloc trapped: " << WasmTrapMsg(trap);
    return 0;
  }
  EXPECT_EQ(result.kind, WASMTIME_I32);
  return static_cast<uint32_t>(result.of.i32);
}

void CelResetCall(const RuntimeHarness& h, uint32_t base, uint32_t limit) {
  wasmtime_context_t* ctx = wasmtime_store_context(h.store);
  wasmtime_val_t args[2];
  args[0].kind = WASMTIME_I32;
  args[0].of.i32 = static_cast<int32_t>(base);
  args[1].kind = WASMTIME_I32;
  args[1].of.i32 = static_cast<int32_t>(limit);
  wasm_trap_t* trap = nullptr;
  wasmtime_func_t fn = h.cel_reset_fn;
  wasmtime_error_t* err = wasmtime_func_call(ctx, &fn, args, /*nargs=*/2,
                                             /*results=*/nullptr,
                                             /*nresults=*/0, &trap);
  if (err != nullptr) {
    ADD_FAILURE() << "cel_reset trampoline error: " << WasmtimeErrorMsg(err);
    return;
  }
  if (trap != nullptr) {
    ADD_FAILURE() << "cel_reset trapped: " << WasmTrapMsg(trap);
  }
}

uint32_t ReadU32(const RuntimeHarness& h, uint32_t off) {
  wasmtime_context_t* ctx = wasmtime_store_context(h.store);
  wasmtime_memory_t mem = h.memory;
  const uint8_t* base = wasmtime_memory_data(ctx, &mem);
  uint32_t v = 0;
  std::memcpy(&v, base + off, sizeof(v));
  return v;
}

// ————————————— Tests —————————————

TEST(CelRuntimeWasmTest, CelResetWritesArenaCursorAndLimit) {
  RuntimeHarness h;
  ASSERT_TRUE(BuildHarness(&h));
  CelResetCall(h, kArenaBase, kArenaLimit);
  EXPECT_EQ(ReadU32(h, kBumpOffset), kArenaBase);
  EXPECT_EQ(ReadU32(h, kLimitOffset), kArenaLimit);
}

TEST(CelRuntimeWasmTest, CelAllocReturnsCursorAndAdvances) {
  RuntimeHarness h;
  ASSERT_TRUE(BuildHarness(&h));
  CelResetCall(h, kArenaBase, kArenaLimit);

  const uint32_t off1 = CelAllocCall(h, 24);
  EXPECT_EQ(off1, kArenaBase);
  EXPECT_EQ(ReadU32(h, kBumpOffset), kArenaBase + 24);

  // Second alloc should pick up where the first left off.
  const uint32_t off2 = CelAllocCall(h, 16);
  EXPECT_EQ(off2, kArenaBase + 24);
  EXPECT_EQ(ReadU32(h, kBumpOffset), kArenaBase + 24 + 16);
}

TEST(CelRuntimeWasmTest, CelAllocReturnsZeroWhenLimitExceeded) {
  RuntimeHarness h;
  ASSERT_TRUE(BuildHarness(&h));
  // Tiny arena: 64..72 (8 bytes available).  alloc(16) should fail.
  CelResetCall(h, /*base=*/64, /*limit=*/72);
  EXPECT_EQ(CelAllocCall(h, 16), 0u);
  // Cursor unchanged on failure.
  EXPECT_EQ(ReadU32(h, kBumpOffset), 64u);
}

}  // namespace
}  // namespace celwasm
