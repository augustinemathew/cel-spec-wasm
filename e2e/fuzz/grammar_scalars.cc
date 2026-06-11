#include "e2e/fuzz/grammar_scalars.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

namespace {

// Register one Leaf production per (name, type) pair in the
// activation — these are the kIdent leaves the generator can
// emit at any depth.
void RegisterIdentLeaves(GrammarBuilder& b,
                         const std::vector<ActivationBinding>& activation) {
  for (const ActivationBinding& v : activation) {
    b.Leaf(v.type, /*name=*/v.name + "_ident", /*format=*/v.name);
  }
}

}  // namespace

// ── Activation ───────────────────────────────────────────────────

std::vector<ActivationBinding> ActivationSchema() {
  // Two int vars so binary arithmetic on idents has a non-trivial
  // shape; one of each other scalar.  No collision-prone single-
  // letter names — every name carries its type tag, which makes
  // the generated CEL source self-describing in failure messages.
  //
  // `xs` / `ms` are the bound-aggregate entries from the m27
  // vocabulary table: an activation-bound list/map reaches the
  // host-origin aggregate paths (`cel_list_in` over bound data,
  // host map lookup) that literal aggregates — arena-built every
  // eval — never touch.  Their ident leaves feed every existing
  // list<int> / map<string,int> production (size, _in_,
  // comprehension ranges) with no new grammar rules.
  return {
      {"i_a", CelType::Int()},
      {"i_b", CelType::Int()},
      {"u_a", CelType::Uint()},
      {"d_a", CelType::Double()},
      {"b_a", CelType::Bool()},
      {"s_a", CelType::String()},
      {"y_a", CelType::Bytes()},
      {"xs", CelType::List(CelType::Int())},
      {"ms", CelType::Map(CelType::String(), CelType::Int())},
  };
}

// ── Catalog ──────────────────────────────────────────────────────
// One registration helper per production family, mirroring the
// section structure of the m27 design doc; `RegisterScalarProductions`
// is the ordered composition.

