// M13 Probe 2 — Go-authored foreign wasm module.
//
// Stand-in for what a real user would write to back a `Foreign`-aliased
// CEL custom function from Go.  The Go side of the M13 cross-language
// ABI contract (§4.5 of doc/implementation-plan/rewrite/m13-custom-fns.md).
//
// CEL declaration this targets:
//
//   bool rules.allow(this string userId, string resource);
//
// (Receiver-style with string receivers — proto messages are
// disallowed across the foreign-wasm boundary in v1; see §4.5.1
// of m13-custom-fns.md.)
//
// Synthesised overload-id (per the .celfn grammar's argkind rules):
//
//   allow_string_string
//
// Build (requires TinyGo on PATH; brew install tinygo on darwin):
//
//   tinygo build -target=wasm-unknown -no-debug \
//     -o rules.wasm ./compiler_v2/probes/m13_custom_fns/rules/
//
// The resulting `rules.wasm` is drop-in compatible with the Probe 1
// harness — swap the stub WAT for it and Probe 1's assertions still
// pass.  That's the entire point of Probe 2: prove TinyGo can hit
// the ABI without help.
//
// THIS FILE IS NOT YET BUILT BY BAZEL.  Probe 2 is gated on TinyGo
// being installed; see doc/implementation-plan/rewrite/m13-probes.md
// for the toolchain status.

package main

import "unsafe"

// CelKind tags.  Wire values frozen as of M13.A; the
// `cel.toolchain` custom section celfnc emits asserts match against
// the engine's ABI version at Engine::Instantiate.
//
// Must match compiler_v2/runtime/cel_data.h's `CelKind` enum.
const (
	celBool   = 1
	celString = 5
)

// CelValue layout — must match the 24-byte struct in
// compiler_v2/runtime/cel_data.h.  `Payload` is a raw 16-byte union;
// the layout is kind-driven and the typed bindings live in
// `celfnc`-generated stubs (Probe 4).  For Probe 2 we operate on
// the bytes directly to keep the toolchain surface minimal.
type celValue struct {
	Kind    uint32
	_pad    uint32
	Payload [16]byte
}

func slotAt(ptr uint32) *celValue {
	return (*celValue)(unsafe.Pointer(uintptr(ptr)))
}

// allow — exported wasm function.
//
// The exported name MUST match the overload-id the .celfn parser
// synthesises from the declaration.  TinyGo's `//go:wasmexport`
// directive (Go 1.21+ via TinyGo's wasm-unknown target) preserves
// the function name verbatim in the wasm `(export …)` section.
//
//go:wasmexport allow_string_string
func allow(out, userID, resource uint32) {
	u := slotAt(userID)
	r := slotAt(resource)

	// For Probe 2 the goal is "prove TinyGo can read the args and
	// write a result through the shared CelValue ABI" — not "make
	// a realistic decision."  Validate the args have the expected
	// kinds (proves we read shared memory correctly) and write
	// the bool conjunction as the result.
	//
	// A real foreign fn would decode the strings from shared memory
	// (CelSpan = {ptr, len} at payload offset 0) and inspect their
	// bytes.  The decode is straightforward — see comments inline
	// — but the probe doesn't need it to validate the ABI shape.
	ok := u.Kind == celString && r.Kind == celString

	o := slotAt(out)
	o.Kind = celBool
	// Write `1` for true, `0` for false into the leading 8 bytes of
	// the payload union — matches CelValue::payload.b (i32 at
	// offset 8) padded to 8 bytes for alignment.
	*(*uint64)(unsafe.Pointer(&o.Payload[0])) = boolU64(ok)
}

func boolU64(b bool) uint64 {
	if b {
		return 1
	}
	return 0
}

// TinyGo's wasm-unknown target still requires a main entry, but it
// does not run any startup code beyond what's strictly needed for
// the runtime — the function is effectively empty for our purposes.
func main() {}
