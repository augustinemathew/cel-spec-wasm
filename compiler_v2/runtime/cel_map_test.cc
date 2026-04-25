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

#include "compiler_v2/runtime/cel_map.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

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
 public:  // public so parameterized lambdas can use the helpers.
  void SetUp() override {
    cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
    g_host_lookup_calls = 0;
  }
  uint32_t NewSlot() {
    return cel_alloc(static_cast<uint32_t>(sizeof(CelValue)));
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

INSTANTIATE_TEST_SUITE_P(
    AllKeyKinds, MapKeyRoundTripTest,
    ::testing::Values(
        RoundTripCase{
            "bool_true",
            +[](MapTest&) { return cel_make_bool(1); },
            111,
        },
        RoundTripCase{
            "bool_false",
            +[](MapTest&) { return cel_make_bool(0); },
            222,
        },
        RoundTripCase{
            "int_zero",
            +[](MapTest&) { return cel_make_int(0); },
            10,
        },
        RoundTripCase{
            "int_negative",
            +[](MapTest&) { return cel_make_int(-1); },
            20,
        },
        RoundTripCase{
            "int_min",
            +[](MapTest&) { return cel_make_int(INT64_MIN); },
            30,
        },
        RoundTripCase{
            "int_max",
            +[](MapTest&) { return cel_make_int(INT64_MAX); },
            40,
        },
        RoundTripCase{
            "uint_zero",
            +[](MapTest&) { return cel_make_uint(0); },
            50,
        },
        RoundTripCase{
            "uint_max",
            +[](MapTest&) { return cel_make_uint(UINT64_MAX); },
            60,
        },
        RoundTripCase{
            "string_empty",
            +[](MapTest& t) { return t.Str(""); },
            70,
        },
        RoundTripCase{
            "string_simple",
            +[](MapTest& t) { return t.Str("hello"); },
            80,
        },
        RoundTripCase{
            "string_multibyte_utf8",
            +[](MapTest& t) { return t.Str("héllo"); },
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
class MapCrossTypeTest
    : public MapTest,
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
            +[]() { return cel_make_int(42); },
            +[]() { return cel_make_uint(42); },
            true,
            420,
        },
        CrossTypeCase{
            "uint_stored_int_lookup_hits",
            +[]() { return cel_make_uint(7); },
            +[]() { return cel_make_int(7); },
            true,
            700,
        },
        // Negative int never equals any uint.
        CrossTypeCase{
            "negative_int_never_matches_uint",
            +[]() { return cel_make_uint(0); },
            +[]() { return cel_make_int(-1); },
            false,
            0,
        },
        // uint 2^63 has no representable int — int(INT64_MAX) misses.
        CrossTypeCase{
            "uint_above_int_max_never_matches_int",
            +[]() { return cel_make_uint(uint64_t{1} << 63); },
            +[]() { return cel_make_int(INT64_MAX); },
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
    ::testing::Values(static_cast<uint32_t>(CEL_NULL),
                      static_cast<uint32_t>(CEL_DOUBLE),
                      static_cast<uint32_t>(CEL_BYTES),
                      static_cast<uint32_t>(CEL_LIST_ARENA),
                      static_cast<uint32_t>(CEL_LIST_HOST),
                      static_cast<uint32_t>(CEL_MAP_ARENA),
                      static_cast<uint32_t>(CEL_MAP_HOST),
                      static_cast<uint32_t>(CEL_MESSAGE),
                      static_cast<uint32_t>(CEL_TYPE),
                      static_cast<uint32_t>(CEL_DURATION),
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
    ::testing::Values(
        OperandPropagationCase{"unknown_propagates", CEL_UNKNOWN, 42,
                               CEL_UNKNOWN},
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

}  // namespace
}  // namespace celwasm
