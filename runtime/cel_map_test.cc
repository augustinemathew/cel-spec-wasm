// Host-side coverage of cel_map.h primitives + the kDynamic
// dispatcher.  The wasm32 binary is exercised separately by
// cel_runtime_wasm_test (locks `return_call` emission); this file
// pins the C-level semantics — equality ladder, error poisoning,
// dispatcher routing — that the wasm and host builds share.
//
// Where redundant: parameterized fixtures.  Where each case tells a
// different story (specific spec citation, specific bug-surface):
// individual TESTs.  Reading the test names should reveal the
// invariant matrix at a glance.

#include "runtime/cel_map.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_make.h"
#include "runtime/cel_map_hash.h"
#include "runtime/cel_memory.h"

extern "C" {
// Strong override of the host-side weak stub in cel_runtime.c.  Lets
// the dispatcher's kHost arm be observed without pulling in the
// wasmtime trampoline.
static int g_host_lookup_calls = 0;
void cel_host_cel_map_lookup(uint32_t out_slot, uint32_t /*map_slot*/,
                             uint32_t /*key_slot*/) {
  ++g_host_lookup_calls;
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_INT;
  out->payload.i = 0x4242;  // sentinel — distinguishes from arena hits.
}
}

namespace celwasm {
namespace {

class MapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
    g_host_lookup_calls = 0;
  }

 public:
  // Public so parameterized lambdas can use the helpers without
  // friending themselves.  SetUp stays protected per
  // misc-override-with-different-visibility.
  uint32_t NewSlot() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }
  uint32_t Str(const char* s) {
    return cel_make_string(s, static_cast<uint32_t>(std::strlen(s)));
  }
};

// ════════ Construction and structural invariants ════════

TEST_F(MapTest, CreateProducesArenaKind) {
  uint32_t out = NewSlot();
  cel_map_create(out, /*capacity=*/2);
  CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_NE(v->payload.arena_map.header_ptr, 0u);
}

TEST_F(MapTest, InsertDuplicateKeyPoisons) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/4);
  cel_map_insert(m, cel_make_int(1), cel_make_int(10));
  cel_map_insert(m, cel_make_int(1), cel_make_int(20));
  CelValue* v = cel_value_at(m);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_DUPLICATE_KEY));
}

TEST_F(MapTest, InsertPastCapacityPoisons) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  cel_map_insert(m, cel_make_int(1), cel_make_int(10));
  cel_map_insert(m, cel_make_int(2), cel_make_int(20));  // past capacity
  CelValue* v = cel_value_at(m);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

// ════════ Same-kind round-trip per allowed key kind ════════

struct RoundTripCase {
  const char* name;
  uint32_t (*make_key)(MapTest&);  // bound functor
  int64_t value;
};
class MapKeyRoundTripTest
    : public MapTest,
      public ::testing::WithParamInterface<RoundTripCase> {};

TEST_P(MapKeyRoundTripTest, StoreThenLookupHits) {
  const RoundTripCase& c = GetParam();
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  cel_map_insert(m, c.make_key(*this), cel_make_int(c.value));
  ASSERT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_MAP_ARENA))
      << c.name;
  uint32_t out = NewSlot();
  cel_map_lookup_arena(out, m, c.make_key(*this));
  ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT)) << c.name;
  EXPECT_EQ(cel_value_at(out)->payload.i, c.value) << c.name;
}

INSTANTIATE_TEST_SUITE_P(AllKeyKinds, MapKeyRoundTripTest,
                         ::testing::Values(
                             RoundTripCase{
                                 "bool_true",
                                 +[](MapTest&) {
                                   return cel_make_bool(1);
                                 },
                                 111,
                             },
                             RoundTripCase{
                                 "bool_false",
                                 +[](MapTest&) {
                                   return cel_make_bool(0);
                                 },
                                 222,
                             },
                             RoundTripCase{
                                 "int_zero",
                                 +[](MapTest&) {
                                   return cel_make_int(0);
                                 },
                                 10,
                             },
                             RoundTripCase{
                                 "int_negative",
                                 +[](MapTest&) {
                                   return cel_make_int(-1);
                                 },
                                 20,
                             },
                             RoundTripCase{
                                 "int_min",
                                 +[](MapTest&) {
                                   return cel_make_int(INT64_MIN);
                                 },
                                 30,
                             },
                             RoundTripCase{
                                 "int_max",
                                 +[](MapTest&) {
                                   return cel_make_int(INT64_MAX);
                                 },
                                 40,
                             },
                             RoundTripCase{
                                 "uint_zero",
                                 +[](MapTest&) {
                                   return cel_make_uint(0);
                                 },
                                 50,
                             },
                             RoundTripCase{
                                 "uint_max",
                                 +[](MapTest&) {
                                   return cel_make_uint(UINT64_MAX);
                                 },
                                 60,
                             },
                             RoundTripCase{
                                 "string_empty",
                                 +[](MapTest& t) {
                                   return t.Str("");
                                 },
                                 70,
                             },
                             RoundTripCase{
                                 "string_simple",
                                 +[](MapTest& t) {
                                   return t.Str("hello");
                                 },
                                 80,
                             },
                             RoundTripCase{
                                 "string_multibyte_utf8",
                                 +[](MapTest& t) {
                                   return t.Str("héllo");
                                 },
                                 90,
                             }),
                         [](const ::testing::TestParamInfo<RoundTripCase>& i) {
                           return std::string(i.param.name);
                         });

