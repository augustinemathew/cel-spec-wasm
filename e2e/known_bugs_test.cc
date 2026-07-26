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
#include "eval/internal/cel_host.h"  // HostListBacking definition
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0004
severity: P0
kind: wrong-value
summary: numeric map-key equality rounds a >2^53 int key to double, so a distinct double query key spuriously matches
repro: dyn(9007199254740992.0) in {9007199254740993: 'a'}
bindings: none
actual: true
expected: false
layer: runtime/cel_runtime.c:42-43 (map_keys_equal) via runtime/cel_compare.c:139 (cmp_double)
blocked-by: none
found-by: manual known-bugs sweep; re-confirmed 2026-07-25 through tools/cel eval
fix-hint: map_keys_equal compares two numeric keys with
  numeric_compare_kernel(...) == kCmpEqual, which for the int-vs-double case
  casts the int to double (cel_compare.c:139, cmp_double((double)a, b)); for
  |int| > 2^53 that cast is lossy, so 9007199254740993 and 9007199254740992.0
  collapse to the same f64. The spec requires key lookup to use LOSSLESS
  convertibility, not a rounding compare - see cel-cpp
  equality_functions.cc CheckAlternativeNumericType and
  internal/number.h LosslessConvertibleToInt. Route map_keys_equal, `in` and
  lookup through a lossless-eq predicate and leave numeric_compare_kernel for
  ordering only. The dyn(...) wrapper in the repro is only there to clear the
  static-subset checker for a cross-type `in`; the defect is in the runtime
  kernel. Silently wrong with no error, hence P0.
issue: none
)CELBUG";
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
  // `size([0..4000])` — formerly a "small result, huge intermediate"
  // cliff: the list once had to be built in the arena per Eval (~96 KB,
  // over the old 64 KB arena) and its rodata once overflowed the old
  // 8 KiB static window.  Now the const list is materialized into the
  // 256 KiB rodata window at compile time, so no intermediate is built
  // at all and `size()` is one O(1) load.
  auto v = TryEval("size(" + ListLiteral(4000) + ")");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 4000);
}

TEST(KnownBugs, MapSizeArenaCliff) {
  // `size({0:0 .. 1999:1999})` — a 2000-entry map is ~96 KB (48 B/entry).
  // Const maps are not yet materialized (that is the Swiss-table slice),
  // so the map is still built in the arena per Eval — but the arena now
  // grows in chunks (cel_arena.c) instead of failing at a fixed 64 KB
  // cliff, and its const-keyed rodata fits the 256 KiB window, so the
  // build succeeds and `size()` returns the count.
  auto v = TryEval("size(" + MapLiteral(2000) + ")");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 2000);
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0005
severity: P1
kind: wrong-value
summary: dyn double list-index coercion diverges from the lists.textproto row
repro: [7, 8, 9][dyn(0.0)]
bindings: none
actual: unverified - the original pin message recorded only "verified reproducing" and no observed value. Re-probed 2026-07-25 through tools/cel eval (static link mode), which returns 7 (the spec-correct answer), so this pin may be STALE.
expected: 7
layer: the list-index dispatcher for a dyn-typed index (compiler/codegen expr_lower index arm + runtime/cel_list.c index coercion)
blocked-by: none
found-by: conformance corpus spec/tests/simple/testdata/lists.textproto (whole-number double index row)
fix-hint: doc/implementation-plan/known-issues-findings.md records this row as
  "returns error; conformance fixture expects a value", but a 2026-07-25 CLI
  re-probe returned the correct value. BEFORE working this pin, delete the
  GTEST_SKIP and run the assertion under BOTH e2e link modes
  (known_bugs_test_static and known_bugs_test_dynamic) - if it passes, the
  right change is to remove the pin, not to fix anything. Mirror of CELW-0006
  (the uint index form); settle both with the same probe.
status: possibly-stale - re-run the assertion unskipped before working it
issue: none
)CELBUG";
  // lists.textproto: [7,8,9][dyn(0.0)] == 7 (whole-number double index).
  auto v = TryEval("[7, 8, 9][dyn(0.0)]");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 7);
}

TEST(KnownBugs, DynUintListIndexCoercion) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0006
severity: P1
kind: wrong-value
summary: dyn uint list-index coercion diverges from the lists.textproto row
repro: [7, 8, 9][dyn(0u)]
bindings: none
actual: unverified - the original pin message recorded only "verified reproducing" and no observed value. Re-probed 2026-07-25 through tools/cel eval (static link mode), which returns 7 (the spec-correct answer), so this pin may be STALE.
expected: 7
layer: the list-index dispatcher for a dyn-typed index (compiler/codegen expr_lower index arm + runtime/cel_list.c index coercion)
blocked-by: none
found-by: conformance corpus spec/tests/simple/testdata/lists.textproto (uint index row)
fix-hint: identical situation to CELW-0005, one type over - see that pin's
  fix-hint. doc/implementation-plan/known-issues-findings.md records the row
  as returning an error; a 2026-07-25 CLI re-probe returned 7. Delete the
  GTEST_SKIP and run under both e2e link modes before doing any work.
status: possibly-stale - re-run the assertion unskipped before working it
issue: none
)CELBUG";
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0007
severity: P1
kind: wrong-value
summary: indexOf/lastIndexOf bound the pos argument by BYTE length, not code-point count
repro: 'éé'.indexOf('x', 3)
bindings: none
actual: -1
expected: an evaluation error (pos out of range)
layer: runtime/cel_string_ext_search.cc:122-133 (ValidatePos)
blocked-by: none
found-by: manual known-bugs sweep; re-confirmed 2026-07-25 through tools/cel eval
fix-hint: 'éé' is 2 code points / 4 bytes. ValidatePos bounds pos against the
  BYTE length, but cel-cpp bounds it against Size() = the code-point count, so
  pos=3 is out of range upstream and a value here. Silently wrong (a plausible
  -1 with no error), so any caller that treats -1 as "not found" gets a
  believable wrong answer on any multi-byte string. Fix by counting code
  points in ValidatePos, the same walk cel_string_size_at_v already does.
issue: none
)CELBUG";
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0008
severity: P1
kind: over-permissive
summary: %f / %e accept an int operand and render it; cel-cpp errors
repro: '%f'.format([42])
bindings: none
actual: "42.000000"
expected: an evaluation error (cel-cpp: "expected a double but got a int")
layer: runtime/cel_string_format_render.cc:277-291 (ToDouble)
blocked-by: none
found-by: manual known-bugs sweep; re-confirmed 2026-07-25 through tools/cel eval
fix-hint: cel-cpp's %f/%e accept ONLY a double or the string tokens
  NaN / Infinity. Our ToDouble additionally accepts int and renders it, so we
  are the over-permissive side. Note the paired pin CELW-0009, which is the
  opposite direction on the same helper - ToDouble also REJECTS the string
  tokens it should accept. Fix both in one edit to ToDouble: reject int,
  accept the NaN / Infinity string tokens.
issue: none
)CELBUG";
  // cel-cpp %f/%e accept only double or the string tokens NaN/Infinity; %f of
  // an int errors ("expected a double but got a int"). cel2's ToDouble
  // (cel_string_format_render.cc:277-291) accepts int and renders it.
  auto v = TryEval("'%f'.format([42])");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(v->kind(), Value::Kind::kError)
      << "%f of an int should error, kind " << static_cast<int>(v->kind());
}

