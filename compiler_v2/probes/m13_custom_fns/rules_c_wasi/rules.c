// M13 Probe 3 — WASI-SDK C foreign wasm module.
//
// The wasi-libc-linked C variant of Probe 3's bare-C module.
// Same source shape; different build flags.  The build_rules.sh
// uses `--target=wasm32-wasi` + `-mexec-model=reactor`, which
// produces a wasm module that:
//
//   * imports `wasi_snapshot_preview1.*` syscalls (for any libc
//     functions that need them — though this minimal source uses
//     none),
//   * exports `_initialize` (reactor model — must be called once
//     before other exports; analogous to TinyGo's `_initialize`),
//   * exports `memory` (defines its own),
//   * exports our `allow_string_string`.
//
// This is the toolchain a real production C user would reach for:
// it's stable, has a complete libc, supports threads (sort of),
// and is what wasi-sdk ships out of the box.  The bare-C path
// (rules_c/) is for the embedded / no-libc case; this one is
// "C as people actually write it."

#include <stdint.h>

enum CelKind {
    CEL_BOOL = 1,
    CEL_STRING = 5,
};

typedef struct {
    uint32_t kind;
    uint32_t _pad;
    uint8_t  payload[16];
} CelValue;

__attribute__((export_name("allow_string_string")))
void allow(uint32_t out, uint32_t user_id, uint32_t resource) {
    CelValue* u = (CelValue*)(uintptr_t)user_id;
    CelValue* r = (CelValue*)(uintptr_t)resource;

    int ok = (u->kind == CEL_STRING) && (r->kind == CEL_STRING);

    CelValue* o = (CelValue*)(uintptr_t)out;
    o->kind = CEL_BOOL;
    uint64_t* slot = (uint64_t*)&o->payload[0];
    *slot = (uint64_t)(ok ? 1 : 0);
}
