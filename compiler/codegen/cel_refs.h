// Host-owned reference helpers for the eval module.
//
// See `doc/wasm-compiler-design.md` §7.1 — the compiler emits a
// dedicated `externref` table (`$cel_refs`) so proto messages and
// other host-owned values can travel through evaluated CEL expressions
// as opaque handles.  The helpers here are the write side of that
// table: a monotonic slot allocator plus a lookup function plus a
// reset hook the host uses between evaluations.
//
// Scope (M2): slot-table plumbing only.  The `CelValue`-shaped helpers
// `cel_wrap_message` / `cel_unwrap_message` land with the runtime
// wasm32 cross-compile (deliverable #3 of M2) because they need
// `cel_alloc` in the same module.
//
// Emitted WAT for reference:
//
//   (global $cel_refs_next (mut i32) (i32.const 1))
//
//   (func $cel_ref_intern (param $r externref) (result i32)
//     (local $slot i32)
//     (local.set $slot (global.get $cel_refs_next))
//     (table.set $cel_refs (local.get $slot) (local.get $r))
//     (global.set $cel_refs_next
//       (i32.add (local.get $slot) (i32.const 1)))
//     (local.get $slot))
//
//   (func $cel_ref_get (param $slot i32) (result externref)
//     (table.get $cel_refs (local.get $slot)))
//
//   (func $cel_refs_reset
//     (global.set $cel_refs_next (i32.const 1)))
//
// Slot 0 is reserved as a null sentinel so wrapper code can encode
// "no host handle" with a single i32 test.

#ifndef CELWASM_COMPILER_CODEGEN_CEL_REFS_H_
#define CELWASM_COMPILER_CODEGEN_CEL_REFS_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/module.h"

namespace celwasm {

// Declares `$cel_refs` as an externref table, emits the bump global
// and the three helper functions, and exports table + functions under
// their internal names.
//
// `table_name` is the internal (and exported) name of the table —
// pass `"$cel_refs"` to match the design doc.  `initial_slots` must
// be at least 2: slot 0 is the null sentinel, slot 1 is the first
// slot `cel_ref_intern` will hand out.
//
// Emitted function exports: `cel_ref_intern`, `cel_ref_get`,
// `cel_refs_reset`.  The table itself is also exported under
// `table_name`.
ABSL_MUST_USE_RESULT absl::Status AddCelRefsTableAndHelpers(
    WasmModule& mod, absl::string_view table_name, uint32_t initial_slots);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_CEL_REFS_H_
