#ifndef CELWASM_COMPILER_IR_STATIC_SUBSET_H_
#define CELWASM_COMPILER_IR_STATIC_SUBSET_H_

#include "absl/status/status.h"
#include "common/ast.h"

namespace celwasm {

// Enforces the static-typed subset of CEL that our AOT compiler accepts.
//
// Walks every `Expr` node reachable from `ast.root_expr()` and rejects any
// node whose inferred type is DYN or ERROR.  A node is DYN when it has no
// entry in `ast.type_map()` (the checker omits DYN to save space) or when its
// entry is `DynTypeSpec` / `ErrorTypeSpec`.
//
// Returns OK if every reachable node has a concrete type.  On failure returns
// `InvalidArgumentError` with a human-readable list of offending expr ids.
absl::Status RejectDyn(const cel::Ast& ast);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_IR_STATIC_SUBSET_H_
