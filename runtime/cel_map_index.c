// SwissTable hash index over an arena map's dense entries run.
//
// PURE ACCELERATOR — every kernel that consumes the index has an
// `index_offset == 0` linear-scan fallback, so this TU never changes
// CEL semantics, only the cost of keyed map operations.  Built once as
// the terminal map-construction step (`cel_map_index_build`); probed by
// `cel_map_index_find`.  Compiled into BOTH the native host build and
// the wasm32 runtime; the placement / hashing comes verbatim from the
// shared, dual-build `cel_map_hash.h` kernel.
//
// Layout + invariants:
// `doc/implementation-plan/rewrite/m32-swisstable-map-index.md` §3.2.

#include "runtime/cel_map.h"

#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_map_hash.h"

// ---- entry accessors (mirror the file-static helpers in cel_runtime.c;
// re-derived here because this is a separate TU) -------------------------

static CelValue* index_map_entry_key(const ArenaMapHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->entries_offset +
                     ((size_t)kCelMapEntryStride * i));
}

// ---- index block geometry (§3.2) --------------------------------------
//
// One contiguous allocation: [control bytes][pad to 4B][u32 slot array].
//   index_offset + 0           : ctrl[0 .. num_slots-1]   (1 byte each)
//   index_offset + num_slots   : ctrl clone[0 .. 6]       (mirror first 7)
//   index_offset + ctrl_total  : (pad to 4-byte align)
//   slot_array_offset          : slot[0 .. num_slots-1]   (u32 each)
// The trailing kGroupWidth-1 = 7 cloned control bytes let an 8-byte SWAR
// group load starting at any slot read valid bytes without a wrap branch.

static uint32_t ctrl_total_bytes(uint32_t num_slots) {
  return num_slots + (uint32_t)(kGroupWidth - 1);
}

static uint32_t align_up_4(uint32_t n) {
  return (n + 3u) & ~3u;
}

static uint32_t slot_array_offset_from_ctrl(uint32_t num_slots) {
  return align_up_4(ctrl_total_bytes(num_slots));
}

// Total index block size in bytes for `num_slots`.
static uint32_t index_block_bytes(uint32_t num_slots) {
  return slot_array_offset_from_ctrl(num_slots) +
         (num_slots * (uint32_t)sizeof(uint32_t));
}

// ---- probe ------------------------------------------------------------

uint32_t cel_map_index_find(const ArenaMapHeader* hdr, const CelValue* key) {
  if (hdr->index_offset == 0) return UINT32_MAX;  // no index -> linear.

  const uint32_t num_slots = cel_map_index_num_slots(hdr->count);
  const uint32_t mask = num_slots - 1u;
  uint8_t* base = cel_memory_base_();
  const uint8_t* ctrl = base + hdr->index_offset;
  const uint32_t* slots =
      (const uint32_t*)(base + hdr->index_offset +
                        slot_array_offset_from_ctrl(num_slots));

  const uint64_t h = cel_map_key_hash(key);
  const uint8_t h2 = cel_h2(h);
  uint32_t seq = (uint32_t)(cel_h1(h) & mask);
  uint32_t step = kGroupWidth;

  // Triangular quadratic probe over groups; on a power-of-two table this
  // visits every group exactly once, and load factor < 1 guarantees a
  // group_match_empty stop.
  for (;;) {
    const uint64_t group = cel_group_load(ctrl + seq);

    // Candidate lanes whose control byte == h2; confirm each with
    // cel_value_eq (collisions re-check).
    uint64_t match = group_match(group, h2);
    while (match != 0) {
      const uint32_t lane = (uint32_t)(__builtin_ctzll(match) >> 3);
      const uint32_t slot = (seq + lane) & mask;
      const uint32_t entry = slots[slot];
      if (cel_value_eq(index_map_entry_key(hdr, entry), key)) {
        return entry;
      }
      match &= match - 1;  // clear the lowest matched lane.
    }

    // An empty slot in this group means the key was never inserted.
    if (group_match_empty(group) != 0) {
      return UINT32_MAX;
    }

    seq = (seq + step) & mask;
    step += kGroupWidth;
  }
}

// ---- build ------------------------------------------------------------