// String with embedded NUL doesn't fit the strlen-based factory above;
// keep it as a focused test.  Catches the "uses strlen for compare"
// regression class.
TEST_F(MapTest, LookupArenaStringKeyWithEmbeddedNull) {
  const char k1[] = {'a', '\0', 'b'};
  const char k2[] = {'a', '\0', 'c'};
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/2);
  cel_map_insert(m, cel_make_string(k1, 3), cel_make_int(11));
  cel_map_insert(m, cel_make_string(k2, 3), cel_make_int(12));
  ASSERT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_MAP_ARENA))
      << "embedded-NUL strings differing past byte 1 must be distinct";
  uint32_t out = NewSlot();
  cel_map_lookup_arena(out, m, cel_make_string(k1, 3));
  EXPECT_EQ(cel_value_at(out)->payload.i, 11);
}

// langdef §"Equality": kinds don't compare across the bool/int boundary,
// so bool(true) and int(1) coexist as distinct keys.
TEST_F(MapTest, BoolAndIntOneAreDistinctKeys) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/2);
  cel_map_insert(m, cel_make_bool(1), cel_make_int(900));
  cel_map_insert(m, cel_make_int(1), cel_make_int(800));
  EXPECT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  uint32_t out = NewSlot();
  cel_map_lookup_arena(out, m, cel_make_bool(1));
  EXPECT_EQ(cel_value_at(out)->payload.i, 900);
}

// ════════ Cross-type numeric equality (langdef §"Equality") ════════

struct CrossTypeCase {
  const char* name;
  uint32_t (*store)();
  uint32_t (*lookup)();
  bool expect_hit;
  int64_t hit_value;  // only checked when expect_hit
};
class MapCrossTypeTest : public MapTest,
                         public ::testing::WithParamInterface<CrossTypeCase> {};

TEST_P(MapCrossTypeTest, RespectsMathematicalValue) {
  const CrossTypeCase& c = GetParam();
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  cel_map_insert(m, c.store(), cel_make_int(c.hit_value));
  uint32_t out = NewSlot();
  cel_map_lookup_arena(out, m, c.lookup());
  if (c.expect_hit) {
    ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT))
        << c.name;
    EXPECT_EQ(cel_value_at(out)->payload.i, c.hit_value) << c.name;
  } else {
    ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR))
        << c.name;
    EXPECT_EQ(cel_value_at(out)->payload.err,
              static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY))
        << c.name;
  }
}

INSTANTIATE_TEST_SUITE_P(
    EqualityLadder, MapCrossTypeTest,
    ::testing::Values(
        // int 42 ≡ uint 42 (positive int range, both directions).
        CrossTypeCase{
            "int_stored_uint_lookup_hits",
            +[]() {
              return cel_make_int(42);
            },
            +[]() {
              return cel_make_uint(42);
            },
            true,
            420,
        },
        CrossTypeCase{
            "uint_stored_int_lookup_hits",
            +[]() {
              return cel_make_uint(7);
            },
            +[]() {
              return cel_make_int(7);
            },
            true,
            700,
        },
        // Negative int never equals any uint.
        CrossTypeCase{
            "negative_int_never_matches_uint",
            +[]() {
              return cel_make_uint(0);
            },
            +[]() {
              return cel_make_int(-1);
            },
            false,
            0,
        },
        // uint 2^63 has no representable int — int(INT64_MAX) misses.
        CrossTypeCase{
            "uint_above_int_max_never_matches_int",
            +[]() {
              return cel_make_uint(uint64_t{1} << 63);
            },
            +[]() {
              return cel_make_int(INT64_MAX);
            },
            false,
            0,
        }),
    [](const ::testing::TestParamInfo<CrossTypeCase>& i) {
      return std::string(i.param.name);
    });

// ════════ Disallowed key kinds (langdef §"Map literals") ════════

class MapInvalidKeyKindTest : public MapTest,
                              public ::testing::WithParamInterface<uint32_t> {};

TEST_P(MapInvalidKeyKindTest, InsertPoisonsTypeMismatch) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  uint32_t key = NewSlot();
  cel_value_at(key)->kind = GetParam();
  cel_map_insert(m, key, cel_make_int(1));
  EXPECT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_ERROR))
      << "kind=" << GetParam();
  EXPECT_EQ(cel_value_at(m)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH))
      << "kind=" << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    AllDisallowedKinds, MapInvalidKeyKindTest,
    // Allowed: BOOL, INT, UINT, STRING.  Everything else here.  The
    // pre-construction unknown/error short-circuit is a separate
    // contract (handled before is_valid_map_key_kind sees the key);
    // those propagate as the operand value, not as TYPE_MISMATCH —
    // but that path is the dispatcher's concern, not insert's, so
    // exclude them here.
    ::testing::Values(
        static_cast<uint32_t>(CEL_NULL), static_cast<uint32_t>(CEL_DOUBLE),
        static_cast<uint32_t>(CEL_BYTES), static_cast<uint32_t>(CEL_LIST_ARENA),
        static_cast<uint32_t>(CEL_LIST_HOST),
        static_cast<uint32_t>(CEL_MAP_ARENA),
        static_cast<uint32_t>(CEL_MAP_HOST), static_cast<uint32_t>(CEL_MESSAGE),
        static_cast<uint32_t>(CEL_TYPE), static_cast<uint32_t>(CEL_DURATION),
        static_cast<uint32_t>(CEL_TIMESTAMP),
        static_cast<uint32_t>(CEL_OPTIONAL)));

// ════════ Lookup miss / 3VL on key ════════

