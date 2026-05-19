#include "compiler_v2/runtime/cel_3vl.h"

#include <cstdint>
#include <vector>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_layout.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// M5.G — 3VL / control-flow helper coverage.  The 4×4 truth tables
// for cel_and / cel_or factor naturally into a parameterised matrix:
// each row is `(a_kind, a_bool, b_kind, b_bool, expected_kind,
// expected_bool)` and the body is "build operands, call helper,
// compare slot".  cel_not (4 rows + kind-mismatch) and
// cel_unknown_merge (5 distinct cases — empty/empty, empty/non,
// disjoint, overlap, kind-mismatch) tell distinct stories so each
// stays a focused TEST_F.

namespace celwasm {
namespace {

enum class OpKind {
  kBoolFalse,
  kBoolTrue,
  kError,
  kUnknown,
};

class ThreeVLTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }

  uint32_t MakeOut() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  uint32_t MakeBool(bool b) {
    return cel_make_bool(b ? 1 : 0);
  }

  uint32_t MakeError() {
    uint32_t off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(off);
    v->kind = CEL_ERROR;
    v->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
    return off;
  }

  // UnknownSet wire shape: payload.unk → 2-word descriptor
  // {ids_off, len}; ids_off → contiguous u32 array of attribute ids.
  // Empty means payload.unk = 0 (langdef-compatible "unspecified
  // provenance" UNKNOWN).
  uint32_t MakeUnknownEmpty() {
    uint32_t off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(off);
    v->kind = CEL_UNKNOWN;
    v->payload.unk = 0;
    return off;
  }

  uint32_t MakeUnknownWithIds(const std::vector<uint32_t>& ids) {
    uint32_t bytes = static_cast<uint32_t>(ids.size() * sizeof(uint32_t));
    if (bytes == 0) bytes = static_cast<uint32_t>(sizeof(uint32_t));
    uint32_t ids_off = arena_alloc(bytes);
    auto* dst = reinterpret_cast<uint32_t*>(cel_mem_base() + ids_off);
    for (size_t i = 0; i < ids.size(); ++i)
      dst[i] = ids[i];

    uint32_t desc_off =
        arena_alloc(static_cast<uint32_t>(2 * sizeof(uint32_t)));
    auto* desc = reinterpret_cast<uint32_t*>(cel_mem_base() + desc_off);
    desc[0] = ids_off;
    desc[1] = static_cast<uint32_t>(ids.size());

    uint32_t cv_off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(cv_off);
    v->kind = CEL_UNKNOWN;
    v->payload.unk = desc_off;
    return cv_off;
  }

  uint32_t MakeOperand(OpKind k) {
    switch (k) {
      case OpKind::kBoolFalse:
        return MakeBool(false);
      case OpKind::kBoolTrue:
        return MakeBool(true);
      case OpKind::kError:
        return MakeError();
      case OpKind::kUnknown:
        return MakeUnknownEmpty();
    }
    return 0;
  }

  std::vector<uint32_t> ReadUnknownIds(uint32_t slot) {
    const CelValue* v = cel_value_at(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_UNKNOWN));
    if (v->payload.unk == 0) return {};
    auto* desc =
        reinterpret_cast<const uint32_t*>(cel_mem_base() + v->payload.unk);
    uint32_t ids_off = desc[0];
    uint32_t len = desc[1];
    auto* ids = reinterpret_cast<const uint32_t*>(cel_mem_base() + ids_off);
    return std::vector<uint32_t>(ids, ids + len);
  }
};

// ── 4×4 truth-table matrices for cel_and / cel_or ─────────────────
//
// Per langdef §"Logical operators", the truth table is:
//
//   cel_and    | F(false) | T(true) | ERROR  | UNKNOWN
//   -----------+----------+---------+--------+--------
//   F(false)   |  F       |  F      |  F     |  F
//   T(true)    |  F       |  T      |  ERROR |  UNKNOWN
//   ERROR      |  F       |  ERROR  |  ERROR |  ERROR
//   UNKNOWN    |  F       |  UNKNOWN|  ERROR |  UNKNOWN
//
//   cel_or     | F        | T       | ERROR  | UNKNOWN
//   -----------+----------+---------+--------+--------
//   F          |  F       |  T      |  ERROR |  UNKNOWN
//   T          |  T       |  T      |  T     |  T
//   ERROR      |  ERROR   |  T      |  ERROR |  ERROR
//   UNKNOWN    |  UNKNOWN |  T      |  ERROR |  UNKNOWN

struct LogicCase {
  const char* name;
  OpKind a;
  OpKind b;
  uint32_t expected_kind;  // CEL_BOOL / CEL_ERROR / CEL_UNKNOWN
  bool expected_bool;      // ignored unless expected_kind == CEL_BOOL
};

