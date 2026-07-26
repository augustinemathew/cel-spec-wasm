// Unit coverage for the list-bridging kernels in
// `cel_string_ext_list.cc`: split × 2, join × 2.  Spec rows lifted
// from `tests/simple/testdata/string_ext.textproto` per-function
// sections (`split`, `join`).
//
// `join` accepts a list of EITHER origin: an arena list is walked in
// place, a `CEL_LIST_HOST` list (an activation-bound `list<string>`,
// a proto repeated field) is first lifted into the arena by
// `cel_list_arena_view` → `cel_host.cel_list_iter_open`.  The
// host-origin cases below drive that lift through a scripted strong
// override of the host-build stub, mirroring `cel_deep_eq_test.cc`.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_ext.h"
#include "runtime/string_ext_test_helpers.h"

namespace {

// ref_slot → the string elements a host-backed list vends.  The
// scripted `cel_list_iter_open` below materialises these into the
// arena exactly as the production trampoline
// (eval/internal/cel_host.cc `SnapshotHostListToArena`) does.
std::map<uint32_t, std::vector<std::string>>& HostListTable() {
  static auto* t = new std::map<uint32_t, std::vector<std::string>>();
  return *t;
}
int g_iter_open_calls = 0;

}  // namespace

extern "C" {
// Strong override of the weak host-build stub in cel_runtime.c.
void cel_host_cel_list_iter_open(uint32_t out_slot, uint32_t list_slot) {
  ++g_iter_open_calls;
  const CelValue* src = cel_value_at(list_slot);
  const std::vector<std::string>& elements =
      HostListTable()[src->payload.ref_slot];
  const uint32_t hdr_off =
      arena_alloc(static_cast<uint32_t>(sizeof(ArenaListHeader)));
  uint32_t elements_off = 0;
  if (!elements.empty()) {
    elements_off = arena_alloc(static_cast<uint32_t>(
        static_cast<size_t>(kCelListEntryStride) * elements.size()));
  }
  auto* hdr = reinterpret_cast<ArenaListHeader*>(cel_mem_base() + hdr_off);
  hdr->count = static_cast<uint32_t>(elements.size());
  hdr->capacity = hdr->count;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  for (size_t k = 0; k < elements.size(); ++k) {
    auto* elt = reinterpret_cast<CelValue*>(
        cel_mem_base() + elements_off +
        (static_cast<size_t>(kCelListEntryStride) * k));
    uint32_t byte_off = 0;
    const auto byte_len = static_cast<uint32_t>(elements[k].size());
    if (byte_len > 0) {
      byte_off = arena_alloc(byte_len);
      std::memcpy(cel_mem_base() + byte_off, elements[k].data(), byte_len);
    }
    elt->kind = CEL_STRING;
    elt->payload.s.ptr = byte_off;
    elt->payload.s.len = byte_len;
  }
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
}
}

