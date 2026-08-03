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
#include <limits>
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

// Strong override of the `cel_host.cel_map_iter_open` weak stub, so the
// HOST arm of the map iterator (and of `cel_map_merge_at`) can be
// exercised without the wasmtime trampoline.  Writes the same
// `MapIterState` shape the real trampoline does — `{kind=HOST(1),
// cursor=0, payload=snapshot_offset, count}` at `state_offset`, with a
// snapshot of `count` 48-byte {key, value} CelValue pairs (see
// eval/internal/cel_host.cc `WriteEmptyMapIterState`).  `count == 0`
// (the default) reproduces the weak stub exactly, which is what
// `HostMapInitReturnsZeroHandle` depends on.
static uint32_t g_host_iter_entry_count = 0;
static int64_t g_host_iter_first_key = 0;
void cel_host_cel_map_iter_open(uint32_t state_offset, uint32_t /*map_slot*/) {
  auto* state = reinterpret_cast<uint32_t*>(cel_mem_base() + state_offset);
  state[0] = 1;  // MAP_ITER_KIND_HOST
  state[1] = 0;  // cursor
  state[2] = 0;  // payload (snapshot offset)
  state[3] = g_host_iter_entry_count;
  if (g_host_iter_entry_count == 0) return;
  const uint32_t snapshot = arena_alloc(
      g_host_iter_entry_count * 2u * static_cast<uint32_t>(sizeof(CelValue)));
  state[2] = snapshot;
  for (uint32_t i = 0; i < g_host_iter_entry_count; ++i) {
    const int64_t k = g_host_iter_first_key + i;
    CelValue* key = cel_value_at(
        snapshot + (i * 2u * static_cast<uint32_t>(sizeof(CelValue))));
    CelValue* val = cel_value_at(
        snapshot + (((i * 2u) + 1u) * static_cast<uint32_t>(sizeof(CelValue))));
    key->kind = CEL_INT;
    key->payload.i = k;
    val->kind = CEL_INT;
    val->payload.i = k * 10;
  }
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
    g_host_iter_entry_count = 0;
    g_host_iter_first_key = 0;
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

// ≥2^53 double lookup keys go through the INDEX like every other key.
// The hash folds a double onto the integer it exactly represents and
// `cel_map_key_eq` accepts exactly that conversion, so hash and
// comparator agree at every magnitude — there is no bypass.  (An
// earlier `key_forces_linear` predicate routed these keys to a linear
// scan; it existed only because the comparator was then the lossy `==`
// rule, under which one double matched a RANGE of ints that no single
// hash token could reach.)  This asserts indexed == linear parity for
// exactly those keys.
TEST_F(MapIndexTest, LargeDoubleKeyResolvesThroughTheIndex) {
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
  // A double exactly equal to 2^53 (representable) — hits entry 0.
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
// (cel_map_key_eq treats int 1 and uint 1 as equal map keys); cel-cpp
// accepts `{1:'a', 1u:'b'}` as two distinct keys — a separate
// int-vs-uint key-identity gap, not the lossless-conversion rule.
TEST_F(MapIndexTest, BuildPoisonsCrossKindDuplicate) {
  // Need ≥ threshold entries so the build runs; pad with distinct keys.
  constexpr uint32_t kN = 9;
  uint32_t m = NewSlot();
  cel_map_create(m, kN);
  cel_map_insert(m, cel_make_int(1), cel_make_int(1));
  // The dup: uint 1 compares equal to int 1 under cel_map_key_eq, but
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
// cel_map_key_eq-equal keys straight into the entries run (via
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

// ══════════════════════════════════════════════════════════════════
// Lossless map-key lookup (CELW-0004).
//
// Map-key matching is EXACT, not the lossy `==` rule: cel-cpp converts
// the query key to the stored key's type only when the conversion
// round-trips (`internal/number.h` `LosslessConvertibleToIntVisitor` /
// `LosslessConvertibleToUintVisitor`, driven from
// `eval/eval/container_access_step.cc::LookupInMap` and
// `runtime/standard/container_membership_functions.cc`'s
// `doubleKeyInSet`).  Above 2^53 one double is the rounded image of a
// RANGE of int64s, so a rounded double must MISS a neighbouring stored
// int even though `==` calls them equal.
//
// Oracle-pinned in `testdata/cel_cpp_oracle_test.cc`
// (`MapKeyNumericCrossType`):
//   `dyn(9007199254740993) == 9007199254740992.0`        -> true
//   `dyn(9007199254740992.0) in {9007199254740993: 'a'}` -> false
//
// Every case runs through BOTH the linear scan and the SwissTable index
// (the map is padded past `kCelMapIndexThreshold`), because the index
// and the comparator must agree exactly — a hash that folded a rounded
// double onto a different token than the comparator accepts is a silent
// false miss.
// ══════════════════════════════════════════════════════════════════

constexpr int64_t kTwo53 = 9007199254740992LL;  // 2^53
constexpr double kTwo53D = 9007199254740992.0;
constexpr double kTwo53Plus2D = 9007199254740994.0;

struct LosslessKeyCase {
  const char* name;
  // Stored key (int or uint), built into a padded map.
  uint32_t (*store)();
  // Double lookup key.
  double query;
  bool expect_hit;
};

class MapLosslessKeyTest
    : public MapTest,
      public ::testing::WithParamInterface<LosslessKeyCase> {
 public:
  // A map holding `key` plus `kPad` filler entries, so `count` clears
  // `kCelMapIndexThreshold` and `cel_map_index_build` really allocates.
  // Filler keys are small strings — they can never collide with a
  // numeric key under any rule.
  static constexpr uint32_t kPad = 12;
  uint32_t MakePaddedMap(uint32_t key_slot, bool build_index) {
    uint32_t m = NewSlot();
    cel_map_create(m, kPad + 1);
    cel_map_insert(m, key_slot, cel_make_int(777));
    for (uint32_t i = 0; i < kPad; ++i) {
      const std::string f = "pad" + std::to_string(i);
      cel_map_insert(m,
                     cel_make_string(f.data(), static_cast<uint32_t>(f.size())),
                     cel_make_int(static_cast<int64_t>(i)));
    }
    if (build_index) cel_map_index_build(m);
    return m;
  }
};

TEST_P(MapLosslessKeyTest, LookupMatchesLosslessRuleOnBothPaths) {
  const LosslessKeyCase& c = GetParam();
  for (const bool indexed : {false, true}) {
    uint32_t m = MakePaddedMap(c.store(), indexed);
    if (indexed) {
      const auto* hdr = reinterpret_cast<const ArenaMapHeader*>(
          cel_mem_base() + cel_value_at(m)->payload.arena_map.header_ptr);
      ASSERT_NE(hdr->index_offset, 0u)
          << c.name << ": padded map should carry an index";
    }
    uint32_t out = NewSlot();
    cel_map_lookup_arena(out, m, cel_make_double(c.query));
    if (c.expect_hit) {
      ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT))
          << c.name << " indexed=" << indexed;
      EXPECT_EQ(cel_value_at(out)->payload.i, 777)
          << c.name << " indexed=" << indexed;
    } else {
      ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR))
          << c.name << " indexed=" << indexed;
      EXPECT_EQ(cel_value_at(out)->payload.err,
                static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY))
          << c.name << " indexed=" << indexed;
    }
    // `k in m` must agree with `m[k]` on both paths.
    uint32_t in_out = NewSlot();
    cel_map_in_arena(in_out, cel_make_double(c.query), m);
    ASSERT_EQ(cel_value_at(in_out)->kind, static_cast<uint32_t>(CEL_BOOL))
        << c.name;
    EXPECT_EQ(cel_value_at(in_out)->payload.b != 0, c.expect_hit)
        << "in " << c.name << " indexed=" << indexed;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Int53Boundary, MapLosslessKeyTest,
    ::testing::Values(
        LosslessKeyCase{"stored_2p53_minus_1_query_exact",
                        +[]() {
                          return cel_make_int(kTwo53 - 1);
                        },
                        9007199254740991.0, true},
        LosslessKeyCase{"stored_2p53_query_exact",
                        +[]() {
                          return cel_make_int(kTwo53);
                        },
                        kTwo53D, true},
        // CELW-0004: 2^53+1 has no exact double; the rounded 2^53.0 must
        // NOT find it (though `==` says they are equal).
        LosslessKeyCase{"stored_2p53_plus_1_query_rounded_misses",
                        +[]() {
                          return cel_make_int(kTwo53 + 1);
                        },
                        kTwo53D, false},
        LosslessKeyCase{"stored_2p53_plus_1_query_upper_neighbour_misses",
                        +[]() {
                          return cel_make_int(kTwo53 + 1);
                        },
                        kTwo53Plus2D, false},
        LosslessKeyCase{"stored_2p53_plus_2_query_exact",
                        +[]() {
                          return cel_make_int(kTwo53 + 2);
                        },
                        kTwo53Plus2D, true},
        LosslessKeyCase{"stored_2p54_plus_1_query_rounded_misses",
                        +[]() {
                          return cel_make_int(18014398509481985LL);
                        },
                        18014398509481984.0, false},
        // Negative equivalents.
        LosslessKeyCase{"stored_neg_2p53_query_exact",
                        +[]() {
                          return cel_make_int(-kTwo53);
                        },
                        -kTwo53D, true},
        LosslessKeyCase{"stored_neg_2p53_minus_1_query_rounded_misses",
                        +[]() {
                          return cel_make_int(-(kTwo53 + 1));
                        },
                        -kTwo53D, false},
        LosslessKeyCase{"stored_int64_max_query_rounded_misses",
                        +[]() {
                          return cel_make_int(INT64_MAX);
                        },
                        9223372036854775808.0, false},
        LosslessKeyCase{"stored_int64_min_query_exact",
                        +[]() {
                          return cel_make_int(INT64_MIN);
                        },
                        -9223372036854775808.0, true}),
    [](const ::testing::TestParamInfo<LosslessKeyCase>& i) {
      return std::string(i.param.name);
    });

