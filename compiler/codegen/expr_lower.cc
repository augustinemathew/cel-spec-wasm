#include "compiler/codegen/expr_lower.h"

#include <algorithm>
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
#include "compiler/codegen/attribute_pool.h"
#include "compiler/codegen/cel_refs.h"
#include "compiler/codegen/field_name_pool.h"
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

  // Intern table for `(proto_field_number, name)` pairs the lowered
  // expression references.  Every `SelectExpr` lowering calls
  // `field_pool.Intern` to get the i32 the emitted wasm passes to
  // `cel_host.get_field` / `cel_host.has_field`.  The pool is
  // serialized into `CelAbi.fields` so the host can round-trip the
  // IDs back to descriptors (or raw names, for non-proto backings).
  // `BuildCelAbi` walks the AST the same way via `FromTypedAst`, so
  // the two pools produce identical entries by construction.
  FieldNamePool field_pool;

  // Intern table for attribute paths (M4 Slice E2a.1).  Each
  // `kSelectExpr` call site interns its full path (variable + every
  // qualifier from root down to the select) and the returned
  // `attr_id` becomes the third i32 `cel_host.get_field` receives.
  // The host resolves it back to a cel-cpp-style `Attribute` via the
  // `CelAbi.attributes` table and matches it against any unknown
  // patterns the embedder configured.  Same walk order as
  // `field_pool`, so IDs stay in lockstep with `BuildCelAbi`.
  AttributePool attr_pool;

  // Sret eval ABI (M4 Slice C commit 3): param 0 is an i32 offset
  // into the runtime's arena where the eval function must write its
  // final 24-byte CelValue.  Declared variables follow, starting at
  // param index 1, in the order `BuildParamList` inserts them.
  // Kept as a constant rather than a computed field because the
  // slot's position is fixed by the ABI; callers that need it load
  // `local.get 0` (see `EmitSretStore`).
  static constexpr BinaryenIndex kOutSlotParam = 0;
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

  // Pre-body setup expressions emitted by BuildParamList to box raw
  // scalar params (today: bool only) into Repr-matching CelValue
  // offsets before the lowered body runs.  `WithScratchSlotPrologue`
  // prepends these in registration order.
  std::vector<BinaryenExpressionRef> prologue_setups;

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

// Sret-root boxing imports (M4 Slice C commit 3).  The eval function
// writes its final CelValue into a caller-owned 24-byte slot via one
// of these helpers — `cel_box_<repr>_at` for scalars, a 24-byte
// memcpy (`cel_copy_celvalue_at`) for string / bytes / message roots
// whose Repr already travels as a CelValue offset.
void DeclareSretRootImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType i64 = BinaryenTypeInt64();
  const BinaryenType f64 = BinaryenTypeFloat64();
  const BinaryenType none = BinaryenTypeNone();
  ImportCel2(mod, "cel_box_int", i32, i64, none);
  ImportCel2(mod, "cel_box_uint", i32, i64, none);
  ImportCel2(mod, "cel_box_double", i32, f64, none);
  ImportCel2(mod, "cel_copy_celvalue_at", i32, i32, none);
  ImportCel2(mod, "cel_set_error_at", i32, i32, none);
}

// 3VL-aware scalar comparison imports (M4 Slice F1).  Six ops per
// scalar kind (int/uint/double) plus bool eq/ne — 20 helpers total.
// Taken when either operand's subtree can produce CEL_UNKNOWN /
// CEL_ERROR; definite-both-sides expressions stay on the scalar
// fast path.
void DeclareBoxedCmpImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  ImportCel2(mod, "cel_cmp_int_eq", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_int_ne", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_int_lt", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_int_le", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_int_gt", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_int_ge", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_uint_eq", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_uint_ne", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_uint_lt", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_uint_le", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_uint_gt", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_uint_ge", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_double_eq", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_double_ne", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_double_lt", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_double_le", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_double_gt", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_double_ge", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_bool_eq", i32, i32, i32);
  ImportCel2(mod, "cel_cmp_bool_ne", i32, i32, i32);
}

// Uniform-boxed `_v` variants (Slice F Step 4).  Same role as their
// non-`_v` siblings above, but operands and results travel as
// CelValue offsets and the helper absorbs UNKNOWN / ERROR per
// `cel_status_either` before computing.  Codegen routes
// `LowerSpanEquality` / `LowerSpanConcat` / `LowerStringMemberCall`
// through these so wrapping 3VL absorbers see the dominant non-OK
// as a CelValue.  Size stays on the scalar helper at root; its
// boxed route lives in `LowerExprBoxed`.
void DeclareBoxedSpanImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  ImportCel2(mod, "cel_string_eq_v", i32, i32, i32);
  ImportCel2(mod, "cel_bytes_eq_v", i32, i32, i32);
  ImportCel2(mod, "cel_string_concat_v", i32, i32, i32);
  ImportCel2(mod, "cel_bytes_concat_v", i32, i32, i32);
  ImportCel2(mod, "cel_string_starts_with_v", i32, i32, i32);
  ImportCel2(mod, "cel_string_ends_with_v", i32, i32, i32);
  ImportCel2(mod, "cel_string_contains_v", i32, i32, i32);
  ImportCel1(mod, "cel_string_size_v", i32, i32);
  ImportCel1(mod, "cel_bytes_size_v", i32, i32);
  // Step 5: message-equality absorption prologue — codegen composes
  // this with the host's `message_eq` to absorb non-OK message operands.
  ImportCel2(mod, "cel_message_eq_prologue_v", i32, i32, i32);
}

// Allocation / span-literal / equality / size / member-call imports.
void DeclareAllocAndSpanImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType i64 = BinaryenTypeInt64();
  const BinaryenType f64 = BinaryenTypeFloat64();
  // Allocation + string/bytes construction (M3 slice A).
  ImportCel1(mod, "cel_alloc", i32, i32);
  ImportCel1(mod, "cel_make_bool", i32, i32);
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
  DeclareBoxedSpanImports(mod);
  // Scalar-unbox siblings (uniform boxed ABI Step 1): CelValue offset
  // → raw wasm scalar.  Paired with `cel_make_int`/`_uint`/`_double`
  // at param entry so ident reads are always a boxed-then-unboxed
  // round trip — see BuildParamList for the auto-boxing prologue.
  ImportCel1(mod, "cel_int_from_value", i32, i64);
  ImportCel1(mod, "cel_uint_from_value", i32, i64);
  ImportCel1(mod, "cel_double_from_value", i32, f64);
  // Member-call helpers (slice E).  Each returns i32 0/1.
  ImportCel2(mod, "cel_string_starts_with", i32, i32, i32);
  ImportCel2(mod, "cel_string_ends_with", i32, i32, i32);
  ImportCel2(mod, "cel_string_contains", i32, i32, i32);
  // 3VL helpers (M4 Slice C / 3b2): `cel_and`/`cel_or`/`cel_not` take
  // and return CelValue offsets, propagating CEL_UNKNOWN / CEL_ERROR
  // through the logical operators.  Repr::kBool now travels as a
  // CelValue offset precisely so codegen can hand these values to the
  // 3VL helpers without a separate boxing step.
  ImportCel1(mod, "cel_not", i32, i32);
  ImportCel2(mod, "cel_and", i32, i32, i32);
  ImportCel2(mod, "cel_or", i32, i32, i32);
  DeclareBoxedCmpImports(mod);
  // Int boxing helpers (F1 boxed path): scalar → CelValue offset.
  ImportCel1(mod, "cel_make_int", i64, i32);
  ImportCel1(mod, "cel_make_uint", i64, i32);
  ImportCel1(mod, "cel_make_double", f64, i32);
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
  // Boxed-operand variants (Slice F Step 2): `(slot, a_off, b_off)`.
  // Used by `LowerCheckedArithBoxed` so arith inside a boxed
  // absorber sees non-OK operands as values instead of trapping.
  BinaryenType vv_params[3] = {i32, i32, i32};
  auto import_vv = [&](absl::string_view name) {
    mod.AddFunctionImport(name, /*external_module=*/"cel",
                          /*external_base=*/name,
                          absl::Span<const BinaryenType>(vv_params, 3), none);
  };
  import_vv("cel_int_add_at_vv");
  import_vv("cel_int_sub_at_vv");
  import_vv("cel_int_mul_at_vv");
  import_vv("cel_int_div_at_vv");
  import_vv("cel_int_mod_at_vv");
  import_vv("cel_uint_add_at_vv");
  import_vv("cel_uint_sub_at_vv");
  import_vv("cel_uint_mul_at_vv");
  import_vv("cel_uint_div_at_vv");
  import_vv("cel_uint_mod_at_vv");
  BinaryenType neg_v_params[2] = {i32, i32};
  mod.AddFunctionImport("cel_int_neg_at_v", /*external_module=*/"cel",
                        /*external_base=*/"cel_int_neg_at_v",
                        absl::Span<const BinaryenType>(neg_v_params, 2), none);
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
  DeclareSretRootImports(mod);
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
  // get_field(msg, field_intern_id, attr_id, out_cv_offset) → ().
  // The host writes a full 24-byte CelValue at
  // `cel_mem_base() + out_cv_offset`.  `field_intern_id` is an index
  // into `CelAbi.fields`; `attr_id` is an index into
  // `CelAbi.attributes` (partial-eval attribute path, M4 Slice
  // E2a.1).  The host resolves both at instantiation time and uses
  // the attribute path to match against any host-configured unknown
  // patterns — on a FULL match the trampoline writes
  // `CelValue{CEL_UNKNOWN}` and skips the field read.  This keeps
  // the wasm surface proto-agnostic: non-proto backings resolve the
  // intern IDs the same way.
  BinaryenType gf_params[4] = {extref, i32, i32, i32};
  mod.AddFunctionImport("get_field", /*external_module=*/"cel_host",
                        /*external_base=*/"get_field",
                        absl::Span<const BinaryenType>(gf_params, 4),
                        BinaryenTypeNone());
  // has_field(msg, field_intern_id) → i32 (0/1).  `has()` is not yet
  // unknown-aware — its i32 return can't carry CEL_UNKNOWN, and
  // absorbing unknowns through `has()` is tracked with Slice F
  // (3VL-absorption for the non-absorbing operators).  Leaving the
  // arg list at two keeps the codegen emit simple until that slice
  // widens the signature to `(externref, i32, i32) → i32` (attr_id
  // added) and grows a return convention that can encode UNKNOWN.
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

