#include "abi/wasm_binary.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

namespace {

// Preamble: 4-byte `\0asm` magic + 4-byte little-endian word.
constexpr uint8_t kWasmMagic[4] = {0x00, 0x61, 0x73, 0x6d};
constexpr size_t kPreambleSize = 8;
// Core-module version word.
constexpr uint32_t kCoreVersionWord = 0x00000001;

// Custom sections have section id 0.
constexpr uint8_t kCustomSectionId = 0;

// Upper bound on unsigned LEB128 for a u32 (5 x 7 bits).
constexpr size_t kMaxU32LebBytes = 5;

// Reads the section-size / name-length LEB at `*pos`, mapping a
// truncated or overlong encoding to InvalidArgument.
absl::StatusOr<uint32_t> ReadFramingLeb(absl::Span<const uint8_t> bytes,
                                        size_t* pos,
                                        absl::string_view what) {
  uint32_t value = 0;
  if (!ReadLeb128U32(bytes, pos, &value)) {
    return absl::InvalidArgumentError(
        absl::StrCat("wasm_binary: truncated or overlong LEB128 ", what));
  }
  return value;
}

}  // namespace

bool IsCoreModule(absl::Span<const uint8_t> bytes) {
  if (bytes.size() < kPreambleSize) return false;
  if (std::memcmp(bytes.data(), kWasmMagic, sizeof(kWasmMagic)) != 0) {
    return false;
  }
  uint32_t word = 0;
  std::memcpy(&word, bytes.data() + sizeof(kWasmMagic), sizeof(word));
  // Open set by design: any other version word — including a
  // Component-Model component's 0x0001000d — is "not a core module",
  // not an invariant violation.
  return word == kCoreVersionWord;
}

bool ReadLeb128U32(absl::Span<const uint8_t> bytes, size_t* pos,
                   uint32_t* out) {
  *out = 0;
  uint32_t shift = 0;
  for (size_t i = 0; i < kMaxU32LebBytes; ++i) {
    if (*pos >= bytes.size()) return false;
    const uint8_t b = bytes[*pos];
    ++*pos;
    *out |= static_cast<uint32_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

void AppendLeb128U32(std::vector<uint8_t>& out, uint32_t value) {
  while (true) {
    const uint8_t b = value & 0x7f;
    value >>= 7;
    if (value == 0) {
      out.push_back(b);
      return;
    }
    out.push_back(b | 0x80u);
  }
}

namespace {

// Reads the custom-section name at `*pos` (with the section ending at
// `section_end`), advancing past it.  InvalidArgument when the name
// length overruns the section.
absl::StatusOr<absl::string_view> ReadSectionName(
    absl::Span<const uint8_t> bytes, size_t* pos, size_t section_end) {
  auto name_len_or = ReadFramingLeb(bytes, pos, "custom-section name length");
  if (!name_len_or.ok()) return name_len_or.status();
  if (static_cast<uint64_t>(*pos) + *name_len_or > section_end) {
    return absl::InvalidArgumentError(
        "wasm_binary: custom section name length runs past section end");
  }
  const absl::string_view name(
      reinterpret_cast<const char*>(bytes.data() + *pos), *name_len_or);
  *pos += *name_len_or;
  return name;
}

}  // namespace

absl::StatusOr<absl::Span<const uint8_t>> FindCustomSection(
    absl::Span<const uint8_t> wasm_bytes, absl::string_view name) {
  if (!IsCoreModule(wasm_bytes)) {
    return absl::InvalidArgumentError(
        "wasm_binary: bytes are not a core wasm module (bad or truncated "
        "preamble)");
  }
  size_t pos = kPreambleSize;
  std::optional<absl::Span<const uint8_t>> found;
  while (pos < wasm_bytes.size()) {
    const uint8_t section_id = wasm_bytes[pos];
    ++pos;
    auto size_or = ReadFramingLeb(wasm_bytes, &pos, "section size");
    if (!size_or.ok()) return size_or.status();
    if (static_cast<uint64_t>(pos) + *size_or > wasm_bytes.size()) {
      return absl::InvalidArgumentError(
          "wasm_binary: section size runs past end of binary");
    }
    const size_t section_end = pos + *size_or;
    // Non-custom sections are skipped opaquely, never recursed into:
    // this walker is top-level only by contract.
    if (section_id == kCustomSectionId) {
      auto name_or = ReadSectionName(wasm_bytes, &pos, section_end);
      if (!name_or.ok()) return name_or.status();
      if (*name_or == name) {
        if (found.has_value()) {
          return absl::InvalidArgumentError(absl::StrCat(
              "wasm_binary: duplicate top-level custom section `", name, "`"));
        }
        found = wasm_bytes.subspan(pos, section_end - pos);
      }
    }
    pos = section_end;
  }
  if (found.has_value()) return *found;
  return absl::NotFoundError(absl::StrCat(
      "wasm_binary: custom section `", name, "` not found at top level"));
}

absl::StatusOr<std::vector<uint8_t>> AppendCustomSection(
    absl::Span<const uint8_t> wasm_bytes, absl::string_view name,
    absl::Span<const uint8_t> payload) {
  // FindCustomSection re-validates the preamble and the full section
  // walk — a binary we can't walk is not one we may append to.
  auto existing = FindCustomSection(wasm_bytes, name);
  if (existing.ok()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "wasm_binary: custom section `", name, "` already present"));
  }
  if (!absl::IsNotFound(existing.status())) return existing.status();
  std::vector<uint8_t> out(wasm_bytes.begin(), wasm_bytes.end());
  const std::vector<uint8_t> section = BuildCustomSection(name, payload);
  out.insert(out.end(), section.begin(), section.end());
  return out;
}

std::vector<uint8_t> BuildCustomSection(absl::string_view name,
                                        absl::Span<const uint8_t> payload) {
  std::vector<uint8_t> body;
  AppendLeb128U32(body, static_cast<uint32_t>(name.size()));
  body.insert(body.end(), name.begin(), name.end());
  body.insert(body.end(), payload.begin(), payload.end());
  std::vector<uint8_t> out;
  out.push_back(kCustomSectionId);
  AppendLeb128U32(out, static_cast<uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace celwasm
