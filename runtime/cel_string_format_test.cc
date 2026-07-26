// M12 Slice D — unit tests for `cel_string_format_internal::ParseFormat`.
// Renderer body (`cel_string_format_at_vv`) is stubbed until Slice E,
// so this TU exercises the parser via the internal header directly.
//
// Coverage:
//   - Every directive type byte (s, d, f, e, b, o, x, X) parses.
//   - `%%` resolves to a literal `%` byte and joins surrounding
//     literal runs.
//   - Literal runs coalesce (one `kLiteral` op per run, not one
//     per byte).
//   - Precision parses for `%.<n>f` / `%.<n>e`; ignored on every
//     other directive (silently dropped to match cel-cpp).
//   - Every malformed shape from §8 of the M12 plan rejects:
//       * `%` at end of string
//       * `%.` with no type byte
//       * `%.<n>` with no type byte
//       * unknown type byte
//       * precision > 1000
//       * non-digit immediately after `%.`
//
// Diagnostic strings match cel-cpp's `ParsePrecision` / `Format`
// verbatim so future drift surfaces immediately.

#include "runtime/cel_string_format_internal.h"

#include "absl/status/status.h"
#include "gtest/gtest.h"

namespace celwasm::string_format_internal {
namespace {

// ───────────────────────────────────────────────────────────────
// Happy-path matrix — one row per directive type.
// ───────────────────────────────────────────────────────────────

TEST(ParseFormatTest, EmptyFormat) {
  auto res = ParseFormat("");
  ASSERT_TRUE(res.ok()) << res.status();
  EXPECT_TRUE(res->empty());
}

TEST(ParseFormatTest, PureLiteralCoalescesIntoOneOp) {
  auto res = ParseFormat("no substitution");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kLiteral);
  EXPECT_EQ((*res)[0].byte_off, 0u);
  EXPECT_EQ((*res)[0].len, 15u);
}

TEST(ParseFormatTest, SubstringDirective) {
  auto res = ParseFormat("%s");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kSubstring);
  EXPECT_EQ((*res)[0].precision, kPrecisionDefault);
}

TEST(ParseFormatTest, DecimalDirective) {
  auto res = ParseFormat("%d");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kDecimal);
}

TEST(ParseFormatTest, FixedDirectiveNoPrecision) {
  auto res = ParseFormat("%f");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kFixed);
  EXPECT_EQ((*res)[0].precision, kPrecisionDefault);
}

TEST(ParseFormatTest, FixedDirectiveWithPrecision) {
  auto res = ParseFormat("%.3f");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kFixed);
  EXPECT_EQ((*res)[0].precision, 3);
}

TEST(ParseFormatTest, ScientificDirectiveWithPrecision) {
  auto res = ParseFormat("%.10e");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kScientific);
  EXPECT_EQ((*res)[0].precision, 10);
}

TEST(ParseFormatTest, BinaryDirective) {
  auto res = ParseFormat("%b");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kBinary);
}

TEST(ParseFormatTest, OctalDirective) {
  auto res = ParseFormat("%o");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kOctal);
}

TEST(ParseFormatTest, HexLowerDirective) {
  auto res = ParseFormat("%x");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kHexLower);
}

TEST(ParseFormatTest, HexUpperDirective) {
  auto res = ParseFormat("%X");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kHexUpper);
}

// ───────────────────────────────────────────────────────────────
// Mixed literal + directive runs.
// ───────────────────────────────────────────────────────────────

TEST(ParseFormatTest, LiteralBeforeDirective) {
  auto res = ParseFormat("str is %s and some more");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 3u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kLiteral);
  EXPECT_EQ((*res)[0].byte_off, 0u);
  EXPECT_EQ((*res)[0].len, 7u);  // "str is "
  EXPECT_EQ((*res)[1].kind, DirectiveKind::kSubstring);
  EXPECT_EQ((*res)[2].kind, DirectiveKind::kLiteral);
  EXPECT_EQ((*res)[2].byte_off, 9u);
  EXPECT_EQ((*res)[2].len, 14u);  // " and some more"
}

TEST(ParseFormatTest, AdjacentDirectives) {
  auto res = ParseFormat("%d%s%x");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 3u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kDecimal);
  EXPECT_EQ((*res)[1].kind, DirectiveKind::kSubstring);
  EXPECT_EQ((*res)[2].kind, DirectiveKind::kHexLower);
}

