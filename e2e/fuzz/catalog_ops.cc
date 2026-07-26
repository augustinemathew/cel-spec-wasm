#include "e2e/fuzz/catalog.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// Operators over scalars: arithmetic (total + fallible),
// math_ext, comparisons + logic, concat/ternary/size, and the
// conversion family.

// Arithmetic (+, -, *) — no / or % per the safety policy.
void RegisterArithmetic(GrammarBuilder& b) {
  // Int: unary neg + 3 binaries.
  b.Unary(CelType::Int(), "int_neg", "(-%0)", CelType::Int());
  b.Binary(CelType::Int(), "int_add", "(%0 + %1)", CelType::Int(),
           CelType::Int());
  b.Binary(CelType::Int(), "int_sub", "(%0 - %1)", CelType::Int(),
           CelType::Int());
  b.Binary(CelType::Int(), "int_mul", "(%0 * %1)", CelType::Int(),
           CelType::Int());

  // Uint: no unary neg (would produce a negative result, undefined
  // for uint), no subtraction (would underflow on `0u - 1u`).  Only
  // add + mul — both safe with small constants and short chains.
  b.Binary(CelType::Uint(), "uint_add", "(%0 + %1)", CelType::Uint(),
           CelType::Uint());
  b.Binary(CelType::Uint(), "uint_mul", "(%0 * %1)", CelType::Uint(),
           CelType::Uint());

  // Double: full arithmetic.  Overflow produces +/-inf, which is
  // a valid double value (not an error in CEL), so all four are
  // total.
  b.Unary(CelType::Double(), "double_neg", "(-%0)", CelType::Double());
  b.Binary(CelType::Double(), "double_add", "(%0 + %1)", CelType::Double(),
           CelType::Double());
  b.Binary(CelType::Double(), "double_sub", "(%0 - %1)", CelType::Double(),
           CelType::Double());
  b.Binary(CelType::Double(), "double_mul", "(%0 * %1)", CelType::Double(),
           CelType::Double());
}

// Fallible integer arithmetic — `/` and `%` over int/uint.  These
// ERROR on a zero divisor (and int `/` errors on INT64_MIN / -1
// overflow), and a zero divisor is reachable: the leaf set has
// `0` / `0u`, and a sub-expression can compute zero.  Admitting
// them is the point of m30.B — an error flowing through
// comprehensions and 3VL logic is the bug class the guarded-total
// grammar could never reach (cf. `ExistsAbsorbsErrorAccumulator`).
// Safe to admit now that error-ness is a compared dimension: both
// engines erroring is agreement; only one erroring is a find.
//
// (Double `/` is NOT here — `x / 0.0` is ±inf/NaN, a VALUE in CEL,
// so it stays with the total double ops above.)
void RegisterFallibleArithmetic(GrammarBuilder& b) {
  b.Binary(CelType::Int(), "int_div", "(%0 / %1)", CelType::Int(),
           CelType::Int());
  b.Binary(CelType::Int(), "int_mod", "(%0 % %1)", CelType::Int(),
           CelType::Int());
  b.Binary(CelType::Uint(), "uint_div", "(%0 / %1)", CelType::Uint(),
           CelType::Uint());
  b.Binary(CelType::Uint(), "uint_mod", "(%0 % %1)", CelType::Uint(),
           CelType::Uint());
}

