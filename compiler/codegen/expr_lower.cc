#include "compiler/codegen/expr_lower.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "base/builtins.h"
#include "binaryen-c.h"
#include "common/ast.h"
#include "common/constant.h"
#include "common/expr.h"
#include "common/operators.h"
#include "compiler/codegen/cel_refs.h"
#include "compiler/codegen/module.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {
namespace {

namespace op = ::google::api::expr::common;

// Per-eval-function lowering state.  Holds the parameter layout (one
// WASM param per user-declared variable, in spec order) and any scratch
// locals the body accumulates — for example, the string-constant
// lowering allocates one i32 per literal to hold the `cel_alloc` result
// across the subsequent `i32.store8` chain.
//
// Local indexing in WebAssembly: parameters come first (0..num_params),
// then local variables (num_params..num_params+local_types.size()).
// `AddLocal` returns the absolute index in that single space.
//
// `idents` maps declared variable name → param index; `kIdentExpr`
// lowers to `local.get idents[name]`.  Misses surface as an
// `InvalidArgument` status, which is stronger than a lookup miss:
// the checker already rejects unknown identifiers, so a miss here
// indicates either a bug in the frontend (a var used in the
// expression wasn't declared) or a downstream caller that passed an
// `TypedAst` whose `variables()` list is out of sync with the AST.
//
// The context does NOT track which runtime imports have been
// declared: we always link eval modules fully against the runtime
// (see DeclareRuntimeImports), so there is nothing to gate on.
struct LoweringContext {
  WasmModule& mod;
  uint32_t num_params = 0;
  std::vector<BinaryenType> local_types;
  absl::flat_hash_map<std::string, BinaryenIndex> idents;
  // Lazily materialized scratch slot: a single i32 local holding the
  // offset (into the runtime's arena) of a 24-byte CelValue region
  // that the sret arithmetic helpers write into.  Created on first
  // demand via GetScratchSlotLocal(); `LowerToEvalFunction` then
  // wraps the body with a prologue that runs
  // `local.set $slot (call $cel_alloc (i32.const 24))` at eval entry.
  // One slot suffices today because every checked-arithmetic codegen
  // shape loads the payload into a wasm local before the next
  // helper call reuses the slot (straight-line tree evaluation).
  std::optional<BinaryenIndex> scratch_slot;

  BinaryenIndex AddLocal(BinaryenType type) {
    local_types.push_back(type);
    return static_cast<BinaryenIndex>(num_params + local_types.size() - 1);
  }

  BinaryenIndex GetScratchSlotLocal() {
    if (!scratch_slot.has_value()) {
      scratch_slot = AddLocal(BinaryenTypeInt32());
    }
    return *scratch_slot;
  }
};

absl::StatusOr<BinaryenExpressionRef> LowerExpr(LoweringContext& ctx,
                                                const TypedAst& ast,
                                                const cel::Expr& expr);

// Small helpers that wrap `mod.AddFunctionImport` with the right arity-
// encoded `absl::Span`.  All eval-module imports live under the `cel`
// external-module name (the runtime), so that is hardcoded here; host-
// owned imports are declared by `DeclareHostImports` below.
void ImportCel0(WasmModule& mod, absl::string_view name, BinaryenType result) {
  mod.AddFunctionImport(name, /*external_module=*/"cel",
                        /*external_base=*/name,
                        absl::Span<const BinaryenType>(), result);
}
void ImportCel1(WasmModule& mod, absl::string_view name, BinaryenType p0,
                BinaryenType result) {
  mod.AddFunctionImport(name, /*external_module=*/"cel",
                        /*external_base=*/name,
                        absl::Span<const BinaryenType>(&p0, 1), result);
}
void ImportCel2(WasmModule& mod, absl::string_view name, BinaryenType p0,
                BinaryenType p1, BinaryenType result) {
  BinaryenType params[2] = {p0, p1};
  mod.AddFunctionImport(name, /*external_module=*/"cel",
                        /*external_base=*/name,
                        absl::Span<const BinaryenType>(params, 2), result);
}

// Allocation / span-literal / equality / size / member-call imports.
void DeclareAllocAndSpanImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType i64 = BinaryenTypeInt64();
  // Allocation + string/bytes construction (M3 slice A).
  ImportCel1(mod, "cel_alloc", i32, i32);
  ImportCel2(mod, "cel_make_string_view", i32, i32, i32);
  ImportCel2(mod, "cel_make_bytes_view", i32, i32, i32);
  // Message-value construction (M3 slice G): wraps an interned `$cel_refs`
  // slot into a heap-allocated CelValue whose kind is CEL_MESSAGE.  Used
  // by `cel_wrap_message` in the eval module.
  ImportCel1(mod, "cel_make_message", i32, i32);
  // `cel_mem_base` returns the absolute linear-memory offset at which
  // the runtime's arena (`g_memory`) begins.  The eval module needs
  // this to translate arena-relative offsets returned by `cel_alloc`
  // into absolute offsets usable with `i32.store8` (see
  // LowerSpanLiteral for the translation).
  ImportCel0(mod, "cel_mem_base", i32);
  // String/bytes helpers (equality, concat, size, value-unbox).
  ImportCel2(mod, "cel_string_eq", i32, i32, i32);
  ImportCel2(mod, "cel_bytes_eq", i32, i32, i32);
  ImportCel2(mod, "cel_string_concat", i32, i32, i32);
  ImportCel2(mod, "cel_bytes_concat", i32, i32, i32);
  ImportCel1(mod, "cel_string_size", i32, i64);
  ImportCel1(mod, "cel_bytes_size", i32, i64);
  ImportCel1(mod, "cel_bool_from_value", i32, i32);
  // Member-call helpers (slice E).  Each returns i32 0/1.
  ImportCel2(mod, "cel_string_starts_with", i32, i32, i32);
  ImportCel2(mod, "cel_string_ends_with", i32, i32, i32);
  ImportCel2(mod, "cel_string_contains", i32, i32, i32);
}

// Checked arithmetic (M4 Slice C commit 2).  Codegen uses the
// scalar-arg sret variants — `cel_int_add_at_ii(out, i64, i64)` —
// where `out` is a 24-byte scratch slot offset the helper writes
// the result CelValue into (kind + payload).  Codegen loads the
// kind tag inline; on CEL_ERROR it still traps (the observable-
// ERROR 3VL plumbing lands in commit 3 with the &&/|| retrofit).
// Switching from the Slice B `_ii`-returning-offset shape to sret
// removes the per-operation arena bump and unlocks the single-
// slot temporary reuse that commit 3 depends on.
void DeclareCheckedArithmeticImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType i64 = BinaryenTypeInt64();
  const BinaryenType none = BinaryenTypeNone();
  BinaryenType int_params[3] = {i32, i64, i64};
  auto import_int = [&](absl::string_view name) {
    mod.AddFunctionImport(name, /*external_module=*/"cel",
                          /*external_base=*/name,
                          absl::Span<const BinaryenType>(int_params, 3), none);
  };
  import_int("cel_int_add_at_ii");
  import_int("cel_int_sub_at_ii");
  import_int("cel_int_mul_at_ii");
  import_int("cel_int_div_at_ii");
  import_int("cel_int_mod_at_ii");
  import_int("cel_uint_add_at_uu");
  import_int("cel_uint_sub_at_uu");
  import_int("cel_uint_mul_at_uu");
  import_int("cel_uint_div_at_uu");
  import_int("cel_uint_mod_at_uu");
  BinaryenType neg_params[2] = {i32, i64};
  mod.AddFunctionImport("cel_int_neg_at_i", /*external_module=*/"cel",
                        /*external_base=*/"cel_int_neg_at_i",
                        absl::Span<const BinaryenType>(neg_params, 2), none);
}

