// Known-bugs registry — each test documents a CONFIRMED defect as a
// runnable regression, gated by GTEST_SKIP so the suite stays green
// until the bug is fixed.
//
// Workflow:
//   1. A bug is found and **verified to actually reproduce** (run the
//      expression through the real Compile -> Plan -> Eval pipeline and
//      observe the wrong result) before it earns an entry here.
//   2. The test asserts the SPEC-CORRECT behavior and then `GTEST_SKIP`s
//      with a one-line reason + the root-cause file:line.  Skipped, so
//      CI stays green; documented, so the bug isn't lost.
//   3. To fix: delete the `GTEST_SKIP(...)` line.  The assertion is now
//      a live regression guard — make it pass.
//
// This is deliberately NOT in the lint/cleanup backlog docs: those
// describe deferred work in prose; this file *executes* the bug, so a
// fix is provably a fix.

#include <cstdint>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Full pipeline, returning status so a checker/compile rejection is
// visible to the test rather than aborting (these are bug probes — we
// need to see *how* an expression behaves, including "won't compile").
absl::StatusOr<Value> TryEval(absl::string_view source) {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  if (!compiler.ok()) return compiler.status();
  auto program = compiler->Compile(source);
  if (!program.ok()) return program.status();
  auto instance = GlobalEngine().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a;
  return instance->Eval(a);
}

// ──────────────────────────────────────────────────────────────────
// BUG: lossy double rounding in numeric map-key equality.
//
// `map_keys_equal` (runtime/cel_runtime.c:42-43) compares
// two numeric keys with `numeric_compare_kernel(...) == kCmpEqual`,
// which for the int-vs-double case casts the int to double
// (cel_compare.c:139, `cmp_double((double)a, b)`).  For |int| > 2^53
// that cast is lossy, so a double query key spuriously equals a
// DISTINCT stored int key.
//
// The spec (cel-cpp `equality_functions.cc` `CheckAlternativeNumericType`
// + `internal/number.h` `LosslessConvertibleToInt`) requires key lookup
// to use lossless convertibility, NOT a rounding compare.
//
// `9007199254740992.0` and `9007199254740993` are distinct integers but
// the same f64, so:
//   dyn(9007199254740992.0) in {9007199254740993: 'a'}
//   spec / cel-cpp -> false   (no lossless match for the stored key)
//   cel2 currently -> true    (lossy (double)9007199254740993 == ...992.0)
//
// `dyn(...)` is required only to clear the static-subset checker for a
// cross-type `in` (cf. m5_test.cc); the bug is in the runtime kernel.
//
// Fix: route map_keys_equal / `in` / lookup through a lossless-eq
// predicate (mirror LosslessConvertibleToInt/Uint), leaving
// numeric_compare_kernel for ordering only.  Then delete the SKIP.
// ──────────────────────────────────────────────────────────────────
TEST(KnownBugs, MapKeyLossyDoubleEquality) {
  GTEST_SKIP()
      << "KNOWN BUG (verified: returns true, want false): lossy "
         "double rounding in numeric map-key equality, "
         "cel_runtime.c:42-43 via cel_compare.c:139. "
         "Delete this line when fixed — the assertion below guards it.";
  auto v = TryEval("dyn(9007199254740992.0) in {9007199254740993: 'a'}");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  // Spec: the double does NOT match the distinct stored int key.
  EXPECT_FALSE(*v->AsBool())
      << "map-key equality matched a >2^53 int against a rounded double "
         "(cel_runtime.c:42-43 via cel_compare.c:139)";
}

// ══════════════════════════════════════════════════════════════════
// CLASS: memory-model cliffs.  The per-Eval bump arena is a fixed
// 64 KiB (cel_layout.h:41), malloc'd once, NEVER grown, and reset only
// between Evals — nothing is reclaimed mid-Eval (cel_arena.c:85,106).
// So an expression's PEAK intermediate footprint, not its result size,
// is what's bounded: ~1365 CelValues (24 B) / ~680 map entries (48 B)
// for the entire expression.  Past that, `arena_alloc` returns 0 and
// the value poisons to CEL_ERR_OVERFLOW — a correct expression becomes
// uncomputable, and (for untrusted CEL, the canonical use case) a
// trivial resource-exhaustion vector with no backpressure.
//
// Each test below is a real expression with a SMALL result but a peak
// footprint over 64 KiB; the assertion is the spec-correct result, and
// the SKIP marks it as currently overflowing.
//
// NOTE — mitigation, deliberately left as a note, not implemented:
// a Sethi-Ullman-style evaluation ordering (emit the operand subtree
// that needs MORE temporaries first, so the count of simultaneously-
// live arena intermediates is minimized) would lower peak footprint for
// many expression shapes and push this cliff out without changing the
// arena.  It does NOT rescue a single oversized literal/collection
// (an N-element value still needs N·24 B live at once) — that case
// needs an arena grow/spill path.  Track both together when fixing.
// ══════════════════════════════════════════════════════════════════

