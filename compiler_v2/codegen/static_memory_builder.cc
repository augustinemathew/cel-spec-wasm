#include "compiler_v2/codegen/static_memory_builder.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/runtime/cel_runtime.h"

namespace celwasm {

namespace {

// CelValue is 24 bytes, 8-byte aligned.  The runtime struct
// (compiler_v2/runtime/cel_runtime.h) has a `_Static_assert
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

// Emits the first 8 bytes of a CelValue frame: (kind u32, pad u32).
// Leaves the write cursor at the start of the 16-byte payload area.
void AppendHeaderPrefix(std::vector<uint8_t>& buf, CelKind kind) {
  AppendU32LE(buf, static_cast<uint32_t>(kind));
  AppendU32LE(buf, 0u);  // _pad
}

// Pads `buf` up to the next multiple of `align` with zeros.
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

uint32_t StaticMemoryBuilder::AppendScalar(const CelScalarPayload& p) {
  ABSL_CHECK_EQ(buf_.size() % kAlign, 0u)
      << "StaticMemoryBuilder cursor is not 8-byte aligned";
  const auto local_offset = static_cast<uint32_t>(buf_.size());

  AppendHeaderPrefix(buf_, p.kind);
  switch (p.kind) {
    case CEL_NULL:
      AppendZeros(buf_, 16);  // no payload
      break;
    case CEL_BOOL:
      AppendU32LE(buf_, static_cast<uint32_t>(p.value.b != 0 ? 1 : 0));
      AppendZeros(buf_, 12);  // tail of the 16-byte payload area
      break;
    case CEL_INT:
      AppendU64LE(buf_, static_cast<uint64_t>(p.value.i));
      AppendZeros(buf_, 8);
      break;
    case CEL_UINT:
      AppendU64LE(buf_, p.value.u);
      AppendZeros(buf_, 8);
      break;
    case CEL_DOUBLE: {
      uint64_t bits;
      static_assert(sizeof(bits) == sizeof(p.value.d),
                    "double must bit-cast to uint64_t");
      std::memcpy(&bits, &p.value.d, sizeof(bits));
      AppendU64LE(buf_, bits);
      AppendZeros(buf_, 8);
      break;
    }
    default:
      ABSL_CHECK(false) << "AppendScalar: non-scalar kind "
                        << static_cast<int>(p.kind);
  }
  ABSL_CHECK_EQ(buf_.size() - local_offset, kCelValueSize);
  return base_offset_ + local_offset;
}

uint32_t StaticMemoryBuilder::AppendSpan(CelKind kind,
                                         absl::string_view bytes) {
  ABSL_CHECK(kind == CEL_STRING || kind == CEL_BYTES)
      << "AppendSpan: expected CEL_STRING or CEL_BYTES, got "
      << static_cast<int>(kind);
  ABSL_CHECK_EQ(buf_.size() % kAlign, 0u)
      << "StaticMemoryBuilder cursor is not 8-byte aligned";
  const auto header_local = static_cast<uint32_t>(buf_.size());
  const uint32_t payload_local = header_local + kCelValueSize;
  const uint32_t payload_abs = base_offset_ + payload_local;
  const auto len = static_cast<uint32_t>(bytes.size());

  // Header: kind, pad, CelSpan { ptr, len }, tail zeros.
  AppendHeaderPrefix(buf_, kind);
  AppendU32LE(buf_, payload_abs);
  AppendU32LE(buf_, len);
  AppendZeros(buf_, 8);  // tail of the 16-byte payload area

  // Span payload bytes follow the header (1-byte alignment).
  buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  // Pad next write back to 8-byte alignment.
  PadTo(buf_, kAlign);

  return base_offset_ + header_local;
}

absl::StatusOr<uint32_t> StaticMemoryBuilder::AppendList(
    absl::Span<const uint32_t> element_offsets) {
  (void)element_offsets;
  return absl::UnimplementedError(
      "StaticMemoryBuilder::AppendList is not implemented until M5");
}

absl::StatusOr<uint32_t> StaticMemoryBuilder::AppendMap(
    absl::Span<const uint32_t> key_offsets,
    absl::Span<const uint32_t> value_offsets) {
  (void)key_offsets;
  (void)value_offsets;
  return absl::UnimplementedError(
      "StaticMemoryBuilder::AppendMap is not implemented until M6");
}

std::vector<uint8_t> StaticMemoryBuilder::Finalize() && {
  return std::move(buf_);
}

}  // namespace celwasm