TEST(KnownBugs, FormatFixedAcceptsNanToken) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0009
severity: P1
kind: missing-feature
summary: %f of the string tokens NaN / Infinity errors; cel-cpp renders them
repro: '%f'.format(['NaN'])
bindings: none
actual: an evaluation error (cel eval prints `error: invalid_argument`)
expected: the string "NaN"
layer: runtime/cel_string_format_render.cc:277-291 (ToDouble - no string-token path)
blocked-by: none
found-by: manual known-bugs sweep; re-confirmed 2026-07-25 through tools/cel eval
fix-hint: cel-cpp's %f/%e accept a double OR the literal string tokens
  NaN / Infinity, rendering the token verbatim. Our ToDouble has no
  string arm at all, so it errors. Paired with CELW-0008, which is the same
  helper being too permissive in the other direction; fix them together.
issue: none
)CELBUG";
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0010
severity: P0
kind: wrong-value
summary: exists short-circuits on an ERROR/UNKNOWN accumulator instead of absorbing it when a later element matches
repro: [0, 2].exists(x, 2/x == 1)
bindings: none
actual: an evaluation error (cel eval prints `error: divide_by_zero`)
expected: true
layer: compiler/codegen/expr_lower_comprehension.cc:642-645 (the loop-cond peephole)
blocked-by: none
found-by: manual known-bugs sweep; re-confirmed 2026-07-25 through tools/cel eval
fix-hint: `exists` desugars to `@result || pred`, so per langdef 3VL an error
  from one element is ABSORBED once a later element matches; the error only
  surfaces if NO element ever matches. Our loop-cond peephole br_if-exits on
  the accu's bool-payload BITS without checking accu.kind == CEL_BOOL, so an
  ERROR accu (a non-zero error code sitting in the bool payload slot) reads as
  "true", short-circuits the loop, and becomes the result. Gate the br_if on
  the kind, not the payload bits. P0: reachable from ordinary input, and the
  wrong answer is an error where a value was due.
issue: none
)CELBUG";
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0011
severity: P1
kind: wrong-value
summary: a duplicate map key built by transformMapEntry is last-write-wins; cel-cpp errors
repro: {1: 2, 3: 4}.transformMapEntry(k, v, {0: v})
bindings: none
actual: {0: 4}
expected: an evaluation error (duplicate map key)
layer: runtime/cel_runtime.c:157-162 (cel_map_insert_at)
blocked-by: none
found-by: manual known-bugs sweep; re-confirmed 2026-07-25 through tools/cel eval
fix-hint: comprehensions_v2 builds the result through cel.@mapInsert, which is
  MapBuilder::Put in cel-cpp (common/values/value_builder.cc) and errors on a
  duplicate key. Our cel_map_insert_at silently overwrites. Silently wrong -
  the collision is invisible in the result - so treat the severity as the
  input-rarity call, not a "loud error" call. The fix has to distinguish the
  insert-into-comprehension-accumulator path (must error) from map-literal
  construction, which has its own duplicate-key rules.
issue: none
)CELBUG";
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0012
severity: P0
kind: crash
summary: transformMapEntry with a computed (non-literal) entry expression ABORTS the compiler
repro: {1: 2, 3: 4}.transformMapEntry(k, v, k == 1 ? {k: v} : {})
bindings: none
actual: process abort. Re-confirmed 2026-07-25 through tools/cel eval: "F expr_lower_comprehension.cc:888] Check failed: false transformMapEntry: non-literal entry expression unsupported (needs runtime map-merge helper); entry kind=4" followed by a Check-failure stack trace.
expected: a value, or an absl::Status rejection - never an abort
layer: compiler/codegen/expr_lower_comprehension.cc:888 (originally cited as :800-805; the ABSL_CHECK has moved)
blocked-by: none
found-by: manual, via the cel CLI; re-confirmed 2026-07-25
fix-hint: the entry arm only handles a literal kMapExpr and ABSL_CHECK(false)s
  on anything else, so a ternary (or any computed entry) takes the whole
  process down. Two acceptable fixes: emit the runtime map-merge helper the
  check message names, or - as a strictly smaller first step - reject the
  shape at the frontend with an InvalidArgument status so a bad expression
  can never reach codegen. DO NOT delete the GTEST_SKIP before the fix lands:
  running this case unskipped aborts the whole test binary, taking every
  other case in it with it.
issue: none
)CELBUG";
  auto v =
      TryEval("{1: 2, 3: 4}.transformMapEntry(k, v, k == 1 ? {k: v} : {})");
  EXPECT_TRUE(v.ok()) << "should return a value/status, not crash: "
                      << v.status();
}

// ══════════════════════════════════════════════════════════════════
// CLASS: timestamp range + value formatting (wave 2).
// ══════════════════════════════════════════════════════════════════

TEST(KnownBugs, MaxRangeTimestampConstruction) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0013
severity: P1
kind: wrong-value
summary: the maximum in-range timestamp is rejected because any positive nanos at the max second overflows
repro: string(timestamp('9999-12-31T23:59:59.999999999Z'))
bindings: none
actual: CEL_ERR_OVERFLOW at construction (as recorded when the pin was written). Re-probed 2026-07-25 through tools/cel eval, which returned the EXPECTED string - so this pin may be STALE.
expected: "9999-12-31T23:59:59.999999999Z"
layer: runtime/cel_time_parse.cc:178 (the `seconds == MAX && nanos > 0` guard) and runtime/cel_time.c:46
blocked-by: none
found-by: conformance corpus spec/tests/simple/testdata/timestamps.textproto (max timestamp row)
fix-hint: cel-cpp's MaxTimestamp is seconds 253402300799 PLUS nanos 999999999,
  so positive nanos at the max second are in range; our guard rejects any
  nanos > 0 there. Before working this, delete the GTEST_SKIP and run the
  assertion under both e2e link modes - a 2026-07-25 CLI re-probe already
  returns the expected string, so the pin may simply be closable.
status: possibly-stale - re-run the assertion unskipped before working it
issue: none
)CELBUG";
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

// e2e/fuzz found (string seed=113, M30 string-functions grammar):
// `string(4294967295.0)` renders the SCIENTIFIC form
// "4.294967295e+09" where the conformant shortest-round-trip is the
// FIXED form "4294967295".  `std::chars_format::general` must choose
// fixed vs scientific by output length (fixed preferred): fixed is
// 10 chars, scientific 15, so fixed wins — cel-cpp's FormatDouble
// (same to_chars call) produces "4294967295".  Our wasm libc++
// `to_chars(double, general)` (cel_convert_double_format.cc) emits
// scientific here instead — a toolchain-level non-conformance.
// (Asserted directly, not via the differential oracle: the oracle's
// cel-cpp build lacks <charconv> double-to-chars and falls back to
// %.17g, so it cannot answer double-format questions — which is why
// `string(double)` is excluded from the fuzz grammar.)  Sits beside
// DoubleToStringShortestRoundTrip / DoubleToStringExponentForm,
// which pin the cases that already work.
TEST(KnownBugs, PbtStringDoubleScientificForm) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0014
severity: P2
kind: wrong-value
summary: string(double) picks the scientific form where the fixed form is shorter
repro: string(4294967295.0)
bindings: none
actual: "4.294967295e+09"
expected: "4294967295"
layer: runtime/cel_convert_double_format.cc (the std::to_chars(general) call, as built for wasm32-wasi)
blocked-by: none
found-by: e2e/fuzz string seed=113 (M30 string-functions grammar); re-confirmed 2026-07-25 through tools/cel eval
fix-hint: std::chars_format::general must choose fixed vs scientific by output
  LENGTH, preferring fixed on a tie - fixed is 10 chars here, scientific 15,
  so fixed wins, and cel-cpp's FormatDouble (the same to_chars call) produces
  "4294967295". Our wasm libc++ to_chars emits scientific instead, i.e. this
  is a toolchain-level non-conformance rather than a bug in our kernel;
  confirm against the wasi-sdk libc++ before rewriting our formatter. This
  case cannot be settled through the differential oracle: the oracle's cel-cpp
  build lacks <charconv> double-to-chars and falls back to %.17g, which is why
  string(double) is excluded from the fuzz grammar. Sits beside
  DoubleToStringShortestRoundTrip / DoubleToStringExponentForm, which pin the
  cases that already work.
