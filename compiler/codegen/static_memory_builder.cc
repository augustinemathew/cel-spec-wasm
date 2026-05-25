#include "compiler/codegen/static_memory_builder.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "runtime/cel_runtime.h"

namespace celwasm {

namespace {

// CelValue is 24 bytes, 8-byte aligned.  The runtime struct
// (runtime/cel_runtime.h) has a `_Static_assert
// sizeof(CelValue) == 24` guarding the frame size; tests pin the
// byte layout within the frame.
constexpr size_t kCelValueSize = 24;
constexpr uint32_t kAlign = 8;

void AppendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
  }
}

void AppendU64LE(std::vector<uint8_t>& buf, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
  }
}

void AppendZeros(std::vector<uint8_t>& buf, size_t n) {
  buf.insert(buf.end(), n, static_cast<uint8_t>(0));
}

void PadTo(std::vector<uint8_t>& buf, uint32_t align) {
  const size_t misalign = buf.size() % align;
  if (misalign != 0) {
    AppendZeros(buf, align - misalign);
  }
}

}  // namespace

StaticMemoryBuilder::StaticMemoryBuilder(uint32_t base_offset)
    : base_offset_(base_offset) {
  ABSL_CHECK_EQ(base_offset_ % kAlign, 0u)
      << "base_offset must be 8-byte aligned, got " << base_offset_;
}

uint32_t StaticMemoryBuilder::OpenFrame(CelKind kind) {
  ABSL_CHECK_EQ(buf_.size() % kAlign, 0u)
      << "StaticMemoryBuilder cursor is not 8-byte aligned";
  const auto local = static_cast<uint32_t>(buf_.size());
  AppendU32LE(buf_, static_cast<uint32_t>(kind));
  AppendU32LE(buf_, 0u);  // _pad
  return local;
}

uint32_t StaticMemoryBuilder::AllocateNull() {
  const uint32_t local = OpenFrame(CEL_NULL);
  AppendZeros(buf_, 16);
  ABSL_CHECK_EQ(buf_.size() - local, kCelValueSize);
  return base_offset_ + local;
}

uint32_t StaticMemoryBuilder::AllocateBool(bool v) {
  const uint32_t local = OpenFrame(CEL_BOOL);
  AppendU32LE(buf_, v ? 1u : 0u);
  AppendZeros(buf_, 12);
  ABSL_CHECK_EQ(buf_.size() - local, kCelValueSize);
  return base_offset_ + local;
}

uint32_t StaticMemoryBuilder::AllocateInt(int64_t v) {
  const uint32_t local = OpenFrame(CEL_INT);
  AppendU64LE(buf_, static_cast<uint64_t>(v));
  AppendZeros(buf_, 8);
  ABSL_CHECK_EQ(buf_.size() - local, kCelValueSize);
  return base_offset_ + local;
}

uint32_t StaticMemoryBuilder::AllocateUint(uint64_t v) {
  const uint32_t local = OpenFrame(CEL_UINT);
  AppendU64LE(buf_, v);
  AppendZeros(buf_, 8);
  ABSL_CHECK_EQ(buf_.size() - local, kCelValueSize);
  return base_offset_ + local;
}

uint32_t StaticMemoryBuilder::AllocateDouble(double v) {
  const uint32_t local = OpenFrame(CEL_DOUBLE);
  uint64_t bits;
  static_assert(sizeof(bits) == sizeof(v), "double must bit-cast to uint64_t");
  std::memcpy(&bits, &v, sizeof(bits));
  AppendU64LE(buf_, bits);
  AppendZeros(buf_, 8);
  ABSL_CHECK_EQ(buf_.size() - local, kCelValueSize);
  return base_offset_ + local;
}

uint32_t StaticMemoryBuilder::AllocateSpan(CelKind kind,
                                           absl::string_view bytes) {
  const uint32_t local = OpenFrame(kind);
  const uint32_t payload_abs = base_offset_ + local + kCelValueSize;
  const auto len = static_cast<uint32_t>(bytes.size());

  // CelSpan { ptr, len }, then tail zeros of the 16-byte payload area.
  AppendU32LE(buf_, payload_abs);
  AppendU32LE(buf_, len);
  AppendZeros(buf_, 8);

  // Span payload bytes follow the frame (1-byte alignment), then pad
  // the cursor back to 8-byte alignment for the next frame.
  buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  PadTo(buf_, kAlign);

  return base_offset_ + local;
}

uint32_t StaticMemoryBuilder::AllocateString(absl::string_view s) {
  return AllocateSpan(CEL_STRING, s);
}

uint32_t StaticMemoryBuilder::AllocateBytes(absl::string_view b) {
  return AllocateSpan(CEL_BYTES, b);
}

uint32_t StaticMemoryBuilder::AllocateType(absl::string_view name) {
  // Identical payload to AllocateString — only the kind tag differs.
  return AllocateSpan(CEL_TYPE, name);
}

uint32_t StaticMemoryBuilder::AllocateList(
    absl::Span<const uint32_t> element_offsets) {
  (void)element_offsets;
  ABSL_CHECK(false) << "StaticMemoryBuilder::AllocateList is a stub until M5";
  return 0u;  // unreachable; keeps some compilers' flow analysis happy
}

uint32_t StaticMemoryBuilder::AllocateMap(
    absl::Span<const uint32_t> key_offsets,
    absl::Span<const uint32_t> value_offsets) {
  (void)key_offsets;
  (void)value_offsets;
  ABSL_CHECK(false) << "StaticMemoryBuilder::AllocateMap is a stub until M6";
  return 0u;  // unreachable
}

std::vector<uint8_t> StaticMemoryBuilder::Finalize() && {
  return std::move(buf_);
}

}  // namespace celwasm