// Forward declarations for the scalar-unbox helpers (definitions live
// below, next to `BoxBool`).  `LowerIdent` calls these to unbox
// auto-boxed int / uint / double params back to raw wasm scalars.
BinaryenExpressionRef UnboxInt(BinaryenModuleRef m,
                               BinaryenExpressionRef boxed);
BinaryenExpressionRef UnboxUint(BinaryenModuleRef m,
                                BinaryenExpressionRef boxed);
BinaryenExpressionRef UnboxDouble(BinaryenModuleRef m,
                                  BinaryenExpressionRef boxed);

// Lowers an `IdentExpr` to a `local.get` against the local slot the
// variable was assigned in `LowerToEvalFunction`.  For scalar reprs
// (bool / int / uint / double) the slot holds a CelValue offset
// (auto-boxed at $eval entry, see BuildParamList); we load the offset
// and immediately unbox to the raw wasm scalar so the rest of codegen
// keeps speaking raw scalars.  This is a runtime no-op (box-then-unbox)
// today — Step 1 of the uniform boxed ABI retrofit moves the source
// of truth to the boxed form without flipping any semantics yet.
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
  BinaryenModuleRef m = ctx.mod.raw();
  // Bool ident reads are already i32 offsets post-3b2 — `LowerIdent`
  // returns the boxed offset directly and the `_?_:_` site calls
  // `cel_bool_from_value` where it needs raw truth.  The other three
  // scalar reprs unbox inline so existing consumers (arithmetic,
  // payload.i loads) continue to see raw wasm scalars.
  switch (*repr) {
    case Repr::kInt:
      return UnboxInt(m, BinaryenLocalGet(m, it->second, BinaryenTypeInt32()));
    case Repr::kUint:
      return UnboxUint(m, BinaryenLocalGet(m, it->second, BinaryenTypeInt32()));
    case Repr::kDouble:
      return UnboxDouble(m,
                         BinaryenLocalGet(m, it->second, BinaryenTypeInt32()));
    default:
      return BinaryenLocalGet(m, it->second, t);
  }
}

