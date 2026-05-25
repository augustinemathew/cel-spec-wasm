// `celwasm::HostCallback` — the raw low-level callback type for
// `Engine::AddFunction` (the `@host.<name>` impl).  Pulled into its
// own header so the internal `WasmtimeEngineState` (which needs to
// hold registered callbacks) doesn't pick up `engine.h`'s public
// surface — avoids a circular include between `engine.h` and
// `internal/wasmtime_engine_state.h`.
//
// See `engine.h` for the full contract; this header carries only
// the type alias.

#ifndef CELWASM_EVAL_HOST_CALLBACK_H_
#define CELWASM_EVAL_HOST_CALLBACK_H_

#include <cstdint>
#include <functional>

#include "absl/status/status.h"
#include "absl/types/span.h"

namespace celwasm {

using HostCallback = std::function<absl::Status(
    uint8_t* memory, size_t mem_size, uint32_t out_slot,
    absl::Span<const uint32_t> arg_slots)>;

}  // namespace celwasm

#endif  // CELWASM_EVAL_HOST_CALLBACK_H_
