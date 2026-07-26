#include "e2e/fuzz/catalog.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// String functions: receiver-form scalar returns, strings.format,
// and the string â list<string> bridges (split / join).

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
  // The two-arg substring(start, end) is withheld: cel-cpp errors on
  // every `end == size()` slice (a SubstringImpl off-by-one — the
  // end-index check only fires inside the codepoint loop, which the
  // full-length end never reaches), while we return the value.  The
  // grammar can't avoid generating end == size, so this production
  // would diverge nondeterministically.  Pinned as
  // `PbtSubstringEndEqualsSizeOverPermissive` in known_bugs_test.cc.
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

// strings.format — `"<directives>".format([args])`.  format's
// parameter is an explicit `list(dyn)`, which the static subset
// admits (RejectDyn permits explicit-dyn params), so heterogeneous
// arg lists type-check.  Each production pairs a directive with the
// matching arg type — the type-MISMATCHED combos (`%f` of int /
// string) are deliberately absent: cel-cpp errors on them, we
// don't, and that's already pinned (FormatFixedRejectsInt /
// FormatFixedAcceptsNanToken).  Format strings never contain `%0`-
// `%9` (which would collide with the grammar's `%i` placeholders).
void RegisterStringFormat(GrammarBuilder& b) {
  const CelType s = CelType::String();
  const CelType i = CelType::Int();
  // Single-directive, directive matches arg type.
  b.Unary(s, "fmt_d_int", R"("%d".format([%0]))", i);
  b.Unary(s, "fmt_x_int", R"("%x".format([%0]))", i);
  b.Unary(s, "fmt_o_int", R"("%o".format([%0]))", i);
  b.Unary(s, "fmt_d_uint", R"("%d".format([%0]))", CelType::Uint());
  b.Unary(s, "fmt_s_string", R"("%s".format([%0]))", s);
  b.Unary(s, "fmt_b_bool", R"("%b".format([%0]))", CelType::Bool());
  b.Unary(s, "fmt_f_double", R"("%f".format([%0]))", CelType::Double());
  b.Unary(s, "fmt_e_double", R"("%e".format([%0]))", CelType::Double());
  // Surrounding literal text + the `%%` escape.
  b.Unary(s, "fmt_bracket_int", R"("[%d]".format([%0]))", i);
  b.Unary(s, "fmt_pct_int", R"("%d%%".format([%0]))", i);
  // Two-directive, two-arg (the heterogeneous arg list).
  b.Binary(s, "fmt_d_s", R"("%d %s".format([%0, %1]))", i, s);
  b.Binary(s, "fmt_s_d", R"("%s=%d".format([%0, %1]))", s, i);
}

// String functions that bridge string ↔ list<string>: `split`
// (string → list<string>) and `join` (list<string> → string).
// Total over their typed inputs (an empty separator splits into
// characters; join concatenates).  The scalar-returning string
// functions (contains/indexOf/matches/…) live in the scalar
// catalog.
void RegisterStringAggregateFunctions(GrammarBuilder& b) {
  const CelType ls = CelType::List(CelType::String());
  b.Binary(ls, "string_split", "(%0).split(%1)", CelType::String(),
           CelType::String());
  b.Unary(CelType::String(), "list_join", "(%0).join()", ls);
  b.Binary(CelType::String(), "list_join_sep", "(%0).join(%1)", ls,
           CelType::String());
}

}  // namespace celwasm::fuzz
