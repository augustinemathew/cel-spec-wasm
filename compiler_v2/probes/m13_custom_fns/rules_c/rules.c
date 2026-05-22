// M13 Probe 3 — Bare-C foreign wasm module.
//
// The C-no-stdlib variant of Probe 2's Go module.  Exports the same
// `allow_string_string` symbol with the same `(out_slot, *arg_slots)
// → void` ABI; the harness (m13_p3_c_test.cc) is byte-identical to
// m13_p2_test.cc except for the wasm path.
//
// Build via build_rules.sh.  Requires brew's LLVM (Apple's clang
// ships wasm32 backend but no `--target=wasm32` linker support);
// uses `/opt/homebrew/opt/llvm/bin/clang`.

#include <stdint.h>

// CelKind tags — must match compiler_v2/runtime/cel_data.h.
// Wire values frozen as of M13.A.
enum CelKind {
    CEL_BOOL = 1,
    CEL_STRING = 5,
};

// CelValue layout — 24 bytes, must match the canonical struct in
// compiler_v2/runtime/cel_data.h.  Packed for explicit layout.
typedef struct {
    uint32_t kind;
    uint32_t _pad;
    uint8_t  payload[16];
} CelValue;

// Foreign-fn entry point.  Reads CelValues at arg slot offsets,
// writes the result CelValue at out_slot.  Same shape and naming
// convention as the Go-authored counterpart in
// `compiler_v2/probes/m13_custom_fns/rules/rules.go`.
//
// CEL declaration this targets:
//
//     bool rules.allow(this string userId, string resource);
//
// Synthesised overload-id: `allow_string_string`.
__attribute__((export_name("allow_string_string")))
void allow(uint32_t out, uint32_t user_id, uint32_t resource) {
    CelValue* u = (CelValue*)(uintptr_t)user_id;
    CelValue* r = (CelValue*)(uintptr_t)resource;

    // Validate args have the expected CelKind tags (proves we read
    // shared memory correctly across the wasm boundary).  A real
    // foreign fn would decode the CelSpan {ptr, len} payload to
    // read the actual string bytes; the probe doesn't need that
    // to validate the ABI shape.
    int ok = (u->kind == CEL_STRING) && (r->kind == CEL_STRING);

    CelValue* o = (CelValue*)(uintptr_t)out;
    o->kind = CEL_BOOL;
    // Bool value lands in payload[0..8) as a 64-bit int.  Matches
    // the canonical layout (payload.b is i32 at byte offset 0,
    // padded to 8 bytes for alignment).
    uint64_t* slot = (uint64_t*)&o->payload[0];
    *slot = (uint64_t)(ok ? 1 : 0);
}
