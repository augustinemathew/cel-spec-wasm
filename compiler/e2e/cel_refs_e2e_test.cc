// End-to-end round-trip for the `$cel_refs` externref helpers.
//
// Emits a module that only declares the table + helper functions via
// `AddCelRefsTableAndHelpers`, instantiates it under wasmtime, and
// walks an externref through `cel_ref_intern` → `cel_ref_get`.  This
// is the one test in the repo that proves the host handle contract
// (design §7.1) actually works end-to-end.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "compiler/codegen/cel_refs.h"
#include "compiler/codegen/module.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

std::string ErrorMessage(wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string out(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return out;
}

// wasmtime defaults to MVP features.  The emitted `$cel_refs` table
// uses `(ref.null externref)` as its slot initializer, which requires
// both reference-types and function-references.  Every test here runs
// under the same engine config, so build one and let the test body
// consume it.
wasm_engine_t* NewEngineWithRefTypes() {
  wasm_config_t* config = wasm_config_new();
  wasmtime_config_wasm_reference_types_set(config, true);
  wasmtime_config_wasm_function_references_set(config, true);
  // Binaryen enables GC when it emits typed `ref.null externref`
  // initializers, so the consumer has to as well — otherwise wasmtime
  // rejects the module at parse time with "heap types not supported".
  wasmtime_config_wasm_gc_set(config, true);
  return wasm_engine_new_with_config(config);
}

TEST(CelRefsE2ETest, InternAndGetRoundTripRestoresHostPointer) {
  WasmModule mod;
  ASSERT_THAT(
      AddCelRefsTableAndHelpers(mod, "$cel_refs", /*initial_slots=*/16),
      IsOk());
  ASSERT_THAT(mod.Validate(), IsOk());
  auto bytes = mod.Serialize();
  ASSERT_THAT(bytes, IsOk());

  wasm_engine_t* engine = NewEngineWithRefTypes();
  wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
  wasmtime_context_t* ctx = wasmtime_store_context(store);

  wasmtime_module_t* wmod = nullptr;
  {
    wasmtime_error_t* err =
        wasmtime_module_new(engine, bytes->data(), bytes->size(), &wmod);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
  }

  wasmtime_instance_t instance;
  wasm_trap_t* trap = nullptr;
  {
    wasmtime_error_t* err = wasmtime_instance_new(
        ctx, wmod, /*imports=*/nullptr, /*nimports=*/0, &instance, &trap);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
    ASSERT_EQ(trap, nullptr);
  }

  wasmtime_extern_t intern_ext, get_ext, reset_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(
      ctx, &instance, "cel_ref_intern", std::strlen("cel_ref_intern"),
      &intern_ext));
  ASSERT_TRUE(wasmtime_instance_export_get(
      ctx, &instance, "cel_ref_get", std::strlen("cel_ref_get"), &get_ext));
  ASSERT_TRUE(wasmtime_instance_export_get(
      ctx, &instance, "cel_refs_reset", std::strlen("cel_refs_reset"),
      &reset_ext));
  ASSERT_EQ(intern_ext.kind, WASMTIME_EXTERN_FUNC);
  ASSERT_EQ(get_ext.kind, WASMTIME_EXTERN_FUNC);
  ASSERT_EQ(reset_ext.kind, WASMTIME_EXTERN_FUNC);

  // Build an externref that wraps a fixed pointer the host can later
  // recognise.  Using `(void*)0xCAFE` rather than a real allocation
  // keeps the test hermetic — wasmtime never dereferences the data,
  // just hands it back via `wasmtime_externref_data`.
  void* const kHostPayload = reinterpret_cast<void*>(
      static_cast<uintptr_t>(0xCAFEuL));
  wasmtime_val_t ref_in{};
  ref_in.kind = WASMTIME_EXTERNREF;
  ASSERT_TRUE(wasmtime_externref_new(ctx, kHostPayload, /*finalizer=*/nullptr,
                                     &ref_in.of.externref));

  // intern(ref) -> i32 slot.
  wasmtime_val_t slot{};
  {
    wasmtime_error_t* err =
        wasmtime_func_call(ctx, &intern_ext.of.func, &ref_in, 1, &slot, 1,
                           &trap);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
    ASSERT_EQ(trap, nullptr);
  }
  ASSERT_EQ(slot.kind, WASMTIME_I32);
  // Slot 0 is reserved; first allocation lands in slot 1.
  EXPECT_EQ(slot.of.i32, 1);

  // get(slot) -> externref, then verify data() round-trips.
  wasmtime_val_t ref_out{};
  {
    wasmtime_error_t* err = wasmtime_func_call(ctx, &get_ext.of.func, &slot,
                                               1, &ref_out, 1, &trap);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
    ASSERT_EQ(trap, nullptr);
  }
  ASSERT_EQ(ref_out.kind, WASMTIME_EXTERNREF);
  void* got = wasmtime_externref_data(ctx, &ref_out.of.externref);
  EXPECT_EQ(got, kHostPayload);

  // Second intern lands at slot 2 — bump global advanced.
  wasmtime_val_t ref_in2{};
  ref_in2.kind = WASMTIME_EXTERNREF;
  ASSERT_TRUE(wasmtime_externref_new(ctx, kHostPayload, nullptr,
                                     &ref_in2.of.externref));
  wasmtime_val_t slot2{};
  {
    wasmtime_error_t* err =
        wasmtime_func_call(ctx, &intern_ext.of.func, &ref_in2, 1, &slot2, 1,
                           &trap);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
    ASSERT_EQ(trap, nullptr);
  }
  EXPECT_EQ(slot2.of.i32, 2);

  // After reset, the allocator rewinds to 1.
  wasmtime_val_t reset_result{};  // void return.
  {
    wasmtime_error_t* err = wasmtime_func_call(
        ctx, &reset_ext.of.func, /*args=*/nullptr, /*nargs=*/0,
        &reset_result, /*nresults=*/0, &trap);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
    ASSERT_EQ(trap, nullptr);
  }
  wasmtime_val_t ref_in3{};
  ref_in3.kind = WASMTIME_EXTERNREF;
  ASSERT_TRUE(wasmtime_externref_new(ctx, kHostPayload, nullptr,
                                     &ref_in3.of.externref));
  wasmtime_val_t slot3{};
  {
    wasmtime_error_t* err =
        wasmtime_func_call(ctx, &intern_ext.of.func, &ref_in3, 1, &slot3, 1,
                           &trap);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
    ASSERT_EQ(trap, nullptr);
  }
  EXPECT_EQ(slot3.of.i32, 1);

  wasmtime_val_unroot(&ref_in);
  wasmtime_val_unroot(&ref_in2);
  wasmtime_val_unroot(&ref_in3);
  wasmtime_val_unroot(&ref_out);
  wasmtime_extern_delete(&intern_ext);
  wasmtime_extern_delete(&get_ext);
  wasmtime_extern_delete(&reset_ext);
  wasmtime_module_delete(wmod);
  wasmtime_store_delete(store);
  wasm_engine_delete(engine);
}

