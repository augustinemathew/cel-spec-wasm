// Locks the runtime null-pointer-elision bug closed and verifies
// the post-WASI arena ABI (see
// `doc/implementation-plan/rewrite/wasi/DESIGN.md` §4 + §6).
//
// `cel_memory_base_()` returns the runtime-side base offset for
// arena bookkeeping.  Without an opacity barrier in the C source,
// clang would treat `*(uint32_t*)(0+off) = v` as undefined and
// elide the store; `arena_reset` would compile to a no-op +
// `cel_log` call and `arena_alloc` to `unreachable`.  See
// `runtime/cel_runtime.c::cel_memory_base_()`.
//
// This test stands up a minimal wasmtime harness that:
//   1. Allocates a host-owned 2-page wasmtime_memory_t.
//   2. Binds it as `cel.memory` on a linker.
//   3. Stubs the cel_env / cel_host / wasi_snapshot_preview1 imports.
//   4. Instantiates cel_runtime.wasm against that linker.
//   5. Calls `arena_init(cap)` once (per-Instance setup) and
//      verifies `arena_capacity()` reflects the requested capacity.
//   6. Calls `arena_alloc(n)` and verifies the cursor advances by
//      `n` rounded up to 8-byte alignment.
//   7. Calls `arena_reset()` and verifies the cursor returns to 0.
//   8. Verifies `arena_alloc` returns 0 on OOM and leaves the
//      cursor unchanged.
//
// If any of these regress (e.g. a future clang upgrade re-discovers
// the null-UB elision, or the arena contract drifts), this test
// fails immediately rather than the bug staying latent until a
// downstream caller actually invokes the runtime on the wasm path.

#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/cel_runtime_wasm_bytes.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// 64 KiB matches `CELWASM_ARENA_CAPACITY_BYTES` — the default the
// host's `engine.cc::InstantiateRuntime` passes to `arena_init`.
// Tests pick a smaller capacity for the OOM case.
constexpr uint32_t kDefaultArenaCapacity = 64 * 1024;

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
// CEL_LOG("enter") at the top of arena_reset / arena_alloc; we don't
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

// Four-i32 in, void out.  Matches `cel_host.cel_timestamp_tz_accessor`,
// which the runtime imports unconditionally (declared in cel_time.c
// regardless of whether the test path reaches a `.year()` etc.
// accessor with a non-UTC timezone).
wasm_functype_t* HostFourArgFuncType() {
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

// Generic wasi-import no-op stub.  Every wasi-snapshot-preview1
// function we stub here either returns an errno (i32, 0 = success)
// or returns no result (proc_exit).  This single callback handles
// both shapes by checking `nresults`.  The runtime never *calls*
// any of these — they're kept alive by wasi-libc's startup code
// and the wasm linker refuses to instantiate the module if any
// declared import is unresolved.
wasm_trap_t* WasiNopStub(void*, wasmtime_caller_t*, const wasmtime_val_t*,
                         size_t, wasmtime_val_t* results, size_t nresults) {
  if (nresults >= 1) {
    results[0].kind = WASMTIME_I32;
    results[0].of.i32 = 0;
  }
  return nullptr;
}

// Owns the wasmtime state for one instantiated cel_runtime.wasm.
// Public fields so the test can drive arena_reset / arena_alloc and
// peek at memory bytes directly.
struct RuntimeHarness {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_linker_t* linker = nullptr;
  wasmtime_module_t* module = nullptr;
  wasmtime_memory_t memory{};
  wasmtime_instance_t instance{};
  wasmtime_func_t arena_init_fn{};
  wasmtime_func_t arena_reset_fn{};
  wasmtime_func_t arena_alloc_fn{};
  wasmtime_func_t arena_cursor_fn{};
  wasmtime_func_t arena_capacity_fn{};

  ~RuntimeHarness() {
    if (module != nullptr) wasmtime_module_delete(module);
    if (linker != nullptr) wasmtime_linker_delete(linker);
    if (store != nullptr) wasmtime_store_delete(store);
    if (engine != nullptr) wasm_engine_delete(engine);
  }
};

// Pulls one i32-returning or void-returning func export off an
// instance and stores its handle.  Reused for arena_reset (void) and
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
  // Phase C: the runtime is now built against `wasm32-wasi-threads`
  // (cctz needs `<mutex>`).  Enable threads + shared memory in the
  // engine so wasmtime accepts the module's shared-memory import.
  // `wasm_threads_set` enables the threads proposal (atomic ops);
  // `shared_memory_set` is the separate switch for shared linear
  // memory support — both are needed to instantiate.
  wasmtime_config_wasm_threads_set(config, true);
  wasmtime_config_shared_memory_set(config, true);
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

// Register a single wasi_snapshot_preview1 import as a no-op stub.
// `param_kinds` lists the wasm valtype kind for each parameter
// (WASM_I32 / WASM_I64); `has_i32_result` is true for everything
// except `proc_exit`.  The functype is owned by wasmtime_linker
// after the define call.
::testing::AssertionResult DefineWasiStub(
    wasmtime_linker_t* linker, const char* fn_name, size_t fn_name_len,
    std::initializer_list<wasm_valkind_t> param_kinds, bool has_i32_result) {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  if (param_kinds.size() == 0) {
    wasm_valtype_vec_new_empty(&params);
  } else {
    std::vector<wasm_valtype_t*> p;
    p.reserve(param_kinds.size());
    for (wasm_valkind_t k : param_kinds) {
      p.push_back(wasm_valtype_new(k));
    }
    wasm_valtype_vec_new(&params, p.size(), p.data());
  }
  if (has_i32_result) {
    wasm_valtype_t* r = wasm_valtype_new(WASM_I32);
    wasm_valtype_vec_new(&results, 1, &r);
  } else {
    wasm_valtype_vec_new_empty(&results);
  }
  wasm_functype_t* ft = wasm_functype_new(&params, &results);
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, "wasi_snapshot_preview1", 22, fn_name, fn_name_len, ft,
      WasiNopStub, /*data=*/nullptr, /*finalizer=*/nullptr);
  wasm_functype_delete(ft);
  if (err != nullptr) {
    return ::testing::AssertionFailure()
           << "define wasi_snapshot_preview1." << fn_name << ": "
           << WasmtimeErrorMsg(err);
  }
  return ::testing::AssertionSuccess();
}