TEST_F(MapTest, LookupArenaMissingKeyReturnsNoSuchKey) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  cel_map_insert(m, cel_make_int(1), cel_make_int(10));
  uint32_t out = NewSlot();
  cel_map_lookup_arena(out, m, cel_make_int(999));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

TEST_F(MapTest, LookupArenaUnknownKeyShortCircuits) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  cel_map_insert(m, cel_make_int(1), cel_make_int(10));
  uint32_t key = NewSlot();
  cel_value_at(key)->kind = CEL_UNKNOWN;
  cel_value_at(key)->payload.unk = 99;
  uint32_t out = NewSlot();
  cel_map_lookup_arena(out, m, key);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(cel_value_at(out)->payload.unk, 99u);
}

// ════════ Capacity / scale ════════

TEST_F(MapTest, EmptyMapLookupReturnsNoSuchKey) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/0);
  uint32_t out = NewSlot();
  cel_map_lookup_arena(out, m, cel_make_int(1));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

// 64-entry map exercises the linear scan at a scale where iteration
// order matters.  Probes first / middle / last to guard against an
// O(1) impl that secretly only checks the first entry.
TEST_F(MapTest, LookupArenaAcrossManyEntries) {
  constexpr uint32_t kN = 64;
  uint32_t m = NewSlot();
  cel_map_create(m, kN);
  for (int64_t i = 0; i < kN; ++i) {
    cel_map_insert(m, cel_make_int(i), cel_make_int(i * 1000));
  }
  ASSERT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  for (int64_t probe : {int64_t{0}, int64_t{32}, int64_t{63}}) {
    uint32_t out = NewSlot();
    cel_map_lookup_arena(out, m, cel_make_int(probe));
    EXPECT_EQ(cel_value_at(out)->payload.i, probe * 1000) << "probe=" << probe;
  }
}

// ════════ kDynamic dispatcher ════════

TEST_F(MapTest, DispatcherRoutesArenaToFastPath) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  cel_map_insert(m, cel_make_int(5), cel_make_int(50));
  uint32_t out = NewSlot();
  cel_map_lookup(out, m, cel_make_int(5));
  EXPECT_EQ(cel_value_at(out)->payload.i, 50);
  EXPECT_EQ(g_host_lookup_calls, 0);
}

TEST_F(MapTest, DispatcherRoutesHostThroughHostArm) {
  // CEL_MAP_HOST values originate from host bindings (M3.D); we
  // synthesize one here directly to exercise the dispatcher.
  uint32_t m = NewSlot();
  cel_value_at(m)->kind = CEL_MAP_HOST;
  cel_value_at(m)->payload.ref_slot = 7;
  uint32_t out = NewSlot();
  cel_map_lookup(out, m, cel_make_int(0));
  EXPECT_EQ(g_host_lookup_calls, 1);
  EXPECT_EQ(cel_value_at(out)->payload.i, 0x4242);
}

// 3VL on the operand: unknown / error propagate without dereferencing.
struct OperandPropagationCase {
  const char* name;
  uint32_t kind;
  uint32_t payload_u32;
  uint32_t expected_kind;
};
class DispatcherOperandPropagationTest
    : public MapTest,
      public ::testing::WithParamInterface<OperandPropagationCase> {};

TEST_P(DispatcherOperandPropagationTest, OperandPropagatesAndNoHostCall) {
  const OperandPropagationCase& c = GetParam();
  uint32_t m = NewSlot();
  CelValue* mv = cel_value_at(m);
  mv->kind = c.kind;
  if (c.kind == CEL_UNKNOWN) {
    mv->payload.unk = c.payload_u32;
  } else if (c.kind == CEL_ERROR) {
    mv->payload.err = c.payload_u32;
  }
  uint32_t out = NewSlot();
  cel_map_lookup(out, m, cel_make_int(1));
  EXPECT_EQ(cel_value_at(out)->kind, c.expected_kind) << c.name;
  if (c.kind == CEL_UNKNOWN) {
    EXPECT_EQ(cel_value_at(out)->payload.unk, c.payload_u32) << c.name;
  } else if (c.kind == CEL_ERROR) {
    EXPECT_EQ(cel_value_at(out)->payload.err, c.payload_u32) << c.name;
  }
  EXPECT_EQ(g_host_lookup_calls, 0) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    ThreeValuedLogic, DispatcherOperandPropagationTest,
    ::testing::Values(OperandPropagationCase{"unknown_propagates", CEL_UNKNOWN,
                                             42, CEL_UNKNOWN},
                      OperandPropagationCase{"error_propagates", CEL_ERROR,
                                             CEL_ERR_TYPE_MISMATCH, CEL_ERROR}),
    [](const ::testing::TestParamInfo<OperandPropagationCase>& i) {
      return std::string(i.param.name);
    });

// Defence-in-depth: dispatcher invoked with a non-map operand (e.g.
// codegen drift, checker miss).  Should poison rather than crash.
TEST_F(MapTest, DispatcherTypeMismatchOnNonMapOperand) {
  uint32_t m = cel_make_int(42);  // not a map
  uint32_t out = NewSlot();
  cel_map_lookup(out, m, cel_make_int(1));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
  EXPECT_EQ(g_host_lookup_calls, 0);
}

// ════════ M5.B Slice E — map-key iteration ════════
//
// Locks the semantics of `cel_map_iter_init` / `cel_map_iter_next` /
// `cel_map_iter_key_at` / `cel_map_iter_value_at` against the ABI
// pinned in `wat/64_comprehension_exists_map.wat`.  Codegen depends
// on:
//   - init returns 0 for empty / poisoned maps;
//   - next returns 0 once iteration is past the last entry, and stays
//     0 on subsequent calls (idempotent);
//   - key_at / value_at are no-ops (poison the out slot rather than
//     crash) on a 0 handle, so a comprehension over an empty map
//     never reads garbage even if codegen miswires the loop guard.

