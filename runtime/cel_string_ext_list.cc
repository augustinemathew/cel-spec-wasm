// M12 Slice C — list-bridging string_ext kernels: `s.split(sep[, n])`,
// `list.join([sep])`.  Bridges `CEL_STRING` ↔ `CEL_LIST_ARENA` of
// strings.
//
// Per §4.1 of `m12-string-ext.md`, cross-cutting helpers live in
// `cel_string_ext_internal.h`; this TU owns the list-construction
// glue (direct stamps into the arena list's element array) and the
// split / join algorithms (in the anonymous namespace).
//
// Implementations track cel-cpp's `StringValue::Split` /
// `::Join` (`common/values/string_value.cc`) line-for-line so the
// conformance fixture (`string_ext.textproto::split` / `::join`)
// matches.

#include "runtime/cel_string_ext.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_list.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_ext_internal.h"

namespace {

using celwasm::string_ext_internal::Absorb3vlBinary;
using celwasm::string_ext_internal::Absorb3vlUnary;
using celwasm::string_ext_internal::BorrowSpan;
using celwasm::string_ext_internal::Poison;
using celwasm::string_ext_internal::Utf8Decode;
using celwasm::string_ext_internal::WriteStringFromBytes;

// Local mirror of `arena_list_header` / `arena_list_element` from
// `cel_runtime.c` — those are static in the C TU and not part of
// the public ABI.  Re-declared here so this C++ TU can stamp
// CelValues into the list backing without going through
// `cel_list_append_at` (which requires a pre-populated value slot
// and an extra arena allocation per element).
ArenaListHeader* ListHeader(const CelValue* l) {
  return reinterpret_cast<ArenaListHeader*>(cel_mem_base() +
                                            l->payload.arena_list.header_ptr);
}

CelValue* ListElement(ArenaListHeader* hdr, uint32_t i) {
  return reinterpret_cast<CelValue*>(
      cel_mem_base() + hdr->elements_offset +
      (static_cast<size_t>(kCelListEntryStride) * i));
}

// Allocate a CEL_LIST_ARENA with `capacity` slots; stamp the header
// in linear memory and write `out`.  Returns nullptr on OOM
// (out is already poisoned).
ArenaListHeader* AllocList(CelValue* out, uint32_t capacity) {
  const uint32_t hdr_off =
      arena_alloc(static_cast<uint32_t>(sizeof(ArenaListHeader)));
  if (hdr_off == 0) {
    Poison(out, CEL_ERR_OVERFLOW);
    return nullptr;
  }
  uint32_t elements_off = 0;
  if (capacity > 0) {
    elements_off = arena_alloc(static_cast<uint32_t>(
        static_cast<size_t>(kCelListEntryStride) * capacity));
    if (elements_off == 0) {
      Poison(out, CEL_ERR_OVERFLOW);
      return nullptr;
    }
  }
  auto* hdr = reinterpret_cast<ArenaListHeader*>(cel_mem_base() + hdr_off);
  hdr->count = capacity;  // pre-populated below by callers
  hdr->capacity = capacity;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
  return hdr;
}

// Resolve a list operand to a slot whose CelValue is arena-shaped so
// the join walk below can read the elements run directly.  Arena
// operands pass through; a `CEL_LIST_HOST` operand is snapshotted into
// the arena by `cel_list_arena_view` (which routes through
// `cel_host.cel_list_iter_open`) — that is what makes
// `boundList.join()` work at all, since the elements only exist on the
// host side until they are materialised.  Returns 0 after poisoning
// `out` when the operand is not list-shaped.
uint32_t ArenaListViewSlot(uint32_t out_slot, uint32_t list_slot) {
  const CelValue* list = cel_value_at(list_slot);
  if (list->kind != CEL_LIST_ARENA && list->kind != CEL_LIST_HOST) {
    Poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
    return 0;
  }
  return cel_list_arena_view(list_slot);
}

// Stamp a subspan-string CelValue into an arena-list element slot.
// `byte_off` + `byte_len` are absolute byte positions in the source
// linear-memory span (NOT relative to the source string).
void StampSubstring(CelValue* slot, uint32_t source_ptr, uint32_t byte_off,
                    uint32_t byte_len) {
  slot->kind = CEL_STRING;
  slot->payload.s.ptr = byte_len == 0 ? 0u : source_ptr + byte_off;
  slot->payload.s.len = byte_len;
}

// Compute the split byte ranges per cel-cpp `Split` (lines
// ~1300-1360).  `limit` is already normalised at the caller: 0 →
// empty list (handled upstream), <0 → INT64_MAX, >0 → at most
// `limit-1` splits / `limit` pieces.  Empty delimiter → split by
// code-point.
void ComputeSplitRanges(absl::string_view haystack, absl::string_view sep,
                        int64_t limit,
                        std::vector<std::pair<uint32_t, uint32_t>>* ranges) {
  const auto haystack_size = static_cast<uint32_t>(haystack.size());
  uint32_t pos = 0;
  if (sep.empty()) {
    const auto* base = reinterpret_cast<const uint8_t*>(haystack.data());
    while (pos < haystack_size && limit > 1) {
      uint32_t cp = 0;
      const size_t units = Utf8Decode(base + pos, base + haystack_size, &cp);
      ranges->emplace_back(pos, pos + static_cast<uint32_t>(units));
      pos += static_cast<uint32_t>(units);
      --limit;
    }
  } else {
    const auto sep_size = static_cast<uint32_t>(sep.size());
    while (pos < haystack_size && limit > 1) {
      const size_t next = haystack.find(sep, pos);
      if (next == absl::string_view::npos) break;
      ranges->emplace_back(pos, static_cast<uint32_t>(next));
      pos = static_cast<uint32_t>(next) + sep_size;
      --limit;
    }
  }
  // cel-cpp `Split` final-piece rule (lines ~1352-1354): push a
  // trailing piece unless ALL of (splits non-empty, sep empty, pos
  // exhausted) hold — i.e. push if any of (splits empty, sep
  // non-empty, pos < len).
  if (ranges->empty() || !sep.empty() || pos < haystack_size) {
    ranges->emplace_back(pos, haystack_size);
  }
}

void DoSplit(CelValue* out, const CelValue* s, const CelValue* sep,
             int64_t limit) {
  if (limit == 0) {
    // Empty list — cel-cpp `Split` lines ~1305-1307.
    if (AllocList(out, 0) != nullptr) {
      ListHeader(out)->count = 0;
    }
    return;
  }
  if (limit < 0) limit = std::numeric_limits<int64_t>::max();
  // Capture the source pointer BEFORE AllocList writes into `out`.
  // The compiler may assign `out` the same workspace slot as `s`
  // (slot reuse when `s` is a computed temporary, e.g.
  // `('a'+'b').split(sep)`).  `AllocList(out, …)` then stamps a
  // CEL_LIST_ARENA header over that slot, so re-reading
  // `s->payload.s.ptr` afterward yields the header offset, not the
  // string data — every element would stamp garbage.  The string
  // DATA at `source_ptr` is untouched (only the CelValue slot is
  // overwritten), so elements pointing into it stay valid.  Literal
  // / variable receivers never alias the output slot, which is why
  // this only bit computed receivers.
  const uint32_t source_ptr = s->payload.s.ptr;
  const absl::string_view haystack = BorrowSpan(s->payload.s);
  const absl::string_view sep_view = BorrowSpan(sep->payload.s);
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  ComputeSplitRanges(haystack, sep_view, limit, &ranges);
  ArenaListHeader* hdr = AllocList(out, static_cast<uint32_t>(ranges.size()));
  if (hdr == nullptr) return;
  for (uint32_t k = 0; k < ranges.size(); ++k) {
    StampSubstring(ListElement(hdr, k), source_ptr, ranges[k].first,
                   ranges[k].second - ranges[k].first);
  }
}

// Walk an arena list, concatenating every CEL_STRING element with
// `sep` between.  Returns false if any element is the wrong kind
// (`out` already poisoned) or on arena OOM.
bool BuildJoin(CelValue* out, const CelValue* list, absl::string_view sep) {
  if (list->payload.arena_list.header_ptr == 0) {
    // Header-less empty list (the shape `cel_list_arena_view` vends
    // when a host snapshot has nothing to snapshot).  Offset 0 is
    // never a real allocation, so there is nothing to walk.
    WriteStringFromBytes(out, nullptr, 0);
    return true;
  }
  ArenaListHeader* hdr = ListHeader(list);
  if (hdr->count == 0) {
    WriteStringFromBytes(out, nullptr, 0);
    return true;
  }
  // First pass — validate every element is a string and compute
  // total byte size.  Doing the validation upfront means a
  // half-built output never escapes on error.
  size_t total = 0;
  for (uint32_t k = 0; k < hdr->count; ++k) {
    const CelValue* elt = ListElement(hdr, k);
    if (elt->kind == CEL_ERROR || elt->kind == CEL_UNKNOWN) {
      // A poisoned element — the one-element view `cel_list_arena_view`
      // vends when the host snapshot itself failed (arena OOM).
      // Propagate it verbatim rather than reporting a kind mismatch.
      *out = *elt;
      return false;
    }
    if (elt->kind != CEL_STRING) {
      Poison(out, CEL_ERR_TYPE_MISMATCH);
      return false;
    }
    total += elt->payload.s.len;
  }
  total += static_cast<size_t>(hdr->count - 1) * sep.size();
  std::string buf;
  buf.reserve(total);
  for (uint32_t k = 0; k < hdr->count; ++k) {
    if (k > 0) buf.append(sep.data(), sep.size());
    const CelValue* elt = ListElement(hdr, k);
    const absl::string_view span = BorrowSpan(elt->payload.s);
    buf.append(span.data(), span.size());
  }
  return WriteStringFromBytes(out, buf.data(), buf.size());
}

}  // namespace

