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
#include "runtime/cel_data.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_format_internal.h"

namespace celwasm::string_format_internal {

namespace {

// Borrow a CelSpan as an absl::string_view into linear memory.
absl::string_view BorrowSpan(const CelSpan& s) {
  return {reinterpret_cast<const char*>(cel_mem_base()) + s.ptr, s.len};
}

// Pull a key/value pair out of an arena map's entries run.  Each
// entry is `{key:CelValue, value:CelValue}` at `entries_offset +
// i*kCelMapEntryStride` (key first, value at offset
// `sizeof(CelValue)`).  Mirrors `arena_map_entry_key` /
// `arena_map_entry_val` in `cel_runtime.c`.
const CelValue* MapKey(const ArenaMapHeader* hdr, uint32_t i) {
  return reinterpret_cast<const CelValue*>(
      cel_mem_base() + hdr->entries_offset +
      (static_cast<size_t>(kCelMapEntryStride) * i));
}

const CelValue* MapVal(const ArenaMapHeader* hdr, uint32_t i) {
  return reinterpret_cast<const CelValue*>(
      cel_mem_base() + hdr->entries_offset +
      (static_cast<size_t>(kCelMapEntryStride) * i) + sizeof(CelValue));
}

const CelValue* ListElement(const ArenaListHeader* hdr, uint32_t i) {
  return reinterpret_cast<const CelValue*>(
      cel_mem_base() + hdr->elements_offset +
      (static_cast<size_t>(kCelListEntryStride) * i));
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

// Duration `%s` canonical form — cel-cpp's `FormatDuration`.  Negative
// duration gets a leading `-`; absolute seconds + nanos.  Trailing
// zero triples are NOT stripped (cel-cpp uses 3/6/9-digit fields
// depending on which place is the lowest non-zero digit; matches
// `proto.duration` JSON form).
void AppendDurationCanonical(std::string& buf, int64_t secs, int32_t nanos) {
  if (secs == 0 && nanos == 0) {
    buf.append("0s");
    return;
  }
  const bool negative = secs < 0 || nanos < 0;
  if (negative) {
    buf.push_back('-');
    if (secs < 0) secs = -secs;
    if (nanos < 0) nanos = -nanos;
  }
  absl::StrAppend(&buf, secs);
  if (nanos != 0) {
    buf.push_back('.');
    static constexpr int32_t kNanosPerMs = 1000000;
    static constexpr int32_t kNanosPerUs = 1000;
    if (nanos % kNanosPerMs == 0) {
      absl::StrAppend(&buf, absl::StrFormat("%03d", nanos / kNanosPerMs));
    } else if (nanos % kNanosPerUs == 0) {
      absl::StrAppend(&buf, absl::StrFormat("%06d", nanos / kNanosPerUs));
    } else {
      absl::StrAppend(&buf, absl::StrFormat("%09d", nanos));
    }
  }
  buf.push_back('s');
}

// Timestamp `%s` canonical form — RFC3339 with trailing `Z` (UTC).
// Mirrors `cel_timestamp_format_at_v` in `cel_time_parse.cc`.
void AppendTimestampCanonical(std::string& buf, int64_t secs, int32_t nanos) {
  const absl::Time t =
      absl::UnixEpoch() + absl::Seconds(secs) + absl::Nanoseconds(nanos);
  std::string s = absl::FormatTime(absl::RFC3339_full, t, absl::UTCTimeZone());
  constexpr absl::string_view kUtcOffset = "+00:00";
  if (s.size() > kUtcOffset.size() &&
      s.compare(s.size() - kUtcOffset.size(), kUtcOffset.size(), kUtcOffset) ==
          0) {
    s.resize(s.size() - kUtcOffset.size());
    s.push_back('Z');
  }
  buf.append(s);
}

// %s on a list: walk arena list, render each via RenderString,
// comma-space separate, wrap in `[]`.  Returns false on first
// element-level error.
bool AppendListCanonical(std::string& buf, const CelValue* v) {
  if (v->kind != CEL_LIST_ARENA) return false;  // host-list deferred
  buf.push_back('[');
  const auto* hdr = reinterpret_cast<const ArenaListHeader*>(
      cel_mem_base() + v->payload.arena_list.header_ptr);
  for (uint32_t k = 0; k < hdr->count; ++k) {
    if (k > 0) buf.append(", ");
    if (!RenderString(buf, ListElement(hdr, k))) return false;
  }
  buf.push_back(']');
  return true;
}

// %s on a map: cel-cpp builds a btree_map<stringified_key, value>
// so iteration order is lexicographic on the key's string form.
// Allowed key kinds (per cel-cpp's FormatMap): STRING, BOOL, INT,
// UINT.  Everything else errors out.
bool AppendMapCanonical(std::string& buf, const CelValue* v) {
  if (v->kind != CEL_MAP_ARENA) return false;  // host-map deferred
  const auto* hdr = reinterpret_cast<const ArenaMapHeader*>(
      cel_mem_base() + v->payload.arena_map.header_ptr);
  std::map<std::string, const CelValue*> sorted;
  for (uint32_t k = 0; k < hdr->count; ++k) {
    const CelValue* key = MapKey(hdr, k);
    if (key->kind != CEL_STRING && key->kind != CEL_BOOL &&
        key->kind != CEL_INT && key->kind != CEL_UINT) {
      return false;
    }
    std::string key_str;
    if (!RenderString(key_str, key)) return false;
    sorted.emplace(std::move(key_str), MapVal(hdr, k));
  }
  buf.push_back('{');
  bool first = true;
  for (const auto& [key_str, val] : sorted) {
    if (!first) buf.append(", ");
    first = false;
    buf.append(key_str);
    buf.append(": ");
    if (!RenderString(buf, val)) return false;
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
      return AppendListCanonical(buf, v);
    case CEL_MAP_ARENA:
      return AppendMapCanonical(buf, v);
    default:
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
