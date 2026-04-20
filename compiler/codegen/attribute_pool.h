// Dedup table for attribute paths referenced by a single eval
// module's `kSelectExpr` call sites.  Codegen walks the typed AST in
// pre-order and, at each select, builds the path from the root
// identifier down to the select (e.g. `request.user.name`) and
// interns it.  The returned `attr_id` becomes the i32 argument the
// emitted wasm passes to `cel_host.get_field` alongside the field
// intern ID; the host resolves it at instantiation time by indexing
// into the `CelAbi.attributes` table built from the same walk.
//
// Why path-based interning:
//   - `AttributePattern::IsMatch` is defined over the whole
//     qualifier chain (variable + fields).  Giving the host the
//     full path at every select site lets it run the same match
//     semantics as cel-cpp's partial-evaluator without rebuilding
//     paths from chained host calls.
//   - Paths that fail to resolve to an identifier root (e.g. the
//     operand of a future user-function call) are stored with an
//     empty `variable`; the host treats them as always NONE.
//
// Determinism mirrors `FieldNamePool`: codegen and `BuildCelAbi`
// walk the AST through the same `FromTypedAst` entry point so the
// intern-IDs they hand out agree by construction.

#ifndef CELWASM_COMPILER_CODEGEN_ATTRIBUTE_POOL_H_
#define CELWASM_COMPILER_CODEGEN_ATTRIBUTE_POOL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "common/expr.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

class AttributePool {
 public:
  struct Entry {
    std::string variable;  // empty ⇒ non-ident root; host sees "no match".
    std::vector<std::string> qualifiers;
  };

  AttributePool() = default;

  // Returns the attr_id for `(variable, qualifiers)`, allocating a
  // fresh entry at the tail if the path has not been seen.  IDs are
  // assigned densely from zero in insertion order.
  uint32_t Intern(absl::string_view variable,
                  absl::Span<const std::string> qualifiers);

  // Builds the attribute path of a `kSelectExpr` by walking down the
  // operand chain until it hits a root node (ident or anything else).
  // Returns `{variable, qualifiers}` with qualifiers in
  // outer-to-inner order (i.e. `a.b.c` on the outer select produces
  // `{variable="a", qualifiers=["b","c"]}`).  When the root is not a
  // bare identifier, `variable` is empty — the codegen still emits an
  // attr_id so the ABI record exists, but the host will never match
  // it against a pattern.
  static Entry BuildPath(const cel::Expr& select_expr);

  // Builds a pool by walking `ast` in pre-order and interning every
  // `SelectExpr`'s full attribute path.  Walk order matches
  // `FieldNamePool::FromTypedAst` — both codegen and `BuildCelAbi`
  // invoke these parallel builds so their intern IDs agree.
  static AttributePool FromTypedAst(const TypedAst& ast);

  absl::Span<const Entry> entries() const { return entries_; }

 private:
  std::vector<Entry> entries_;
  absl::flat_hash_map<std::string, uint32_t> index_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_ATTRIBUTE_POOL_H_
