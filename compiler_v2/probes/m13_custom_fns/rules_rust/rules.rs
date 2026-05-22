// M13 Probe 3 — Rust foreign wasm module.
//
// The Rust variant of Probe 2's TinyGo module / Probe 3's bare-C
// module.  Exports the same `allow_string_string` symbol with the
// same `(out_slot, *arg_slots) → void` ABI; the harness
// (m13_p3_rust_test.cc) is byte-identical to m13_p2_test.cc except
// for the wasm path.
//
// Build via build_rules.sh.  Requires rustup with the
// `wasm32-unknown-unknown` target installed
// (`rustup target add wasm32-unknown-unknown`).

#![no_main]
#![no_std]

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

// CelKind tags.  Wire values frozen as of M13.A; must match
// compiler_v2/runtime/cel_data.h's `CelKind` enum.
const CEL_BOOL: u32 = 1;
const CEL_STRING: u32 = 5;

// CelValue layout — must match the 24-byte struct in
// compiler_v2/runtime/cel_data.h.  `#[repr(C)]` pins the layout to
// the documented C ABI.
#[repr(C)]
struct CelValue {
    kind: u32,
    _pad: u32,
    payload: [u8; 16],
}

#[inline]
unsafe fn slot_at<'a>(ptr: u32) -> &'a mut CelValue {
    &mut *(ptr as usize as *mut CelValue)
}

// allow — exported wasm function.
//
// `#[no_mangle]` preserves the symbol name; `extern "C"` pins the
// calling convention; `#[export_name(...)]` would let us rename
// the export but we want `allow_string_string` to match the
// synthesised overload-id verbatim, so just naming the Rust
// function `allow_string_string` is sufficient.
//
// CEL declaration this targets:
//
//     bool rules.allow(this string userId, string resource);
//
// Synthesised overload-id: `allow_string_string`.
#[no_mangle]
pub unsafe extern "C" fn allow_string_string(out: u32, user_id: u32, resource: u32) {
    let u = slot_at(user_id);
    let r = slot_at(resource);

    // Same shape as the Go / C probes: validate arg kinds, write a
    // bool result.  A real foreign fn would decode the strings from
    // shared memory (CelSpan = {ptr, len} at payload offset 0).
    let ok = u.kind == CEL_STRING && r.kind == CEL_STRING;

    let o = slot_at(out);
    o.kind = CEL_BOOL;
    // Bool value lands in payload[0..8) as a u64.  Matches the
    // canonical layout (payload.b is i32 at byte offset 0, padded
    // to 8 bytes for alignment).
    let slot_u64 = &mut o.payload as *mut [u8; 16] as *mut u64;
    *slot_u64 = if ok { 1 } else { 0 };
}
