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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

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
  auto program = compiler->Compile(source, e2e::DefaultOpts());
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
      << "KNOWN LIMITATION (verified 2026-06-10: now RESOURCE_EXHAUSTED at "
         "Compile — the 4000-const rodata (~96 KB) trips the 8192-byte "
         "static-region gate in compiler/internal/compile.cc before the "
         "64 KiB arena cliff (cel_layout.h:41 / cel_arena.c:85) is even "
         "reached; want 4000.  Delete this line when BOTH the static "
         "region can relocate/grow AND the arena can grow/spill (see "
         "Sethi-Ullman note above).";
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
      << "KNOWN LIMITATION (verified 2026-06-10: now RESOURCE_EXHAUSTED at "
         "Compile — the 4000-const rodata (~96 KB) trips the 8192-byte "
         "static-region gate in compiler/internal/compile.cc before the "
         "64 KiB arena cliff (cel_layout.h:41 / cel_arena.c:85) is even "
         "reached; want 2000.  Delete this line when BOTH the static "
         "region can relocate/grow AND the arena can grow/spill (see "
         "Sethi-Ullman note above).";
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
  // string.textproto/size_one_unicode: size('ÿ') == 1 code point.
  // Fixed 2026-06-05 by reworking `cel_string_size_at_v` to walk the
  // UTF-8 byte stream and count codepoint-starting bytes.
  auto v = TryEval("size('ÿ')");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 1) << "size() counted UTF-8 bytes, not code points";
}

TEST(KnownBugs, SizeStringMultibyte) {
  // string.textproto/size_unicode: size('πέντε') == 5 (got 10).
  // Fixed 2026-06-05 — same kernel as SizeStringCountsBytesNotCodepoints.
  auto v = TryEval("size('πέντε')");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 5);
}

TEST(KnownBugs, HasOnMapPresentKey) {
  // fields.textproto: has({'a':1,'b':2}.a) == true.  Map-dot-field sugar
  // (langdef §"Field selection on maps"); fixed by EmitKSelect's kMap
  // branch (cleanup-backlog #9, 2026-06-05).
  auto v = TryEval("has({'a': 1, 'b': 2}.a)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());
}

TEST(KnownBugs, HasOnMapAbsentKey) {
  // fields.textproto: has({'a':1,'b':2}.c) == false.  Map-dot-field sugar;
  // fixed by EmitKSelect's kMap branch (cleanup-backlog #9, 2026-06-05).
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
  // bindings_ext.textproto: cel.bind(x, {'y': 0}, x.y == 0) == true.
  // Map-dot-field sugar through a bound variable; fixed by EmitKSelect's
  // kMap branch (cleanup-backlog #9, 2026-06-05).
  auto v = TryEval("cel.bind(x, {'y': 0}, x.y == 0)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());
}

TEST(KnownBugs, ComprehensionVarSelector) {
  // namespace.textproto: [{'z': 0}].exists(y, y.z == 0) == true.
  // Map-dot-field sugar on a comprehension iter var (langdef §"Field
  // selection"); fixed by EmitKSelect's kMap branch (cleanup-backlog
  // #9, 2026-06-05).
  auto v = TryEval("[{'z': 0}].exists(y, y.z == 0)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());
}

TEST(KnownBugs, ReservedWordMapSelector) {
  // parse.textproto: {'as': 1}.as == 1 (reserved-but-not-keyword selector).
  // Per CEL grammar, reserved words are admitted in selector position;
  // fixed by EmitKSelect's kMap branch (cleanup-backlog #9, 2026-06-05).
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
  // langdef §"Field selection on maps": `m.k` is sugar for `m['k']`.
  // Regression pin for the cluster fixed via cleanup-backlog #9
  // (2026-06-05): EmitKSelect gained a kMap branch that routes through
  // `cel_map_lookup` (or `cel_map_in` for `has()`) with the field name
  // lifted to rodata as a CEL_STRING CelValue.  Closes ~29 conformance
  // rows: 17 reserved-word selectors (parse/selectors), 4 quoted-map
  // and 2 map_has rows (fields), 3 optional-chaining rows (optionals),
  // 1 bind-shadow row (bindings_ext), 2 comprehension-shadowing
  // selector rows (namespace_shadowing).
  auto v = TryEval("{'a': 1}.a");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 1);
}

// Map-dot-field sugar coverage matrix.  Per the testing rule in
// CLAUDE.md "Cover the edge-case matrix": every shape that exercises
// the kMap branch of EmitKSelect.
TEST(KnownBugs, MapDotFieldNestedMapValue) {
  // Outer .c routes the inner kSelect (map-of-map) to cel_map_lookup
  // (kDynamic dispatcher) since the operand's static origin is kHost
  // but the runtime value is CEL_MAP_ARENA.
  auto v = TryEval("{'c': {'d': 1}}.c.d");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v->AsInt(), 1);
}

TEST(KnownBugs, MapDotFieldThenIndex) {
  // `{'c': {...}}.c['dashed-index']`: same nested-origin issue but at the
  // `_[_]` call (EmitKIndexCall) — operand is a kSelectExpr returning a
  // map, so codegen falls through to the kDynamic dispatcher.
  auto v = TryEval("{'c': {'k': 'v'}}.c['k']");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString);
  EXPECT_EQ(*v->AsString(), "v");
}

