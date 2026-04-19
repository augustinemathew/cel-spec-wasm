#ifndef CELWASM_COMPILER_IR_TYPED_AST_H_
#define CELWASM_COMPILER_IR_TYPED_AST_H_

#include <memory>
#include <utility>

#include "common/ast.h"
#include "common/ast/metadata.h"
#include "compiler/ir/annotations.h"

namespace celwasm {

// Maps a `cel::TypeSpec` (as stored in a checked `cel::Ast`) to the ABI
// representation used by our WASM codegen.
Repr ReprOf(const cel::TypeSpec& type);

// Owned bundle of a type-checked `cel::Ast` plus a side-map of per-node
// `NodeAnnotation`s keyed by `cel::ExprId`.
//
// We deliberately do not wrap `cel::Ast` in a heavier IR.  The checker already
// produces every piece of information we need:
//   * `ast().type_map()[id]`        — inferred TypeSpec for each node.
//   * `ast().reference_map()[id]`   — resolved overload/decl reference.
//   * `annotations()[id].repr`      — ABI representation driven by the type.
// Downstream passes operate on all three maps simultaneously.
class TypedAst {
 public:
  TypedAst() = default;

  TypedAst(std::unique_ptr<cel::Ast> ast, WasmAnnotations annotations)
      : ast_(std::move(ast)), annotations_(std::move(annotations)) {}

  TypedAst(TypedAst&&) = default;
  TypedAst& operator=(TypedAst&&) = default;

  TypedAst(const TypedAst&) = delete;
  TypedAst& operator=(const TypedAst&) = delete;

  bool has_ast() const { return ast_ != nullptr; }

  const cel::Ast& ast() const { return *ast_; }
  cel::Ast& mutable_ast() { return *ast_; }

  const WasmAnnotations& annotations() const { return annotations_; }
  WasmAnnotations& mutable_annotations() { return annotations_; }

 private:
  std::unique_ptr<cel::Ast> ast_;
  WasmAnnotations annotations_;
};

// Seeds `annotations` with a `Repr` for every node that appears in
// `ast.type_map()`.  DYN nodes (absent from the map) are intentionally left
// unannotated; the static-subset validator rejects them.
void PopulateAnnotations(const cel::Ast& ast, WasmAnnotations& annotations);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_IR_TYPED_AST_H_