namespace celwasm {
namespace {

// Decode a CEL_LIST_ARENA result into a `std::vector<std::string>` for
// readable matchers.  Element kinds are asserted as CEL_STRING.
std::vector<std::string> ListAsStrings(const CelValue* v) {
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  auto* hdr = reinterpret_cast<const ArenaListHeader*>(
      cel_mem_base() + v->payload.arena_list.header_ptr);
  std::vector<std::string> out;
  out.reserve(hdr->count);
  for (uint32_t k = 0; k < hdr->count; ++k) {
    const auto* elt = reinterpret_cast<const CelValue*>(
        cel_mem_base() + hdr->elements_offset +
        (static_cast<size_t>(kCelListEntryStride) * k));
    EXPECT_EQ(elt->kind, static_cast<uint32_t>(CEL_STRING));
    if (elt->payload.s.len == 0) {
      out.emplace_back();
    } else {
      out.emplace_back(
          reinterpret_cast<const char*>(cel_mem_base() + elt->payload.s.ptr),
          elt->payload.s.len);
    }
  }
  return out;
}

// Build a CEL_LIST_ARENA of CEL_STRING elements for join inputs.
// Helper because the public `cel_list_create` + `cel_list_append_at`
// requires a per-element temp slot — this fixture-local builder
// stamps each element directly.
uint32_t BuildArenaStringList(const std::vector<std::string>& elements) {
  uint32_t list_slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* out = cel_value_at(list_slot);
  uint32_t hdr_off =
      arena_alloc(static_cast<uint32_t>(sizeof(ArenaListHeader)));
  uint32_t elements_off = 0;
  if (!elements.empty()) {
    elements_off = arena_alloc(static_cast<uint32_t>(
        static_cast<size_t>(kCelListEntryStride) * elements.size()));
  }
  auto* hdr = reinterpret_cast<ArenaListHeader*>(cel_mem_base() + hdr_off);
  hdr->count = static_cast<uint32_t>(elements.size());
  hdr->capacity = static_cast<uint32_t>(elements.size());
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
  for (size_t k = 0; k < elements.size(); ++k) {
    auto* elt = reinterpret_cast<CelValue*>(
        cel_mem_base() + elements_off +
        (static_cast<size_t>(kCelListEntryStride) * k));
    uint32_t byte_off = 0;
    const auto byte_len = static_cast<uint32_t>(elements[k].size());
    if (byte_len > 0) {
      byte_off = arena_alloc(byte_len);
      std::memcpy(cel_mem_base() + byte_off, elements[k].data(), byte_len);
    }
    elt->kind = CEL_STRING;
    elt->payload.s.ptr = byte_off;
    elt->payload.s.len = byte_len;
  }
  return list_slot;
}

// Build a CEL_LIST_HOST slot whose backing vends `elements`.  The
// scripted `cel_list_iter_open` above resolves the ref_slot.
uint32_t BuildHostStringList(uint32_t ref_slot,
                             const std::vector<std::string>& elements) {
  HostListTable()[ref_slot] = elements;
  uint32_t list_slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* v = cel_value_at(list_slot);
  v->kind = CEL_LIST_HOST;
  v->payload.ref_slot = ref_slot;
  return list_slot;
}

// Build a CEL_LIST_ARENA whose element[0] carries a poison CelValue —
// the one-element view `cel_list_arena_view` vends when a host
// snapshot fails on arena OOM.
uint32_t BuildListWithPoisonElement(uint32_t err) {
  uint32_t list_slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* out = cel_value_at(list_slot);
  uint32_t hdr_off =
      arena_alloc(static_cast<uint32_t>(sizeof(ArenaListHeader)));
  uint32_t elements_off = arena_alloc(kCelListEntryStride);
  auto* hdr = reinterpret_cast<ArenaListHeader*>(cel_mem_base() + hdr_off);
  hdr->count = 1;
  hdr->capacity = 1;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
  auto* elt = reinterpret_cast<CelValue*>(cel_mem_base() + elements_off);
  elt->kind = CEL_ERROR;
  elt->payload.err = err;
  return list_slot;
}

// Build a CEL_LIST_ARENA whose element[0] is a non-string CelValue
// (an int).  Used to exercise the join() type-mismatch path.
uint32_t BuildListWithNonStringElement() {
  uint32_t list_slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* out = cel_value_at(list_slot);
  uint32_t hdr_off =
      arena_alloc(static_cast<uint32_t>(sizeof(ArenaListHeader)));
  uint32_t elements_off = arena_alloc(kCelListEntryStride);
  auto* hdr = reinterpret_cast<ArenaListHeader*>(cel_mem_base() + hdr_off);
  hdr->count = 1;
  hdr->capacity = 1;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
  auto* elt = reinterpret_cast<CelValue*>(cel_mem_base() + elements_off);
  elt->kind = CEL_INT;
  elt->payload.i = 42;
  return list_slot;
}

// ===============================================================
// split — conformance fixture rows + boundary matrix.
// ===============================================================

TEST_F(StringExtFixture, SplitBasicByDelimiter) {
  // Spec: `'hello world'.split(' ') == ['hello', 'world']`.
  uint32_t out = MakeOut();
  cel_string_split_at_vv(out, MakeStr("hello world"), MakeStr(" "));
  EXPECT_EQ(ListAsStrings(At(out)),
            (std::vector<std::string>{"hello", "world"}));
}

TEST_F(StringExtFixture, SplitLimitZeroIsEmptyList) {
  // Spec: `'hello world events!'.split(' ', 0) == []`.
  uint32_t out = MakeOut();
  cel_string_split_n_at_vvv(out, MakeStr("hello world events!"), MakeStr(" "),
                            MakeInt(0));
  EXPECT_EQ(ListAsStrings(At(out)), (std::vector<std::string>{}));
}

TEST_F(StringExtFixture, SplitLimitOneIsWholeString) {
  // Spec: `'hello world events!'.split(' ', 1) == ['hello world events!']`.
  uint32_t out = MakeOut();
  cel_string_split_n_at_vvv(out, MakeStr("hello world events!"), MakeStr(" "),
                            MakeInt(1));
  EXPECT_EQ(ListAsStrings(At(out)),
            (std::vector<std::string>{"hello world events!"}));
}

TEST_F(StringExtFixture, SplitNegativeLimitUnlimited) {
  // Spec: `'o©o©o©o'.split('©', -1) == ['o', 'o', 'o', 'o']`.  Locks
  // the unlimited-limit path on multi-byte delimiter.
  uint32_t out = MakeOut();
  cel_string_split_n_at_vvv(out, MakeStr("o©o©o©o"), MakeStr("©"), MakeInt(-1));
  EXPECT_EQ(ListAsStrings(At(out)),
            (std::vector<std::string>{"o", "o", "o", "o"}));
}

TEST_F(StringExtFixture, SplitNoMatchReturnsWholeString) {
  uint32_t out = MakeOut();
  cel_string_split_at_vv(out, MakeStr("hello"), MakeStr("X"));
  EXPECT_EQ(ListAsStrings(At(out)), (std::vector<std::string>{"hello"}));
}

// Output slot aliases the source-string slot — the shape the
// compiler produces when the receiver is a computed temporary
// (`('a'+'b').split(...)`).  AllocList stamps the result list over
// the slot that also held `s`, so DoSplit MUST capture the source
// pointer before AllocList; otherwise every element stamps the
// list header's bytes as string data.  Found by e2e/fuzz
// (`list_string` seed=104): `('hello'+'hello').split('é')` returned
// garbage instead of `["hellohello"]`.
TEST_F(StringExtFixture, SplitOutputAliasesSourceSlot) {
  uint32_t s = MakeStr("hellohello");
  cel_string_split_at_vv(/*out=*/s, /*s=*/s, MakeStr("X"));
  EXPECT_EQ(ListAsStrings(At(s)), (std::vector<std::string>{"hellohello"}));
}

TEST_F(StringExtFixture, SplitOutputAliasesSourceSlotWithMatches) {
  uint32_t s = MakeStr("a,b,c");
  cel_string_split_at_vv(/*out=*/s, /*s=*/s, MakeStr(","));
  EXPECT_EQ(ListAsStrings(At(s)), (std::vector<std::string>{"a", "b", "c"}));
}

TEST_F(StringExtFixture, SplitEmptyHaystackWithSep) {
  uint32_t out = MakeOut();
  cel_string_split_at_vv(out, MakeStr(""), MakeStr(" "));
  // cel-cpp: empty haystack + non-empty sep → `[""]` (single
  // empty-string piece since the final-piece rule forces a push when
  // sep is non-empty).
  EXPECT_EQ(ListAsStrings(At(out)), (std::vector<std::string>{""}));
}

TEST_F(StringExtFixture, SplitEmptySepCodePoints) {
  // Empty delimiter: split into code-points.  Locks the
  // multi-byte-codepoint stride of the empty-sep arm.
  uint32_t out = MakeOut();
  cel_string_split_at_vv(out, MakeStr("a©b"), MakeStr(""));
  EXPECT_EQ(ListAsStrings(At(out)), (std::vector<std::string>{"a", "©", "b"}));
}

TEST_F(StringExtFixture, SplitEmptySepLimitsByCodePoint) {
  // Empty delimiter + limit=2: emit 1 code-point split + tail.
  // For "abc" with limit=2: ["a", "bc"].
  uint32_t out = MakeOut();
  cel_string_split_n_at_vvv(out, MakeStr("abc"), MakeStr(""), MakeInt(2));
  EXPECT_EQ(ListAsStrings(At(out)), (std::vector<std::string>{"a", "bc"}));
}

TEST_F(StringExtFixture, SplitTrailingDelimiterProducesEmpty) {
  // "a," split by "," → ["a", ""].  Trailing empty piece is locked
  // in by the final-piece rule (sep non-empty → push).
  uint32_t out = MakeOut();
  cel_string_split_at_vv(out, MakeStr("a,"), MakeStr(","));
  EXPECT_EQ(ListAsStrings(At(out)), (std::vector<std::string>{"a", ""}));
}

TEST_F(StringExtFixture, SplitEnvelopeAbsorbsError) {
  uint32_t out = MakeOut();
  cel_string_split_at_vv(out, MakeError(), MakeStr(" "));
  ExpectKind(out, CEL_ERROR);
}

TEST_F(StringExtFixture, SplitKindMismatch) {
  uint32_t out = MakeOut();
  cel_string_split_at_vv(out, MakeInt(1), MakeStr(" "));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(StringExtFixture, SplitNKindMismatchOnN) {
  uint32_t out = MakeOut();
  cel_string_split_n_at_vvv(out, MakeStr("a"), MakeStr(" "), MakeStr("1"));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

// ===============================================================
// join — conformance fixture rows + boundary matrix.
// ===============================================================

TEST_F(StringExtFixture, JoinEmptySeparator) {
  // Spec: `['x', 'y'].join() == 'xy'`.
  uint32_t out = MakeOut();
  cel_string_join_at_v(out, BuildArenaStringList({"x", "y"}));
  ExpectStr(out, "xy");
}

TEST_F(StringExtFixture, JoinWithDashSeparator) {
  // Spec: `['x', 'y'].join('-') == 'x-y'`.
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildArenaStringList({"x", "y"}),
                            MakeStr("-"));
  ExpectStr(out, "x-y");
}

TEST_F(StringExtFixture, JoinEmptyListEmptyResult) {
  // Spec: `[].join() == ''`.
  uint32_t out = MakeOut();
  cel_string_join_at_v(out, BuildArenaStringList({}));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, JoinEmptyListWithSepEmptyResult) {
  // Spec: `[].join('-') == ''`.  Separator is ignored for empty
  // lists.
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildArenaStringList({}), MakeStr("-"));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, JoinSingleElementNoSep) {
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildArenaStringList({"only"}), MakeStr("XX"));
  ExpectStr(out, "only");
}

TEST_F(StringExtFixture, JoinThreeElementsWithMultiByteSep) {
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildArenaStringList({"a", "b", "c"}),
                            MakeStr("©"));
  ExpectStr(out, "a©b©c");
}

