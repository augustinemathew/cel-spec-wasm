// M12 Slice E — format renderer implementations.  One `Render*`
// function per directive type; each is short and dispatches on the
// argument's `CelKind`.
//
// Behaviour tracks cel-cpp `extensions/formatting.cc::FormatString`
// + `FormatDecimal` / `FormatFixed` / `FormatScientific` /
// `FormatBinary` / `FormatOctal` / `FormatHex` line-by-line so the
// 78-row `string_ext.textproto::format` conformance section
// matches without re-deriving spec corners.  Where cel-cpp uses
// `absl::StrFormat` we use the same; where it uses `absl::StrAppend`
// (default-precision double rendering) we use the same — keeps the
// byte-level output identical.
//
// Lifecycle: renderers append to a caller-owned `std::string& buf`
// and return `false` on error.  No arena allocation here — the
// dispatcher in `cel_string_format.cc` does that once at the end.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <type_traits>

#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_list.h"
#include "runtime/cel_map.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_format_internal.h"
#include "runtime/cel_time_canonical.h"

namespace celwasm::string_format_internal {

namespace {

// Borrow a CelSpan as an absl::string_view into linear memory.
absl::string_view BorrowSpan(const CelSpan& s) {
  return {reinterpret_cast<const char*>(cel_mem_base()) + s.ptr, s.len};
}

// `%s` on a double: cel-cpp uses `absl::StrAppend(&scratch, number)`
// for finite values; NaN / ±Inf use the canonical string tokens.
void AppendDoubleCanonical(std::string& buf, double v) {
  if (std::isnan(v)) {
    buf.append("NaN");
    return;
  }
  if (v == std::numeric_limits<double>::infinity()) {
    buf.append("Infinity");
    return;
  }
  if (v == -std::numeric_limits<double>::infinity()) {
    buf.append("-Infinity");
    return;
  }
  absl::StrAppend(&buf, v);
}

// Duration / timestamp `%s` canonical forms — the shared
// `cel_time_canonical` helpers, the same implementation the
// `string(<timestamp|duration>)` conversion kernels in
// `cel_time_parse.cc` use.  Conformance scores these byte-exactly,
// so `%s` and `string()` must never drift.
void AppendDurationCanonical(std::string& buf, int64_t secs, int32_t nanos) {
  buf.append(FormatProtoDuration(secs, nanos));
}

void AppendTimestampCanonical(std::string& buf, int64_t secs, int32_t nanos) {
  buf.append(FormatTimestampRfc3339(secs, nanos));
}

// Linear-memory offset of a CelValue a caller handed us.  Every
// CelValue a renderer sees lives in the shared linear memory (a
// workspace slot, an arena elements run, or a map-iter snapshot
// cell), so the round-trip is exact.  Needed because the origin-
// normalising helpers (`cel_list_arena_view`, `cel_map_iter_init`)
// address values by SLOT, not by pointer.
uint32_t SlotOf(const CelValue* v) {
  return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(v) -
                               cel_mem_base());
}

// Element count / element slot of an arena list, addressed by header
// OFFSET so both survive a base relocation.  A zero header offset is
// the header-less empty shape (offset 0 is never a real allocation).
uint32_t ListCountAt(uint32_t header_ptr) {
  if (header_ptr == 0) return 0;
  return reinterpret_cast<const ArenaListHeader*>(cel_mem_base() + header_ptr)
      ->count;
}

uint32_t ListElementSlot(uint32_t header_ptr, uint32_t i) {
  const auto* hdr =
      reinterpret_cast<const ArenaListHeader*>(cel_mem_base() + header_ptr);
  return hdr->elements_offset +
         (static_cast<uint32_t>(kCelListEntryStride) * i);
}

// Render the CelValue at `slot`.  The slot-addressed entry point:
// nested aggregates can allocate while being lifted, so a renderer
// must never hold a CelValue* across a recursive call.
bool RenderStringAtSlot(std::string& buf, uint32_t slot) {
  return RenderString(buf, cel_value_at(slot));
}

// %s on a list of EITHER origin: normalise through
// `cel_list_arena_view` (arena operands pass through; a host list is
// snapshotted via `cel_host.cel_list_iter_open`), then walk the
// elements run, render each, comma-space separate, wrap in `[]`.
// Returns false on the first element-level error.
bool AppendListCanonical(std::string& buf, uint32_t list_slot) {
  const uint32_t view_slot = cel_list_arena_view(list_slot);
  const CelValue* view = cel_value_at(view_slot);
  if (view->kind != CEL_LIST_ARENA) return false;  // runtime drift
  const uint32_t header_ptr = view->payload.arena_list.header_ptr;
  const uint32_t count = ListCountAt(header_ptr);
  buf.push_back('[');
  for (uint32_t k = 0; k < count; ++k) {
    if (k > 0) buf.append(", ");
    // Re-derive per iteration: a nested host element lifts through
    // `cel_list_arena_view`, which allocates.
    if (!RenderStringAtSlot(buf, ListElementSlot(header_ptr, k))) return false;
  }
  buf.push_back(']');
  return true;
}

// cel-cpp's FormatMap admits STRING / BOOL / INT / UINT keys only.
bool IsFormattableMapKey(uint32_t kind) {
  return kind == CEL_STRING || kind == CEL_BOOL || kind == CEL_INT ||
         kind == CEL_UINT;
}

// %s on a map of EITHER origin.  cel-cpp builds a
// btree_map<stringified_key, value>, so iteration order is
// lexicographic on the key's string form.
//
// The walk goes through the origin-agnostic map iterator
// (`cel_map_iter_init` / `_next` / `_key_at` / `_value_at`), which
// passes an arena map through in place and snapshots a host map via
// `cel_host.cel_map_iter_open`.  Key AND value are rendered to
// strings during the walk rather than collected as pointers: the
// iterator copies each entry into a shared scratch pair that the
// next `_next` overwrites, and rendering a nested aggregate value can
// relocate the linear-memory base.
bool AppendMapCanonical(std::string& buf, uint32_t map_slot) {
  const uint32_t scratch = arena_alloc(2u * sizeof(CelValue));
  if (scratch == 0) return false;  // arena OOM
  const uint32_t key_slot = scratch;
  const uint32_t val_slot = scratch + sizeof(CelValue);
  const uint32_t iter = cel_map_iter_init(map_slot);
  std::map<std::string, std::string> sorted;
  while (cel_map_iter_next(iter) != 0) {
    cel_map_iter_key_at(key_slot, iter);
    if (!IsFormattableMapKey(cel_value_at(key_slot)->kind)) return false;
    std::string key_str;
    if (!RenderStringAtSlot(key_str, key_slot)) return false;
    cel_map_iter_value_at(val_slot, iter);
    std::string val_str;
    if (!RenderStringAtSlot(val_str, val_slot)) return false;
    sorted.emplace(std::move(key_str), std::move(val_str));
  }
  buf.push_back('{');
  bool first = true;
  for (const auto& [key_str, val_str] : sorted) {
    if (!first) buf.append(", ");
    first = false;
    buf.append(key_str);
    buf.append(": ");
    buf.append(val_str);
  }
  buf.push_back('}');
  return true;
}

// %s on a TYPE value: just the type-name string.
void AppendTypeName(std::string& buf, const CelValue* v) {
  buf.append(BorrowSpan(v->payload.s));
}

// Return the magnitude of a signed int64 as a uint64.  Handles
// INT64_MIN safely (negating INT64_MIN as a signed int is UB; we
// cast to unsigned first, then negate).
uint64_t SignedAbsAsUint(int64_t v) {
  if (v >= 0) return static_cast<uint64_t>(v);
  return static_cast<uint64_t>(-(v + 1)) + 1u;
}

}  // namespace