// `%%` resolves to a single literal `%` byte (the second `%` from
// the `%%` pair).  The coalescer can't span the gap byte at offset
// 1 (the leading `%`) — that byte doesn't appear in the rendered
// output, so the literal op's byte range must skip it.  Result:
// one op for the `a` byte at offset 0, one op for `%b` at
// offsets [2, 4) (the second `%` joins with the trailing `b`).
//
// `byte_off` + `len` name a CONTIGUOUS run of source bytes that
// renders 1:1; the renderer walks the range and emits those bytes
// verbatim.  `%%` is intentionally NOT a single-op (the
// alternative of emitting only a `%` byte would require renderer
// support for "literal op carrying replacement bytes", which adds
// no value).
TEST(ParseFormatTest, PercentPercentSplitsLiteralRun) {
  auto res = ParseFormat("a%%b");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 2u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kLiteral);
  EXPECT_EQ((*res)[0].byte_off, 0u);
  EXPECT_EQ((*res)[0].len, 1u);
  EXPECT_EQ((*res)[1].kind, DirectiveKind::kLiteral);
  EXPECT_EQ((*res)[1].byte_off, 2u);
  EXPECT_EQ((*res)[1].len, 2u);
}

TEST(ParseFormatTest, MultipleDirectives) {
  auto res = ParseFormat("[%d, %d, %d]");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 7u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kLiteral);
  EXPECT_EQ((*res)[1].kind, DirectiveKind::kDecimal);
  EXPECT_EQ((*res)[2].kind, DirectiveKind::kLiteral);
  EXPECT_EQ((*res)[3].kind, DirectiveKind::kDecimal);
  EXPECT_EQ((*res)[4].kind, DirectiveKind::kLiteral);
  EXPECT_EQ((*res)[5].kind, DirectiveKind::kDecimal);
  EXPECT_EQ((*res)[6].kind, DirectiveKind::kLiteral);
}

// Precision boundary at the max — accepted.
TEST(ParseFormatTest, PrecisionAtBoundary) {
  auto res = ParseFormat("%.1000f");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kFixed);
  EXPECT_EQ((*res)[0].precision, 1000);
}

TEST(ParseFormatTest, PrecisionZero) {
  auto res = ParseFormat("%.0f");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].precision, 0);
}

// Precision on a non-numeric directive: cel-cpp silently drops it.
// We mirror — `%.5s` parses as `kSubstring` with `precision` left
// at `kPrecisionDefault`.
TEST(ParseFormatTest, PrecisionSilentlyDroppedOnSubstring) {
  auto res = ParseFormat("%.5s");
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res->size(), 1u);
  EXPECT_EQ((*res)[0].kind, DirectiveKind::kSubstring);
  EXPECT_EQ((*res)[0].precision, kPrecisionDefault);
}

// ───────────────────────────────────────────────────────────────
// Malformed inputs — every reject path from the §8 plan.
// ───────────────────────────────────────────────────────────────

TEST(ParseFormatTest, RejectsPercentAtEof) {
  auto res = ParseFormat("%");
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(res.status().message(), "unexpected end of format string");
}

TEST(ParseFormatTest, RejectsLeadingLiteralThenPercentAtEof) {
  auto res = ParseFormat("hello %");
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ParseFormatTest, RejectsBareDot) {
  // `%.` with no digits and no type byte.
  auto res = ParseFormat("%.");
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().message(),
            "unable to find end of precision specifier");
}

TEST(ParseFormatTest, RejectsDotDigitsNoType) {
  // `%.<n>` with no type byte.
  auto res = ParseFormat("%.5");
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().message(),
            "unable to find end of precision specifier");
}

TEST(ParseFormatTest, RejectsUnknownTypeByte) {
  auto res = ParseFormat("%q");
  ASSERT_FALSE(res.ok());
  // cel-cpp's diagnostic format includes the offending byte.
  EXPECT_EQ(res.status().message(), "unrecognized formatting clause \"q\"");
}

TEST(ParseFormatTest, RejectsUnknownTypeByteAfterPrecision) {
  auto res = ParseFormat("%.3z");
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().message(), "unrecognized formatting clause \"z\"");
}

TEST(ParseFormatTest, RejectsPrecisionOverMax) {
  auto res = ParseFormat("%.1001f");
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().message(),
            "precision specifier exceeds maximum of 1000");
}

TEST(ParseFormatTest, RejectsRepeatedDotInPrecision) {
  // `%.5.3f` — after the first run of digits the loop stops at the
  // second `.`; we then expect a type byte but see `.`, which
  // doesn't match any known directive type byte.  cel-cpp surfaces
  // this as `unrecognized formatting clause ".".
  auto res = ParseFormat("%.5.3f");
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().message(), "unrecognized formatting clause \".\"");
}