// Declares every import the eval module may reference, up front.  Each
// eval module is always linked against the runtime at instantiation
// time, so declaring imports the current AST happens not to use is
// harmless — wasmtime accepts unused imports as long as they resolve.
// Keeping the set fixed here avoids per-subtree "is this declared yet?"
// bookkeeping and mirrors the design doc's two-module layout.
absl::Status DeclareRuntimeImports(WasmModule& mod) {
  // Shared linear memory — every CelValue offset the codegen emits
  // is interpreted against the runtime's arena.  One page minimum.
  auto s = mod.AddMemoryImport(/*external_module=*/"cel",
                               /*external_base=*/"memory",
                               /*initial_pages=*/1,
                               /*max_pages=*/std::nullopt);
  if (!s.ok()) {
    return s;
  }
  DeclareAllocAndSpanImports(mod);
  DeclareCheckedArithmeticImports(mod);
  return absl::OkStatus();
}

// Host-side proto access (slice G2+).  Lives under the `cel_host` module
// rather than `cel` because the host, not the runtime, backs these
// imports.  Declared unconditionally — eval modules that don't touch
// proto fields still satisfy wasmtime's "all imports must resolve"
// rule, since the linker binds them from `CelHostEnv` regardless of
// whether the body ever calls them.
void DeclareHostImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType extref = BinaryenTypeExternref();
  // get_field(msg, field_number, out_cv_offset) → ().  The host writes
  // a full 24-byte CelValue at `cel_mem_base() + out_cv_offset`.
  BinaryenType gf_params[3] = {extref, i32, i32};
  mod.AddFunctionImport("get_field", /*external_module=*/"cel_host",
                        /*external_base=*/"get_field",
                        absl::Span<const BinaryenType>(gf_params, 3),
                        BinaryenTypeNone());
  // has_field(msg, field_number) → i32 (0/1).
  BinaryenType hf_params[2] = {extref, i32};
  mod.AddFunctionImport("has_field", /*external_module=*/"cel_host",
                        /*external_base=*/"has_field",
                        absl::Span<const BinaryenType>(hf_params, 2), i32);
  // message_eq(a, b) → i32 (0/1).
  BinaryenType me_params[2] = {extref, extref};
  mod.AddFunctionImport("message_eq", /*external_module=*/"cel_host",
                        /*external_base=*/"message_eq",
                        absl::Span<const BinaryenType>(me_params, 2), i32);
}

absl::Status UnimplementedKind(absl::string_view kind, int64_t id) {
  return absl::UnimplementedError(absl::StrCat(
      "expr_lower: ", kind, " is not yet supported (expr id ", id, ")"));
}

absl::Status UnimplementedRepr(absl::string_view op_name, Repr r, int64_t id) {
  return absl::UnimplementedError(absl::StrCat(
      "expr_lower: operator `", op_name, "` does not support Repr `",
      ReprName(r), "` (expr id ", id, ")"));
}

// The checker populates annotations() for every typed node.  Failing to
// find one means the AST was not run through PopulateAnnotations (which
// our frontend does) or the node is DYN (which static_subset rejects).
absl::StatusOr<Repr> ReprOf(const TypedAst& ast, const cel::Expr& e) {
  const NodeAnnotation* a = ast.annotations().Find(e.id());
  if (a == nullptr || a->repr == Repr::kUnknown) {
    return absl::FailedPreconditionError(
        absl::StrCat("expr_lower: missing Repr annotation for expr id ", e.id(),
                     " — was PopulateAnnotations run? did RejectDyn pass?"));
  }
  return a->repr;
}

// Lowers a span literal (string or bytes) as:
//   (block (result i32)
//     (local.set $scratch (call $cel_alloc (i32.const len)))
//     (local.set $abs    (i32.add (call $cel_mem_base)
//                                 (local.get $scratch)))
//     (i32.store8 offset=0 (local.get $abs) (i32.const b0))
//     ... one i32.store8 per byte ...
//     (call $ctor_helper (local.get $scratch) (i32.const len)))
//
// Rationale: we deliberately do NOT use a wasm data segment.  The eval
// module and the runtime module share the runtime's linear memory, and
// a data segment in the eval module would need to agree on a byte range
// that doesn't alias the runtime's own static data / bump arena.  That
// coordination is brittle; using `cel_alloc` at instantiation time
// simply rents a fresh, never-aliasing region from the runtime itself.
//
// `cel_alloc` returns an offset relative to the runtime's `g_memory`
// array, not an absolute linear-memory offset; `i32.store8` wants an
// absolute offset.  We translate once per literal by adding
// `cel_mem_base()` (the absolute linear-memory address of `g_memory`).
// The constructor helper still takes the arena-relative offset — the
// runtime internally reconstructs the absolute address as
// `g_memory + ptr` on every access, so its CelValue payloads stay
// arena-relative for portability with the native-host tests.
//
// The store-per-byte expansion is verbose but is only emitted per
// literal (once), and the resulting code is trivial for wasmtime's
// baseline compiler.  String vs. bytes differ only in the constructor
// helper (`cel_make_string_view` vs. `cel_make_bytes_view`), so the
// helper is parameterised on the function name.
absl::StatusOr<BinaryenExpressionRef> LowerSpanLiteral(
    LoweringContext& ctx, absl::string_view bytes, const char* ctor_helper) {
  BinaryenModuleRef m = ctx.mod.raw();
  const auto len = static_cast<uint32_t>(bytes.size());
  const BinaryenIndex scratch = ctx.AddLocal(BinaryenTypeInt32());
  const BinaryenIndex abs = ctx.AddLocal(BinaryenTypeInt32());

  std::vector<BinaryenExpressionRef> children;
  children.reserve(3 + bytes.size());

  // local.set $scratch (call $cel_alloc len)
  {
    BinaryenExpressionRef alloc_arg =
        BinaryenConst(m, BinaryenLiteralInt32(static_cast<int32_t>(len)));
    BinaryenExpressionRef alloc_call =
        BinaryenCall(m, "cel_alloc", &alloc_arg, 1, BinaryenTypeInt32());
    children.push_back(BinaryenLocalSet(m, scratch, alloc_call));
  }

  // local.set $abs (i32.add (call $cel_mem_base) (local.get $scratch))
  {
    BinaryenExpressionRef base_call =
        BinaryenCall(m, "cel_mem_base", /*operands=*/nullptr, /*numOperands=*/0,
                     BinaryenTypeInt32());
    BinaryenExpressionRef sum =
        BinaryenBinary(m, BinaryenAddInt32(), base_call,
                       BinaryenLocalGet(m, scratch, BinaryenTypeInt32()));
    children.push_back(BinaryenLocalSet(m, abs, sum));
  }

  // One i32.store8 per byte, using `offset=i` to avoid emitting a
  // separate add for each position.
  for (uint32_t i = 0; i < len; ++i) {
    const auto byte = static_cast<uint8_t>(bytes.at(i));
    BinaryenExpressionRef ptr = BinaryenLocalGet(m, abs, BinaryenTypeInt32());
    BinaryenExpressionRef value =
        BinaryenConst(m, BinaryenLiteralInt32(static_cast<int32_t>(byte)));
    children.push_back(BinaryenStore(m,
                                     /*bytes=*/1,
                                     /*offset=*/i,
                                     /*align=*/1, ptr, value,
                                     /*type=*/BinaryenTypeInt32(),
                                     /*memoryName=*/"memory"));
  }

  // Trailing call that produces the block's i32 result.
  {
    BinaryenExpressionRef args[2] = {
        BinaryenLocalGet(m, scratch, BinaryenTypeInt32()),
        BinaryenConst(m, BinaryenLiteralInt32(static_cast<int32_t>(len))),
    };
    children.push_back(
        BinaryenCall(m, ctor_helper, args, 2, BinaryenTypeInt32()));
  }

  return BinaryenBlock(m,
                       /*name=*/nullptr, children.data(),
                       static_cast<BinaryenIndex>(children.size()),
                       /*type=*/BinaryenTypeInt32());
}

