#include "compiler/codegen/static_memory_builder.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_internal.h"  // cel_value_eq
#include "runtime/cel_layout.h"
#include "runtime/cel_make.h"
#include "runtime/cel_map_hash.h"
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

// ---- baked SwissTable index geometry (mirrors cel_map_index.c §3.2) ----
//
// The placement math + control-byte layout are re-derived here from the
// shared frozen kernel constants (cel_map_hash.h) so the baked index is
// byte-identical to what `cel_map_index_build` produces at runtime.

uint32_t IndexCtrlTotalBytes(uint32_t num_slots) {
  return num_slots + static_cast<uint32_t>(kGroupWidth - 1);
}

uint32_t IndexAlignUp4(uint32_t n) {
  return (n + 3u) & ~3u;
}

uint32_t IndexSlotArrayOffset(uint32_t num_slots) {
  return IndexAlignUp4(IndexCtrlTotalBytes(num_slots));
}

uint32_t IndexBlockBytes(uint32_t num_slots) {
  return IndexSlotArrayOffset(num_slots) +
         (num_slots * static_cast<uint32_t>(sizeof(uint32_t)));
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

namespace {

// Build the placement key the hash / equality kernels read.  For a span
// key (string / bytes) the bytes live in the builder's not-yet-finalized
// buffer (unreadable through `cel_memory_base_()`), so copy them into the
// native runtime arena via `cel_make_string` / `cel_make_bytes` and return
// a key whose CelSpan points at the staged copy.  Non-span keys carry
// their payload in-struct and pass through verbatim.
//
// Byte-identity: `cel_map_key_hash` / `cel_value_eq` over the staged key
// read the same content + length the runtime would, and both are
// content-based (the offset value never enters the hash), so the computed
// control byte + slot placement match `cel_map_index_build` bit-for-bit.
// Returns std::nullopt iff the key bytes overflow the codegen staging
// arena (`cel_make_*` returns 0): the caller then declines to materialize
// and keeps the per-Eval build path, which stages keys into the larger
// eval-time arena.  This is a graceful fallback, NOT a process abort — a
// large-string-key const map is a legitimate input and must not crash the
// compiler (the build path handles it identically to a non-const map).
std::optional<CelValue> StagePlacementKey(const CelValue& key,
                                          const std::vector<uint8_t>& buf,
                                          uint32_t base_offset) {
  if (key.kind != CEL_STRING && key.kind != CEL_BYTES) {
    return key;  // bool / int / uint: payload is in-struct.
  }
  const uint32_t abs_ptr = key.payload.s.ptr;
  const uint32_t len = key.payload.s.len;
  ABSL_CHECK_GE(abs_ptr, base_offset)
      << "StagePlacementKey: span key ptr " << abs_ptr
      << " precedes the builder base_offset " << base_offset
      << " (key bytes must be allocated in THIS builder)";
  const uint32_t local = abs_ptr - base_offset;
  ABSL_CHECK_LE(static_cast<size_t>(local) + len, buf.size())
      << "StagePlacementKey: span key [" << local << ", " << local + len
      << ") runs past the builder buffer (size " << buf.size() << ")";
  const char* src = reinterpret_cast<const char*>(buf.data()) + local;
  const uint32_t off = key.kind == CEL_STRING ? cel_make_string(src, len)
                                              : cel_make_bytes(src, len);
  if (off == 0u) {
    return std::nullopt;  // staging-arena OOM at codegen → keep build path.
  }
  return *cel_value_at(off);
}

// Place staged entry `i` into the index, mirroring `index_place_entry` in
// cel_map_index.c verbatim (same triangular probe, same control-byte /
// clone / slot writes, same dup-via-cel_value_eq detection).  Returns true
// on placement, false on a duplicate key.  `ctrl` / `slots` point at the
// in-progress baked index; `staged[]` are the placement keys.
bool PlaceBakedEntry(absl::Span<const CelValue> staged, uint8_t* ctrl,
                     uint32_t* slots, uint32_t num_slots, uint32_t i) {
  const CelValue& key = staged[i];
  const uint64_t h = cel_map_key_hash(&key);
  const uint8_t h2 = cel_h2(h);
  const uint32_t mask = num_slots - 1u;
  auto seq = static_cast<uint32_t>(cel_h1(h) & mask);
  uint32_t step = kGroupWidth;
  for (;;) {
    const uint64_t group = cel_group_load(ctrl + seq);
    uint64_t match = group_match(group, h2);
    while (match != 0) {
      const auto lane = static_cast<uint32_t>(__builtin_ctzll(match) >> 3);
      const uint32_t slot = (seq + lane) & mask;
      if (cel_value_eq(&staged[slots[slot]], &key)) {
        return false;  // duplicate key.
      }
      match &= match - 1;
    }
    const uint64_t empties = group_match_empty(group);
    if (empties != 0) {
      const auto lane = static_cast<uint32_t>(__builtin_ctzll(empties) >> 3);
      const uint32_t slot = (seq + lane) & mask;
      ctrl[slot] = h2;
      if (slot < static_cast<uint32_t>(kGroupWidth - 1)) {
        ctrl[slot + num_slots] = h2;
      }
      slots[slot] = i;
      return true;
    }
    seq = (seq + step) & mask;
    step += kGroupWidth;
  }
}

// Compute the baked index block for the staged keys: an
// IndexBlockBytes(num_slots)-byte buffer laid out [ctrl + clone][pad][u32
// slots], control bytes kEmpty-initialised then filled by PlaceBakedEntry
// in source order — byte-identical to `cel_map_index_build`'s output.
// Returns std::nullopt iff a duplicate key is detected (caller falls back
// to the per-Eval build path so the runtime poisons CEL_ERR_DUPLICATE_KEY).
std::optional<std::vector<uint8_t>> BakeIndexBlock(
    absl::Span<const CelValue> staged, uint32_t num_slots) {
  const uint32_t block_bytes = IndexBlockBytes(num_slots);
  std::vector<uint8_t> block(block_bytes, 0u);
  uint8_t* ctrl = block.data();
  auto* slots = reinterpret_cast<uint32_t*>(block.data() +
                                            IndexSlotArrayOffset(num_slots));
  // Control bytes (including the 7 cloned mirror bytes) start at kEmpty,
  // not 0 (a valid H2) — matching cel_map_index_build's init loop.
  const uint32_t ctrl_bytes = IndexCtrlTotalBytes(num_slots);
  for (uint32_t b = 0; b < ctrl_bytes; ++b) {
    ctrl[b] = kEmpty;
  }
  for (uint32_t i = 0; i < staged.size(); ++i) {
    if (!PlaceBakedEntry(staged, ctrl, slots, num_slots, i)) {
      return std::nullopt;  // duplicate key.
    }
  }
  return block;
}

// Outcome of staging a const map's keys and deciding its index.
// `materializable == false` means the caller must NOT materialize and must
// keep the per-Eval build path — either a duplicate key (the build path
// poisons CEL_ERR_DUPLICATE_KEY) or key-staging OOM at codegen (the build
// path stages keys into the larger eval-time arena).  `index` holds the
// baked index block iff one was baked (N >= threshold).
struct MapIndexDecision {
  bool materializable = true;
  std::optional<std::vector<uint8_t>> index;
};

// Stage every key (so the hash / equality kernels can read its bytes),
// detect duplicates, and bake the SwissTable index for N >= threshold.
// Decided BEFORE the caller writes anything into the builder buffer.  A
// single-entry map can't carry a dup and needs no index.
MapIndexDecision StageAndDecideIndex(
    absl::Span<const StaticMemoryBuilder::MapEntry> entries,
    const std::vector<uint8_t>& buf, uint32_t base_offset) {
  const auto n = static_cast<uint32_t>(entries.size());
  if (n <= 1) return MapIndexDecision{};
  arena_init(CELWASM_ARENA_CAPACITY_BYTES);
  arena_reset();
  std::vector<CelValue> staged;
  staged.reserve(n);
  for (const StaticMemoryBuilder::MapEntry& e : entries) {
    std::optional<CelValue> sk = StagePlacementKey(e.key, buf, base_offset);
    if (!sk.has_value()) {
      // Key bytes overflow the codegen staging arena — keep the build path.
      return MapIndexDecision{/*materializable=*/false, std::nullopt};
    }
    staged.push_back(*sk);
  }
  if (n >= static_cast<uint32_t>(kCelMapIndexThreshold)) {
    // Bake the index; its placement loop also detects dups (nullopt).
    std::optional<std::vector<uint8_t>> block =
        BakeIndexBlock(staged, cel_map_index_num_slots(n));
    if (!block.has_value()) {
      return MapIndexDecision{/*materializable=*/false, std::nullopt};
    }
    return MapIndexDecision{/*materializable=*/true, std::move(block)};
  }
  // Below threshold: no index; dup detection is the O(n^2) cel_value_eq
  // scan cel_map_insert performs.
  for (uint32_t i = 0; i < n; ++i) {
    for (uint32_t j = i + 1; j < n; ++j) {
      if (cel_value_eq(&staged[i], &staged[j])) {
        return MapIndexDecision{/*materializable=*/false, std::nullopt};
      }
    }
  }
  return MapIndexDecision{};
}

// Append the baked index block (if any) after the entry run and patch the
// header's index_offset field in place.  Returns the absolute index_offset
// (0 when no block was baked).  The entry run ends 8-aligned (48B stride),
// so the block lands 8-aligned exactly as arena_alloc would place it.
uint32_t AppendBakedIndex(std::vector<uint8_t>& buf, uint32_t base_offset,
                          uint32_t index_offset_field_local,
                          const std::optional<std::vector<uint8_t>>& block) {
  if (!block.has_value()) return 0;
  ABSL_CHECK_EQ(buf.size() % kAlign, 0u)
      << "index block must start 8-byte aligned (entry run is 48B stride)";
  const uint32_t index_offset = base_offset + static_cast<uint32_t>(buf.size());
  buf.insert(buf.end(), block->begin(), block->end());
  PadTo(buf, kAlign);
  for (int i = 0; i < 4; ++i) {
    buf[index_offset_field_local + i] =
        static_cast<uint8_t>((index_offset >> (i * 8)) & 0xFFu);
  }
  return index_offset;
}

}  // namespace

std::optional<StaticMemoryBuilder::MaterializedMap>
StaticMemoryBuilder::MaterializeMap(absl::Span<const MapEntry> entries) {
  static_assert(sizeof(ArenaMapHeader) == 16,
                "ArenaMapHeader must be 16 bytes");
  static_assert(kCelMapEntryStride == 2 * kCelValueSize,
                "map entry stride must be two CelValues (key, val)");
  const auto n = static_cast<uint32_t>(entries.size());

  // Decline to materialize a dup-key map (the build path poisons
  // CEL_ERR_DUPLICATE_KEY) or one whose keys overflow the codegen staging
  // arena (the build path stages keys into the eval-time arena) — either
  // way keep the per-Eval build path.
  MapIndexDecision decision = StageAndDecideIndex(entries, buf_, base_offset_);
  if (!decision.materializable) return std::nullopt;

  // Header sits at the (8-aligned) cursor; its 48-byte (key,val) run
  // follows immediately, mirroring cel_map_create's header+entries allocs.
  ABSL_CHECK_EQ(buf_.size() % kAlign, 0u)
      << "StaticMemoryBuilder cursor is not 8-byte aligned";
  const auto header_local = static_cast<uint32_t>(buf_.size());
  const uint32_t run_local = header_local + sizeof(ArenaMapHeader);
  const uint32_t entries_offset = (n == 0) ? 0u : base_offset_ + run_local;

  AppendU32LE(buf_, n);               // count
  AppendU32LE(buf_, n);               // capacity
  AppendU32LE(buf_, entries_offset);  // entries_offset
  AppendU32LE(buf_, 0u);  // index_offset (patched by AppendBakedIndex)

  // Entry run: N contiguous 48-byte {key, val} pairs in source order.
  for (const MapEntry& e : entries) {
    AppendCelValue(buf_, e.key);
    AppendCelValue(buf_, e.value);
  }

  const uint32_t index_offset =
      AppendBakedIndex(buf_, base_offset_, header_local + 12u, decision.index);

  // Outer CEL_MAP_ARENA frame whose header_ptr points at the header.
  const uint32_t frame_local = OpenFrame(CEL_MAP_ARENA);
  const uint32_t header_abs = base_offset_ + header_local;
  AppendU32LE(buf_, header_abs);  // arena_map.header_ptr @ payload+0
  AppendZeros(buf_, 12);          // rest of the 16-byte payload area
  ABSL_CHECK_EQ(buf_.size() - frame_local, kCelValueSize);

  CelValue frame{};
  frame.kind = CEL_MAP_ARENA;
  frame.payload.arena_map.header_ptr = header_abs;
  return MaterializedMap{base_offset_ + frame_local, entries_offset,
                         index_offset, frame};
}

std::vector<uint8_t> StaticMemoryBuilder::Finalize() && {
  return std::move(buf_);
}

}  // namespace celwasm