// A literal `%%` followed by `%` at the end of the string still
// rejects.  Asserts the parser doesn't accidentally swallow the
// trailing `%` as part of `%%`.
TEST(ParseFormatTest, RejectsPercentAfterPercentPercent) {
  auto res = ParseFormat("%%%");
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().message(), "unexpected end of format string");
}

}  // namespace
}  // namespace celwasm::string_format_internal

// ───────────────────────────────────────────────────────────────
// Dispatcher / renderer tests — exercise the public ABI
// `cel_string_format_at_vv` directly.  Uses the shared
// `StringExtFixture` so each TEST_F runs in a fresh arena.
// ───────────────────────────────────────────────────────────────

#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_format.h"
#include "runtime/string_ext_test_helpers.h"

namespace {

// ref_slot → the elements / entries a host-backed aggregate vends.
// The scripted trampolines below materialise these into the arena
// exactly as the production ones do (eval/internal/cel_host.cc
// `SnapshotHostListToArena` / `SnapshotMapEntriesToArena`).
std::map<uint32_t, std::vector<CelValue>>& FormatHostListTable() {
  static auto* t = new std::map<uint32_t, std::vector<CelValue>>();
  return *t;
}
std::map<uint32_t, std::vector<std::pair<CelValue, CelValue>>>&
FormatHostMapTable() {
  static auto* t =
      new std::map<uint32_t, std::vector<std::pair<CelValue, CelValue>>>();
  return *t;
}

}  // namespace

extern "C" {

// Strong overrides of the weak host-build stubs in cel_runtime.c.
void cel_host_cel_list_iter_open(uint32_t out_slot, uint32_t list_slot) {
  const std::vector<CelValue> elements =
      FormatHostListTable()[cel_value_at(list_slot)->payload.ref_slot];
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
    *reinterpret_cast<CelValue*>(
        cel_mem_base() + elements_off +
        (static_cast<size_t>(kCelListEntryStride) * k)) = elements[k];
  }
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
}

// MapIterState is `{u32 kind; u32 cursor; u32 payload; u32 count;}`
// (cel_runtime.c); the HOST snapshot payload is `count` × 48-byte
// entries, key at +0 and value at +24.
void cel_host_cel_map_iter_open(uint32_t state_offset, uint32_t map_slot) {
  constexpr uint32_t kHostKind = 1u;
  constexpr uint32_t kPerEntry = 2u * sizeof(CelValue);
  const std::vector<std::pair<CelValue, CelValue>> entries =
      FormatHostMapTable()[cel_value_at(map_slot)->payload.ref_slot];
  uint32_t snapshot_off = 0;
  if (!entries.empty()) {
    snapshot_off =
        arena_alloc(static_cast<uint32_t>(entries.size()) * kPerEntry);
    for (size_t i = 0; i < entries.size(); ++i) {
      const uint32_t key_off =
          snapshot_off + (static_cast<uint32_t>(i) * kPerEntry);
      *reinterpret_cast<CelValue*>(cel_mem_base() + key_off) = entries[i].first;
      *reinterpret_cast<CelValue*>(cel_mem_base() + key_off +
                                   sizeof(CelValue)) = entries[i].second;
    }
  }
  auto* state = reinterpret_cast<uint32_t*>(cel_mem_base() + state_offset);
  state[0] = kHostKind;
  state[1] = 0u;
  state[2] = snapshot_off;
  state[3] = static_cast<uint32_t>(entries.size());
}

}  // extern "C"

namespace celwasm {
namespace {

class FormatDispatcherTest : public StringExtFixture {
 protected:
  // Build a CEL_LIST_ARENA holding `slots` (each slot is a
  // pre-populated CelValue slot offset).  Copies the slot's
  // payload into a fresh element-array entry so per-test slot
  // reuse stays safe.
  uint32_t MakeArenaList(std::initializer_list<uint32_t> slots) {
    const uint32_t list_slot =
        arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* list = cel_value_at(list_slot);
    const uint32_t hdr_off =
        arena_alloc(static_cast<uint32_t>(sizeof(ArenaListHeader)));
    const auto count = static_cast<uint32_t>(slots.size());
    const uint32_t elements_off =
        count == 0 ? 0u
                   : arena_alloc(static_cast<uint32_t>(
                         static_cast<size_t>(kCelListEntryStride) * count));
    auto* hdr = reinterpret_cast<ArenaListHeader*>(cel_mem_base() + hdr_off);
    hdr->count = count;
    hdr->capacity = count;
    hdr->elements_offset = elements_off;
    hdr->_pad = 0;
    list->kind = CEL_LIST_ARENA;
    list->payload.arena_list.header_ptr = hdr_off;
    uint32_t k = 0;
    for (uint32_t slot : slots) {
      *reinterpret_cast<CelValue*>(
          cel_mem_base() + elements_off +
          (static_cast<size_t>(kCelListEntryStride) * k)) = *cel_value_at(slot);
      ++k;
    }
    return list_slot;
  }

