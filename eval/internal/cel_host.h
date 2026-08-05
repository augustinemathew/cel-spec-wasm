// cel_host — host-side helpers the wasm expr module calls via its
// `cel_host.*` imports.  Three-layer split (see
// `doc/implementation-plan/rewrite/cel-host-surface.md` §4):
//
//   Layer 1: HostMessageBacking — pure CEL field-read semantics.
//   Layer 2: trampoline — adapts Layer 1 to the wasm ABI
//            (runtime-agnostic, driven by MemoryView / ExternrefTable /
//            ArenaAllocator; per-family TUs below).
//   Layer 3: wasmtime glue.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_H_

// Aggregator header.  The former monolith is split by trampoline
// family; existing consumers keep this single include, and each
// family header remains includable on its own.
#include "eval/internal/cel_host_backing.h"  // IWYU pragma: export
#include "eval/internal/cel_host_common.h"   // IWYU pragma: export
#include "eval/internal/cel_host_list.h"     // IWYU pragma: export
#include "eval/internal/cel_host_map.h"      // IWYU pragma: export
#include "eval/internal/cel_host_memory.h"   // IWYU pragma: export
#include "eval/internal/cel_host_message.h"  // IWYU pragma: export
#include "eval/internal/cel_host_time.h"     // IWYU pragma: export

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_H_
