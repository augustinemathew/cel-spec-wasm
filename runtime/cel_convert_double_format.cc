// `cel_double_to_string_at_v` — formats a CEL double into its
// canonical decimal string form.
//
// Carved out of `cel_convert.c` (which is C11) into a sibling C++ TU
// so we can use `std::to_chars(double, chars_format::general)` from
// libc++.  `to_chars` produces the **shortest decimal representation
// that round-trips to the exact double** (Ryu / Grisu shape) — the
// hand-rolled per-digit `frac *= 10` loop the old C path used
// accumulated rounding error past ~6 fractional digits and produced
// `"123.45600000000000306"` for `string(123.456)`, breaking the
// `conversions/string/double` corpus row.
//
// This mirrors cel-cpp's `FormatDouble` in
// `third_party/cel-cpp/runtime/standard/type_conversion_functions.cc:56`,
// which delegates to the same `std::to_chars` overload when
// `<charconv>` is available (the wasi-sdk libc++ ships it).  The
// wider catalogue of conversions stays in `cel_convert.c` as C; only
// the double→string kernel needs C++.
//
// The 3VL absorb + kind check + arena allocation contracts are
// identical to every other `cel_*_at_v` helper — see `cel_convert.h`
// §"Semantics" for the spec.

#include <charconv>
#include <cstdint>
#include <cstring>
#include <system_error>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_log.h"

namespace {

// True iff `v` is NaN.  IEEE-754 makes NaN the only value that
// compares unequal to itself.  Mirrors `is_nan` in `cel_convert.c`
// (the freestanding wasm32 runtime build links no <math.h>).
inline bool IsNan(double v) {
  return v != v;
}

// Arena-allocate `len` bytes and stamp the slot to
// `{CEL_STRING, payload.s = {off, len}}`.  Returns false (and poisons
// with `CEL_ERR_OVERFLOW`) on arena OOM.  Mirrors `stamp_string` in
// `cel_convert.c` so the kernel's contract matches every other
// arena-output kernel.
bool StampString(CelValue* out, const char* src, uint32_t len) {
  uint32_t off = arena_alloc(len);
  if (off == 0 && len > 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return false;
  }
  uint8_t* dst = cel_memory_base_() + off;
  std::memcpy(dst, src, len);
  out->kind = CEL_STRING;
  out->_pad = 0;
  out->payload.s.ptr = off;
  out->payload.s.len = len;
  return true;
}

// Handle NaN / +Inf / -Inf.  Returns true if a special was written;
// false if `v` is a regular value (caller continues).  Strings match
// the historical surface (`"nan"` / `"+Inf"` / `"-Inf"`) — the
// conformance corpus does not pin these (langdef §"JSON conversion"
// specifies `"Infinity"` / `"NaN"` only for JSON, not for `string()`)
// so we keep the established shape rather than churn callers.
bool HandleSpecial(CelValue* out, double v) {
  if (IsNan(v)) {
    return StampString(out, "nan", 3);
  }
  // __builtin_inf() is constant-folded and avoids pulling in <cmath>
  // / <math.h>, neither of which the freestanding wasm32 build links.
  const double kPosInf = __builtin_inf();
  if (v == kPosInf) {
    return StampString(out, "+Inf", 4);
  }
  if (v == -kPosInf) {
    return StampString(out, "-Inf", 4);
  }
  // ±0.0.  `std::to_chars(-0.0, general)` produces `"-0"`, but the
  // historical surface (and our existing test pins) treats -0.0 as
  // numerically equal to 0.0 and emits the unsigned `"0"`.  CEL spec
  // does not distinguish, and `0.0 == -0.0` per IEEE-754 holds in
  // CEL's `==` (langdef §"Equality").  Special-case so we do not
  // regress the existing surface.
  if (v == 0.0) {
    return StampString(out, "0", 1);
  }
  return false;
}

}  // namespace

extern "C" void cel_double_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const double v = a->payload.d;
  if (HandleSpecial(out, v)) return;

  // `std::to_chars(double, general)` produces the shortest decimal
  // representation that round-trips to the exact double.  32 bytes is
  // ample: the worst case is a 17-significant-digit shortest-round-
  // trip mantissa plus sign, decimal point, `e`, sign, and 3-digit
  // exponent (≈25 bytes); 32 leaves margin.  cel-cpp's `FormatDouble`
  // uses the same `kBufSize = 32`.
  constexpr int kBufSize = 32;
  char buf[kBufSize];
  std::to_chars_result result =
      std::to_chars(buf, buf + kBufSize, v, std::chars_format::general);
  if (result.ec != std::errc()) {
    // Defensive: `to_chars(double, general)` into a 32-byte buffer
    // cannot return `value_too_large` for any finite IEEE-754 double
    // (the longest shortest-round-trip form is ~24 chars).  If we
    // ever do see one, surface it as an overflow error rather than
    // silently writing garbage.
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  auto len = static_cast<uint32_t>(result.ptr - buf);
  (void)StampString(out, buf, len);
}