// "[0, 1, …, n-1]" as CEL source.
std::string ListLiteral(int n) {
  std::string s = "[";
  for (int i = 0; i < n; ++i) {
    if (i != 0) s += ", ";
    s += std::to_string(i);
  }
  s += "]";
  return s;
}

// "{0: 0, 1: 1, …, n-1: n-1}" as CEL source.
std::string MapLiteral(int n) {
  std::string s = "{";
  for (int i = 0; i < n; ++i) {
    if (i != 0) s += ", ";
    s += std::to_string(i) + ": " + std::to_string(i);
  }
  s += "}";
  return s;
}

TEST(KnownBugs, ExpressionIntermediatesArenaCliff) {
  GTEST_SKIP()
      << "KNOWN LIMITATION (verified: returns CEL_ERR_OVERFLOW, want "
         "4000): peak list intermediate exceeds the fixed 64 KiB "
         "arena, cel_layout.h:41 / cel_arena.c:85. Delete this line "
         "when the arena can grow/spill (see Sethi-Ullman note above).";
  // `size([0..4000])` — the RESULT is one int (8 B), but the intermediate
  // list needs ~96 KB, over the 64 KB arena.  Small result, huge peak:
  // the canonical "intermediates dominate" cliff.
  auto v = TryEval("size(" + ListLiteral(4000) + ")");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 4000)
      << "list intermediate exceeded the 64 KiB arena (cel_layout.h:41)";
}

TEST(KnownBugs, MapSizeArenaCliff) {
  GTEST_SKIP()
      << "KNOWN LIMITATION (verified: returns CEL_ERR_OVERFLOW, want "
         "2000): peak map intermediate exceeds the fixed 64 KiB "
         "arena, cel_layout.h:41 / cel_arena.c:85. Delete this line "
         "when the arena can grow/spill (see Sethi-Ullman note above).";
  // A 2000-entry map needs ~96 KB (48 B/entry) and overflows, though
  // `size()` returns a single int.
  auto v = TryEval("size(" + MapLiteral(2000) + ")");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 2000)
      << "map intermediate exceeded the 64 KiB arena (cel_layout.h:41)";
}

// ══════════════════════════════════════════════════════════════════
// CLASS: conformance-confirmed divergences (mined from the 148 FAIL
// rows in tests/simple/testdata/*.textproto).  Plain-CEL only (no proto
// / host fixtures).  Added in verify mode first; skips applied only to
// the ones that actually reproduce through this engine.
// ══════════════════════════════════════════════════════════════════

TEST(KnownBugs, SizeStringCountsBytesNotCodepoints) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // string.textproto: size('ÿ') == 1 code point (got 2 = byte length).
  auto v = TryEval("size('ÿ')");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 1) << "size() counted UTF-8 bytes, not code points";
}

TEST(KnownBugs, SizeStringMultibyte) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // string.textproto: size('πέντε') == 5 (got 10).
  auto v = TryEval("size('πέντε')");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 5);
}

TEST(KnownBugs, HasOnMapPresentKey) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // fields.textproto: has({'a':1,'b':2}.a) == true.
  auto v = TryEval("has({'a': 1, 'b': 2}.a)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());
}

TEST(KnownBugs, HasOnMapAbsentKey) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // fields.textproto: has({'a':1,'b':2}.c) == false.
  auto v = TryEval("has({'a': 1, 'b': 2}.c)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_FALSE(*v->AsBool());
}

TEST(KnownBugs, DynDoubleListIndexCoercion) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // lists.textproto: [7,8,9][dyn(0.0)] == 7 (whole-number double index).
  auto v = TryEval("[7, 8, 9][dyn(0.0)]");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 7);
}

TEST(KnownBugs, DynUintListIndexCoercion) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // lists.textproto: [7,8,9][dyn(0u)] == 7.
  auto v = TryEval("[7, 8, 9][dyn(0u)]");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 7);
}