// ───────────────────────────────────────────────────────────────
// %s — canonical string form.
// ───────────────────────────────────────────────────────────────

bool RenderString(std::string& buf, const CelValue* v) {
  switch (v->kind) {
    case CEL_NULL:
      buf.append("null");
      return true;
    case CEL_BOOL:
      buf.append(v->payload.b ? "true" : "false");
      return true;
    case CEL_INT:
      absl::StrAppend(&buf, v->payload.i);
      return true;
    case CEL_UINT:
      absl::StrAppend(&buf, v->payload.u);
      return true;
    case CEL_DOUBLE:
      AppendDoubleCanonical(buf, v->payload.d);
      return true;
    case CEL_STRING:
      buf.append(BorrowSpan(v->payload.s));
      return true;
    case CEL_BYTES:
      // cel-cpp treats `%s` on bytes as a UTF-8 view of the raw
      // payload — i.e. the bytes copy out verbatim.  Same here.
      buf.append(BorrowSpan(v->payload.bytes));
      return true;
    case CEL_TYPE:
      AppendTypeName(buf, v);
      return true;
    case CEL_TIMESTAMP:
      AppendTimestampCanonical(buf, v->payload.ts.seconds, v->payload.ts.nanos);
      return true;
    case CEL_DURATION:
      AppendDurationCanonical(buf, v->payload.dur.seconds,
                              v->payload.dur.nanos);
      return true;
    case CEL_LIST_ARENA:
    case CEL_LIST_HOST:
      return AppendListCanonical(buf, SlotOf(v));
    case CEL_MAP_ARENA:
    case CEL_MAP_HOST:
      return AppendMapCanonical(buf, SlotOf(v));
    default:
      // Open by construction: `kind` is a wire field read out of
      // linear memory.  ERROR / UNKNOWN / MESSAGE have no canonical
      // `%s` form, and the caller turns the `false` into
      // CEL_ERR_INVALID_ARGUMENT.
      return false;
  }
}

