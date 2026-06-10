#ifndef CELWASM_COMPILER_CODEGEN_EXPR_LOWER_INTERNAL_H_
#define CELWASM_COMPILER_CODEGEN_EXPR_LOWER_INTERNAL_H_

// Internal header shared between expr_lower.cc and
// expr_lower_comprehension.cc.  Not part of the public surface —
// callers outside the codegen TUs should use expr_lower.h.
//
// Split rationale: the comprehension codegen is structurally
// distinct from the other arms (its own scope machinery, its own
// runtime helper set, its own pattern-detector zoo).  Splitting
// keeps each TU under the cognitive-budget threshold (~1100 LoC)
// and isolates the comprehension surface for the planned
// simplification pass tracked in
// `rewrite/m5b-comprehensions-simplification.md`.

#include <cstdint>
#include <vector>

#include "absl/status/statusor.h"
#include "binaryen-c.h"
#include "common/expr.h"
#include "compiler/codegen/expr_lower.h"  // FieldRefRow
#include "compiler/codegen/layout_pass.h"
#include "compiler/codegen/module.h"
#include "compiler/codegen/overload_table.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

// Shared per-eval emission context — threaded through every `Emit`
// call so kSelect can recurse into its operand and append field-ref
// rows without signature churn.  Originally lived inside
// expr_lower.cc's anonymous namespace; surfaced here so the
// comprehension TU can reach into the same state.
struct EmitCtx {
  WasmModule& mod;
  const TypedAst& ast;
  const StaticLayout& layout;
  std::vector<FieldRefRow>& field_refs;
  // Looked up at every general-arm `kCallExpr` to map the resolved
  // cel-cpp `overload_id` (e.g. `add_int64`) onto the wasm helper
  // this codegen emits a `BinaryenCall` to.
  const OverloadTable& overload_table;
  // Wasm-local index offset applied to every kIdent load.  `$eval`
  // declares no wasm params and leaves this 0.  CEL-defined custom
  // function bodies declare `num_args` i32 params at the front of
  // the local space (`out_slot, arg0, ...`), so their referenced
  // variables — which `ResolvePass` assigned 0-based `local_index`
  // values to — live at wasm locals `[num_args, num_args + K)`.
  // See `library_module.cc::LowerCelDefinedFn`.
  uint32_t wasm_local_offset = 0;
};

// Top-level dispatcher.  Defined in expr_lower.cc.  Comprehension
// codegen calls it to recurse into iter_range / accu_init /
// loop_cond / loop_step / result sub-expressions.
absl::StatusOr<BinaryenExpressionRef> Emit(EmitCtx& ctx, const cel::Expr& expr);

// ── Wasm-emission primitives shared between the two TUs ──────────

// `(i32.const value)`.
BinaryenExpressionRef I32Const(WasmModule& mod, uint32_t value);

// Wrappers around `BinaryenLoad` / `BinaryenStore` that omit the
// trailing memory-name parameter and always pass `nullptr` for it,
// so the emitted access targets whatever memory the module has.
// This is load-bearing: in static mode the adopted runtime's memory
// is named "0", not "memory", so a call site that hard-codes a name
// like "memory" works in dynamic mode and silently breaks static
// mode (see `doc/implementation-plan/rewrite/m28-configurable-linking.md`
// §5.3).  All codegen loads/stores MUST go through these wrappers —
// do not call `BinaryenLoad` / `BinaryenStore` directly.
BinaryenExpressionRef CodegenLoad(BinaryenModuleRef module, uint32_t bytes,
                                  bool signed_, uint32_t offset, uint32_t align,
                                  BinaryenType type, BinaryenExpressionRef ptr);
BinaryenExpressionRef CodegenStore(BinaryenModuleRef module, uint32_t bytes,
                                   uint32_t offset, uint32_t align,
                                   BinaryenExpressionRef ptr,
                                   BinaryenExpressionRef value,
                                   BinaryenType type);

// `(call $cel.cel_copy_slot (i32.const dst) (i32.const src))`.
BinaryenExpressionRef EmitCelCopySlot(EmitCtx& ctx, uint32_t dst_slot,
                                      uint32_t src_slot);

// Variant of EmitCelCopySlot taking the source as an arbitrary
// i32-returning expression instead of a slot constant.  Used by
// the ternary arm where the source is the rodata / local /
// workspace slot offset returned by Emit-ing the arm expression.
BinaryenExpressionRef BuildConditionalArm(EmitCtx& ctx,
                                          BinaryenExpressionRef eval_expr,
                                          uint32_t out_slot);

// `(i32.eq (i32.load offset=N <slot>) <expected>)` — the CelValue
// kind / payload probe used by the ternary's nested-if shape and
// the comprehension loop-cond peephole.
BinaryenExpressionRef LoadSlotI32Eq(EmitCtx& ctx, uint32_t slot,
                                    uint32_t offset, int32_t expected);

// `(i32.ne (i32.load offset=N <slot>) <expected>)`.
BinaryenExpressionRef LoadSlotI32Ne(EmitCtx& ctx, uint32_t slot,
                                    uint32_t offset, int32_t expected);

// `(call $cel.cel_list_append_at <list_slot> <elem_eval>)`.
// `elem_eval` is an i32-valued sub-expression whose runtime value
// is the linear-memory offset of the element's CelValue.  Universal
// write for arena lists — shared between kListExpr literal codegen
// (expr_lower.cc) and comprehension accu codegen
// (expr_lower_comprehension.cc).
BinaryenExpressionRef EmitCelListAppendCall(EmitCtx& ctx, uint32_t list_slot,
                                            BinaryenExpressionRef elem_eval);

// ── Comprehension entry point (defined in expr_lower_comprehension.cc) ──

// Lower a `kComprehensionExpr` to wasm.  Handles all comprehension
// shapes — exists/all/exists_one/map/filter/transformList/
// transformMap/transformMapEntry + cel.bind via Shape-C fast path.
// The dispatcher in expr_lower.cc forwards here for the
// `kComprehensionExpr` arm of Emit.
absl::StatusOr<BinaryenExpressionRef> LowerComprehension(
    EmitCtx& ctx, const cel::Expr& expr, const cel::ComprehensionExpr& comp,
    const NodeAnnotation& ann);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_EXPR_LOWER_INTERNAL_H_