TEST(KnownBugs, MapDotFieldBacktickQuotedSlash) {
  // fields.textproto/quoted_map_fields/field_access_slash:
  // `{'/api/v1': true, '/api/v2': false}.`/api/v1`` → true.
  auto v = TryEval("{'/api/v1': true, '/api/v2': false}.`/api/v1`");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v->AsBool());
}

TEST(KnownBugs, MapDotFieldBacktickQuotedDot) {
  // fields.textproto/quoted_map_fields/field_access_dot:
  // `{'foo.txt': 32, 'bar.csv': 1024}.`foo.txt`` → 32.
  auto v = TryEval("{'foo.txt': 32, 'bar.csv': 1024}.`foo.txt`");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v->AsInt(), 32);
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

// INVARIANT — `string(double)` MUST round-trip to the exact double via
// the shortest decimal representation.  E2E regression pin for the
// `conversions/string/double` corpus row
// (`spec/tests/simple/testdata/conversions.textproto:264-266`).  Fix
// landed 2026-06-06 by routing the kernel through `std::to_chars(buf,
// end, v, chars_format::general)` in `cel_convert_double_format.cc`,
// mirroring cel-cpp's `FormatDouble`
// (`runtime/standard/type_conversion_functions.cc:56`).  A future
// change that swaps to a non-shortest formatter (`%.17g`, hand-rolled
// per-digit `frac*10`, …) will break this assertion by producing
// `"123.45600000000000306"` again — same failure mode as cleanup-
// backlog #38.
TEST(KnownBugs, DoubleToStringShortestRoundTrip) {
  auto v = TryEval("string(123.456)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsString(), "123.456");
}

// INVARIANT — `string(1e10)` MUST emit the scientific form `"1e+10"`,
// not the fixed form `"10000000000"`.  `std::to_chars(general)` picks
// fixed vs. scientific by minimising output length; for `1e10` the
// scientific form (5 chars) wins over fixed (11 chars).  cel-cpp's
// `FormatDouble` produces the same.  A future formatter swap that
// reintroduces a hard-coded magnitude threshold (the old C path used
// `< 1e18` for fixed) will break this case.
TEST(KnownBugs, DoubleToStringExponentForm) {
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

// ══════════════════════════════════════════════════════════════════
// CLASS: large-input ceilings surfaced by `bench/in_operator_bench.cc`.
// Each is paired with a backlog entry in
// `doc/implementation-plan/cleanup-backlog.md` (#15 / #16 / #17).
// ══════════════════════════════════════════════════════════════════

// Helper: Compile + Plan + Bind + Eval in one call.  `declare` runs
// against the Compiler::Builder before Build (declares variables);
// `bind` runs against the Activation before Eval (binds values).
// Returns the eval result so a parse / plan / eval rejection is
// visible to the test rather than ASSERT-failing inside the helper.
template <typename DeclareFn, typename BindFn>
absl::StatusOr<Value> TryEvalActivated(absl::string_view source,
                                       DeclareFn&& declare, BindFn&& bind) {
  Compiler::Builder b;
  std::forward<DeclareFn>(declare)(b);
  auto compiler = std::move(b).Build();
  if (!compiler.ok()) return compiler.status();
  auto program = compiler->Compile(source, e2e::DefaultOpts());
  if (!program.ok()) return program.status();
  auto instance = GlobalEngine().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a;
  std::forward<BindFn>(bind)(a);
  return instance->Eval(a);
}

// Builds `x in [0, 1, 2, ..., n-1]`.
std::string MakeIntListInSource(int n) {
  std::string s = "x in [";
  s.reserve(static_cast<size_t>(n) * 8);
  for (int i = 0; i < n; ++i) {
    if (i > 0) s.append(", ");
    s.append(std::to_string(i));
  }
  s.push_back(']');
  return s;
}

// ──────────────────────────────────────────────────────────────────
// BUG (cleanup-backlog #15): cel-cpp parser caps source at 100 000
// codepoints (`expression_size_codepoint_limit`,
// `third_party/cel-cpp/parser/options.h:37`), and our
// `DefaultParserOptions()` in
// `compiler/frontend/parse_and_check.cc:1079` doesn't override the
// default.  `CompilerOptions` exposes no knob to raise it.
// Surfaced by `bench/in_operator_bench.cc` — a literal
// `[0..999_999]` source (~7.9 MB) is rejected at parse.
// ──────────────────────────────────────────────────────────────────
TEST(KnownBugs, ParserSourceCodepointLimitNotConfigurable) {
  GTEST_SKIP()
      << "KNOWN LIMIT (verified: returns INVALID_ARGUMENT / 'expression "
         "size exceeds codepoint limit', want OK): cel-cpp parser "
         "default cap of 100 k codepoints; DefaultParserOptions at "
         "compiler/frontend/parse_and_check.cc:1079 doesn't override "
         "it and no CompilerOptions knob exists.  Delete this line "
         "when the cap is raised or exposed via CompilerOptions "
         "(cleanup-backlog #15).";
  // A 110 k-codepoint single string literal is the simplest probe;
  // no bindings required.  cel-cpp counts the surrounding quotes too
  // so 110 k content + 2 quotes pushes well past the cap.
  std::string source = "'";
  source.append(110'000, 'x');
  source.push_back('\'');
  auto v = TryEval(source);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(v->AsString()->size(), 110'000u);
}

// ──────────────────────────────────────────────────────────────────
// REGRESSION (cleanup-backlog #16, formerly **P0**): literal
// `x in [0..9999]` used to compile + plan cleanly and then PANIC
// wasmtime at Eval (`store.rs:2440 assertion failed:
// fault.is_none()`, Rust abort, host process dies).  Root cause:
// neither Compile arm validated the expression's static region
// (rodata + workspace) against the 8192-byte low-memory window the
// runtime reserves for it (`-Wl,--global-base=8192`,
// runtime/cel_layout.h `CELWASM_RESERVED_LOW_MEMORY_BYTES`) — the
// 240 KB rodata segment was applied over the runtime's static data
// / heap in the SHARED memory at instantiate time.  After the
// memory-ownership flip the same corruption surfaced as
// `wasm trap: unaligned atomic` (clobbered dlmalloc state) instead
// of the panic; both shapes are closed by the compile-time gate
// (`ValidateExprStaticRegion`, compiler/internal/compile.cc): the
// expression is now REJECTED AT COMPILE with ResourceExhausted in
// BOTH link modes.
//
// ASPIRATION (future arena/region work): the original want was
// OK + true — a 10 K-element literal list is a legitimate
// expression.  Supporting it needs a relocatable / growable static
// region (and the arena grow/spill noted at the top of this file),
// at which point these assertions flip back to value checks.
// ──────────────────────────────────────────────────────────────────
TEST(KnownBugs, LiteralIntListInScanRejectedAtCompileAt10K) {
  constexpr int kN = 10'000;
  const std::string source = MakeIntListInSource(kN);
  auto v = TryEvalActivated(
      source,
      [](Compiler::Builder& b) {
        b.DeclareVariable("x", CelType::Int());
      },
      [](Activation& a) {
        a.Bind("x", Value::Int(kN - 2));
      });
  // Graceful compile-time rejection — NOT a wasmtime panic, NOT a
  // mid-eval corruption trap.
  ASSERT_FALSE(v.ok()) << "10K-element literal list unexpectedly evaluated — "
                          "if the static-region budget grew, update the "
                          "boundary tests below too";
  EXPECT_EQ(v.status().code(), absl::StatusCode::kResourceExhausted)
      << v.status();
}

// Boundary pin for the static-region gate, both modes.  For a literal
// `x in [0..N-1]` the rodata grows ~24 bytes per int constant, and the
// slot-exhaustion gate (LayoutPass::MaxWorkspaceBytes) reserves a
// `kGuardBytes`=256-byte guard band below wasi-libc's static data at
// `kReservedLowMemoryBytes`=8192 — so the workspace the `in`-scan needs
// must fit in `8192 - 256 - rodata_end`.  Empirically (re-probed
// 2026-06-10 via tools/cel after the slot-reuse merge) N=327 is the
// largest list whose rodata still leaves room for the scan workspace;
// N=328 is the smallest that overflows.  Unlike the deep `+`-chain,
// this ceiling is rodata-bound, so slot reuse does NOT raise it — the
// constants themselves consume the window.
TEST(KnownBugs, LiteralIntListInScanLargestFittingEvals) {
  constexpr int kN = 327;  // largest N whose region fits the window
  const std::string source = MakeIntListInSource(kN);
  auto v = TryEvalActivated(
      source,
      [](Compiler::Builder& b) {
        b.DeclareVariable("x", CelType::Int());
      },
      [](Activation& a) {
        a.Bind("x", Value::Int(kN - 2));
      });
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());
}

TEST(KnownBugs, LiteralIntListInScanJustOverWindowRejectedAtCompile) {
  constexpr int kN = 328;  // smallest N whose region overflows the window
  const std::string source = MakeIntListInSource(kN);
  auto v = TryEvalActivated(
      source,
      [](Compiler::Builder& b) {
        b.DeclareVariable("x", CelType::Int());
      },
      [](Activation& a) {
        a.Bind("x", Value::Int(kN - 2));
      });
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(v.status().code(), absl::StatusCode::kResourceExhausted)
      << v.status();
}

// ──────────────────────────────────────────────────────────────────
// BUG (cleanup-backlog #17): a bound `list<string>` of ≥ 10 000
// 50-byte strings Eval'd through `perm in perms` returns
// `FAILED_PRECONDITION: arena OOM in CelMapLookupImpl` from
// inside `cel_list_in`'s trampoline.  Distinct from #16 because
// (a) graceful error (no panic), (b) the source list is a bound
// variable, not a literal — OOM happens during scan, not list
// construction.  Production CEL-policy / IAM workloads with
// large permission sets hit this.
// ──────────────────────────────────────────────────────────────────
TEST(KnownBugs, BoundStringListInScanArenaOomAt10K) {
  GTEST_SKIP() << "KNOWN BUG (verified: returns FAILED_PRECONDITION / "
                  "'arena OOM in CelMapLookupImpl' from inside cel_list_in, "
                  "want OK + true).  10 k bound 50-byte strings exhaust the "
                  "64 KiB per-Eval arena (runtime/cel_layout.h:16) during "
                  "the linear scan.  Delete this line when cel_list_in "
                  "stops materialising O(N) arena state per scan, OR the "
                  "arena can grow on demand (cleanup-backlog #17).";
  constexpr size_t kN = 10'000;
  // Build N unique 50-byte strings deterministically.
  std::vector<Value> elements;
  elements.reserve(kN);
  for (size_t i = 0; i < kN; ++i) {
    std::string s(50, '0');
    s.replace(0, 8, "iam.perm");
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%zu", i);
    s.replace(40, std::strlen(buf), buf);
    elements.push_back(Value::String(std::move(s)));
  }
  // Pick the last one as the needle (worst-case scan, but also the
  // case where the scan eventually MATCHES — so the post-fix
  // assertion is `true`).
  std::string needle(50, '0');
  needle.replace(0, 8, "iam.perm");
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%zu", kN - 1);
  needle.replace(40, std::strlen(buf), buf);
  auto v = TryEvalActivated(
      "perm in perms",
      [](Compiler::Builder& b) {
        b.DeclareVariable("perm", CelType::String());
        b.DeclareVariable("perms", CelType::List(CelType::String()));
      },
      [&](Activation& a) {
        a.Bind("perm", Value::String(needle));
        a.Bind("perms", Value::List(std::move(elements)));
      });
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());
}

// ──────────────────────────────────────────────────────────────────
// Long-arithmetic regressions — paired positive / known-bug.
//
// Source: `a_0*a_{(0*7+3)%10} + a_1*a_{(1*7+3)%10} + ...` repeated
// `kTerms` times over 10 int vars `a..j` bound to the first 10
// primes.  Same shape as `bench/in_operator_bench.cc`'s
// `BM_Eval_LongArith_*Terms`.
//
// Both tests below share the same expression-building helper inline
// (no shared header — these two tests are the only callers) so the
// matrix is greppable in one place.
// ──────────────────────────────────────────────────────────────────

namespace {

std::string MakeLongArithSource(int n_terms) {
  static constexpr char kVars[] = "abcdefghij";
  std::string s;
  s.reserve(static_cast<size_t>(n_terms) * 5);
  for (int i = 0; i < n_terms; ++i) {
    if (i > 0) s.append(" + ");
    s.push_back(kVars[i % 10]);
    s.push_back('*');
    s.push_back(kVars[((i * 7) + 3) % 10]);
  }
  return s;
}

void BindLongArithVars(Activation& a) {
  static constexpr int64_t kVals[10] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29};
  for (int i = 0; i < 10; ++i) {
    a.Bind(std::string(1, static_cast<char>('a' + i)), Value::Int(kVals[i]));
  }
}

void DeclareLongArithVars(Compiler::Builder& b) {
  for (char c = 'a'; c <= 'j'; ++c) {
    b.DeclareVariable(std::string(1, c), CelType::Int());
  }
}

// Pre-compute the spec-correct result on the C++ side so the
// assertion is independent of CEL's evaluator (and we know the value
// will arrive once the bug is closed).
int64_t ExpectedLongArithResult(int n_terms) {
  static constexpr int64_t kVals[10] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29};
  int64_t sum = 0;
  for (int i = 0; i < n_terms; ++i) {
    sum += kVals[i % 10] * kVals[((i * 7) + 3) % 10];
  }
  return sum;
}

}  // namespace

