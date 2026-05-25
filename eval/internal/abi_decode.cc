#include "eval/internal/abi_decode.h"

#include <cstdint>
#include <cstring>

#include "abi/cel_abi.pb.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/ir/annotations.h"

namespace celwasm {

namespace {

// Wasm module header: 4-byte magic + 4-byte version.
constexpr uint8_t kWasmMagic[4] = {0x00, 0x61, 0x73, 0x6d};
constexpr uint32_t kWasmVersion = 1;

// Custom sections have section_id = 0.
constexpr uint8_t kCustomSectionId = 0;

// Upper bound on unsigned LEB128 for a u32 (5 × 7 bits).
constexpr size_t kMaxU32LebBytes = 5;

absl::StatusOr<uint32_t> DecodeLeb128U32(absl::Span<const uint8_t> bytes,
                                         size_t* pos) {
  uint32_t result = 0;
  uint32_t shift = 0;
  for (size_t i = 0; i < kMaxU32LebBytes; ++i) {
    if (*pos >= bytes.size()) {
      return absl::InvalidArgumentError("abi_decode: truncated LEB128 u32");
    }
    const uint8_t b = bytes[*pos];
    ++*pos;
    result |= static_cast<uint32_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) return result;
    shift += 7;
  }
  return absl::InvalidArgumentError(
      "abi_decode: LEB128 u32 exceeds five bytes");
}

absl::Status CheckWasmHeader(absl::Span<const uint8_t> bytes, size_t* pos) {
  if (bytes.size() < 8) {
    return absl::InvalidArgumentError(
        "abi_decode: wasm byte stream shorter than 8-byte header");
  }
  if (std::memcmp(bytes.data(), kWasmMagic, 4) != 0) {
    return absl::InvalidArgumentError(
        "abi_decode: wasm magic bytes do not match \\0asm");
  }
  uint32_t version = 0;
  std::memcpy(&version, bytes.data() + 4, sizeof(version));
  if (version != kWasmVersion) {
    return absl::InvalidArgumentError(absl::StrCat(
        "abi_decode: unsupported wasm version ", version, " (expected 1)"));
  }
  *pos = 8;
  return absl::OkStatus();
}

absl::StatusOr<absl::Span<const uint8_t>> FindCustomSection(
    absl::Span<const uint8_t> bytes, absl::string_view target_name) {
  size_t pos = 0;
  if (auto s = CheckWasmHeader(bytes, &pos); !s.ok()) return s;

  while (pos < bytes.size()) {
    const uint8_t section_id = bytes[pos];
    ++pos;
    auto size_or = DecodeLeb128U32(bytes, &pos);
    if (!size_or.ok()) return size_or.status();
    const uint32_t section_size = *size_or;
    if (static_cast<std::uint64_t>(pos) + section_size > bytes.size()) {
      return absl::InvalidArgumentError(
          "abi_decode: section size runs past end of module");
    }
    const size_t section_end = pos + section_size;

    if (section_id != kCustomSectionId) {
      pos = section_end;
      continue;
    }

    auto name_len_or = DecodeLeb128U32(bytes, &pos);
    if (!name_len_or.ok()) return name_len_or.status();
    const uint32_t name_len = *name_len_or;
    if (static_cast<std::uint64_t>(pos) + name_len > section_end) {
      return absl::InvalidArgumentError(
          "abi_decode: custom section name length runs past section end");
    }
    const absl::string_view name(
        reinterpret_cast<const char*>(bytes.data() + pos), name_len);
    pos += name_len;

    if (name == target_name) {
      return bytes.subspan(pos, section_end - pos);
    }
    pos = section_end;
  }
  return absl::NotFoundError(absl::StrCat("abi_decode: custom section `",
                                          target_name,
                                          "` not found in wasm byte stream"));
}

}  // namespace

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
