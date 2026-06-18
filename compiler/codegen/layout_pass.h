#ifndef CELWASM_COMPILER_CODEGEN_LAYOUT_PASS_H_
#define CELWASM_COMPILER_CODEGEN_LAYOUT_PASS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/resolve_pass.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "compiler/memory_layout.h"

namespace celwasm {

// One referenced variable after layout: the `ResolvedVariable` shape
// extended with the absolute linear-memory offset of its 24-byte
// workspace CelValue slot.  Populated by LayoutPass; consumed by:
//
//   - `expr_lower.cc`
//       * `$eval` prelude — one `BinaryenLocalSet(local_index,
//         i32.const <slot_offset>)` per variable.
//       * kIdent arm — `BinaryenLocalGet(local_index, i32)`.
//       (Per `rewrite/m2-ident-select-unknowns.md` §2.6.)
//   - the cel.abi module emitter — writes `cel.abi.variables[]` so
//     the host-side Activation marshal (`Instance::Eval`) can encode
//     each bound Value into the CelValue wire format at `slot_offset`
//     before calling `$eval`.
struct LaidOutVariable {
  std::string name;
  uint32_t local_index = 0;
  Repr repr = Repr::kUnknown;
  // Workspace slot byte offset for variables whose wasm local holds
  // a slot pointer (`kFreeVariable`, `kComprehensionAccu`).  Zero
  // for `kComprehensionIter` — the iter local holds a *moving*
  // pointer into the iter_range's element run (list iteration) or
  // the output of `cel_map_iter_key_at` (map iteration); no fixed
  // workspace cell.
  uint32_t slot_offset = 0;
  // Propagated from `ResolvedVariable::kind`.  EmitVariablePrelude
  // skips entries with `kind != kFreeVariable` — comp-scope
  // entries are set by the comprehension's loop prologue, not by
  // the function prelude.  ABI emitter also filters to
  // `kFreeVariable` entries when populating `cel.abi.variables[]`.
  ResolvedVariableKind kind = ResolvedVariableKind::kFreeVariable;
};

// Layout-time knobs for the pass.  Today: `debug_layout` turns off slot
// reuse (each workspace `Acquire` hands out a fresh cell).
struct LayoutOptions {
  bool debug_layout = false;
  // Override `StaticLayout::rodata_base`.  0 (the default) means
  // "use the design default" — 16, immediately past the two
  // reserved low slots.  Non-zero values shift every rodata-,
  // workspace-, and arena-derived offset by the same amount.
  //
  // Exists so multiple modules instantiated against the same shared
  // `cel.memory` can be given non-overlapping rodata ranges — without
  // this, two such modules would write their data segments on top of
  // each other (see the WAT trace at `wat/45b_foo_module.wat`).
  uint32_t rodata_base_override = 0;
};

// Compile-time message prefix returned by `LayoutPass` when the
// chosen expression would need more workspace than the reserved
// low region of `cel.memory` permits.  The actual byte cap is
// computed dynamically from the rodata footprint via
// `MemoryLayout::MaxWorkspaceBytes` — there is no single constant
// limit, because rodata and workspace share the [16, 262144) window
// below wasi-libc's static data.  An expression with no rodata
// can use the full headroom; rodata (including materialized const
// aggregates) eats into it before the gate trips.
inline constexpr absl::string_view kSlotExhaustedMessagePrefix =
    "expression requires too many workspace slots";

// Output of the second codegen pipeline pass.  Extends `annotations` from
// ResolveOutput by writing `.storage` on every node whose result has a
// known memory location at compile time:
//   - `kConst` nodes (rodata-packed)
//   - `kIdent` nodes (workspace-slot pointing at the referenced
//     variable's 24-byte CelValue cell)
//   - `kSelect` / `kCallExpr` / `kMapExpr` / `kListExpr` /
//     `kStructExpr` / `kComprehensionExpr` (workspace slots
//     assigned by `SlotAllocator`).
// Other non-populated kinds are left with `.storage.kind == kNone`;
// expr_lower CHECKs or returns `Unimplemented` for them.
//
// Memory map the output describes:
//
//     [0..8)     reserved (null sentinel)
//     [8..16)    reserved (legacy arena cursor/limit slot — no
//                longer consulted by codegen; see
//                `rewrite/wasi/DESIGN.md` §4)
//     [rodata_base..rodata_base+rodata.size())
//                active data segment (kConst CelValues + payload bytes)
//     [workspace_base..workspace_base+workspace_bytes)
//                24B cells — one per referenced variable, plus
//                SlotAllocator-owned scratch cells
//     [arena_base..)  grows forward via arena_alloc (lives in
//                     wasi-libc dlmalloc heap; see
//                     `rewrite/wasi/DESIGN.md` §4)
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
  // (see `rewrite/m2-ident-select-unknowns.md`); LayoutPass
  // neither reads nor mutates it.
  std::vector<AttributeEntryRow> attributes;

  // Message-type intern table — carried forward from ResolveOutput
  // so `BuildCelAbi` can serialise it into `cel.abi.types[]`.
  // Entry 0 is the reserved sentinel.  Populated by ResolvePass's
  // `MessageTypeIdVisitor` (see `rewrite/m7-proto-literals.md`);
  // LayoutPass neither reads nor mutates it.
  std::vector<MessageTypeRow> message_types;

  std::vector<uint8_t> rodata;
  uint32_t rodata_base = MemoryLayout::kRodataBaseMin;
  uint32_t workspace_base = 0;
  uint32_t workspace_bytes = 0;
  uint32_t arena_base = 0;

  uint32_t peak_slots = 0;
  bool debug_mode = false;

  // Per-comprehension auxiliary wasm locals (e.g. the
  // list-iteration `end_off` pointer; the map-iteration cursor; the
  // two-iter-var index counter; the source-address i32 for host
  // list/map iter_ranges).  Codegen for `kComprehensionExpr` emits
  // `local.set` against these on entry to the comprehension's loop
  // prologue.  Pre-allocated at LayoutPass time so
  // `LowerToEvalFunction` knows the total local count when calling
  // `BinaryenAddFunction`.  Three indices per comp covers every
  // shape (Shape A list: end_off + source_addr [+ index for
  // two-iter]; Shape B map: iter_cursor + source_addr; Shape C
  // cel.bind: none used).  source_addr (aux0+2) holds the
  // kind-dispatched source slot returned by `cel_list_arena_view`
  // for lists, or is unused for maps (`cel_map_iter_init`
  // dispatches internally).  See `rewrite/m5-comprehensions-
  // design.md` + m5b §CCF-8.
  uint32_t comprehension_extra_locals_per_comp = 3;
  // Total local count = `variables.size()` + 3 × (#comprehensions).
  uint32_t total_wasm_locals = 0;
};

// Runs the layout pass on a resolved TypedAst.  Takes the ResolveOutput
// by value — its annotations are extended in place with `.storage`
// writes and returned as part of the StaticLayout.  See
// `rewrite/design.md` §6 LayoutPass.  Expression kinds with no
// allocated storage leave `.storage.kind == kNone`; expr_lower
// CHECKs or returns `Unimplemented` for them.
ABSL_MUST_USE_RESULT absl::StatusOr<StaticLayout> LayoutPass(
    const TypedAst& ast, ResolveOutput resolved,
    const LayoutOptions& opts = {});

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_LAYOUT_PASS_H_
