#ifndef CELWASM_ABI_CELFN_WIRE_H_
#define CELWASM_ABI_CELFN_WIRE_H_

// The `cel.abi` wire type vocabulary (`abi.Type` — THE wire
// spelling of a CEL type) and its mapping to/from the celfn
// signature vocabulary.  `Type` is general — `RequiredFunction`
// carries it today, and future surfaces (e.g. variable
// introspection via `VariableEntry`'s reserved slot 5) adopt the
// same message rather than minting consumer-specific spellings.
//
// The pieces, shared by the compile-side emitter
// (`BuildRequiredFunctions` in abi/cel_abi_emit.cc) and the
// eval-side Plan verification (which compares a decoded
// `RequiredFunction` row against a registered `CelfnDecl`):
//
//   - `TypeFromCelfn`   — `CelfnType` → wire `Type`, recursive.
//   - `TypeEquals`      — recursive structural equality over wire
//     `Type`s; proto-FQN-sensitive; unknown kinds compare
//     numerically (open-set wire data is never rejected here).
//   - `RenderType`      — one wire `Type` → its `.celfn` grammar
//     spelling.  THE type renderer for error messages: compose with
//     `TypeFromCelfn` to render a `CelfnType` diagnostic so the
//     spelling can never drift from the grammar.
//   - `RenderSignature`   — a `RequiredFunction` row → the `.celfn`
//     source spelling, e.g. `bool is_adult(proto(acme.User))`.
//     THE renderer for signature strings in error messages: emit
//     tests and Engine::Plan diagnostics both call it, so a
//     signature always reads identically everywhere.
//   - `RequiredFunctionFromDecl` — a whole `CelfnDecl` → its wire
//     `RequiredFunction` row.  Shared by the emitter's per-import
//     row build and the eval side's registered-decl spelling (so
//     both sides of a Plan-time compare are built by one function).
//
// See doc/implementation-plan/rewrite/m35-plugin-ergonomics.md §5.

#include <string>

#include "abi/cel_abi.pb.h"
#include "compiler/celfn/function_library.h"
#include "shared/type.h"

namespace celwasm {

// Translate a `CelType` (the one C++ type vocabulary, shared/type.h)
// into its wire `Type` spelling.  Recursive: `list<T>` / `map<K, V>`
// / `optional<T>` element types land in `params` (per the Type.Kind
// comments).  `kMessage`'s fully-qualified name lands in `proto_fqn`.
//
// Total over every representable `CelType::Kind`; `kUnknown` (the
// default-constructed sentinel) is a builder-invariant violation and
// CHECK-fails.
celwasm::abi::Type TypeFromCelType(const CelType& type);

// Translate a compile-time `CelfnType` into its wire `Type`
// spelling.  Recursive: `list<T>` / `map<K, V>` / `optional<T>`
// element types land in `params` (per the Type.Kind comments).
// `proto(...)`'s fully-qualified name lands in `proto_fqn`.
//
// Total over every `CelfnType::Kind`; a malformed input shape (a
// kList with no element type, a kMap without exactly [key, value])
// is a builder-invariant violation and CHECK-fails.
celwasm::abi::Type TypeFromCelfn(const CelfnType& type);

// Recursive structural equality over wire `Type`s.  Two types are
// equal iff their kinds match (numerically — unknown future kinds
// are compared by value, never rejected), their `proto_fqn`s match
// byte-for-byte, and their `params` match pairwise (same count,
// each recursively equal).
bool TypeEquals(const celwasm::abi::Type& a, const celwasm::abi::Type& b);

// Render one wire `Type` in its `.celfn` grammar spelling
// (`Duration`, `Timestamp`, `map<K, V>`, `list<T>`, `optional<T>`,
// `proto(<fqn>)`).  An unknown wire kind renders as `<kind N>` —
// open-set wire data is never rejected.  A composite with missing
// element types renders the gap as `?`.
std::string RenderType(const celwasm::abi::Type& type);

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

// Translate a whole `CelfnDecl` into its wire `RequiredFunction`
// row: overload_id, fn_name, backend (kHost → HOST, kPlugin →
// PLUGIN), per-param `Type`s (out_slot excluded), return type,
// is_receiver.  A `kCelDefined` decl has no `cel_fn` wire backend
// (its imports are per-module aliases) — passing one is a caller
// invariant violation and CHECK-fails.
celwasm::abi::RequiredFunction RequiredFunctionFromDecl(const CelfnDecl& decl);

}  // namespace celwasm

#endif  // CELWASM_ABI_CELFN_WIRE_H_
