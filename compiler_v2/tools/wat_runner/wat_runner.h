// wat_runner — execute a hand-written WAT expression module through
// wasmtime + cel_runtime.wasm, with optional stubbed cel_host imports
// and pre-populated memory bytes.
//
// Purpose: prototype new codegen arms and host-ABI shapes at the WAT
// level before writing any expr_lower C++.  Per CLAUDE.md's "WAT-first"
// rule: every codegen arm starts with a WAT in
// `doc/implementation-plan/rewrite/wat/`, validates via `wasm-as`, and
// runs end-to-end through this harness with stub impls for any not-
// yet-implemented host functions.  Only after the WAT works end-to-end
// do we implement the codegen arm — and its output must match the WAT
// byte-for-byte (modulo Binaryen-assigned names).
//
// What this does:
//
//   1. Assembles a WAT string to wasm bytes via `wasmtime_wat2wasm`.
//   2. Initialises a per-run wasmtime store + 2-page memory, matching
//      `cel::Engine::Plan`'s M1 wiring.
//   3. Instantiates `cel_runtime.wasm`, binds its `cel_reset` /
//      `cel_alloc` exports onto the linker.
//   4. Optionally registers `cel_host.cel_get_field` /
//      `cel_host.cel_has_field` as stubs — each stub is a
//      caller-supplied std::function that writes a CelValue to the
//      out_slot.
//   5. Applies caller-supplied memory pre-writes (simulates the
//      host-side Activation marshal writing variable CelValues into
//      their workspace slots before Eval).
//   6. Runs `$eval`, collects the returned i32 offset, snapshots
//      memory, returns both.
//
// What this is NOT:
//
//   - Production code.  Production instantiation flows through
//     `cel::Engine::Plan` + per-Instance state.  This harness shares
//     no code with Engine::Plan on purpose — if the prototype here
//     depended on the shape of Engine::Plan, the harness couldn't be
//     used to design changes to that shape.
//   - A test runner.  Tests use this harness but the harness itself
//     ships no assertions.

#ifndef CELWASM_COMPILER_V2_TOOLS_WAT_RUNNER_WAT_RUNNER_H_
#define CELWASM_COMPILER_V2_TOOLS_WAT_RUNNER_WAT_RUNNER_H_

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace celwasm {

// One stub for a cel_host.* import.  Signature matches the M2.C
// wire ABI: `(out_slot, msg_slot, field_ref_id, attribute_id)`.  The
// stub writes a CelValue to `memory[out_slot..out_slot+24)` — in
// production this is what ProtoBacking::ReadField + Layer-2
// trampoline does.  The stub gets full access to the linear-memory
// byte buffer so it can also read `msg_slot`'s CelValue to simulate
// more realistic backings.
using CelHostStub = std::function<void(uint32_t out_slot,
                                       uint32_t msg_slot,
                                       uint32_t field_ref_id,
                                       uint32_t attribute_id,
                                       uint8_t* memory, size_t mem_size)>;

struct WatRunInput {
  absl::string_view wat;

  // Memory pre-writes applied AFTER instantiation but BEFORE the
  // `$eval` call.  Simulates the host-side Activation marshal writing
  // bound variable CelValues into their workspace slots.  Each entry
  // writes its bytes starting at the given absolute linear-memory
  // offset.  Overlapping entries are applied in order; later writes
  // win.
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pre_writes;

  // Optional stub for `cel_host.cel_get_field`.  When the WAT imports
  // this function and no stub is supplied, instantiation fails.
  CelHostStub cel_get_field_stub;

  // Optional stub for `cel_host.cel_has_field`.  Same contract.
  CelHostStub cel_has_field_stub;
};

struct WatRunOutput {
  // Whatever `$eval` returned — an i32 in our ABI, conventionally the
  // offset of a 24-byte CelValue in linear memory.
  uint32_t eval_return = 0;

  // Snapshot of linear memory AFTER `$eval` returns.  Size = 2 pages
  // (128 KiB).  Callers decode the CelValue at `eval_return` out of
  // this buffer rather than out of the wasmtime-owned memory, so
  // there's no lifetime coupling with the store.
  std::vector<uint8_t> memory_after;
};

// Runs `input.wat` end-to-end.  Returns InvalidArgument on WAT
// assembly errors, FailedPrecondition on instantiation errors,
// Internal on wasmtime traps.
ABSL_MUST_USE_RESULT absl::StatusOr<WatRunOutput> RunWat(
    const WatRunInput& input);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_TOOLS_WAT_RUNNER_WAT_RUNNER_H_