issue: none
)CELBUG";
  auto v = TryEval("string(4294967295.0)");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsString(), "4294967295");
}

TEST(KnownBugs, OptionalSelectOnMapRejected) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0015
severity: P1
kind: missing-feature
summary: the static subset wrongly rejects the whole .?field optional-select syntax
repro: {'a': 1}.?a.value()
bindings: none
actual: a compile rejection. Re-confirmed 2026-07-25 through tools/cel eval: "ERROR: expression is not in the static subset: expr id=6 is dyn (no type_map entry)".
expected: 1
layer: compiler/frontend/parse_and_check.cc:631-641 (RejectDyn)
blocked-by: none
found-by: conformance corpus (optionals rows); re-confirmed 2026-07-25 through tools/cel eval
fix-hint: cel-cpp's HandleOptSelect ERASES the type of the synthetic
  field-name argument (checker/type_checker_impl.cc:1168). RejectDyn then
  recurses into that untyped child and flags "no type_map entry", which blocks
  the entire `.?` syntax rather than any genuinely dynamic expression. The fix
  is to teach RejectDyn to skip the synthetic field-name child of an optional
  select, not to relax the dyn gate generally.
issue: none
)CELBUG";
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0016
severity: P1
kind: missing-feature
summary: double(<string>) does not strip surrounding ASCII whitespace, so a padded numeral errors
repro: double('  3.14  ')
bindings: none
actual: an evaluation error. Re-confirmed 2026-07-25 through tools/cel eval, which prints `error: overflow`.
expected: 3.14
layer: runtime/cel_convert.c:350-362 (parse_double_str)
blocked-by: none
found-by: conformance corpus (conversions rows); re-confirmed 2026-07-25 through tools/cel eval
fix-hint: cel-cpp's double(string) goes through absl::SimpleAtod, which strips
  surrounding ASCII whitespace before parsing; parse_double_str does not skip
  leading or trailing space. The strip itself is trivial. The ORIGINAL pin
  message also claimed parse_double_str was 1 ULP off on 3.14
  (3.1399999999999997) and tied this to CELW-0003; a 2026-07-25 re-probe
  showed `double("3.14")` returning exactly 3.14, so that half of the claim
  looks stale - but CELW-0003 (double of a 21-digit decimal string) still
  reproduces, so the correctly-rounded-parse work is real and this pin should
  be closed alongside it. Distinct from the documented hex-float gap
  (m10-conversions.md:649), which is left as known.
issue: none
)CELBUG";
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
// CLASS: large-input ceilings surfaced by the in-operator benches
// (now `benchmark/eval` corpus lists cells +
// `benchmark/compiler/in_operator_compile_bench.cc`).
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
// Surfaced by the in-operator compile benches (now
// `benchmark/compiler/in_operator_compile_bench.cc`) — a literal
// `[0..999_999]` source (~7.9 MB) is rejected at parse.
// ──────────────────────────────────────────────────────────────────
TEST(KnownBugs, ParserSourceCodepointLimitNotConfigurable) {
  GTEST_SKIP() << R"CELSKIP(CELSKIP v1
reason: deferred-feature
why-not-a-bug: this is an unexposed configuration knob, not a miscompile.
  cel-cpp's parser caps source at 100 000 codepoints
  (expression_size_codepoint_limit, third_party/cel-cpp/parser/options.h:37);
  our DefaultParserOptions (compiler/frontend/parse_and_check.cc:1079) leaves
  the upstream default in place and CompilerOptions exposes no override. The
  rejection is LOUD and correct-shaped - verified INVALID_ARGUMENT
  "expression size exceeds codepoint limit" - so nothing miscompiles; a
  110 k-codepoint source is simply refused. Un-skip when the cap is raised or
  surfaced through CompilerOptions.
citation: doc/implementation-plan/cleanup-backlog.md #15; third_party/cel-cpp/parser/options.h:37
)CELSKIP";
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
// FIXED (cleanup-backlog #16, formerly **P0**): literal
// `x in [0..9999]` used to compile + plan cleanly and then PANIC
// wasmtime at Eval (`store.rs:2440 assertion failed: fault.is_none()`,
// Rust abort).  Root cause: neither Compile arm validated the
// expression's static region against the (then 8 KiB) low-memory
// window, so the ~240 KB rodata clobbered the runtime's static data in
// the shared memory.  Two changes closed it: the compile-time
// static-region gate (`ValidateExprStaticRegion`), and the m31 §10
// window raise to 256 KiB — which makes a 10 K-element literal list a
// legitimate, materialized expression that evaluates correctly (the
// original "OK + true" aspiration).  The exact rodata-window boundary
// (10909 / 10910 elements) is pinned in `e2e/limits_test.cc`.
// ──────────────────────────────────────────────────────────────────
TEST(KnownBugs, LiteralIntListInScan10KEvals) {
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
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool());  // kN-2 is a member of [0..kN-1]
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
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0017
severity: P1
kind: missing-feature
summary: `x in xs` over a bound list of 10 k 50-byte strings exhausts the per-Eval arena mid-scan
repro: perm in perms
bindings: perm = <a 50-byte string>, perms = a bound list<string> of 10 000 unique 50-byte strings (the last of which is `perm`)
actual: FAILED_PRECONDITION "arena OOM in CelMapLookupImpl", raised from inside cel_list_in's trampoline
expected: OK, with the value true
layer: runtime/cel_layout.h:16 (the per-Eval arena) plus the cel_list_in scan path in eval/internal/cel_host.cc
blocked-by: none
found-by: the in-operator eval benches (benchmark/eval corpus lists cells)
fix-hint: this is a graceful error, not a crash, and it is distinct from the
  literal-list case (which was fixed by the rodata-window raise): the source
  list here is a BOUND variable, so the OOM happens during the scan, not
  during list construction. Two independent fixes would each close it - stop
  cel_list_in materialising O(N) arena state per scan, or let the arena grow
  on demand. Production CEL-policy / IAM workloads with large permission sets
  hit this shape directly. Sits adjacent to the host-origin aggregate family
  (operations that must reach INSIDE an activation-bound aggregate); if that
  family is reworked, re-measure this before working it separately.