// Slot 0 is the null sentinel — `cel_ref_get(0)` must return a null
// externref, not an unrelated host value.
TEST(CelRefsE2ETest, SlotZeroIsNullSentinel) {
  WasmModule mod;
  ASSERT_THAT(AddCelRefsTableAndHelpers(mod, "$cel_refs", 4), IsOk());
  auto bytes = mod.Serialize();
  ASSERT_THAT(bytes, IsOk());

  wasm_engine_t* engine = NewEngineWithRefTypes();
  wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
  wasmtime_context_t* ctx = wasmtime_store_context(store);

  wasmtime_module_t* wmod = nullptr;
  {
    wasmtime_error_t* err =
        wasmtime_module_new(engine, bytes->data(), bytes->size(), &wmod);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
  }
  wasmtime_instance_t instance;
  wasm_trap_t* trap = nullptr;
  {
    wasmtime_error_t* err = wasmtime_instance_new(ctx, wmod, nullptr, 0,
                                                  &instance, &trap);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
    ASSERT_EQ(trap, nullptr);
  }

  wasmtime_extern_t get_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(
      ctx, &instance, "cel_ref_get", std::strlen("cel_ref_get"), &get_ext));

  wasmtime_val_t slot0{};
  slot0.kind = WASMTIME_I32;
  slot0.of.i32 = 0;
  wasmtime_val_t out{};
  {
    wasmtime_error_t* err = wasmtime_func_call(ctx, &get_ext.of.func, &slot0,
                                               1, &out, 1, &trap);
    ASSERT_EQ(err, nullptr) << ErrorMessage(err);
    ASSERT_EQ(trap, nullptr);
  }
  ASSERT_EQ(out.kind, WASMTIME_EXTERNREF);
  EXPECT_TRUE(wasmtime_externref_is_null(&out.of.externref));

  wasmtime_val_unroot(&out);
  wasmtime_extern_delete(&get_ext);
  wasmtime_module_delete(wmod);
  wasmtime_store_delete(store);
  wasm_engine_delete(engine);
}

}  // namespace
}  // namespace celwasm