INSTANTIATE_TEST_SUITE_P(
    UintBoundary, MapLosslessKeyTest,
    ::testing::Values(
        LosslessKeyCase{"stored_uint_2p53_query_exact",
                        +[]() {
                          return cel_make_uint(9007199254740992ULL);
                        },
                        kTwo53D, true},
        LosslessKeyCase{"stored_uint_2p53_plus_1_query_rounded_misses",
                        +[]() {
                          return cel_make_uint(9007199254740993ULL);
                        },
                        kTwo53D, false},
        LosslessKeyCase{"stored_uint_2p63_query_exact",
                        +[]() {
                          return cel_make_uint(9223372036854775808ULL);
                        },
                        9223372036854775808.0, true},
        LosslessKeyCase{"stored_uint_2p63_plus_1_query_rounded_misses",
                        +[]() {
                          return cel_make_uint(9223372036854775809ULL);
                        },
                        9223372036854775808.0, false},
        // UINT64_MAX rounds to 2^64, which is out of uint64 range.
        LosslessKeyCase{"stored_uint64_max_query_rounded_misses",
                        +[]() {
                          return cel_make_uint(UINT64_MAX);
                        },
                        18446744073709551616.0, false},
        LosslessKeyCase{"stored_uint_max_representable_double_hits",
                        +[]() {
                          return cel_make_uint(18446744073709549568ULL);
                        },
                        18446744073709549568.0, true},
        LosslessKeyCase{"stored_uint_zero_query_negative_misses",
                        +[]() {
                          return cel_make_uint(0);
                        },
                        -1.0, false}),
    [](const ::testing::TestParamInfo<LosslessKeyCase>& i) {
      return std::string(i.param.name);
    });