TEST_F(StringExtFixture, JoinElementsWithEmptyStrings) {
  // Empty-string elements interleave with sep normally — no special
  // case in cel-cpp.
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildArenaStringList({"", "x", ""}),
                            MakeStr("|"));
  ExpectStr(out, "|x|");
}

TEST_F(StringExtFixture, JoinNonStringElementErrors) {
  // cel-cpp errors on the first non-string element.  Our impl
  // poisons with CEL_ERR_TYPE_MISMATCH (validates upfront so no
  // half-built output escapes).
  uint32_t out = MakeOut();
  cel_string_join_at_v(out, BuildListWithNonStringElement());
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(StringExtFixture, JoinNonListErrors) {
  uint32_t out = MakeOut();
  cel_string_join_at_v(out, MakeStr("not a list"));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(StringExtFixture, JoinSepKindMismatch) {
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildArenaStringList({"a"}), MakeInt(1));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(StringExtFixture, JoinEnvelopeAbsorbsError) {
  uint32_t out = MakeOut();
  cel_string_join_at_v(out, MakeError());
  ExpectKind(out, CEL_ERROR);
}

// ===============================================================
// join over a HOST-BACKED list.  An activation-bound `list<string>`
// (or a proto repeated field) reaches the kernel as CEL_LIST_HOST —
// its elements exist only on the host side until
// `cel_list_arena_view` snapshots them, which is what these cases
// pin.  Before that lift was wired, every one of them returned
// CEL_ERR_TYPE_MISMATCH.
// ===============================================================

// Fixture that also resets the scripted host-list table + call count.
class HostListJoinFixture : public StringExtFixture {
 protected:
  void SetUp() override {
    StringExtFixture::SetUp();
    HostListTable().clear();
    g_iter_open_calls = 0;
  }
};

TEST_F(HostListJoinFixture, JoinHostListWithSeparator) {
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildHostStringList(1, {"a", "b"}),
                            MakeStr("-"));
  ExpectStr(out, "a-b");
  EXPECT_EQ(g_iter_open_calls, 1);
}

TEST_F(HostListJoinFixture, JoinHostListNoSeparator) {
  uint32_t out = MakeOut();
  cel_string_join_at_v(out, BuildHostStringList(1, {"a", "b"}));
  ExpectStr(out, "ab");
  EXPECT_EQ(g_iter_open_calls, 1);
}

TEST_F(HostListJoinFixture, JoinEmptyHostList) {
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildHostStringList(1, {}), MakeStr("-"));
  ExpectStr(out, "");
  out = MakeOut();
  cel_string_join_at_v(out, BuildHostStringList(2, {}));
  ExpectStr(out, "");
}

