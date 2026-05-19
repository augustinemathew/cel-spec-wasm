// CEL WASM runtime — umbrella header.
//
// The runtime is split across topic-specific headers so callers can
// include only what they use; this file re-exports all of them for
// backwards compatibility and for call sites that want the whole
// surface.
//
//   cel_data.h    — CelKind, CelValue, CelSpan / CelArray / CelMap,
//                   error codes.  Pure types; no function decls.
//   cel_memory.h  — cel_mem_base / cel_mem_size accessors over the
//                   shared linear memory.
//   cel_arena.h   — cel_reset / arena_alloc / cel_value_at bump-allocator
//                   helpers.
//   cel_make.h    — cel_make_* scalar constructors.
//   cel_log.h     — cel_log trampoline, CEL_LOG macros, tag enum.
//
// Memory model (parent design doc §8.2): the expr module defines and
// exports `(memory $mem 1)`; the runtime module imports it via
// `(import "cel" "memory" (memory $mem 1))`.  Every "pointer" is a
// 32-bit byte offset into that shared memory.  Arena state lives at
// fixed offsets:
//
//   [0..8)    reserved sentinel (offset 0 = "absent" everywhere)
//   [8..12)   u32  bump
//   [12..16)  u32  limit
//   [16..)    .rodata followed by the bump arena
//
// Codegen emits `cel_reset(arena_base, arena_limit)` at the top of
// each generated `eval()` (both args compile-time constants), so the
// host just instantiates and calls `eval()` — no separate init phase.
//
// M1 deliberately ships the minimum: CelValue layout, arena, and the
// scalar `cel_make_*` helpers.  Arithmetic / comparison / 3VL /
// collection primitives land with the milestones that add the
// corresponding codegen arms.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_RUNTIME_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_RUNTIME_H_

#include "compiler_v2/runtime/cel_3vl.h"
#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_arith.h"
#include "compiler_v2/runtime/cel_compare.h"
#include "compiler_v2/runtime/cel_convert.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_list.h"
#include "compiler_v2/runtime/cel_log.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_map.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "compiler_v2/runtime/cel_string_ops.h"
#include "compiler_v2/runtime/cel_type.h"

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_RUNTIME_H_
