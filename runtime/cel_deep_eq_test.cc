// Deep (non-scalar element) equality through the kernel's arena fast
// paths — `cel_list_eq_arena` / `cel_map_eq_arena` / `cel_list_in_arena`
// and the `cel_equals_at_vv` dispatcher above them.
//
// The bug class under test (cleanup-backlog #40 latent-gap note):
// the arena+arena walks compared elements with the SCALAR-only
// matcher `cel_value_eq`, which returns 0 for CEL_MESSAGE and nested
// aggregates — so `[Msg{x:1}] == [Msg{x:1}]`, `[[1]] == [[1]]`,
// `[{1:2}] == [{1:2}]` and `Msg in [Msg]` all silently evaluated
// `false`.  The fix routes non-scalar element pairs through the
// polymorphic equality kernel (`deep_values_equal`), which recurses
// into the aggregate dispatchers and the `cel_host.cel_message_eq`
// trampoline.
//
// Coverage matrix:
//   - arena+arena lists of messages: equal / unequal / length
//     mismatch / empty×nonempty
//   - arena+arena maps with message values: equal / unequal
//   - nested aggregate-in-aggregate: list-in-list, map-in-list,
//     list-in-map, two levels deep
//   - scalar arena+arena control: fast path intact AND no host trip
//   - equivalence: list-of-message verdict == direct `msg == msg`
//     verdict on the same backings (both route through the same
//     `cel_host.cel_message_eq` surface)
//   - wire contract: the kernel hands the trampoline the ELEMENT
//     cells' byte offsets (CEL_MESSAGE CelValues), not the list slots
//   - `in` with a message needle: found / not found
//   - nested CEL_LIST_HOST / CEL_MAP_HOST elements route through the
//     host `cel_list_eq` / `cel_map_eq` trampolines
//   - message vs scalar element kinds compare unequal (cross-kind
//     `false` per langdef §"Equality")
//   - not-comparable message pair (trampoline reports TYPE_MISMATCH)
//     compares UNEQUAL inside a list walk, matching the host-side
//     walk's contract (eval/internal/cel_host.cc ListEqElementEquals)
//   - CEL_TYPE elements ([int] == [int]) compare by name bytes
//
// The host trampolines are strong overrides of the weak host-build
// stubs in cel_runtime.c: message equality consults a test-local
// msg_slot → int table (semantic message equality is owned by
// CelMessageEqImpl, pinned in eval/internal/ and e2e — here we pin
// ROUTING and verdict plumbing).

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>

#include "gtest/gtest.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_list.h"
#include "runtime/cel_make.h"
#include "runtime/cel_map.h"
#include "runtime/cel_memory.h"

extern "C" {
// Wasm-exported by cel_runtime.c but intentionally not in an umbrella
// header (host code reaches equality via specific kernels).
void cel_equals_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
}

namespace {

// ── Scripted host trampolines ──────────────────────────────────────

// msg_slot → fake message payload.  Two slots are "equal messages"
// iff their mapped ints match; a slot absent from the table is
// not-comparable (mirrors a non-proto backing) and the override
// reports TYPE_MISMATCH like CelMessageEqImpl does.
std::map<uint32_t, int>& MsgTable() {
  static auto* t = new std::map<uint32_t, int>();
  return *t;
}
int g_message_eq_calls = 0;
uint32_t g_message_eq_last_a = 0;
uint32_t g_message_eq_last_b = 0;

int g_host_list_eq_calls = 0;
int g_host_map_eq_calls = 0;

}  // namespace