// math extension (`math.*`).  Namespace-qualified functions; the
// boundary numeric leaves (M30.A) make these bug-rich: abs(INT64_MIN)
// overflows, bit shifts past 63 error, sqrt of a negative is NaN
// (a value), round/trunc on the 2^53 boundary.  Grouped by return
// type.  Fallible members (abs-int overflow, bitShift range) are
// safe to admit — error-ness is a compared dimension.
void RegisterMathExt(GrammarBuilder& b) {
  const CelType i = CelType::Int();
  const CelType u = CelType::Uint();
  const CelType d = CelType::Double();
  // → Int (from int args).
  b.Unary(i, "math_abs_int", "math.abs(%0)", i);
  b.Unary(i, "math_sign_int", "math.sign(%0)", i);
  b.Binary(i, "math_bitand_int", "math.bitAnd(%0, %1)", i, i);
  b.Binary(i, "math_bitor_int", "math.bitOr(%0, %1)", i, i);
  b.Binary(i, "math_bitxor_int", "math.bitXor(%0, %1)", i, i);
  b.Unary(i, "math_bitnot_int", "math.bitNot(%0)", i);
  b.Binary(i, "math_bitshl_int", "math.bitShiftLeft(%0, %1)", i, i);
  b.Binary(i, "math_bitshr_int", "math.bitShiftRight(%0, %1)", i, i);
  // → Uint.
  b.Unary(u, "math_abs_uint", "math.abs(%0)", u);
  b.Unary(u, "math_sign_uint", "math.sign(%0)", u);
  b.Binary(u, "math_bitand_uint", "math.bitAnd(%0, %1)", u, u);
  b.Binary(u, "math_bitor_uint", "math.bitOr(%0, %1)", u, u);
  b.Binary(u, "math_bitxor_uint", "math.bitXor(%0, %1)", u, u);
  b.Unary(u, "math_bitnot_uint", "math.bitNot(%0)", u);
  b.Binary(u, "math_bitshl_uint", "math.bitShiftLeft(%0, %1)", u, i);
  b.Binary(u, "math_bitshr_uint", "math.bitShiftRight(%0, %1)", u, i);
  // → Double.
  b.Unary(d, "math_abs_double", "math.abs(%0)", d);
  b.Unary(d, "math_sign_double", "math.sign(%0)", d);
  b.Unary(d, "math_ceil_double", "math.ceil(%0)", d);
  b.Unary(d, "math_floor_double", "math.floor(%0)", d);
  b.Unary(d, "math_round_double", "math.round(%0)", d);
  b.Unary(d, "math_trunc_double", "math.trunc(%0)", d);
  b.Unary(d, "math_sqrt_int", "math.sqrt(%0)", i);
  b.Unary(d, "math_sqrt_uint", "math.sqrt(%0)", u);
  b.Unary(d, "math_sqrt_double", "math.sqrt(%0)", d);
  // → Bool.
  b.Unary(CelType::Bool(), "math_is_finite", "math.isFinite(%0)", d);
  b.Unary(CelType::Bool(), "math_is_inf", "math.isInf(%0)", d);
  b.Unary(CelType::Bool(), "math_is_nan", "math.isNaN(%0)", d);
}

