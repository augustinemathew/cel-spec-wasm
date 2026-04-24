#ifndef CELWASM_COMPILER_V2_CODEGEN_RESOLVE_PASS_H_
#define CELWASM_COMPILER_V2_CODEGEN_RESOLVE_PASS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

// One referenced free variable — populated by ResolvePass when it
// sees a `kIdentExpr` whose name matches a checker-declared variable.
// The same variable may be referenced many times (each kIdent node
// gets the same `local_index` in its NodeAnnotation), but we record
// each distinct name exactly once here.
//
// `repr` comes from the checker's `type_map` for the kIdent node —
// which is the declared variable's type — and tells the host-side
// Activation marshal (Instance::Eval) how to encode the bound Value
// into a 24-byte CelValue at the workspace slot.
//
// `local_index` is dense across referenced variables: 0, 1, 2, ...
// in first-seen order.  The index also indexes `cel.abi.variables[]`
// — the ABI the host reads at Plan time.
struct ResolvedVariable {
  std::string name;
  uint32_t local_index = 0;
  Repr repr = Repr::kUnknown;
};

// Output of the first codegen pipeline pass.  `annotations` carries `repr`
// (every typed node), `field_number` (select nodes, M2.C+), `overload_id`
// (call nodes, M3+), `local_index` (ident nodes, M2.B+), `scope_id`
// (comprehension nodes, M5+), `attribute_id` (M2.E+).
// `storage` stays zero-initialised here — LayoutPass fills it next.
//
// `variables` is the compact list of referenced free variables in
// first-seen order; each `NodeAnnotation::local_index` on a kIdent
// indexes into it.  An unreferenced declared variable does NOT
// appear here (no slot reserved, no entry in the cel.abi).  The
// size of this vector is also the count of wasm locals the lowered
// `$eval` function declares — one i32 per referenced variable.
//
// `max_scope_id` is the highest `scope_id` assigned during the walk.
// 0 means "no comprehensions were present".  M5's comprehension
// scope work extends `variables` (iter/accu names treated uniformly
// with free variables from codegen's point of view).
struct ResolveOutput {
  WasmAnnotations annotations;
  std::vector<ResolvedVariable> variables;
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
