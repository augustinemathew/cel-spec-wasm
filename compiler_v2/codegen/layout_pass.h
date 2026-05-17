#ifndef CELWASM_COMPILER_V2_CODEGEN_LAYOUT_PASS_H_
#define CELWASM_COMPILER_V2_CODEGEN_LAYOUT_PASS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

// One referenced variable after layout: the `ResolvedVariable` shape
// extended with the absolute linear-memory offset of its 24-byte
// workspace CelValue slot.  Populated by LayoutPass; consumed by:
//
//   - `expr_lower.cc`
//       * `$eval` prelude — one `BinaryenLocalSet(local_index,
//         i32.const <slot_offset>)` per variable.
//       * kIdent arm — `BinaryenLocalGet(local_index, i32)`.
//       (Per m2-ident-select-unknowns.md §2.6 / §5 Slice M2.B.)
//   - the cel.abi module emitter — writes `cel.abi.variables[]` so
//     the host-side Activation marshal (`Instance::Eval`) can encode
//     each bound Value into the CelValue wire format at `slot_offset`
//     before calling `$eval`.
struct LaidOutVariable {
  std::string name;
  uint32_t local_index = 0;
  Repr repr = Repr::kUnknown;
  // Workspace slot byte offset for variables whose wasm local holds
  // a slot pointer (`kFreeVariable`, `kComprehensionAccu`,
  // `kComprehensionIndex`).  Zero for `kComprehensionIter` — the
  // iter local holds a *moving* pointer into the iter_range's
  // element run (list iteration) or the output of
  // `cel_map_iter_key_at` (map iteration); no fixed workspace cell.
  uint32_t slot_offset = 0;
  // Propagated from `ResolvedVariable::kind`.  EmitVariablePrelude
  // skips entries with `kind != kFreeVariable` — comp-scope
  // entries are set by the comprehension's loop prologue, not by
  // the function prelude.  ABI emitter also filters to
  // `kFreeVariable` entries when populating `cel.abi.variables[]`.
  ResolvedVariableKind kind = ResolvedVariableKind::kFreeVariable;
};

// Layout-time knobs for the pass.  Today: `debug_layout` turns off slot
// reuse (each workspace `Acquire` hands out a fresh cell).  M1 has no
// workspace slots to acquire, so the flag is plumbed but inert.
struct LayoutOptions {
  bool debug_layout = false;
};

// Output of the second codegen pipeline pass.  Extends `annotations` from
// ResolveOutput by writing `.storage` on every node whose result has a
// known memory location at compile time.  M2 populates storage for:
//   - `kConst` nodes (rodata-packed — unchanged from M1)
//   - `kIdent` nodes (workspace-slot pointing at the referenced
//     variable's 24-byte CelValue cell)
// Other non-populated kinds are left with `.storage.kind == kNone`;
// expr_lower CHECKs or returns `Unimplemented` for them.
//
// Memory map the output describes:
//
//     [0..8)     reserved (null sentinel)
//     [8..16)    arena cursor + limit (bytes 8/12) — written by cel_reset
//     [rodata_base..rodata_base+rodata.size())
//                active data segment (kConst CelValues + payload bytes)
//     [workspace_base..workspace_base+workspace_bytes)
//                24B cells — one per referenced variable (M2), plus
//                eventual SlotAllocator-owned scratch cells (M3+)
//     [arena_base..)  grows forward via cel_alloc
//
// `rodata_base` is fixed at 16 (skip past the two reserved slots).
// `workspace_base` is `rodata_base + rodata.size()` rounded up to 8;
// `arena_base` is `workspace_base + workspace_bytes`, also 8-aligned.
struct StaticLayout {
  WasmAnnotations annotations;

  // One entry per referenced variable, in first-seen order
  // (local_index 0, 1, 2, ...).  Each entry's `slot_offset` is the
  // absolute linear-memory byte offset of its 24-byte CelValue slot.
  // `variables.size()` is also the count of wasm locals the lowered
  // `$eval` declares — one i32 per referenced variable.
  std::vector<LaidOutVariable> variables;

  // Attribute intern table — carried forward from ResolveOutput
  // so `BuildCelAbi` can serialise it into `cel.abi.attributes[]`.
  // Entry 0 is the reserved sentinel.  Populated by ResolvePass
  // (M2.E); LayoutPass neither reads nor mutates it.
  std::vector<AttributeEntryRow> attributes;

  // M7.A: message-type intern table — carried forward from
  // ResolveOutput so `BuildCelAbi` can serialise it into
  // `cel.abi.types[]`.  Entry 0 is the reserved sentinel.
  // Populated by ResolvePass's `MessageTypeIdVisitor` (M7.A);
  // LayoutPass neither reads nor mutates it.
  std::vector<MessageTypeRow> message_types;

  std::vector<uint8_t> rodata;
  uint32_t rodata_base = 16;
  uint32_t workspace_base = 0;
  uint32_t workspace_bytes = 0;
  uint32_t arena_base = 0;

  uint32_t peak_slots = 0;
  bool debug_mode = false;
};

// Runs the layout pass on a resolved TypedAst.  Takes the ResolveOutput
// by value — its annotations are extended in place with `.storage`
// writes and returned as part of the StaticLayout.  M1 implements only
// the kConst arm: the pass visits every Constant node, dispatches on its
// variant, packs the CelValue into rodata via StaticMemoryBuilder, and
// writes `{StorageKind::kStaticRodata, offset}` onto the annotation.
//
// Non-kConst expression kinds leave their storage at the zero sentinel.
// expr_lower rejects them with `absl::UnimplementedError` at M1, so the
// zero sentinel is never consumed; M2/M3/M5 will fill in storage for
// idents, calls, and comprehension scopes as those kinds come online.
ABSL_MUST_USE_RESULT absl::StatusOr<StaticLayout> LayoutPass(
    const TypedAst& ast, ResolveOutput resolved,
    const LayoutOptions& opts = {});

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_LAYOUT_PASS_H_
