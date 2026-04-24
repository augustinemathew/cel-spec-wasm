// Load-time decoder for the `cel.abi` custom section.
//
// Compile-side counterpart: `compiler_v2/abi/cel_abi_emit.{h,cc}`
// writes the section into the emitted wasm; this library reads it
// back at Engine::Plan time.  Output is an in-memory
// `VariableTable` the host marshal (Instance::Eval(Activation))
// indexes by name when encoding a bound Value into its workspace
// slot.
//
// Why we own this rather than reusing binaryen: binaryen parses
// wasm into its IR but discards the byte form of custom sections
// unless you ask.  We take the simpler path of parsing the raw
// wasm byte stream ourselves — the custom-section framing is
// trivial (§Appendix: wasm binary format) and stays reachable
// without a Binaryen-specific API.

#ifndef CELWASM_COMPILER_V2_API_INTERNAL_ABI_DECODE_H_
#define CELWASM_COMPILER_V2_API_INTERNAL_ABI_DECODE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/ir/annotations.h"  // for Repr

namespace celwasm {

// One resolved variable after decode.  Mirrors
// `celwasm.abi.VariableEntry` with the repr field promoted to its
// typed enum.
struct DecodedVariable {
  std::string name;
  uint32_t local_index = 0;
  uint32_t slot_offset = 0;
  Repr repr = Repr::kUnknown;
};

// Decoded `cel.abi` payload — the in-memory shape `Engine::Plan`
// stores on the Instance.  Future milestones (M2.C / M2.E) populate
// `fields` + `attributes`; the struct is shipped with them now so
// the Engine::Plan → Instance wiring is additive when those land.
struct DecodedCelAbi {
  uint32_t version = 0;

  // Dense by local_index.  `variables[i].local_index == i` always
  // (the emitter emits in local_index order).
  std::vector<DecodedVariable> variables{};

  // Name-indexed view into `variables` for host-side marshal
  // lookup.  String keys pair with `variables[i].name` — so the
  // map's lifetime is coupled to the struct.  Users must not
  // modify `variables` after building the map.
  absl::flat_hash_map<std::string, const DecodedVariable*> by_name{};

  // Rebuild by_name from variables.  Called by decoders; exposed
  // for tests.
  void RebuildNameIndex();
};

// Decode the `cel.abi` custom section from a wasm module's raw
// bytes.  Returns:
//   - OK: decoded payload.
//   - InvalidArgument: malformed wasm header, malformed custom
//     section framing, or the section's payload fails to parse as
//     `celwasm.abi.CelAbi`.
//   - NotFound: the module has no `cel.abi` custom section.  M1
//     modules (and synthetic-wasm engine_test fixtures) take this
//     path; callers of Engine::Plan at M2+ should fall back to an
//     empty DecodedCelAbi when they see NotFound (no variables to
//     marshal).
ABSL_MUST_USE_RESULT absl::StatusOr<DecodedCelAbi> DecodeCelAbiFromWasm(
    absl::Span<const uint8_t> wasm_bytes);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_ABI_DECODE_H_
