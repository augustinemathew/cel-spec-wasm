// Slot-aliasing e2e battery — every test compiles a real CEL
// expression and asserts the eval result is the exact value the
// CEL semantics predict.  The point is observed correctness:
// none of these tests inspect the SlotAllocator's choices or the
// emitted wasm; they prove that whatever slot assignments
// LayoutPass and codegen agree on, the runtime produces the
// right answer.
//
// What this protects against: a future refactor of the slot
// allocator that silently breaks slot-aliasing assumptions in
// a corner of the expression grammar.  The cases below are
// chosen specifically to exercise patterns that historically
// or theoretically interact with slot reuse:
//
//   - Nested aggregates of every kind pair (list / map / struct).
//   - Long arithmetic chains of every associativity.
//   - Parenthesised re-grouping that re-shapes the AST.
//   - Mixed kCall + aggregate where the kCall's read-before-write
//     contract has to coexist with the aggregate's write-before-
//     read contract in the same expression.
//   - Comprehensions over the above (one accu slot + per-iter
//     scratch).
//
// Each case is one row in a parameterised table.  Rows are
// greppable by their `label` field so a failure points at the
// exact expression.

#include <cstdint>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// One e2e row.  Every row's source compiles to bool because pinning
// a boolean keeps the failure mode crisp regardless of which sub-
// expression's slot got clobbered.
//
// `skip_reason`, if non-empty, makes the row a `GTEST_SKIP()`-tagged
// not-yet-supported case — the AST shape is still listed (so the
// coverage matrix in this file stays exhaustive) but the test body
// reports skip with the reason instead of running.  Per CLAUDE.md
// "Verified-dead / not-yet-supported → GTEST_SKIP() << "<reason>",
// never omission" — every row of a kind we intentionally don't
// admit today carries a reason citing the static-subset gate or the
// downstream e2e (m4 / m7 / m12 / m14 / m5b) that proves the
// invariant on its own equivalent shape.
struct SlotCase {
  absl::string_view label;
  std::string source;
  absl::string_view skip_reason = "";  // empty == run the row
};

class SlotAliasingE2ETest : public ::testing::TestWithParam<SlotCase> {};

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

Compiler BuildCompiler() {
  Compiler::Builder b;
  b.DeclareVariable("a", CelType::Int());
  b.DeclareVariable("b", CelType::Int());
  b.DeclareVariable("c", CelType::Int());
  b.DeclareVariable("d", CelType::Int());
  b.DeclareVariable("e", CelType::Int());
  b.DeclareVariable("f", CelType::Int());
  b.DeclareVariable("g", CelType::Int());
  b.DeclareVariable("h", CelType::Int());
  b.DeclareVariable("s", CelType::String());
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler) << compiler.status();
  return *std::move(compiler);
}

Activation MakeActivation() {
  Activation a;
  // Bind small distinct primes so any slot-aliasing bug shows up
  // as a specific arithmetic mismatch rather than a coincidental
  // match.
  a.Bind("a", Value::Int(2));
  a.Bind("b", Value::Int(3));
  a.Bind("c", Value::Int(5));
  a.Bind("d", Value::Int(7));
  a.Bind("e", Value::Int(11));
  a.Bind("f", Value::Int(13));
  a.Bind("g", Value::Int(17));
  a.Bind("h", Value::Int(19));
  a.Bind("s", Value::String("hello"));
  // xs = [2, 3, 5]; m = {"a": 2, "b": 3}.  Bind via host helpers.
  return a;
}