// ───────────────────────────────────────────────────────────────
// split
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_split_at_vv(uint32_t out_slot, uint32_t s_slot,
                                       uint32_t sep_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sep = cel_value_at(sep_slot);
  if (Absorb3vlBinary(out, s, sep)) return;
  if (s->kind != CEL_STRING || sep->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // 2-arg `split` has no limit → unlimited (cel-cpp passes -1).
  DoSplit(out, s, sep, -1);
}

extern "C" void cel_string_split_n_at_vvv(uint32_t out_slot, uint32_t s_slot,
                                          uint32_t sep_slot, uint32_t n_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sep = cel_value_at(sep_slot);
  const CelValue* n = cel_value_at(n_slot);
  if (Absorb3vlBinary(out, s, sep)) return;
  if (Absorb3vlUnary(out, n)) return;
  if (s->kind != CEL_STRING || sep->kind != CEL_STRING || n->kind != CEL_INT) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  DoSplit(out, s, sep, n->payload.i);
}

// ───────────────────────────────────────────────────────────────
// join
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_join_at_v(uint32_t out_slot, uint32_t list_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* list = cel_value_at(list_slot);
  if (Absorb3vlUnary(out, list)) return;
  const uint32_t view_slot = ArenaListViewSlot(out_slot, list_slot);
  if (view_slot == 0) return;  // not list-shaped; `out` already poisoned.
  BuildJoin(cel_value_at(out_slot), cel_value_at(view_slot),
            absl::string_view());
}

extern "C" void cel_string_join_sep_at_vv(uint32_t out_slot, uint32_t list_slot,
                                          uint32_t sep_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* list = cel_value_at(list_slot);
  const CelValue* sep = cel_value_at(sep_slot);
  if (Absorb3vlBinary(out, list, sep)) return;
  if (sep->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Capture the separator BYTES before the view call: snapshotting a
  // host list allocates, and `sep` is only a pointer into the slot
  // array.  The string data itself is untouched by arena growth, so
  // the span stays valid.
  const absl::string_view sep_view = BorrowSpan(sep->payload.s);
  const uint32_t view_slot = ArenaListViewSlot(out_slot, list_slot);
  if (view_slot == 0) return;  // not list-shaped; `out` already poisoned.
  BuildJoin(cel_value_at(out_slot), cel_value_at(view_slot), sep_view);
}
