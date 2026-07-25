#include "eval/internal/abi_decode.h"

#include <cstdint>

#include "abi/cel_abi.pb.h"
#include "abi/wasm_binary.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "compiler/ir/annotations.h"

namespace celwasm {

Repr DecodeRepr(uint32_t wire_value) {
  const uint32_t v = wire_value;
  switch (v) {
    case static_cast<uint32_t>(Repr::kUnknown):
      return Repr::kUnknown;
    case static_cast<uint32_t>(Repr::kNull):
      return Repr::kNull;
    case static_cast<uint32_t>(Repr::kBool):
      return Repr::kBool;
    case static_cast<uint32_t>(Repr::kInt):
      return Repr::kInt;
    case static_cast<uint32_t>(Repr::kUint):
      return Repr::kUint;
    case static_cast<uint32_t>(Repr::kDouble):
      return Repr::kDouble;
    case static_cast<uint32_t>(Repr::kString):
      return Repr::kString;
    case static_cast<uint32_t>(Repr::kBytes):
      return Repr::kBytes;
    case static_cast<uint32_t>(Repr::kList):
      return Repr::kList;
    case static_cast<uint32_t>(Repr::kMap):
      return Repr::kMap;
    case static_cast<uint32_t>(Repr::kMessage):
      return Repr::kMessage;
    case static_cast<uint32_t>(Repr::kEnum):
      return Repr::kEnum;
    case static_cast<uint32_t>(Repr::kDuration):
      return Repr::kDuration;
    case static_cast<uint32_t>(Repr::kTimestamp):
      return Repr::kTimestamp;
    case static_cast<uint32_t>(Repr::kType):
      return Repr::kType;
    default:
      return Repr::kUnknown;
  }
}

// Public declaration lives in abi_decode.h; clang-tidy's include
// path for the header is incomplete in compile_commands.json and
// it mistakes this for a static candidate.
// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::StatusOr<celwasm::abi::CelAbi> DecodeCelAbiFromWasm(
    absl::Span<const uint8_t> wasm_bytes) {
  // `cel.abi` rides on the Program's CORE module.  Component-layer
  // bytes (which FindCustomSection would happily walk) are rejected
  // outright — a component reaching this decoder is a routing bug.
  if (!IsCoreModule(wasm_bytes)) {
    return absl::InvalidArgumentError(
        "abi_decode: not a core wasm module (bad or truncated preamble, "
        "unsupported version, or component-layer bytes)");
  }
  auto payload_or = FindCustomSection(wasm_bytes, "cel.abi");
  if (!payload_or.ok()) return payload_or.status();
  celwasm::abi::CelAbi abi;
  if (!abi.ParseFromArray(payload_or->data(),
                          static_cast<int>(payload_or->size()))) {
    return absl::InvalidArgumentError(
        "abi_decode: cel.abi payload failed CelAbi::ParseFromArray");
  }
  return abi;
}

}  // namespace celwasm
