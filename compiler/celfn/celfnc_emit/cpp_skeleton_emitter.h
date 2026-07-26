// celfnc_emit/cpp_skeleton_emitter — emit `user_fns.h`, the author-
// facing declaration of every `<module>::CamelFn(...)` the
// generated_stub will call.  Native C++ types throughout; the author
// fills in the bodies in `user_fns.cc`.
//
// Convention (m26 §2.1):
//   - Function name: snake-case `<fn_name>` from the IDL becomes
//     CamelCase `<CamelFn>` in C++ (`allow_user` → `AllowUser`).
//     Same translation cpp_stub_emitter uses; centralized in
//     `SnakeToCamel`.
//   - Param types: `string_view` for incoming strings (zero-copy
//     view into wasm memory); `const std::vector<T>&` / `const
//     std::map<K,V>&` for aggregates; `const acme::User&` for
//     protos (the author's generated proto class); native C types
//     for scalars.
//   - Return types: `std::string` for strings (owning, gets copied
//     out via `codec::lower`); `std::vector` / `std::map` /
//     `acme::User` for aggregates / protos; native C types for
//     scalars.
//
// The skeleton emitter is paired with an optional `--emit-skeleton`
// flag on `cel generate` that ALSO writes a stub `user_fns.cc`
// containing one TODO-bodied function per decl.  On second+
// invocations the stub is NOT overwritten — author bodies are
// sacred (m26 §11 "Out of scope" item #6).  This header only emits
// the `.h`; the optional `.cc` writer is a separate concern
// implemented at the CLI layer.

#ifndef CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_SKELETON_EMITTER_H_
#define CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_SKELETON_EMITTER_H_

#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::celfnc_emit {

// Render `user_fns.h` text.
//
//   - `lib`: only kPlugin decls drive emission.
//   - `cpp_namespace`: drives `namespace <cpp_namespace> { ... }`.
//     Empty → emit at global scope.
//   - `extra_includes`: emitted before the standard library
//     headers; same channel cel_wasm_plugin uses to forward
//     proto-header paths from the macro caller.
//
// Errors: `FailedPrecondition` if a permanently-rejected
// CelType::Kind (`optional<T>` / `type`) reaches the emitter.
ABSL_MUST_USE_RESULT absl::StatusOr<std::string> EmitUserFnsH(
    const FunctionLibrary& lib, absl::string_view cpp_namespace,
    const std::vector<std::string>& extra_includes);

}  // namespace celwasm::celfnc_emit

#endif  // CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_SKELETON_EMITTER_H_
