// Package main is a throwaway probe validating the celfnc foreign-Go ABI
// for a PROTO-message arg via SERIALIZATION (user-guide §8.5):
// `bool isAdult(User user)`.
//
// Per the design, a proto crosses the foreign boundary as protobuf-BINARY
// bytes: the host serializes the message, passes (ptr,len) into THIS module's
// memory, and the generated glue proto.Unmarshal's it into the Go-generated
// User type. This file validates that proto.Unmarshal actually links + runs
// inside a Go wasip1 wasm module.
package main

import (
	"unsafe"

	"google.golang.org/protobuf/proto"
	userpb "protocase/userpb"
)

// ---- USER CODE (natural Go) ----------------------------------------------

// IsAdult reports whether the user is at least 18.
func IsAdult(u *userpb.User) bool {
	return u.GetAge() >= 18
}

// ---- GENERATED GLUE (what celfnc must emit for a proto->bool fn) ---------

func bytesFromMem(ptr, length uint32) []byte {
	if length == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(uintptr(ptr))), int(length))
}

// isAdult is the fixed-ABI export. The (ptr,len) points at the host-serialized
// acme.User protobuf bytes in THIS module's memory. The glue Unmarshals them
// into the generated User type, then calls the user's IsAdult.
//
//go:wasmexport isAdult
func isAdult(ptr, length uint32) uint32 {
	var u userpb.User
	if err := proto.Unmarshal(bytesFromMem(ptr, length), &u); err != nil {
		// A real shim writes kError to the return area / signals the host;
		// for a bool-return probe we surface as false (and the host could
		// inspect a separate status). Marshalling failure is a contract bug.
		return 0
	}
	if IsAdult(&u) {
		return 1
	}
	return 0
}

//go:wasmexport celfn_realloc
func celfn_realloc(ptr, oldLen, align, newLen uint32) uint32 {
	if newLen == 0 {
		return 0
	}
	buf := make([]byte, newLen)
	p := uint32(uintptr(unsafe.Pointer(unsafe.SliceData(buf))))
	pins[p] = buf
	if ptr != 0 {
		if old, ok := pins[ptr]; ok {
			copy(buf, old)
			delete(pins, ptr)
		}
	}
	return p
}

var pins = map[uint32][]byte{}

func main() {}