// Lowers an `IdentExpr` to a `local.get` against the param slot the
// variable was assigned in `LowerToEvalFunction`.  The node's Repr
// annotation drives the result type — it must match the param's
// declared BinaryenType (both are derived from the same user-supplied
// type string upstream, so agreement is by construction).
absl::StatusOr<BinaryenExpressionRef> LowerIdent(LoweringContext& ctx,
                                                 const TypedAst& ast,
                                                 const cel::Expr& expr) {
  const std::string& name = expr.ident_expr().name();
  auto it = ctx.idents.find(name);
  if (it == ctx.idents.end()) {
    return absl::InvalidArgumentError(
        absl::StrCat("expr_lower: identifier `", name,
                     "` was not declared in "
                     "CheckOptions::variable_specs (expr id ",
                     expr.id(), ")"));
  }
  auto repr = ReprOf(ast, expr);
  if (!repr.ok()) return repr.status();
  BinaryenType t = WasmTypeFor(*repr);
  if (t == BinaryenTypeNone()) {
    return absl::UnimplementedError(absl::StrCat(
        "expr_lower: identifier `", name, "` has Repr `", ReprName(*repr),
        "` which has no scalar ABI lowering (expr id ", expr.id(), ")"));
  }
  return BinaryenLocalGet(ctx.mod.raw(), it->second, t);
}

absl::StatusOr<BinaryenExpressionRef> LowerConstant(LoweringContext& ctx,
                                                    const TypedAst& ast,
                                                    const cel::Expr& expr) {
  const cel::Constant& c = expr.const_expr();
  BinaryenModuleRef m = ctx.mod.raw();
  switch (c.kind_case()) {
    case cel::ConstantKindCase::kBool:
      return BinaryenConst(m, BinaryenLiteralInt32(c.bool_value() ? 1 : 0));
    case cel::ConstantKindCase::kInt:
      return BinaryenConst(m, BinaryenLiteralInt64(c.int_value()));
    case cel::ConstantKindCase::kUint:
      return BinaryenConst(
          m, BinaryenLiteralInt64(static_cast<int64_t>(c.uint_value())));
    case cel::ConstantKindCase::kDouble:
      return BinaryenConst(m, BinaryenLiteralFloat64(c.double_value()));
    case cel::ConstantKindCase::kString:
      return LowerSpanLiteral(ctx, c.string_value(), "cel_make_string_view");
    case cel::ConstantKindCase::kBytes:
      return LowerSpanLiteral(ctx, c.bytes_value(), "cel_make_bytes_view");
    default:
      return absl::UnimplementedError(absl::StrCat(
          "expr_lower: constant kind ", static_cast<int>(c.kind_case()),
          " not yet supported (expr id ", expr.id(), ")"));
  }
  (void)ast;
}

// CEL_ERROR kind tag.  Keep in sync with `CelKind` in cel_runtime.h.
// Used by the checked-arithmetic unbox path to recognise an ERROR box.
constexpr int32_t kCelErrorKind = 15;

// Wraps a sret checked-arithmetic helper call so the overall
// expression has the same i64-returning shape as native wasm
// arithmetic.  The helper writes a 24-byte CelValue into the
// scratch slot (pre-allocated at eval entry); codegen loads the
// kind tag inline and traps on CEL_ERROR.
//
// Pattern:
//   (block (result i64)
//     (call $helper (local.get $slot) lhs rhs)
//     (if (i32.eq (i32.load (i32.add base (local.get $slot)))
//                 (i32.const 15))
//       (unreachable))
//     (i64.load offset=8 (i32.add base (local.get $slot))))
//
// "unreachable" surfaces through wasmtime as a trap, which the host
// catches and converts to an `absl::Status` error.  The CEL-correct
// behavior is an observable CelValue ERROR, not a trap; that retrofit
// lands with the 3VL &&/|| rewrite (which forces arithmetic roots to
// propagate status end-to-end).  Until then, "trap on overflow"
// closes the "INT_MAX + 1 is observable" testing-checklist row.
BinaryenExpressionRef EmitCheckedArithmetic(LoweringContext& ctx,
                                            const char* helper,
                                            BinaryenExpressionRef lhs,
                                            BinaryenExpressionRef rhs) {
  BinaryenModuleRef m = ctx.mod.raw();
  const BinaryenIndex slot = ctx.GetScratchSlotLocal();
  BinaryenExpressionRef slot_get =
      BinaryenLocalGet(m, slot, BinaryenTypeInt32());
  BinaryenExpressionRef call_args[3] = {slot_get, lhs, rhs};
  BinaryenExpressionRef call_helper =
      BinaryenCall(m, helper, call_args, 3, BinaryenTypeNone());

  // abs = cel_mem_base() + slot
  auto abs_expr = [&]() {
    BinaryenExpressionRef base_call =
        BinaryenCall(m, "cel_mem_base", /*operands=*/nullptr,
                     /*numOperands=*/0, BinaryenTypeInt32());
    return BinaryenBinary(m, BinaryenAddInt32(), base_call,
                          BinaryenLocalGet(m, slot, BinaryenTypeInt32()));
  };

  // if (kind == CEL_ERROR) unreachable
  BinaryenExpressionRef kind_load =
      BinaryenLoad(m, /*bytes=*/4, /*signed_=*/false, /*offset=*/0,
                   /*align=*/4, BinaryenTypeInt32(), abs_expr(),
                   /*memoryName=*/"memory");
  BinaryenExpressionRef is_error =
      BinaryenBinary(m, BinaryenEqInt32(), kind_load,
                     BinaryenConst(m, BinaryenLiteralInt32(kCelErrorKind)));
  BinaryenExpressionRef trap_if =
      BinaryenIf(m, is_error, BinaryenUnreachable(m), /*ifFalse=*/nullptr);

  // payload.i / payload.u at offset 8.  Signed/unsigned distinction is
  // irrelevant at the wasm load level — the bits are the same; the
  // caller already has the Repr-level type info.
  BinaryenExpressionRef payload_load =
      BinaryenLoad(m, /*bytes=*/8, /*signed_=*/false, /*offset=*/8, /*align=*/8,
                   BinaryenTypeInt64(), abs_expr(), /*memoryName=*/"memory");

  BinaryenExpressionRef children[3] = {call_helper, trap_if, payload_load};
  return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/3,
                       BinaryenTypeInt64());
}

// Name of the checked-arithmetic runtime helper for a given op + signedness.
// Returns nullptr if `name` is not one of ADD/SUBTRACT/MULTIPLY/DIVIDE/MODULO.
const char* CheckedIntHelperName(absl::string_view name, bool is_int) {
  if (name == op::CelOperator::ADD) {
    return is_int ? "cel_int_add_at_ii" : "cel_uint_add_at_uu";
  }
  if (name == op::CelOperator::SUBTRACT) {
    return is_int ? "cel_int_sub_at_ii" : "cel_uint_sub_at_uu";
  }
  if (name == op::CelOperator::MULTIPLY) {
    return is_int ? "cel_int_mul_at_ii" : "cel_uint_mul_at_uu";
  }
  if (name == op::CelOperator::DIVIDE) {
    return is_int ? "cel_int_div_at_ii" : "cel_uint_div_at_uu";
  }
  if (name == op::CelOperator::MODULO) {
    return is_int ? "cel_int_mod_at_ii" : "cel_uint_mod_at_uu";
  }
  return nullptr;
}