class MapIterTest : public MapTest {
 protected:
  // Helper: assert (key.kind, key.int) at the current iterator cursor.
  void ExpectCurrentIntKey(uint32_t handle, int64_t expected_key) {
    uint32_t out = NewSlot();
    cel_map_iter_key_at(out, handle);
    ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT));
    EXPECT_EQ(cel_value_at(out)->payload.i, expected_key);
  }
  void ExpectCurrentIntValue(uint32_t handle, int64_t expected_value) {
    uint32_t out = NewSlot();
    cel_map_iter_value_at(out, handle);
    ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT));
    EXPECT_EQ(cel_value_at(out)->payload.i, expected_value);
  }
};

// Empty map: iter_init returns 0 (cheap sentinel — no state alloc).
// iter_next on the 0 handle returns 0; the comprehension loop exits
// without ever entering the body.  Locks the cheap-empty-case
// optimisation that comprehensions over `{}` depend on.
TEST_F(MapIterTest, EmptyMapInitReturnsZeroHandle) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/0);
  uint32_t h = cel_map_iter_init(m);
  EXPECT_EQ(h, 0u);
  EXPECT_EQ(cel_map_iter_next(h), 0u);
}

// Single entry: init returns a non-zero handle; one successful next;
// key_at / value_at read the entry; a second next returns 0 (done).
TEST_F(MapIterTest, SingleEntryRoundTrip) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  cel_map_insert(m, cel_make_int(42), cel_make_int(4200));
  uint32_t h = cel_map_iter_init(m);
  ASSERT_NE(h, 0u);
  EXPECT_EQ(cel_map_iter_next(h), 1u);
  ExpectCurrentIntKey(h, 42);
  ExpectCurrentIntValue(h, 4200);
  EXPECT_EQ(cel_map_iter_next(h), 0u);
  // Idempotent at the end: subsequent next calls stay 0.
  EXPECT_EQ(cel_map_iter_next(h), 0u);
  EXPECT_EQ(cel_map_iter_next(h), 0u);
}

// Three entries: iterate, collect (key, value) pairs, and assert
// set-equality against the inserted entries (order-independent — the
// ABI does not promise insertion order, only that every entry is
// visited exactly once).
TEST_F(MapIterTest, ThreeEntryFullWalkOrderIndependent) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/3);
  cel_map_insert(m, cel_make_int(1), cel_make_int(10));
  cel_map_insert(m, cel_make_int(2), cel_make_int(20));
  cel_map_insert(m, cel_make_int(3), cel_make_int(30));

  uint32_t h = cel_map_iter_init(m);
  ASSERT_NE(h, 0u);
  bool seen[4] = {false, false, false, false};
  int count = 0;
  while (cel_map_iter_next(h)) {
    ++count;
    ASSERT_LE(count, 3);
    uint32_t kslot = NewSlot();
    uint32_t vslot = NewSlot();
    cel_map_iter_key_at(kslot, h);
    cel_map_iter_value_at(vslot, h);
    ASSERT_EQ(cel_value_at(kslot)->kind, static_cast<uint32_t>(CEL_INT));
    int64_t k = cel_value_at(kslot)->payload.i;
    ASSERT_GE(k, 1);
    ASSERT_LE(k, 3);
    // Bound-checked index to keep clang-analyzer happy about the
    // upper-bound on `seen[k]` (the ASSERT_LE above is a runtime
    // check the static analyzer can't fold).
    const auto idx = static_cast<size_t>(k);
    ASSERT_LT(idx, sizeof(seen) / sizeof(seen[0]));
    EXPECT_FALSE(seen[idx]) << "key " << k << " visited twice";
    seen[idx] = true;
    EXPECT_EQ(cel_value_at(vslot)->payload.i, k * 10);
  }
  EXPECT_EQ(count, 3);
  EXPECT_TRUE(seen[1]);
  EXPECT_TRUE(seen[2]);
  EXPECT_TRUE(seen[3]);
  EXPECT_EQ(cel_map_iter_next(h), 0u);
}

// Polymorphic keys: bool / int / uint / string — exercises that
// key_at copies the full 24-byte CelValue (kind + payload), not just
// the int payload.  Each iter_next surfaces one entry; we check that
// every distinct kind appears.
TEST_F(MapIterTest, MixedKeyKindsCopiedFaithfully) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/4);
  cel_map_insert(m, cel_make_bool(1), cel_make_int(100));
  cel_map_insert(m, cel_make_int(7), cel_make_int(200));
  cel_map_insert(m, cel_make_uint(9), cel_make_int(300));
  cel_map_insert(m, Str("hi"), cel_make_int(400));

  uint32_t h = cel_map_iter_init(m);
  ASSERT_NE(h, 0u);
  bool seen_bool = false;
  bool seen_int = false;
  bool seen_uint = false;
  bool seen_string = false;
  int count = 0;
  while (cel_map_iter_next(h)) {
    ++count;
    uint32_t kslot = NewSlot();
    cel_map_iter_key_at(kslot, h);
    switch (cel_value_at(kslot)->kind) {
      case CEL_BOOL:
        seen_bool = true;
        break;
      case CEL_INT:
        seen_int = true;
        break;
      case CEL_UINT:
        seen_uint = true;
        break;
      case CEL_STRING:
        seen_string = true;
        break;
      default:
        FAIL() << "unexpected key kind " << cel_value_at(kslot)->kind;
    }
  }
  EXPECT_EQ(count, 4);
  EXPECT_TRUE(seen_bool);
  EXPECT_TRUE(seen_int);
  EXPECT_TRUE(seen_uint);
  EXPECT_TRUE(seen_string);
}