absl::StatusOr<BinaryenExpressionRef> LowerConstant(LoweringContext& ctx,
                                                    const TypedAst& ast,
                                                    const cel::Expr& expr) {
  const cel::Constant& c = expr.const_expr();
  BinaryenModuleRef m = ctx.mod.raw();
  switch (c.kind_case()) {
    case cel::ConstantKindCase::kBool: {
      // Repr::kBool travels as a CelValue offset (M4 Slice C / 3b2),
      // so bool literals box through `cel_make_bool` rather than
      // emitting a raw 0/1.  The boxed offset is the one every 3VL
      // helper (`cel_and`/`cel_or`/`cel_not`) and every
      // bool-consuming call site speaks.
      BinaryenExpressionRef raw =
          BinaryenConst(m, BinaryenLiteralInt32(c.bool_value() ? 1 : 0));
      return BinaryenCall(m, "cel_make_bool", &raw, 1, BinaryenTypeInt32());
    }
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
// kind tag inline and, on CEL_ERROR, copies the error box into the
// eval function's sret slot (param 0) and early-returns.  This makes
// arithmetic overflow / divide-by-zero an observable CelValue ERROR
// at the host boundary rather than a wasm trap.
//
// Pattern:
//   (block (result i64)
//     (call $helper (local.get $slot) lhs rhs)
//     (if (i32.eq (i32.load (i32.add base (local.get $slot)))
//                 (i32.const 15))
//       (block
//         (call $cel_copy_celvalue_at (local.get $sret) (local.get $slot))
//         (return)))
//     (i64.load offset=8 (i32.add base (local.get $slot))))
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

  // if (kind == CEL_ERROR) { copy scratch -> sret; return }
  BinaryenExpressionRef kind_load =
      BinaryenLoad(m, /*bytes=*/4, /*signed_=*/false, /*offset=*/0,
                   /*align=*/4, BinaryenTypeInt32(), abs_expr(),
                   /*memoryName=*/"memory");
  BinaryenExpressionRef is_error =
      BinaryenBinary(m, BinaryenEqInt32(), kind_load,
                     BinaryenConst(m, BinaryenLiteralInt32(kCelErrorKind)));
  BinaryenExpressionRef copy_args[2] = {
      BinaryenLocalGet(m, LoweringContext::kOutSlotParam, BinaryenTypeInt32()),
      BinaryenLocalGet(m, slot, BinaryenTypeInt32()),
  };
  BinaryenExpressionRef copy_call =
      BinaryenCall(m, "cel_copy_celvalue_at", copy_args, 2, BinaryenTypeNone());
  BinaryenExpressionRef ret = BinaryenReturn(m, /*value=*/nullptr);
  BinaryenExpressionRef err_children[2] = {copy_call, ret};
  BinaryenExpressionRef err_block = BinaryenBlock(
      m, /*name=*/nullptr, err_children, /*numChildren=*/2, BinaryenTypeNone());
  BinaryenExpressionRef err_if =
      BinaryenIf(m, is_error, err_block, /*ifFalse=*/nullptr);

  // payload.i / payload.u at offset 8.  Signed/unsigned distinction is
  // irrelevant at the wasm load level — the bits are the same; the
  // caller already has the Repr-level type info.
  BinaryenExpressionRef payload_load =
      BinaryenLoad(m, /*bytes=*/8, /*signed_=*/false, /*offset=*/8, /*align=*/8,
                   BinaryenTypeInt64(), abs_expr(), /*memoryName=*/"memory");

  BinaryenExpressionRef children[3] = {call_helper, err_if, payload_load};
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

// Boxed-operand counterpart (Slice F Step 2).  Each helper writes its
// result (OK / ERROR / UNKNOWN) into the scratch slot rather than
// requiring kind-check-early-return at the call site, so nested arith
// inside a 3VL absorber can propagate dominant non-OK up through the
// boxed comparison / `cel_or` / `cel_and` chain.
const char* CheckedIntHelperNameV(absl::string_view name, bool is_int) {
  if (name == op::CelOperator::ADD) {
    return is_int ? "cel_int_add_at_vv" : "cel_uint_add_at_vv";
  }
  if (name == op::CelOperator::SUBTRACT) {
    return is_int ? "cel_int_sub_at_vv" : "cel_uint_sub_at_vv";
  }
  if (name == op::CelOperator::MULTIPLY) {
    return is_int ? "cel_int_mul_at_vv" : "cel_uint_mul_at_vv";
  }
  if (name == op::CelOperator::DIVIDE) {
    return is_int ? "cel_int_div_at_vv" : "cel_uint_div_at_vv";
  }
  if (name == op::CelOperator::MODULO) {
    return is_int ? "cel_int_mod_at_vv" : "cel_uint_mod_at_vv";
  }
  return nullptr;
}

// String / bytes `_+_` concatenation.  Both cases go through a runtime
// helper because the result needs a fresh arena-owned CelValue.  Uses
// the `_v` absorbing variant (Slice F Step 4) so an UNKNOWN / ERROR
// operand surfaces as a CelValue offset the wrapping 3VL absorber can
// see — the old helper returned 0 on non-OK input, which `cel_or` /
// `cel_and` would have mistaken for a type error.
BinaryenExpressionRef LowerSpanConcat(Repr r, BinaryenExpressionRef lhs,
                                      BinaryenExpressionRef rhs,
                                      BinaryenModuleRef m) {
  const char* helper =
      (r == Repr::kString) ? "cel_string_concat_v" : "cel_bytes_concat_v";
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

// Boxes a raw wasm i32 0/1 into a Repr::kBool CelValue offset via
// `cel_make_bool`.  String / bytes / message equality helpers return
// a raw i32 and wrap through this before handing control back to the
// boxed-ABI surface.
BinaryenExpressionRef BoxBool(BinaryenModuleRef m, BinaryenExpressionRef raw) {
  return BinaryenCall(m, "cel_make_bool", &raw, 1, BinaryenTypeInt32());
}

// Scalar-unbox siblings for the uniform boxed ABI (Step 1).  Each takes
// a CelValue offset and returns the raw wasm scalar payload of the
// expected kind.  Consumers of int / uint / double ident reads call
// these so the rest of codegen keeps speaking raw scalars while the
// param ABI stays boxed.
BinaryenExpressionRef UnboxInt(BinaryenModuleRef m,
                               BinaryenExpressionRef boxed) {
  return BinaryenCall(m, "cel_int_from_value", &boxed, 1, BinaryenTypeInt64());
}

BinaryenExpressionRef UnboxUint(BinaryenModuleRef m,
                                BinaryenExpressionRef boxed) {
  return BinaryenCall(m, "cel_uint_from_value", &boxed, 1, BinaryenTypeInt64());
}

BinaryenExpressionRef UnboxDouble(BinaryenModuleRef m,
                                  BinaryenExpressionRef boxed) {
  return BinaryenCall(m, "cel_double_from_value", &boxed, 1,
                      BinaryenTypeFloat64());
}

// String / bytes equality can't be a single opcode — both operands
// are i32 offsets to CelValues, and the payload compare must walk
// the bytes in linear memory.  Delegates to the `_v` absorbing helper
// (Slice F Step 4), which returns a CelValue offset (CEL_BOOL on OK,
// or the dominant non-OK status via `cel_status_either`).  For `_!=_`
// we wrap the result in `cel_not` rather than raw `i32.eqz` so
// UNKNOWN / ERROR propagates unchanged instead of collapsing to a
// bogus bool.
BinaryenExpressionRef LowerSpanEquality(absl::string_view name, Repr arg_r,
                                        BinaryenExpressionRef lhs,
                                        BinaryenExpressionRef rhs,
                                        BinaryenModuleRef m) {
  const char* helper =
      (arg_r == Repr::kString) ? "cel_string_eq_v" : "cel_bytes_eq_v";
  BinaryenExpressionRef args[2] = {lhs, rhs};
  BinaryenExpressionRef call =
      BinaryenCall(m, helper, args, 2, BinaryenTypeInt32());
  if (name == op::CelOperator::NOT_EQUALS) {
    BinaryenExpressionRef not_args[1] = {call};
    return BinaryenCall(m, "cel_not", not_args, 1, BinaryenTypeInt32());
  }
  return call;
}

// Message equality can't be a structural comparison of externrefs —
// two externrefs may point at structurally equal messages from
// different descriptor origins, and the CEL spec demands the host's
// descriptor-aware equality.  Delegate to cel_host.message_eq and,
// for `_!=_`, invert with i32.eqz — same pattern as string/bytes.
//
// Superseded by `LowerMessageEqualityBoxed` (Slice F Step 5) for
// in-tree consumers; kept only until the step 7 sweep removes its
// last call sites.
BinaryenExpressionRef LowerMessageEquality(absl::string_view name,
                                           BinaryenExpressionRef lhs,
                                           BinaryenExpressionRef rhs,
                                           BinaryenModuleRef m) {
  BinaryenExpressionRef args[2] = {lhs, rhs};
  BinaryenExpressionRef call =
      BinaryenCall(m, "message_eq", args, 2, BinaryenTypeInt32());
  BinaryenExpressionRef raw = (name == op::CelOperator::NOT_EQUALS)
                                  ? BinaryenUnary(m, BinaryenEqZInt32(), call)
                                  : call;
  return BoxBool(m, raw);
}

// String / bytes / message eq/ne fallback.  Scalar and bool compares
// (int / uint / double / bool, both eq/ne and ordered) are handled
// upstream in `LowerBinaryCall` via the uniform boxed path through
// `LowerBoxedComparison` — the checker rules out ordered compare on
// these Reprs, so only eq/ne reaches here.  Steps 4 / 5 will move
// these onto CelValue-offset helpers too.
absl::StatusOr<BinaryenExpressionRef> LowerComparison(absl::string_view name,
                                                      Repr arg_r,
                                                      BinaryenExpressionRef lhs,
                                                      BinaryenExpressionRef rhs,
                                                      LoweringContext& ctx) {
  BinaryenModuleRef m = ctx.mod.raw();
  if (arg_r == Repr::kString || arg_r == Repr::kBytes) {
    return LowerSpanEquality(name, arg_r, lhs, rhs, m);
  }
  if (arg_r == Repr::kMessage) {
    return LowerMessageEquality(name, lhs, rhs, m);
  }
  return UnimplementedRepr(name, arg_r, 0);
}

// ---- Slice F1: 3VL-aware boxed comparison path ---------------------------
//
// When either comparison operand's subtree can leave a non-OK CelValue
// (UNKNOWN / ERROR) in a scratch slot, we can't use the scalar-fast-path
// lowering above: the scalar opcode expects raw i64 / f64 payloads, and
// the producer would have to early-return from `$eval` before the
// comparison ever sees the non-OK.  The boxed path lowers both operands
// as CelValue offsets, then dispatches to `cel_cmp_<kind>_<op>`, which
// forwards `cel_status_either(a, b)` when either side is non-OK so
// wrapping `&&` / `||` can apply CEL's absorption rules.

// Bundles the pieces both `LowerSelectField` and `LowerSelectTestOnly`
// need from the operand side: the lowered operand expression, the
// `(field_number, field_name)` intern-ID the host receives as
// `field_intern_id`, and the attribute-path intern-ID the host uses
// to look up the select site in `CelAbi.attributes` for partial-eval
// pattern matching.  Defined here rather than next to
// `LowerSelectOperand` below because F1's `LowerSelectFieldBoxed`
// dereferences it at this higher position in the TU.
struct SelectOperand {
  BinaryenExpressionRef operand;
  int32_t intern_id;
  int32_t attr_id;
};

// Forward decls: these helpers are defined later in the file but
// `LowerSelectFieldBoxed` + `LowerCheckedArithBoxed` call them from
// inside the F1 boxed-path block.
absl::StatusOr<SelectOperand> LowerSelectOperand(LoweringContext& ctx,
                                                 const TypedAst& ast,
                                                 const cel::SelectExpr& select,
                                                 int64_t expr_id);
absl::StatusOr<BinaryenExpressionRef> LowerExprBoxed(LoweringContext& ctx,
                                                     const TypedAst& ast,
                                                     const cel::Expr& expr);
absl::StatusOr<BinaryenExpressionRef> LowerConditionalBoxed(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call);

// Select-of-field variant that returns the scratch offset directly (no
// `EmitSretEarlyReturnIfNonOk`, no payload load).  Used by the boxed
// comparison path so the caller's 3VL-aware helper can see an UNKNOWN
// / ERROR CelValue as a value rather than short-circuiting `$eval`.
absl::StatusOr<BinaryenExpressionRef> LowerSelectFieldBoxed(
    LoweringContext& ctx, const TypedAst& ast, const cel::Expr& expr) {
  auto op = LowerSelectOperand(ctx, ast, expr.select_expr(), expr.id());
  if (!op.ok()) return op.status();
  BinaryenModuleRef m = ctx.mod.raw();
  const BinaryenIndex scratch = ctx.AddLocal(BinaryenTypeInt32());
  BinaryenExpressionRef alloc_arg = BinaryenConst(m, BinaryenLiteralInt32(24));
  BinaryenExpressionRef alloc_call =
      BinaryenCall(m, "cel_alloc", &alloc_arg, 1, BinaryenTypeInt32());
  BinaryenExpressionRef set_scratch = BinaryenLocalSet(m, scratch, alloc_call);
  BinaryenExpressionRef args[4] = {
      op->operand,
      BinaryenConst(m, BinaryenLiteralInt32(op->intern_id)),
      BinaryenConst(m, BinaryenLiteralInt32(op->attr_id)),
      BinaryenLocalGet(m, scratch, BinaryenTypeInt32()),
  };
  BinaryenExpressionRef get_field_call =
      BinaryenCall(m, "get_field", args, 4, BinaryenTypeNone());
  BinaryenExpressionRef children[3] = {
      set_scratch, get_field_call,
      BinaryenLocalGet(m, scratch, BinaryenTypeInt32())};
  return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/3,
                       BinaryenTypeInt32());
}

// Checked int/uint arithmetic variant that returns the scratch offset
// directly (no kind-check, no payload load).  Operands are lowered
// through `LowerExprBoxed` so nested arith / select / ident subtrees
// arrive as CelValue offsets; the `_at_vv` helper absorbs dominant
// non-OK via `cel_status_either` before performing the scalar op.
// This is the path rows 4 / 18 / 22 take through a wrapping
// `LowerBoxedComparison` / `cel_or` / `cel_and`.
absl::StatusOr<BinaryenExpressionRef> LowerCheckedArithBoxed(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call,
    Repr r) {
  const bool is_int = (r == Repr::kInt);
  const char* helper = CheckedIntHelperNameV(call.function(), is_int);
  if (helper == nullptr) {
    return absl::InternalError(absl::StrCat(
        "LowerCheckedArithBoxed: unhandled op `", call.function(), "`"));
  }
  auto lhs = LowerExprBoxed(ctx, ast, call.args().at(0));
  if (!lhs.ok()) return lhs.status();
  auto rhs = LowerExprBoxed(ctx, ast, call.args().at(1));
  if (!rhs.ok()) return rhs.status();
  BinaryenModuleRef m = ctx.mod.raw();
  const BinaryenIndex scratch = ctx.AddLocal(BinaryenTypeInt32());
  BinaryenExpressionRef alloc_arg = BinaryenConst(m, BinaryenLiteralInt32(24));
  BinaryenExpressionRef alloc_call =
      BinaryenCall(m, "cel_alloc", &alloc_arg, 1, BinaryenTypeInt32());
  BinaryenExpressionRef set_scratch = BinaryenLocalSet(m, scratch, alloc_call);
  BinaryenExpressionRef call_args[3] = {
      BinaryenLocalGet(m, scratch, BinaryenTypeInt32()), *lhs, *rhs};
  BinaryenExpressionRef helper_call =
      BinaryenCall(m, helper, call_args, 3, BinaryenTypeNone());
  BinaryenExpressionRef children[3] = {
      set_scratch, helper_call,
      BinaryenLocalGet(m, scratch, BinaryenTypeInt32())};
  return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/3,
                       BinaryenTypeInt32());
}

// Lowers `expr` to a CelValue offset regardless of its static Repr.
// Mirrors `LowerExpr` but keeps everything in the boxed ABI so a
// wrapping 3VL-aware consumer (`cel_cmp_*`, `cel_and`, ...) can see
// UNKNOWN / ERROR operand values.  Reprs that already travel as
// `size(s)` / `size(b)` in the boxed path routes through the absorbing
// `_v` variant so an UNKNOWN / ERROR operand surfaces as the CelValue
// the wrapping absorber (`cel_or` / `cel_cmp_int_*`) can see.  Scalar-
// path consumers (root, scalar arith) keep using the i64-returning
// `cel_string_size` / `cel_bytes_size` in `LowerSizeCall`; step 7's
// sweep collapses the two when Repr::kInt everywhere travels boxed.
// Returns nullopt if the call isn't a span-arg size().
absl::StatusOr<std::optional<BinaryenExpressionRef>> LowerSizeCallBoxed(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call) {
  if (call.function() != "size" || call.args().size() != 1) {
    return std::optional<BinaryenExpressionRef>{};
  }
  auto arg_r = ReprOf(ast, call.args().at(0));
  if (!arg_r.ok()) return arg_r.status();
  if (*arg_r != Repr::kString && *arg_r != Repr::kBytes) {
    return std::optional<BinaryenExpressionRef>{};
  }
  auto arg = LowerExpr(ctx, ast, call.args().at(0));
  if (!arg.ok()) return arg.status();
  const char* helper =
      (*arg_r == Repr::kString) ? "cel_string_size_v" : "cel_bytes_size_v";
  BinaryenExpressionRef a = *arg;
  return std::optional<BinaryenExpressionRef>(
      BinaryenCall(ctx.mod.raw(), helper, &a, 1, BinaryenTypeInt32()));
}

// kMessage is the one existing CelValue-tracked Repr that isn't
// payload_is_offset: `LowerSelectField(kMessage)` does a cel_unwrap
// load and `LowerIdent(kMessage)` returns the externref param
// directly.  Boxed consumers want a CelValue offset instead, so
// route select-sourced messages through the offset-returning
// `LowerSelectFieldBoxed`; everything else gets wrapped back to an
// offset via `cel_wrap_message` (externref → CelValue offset).
absl::StatusOr<BinaryenExpressionRef> LowerMessageBoxed(LoweringContext& ctx,
                                                        const TypedAst& ast,
                                                        const cel::Expr& expr) {
  if (expr.kind_case() == cel::ExprKindCase::kSelectExpr &&
      !expr.select_expr().test_only()) {
    return LowerSelectFieldBoxed(ctx, ast, expr);
  }
  auto raw = LowerExpr(ctx, ast, expr);
  if (!raw.ok()) return raw.status();
  BinaryenExpressionRef arg = *raw;
  return BinaryenCall(ctx.mod.raw(), "cel_wrap_message", &arg, 1,
                      BinaryenTypeInt32());
}

// Literal / ident / pure-expression fallback: lower normally and
// wrap the scalar with `cel_make_<kind>`.  The wrapped constructor
// allocates a fresh 24-byte CelValue per call — fine for F1 since
// arenas are reset between evals and the scratch lifetimes are short.
absl::StatusOr<BinaryenExpressionRef> LowerScalarBoxedFallback(
    LoweringContext& ctx, const TypedAst& ast, const cel::Expr& expr, Repr r) {
  auto raw = LowerExpr(ctx, ast, expr);
  if (!raw.ok()) return raw.status();
  BinaryenModuleRef m = ctx.mod.raw();
  BinaryenExpressionRef raw_ref = *raw;
  const BinaryenType i32 = BinaryenTypeInt32();
  switch (r) {
    case Repr::kInt:
      return BinaryenCall(m, "cel_make_int", &raw_ref, 1, i32);
    case Repr::kUint:
      return BinaryenCall(m, "cel_make_uint", &raw_ref, 1, i32);
    case Repr::kDouble:
      return BinaryenCall(m, "cel_make_double", &raw_ref, 1, i32);
    default:
      break;
  }
  return absl::UnimplementedError(
      absl::StrCat("LowerExprBoxed: unsupported Repr `", ReprName(r),
                   "` for expr id ", expr.id()));
}

// Scalar-valued producers whose normal path either early-returns
// from `$eval` on non-OK (Select-of-scalar) or emits a kind-check
// that loses the error box (checked arith).  Returns engaged only
// if `expr` is one of those shapes; otherwise returns nullopt and
// the caller falls through to `LowerScalarBoxedFallback`.
absl::StatusOr<std::optional<BinaryenExpressionRef>> LowerScalarProducerBoxed(
    LoweringContext& ctx, const TypedAst& ast, const cel::Expr& expr, Repr r) {
  if (expr.kind_case() == cel::ExprKindCase::kSelectExpr &&
      !expr.select_expr().test_only()) {
    auto v = LowerSelectFieldBoxed(ctx, ast, expr);
    if (!v.ok()) return v.status();
    return std::optional<BinaryenExpressionRef>(*v);
  }
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return std::nullopt;
  const cel::CallExpr& call = expr.call_expr();
  const std::string& fn = call.function();
  if ((fn == op::CelOperator::ADD || fn == op::CelOperator::SUBTRACT ||
       fn == op::CelOperator::MULTIPLY || fn == op::CelOperator::DIVIDE ||
       fn == op::CelOperator::MODULO) &&
      (r == Repr::kInt || r == Repr::kUint)) {
    auto v = LowerCheckedArithBoxed(ctx, ast, call, r);
    if (!v.ok()) return v.status();
    return std::optional<BinaryenExpressionRef>(*v);
  }
  return LowerSizeCallBoxed(ctx, ast, call);
}

// CelValue offsets (kBool, kString, kBytes, kMessage, kNull) fall
// through to `LowerExpr`; scalar Reprs (kInt / kUint / kDouble) are
// boxed via `cel_make_<kind>` or, for producer nodes whose scalar path
// early-returns on non-OK (Select-of-scalar, checked arith), via
// dedicated `*Boxed` variants that return the scratch offset.
absl::StatusOr<BinaryenExpressionRef> LowerExprBoxed(LoweringContext& ctx,
                                                     const TypedAst& ast,
                                                     const cel::Expr& expr) {
  auto repr = ReprOf(ast, expr);
  if (!repr.ok()) return repr.status();
  // Ternary routes through the boxed form regardless of Repr (Slice F
  // step 6).  Scalar `LowerConditional` early-returns from `$eval` on
  // non-OK cond, which bypasses every wrapping absorber.  Rows 8 / 19
  // of the plan — the absorber must see the UNKNOWN / ERROR as a
  // value, not be skipped.
  if (expr.kind_case() == cel::ExprKindCase::kCallExpr &&
      expr.call_expr().function() == op::CelOperator::CONDITIONAL) {
    return LowerConditionalBoxed(ctx, ast, expr.call_expr());
  }
  // Reprs that already travel as CelValue offsets — normal LowerExpr
  // works.  Select-of-bool/string/bytes returns the scratch offset
  // today (no early-return — see `LowerSelectField`'s
  // `payload_is_offset` branch).
  if (*repr == Repr::kBool || *repr == Repr::kString || *repr == Repr::kBytes ||
      *repr == Repr::kNull) {
    return LowerExpr(ctx, ast, expr);
  }
  if (*repr == Repr::kMessage) return LowerMessageBoxed(ctx, ast, expr);
  auto producer = LowerScalarProducerBoxed(ctx, ast, expr, *repr);
  if (!producer.ok()) return producer.status();
  if (producer->has_value()) return **producer;
  return LowerScalarBoxedFallback(ctx, ast, expr, *repr);
}

// Helper-name tables for the F1 boxed comparison dispatch.
const char* BoxedCmpIntHelper(absl::string_view op) {
  if (op == op::CelOperator::EQUALS) return "cel_cmp_int_eq";
  if (op == op::CelOperator::NOT_EQUALS) return "cel_cmp_int_ne";
  if (op == op::CelOperator::LESS) return "cel_cmp_int_lt";
  if (op == op::CelOperator::LESS_EQUALS) return "cel_cmp_int_le";
  if (op == op::CelOperator::GREATER) return "cel_cmp_int_gt";
  if (op == op::CelOperator::GREATER_EQUALS) return "cel_cmp_int_ge";
  return nullptr;
}
const char* BoxedCmpUintHelper(absl::string_view op) {
  if (op == op::CelOperator::EQUALS) return "cel_cmp_uint_eq";
  if (op == op::CelOperator::NOT_EQUALS) return "cel_cmp_uint_ne";
  if (op == op::CelOperator::LESS) return "cel_cmp_uint_lt";
  if (op == op::CelOperator::LESS_EQUALS) return "cel_cmp_uint_le";
  if (op == op::CelOperator::GREATER) return "cel_cmp_uint_gt";
  if (op == op::CelOperator::GREATER_EQUALS) return "cel_cmp_uint_ge";
  return nullptr;
}
const char* BoxedCmpDoubleHelper(absl::string_view op) {
  if (op == op::CelOperator::EQUALS) return "cel_cmp_double_eq";
  if (op == op::CelOperator::NOT_EQUALS) return "cel_cmp_double_ne";
  if (op == op::CelOperator::LESS) return "cel_cmp_double_lt";
  if (op == op::CelOperator::LESS_EQUALS) return "cel_cmp_double_le";
  if (op == op::CelOperator::GREATER) return "cel_cmp_double_gt";
  if (op == op::CelOperator::GREATER_EQUALS) return "cel_cmp_double_ge";
  return nullptr;
}
const char* BoxedCmpHelper(absl::string_view op, Repr r) {
  switch (r) {
    case Repr::kInt:
      return BoxedCmpIntHelper(op);
    case Repr::kUint:
      return BoxedCmpUintHelper(op);
    case Repr::kDouble:
      return BoxedCmpDoubleHelper(op);
    case Repr::kBool:
      if (op == op::CelOperator::EQUALS) return "cel_cmp_bool_eq";
      if (op == op::CelOperator::NOT_EQUALS) return "cel_cmp_bool_ne";
      return nullptr;
    default:
      return nullptr;
  }
}

// OK branch of message equality: cel_make_bool(message_eq(
//   cel_unwrap_message(a_local), cel_unwrap_message(b_local))), with
// an optional `i32.eqz` flip for `_!=_`.
BinaryenExpressionRef BuildMessageEqOkBranch(BinaryenModuleRef m,
                                             BinaryenIndex a_local,
                                             BinaryenIndex b_local,
                                             bool is_not_equals) {
  const BinaryenType i32 = BinaryenTypeInt32();
  BinaryenExpressionRef ua_arg = BinaryenLocalGet(m, a_local, i32);
  BinaryenExpressionRef ub_arg = BinaryenLocalGet(m, b_local, i32);
  BinaryenExpressionRef me_args[2] = {
      BinaryenCall(m, "cel_unwrap_message", &ua_arg, 1,
                   BinaryenTypeExternref()),
      BinaryenCall(m, "cel_unwrap_message", &ub_arg, 1,
                   BinaryenTypeExternref()),
  };
  BinaryenExpressionRef eq_call =
      BinaryenCall(m, "message_eq", me_args, 2, i32);
  BinaryenExpressionRef raw =
      is_not_equals ? BinaryenUnary(m, BinaryenEqZInt32(), eq_call) : eq_call;
  return BinaryenCall(m, "cel_make_bool", &raw, 1, i32);
}

// Boxed-operand message equality (Slice F Step 5).  Row 14 of the
// plan: `msg.sub_msg == other || true` must absorb an UNKNOWN / ERROR
// sub-message into the wrapping `||`.  Shape:
//
//   a_local = LowerExprBoxed(lhs)        ; CelValue offset
//   b_local = LowerExprBoxed(rhs)
//   p_local = cel_message_eq_prologue_v(a_local, b_local)
//   if p_local != 0 { p_local }          ; propagate non-OK verbatim
//   else { cel_make_bool(message_eq(cel_unwrap_message(a_local),
//                                    cel_unwrap_message(b_local))) }
//
// `cel_wrap_message` / `cel_unwrap_message` are expr-module helpers
// defined in `cel_refs.cc`.
absl::StatusOr<BinaryenExpressionRef> LowerMessageEqualityBoxed(
    LoweringContext& ctx, const TypedAst& ast, absl::string_view name,
    const cel::CallExpr& call) {
  if (call.args().size() != 2) {
    return absl::InvalidArgumentError(absl::StrCat(
        "`", name, "` takes 2 arguments, got ", call.args().size()));
  }
  auto lhs = LowerExprBoxed(ctx, ast, call.args().at(0));
  if (!lhs.ok()) return lhs.status();
  auto rhs = LowerExprBoxed(ctx, ast, call.args().at(1));
  if (!rhs.ok()) return rhs.status();
  BinaryenModuleRef m = ctx.mod.raw();
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenIndex a_local = ctx.AddLocal(i32);
  const BinaryenIndex b_local = ctx.AddLocal(i32);
  const BinaryenIndex p_local = ctx.AddLocal(i32);
  BinaryenExpressionRef prol_args[2] = {
      BinaryenLocalGet(m, a_local, i32),
      BinaryenLocalGet(m, b_local, i32),
  };
  BinaryenExpressionRef ok_branch = BuildMessageEqOkBranch(
      m, a_local, b_local, name == op::CelOperator::NOT_EQUALS);
  // BinaryenIf: if (cond != 0) non_ok else ok.  `cond` is 0 on OK
  // (prologue returned 0), non-zero otherwise.
  BinaryenExpressionRef children[4] = {
      BinaryenLocalSet(m, a_local, *lhs),
      BinaryenLocalSet(m, b_local, *rhs),
      BinaryenLocalSet(
          m, p_local,
          BinaryenCall(m, "cel_message_eq_prologue_v", prol_args, 2, i32)),
      BinaryenIf(m, BinaryenLocalGet(m, p_local, i32),
                 BinaryenLocalGet(m, p_local, i32), ok_branch),
  };
  return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/4, i32);
}

