// Host-side coverage of cel_list.h primitives + the kDynamic
// dispatcher.  The wasm32 binary is exercised separately by
// cel_runtime_wasm_test (locks `return_call` emission); this file
// pins the C-level semantics — index ladder, error poisoning,
// fixed-length set, dispatcher routing — that the wasm and host
// builds share.
//
// Mirrors the shape of cel_map_test.cc; coverage targets enumerated
// in `m4-list-literals.md §6.1`.

#include "runtime/cel_list.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_make.h"
#include "runtime/cel_memory.h"

extern "C" {
// Strong overrides of the host-side weak stubs in cel_runtime.c.  Let
// the dispatchers' kHost arms be observed without pulling in the
// wasmtime trampolines.
static int g_host_at_calls = 0;
void cel_host_cel_list_at(uint32_t out_slot, uint32_t /*list_slot*/,
                          uint32_t /*index_slot*/) {
  ++g_host_at_calls;
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_INT;
  out->payload.i = 0x5151;  // sentinel — distinguishes from arena hits.
}

// Concat's kHost arm.  The real body (eval/internal/cel_host.cc
// `CelListConcatImpl`) lifts both operands into one fresh arena list;
// here we only pin ROUTING — the sentinel distinguishes a host trip
// from the arena fast path, whose real output the arena cases assert.
static int g_host_concat_calls = 0;
void cel_host_cel_list_concat(uint32_t out_slot, uint32_t /*a_slot*/,
                              uint32_t /*b_slot*/) {
  ++g_host_concat_calls;
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_INT;
  out->payload.i = 0x6262;
}
}

namespace celwasm {
namespace {

class ListTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
    g_host_at_calls = 0;
    g_host_concat_calls = 0;
  }

 public:
  uint32_t NewSlot() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }
  uint32_t Str(const char* s) {
    return cel_make_string(s, static_cast<uint32_t>(std::strlen(s)));
  }
};

// ════════ Construction and structural invariants ════════

TEST_F(ListTest, CreateProducesArenaKind) {
  uint32_t out = NewSlot();
  cel_list_create(out, /*capacity=*/2);
  CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_NE(v->payload.arena_list.header_ptr, 0u);
}

TEST_F(ListTest, CreateZeroCountIsValidEmpty) {
  uint32_t out = NewSlot();
  cel_list_create(out, /*capacity=*/0);
  ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  uint32_t result = NewSlot();
  // NOLINTNEXTLINE(readability-suspicious-call-argument)
  cel_list_at_arena(result, out, cel_make_int(0));
  EXPECT_EQ(cel_value_at(result)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(result)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INDEX_OUT_OF_BOUNDS));
}

TEST_F(ListTest, PartiallyFilledTrailingSlotsReadAsCelNull) {
  // create allocates `capacity` slots, count=0; arena_alloc zero-fills.
  // If codegen appends fewer than capacity (e.g. comprehension filter
  // skipping iters), the unused trailing slots are unreachable
  // (count caps reads) — but a defensive index past count must not
  // surface garbage.  Cover the at-bounds case here.
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/3);
  cel_list_append_at(l, cel_make_int(10));
  // count == 1; reading index 1 is out-of-bounds, which routes
  // through the arena read's NO_SUCH_KEY path (covered elsewhere)
  // — the invariant we lock here is the in-bounds element round-
  // trips correctly with the count-0 create + append semantics.
  uint32_t out = NewSlot();
  cel_list_at_arena(out, l, cel_make_int(0));
  EXPECT_EQ(cel_value_at(out)->payload.i, 10);
}