TEST(KnownBugs, IntFromDoubleOutOfRange) {
  // conversions.textproto: int(-9223372036854775808.0) is out of range ->
  // error per spec; cel2 returns a valid int instead.
  auto v = TryEval("int(-9223372036854775808.0)");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(v->kind(), Value::Kind::kError)
      << "int(-2^63 double) should be a range error, got kind "
      << static_cast<int>(v->kind());
}

TEST(KnownBugs, CelBindSelectorOnBoundVar) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // bindings_ext.textproto: cel.bind(x, {'y': 0}, x.y == 0) == true.
  auto v = TryEval("cel.bind(x, {'y': 0}, x.y == 0)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());
}

TEST(KnownBugs, ComprehensionVarSelector) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // namespace.textproto: [{'z': 0}].exists(y, y.z == 0) == true.
  auto v = TryEval("[{'z': 0}].exists(y, y.z == 0)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());
}

TEST(KnownBugs, ReservedWordMapSelector) {
  GTEST_SKIP() << "KNOWN BUG (verified reproducing); delete this line to fix — "
                  "the assertions below then guard it. See the per-test "
                  "comment + doc/implementation-plan/known-issues-findings.md.";
  // parse.textproto: {'as': 1}.as == 1 (reserved-but-not-keyword selector).
  auto v = TryEval("{'as': 1}.as");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 1);
}

TEST(KnownBugs, IntFromStringLeadingPlus) {
  // cel_convert.c:203-215 — parse_int64_str handles only leading '-';
  // cel-cpp's absl::SimpleAtoi accepts '+5'. Expected 5, got error.
  auto v = TryEval("int('+5')");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 5);
}

TEST(KnownBugs, UintFromStringLeadingPlus) {
  // cel_convert.c:217-223 — parse_uint64_str has no sign handling.
  auto v = TryEval("uint('+5')");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kUint) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsUint(), 5u);
}

TEST(KnownBugs, MapFieldSelectSugar) {
  GTEST_SKIP()
      << "KNOWN BUG (verified: CEL_ERR_TYPE_MISMATCH, want 1): EmitKSelect has "
         "no map-operand path; m.k sugar hits the message field trampoline "
         "(expr_lower.cc:199-261 / cel_host.cc:1394). One fix flips the whole "
         "map-select cluster. Delete to fix.";
  // ROOT CAUSE + minimal repro of the map-select cluster: HasOnMapPresentKey,
  // HasOnMapAbsentKey, ReservedWordMapSelector, CelBindSelectorOnBoundVar,
  // ComprehensionVarSelector (and backtick-quoted selectors) ALL share this.
  // EmitKSelect (expr_lower.cc:199-261) has no map-operand branch, so `m.k`
  // (≡ m['k'] sugar) lowers to the proto field-read trampoline, which
  // rejects a non-message operand at cel_host.cc:1394 -> CEL_ERR_TYPE_MISMATCH.
  // {'a': 1}.a should be 1.  One fix (map branch in EmitKSelect) flips the
  // whole cluster.
  auto v = TryEval("{'a': 1}.a");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 1);
}

// ══════════════════════════════════════════════════════════════════
// CLASS: string extension kernels (wave 4).
// ══════════════════════════════════════════════════════════════════

TEST(KnownBugs, IndexOfPosBoundIsByteNotCodepoint) {
  GTEST_SKIP()
      << "KNOWN BUG (verified: returns -1, want out-of-range error): "
         "indexOf/lastIndexOf pos bounded by byte length not code-point count, "
         "cel_string_ext_search.cc:122-133. Delete to fix.";
  // 'éé' = 2 code points / 4 bytes. indexOf(sub, pos) bounds pos against the
  // BYTE length (cel_string_ext_search.cc:122-133 ValidatePos), but cel-cpp
  // bounds pos against Size() = code-point count, so pos=3 is out of range.
  // Spec: error; cel2 silently returns -1.
  auto v = TryEval("'éé'.indexOf('x', 3)");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(v->kind(), Value::Kind::kError)
      << "indexOf pos=3 on a 2-codepoint string should be out-of-range, kind "
      << static_cast<int>(v->kind());
}

