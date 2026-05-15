// type(x) — M9.B type-subsystem runtime kernel.
//
// Carved out of cel_runtime.c per
// `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md`
// (post-M10 follow-up — `cel_type.h` shipped alongside M9; the body
// stayed in cel_runtime.c until now).  See cel_type.h for the public
// ABI and m9-type-subsystem.md for the design rationale.

#include "compiler_v2/runtime/cel_type.h"

#include <stddef.h>
#include <stdint.h>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_internal.h"

// Indexed by CelKind value.  NULL entries are kinds the helper
// does not handle directly: CEL_MESSAGE dispatches to the host
// trampoline (M9.C); CEL_OPTIONAL is an optionals-pass concern
// (out of M9 scope); CEL_UNKNOWN / CEL_ERROR are absorbed by the
// 3VL prelude before reaching this table.
//
// CelKind tail value is CEL_LIST_HOST = 17, so the array has 18
// slots.
static const char* const kPrimitiveTypeName[18] = {
    "null_type",                  // CEL_NULL = 0
    "bool",                       // CEL_BOOL = 1
    "int",                        // CEL_INT = 2
    "uint",                       // CEL_UINT = 3
    "double",                     // CEL_DOUBLE = 4
    "string",                     // CEL_STRING = 5
    "bytes",                      // CEL_BYTES = 6
    "list",                       // CEL_LIST_ARENA = 7
    "map",                        // CEL_MAP_ARENA = 8
    "map",                        // CEL_MAP_HOST = 9
    NULL,                         // CEL_MESSAGE = 10  (host)
    "type",                       // CEL_TYPE = 11
    "google.protobuf.Duration",   // CEL_DURATION = 12
    "google.protobuf.Timestamp",  // CEL_TIMESTAMP = 13
    NULL,                         // CEL_OPTIONAL = 14 (optionals-pass)
    NULL,                         // CEL_UNKNOWN = 15 (absorbed)
    NULL,                         // CEL_ERROR = 16 (absorbed)
    "list",                       // CEL_LIST_HOST = 17
};

// Forward decl of the M9.C host trampoline.  Same import pattern as
// `cel_host_cel_map_lookup`: `__wasm__` ⇒ `import_module` attribute
// (resolved at instantiation by wasmtime); host build ⇒ weak no-op
// stub (poison kTypeMismatch) so unit tests link without the wasmtime
// trampoline.  M9.C lands the strong override in
// `compiler_v2/api/internal/cel_host.cc` for both directions.
#ifdef __wasm__
extern void cel_host_resolve_message_type_name(uint32_t out_slot,
                                               uint32_t in_slot)
    __attribute__((import_module("cel_host"),
                   import_name("resolve_message_type_name")));
#else
__attribute__((weak)) void
cel_host_resolve_message_type_name(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t in_slot) {
  (void)in_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

void cel_type_of_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind == CEL_MESSAGE) {
    // M9.C trampoline.  Until M9.C ships the host wiring, this call
    // traps "unknown import" at wasm runtime — a clean failure
    // mode, not a silent miscompile.  The host build links against
    // a stub that lives in `cel_host.cc` (M9.C).
    cel_host_resolve_message_type_name(out_slot, in_slot);
    return;
  }
  const char* name = NULL;
  if (in->kind < (sizeof(kPrimitiveTypeName) / sizeof(kPrimitiveTypeName[0]))) {
    name = kPrimitiveTypeName[in->kind];
  }
  if (name == NULL) {
    // Unknown / unhandled kind (e.g. CEL_OPTIONAL) — surface a clean
    // type-mismatch error so callers see a rejection rather than a
    // miscompile.
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Compute length without strlen (freestanding wasm doesn't link
  // libc by default; spell out the byte-loop).
  uint32_t len = 0;
  while (name[len] != 0) {
    ++len;
  }
  // Allocate fresh bytes in the per-Eval arena and copy the name in.
  // cel_alloc bumps the cursor by align_up(n, 8) and returns 0 on
  // OOM (or when n==0).  Even an empty name doesn't reach here —
  // every entry in `kPrimitiveTypeName` is non-empty — but defend
  // anyway for future kinds.
  uint32_t off = cel_alloc(len);
  if (off == 0 && len > 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint8_t* base = cel_memory_base_();
  for (uint32_t i = 0; i < len; ++i) {
    base[off + i] = (uint8_t)name[i];
  }
  out->kind = CEL_TYPE;
  out->_pad = 0;
  out->payload.s.ptr = off;
  out->payload.s.len = len;
}
