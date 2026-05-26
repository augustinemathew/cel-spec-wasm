// Package main is a throwaway probe validating the celfnc foreign-Go ABI
// for a string -> bool function (`bool isValidName(string foo)`).
//
// Layered exactly as celfnc would emit:
//   - the USER writes the natural Go function `IsValidName`.
//   - celfnc emits the GLUE below: a per-fn `//go:wasmexport isValidName`
//     entry that lifts the (ptr,len) string from THIS module's own linear
//     memory, plus a `//go:wasmexport celfn_realloc` the host calls to
//     place the argument bytes in this module's memory.
package main

import "unsafe"

// ---- USER CODE (natural Go; celfnc never touches this) -------------------

// IsValidName reports whether foo is a non-empty name starting with A-Z.
func IsValidName(foo string) bool {
	return len(foo) > 0 && foo[0] >= 'A' && foo[0] <= 'Z'
}

// ---- GENERATED GLUE (what celfnc must emit) ------------------------------

// strFromMem lifts a (ptr,len) byte run from this module's OWN linear
// memory into a Go string WITHOUT copying. The bytes were written there by
// the host trampoline via celfn_realloc just before this call, and remain
// valid for the duration of the call (§5.5 inbound-arg lifetime).
func strFromMem(ptr, length uint32) string {
	if length == 0 {
		return ""
	}
	return unsafe.String((*byte)(unsafe.Pointer(uintptr(ptr))), int(length))
}

// isValidName is the fixed-ABI export the host trampoline calls.
// Signature: (i32 ptr, i32 len) -> i32 (0/1). Named after the overload id.
//
//go:wasmexport isValidName
func isValidName(ptr, length uint32) uint32 {
	if IsValidName(strFromMem(ptr, length)) {
		return 1
	}
	return 0
}

// celfn_realloc is the cabi_realloc-shaped allocator the host uses to place
// inbound argument bytes into THIS module's memory. ptr==0 => fresh alloc.
// We back it with a Go-managed pinned buffer pool so the GC cannot move or
// reclaim the bytes while the host writes into them and the call runs.
//
//go:wasmexport celfn_realloc
func celfn_realloc(ptr, oldLen, align, newLen uint32) uint32 {
	if newLen == 0 {
		return 0
	}
	// Fresh allocation: a Go byte slice. We keep it alive in `pins` and hand
	// back the address of its backing array. Single-threaded; freed lazily.
	buf := make([]byte, newLen)
	p := uint32(uintptr(unsafe.Pointer(unsafe.SliceData(buf))))
	pins[p] = buf
	if ptr != 0 {
		// grow: copy old contents forward, drop old pin.
		if old, ok := pins[ptr]; ok {
			copy(buf, old)
			delete(pins, ptr)
		}
	}
	return p
}

// pins keeps realloc'd buffers reachable so the GC never collects/moves them
// across the host write + foreign call. A real celfnc shim would reset this
// per-call; for the probe we leak (single-shot calls).
var pins = map[uint32][]byte{}

// main is required for a Go wasip1 build; a reactor has no real entry point.
func main() {}