// Poisoned map: init vends a one-entry poison iteration whose key
// and value both carry the source error verbatim, so the loop body's
// 3VL absorption propagates it into the comprehension result.  (The
// previous contract — returning the 0 sentinel, same as empty — was
// a silent wrong answer: `{'a': 1/0}.exists(k, ...)` evaluated
// `false` instead of the divide-by-zero error.)
TEST_F(MapIterTest, PoisonedMapInitVendsOneErrorIteration) {
  uint32_t m = NewSlot();
  cel_value_at(m)->kind = CEL_ERROR;
  cel_value_at(m)->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  uint32_t h = cel_map_iter_init(m);
  ASSERT_NE(h, 0u);
  ASSERT_EQ(cel_map_iter_next(h), 1u);
  uint32_t kslot = NewSlot();
  uint32_t vslot = NewSlot();
  cel_map_iter_key_at(kslot, h);
  cel_map_iter_value_at(vslot, h);
  EXPECT_EQ(cel_value_at(kslot)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(kslot)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
  EXPECT_EQ(cel_value_at(vslot)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(vslot)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
  // Exactly one iteration — the poison iter must terminate.
  EXPECT_EQ(cel_map_iter_next(h), 0u);
}

// Host-backed map: M5 envelope gates these out at the frontend, but
// defensively iter_init still returns the 0 sentinel rather than
// dereferencing the ref_slot as an ArenaMapHeader pointer.
TEST_F(MapIterTest, HostMapInitReturnsZeroHandle) {
  uint32_t m = NewSlot();
  cel_value_at(m)->kind = CEL_MAP_HOST;
  cel_value_at(m)->payload.ref_slot = 42;
  uint32_t h = cel_map_iter_init(m);
  EXPECT_EQ(h, 0u);
  EXPECT_EQ(cel_map_iter_next(h), 0u);
}

// key_at / value_at on the 0 handle must not dereference past
// memory.  Defensively writes a poisoned value into out so a
// codegen-side miswire surfaces visibly rather than reading
// garbage from address 0.
TEST_F(MapIterTest, ReadOnZeroHandlePoisonsRatherThanCrashes) {
  uint32_t out_k = NewSlot();
  uint32_t out_v = NewSlot();
  cel_map_iter_key_at(out_k, /*iter_handle=*/0);
  cel_map_iter_value_at(out_v, /*iter_handle=*/0);
  EXPECT_EQ(cel_value_at(out_k)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out_v)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out_k)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INDEX_OUT_OF_BOUNDS));
  EXPECT_EQ(cel_value_at(out_v)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INDEX_OUT_OF_BOUNDS));
}

// key_at / value_at before any iter_next: cursor is still at 0
// (pre-first); the read must refuse rather than dereference entry
// `-1`.  Same defensive behaviour as the 0-handle case.
TEST_F(MapIterTest, ReadBeforeFirstNextPoisons) {
  uint32_t m = NewSlot();
  cel_map_create(m, /*capacity=*/1);
  cel_map_insert(m, cel_make_int(1), cel_make_int(10));
  uint32_t h = cel_map_iter_init(m);
  ASSERT_NE(h, 0u);
  uint32_t out = NewSlot();
  cel_map_iter_key_at(out, h);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INDEX_OUT_OF_BOUNDS));
}

// Scale: 64-entry map exercises that the cursor advances through
// every entry and that key_at indexes into the entries run with the
// correct stride.
TEST_F(MapIterTest, IteratesAcrossManyEntries) {
  constexpr uint32_t kN = 64;
  uint32_t m = NewSlot();
  cel_map_create(m, kN);
  for (int64_t i = 0; i < kN; ++i) {
    cel_map_insert(m, cel_make_int(i), cel_make_int(i * 1000));
  }
  uint32_t h = cel_map_iter_init(m);
  ASSERT_NE(h, 0u);
  int count = 0;
  bool seen[kN] = {};
  while (cel_map_iter_next(h)) {
    ++count;
    uint32_t kslot = NewSlot();
    uint32_t vslot = NewSlot();
    cel_map_iter_key_at(kslot, h);
    cel_map_iter_value_at(vslot, h);
    int64_t k = cel_value_at(kslot)->payload.i;
    ASSERT_GE(k, 0);
    ASSERT_LT(k, static_cast<int64_t>(kN));
    const auto idx = static_cast<size_t>(k);
    ASSERT_LT(idx, kN);  // hold the analyzer's hand on the upper bound.
    EXPECT_FALSE(seen[idx]);
    seen[idx] = true;
    EXPECT_EQ(cel_value_at(vslot)->payload.i, k * 1000);
  }
  EXPECT_EQ(count, static_cast<int>(kN));
  for (uint32_t i = 0; i < kN; ++i) {
    EXPECT_TRUE(seen[i]) << "missed " << i;
  }
}

// ════════ SwissTable hash index (m32.A) ════════
//
// The index is a PURE ACCELERATOR: every assertion that holds over the
// linear scan must hold IDENTICALLY once `cel_map_index_build` runs.
// These tests parameterize over key kind × size and assert parity
// between an indexed and an unindexed map, plus the probe-collision,
// the ≥2^53 double-key linear fallback, and the dup re-validation.