namespace {

void RegisterNumericLeaves(GrammarBuilder& b) {
  // Bool.
  b.Leaf(CelType::Bool(), "bool_true", "true");
  b.Leaf(CelType::Bool(), "bool_false", "false");

  // Int — small values keep most arithmetic chains value-producing;
  // the boundary leaves below deliberately push chains into the
  // overflow regime.  Overflow is a COMPARED dimension (both-error
  // = agreement, our-value-while-oracle-errors = ERROR-DIVERGE),
  // so these are safe to admit — see m30-fuzz-full-dialect.md
  // §M30.A.
  b.Leaf(CelType::Int(), "int_zero", "0");
  b.Leaf(CelType::Int(), "int_one", "1");
  b.Leaf(CelType::Int(), "int_neg_one", "(-1)");
  b.Leaf(CelType::Int(), "int_seven", "7");
  b.Leaf(CelType::Int(), "int_max", "9223372036854775807");
  b.Leaf(CelType::Int(), "int_neg_max", "(-9223372036854775807)");
  // 2^53 ± 1 — the largest ints a double can hold exactly; the
  // lossy int↔double boundary (cf. KnownBugs.MapKeyLossyDoubleEquality).
  b.Leaf(CelType::Int(), "int_2p53", "9007199254740992");
  b.Leaf(CelType::Int(), "int_2p53_plus1", "9007199254740993");
  b.Leaf(CelType::Int(), "int_2p32", "4294967296");

  // Uint — boundary values included for the same reason as int.
  b.Leaf(CelType::Uint(), "uint_zero", "0u");
  b.Leaf(CelType::Uint(), "uint_one", "1u");
  b.Leaf(CelType::Uint(), "uint_seven", "7u");
  b.Leaf(CelType::Uint(), "uint_max", "18446744073709551615u");
  b.Leaf(CelType::Uint(), "uint_2p63", "9223372036854775808u");
  b.Leaf(CelType::Uint(), "uint_2p32_minus1", "4294967295u");

  // Double — fractional values give conversions a non-trivial
  // round-trip target; the boundary set covers signed zero, machine
  // epsilon, the smallest denormal, near-overflow magnitudes (one
  // multiplication from ±inf — a VALUE in CEL, not an error), and
  // the 2^53 exactness limit.
  b.Leaf(CelType::Double(), "double_zero", "0.0");
  b.Leaf(CelType::Double(), "double_one", "1.0");
  b.Leaf(CelType::Double(), "double_neg_one", "(-1.0)");
  b.Leaf(CelType::Double(), "double_half", "0.5");
  b.Leaf(CelType::Double(), "double_neg_zero", "(-0.0)");
  b.Leaf(CelType::Double(), "double_epsilon", "2.220446049250313e-16");
  b.Leaf(CelType::Double(), "double_denorm_min", "5e-324");
  b.Leaf(CelType::Double(), "double_near_max", "1e308");
  b.Leaf(CelType::Double(), "double_neg_near_max", "(-1e308)");
  b.Leaf(CelType::Double(), "double_2p53", "9007199254740992.0");
}

void RegisterLexicalLeaves(GrammarBuilder& b) {
  // String — ASCII plus the UTF-8 width classes (2/3/4-byte), an
  // embedded NUL (the classic C-kernel killer), and a combining
  // mark (é as e + U+0301 — two codepoints, one grapheme).  The
  // size()/indexOf codepoint-vs-byte bug family lives here.
  b.Leaf(CelType::String(), "string_empty", R"("")");
  b.Leaf(CelType::String(), "string_x", R"("x")");
  b.Leaf(CelType::String(), "string_hello", R"("hello")");
  b.Leaf(CelType::String(), "string_2byte", R"("ÿ")");
  b.Leaf(CelType::String(), "string_multibyte", R"("πέντε")");
  b.Leaf(CelType::String(), "string_4byte", R"("💜")");
  b.Leaf(CelType::String(), "string_embedded_nul", R"("a\u0000b")");
  b.Leaf(CelType::String(), "string_combining", R"("é")");

  // Bytes — same shape as string, plus a NUL byte and a sequence
  // that is NOT valid UTF-8 (legal for bytes; lethal for any code
  // path that assumes bytes are stringly).
  b.Leaf(CelType::Bytes(), "bytes_empty", R"(b"")");
  b.Leaf(CelType::Bytes(), "bytes_x", R"(b"x")");
  b.Leaf(CelType::Bytes(), "bytes_nul", R"(b"\x00")");
  b.Leaf(CelType::Bytes(), "bytes_invalid_utf8", R"(b"\xff\xfe")");
}

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

// Scalar-returning string functions (receiver form).  These are
// core dialect the grammar lacked — and the territory where the
// wave-4 codepoint-vs-byte bug cluster was found by hand
// (`IndexOfPosBoundIsByteNotCodepoint`, the size() unicode rows),
// and where the M30.D `split` slot-aliasing miscompile lived.
//
// Total over the typed inputs:
//   - contains/startsWith/endsWith → Bool, always defined
//   - indexOf(sub)/lastIndexOf(sub) → Int, returns -1 when absent
//   - matches(re)       → Bool; the pattern is a generated string,
//     but the leaf alphabet has no regex metacharacters that form
//     an invalid pattern, so it's total in practice (error-ness is
//     compared if that ever changes)
//   - replace(old, new) → String, total
// Fallible (range error — reachable via the boundary int leaves;
// error-ness is a compared dimension, so both-error is agreement):
//   - substring(start) / substring(start, end) → String
// `split` lives in the aggregate catalog (it yields list<string>).
void RegisterStringFunctions(GrammarBuilder& b) {
  b.Binary(CelType::Bool(), "string_contains", "(%0).contains(%1)",
           CelType::String(), CelType::String());
  b.Binary(CelType::Bool(), "string_starts_with", "(%0).startsWith(%1)",
           CelType::String(), CelType::String());
  b.Binary(CelType::Bool(), "string_ends_with", "(%0).endsWith(%1)",
           CelType::String(), CelType::String());
  b.Binary(CelType::Int(), "string_index_of", "(%0).indexOf(%1)",
           CelType::String(), CelType::String());
  b.Binary(CelType::Int(), "string_last_index_of", "(%0).lastIndexOf(%1)",
           CelType::String(), CelType::String());
  b.Binary(CelType::Bool(), "string_matches", "(%0).matches(%1)",
           CelType::String(), CelType::String());
  b.Ternary(CelType::String(), "string_replace", "(%0).replace(%1, %2)",
            CelType::String(), CelType::String(), CelType::String());
  b.Binary(CelType::String(), "string_substring_1", "(%0).substring(%1)",
           CelType::String(), CelType::Int());
  b.Ternary(CelType::String(), "string_substring_2", "(%0).substring(%1, %2)",
            CelType::String(), CelType::Int(), CelType::Int());
  // String → String transforms.  `reverse`/`charAt` are
  // codepoint-handling — the exact territory of the byte-vs-codepoint
  // bug family — and (like `split`) a COMPUTED receiver exercises a
  // different arena path than a literal, so mining over concatenated
  // receivers is the load-bearing case.  `charAt(int)` is fallible
  // (range error via the boundary int leaves; error-compared).
  b.Binary(CelType::String(), "string_char_at", "(%0).charAt(%1)",
           CelType::String(), CelType::Int());
  b.Unary(CelType::String(), "string_lower_ascii", "(%0).lowerAscii()",
          CelType::String());
  b.Unary(CelType::String(), "string_upper_ascii", "(%0).upperAscii()",
          CelType::String());
  b.Unary(CelType::String(), "string_trim", "(%0).trim()", CelType::String());
  b.Unary(CelType::String(), "string_reverse", "(%0).reverse()",
          CelType::String());
  b.Unary(CelType::String(), "strings_quote", "strings.quote(%0)",
          CelType::String());
  // encoders extension — base64.  `encode(bytes)` → String (total);
  // `decode(string)` → Bytes (fallible: non-base64 input errors, so
  // the non-base64 string leaves both-error against cel-cpp).
  b.Unary(CelType::String(), "base64_encode", "base64.encode(%0)",
          CelType::Bytes());
  b.Unary(CelType::Bytes(), "base64_decode", "base64.decode(%0)",
          CelType::String());
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

// Timestamp / duration — leaves, accessors, comparisons, and the
// int/string conversions.  All standard library (no oracle
// extension needed).  Accessors return Int; comparisons Bool;
// `string(...)` String; `int(...)` Int.  The `_with_tz` accessor
// variants (timezone-string second arg) are deferred — they need a
// tz-string leaf set.  The max-range timestamp
// (`9999-12-31T23:59:59.999999999Z`) is left out of the leaves
// until `MaxRangeTimestampConstruction` (known_bugs) is fixed.
void RegisterTemporal(GrammarBuilder& b) {
  const CelType ts = CelType::Timestamp();
  const CelType dur = CelType::Duration();
  const CelType i = CelType::Int();
  // Leaves — a spread of valid instants / durations.
  b.Leaf(ts, "ts_epoch", R"(timestamp("1970-01-01T00:00:00Z"))");
  b.Leaf(ts, "ts_mid", R"(timestamp("2024-03-15T13:45:30Z"))");
  b.Leaf(ts, "ts_leap", R"(timestamp("2000-02-29T23:59:59Z"))");
  b.Leaf(dur, "dur_zero", R"(duration("0s"))");
  b.Leaf(dur, "dur_hour", R"(duration("3661s"))");
  b.Leaf(dur, "dur_neg", R"(duration("-90s"))");
  // Timestamp accessors → Int (no-tz forms).
  for (const char* m : {"getFullYear", "getMonth", "getDayOfMonth", "getDate",
                        "getDayOfWeek", "getDayOfYear", "getHours",
                        "getMinutes", "getSeconds", "getMilliseconds"}) {
    b.Unary(i, std::string("ts_") + m, std::string("(%0).") + m + "()", ts);
  }
  // Duration accessors → Int.
  for (const char* m :
       {"getHours", "getMinutes", "getSeconds", "getMilliseconds"}) {
    b.Unary(i, std::string("dur_") + m, std::string("(%0).") + m + "()", dur);
  }
  // Comparisons (eq/ne/lt/le/gt/ge) → Bool, same-type.
  for (const CelType& t : {ts, dur}) {
    const std::string tag = TypeKey(t);
    b.Binary(CelType::Bool(), tag + "_eq", "(%0 == %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_ne", "(%0 != %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_lt", "(%0 < %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_le", "(%0 <= %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_gt", "(%0 > %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_ge", "(%0 >= %1)", t, t);
  }
  // Conversions.  `int(timestamp)` is standard (unix seconds);
  // `int(duration)` is NOT a cel-cpp overload (the fuzzer found we
  // wrongly accept it — pinned as PbtIntOfDurationOverPermissive in
  // known_bugs), so it is deliberately absent here to keep the
  // grammar emitting only conformant CEL.
  b.Unary(i, "int_from_timestamp", "int(%0)", ts);
  b.Unary(CelType::String(), "string_from_timestamp", "string(%0)", ts);
  b.Unary(CelType::String(), "string_from_duration", "string(%0)", dur);
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

}  // namespace

void RegisterScalarProductions(GrammarBuilder& b) {
  RegisterNumericLeaves(b);
  RegisterLexicalLeaves(b);
  RegisterIdentLeaves(b, ActivationSchema());
  RegisterArithmetic(b);
  RegisterFallibleArithmetic(b);
  RegisterStringFunctions(b);
  RegisterMathExt(b);
  RegisterTemporal(b);
  RegisterBoolProducers(b);
  RegisterMixedTotalOps(b);
  RegisterConversions(b);
}

Grammar BuildScalarGrammar() {
  GrammarBuilder b;
  RegisterScalarProductions(b);
  Grammar g = std::move(b).Build();
  // L1 — Grammar::Validate() must accept the catalog the test
  // binary just constructed.  If this fires, the catalog has a
  // structural bug (missing leaf, mismatched placeholder, etc.)
  // and no PBT iterations should run against it.
  ABSL_CHECK_OK(g.Validate())
      << "scalar grammar failed L1 validation; the catalog in "
         "grammar_scalars.cc is malformed";
  return g;
}

}  // namespace celwasm::fuzz
