#include "compiler/codegen/cel_refs.h"

#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "compiler/codegen/module.h"

namespace celwasm {
namespace {

// The global that tracks the next free slot in `$cel_refs`.  Slot 0
// is reserved as the null sentinel, so the initializer is 1 and no
// user-allocated handle is ever zero.
constexpr const char* kNextGlobal = "cel_refs_next";

constexpr const char* kInternFn = "cel_ref_intern";
constexpr const char* kGetFn = "cel_ref_get";
constexpr const char* kResetFn = "cel_refs_reset";

constexpr const char* kWrapMessageFn = "cel_wrap_message";
constexpr const char* kUnwrapMessageFn = "cel_unwrap_message";

// Byte offset of `payload.msg_slot` within a `CelValue`.  The layout is
// `{ uint32_t kind; uint32_t _pad; union payload; }` and `msg_slot` is
// the first variant of the union, so it sits at offset 8.  The runtime
// pins the struct at 24 bytes via a `_Static_assert` in
// `cel_runtime.h`, so this is stable.
constexpr uint32_t kMsgSlotOffset = 8;

}  // namespace

absl::Status AddCelRefsTableAndHelpers(WasmModule& mod,
                                       absl::string_view table_name,
                                       uint32_t initial_slots) {
  if (initial_slots < 2) {
    return absl::InvalidArgumentError(absl::StrCat(
        "AddCelRefsTableAndHelpers: initial_slots (", initial_slots,
        ") must be >= 2 (slot 0 is the null sentinel)"));
  }

  if (auto s = mod.AddCelRefsTable(table_name, initial_slots,
                                   /*max_slots=*/std::nullopt);
      !s.ok()) {
    return s;
  }

  BinaryenModuleRef m = mod.raw();
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType externref = BinaryenTypeExternref();

  // `(global $cel_refs_next (mut i32) (i32.const 1))`.
  BinaryenAddGlobal(m, kNextGlobal, i32, /*mutable_=*/true,
                    BinaryenConst(m, BinaryenLiteralInt32(1)));

  // cel_ref_intern(ref: externref) -> i32
  //   local 0: ref (param)
  //   local 1: slot (var i32)
  //   slot = global.get cel_refs_next
  //   table.set $cel_refs (slot) (ref)
  //   cel_refs_next = slot + 1
  //   return slot
  {
    const std::string table_c(table_name);
    const BinaryenType intern_params[1] = {externref};
    const BinaryenType intern_locals[1] = {i32};

    BinaryenExpressionRef load_next = BinaryenGlobalGet(m, kNextGlobal, i32);
    BinaryenExpressionRef set_slot =
        BinaryenLocalSet(m, /*index=*/1, load_next);

    BinaryenExpressionRef write_ref =
        BinaryenTableSet(m, table_c.c_str(),
                         /*index=*/BinaryenLocalGet(m, /*index=*/1, i32),
                         /*value=*/BinaryenLocalGet(m, /*index=*/0, externref));

    BinaryenExpressionRef bump = BinaryenGlobalSet(
        m, kNextGlobal,
        BinaryenBinary(m, BinaryenAddInt32(),
                       BinaryenLocalGet(m, /*index=*/1, i32),
                       BinaryenConst(m, BinaryenLiteralInt32(1))));

    BinaryenExpressionRef ret = BinaryenLocalGet(m, /*index=*/1, i32);

    BinaryenExpressionRef children[4] = {set_slot, write_ref, bump, ret};
    BinaryenExpressionRef body =
        BinaryenBlock(m, /*name=*/nullptr, children, 4, i32);

    mod.AddFunction(kInternFn, intern_params, i32, intern_locals, body);
    mod.ExportFunction(kInternFn, kInternFn);
  }

  // cel_ref_get(slot: i32) -> externref
  //   return table.get $cel_refs (slot)
  {
    const std::string table_c(table_name);
    const BinaryenType get_params[1] = {i32};

    BinaryenExpressionRef body =
        BinaryenTableGet(m, table_c.c_str(),
                         /*index=*/BinaryenLocalGet(m, /*index=*/0, i32),
                         /*type=*/externref);

    mod.AddFunction(kGetFn, get_params, externref, {}, body);
    mod.ExportFunction(kGetFn, kGetFn);
  }

  // cel_refs_reset() -> void
  //   cel_refs_next = 1
  //
  // NOTE: this does not clear the table entries — they remain as stale
  // externrefs until overwritten by the next `cel_ref_intern`.  That's
  // intentional: wasmtime keeps externrefs alive via the table's root
  // set, so leaving them around would extend host lifetimes until a
  // re-intern.  The expected host contract is to reset the table from
  // the outside (or drop the store) between evaluations.
  {
    BinaryenExpressionRef body = BinaryenGlobalSet(
        m, kNextGlobal, BinaryenConst(m, BinaryenLiteralInt32(1)));

    mod.AddFunction(kResetFn, {}, BinaryenTypeNone(), {}, body);
    mod.ExportFunction(kResetFn, kResetFn);
  }

  mod.ExportTable(table_name, table_name);
  return absl::OkStatus();
}

// cel_wrap_message(ref: externref) -> i32
//   return cel_make_message(cel_ref_intern(ref))
//
// This is a one-liner: `cel_ref_intern` is defined locally by
// `AddCelRefsTableAndHelpers`, and `cel_make_message` is a runtime
// import that already knows how to write `{kind=CEL_MESSAGE, msg_slot}`
// into a freshly-allocated `CelValue`.  Composing them keeps layout
// knowledge in the runtime where it belongs.
//
// cel_unwrap_message(cv: i32) -> externref
//   slot = i32.load offset=8 (cel_mem_base() + cv)
//   return cel_ref_get(slot)
//
// We do the load inline rather than call a runtime helper because it
// is a single i32 load at a fixed offset; adding a `cel_msg_slot(cv)`
// accessor to the runtime for one caller would be noise.  The offset
// is pinned by `kMsgSlotOffset` above and guarded by a static_assert
// on the C side.
absl::Status AddMessageWrapHelpers(WasmModule& mod,
                                   absl::string_view table_name) {
  // The table name isn't referenced directly — the helpers call
  // `cel_ref_intern` / `cel_ref_get` by function name, and those
  // already know which table they're bound to.  The parameter is
  // retained for symmetry with `AddCelRefsTableAndHelpers` and as
  // forward-compatibility for a future assertion that the caller
  // declared the expected table.
  (void)table_name;
  BinaryenModuleRef m = mod.raw();
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType externref = BinaryenTypeExternref();

  // cel_wrap_message(externref) -> i32.
  {
    const BinaryenType params[1] = {externref};

    BinaryenExpressionRef ref_arg = BinaryenLocalGet(m, /*index=*/0, externref);
    BinaryenExpressionRef intern_call =
        BinaryenCall(m, kInternFn, &ref_arg, 1, i32);
    BinaryenExpressionRef body =
        BinaryenCall(m, "cel_make_message", &intern_call, 1, i32);

    mod.AddFunction(kWrapMessageFn, params, i32, {}, body);
    mod.ExportFunction(kWrapMessageFn, kWrapMessageFn);
  }

  // cel_unwrap_message(i32) -> externref.
  {
    const BinaryenType params[1] = {i32};

    // abs = cel_mem_base() + cv  (arena-relative -> absolute, same
    // translation as LowerSpanLiteral).
    BinaryenExpressionRef base_call = BinaryenCall(
        m, "cel_mem_base", /*operands=*/nullptr, /*numOperands=*/0, i32);
    BinaryenExpressionRef cv_arg = BinaryenLocalGet(m, /*index=*/0, i32);
    BinaryenExpressionRef abs =
        BinaryenBinary(m, BinaryenAddInt32(), base_call, cv_arg);

    BinaryenExpressionRef slot =
        BinaryenLoad(m, /*bytes=*/4, /*signed_=*/false,
                     /*offset=*/kMsgSlotOffset, /*align=*/4, i32, abs,
                     /*memoryName=*/"memory");

    BinaryenExpressionRef body = BinaryenCall(m, kGetFn, &slot, 1, externref);

    mod.AddFunction(kUnwrapMessageFn, params, externref, {}, body);
    mod.ExportFunction(kUnwrapMessageFn, kUnwrapMessageFn);
  }

  return absl::OkStatus();
}

}  // namespace celwasm
