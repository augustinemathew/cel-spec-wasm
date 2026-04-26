#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_list.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_map.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

// M5.D step 1 — aggregate-op kArena fast paths.  Coverage:
//   - happy-path list size / in / eq / concat
//   - happy-path map size / in / eq (set-equality on entries)
//   - boundary: empty list/map; not-found `in`; same-content
//     different-order map equality (order-irrelevant per langdef)
//   - 3VL absorption + type-mismatch
//
// Parameterized tables consolidate the structurally-identical
// happy/false rows (size→int, helper→bool); spec-citation cases
// (langdef positional list-eq vs set-eq map-eq, cross-type uint
// needle, 3VL absorption per envelope side) stay as focused
// TEST_F so the spec source-of-truth is explicit at each
// assertion.
//
// Cross-type numeric equality (e.g. `1 in [1u]`) is deferred to
// M5.B step 2's `cel_numeric_*` ladder; the helper currently
// declines such mixed-numeric-kind matches.

namespace celwasm {
namespace {

class AggregateArenaTest : public ::testing::Test {
 public:  // public so parameterized lambdas can use the helpers.
  void SetUp() override {
    cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  }

  uint32_t MakeOut() {
    return cel_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  const CelValue* At(uint32_t slot) {
    return cel_value_at(slot);
  }

  // Build a CEL_LIST_ARENA at a fresh slot from the given int
  // values.  Returns the list-slot offset; cel_list_create
  // allocates a header + elements run.
  uint32_t MakeIntList(std::initializer_list<int64_t> elems) {
    uint32_t list = MakeOut();
    cel_list_create(list, static_cast<uint32_t>(elems.size()));
    uint32_t i = 0;
    for (int64_t v : elems) {
      cel_list_set(list, i++, cel_make_int(v));
    }
    return list;
  }

  // Same shape for an empty arena map (post-create, ready for
  // assertions on size/in/eq).  Pass `capacity=0` to mirror a
  // genuinely-empty literal.
  uint32_t MakeMap(std::initializer_list<std::pair<int64_t, int64_t>> entries) {
    uint32_t m = MakeOut();
    cel_map_create(m, static_cast<uint32_t>(entries.size()));
    for (const auto& [k, v] : entries) {
      cel_map_insert(m, cel_make_int(k), cel_make_int(v));
    }
    return m;
  }

  uint32_t EmptyList() {
    uint32_t l = MakeOut();
    cel_list_create(l, 0);
    return l;
  }

  uint32_t EmptyMap() {
    uint32_t m = MakeOut();
    cel_map_create(m, 0);
    return m;
  }

  bool ReadBool(uint32_t slot) {
    EXPECT_EQ(At(slot)->kind, static_cast<uint32_t>(CEL_BOOL));
    return At(slot)->payload.b != 0;
  }

  int64_t ReadInt(uint32_t slot) {
    EXPECT_EQ(At(slot)->kind, static_cast<uint32_t>(CEL_INT));
    return At(slot)->payload.i;
  }
};

// ── Parameterized: size helpers (list / map) → int ────────────

struct SizeIntCase {
  const char* name;
  void (*helper)(uint32_t, uint32_t);
  uint32_t (*build)(AggregateArenaTest&);
  int64_t expected;
};

class AggregateSizeTest : public AggregateArenaTest,
                          public ::testing::WithParamInterface<SizeIntCase> {};

TEST_P(AggregateSizeTest, ProducesExpectedCount) {
  const SizeIntCase& c = GetParam();
  uint32_t v = c.build(*this);
  uint32_t out = MakeOut();
  c.helper(out, v);
  EXPECT_EQ(ReadInt(out), c.expected) << c.name;
}

INSTANTIATE_TEST_SUITE_P(All, AggregateSizeTest,
                         ::testing::Values(
                             SizeIntCase{
                                 "list_size_three",
                                 cel_list_size_arena,
                                 +[](AggregateArenaTest& t) {
                                   return t.MakeIntList({10, 20, 30});
                                 },
                                 3,
                             },
                             SizeIntCase{
                                 "list_size_empty",
                                 cel_list_size_arena,
                                 +[](AggregateArenaTest& t) {
                                   return t.EmptyList();
                                 },
                                 0,
                             },
                             SizeIntCase{
                                 "map_size_two",
                                 cel_map_size_arena,
                                 +[](AggregateArenaTest& t) {
                                   return t.MakeMap({{1, 10}, {2, 20}});
                                 },
                                 2,
                             },
                             SizeIntCase{
                                 "map_size_empty",
                                 cel_map_size_arena,
                                 +[](AggregateArenaTest& t) {
                                   return t.EmptyMap();
                                 },
                                 0,
                             }),
                         [](const auto& info) {
                           return std::string(info.param.name);
                         });

// ── Parameterized: binary aggregate helpers → bool ────────────
//
// Covers the structurally-identical happy/false pairs for
// list_in / list_eq / map_in / map_eq.  Each row carries the
// helper, the "left" + "right" operand factories, and the
// expected bool.  Distinct-story cases (cross-type numeric
// needles, set-equality-on-entries, positional-vs-set semantics
// nuance) are TEST_F below.

struct AggregateBoolCase {
  const char* name;
  void (*helper)(uint32_t, uint32_t, uint32_t);
  uint32_t (*lhs)(AggregateArenaTest&);
  uint32_t (*rhs)(AggregateArenaTest&);
  bool expected;
};

class AggregateBoolTest
    : public AggregateArenaTest,
      public ::testing::WithParamInterface<AggregateBoolCase> {};

TEST_P(AggregateBoolTest, ProducesExpectedBool) {
  const AggregateBoolCase& c = GetParam();
  uint32_t out = MakeOut();
  c.helper(out, c.lhs(*this), c.rhs(*this));
  EXPECT_EQ(ReadBool(out), c.expected) << c.name;
}

INSTANTIATE_TEST_SUITE_P(ListIn, AggregateBoolTest,
                         ::testing::Values(AggregateBoolCase{
                                               "list_in_finds_existing",
                                               cel_list_in_arena,
                                               +[](AggregateArenaTest&) {
                                                 return cel_make_int(20);
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeIntList(
                                                     {10, 20, 30});
                                               },
                                               true,
                                           },
                                           AggregateBoolCase{
                                               "list_in_rejects_absent",
                                               cel_list_in_arena,
                                               +[](AggregateArenaTest&) {
                                                 return cel_make_int(99);
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeIntList(
                                                     {10, 20, 30});
                                               },
                                               false,
                                           },
                                           AggregateBoolCase{
                                               "list_in_on_empty",
                                               cel_list_in_arena,
                                               +[](AggregateArenaTest&) {
                                                 return cel_make_int(0);
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.EmptyList();
                                               },
                                               false,
                                           }),
                         [](const auto& info) {
                           return std::string(info.param.name);
                         });

INSTANTIATE_TEST_SUITE_P(
    ListEq, AggregateBoolTest,
    ::testing::Values(AggregateBoolCase{
                          "list_eq_same_content",
                          cel_list_eq_arena,
                          +[](AggregateArenaTest& t) {
                            return t.MakeIntList({1, 2, 3});
                          },
                          +[](AggregateArenaTest& t) {
                            return t.MakeIntList({1, 2, 3});
                          },
                          true,
                      },
                      AggregateBoolCase{
                          "list_eq_different_length",
                          cel_list_eq_arena,
                          +[](AggregateArenaTest& t) {
                            return t.MakeIntList({1, 2});
                          },
                          +[](AggregateArenaTest& t) {
                            return t.MakeIntList({1, 2, 3});
                          },
                          false,
                      },
                      AggregateBoolCase{
                          "list_eq_both_empty",
                          cel_list_eq_arena,
                          +[](AggregateArenaTest& t) {
                            return t.EmptyList();
                          },
                          +[](AggregateArenaTest& t) {
                            return t.EmptyList();
                          },
                          true,
                      }),
    [](const auto& info) {
      return std::string(info.param.name);
    });

INSTANTIATE_TEST_SUITE_P(MapIn, AggregateBoolTest,
                         ::testing::Values(AggregateBoolCase{
                                               "map_in_finds_key",
                                               cel_map_in_arena,
                                               +[](AggregateArenaTest&) {
                                                 return cel_make_int(1);
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeMap(
                                                     {{1, 10}, {2, 20}});
                                               },
                                               true,
                                           },
                                           AggregateBoolCase{
                                               "map_in_rejects_absent_key",
                                               cel_map_in_arena,
                                               +[](AggregateArenaTest&) {
                                                 return cel_make_int(99);
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeMap(
                                                     {{1, 10}, {2, 20}});
                                               },
                                               false,
                                           }),
                         [](const auto& info) {
                           return std::string(info.param.name);
                         });

INSTANTIATE_TEST_SUITE_P(MapEq, AggregateBoolTest,
                         ::testing::Values(AggregateBoolCase{
                                               "map_eq_different_value",
                                               cel_map_eq_arena,
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeMap({{1, 10}});
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeMap({{1, 99}});
                                               },
                                               false,
                                           },
                                           AggregateBoolCase{
                                               "map_eq_different_key",
                                               cel_map_eq_arena,
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeMap({{1, 10}});
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeMap({{2, 10}});
                                               },
                                               false,
                                           },
                                           AggregateBoolCase{
                                               "map_eq_different_size",
                                               cel_map_eq_arena,
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeMap({{1, 10}});
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeMap(
                                                     {{1, 10}, {2, 20}});
                                               },
                                               false,
                                           },
                                           AggregateBoolCase{
                                               "map_eq_both_empty",
                                               cel_map_eq_arena,
                                               +[](AggregateArenaTest& t) {
                                                 return t.EmptyMap();
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.EmptyMap();
                                               },
                                               true,
                                           }),
                         [](const auto& info) {
                           return std::string(info.param.name);
                         });