// Place entry `i` into the (already kEmpty-initialised) index.  Returns 1
// on success, 0 if the key duplicates an already-placed key (the caller
// then poisons the map, matching cel_map_insert's dup path).
static int index_place_entry(const ArenaMapHeader* hdr, uint8_t* ctrl,
                             uint32_t* slots, uint32_t num_slots, uint32_t i) {
  const CelValue* key = index_map_entry_key(hdr, i);
  const uint64_t h = cel_map_key_hash(key);
  const uint8_t h2 = cel_h2(h);
  const uint32_t mask = num_slots - 1u;
  uint32_t seq = (uint32_t)(cel_h1(h) & mask);
  uint32_t step = kGroupWidth;

  for (;;) {
    const uint64_t group = cel_group_load(ctrl + seq);

    // A full slot whose key compares equal to this one is a duplicate.
    uint64_t match = group_match(group, h2);
    while (match != 0) {
      const uint32_t lane = (uint32_t)(__builtin_ctzll(match) >> 3);
      const uint32_t slot = (seq + lane) & mask;
      if (cel_value_eq(index_map_entry_key(hdr, slots[slot]), key)) {
        return 0;  // duplicate key.
      }
      match &= match - 1;
    }

    // Otherwise place at the first empty slot in this group.
    const uint64_t empties = group_match_empty(group);
    if (empties != 0) {
      const uint32_t lane = (uint32_t)(__builtin_ctzll(empties) >> 3);
      const uint32_t slot = (seq + lane) & mask;
      ctrl[slot] = h2;
      // Mirror into the cloned control region so wrap-free group loads
      // see the same byte.
      if (slot < (uint32_t)(kGroupWidth - 1)) {
        ctrl[slot + num_slots] = h2;
      }
      slots[slot] = i;
      return 1;
    }

    seq = (seq + step) & mask;
    step += kGroupWidth;
  }
}

void cel_map_index_build(uint32_t map_slot) {
  CelValue* m = cel_value_at(map_slot);
  // Poisoned / non-arena maps carry no index; nothing to build.
  if (m->kind != CEL_MAP_ARENA) return;

  ArenaMapHeader* hdr =
      (ArenaMapHeader*)(cel_memory_base_() + m->payload.arena_map.header_ptr);

  // Tiny maps stay index-free: a linear scan over a few cache lines wins.
  if (hdr->count < (uint32_t)kCelMapIndexThreshold) {
    hdr->index_offset = 0;
    return;
  }

  const uint32_t num_slots = cel_map_index_num_slots(hdr->count);

  // Allocate the contiguous [ctrl][pad][slots] block.  arena_alloc may
  // memory.grow + relocate the base on wasm32, so re-derive every pointer
  // AFTER it returns.  On failure: degrade to linear scan (NEVER poison —
  // the index is a pure accelerator).
  const uint32_t block_off = arena_alloc(index_block_bytes(num_slots));
  // Re-derive the header (allocation may have relocated the base).
  m = cel_value_at(map_slot);
  hdr = (ArenaMapHeader*)(cel_memory_base_() + m->payload.arena_map.header_ptr);
  if (block_off == 0) {
    hdr->index_offset = 0;
    return;
  }

  uint8_t* base = cel_memory_base_();
  uint8_t* ctrl = base + block_off;
  uint32_t* slots =
      (uint32_t*)(base + block_off + slot_array_offset_from_ctrl(num_slots));

  // arena_alloc zero-inits; control bytes must be kEmpty (0x80), not 0
  // (a valid H2).  Initialise the full cloned-control span.
  const uint32_t ctrl_bytes = ctrl_total_bytes(num_slots);
  for (uint32_t b = 0; b < ctrl_bytes; ++b) {
    ctrl[b] = kEmpty;
  }

  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (!index_place_entry(hdr, ctrl, slots, num_slots, i)) {
      // Duplicate key: poison the map value/header exactly as
      // cel_map_insert does, and leave index_offset 0 (no usable index
      // over a poisoned map).
      hdr->index_offset = 0;
      poison(m, CEL_ERR_DUPLICATE_KEY);
      return;
    }
  }

  // Publish the index last: a kernel that reads index_offset != 0 is then
  // guaranteed a fully-populated block.
  hdr->index_offset = block_off;
}
