#ifndef CELWASM_COMPILER_IR_TYPED_AST_H_
#define CELWASM_COMPILER_IR_TYPED_AST_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/ast.h"
#include "common/ast/metadata.h"
#include "common/type.h"
#include "compiler/ir/annotations.h"
#include "google/protobuf/descriptor.h"
#include "shared/type.h"

namespace celwasm {

// Maps a `cel::TypeSpec` (as stored in a checked `cel::Ast`) to the ABI
// representation used by our WASM codegen.
Repr ReprOf(const cel::TypeSpec& type);

// Maps a `cel::Type` (as produced by the checker's type parser) to our ABI
// representation.  Returns `Repr::kUnknown` for types that have no scalar
// ABI encoding in the static subset (dyn, type-param, function, ...).
Repr ReprOf(const cel::Type& type);

// One user-declared variable, captured in the order it appeared in
// `CheckOptions::variable_specs`.  Codegen binds each entry to a function
// parameter at the corresponding index: the first declared variable is the
// eval function's first parameter, and so on.  Storing the Repr here — as
// opposed to re-deriving it from the AST's `type_map` — lets codegen shape
// the function signature even for variables the expression doesn't
// reference (which never appear in `type_map`).
struct Variable {
  std::string name;
  Repr repr = Repr::kUnknown;
  // The declared type in full.  `repr` is the wire kind the marshal
  // encodes against and says nothing about a list's element type, a
  // map's key/value types, or a message's FQN; this carries the rest
  // through to `cel.abi` for consumers that describe or bind the
  // variable.  Default-constructed (kUnknown) for variables that
  // never came from a declaration.
  CelType type;
};

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

  TypedAst(std::unique_ptr<cel::Ast> ast, WasmAnnotations annotations,
           std::vector<Variable> variables = {})
      : ast_(std::move(ast)),
        annotations_(std::move(annotations)),
        variables_(std::move(variables)) {}

  TypedAst(TypedAst&&) = default;
  TypedAst& operator=(TypedAst&&) = default;

  TypedAst(const TypedAst&) = delete;
  TypedAst& operator=(const TypedAst&) = delete;

  bool has_ast() const {
    return ast_ != nullptr;
  }

  const cel::Ast& ast() const {
    return *ast_;
  }
  cel::Ast& mutable_ast() {
    return *ast_;
  }

  const WasmAnnotations& annotations() const {
    return annotations_;
  }
  WasmAnnotations& mutable_annotations() {
    return annotations_;
  }

  // Variables declared in `CheckOptions::variable_specs`, in order.
  const std::vector<Variable>& variables() const {
    return variables_;
  }

 private:
  std::unique_ptr<cel::Ast> ast_;
  WasmAnnotations annotations_;
  std::vector<Variable> variables_;
};

// Seeds `annotations` with a `Repr` for every node that appears in
// `ast.type_map()`.  DYN nodes (absent from the map) are intentionally left
// unannotated; the static-subset validator rejects them.
//
// When `pool` is non-null, every `SelectExpr` node is also walked and its
// resolved proto field number is written to
// `NodeAnnotation::field_number` — cel-cpp's `reference_map` does not carry
// field numbers, so codegen cannot recover them from the checked AST alone
// (the read-side fallback path treats `field_number == 0` as "resolve by
// name").  Nodes whose operand type is not a message, or whose field name
// does not resolve through `pool`, are left with `field_number = 0`.
// A null `pool` skips SelectExpr resolution entirely.
void PopulateAnnotations(const cel::Ast& ast,
                         const google::protobuf::DescriptorPool* pool,
                         WasmAnnotations& annotations);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_IR_TYPED_AST_H_