// Boxed-operand comparison: both operands arrive as CelValue offsets
// via `LowerExprBoxed`, and the runtime helper returns a CelValue
// offset (Repr::kBool or the propagated UNKNOWN / ERROR).  Under the
// uniform-boxed ABI (Step 3), every scalar / bool comparison lowers
// through here — there is no scalar fast path.  NaN-in-ordered-
// compare ERROR propagation is handled by `cel_cmp_double_{lt,le,
// gt,ge}` in the runtime, not in codegen.
absl::StatusOr<BinaryenExpressionRef> LowerBoxedComparison(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call,
    absl::string_view fn, Repr arg_r) {
  const char* helper = BoxedCmpHelper(fn, arg_r);
  if (helper == nullptr) {
    return absl::UnimplementedError(
        absl::StrCat("LowerBoxedComparison: no 3VL helper for `", fn,
                     "` on Repr `", ReprName(arg_r), "`"));
  }
  auto lhs = LowerExprBoxed(ctx, ast, call.args().at(0));
  if (!lhs.ok()) return lhs.status();
  auto rhs = LowerExprBoxed(ctx, ast, call.args().at(1));
  if (!rhs.ok()) return rhs.status();
  BinaryenExpressionRef args[2] = {*lhs, *rhs};
  return BinaryenCall(ctx.mod.raw(), helper, args, 2, BinaryenTypeInt32());
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
    helper = "cel_string_starts_with_v";
  } else if (fn == b::kStringEndsWith) {
    helper = "cel_string_ends_with_v";
  } else if (fn == b::kStringContains) {
    helper = "cel_string_contains_v";
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
  // `_v` helper returns a CelValue offset directly (CEL_BOOL on OK,
  // dominant non-OK on absorption); no `cel_make_bool` wrap needed.
  return std::optional<BinaryenExpressionRef>(
      BinaryenCall(ctx.mod.raw(), helper, args, 2, BinaryenTypeInt32()));
}