extern "C" {
// Strong overrides of the weak stubs declared in cel_runtime.c.
void cel_host_cel_message_eq(uint32_t out_slot, uint32_t a_slot,
                             uint32_t b_slot) {
  ++g_message_eq_calls;
  g_message_eq_last_a = a_slot;
  g_message_eq_last_b = b_slot;
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  auto ia = MsgTable().find(a->payload.msg_slot);
  auto ib = MsgTable().find(b->payload.msg_slot);
  if (ia == MsgTable().end() || ib == MsgTable().end()) {
    out->kind = CEL_ERROR;  // not-comparable, like CelMessageEqImpl
    out->payload.err = CEL_ERR_TYPE_MISMATCH;
    return;
  }
  out->kind = CEL_BOOL;
  out->payload.b = ia->second == ib->second ? 1 : 0;
}
// Host-origin aggregate equality: equal iff the ref_slots match.
// Pins that nested host-origin elements reach the trampoline (the
// real CelListEqImpl/CelMapEqImpl semantics are pinned in
// eval/internal/cel_list_eq_impl_test.cc).
void cel_host_cel_list_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  ++g_host_list_eq_calls;
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_BOOL;
  out->payload.b = cel_value_at(a_slot)->payload.ref_slot ==
                           cel_value_at(b_slot)->payload.ref_slot
                       ? 1
                       : 0;
}
void cel_host_cel_map_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  ++g_host_map_eq_calls;
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_BOOL;
  out->payload.b = cel_value_at(a_slot)->payload.ref_slot ==
                           cel_value_at(b_slot)->payload.ref_slot
                       ? 1
                       : 0;
}
}

namespace celwasm {
namespace {

class DeepEqTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
    MsgTable().clear();
    g_message_eq_calls = 0;
    g_message_eq_last_a = 0;
    g_message_eq_last_b = 0;
    g_host_list_eq_calls = 0;
    g_host_map_eq_calls = 0;
  }

  uint32_t NewSlot() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  // A CEL_MESSAGE CelValue slot whose fake backing holds `payload`.
  uint32_t MakeMessage(uint32_t msg_slot, int payload) {
    MsgTable()[msg_slot] = payload;
    uint32_t s = NewSlot();
    cel_value_at(s)->kind = CEL_MESSAGE;
    cel_value_at(s)->payload.msg_slot = msg_slot;
    return s;
  }

  // A CEL_MESSAGE slot whose msg_slot is NOT in the table — the
  // trampoline reports it not-comparable.
  uint32_t MakeUncomparableMessage(uint32_t msg_slot) {
    uint32_t s = NewSlot();
    cel_value_at(s)->kind = CEL_MESSAGE;
    cel_value_at(s)->payload.msg_slot = msg_slot;
    return s;
  }

  uint32_t MakeHostList(uint32_t ref_slot) {
    uint32_t s = NewSlot();
    cel_value_at(s)->kind = CEL_LIST_HOST;
    cel_value_at(s)->payload.ref_slot = ref_slot;
    return s;
  }

  uint32_t MakeHostMap(uint32_t ref_slot) {
    uint32_t s = NewSlot();
    cel_value_at(s)->kind = CEL_MAP_HOST;
    cel_value_at(s)->payload.ref_slot = ref_slot;
    return s;
  }

  // CEL_TYPE value over the given name bytes (copied into the arena).
  uint32_t MakeType(const std::string& name) {
    uint32_t bytes = arena_alloc(static_cast<uint32_t>(name.size()));
    std::memcpy(cel_mem_base() + bytes, name.data(), name.size());
    uint32_t s = NewSlot();
    cel_value_at(s)->kind = CEL_TYPE;
    cel_value_at(s)->payload.s.ptr = bytes;
    cel_value_at(s)->payload.s.len = static_cast<uint32_t>(name.size());
    return s;
  }

  // Arena list from already-built element slots.
  uint32_t MakeList(std::initializer_list<uint32_t> elem_slots) {
    uint32_t l = NewSlot();
    cel_list_create(l, static_cast<uint32_t>(elem_slots.size()));
    for (uint32_t e : elem_slots) {
      cel_list_append_at(l, e);
    }
    return l;
  }

  uint32_t MakeIntList(std::initializer_list<int64_t> elems) {
    uint32_t l = NewSlot();
    cel_list_create(l, static_cast<uint32_t>(elems.size()));
    for (int64_t v : elems) {
      cel_list_append_at(l, cel_make_int(v));
    }
    return l;
  }