INSTANTIATE_TEST_SUITE_P(
    NonFiniteQuery, MapLosslessKeyTest,
    ::testing::Values(
        LosslessKeyCase{"nan_query_misses",
                        +[]() {
                          return cel_make_int(0);
                        },
                        std::numeric_limits<double>::quiet_NaN(), false},
        LosslessKeyCase{"pos_inf_query_misses",
                        +[]() {
                          return cel_make_int(INT64_MAX);
                        },
                        std::numeric_limits<double>::infinity(), false},
        LosslessKeyCase{"neg_inf_query_misses",
                        +[]() {
                          return cel_make_int(INT64_MIN);
                        },
                        -std::numeric_limits<double>::infinity(), false},
        LosslessKeyCase{"non_integral_query_misses",
                        +[]() {
                          return cel_make_int(1);
                        },
                        1.5, false},
        LosslessKeyCase{"integral_query_below_2p53_hits",
                        +[]() {
                          return cel_make_int(3);
                        },
                        3.0, true}),
    [](const ::testing::TestParamInfo<LosslessKeyCase>& i) {
      return std::string(i.param.name);
    });

// The KEY half of map equality uses the same lossless rule (cel-cpp
// `equality_functions.cc::CheckAlternativeNumericType`).  Our stored
// keys are never doubles, so the reachable divergence here is int-vs-
// uint — which both rules compare exactly.  This pins that map equality
// keeps matching int and uint keys of the same mathematical value, and
// stops matching ones that only agree after rounding.
TEST_F(MapTest, MapEqualityMatchesIntAndUintKeysOfSameValue) {
  uint32_t a = NewSlot();
  uint32_t b = NewSlot();
  cel_map_create(a, 1);
  cel_map_create(b, 1);
  cel_map_insert(a, cel_make_int(kTwo53 + 1), cel_make_int(5));
  cel_map_insert(b, cel_make_uint(9007199254740993ULL), cel_make_int(5));
  uint32_t out = NewSlot();
  cel_map_eq_arena(out, a, b);
  ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(cel_value_at(out)->payload.b, 1);
}