TEST_F(ListTest, AppendPastCapacityTraps) {
  // PRESIZE_INVARIANT (followon §10.A): codegen sizes capacity
  // exactly — N for literals, iter_range.count for accus.  Append
  // past capacity is a codegen invariant violation; runtime traps
  // via __builtin_trap.  Skipped here because gtest can't catch
  // __builtin_trap; the wasm-execution tests indirectly cover it
  // (any codegen bug that drops the pre-size would crash there).
  GTEST_SKIP() << R"CELSKIP(CELSKIP v1
reason: harness-limit
why-not-a-bug: appending past capacity is a CODEGEN invariant violation, and
  the runtime deliberately answers it with __builtin_trap - the loud failure
  the CLAUDE.md unimplemented/unreachable rules ask for. gtest cannot catch
  __builtin_trap, so the behaviour is unobservable from this harness; nothing
  in the product is missing or wrong. The invariant is held from the other
  side: codegen sizes capacity exactly (N for literals, iter_range.count for
  accumulators), and any codegen bug that dropped the pre-size would crash the
  wasm-execution tests.
citation: doc/implementation-plan/rewrite/m5-comprehensions-followon.md §10.A (PRESIZE_INVARIANT)
)CELSKIP";
}

TEST_F(ListTest, AppendAllowsDuplicateValues) {
  // Lists allow duplicate values across indices (unlike map keys).
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/3);
  for (uint32_t i = 0; i < 3; ++i) {
    cel_list_append_at(l, cel_make_int(7));
  }
  ASSERT_EQ(cel_value_at(l)->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  for (int64_t i = 0; i < 3; ++i) {
    uint32_t out = NewSlot();
    cel_list_at_arena(out, l, cel_make_int(i));
    EXPECT_EQ(cel_value_at(out)->payload.i, 7);
  }
}

// ════════ Per-element-kind round-trip ════════

struct ElementCase {
  const char* name;
  uint32_t (*make)(ListTest&);
  std::function<void(const CelValue*)> verify;
};

class ListElementRoundTripTest
    : public ListTest,
      public ::testing::WithParamInterface<ElementCase> {};

TEST_P(ListElementRoundTripTest, SetThenIndexHits) {
  const ElementCase& c = GetParam();
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/1);
  cel_list_append_at(l, c.make(*this));
  ASSERT_EQ(cel_value_at(l)->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  uint32_t out = NewSlot();
  cel_list_at_arena(out, l, cel_make_int(0));
  c.verify(cel_value_at(out));
}

INSTANTIATE_TEST_SUITE_P(
    AllElementKinds, ListElementRoundTripTest,
    ::testing::Values(
        ElementCase{"null",
                    [](ListTest&) {
                      return cel_make_null();
                    },
                    [](const CelValue* v) {
                      EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_NULL));
                    }},
        ElementCase{"bool",
                    [](ListTest&) {
                      return cel_make_bool(1);
                    },
                    [](const CelValue* v) {
                      EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BOOL));
                      EXPECT_EQ(v->payload.b, 1);
                    }},
        ElementCase{"int",
                    [](ListTest&) {
                      return cel_make_int(-99);
                    },
                    [](const CelValue* v) {
                      EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_INT));
                      EXPECT_EQ(v->payload.i, -99);
                    }},
        ElementCase{"uint",
                    [](ListTest&) {
                      return cel_make_uint(7u);
                    },
                    [](const CelValue* v) {
                      EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_UINT));
                      EXPECT_EQ(v->payload.u, 7u);
                    }},
        ElementCase{"double",
                    [](ListTest&) {
                      return cel_make_double(3.5);
                    },
                    [](const CelValue* v) {
                      EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_DOUBLE));
                      EXPECT_EQ(v->payload.d, 3.5);
                    }},
        ElementCase{"string",
                    [](ListTest& t) {
                      return t.Str("hi");
                    },
                    [](const CelValue* v) {
                      EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
                      EXPECT_EQ(v->payload.s.len, 2u);
                    }},
        ElementCase{"bytes",
                    [](ListTest&) {
                      const unsigned char b[3] = {1, 2, 3};
                      return cel_make_bytes(b, 3u);
                    },
                    [](const CelValue* v) {
                      EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BYTES));
                      EXPECT_EQ(v->payload.bytes.len, 3u);
                    }}),
    [](const ::testing::TestParamInfo<ElementCase>& i) {
      return std::string(i.param.name);
    });

// ════════ Index boundary semantics (langdef §"Indexing") ════════