issue: none
)CELBUG";
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
// primes.  Same shape as the `benchmark/eval` corpus long-arith
// cells (formerly `bench/in_operator_bench.cc`'s
// `BM_Eval_LongArith_*Terms`).
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
// (`parse_and_check.cc::DefaultParserOptions`: 32 → the
// expression-depth-gate-aligned limit) plus the codegen / Eval path
// actually work for a long-but-not-pathological `+`-chain.  With the
// LIFO free-list slot reuse in
// `SlotAllocator::Release`, a left-associative `+`-chain reuses a
// handful of workspace cells regardless of length, so this size — and
// the 2000-term sibling below — both fit comfortably inside the
// 256 KiB reserved window.
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
// cells, so the static region stays well inside the 256 KiB window
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
// REGRESSION (cleanup-backlog #45): deep left-associative chains.
// Codegen emits an N-term chain as an N-deep nested wasm expression
// tree, and both the host compiler and wasmtime walk that tree
// recursively — one native stack frame per nesting level — so past
// depth ≈4.6k the walk overflowed the ~8 MiB native stack: a SIGSEGV
// on valid CEL.  Verified boundary pre-fix (2026-06-10): N=4653
// compiled + evaled fine, N=4654 segfaulted at Plan time (wasmtime
// validation / Cranelift JIT walking the deep tree).
//
// Interim fix: the expression-depth gate in
// `compiler/frontend/parse_and_check.cc`
// (`kMaxExpressionNestingDepth` + the parser-limit alignment) rejects
// over-deep expressions at Compile with ResourceExhausted, in both
// link modes.  The real fix — flattening codegen so expression-tree
// depth is O(1) for any N — remains open under #45(b); when it
// lands, the rejection assertion below flips back to a value check.
// ──────────────────────────────────────────────────────────────────
TEST(KnownBugs, DeepArithChainFormerSegvBoundaryRejectedAtCompile) {
  constexpr int kN = 4654;  // smallest N that SIGSEGV'd pre-fix
  const std::string source = MakeLongArithSource(kN);
  auto v = TryEvalActivated(source, DeclareLongArithVars, BindLongArithVars);
  // Graceful compile-time rejection — NOT a SIGSEGV, NOT a pass.
  ASSERT_FALSE(v.ok()) << "4654-term chain unexpectedly evaluated — only "
                          "remove the depth gate once codegen flattening "
                          "(cleanup-backlog #45(b)) has landed";
  EXPECT_EQ(v.status().code(), absl::StatusCode::kResourceExhausted)
      << v.status();
}

// Headline bench case (the `benchmark/eval` corpus long-arith cell,
// formerly `bench/in_operator_bench.cc`'s
// `BM_Eval_LongArith_10kTerms`, kTerms=1000): a 1000-term chain must
// stay comfortably inside the depth gate, compiling AND evaluating
// to its value in both link modes.
TEST(KnownBugs, LongArith1000TermsStillEvalsUnderDepthGate) {
  constexpr int kN = 1000;
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

void DeclareScalarPbtVars(Compiler::Builder& b) {
  b.DeclareVariable("i_a", CelType::Int());
  b.DeclareVariable("i_b", CelType::Int());
  b.DeclareVariable("b_a", CelType::Bool());
}

void BindScalarPbtVars(Activation& a) {
  a.Bind("i_a", Value::Int(7));
  a.Bind("i_b", Value::Int(11));
  a.Bind("b_a", Value::Bool(true));
}

// Slice C PBT activation — Slice B vars plus uint / double / string
// / bytes leaves the full grammar pulls from.  Mirrors
// `e2e/fuzz/oracle_harness.cc::BoundActivationEntries` so a repro
// the miner shrunk runs unchanged here.
void DeclareFullPbtVars(Compiler::Builder& b) {
  b.DeclareVariable("i_a", CelType::Int());
  b.DeclareVariable("i_b", CelType::Int());
  b.DeclareVariable("u_a", CelType::Uint());
  b.DeclareVariable("d_a", CelType::Double());
  b.DeclareVariable("b_a", CelType::Bool());
  b.DeclareVariable("s_a", CelType::String());
  b.DeclareVariable("y_a", CelType::Bytes());
}

void BindFullPbtVars(Activation& a) {
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
  auto v = TryEvalActivated(source, pbt_repro::DeclareScalarPbtVars,
                            pbt_repro::BindScalarPbtVars);
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
  auto v = TryEvalActivated(source, pbt_repro::DeclareFullPbtVars,
                            pbt_repro::BindFullPbtVars);
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
  auto v = TryEvalActivated(source, pbt_repro::DeclareFullPbtVars,
                            pbt_repro::BindFullPbtVars);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBytes) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsBytes(), "hix");
}

// PBT int seed=137 (full grammar, 2026-06-05) — reduced form
// of the `size((cond ? <bytes-ternary> : <bytes-ternary>))` shape
// that was producing kError because the inner bytes ternary's
// result kind was being read from the wrong slot.  Spec eval: the
// exists_one is true (one key, predicate true), inner ternary's
// then-arm is `(y_a + y_a)` = b"hihi" (size 4).
TEST(KnownBugs, PbtSizeOfExistsOneTernaryBytes) {
  constexpr absl::string_view source =
      R"(size((({"k": 1}).exists_one(k, true) ? (y_a + y_a) : (y_a + b"x"))))";
  auto v = TryEvalActivated(source, pbt_repro::DeclareFullPbtVars,
                            pbt_repro::BindFullPbtVars);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), 4);
}

// PBT list_string seed=104 (M30.D string-functions grammar,
// 2026-06-11).  `split` on a COMPUTED receiver returned garbage
// memory bytes: `('hello' + 'hello').split('é')` produced a list
// whose only element was the result list's own ArenaListHeader
// bytes (`\x01\x00\x00\x00…`) instead of `"hellohello"`.  Root
// cause: the compiler assigns the split's output the same workspace
// slot as its computed-string input (slot reuse), and
// `cel_string_ext_list.cc::DoSplit` re-read `s->payload.s.ptr`
// AFTER `AllocList(out, …)` had stamped a CEL_LIST_ARENA header over
// that shared slot.  Fixed by capturing `source_ptr` before
// AllocList.  Literal receivers never alias the output slot, which
// is why the CLI literal case (`'hellohello'.split('é')`) always
// worked and only the differential fuzzer caught it.  Live guard.
TEST(KnownBugs, PbtSplitComputedReceiverSlotAlias) {
  // Assert through CEL scalars (size / index) so the test needs no
  // HostListBacking access: both reach into the split result, which
  // is where the garbage surfaced.
  auto sz = TryEval(R"(size(("hello" + "hello").split("é")))");
  ASSERT_TRUE(sz.ok()) << sz.status();
  ASSERT_EQ(sz->kind(), Value::Kind::kInt) << static_cast<int>(sz->kind());
  EXPECT_EQ(*sz->AsInt(), 1);

  auto el = TryEval(R"((("hello" + "hello").split("é"))[0])");
  ASSERT_TRUE(el.ok()) << el.status();
  ASSERT_EQ(el->kind(), Value::Kind::kString) << static_cast<int>(el->kind());
  EXPECT_EQ(*el->AsString(), "hellohello");
}

