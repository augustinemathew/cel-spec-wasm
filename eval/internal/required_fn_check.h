// Plan-time required-function verification.
//
// `Engine::Plan` calls `CheckRequiredFunctions` after decoding the
// Program's `cel.abi` section (and after the `runtime_abi_version`
// check), BEFORE any registered extension binds: every
// `required_functions[]` row must be satisfiable by the engine's
// registration-time-frozen state, or Plan fails with a
// FailedPrecondition whose message shapes are frozen in
// doc/implementation-plan/rewrite/m35-plugin-ergonomics.md §5.3.
//
// Per row, first failure wins (wire order, deterministic):
//
//   - HOST rows resolve against `host_callbacks[overload_id]`.
//     Missing → fail; wasm arity (`param_types.size() + 1 !=
//     num_args`) → fail; full recursive type compare only when the
//     registration captured a decl (`RegisteredHostCallback::
//     decl_signature`, populated by `Engine::BindFunction`) — raw
//     `AddFunction` / `AddTypedFunction` registrations are
//     arity-only.
//   - Rows carrying the retired wasm-component plugin backend (wire
//     value 2, the former `RequiredFunction.backend` PLUGIN entry)
//     are rejected outright — this engine has no plugin backend, so
//     such a Program can never run and must fail loudly here, not
//     as an opaque wasmtime link error.
//   - Rows with an unknown/unspecified backend are skipped —
//     open-set wire data is never rejected here; an unbound import
//     still fails loudly at wasmtime link time.
//
// A Program with an empty `required_functions` list (no custom-fn
// call sites, or emitted before the field existed) no-ops entirely.
//
// Thread-safety: reads registration-frozen state only (the
// registration family is startup-only; `Plan` is concurrent-safe).

#ifndef CELWASM_EVAL_INTERNAL_REQUIRED_FN_CHECK_H_
#define CELWASM_EVAL_INTERNAL_REQUIRED_FN_CHECK_H_

#include <map>
#include <string>

#include "abi/cel_abi.pb.h"
#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "eval/internal/wasmtime_engine_state.h"

namespace celwasm {

// Verify every `abi.required_functions()` row against the engine's
// registered host callbacks.  OK when every row is satisfied (or the
// list is empty); FailedPrecondition on the first unsatisfied row, in
// wire order, with the frozen message shapes described above.
ABSL_MUST_USE_RESULT absl::Status CheckRequiredFunctions(
    const celwasm::abi::CelAbi& abi,
    const std::map<std::string, RegisteredHostCallback>& host_callbacks);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_REQUIRED_FN_CHECK_H_