class CelAndTest : public ThreeVLTest,
                   public ::testing::WithParamInterface<LogicCase> {};

TEST_P(CelAndTest, MatchesTruthTable) {
  const LogicCase& c = GetParam();
  uint32_t a = MakeOperand(c.a);
  uint32_t b = MakeOperand(c.b);
  uint32_t out = MakeOut();
  cel_and(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, c.expected_kind) << c.name;
  if (c.expected_kind == CEL_BOOL) {
    EXPECT_EQ(v->payload.b != 0, c.expected_bool) << c.name;
  }
}

INSTANTIATE_TEST_SUITE_P(
    AndMatrix, CelAndTest,
    ::testing::Values(
        LogicCase{"F_F", OpKind::kBoolFalse, OpKind::kBoolFalse, CEL_BOOL,
                  false},
        LogicCase{"F_T", OpKind::kBoolFalse, OpKind::kBoolTrue, CEL_BOOL,
                  false},
        LogicCase{"F_E", OpKind::kBoolFalse, OpKind::kError, CEL_BOOL, false},
        LogicCase{"F_U", OpKind::kBoolFalse, OpKind::kUnknown, CEL_BOOL, false},
        LogicCase{"T_F", OpKind::kBoolTrue, OpKind::kBoolFalse, CEL_BOOL,
                  false},
        LogicCase{"T_T", OpKind::kBoolTrue, OpKind::kBoolTrue, CEL_BOOL, true},
        LogicCase{"T_E", OpKind::kBoolTrue, OpKind::kError, CEL_ERROR, false},
        LogicCase{"T_U", OpKind::kBoolTrue, OpKind::kUnknown, CEL_UNKNOWN,
                  false},
        LogicCase{"E_F", OpKind::kError, OpKind::kBoolFalse, CEL_BOOL, false},
        LogicCase{"E_T", OpKind::kError, OpKind::kBoolTrue, CEL_ERROR, false},
        LogicCase{"E_E", OpKind::kError, OpKind::kError, CEL_ERROR, false},
        LogicCase{"E_U", OpKind::kError, OpKind::kUnknown, CEL_ERROR, false},
        LogicCase{"U_F", OpKind::kUnknown, OpKind::kBoolFalse, CEL_BOOL, false},
        LogicCase{"U_T", OpKind::kUnknown, OpKind::kBoolTrue, CEL_UNKNOWN,
                  false},
        LogicCase{"U_E", OpKind::kUnknown, OpKind::kError, CEL_ERROR, false},
        LogicCase{"U_U", OpKind::kUnknown, OpKind::kUnknown, CEL_UNKNOWN,
                  false}));

class CelOrTest : public ThreeVLTest,
                  public ::testing::WithParamInterface<LogicCase> {};

TEST_P(CelOrTest, MatchesTruthTable) {
  const LogicCase& c = GetParam();
  uint32_t a = MakeOperand(c.a);
  uint32_t b = MakeOperand(c.b);
  uint32_t out = MakeOut();
  cel_or(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, c.expected_kind) << c.name;
  if (c.expected_kind == CEL_BOOL) {
    EXPECT_EQ(v->payload.b != 0, c.expected_bool) << c.name;
  }
}

INSTANTIATE_TEST_SUITE_P(
    OrMatrix, CelOrTest,
    ::testing::Values(
        LogicCase{"F_F", OpKind::kBoolFalse, OpKind::kBoolFalse, CEL_BOOL,
                  false},
        LogicCase{"F_T", OpKind::kBoolFalse, OpKind::kBoolTrue, CEL_BOOL, true},
        LogicCase{"F_E", OpKind::kBoolFalse, OpKind::kError, CEL_ERROR, false},
        LogicCase{"F_U", OpKind::kBoolFalse, OpKind::kUnknown, CEL_UNKNOWN,
                  false},
        LogicCase{"T_F", OpKind::kBoolTrue, OpKind::kBoolFalse, CEL_BOOL, true},
        LogicCase{"T_T", OpKind::kBoolTrue, OpKind::kBoolTrue, CEL_BOOL, true},
        LogicCase{"T_E", OpKind::kBoolTrue, OpKind::kError, CEL_BOOL, true},
        LogicCase{"T_U", OpKind::kBoolTrue, OpKind::kUnknown, CEL_BOOL, true},
        LogicCase{"E_F", OpKind::kError, OpKind::kBoolFalse, CEL_ERROR, false},
        LogicCase{"E_T", OpKind::kError, OpKind::kBoolTrue, CEL_BOOL, true},
        LogicCase{"E_E", OpKind::kError, OpKind::kError, CEL_ERROR, false},
        LogicCase{"E_U", OpKind::kError, OpKind::kUnknown, CEL_ERROR, false},
        LogicCase{"U_F", OpKind::kUnknown, OpKind::kBoolFalse, CEL_UNKNOWN,
                  false},
        LogicCase{"U_T", OpKind::kUnknown, OpKind::kBoolTrue, CEL_BOOL, true},
        LogicCase{"U_E", OpKind::kUnknown, OpKind::kError, CEL_ERROR, false},
        LogicCase{"U_U", OpKind::kUnknown, OpKind::kUnknown, CEL_UNKNOWN,
                  false}));

