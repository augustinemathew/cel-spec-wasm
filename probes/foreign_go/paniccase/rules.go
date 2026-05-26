// Probe: how do Go panics cross the foreign wasm boundary?
//
// Validates the §5.2 step-4 claim ("kError for a foreign trap or contract
// violation"). Tests several failure modes a real user fn could hit:
//   1. explicit panic()           -- a Go runtime panic
//   2. nil pointer dereference     -- a runtime panic from bad memory access
//   3. index out of range          -- a runtime panic
//   4. recovered panic             -- the shim recovers and returns an error code
// plus a clean success for contrast and post-failure reuse testing.
package main

import "unsafe"

// ---- failure-mode entry points (each //go:wasmexport so the host can call) ----

//go:wasmexport ok
func ok() uint32 { return 7 } // sentinel success

//go:wasmexport doPanic
func doPanic() uint32 {
	panic("user fn blew up")
}

//go:wasmexport doNilDeref
func doNilDeref() uint32 {
	var p *uint32
	return *p // nil deref -> runtime panic
}

//go:wasmexport doOOB
func doOOB(i uint32) uint32 {
	s := []uint32{1, 2, 3}
	return s[i] // i >= 3 -> index out of range panic
}

// doRecovered models a GENERATED-SHIM strategy: wrap the user call in a
// recover() so a panic becomes a clean error return (status code) instead of
// a trap. Returns 0xFFFFFFFF on panic, else the value.
//
//go:wasmexport doRecovered
func doRecovered(shouldPanic uint32) (result uint32) {
	defer func() {
		if r := recover(); r != nil {
			result = 0xFFFFFFFF // sentinel: "foreign contract violation"
		}
	}()
	if shouldPanic != 0 {
		panic("recoverable")
	}
	return 42
}

// doRecoveredRuntime models the shim recovering a RUNTIME panic (nil deref),
// which is the realistic user-code failure -- not an explicit panic().
//
//go:wasmexport doRecoveredRuntime
func doRecoveredRuntime() (result uint32) {
	defer func() {
		if r := recover(); r != nil {
			result = 0xFFFFFFFF
		}
	}()
	var p *uint32
	return *p // nil deref
}

//go:wasmexport celfn_realloc
func celfn_realloc(ptr, oldLen, align, newLen uint32) uint32 {
	if newLen == 0 {
		return 0
	}
	buf := make([]byte, newLen)
	p := uint32(uintptr(unsafe.Pointer(unsafe.SliceData(buf))))
	pins[p] = buf
	return p
}

var pins = map[uint32][]byte{}

func main() {}