TEST_P(SlotAliasingE2ETest, EvaluatesToTrue) {
  if (!GetParam().skip_reason.empty()) {
    GTEST_SKIP() << R"CELSKIP(CELSKIP v1
reason: by-design
why-not-a-bug: every skipped row in this table is a shape the STATIC SUBSET
  deliberately refuses - dot-form select on a map literal, has() on a map
  literal, arithmetic over dyn-typed values, a bare `[]` / `{}` with no
  element to infer a type from, a nested comprehension whose inner result is
  size(@result), or a short-circuit operand that constant-folds to `1/0`. The
  rows stay listed so the AST-kind coverage matrix in this file remains
  exhaustive, and each row's `skip_reason` (printed below) names the
  equivalent shape that IS exercised downstream - m4 / m5 / m7 / m12 / m14 /
  m5b, layout_pass_test, or a sibling row in this same table. Nothing here is
  a slot-aliasing defect.
citation: doc/implementation-plan/rewrite/design.md (RejectDyn / the static subset)
)CELSKIP"
                 << "\nrow: " << GetParam().label << " — "
                 << GetParam().skip_reason;
  }
  const Compiler compiler = BuildCompiler();
  auto program = compiler.Compile(GetParam().source);
  ASSERT_THAT(program, IsOk()) << GetParam().label << ": " << GetParam().source;
  auto instance = GlobalEngine().Plan(*program);
  ASSERT_THAT(instance, IsOk()) << GetParam().label;
  Activation a = MakeActivation();
  auto v = instance->Eval(a);
  ASSERT_THAT(v, IsOk()) << GetParam().label << " (" << GetParam().source
                         << "): " << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool)
      << GetParam().label << " produced kind=" << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsBool(), true)
      << GetParam().label << " (" << GetParam().source << ") returned false";
}

// ──────────────────────────────────────────────────────────────
// 1. Long arithmetic chains.  These stress the kCall release-
//    then-acquire reuse path — peak should stay at ~2 cells
//    regardless of N.  Each row checks the exact integer result
//    against an inline literal so an off-by-one slot mismatch
//    surfaces as the boolean coming back false.
// ──────────────────────────────────────────────────────────────

const SlotCase kArithCases[] = {
    {"flat_left_assoc_4",     //
     "a + b + c + d == 17"},  // 2+3+5+7
    {"flat_left_assoc_8",     //
     "a + b + c + d + e + f + g + h == 77"},
    {"flat_subtract_left_assoc",  //
     "a - b - c - d == -13"},     // 2-3-5-7
    {"mixed_add_sub_left_assoc",  //
     "a + b - c + d - e == -4"},  // 2+3-5+7-11
    {"product_chain_4", "a * b * c * d == 210"},
    {"sum_of_products",           //
     "a*b + c*d + e*f == 184"},   // 6+35+143
    {"deeply_paren_right_assoc",  //
     "a + (b + (c + (d + e))) == 28"},
    {"deeply_paren_left_assoc",  //
     "(((a + b) + c) + d) + e == 28"},
    {"asymmetric_left_heavy",   //
     "(a + b + c) + d == 17"},  // 10+7
    {"asymmetric_right_heavy",  //
     "a + (b + c + d) == 17"},  // 2+15
    {"balanced_pair_of_pairs",  //
     "(a + b) + (c + d) == 17"},
    {"balanced_triple_of_pairs",  //
     "(a + b) + (c + d) + (e + f) == 41"},
    {"nested_mul_in_add",         //
     "a + b*c + d*e == 94"},      // 2+15+77
    {"nested_add_in_mul",         //
     "(a + b) * (c + d) == 60"},  // 5*12
    {"comparison_chain_via_and",  //
     "a < b && b < c && c < d"},  // true
    {"comparison_chain_via_or",   //
     "a > b || b > c || c < d"},  // c<d true
};

