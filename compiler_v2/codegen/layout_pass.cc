#include "compiler_v2/codegen/layout_pass.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "common/ast_traverse.h"
#include "common/ast_visitor_base.h"
#include "common/constant.h"
#include "common/expr.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/codegen/static_memory_builder.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"
#include "compiler_v2/runtime/cel_data.h"

namespace celwasm {

namespace {

// Round `x` up to the next multiple of 8.  The workspace region and the
// arena base must both be 8-aligned so every CelValue frame (24 bytes,
// 8-byte aligned) lands on a clean boundary.
uint32_t RoundUp8(uint32_t x) {
  return (x + 7u) & ~uint32_t{7u};
}

// Walks every kConst node, packs its CelValue into rodata via the
// StaticMemoryBuilder, and writes `{kStaticRodata, offset}` onto the
// node's annotation.  Non-kConst nodes are left at `kNone` in this
// visitor; `IdentStorageVisitor` (below) handles kIdent.
class ConstLayoutVisitor : public cel::AstVisitorBase {
 public:
  ConstLayoutVisitor(StaticMemoryBuilder& builder, WasmAnnotations& annotations)
      : builder_(builder), annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitConst(const cel::Expr& expr, const cel::Constant& c) override {
    const uint32_t offset = Pack(c);
    annotations_[expr.id()].storage =
        Storage{StorageKind::kStaticRodata, offset};
  }

 private:
  uint32_t Pack(const cel::Constant& c) {
    if (c.has_null_value()) return builder_.AllocateNull();
    if (c.has_bool_value()) return builder_.AllocateBool(c.bool_value());
    if (c.has_int_value()) return builder_.AllocateInt(c.int_value());
    if (c.has_uint_value()) return builder_.AllocateUint(c.uint_value());
    if (c.has_double_value()) return builder_.AllocateDouble(c.double_value());
    if (c.has_string_value()) return builder_.AllocateString(c.string_value());
    if (c.has_bytes_value()) return builder_.AllocateBytes(c.bytes_value());
    // Closed set: the checker rejects any other Constant variant before
    // we get here (duration / timestamp literals are not parseable
    // constants).  A new variant reaching this line is an invariant
    // violation, not a legitimate code path (CLAUDE.md).
    ABSL_CHECK(false) << "LayoutPass: unrecognised cel::Constant variant";
    return 0;
  }

  StaticMemoryBuilder& builder_;
  WasmAnnotations& annotations_;
};

// Walks every kIdent node and writes `{kLocal, local_index}` onto
// its annotation.  Per m2-ident-select-unknowns.md §2.6 / Slice
// M2.B: the kIdent arm lowers to `BinaryenLocalGet(local_index,
// i32)`, matched by a `$eval` prelude that sets each local to the
// compile-time-known slot offset.  The slot offset itself lives on
// `StaticLayout::variables[local_index].slot_offset` and the
// prelude reads it at emission time — the kIdent annotation holds
// only the local index.
class IdentStorageVisitor : public cel::AstVisitorBase {
 public:
  IdentStorageVisitor(WasmAnnotations& annotations, uint32_t num_variables)
      : annotations_(annotations), num_variables_(num_variables) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& ident) override {
    NodeAnnotation& ann = annotations_[expr.id()];
    // Sanity: ResolvePass assigned a local_index for every kIdent and
    // appended the matching entry to `variables`.  If this kIdent
    // slipped past ResolvePass with an out-of-range index, the
    // `local.get` we emit would read an undeclared local —
    // invariant violation, crash.
    ABSL_CHECK(ann.local_index < num_variables_)
        << "LayoutPass: kIdent expr_id=" << expr.id() << " name=`"
        << ident.name() << "` has local_index=" << ann.local_index
        << " but only " << num_variables_ << " variables were resolved";
    ann.storage = Storage{StorageKind::kLocal, ann.local_index};
  }

 private:
  WasmAnnotations& annotations_;
  uint32_t num_variables_;
};

}  // namespace

// `resolved` is passed by value — its annotations + variables are
// moved into the StaticLayout below.  clang-tidy's
// performance-unnecessary-value-param sees only the const-access
// reads (the `for (const ResolvedVariable& rv : resolved.variables)`
// loop) and doesn't recognise the std::move(resolved.annotations)
// as a consuming use; suppressed inline.
absl::StatusOr<StaticLayout> LayoutPass(
    const TypedAst& ast,
    ResolveOutput resolved,  // NOLINT(performance-unnecessary-value-param)
    const LayoutOptions& opts) {
  ABSL_CHECK(ast.has_ast()) << "LayoutPass: TypedAst has no checked cel::Ast";

  StaticLayout layout;
  layout.annotations = std::move(resolved.annotations);
  layout.debug_mode = opts.debug_layout;

  // --- Pass A: pack every kConst into rodata. ---
  StaticMemoryBuilder builder(layout.rodata_base);
  ConstLayoutVisitor const_visitor(builder, layout.annotations);
  cel::AstTraverse(ast.ast().root_expr(), const_visitor);
  layout.rodata = std::move(builder).Finalize();

  // --- Pass B: reserve one 24-byte workspace slot per referenced
  // variable.  Workspace sits 8-aligned immediately after rodata; each
  // slot is 24 bytes, and 24 is a multiple of 8 so every slot stays
  // aligned.  Indexed by ResolveOutput::variables order (local_index). ---
  layout.workspace_base = RoundUp8(layout.rodata_base +
                                   static_cast<uint32_t>(layout.rodata.size()));

  constexpr auto kSlotBytes = static_cast<uint32_t>(sizeof(CelValue));
  static_assert(kSlotBytes == 24, "CelValue must remain 24 bytes");
  static_assert(kSlotBytes % 8u == 0u, "CelValue must stay 8-aligned");

  layout.variables.reserve(resolved.variables.size());
  for (const ResolvedVariable& rv : resolved.variables) {
    const uint32_t slot_offset =
        layout.workspace_base + (rv.local_index * kSlotBytes);
    layout.variables.push_back(
        LaidOutVariable{rv.name, rv.local_index, rv.repr, slot_offset});
  }
  layout.workspace_bytes =
      static_cast<uint32_t>(resolved.variables.size()) * kSlotBytes;

  // --- Pass C: tag every kIdent with `{kLocal, local_index}`. ---
  // The wasm-local → slot_offset mapping lives on layout.variables;
  // expr_lower emits the `$eval` prelude that writes each slot
  // offset into its local.
  IdentStorageVisitor ident_visitor(
      layout.annotations, static_cast<uint32_t>(resolved.variables.size()));
  cel::AstTraverse(ast.ast().root_expr(), ident_visitor);

  layout.peak_slots = 0;

  // Arena grows forward from the first 8-aligned byte past workspace.
  layout.arena_base = RoundUp8(layout.workspace_base + layout.workspace_bytes);

  return layout;
}

}  // namespace celwasm