TEST_F(MapTest, MapEqualityRejectsKeysThatOnlyAgreeAfterRounding) {
  uint32_t a = NewSlot();
  uint32_t b = NewSlot();
  cel_map_create(a, 1);
  cel_map_create(b, 1);
  cel_map_insert(a, cel_make_int(kTwo53 + 1), cel_make_int(5));
  cel_map_insert(b, cel_make_uint(9007199254740992ULL), cel_make_int(5));
  uint32_t out = NewSlot();
  cel_map_eq_arena(out, a, b);
  ASSERT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(cel_value_at(out)->payload.b, 0);
}

// ════════ cel_map_merge_at / cel_map_merge_at_if_bool ════════
//
// The general `transformMapEntry` loop step: merge every entry of a
// computed entry map into the accumulator.  Distinguishing properties
// against `cel_map_insert_at` (which these delegate to per entry):
//   - the entry VALUE gets its own 3VL arm (error / unknown / non-map);
//   - the accumulator GROWS instead of trapping, because a computed
//     entry expression has no compile-time key count for codegen to
//     pre-size against.
// Regression cover for CELW-0012 (`{1: 2, 3: 4}.transformMapEntry(k,
// v, k == 1 ? {k: v} : {})` used to ABORT the compiler).

class MapMergeTest : public MapTest {
 public:
  // A fresh CEL_MAP_ARENA accumulator with `capacity` slots.
  uint32_t Accu(uint32_t capacity) {
    uint32_t m = NewSlot();
    cel_map_create(m, capacity);
    return m;
  }
  // A source map of `n` int→int entries starting at `first`.
  uint32_t IntMap(int64_t first, uint32_t n) {
    uint32_t m = NewSlot();
    cel_map_create(m, n);
    for (uint32_t i = 0; i < n; ++i) {
      cel_map_insert(m, cel_make_int(first + i),
                     cel_make_int((first + i) * 10));
    }
    return m;
  }
  int64_t LookupInt(uint32_t map_slot, int64_t key) {
    uint32_t out = NewSlot();
    cel_map_lookup_arena(out, map_slot, cel_make_int(key));
    EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT))
        << "key " << key << " missing";
    return cel_value_at(out)->payload.i;
  }
  static uint32_t MapCount(uint32_t map_slot) {
    auto* hdr = reinterpret_cast<ArenaMapHeader*>(
        cel_mem_base() + cel_value_at(map_slot)->payload.arena_map.header_ptr);
    return hdr->count;
  }
};

TEST_F(MapMergeTest, EmptyEntryMapLeavesAccumulatorUntouched) {
  uint32_t accu = Accu(4);
  cel_map_insert_at(accu, cel_make_int(1), cel_make_int(10));
  uint32_t entry = Accu(0);
  cel_map_merge_at(accu, entry);
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 1u);
  EXPECT_EQ(LookupInt(accu, 1), 10);
}

TEST_F(MapMergeTest, SingleEntryMerges) {
  uint32_t accu = Accu(4);
  cel_map_merge_at(accu, IntMap(/*first=*/7, /*n=*/1));
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 1u);
  EXPECT_EQ(LookupInt(accu, 7), 70);
}

TEST_F(MapMergeTest, MultiEntryMergesEveryPair) {
  uint32_t accu = Accu(8);
  cel_map_merge_at(accu, IntMap(/*first=*/1, /*n=*/3));
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 3u);
  EXPECT_EQ(LookupInt(accu, 1), 10);
  EXPECT_EQ(LookupInt(accu, 2), 20);
  EXPECT_EQ(LookupInt(accu, 3), 30);
}