// PBT int (M30 temporal grammar, 2026-06-11).  Our compiler accepts
// `int(duration(...))` and returns the duration's whole-second count,
// but cel-cpp REJECTS it at type-check ("No matching overloads
// found"): CEL's `int()` converts bool / double / int / string /
// uint / timestamp, NOT duration (cel-cpp
// runtime/standard/type_conversion_functions.cc::RegisterIntConversionFunctions).
// We over-accept because `overload_table.cc` carries a
// `duration_to_int64` seed and our checker admits the overload.
// Harmless to valid programs (a superset), but non-conformant —
// found by e2e/fuzz mining the `int` target.  Fix: drop the
// duration->int overload from the checker + overload_table; then
// `int(duration(...))` rejects at compile and this assertion holds.
TEST(KnownBugs, PbtIntOfDurationOverPermissive) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0001
severity: P1
kind: over-permissive
summary: we accept int(<duration>); cel-cpp rejects it as no-such-overload
repro: int(duration("3661s"))
actual: 3661
expected: a compile-time rejection (no such CEL overload)
layer: compiler/codegen/overload_table.cc (drop the duration_to_int64 seed) + the checker decl
blocked-by: none
found-by: e2e/fuzz mining the int target (2026-06-11)
fix-hint: CEL's int() takes int/uint/double/string/timestamp, NOT duration
  (cel-cpp runtime/standard/type_conversion_functions.cc,
  RegisterIntConversionFunctions). We over-accept because overload_table.cc
  carries a duration_to_int64 seed and our checker admits it. Harmless to
  valid programs (a superset) but non-conformant. This is the mirror of
  CELW-0002 — fix both together, they touch the same table.
issue: none
)CELBUG";
  auto v = TryEval(R"(int(duration("3661s")))");
  EXPECT_FALSE(v.ok())
      << "int(duration) should be rejected (no such CEL overload), got "
      << (v.ok() ? "a value" : v.status().ToString());
}

// PBT list_int (m36 slice 3, 2026-07-25).  FIXED — kept as the
// regression guard.  List concatenation used to be broken for every
// HOST-ORIGIN operand: `[1,2] + [3,4]` (both arena-built literals)
// worked, but the moment either side was an activation-bound list the
// result was a CEL error, where cel-cpp returns the concatenation:
//
//     [1, 2] + [3, 4]      -> ok (list)          <- arena + arena
//     [1, 2] + xs          -> CEL-ERROR          <- arena + host
//     xs + [1, 2]          -> CEL-ERROR          <- host  + arena
//     xs + ys              -> CEL-ERROR          <- host  + host
//
// Found by e2e/fuzz the first time `add_list` was registered as a
// FIRST-CLASS production (m36 slice 2).  It had been "covered" for
// a year only through `filter`/`map` macro expansion, whose
// accumulator concatenates two arena lists — the one origin
// combination that worked.  Oracle: `([1, 2, 3] + xs)` with
// xs=[1,2,3] returns a 6-element list (mining seed 36, list_int d4).
//
// Fix: `CelListConcatImpl` (eval/internal/cel_host.cc) — the
// trampoline the dispatcher already routed every non-arena pairing to
// — now LIFTS both operands into one fresh arena list instead of
// poisoning TYPE_MISMATCH.  The concat production is back in the
// fuzz grammar (RegisterAggregateOperators in grammar_aggregates.cc).
TEST(KnownBugs, PbtListConcatHostOriginPoisons) {
  // Every origin pairing, asserted down to the element values —
  // "it's a list" was what let the arena+arena path masquerade as
  // working coverage for a year.
  auto declare = [](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
    b.DeclareVariable("ys", CelType::List(CelType::Int()));
  };
  auto bind = [](Activation& a) {
    a.Bind("xs", Value::List({Value::Int(3), Value::Int(4)}));
    a.Bind("ys", Value::List({Value::Int(5)}));
  };
  struct Case {
    const char* source;
    std::vector<int64_t> want;
  };
  const std::vector<Case> cases = {
      {"[1, 2] + [3, 4]", {1, 2, 3, 4}},  // arena + arena
      {"[1, 2] + xs", {1, 2, 3, 4}},      // arena + host
      {"xs + [1, 2]", {3, 4, 1, 2}},      // host  + arena
      {"xs + ys", {3, 4, 5}},             // host  + host
      {"xs + []", {3, 4}},                // host  + empty arena
      {"[] + xs", {3, 4}},                // empty arena + host
  };
  for (const Case& c : cases) {
    auto v = TryEvalActivated(c.source, declare, bind);
    ASSERT_TRUE(v.ok()) << c.source << ": " << v.status();
    ASSERT_EQ(v->kind(), Value::Kind::kList)
        << c.source << ": " << static_cast<int>(v->kind());
    auto backing = v->ListBacking();
    ASSERT_TRUE(backing.ok()) << c.source << ": " << backing.status();
    ASSERT_EQ((*backing)->Size(), c.want.size()) << c.source;
    for (size_t i = 0; i < c.want.size(); ++i) {
      auto elt = (*backing)->At(i, CelType::Int());
      ASSERT_TRUE(elt.ok()) << c.source << " [" << i << "]: " << elt.status();
      auto got = elt->AsInt();
      ASSERT_TRUE(got.ok()) << c.source << " [" << i << "]: " << got.status();
      EXPECT_EQ(*got, c.want[i]) << c.source << " [" << i << "]";
    }
  }
}

// Sibling of the concat bug above, same root cause (an operation that
// must MATERIALISE a result could only read arena-backed lists), found
// by probing every aggregate op against a bound operand:
//
//     xs.join("-")   -> CEL-ERROR (type_mismatch)
//     xs.join()      -> CEL-ERROR (type_mismatch)
//
// Fix: `cel_string_join{,_sep}` resolve their operand through
// `cel_list_arena_view`, which snapshots a host-backed list into the
// arena (the same lift the comprehension prologue already used) before
// the join walk.
TEST(KnownBugs, PbtListJoinHostOriginPoisons) {
  auto declare = [](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::String()));
    b.DeclareVariable("empty", CelType::List(CelType::String()));
  };
  auto bind = [](Activation& a) {
    a.Bind("xs", Value::List({Value::String("a"), Value::String("b")}));
    a.Bind("empty", Value::List({}));
  };
  const std::vector<std::pair<const char*, const char*>> cases = {
      {R"(xs.join("-"))", "a-b"},
      {"xs.join()", "ab"},
      {R"(empty.join("-"))", ""},
      {"empty.join()", ""},
      {R"((xs + ["c"]).join("-"))", "a-b-c"},
  };
  for (const auto& [source, want] : cases) {
    auto v = TryEvalActivated(source, declare, bind);
    ASSERT_TRUE(v.ok()) << source << ": " << v.status();
    auto got = v->AsString();
    ASSERT_TRUE(got.ok()) << source << ": " << got.status();
    EXPECT_EQ(*got, want) << source;
  }
}

// PBT list_string (m36 slice 4, 2026-07-25).  `double(<string>)`
// rounds the WRONG WAY by one ULP for large magnitudes.
//
//   double("777777777777777777777")
//     ours    777777777777777704960
//     correct 777777777777777836032   (cel-cpp, and strtod/Python)
//
// The two differ by exactly 131072 == 2^17, which is one ULP at
// this magnitude (the value sits between 2^69 and 2^70, so the
// double spacing is 2^(70-53) = 2^17).  So our decimal->double
// conversion truncates toward zero where IEEE-754 requires
// round-to-nearest.  21 significant digits is past the 17 a double
// can round-trip, which is exactly where a naive accumulate-then-
// scale parse loses the tie-break.
//
// Found by mining `list_string` d4 seed 11, inside
// `"%f".format([double("%o".format([i_max]))])` — the octal
// rendering of INT64_MAX is a 21-digit numeric string, which is how
// the fuzzer reached a magnitude the small numeric-string leaves
// never do.  The divergence was invisible until the comparator
// learned to escape non-printables (the same source carries an
// embedded NUL, which truncated the rendered value so both sides
// printed identically).
TEST(KnownBugs, PbtDoubleFromLargeDecimalStringOffByOneUlp) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0003
severity: P1
kind: precision
summary: double(<21-digit string>) rounds one ULP low
repro: double("777777777777777777777")
actual: 777777777777777704960
expected: 777777777777777836032
layer: the string->double parse behind string_to_double (runtime conversion path)
blocked-by: none
found-by: e2e/fuzz mine_divergences list_string seed=11 depth=4
fix-hint: The two values differ by exactly 131072 == 2^17, which is one ULP
  at this magnitude (the value lies between 2^69 and 2^70, so double spacing
  is 2^(70-53)). We truncate toward zero where IEEE-754 requires
  round-to-nearest-even. 21 significant digits is past the 17 a double can
  round-trip, which is where a naive accumulate-then-scale parse loses the
  tie-break; use a correctly-rounded decimal->binary conversion. Severity is
  P1 not P0 because the error is in the last representable bit, not a
  user-visible wrong answer, but it IS a silent conformance divergence.
