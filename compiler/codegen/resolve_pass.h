#ifndef CELWASM_COMPILER_V2_CODEGEN_RESOLVE_PASS_H_
#define CELWASM_COMPILER_V2_CODEGEN_RESOLVE_PASS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

// Discriminates a `ResolvedVariable`'s lifetime / set-site.
//
//   - `kFreeVariable` — bound by `Activation` at host side; the wasm
//     `$eval` prelude `local.set $i (i32.const slot_offset)` for
//     every entry.  Counts toward `cel.abi.variables[]`.
//
//   - `kComprehensionIter` — bound by the comprehension's loop
//     prologue.  The wasm local holds a *moving pointer* (iter_off)
//     into the iter_range's element run for list iteration, or the
//     output slot of `cel_map_iter_key_at` for map iteration.  NOT
//     set by the function prelude; NOT in `cel.abi.variables[]`.
//
//   - `kComprehensionAccu` — bound by the comprehension's
//     init/loop_step.  The wasm local holds a stable workspace slot
//     offset; `loop_step` re-writes the CelValue at that offset each
//     iteration.  The two-iter-var list source's synthetic index
//     counter also uses this lifecycle — codegen rewrites its slot
//     to `{kind=CEL_INT, payload.i=index}` each iter.  NOT set by
//     the function prelude; NOT in `cel.abi.variables[]`.
enum class ResolvedVariableKind : uint8_t {
  kFreeVariable = 0,
  kComprehensionIter,
  kComprehensionAccu,
};

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
// for `kind == kFreeVariable` entries; comprehension-scope entries
// are flagged via `kind` so the prelude and ABI emitter skip them.
struct ResolvedVariable {
  std::string name;
  uint32_t local_index = 0;
  Repr repr = Repr::kUnknown;
  ResolvedVariableKind kind = ResolvedVariableKind::kFreeVariable;
};

// One row of the attribute intern table — one entry per distinct
// (root_variable, qualifiers) path across the AST.  Indexed by
// `NodeAnnotation::attribute_id`; id 0 is the sentinel "no
// attribute" used on non-path-bearing nodes.  Populated by
// `ResolvePass` by walking kIdent + kSelect post-order (see
// `rewrite/m2-ident-select-unknowns.md`); the trampoline reads this
// at cel_get_field time and appends the selected field to construct
// the full attribute path for unknown-pattern matching.
struct AttributeEntryRow {
  std::string root_variable;
  std::vector<std::string> qualifiers;
};

// One row of the message-type intern table — one entry per distinct
// fully-qualified message name constructed by `kStructExpr` across
// the AST.  Indexed by `NodeAnnotation::message_type_id`; id 0 is
// the sentinel "no type id" carried by every non-struct node.
// Populated by `ResolvePass`'s `MessageTypeIdVisitor` walking
// kStructExpr post-order (see `rewrite/m7-proto-literals.md`).  `BuildCelAbi` serialises this into
// `cel.abi.types[]`; `Engine::Plan` resolves each FQN against the
// descriptor pool to populate the per-Instance type → Descriptor*
// map the `cel_make_message` trampoline reads at eval time.
struct MessageTypeRow {
  std::string fully_qualified_name;
};

// Output of the first codegen pipeline pass.  `annotations` carries `repr`
// (every typed node), `field_number` (select nodes), `overload_id`
// (call nodes), `local_index` (ident nodes), `scope_id`
// (comprehension nodes), and `attribute_id`.  `storage` stays
// zero-initialised here — LayoutPass fills it next.
//
// `variables` is the compact list of referenced free variables in
// first-seen order; each `NodeAnnotation::local_index` on a kIdent
// indexes into it.  An unreferenced declared variable does NOT
// appear here (no slot reserved, no entry in the cel.abi).  The
// size of this vector is also the count of wasm locals the lowered
// `$eval` function declares — one i32 per referenced variable.
//
// `max_scope_id` is the highest `scope_id` assigned during the walk.
// 0 means "no comprehensions were present".  Comprehension scope
// extends `variables` (iter/accu names treated uniformly with
// free variables from codegen's point of view).
struct ResolveOutput {
  WasmAnnotations annotations;
  std::vector<ResolvedVariable> variables;

  // Attribute intern table.  Entry 0 is the reserved "no attribute"
  // sentinel; entries [1..N] are referenced by
  // `NodeAnnotation::attribute_id` on kIdent + kSelect nodes.
  // `BuildCelAbi` serialises this into `cel.abi.attributes[]`.
  std::vector<AttributeEntryRow> attributes;

  // Message-type intern table.  Entry 0 is the reserved
  // "no type" sentinel; entries [1..N] are referenced by
  // `NodeAnnotation::message_type_id` on kStructExpr nodes.
  // `BuildCelAbi` serialises this into `cel.abi.types[]`.
  std::vector<MessageTypeRow> message_types;

  uint32_t max_scope_id = 0;
};

// Runs the resolve pass on a checked TypedAst.  Populates every
// `NodeAnnotation::repr` from the checker's `type_map`; resolves
// idents, overloads, attribute paths, comprehension scopes, and
// message-type interns through the per-kind visitors above.  See
// `rewrite/design.md` §5 ResolvePass.
//
// The pass aborts via `ABSL_CHECK` if any `kConst` node in the AST lacks a
// populated repr at end-of-pass: a kConst without a type_map entry means
// the checker didn't do its job, and silently producing garbage rodata
// bytes is strictly worse than crashing.
ABSL_MUST_USE_RESULT absl::StatusOr<ResolveOutput> ResolvePass(
    const TypedAst& ast);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_RESOLVE_PASS_H_