// Last-write-wins on collision — same rule as cel_map_insert_at, NOT
// cel_map_insert's duplicate-key poison (CELW-0011 tracks the
// divergence from cel-cpp for the accumulator path as a whole).
TEST_F(MapMergeTest, CollidingKeyOverwritesValue) {
  uint32_t accu = Accu(4);
  cel_map_insert_at(accu, cel_make_int(1), cel_make_int(999));
  cel_map_merge_at(accu, IntMap(/*first=*/1, /*n=*/1));
  EXPECT_EQ(MapCount(accu), 1u);
  EXPECT_EQ(LookupInt(accu, 1), 10);
}

// The load-bearing difference from cel_map_insert_at: codegen
// pre-sizes a computed-entry accumulator at ONE entry per iteration,
// so a two-key entry map overruns it.  Growing is correct here;
// trapping (the pre-size invariant) would be a crash on ordinary
// input.
TEST_F(MapMergeTest, GrowsPastPresizedCapacity) {
  uint32_t accu = Accu(1);
  cel_map_merge_at(accu, IntMap(/*first=*/1, /*n=*/5));
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 5u);
  for (int64_t k = 1; k <= 5; ++k) {
    EXPECT_EQ(LookupInt(accu, k), k * 10);
  }
}

// Growth out of a ZERO-capacity accumulator (the pre-size a comprehension
// over an empty range produces) must allocate a run rather than write
// through the null entries_offset.
TEST_F(MapMergeTest, GrowsFromZeroCapacity) {
  uint32_t accu = Accu(0);
  cel_map_merge_at(accu, IntMap(/*first=*/1, /*n=*/2));
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 2u);
  EXPECT_EQ(LookupInt(accu, 1), 10);
  EXPECT_EQ(LookupInt(accu, 2), 20);
}

// Growth drops any hash index (entries moved), so a lookup after a
// growing merge must still find every key — through the linear scan
// until codegen's terminal cel_map_index_build re-indexes.
TEST_F(MapMergeTest, LookupsSurviveGrowthAfterIndexBuild) {
  uint32_t accu = Accu(2);
  cel_map_merge_at(accu, IntMap(/*first=*/1, /*n=*/2));
  cel_map_index_build(accu);
  cel_map_merge_at(accu, IntMap(/*first=*/3, /*n=*/40));
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 42u);
  cel_map_index_build(accu);
  for (int64_t k = 1; k <= 42; ++k) {
    EXPECT_EQ(LookupInt(accu, k), k * 10);
  }
}

// ── 3VL on the entry value ──

