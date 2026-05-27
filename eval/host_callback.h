// `celwasm::HostCallback` — the callback type for `Engine::AddFunction`
// (the `@host.<name>` impl).  Pulled into its own header so the
// internal `WasmtimeEngineState` (which holds registered callbacks) and
// the public `engine.h` (which exposes the type) can both depend on it
// without an engine↔state circular include.
//
// The callback receives a typed `HostCallContext&` (eval/host_call_context.h)
// built by the engine trampoline over the per-Eval linear memory,
// externref table, and arena allocator.  See `engine.h` /
// `HostCallContext` for the full contract.

#ifndef CELWASM_EVAL_HOST_CALLBACK_H_
#define CELWASM_EVAL_HOST_CALLBACK_H_

#include <functional>

#include "absl/status/status.h"

namespace celwasm {

// Full definition in eval/host_call_context.h; forward-declared here so
// this header stays light (the type only appears behind a reference).
class HostCallContext;

using HostCallback = std::function<absl::Status(HostCallContext&)>;

}  // namespace celwasm

#endif  // CELWASM_EVAL_HOST_CALLBACK_H_
