#ifndef CELWASM_COMPILER_V2_CODEGEN_LAYOUT_PASS_H_
#define CELWASM_COMPILER_V2_CODEGEN_LAYOUT_PASS_H_

#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "binaryen-c.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

// Layout-time knobs for the pass.  Today: `debug_layout` turns off slot
// reuse (each workspace `Acquire` hands out a fresh cell).  M1 has no
// workspace slots to acquire, so the flag is plumbed but inert.
struct LayoutOptions {
  bool debug_layout = false;
};

// Output of the second codegen pipeline pass.  Extends `annotations` from
// ResolveOutput by writing `.storage` on every node whose result has a
// known memory location at compile time.  M1 only populates storage for
// `kConst` nodes (all land in `.rodata`); non-kConst nodes are left with
// `.storage.kind == kNone`, which is fine because expr_lower returns
// `absl::UnimplementedError` for them at M1.
//
// Memory map the output describes:
//
//     [0..8)     reserved (null sentinel)
//     [8..16)    arena cursor + limit (bytes 8/12) — written by cel_reset
//     [rodata_base..rodata_base+rodata.size())
//                active data segment (kConst CelValues + payload bytes)
//     [workspace_base..workspace_base+workspace_bytes)
//                SlotAllocator-owned 24B cells (empty at M1)
//     [arena_base..)  grows forward via cel_alloc
//
// `rodata_base` is fixed at 16 (skip past the two reserved slots).
// `workspace_base` is `rodata_base + rodata.size()` rounded up to 8;
// `arena_base` is `workspace_base + workspace_bytes`, also 8-aligned.
struct StaticLayout {
  WasmAnnotations annotations;

  // NOLINTNEXTLINE(readability-redundant-member-init) — explicit defaults
  // satisfy cppcoreguidelines-pro-type-member-init on the aggregate
  // without forcing callers into a boilerplate constructor.
  std::vector<uint8_t> rodata = {};
  uint32_t rodata_base = 16;
  uint32_t workspace_base = 0;
  uint32_t workspace_bytes = 0;
  uint32_t arena_base = 0;

  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::vector<BinaryenType> local_types = {};

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