// ── kind-mismatch (non-3VL operand) ─────────────────────────────

// Per langdef "false && X = false (any X)": the OK(false) absorber
// dominates a non-3VL right operand without raising a type error.
TEST_F(ThreeVLTest, AndFalseAbsorbsNonThreeVL) {
  uint32_t a = MakeBool(false);
  uint32_t b = cel_make_int(1);
  uint32_t out = MakeOut();
  cel_and(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(v->payload.b, 0);
}

TEST_F(ThreeVLTest, AndTrueWithIntPoisons) {
  uint32_t a = MakeBool(true);
  uint32_t b = cel_make_int(1);
  uint32_t out = MakeOut();
  cel_and(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// Per langdef "true || X = true (any X)": symmetric absorption for
// `_||_`.
TEST_F(ThreeVLTest, OrTrueAbsorbsNonThreeVL) {
  uint32_t a = MakeBool(true);
  uint32_t b = cel_make_int(1);
  uint32_t out = MakeOut();
  cel_or(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(v->payload.b, 1);
}

TEST_F(ThreeVLTest, OrFalseWithIntPoisons) {
  uint32_t a = MakeBool(false);
  uint32_t b = cel_make_int(1);
  uint32_t out = MakeOut();
  cel_or(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// ── both-UNKNOWN merges contribute ids to the result set ─────────
//
// Each operand carries a non-empty UnknownSet; the output's set is
// the sorted-deduped union.  Asserts canonical (sorted) form because
// `cel_unknown_merge`'s correctness is the load-bearing invariant
// for downstream comparison tests.

TEST_F(ThreeVLTest, AndBothUnknownMergesIds) {
  uint32_t a = MakeUnknownWithIds({1, 3, 5});
  uint32_t b = MakeUnknownWithIds({2, 3, 4});
  uint32_t out = MakeOut();
  cel_and(out, a, b);
  EXPECT_THAT(ReadUnknownIds(out), ::testing::ElementsAre(1u, 2u, 3u, 4u, 5u));
}

TEST_F(ThreeVLTest, OrBothUnknownMergesIds) {
  uint32_t a = MakeUnknownWithIds({10, 20});
  uint32_t b = MakeUnknownWithIds({5, 20, 30});
  uint32_t out = MakeOut();
  cel_or(out, a, b);
  EXPECT_THAT(ReadUnknownIds(out), ::testing::ElementsAre(5u, 10u, 20u, 30u));
}

// ── cel_not ─────────────────────────────────────────────────────

TEST_F(ThreeVLTest, NotOfTrueIsFalse) {
  uint32_t a = MakeBool(true);
  uint32_t out = MakeOut();
  cel_not(out, a);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(v->payload.b, 0);
}

TEST_F(ThreeVLTest, NotOfFalseIsTrue) {
  uint32_t a = MakeBool(false);
  uint32_t out = MakeOut();
  cel_not(out, a);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(v->payload.b, 1);
}

TEST_F(ThreeVLTest, NotOfErrorPropagates) {
  uint32_t a = MakeError();
  uint32_t out = MakeOut();
  cel_not(out, a);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(ThreeVLTest, NotOfUnknownPropagates) {
  uint32_t a = MakeUnknownWithIds({7, 9});
  uint32_t out = MakeOut();
  cel_not(out, a);
  EXPECT_THAT(ReadUnknownIds(out), ::testing::ElementsAre(7u, 9u));
}

TEST_F(ThreeVLTest, NotOfIntPoisons) {
  uint32_t a = cel_make_int(1);
  uint32_t out = MakeOut();
  cel_not(out, a);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// ── cel_unknown_merge ───────────────────────────────────────────

TEST_F(ThreeVLTest, UnknownMergeBothEmptyYieldsEmpty) {
  uint32_t a = MakeUnknownEmpty();
  uint32_t b = MakeUnknownEmpty();
  uint32_t out = MakeOut();
  cel_unknown_merge(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(v->payload.unk, 0u);
}

TEST_F(ThreeVLTest, UnknownMergeEmptyAndNonEmptyTakesNonEmpty) {
  uint32_t a = MakeUnknownEmpty();
  uint32_t b = MakeUnknownWithIds({4, 8});
  uint32_t out = MakeOut();
  cel_unknown_merge(out, a, b);
  EXPECT_THAT(ReadUnknownIds(out), ::testing::ElementsAre(4u, 8u));
}

TEST_F(ThreeVLTest, UnknownMergeNonEmptyAndEmptyTakesNonEmpty) {
  uint32_t a = MakeUnknownWithIds({1, 2});
  uint32_t b = MakeUnknownEmpty();
  uint32_t out = MakeOut();
  cel_unknown_merge(out, a, b);
  EXPECT_THAT(ReadUnknownIds(out), ::testing::ElementsAre(1u, 2u));
}

TEST_F(ThreeVLTest, UnknownMergeSortedDisjointConcatenates) {
  uint32_t a = MakeUnknownWithIds({1, 3, 5});
  uint32_t b = MakeUnknownWithIds({2, 4, 6});
  uint32_t out = MakeOut();
  cel_unknown_merge(out, a, b);
  EXPECT_THAT(ReadUnknownIds(out),
              ::testing::ElementsAre(1u, 2u, 3u, 4u, 5u, 6u));
}

TEST_F(ThreeVLTest, UnknownMergeOverlappingDeduplicates) {
  uint32_t a = MakeUnknownWithIds({1, 2, 3});
  uint32_t b = MakeUnknownWithIds({2, 3, 4});
  uint32_t out = MakeOut();
  cel_unknown_merge(out, a, b);
  EXPECT_THAT(ReadUnknownIds(out), ::testing::ElementsAre(1u, 2u, 3u, 4u));
}

TEST_F(ThreeVLTest, UnknownMergeKindMismatchPoisons) {
  uint32_t a = MakeUnknownEmpty();
  uint32_t b = MakeBool(true);
  uint32_t out = MakeOut();
  cel_unknown_merge(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// ── cel_copy_slot ────────────────────────────────────────────────

TEST_F(ThreeVLTest, CopySlotMaterialisesValueByteForByte) {
  uint32_t src = cel_make_int(42);
  uint32_t dst = MakeOut();
  cel_copy_slot(dst, src);
  const CelValue* v = cel_value_at(dst);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(v->payload.i, 42);
}

TEST_F(ThreeVLTest, CopySlotPreservesUnknownDescriptor) {
  uint32_t src = MakeUnknownWithIds({11, 13});
  uint32_t dst = MakeOut();
  cel_copy_slot(dst, src);
  EXPECT_THAT(ReadUnknownIds(dst), ::testing::ElementsAre(11u, 13u));
}

// ── Arena OOM in cel_unknown_merge (DESIGN §5 A10) ─────────────────
//
// Merging two non-empty UnknownSets allocates a fresh descriptor +
// ids buffer via arena_alloc.  On OOM, cel_3vl.c:122-128 re-derives
// the out pointer and poisons with CEL_ERR_OVERFLOW.  Verify the
// graceful failure path.

TEST_F(ThreeVLTest, UnknownMergeOomPoisonsWithOverflow) {
  uint32_t a = MakeUnknownWithIds({1, 2, 3});
  uint32_t b = MakeUnknownWithIds({4, 5, 6});
  uint32_t out = MakeOut();
  // Drain the arena to 0 bytes free — neither the new ids array
  // nor the descriptor will fit.
  uint32_t remaining = arena_capacity() - arena_cursor();
  if (remaining > 0u) {
    ASSERT_NE(arena_alloc(remaining), 0u);
  }
  cel_unknown_merge(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

// When one side is empty, merge takes the other side's descriptor
// without any allocation — no OOM even if the arena is full.
// (cel_3vl.c:110-120, the empty-side early returns.)
TEST_F(ThreeVLTest, UnknownMergeEmptySideSucceedsEvenWhenArenaFull) {
  uint32_t a = MakeUnknownEmpty();
  uint32_t b = MakeUnknownWithIds({42});
  uint32_t out = MakeOut();
  // Fill the arena.
  uint32_t remaining = arena_capacity() - arena_cursor();
  if (remaining > 0u) {
    ASSERT_NE(arena_alloc(remaining), 0u);
  }
  cel_unknown_merge(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_THAT(ReadUnknownIds(out), ::testing::ElementsAre(42u));
}

// Both sides empty → no allocation, no OOM regardless of arena.
TEST_F(ThreeVLTest, UnknownMergeBothEmptyNeverNeedsArena) {
  uint32_t a = MakeUnknownEmpty();
  uint32_t b = MakeUnknownEmpty();
  uint32_t out = MakeOut();
  uint32_t remaining = arena_capacity() - arena_cursor();
  if (remaining > 0u) {
    ASSERT_NE(arena_alloc(remaining), 0u);
  }
  cel_unknown_merge(out, a, b);
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(v->payload.unk, 0u);
}

}  // namespace
}  // namespace celwasm