// Positive baseline — confirms the parser-depth bump
// (`parse_and_check.cc::DefaultParserOptions`: 32 → 16 384) plus the
// codegen / Eval path actually work for a long-but-not-pathological
// `+`-chain.  With the LIFO free-list slot reuse in
// `SlotAllocator::Release`, a left-associative `+`-chain reuses a
// handful of workspace cells regardless of length, so this size — and
// the 2000-term sibling below — both fit comfortably inside the
// 8192-byte reserved window.
TEST(KnownBugs, LongArith165TermsWorks) {
  constexpr int kN = 165;
  const std::string source = MakeLongArithSource(kN);
  auto v = TryEvalActivated(source, DeclareLongArithVars, BindLongArithVars);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), ExpectedLongArithResult(kN));
}

// ──────────────────────────────────────────────────────────────────
// REGRESSION (long-arith codegen): kept around the same shape that
// originally exposed the "unaligned atomic" trap at N >= 2000.
// Original symptom — `wasm trap: unaligned atomic` from inside an
// expression-module call into a runtime helper at N = 2000+ — was
// not an alignment issue per se: `SlotAllocator::Release` was a no-op
// (see compiler/codegen/slot_allocator.cc) so every intermediate of
// `a*b + a*b + ... + a*b` got a fresh workspace cell — peak slots
// = N-1.  Past N=2000 the workspace grew across a wasm-page boundary
// and the runtime helpers' atomic ops landed on misaligned offsets.
// Fixed by giving Release a LIFO free list so the same chain peaks
// at one workspace slot — the whole `+`-chain reuses a handful of
// cells, so the static region stays well inside the 8192-byte window
// and a 2000-term chain now compiles AND evaluates to its value.
// Validated bottom-up by compiler/codegen/slot_allocator_test::
//   LeftAssocAdditionChainAfterReleaseFix/N2000 (no codegen, exact
//   peak count) and top-down by this e2e (full pipeline through
//   wasmtime).  Keep the e2e — it pins the cross-page-boundary case
//   the unit test can't reach.
// ──────────────────────────────────────────────────────────────────
TEST(KnownBugs, LongArith_2000Terms_NoUnalignedAtomicTrap) {
  constexpr int kN = 2000;
  const std::string source = MakeLongArithSource(kN);
  auto v = TryEvalActivated(source, DeclareLongArithVars, BindLongArithVars);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), ExpectedLongArithResult(kN));
}