issue: none
)CELBUG";
  auto v = TryEval(R"(double("777777777777777777777"))");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kDouble);
  EXPECT_EQ(*v->AsDouble(), 777777777777777836032.0);
}

// PBT duration (m36 slice 2, 2026-07-25).  `duration(<int>)` — the
// MIRROR of PbtIntOfDurationOverPermissive above.  We compile it and
// return that many seconds (`duration(1)` -> "1s"); cel-cpp's
// RUNTIME errors "No matching overloads found : duration(int64)".
//
// The subtle part, and why reading cel-cpp source alone would have
// gotten this wrong: cel-cpp's CHECKER *does* declare the overload
// (`StandardOverloadIds::kIntToDuration` is added to the `duration`
// FunctionDecl in checker/standard_library.cc:369), so a source
// grep says "int->duration is standard CEL".  Its runtime never
// registers an implementation, so the call type-checks and then
// fails at evaluation.  cel-cpp is internally inconsistent here;
// conformance is scored against the runtime, so OUR acceptance is
// the non-conformant side.  (`timestamp(<int>)` is NOT affected —
// its runtime impl exists, and the fuzzer mines it clean.)
//
// Found by e2e/fuzz mining the `duration` target at depth 4
// (seed 4) immediately after the m36 slice-2 conversion
// productions landed; reduced to `duration(1)`.  The production is
// withheld from the grammar (like int(duration)) so the nightly
// does not diverge on a bug we already track.  Fix: drop the
// int64_to_duration overload from our checker + overload_table, or
// wait for upstream to add the runtime impl and re-confirm with
// the oracle.
TEST(KnownBugs, PbtDurationFromIntOverPermissive) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0002
severity: P1
kind: over-permissive
summary: we accept duration(<int>); cel-cpp's runtime rejects it
repro: duration(1)
actual: a duration value of 1s
expected: an evaluation error (cel-cpp: "No matching overloads found : duration(int64)")
layer: compiler/codegen/overload_table.cc (drop the int64_to_duration seed) + the checker decl
blocked-by: none
found-by: e2e/fuzz mine_divergences duration seed=4 depth=4
fix-hint: cel-cpp is internally inconsistent here — its CHECKER declares
  the overload (checker/standard_library.cc:369, kIntToDuration) but its
  runtime never registers an implementation, so the call type-checks and
  then fails at eval. Conformance is scored against the runtime, so OUR
  acceptance is the non-conformant side. Remove the overload from our
  checker + overload_table so it rejects at compile time. NOTE the
  asymmetry: timestamp(<int>) IS declared AND implemented upstream, mines
  clean, and must keep working.
issue: none
)CELBUG";
  auto v = TryEval("duration(1)");
  EXPECT_FALSE(v.ok())
      << "duration(int) should be rejected (cel-cpp's runtime has no such "
      << "overload), got " << (v.ok() ? "a value" : v.status().ToString());
}

// PBT int (M30 string grammar, 2026-06-11).  Two-arg
// `<string>.substring(start, end)` with `end == size()` (and
// `start != end`) returns the tail slice in our pipeline but is a
// CEL ERROR in cel-cpp.  Oracle-confirmed (SubstringProbe, since
// removed): `'hello'.substring(0, 5)`, `'hello'.substring(1, 5)`,
// and the fuzz repro `'false'.substring(1, 5)` all error in cel-cpp
// with "<start> or <end> is greater than <string>.size()", while
// `(0, 4)` and `(5, 5)` are values.  Root cause is upstream: in
// `third_party/cel-cpp/common/values/string_value.cc` SubstringImpl
// (~line 705) the `size_code_points == end` early-return only runs
// at the top of the codepoint loop, and the loop exits the moment
// the string is consumed — so an `end` equal to the full codepoint
// count is never matched, and the post-loop fallback only accepts
// `start == end`.  The public `StringValue::Substring` bounds-check
// (line 773) uses BYTE size with `>`, so `end == size` passes the
// guard and reaches the buggy slice.  We are the more-permissive
// (arguably more-correct) side, but conformance scores against
// cel-cpp.  Found by e2e/fuzz mining the `int` target (substring
// nested under lastIndexOf).  Two-arg substring is withheld from the
// fuzz grammar (grammar_scalars.cc) so mining stays clean.  Fix
// direction (to match the oracle): error when `end == size() &&
// start != end`; but matching a likely upstream off-by-one is a
// judgment call — left as a pin.
TEST(KnownBugs, PbtSubstringEndEqualsSizeOverPermissive) {
  GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0018
severity: P1
kind: over-permissive
summary: two-arg substring(start, end) with end == size() returns the tail slice; cel-cpp errors
repro: "false".substring(1, 5)
bindings: none
actual: "alse"
expected: an evaluation error (cel-cpp: "<start> or <end> is greater than <string>.size()")
layer: runtime/cel_string_ext (the two-arg substring kernel)
blocked-by: none
found-by: e2e/fuzz mining the int target (substring nested under lastIndexOf); oracle-confirmed via the since-removed SubstringProbe
fix-hint: the root cause is UPSTREAM. In
  third_party/cel-cpp/common/values/string_value.cc SubstringImpl (~line 705)
  the `size_code_points == end` early-return only runs at the top of the
  codepoint loop, and the loop exits the moment the string is consumed, so an
  `end` equal to the full codepoint count is never matched and the post-loop
  fallback only accepts start == end. The public StringValue::Substring
  bounds-check (line 773) uses BYTE size with `>`, so end == size passes the
  guard and reaches the buggy slice. Oracle: 'hello'.substring(0, 5),
  'hello'.substring(1, 5) and this repro all ERROR upstream, while (0, 4) and
  (5, 5) are values. We are the more-permissive - arguably more correct -
  side, but conformance scores against cel-cpp. Matching a likely upstream
  off-by-one is a judgment call, which is why this is pinned rather than
  fixed; two-arg substring is withheld from the fuzz grammar
  (compiler/../grammar_scalars.cc) so mining stays clean.
issue: none
)CELBUG";
  auto v = TryEval(R"("false".substring(1, 5))");
  EXPECT_FALSE(v.ok())
      << "substring(start, size) with start != size should error (matching "
         "cel-cpp), got "
      << (v.ok() ? "a value" : v.status().ToString());
}

