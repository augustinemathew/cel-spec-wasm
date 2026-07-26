// celfnc_emit/cpp_stub_emitter — emit the C++ `generated_stub.cc`,
// the implementation file that fulfils each wit-bindgen-emitted
// export declaration by routing through codec.h into the author's
// `user::FnName(...)` impl.
//
// Mechanical: one function body per kPlugin decl.  Body
// shape per export, by return-type category:
//
//   - Scalar return (bool / int / uint / double):
//       int64_t exports_<pkg>_<iface>_<id>(int64_t a, int64_t b) {
//         return <module>::<CamelFn>(a, b);
//       }
//
//   - String / bytes / list / map / record / proto return:
//       void exports_<pkg>_<iface>_<id>(
//           <customfn_T>* in1, <customfn_T>* in2,
//           <customfn_R>* ret) {
//         <module>::codec::lower(
//             ret, <module>::<CamelFn>(<module>::codec::lift(*in1),
//                                      <module>::codec::lift(*in2)));
//       }
//
// Argument-side lift: scalars pass by value; everything else
// passes by `<customfn_T>*` and gets `codec::lift(*ptr)`'d at the
// call site.  Proto args call `codec::lift_proto<acme::User>(*ptr)`.
//
// File-level: emits `#include` directives for `customfn.h`,
// `codec.h`, `user_fns.h`, and (when any decl is proto-typed) the
// caller-supplied proto headers via a `--proto-deps` flag at
// `cel generate` time.

#ifndef CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_STUB_EMITTER_H_
#define CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_STUB_EMITTER_H_

#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::celfnc_emit {

// Snake-case → CamelCase conversion for user-facing C++ function
// names (m26 §2.1 author convention: `allow_user` → `AllowUser`).
// Public for test access and reuse by the skeleton emitter.
std::string SnakeToCamel(absl::string_view snake);

// Render `generated_stub.cc` text.
//
//   - `lib`: only kPlugin decls drive emission.  Other
//     backends have no WIT export surface (m24 §A).
//   - `cpp_namespace`: the `<module>` namespace
//     (`namespace <cpp_namespace> { ... }`).  Empty → global.
//   - `wit_package_name`: drives the `exports_<pkg_normalized>_fns_`
//     prefix on every export, same rule as codec.h
//     (m26 §3.5.1; `cel:customfn` → `cel_customfn`).
//   - `extra_includes`: passed to `cel generate` as `--include` flags;
//     emit each as `#include "<inc>"` between the standard headers
//     and the codec/author/user headers.  Used to pull in
//     generated proto headers when any decl is proto-typed.
//
// Errors: `FailedPrecondition` if a permanently-rejected
// CelType::Kind (`optional<T>` / `type`) reaches the emitter.
ABSL_MUST_USE_RESULT absl::StatusOr<std::string> EmitStubCc(
    const FunctionLibrary& lib, absl::string_view cpp_namespace,
    absl::string_view wit_package_name,
    const std::vector<std::string>& extra_includes);

}  // namespace celwasm::celfnc_emit

#endif  // CELWASM_COMPILER_CELFN_CELFNC_EMIT_CPP_STUB_EMITTER_H_
