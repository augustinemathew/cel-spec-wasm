#include "e2e/fuzz/catalog.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// Adversarial scalar leaf values — the boundary numerics and the
// unicode/NUL lexical set.  Every leaf carries its rationale.

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
  // Exact INT64_MIN — can't be a plain literal (the magnitude
  // 9223372036854775808 overflows int64 at parse time), so build it by
  // subtraction.  This is the two's-complement asymmetry boundary:
  // negate / abs / `* -1` / `/ -1` / `% -1` / `- 1` all overflow here,
  // and cel-cpp errors on each — both-error agreement (mining
  // surfaced the one case we got wrong, `% -1`, now fixed).
  b.Leaf(CelType::Int(), "int_min", "(-9223372036854775807 - 1)");
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

  // Numeric-shaped strings — these light up the SUCCESS path of the
  // string-parse conversions (`int`/`uint`/`double`/`bool` of string),
  // which the non-numeric leaves above only ever drive into the
  // both-error branch.  Only oracle-agreeing forms are admitted: a
  // plain int, a plain double, and a negative int.  The
  // whitespace-padded (`"  3.14  "`) and leading-`+` (`"+5"`) forms are
  // deliberately withheld — they trip our over-permissive parse vs
  // cel-cpp (pinned `DoubleFromStringRejectsWhitespace` /
  // `IntFromStringLeadingPlus` / `UintFromStringLeadingPlus`).
  b.Leaf(CelType::String(), "string_num_int", R"("42")");
  b.Leaf(CelType::String(), "string_num_double", R"("3.14")");
  b.Leaf(CelType::String(), "string_num_neg", R"("-7")");

  // Bytes — same shape as string, plus a NUL byte and a sequence
  // that is NOT valid UTF-8 (legal for bytes; lethal for any code
  // path that assumes bytes are stringly).
  b.Leaf(CelType::Bytes(), "bytes_empty", R"(b"")");
  b.Leaf(CelType::Bytes(), "bytes_x", R"(b"x")");
  b.Leaf(CelType::Bytes(), "bytes_nul", R"(b"\x00")");
  b.Leaf(CelType::Bytes(), "bytes_invalid_utf8", R"(b"\xff\xfe")");
}

}  // namespace celwasm::fuzz