  // Build a CEL_MAP_ARENA holding `entries` (pairs of pre-populated
  // CelValue slot offsets).  Entries are 48 bytes: key at +0, value
  // at +24 — the layout `cel_runtime.c` writes.
  uint32_t MakeArenaMap(
      std::initializer_list<std::pair<uint32_t, uint32_t>> entries) {
    const uint32_t map_slot =
        arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    const uint32_t hdr_off =
        arena_alloc(static_cast<uint32_t>(sizeof(ArenaMapHeader)));
    const auto count = static_cast<uint32_t>(entries.size());
    const uint32_t entries_off =
        count == 0 ? 0u
                   : arena_alloc(static_cast<uint32_t>(
                         static_cast<size_t>(kCelMapEntryStride) * count));
    auto* hdr = reinterpret_cast<ArenaMapHeader*>(cel_mem_base() + hdr_off);
    hdr->count = count;
    hdr->capacity = count;
    hdr->entries_offset = entries_off;
    uint32_t k = 0;
    for (const auto& [key, val] : entries) {
      uint8_t* entry = cel_mem_base() + entries_off +
                       (static_cast<size_t>(kCelMapEntryStride) * k);
      *reinterpret_cast<CelValue*>(entry) = *cel_value_at(key);
      *reinterpret_cast<CelValue*>(entry + sizeof(CelValue)) =
          *cel_value_at(val);
      ++k;
    }
    CelValue* map = cel_value_at(map_slot);
    map->kind = CEL_MAP_ARENA;
    map->payload.arena_map.header_ptr = hdr_off;
    return map_slot;
  }

  uint32_t MakeDouble(double d) {
    const uint32_t s = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(s);
    v->kind = CEL_DOUBLE;
    v->payload.d = d;
    return s;
  }

  uint32_t MakeUint(uint64_t u) {
    const uint32_t s = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(s);
    v->kind = CEL_UINT;
    v->payload.u = u;
    return s;
  }

  uint32_t MakeBool(bool b) {
    const uint32_t s = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(s);
    v->kind = CEL_BOOL;
    v->payload.b = b ? 1 : 0;
    return s;
  }

  uint32_t MakeNull() {
    const uint32_t s = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(s);
    v->kind = CEL_NULL;
    v->payload.i = 0;
    return s;
  }

  uint32_t MakeBytes(const char* data, uint32_t n) {
    const uint32_t s = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(s);
    v->kind = CEL_BYTES;
    if (n == 0) {
      v->payload.bytes.ptr = 0;
      v->payload.bytes.len = 0;
    } else {
      const uint32_t off = arena_alloc(n);
      std::memcpy(cel_mem_base() + off, data, n);
      v->payload.bytes.ptr = off;
      v->payload.bytes.len = n;
    }
    return s;
  }
};

// ── Pure literal — no directives, no args. ─────────────────────

TEST_F(FormatDispatcherTest, PureLiteral) {
  const uint32_t s = MakeStr("no substitution");
  const uint32_t args = MakeArenaList({});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "no substitution");
}

TEST_F(FormatDispatcherTest, EmptyFormat) {
  const uint32_t s = MakeStr("");
  const uint32_t args = MakeArenaList({});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "");
}

// ── %% escape. ────────────────────────────────────────────────

TEST_F(FormatDispatcherTest, PercentEscape) {
  const uint32_t s = MakeStr("%% and also %%");
  const uint32_t args = MakeArenaList({});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "% and also %");
}

TEST_F(FormatDispatcherTest, PercentEscapeAroundDirective) {
  const uint32_t s = MakeStr("%%%s%%");
  const uint32_t args = MakeArenaList({MakeStr("text")});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "%text%");
}

// ── %s — per-CelKind dispatch. ────────────────────────────────

TEST_F(FormatDispatcherTest, StringFromString) {
  const uint32_t s = MakeStr("str is %s and some more");
  const uint32_t args = MakeArenaList({MakeStr("filler")});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "str is filler and some more");
}

