#ifndef CELWASM_COMPILER_CODEGEN_STATIC_MEMORY_BUILDER_H_
#define CELWASM_COMPILER_CODEGEN_STATIC_MEMORY_BUILDER_H_

// Packs compile-time-known CelValues into a byte buffer destined
// for the wasm module's `.rodata` data segment.  LayoutPass calls
// one `Allocate*` per `kConst` literal in the AST; each returns
// the frame's byte offset inside the compiled module's linear
// memory.  The buffer is move-returned via `Finalize() &&` and
// stamped into the data segment by codegen.
//
// Return type.  All `Allocate*` methods return `uint32_t` — an
// absolute wasm32 linear-memory byte offset (buffer-local offset
// plus `base_offset`), ready to drop into an `i32.const` or a
// CelSpan without further arithmetic.  Not a host C++ pointer;
// the value is only meaningful inside the emitted wasm module's
// memory.  If we ever target wasm64 this becomes `uint64_t`.
//
// Infallible.  Rodata packing has no failure mode by design (see
// `rewrite/design.md` §6.2.1): no cap, no fallback, no runtime-
// initialised-literal variant — every literal lands in rodata and
// the buffer grows as needed.  Hence no `absl::StatusOr` on any
// Allocate return path.
//
// CelValue is 24 bytes, 8-byte aligned.  For string / bytes the
// payload bytes follow the 24-byte frame directly and the cursor
// is padded back to 8-byte alignment before the next Allocate,
// so every frame lands on an 8-byte boundary.
//
// Constant lists and maps are materialized too: `MaterializeList` /
// `MaterializeMap` write the byte-identical in-arena representation the
// runtime kernels would have built, so a const `[…]` / `{…}` lowers to a
// single `i32.const` and the read-only kernels cannot tell it from an
// arena-built aggregate.  For maps with `count >= kCelMapIndexThreshold`
// the SwissTable hash index is baked byte-identically to what
// `cel_map_index_build` produces, so the index path resolves lookups in
// a materialized map exactly as in a runtime-built one.
//
// Out of scope: messages (host-side proto handles, different
// machinery).

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "runtime/cel_runtime.h"

namespace celwasm {

class StaticMemoryBuilder {
 public:
  // `base_offset` is the linear-memory offset at which this rodata
  // segment will start.  Returned offsets are absolute linear-memory
  // offsets (`base_offset` + buffer-local offset) so callers can
  // drop them into CelSpan / i32.const without an extra add.
  explicit StaticMemoryBuilder(uint32_t base_offset);

  // Allocate a 24-byte CelValue frame in rodata holding one
  // compile-time-known scalar and return its absolute linear-memory
  // offset.  Bytes within the frame beyond the scalar payload are
  // zero-filled so the whole frame is bit-for-bit deterministic.
  uint32_t AllocateNull();
  uint32_t AllocateBool(bool v);
  uint32_t AllocateInt(int64_t v);
  uint32_t AllocateUint(uint64_t v);
  uint32_t AllocateDouble(double v);

  // Allocate a 24-byte CelValue frame whose CelSpan points at the
  // payload bytes that follow immediately after the frame, then pad
  // the next write cursor back to 8-byte alignment.  Returns the
  // frame's absolute linear-memory offset.
  uint32_t AllocateString(absl::string_view s);
  uint32_t AllocateBytes(absl::string_view b);
  // Type-of-types value.  Same payload shape as a string (CelSpan
  // into rodata-resident bytes) but `kind = CEL_TYPE`.  Used by
  // `LayoutPass::ConstLayoutVisitor` when the kConstant's
  // checker-assigned `Repr` is `kType` — see
  // `rewrite/m9-type-subsystem.md`.
  uint32_t AllocateType(absl::string_view name);

  // Result of materializing a constant aggregate.
  struct MaterializedAggregate {
    // Absolute linear-memory offset of the outer CelValue frame — what
    // a top-level const aggregate lowers to via a single `i32.const`.
    uint32_t frame_offset;
    // Absolute offset of the element run (== the header's
    // `elements_offset`); element `i`'s 24-byte CelValue lives at
    // `elements_offset + 24*i`.  Zero for an empty list (no run).
    uint32_t elements_offset;
    // A copy of the value at `frame_offset`.  To nest this aggregate
    // inside another, pass `frame` as an element of the enclosing
    // `MaterializeList` — its `header_ptr` already points at this
    // aggregate's header.  (Needed because the buffer is not readable
    // until `Finalize`, so the caller gets the embeddable value here.)
    CelValue frame;
  };

