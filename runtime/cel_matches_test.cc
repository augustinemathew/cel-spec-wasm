// Coverage for the Phase C `cel_matches_at_vv` regex kernel in
// `cel_matches.{h,cc}`.  RE2 PartialMatch with a per-Instance
// single-slot most-recent-pattern cache.
//
// Test discipline mirrors `cel_time_parse_test.cc` /
// `cel_convert_test.cc`: focused TEST_F coverage for 3VL absorb +
// kind-mismatch + cache-fail; parameterised TEST_P over the spec's
// admit set (the 9 rows in `tests/simple/testdata/string.textproto`
// `matches` section verbatim — these are the cases C5 conformance
// flips from SKIP to PASS).

#include "compiler_v2/runtime/cel_matches.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_layout.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class MatchesFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }

  uint32_t MakeOut() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  uint32_t MakeStr(const char* s) {
    return cel_make_string(s, static_cast<uint32_t>(std::strlen(s)));
  }

  uint32_t MakeStrLen(const char* s, uint32_t n) {
    return cel_make_string(s, n);
  }

  uint32_t MakeError() {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_ERROR;
    v->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
    return slot;
  }

  uint32_t MakeUnknown() {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_UNKNOWN;
    v->payload.unk = 0u;
    return slot;
  }

  uint32_t MakeInt(int64_t i) {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_INT;
    v->payload.i = i;
    return slot;
  }

  const CelValue* At(uint32_t slot) {
    return cel_value_at(slot);
  }

  void ExpectBool(uint32_t slot, int32_t expected) {
    const CelValue* v = At(slot);
    ASSERT_EQ(v->kind, static_cast<uint32_t>(CEL_BOOL));
    EXPECT_EQ(v->payload.b, expected);
  }

  void ExpectError(uint32_t slot, uint32_t err) {
    const CelValue* v = At(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
    EXPECT_EQ(v->payload.err, err);
  }
};

// ───────────────────────────────────────────────────────────────
// 3VL absorb + kind-mismatch (negative coverage).
// ───────────────────────────────────────────────────────────────

TEST_F(MatchesFixture, AbsorbsErrorOnText) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeError(), MakeStr("a"));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
}

TEST_F(MatchesFixture, AbsorbsErrorOnPattern) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr("a"), MakeError());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
}

TEST_F(MatchesFixture, AbsorbsUnknownOnText) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeUnknown(), MakeStr("a"));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

TEST_F(MatchesFixture, AbsorbsUnknownOnPattern) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr("a"), MakeUnknown());
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

TEST_F(MatchesFixture, KindMismatchTextNonString) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeInt(1), MakeStr("a"));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(MatchesFixture, KindMismatchPatternNonString) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr("a"), MakeInt(1));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

// Pattern compile failure → CEL_ERROR(INVALID_ARGUMENT).  Unclosed
// group is an obvious malformed regex RE2 rejects.
TEST_F(MatchesFixture, BadPatternErrors) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr("foo"), MakeStr("("));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

// ───────────────────────────────────────────────────────────────
// Admit-set matrix — `tests/simple/testdata/string.textproto`
// `matches` section verbatim.  These rows are the 9 conformance
// SKIPs C5 flips to PASS.
// ───────────────────────────────────────────────────────────────

struct MatchRow {
  const char* label;
  const char* text;
  const char* pattern;
  int32_t expected;  // 0 or 1
};

class MatchesAdmitTable : public MatchesFixture,
                          public ::testing::WithParamInterface<MatchRow> {};

TEST_P(MatchesAdmitTable, ConformanceCase) {
  const MatchRow& row = GetParam();
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr(row.text), MakeStr(row.pattern));
  ExpectBool(out, row.expected);
}

INSTANTIATE_TEST_SUITE_P(
    SpecRows, MatchesAdmitTable,
    ::testing::Values(
        MatchRow{"basic", "hubba", "ubb", 1},
        MatchRow{"empty_target", "", "foo|bar", 0},
        MatchRow{"empty_arg", "cows", "", 1},
        MatchRow{"empty_empty", "", "", 1},
        MatchRow{"re_concat", "abcd", "bc", 1},
        MatchRow{"re_alt", "grey", "gr(a|e)y", 1},
        MatchRow{"re_rep", "banana", "ba(na)*", 1},
        MatchRow{"unicode", "mañana", "a+ñ+a+", 1},
        // The unicode_smp spec row uses U+1F431 / U+1F600.  Encoded
        // here as raw UTF-8 sequences (4 bytes per code point).
        MatchRow{"unicode_smp",
                 "\xF0\x9F\x90\xB1\xF0\x9F\x98\x80\xF0\x9F\x98\x80",
                 "(a|\xF0\x9F\x98\x80){2}", 1}),
    [](const ::testing::TestParamInfo<MatchRow>& info) {
      return std::string(info.param.label);
    });

// ───────────────────────────────────────────────────────────────
// Cache behaviour — these don't assert internal cache state, just
// that the warm path and pattern-switch path both produce correct
// results.  The kernel keeps a single-slot most-recent-pattern
// cache; repeat-same-pattern hits, distinct patterns recompile.
// ───────────────────────────────────────────────────────────────