// PBT int (M30 INT64_MIN leaf, 2026-06-11) — FIXED.  `INT64_MIN % -1`
// returned 0 in our runtime (cel_int_mod_at_vv) on a wrong "cel-cpp
// returns 0" assumption; cel-cpp ERRORS with integer overflow
// (CheckedMod, third_party/cel-cpp/internal/overflow.cc — the implied
// division INT64_MIN/-1 overflows).  Oracle-confirmed when the exact
// INT64_MIN leaf was added to the differential grammar.  Fixed to
// poison CEL_ERR_OVERFLOW, mirroring divide.  Live regression below;
// kernel-level pin in runtime/cel_arith_test.cc::IntModIntMinByNegOne-
// Poisons.
TEST(KnownBugs, PbtModuloInt64MinByNegOneOverflows) {
  // The overflow surfaces as a CEL error VALUE (3VL kError), not a
  // failed status — eval succeeds, the result is an error.
  auto v = TryEval(R"((-9223372036854775807 - 1) % (-1))");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError())
      << "INT64_MIN % -1 should be an overflow error (matching cel-cpp), got "
         "value kind "
      << static_cast<int>(v->kind());
}

// ──────────────────────────────────────────────────────────────────
// HOST-ORIGIN AGGREGATE FAMILY (2026-07-25 unimplemented sweep).
//
// Siblings of the concat/join pair above, same root shape: an
// operation that has to look INSIDE an aggregate could only read
// arena-built (literal) operands, and expressed "no host arm yet"
// as a `return false` or a `poison(TYPE_MISMATCH)` rather than a
// loud check.  Three of the five returned a plausible WRONG VALUE
// with no error, which is the most severe class — `xss == xss`
// returned `false`, violating reflexivity.
//
// The shared declare/bind fixture below is reused by every case so
// the origin axis (arena literal vs `Activation::Bind`) is the only
// thing that varies.
// ──────────────────────────────────────────────────────────────────

void DeclareHostAggregates(Compiler::Builder& b) {
  b.DeclareVariable("xss", CelType::List(CelType::List(CelType::Int())));
  b.DeclareVariable("yss", CelType::List(CelType::List(CelType::Int())));
  b.DeclareVariable("zss", CelType::List(CelType::List(CelType::Int())));
  b.DeclareVariable("empty_ss", CelType::List(CelType::List(CelType::Int())));
  b.DeclareVariable("mins", CelType::List(CelType::List(CelType::Int())));
  b.DeclareVariable("xs", CelType::List(CelType::Int()));
  b.DeclareVariable("empty_xs", CelType::List(CelType::Int()));
  b.DeclareVariable(
      "mms", CelType::List(CelType::Map(CelType::String(), CelType::Int())));
  b.DeclareVariable(
      "mm", CelType::Map(CelType::String(), CelType::List(CelType::Int())));
  b.DeclareVariable(
      "nn", CelType::Map(CelType::String(), CelType::List(CelType::Int())));
  b.DeclareVariable(
      "pp", CelType::Map(CelType::String(), CelType::List(CelType::Int())));
  b.DeclareVariable("empty_mm", CelType::Map(CelType::String(),
                                             CelType::List(CelType::Int())));
  b.DeclareVariable(
      "deep_m", CelType::Map(CelType::String(),
                             CelType::Map(CelType::String(), CelType::Int())));
  b.DeclareVariable("ms", CelType::Map(CelType::String(), CelType::Int()));
}

constexpr int64_t kInt64Min = -9223372036854775807LL - 1;

void BindHostAggregates(Activation& a) {
  auto list12 = [] {
    return Value::List({Value::Int(1), Value::Int(2)});
  };
  a.Bind("xss", Value::List({list12()}));
  a.Bind("yss", Value::List({list12()}));
  a.Bind("zss", Value::List({Value::List({Value::Int(1), Value::Int(3)})}));
  a.Bind("empty_ss", Value::List({}));
  a.Bind("mins", Value::List({Value::List({Value::Int(kInt64Min)})}));
  a.Bind("xs", list12());
  a.Bind("empty_xs", Value::List({}));
  a.Bind("mms",
         Value::List({Value::Map({{Value::String("a"), Value::Int(1)}})}));
  a.Bind("mm",
         Value::Map({{Value::String("a"), Value::List({Value::Int(1)})}}));
  a.Bind("nn",
         Value::Map({{Value::String("a"), Value::List({Value::Int(1)})}}));
  a.Bind("pp",
         Value::Map({{Value::String("a"), Value::List({Value::Int(2)})}}));
  a.Bind("empty_mm", Value::Map({}));
  a.Bind("deep_m",
         Value::Map({{Value::String("a"),
                      Value::Map({{Value::String("b"), Value::Int(7)}})}}));
  a.Bind("ms", Value::Map({{Value::String("a"), Value::Int(1)}}));
}

absl::StatusOr<Value> EvalHostAggregate(absl::string_view source) {
  return TryEvalActivated(source, DeclareHostAggregates, BindHostAggregates);
}

// Assert `source` evaluates to the boolean `want`.
void ExpectBool(absl::string_view source, bool want) {
  auto v = EvalHostAggregate(source);
  ASSERT_TRUE(v.ok()) << source << ": " << v.status();
  auto got = v->AsBool();
  ASSERT_TRUE(got.ok()) << source << ": " << got.status() << " (kind "
                        << static_cast<int>(v->kind()) << ")";
  EXPECT_EQ(*got, want) << source;
}

// F1 — list equality over host-origin operands with AGGREGATE
// elements returned `false` for every pairing, including `xss == xss`.
// Root: `EncodeBackingScalar` (eval/internal/cel_host.cc) encoded every
// aggregate element to a `CEL_ERROR{TYPE_MISMATCH}` placeholder, and
// `HostScalarSameKindEq`'s `default:` then compared two placeholders
// unequal.  Arena twins (`[[1,2]] == [[1,2]]`) were always correct, so
// the delta was purely operand origin.
TEST(KnownBugs, PbtListEqNestedAggregateHostOrigin) {
  struct Case {
    const char* source;
    bool want;
  };
  const std::vector<Case> cases = {
      {"[[1, 2]] == [[1, 2]]", true},   // arena + arena (baseline)
      {"xss == [[1, 2]]", true},        // host  + arena
      {"[[1, 2]] == xss", true},        // arena + host
      {"xss == yss", true},             // host  + host
      {"xss == xss", true},             // reflexivity
      {"xss == zss", false},            // nested + different
      {"xss == [[1, 3]]", false},       // nested + different (arena rhs)
      {"xss == [[1, 2], [3]]", false},  // different length
      {"empty_ss == []", true},         // empty aggregate
      {"[] == empty_ss", true},
      {"empty_ss == xss", false},
      // Boundary element: INT64_MIN must round-trip through the
      // element encoder, not through a lossy double.
      {"mins == [[-9223372036854775807 - 1]]", true},
      {"mins == [[-9223372036854775807]]", false},
      // Nested MAP elements take the same walk.
      {R"(mms == [{"a": 1}])", true},
      {R"(mms == [{"a": 2}])", false},
      {R"([{"a": 1}] == mms)", true},
  };
  for (const Case& c : cases) {
    ExpectBool(c.source, c.want);
  }
}