TEST_F(FormatDispatcherTest, StringFromInt) {
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeArenaList({MakeInt(999999999999LL)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "999999999999");
}

TEST_F(FormatDispatcherTest, StringFromBool) {
  const uint32_t s = MakeStr("true bool: %s, false bool: %s");
  const uint32_t args = MakeArenaList({MakeBool(true), MakeBool(false)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "true bool: true, false bool: false");
}

TEST_F(FormatDispatcherTest, StringFromNull) {
  const uint32_t s = MakeStr("null: %s");
  const uint32_t args = MakeArenaList({MakeNull()});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "null: null");
}

TEST_F(FormatDispatcherTest, StringFromBytes) {
  const uint32_t s = MakeStr("some bytes: %s");
  const uint32_t args = MakeArenaList({MakeBytes("xyz", 3)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "some bytes: xyz");
}

// ── %d — decimal. ─────────────────────────────────────────────

TEST_F(FormatDispatcherTest, DecimalFromInt) {
  const uint32_t s = MakeStr("%d");
  const uint32_t args = MakeArenaList({MakeInt(42)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "42");
}

TEST_F(FormatDispatcherTest, DecimalFromUint) {
  const uint32_t s = MakeStr("%d");
  const uint32_t args = MakeArenaList({MakeUint(64ULL)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "64");
}

TEST_F(FormatDispatcherTest, DecimalNegativeInt) {
  const uint32_t s = MakeStr("%d");
  const uint32_t args = MakeArenaList({MakeInt(-1234)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "-1234");
}

// ── %f — fixed-point.  cel-cpp default precision 6. ──────────

TEST_F(FormatDispatcherTest, FixedDefaultPrecision) {
  const uint32_t s = MakeStr("%f");
  const uint32_t args = MakeArenaList({MakeDouble(2.71828)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "2.718280");
}

TEST_F(FormatDispatcherTest, FixedExplicitPrecision) {
  const uint32_t s = MakeStr("%.2f");
  const uint32_t args = MakeArenaList({MakeDouble(10000.1234)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "10000.12");
}

TEST_F(FormatDispatcherTest, FixedNaN) {
  const uint32_t s = MakeStr("%f");
  const uint32_t args =
      MakeArenaList({MakeDouble(std::numeric_limits<double>::quiet_NaN())});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "NaN");
}

TEST_F(FormatDispatcherTest, FixedPosInf) {
  const uint32_t s = MakeStr("%f");
  const uint32_t args =
      MakeArenaList({MakeDouble(std::numeric_limits<double>::infinity())});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "Infinity");
}

TEST_F(FormatDispatcherTest, FixedNegInf) {
  const uint32_t s = MakeStr("%f");
  const uint32_t args =
      MakeArenaList({MakeDouble(-std::numeric_limits<double>::infinity())});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "-Infinity");
}

// ── %e — scientific. ─────────────────────────────────────────

TEST_F(FormatDispatcherTest, ScientificDefaultPrecision) {
  const uint32_t s = MakeStr("%e");
  const uint32_t args = MakeArenaList({MakeDouble(2.71828)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "2.718280e+00");
}

TEST_F(FormatDispatcherTest, ScientificExplicitPrecision) {
  const uint32_t s = MakeStr("%.6e");
  const uint32_t args = MakeArenaList({MakeDouble(1052.032911275)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "1.052033e+03");
}

// ── %b — binary. ─────────────────────────────────────────────

TEST_F(FormatDispatcherTest, BinaryPositive) {
  const uint32_t s = MakeStr("%b");
  const uint32_t args = MakeArenaList({MakeInt(5)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "101");
}

TEST_F(FormatDispatcherTest, BinaryUint) {
  const uint32_t s = MakeStr("%b");
  const uint32_t args = MakeArenaList({MakeUint(64)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "1000000");
}

TEST_F(FormatDispatcherTest, BinaryBool) {
  const uint32_t s = MakeStr("%b");
  const uint32_t args = MakeArenaList({MakeBool(true)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "1");
}

TEST_F(FormatDispatcherTest, BinaryZero) {
  const uint32_t s = MakeStr("%b");
  const uint32_t args = MakeArenaList({MakeInt(0)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "0");
}

// ── %o — octal. ──────────────────────────────────────────────

TEST_F(FormatDispatcherTest, OctalPositive) {
  const uint32_t s = MakeStr("%o");
  const uint32_t args = MakeArenaList({MakeInt(11)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "13");
}

TEST_F(FormatDispatcherTest, OctalUint) {
  const uint32_t s = MakeStr("%o");
  const uint32_t args = MakeArenaList({MakeUint(65535)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "177777");
}

// ── %x / %X — hex. ───────────────────────────────────────────

TEST_F(FormatDispatcherTest, HexLowerInt) {
  const uint32_t s = MakeStr("%x");
  const uint32_t args = MakeArenaList({MakeInt(30)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "1e");
}

TEST_F(FormatDispatcherTest, HexUpperInt) {
  const uint32_t s = MakeStr("%X");
  const uint32_t args = MakeArenaList({MakeInt(30)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "1E");
}

TEST_F(FormatDispatcherTest, HexLowerString) {
  const uint32_t s = MakeStr("%x");
  const uint32_t args = MakeArenaList({MakeStr("Hello world!")});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "48656c6c6f20776f726c6421");
}

TEST_F(FormatDispatcherTest, HexUpperString) {
  const uint32_t s = MakeStr("%X");
  const uint32_t args = MakeArenaList({MakeStr("Hello world!")});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "48656C6C6F20776F726C6421");
}

TEST_F(FormatDispatcherTest, HexLowerBytes) {
  const uint32_t s = MakeStr("%x");
  const uint32_t args = MakeArenaList({MakeBytes("byte string", 11)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "6279746520737472696e67");
}

// ── %s for timestamp / duration / type. ──────────────────────

TEST_F(FormatDispatcherTest, StringFromTimestamp) {
  const uint32_t ts_slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* ts = cel_value_at(ts_slot);
  ts->kind = CEL_TIMESTAMP;
  // 1675467080 == 2023-02-03T23:31:20Z (UTC).  Verified via
  // `date -u -j -f "%s" 1675467080`.  Spec row
  // `TimestampSupportForString` checks this exact string.
  ts->payload.ts.seconds = 1675467080;
  ts->payload.ts.nanos = 0;
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeArenaList({ts_slot});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "2023-02-03T23:31:20Z");
}

TEST_F(FormatDispatcherTest, StringFromDuration) {
  // 1h45m47s = 6347 seconds.  cel-cpp emits "6347s".
  const uint32_t d_slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* d = cel_value_at(d_slot);
  d->kind = CEL_DURATION;
  d->payload.dur.seconds = 6347;
  d->payload.dur.nanos = 0;
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeArenaList({d_slot});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "6347s");
}

TEST_F(FormatDispatcherTest, StringFromType) {
  const uint32_t t_slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* t = cel_value_at(t_slot);
  t->kind = CEL_TYPE;
  static constexpr char kTypeName[] = "string";
  const uint32_t name_off = arena_alloc(sizeof(kTypeName) - 1);
  std::memcpy(cel_mem_base() + name_off, kTypeName, sizeof(kTypeName) - 1);
  t->payload.s.ptr = name_off;
  t->payload.s.len = sizeof(kTypeName) - 1;
  const uint32_t s = MakeStr("type is %s");
  const uint32_t args = MakeArenaList({t_slot});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "type is string");
}

// ── %s on a list — recursive RenderString. ───────────────────

TEST_F(FormatDispatcherTest, StringFromList) {
  const uint32_t inner =
      MakeArenaList({MakeStr("abc"), MakeInt(42), MakeNull()});
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeArenaList({inner});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "[abc, 42, null]");
}

TEST_F(FormatDispatcherTest, StringFromEmptyList) {
  const uint32_t inner = MakeArenaList({});
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeArenaList({inner});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "[]");
}

// Multiple substitutions in one format string.
TEST_F(FormatDispatcherTest, MultipleSubstitutions) {
  const uint32_t s = MakeStr("%d %s %d");
  const uint32_t args =
      MakeArenaList({MakeInt(1), MakeStr("middle"), MakeInt(3)});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "1 middle 3");
}

// ── Error paths. ─────────────────────────────────────────────

TEST_F(FormatDispatcherTest, RejectsMalformedFormat) {
  const uint32_t s = MakeStr("%");
  const uint32_t args = MakeArenaList({});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(FormatDispatcherTest, RejectsKindMismatchForDecimal) {
  const uint32_t s = MakeStr("%d");
  const uint32_t args = MakeArenaList({MakeStr("not a number")});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(FormatDispatcherTest, RejectsTooFewArgs) {
  const uint32_t s = MakeStr("%s %s");
  const uint32_t args = MakeArenaList({MakeStr("only-one")});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(FormatDispatcherTest, AcceptsTooManyArgs) {
  // cel-cpp silently ignores trailing unconsumed list elements.
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeArenaList({MakeStr("used"), MakeStr("ignored")});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "used");
}

// ── %s on an arena map — key-sorted canonical form. ──────────

TEST_F(FormatDispatcherTest, StringFromArenaMapSortsByStringifiedKey) {
  // cel-cpp's FormatMap builds a btree_map<stringified_key, value>,
  // so iteration order is lexicographic on the key's string form.
  const uint32_t inner =
      MakeArenaMap({{MakeStr("b"), MakeInt(2)}, {MakeStr("a"), MakeInt(1)}});
  const uint32_t s = MakeStr("%s");
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, MakeArenaList({inner}));
  ExpectStr(out, "{a: 1, b: 2}");
}

TEST_F(FormatDispatcherTest, StringFromEmptyArenaMap) {
  const uint32_t inner = MakeArenaMap({});
  const uint32_t s = MakeStr("%s");
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, MakeArenaList({inner}));
  ExpectStr(out, "{}");
}

TEST_F(FormatDispatcherTest, RejectsArenaMapWithUnformattableKey) {
  // cel-cpp's FormatMap admits STRING / BOOL / INT / UINT keys only.
  const uint32_t inner = MakeArenaMap({{MakeDouble(1.5), MakeInt(1)}});
  const uint32_t s = MakeStr("%s");
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, MakeArenaList({inner}));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

// ═══════════════════════════════════════════════════════════════
// HOST-ORIGIN aggregates.
//
// An activation-bound `list<T>` / `map<K, V>` (or a proto repeated /
// map field) reaches the kernel as CEL_LIST_HOST / CEL_MAP_HOST —
// its elements exist only on the host side until
// `cel_list_arena_view` / `cel_map_iter_init` materialise them.
// Two independent gates had to move:
//
//   - `"%s".format([xs])` — a host aggregate NESTED inside an arena
//     args list, which drives `AppendListCanonical` /
//     `AppendMapCanonical` (these returned `false`, surfacing as
//     INVALID_ARGUMENT);
//   - `"%s".format(xs)`   — the ARGS list itself being host-backed,
//     which the dispatcher rejected as TYPE_MISMATCH.
//
// Both are pinned below, plus the arena twins so a regression on
// either origin is visible in the same file.
// ═══════════════════════════════════════════════════════════════

class HostAggregateFormatTest : public FormatDispatcherTest {
 protected:
  void SetUp() override {
    FormatDispatcherTest::SetUp();
    FormatHostListTable().clear();
    FormatHostMapTable().clear();
  }

  // A CEL_LIST_HOST slot whose scripted backing vends `elements`.
  uint32_t MakeHostList(uint32_t ref_slot,
                        std::initializer_list<uint32_t> slots) {
    std::vector<CelValue>& elements = FormatHostListTable()[ref_slot];
    for (uint32_t slot : slots)
      elements.push_back(*cel_value_at(slot));
    const uint32_t s = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(s);
    v->kind = CEL_LIST_HOST;
    v->payload.ref_slot = ref_slot;
    return s;
  }

  // A CEL_MAP_HOST slot whose scripted backing vends `entries`.
  uint32_t MakeHostMap(
      uint32_t ref_slot,
      std::initializer_list<std::pair<uint32_t, uint32_t>> entries) {
    auto& stored = FormatHostMapTable()[ref_slot];
    for (const auto& [k, v] : entries) {
      stored.emplace_back(*cel_value_at(k), *cel_value_at(v));
    }
    const uint32_t s = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* cv = cel_value_at(s);
    cv->kind = CEL_MAP_HOST;
    cv->payload.ref_slot = ref_slot;
    return s;
  }

  // `"%s".format([agg])` — the aggregate nested inside arena args.
  void ExpectNested(uint32_t agg_slot, const char* want) {
    const uint32_t out = MakeOut();
    cel_string_format_at_vv(out, MakeStr("%s"), MakeArenaList({agg_slot}));
    ExpectStr(out, want);
  }
};

TEST_F(HostAggregateFormatTest, NestedHostListRenders) {
  ExpectNested(MakeHostList(1, {MakeInt(1), MakeInt(2)}), "[1, 2]");
}

TEST_F(HostAggregateFormatTest, NestedArenaListRendersIdentically) {
  ExpectNested(MakeArenaList({MakeInt(1), MakeInt(2)}), "[1, 2]");
}

TEST_F(HostAggregateFormatTest, NestedEmptyHostListRenders) {
  ExpectNested(MakeHostList(1, {}), "[]");
}

TEST_F(HostAggregateFormatTest, NestedHostListOfHostListsRenders) {
  const uint32_t inner = MakeHostList(1, {MakeInt(1), MakeInt(2)});
  ExpectNested(MakeHostList(2, {inner}), "[[1, 2]]");
}

TEST_F(HostAggregateFormatTest, NestedHostListBoundaryIntegers) {
  ExpectNested(MakeHostList(1, {MakeInt(-9223372036854775807LL - 1),
                                MakeInt(9223372036854775807LL)}),
               "[-9223372036854775808, 9223372036854775807]");
}

TEST_F(HostAggregateFormatTest, NestedHostMapRendersKeySorted) {
  ExpectNested(
      MakeHostMap(1, {{MakeStr("b"), MakeInt(2)}, {MakeStr("a"), MakeInt(1)}}),
      "{a: 1, b: 2}");
}

TEST_F(HostAggregateFormatTest, NestedEmptyHostMapRenders) {
  ExpectNested(MakeHostMap(1, {}), "{}");
}

TEST_F(HostAggregateFormatTest, NestedHostMapOfHostListsRenders) {
  const uint32_t inner = MakeHostList(1, {MakeInt(1)});
  ExpectNested(MakeHostMap(2, {{MakeStr("a"), inner}}), "{a: [1]}");
}

TEST_F(HostAggregateFormatTest, RejectsHostMapWithUnformattableKey) {
  const uint32_t out = MakeOut();
  const uint32_t m = MakeHostMap(1, {{MakeDouble(1.5), MakeInt(1)}});
  cel_string_format_at_vv(out, MakeStr("%s"), MakeArenaList({m}));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(HostAggregateFormatTest, HostListAsTheArgsListItself) {
  // `"%s".format(xs)` — the args slot is host-backed, so each
  // directive consumes one ELEMENT of the bound list.
  const uint32_t args = MakeHostList(1, {MakeInt(1), MakeStr("two")});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, MakeStr("%s-%s"), args);
  ExpectStr(out, "1-two");
}

TEST_F(HostAggregateFormatTest, EmptyHostArgsListWithNoDirectives) {
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, MakeStr("literal"), MakeHostList(1, {}));
  ExpectStr(out, "literal");
}

TEST_F(HostAggregateFormatTest, EmptyHostArgsListWithADirectiveRejects) {
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, MakeStr("%s"), MakeHostList(1, {}));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(HostAggregateFormatTest, NonListArgsOperandStillPoisonsTypeMismatch) {
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, MakeStr("%s"), MakeInt(7));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(HostAggregateFormatTest, PoisonedElementOfHostArgsListAbsorbs) {
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, MakeStr("%s"), MakeHostList(1, {MakeError()}));
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(FormatDispatcherTest, AbsorbsErrorArg) {
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeArenaList({MakeError()});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(FormatDispatcherTest, AbsorbsUnknownArg) {
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeArenaList({MakeUnknown()});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectKind(out, CEL_UNKNOWN);
}

TEST_F(FormatDispatcherTest, AbsorbsErrorFormat) {
  const uint32_t s = MakeError();
  const uint32_t args = MakeArenaList({});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(FormatDispatcherTest, KindMismatchFormatString) {
  const uint32_t s = MakeInt(42);
  const uint32_t args = MakeArenaList({});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(FormatDispatcherTest, KindMismatchArgsList) {
  const uint32_t s = MakeStr("%s");
  const uint32_t args = MakeStr("not a list");
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

// ── Cache regression — empty format string. ─────────────────
// `cel_matches.cc`'s empty-pattern bug applies here too: without
// the `CachedInitialized` flag the very first call with an empty
// format string would spuriously hit and replay default state.
// Lock the behavior.

TEST_F(FormatDispatcherTest, EmptyFormatFirstCallReturnsEmpty) {
  const uint32_t s = MakeStr("");
  const uint32_t args = MakeArenaList({});
  const uint32_t out = MakeOut();
  cel_string_format_at_vv(out, s, args);
  ExpectStr(out, "");
}

// Repeated calls with the same format hit the cache.  Verified
// indirectly — the output must be byte-identical.
TEST_F(FormatDispatcherTest, CacheHitRepeatedSameFormat) {
  for (int i = 0; i < 3; ++i) {
    const uint32_t s = MakeStr("%d items");
    const uint32_t args = MakeArenaList({MakeInt(7 + i)});
    const uint32_t out = MakeOut();
    cel_string_format_at_vv(out, s, args);
    ExpectStr(out, absl::StrCat(7 + i, " items"));
  }
}

// Switching format strings re-parses without state bleed.
TEST_F(FormatDispatcherTest, CacheMissAcrossFormats) {
  {
    const uint32_t s = MakeStr("first %s");
    const uint32_t args = MakeArenaList({MakeStr("A")});
    const uint32_t out = MakeOut();
    cel_string_format_at_vv(out, s, args);
    ExpectStr(out, "first A");
  }
  {
    const uint32_t s = MakeStr("second: %d");
    const uint32_t args = MakeArenaList({MakeInt(99)});
    const uint32_t out = MakeOut();
    cel_string_format_at_vv(out, s, args);
    ExpectStr(out, "second: 99");
  }
}

// Cached parse error sticks — repeated bad format calls re-emit
// the same error without crashing.
TEST_F(FormatDispatcherTest, CachedParseErrorSticks) {
  for (int i = 0; i < 3; ++i) {
    const uint32_t s = MakeStr("%");
    const uint32_t args = MakeArenaList({});
    const uint32_t out = MakeOut();
    cel_string_format_at_vv(out, s, args);
    ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
  }
}

}  // namespace
}  // namespace celwasm