TEST(KnownBugs, FormatFixedRejectsInt) {
  GTEST_SKIP() << "KNOWN BUG (verified: renders '42.000000', want error): "
                  "%f/%e accept int; cel-cpp errors. "
                  "cel_string_format_render.cc:277-291. Delete to fix.";
  // cel-cpp %f/%e accept only double or the string tokens NaN/Infinity; %f of
  // an int errors ("expected a double but got a int"). cel2's ToDouble
  // (cel_string_format_render.cc:277-291) accepts int and renders it.
  auto v = TryEval("'%f'.format([42])");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(v->kind(), Value::Kind::kError)
      << "%f of an int should error, kind " << static_cast<int>(v->kind());
}

TEST(KnownBugs, FormatFixedAcceptsNanToken) {
  GTEST_SKIP()
      << "KNOWN BUG (verified: errors, want 'NaN'): %f of string token "
         "'NaN'/'Infinity' should render; ToDouble has no string-token path, "
         "cel_string_format_render.cc:277-291. Delete to fix.";
  // cel-cpp %f of the string "NaN" renders "NaN"; cel2 errors (ToDouble has
  // no string-token path, cel_string_format_render.cc:277-291).
  auto v = TryEval("'%f'.format(['NaN'])");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsString(), "NaN");
}

// ══════════════════════════════════════════════════════════════════
// CLASS: comprehension / macro semantics (wave 3).
// ══════════════════════════════════════════════════════════════════

TEST(KnownBugs, ExistsAbsorbsErrorAccumulator) {
  GTEST_SKIP() << "KNOWN BUG (verified: returns error, want true): exists "
                  "short-circuits on an ERROR/UNKNOWN accumulator "
                  "(expr_lower_comprehension.cc:642-645 checks bool-payload "
                  "bits, not accu.kind). Delete to fix.";
  // `exists` is `@result || pred`: an error from one element is absorbed
  // once a LATER element matches; the error only surfaces if NO element
  // ever matches. cel2's loop-cond peephole (expr_lower_comprehension.cc:
  // 642-645) br_if-exits on the accu's bool-payload bits without checking
  // accu.kind==CEL_BOOL, so an ERROR accu (non-zero err code in the bool
  // slot) short-circuits and becomes the result.
  // [0, 2].exists(x, 2/x == 1): x=0 errors, x=2 matches -> spec: true.
  auto v = TryEval("[0, 2].exists(x, 2/x == 1)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool()) << "exists short-circuited on an error accumulator";
}

TEST(KnownBugs, TransformMapEntryDuplicateKey) {
  GTEST_SKIP() << "KNOWN BUG (verified: returns {0:4}, want duplicate-key "
                  "error): cel_map_insert_at overwrites last-write-wins "
                  "(cel_runtime.c:157-162). Delete to fix.";
  // comprehensions_v2 builds the result via cel.@mapInsert == MapBuilder::Put,
  // which errors on a duplicate key (cel-cpp value_builder.cc). cel2's
  // cel_map_insert_at (cel_runtime.c:157-162) overwrites last-write-wins.
  // {1:2,3:4}.transformMapEntry(k, v, {0: v}) collides on key 0 -> want
  // a duplicate-key error; cel2 returns {0: 4}.
  auto v = TryEval("{1: 2, 3: 4}.transformMapEntry(k, v, {0: v})");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(v->kind(), Value::Kind::kError)
      << "duplicate map key silently overwrote (cel_runtime.c:157-162), kind "
      << static_cast<int>(v->kind());
}

TEST(KnownBugs, TransformMapEntryComputedEntryCrash) {
  // CRASH (verified via the cel CLI): a transformMapEntry whose entry expr
  // is not a literal kMapExpr (here a ternary) hits ABSL_CHECK(false) at
  // expr_lower_comprehension.cc:800-805 / :826-831 and ABORTS the compiler
  // instead of returning a status. Kept SKIPPED because running it would
  // abort this whole test binary; delete the skip only alongside the fix.
  GTEST_SKIP()
      << "KNOWN BUG (verified via cel CLI: ABSL_CHECK abort): "
         "transformMapEntry with a computed (non-literal) entry "
         "crashes the compiler, expr_lower_comprehension.cc:800-805. "
         "Running unskipped ABORTS the process — fix first, then unskip.";
  auto v =
      TryEval("{1: 2, 3: 4}.transformMapEntry(k, v, k == 1 ? {k: v} : {})");
  EXPECT_TRUE(v.ok()) << "should return a value/status, not crash: "
                      << v.status();
}

// ══════════════════════════════════════════════════════════════════
// CLASS: timestamp range + value formatting (wave 2).
// ══════════════════════════════════════════════════════════════════

TEST(KnownBugs, MaxRangeTimestampConstruction) {
  GTEST_SKIP() << "KNOWN BUG (verified: CEL_ERR_OVERFLOW, want the timestamp "
                  "string): max-range timestamp nanos rejected, "
                  "cel_time_parse.cc:178 / cel_time.c:46. Delete to fix.";
  // timestamps.textproto: the maximum timestamp
  // 9999-12-31T23:59:59.999999999Z is valid (cel-cpp MaxTimestamp =
  // seconds 253402300799 + nanos 999999999), but cel2 rejects ANY
  // positive nanos at the max second — cel_time_parse.cc:178
  // (`seconds==MAX && nanos>0`) / cel_time.c:46. Want the round-trip
  // string; got CEL_ERR_OVERFLOW at construction.
  auto v = TryEval("string(timestamp('9999-12-31T23:59:59.999999999Z'))");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsString(), "9999-12-31T23:59:59.999999999Z");
}

