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
  // String / bytes — equality only.  Ordering on string / bytes is
  // CEL-spec'd but its semantics interact with locale handling we
  // haven't yet decided to admit through the static subset (see
  // m12 string-ext rejection rows); planned under m30.D.
  for (const CelType& lex : {CelType::String(), CelType::Bytes()}) {
    const std::string tag = TypeKey(lex);
    b.Binary(CelType::Bool(), tag + "_eq", "(%0 == %1)", lex, lex);
    b.Binary(CelType::Bool(), tag + "_ne", "(%0 != %1)", lex, lex);
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

  // int(uint) / uint(int) / int(double) / uint(double) / int(string)
  // / uint(string) are partial — they can range-fail.  Deliberately
  // omitted here; planned under m30.B with literal-bounded
  // sources.
}

}  // namespace

void RegisterScalarProductions(GrammarBuilder& b) {
  RegisterNumericLeaves(b);
  RegisterLexicalLeaves(b);
  RegisterIdentLeaves(b, ActivationSchema());
  RegisterArithmetic(b);
  RegisterFallibleArithmetic(b);
  RegisterStringFunctions(b);
  RegisterBoolProducers(b);
  RegisterMixedTotalOps(b);
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