// ──────────────────────────────────────────────────────────────────
// CLASS: PBT-discovered divergences.  Each row was first surfaced by
// `e2e/fuzz/cel_oracle_property_test.cc`'s value-oracle property
// (Slice B of M27 — see
// `doc/implementation-plan/rewrite/m27-pbt-cel-generator.md`).
//
// The PBT property generates a CEL source string, evals it through
// BOTH our pipeline and cel-cpp, and expects identical bool results.
// When it finds a divergence, the shrunken source is pinned here as
// a focused regression test against a SMALL set of bound activation
// values.  The PBT tool stays as the discovery surface; these rows
// pin specific bugs so the fix has a concrete target.
//
// Each row carries the fuzztest seed that originally surfaced it so
// a future PBT run can re-find it deterministically.
// ──────────────────────────────────────────────────────────────────

namespace pbt_repro {

void DeclareSliceBPbtVars(Compiler::Builder& b) {
  b.DeclareVariable("i_a", CelType::Int());
  b.DeclareVariable("i_b", CelType::Int());
  b.DeclareVariable("b_a", CelType::Bool());
}

void BindSliceBPbtVars(Activation& a) {
  a.Bind("i_a", Value::Int(7));
  a.Bind("i_b", Value::Int(11));
  a.Bind("b_a", Value::Bool(true));
}

// Slice C PBT activation — Slice B vars plus uint / double / string
// / bytes leaves the Slice C grammar pulls from.  Mirrors
// `e2e/fuzz/oracle_harness.cc::SliceBBoundActivation` so a repro
// the miner shrunk runs unchanged here.
void DeclareSliceCPbtVars(Compiler::Builder& b) {
  b.DeclareVariable("i_a", CelType::Int());
  b.DeclareVariable("i_b", CelType::Int());
  b.DeclareVariable("u_a", CelType::Uint());
  b.DeclareVariable("d_a", CelType::Double());
  b.DeclareVariable("b_a", CelType::Bool());
  b.DeclareVariable("s_a", CelType::String());
  b.DeclareVariable("y_a", CelType::Bytes());
}

void BindSliceCPbtVars(Activation& a) {
  a.Bind("i_a", Value::Int(7));
  a.Bind("i_b", Value::Int(11));
  a.Bind("u_a", Value::Uint(5));
  a.Bind("d_a", Value::Double(3.14));
  a.Bind("b_a", Value::Bool(true));
  a.Bind("s_a", Value::String("hello"));
  a.Bind("y_a", Value::Bytes("hi"));
}

}  // namespace pbt_repro