TEST_F(HostListJoinFixture, JoinSingleElementHostList) {
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildHostStringList(1, {"only"}),
                            MakeStr("XX"));
  ExpectStr(out, "only");
}

TEST_F(HostListJoinFixture, JoinHostListWithEmptyStringElements) {
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildHostStringList(1, {"", "x", ""}),
                            MakeStr("|"));
  ExpectStr(out, "|x|");
}

TEST_F(HostListJoinFixture, JoinHostListMultiByteSeparator) {
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildHostStringList(1, {"a", "b", "c"}),
                            MakeStr("©"));
  ExpectStr(out, "a©b©c");
}

TEST_F(HostListJoinFixture, JoinHostListSepKindMismatchSkipsTheLift) {
  // A bad separator is rejected before the (potentially expensive)
  // host snapshot runs.
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildHostStringList(1, {"a"}), MakeInt(1));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
  EXPECT_EQ(g_iter_open_calls, 0);
}

TEST_F(HostListJoinFixture, ArenaListDoesNotTripTheHostArm) {
  // Arena operands pass through `cel_list_arena_view` as identity —
  // no host trip, same result as before the lift was wired.
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, BuildArenaStringList({"x", "y"}),
                            MakeStr("-"));
  ExpectStr(out, "x-y");
  EXPECT_EQ(g_iter_open_calls, 0);
}

TEST_F(HostListJoinFixture, JoinPoisonedElementPropagatesVerbatim) {
  // The one-element poison view `cel_list_arena_view` vends on a
  // failed snapshot must surface AS ITS OWN error code, not be
  // re-labelled a kind mismatch.
  uint32_t out = MakeOut();
  cel_string_join_at_v(out, BuildListWithPoisonElement(CEL_ERR_OVERFLOW));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(HostListJoinFixture, JoinHeaderlessArenaListIsEmpty) {
  // `header_ptr == 0` is the "nothing allocated" shape; offset 0 is
  // never a real allocation, so it must read as an empty list rather
  // than dereferencing into the reserved bytes.
  uint32_t list_slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* v = cel_value_at(list_slot);
  v->kind = CEL_LIST_ARENA;
  v->payload.arena_list.header_ptr = 0;
  uint32_t out = MakeOut();
  cel_string_join_sep_at_vv(out, list_slot, MakeStr("-"));
  ExpectStr(out, "");
}

}  // namespace
}  // namespace celwasm