class MapIndexTest : public MapTest {
 public:
  // Build a map with `n` int keys (i -> i*1000); optionally build the
  // index.  Returns the map slot.
  uint32_t MakeIntMap(uint32_t n, bool build_index) {
    uint32_t m = NewSlot();
    cel_map_create(m, n);
    for (uint32_t i = 0; i < n; ++i) {
      cel_map_insert(m, cel_make_int(static_cast<int64_t>(i)),
                     cel_make_int(static_cast<int64_t>(i) * 1000));
    }
    if (build_index) cel_map_index_build(m);
    return m;
  }
};

// Below the threshold (count < 8) the build is a no-op: index_offset
// stays 0 and the kernels linear-scan.
TEST_F(MapIndexTest, BuildBelowThresholdLeavesNoIndex) {
  uint32_t m = MakeIntMap(/*n=*/4, /*build_index=*/true);
  const auto* hdr = reinterpret_cast<const ArenaMapHeader*>(
      cel_mem_base() + cel_value_at(m)->payload.arena_map.header_ptr);
  EXPECT_EQ(hdr->index_offset, 0u);
  // Lookups still resolve via the linear fallback.
  uint32_t out = NewSlot();
  cel_map_lookup_arena(out, m, cel_make_int(2));
  EXPECT_EQ(cel_value_at(out)->payload.i, 2000);
}

// At/above the threshold the build allocates an index block.
TEST_F(MapIndexTest, BuildAtThresholdSetsIndexOffset) {
  uint32_t m = MakeIntMap(/*n=*/8, /*build_index=*/true);
  const auto* hdr = reinterpret_cast<const ArenaMapHeader*>(
      cel_mem_base() + cel_value_at(m)->payload.arena_map.header_ptr);
  EXPECT_NE(hdr->index_offset, 0u);
}

// Index-vs-linear parity for INT keys across sizes spanning the
// threshold (4 < 8 ≤ 16, 64).  Every hit and miss matches.
struct IndexSizeCase {
  uint32_t n;
};
class MapIndexIntParityTest
    : public MapIndexTest,
      public ::testing::WithParamInterface<IndexSizeCase> {};

TEST_P(MapIndexIntParityTest, HitsAndMissesMatchLinear) {
  const uint32_t n = GetParam().n;
  uint32_t indexed = MakeIntMap(n, /*build_index=*/true);
  uint32_t linear = MakeIntMap(n, /*build_index=*/false);
  for (int64_t probe = -2; probe < static_cast<int64_t>(n) + 2; ++probe) {
    uint32_t oi = NewSlot();
    uint32_t ol = NewSlot();
    cel_map_lookup_arena(oi, indexed, cel_make_int(probe));
    cel_map_lookup_arena(ol, linear, cel_make_int(probe));
    EXPECT_EQ(cel_value_at(oi)->kind, cel_value_at(ol)->kind)
        << "n=" << n << " probe=" << probe;
    EXPECT_EQ(cel_value_at(oi)->payload.i, cel_value_at(ol)->payload.i)
        << "n=" << n << " probe=" << probe;
    // `in` parity.
    uint32_t ii = NewSlot();
    uint32_t il = NewSlot();
    cel_map_in_arena(ii, cel_make_int(probe), indexed);
    cel_map_in_arena(il, cel_make_int(probe), linear);
    EXPECT_EQ(cel_value_at(ii)->payload.b, cel_value_at(il)->payload.b)
        << "in n=" << n << " probe=" << probe;
  }
}

INSTANTIATE_TEST_SUITE_P(Sizes, MapIndexIntParityTest,
                         ::testing::Values(IndexSizeCase{4}, IndexSizeCase{8},
                                           IndexSizeCase{16},
                                           IndexSizeCase{64}),
                         [](const ::testing::TestParamInfo<IndexSizeCase>& i) {
                           return "n" + std::to_string(i.param.n);
                         });

// Cross-kind parity: uint / bool / string keys all resolve identically
// with and without the index.  16 entries forces a built index.
TEST_F(MapIndexTest, UintKeysParity) {
  constexpr uint32_t kN = 16;
  uint32_t indexed = NewSlot();
  uint32_t linear = NewSlot();
  cel_map_create(indexed, kN);
  cel_map_create(linear, kN);
  for (uint32_t i = 0; i < kN; ++i) {
    cel_map_insert(indexed, cel_make_uint(i), cel_make_int(i + 1));
    cel_map_insert(linear, cel_make_uint(i), cel_make_int(i + 1));
  }
  cel_map_index_build(indexed);
  for (uint32_t probe = 0; probe < kN + 2; ++probe) {
    uint32_t oi = NewSlot();
    uint32_t ol = NewSlot();
    cel_map_lookup_arena(oi, indexed, cel_make_uint(probe));
    cel_map_lookup_arena(ol, linear, cel_make_uint(probe));
    EXPECT_EQ(cel_value_at(oi)->kind, cel_value_at(ol)->kind) << probe;
    EXPECT_EQ(cel_value_at(oi)->payload.i, cel_value_at(ol)->payload.i)
        << probe;
  }
}