// F1 (map half) — map equality over host-origin operands with
// aggregate VALUES.  Same root cause: `SnapshotMapEntries` encoded
// aggregate values through `EncodeBackingScalar`, so `mm == mm` was
// `false`.
TEST(KnownBugs, PbtMapEqNestedAggregateHostOrigin) {
  struct Case {
    const char* source;
    bool want;
  };
  const std::vector<Case> cases = {
      {R"({"a": [1]} == {"a": [1]})", true},  // arena + arena (baseline)
      {R"(mm == {"a": [1]})", true},          // host  + arena
      {R"({"a": [1]} == mm)", true},          // arena + host
      {"mm == nn", true},                     // host  + host
      {"mm == mm", true},                     // reflexivity
      {"mm == pp", false},                    // nested + different
      {R"(mm == {"a": [2]})", false},
      {R"(mm == {"b": [1]})", false},  // key differs
      {"empty_mm == {}", true},        // empty aggregate
      {"mm == empty_mm", false},
      // Nested MAP values.
      {R"(deep_m == {"a": {"b": 7}})", true},
      {R"(deep_m == {"a": {"b": 8}})", false},
  };
  for (const Case& c : cases) {
    ExpectBool(c.source, c.want);
  }
}

// F2 — `x in host_list` returned `false` for every aggregate needle.
// Root: `BackingValueEqualsQuery`'s `default:` (eval/internal/
// cel_host.cc) answered `false` for kList / kMap / kMessage backing
// elements, so the scan could never match.  The asymmetry was the
// tell: the *equality* walk had grown a real message arm, the `in`
// scan never did.
TEST(KnownBugs, PbtListInAggregateNeedleHostOrigin) {
  struct Case {
    const char* source;
    bool want;
  };
  const std::vector<Case> cases = {
      {"[1, 2] in [[1, 2]]", true},  // arena + arena (baseline)
      {"[1, 2] in xss", true},       // arena needle, host haystack
      {"xs in xss", true},           // host  needle, host haystack
      {"xs in [[1, 2]]", true},      // host  needle, arena haystack
      {"[1, 3] in xss", false},      // nested + different
      {"[1, 2, 3] in xss", false},
      {"[1, 2] in empty_ss", false},  // empty aggregate
      {"empty_xs in xss", false},
      // Boundary element.
      {"[-9223372036854775807 - 1] in mins", true},
      {"[-9223372036854775807] in mins", false},
      // Map needle.
      {R"({"a": 1} in mms)", true},
      {R"({"a": 2} in mms)", false},
      {"ms in mms", true},
  };
  for (const Case& c : cases) {
    ExpectBool(c.source, c.want);
  }
}

// F3 — `math.least(l)` / `math.greatest(l)` poisoned TYPE_MISMATCH for
// a host-origin list (`runtime/cel_math_ext.c`'s `math_minmax_list`
// accepted `CEL_LIST_ARENA` only).  Indistinguishable from a real type
// error.  The 3-arg macro form always rewrites to an arena literal,
// which is why this survived: only the single-list-argument form over
// a bound variable reaches the host arm.
TEST(KnownBugs, PbtMathMinMaxHostListPoisons) {
  auto declare = [](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
    b.DeclareVariable("empty_xs", CelType::List(CelType::Int()));
    b.DeclareVariable("bounds", CelType::List(CelType::Int()));
    b.DeclareVariable("us", CelType::List(CelType::Uint()));
    b.DeclareVariable("ds", CelType::List(CelType::Double()));
  };
  auto bind = [](Activation& a) {
    a.Bind("xs", Value::List({Value::Int(3), Value::Int(1), Value::Int(2)}));
    a.Bind("empty_xs", Value::List({}));
    a.Bind("bounds", Value::List({Value::Int(9223372036854775807LL),
                                  Value::Int(kInt64Min)}));
    a.Bind("us", Value::List({Value::Uint(1), Value::Uint(2)}));
    a.Bind("ds", Value::List({Value::Double(1.5), Value::Double(0.5)}));
  };
  struct IntCase {
    const char* source;
    int64_t want;
  };
  const std::vector<IntCase> int_cases = {
      {"math.least([3, 1, 2])", 1},       // arena (baseline)
      {"math.least(xs)", 1},              // host
      {"math.greatest(xs)", 3},           // host
      {"math.least(bounds)", kInt64Min},  // boundary element
      {"math.greatest(bounds)", 9223372036854775807LL},
      {"math.least(xs + [0])", 0},  // host lifted through concat
  };
  for (const IntCase& c : int_cases) {
    auto v = TryEvalActivated(c.source, declare, bind);
    ASSERT_TRUE(v.ok()) << c.source << ": " << v.status();
    auto got = v->AsInt();
    ASSERT_TRUE(got.ok()) << c.source << ": " << got.status() << " (kind "
                          << static_cast<int>(v->kind()) << ")";
    EXPECT_EQ(*got, c.want) << c.source;
  }
  // uint / double element kinds take the same walk.
  auto u = TryEvalActivated("math.greatest(us)", declare, bind);
  ASSERT_TRUE(u.ok()) << u.status();
  auto u_got = u->AsUint();
  ASSERT_TRUE(u_got.ok()) << u_got.status();
  EXPECT_EQ(*u_got, 2u);
  auto d = TryEvalActivated("math.least(ds)", declare, bind);
  ASSERT_TRUE(d.ok()) << d.status();
  auto d_got = d->AsDouble();
  ASSERT_TRUE(d_got.ok()) << d_got.status();
  EXPECT_EQ(*d_got, 0.5);
  // Empty aggregate: cel-cpp's math_ext.cc:106 returns
  // "math.@min argument must not be empty" — an error VALUE, not a
  // type mismatch and not a silent answer.
  auto empty = TryEvalActivated("math.least(empty_xs)", declare, bind);
  ASSERT_TRUE(empty.ok()) << empty.status();
  EXPECT_TRUE(empty->IsError())
      << "math.least(<empty host list>) should be an error, got kind "
      << static_cast<int>(empty->kind());
}

// F4 — `"…".format(args)` refused a host-origin args list
// (`runtime/cel_string_format.cc`), and `%s` rendering of a host list
// or host map NESTED inside an arena args list returned `false`
// (`runtime/cel_string_format_render.cc`'s `AppendListCanonical` /
// `AppendMapCanonical`), which surfaced as INVALID_ARGUMENT.  Both
// gates had to move: `"%s".format([xs])` hits only the second.
TEST(KnownBugs, PbtStringFormatHostAggregatePoisons) {
  const std::vector<std::pair<const char*, const char*>> cases = {
      {R"("%s".format([[1, 2]]))", "[1, 2]"},  // arena (baseline)
      {R"("%s".format([xs]))", "[1, 2]"},      // host list nested in arena args
      {R"("%s".format([ms]))", "{a: 1}"},      // host map nested in arena args
      {R"("%s".format([xss]))", "[[1, 2]]"},   // host list of host lists
      {R"("%s".format([mm]))", "{a: [1]}"},    // host map of host lists
      {R"("%s".format([empty_xs]))", "[]"},    // empty aggregate
      {R"("%s".format([empty_mm]))", "{}"},
      {R"("%s".format([mins]))", "[[-9223372036854775808]]"},  // boundary
      {R"("%s".format(xs))", "1"},  // host list AS the args list
      {R"("%s-%s".format(xs))", "1-2"},
      {R"("%s".format([1, 2]))", "1"},  // arena args (baseline)
  };
  for (const auto& [source, want] : cases) {
    auto v = EvalHostAggregate(source);
    ASSERT_TRUE(v.ok()) << source << ": " << v.status();
    auto got = v->AsString();
    ASSERT_TRUE(got.ok()) << source << ": " << got.status() << " (kind "
                          << static_cast<int>(v->kind()) << ")";
    EXPECT_EQ(*got, want) << source;
  }
}

}  // namespace
}  // namespace celwasm
