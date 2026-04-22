#ifndef CELWASM_COMPILER_V2_CODEGEN_RESOLVE_PASS_H_
#define CELWASM_COMPILER_V2_CODEGEN_RESOLVE_PASS_H_

#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "binaryen-c.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

// Output of the first codegen pipeline pass.  `annotations` carries `repr`
// (every typed node), `field_number` (select nodes, M2+), `overload_id`
// (call nodes, M3+), `local_index` / `scope_id` (ident + comprehension
// nodes, M2/M5).  `storage` stays zero-initialised here — LayoutPass fills
// it next.
//
// `local_types` is the wasm local declaration list that the eventual
// `$eval` function will carry.  M1 emits an empty list (no idents or
// comprehensions yet); the vector is still plumbed end-to-end so
// LayoutPass and the module emitter don't have to special-case "no
// locals".
//
// `max_scope_id` is the highest `scope_id` assigned during the walk.  0
// means "no comprehensions were present" (the M1 case).
struct ResolveOutput {
  WasmAnnotations annotations;
  std::vector<BinaryenType> local_types;
  uint32_t max_scope_id = 0;
};

// Runs the resolve pass on a checked TypedAst.  M1 implements only the
// `repr`-population half of the pass: for every node in the checker's
// `type_map`, the corresponding `NodeAnnotation::repr` is written.  Other
// fields are left at their zero sentinels because M1's expression surface
// is pure literals — no idents to bind, no overloads to intern, no
// comprehension scopes to push.
//
// The pass aborts via `ABSL_CHECK` if any `kConst` node in the AST lacks a
// populated repr at end-of-pass: a kConst without a type_map entry means
// the checker didn't do its job, and silently producing garbage rodata
// bytes is strictly worse than crashing.
ABSL_MUST_USE_RESULT absl::StatusOr<ResolveOutput> ResolvePass(
    const TypedAst& ast);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_RESOLVE_PASS_H_