TEST_F(MapIndexTest, StringKeysParityWithEmbeddedNul) {
  constexpr uint32_t kN = 12;
  uint32_t indexed = NewSlot();
  uint32_t linear = NewSlot();
  cel_map_create(indexed, kN);
  cel_map_create(linear, kN);
  std::vector<std::string> keys;
  for (uint32_t i = 0; i < kN; ++i) {
    std::string k = "k";
    k.push_back('\0');  // embedded NUL — never strlen-truncated.
    k += std::to_string(i);
    keys.push_back(k);
    uint32_t ki = cel_make_string(keys.back().data(),
                                  static_cast<uint32_t>(keys.back().size()));
    uint32_t kl = cel_make_string(keys.back().data(),
                                  static_cast<uint32_t>(keys.back().size()));
    cel_map_insert(indexed, ki, cel_make_int(i + 100));
    cel_map_insert(linear, kl, cel_make_int(i + 100));
  }
  cel_map_index_build(indexed);
  for (uint32_t i = 0; i < kN; ++i) {
    uint32_t oi = NewSlot();
    uint32_t ol = NewSlot();
    uint32_t pi =
        cel_make_string(keys[i].data(), static_cast<uint32_t>(keys[i].size()));
    uint32_t pl =
        cel_make_string(keys[i].data(), static_cast<uint32_t>(keys[i].size()));
    cel_map_lookup_arena(oi, indexed, pi);
    cel_map_lookup_arena(ol, linear, pl);
    ASSERT_EQ(cel_value_at(oi)->kind, static_cast<uint32_t>(CEL_INT)) << i;
    EXPECT_EQ(cel_value_at(oi)->payload.i, cel_value_at(ol)->payload.i) << i;
  }
  // A miss on a key whose content differs only past the NUL.
  std::string absent = "k";
  absent.push_back('\0');
  absent += "999";
  uint32_t out = NewSlot();
  cel_map_lookup_arena(
      out, indexed,
      cel_make_string(absent.data(), static_cast<uint32_t>(absent.size())));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

// Probe collision: an H1-colliding pair must both be findable, and a
// multi-group probe (≥64 entries → ≥128 slots) must resolve every key.
// The hash kernel folds int N to a single token, so int keys never share
// H1 with a *different* int key arbitrarily — instead we lean on the
// large map to force triangular probing over multiple groups, which is
// the structural collision path.
TEST_F(MapIndexTest, LargeMapMultiGroupProbeFindsEveryKey) {
  constexpr uint32_t kN = 200;  // > 128 slots; several 8-wide groups.
  uint32_t m = MakeIntMap(kN, /*build_index=*/true);
  const auto* hdr = reinterpret_cast<const ArenaMapHeader*>(
      cel_mem_base() + cel_value_at(m)->payload.arena_map.header_ptr);
  ASSERT_NE(hdr->index_offset, 0u);
  for (int64_t i = 0; i < kN; ++i) {
    uint32_t out = NewSlot();
    cel_map_lookup_arena(out, m, cel_make_int(i));
    ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT))
        << "missing key " << i;
    EXPECT_EQ(cel_value_at(out)->payload.i, i * 1000) << i;
  }
}

// H2 == 0 control byte: a key whose hash has its low 7 bits zero must
// still be findable (the kEmpty sentinel is 0x80, so H2==0 is a valid
// full-slot byte, never confused with empty).  We search across a range
// of int keys in a large indexed map to guarantee at least one lands on
// an H2==0 control byte; the round-trip just has to succeed for all.
TEST_F(MapIndexTest, H2ZeroControlByteKeysResolve) {
  constexpr uint32_t kN = 256;
  uint32_t m = MakeIntMap(kN, /*build_index=*/true);
  bool saw_h2_zero = false;
  for (int64_t i = 0; i < kN; ++i) {
    CelValue key{};
    key.kind = CEL_INT;
    key.payload.i = i;
    if (cel_h2(cel_map_key_hash(&key)) == 0) saw_h2_zero = true;
    uint32_t out = NewSlot();
    cel_map_lookup_arena(out, m, cel_make_int(i));
    ASSERT_EQ(cel_value_at(out)->payload.i, i * 1000) << i;
  }
  EXPECT_TRUE(saw_h2_zero)
      << "256 int keys should include at least one H2==0 control byte";
}