// `!_` — logical not.  3VL (M4 Slice C / 3b2): calls `cel_not` so
// CEL_UNKNOWN / CEL_ERROR operands are passed through unchanged
// rather than collapsed to 0/1 by `i32.eqz`.  Operand lowered via
// `LowerExprBoxed` so a nested ternary routes through the boxed form
// (Slice F step 6) and doesn't bypass `cel_not` via `$eval`
// early-return.
absl::StatusOr<BinaryenExpressionRef> LowerLogicalNot(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call) {
  if (call.args().size() != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("`!_` takes 1 argument, got ", call.args().size()));
  }
  auto v = LowerExprBoxed(ctx, ast, call.args().at(0));
  if (!v.ok()) return v.status();
  BinaryenExpressionRef arg = *v;
  return BinaryenCall(ctx.mod.raw(), "cel_not", &arg, 1, BinaryenTypeInt32());
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

// Emits `if (kind(*cond_local) >= CEL_UNKNOWN) {
//   cel_copy_celvalue_at($sret, *cond_local); return;
// }` — the sret early-exit that propagates a non-OK CelValue out
// through the eval sret slot.  Shared by the ternary 3VL guard and
// any future UNKNOWN/ERROR propagation site.
BinaryenExpressionRef EmitSretEarlyReturnIfNonOk(LoweringContext& ctx,
                                                 BinaryenIndex cond_local) {
  BinaryenModuleRef m = ctx.mod.raw();
  BinaryenExpressionRef base_call = BinaryenCall(
      m, "cel_mem_base", /*operands=*/nullptr, 0, BinaryenTypeInt32());
  BinaryenExpressionRef abs_expr =
      BinaryenBinary(m, BinaryenAddInt32(), base_call,
                     BinaryenLocalGet(m, cond_local, BinaryenTypeInt32()));
  BinaryenExpressionRef kind_load =
      BinaryenLoad(m, /*bytes=*/4, /*signed_=*/false, /*offset=*/0,
                   /*align=*/4, BinaryenTypeInt32(), abs_expr,
                   /*memoryName=*/"memory");
  // kind >= CEL_UNKNOWN (14) catches both CEL_UNKNOWN (14) and
  // CEL_ERROR (15); any other non-bool kind is a checker miss.
  constexpr int32_t kCelUnknownKind = 14;
  BinaryenExpressionRef is_non_ok =
      BinaryenBinary(m, BinaryenGeUInt32(), kind_load,
                     BinaryenConst(m, BinaryenLiteralInt32(kCelUnknownKind)));
  BinaryenExpressionRef copy_args[2] = {
      BinaryenLocalGet(m, LoweringContext::kOutSlotParam, BinaryenTypeInt32()),
      BinaryenLocalGet(m, cond_local, BinaryenTypeInt32()),
  };
  BinaryenExpressionRef copy_call =
      BinaryenCall(m, "cel_copy_celvalue_at", copy_args, 2, BinaryenTypeNone());
  BinaryenExpressionRef ret = BinaryenReturn(m, /*value=*/nullptr);
  BinaryenExpressionRef err_children[2] = {copy_call, ret};
  BinaryenExpressionRef err_block = BinaryenBlock(
      m, /*name=*/nullptr, err_children, /*numChildren=*/2, BinaryenTypeNone());
  return BinaryenIf(m, is_non_ok, err_block, /*ifFalse=*/nullptr);
}