  // Arena map {int key → value slot}.
  uint32_t MakeMap(
      std::initializer_list<std::pair<int64_t, uint32_t>> entries) {
    uint32_t m = NewSlot();
    cel_map_create(m, static_cast<uint32_t>(entries.size()));
    for (const auto& [k, vslot] : entries) {
      cel_map_insert(m, cel_make_int(k), vslot);
    }
    return m;
  }

  // Runs the polymorphic dispatcher (what `==` lowers to) and reads
  // the bool verdict.
  bool Equals(uint32_t a, uint32_t b) {
    uint32_t out = NewSlot();
    cel_equals_at_vv(out, a, b);
    EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_BOOL));
    return cel_value_at(out)->payload.b != 0;
  }

  bool ListIn(uint32_t value_slot, uint32_t list_slot) {
    uint32_t out = NewSlot();
    cel_list_in_arena(out, value_slot, list_slot);
    EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_BOOL));
    return cel_value_at(out)->payload.b != 0;
  }
};

// ── arena+arena lists of messages ──────────────────────────────────

TEST_F(DeepEqTest, MessageListsEqual) {
  uint32_t a = MakeList({MakeMessage(1, 7), MakeMessage(2, 9)});
  uint32_t b = MakeList({MakeMessage(3, 7), MakeMessage(4, 9)});
  EXPECT_TRUE(Equals(a, b));
  EXPECT_GE(g_message_eq_calls, 2);  // one trampoline trip per pair
}

TEST_F(DeepEqTest, MessageListsUnequalValue) {
  uint32_t a = MakeList({MakeMessage(1, 7)});
  uint32_t b = MakeList({MakeMessage(2, 8)});
  EXPECT_FALSE(Equals(a, b));
}

TEST_F(DeepEqTest, MessageListsLengthMismatch) {
  uint32_t a = MakeList({MakeMessage(1, 7), MakeMessage(2, 7)});
  uint32_t b = MakeList({MakeMessage(3, 7)});
  EXPECT_FALSE(Equals(a, b));
  EXPECT_EQ(g_message_eq_calls, 0);  // length gate fires first
}

TEST_F(DeepEqTest, MessageListEmptyVsNonEmpty) {
  uint32_t a = MakeList({});
  uint32_t b = MakeList({MakeMessage(1, 7)});
  EXPECT_FALSE(Equals(a, b));
}

// ── equivalence with direct msg == msg ─────────────────────────────

TEST_F(DeepEqTest, ListVerdictMatchesDirectMessageEq) {
  uint32_t m1 = MakeMessage(1, 7);
  uint32_t m2 = MakeMessage(2, 7);
  uint32_t m3 = MakeMessage(3, 8);
  // Direct CEL_MESSAGE equality through the dispatcher.
  const bool direct_eq = Equals(m1, m2);
  const bool direct_ne = Equals(m1, m3);
  // The same backings wrapped in single-element arena lists must
  // reach the identical verdicts via the identical trampoline.
  EXPECT_EQ(Equals(MakeList({m1}), MakeList({m2})), direct_eq);
  EXPECT_EQ(Equals(MakeList({m1}), MakeList({m3})), direct_ne);
  EXPECT_TRUE(direct_eq);
  EXPECT_FALSE(direct_ne);
}

// Wire contract: the kernel hands the trampoline the element CELLS'
// byte offsets (each a CEL_MESSAGE CelValue), not the list slots.
TEST_F(DeepEqTest, TrampolineReceivesElementCellOffsets) {
  uint32_t a = MakeList({MakeMessage(1, 7)});
  uint32_t b = MakeList({MakeMessage(2, 7)});
  ASSERT_TRUE(Equals(a, b));
  ASSERT_EQ(g_message_eq_calls, 1);
  EXPECT_NE(g_message_eq_last_a, a);
  EXPECT_NE(g_message_eq_last_b, b);
  EXPECT_EQ(cel_value_at(g_message_eq_last_a)->kind,
            static_cast<uint32_t>(CEL_MESSAGE));
  EXPECT_EQ(cel_value_at(g_message_eq_last_b)->kind,
            static_cast<uint32_t>(CEL_MESSAGE));
  EXPECT_EQ(cel_value_at(g_message_eq_last_a)->payload.msg_slot, 1u);
  EXPECT_EQ(cel_value_at(g_message_eq_last_b)->payload.msg_slot, 2u);
}