// ── list_concat: post-concat size assertion is structurally
//                 identical across non-empty / one-empty / both-
//                 empty.  The full element-walk is its own story
//                 (asserts indexed access shape) and stays TEST_F.

struct ConcatSizeCase {
  const char* name;
  uint32_t (*lhs)(AggregateArenaTest&);
  uint32_t (*rhs)(AggregateArenaTest&);
  int64_t expected_size;
};

class ListConcatSizeTest
    : public AggregateArenaTest,
      public ::testing::WithParamInterface<ConcatSizeCase> {};

TEST_P(ListConcatSizeTest, ProducesExpectedSize) {
  const ConcatSizeCase& c = GetParam();
  uint32_t out = MakeOut();
  cel_list_concat_arena(out, c.lhs(*this), c.rhs(*this));
  ASSERT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_LIST_ARENA)) << c.name;
  uint32_t size_out = MakeOut();
  cel_list_size_arena(size_out, out);
  EXPECT_EQ(ReadInt(size_out), c.expected_size) << c.name;
}

INSTANTIATE_TEST_SUITE_P(All, ListConcatSizeTest,
                         ::testing::Values(ConcatSizeCase{
                                               "list_concat_with_empty",
                                               +[](AggregateArenaTest& t) {
                                                 return t.MakeIntList({1, 2});
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.EmptyList();
                                               },
                                               2,
                                           },
                                           ConcatSizeCase{
                                               "list_concat_both_empty",
                                               +[](AggregateArenaTest& t) {
                                                 return t.EmptyList();
                                               },
                                               +[](AggregateArenaTest& t) {
                                                 return t.EmptyList();
                                               },
                                               0,
                                           }),
                         [](const auto& info) {
                           return std::string(info.param.name);
                         });

