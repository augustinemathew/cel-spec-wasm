#include "e2e/fuzz/grammar_slice_b.h"

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

std::vector<ActivationBinding> SliceBActivation() {
  // Two int vars so binary arithmetic on idents has a non-trivial
  // shape; one of each other scalar.  No collision-prone single-
  // letter names — every name carries its type tag, which makes
  // the generated CEL source self-describing in failure messages.
  return {
      {"i_a", CelType::Int()},
      {"i_b", CelType::Int()},
      {"u_a", CelType::Uint()},
      {"d_a", CelType::Double()},
      {"b_a", CelType::Bool()},
      {"s_a", CelType::String()},
      {"y_a", CelType::Bytes()},
  };
}

// ── Catalog ──────────────────────────────────────────────────────

Grammar BuildSliceBGrammar() {
  GrammarBuilder b;

  // ── Constant leaves ────────────────────────────────────────────
  // Bool.
  b.Leaf(CelType::Bool(), "bool_true",  "true");
  b.Leaf(CelType::Bool(), "bool_false", "false");

  // Int — small distinct values so depth-N arithmetic chains can't
  // overflow even at max depth.
  b.Leaf(CelType::Int(), "int_zero",    "0");
  b.Leaf(CelType::Int(), "int_one",     "1");
  b.Leaf(CelType::Int(), "int_neg_one", "(-1)");
  b.Leaf(CelType::Int(), "int_seven",   "7");

  // Uint — only non-negative values; no max-value constants since
  // even uint_add of two large uints can overflow under the spec.
  b.Leaf(CelType::Uint(), "uint_zero", "0u");
  b.Leaf(CelType::Uint(), "uint_one",  "1u");
  b.Leaf(CelType::Uint(), "uint_seven", "7u");

  // Double — including a fractional value so type conversions
  // (`double(int)` etc.) have a non-trivial round-trip target.
  b.Leaf(CelType::Double(), "double_zero",    "0.0");
  b.Leaf(CelType::Double(), "double_one",     "1.0");
  b.Leaf(CelType::Double(), "double_neg_one", "(-1.0)");
  b.Leaf(CelType::Double(), "double_half",    "0.5");

  // String — empty, single char, multi-char.  All ASCII; UTF-8
  // edge cases live in m12 e2e and the dedicated unicode tests.
  b.Leaf(CelType::String(), "string_empty", R"("")");
  b.Leaf(CelType::String(), "string_x",     R"("x")");
  b.Leaf(CelType::String(), "string_hello", R"("hello")");

  // Bytes — same shape as string.
  b.Leaf(CelType::Bytes(), "bytes_empty", R"(b"")");
  b.Leaf(CelType::Bytes(), "bytes_x",     R"(b"x")");

  // ── Ident leaves ───────────────────────────────────────────────
  RegisterIdentLeaves(b, SliceBActivation());

  // ── Arithmetic (+, -, *) — no / or % per the safety policy ────
  // Int: unary neg + 3 binaries.
  b.Unary (CelType::Int(), "int_neg", "(-%0)",     CelType::Int());
  b.Binary(CelType::Int(), "int_add", "(%0 + %1)", CelType::Int(), CelType::Int());
  b.Binary(CelType::Int(), "int_sub", "(%0 - %1)", CelType::Int(), CelType::Int());
  b.Binary(CelType::Int(), "int_mul", "(%0 * %1)", CelType::Int(), CelType::Int());

  // Uint: no unary neg (would produce a negative result, undefined
  // for uint), no subtraction (would underflow on `0u - 1u`).  Only
  // add + mul — both safe with small constants and short chains.
  b.Binary(CelType::Uint(), "uint_add", "(%0 + %1)",
           CelType::Uint(), CelType::Uint());
  b.Binary(CelType::Uint(), "uint_mul", "(%0 * %1)",
           CelType::Uint(), CelType::Uint());

  // Double: full arithmetic.  Overflow produces +/-inf, which is
  // a valid double value (not an error in CEL), so all four are
  // total.
  b.Unary (CelType::Double(), "double_neg",
           "(-%0)",     CelType::Double());
  b.Binary(CelType::Double(), "double_add",
           "(%0 + %1)", CelType::Double(), CelType::Double());
  b.Binary(CelType::Double(), "double_sub",
           "(%0 - %1)", CelType::Double(), CelType::Double());
  b.Binary(CelType::Double(), "double_mul",
           "(%0 * %1)", CelType::Double(), CelType::Double());

  // ── Comparison (yields Bool) — every CEL-spec overload ────────
  for (CelType numeric : {CelType::Int(), CelType::Uint(), CelType::Double()}) {
    const std::string tag = TypeKey(numeric);
    b.Binary(CelType::Bool(), tag + "_eq", "(%0 == %1)", numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_ne", "(%0 != %1)", numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_lt", "(%0 < %1)",  numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_le", "(%0 <= %1)", numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_gt", "(%0 > %1)",  numeric, numeric);
    b.Binary(CelType::Bool(), tag + "_ge", "(%0 >= %1)", numeric, numeric);
  }
  // String / bytes — equality only.  Ordering on string / bytes is
  // CEL-spec'd but its semantics interact with locale handling we
  // haven't yet decided to admit through the static subset (see
  // m12 string-ext rejection rows); deferred to Slice C.
  for (CelType lex : {CelType::String(), CelType::Bytes()}) {
    const std::string tag = TypeKey(lex);
    b.Binary(CelType::Bool(), tag + "_eq", "(%0 == %1)", lex, lex);
    b.Binary(CelType::Bool(), tag + "_ne", "(%0 != %1)", lex, lex);
  }
  // Bool equality only — there's no ordering on bool in CEL.
  b.Binary(CelType::Bool(), "bool_eq", "(%0 == %1)",
           CelType::Bool(), CelType::Bool());
  b.Binary(CelType::Bool(), "bool_ne", "(%0 != %1)",
           CelType::Bool(), CelType::Bool());

  // ── Logical ───────────────────────────────────────────────────
  b.Unary (CelType::Bool(), "bool_not", "(!%0)",       CelType::Bool());
  b.Binary(CelType::Bool(), "bool_and", "(%0 && %1)",  CelType::Bool(), CelType::Bool());
  b.Binary(CelType::Bool(), "bool_or",  "(%0 || %1)",  CelType::Bool(), CelType::Bool());

  // ── Concat — total over the typed input domain ───────────────
  b.Binary(CelType::String(), "string_concat", "(%0 + %1)",
           CelType::String(), CelType::String());
  b.Binary(CelType::Bytes(),  "bytes_concat",  "(%0 + %1)",
           CelType::Bytes(),  CelType::Bytes());

  // ── Ternary — one rule per scalar result type ────────────────
  for (CelType t : {CelType::Bool(), CelType::Int(), CelType::Uint(),
                    CelType::Double(), CelType::String(), CelType::Bytes()}) {
    b.Ternary(t, TypeKey(t) + "_ternary", "(%0 ? %1 : %2)",
              CelType::Bool(), t, t);
  }

  // ── size on lexicographic scalars ─────────────────────────────
  b.Unary(CelType::Int(), "size_string", "size(%0)", CelType::String());
  b.Unary(CelType::Int(), "size_bytes",  "size(%0)", CelType::Bytes());

  // ── Total type conversions ────────────────────────────────────
  // double(int) and double(uint) are total: every int and every
  // uint round-trips to a representable double (with possible loss
  // of precision past 2^53, but no domain error).
  b.Unary(CelType::Double(), "double_from_int",
          "double(%0)", CelType::Int());
  b.Unary(CelType::Double(), "double_from_uint",
          "double(%0)", CelType::Uint());

  // int(uint) / uint(int) / int(double) / uint(double) / int(string)
  // / uint(string) are partial — they can range-fail.  Deliberately
  // omitted from Slice B; revisit in Slice C with literal-bounded
  // sources.

  Grammar g = std::move(b).Build();
  // L1 — Grammar::Validate() must accept the catalog the test
  // binary just constructed.  If this fires, the catalog has a
  // structural bug (missing leaf, mismatched placeholder, etc.)
  // and no PBT iterations should run against it.
  ABSL_CHECK_OK(g.Validate())
      << "Slice B grammar failed L1 validation; the catalog in "
         "grammar_slice_b.cc is malformed";
  return g;
}

}  // namespace celwasm::fuzz