// ───────────────────────────────────────────────────────────────
// %d — decimal.  cel-cpp accepts INT, UINT, DOUBLE; doubles route
// through `FormatDouble` with no precision (default 6) — i.e.
// "1.234500" for `1.2345`.  Bool / string / etc. error.
// ───────────────────────────────────────────────────────────────

bool RenderDecimal(std::string& buf, const CelValue* v) {
  switch (v->kind) {
    case CEL_INT:
      absl::StrAppend(&buf, v->payload.i);
      return true;
    case CEL_UINT:
      absl::StrAppend(&buf, v->payload.u);
      return true;
    case CEL_DOUBLE:
      return RenderFixed(buf, v, kPrecisionDefault);
    default:
      return false;
  }
}

// ───────────────────────────────────────────────────────────────
// %f / %e — fixed / scientific.  Per cel-cpp `FormatFixed` /
// `FormatScientific`: input must be DOUBLE or a STRING naming a
// finite/NaN/Inf value.  We accept DOUBLE, INT, UINT (INT/UINT
// converted to double) — matches the spec table.
// ───────────────────────────────────────────────────────────────

namespace {

// Convert any of DOUBLE / INT / UINT to a double; returns false on
// kind mismatch.
bool ToDouble(const CelValue* v, double* out) {
  switch (v->kind) {
    case CEL_DOUBLE:
      *out = v->payload.d;
      return true;
    case CEL_INT:
      *out = static_cast<double>(v->payload.i);
      return true;
    case CEL_UINT:
      *out = static_cast<double>(v->payload.u);
      return true;
    default:
      return false;
  }
}

// Per cel-cpp `FormatDouble`: NaN / ±Inf surface as the canonical
// tokens; finite values format with explicit precision (default 6).
void RenderDoubleWithPrecision(std::string& buf, double v, int precision,
                               bool scientific) {
  if (std::isnan(v)) {
    buf.append("NaN");
    return;
  }
  if (v == std::numeric_limits<double>::infinity()) {
    buf.append("Infinity");
    return;
  }
  if (v == -std::numeric_limits<double>::infinity()) {
    buf.append("-Infinity");
    return;
  }
  const int p = (precision == kPrecisionDefault) ? 6 : precision;
  absl::StrAppend(&buf, scientific ? absl::StrFormat("%.*e", p, v)
                                   : absl::StrFormat("%.*f", p, v));
}

}  // namespace