// String / bytes `_+_` concatenation.  Both cases go through a runtime
// helper because the result needs a fresh arena-owned CelValue.
BinaryenExpressionRef LowerSpanConcat(Repr r, BinaryenExpressionRef lhs,
                                      BinaryenExpressionRef rhs,
                                      BinaryenModuleRef m) {
  const char* helper =
      (r == Repr::kString) ? "cel_string_concat" : "cel_bytes_concat";
  BinaryenExpressionRef args[2] = {lhs, rhs};
  return BinaryenCall(m, helper, args, 2, BinaryenTypeInt32());
}

// Int / uint arithmetic goes through the checked helpers so overflow
// and div-by-zero become observable (as a trap today, a CEL ERROR
// value once the 3VL retrofit lands).
absl::StatusOr<BinaryenExpressionRef> LowerCheckedIntArith(
    absl::string_view name, Repr r, BinaryenExpressionRef lhs,
    BinaryenExpressionRef rhs, LoweringContext& ctx) {
  const bool is_int = (r == Repr::kInt);
  const char* helper = CheckedIntHelperName(name, is_int);
  if (helper == nullptr) {
    return absl::InternalError(absl::StrCat(
        "LowerCheckedIntArith: unhandled int/uint op `", name, "`"));
  }
  return EmitCheckedArithmetic(ctx, helper, lhs, rhs);
}

// Double arithmetic stays inline — IEEE 754 defines overflow to
// +/-Inf and div-by-zero to +/-Inf / NaN per CEL §langdef.
absl::StatusOr<BinaryenExpressionRef> LowerDoubleArithmetic(
    absl::string_view name, BinaryenExpressionRef lhs,
    BinaryenExpressionRef rhs, BinaryenModuleRef m) {
  if (name == op::CelOperator::ADD) {
    return BinaryenBinary(m, BinaryenAddFloat64(), lhs, rhs);
  }
  if (name == op::CelOperator::SUBTRACT) {
    return BinaryenBinary(m, BinaryenSubFloat64(), lhs, rhs);
  }
  if (name == op::CelOperator::MULTIPLY) {
    return BinaryenBinary(m, BinaryenMulFloat64(), lhs, rhs);
  }
  if (name == op::CelOperator::DIVIDE) {
    return BinaryenBinary(m, BinaryenDivFloat64(), lhs, rhs);
  }
  if (name == op::CelOperator::MODULO) {
    // CEL has no % for double; checker rejects it, but if one sneaks
    // through we surface a clear error rather than emitting bad IR.
    return UnimplementedRepr(name, Repr::kDouble, 0);
  }
  return absl::InternalError(
      absl::StrCat("LowerDoubleArithmetic: unhandled op `", name, "`"));
}

// Binary arithmetic.  Dispatch on the Repr of the first operand (the
// checker guarantees both operands share a type for these overloads).
absl::StatusOr<BinaryenExpressionRef> LowerArithmetic(absl::string_view name,
                                                      Repr r,
                                                      BinaryenExpressionRef lhs,
                                                      BinaryenExpressionRef rhs,
                                                      LoweringContext& ctx) {
  BinaryenModuleRef m = ctx.mod.raw();
  if (name == op::CelOperator::ADD &&
      (r == Repr::kString || r == Repr::kBytes)) {
    return LowerSpanConcat(r, lhs, rhs, m);
  }
  if (r == Repr::kInt || r == Repr::kUint) {
    return LowerCheckedIntArith(name, r, lhs, rhs, ctx);
  }
  if (r == Repr::kDouble) {
    return LowerDoubleArithmetic(name, lhs, rhs, m);
  }
  return UnimplementedRepr(name, r, 0);
}

// Scalar `_==_` / `_!=_` opcode for bool / int / uint / double.  The
// caller handles the span / message cases separately because they need
// a runtime call rather than a single opcode.
absl::StatusOr<BinaryenOp> ScalarEqualityOp(absl::string_view name,
                                            Repr arg_r) {
  const bool eq = (name == op::CelOperator::EQUALS);
  switch (arg_r) {
    case Repr::kBool:
      return eq ? BinaryenEqInt32() : BinaryenNeInt32();
    case Repr::kInt:
    case Repr::kUint:
      return eq ? BinaryenEqInt64() : BinaryenNeInt64();
    case Repr::kDouble:
      return eq ? BinaryenEqFloat64() : BinaryenNeFloat64();
    default:
      return UnimplementedRepr(name, arg_r, 0);
  }
}

// Ordered-compare opcode for int64 operands.
absl::StatusOr<BinaryenOp> OrderedIntOp(absl::string_view name) {
  if (name == op::CelOperator::LESS) return BinaryenLtSInt64();
  if (name == op::CelOperator::LESS_EQUALS) return BinaryenLeSInt64();
  if (name == op::CelOperator::GREATER) return BinaryenGtSInt64();
  if (name == op::CelOperator::GREATER_EQUALS) return BinaryenGeSInt64();
  return absl::InternalError(
      absl::StrCat("OrderedIntOp: not an ordered op: `", name, "`"));
}

absl::StatusOr<BinaryenOp> OrderedUintOp(absl::string_view name) {
  if (name == op::CelOperator::LESS) return BinaryenLtUInt64();
  if (name == op::CelOperator::LESS_EQUALS) return BinaryenLeUInt64();
  if (name == op::CelOperator::GREATER) return BinaryenGtUInt64();
  if (name == op::CelOperator::GREATER_EQUALS) return BinaryenGeUInt64();
  return absl::InternalError(
      absl::StrCat("OrderedUintOp: not an ordered op: `", name, "`"));
}

absl::StatusOr<BinaryenOp> OrderedDoubleOp(absl::string_view name) {
  if (name == op::CelOperator::LESS) return BinaryenLtFloat64();
  if (name == op::CelOperator::LESS_EQUALS) return BinaryenLeFloat64();
  if (name == op::CelOperator::GREATER) return BinaryenGtFloat64();
  if (name == op::CelOperator::GREATER_EQUALS) return BinaryenGeFloat64();
  return absl::InternalError(
      absl::StrCat("OrderedDoubleOp: not an ordered op: `", name, "`"));
}

absl::StatusOr<BinaryenOp> OrderedCompareOp(absl::string_view name,
                                            Repr arg_r) {
  switch (arg_r) {
    case Repr::kInt:
      return OrderedIntOp(name);
    case Repr::kUint:
      return OrderedUintOp(name);
    case Repr::kDouble:
      return OrderedDoubleOp(name);
    default:
      return UnimplementedRepr(name, arg_r, 0);
  }
}

// String / bytes equality can't be a single opcode — both operands
// are i32 offsets to CelValues, and the payload compare must walk
// the bytes in linear memory.  Delegate to the runtime helper and,
// for `_!=_`, invert with `i32.eqz`.
BinaryenExpressionRef LowerSpanEquality(absl::string_view name, Repr arg_r,
                                        BinaryenExpressionRef lhs,
                                        BinaryenExpressionRef rhs,
                                        BinaryenModuleRef m) {
  const char* helper =
      (arg_r == Repr::kString) ? "cel_string_eq" : "cel_bytes_eq";
  BinaryenExpressionRef args[2] = {lhs, rhs};
  BinaryenExpressionRef call =
      BinaryenCall(m, helper, args, 2, BinaryenTypeInt32());
  return (name == op::CelOperator::NOT_EQUALS)
             ? BinaryenUnary(m, BinaryenEqZInt32(), call)
             : call;
}