// ── arena+arena maps with message values ───────────────────────────

TEST_F(DeepEqTest, MessageValuedMapsEqual) {
  uint32_t a = MakeMap({{1, MakeMessage(1, 7)}, {2, MakeMessage(2, 9)}});
  uint32_t b = MakeMap({{2, MakeMessage(3, 9)}, {1, MakeMessage(4, 7)}});
  EXPECT_TRUE(Equals(a, b));  // set-equality: order irrelevant
}

TEST_F(DeepEqTest, MessageValuedMapsUnequal) {
  uint32_t a = MakeMap({{1, MakeMessage(1, 7)}});
  uint32_t b = MakeMap({{1, MakeMessage(2, 8)}});
  EXPECT_FALSE(Equals(a, b));
}

// ── nested aggregate-in-aggregate ──────────────────────────────────

TEST_F(DeepEqTest, ListOfListEqual) {
  uint32_t a = MakeList({MakeIntList({1, 2}), MakeIntList({3})});
  uint32_t b = MakeList({MakeIntList({1, 2}), MakeIntList({3})});
  EXPECT_TRUE(Equals(a, b));
}

TEST_F(DeepEqTest, ListOfListUnequal) {
  EXPECT_FALSE(Equals(MakeList({MakeIntList({1})}),  //
                      MakeList({MakeIntList({2})})));
}

TEST_F(DeepEqTest, ListOfEmptyListEqual) {
  // Conformance row comparisons/ne_literal/not_ne_list_of_list:
  // `[[]] != [[]]` is false, i.e. `[[]] == [[]]` is true.
  EXPECT_TRUE(Equals(MakeList({MakeIntList({})}),  //
                     MakeList({MakeIntList({})})));
}

TEST_F(DeepEqTest, MapInListEqual) {
  uint32_t a = MakeList({MakeMap({{1, cel_make_int(2)}})});
  uint32_t b = MakeList({MakeMap({{1, cel_make_int(2)}})});
  EXPECT_TRUE(Equals(a, b));
}

TEST_F(DeepEqTest, ListInMapValueEqual) {
  uint32_t a = MakeMap({{1, MakeIntList({1})}});
  uint32_t b = MakeMap({{1, MakeIntList({1})}});
  EXPECT_TRUE(Equals(a, b));
  EXPECT_FALSE(Equals(a, MakeMap({{1, MakeIntList({2})}})));
}

TEST_F(DeepEqTest, TwoLevelsDeepEqual) {
  uint32_t a = MakeList({MakeList({MakeIntList({5})})});
  uint32_t b = MakeList({MakeList({MakeIntList({5})})});
  uint32_t c = MakeList({MakeList({MakeIntList({6})})});
  EXPECT_TRUE(Equals(a, b));
  EXPECT_FALSE(Equals(a, c));
}

// ── scalar control: fast path intact, no host trip ─────────────────

TEST_F(DeepEqTest, ScalarListsStayOnFastPath) {
  EXPECT_TRUE(Equals(MakeIntList({1, 2, 3}), MakeIntList({1, 2, 3})));
  EXPECT_FALSE(Equals(MakeIntList({1, 2, 3}), MakeIntList({1, 2, 4})));
  EXPECT_EQ(g_message_eq_calls, 0);
  EXPECT_EQ(g_host_list_eq_calls, 0);
  EXPECT_EQ(g_host_map_eq_calls, 0);
}