// Boxed ternary (Slice F step 6).  Every arm is lowered via
// `LowerExprBoxed` so the result is a CelValue offset.  If the cond
// is UNKNOWN / ERROR we return the cond offset verbatim — the
// wrapping absorber (`cel_or` / `cel_and` / `cel_not` / boxed
// comparison) will see it as a value rather than being skipped by a
// scalar-path `$eval` early return.  Rows 8 / 19 of the plan.
//
// Shape:
//
//   cond_local = LowerExprBoxed(cond)           ; CelValue offset
//   if kind(*cond_local) >= CEL_UNKNOWN
//     cond_local                                 ; propagate non-OK
//   else
//     BinaryenIf(cel_bool_from_value(cond_local),
//                LowerExprBoxed(then), LowerExprBoxed(else))
absl::StatusOr<BinaryenExpressionRef> LowerConditionalBoxed(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call) {
  if (call.args().size() != 3) {
    return absl::InvalidArgumentError(
        absl::StrCat("`_?_:_` takes 3 arguments, got ", call.args().size()));
  }
  auto cond = LowerExprBoxed(ctx, ast, call.args().at(0));
  if (!cond.ok()) return cond.status();
  auto t = LowerExprBoxed(ctx, ast, call.args().at(1));
  if (!t.ok()) return t.status();
  auto f = LowerExprBoxed(ctx, ast, call.args().at(2));
  if (!f.ok()) return f.status();
  BinaryenModuleRef m = ctx.mod.raw();
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenIndex cond_local = ctx.AddLocal(i32);
  // `kind(*cond_local) >= CEL_UNKNOWN (14)` — catches both UNKNOWN
  // (14) and ERROR (15).  Matches the kind-byte shape used by
  // `EmitSretEarlyReturnIfNonOk` (4-byte load, align=4).
  constexpr int32_t kCelUnknownKind = 14;
  BinaryenExpressionRef abs_ptr = BinaryenBinary(
      m, BinaryenAddInt32(), BinaryenCall(m, "cel_mem_base", nullptr, 0, i32),
      BinaryenLocalGet(m, cond_local, i32));
  BinaryenExpressionRef is_non_ok =
      BinaryenBinary(m, BinaryenGeUInt32(),
                     BinaryenLoad(m, 4, /*signed_=*/false, /*offset=*/0,
                                  /*align=*/4, i32, abs_ptr, "memory"),
                     BinaryenConst(m, BinaryenLiteralInt32(kCelUnknownKind)));
  BinaryenExpressionRef unbox_arg = BinaryenLocalGet(m, cond_local, i32);
  BinaryenExpressionRef inner_if = BinaryenIf(
      m, BinaryenCall(m, "cel_bool_from_value", &unbox_arg, 1, i32), *t, *f);
  BinaryenExpressionRef outer_if =
      BinaryenIf(m, is_non_ok, BinaryenLocalGet(m, cond_local, i32), inner_if);
  BinaryenExpressionRef children[2] = {BinaryenLocalSet(m, cond_local, *cond),
                                       outer_if};
  return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/2, i32);
}

// `_?_:_` — ternary with 3VL condition handling (M4 Slice E1).  Both
// branches must have the same Repr (checker invariant); we rely on
// BinaryenIf to validate type agreement.  The condition is a
// Repr::kBool (CelValue offset), so we first probe its kind byte and
// early-return from $eval when the cond is CEL_UNKNOWN or CEL_ERROR,
// propagating the cond as the eval result (see
// `EmitSretEarlyReturnIfNonOk`).  On the OK path we unbox to raw i32
// via `cel_bool_from_value` and dispatch to the then / else branches.
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
  BinaryenModuleRef m = ctx.mod.raw();
  const BinaryenIndex cond_local = ctx.AddLocal(BinaryenTypeInt32());
  BinaryenExpressionRef cond_set = BinaryenLocalSet(m, cond_local, *cond);
  BinaryenExpressionRef err_if = EmitSretEarlyReturnIfNonOk(ctx, cond_local);
  BinaryenExpressionRef unbox_args[1] = {
      BinaryenLocalGet(m, cond_local, BinaryenTypeInt32()),
  };
  BinaryenExpressionRef unbox =
      BinaryenCall(m, "cel_bool_from_value", unbox_args,
                   /*numOperands=*/1, BinaryenTypeInt32());
  BinaryenExpressionRef inner_if = BinaryenIf(m, unbox, *t, *f);
  BinaryenExpressionRef children[3] = {cond_set, err_if, inner_if};
  return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/3,
                       BinaryenExpressionGetType(*t));
}