// ── Spec-citation focused tests (TEST_F) ──────────────────────

// list_size on a non-list operand poisons.  Distinct from the
// 3VL envelope (no UNKNOWN/ERROR involved); this is the
// type-mismatch path on a unary helper.
TEST_F(AggregateArenaTest, ListSizeTypeMismatch) {
  uint32_t s = cel_make_int(5);
  uint32_t out = MakeOut();
  cel_list_size_arena(out, s);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// langdef §"Equality": list equality is POSITIONAL — order
// matters.  Set-semantics is for maps only.  This row asserts
// that distinction, so it stays focused (not in the eq table).
TEST_F(AggregateArenaTest, ListEqDifferentOrderIsFalse) {
  uint32_t a = MakeIntList({1, 2, 3});
  uint32_t b = MakeIntList({3, 2, 1});
  uint32_t out = MakeOut();
  cel_list_eq_arena(out, a, b);
  EXPECT_FALSE(ReadBool(out));
}

// Cross-type numeric: needle is uint, list is int.
// `map_keys_equal` already implements the int↔uint ladder
// (see numeric_keys_equal); same matcher is reused here.
// Distinct-story: locks the cel-cpp parity citation in place.
TEST_F(AggregateArenaTest, ListInUintNeedleAcrossInt) {
  uint32_t l = MakeIntList({1, 2, 3});
  uint32_t out = MakeOut();
  cel_list_in_arena(out, cel_make_uint(2), l);
  EXPECT_TRUE(ReadBool(out));
}

// Verifies the concatenated list's element-walk: indices 0..3
// must read back 1..4.  The size assertion is in the
// parameterized table; this one nails the positional layout.
TEST_F(AggregateArenaTest, ListConcatAppendsElementsInOrder) {
  uint32_t a = MakeIntList({1, 2});
  uint32_t b = MakeIntList({3, 4});
  uint32_t out = MakeOut();
  cel_list_concat_arena(out, a, b);
  ASSERT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  uint32_t size_out = MakeOut();
  cel_list_size_arena(size_out, out);
  EXPECT_EQ(ReadInt(size_out), 4);
  uint32_t e_out = MakeOut();
  for (int64_t i = 0; i < 4; ++i) {
    cel_list_at_arena(e_out, out, cel_make_int(i));
    EXPECT_EQ(ReadInt(e_out), i + 1);
  }
}

// langdef §"Equality": map equality is set-equality on entries
// — order is irrelevant.  Build two maps with the same entries
// but different insertion order; equality must be true.  This
// is the spec-citation row that distinguishes maps from lists.
TEST_F(AggregateArenaTest, MapEqSetSemanticsOnEntries) {
  uint32_t a = MakeMap({{1, 10}, {2, 20}, {3, 30}});
  uint32_t b = MakeMap({{3, 30}, {1, 10}, {2, 20}});
  uint32_t out = MakeOut();
  cel_map_eq_arena(out, a, b);
  EXPECT_TRUE(ReadBool(out));
}

// ── 3VL + type-mismatch envelope (one example each) ───────────

TEST_F(AggregateArenaTest, ListInAbsorbsErrorOperand) {
  uint32_t err_off = cel_alloc(sizeof(CelValue));
  CelValue* err = cel_value_at(err_off);
  err->kind = CEL_ERROR;
  err->payload.err = CEL_ERR_OVERFLOW;
  uint32_t l = MakeIntList({1, 2});
  uint32_t out = MakeOut();
  cel_list_in_arena(out, err_off, l);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(AggregateArenaTest, MapEqAbsorbsUnknown) {
  uint32_t unk_off = cel_alloc(sizeof(CelValue));
  CelValue* unk = cel_value_at(unk_off);
  unk->kind = CEL_UNKNOWN;
  unk->payload.unk = 42;
  uint32_t m = MakeMap({{1, 1}});
  uint32_t out = MakeOut();
  cel_map_eq_arena(out, m, unk_off);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(out)->payload.unk, 42u);
}

TEST_F(AggregateArenaTest, ListEqRejectsScalarOperand) {
  uint32_t l = MakeIntList({1});
  uint32_t s = cel_make_int(1);
  uint32_t out = MakeOut();
  cel_list_eq_arena(out, l, s);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// ── Slice 1.6 — polymorphic element-equality (`cel_value_eq_polymorphic`)
// covering cross-numeric membership through `cel_list_in_arena` and
// `cel_map_in_arena`.  Spec: langdef §"List Membership (in)" +
// §"Equality" — element equality uses the polymorphic ladder so
// `1 in [1.0]` is true.  Conformance corpus rows: `int_in_doubles`,
// `double_in_ints`, `uint_in_ints` etc. (`lists.textproto`).

// Build a list of doubles to test cross-numeric `in` against int /
// uint queries.
class CrossNumericMembershipArenaTest : public AggregateArenaTest {
 public:
  uint32_t MakeDoubleList(std::initializer_list<double> elems) {
    uint32_t l = MakeOut();
    cel_list_create(l, static_cast<uint32_t>(elems.size()));
    uint32_t i = 0;
    for (double v : elems) {
      cel_list_set(l, i++, cel_make_double(v));
    }
    return l;
  }
  uint32_t MakeUintList(std::initializer_list<uint64_t> elems) {
    uint32_t l = MakeOut();
    cel_list_create(l, static_cast<uint32_t>(elems.size()));
    uint32_t i = 0;
    for (uint64_t v : elems) {
      cel_list_set(l, i++, cel_make_uint(v));
    }
    return l;
  }
};

// `1 in [1.0, 2.0]` — int query against double-element list.
// Pre-Slice-1.6 returned false (kind mismatch); now matches via the
// polymorphic numeric kernel.
TEST_F(CrossNumericMembershipArenaTest, IntInListOfDoubles) {
  uint32_t l = MakeDoubleList({1.0, 2.0});
  uint32_t out = MakeOut();
  cel_list_in_arena(out, cel_make_int(1), l);
  EXPECT_TRUE(ReadBool(out));
}

// `1.0 in [1, 2]` — double query against int-element list.
TEST_F(CrossNumericMembershipArenaTest, DoubleInListOfInts) {
  uint32_t l = MakeIntList({1, 2});
  uint32_t out = MakeOut();
  cel_list_in_arena(out, cel_make_double(1.0), l);
  EXPECT_TRUE(ReadBool(out));
}

// `3u in [3]` — uint query against int-element list.
TEST_F(CrossNumericMembershipArenaTest, UintInListOfInts) {
  uint32_t l = MakeIntList({3});
  uint32_t out = MakeOut();
  cel_list_in_arena(out, cel_make_uint(3), l);
  EXPECT_TRUE(ReadBool(out));
}

// `1.5 in [1, 2]` — double query, no integer in list rounds to 1.5.
TEST_F(CrossNumericMembershipArenaTest, FractionalDoubleNotInListOfInts) {
  uint32_t l = MakeIntList({1, 2});
  uint32_t out = MakeOut();
  cel_list_in_arena(out, cel_make_double(1.5), l);
  EXPECT_FALSE(ReadBool(out));
}

// `NaN in [NaN, 1.0]` — NaN matcher returns false on every element,
// including NaN itself.  IEEE 754: `NaN == NaN` is false.
TEST_F(CrossNumericMembershipArenaTest, NaNNotInListOfDoubles) {
  uint32_t l = MakeDoubleList({0.0 / 0.0, 1.0});
  uint32_t out = MakeOut();
  cel_list_in_arena(out, cel_make_double(0.0 / 0.0), l);
  EXPECT_FALSE(ReadBool(out));
}

// `-1 in [0u, 1u, 2u]` — negative int never equals any uint
// (`cmp_int_vs_uint` short-circuits negative → kCmpLess; never
// kCmpEqual).
TEST_F(CrossNumericMembershipArenaTest, NegativeIntNotInListOfUints) {
  uint32_t l = MakeUintList({0, 1, 2});
  uint32_t out = MakeOut();
  cel_list_in_arena(out, cel_make_int(-1), l);
  EXPECT_FALSE(ReadBool(out));
}

// `1 in {1u: "x"}` — int query, uint-typed map keys.  Slice 1.6
// updated `map_keys_equal` to consult `numeric_compare_kernel`
// for any numeric pair (including int↔uint via the ladder, which
// `numeric_keys_equal` already handled — so this is a regression
// guard).
TEST_F(CrossNumericMembershipArenaTest, IntInMapOfUintKeys) {
  uint32_t m = MakeOut();
  cel_map_create(m, 1);
  cel_map_insert(m, cel_make_uint(1), cel_make_int(99));
  uint32_t out = MakeOut();
  cel_map_in_arena(out, cel_make_int(1), m);
  EXPECT_TRUE(ReadBool(out));
}

// `1.0 in {1: "x"}` — double-typed query against int keys.  Slice
// 1.6's polymorphic ladder makes this work; previously returned
// false (or worse, type-mismatch).  Note: double is NOT a valid
// map-key kind on the *insertion* side.
TEST_F(CrossNumericMembershipArenaTest, DoubleInMapOfIntKeys) {
  uint32_t m = MakeOut();
  cel_map_create(m, 1);
  cel_map_insert(m, cel_make_int(1), cel_make_int(99));
  uint32_t out = MakeOut();
  cel_map_in_arena(out, cel_make_double(1.0), m);
  EXPECT_TRUE(ReadBool(out));
}

}  // namespace
}  // namespace celwasm