bool RenderFixed(std::string& buf, const CelValue* v, int precision) {
  double d;
  if (!ToDouble(v, &d)) return false;
  RenderDoubleWithPrecision(buf, d, precision, /*scientific=*/false);
  return true;
}

bool RenderScientific(std::string& buf, const CelValue* v, int precision) {
  double d;
  if (!ToDouble(v, &d)) return false;
  RenderDoubleWithPrecision(buf, d, precision, /*scientific=*/true);
  return true;
}

// ───────────────────────────────────────────────────────────────
// %b — binary.  INT, UINT, BOOL.  Signed ints emit a leading `-`
// for negatives (Go-style); bool emits "1"/"0".
// ───────────────────────────────────────────────────────────────

namespace {

void AppendBinaryUint(std::string& buf, uint64_t v) {
  if (v == 0) {
    buf.push_back('0');
    return;
  }
  // Reserve max 64 bits.
  char tmp[64];
  size_t n = 0;
  while (v != 0) {
    tmp[n++] = (v & 1u) ? '1' : '0';
    v >>= 1;
  }
  while (n > 0) {
    buf.push_back(tmp[--n]);
  }
}

}  // namespace

bool RenderBinary(std::string& buf, const CelValue* v) {
  switch (v->kind) {
    case CEL_INT: {
      const int64_t x = v->payload.i;
      if (x < 0) buf.push_back('-');
      AppendBinaryUint(buf, SignedAbsAsUint(x));
      return true;
    }
    case CEL_UINT:
      AppendBinaryUint(buf, v->payload.u);
      return true;
    case CEL_BOOL:
      buf.push_back(v->payload.b ? '1' : '0');
      return true;
    default:
      return false;
  }
}

// ───────────────────────────────────────────────────────────────
// %o — octal.  INT, UINT.  Signed ints get a leading `-` for
// negatives.
// ───────────────────────────────────────────────────────────────

bool RenderOctal(std::string& buf, const CelValue* v) {
  switch (v->kind) {
    case CEL_INT: {
      const int64_t x = v->payload.i;
      if (x < 0) {
        absl::StrAppend(&buf, absl::StrFormat("-%o", SignedAbsAsUint(x)));
      } else {
        absl::StrAppend(&buf, absl::StrFormat("%o", x));
      }
      return true;
    }
    case CEL_UINT:
      absl::StrAppend(&buf, absl::StrFormat("%o", v->payload.u));
      return true;
    default:
      return false;
  }
}

// ───────────────────────────────────────────────────────────────
// %x / %X — hex.  INT, UINT, STRING, BYTES.  Strings / bytes
// render as hex of the raw byte sequence (cel-cpp uses
// `absl::BytesToHexString`).  Negative ints get a leading `-`.
// ───────────────────────────────────────────────────────────────

namespace {

void AppendBytesAsHex(std::string& buf, absl::string_view bytes, bool upper) {
  std::string hex = absl::BytesToHexString(bytes);
  if (upper) absl::AsciiStrToUpper(&hex);
  buf.append(hex);
}

}  // namespace

namespace {

void AppendHexUint(std::string& buf, uint64_t v, bool upper) {
  absl::StrAppend(&buf,
                  upper ? absl::StrFormat("%X", v) : absl::StrFormat("%x", v));
}

}  // namespace

bool RenderHex(std::string& buf, const CelValue* v, bool upper) {
  switch (v->kind) {
    case CEL_INT: {
      const int64_t x = v->payload.i;
      if (x < 0) {
        buf.push_back('-');
        AppendHexUint(buf, SignedAbsAsUint(x), upper);
      } else {
        AppendHexUint(buf, static_cast<uint64_t>(x), upper);
      }
      return true;
    }
    case CEL_UINT:
      AppendHexUint(buf, v->payload.u, upper);
      return true;
    case CEL_STRING:
      AppendBytesAsHex(buf, BorrowSpan(v->payload.s), upper);
      return true;
    case CEL_BYTES:
      AppendBytesAsHex(buf, BorrowSpan(v->payload.bytes), upper);
      return true;
    default:
      return false;
  }
}

}  // namespace celwasm::string_format_internal
