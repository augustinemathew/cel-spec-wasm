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
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace cel {
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
// `map_keys_equal` (compiler_v2/runtime/cel_runtime.c:42-43) compares
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
  GTEST_SKIP() << "KNOWN BUG (verified: returns true, want false): lossy "
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
  GTEST_SKIP() << "KNOWN LIMITATION (verified: returns CEL_ERR_OVERFLOW, want "
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
  GTEST_SKIP() << "KNOWN LIMITATION (verified: returns CEL_ERR_OVERFLOW, want "
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

}  // namespace
}  // namespace cel