TEST_F(MatchesFixture, RepeatedSamePatternCacheHit) {
  // Same pattern + text 1000× — first call compiles + caches; the
  // remaining 999 should hit the cache.  Correctness lock-in.
  // arena_reset() each iter so the 64 KiB arena cap doesn't OOM
  // under repeated MakeStr calls (the cache itself is module-static
  // / dlmalloc-backed, not in the arena).
  for (int i = 0; i < 1000; ++i) {
    arena_reset();
    uint32_t out = MakeOut();
    cel_matches_at_vv(out, MakeStr("hello world"), MakeStr("w[oa]rld"));
    ExpectBool(out, 1);
  }
}

TEST_F(MatchesFixture, AlternatingPatternsBothCorrect) {
  // Two patterns alternating — each call is a cache miss, so the
  // recompile path fires every time.  Both must produce the right
  // answer despite the cache thrashing.
  for (int i = 0; i < 50; ++i) {
    arena_reset();
    uint32_t a = MakeOut();
    cel_matches_at_vv(a, MakeStr("abc"), MakeStr("^a"));
    ExpectBool(a, 1);
    arena_reset();
    uint32_t b = MakeOut();
    cel_matches_at_vv(b, MakeStr("xyz"), MakeStr("z$"));
    ExpectBool(b, 1);
  }
}

// A failed-compile cached as nullptr is sticky: a repeat invocation
// of the SAME bad pattern hits the cache and returns error without
// recompiling.  Probes the nullptr branch in the lookup path.
TEST_F(MatchesFixture, BadPatternIsStickyError) {
  for (int i = 0; i < 5; ++i) {
    arena_reset();
    uint32_t out = MakeOut();
    cel_matches_at_vv(out, MakeStr("any"), MakeStr("[unclosed"));
    ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
  }
}

// ───────────────────────────────────────────────────────────────
// Boundary inputs.
// ───────────────────────────────────────────────────────────────

TEST_F(MatchesFixture, AnchorAtStart) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr("hello"), MakeStr("^hel"));
  ExpectBool(out, 1);
}

TEST_F(MatchesFixture, AnchorAtEnd) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr("hello"), MakeStr("llo$"));
  ExpectBool(out, 1);
}

TEST_F(MatchesFixture, AnchorAtStartNoMatch) {
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr("xhello"), MakeStr("^hel"));
  ExpectBool(out, 0);
}

TEST_F(MatchesFixture, EmbeddedNulInText) {
  // CEL strings can carry embedded NULs.  RE2 should treat them as
  // ordinary bytes; PartialMatch with the 3-byte target containing
  // a NUL should still hit the suffix pattern.
  const char kText[] = {'a', '\0', 'b'};
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStrLen(kText, 3), MakeStr("b$"));
  ExpectBool(out, 1);
}

TEST_F(MatchesFixture, EmptyTextEmptyPatternMatches) {
  // Boundary explicit — both inputs empty.  Mirrors the spec
  // `empty_empty` row but pinned as a standalone case so future
  // changes to the param table can't erase it.
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStrLen("", 0), MakeStrLen("", 0));
  ExpectBool(out, 1);
}

TEST_F(MatchesFixture, EmptyPatternAfterNonEmptyPattern) {
  // Regression: before the `CachedInitialized` flag landed, the
  // empty pattern as the very first call to the kernel would
  // spuriously poison because `CachedPattern() == ""` matched the
  // default-constructed empty pattern and the compile path was
  // skipped while `CachedRe()` was still null.  Conformance row
  // `matches/empty_arg` caught it at the e2e layer; this test
  // pins the regression at the kernel layer by warming the cache
  // with a distinct pattern, then issuing the empty-pattern call
  // — that's the same cold-cache pattern-switch path the
  // conformance row exercises on a fresh wasm Instance.
  arena_reset();
  uint32_t warm_out = MakeOut();
  cel_matches_at_vv(warm_out, MakeStr("dummy"), MakeStr("d"));
  arena_reset();
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStr("cows"), MakeStrLen("", 0));
  ExpectBool(out, 1);
}

TEST_F(MatchesFixture, InvalidUtf8InTextDoesNotCrash) {
  // RE2 accepts invalid UTF-8 as raw bytes by default (the
  // `Latin1` encoding mode is the implicit fallback for sequences
  // it can't decode as UTF-8).  Pin the no-crash contract — the
  // exact match/no-match outcome is RE2-version-sensitive, so the
  // assertion is only that the kernel doesn't poison or trap.
  const char kBadUtf8[] = {'\xC0', '\xC1', '\xFF'};
  uint32_t out = MakeOut();
  cel_matches_at_vv(out, MakeStrLen(kBadUtf8, 3), MakeStr("."));
  const CelValue* v = At(out);
  EXPECT_TRUE(v->kind == CEL_BOOL)
      << "expected a bool result (match-or-not), got kind=" << v->kind;
}

TEST_F(MatchesFixture, LongTextLongPattern) {
  // Boundary: ~4 KiB text + a non-trivial pattern.  Pins that we
  // don't have a hidden size cap somewhere in the cache or the
  // span-borrow path.
  std::string text(4096, 'a');
  text += "needle";
  uint32_t out = MakeOut();
  cel_matches_at_vv(out,
                    MakeStrLen(text.data(), static_cast<uint32_t>(text.size())),
                    MakeStr("a{1000}needle$"));
  ExpectBool(out, 1);
}

}  // namespace
}  // namespace celwasm