// Message equality can't be a structural comparison of externrefs —
// two externrefs may point at structurally equal messages from
// different descriptor origins, and the CEL spec demands the host's
// descriptor-aware equality.  Delegate to cel_host.message_eq and,
// for `_!=_`, invert with i32.eqz — same pattern as string/bytes.
BinaryenExpressionRef LowerMessageEquality(absl::string_view name,
                                           BinaryenExpressionRef lhs,
                                           BinaryenExpressionRef rhs,
                                           BinaryenModuleRef m) {
  BinaryenExpressionRef args[2] = {lhs, rhs};
  BinaryenExpressionRef call =
      BinaryenCall(m, "message_eq", args, 2, BinaryenTypeInt32());
  return (name == op::CelOperator::NOT_EQUALS)
             ? BinaryenUnary(m, BinaryenEqZInt32(), call)
             : call;
}

// Ordered double compare with NaN-trap guard (M4 Slice D).  IEEE 754
// defines `NaN < x`, `NaN <= x`, `NaN > x`, `NaN >= x` as all false
// (unordered), but CEL §langdef requires NaN-in-ordered-compare to
// produce ERROR rather than a bogus `false`.  We trap on NaN on
// either side; the observable-ERROR retrofit lands with the 3VL
// `&&` / `||` work.
//
// NaN detection uses `x != x`, which IEEE 754 defines as true iff
// x is NaN (NaN is the only value that compares unequal to itself).
BinaryenExpressionRef LowerDoubleOrderedCompare(LoweringContext& ctx,
                                                BinaryenOp op,
                                                BinaryenExpressionRef lhs,
                                                BinaryenExpressionRef rhs) {
  BinaryenModuleRef m = ctx.mod.raw();
  const BinaryenIndex la = ctx.AddLocal(BinaryenTypeFloat64());
  const BinaryenIndex lb = ctx.AddLocal(BinaryenTypeFloat64());
  auto get_a = [&]() {
    return BinaryenLocalGet(m, la, BinaryenTypeFloat64());
  };
  auto get_b = [&]() {
    return BinaryenLocalGet(m, lb, BinaryenTypeFloat64());
  };
  BinaryenExpressionRef set_a = BinaryenLocalSet(m, la, lhs);
  BinaryenExpressionRef set_b = BinaryenLocalSet(m, lb, rhs);
  BinaryenExpressionRef a_is_nan =
      BinaryenBinary(m, BinaryenNeFloat64(), get_a(), get_a());
  BinaryenExpressionRef b_is_nan =
      BinaryenBinary(m, BinaryenNeFloat64(), get_b(), get_b());
  BinaryenExpressionRef any_nan =
      BinaryenBinary(m, BinaryenOrInt32(), a_is_nan, b_is_nan);
  BinaryenExpressionRef trap_if =
      BinaryenIf(m, any_nan, BinaryenUnreachable(m), /*ifFalse=*/nullptr);
  BinaryenExpressionRef cmp = BinaryenBinary(m, op, get_a(), get_b());
  BinaryenExpressionRef children[4] = {set_a, set_b, trap_if, cmp};
  return BinaryenBlock(m, /*name=*/nullptr, children,
                       /*numChildren=*/4, BinaryenTypeInt32());
}

absl::StatusOr<BinaryenExpressionRef> LowerComparison(absl::string_view name,
                                                      Repr arg_r,
                                                      BinaryenExpressionRef lhs,
                                                      BinaryenExpressionRef rhs,
                                                      LoweringContext& ctx) {
  BinaryenModuleRef m = ctx.mod.raw();
  const bool eq = (name == op::CelOperator::EQUALS);
  const bool ne = (name == op::CelOperator::NOT_EQUALS);
  if (eq || ne) {
    if (arg_r == Repr::kString || arg_r == Repr::kBytes) {
      return LowerSpanEquality(name, arg_r, lhs, rhs, m);
    }
    if (arg_r == Repr::kMessage) {
      return LowerMessageEquality(name, lhs, rhs, m);
    }
    auto bop = ScalarEqualityOp(name, arg_r);
    if (!bop.ok()) return bop.status();
    return BinaryenBinary(m, *bop, lhs, rhs);
  }
  auto bop = OrderedCompareOp(name, arg_r);
  if (!bop.ok()) return bop.status();
  if (arg_r == Repr::kDouble) {
    return LowerDoubleOrderedCompare(ctx, *bop, lhs, rhs);
  }
  return BinaryenBinary(m, *bop, lhs, rhs);
}

// Member-call dispatch for the string extension methods (§9):
// `.startsWith`, `.endsWith`, `.contains`.  All three share the same
// shape: lower the receiver + arg and call a runtime helper returning
// i32 0/1.  Returns `std::nullopt` if `fn` is not one of the three
// string methods; the caller then falls through to the generic
// "member call not implemented" error.
absl::StatusOr<std::optional<BinaryenExpressionRef>> LowerStringMemberCall(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call,
    int64_t expr_id) {
  namespace b = ::cel::builtin;
  const std::string& fn = call.function();
  const char* helper = nullptr;
  if (fn == b::kStringStartsWith) {
    helper = "cel_string_starts_with";
  } else if (fn == b::kStringEndsWith) {
    helper = "cel_string_ends_with";
  } else if (fn == b::kStringContains) {
    helper = "cel_string_contains";
  } else {
    return std::optional<BinaryenExpressionRef>{};
  }

  if (call.args().size() != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("`", fn, "` takes 1 argument, got ", call.args().size(),
                     " (expr id ", expr_id, ")"));
  }
  auto recv_r = ReprOf(ast, call.target());
  if (!recv_r.ok()) return recv_r.status();
  if (*recv_r != Repr::kString) {
    return UnimplementedRepr(fn, *recv_r, expr_id);
  }
  auto recv = LowerExpr(ctx, ast, call.target());
  if (!recv.ok()) return recv.status();
  auto arg = LowerExpr(ctx, ast, call.args().at(0));
  if (!arg.ok()) return arg.status();
  BinaryenExpressionRef args[2] = {*recv, *arg};
  return std::optional<BinaryenExpressionRef>(
      BinaryenCall(ctx.mod.raw(), helper, args, 2, BinaryenTypeInt32()));
}

// `!_` — logical not.
absl::StatusOr<BinaryenExpressionRef> LowerLogicalNot(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call) {
  if (call.args().size() != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("`!_` takes 1 argument, got ", call.args().size()));
  }
  auto v = LowerExpr(ctx, ast, call.args().at(0));
  if (!v.ok()) return v.status();
  return BinaryenUnary(ctx.mod.raw(), BinaryenEqZInt32(), *v);
}

// `-_` — unary negate.  0 - x preserves i64 signedness; CEL also
// rejects negating uint.
absl::StatusOr<BinaryenExpressionRef> LowerNegate(LoweringContext& ctx,
                                                  const TypedAst& ast,
                                                  const cel::CallExpr& call,
                                                  int64_t expr_id) {
  if (call.args().size() != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("`-_` takes 1 argument, got ", call.args().size()));
  }
  auto v = LowerExpr(ctx, ast, call.args().at(0));
  if (!v.ok()) return v.status();
  auto arg_r = ReprOf(ast, call.args().at(0));
  if (!arg_r.ok()) return arg_r.status();
  BinaryenModuleRef m = ctx.mod.raw();
  switch (*arg_r) {
    case Repr::kInt:
      return BinaryenBinary(m, BinaryenSubInt64(),
                            BinaryenConst(m, BinaryenLiteralInt64(0)), *v);
    case Repr::kDouble:
      return BinaryenUnary(m, BinaryenNegFloat64(), *v);
    default:
      return UnimplementedRepr(op::CelOperator::NEGATE, *arg_r, expr_id);
  }
}

