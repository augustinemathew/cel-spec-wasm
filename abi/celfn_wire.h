#ifndef CELWASM_ABI_CELFN_WIRE_H_
#define CELWASM_ABI_CELFN_WIRE_H_

// celfn signature vocabulary ↔ `cel.abi` wire mapping.
//
// Three pieces, shared by the compile-side emitter
// (`BuildRequiredFunctions` in abi/cel_abi_emit.cc) and the
// eval-side Plan verification (which compares a decoded
// `RequiredFunction` row against a registered `CelfnDecl`):
//
//   - `FnTypeFromCelfn`   — `CelfnType` → wire `FnType`, recursive.
//   - `FnTypeEquals`      — recursive structural equality over wire
//     `FnType`s; proto-FQN-sensitive; unknown kinds compare
//     numerically (open-set wire data is never rejected here).
//   - `RenderSignature`   — a `RequiredFunction` row → the `.celfn`
//     source spelling, e.g. `bool is_adult(proto(acme.User))`.
//     THE renderer for signature strings in error messages: emit
//     tests and Engine::Plan diagnostics both call it, so a
//     signature always reads identically everywhere.
//
// See doc/implementation-plan/rewrite/m35-plugin-ergonomics.md §5.

#include <string>

#include "abi/cel_abi.pb.h"
#include "compiler/celfn/function_library.h"

namespace celwasm {

// Translate a compile-time `CelfnType` into its wire `FnType`
// spelling.  Recursive: `list<T>` / `map<K, V>` / `optional<T>`
// element types land in `params` (per the FnType.Kind comments).
// `proto(...)`'s fully-qualified name lands in `proto_fqn`.
//
// Total over every `CelfnType::Kind`; a malformed input shape (a
// kList with no element type, a kMap without exactly [key, value])
// is a builder-invariant violation and CHECK-fails.
celwasm::abi::FnType FnTypeFromCelfn(const CelfnType& type);

// Recursive structural equality over wire `FnType`s.  Two types are
// equal iff their kinds match (numerically — unknown future kinds
// are compared by value, never rejected), their `proto_fqn`s match
// byte-for-byte, and their `params` match pairwise (same count,
// each recursively equal).
bool FnTypeEquals(const celwasm::abi::FnType& a, const celwasm::abi::FnType& b);

// Render a `RequiredFunction` row as its `.celfn` source spelling:
//
//   `bool is_adult(proto(acme.User))`
//   `string upper(this string)`            (is_receiver)
//   `int invocation_id()`                  (no params)
//   `list<int> f(map<string, double>)`     (nested generics)
//
// Types use the `.celfn` grammar spellings (`Duration`,
// `Timestamp`, `proto(<fqn>)`, `map<K, V>`, `optional<T>`).  An
// unknown wire kind renders as `<kind N>` — messages must not
// reject open-set wire data.
std::string RenderSignature(const celwasm::abi::RequiredFunction& fn);

}  // namespace celwasm

#endif  // CELWASM_ABI_CELFN_WIRE_H_