TEST(KnownBugs, DoubleToStringShortestRoundTrip) {
  GTEST_SKIP() << "KNOWN BUG (verified: '123.45600000000000306', want "
                  "'123.456'): double->string is not shortest-round-trip, "
                  "cel_convert.c:579-724. Delete to fix.";
  // conversions.textproto: string(123.456) == "123.456". cel2's iterative
  // frac*10 formatter (cel_convert.c:579-724) is not shortest-round-trip
  // and emits trailing garbage ("123.45600000000000306").
  auto v = TryEval("string(123.456)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsString(), "123.456");
}

TEST(KnownBugs, DoubleToStringExponentForm) {
  GTEST_SKIP()
      << "KNOWN BUG (verified: '10000000000', want '1e+10'): double->string "
         "exponent threshold differs from std::to_chars(general), "
         "cel_convert.c:665-699. Delete to fix.";
  // cel-cpp uses std::to_chars(general): string(1e10) == "1e+10".
  // cel2 emits "10000000000" (wrong exponent threshold,
  // cel_convert.c:665-699).
  auto v = TryEval("string(1e10)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsString(), "1e+10");
}

TEST(KnownBugs, OptionalSelectOnMapRejected) {
  GTEST_SKIP()
      << "KNOWN BUG (verified: checker-rejected, want 1): static subset "
         "wrongly rejects .?field optional select (recurses into the synthetic "
         "field-name child), parse_and_check.cc:631-641. Delete to fix.";
  // `.?field` (optional select) is wrongly rejected by the static subset.
  // cel-cpp's HandleOptSelect erases the synthetic field-name arg's type
  // (type_checker_impl.cc:1168); cel2's RejectDyn recurses into that untyped
  // child and flags "no type_map entry" (parse_and_check.cc:631-641),
  // blocking the entire `.?` syntax. {'a': 1}.?a.value() should be 1.
  auto v = TryEval("{'a': 1}.?a.value()");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 1);
}

TEST(KnownBugs, DoubleFromStringRejectsWhitespace) {
  GTEST_SKIP()
      << "KNOWN BUG (verified): double('  3.14  ') needs BOTH "
         "whitespace stripping AND correctly-rounded decimal->double. "
         "The whitespace strip is trivial, but cel2's parse_double_str "
         "(cel_convert.c) is 1 ULP off (3.14 -> 3.1399999999999997) — "
         "same imprecise-decimal<->double family as "
         "DoubleToStringShortestRoundTrip. Needs a correctly-rounded "
         "decimal->double; not Tier-1. Delete to fix.";
  // cel-cpp double(string) uses absl::SimpleAtod, which strips surrounding
  // ASCII whitespace before parsing; cel2's parse_double_str
  // (cel_convert.c:350-362) does not skip leading/trailing space, so
  // double('  3.14  ') errors. Want 3.14. (Distinct from the documented
  // hex-float gap, m10-conversions.md:649, which is left as known.)
  auto v = TryEval("double('  3.14  ')");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kDouble) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsDouble(), 3.14);
}

// NOTE: two wave-1 agent candidates were checked here and did NOT
// reproduce (engine returns the correct answer), so they are NOT bugs
// and are deliberately absent: `dyn(1) == 1u` correctly returns true
// (cross-numeric == via dyn works), and `[1] + []` correctly evaluates
// to list(int) (the static subset does NOT reject it).  Verify-first.

}  // namespace
}  // namespace celwasm