// Comparison + logical (both yield Bool) — every CEL-spec overload.
void RegisterBoolProducers(GrammarBuilder& b) {
  for (const CelType& numeric :
       {CelType::Int(), CelType::Uint(), CelType::Double()}) {
    const std::string tag = TypeKey(numeric);
    b.Binary(CelType::Bool(), tag + "_eq", "(%0 == %1)", numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_ne", "(%0 != %1)", numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_lt", "(%0 < %1)", numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_le", "(%0 <= %1)", numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_gt", "(%0 > %1)", numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_ge", "(%0 >= %1)", numeric, numeric);
  }
  // String / bytes — full comparison set.  Ordering is bytewise
  // (lexicographic over the UTF-8 / raw bytes) in both cel-cpp and
  // our runtime; it's in the static subset (`'a' < 'b'`
  // type-checks), so the earlier eq/ne-only restriction was overly
  // cautious.  The multi-byte / embedded-NUL string leaves make the
  // ordering bug-prone (a byte-vs-codepoint comparison would
  // diverge).
  for (const CelType& lex : {CelType::String(), CelType::Bytes()}) {
    const std::string tag = TypeKey(lex);
    b.Binary(CelType::Bool(), tag + "_eq", "(%0 == %1)", lex, lex);
    b.Binary(CelType::Bool(), tag + "_ne", "(%0 != %1)", lex, lex);
    b.Binary(CelType::Bool(), tag + "_lt", "(%0 < %1)", lex, lex);
    b.Binary(CelType::Bool(), tag + "_le", "(%0 <= %1)", lex, lex);
    b.Binary(CelType::Bool(), tag + "_gt", "(%0 > %1)", lex, lex);
    b.Binary(CelType::Bool(), tag + "_ge", "(%0 >= %1)", lex, lex);
  }
  // Bool equality only — there's no ordering on bool in CEL.
  b.Binary(CelType::Bool(), "bool_eq", "(%0 == %1)", CelType::Bool(),
           CelType::Bool());
  b.Binary(CelType::Bool(), "bool_ne", "(%0 != %1)", CelType::Bool(),
           CelType::Bool());

  // ── Logical ───────────────────────────────────────────────────
  b.Unary(CelType::Bool(), "bool_not", "(!%0)", CelType::Bool());
  b.Binary(CelType::Bool(), "bool_and", "(%0 && %1)", CelType::Bool(),
           CelType::Bool());
  b.Binary(CelType::Bool(), "bool_or", "(%0 || %1)", CelType::Bool(),
           CelType::Bool());
}

// Concat / ternary / size / total conversions.
void RegisterMixedTotalOps(GrammarBuilder& b) {
  // ── Concat — total over the typed input domain ───────────────
  b.Binary(CelType::String(), "string_concat", "(%0 + %1)", CelType::String(),
           CelType::String());
  b.Binary(CelType::Bytes(), "bytes_concat", "(%0 + %1)", CelType::Bytes(),
           CelType::Bytes());

  // ── Ternary — one rule per scalar result type ────────────────
  for (const CelType& t :
       {CelType::Bool(), CelType::Int(), CelType::Uint(), CelType::Double(),
        CelType::String(), CelType::Bytes()}) {
    b.Ternary(t, TypeKey(t) + "_ternary", "(%0 ? %1 : %2)", CelType::Bool(), t,
              t);
  }

  // ── size on lexicographic scalars ─────────────────────────────
  b.Unary(CelType::Int(), "size_string", "size(%0)", CelType::String());
  b.Unary(CelType::Int(), "size_bytes", "size(%0)", CelType::Bytes());

  // ── Total type conversions ────────────────────────────────────
  // double(int) and double(uint) are total: every int and every
  // uint round-trips to a representable double (with possible loss
  // of precision past 2^53, but no domain error).
  b.Unary(CelType::Double(), "double_from_int", "double(%0)", CelType::Int());
  b.Unary(CelType::Double(), "double_from_uint", "double(%0)", CelType::Uint());

  // double(int) and double(uint) above are the only TOTAL numeric
  // conversions; the rest live in RegisterConversions (many are
  // fallible — range/parse errors — but error-ness is compared).
}

// The conversion family beyond `double(int|uint)`.  Cross-numeric
// (`int(uint)` / `uint(int)` / `int(double)` / `uint(double)`),
// `string(x)`, `bytes(string)`, and the fallible string→numeric /
// string→bool parses.  Range and parse failures are reachable (the
// boundary leaves overflow int(double); the non-numeric string
// leaves fail int(string)), but error-ness is a compared dimension
// so both-error is agreement.  NB: `int(duration)` is deliberately
// NOT here — cel-cpp rejects it (PbtIntOfDurationOverPermissive).
void RegisterConversions(GrammarBuilder& b) {
  const CelType i = CelType::Int();
  const CelType u = CelType::Uint();
  const CelType d = CelType::Double();
  const CelType s = CelType::String();
  // → Int.
  b.Unary(i, "int_from_uint", "int(%0)", u);
  b.Unary(i, "int_from_double", "int(%0)", d);
  b.Unary(i, "int_from_string", "int(%0)", s);
  // → Uint.
  b.Unary(u, "uint_from_int", "uint(%0)", i);
  b.Unary(u, "uint_from_double", "uint(%0)", d);
  b.Unary(u, "uint_from_string", "uint(%0)", s);
  // → Double.
  b.Unary(d, "double_from_string", "double(%0)", s);
  // → String.  NB: `string(double)` is deliberately ABSENT — the
  // oracle cannot validate double→string (its cel-cpp build lacks
  // <charconv> double-to-chars, so it falls back to `%.17g`
  // full-precision, e.g. `string(3.14)` → "3.1400000000000001"
  // instead of the conformant shortest "3.14").  Worse, our own
  // wasm libc++ `to_chars(general)` emits scientific where
  // conformant `to_chars` gives fixed (`string(4294967295.0)` →
  // "4.294967295e+09" vs "4294967295") — a real bug pinned as
  // PbtStringDoubleScientificForm.  Double formatting is pinned
  // directly in known_bugs (DoubleToString*), not fuzzed.
  b.Unary(s, "string_from_int", "string(%0)", i);
  b.Unary(s, "string_from_uint", "string(%0)", u);
  b.Unary(s, "string_from_bool", "string(%0)", CelType::Bool());
  b.Unary(s, "string_from_bytes", "string(%0)", CelType::Bytes());
  // → Bytes.
  b.Unary(CelType::Bytes(), "bytes_from_string", "bytes(%0)", s);
  // → Bool.
  b.Unary(CelType::Bool(), "bool_from_string", "bool(%0)", s);
}

}  // namespace celwasm::fuzz