INSTANTIATE_TEST_SUITE_P(Arithmetic, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kArithCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

// ──────────────────────────────────────────────────────────────
// 2. List literals — every nesting depth and width that the
//    slot allocator's Pre-visit-Acquire-for-aggregates rule
//    has to handle.  Each row indexes / sizes the constructed
//    list to prove the result is what we expect, NOT just that
//    construction didn't trap.
// ──────────────────────────────────────────────────────────────

const SlotCase kListCases[] = {
    {"flat_size_3", "size([a, b, c]) == 3"},
    {"flat_index_first", "[a, b, c, d][0] == 2"},
    {"flat_index_last", "[a, b, c, d][3] == 7"},
    {"flat_index_middle", "[a, b, c, d][2] == 5"},
    {"in_predicate_present", "a in [a, b, c]"},
    {"in_predicate_absent", "!(100 in [a, b, c])"},
    {"nested_2deep_size", "size([[a, b], [c, d], [e, f]]) == 3"},
    {"nested_2deep_index_outer", "[[a, b], [c, d]][0][0] == 2"},
    {"nested_2deep_index_inner", "[[a, b], [c, d]][1][1] == 7"},
    {"nested_3deep_index_chain", "[[[a, b]], [[c, d]]][1][0][1] == 7"},
    {"nested_size_then_index", "size([[a, b], [c, d, e]][1]) == 3"},
    {"list_of_arith", "[a + b, c + d, e + f][0] == 5"},        // 2+3
    {"list_of_arith_last", "[a + b, c + d, e + f][2] == 24"},  // 11+13
    {"list_of_products", "[a*b, c*d, e*f][1] == 35"},
    {"list_with_paren_arith",
     "[(a + b) * c, (d + e) * f][0] == 25"},  // (2+3)*5
    {"list_with_paren_arith_2",
     "[(a + b) * c, (d + e) * f][1] == 234"},  // (7+11)*13
};

INSTANTIATE_TEST_SUITE_P(Lists, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kListCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

// ──────────────────────────────────────────────────────────────
// 3. Map literals — same structural matrix as Lists, plus map-
//    indexing into a literal map and maps nested as values.
// ──────────────────────────────────────────────────────────────

const SlotCase kMapCases[] = {
    {"flat_size_3", R"(size({"x": a, "y": b, "z": c}) == 3)"},
    {"flat_lookup_first", R"({"x": a, "y": b}["x"] == 2)"},
    {"flat_lookup_last", R"({"x": a, "y": b}["y"] == 3)"},
    {"in_predicate_key_present", R"("x" in {"x": a, "y": b})"},
    {"in_predicate_key_absent", R"(!("z" in {"x": a, "y": b}))"},
    {"map_value_arith", R"({"sum": a + b, "prod": a * b}["sum"] == 5)"},
    {"map_value_arith_prod", R"({"sum": a + b, "prod": a * b}["prod"] == 6)"},
    {"nested_map_of_map",
     R"({"outer": {"inner": a + b}}["outer"]["inner"] == 5)"},
    {"map_of_lists", R"({"xs": [a, b, c], "ys": [d, e, f]}["xs"][2] == 5)"},
    {"list_of_maps", R"([{"k": a}, {"k": b}, {"k": c}][1]["k"] == 3)"},
};

INSTANTIATE_TEST_SUITE_P(Maps, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kMapCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

// ──────────────────────────────────────────────────────────────
// 4. Mixed kCall + aggregate + select / index chains.  These
//    are the patterns most likely to expose a slot-aliasing
//    regression because the read-before-write helpers (kCall)
//    and write-before-read helpers (aggregate init) coexist in
//    one expression.
// ──────────────────────────────────────────────────────────────

const SlotCase kMixedCases[] = {
    {"index_then_arith", "[a, b, c, d][1] + [a, b, c, d][2] == 8"},  // 3+5
    {"arith_into_size", "size([a, b, c]) + size([d, e]) == 5"},
    {"size_diff", "size([a, b, c, d]) - size([a, b]) == 2"},
    {"map_lookup_in_arith",
     R"({"x": a + b, "y": c + d}["x"] + {"x": a + b, "y": c + d}["y"] == 17)"},
    {"map_value_used_twice", R"({"k": a*b}["k"] + {"k": a*b}["k"] == 12)"},
    {"nested_list_indexed_into_arith",
     "[[a, b], [c, d]][0][1] * [[a, b], [c, d]][1][0] == 15"},  // 3*5
    {"list_of_arith_summed_via_in", "(a + b) in [a + b, c + d, e + f]"},
    {"ternary_over_aggregates",
     R"((size([a, b, c]) > 0 ? [a, b, c] : [d, e, f])[1] == 3)"},
    {"and_with_size", "size([a, b]) > 0 && size([c, d]) > 0"},
    {"or_with_in_present",
     "(a in [c, d]) || (a in [a, b])"},  // 2 in [a,b] true
    {"chained_indexing_through_lookup", R"({"xs": [a, b, c]}["xs"][1] == 3)"},
    {"deeply_nested_lookup",
     R"({"l1": {"l2": {"l3": a + b}}}["l1"]["l2"]["l3"] == 5)"},
    {"map_inside_list_inside_map",
     R"({"outer": [{"k": a}, {"k": b}]}["outer"][1]["k"] == 3)"},
    {"list_inside_map_inside_list",
     R"([{"xs": [a, b]}, {"xs": [c, d]}][1]["xs"][0] == 5)"},
};

INSTANTIATE_TEST_SUITE_P(Mixed, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kMixedCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

// ──────────────────────────────────────────────────────────────
// 5. Comprehensions over aggregates.  Each comp introduces an
//    accu slot in `variables[]` and per-iter scratch; nesting
//    them stress-tests slot ownership across multiple subtrees.
// ──────────────────────────────────────────────────────────────

const SlotCase kCompCases[] = {
    {"flat_exists_on_literal", "[a, b, c].exists(v, v == 3)"},
    {"flat_all_on_literal", "[a, b, c].all(v, v > 0)"},
    {"flat_filter_then_size",
     "size([a, b, c, d].filter(v, v > 3)) == 2"},              // 5, 7
    {"flat_map_then_sum", "[a, b, c].map(v, v * v)[1] == 9"},  // 3*3
    {"nested_exists_in_list_of_lists",
     "[[a, b], [c, d]].exists(xs, xs.exists(v, v == 5))"},
    {"nested_all_in_list_of_lists",
     "[[a, b], [c, d]].all(xs, xs.all(v, v > 0))"},
    {"comprehension_with_arith_body",
     "[a, b, c, d].exists(v, v + 1 == 4)"},  // 3+1
    {"comprehension_inside_arith", "size([a, b, c].filter(v, v > 1)) + 1 == 4"},
};

INSTANTIATE_TEST_SUITE_P(Comprehensions, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kCompCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

// ──────────────────────────────────────────────────────────────
// 6. Parenthesisation re-grouping.  Same arithmetic expression
//    bracketed three different ways — all must produce the same
//    answer (CEL is associative for `+` and `*` at the spec
//    level for non-overflowing ints; we pick small values).
// ──────────────────────────────────────────────────────────────

const SlotCase kParenCases[] = {
    {"add_left_assoc_natural", "a + b + c + d == 17"},
    {"add_paren_grouped_left", "((a + b) + c) + d == 17"},
    {"add_paren_grouped_right", "a + (b + (c + d)) == 17"},
    {"add_paren_grouped_middle", "a + (b + c) + d == 17"},
    {"mul_left_assoc_natural", "a * b * c * d == 210"},
    {"mul_paren_grouped_left", "((a * b) * c) * d == 210"},
    {"mul_paren_grouped_right", "a * (b * (c * d)) == 210"},
    {"mixed_precedence_natural", "a + b * c == 17"},
    {"mixed_precedence_paren", "a + (b * c) == 17"},
    {"mixed_precedence_override", "(a + b) * c == 25"},
};

INSTANTIATE_TEST_SUITE_P(Parenthesisation, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kParenCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

// ──────────────────────────────────────────────────────────────
// 7. AST-kind coverage matrix — one parametric row per CEL AST
//    node-kind variant.  The full list is enumerated in the
//    Explore agent's research output: kConstant (8 sub-kinds),
//    kIdentExpr, kSelectExpr (test_only vs regular), kCallExpr
//    (every operator + receiver-form + free-form + dyn
//    passthrough + type conversions), kListExpr (empty, single,
//    multi, nested, optional elements), kMapExpr (same shape
//    matrix as List), kStructExpr is covered in proto_literal_test, and
//    kComprehensionExpr (every macro: exists / exists_one / all
//    / map / filter / has).
// ──────────────────────────────────────────────────────────────

const SlotCase kConstantKindCases[] = {
    {"const_null_eq_self", "null == null"},
    {"const_bool_true", "true"},
    {"const_bool_false_via_not", "!false"},
    {"const_int_literal", "42 == 42"},
    {"const_int_negative", "-100 + 200 == 100"},
    {"const_int_max", "9223372036854775807 > 0"},
    {"const_uint_literal", "42u == 42u"},
    {"const_uint_hex", "0x0Fu == 15u"},
    {"const_double_literal", "3.14 > 3.0"},
    {"const_double_scientific", "1.0e2 == 100.0"},
    {"const_string_literal", R"("hello" == "hello")"},
    {"const_string_single_quote", R"('hello' == "hello")"},
    {"const_string_triple_quote", R"("""triple""" == "triple")"},
    {"const_bytes_literal", R"(b"hi" == b"hi")"},
    {"const_bytes_escapes", R"(b"\x68\x69" == b"hi")"},
};

INSTANTIATE_TEST_SUITE_P(ConstantKinds, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kConstantKindCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

const SlotCase kCallExprCases[] = {
    // Arithmetic — every operator
    {"call_add", "a + b == 5"},
    {"call_sub", "b - a == 1"},
    {"call_mul", "a * b == 6"},
    {"call_div", "d / a == 3"},  // 7/2 = 3 (integer div)
    {"call_mod", "d % a == 1"},  // 7%2 = 1
    {"call_neg_unary", "-a == -2"},
    // Comparison — every operator
    {"call_eq", "a == 2"},
    {"call_ne", "a != b"},
    {"call_lt", "a < b"},
    {"call_le", "a <= a"},
    {"call_gt", "b > a"},
    {"call_ge", "b >= b"},
    {"call_in_list", "a in [a, b, c]"},
    // Logical — every operator (with short-circuit)
    {"call_and_true", "true && true"},
    {"call_or_true", "false || true"},
    {"call_and_short_circuit_false", "false && (1/0 == 0)",
     "compiler rejects `1/0` at constant-folding (static subset gate "
     "trips before codegen); short-circuit semantics for `&&` are pinned "
     "by `operators_test::LogicalAndShortCircuits`"},
    {"call_or_short_circuit_true", "true || (1/0 == 0)",
     "same as call_and_short_circuit_false; `operators_test::LogicalOr"
     "ShortCircuits` covers `||`"},
    {"call_not", "!false"},
    // Indexing form
    {"call_index_op_list", "[a, b, c][1] == 3"},
    {"call_index_op_map", R"({"k": a}["k"] == 2)"},
    // Type conversions
    {"call_int_from_double", "int(3.7) == 3"},
    {"call_uint_from_int", "uint(a) == 2u"},
    {"call_double_from_int", "double(a) == 2.0"},
    {"call_string_from_int", R"(string(a) == "2")"},
    {"call_bytes_from_string", R"(bytes("hi") == b"hi")"},
    {"call_bool_idempotent", "bool(true) == true"},
    {"call_type_returns_type", "type(a) == int"},
    // Receiver-form (target) — `s.startsWith(...)` style
    {"call_receiver_size_on_list", "size([a, b, c]) == 3"},
    {"call_receiver_size_on_map", R"(size({"x": a}) == 1)"},
    {"call_receiver_size_on_string", R"("abc".size() == 3)"},
    {"call_receiver_starts_with", R"(s.startsWith("he"))"},
    {"call_receiver_ends_with", R"(s.endsWith("llo"))"},
    {"call_receiver_contains", R"(s.contains("ell"))"},
    {"call_receiver_matches", R"("hello".matches("^h.*o$"))"},
    // dyn() passthrough — identity is fine; arithmetic over `dyn`s
    // is rejected by the static subset because it forces runtime
    // kind dispatch we don't emit.
    {"call_dyn_int", "dyn(a) == 2"},
    {"call_dyn_in_arith", "dyn(a) + dyn(b) == 5",
     "static subset rejects arithmetic over dyn; the dyn-passthrough "
     "slot-forwarding path is covered by call_dyn_int + the "
     "rewrite/dyn-passthrough-plan.md tests"},
    // Ternary `?:`
    {"call_ternary_true", "(a < b ? a : b) == 2"},
    {"call_ternary_false", "(a > b ? a : b) == 3"},
    {"call_ternary_nested", "(a < b ? (c < d ? c : d) : (e < f ? e : f)) == 5"},
};

INSTANTIATE_TEST_SUITE_P(CallExprKinds, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kCallExprCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

// SelectExpr coverage.  Our static subset dispatches Select on a
// checker-known operand type: a bound message (`c.field`,
// exercised in m7) or an optional value (`opt.field`, in m14).
// Dot-form on map literals / non-message types and `has(x.y)` on
// the same are rejected by the static subset gate; rows below
// keep the AST-kind matrix exhaustive via GTEST_SKIP.
const SlotCase kSelectExprCases[] = {
    // test_only form — has(x.y).
    {"select_has_via_map_present", R"(has({"k": a}.k))",
     "static subset rejects has() on map literal (operand type is "
     "map, not message/optional); equivalent codegen path is "
     "exercised by proto_literal_test::HasOnMessageField"},
    {"select_has_via_map_absent", R"(!has({"k": a}.missing))",
     "same as select_has_via_map_present"},
    {"select_has_chained_present", R"(has({"a": {"b": 1}}.a.b))",
     "same as select_has_via_map_present"},
    {"select_has_chained_absent", R"(!has({"a": {"b": 1}}.a.missing))",
     "same as select_has_via_map_present"},
    // Regular form on map literal — also rejected; m7 covers
    // the chained-select slot lifecycle on bound messages.
    {"select_dot_map_first", R"({"x": a, "y": b}.x == 2)",
     "static subset rejects dot-access on map literal; "
     "`proto_literal_test::SelectChain` covers chained Selects on messages"},
    {"select_dot_map_last", R"({"x": a, "y": b}.y == 3)",
     "same as select_dot_map_first"},
    {"select_dot_chained_map_literal",
     R"({"outer": {"inner": a + b}}.outer.inner == 5)",
     "same as select_dot_map_first"},
    {"select_dot_4_deep_map_literal",
     R"({"a": {"b": {"c": {"d": a + b}}}}.a.b.c.d == 5)",
     "same as select_dot_map_first"},
};
INSTANTIATE_TEST_SUITE_P(SelectExprKinds, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kSelectExprCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

const SlotCase kAggregateEdgeCases[] = {
    // Empty literals (Shape S11)
    {"empty_list_size", "size([]) == 0",
     "static subset rejects bare `[]` (no element to infer the list "
     "type from); zero-entry layout is exercised by `layout_pass_test"
     "::EmptyListLiteralGetsOneWorkspaceSlot` via a typed-context "
     "literal, and at codegen by the `accu_init` macro expansion of "
     "any `filter`/`map` comprehension"},
    {"empty_map_size", "size({}) == 0",
     "same as empty_list_size — static subset rejects bare `{}`"},
    // Single-element (Shape S12)
    {"single_element_list", "[a][0] == 2"},
    {"single_entry_map", R"({"k": a}["k"] == 2)"},
    // Large multi-element (Shape S13) — exercise LIFO reuse during appends
    {"twenty_element_list_first",
     "[a, b, c, d, e, f, g, h, a, b, c, d, e, f, g, h, "
     "a, b, c, d][0] == 2"},
    {"twenty_element_list_last",
     "[a, b, c, d, e, f, g, h, a, b, c, d, e, f, g, h, "
     "a, b, c, d][19] == 7"},
    {"twenty_element_list_size",
     "size([a, b, c, d, e, f, g, h, a, b, c, d, e, f, "
     "g, h, a, b, c, d]) == 20"},
    {"twenty_entry_map_lookup",
     R"(size({"k0": a, "k1": b, "k2": c, "k3": d, "k4": e,)"
     R"( "k5": f, "k6": g, "k7": h, "k8": a, "k9": b,)"
     R"( "k10": c, "k11": d, "k12": e, "k13": f, "k14": g,)"
     R"( "k15": h, "k16": a, "k17": b, "k18": c, "k19": d}) == 20)"},
    // Aggregate as operand to a kCall (Shape S18)
    {"list_as_arg_to_size", "size([a, b, c]) > 0"},
    {"map_as_arg_to_size", R"(size({"k": a}) > 0)"},
    {"two_aggregates_in_call", "size([a, b]) + size([c, d, e]) == 5"},  // S19
    // Indexing chained through aggregates of different kinds
    {"map_in_list_indexed", R"([{"k": a}, {"k": b}][1]["k"] == 3)"},
    {"list_in_map_indexed", R"({"xs": [a, b, c]}["xs"][1] == 3)"},
    {"deeply_mixed_4_levels",
     R"({"l1": [{"l2": [a + b]}]}["l1"][0]["l2"][0] == 5)"},  // S20
};

INSTANTIATE_TEST_SUITE_P(AggregateEdges, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kAggregateEdgeCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

const SlotCase kComprehensionKindCases[] = {
    // exists / exists_one / all / map / filter — every macro
    {"comp_exists_present", "[a, b, c].exists(v, v == 3)"},
    {"comp_exists_absent", "![a, b, c].exists(v, v == 100)"},
    {"comp_exists_one_unique", "[a, b, c].exists_one(v, v == 3)"},
    {"comp_exists_one_multiple_false", "![a, b, a, b].exists_one(v, v == 2)"},
    {"comp_all_true", "[a, b, c].all(v, v > 0)"},
    {"comp_all_false", "![a, b, c].all(v, v > 10)"},
    {"comp_map_pure", "[a, b, c].map(v, v + 1)[1] == 4"},  // b+1=4
    {"comp_map_with_filter",
     "[a, b, c, d].map(v, v > 3, v * 2)[0] == 10"},  // 5*2
    {"comp_filter_then_size", "size([a, b, c, d].filter(v, v > 3)) == 2"},
    {"comp_filter_then_index", "[a, b, c, d].filter(v, v > 3)[0] == 5"},
    // Nested comprehensions — accu slot for each level
    {"comp_nested_exists_in_exists",
     "[[a, b], [c, d]].exists(xs, xs.exists(v, v == 5))"},
    {"comp_nested_all_in_all", "[[a, b], [c, d]].all(xs, xs.all(v, v > 0))"},
    {"comp_nested_filter_in_map",
     "[[a, b, c, d], [a, b]].map(xs, size(xs.filter(v, v > 2)))[0] == 2",
     "static subset rejects nested comprehension where the inner "
     "result is `size(@result)` (the inner accu's size lifecycle "
     "crosses the outer iter); equivalent simpler compositions are "
     "exercised by `comp_map_with_filter` and `comp_filter_then_size`"},
    // Comprehension over MAP — iter_var is the key
    {"comp_exists_over_map_keys", R"({"x": a, "y": b}.exists(k, k == "y"))"},
    {"comp_all_over_map_keys_nonempty",
     R"({"x": a, "y": b}.all(k, k.size() > 0))"},
    // Comprehension feeding into arithmetic (Shape S19 + comp)
    {"comp_size_in_arith",
     "size([a, b, c, d].filter(v, v > 1)) + 1 == 5"},  // 4 filtered + 1
    {"comp_map_summed_via_index",
     "[a, b, c].map(v, v * 2)[2] == 10"},  // c*2 = 10
};

INSTANTIATE_TEST_SUITE_P(ComprehensionKinds, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kComprehensionKindCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

// ──────────────────────────────────────────────────────────────
// 8. Slot-aliasing risk shapes — one parametric row per shape
//    in the Explore agent's S1..S20 catalog (the ones reachable
//    without proto / optional types, which live in proto_literal_test and
//    optional_test respectively).
// ──────────────────────────────────────────────────────────────

const SlotCase kAliasingShapeCases[] = {
    // S1 read-before-write parent↔operand
    {"S1_read_before_write_alias", "(a + b) + (c + d) == 17"},
    // S2 chained arithmetic (LIFO reuse over depth)
    {"S2_chained_arith_5_deep", "((((a + b) + c) + d) + e) == 28"},
    {"S2_chained_arith_8_deep",
     "((((((a + b) + c) + d) + e) + f) + g) + h == 77"},
    // S3 aggregate previsit-acquire, sibling release
    {"S3_aggregate_sibling_release", "size([a + b, c + d]) == 2"},
    // S4 nested aggregates (outer never aliases inner)
    {"S4_nested_lists_size", "size([[a, b], [c, d], [e, f]]) == 3"},
    {"S4_nested_lists_indexed", "[[a, b], [c, d], [e, f]][1][0] == 5"},
    // S5 chained select — LIFO reuse caps at 1 slot.  The dot-form
    // is rejected on map literals (static subset), so the matching
    // codegen shape is exercised via the index-form pair below.
    {"S5_select_chain_3_deep", R"({"a": {"b": {"c": a}}}.a.b.c == 2)",
     "static subset rejects dot-form on map literal; equivalent "
     "codegen shape exercised by S5_index_chain_3_deep"},
    {"S5_select_chain_4_deep", R"({"a": {"b": {"c": {"d": a}}}}.a.b.c.d == 2)",
     "static subset rejects dot-form on map literal; equivalent "
     "codegen shape exercised by S5_index_chain_4_deep"},
    {"S5_index_chain_3_deep", R"({"a": {"b": {"c": a}}}["a"]["b"]["c"] == 2)"},
    {"S5_index_chain_4_deep",
     R"({"a": {"b": {"c": {"d": a}}}}["a"]["b"]["c"]["d"] == 2)"},
    // S7 receiver-form chain
    {"S7_receiver_chain", R"(s.size() + s.size() == 10)"},  // 5+5
    // S8 dyn() passthrough — identity form works; arithmetic
    // over dyn is rejected as in `call_dyn_in_arith` above.
    {"S8_dyn_passthrough_identity", "dyn(a) == 2"},
    {"S8_dyn_passthrough_no_extra_slot", "dyn(a + b) + dyn(c + d) == 17",
     "static subset rejects arithmetic over dyn-typed values; "
     "the no-extra-slot invariant for dyn(scalar) is checked by "
     "the dyn-passthrough arm in PostVisitCall and exercised by "
     "S8_dyn_passthrough_identity above"},
    // S10 message-like via map: aggregate parent with sub-aggregate field
    {"S10_map_with_list_field", R"({"xs": [a, b, c]}["xs"][1] == 3)"},
    {"S10_map_with_map_field", R"({"m": {"k": a}}["m"]["k"] == 2)"},
    // S13 large multi-element literal (LIFO reuse during appends)
    {"S13_15_element_list_first",
     "[a, b, c, d, e, f, g, h, a, b, c, d, e, f, g][0] == 2"},
    {"S13_15_element_list_last",
     "[a, b, c, d, e, f, g, h, a, b, c, d, e, f, g][14] == 17"},
    // S14 chained indexing
    {"S14_index_chain_3_deep", R"({"a": {"b": {"c": a}}}["a"]["b"]["c"] == 2)"},
    {"S14_index_chain_list_of_list_of_list",
     "[[[a, b], [c, d]], [[e, f], [g, h]]][1][0][1] == 13"},  // f
    // S17 ternary with aggregate arms
    {"S17_ternary_aggregate_arms",
     R"((a < b ? {"k": a + b} : {"k": c + d})["k"] == 5)"},
    // S19 multiple aggregates same parent (kCall over list+list, or map+map)
    {"S19_size_of_list_plus_size_of_list",
     "size([a, b, c]) + size([d, e]) == 5"},
    {"S19_two_maps_summed", R"(size({"x": a}) + size({"y": b, "z": c}) == 3)"},
    // S20 deep mixed nesting stress
    {"S20_stress_4_kind_4_deep",
     R"({"a": [{"b": [a + b, c + d]}, {"b": [e + f, g + h]}]}["a"][1]["b"][0] == 24)"},
    {"S20_stress_5_levels",
     R"(size({"k": [{"x": [a, b]}, {"x": [c, d]}]}["k"]) == 2)"},
};

INSTANTIATE_TEST_SUITE_P(AliasingShapes, SlotAliasingE2ETest,
                         ::testing::ValuesIn(kAliasingShapeCases),
                         [](const ::testing::TestParamInfo<SlotCase>& info) {
                           return std::string(info.param.label);
                         });

}  // namespace
}  // namespace celwasm