// Define stubs for every wasi_snapshot_preview1 function the
// wasi-sdk-built runtime imports.  wasi-libc's startup code
// references all of these unconditionally (environ, file
// descriptors for stderr, proc_exit on abort); wasm-ld keeps the
// imports alive even though the C-only runtime never calls them.
// Wasmtime rejects instantiation if any declared import is
// unresolved.  Signatures from the
// [wasi-snapshot-preview1 spec](https://github.com/WebAssembly/WASI/blob/main/legacy/preview1/docs.md).
::testing::AssertionResult DefineWasiStubs(wasmtime_linker_t* linker) {
  struct Stub {
    const char* name = nullptr;
    std::size_t name_len = 0;
    std::initializer_list<wasm_valkind_t> params;
    bool has_i32_result = true;
  };
  // Each row mirrors one wasi import in the runtime.  Update both
  // this list AND the `cel_runtime.wasm` import set if wasi-sdk
  // libc starts pulling in a new symbol.
  const Stub kStubs[] = {
      {"environ_get", 11, {WASM_I32, WASM_I32}, true},
      {"environ_sizes_get", 17, {WASM_I32, WASM_I32}, true},
      {"fd_close", 8, {WASM_I32}, true},
      {"fd_prestat_get", 14, {WASM_I32, WASM_I32}, true},
      {"fd_prestat_dir_name", 19, {WASM_I32, WASM_I32, WASM_I32}, true},
      {"fd_seek", 7, {WASM_I32, WASM_I64, WASM_I32, WASM_I32}, true},
      {"fd_write", 8, {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, true},
      {"proc_exit", 9, {WASM_I32}, false},
      {"sched_yield", 11, {}, true},
      {"random_get", 10, {WASM_I32, WASM_I32}, true},
      // Pulled in by absl::time / cctz once RE2 + absl land via
      // Phase C C1/C2.  Used to set up cctz's lazy-init time-zone
      // tables; never called on the hot path of any kernel in this
      // test, so a no-op stub is sufficient.
      {"clock_time_get", 14, {WASM_I32, WASM_I64, WASM_I32}, true},
      // Same family — pulled in by wasi-libc once RE2/absl are
      // linked (the libc startup wires poll-based blocking even
      // though no kernel actually polls).
      {"poll_oneoff", 11, {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, true},
      // M12 string_ext: linking absl::strings::str_format (used by
      // the format renderer) pulls in fstat / read probes for
      // stderr-on-panic paths.  Never called from the kernels
      // under test; no-op stubs are sufficient.
      {"fd_fdstat_get", 13, {WASM_I32, WASM_I32}, true},
      {"fd_read", 7, {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, true},
  };
  for (const Stub& s : kStubs) {
    auto r =
        DefineWasiStub(linker, s.name, s.name_len, s.params, s.has_i32_result);
    if (!r) return r;
  }
  return ::testing::AssertionSuccess();
}

// Define the cel_host.* aggregate-op + message-resolver + ts-accessor
// trampolines as no-op stubs.  The runtime imports these symbols
// unconditionally; none of the tests here exercise the kHost paths.
::testing::AssertionResult DefineCelHostStubs(wasmtime_linker_t* linker) {
  // M5.D step 2: aggregate-op kHost imports.  size helpers are
  // 2-arg, in/eq/concat (and cel_message_eq) are 3-arg.
  struct Entry {
    const char* name;
    size_t name_len;
    int arity;
  };
  static const Entry kEntries[] = {
      {"cel_map_lookup", 14, 3},
      {"cel_list_at", 11, 3},
      {"cel_list_size", 13, 2},
      {"cel_list_in", 11, 3},
      {"cel_list_eq", 11, 3},
      {"cel_list_concat", 15, 3},
      {"cel_map_size", 12, 2},
      {"cel_map_in", 10, 3},
      {"cel_map_eq", 10, 3},
      {"cel_message_eq", 14, 3},
      // M9.B: `type(message)` descriptor-FQN resolver.  M7B.E:
      // timestamp-with-TZ accessor trampoline.  Both unconditional
      // imports.
      {"resolve_message_type_name", 25, 2},
      {"cel_timestamp_tz_accessor", 25, 4},
      // `cel_set_field_at_if_present` imports `cel_host.cel_set_field`
      // for the Some-path delegation.  The runtime pulls this symbol in
      // unconditionally; the arena-only harness here doesn't exercise
      // the kernel but still needs the import bound.
      {"cel_set_field", 13, 3},
      // Comprehension iter snapshots for host-backed list/map sources
      // (m5b §CCF-8).  Imported unconditionally by the kDynamic
      // dispatchers in cel_runtime.c; no-op stubs are sufficient
      // because none of the runtime-wasm-only tests exercise the
      // host-backed paths.
      {"cel_list_iter_open", 18, 2},
      {"cel_map_iter_open", 17, 2},
  };
  for (const auto& e : kEntries) {
    wasm_functype_t* ft;
    if (e.arity == 2) {
      ft = HostTwoArgFuncType();
    } else if (e.arity == 3) {
      ft = HostThreeArgFuncType();
    } else {
      ft = HostFourArgFuncType();
    }
    wasmtime_error_t* err = wasmtime_linker_define_func(
        linker, "cel_host", 8, e.name, e.name_len, ft, NoopCelHostThreeArg,
        /*data=*/nullptr, /*finalizer=*/nullptr);
    wasm_functype_delete(ft);
    if (err != nullptr) {
      return ::testing::AssertionFailure()
             << "define cel_host." << e.name << ": " << WasmtimeErrorMsg(err);
    }
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
  if (auto r = DefineCelHostStubs(h->linker); !r) return r;
  if (auto r = DefineWasiStubs(h->linker); !r) return r;
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
// arena_reset / arena_alloc func handles.  Linker must already have
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
  struct ExportRef {
    const char* name;
    size_t name_len;
    wasmtime_func_t* out;
  };
  const ExportRef exports[] = {
      {"arena_init", 10, &h->arena_init_fn},
      {"arena_reset", 11, &h->arena_reset_fn},
      {"arena_alloc", 11, &h->arena_alloc_fn},
      {"arena_cursor", 12, &h->arena_cursor_fn},
      {"arena_capacity", 14, &h->arena_capacity_fn},
  };
  for (const ExportRef& e : exports) {
    if (auto r = LookupFunc(ctx, h->instance, e.name, e.name_len, e.out); !r) {
      return r;
    }
  }
  return ::testing::AssertionSuccess();
}

// Top-level harness builder.  Three steps, each its own helper to
// stay under the 60-line function-size lint.
::testing::AssertionResult BuildHarness(RuntimeHarness* h) {
  if (auto r = InitEngineStoreMemory(h); !r) return r;
  if (auto r = InitLinker(h); !r) return r;
  return InstantiateRuntime(h);
}

// Call one of the arena_* exports.  `n_args` arg slots in `args`,
// `n_results` results returned in `result` (use 0 if void).  Returns
// the i32 result, or 0 on trap / error (call site decides whether
// that's expected via separate EXPECT macros).
uint32_t CallI32(const RuntimeHarness& h, const wasmtime_func_t& fn,
                 const char* name, const wasmtime_val_t* args,
                 std::size_t n_args, bool returns_i32) {
  wasmtime_context_t* ctx = wasmtime_store_context(h.store);
  wasmtime_val_t result;
  wasm_trap_t* trap = nullptr;
  wasmtime_func_t fn_copy = fn;
  wasmtime_error_t* err = wasmtime_func_call(ctx, &fn_copy, args, n_args,
                                             returns_i32 ? &result : nullptr,
                                             returns_i32 ? 1 : 0, &trap);
  if (err != nullptr) {
    ADD_FAILURE() << name << " trampoline error: " << WasmtimeErrorMsg(err);
    return 0;
  }
  if (trap != nullptr) {
    ADD_FAILURE() << name << " trapped: " << WasmTrapMsg(trap);
    return 0;
  }
  if (returns_i32) {
    EXPECT_EQ(result.kind, WASMTIME_I32);
    return static_cast<uint32_t>(result.of.i32);
  }
  return 0;
}

void ArenaInit(const RuntimeHarness& h, uint32_t cap_bytes) {
  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = static_cast<int32_t>(cap_bytes);
  CallI32(h, h.arena_init_fn, "arena_init", &arg, 1, /*returns_i32=*/false);
}

void ArenaReset(const RuntimeHarness& h) {
  CallI32(h, h.arena_reset_fn, "arena_reset", /*args=*/nullptr, 0,
          /*returns_i32=*/false);
}

uint32_t ArenaAlloc(const RuntimeHarness& h, uint32_t n) {
  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = static_cast<int32_t>(n);
  return CallI32(h, h.arena_alloc_fn, "arena_alloc", &arg, 1,
                 /*returns_i32=*/true);
}

uint32_t ArenaCursor(const RuntimeHarness& h) {
  return CallI32(h, h.arena_cursor_fn, "arena_cursor", nullptr, 0,
                 /*returns_i32=*/true);
}

uint32_t ArenaCapacity(const RuntimeHarness& h) {
  return CallI32(h, h.arena_capacity_fn, "arena_capacity", nullptr, 0,
                 /*returns_i32=*/true);
}

// ————————————— Tests —————————————

TEST(CelRuntimeWasmTest, ArenaInitSetsCapacity) {
  RuntimeHarness h;
  ASSERT_TRUE(BuildHarness(&h));
  ArenaInit(h, kDefaultArenaCapacity);
  EXPECT_EQ(ArenaCapacity(h), kDefaultArenaCapacity);
  EXPECT_EQ(ArenaCursor(h), 0u);
}

TEST(CelRuntimeWasmTest, ArenaAllocAdvancesCursor) {
  RuntimeHarness h;
  ASSERT_TRUE(BuildHarness(&h));
  ArenaInit(h, kDefaultArenaCapacity);

  const uint32_t off1 = ArenaAlloc(h, 24);
  EXPECT_NE(off1, 0u);
  EXPECT_EQ(ArenaCursor(h), 24u);

  // Second alloc should pick up where the first left off.
  const uint32_t off2 = ArenaAlloc(h, 16);
  EXPECT_EQ(off2, off1 + 24);
  EXPECT_EQ(ArenaCursor(h), 24u + 16u);
}

TEST(CelRuntimeWasmTest, ArenaResetReturnsCursorToZero) {
  RuntimeHarness h;
  ASSERT_TRUE(BuildHarness(&h));
  ArenaInit(h, kDefaultArenaCapacity);
  ArenaAlloc(h, 24);
  ASSERT_EQ(ArenaCursor(h), 24u);
  ArenaReset(h);
  EXPECT_EQ(ArenaCursor(h), 0u);
}

TEST(CelRuntimeWasmTest, ArenaGrowsOnDemandWhenInitialCapacityExceeded) {
  RuntimeHarness h;
  ASSERT_TRUE(BuildHarness(&h));
  // Tiny arena: 32 bytes total in the first chunk.  alloc(64)
  // must succeed by malloc'ing a fresh chunk sized to fit — the
  // chained-arena contract (cleanup-backlog #34).  The pre-#34
  // fixed-cap arena returned 0 here.
  ArenaInit(h, 32);
  const uint32_t off = ArenaAlloc(h, 64);
  EXPECT_NE(off, 0u) << "alloc(64) on a 32-byte first chunk must grow a "
                        "new chunk and succeed (cleanup-backlog #34)";
  // The first chunk's cursor is reported (back-compat semantic).
  // The grown chunk's bytes don't show up here — embedders that
  // need total-used should call into a per-chunk diagnostic
  // surface that hasn't been wired yet (acceptable: this is a
  // diagnostic accessor, not a correctness invariant).
}

}  // namespace
}  // namespace celwasm
