// rules.go — USER CODE. celfnc generates this ONCE as empty //TODO stubs
// (signatures derived from the .celfn); you fill in the bodies. Regenerating
// the library never overwrites this file (it only refreshes celfn_abi.go and
// celfn_exports.go), so your implementations are safe.
//
// What celfnc emits as the initial scaffold (signatures only, bodies //TODO):
//
//	// IsValidName implements `bool isValidName(string foo)`.
//	func IsValidName(foo string) bool {
//		// TODO: implement isValidName
//		panic("celfn: isValidName not implemented")
//	}
//
//	// IsAdult implements `bool isAdult(proto(acme.User) user)`.
//	func IsAdult(user *userpb.User) bool {
//		// TODO: implement isAdult
//		panic("celfn: isAdult not implemented")
//	}
//
// Below are the FILLED-IN versions so the probe builds + runs end-to-end.
// (The trampolines in celfn_exports.go call these; guard() turns the initial
// `panic("not implemented")` stub into a clean kError at the host, so an
// unimplemented fn fails loud-but-safe rather than trapping the instance.)

package main

import userpb "layered/userpb"

// IsValidName implements `bool isValidName(string foo)`.
func IsValidName(foo string) bool {
	return len(foo) > 0 && foo[0] >= 'A' && foo[0] <= 'Z'
}

// IsAdult implements `bool isAdult(proto(acme.User) user)`.
func IsAdult(user *userpb.User) bool {
	return user.GetAge() >= 18
}
