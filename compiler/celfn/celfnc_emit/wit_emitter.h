// celfnc_emit/wit_emitter — translate a parsed celfn IDL
// (CelfnDecl set with kForeignComponent backend) into the text of a
// `fns.wit` Component-Model interface file consumable by
// `wit-bindgen c --world customfn`.
//
// Scope:
//   - Every CEL type the foreign-component decl surface admits, per
//     m24 §6 — bool / int / uint / double / null / string / bytes /
//     duration / timestamp / list<T> / map<K,V> / proto(fqn).
//   - Arbitrary nesting via concrete expansion (m24 §6).
//   - The two permanently-rejected shapes — `optional<T>` and `type`
//     — never reach this layer because `FunctionLibrary::Builder::Build()`
//     refuses them upstream (m24 §A.4 + cleanup in §14).  An assertion
//     guards against a future regression that lets one slip through.
//
// Identifier rules:
//   - Function names in WIT are kebab-case; the celfn IDL synthesises
//     snake_case overload-ids (`add_int_int`).  We translate at emit
//     time, the same translation `Engine::AddComponent` does at the
//     component-export-lookup site (snake_case ↔ kebab-case is the
//     stable translation for the entire pipeline).
//   - Record types (`duration`, `timestamp`) declare INSIDE the
//     interface — wit-bindgen 0.57 rejects top-level `record` decls
//     ("expected `world`, `interface` or `use`, found keyword
//     `record`"), verified empirically against
//     wit-bindgen-0.57.1-aarch64-macos against an early draft of
//     `fns.wit`.
//
// Output stability: byte-for-byte deterministic given the same input
// decl set; ordering follows decl-insertion order to match how the
// downstream `wit-bindgen c` output sorts (insertion-order is also
// what `function_library_test.cc::SynthesisesOverloadIdsForAllTypes`
// asserts).

#ifndef CELWASM_COMPILER_CELFN_CELFNC_EMIT_WIT_EMITTER_H_
#define CELWASM_COMPILER_CELFN_CELFNC_EMIT_WIT_EMITTER_H_

#include <string>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::celfnc_emit {

// Render a `fns.wit` file as a string.  Inputs:
//   - `lib`: every kForeignComponent decl in the library is emitted as
//     a typed function inside `interface fns`.  Non-foreign-component
//     decls are ignored (they have no WIT surface — they dispatch
//     differently).
//   - `package_name`: the WIT package identifier (e.g. "cel:customfn").
//   - `package_version`: the WIT package version (e.g. "0.1.0"); empty
//     suppresses the `@version` suffix.
//
// Returns the rendered text.  Errors:
//   - `FailedPrecondition` if a kForeignComponent decl somehow carries
//     `optional<T>` or `type` (the Builder gates should have rejected
//     it; this is the regression tripwire).
//   - `Internal` on any closed-set switch falling through (e.g. a new
//     `CelfnType::Kind` was added without updating this emitter).
ABSL_MUST_USE_RESULT absl::StatusOr<std::string> EmitWit(
    const FunctionLibrary& lib, absl::string_view package_name,
    absl::string_view package_version);

// snake_case ↔ kebab-case translation for WIT function names.  Public
// for test access and for reuse by the codec / stub emitters (which
// also need the kebab form for the canonical export-name attribute).
std::string SnakeToKebab(absl::string_view snake);

}  // namespace celwasm::celfnc_emit

#endif  // CELWASM_COMPILER_CELFN_CELFNC_EMIT_WIT_EMITTER_H_