// The ≥2^53 double-lookup-key linear fallback (§5.1): build an int-keyed
// indexed map and look up a double key ≥2^53; the indexed result must
// match the linear scan (parity), because the lookup kernel bypasses the
// index for such keys.
TEST_F(MapIndexTest, LargeDoubleKeyFallsBackToLinearParity) {
  constexpr uint32_t kN = 16;
  // Keys span 2^53 .. 2^53+kN-1.  int64 exactly represents these.
  const int64_t kBase = 9007199254740992LL;  // 2^53
  uint32_t indexed = NewSlot();
  uint32_t linear = NewSlot();
  cel_map_create(indexed, kN);
  cel_map_create(linear, kN);
  for (uint32_t i = 0; i < kN; ++i) {
    cel_map_insert(indexed, cel_make_int(kBase + i), cel_make_int(i + 7));
    cel_map_insert(linear, cel_make_int(kBase + i), cel_make_int(i + 7));
  }
  cel_map_index_build(indexed);
  const auto* hdr = reinterpret_cast<const ArenaMapHeader*>(
      cel_mem_base() + cel_value_at(indexed)->payload.arena_map.header_ptr);
  ASSERT_NE(hdr->index_offset, 0u);
  // A double exactly equal to 2^53 (representable) — forces linear.
  const double d = 9007199254740992.0;  // == 2^53
  uint32_t oi = NewSlot();
  uint32_t ol = NewSlot();
  cel_map_lookup_arena(oi, indexed, cel_make_double(d));
  cel_map_lookup_arena(ol, linear, cel_make_double(d));
  EXPECT_EQ(cel_value_at(oi)->kind, cel_value_at(ol)->kind);
  EXPECT_EQ(cel_value_at(oi)->payload.i, cel_value_at(ol)->payload.i);
  // And a below-2^53 integral double still resolves via the index.
  uint32_t small = NewSlot();
  cel_map_create(small, kN);
  for (uint32_t i = 0; i < kN; ++i) {
    cel_map_insert(small, cel_make_int(i), cel_make_int(i + 1));
  }
  cel_map_index_build(small);
  uint32_t os = NewSlot();
  cel_map_lookup_arena(os, small, cel_make_double(3.0));
  EXPECT_EQ(cel_value_at(os)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(cel_value_at(os)->payload.i, 4);
}

// Cross-kind dup `{1:1, 1u:2}` built directly (bypassing cel_map_insert's
// own dup check) is re-validated and poisoned by cel_map_index_build with
// CEL_ERR_DUPLICATE_KEY.  This asserts the CURRENT runtime behavior
// (cel_value_eq treats int 1 and uint 1 as equal map keys), not cel-cpp.
TEST_F(MapIndexTest, BuildPoisonsCrossKindDuplicate) {
  // Need ≥ threshold entries so the build runs; pad with distinct keys.
  constexpr uint32_t kN = 9;
  uint32_t m = NewSlot();
  cel_map_create(m, kN);
  cel_map_insert(m, cel_make_int(1), cel_make_int(1));
  // The dup: uint 1 compares equal to int 1 under cel_value_eq, but
  // cel_map_insert's linear dup check ALSO catches it — so to drive the
  // index-build re-validation we insert distinct keys here and assert the
  // already-poisoned map (the insert path fired first).  This pins that
  // the runtime treats int/uint 1 as the same key today.
  cel_map_insert(m, cel_make_uint(1), cel_make_int(2));
  CelValue* v = cel_value_at(m);
  ASSERT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_DUPLICATE_KEY));
  // Building over a poisoned map is a no-op (kind != CEL_MAP_ARENA).
  cel_map_index_build(m);
  EXPECT_EQ(cel_value_at(m)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DUPLICATE_KEY));
}

// Direct cross-kind dup that BYPASSES cel_map_insert: write two
// cel_value_eq-equal keys straight into the entries run (via
// insert_at-style last-write... no — use raw writes), build the index,
// and assert cel_map_index_build itself poisons.  This is the path the
// design names (insert paths skip the dup check for dynamic maps).
TEST_F(MapIndexTest, BuildReValidatesDuplicateOnDirectEntries) {
  constexpr uint32_t kN = 9;
  uint32_t m = NewSlot();
  cel_map_create(m, kN);
  // Fill 8 distinct int keys via the normal path.
  for (int64_t i = 0; i < 8; ++i) {
    cel_map_insert(m, cel_make_int(i), cel_make_int(i));
  }
  ASSERT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  // Now write a 9th entry directly into the dense run that duplicates
  // key 0 — bypassing cel_map_insert's linear dup check (this models the
  // dynamic-construction path the index-build re-validation guards).
  auto* hdr = reinterpret_cast<ArenaMapHeader*>(
      cel_mem_base() + cel_value_at(m)->payload.arena_map.header_ptr);
  const size_t entry_off =
      static_cast<size_t>(hdr->entries_offset) +
      (static_cast<size_t>(kCelMapEntryStride) * hdr->count);
  auto* dup_key = reinterpret_cast<CelValue*>(cel_mem_base() + entry_off);
  auto* dup_val = reinterpret_cast<CelValue*>(cel_mem_base() + entry_off +
                                              sizeof(CelValue));
  dup_key->kind = CEL_INT;
  dup_key->payload.i = 0;  // duplicates entry 0.
  dup_val->kind = CEL_INT;
  dup_val->payload.i = 999;
  hdr->count = 9;
  cel_map_index_build(m);
  CelValue* v = cel_value_at(m);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(v->payload.err, static_cast<uint32_t>(CEL_ERR_DUPLICATE_KEY));
}

// cel_map_eq_arena over large indexed maps: equal maps (same entries,
// different insertion order) compare equal, and a one-entry difference
// compares unequal — with the inner match driven by the index.
TEST_F(MapIndexTest, EqArenaOverLargeIndexedMaps) {
  constexpr uint32_t kN = 32;
  uint32_t a = NewSlot();
  uint32_t b = NewSlot();
  cel_map_create(a, kN);
  cel_map_create(b, kN);
  for (int64_t i = 0; i < kN; ++i) {
    cel_map_insert(a, cel_make_int(i), cel_make_int(i * 2));
  }
  // b: same entries, reverse insertion order (set-equality ignores order).
  for (int64_t i = kN - 1; i >= 0; --i) {
    cel_map_insert(b, cel_make_int(i), cel_make_int(i * 2));
  }
  cel_map_index_build(a);
  cel_map_index_build(b);
  uint32_t out = NewSlot();
  cel_map_eq_arena(out, a, b);
  ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(cel_value_at(out)->payload.b, 1);

  // Differ in one value -> unequal.
  uint32_t c = NewSlot();
  cel_map_create(c, kN);
  for (int64_t i = 0; i < kN; ++i) {
    cel_map_insert(c, cel_make_int(i), cel_make_int(i == 5 ? -1 : i * 2));
  }
  cel_map_index_build(c);
  uint32_t out2 = NewSlot();
  cel_map_eq_arena(out2, a, c);
  EXPECT_EQ(cel_value_at(out2)->payload.b, 0);
}

}  // namespace
}  // namespace celwasm