TEST_F(DeepEqTest, ScalarMapsStayOnFastPath) {
  uint32_t a = MakeMap({{1, cel_make_int(10)}, {2, cel_make_int(20)}});
  uint32_t b = MakeMap({{2, cel_make_int(20)}, {1, cel_make_int(10)}});
  EXPECT_TRUE(Equals(a, b));
  EXPECT_EQ(g_message_eq_calls, 0);
}

// ── `in` with non-scalar needles ───────────────────────────────────

TEST_F(DeepEqTest, MessageInListFound) {
  uint32_t needle = MakeMessage(1, 7);
  uint32_t hay = MakeList({MakeMessage(2, 6), MakeMessage(3, 7)});
  EXPECT_TRUE(ListIn(needle, hay));
}

TEST_F(DeepEqTest, MessageInListNotFound) {
  uint32_t needle = MakeMessage(1, 7);
  uint32_t hay = MakeList({MakeMessage(2, 6)});
  EXPECT_FALSE(ListIn(needle, hay));
}

TEST_F(DeepEqTest, ListInListOfListsFound) {
  uint32_t needle = MakeIntList({1, 2});
  uint32_t hay = MakeList({MakeIntList({9}), MakeIntList({1, 2})});
  EXPECT_TRUE(ListIn(needle, hay));
}

// ── host-origin elements nested inside arena aggregates ────────────

TEST_F(DeepEqTest, NestedHostListElementsRouteToHostTrampoline) {
  uint32_t a = MakeList({MakeHostList(42)});
  uint32_t b = MakeList({MakeHostList(42)});
  uint32_t c = MakeList({MakeHostList(43)});
  EXPECT_TRUE(Equals(a, b));
  EXPECT_FALSE(Equals(a, c));
  EXPECT_EQ(g_host_list_eq_calls, 2);
}

TEST_F(DeepEqTest, NestedHostMapElementsRouteToHostTrampoline) {
  uint32_t a = MakeList({MakeHostMap(7)});
  uint32_t b = MakeList({MakeHostMap(7)});
  EXPECT_TRUE(Equals(a, b));
  EXPECT_EQ(g_host_map_eq_calls, 1);
}

// Mixed-origin nested pair (arena list element vs host list element)
// also goes through the host trampoline, not the scalar matcher.
TEST_F(DeepEqTest, MixedOriginNestedListPairRoutesToHostTrampoline) {
  uint32_t a = MakeList({MakeIntList({1})});
  uint32_t b = MakeList({MakeHostList(42)});
  EXPECT_FALSE(Equals(a, b));  // scripted trampoline: ref_slot compare
  EXPECT_EQ(g_host_list_eq_calls, 1);
}

// ── cross-kind and not-comparable elements ─────────────────────────

TEST_F(DeepEqTest, MessageVsScalarElementUnequal) {
  // Cross-kind element pair is `false` per langdef §"Equality", not
  // an error.
  uint32_t a = MakeList({MakeMessage(1, 7)});
  uint32_t b = MakeIntList({7});
  EXPECT_FALSE(Equals(a, b));
}

TEST_F(DeepEqTest, NotComparableMessagePairUnequalNotError) {
  // The trampoline reports TYPE_MISMATCH for a not-comparable pair;
  // inside a list walk that is UNEQUAL, not an eval error — the same
  // contract the host-side walk applies (ListEqElementEquals).
  uint32_t a = MakeList({MakeUncomparableMessage(100)});
  uint32_t b = MakeList({MakeMessage(1, 7)});
  EXPECT_FALSE(Equals(a, b));
}

// ── CEL_TYPE elements ──────────────────────────────────────────────

TEST_F(DeepEqTest, TypeElementsCompareByName) {
  EXPECT_TRUE(Equals(MakeList({MakeType("int")}),  //
                     MakeList({MakeType("int")})));
  EXPECT_FALSE(Equals(MakeList({MakeType("int")}),  //
                      MakeList({MakeType("uint")})));
}

}  // namespace
}  // namespace celwasm