TEST_F(MapMergeTest, ErrorEntryPropagatesVerbatim) {
  uint32_t accu = Accu(4);
  uint32_t entry = NewSlot();
  cel_value_at(entry)->kind = CEL_ERROR;
  cel_value_at(entry)->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  cel_map_merge_at(accu, entry);
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(accu)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(MapMergeTest, UnknownEntryPropagatesVerbatim) {
  uint32_t accu = Accu(4);
  uint32_t entry = NewSlot();
  cel_value_at(entry)->kind = CEL_UNKNOWN;
  cel_value_at(entry)->payload.unk = 0x1234;
  cel_map_merge_at(accu, entry);
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(cel_value_at(accu)->payload.unk, 0x1234u);
}

TEST_F(MapMergeTest, NonMapEntryPoisonsTypeMismatch) {
  uint32_t accu = Accu(4);
  cel_map_merge_at(accu, cel_make_int(5));
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(accu)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// A poisoned accumulator stays poisoned — an earlier iteration's error
// must reach `result` rather than being overwritten by a later merge.
TEST_F(MapMergeTest, PoisonedAccumulatorIsStickyNoOp) {
  uint32_t accu = NewSlot();
  cel_value_at(accu)->kind = CEL_ERROR;
  cel_value_at(accu)->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  cel_map_merge_at(accu, IntMap(/*first=*/1, /*n=*/2));
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(accu)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

// An invalid key kind inside the entry map poisons the accumulator and
// stops the merge — entries after the bad one must not land.
TEST_F(MapMergeTest, InvalidKeyKindInEntryPoisonsAndStops) {
  uint32_t accu = Accu(8);
  uint32_t entry = Accu(4);
  cel_map_insert_at(entry, cel_make_int(1), cel_make_int(10));
  // Write a double key straight into the entry run — cel_map_insert
  // would have rejected it, but a host-built map can carry one.
  auto* hdr = reinterpret_cast<ArenaMapHeader*>(
      cel_mem_base() + cel_value_at(entry)->payload.arena_map.header_ptr);
  const size_t off = static_cast<size_t>(hdr->entries_offset) +
                     (static_cast<size_t>(kCelMapEntryStride) * hdr->count);
  auto* bad_key = reinterpret_cast<CelValue*>(cel_mem_base() + off);
  auto* bad_val =
      reinterpret_cast<CelValue*>(cel_mem_base() + off + sizeof(CelValue));
  bad_key->kind = CEL_DOUBLE;
  bad_key->payload.d = 1.5;
  bad_val->kind = CEL_INT;
  bad_val->payload.i = 20;
  hdr->count = 2;
  cel_map_merge_at(accu, entry);
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(accu)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// A HOST-represented entry map (an Activation-bound map reached as the
// entry expression) merges through the iterator arm rather than the
// direct entries walk.
TEST_F(MapMergeTest, HostEntryMapMergesThroughIterator) {
  g_host_iter_entry_count = 3;
  g_host_iter_first_key = 4;
  uint32_t accu = Accu(1);  // pre-sized for one; the merge grows it.
  uint32_t entry = NewSlot();
  cel_value_at(entry)->kind = CEL_MAP_HOST;
  cel_value_at(entry)->payload.ref_slot = 7;
  cel_map_merge_at(accu, entry);
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 3u);
  EXPECT_EQ(LookupInt(accu, 4), 40);
  EXPECT_EQ(LookupInt(accu, 5), 50);
  EXPECT_EQ(LookupInt(accu, 6), 60);
}

TEST_F(MapMergeTest, EmptyHostEntryMapIsNoOp) {
  uint32_t accu = Accu(2);
  cel_map_insert_at(accu, cel_make_int(1), cel_make_int(10));
  uint32_t entry = NewSlot();
  cel_value_at(entry)->kind = CEL_MAP_HOST;
  cel_value_at(entry)->payload.ref_slot = 7;
  cel_map_merge_at(accu, entry);
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 1u);
}

// ── predicate-gated merge ──

TEST_F(MapMergeTest, IfBoolTrueMerges) {
  uint32_t accu = Accu(4);
  cel_map_merge_at_if_bool(accu, cel_make_bool(1), IntMap(1, 2));
  EXPECT_EQ(MapCount(accu), 2u);
}

TEST_F(MapMergeTest, IfBoolFalseIsNoOp) {
  uint32_t accu = Accu(4);
  cel_map_merge_at_if_bool(accu, cel_make_bool(0), IntMap(1, 2));
  ASSERT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapCount(accu), 0u);
}

TEST_F(MapMergeTest, IfBoolErrorPredicatePropagates) {
  uint32_t accu = Accu(4);
  uint32_t pred = NewSlot();
  cel_value_at(pred)->kind = CEL_ERROR;
  cel_value_at(pred)->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  cel_map_merge_at_if_bool(accu, pred, IntMap(1, 2));
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(accu)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(MapMergeTest, IfBoolUnknownPredicatePropagates) {
  uint32_t accu = Accu(4);
  uint32_t pred = NewSlot();
  cel_value_at(pred)->kind = CEL_UNKNOWN;
  cel_value_at(pred)->payload.unk = 0x99;
  cel_map_merge_at_if_bool(accu, pred, IntMap(1, 2));
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(cel_value_at(accu)->payload.unk, 0x99u);
}

TEST_F(MapMergeTest, IfBoolNonBoolPredicatePoisonsTypeMismatch) {
  uint32_t accu = Accu(4);
  cel_map_merge_at_if_bool(accu, cel_make_int(1), IntMap(1, 2));
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(accu)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(MapMergeTest, IfBoolPoisonedAccumulatorIsStickyNoOp) {
  uint32_t accu = NewSlot();
  cel_value_at(accu)->kind = CEL_ERROR;
  cel_value_at(accu)->payload.err = CEL_ERR_NO_SUCH_KEY;
  cel_map_merge_at_if_bool(accu, cel_make_bool(1), IntMap(1, 2));
  EXPECT_EQ(cel_value_at(accu)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(accu)->payload.err,
            static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

}  // namespace
}  // namespace celwasm