// fuzztest seed=3696381601904611693 depth=4 — PBT slice B run on
// 2026-06-05.  Our pipeline returns `kError` on a totally-defined
// Bool expression that the cel-cpp oracle evaluates to `false`.
// Reduced manually from the shrunk source; cel-cpp's reduction
// indicates the ternary-inside-int-subtract path
// `(i_a - (b_a ? 0 : i_b))` is the suspect.  Spec eval:
//   b_a=true   → (b_a ? 0 : i_b) = 0
//   i_a - 0    = 7
//   size("")=0, (0 < 7) = true
//   ("" == "x") = false
//   true == false = false  → the expected eval result.
// FIXED 2026-06-05 — `EmitConditional` now passes the cond's
// `Storage` through `EmitSlotBaseAddress`, which dispatches
// `kLocal` storage as `(local.get index)` instead of treating the
// local index as a literal byte offset.  See
// `compiler/codegen/expr_lower.cc::EmitSlotBaseAddress` +
// `expr_lower_internal.h`'s helper doc.  The assertion below is
// now a live regression guard.
TEST(KnownBugs, PbtTernaryInsideIntSubtract) {
  constexpr absl::string_view source =
      R"(((size("") < (i_a - (b_a ? 0 : i_b))) == ("" == "x")))";
  auto v = TryEvalActivated(source, pbt_repro::DeclareSliceBPbtVars,
                            pbt_repro::BindSliceBPbtVars);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_FALSE(*v->AsBool()) << "ternary inside int subtract evaluated wrong";
}