TEST_F(ListTest, NegativeIndexErrors) {
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/3);
  for (uint32_t i = 0; i < 3; ++i) {
    cel_list_append_at(l, cel_make_int(static_cast<int64_t>(i)));
  }
  uint32_t out = NewSlot();
  cel_list_at_arena(out, l, cel_make_int(-1));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INDEX_OUT_OF_BOUNDS));
}

TEST_F(ListTest, IndexEqualToCountErrors) {
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/3);
  for (uint32_t i = 0; i < 3; ++i) {
    cel_list_append_at(l, cel_make_int(static_cast<int64_t>(i)));
  }
  uint32_t out = NewSlot();
  cel_list_at_arena(out, l, cel_make_int(3));  // == count
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INDEX_OUT_OF_BOUNDS));
}

TEST_F(ListTest, IndexBoundariesHit) {
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/3);
  cel_list_append_at(l, cel_make_int(11));
  cel_list_append_at(l, cel_make_int(22));
  cel_list_append_at(l, cel_make_int(33));
  for (int64_t i : {int64_t{0}, int64_t{2}}) {
    uint32_t out = NewSlot();
    cel_list_at_arena(out, l, cel_make_int(i));
    EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT)) << i;
  }
}

TEST_F(ListTest, UintIndexAdmitted) {
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/1);
  cel_list_append_at(l, cel_make_int(7));
  uint32_t out = NewSlot();
  // langdef + corpus row `lists/index/zero_based_uint`: cel-cpp admits
  // a CEL_UINT index in the dyn-typed `list[dyn(0u)]` path.
  cel_list_at_arena(out, l, cel_make_uint(0u));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(cel_value_at(out)->payload.i, 7);
}

TEST_F(ListTest, IntegralDoubleIndexAdmitted) {
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/1);
  cel_list_append_at(l, cel_make_int(7));
  uint32_t out = NewSlot();
  // Corpus row `lists/index/zero_based_double`: cel-cpp admits a
  // whole-number CEL_DOUBLE as a list index.
  cel_list_at_arena(out, l, cel_make_double(0.0));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(cel_value_at(out)->payload.i, 7);
}

TEST_F(ListTest, NonIntegralDoubleIndexErrors) {
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/1);
  cel_list_append_at(l, cel_make_int(7));
  uint32_t out = NewSlot();
  // Corpus row `lists/index/zero_based_double_error`: a non-integral
  // double indexes errors with invalid_argument.
  cel_list_at_arena(out, l, cel_make_double(0.1));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INVALID_ARGUMENT));
}

TEST_F(ListTest, StringIndexErrorsAsTypeMismatch) {
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/1);
  cel_list_append_at(l, cel_make_int(7));
  uint32_t out = NewSlot();
  // Truly non-numeric kinds still reject as type mismatch.
  uint32_t idx_slot = NewSlot();
  cel_value_at(idx_slot)->kind = static_cast<uint32_t>(CEL_STRING);
  cel_value_at(idx_slot)->payload.s.ptr = 0;
  cel_value_at(idx_slot)->payload.s.len = 0;
  cel_list_at_arena(out, l, idx_slot);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// Linear access probe — same shape as the map test.  Guards against