// `_&&_` / `_||_` — 3VL (M4 Slice C / 3b2).  Delegate to the runtime
// helpers `cel_and` / `cel_or`, which implement the OK / UNKNOWN /
// ERROR truth table from `doc/langdef.md` §partial-evaluation.
//
// Trade-off: we give up wasm-native short-circuit evaluation because
// `cel_and`/`cel_or` evaluate both operands before dispatching.
// Short-circuit in a 3VL setting is subtle — `false && err` must be
// `false`, but the checker doesn't prove the LHS is `false`, so we
// need the helper to see both values to decide.  A future slice can
// reintroduce short-circuit by emitting an inline kind dispatch
// around BinaryenIf when the checker proves the LHS is pure
// Repr::kBool with no UNKNOWN/ERROR potential.
absl::StatusOr<BinaryenExpressionRef> LowerShortCircuit(
    LoweringContext& ctx, const TypedAst& ast, const cel::CallExpr& call,
    absl::string_view fn) {
  if (call.args().size() != 2) {
    return absl::InvalidArgumentError(
        absl::StrCat("`", fn, "` takes 2 arguments, got ", call.args().size()));
  }
  // Operands lowered via `LowerExprBoxed` so nested ternaries route
  // through the boxed form (Slice F step 6).  kBool non-ternary exprs
  // fall through to `LowerExpr` inside `LowerExprBoxed`, so this is a
  // no-op for the common case.
  auto l = LowerExprBoxed(ctx, ast, call.args().at(0));
  if (!l.ok()) return l.status();
  auto r = LowerExprBoxed(ctx, ast, call.args().at(1));
  if (!r.ok()) return r.status();
  BinaryenModuleRef m = ctx.mod.raw();
  const bool is_and = (fn == op::CelOperator::LOGICAL_AND);
  const char* helper = is_and ? "cel_and" : "cel_or";
  BinaryenExpressionRef args[2] = {*l, *r};
  return BinaryenCall(m, helper, args, 2, BinaryenTypeInt32());
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
  const bool is_comparison =
      (fn == op::CelOperator::EQUALS || fn == op::CelOperator::NOT_EQUALS ||
       fn == op::CelOperator::LESS || fn == op::CelOperator::LESS_EQUALS ||
       fn == op::CelOperator::GREATER || fn == op::CelOperator::GREATER_EQUALS);
  // Uniform-boxed ABI (Step 3): every scalar / bool comparison goes
  // through `LowerBoxedComparison`, which hands both operands as
  // CelValue offsets to `cel_cmp_<kind>_<op>`.  The helper forwards
  // `cel_status_either(a, b)` when either side is UNKNOWN / ERROR, so
  // wrapping `&&` / `||` / `?:` see the non-OK as a value.  String /
  // bytes stay on their absorbing `_v` helpers (step 4, via
  // `LowerSpanEquality`).
  if (is_comparison && BoxedCmpHelper(fn, *arg_r) != nullptr) {
    return LowerBoxedComparison(ctx, ast, call, fn, *arg_r);
  }
  // Message eq/ne (Slice F Step 5): boxed-operand path so the host-
  // side `message_eq` call is gated on `cel_message_eq_prologue_v`
  // absorbing a non-OK sub-message.
  if (is_comparison && *arg_r == Repr::kMessage &&
      (fn == op::CelOperator::EQUALS || fn == op::CelOperator::NOT_EQUALS)) {
    return LowerMessageEqualityBoxed(ctx, ast, fn, call);
  }
  auto l = LowerExpr(ctx, ast, call.args().at(0));
  if (!l.ok()) return l.status();
  auto r = LowerExpr(ctx, ast, call.args().at(1));
  if (!r.ok()) return r.status();
  if (fn == op::CelOperator::ADD || fn == op::CelOperator::SUBTRACT ||
      fn == op::CelOperator::MULTIPLY || fn == op::CelOperator::DIVIDE ||
      fn == op::CelOperator::MODULO) {
    return LowerArithmetic(fn, *arg_r, *l, *r, ctx);
  }
  if (is_comparison) {
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
  if (result_r == Repr::kString || result_r == Repr::kBytes ||
      result_r == Repr::kBool) {
    // Repr::kBool travels as a CelValue offset (M4 Slice C / 3b2),
    // same ABI shape as string / bytes — the scratch slot IS the
    // result, kind already set to CEL_BOOL by the host.
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

// Validates the shared preconditions for a lowered `SelectExpr` (field
// number resolved, operand is a message) and lowers the operand.  Both
// the field-read and `has()` paths go through this so a single error
// message covers the "operand is not a message" and "field not found"
// cases regardless of which path the user wrote.
//
// Also interns the `(field_number, field_name)` pair into
// `ctx.field_pool`: the returned `intern_id` is what the emitted
// `get_field` / `has_field` call receives as its i32 argument.  The
// host uses the same intern ID to look up the field's name +
// descriptor at call time (see `CelAbi.fields` in `cel.abi`).
//
// `SelectOperand` is defined above at the top of this TU because the
// Slice F1 boxed-path helpers reference it.
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
  // Intern the outer field first so the codegen walk order matches
  // the pre-order walk in `FieldNamePool::FromTypedAst` (used by
  // `BuildCelAbi`).  Recursing into the operand first would swap the
  // id assignments for nested selects — e.g. for `a.b.c`, codegen
  // would hand out `b=0, c=1` while the ABI table records `c=0, b=1`,
  // and the host-side lookup would dereference the wrong field.
  const uint32_t intern_id = ctx.field_pool.Intern(
      static_cast<uint32_t>(a->field_number), select.field());
  // Intern the full attribute path at the same point so codegen and
  // `BuildCelAbi` hand out matching attr_ids (both walk pre-order
  // outer-first).  Walk the operand chain inward, collect field
  // names in outer-to-inner order, then reverse; root ident provides
  // the variable (empty → non-ident root, host sees "no match").
  std::vector<std::string> qualifiers{std::string(select.field())};
  std::string variable;
  for (const cel::Expr* cur = &select.operand();;) {
    if (cur->kind_case() == cel::ExprKindCase::kSelectExpr) {
      qualifiers.emplace_back(cur->select_expr().field());
      cur = &cur->select_expr().operand();
      continue;
    }
    if (cur->kind_case() == cel::ExprKindCase::kIdentExpr) {
      variable = cur->ident_expr().name();
    }
    break;
  }
  std::reverse(qualifiers.begin(), qualifiers.end());
  const uint32_t attr_id = ctx.attr_pool.Intern(variable, qualifiers);
  auto operand = LowerExpr(ctx, ast, select.operand());
  if (!operand.ok()) return operand.status();
  return SelectOperand{*operand, static_cast<int32_t>(intern_id),
                       static_cast<int32_t>(attr_id)};
}

// Lowers `has(operand.field)` — a `SelectExpr` with `test_only=true` —
// to a single `cel_host.has_field(operand, field_intern_id) → i32` call,
// boxed through `cel_make_bool` so the result lives in the
// Repr::kBool ABI (CelValue offset, M4 Slice C / 3b2) alongside every
// other bool-producing site.
absl::StatusOr<BinaryenExpressionRef> LowerSelectTestOnly(
    LoweringContext& ctx, const TypedAst& ast, const cel::Expr& expr) {
  auto op = LowerSelectOperand(ctx, ast, expr.select_expr(), expr.id());
  if (!op.ok()) return op.status();
  BinaryenModuleRef m = ctx.mod.raw();
  BinaryenExpressionRef args[2] = {
      op->operand,
      BinaryenConst(m, BinaryenLiteralInt32(op->intern_id)),
  };
  BinaryenExpressionRef raw =
      BinaryenCall(m, "has_field", args, 2, BinaryenTypeInt32());
  return BinaryenCall(m, "cel_make_bool", &raw, 1, BinaryenTypeInt32());
}

// Lowers `operand.field` to a three-step block:
//   1. scratch = cel_alloc(24)
//   2. cel_host.get_field(operand, field_intern_id, scratch)
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
  BinaryenExpressionRef args[4] = {
      op->operand,
      BinaryenConst(m, BinaryenLiteralInt32(op->intern_id)),
      BinaryenConst(m, BinaryenLiteralInt32(op->attr_id)),
      BinaryenLocalGet(m, scratch, BinaryenTypeInt32()),
  };
  BinaryenExpressionRef get_field_call =
      BinaryenCall(m, "get_field", args, 4, BinaryenTypeNone());
  auto load = LoadSelectPayload(ctx, scratch, *result_r, expr.id());
  if (!load.ok()) return load.status();
  BinaryenType result_type = WasmTypeFor(*result_r);
  // Partial-eval: if the host marks this attr as unknown (M4 Slice E2a
  // surfaces this via `cel_host.get_field` writing CEL_UNKNOWN into the
  // scratch CelValue instead of resolving the field).  For scalar / message
  // Reprs the
  // load would reinterpret the UNKNOWN payload bytes as a real scalar
  // / externref and silently return garbage, so we insert the sret
  // early-return before the load.  For Repr::kBool / kString / kBytes
  // the load returns the scratch offset itself — UNKNOWN stays visible
  // to the parent (DecodeSlot surfaces it at the top level, and 3VL
  // absorbers `cel_and`/`cel_or`/`cel_not` handle it for bool).  We
  // skip the early-return there so a `c.is_premium && true` style
  // expression absorbs naturally instead of short-circuiting the whole
  // eval.
  const bool payload_is_offset = *result_r == Repr::kBool ||
                                 *result_r == Repr::kString ||
                                 *result_r == Repr::kBytes;
  if (payload_is_offset) {
    BinaryenExpressionRef children[3] = {set_scratch, get_field_call, *load};
    return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/3,
                         result_type);
  }
  BinaryenExpressionRef err_if = EmitSretEarlyReturnIfNonOk(ctx, scratch);
  BinaryenExpressionRef children[4] = {set_scratch, get_field_call, err_if,
                                       *load};
  return BinaryenBlock(m, /*name=*/nullptr, children, /*numChildren=*/4,
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
//
// Param 0 is the sret out-slot (i32 arena offset); declared variables
// follow at indices 1..N.  The `idx = params.size()` pattern below
// reads the post-slot slot before appending, so the first variable
// gets index 1 as required.
// Returns the `cel_make_*` helper that boxes a raw wasm scalar of the
// given Repr into a CelValue offset at $eval entry.  Only Reprs that
// travel as raw scalars over the host ABI need a boxing step — bool /
// int / uint / double.  Other kinds (string, bytes, message, etc.)
// either already arrive boxed or have a separate wrap path.
const char* ScalarBoxHelper(Repr r) {
  switch (r) {
    case Repr::kBool:
      return "cel_make_bool";
    case Repr::kInt:
      return "cel_make_int";
    case Repr::kUint:
      return "cel_make_uint";
    case Repr::kDouble:
      return "cel_make_double";
    default:
      return nullptr;
  }
}

absl::Status BuildParamList(const TypedAst& ast, LoweringContext& ctx,
                            std::vector<BinaryenType>& params,
                            bool& has_message_param) {
  params.reserve(ast.variables().size() + 1);
  params.push_back(BinaryenTypeInt32());  // param 0: sret slot.
  has_message_param = false;
  // Scalar params (bool / int / uint / double) arrive as raw wasm
  // scalars from the host — the uniform boxed ABI expects ident reads
  // to produce CelValue offsets.  We record each scalar param's raw
  // slot and the name it binds so we can emit the boxing prologue in
  // a second pass, after `num_params` is final (AddLocal needs
  // `num_params` to compute local indices).
  struct PendingScalarBox {
    std::string name;
    BinaryenIndex raw_param;
    Repr repr;
  };
  std::vector<PendingScalarBox> pending_scalar_boxes;
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
    if (ScalarBoxHelper(v.repr) != nullptr) {
      pending_scalar_boxes.push_back({v.name, idx, v.repr});
    }
  }
  ctx.num_params = static_cast<uint32_t>(params.size());
  BinaryenModuleRef m = ctx.mod.raw();
  for (const auto& pending : pending_scalar_boxes) {
    const BinaryenIndex boxed = ctx.AddLocal(BinaryenTypeInt32());
    const BinaryenType raw_t = WasmTypeFor(pending.repr);
    BinaryenExpressionRef raw = BinaryenLocalGet(m, pending.raw_param, raw_t);
    BinaryenExpressionRef box = BinaryenCall(m, ScalarBoxHelper(pending.repr),
                                             &raw, 1, BinaryenTypeInt32());
    ctx.prologue_setups.push_back(BinaryenLocalSet(m, boxed, box));
    ctx.idents.insert_or_assign(pending.name, boxed);
  }
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

// Emits the sret-root store: takes a scalar `body` of WasmTypeFor(root_repr)
// and returns an expression of type None that writes the boxed CelValue
// into the caller-provided out slot (param 0).  `body` is consumed — the
// returned expression evaluates it exactly once.  For string / bytes /
// message roots, `body` is already a CelValue offset, so a 24-byte
// memcpy via `cel_copy_celvalue_at` is enough.  For scalar roots the
// paired `cel_box_<repr>_at` helper writes kind + payload.  Reprs with
// no sret encoding today (list / map / type / duration / timestamp /
// enum) surface as `Unimplemented` — none have end-to-end coverage at
// root, so waiting to ship the helpers until there's a concrete caller
// avoids speculative runtime code.
absl::StatusOr<BinaryenExpressionRef> EmitSretStore(LoweringContext& ctx,
                                                    BinaryenExpressionRef body,
                                                    Repr root_repr) {
  BinaryenModuleRef m = ctx.mod.raw();
  BinaryenExpressionRef slot =
      BinaryenLocalGet(m, LoweringContext::kOutSlotParam, BinaryenTypeInt32());
  const char* fn = nullptr;
  switch (root_repr) {
    case Repr::kBool:
      // Post-3b2, Repr::kBool is already a CelValue offset — nothing
      // to box, just copy the 24-byte payload into the sret slot.
      fn = "cel_copy_celvalue_at";
      break;
    case Repr::kInt:
      fn = "cel_box_int";
      break;
    case Repr::kUint:
      fn = "cel_box_uint";
      break;
    case Repr::kDouble:
      fn = "cel_box_double";
      break;
    case Repr::kString:
    case Repr::kBytes:
      fn = "cel_copy_celvalue_at";
      break;
    case Repr::kMessage: {
      // Root Repr::kMessage means `body` is an externref.  Wrap to a
      // CelValue offset via cel_wrap_message and then copy.  M3's
      // `AddMessageSupport` declared both helpers when any message
      // variable was present; it now also runs when the root is a
      // message (`LowerToEvalFunction`).
      BinaryenExpressionRef wrap_args[1] = {body};
      body = BinaryenCall(m, "cel_wrap_message", wrap_args, 1,
                          BinaryenTypeInt32());
      fn = "cel_copy_celvalue_at";
      break;
    }
    default:
      return absl::UnimplementedError(
          absl::StrCat("expr_lower: root Repr `", ReprName(root_repr),
                       "` has no sret-box encoding in M4"));
  }
  BinaryenExpressionRef args[2] = {slot, body};
  return BinaryenCall(m, fn, args, 2, BinaryenTypeNone());
}

// Wraps `body` with the eval prologue: scratch-slot allocation (when
// any sret arithmetic helper needs one) plus any param-boxing setups
// registered during BuildParamList.  cel_reset() rewinds the arena
// between host invocations, so these setups must run on every $eval
// entry — the prologue is per-invocation, not global.
BinaryenExpressionRef WithScratchSlotPrologue(LoweringContext& ctx,
                                              BinaryenExpressionRef body,
                                              BinaryenType body_type) {
  std::vector<BinaryenExpressionRef> children;
  children.reserve(ctx.prologue_setups.size() + 2);
  for (BinaryenExpressionRef setup : ctx.prologue_setups) {
    children.push_back(setup);
  }
  BinaryenModuleRef m = ctx.mod.raw();
  if (ctx.scratch_slot.has_value()) {
    BinaryenExpressionRef slot_size = BinaryenConst(
        m, BinaryenLiteralInt32(static_cast<int32_t>(sizeof(uint64_t) * 3)));
    BinaryenExpressionRef alloc_call =
        BinaryenCall(m, "cel_alloc", &slot_size, 1, BinaryenTypeInt32());
    children.push_back(BinaryenLocalSet(m, *ctx.scratch_slot, alloc_call));
  }
  if (children.empty()) return body;
  children.push_back(body);
  return BinaryenBlock(m, /*name=*/nullptr, children.data(),
                       static_cast<BinaryenIndex>(children.size()), body_type);
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

// Validates the AST root, declares runtime + host imports, builds the
// parameter list from `ast.variables()`, and arranges for message
// support when either a parameter or the root Repr is Message.  On
// success `*root_r` is populated and `params` holds the WASM types for
// the user-declared variables (the sret slot is prepended inside
// BuildParamList).
static absl::Status PrepareEvalFnSignature(const TypedAst& ast, WasmModule& mod,
                                           LoweringContext& ctx,
                                           std::vector<BinaryenType>& params,
                                           Repr& root_r) {
  if (!ast.has_ast()) {
    return absl::InvalidArgumentError(
        "LowerToEvalFunction: TypedAst has no underlying cel::Ast");
  }
  const cel::Expr& root = ast.ast().root_expr();
  auto r = ReprOf(ast, root);
  if (!r.ok()) return r.status();
  root_r = *r;
  if (WasmTypeFor(root_r) == BinaryenTypeNone()) {
    return absl::UnimplementedError(
        absl::StrCat("expr_lower: root Repr `", ReprName(root_r),
                     "` has no scalar ABI lowering in M2"));
  }
  if (auto s = DeclareRuntimeImports(mod); !s.ok()) return s;
  DeclareHostImports(mod);
  bool has_message_param = false;
  if (auto s = BuildParamList(ast, ctx, params, has_message_param); !s.ok()) {
    return s;
  }
  if (has_message_param || root_r == Repr::kMessage) {
    if (auto s = AddMessageSupport(mod); !s.ok()) return s;
  }
  return absl::OkStatus();
}

absl::StatusOr<LoweredFunction> LowerToEvalFunction(const TypedAst& ast,
                                                    absl::string_view func_name,
                                                    WasmModule& mod) {
  std::vector<BinaryenType> params;
  LoweringContext ctx{mod};
  Repr root_r = Repr::kUnknown;
  if (auto s = PrepareEvalFnSignature(ast, mod, ctx, params, root_r); !s.ok()) {
    return s;
  }
  const cel::Expr& root = ast.ast().root_expr();
  auto body = LowerExpr(ctx, ast, root);
  if (!body.ok()) return body.status();
  auto store = EmitSretStore(ctx, *body, root_r);
  if (!store.ok()) return store.status();
  const BinaryenType none = BinaryenTypeNone();
  mod.AddFunction(func_name, params, none,
                  /*local_types=*/ctx.local_types,
                  WithScratchSlotPrologue(ctx, *store, none));
  BinaryenFunctionRef fn =
      BinaryenGetFunction(mod.raw(), std::string(func_name).c_str());
  if (fn == nullptr) {
    return absl::InternalError(
        absl::StrCat("expr_lower: BinaryenGetFunction returned null for `",
                     func_name, "` immediately after AddFunction"));
  }
  return LoweredFunction{fn, none, root_r};
}

}  // namespace celwasm
