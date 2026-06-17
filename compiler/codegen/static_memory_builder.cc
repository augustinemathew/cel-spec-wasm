#include "compiler/codegen/static_memory_builder.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
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

// Append a fully-formed CelValue frame verbatim.  The runtime, the
// eval-side marshalling, and the wasm target are all little-endian and
// share this 24-byte layout, so a raw copy reproduces the bytes
// cel_list_append_at writes (`*element = *value`); this is the same
// host-struct == wasm-bytes assumption the eval CelValue marshalling
// relies on.
void AppendCelValue(std::vector<uint8_t>& buf, const CelValue& v) {
  static_assert(sizeof(CelValue) == kCelValueSize,
                "CelValue must be 24 bytes for byte-identical run packing");
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  buf.insert(buf.end(), p, p + sizeof(CelValue));
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

StaticMemoryBuilder::MaterializedAggregate StaticMemoryBuilder::MaterializeList(
    absl::Span<const CelValue> elements) {
  static_assert(sizeof(ArenaListHeader) == 16,
                "ArenaListHeader must be 16 bytes");
  const auto n = static_cast<uint32_t>(elements.size());

  // Header sits at the (8-aligned) cursor; its run follows immediately,
  // mirroring cel_list_create's two sequential arena_allocs (header then
  // run).  An empty list has no run and elements_offset == 0.
  ABSL_CHECK_EQ(buf_.size() % kAlign, 0u)
      << "StaticMemoryBuilder cursor is not 8-byte aligned";
  const auto header_local = static_cast<uint32_t>(buf_.size());
  const uint32_t run_local = header_local + sizeof(ArenaListHeader);
  const uint32_t elements_offset = (n == 0) ? 0u : base_offset_ + run_local;

  AppendU32LE(buf_, n);                // count
  AppendU32LE(buf_, n);                // capacity
  AppendU32LE(buf_, elements_offset);  // elements_offset
  AppendU32LE(buf_, 0u);               // _pad

  // Element run: N contiguous 24-byte CelValue frames, in index order.
  for (const CelValue& e : elements) {
    AppendCelValue(buf_, e);
  }

  // Outer CEL_LIST_ARENA frame whose header_ptr points at the header.
  const uint32_t frame_local = OpenFrame(CEL_LIST_ARENA);
  const uint32_t header_abs = base_offset_ + header_local;
  AppendU32LE(buf_, header_abs);  // arena_list.header_ptr @ payload+0
  AppendZeros(buf_, 12);          // rest of the 16-byte payload area
  ABSL_CHECK_EQ(buf_.size() - frame_local, kCelValueSize);

  CelValue frame{};
  frame.kind = CEL_LIST_ARENA;
  frame.payload.arena_list.header_ptr = header_abs;
  return MaterializedAggregate{base_offset_ + frame_local, elements_offset,
                               frame};
}

std::vector<uint8_t> StaticMemoryBuilder::Finalize() && {
  return std::move(buf_);
}

}  // namespace celwasm