// `_?_:_` — ternary.  Both branches must have the same Repr (checker
// invariant); we rely on BinaryenIf to validate type agreement.
absl::StatusOr<BinaryenExpressionRef> LowerConditional(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call) {
  if (call.args().size() != 3) {
    return absl::InvalidArgumentError(
        absl::StrCat("`_?_:_` takes 3 arguments, got ", call.args().size()));
  }
  auto cond = LowerExpr(ctx, ast, call.args().at(0));
  if (!cond.ok()) return cond.status();
  auto t = LowerExpr(ctx, ast, call.args().at(1));
  if (!t.ok()) return t.status();
  auto f = LowerExpr(ctx, ast, call.args().at(2));
  if (!f.ok()) return f.status();
  return BinaryenIf(ctx.mod.raw(), *cond, *t, *f);
}

// `_&&_` / `_||_` — lowered as `if` so the RHS is only evaluated when
// necessary.  This is the 2VL shape; the 3VL retrofit lands later.
absl::StatusOr<BinaryenExpressionRef> LowerShortCircuit(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call,
    absl::string_view fn) {
  if (call.args().size() != 2) {
    return absl::InvalidArgumentError(
        absl::StrCat("`", fn, "` takes 2 arguments, got ", call.args().size()));
  }
  auto l = LowerExpr(ctx, ast, call.args().at(0));
  if (!l.ok()) return l.status();
  auto r = LowerExpr(ctx, ast, call.args().at(1));
  if (!r.ok()) return r.status();
  BinaryenModuleRef m = ctx.mod.raw();
  const bool is_and = (fn == op::CelOperator::LOGICAL_AND);
  BinaryenExpressionRef if_true =
      is_and ? *r : BinaryenConst(m, BinaryenLiteralInt32(1));
  BinaryenExpressionRef if_false =
      is_and ? BinaryenConst(m, BinaryenLiteralInt32(0)) : *r;
  return BinaryenIf(m, *l, if_true, if_false);
}

// `size(x)` — the checker resolves the overload to the exact
// container kind, but the function-name string in the AST is still
// `"size"` regardless of overload.  Dispatch on the argument Repr
// and emit a call to the matching runtime helper.  List / map sizes
// land in later milestones alongside the container Reprs themselves.
absl::StatusOr<BinaryenExpressionRef> LowerSizeCall(LoweringContext& ctx,
                                                    const TypedAst& ast,
                                                    const cel::CallExpr& call,
                                                    int64_t expr_id) {
  auto arg_r = ReprOf(ast, call.args().at(0));
  if (!arg_r.ok()) return arg_r.status();
  auto arg = LowerExpr(ctx, ast, call.args().at(0));
  if (!arg.ok()) return arg.status();
  if (*arg_r == Repr::kString || *arg_r == Repr::kBytes) {
    const char* helper =
        (*arg_r == Repr::kString) ? "cel_string_size" : "cel_bytes_size";
    BinaryenExpressionRef a = *arg;
    return BinaryenCall(ctx.mod.raw(), helper, &a, 1, BinaryenTypeInt64());
  }
  return UnimplementedRepr("size", *arg_r, expr_id);
}

// Binary arithmetic and comparisons: both operands share a Repr;
// dispatch on the first.  If the function name isn't a known binary
// arithmetic or comparison op, falls through to UnimplementedKind.
absl::StatusOr<BinaryenExpressionRef> LowerBinaryCall(LoweringContext& ctx,
                                                      const TypedAst& ast,
                                                      const cel::CallExpr& call,
                                                      absl::string_view fn,
                                                      int64_t expr_id) {
  auto arg_r = ReprOf(ast, call.args().at(0));
  if (!arg_r.ok()) return arg_r.status();
  auto l = LowerExpr(ctx, ast, call.args().at(0));
  if (!l.ok()) return l.status();
  auto r = LowerExpr(ctx, ast, call.args().at(1));
  if (!r.ok()) return r.status();
  if (fn == op::CelOperator::ADD || fn == op::CelOperator::SUBTRACT ||
      fn == op::CelOperator::MULTIPLY || fn == op::CelOperator::DIVIDE ||
      fn == op::CelOperator::MODULO) {
    return LowerArithmetic(fn, *arg_r, *l, *r, ctx);
  }
  if (fn == op::CelOperator::EQUALS || fn == op::CelOperator::NOT_EQUALS ||
      fn == op::CelOperator::LESS || fn == op::CelOperator::LESS_EQUALS ||
      fn == op::CelOperator::GREATER || fn == op::CelOperator::GREATER_EQUALS) {
    return LowerComparison(fn, *arg_r, *l, *r, ctx);
  }
  return UnimplementedKind(absl::StrCat("call `", fn, "`"), expr_id);
}

absl::StatusOr<BinaryenExpressionRef> LowerCall(LoweringContext& ctx,
                                                const TypedAst& ast,
                                                const cel::Expr& expr) {
  const cel::CallExpr& call = expr.call_expr();
  const std::string& fn = call.function();

  // Member form (x.f(...)).  The only ones supported today are the three
  // string extension methods; anything else returns UnimplementedKind so
  // a future method (bytes.startsWith, etc.) is forced to add a case
  // rather than silently resolving to a generic no-op.
  if (call.has_target()) {
    auto member = LowerStringMemberCall(ctx, ast, call, expr.id());
    if (!member.ok()) return member.status();
    if (member->has_value()) return **member;
    return UnimplementedKind(absl::StrCat("method call `", fn, "`"), expr.id());
  }
  if (fn == op::CelOperator::LOGICAL_NOT) {
    return LowerLogicalNot(ctx, ast, call);
  }
  if (fn == op::CelOperator::NEGATE) {
    return LowerNegate(ctx, ast, call, expr.id());
  }
  if (fn == op::CelOperator::CONDITIONAL) {
    return LowerConditional(ctx, ast, call);
  }
  if (fn == op::CelOperator::LOGICAL_AND || fn == op::CelOperator::LOGICAL_OR) {
    return LowerShortCircuit(ctx, ast, call, fn);
  }
  if (fn == "size" && call.args().size() == 1) {
    return LowerSizeCall(ctx, ast, call, expr.id());
  }
  if (call.args().size() == 2) {
    return LowerBinaryCall(ctx, ast, call, fn, expr.id());
  }
  return UnimplementedKind(absl::StrCat("call `", fn, "`"), expr.id());
}