  // Materialize a constant list as the byte-identical in-arena
  // representation `cel_list_create` + N×`cel_list_append_at` would
  // build: an `ArenaListHeader { count=N, capacity=N, elements_offset,
  // _pad=0 }` followed immediately by the contiguous N×24-byte element
  // run (so the header→run adjacency matches `cel_list_create`'s two
  // sequential `arena_alloc`s), then the outer `CEL_LIST_ARENA`
  // CelValue frame whose `payload.arena_list.header_ptr` points at the
  // header.
  //
  // `elements` are fully-formed 24-byte CelValue frames in index order;
  // any string / bytes / nested-aggregate payload they reference must
  // already be allocated in THIS builder so the embedded offsets are
  // final (allocate leaves first, then the enclosing list —
  // innermost-first ordering).  Nest a const list by passing the
  // `.frame` of its `MaterializeList` result as an element of the outer
  // list.
  //
  // An empty list writes `elements_offset = 0` with no run, matching
  // `cel_list_create(out, 0)`.  Infallible, like the scalar Allocates.
  MaterializedAggregate MaterializeList(absl::Span<const CelValue> elements);

  // One compile-time-known map entry: a {key, value} pair of fully-formed
  // 24-byte CelValue frames.  Like `MaterializeList`'s elements, any
  // string / bytes / nested-aggregate payload either frame references must
  // already be allocated in THIS builder so the embedded offsets are final
  // (innermost-first ordering).
  struct MapEntry {
    CelValue key;
    CelValue value;
  };

  // Result of materializing a constant map.
  struct MaterializedMap {
    // Absolute linear-memory offset of the outer CEL_MAP_ARENA CelValue
    // frame — what a const map lowers to via a single `i32.const`.
    uint32_t frame_offset;
    // Absolute offset of the 48-byte (key,val) entry run (== the header's
    // `entries_offset`).  Entry `i`'s key lives at `entries_offset + 48*i`
    // and its value at `entries_offset + 48*i + 24`.  Zero for an empty
    // map (no run).
    uint32_t entries_offset;
    // Absolute offset of the baked SwissTable index block, or 0 when none
    // was baked (`count < kCelMapIndexThreshold`) — matching the runtime's
    // `ArenaMapHeader::index_offset` exactly.
    uint32_t index_offset;
    // A copy of the value at `frame_offset` — embed this as the value of a
    // `MapEntry` (or element of a `MaterializeList`) to nest the map.
    CelValue frame;
  };

  // Materialize a constant map as the byte-identical in-arena
  // representation `cel_map_create` + N×`cel_map_insert` (in source order)
  // followed by the terminal `cel_map_index_build` would build: an
  // `ArenaMapHeader { count=N, capacity=N, entries_offset, index_offset }`
  // followed immediately by the contiguous N×48-byte {key,val} entry run
  // (mirroring `cel_map_create`'s header→run adjacency), then — for
  // `N >= kCelMapIndexThreshold` — the SwissTable index block (control
  // bytes + cloned mirror + u32 slot array) placed byte-identically to
  // `cel_map_index_build`, then the outer `CEL_MAP_ARENA` CelValue frame.
  //
  // Entries are written in the given (source) order, matching the runtime
  // build path so a materialized map and a runtime-built one are
  // byte-identical (CEL maps have no observable order — the entry run and
  // the index both depend only on the keys' content, not the offset).
  //
  // Returns `std::nullopt` when the entries contain a duplicate key (under
  // the runtime's `cel_map_key_eq`): a duplicate-key map literal must keep
  // the per-Eval build path so it poisons with `CEL_ERR_DUPLICATE_KEY` at
  // construction time, matching today's semantics.  An empty map writes
  // `entries_offset = 0` / `index_offset = 0` with no run, matching
  // `cel_map_create(out, 0)`.  Infallible otherwise (no rodata cap here).
  std::optional<MaterializedMap> MaterializeMap(
      absl::Span<const MapEntry> entries);

  // Move-return the packed buffer.  After Finalize the builder is
  // consumed.  Use `size_bytes()` beforehand to learn the final size
  // for cel.abi / data-segment bookkeeping.
  std::vector<uint8_t> Finalize() &&;

  uint32_t size_bytes() const {
    return static_cast<uint32_t>(buf_.size());
  }
  uint32_t base_offset() const {
    return base_offset_;
  }

 private:
  // Writes the 8-byte (kind, pad) header prefix at the current
  // cursor and returns the local offset of the frame's start.
  // Caller must then write exactly 16 bytes of payload to complete
  // the 24-byte frame.  CHECKs the cursor is 8-byte aligned.
  uint32_t OpenFrame(CelKind kind);

  // Shared implementation for AllocateString / AllocateBytes.
  uint32_t AllocateSpan(CelKind kind, absl::string_view bytes);

  std::vector<uint8_t> buf_;
  uint32_t base_offset_ = 0;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_STATIC_MEMORY_BUILDER_H_
