#include "compiler_v2/codegen/layout_pass.h"

#include <cstdint>
#include <utility>

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
// node's annotation.  Non-kConst nodes are left at `kNone`; M1's
// expr_lower returns `absl::UnimplementedError` for them, so the zero
// sentinel is never consumed.
class LayoutVisitor : public cel::AstVisitorBase {
 public:
  LayoutVisitor(StaticMemoryBuilder& builder, WasmAnnotations& annotations)
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

}  // namespace

absl::StatusOr<StaticLayout> LayoutPass(const TypedAst& ast,
                                        ResolveOutput resolved,
                                        const LayoutOptions& opts) {
  ABSL_CHECK(ast.has_ast()) << "LayoutPass: TypedAst has no checked cel::Ast";

  StaticLayout layout;
  layout.annotations = std::move(resolved.annotations);
  layout.local_types = std::move(resolved.local_types);
  layout.debug_mode = opts.debug_layout;

  // Pack every kConst into rodata.  rodata_base is fixed at 16 (skip the
  // null sentinel and the arena cursor/limit pair at bytes 0..16).
  StaticMemoryBuilder builder(layout.rodata_base);
  LayoutVisitor visitor(builder, layout.annotations);
  cel::AstTraverse(ast.ast().root_expr(), visitor);
  layout.rodata = std::move(builder).Finalize();

  // Workspace comes after rodata, 8-aligned.  M1 has no workspace slots
  // (expr_lower only handles kConst), so workspace_bytes stays 0 — the
  // SlotAllocator is plumbed so later milestones just flip it on.
  layout.workspace_base = RoundUp8(layout.rodata_base +
                                   static_cast<uint32_t>(layout.rodata.size()));
  layout.workspace_bytes = 0;
  layout.peak_slots = 0;

  // Arena grows forward from the first 8-aligned byte past workspace.
  layout.arena_base = RoundUp8(layout.workspace_base + layout.workspace_bytes);

  return layout;
}

}  // namespace celwasm