// Builds the payload-load tail of a `kSelectExpr` lowering.  `scratch`
// is a local holding the arena-relative offset of a freshly allocated
// 24-byte CelValue that `cel_host.get_field` has written into.
//
// Scalar kinds load through the absolute address
// `cel_mem_base() + scratch + 8` (payload starts at offset 8 — kind is
// u32 @0, _pad is u32 @4).  String / bytes travel through the codegen
// as arena-relative CelValue offsets, so their "load" is just
// `local.get $scratch`.  Message / enum / dur / ts results are out of
// scope for G2 and return Unimplemented so a future caller can't
// silently forge a bogus payload.
absl::StatusOr<BinaryenExpressionRef> LoadSelectPayload(LoweringContext& ctx,
                                                        BinaryenIndex scratch,
                                                        Repr result_r,
                                                        int64_t expr_id) {
  BinaryenModuleRef m = ctx.mod.raw();
  if (result_r == Repr::kString || result_r == Repr::kBytes) {
    return BinaryenLocalGet(m, scratch, BinaryenTypeInt32());
  }
  BinaryenExpressionRef base_call =
      BinaryenCall(m, "cel_mem_base", /*operands=*/nullptr, /*numOperands=*/0,
                   BinaryenTypeInt32());
  BinaryenExpressionRef abs =
      BinaryenBinary(m, BinaryenAddInt32(), base_call,
                     BinaryenLocalGet(m, scratch, BinaryenTypeInt32()));
  const uint32_t payload_off = 8;
  switch (result_r) {
    case Repr::kBool:
      return BinaryenLoad(m, /*bytes=*/4, /*signed_=*/false,
                          /*offset=*/payload_off, /*align=*/4,
                          BinaryenTypeInt32(), abs, /*memoryName=*/"memory");
    case Repr::kInt:
      return BinaryenLoad(m, /*bytes=*/8, /*signed_=*/true,
                          /*offset=*/payload_off, /*align=*/8,
                          BinaryenTypeInt64(), abs, /*memoryName=*/"memory");
    case Repr::kUint:
      return BinaryenLoad(m, /*bytes=*/8, /*signed_=*/false,
                          /*offset=*/payload_off, /*align=*/8,
                          BinaryenTypeInt64(), abs, /*memoryName=*/"memory");
    case Repr::kDouble:
      return BinaryenLoad(m, /*bytes=*/8, /*signed_=*/false,
                          /*offset=*/payload_off, /*align=*/8,
                          BinaryenTypeFloat64(), abs, /*memoryName=*/"memory");
    case Repr::kMessage: {
      // `cel_unwrap_message` takes the arena-relative CelValue pointer
      // (the scratch slot) and does the msg_slot load + table.get
      // internally, so we hand it `scratch` directly instead of
      // pre-loading the slot — pre-loading would treat the slot index
      // as a CelValue address inside `cel_unwrap_message` and walk off
      // the arena.
      BinaryenExpressionRef cv_arg =
          BinaryenLocalGet(m, scratch, BinaryenTypeInt32());
      return BinaryenCall(m, "cel_unwrap_message", &cv_arg, /*numOperands=*/1,
                          BinaryenTypeExternref());
    }
    default:
      return UnimplementedRepr("SelectExpr payload", result_r, expr_id);
  }
}

// Bundles the two pieces both `LowerSelectField` and `LowerSelectTestOnly`
// need from the operand side: the lowered operand expression and the
// pre-resolved proto field number.
struct SelectOperand {
  BinaryenExpressionRef operand;
  int32_t field_number;
};

// Validates the shared preconditions for a lowered `SelectExpr` (field
// number resolved, operand is a message) and lowers the operand.  Both
// the field-read and `has()` paths go through this so a single error
// message covers the "operand is not a message" and "field not found"
// cases regardless of which path the user wrote.
absl::StatusOr<SelectOperand> LowerSelectOperand(LoweringContext& ctx,
                                                 const TypedAst& ast,
                                                 const cel::SelectExpr& select,
                                                 int64_t expr_id) {
  const NodeAnnotation* a = ast.annotations().Find(expr_id);
  if (a == nullptr || a->field_number == 0) {
    return absl::FailedPreconditionError(absl::StrCat(
        "expr_lower: SelectExpr at id ", expr_id,
        " has no resolved field_number — was PopulateAnnotations run "
        "with a descriptor pool, and does the operand type have a "
        "field named `",
        select.field(), "`?"));
  }
  auto operand_r = ReprOf(ast, select.operand());
  if (!operand_r.ok()) return operand_r.status();
  if (*operand_r != Repr::kMessage) {
    return UnimplementedRepr("SelectExpr operand", *operand_r, expr_id);
  }
  auto operand = LowerExpr(ctx, ast, select.operand());
  if (!operand.ok()) return operand.status();
  return SelectOperand{*operand, static_cast<int32_t>(a->field_number)};
}

// Lowers `has(operand.field)` — a `SelectExpr` with `test_only=true` —
// to a single `cel_host.has_field(operand, field_number) → i32` call.
// No scratch CelValue is needed; the host returns 0/1 directly.  The
// checker guarantees the result is a CEL bool (`Repr::kBool`), which
// maps to i32 in the ABI, so the raw call expression is the block's
// result as-is.
absl::StatusOr<BinaryenExpressionRef> LowerSelectTestOnly(
    LoweringContext& ctx, const TypedAst& ast, const cel::Expr& expr) {
  auto op = LowerSelectOperand(ctx, ast, expr.select_expr(), expr.id());
  if (!op.ok()) return op.status();
  BinaryenModuleRef m = ctx.mod.raw();
  BinaryenExpressionRef args[2] = {
      op->operand,
      BinaryenConst(m, BinaryenLiteralInt32(op->field_number)),
  };
  return BinaryenCall(m, "has_field", args, 2, BinaryenTypeInt32());
}

// Lowers `operand.field` to a three-step block:
//   1. scratch = cel_alloc(24)
//   2. cel_host.get_field(operand, field_number, scratch)
//   3. load the payload slice implied by the result's Repr
//
// Only the flat case (operand is a direct kMessage value) is in G2.
// Nested selects — where the operand is itself a kSelectExpr
// returning a message — need the host to intern the submessage into
// `$cel_refs` and hand codegen back an externref; that path lands in
// G4 together with `message_eq`.
absl::StatusOr<BinaryenExpressionRef> LowerSelectField(LoweringContext& ctx,
                                                       const TypedAst& ast,
                                                       const cel::Expr& expr) {
  auto op = LowerSelectOperand(ctx, ast, expr.select_expr(), expr.id());
  if (!op.ok()) return op.status();
  auto result_r = ReprOf(ast, expr);
  if (!result_r.ok()) return result_r.status();

  BinaryenModuleRef m = ctx.mod.raw();
  const BinaryenIndex scratch = ctx.AddLocal(BinaryenTypeInt32());
  BinaryenExpressionRef alloc_arg = BinaryenConst(m, BinaryenLiteralInt32(24));
  BinaryenExpressionRef alloc_call =
      BinaryenCall(m, "cel_alloc", &alloc_arg, 1, BinaryenTypeInt32());
  BinaryenExpressionRef set_scratch = BinaryenLocalSet(m, scratch, alloc_call);
  BinaryenExpressionRef args[3] = {
      op->operand,
      BinaryenConst(m, BinaryenLiteralInt32(op->field_number)),
      BinaryenLocalGet(m, scratch, BinaryenTypeInt32()),
  };
  BinaryenExpressionRef get_field_call =
      BinaryenCall(m, "get_field", args, 3, BinaryenTypeNone());
  auto load = LoadSelectPayload(ctx, scratch, *result_r, expr.id());
  if (!load.ok()) return load.status();
  BinaryenType result_type = WasmTypeFor(*result_r);
  BinaryenExpressionRef children[3] = {set_scratch, get_field_call, *load};
  return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/3,
                       result_type);
}

absl::StatusOr<BinaryenExpressionRef> LowerSelect(LoweringContext& ctx,
                                                  const TypedAst& ast,
                                                  const cel::Expr& expr) {
  if (expr.select_expr().test_only()) {
    return LowerSelectTestOnly(ctx, ast, expr);
  }
  return LowerSelectField(ctx, ast, expr);
}