// PBT seed=1 (target=bool) and seed=3 (target=bytes) on the Slice C
// grammar, 2026-06-05.  cleanup-backlog #32 + #33 — `exists_one`'s
// comprehension result lives in the `comp.result()` sub-expression's
// slot (a `kCallExpr(_==_, @result, 1)` writing a Bool to its own
// workspace slot), NOT in the accu_var's slot which still holds the
// loop counter Int.  Before the fix, `ComprehensionLocalsVisitor::
// PostVisitComprehension` stamped the comp with accu_var's slot, so
// the ternary cond-load read the count Int instead of the loop's
// Bool result and the whole downstream expression poisoned to
// kError.  Pinning shape: an `exists_one` feeding a ternary cond
// whose then-arm is a bytes ident.
//
// Spec eval for the bytes case (b_a=true, d_a=3.14, y_a=b"hi"):
//   {b_a: -1}.exists_one(k, d_a < 1.0)
//     → pred false for the one key → false
//   false ? y_a : b"x"  → b"x"
//   b"x" + b"x"         → b"xx"
TEST(KnownBugs, PbtExistsOneInTernaryCondBytes) {
  constexpr absl::string_view source =
      R"((({b_a: (-1)}).exists_one(k, (d_a < 1.0)) ? y_a : b"x") + b"x")";
  auto v = TryEvalActivated(source, pbt_repro::DeclareSliceCPbtVars,
                            pbt_repro::BindSliceCPbtVars);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBytes) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsBytes(), "xx") << "exists_one cond mis-routed";
}