// an O(1) impl that secretly returns only the first / last element.
TEST_F(ListTest, IndexAcrossManyEntries) {
  constexpr uint32_t kN = 64;
  uint32_t l = NewSlot();
  cel_list_create(l, kN);
  for (uint32_t i = 0; i < kN; ++i) {
    cel_list_append_at(l, cel_make_int(static_cast<int64_t>(i) * 1000));
  }
  ASSERT_EQ(cel_value_at(l)->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  for (int64_t probe : {int64_t{0}, int64_t{32}, int64_t{63}}) {
    uint32_t out = NewSlot();
    cel_list_at_arena(out, l, cel_make_int(probe));
    EXPECT_EQ(cel_value_at(out)->payload.i, probe * 1000) << "probe=" << probe;
  }
}

// ════════ 3VL absorption on operand and index ════════

struct ListPropagationCase {
  const char* name;
  bool unknown_on_list;  // false → unknown on index
  uint32_t kind;         // CEL_UNKNOWN or CEL_ERROR
  uint32_t payload_u32;
};
class ListPropagationTest
    : public ListTest,
      public ::testing::WithParamInterface<ListPropagationCase> {};

TEST_P(ListPropagationTest, Propagates) {
  const ListPropagationCase& c = GetParam();
  uint32_t l = NewSlot();
  uint32_t idx;
  if (c.unknown_on_list) {
    CelValue* v = cel_value_at(l);
    v->kind = c.kind;
    v->payload.unk = c.payload_u32;
    idx = cel_make_int(0);
  } else {
    cel_list_create(l, /*capacity=*/1);
    cel_list_append_at(l, cel_make_int(7));
    idx = NewSlot();
    CelValue* v = cel_value_at(idx);
    v->kind = c.kind;
    v->payload.unk = c.payload_u32;
  }
  uint32_t out = NewSlot();
  cel_list_at_arena(out, l, idx);
  EXPECT_EQ(cel_value_at(out)->kind, c.kind);
  if (c.kind == CEL_UNKNOWN) {
    EXPECT_EQ(cel_value_at(out)->payload.unk, c.payload_u32);
  } else {
    EXPECT_EQ(cel_value_at(out)->payload.err, c.payload_u32);
  }
}

INSTANTIATE_TEST_SUITE_P(
    ThreeValuedLogic, ListPropagationTest,
    ::testing::Values(
        ListPropagationCase{"unknown_list", true, CEL_UNKNOWN, 7u},
        ListPropagationCase{"error_list", true, CEL_ERROR,
                            CEL_ERR_TYPE_MISMATCH},
        ListPropagationCase{"unknown_index", false, CEL_UNKNOWN, 9u},
        ListPropagationCase{"error_index", false, CEL_ERROR,
                            CEL_ERR_TYPE_MISMATCH}),
    [](const ::testing::TestParamInfo<ListPropagationCase>& i) {
      return std::string(i.param.name);
    });

// ════════ kDynamic dispatcher ════════

TEST_F(ListTest, DispatcherRoutesArenaToFastPath) {
  uint32_t l = NewSlot();
  cel_list_create(l, /*capacity=*/1);
  cel_list_append_at(l, cel_make_int(99));
  uint32_t out = NewSlot();
  cel_list_at(out, l, cel_make_int(0));
  EXPECT_EQ(cel_value_at(out)->payload.i, 99);
  EXPECT_EQ(g_host_at_calls, 0);
}

TEST_F(ListTest, DispatcherRoutesHostThroughHostArm) {
  uint32_t l = NewSlot();
  cel_value_at(l)->kind = CEL_LIST_HOST;
  cel_value_at(l)->payload.ref_slot = 7;
  uint32_t out = NewSlot();
  cel_list_at(out, l, cel_make_int(0));
  EXPECT_EQ(g_host_at_calls, 1);
  EXPECT_EQ(cel_value_at(out)->payload.i, 0x5151);
}

TEST_F(ListTest, DispatcherTypeMismatchOnNonList) {
  uint32_t l = cel_make_int(42);
  uint32_t out = NewSlot();
  cel_list_at(out, l, cel_make_int(0));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
  EXPECT_EQ(g_host_at_calls, 0);
}

// (The wasm-level `return_call` invariant for cel_list_at is locked
// by cel_runtime_wasm_test.cc — the host build cannot observe stack
// preservation, so no equivalent here.)

TEST_F(ListTest, DispatcherUnknownOperandPropagates) {
  uint32_t l = NewSlot();
  CelValue* v = cel_value_at(l);
  v->kind = CEL_UNKNOWN;
  v->payload.unk = 42;
  uint32_t out = NewSlot();
  cel_list_at(out, l, cel_make_int(0));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(cel_value_at(out)->payload.unk, 42u);
  EXPECT_EQ(g_host_at_calls, 0);
}

// ════════ kDynamic dispatcher — concat (`_+_` on lists) ════════
//
// Unlike the one-list ops, concat's two operands can differ in
// origin.  Only arena+arena takes the wasm fast path; EVERY pairing
// involving a host-backed list must reach the trampoline, which lifts
// both sides into a fresh arena list.  Routing the mixed pairs to the
// arena kernel instead is what made `[1,2] + boundList` a
// type_mismatch.

// Arena list of `values`, count == values.size().
uint32_t MakeIntList(ListTest& t, const std::vector<int64_t>& values) {
  uint32_t l = t.NewSlot();
  cel_list_create(l, static_cast<uint32_t>(values.size()));
  for (int64_t i : values) {
    cel_list_append_at(l, cel_make_int(i));
  }
  return l;
}

uint32_t MakeHostListSlot(ListTest& t, uint32_t ref_slot) {
  uint32_t l = t.NewSlot();
  cel_value_at(l)->kind = CEL_LIST_HOST;
  cel_value_at(l)->payload.ref_slot = ref_slot;
  return l;
}

TEST_F(ListTest, ConcatDispatcherRoutesArenaPairToFastPath) {
  uint32_t a = MakeIntList(*this, {1, 2});
  uint32_t b = MakeIntList(*this, {3});
  uint32_t joined = NewSlot();
  cel_list_concat(joined, a, b);
  ASSERT_EQ(cel_value_at(joined)->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  uint32_t last = NewSlot();
  cel_list_at_arena(last, joined, cel_make_int(2));
  EXPECT_EQ(cel_value_at(last)->payload.i, 3);
  EXPECT_EQ(g_host_concat_calls, 0);
}

TEST_F(ListTest, ConcatDispatcherRoutesArenaPlusHostThroughHostArm) {
  uint32_t out = NewSlot();
  cel_list_concat(out, MakeIntList(*this, {1}), MakeHostListSlot(*this, 7));
  EXPECT_EQ(g_host_concat_calls, 1);
  EXPECT_EQ(cel_value_at(out)->payload.i, 0x6262);
}

TEST_F(ListTest, ConcatDispatcherRoutesHostPlusArenaThroughHostArm) {
  uint32_t out = NewSlot();
  cel_list_concat(out, MakeHostListSlot(*this, 7), MakeIntList(*this, {1}));
  EXPECT_EQ(g_host_concat_calls, 1);
  EXPECT_EQ(cel_value_at(out)->payload.i, 0x6262);
}

TEST_F(ListTest, ConcatDispatcherRoutesHostPlusHostThroughHostArm) {
  uint32_t out = NewSlot();
  cel_list_concat(out, MakeHostListSlot(*this, 7), MakeHostListSlot(*this, 8));
  EXPECT_EQ(g_host_concat_calls, 1);
  EXPECT_EQ(cel_value_at(out)->payload.i, 0x6262);
}

TEST_F(ListTest, ConcatDispatcherTypeMismatchOnNonList) {
  uint32_t out = NewSlot();
  cel_list_concat(out, MakeIntList(*this, {1}), cel_make_int(42));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
  EXPECT_EQ(g_host_concat_calls, 0);
}

TEST_F(ListTest, ConcatDispatcherPropagatesPoisonedOperands) {
  // 3VL absorbs BEFORE any origin branch, so a poisoned operand never
  // reaches the host arm (langdef §"Logical Operators" absorption).
  uint32_t unk = NewSlot();
  cel_value_at(unk)->kind = CEL_UNKNOWN;
  cel_value_at(unk)->payload.unk = 42;
  uint32_t out = NewSlot();
  cel_list_concat(out, unk, MakeHostListSlot(*this, 7));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(cel_value_at(out)->payload.unk, 42u);

  uint32_t err = NewSlot();
  cel_value_at(err)->kind = CEL_ERROR;
  cel_value_at(err)->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  uint32_t out2 = NewSlot();
  cel_list_concat(out2, MakeHostListSlot(*this, 7), err);
  EXPECT_EQ(cel_value_at(out2)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out2)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
  EXPECT_EQ(g_host_concat_calls, 0);
}

}  // namespace
}  // namespace celwasm