absl::StatusOr<BinaryenExpressionRef> LowerExpr(LoweringContext& ctx,
                                                const TypedAst& ast,
                                                const cel::Expr& expr) {
  switch (expr.kind_case()) {
    case cel::ExprKindCase::kConstant:
      return LowerConstant(ctx, ast, expr);
    case cel::ExprKindCase::kCallExpr:
      return LowerCall(ctx, ast, expr);
    case cel::ExprKindCase::kIdentExpr:
      return LowerIdent(ctx, ast, expr);
    case cel::ExprKindCase::kSelectExpr:
      return LowerSelect(ctx, ast, expr);
    case cel::ExprKindCase::kListExpr:
      return UnimplementedKind("ListExpr", expr.id());
    case cel::ExprKindCase::kStructExpr:
      return UnimplementedKind("StructExpr", expr.id());
    case cel::ExprKindCase::kMapExpr:
      return UnimplementedKind("MapExpr", expr.id());
    case cel::ExprKindCase::kComprehensionExpr:
      return UnimplementedKind("ComprehensionExpr", expr.id());
    case cel::ExprKindCase::kUnspecifiedExpr:
      return absl::InvalidArgumentError(
          absl::StrCat("expr_lower: unspecified expr (id ", expr.id(), ")"));
  }
  return absl::InternalError("expr_lower: unreachable kind_case");
}

// Populates `params`, `ctx.idents`, `ctx.num_params`, and sets
// `has_message_param` iff any variable has Repr::kMessage.  Fails
// cleanly on unsupported Reprs or duplicate variable names.
absl::Status BuildParamList(const TypedAst& ast, LoweringContext& ctx,
                            std::vector<BinaryenType>& params,
                            bool& has_message_param) {
  params.reserve(ast.variables().size());
  has_message_param = false;
  for (const Variable& v : ast.variables()) {
    BinaryenType pt = WasmTypeFor(v.repr);
    if (pt == BinaryenTypeNone()) {
      return absl::UnimplementedError(absl::StrCat(
          "expr_lower: variable `", v.name, "` has Repr `", ReprName(v.repr),
          "` which has no scalar ABI lowering in M3"));
    }
    if (v.repr == Repr::kMessage) has_message_param = true;
    const auto idx = static_cast<BinaryenIndex>(params.size());
    params.push_back(pt);
    const bool inserted = ctx.idents.emplace(v.name, idx).second;
    if (!inserted) {
      return absl::InvalidArgumentError(absl::StrCat(
          "expr_lower: duplicate variable name `", v.name, "` in specs"));
    }
  }
  ctx.num_params = static_cast<uint32_t>(params.size());
  return absl::OkStatus();
}

// Any message-valued variable drags in the externref table + the
// wrap/unwrap helpers.  We emit the table with 16 initial slots:
// a single evaluation never pins more than a handful of messages
// (one per `kIdentExpr`, plus transient wraps for field reads), so
// 16 is a comfortable ceiling that still keeps the table's root
// set small for wasmtime.  Slot 0 is the null sentinel, slot 1 is
// the first allocatable slot.
absl::Status AddMessageSupport(WasmModule& mod) {
  if (auto s = AddCelRefsTableAndHelpers(mod, "$cel_refs",
                                         /*initial_slots=*/16);
      !s.ok()) {
    return s;
  }
  return AddMessageWrapHelpers(mod, "$cel_refs");
}

// Wraps `body` with the scratch-slot prologue when the lowering
// allocated a slot.  cel_reset() rewinds the arena between host
// invocations, so the slot must be (re)allocated on every $eval
// entry — the prologue is per-invocation, not global.
BinaryenExpressionRef WithScratchSlotPrologue(LoweringContext& ctx,
                                              BinaryenExpressionRef body,
                                              BinaryenType body_type) {
  if (!ctx.scratch_slot.has_value()) return body;
  BinaryenModuleRef m = ctx.mod.raw();
  BinaryenExpressionRef slot_size = BinaryenConst(
      m, BinaryenLiteralInt32(static_cast<int32_t>(sizeof(uint64_t) * 3)));
  BinaryenExpressionRef alloc_call =
      BinaryenCall(m, "cel_alloc", &slot_size, 1, BinaryenTypeInt32());
  BinaryenExpressionRef set_slot =
      BinaryenLocalSet(m, *ctx.scratch_slot, alloc_call);
  BinaryenExpressionRef children[2] = {set_slot, body};
  return BinaryenBlock(m, /*name=*/nullptr, children,
                       /*numChildren=*/2, body_type);
}

}  // namespace

BinaryenType WasmTypeFor(Repr r) {
  switch (r) {
    // Bool / type / string / bytes all travel as i32 in the ABI.
    // Strings and bytes are offsets into the shared linear memory —
    // each is a pointer to a `CelValue` owned by the runtime's arena
    // (see LowerSpanLiteral).  Types are encoded as internal type-id
    // integers (placeholder — M6).
    case Repr::kBool:
    case Repr::kType:
    case Repr::kString:
    case Repr::kBytes:
      return BinaryenTypeInt32();
    case Repr::kInt:
    case Repr::kUint:
    case Repr::kEnum:
    case Repr::kDuration:
    case Repr::kTimestamp:
      return BinaryenTypeInt64();
    case Repr::kDouble:
      return BinaryenTypeFloat64();
    // Messages travel as `externref`: the host owns the underlying
    // proto object and hands it to the module as an opaque reference.
    // When codegen needs to thread the value through a CelValue* API
    // (equality, size, host calls that return a message), it goes
    // through `cel_wrap_message` — see `compiler/codegen/cel_refs.cc`.
    case Repr::kMessage:
      return BinaryenTypeExternref();
    default:
      return BinaryenTypeNone();
  }
}

absl::StatusOr<LoweredFunction> LowerToEvalFunction(const TypedAst& ast,
                                                    absl::string_view func_name,
                                                    WasmModule& mod) {
  if (!ast.has_ast()) {
    return absl::InvalidArgumentError(
        "LowerToEvalFunction: TypedAst has no underlying cel::Ast");
  }
  const cel::Expr& root = ast.ast().root_expr();
  auto root_r = ReprOf(ast, root);
  if (!root_r.ok()) return root_r.status();
  BinaryenType result_type = WasmTypeFor(*root_r);
  if (result_type == BinaryenTypeNone()) {
    return absl::UnimplementedError(
        absl::StrCat("expr_lower: root Repr `", ReprName(*root_r),
                     "` has no scalar ABI lowering in M2"));
  }
  if (auto s = DeclareRuntimeImports(mod); !s.ok()) return s;
  DeclareHostImports(mod);

  // Build the parameter list from the declared variables.  Each user
  // variable becomes one WASM param whose type is the ABI encoding of
  // its Repr.  Variables whose Repr has no scalar encoding (list, map,
  // dyn) fail cleanly here — their support lives in later milestones.
  std::vector<BinaryenType> params;
  LoweringContext ctx{mod};
  bool has_message_param = false;
  if (auto s = BuildParamList(ast, ctx, params, has_message_param); !s.ok()) {
    return s;
  }
  if (has_message_param) {
    if (auto s = AddMessageSupport(mod); !s.ok()) return s;
  }

  auto body = LowerExpr(ctx, ast, root);
  if (!body.ok()) return body.status();
  mod.AddFunction(func_name, params, result_type,
                  /*local_types=*/ctx.local_types,
                  WithScratchSlotPrologue(ctx, *body, result_type));
  BinaryenFunctionRef fn =
      BinaryenGetFunction(mod.raw(), std::string(func_name).c_str());
  if (fn == nullptr) {
    return absl::InternalError(
        absl::StrCat("expr_lower: BinaryenGetFunction returned null for `",
                     func_name, "` immediately after AddFunction"));
  }
  return LoweredFunction{fn, result_type, *root_r};
}

}  // namespace celwasm