// Companion: when `exists_one` returns true the SAME shape must
// pick the then-arm, returning a bytes-typed value.  Catches a
// regression in the opposite direction.
TEST(KnownBugs, PbtExistsOneInTernaryCondTakesThen) {
  constexpr absl::string_view source =
      R"((({"k": 1}).exists_one(k, true) ? y_a : b"x") + b"x")";
  auto v = TryEvalActivated(source, pbt_repro::DeclareSliceCPbtVars,
                            pbt_repro::BindSliceCPbtVars);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBytes) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsBytes(), "hix");
}

// PBT int seed=137 (Slice C grammar, 2026-06-05) — reduced form
// of the `size((cond ? <bytes-ternary> : <bytes-ternary>))` shape
// that was producing kError because the inner bytes ternary's
// result kind was being read from the wrong slot.  Spec eval: the
// exists_one is true (one key, predicate true), inner ternary's
// then-arm is `(y_a + y_a)` = b"hihi" (size 4).
TEST(KnownBugs, PbtSizeOfExistsOneTernaryBytes) {
  constexpr absl::string_view source =
      R"(size((({"k": 1}).exists_one(k, true) ? (y_a + y_a) : (y_a + b"x"))))";
  auto v = TryEvalActivated(source, pbt_repro::DeclareSliceCPbtVars,
                            pbt_repro::BindSliceCPbtVars);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 4);
}

}  // namespace
}  // namespace celwasm
