// Unit coverage for the Slice B search/extract kernels in
// `cel_string_ext_search.cc`: indexOf × 2, lastIndexOf × 2,
// substring × 2, replace × 2.  Spec rows lifted from
// `tests/simple/testdata/string_ext.textproto` per-function
// sections (`index_of`, `last_index_of`, `substring`, `replace`).

#include <cstdint>
#include <string>

#include "runtime/cel_string_ext.h"
#include "runtime/string_ext_test_helpers.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// ===============================================================
// indexOf — conformance fixture rows + boundary matrix.
// ===============================================================

TEST_F(StringExtFixture, IndexOfEmptyNeedleIsZero) {
  // Spec: `'tacocat'.indexOf('') == 0`.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("tacocat"), MakeStr(""));
  ExpectInt(out, 0);
}

TEST_F(StringExtFixture, IndexOfAsciiMatch) {
  // Spec: `'tacocat'.indexOf('ac') == 1`.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("tacocat"), MakeStr("ac"));
  ExpectInt(out, 1);
}

TEST_F(StringExtFixture, IndexOfNoMatch) {
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("tacocat"), MakeStr("none"));
  ExpectInt(out, -1);
}

TEST_F(StringExtFixture, IndexOfWithPosEmpty) {
  // Spec: `'tacocat'.indexOf('', 3) == 3`.  Empty needle matches at
  // the very first code-point position >= pos.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vvv(out, MakeStr("tacocat"), MakeStr(""), MakeInt(3));
  ExpectInt(out, 3);
}

TEST_F(StringExtFixture, IndexOfWithPosChar) {
  // Spec: `'tacocat'.indexOf('a', 3) == 5`.  Skips the 'a' at cp=1.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vvv(out, MakeStr("tacocat"), MakeStr("a"), MakeInt(3));
  ExpectInt(out, 5);
}

TEST_F(StringExtFixture, IndexOfWithPosString) {
  // Spec: `'tacocat'.indexOf('at', 3) == 5`.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vvv(out, MakeStr("tacocat"), MakeStr("at"),
                             MakeInt(3));
  ExpectInt(out, 5);
}

TEST_F(StringExtFixture, IndexOfUnicodeChar) {
  // Spec: `'ta©o©αT'.indexOf('©') == 2`.  Bytes 2-3 of 'ta©…' are the
  // first '©'; returns the code-point index (2), not the byte index.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("ta©o©αT"), MakeStr("©"));
  ExpectInt(out, 2);
}

TEST_F(StringExtFixture, IndexOfUnicodeCharWithPos) {
  // Spec: `'ta©o©αT'.indexOf('©', 3) == 4`.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vvv(out, MakeStr("ta©o©αT"), MakeStr("©"), MakeInt(3));
  ExpectInt(out, 4);
}

TEST_F(StringExtFixture, IndexOfUnicodeString) {
  // Spec: `'ta©o©αT'.indexOf('©αT', 3) == 4`.  The second '©αT' span
  // starts at code-point 4 / byte 5.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vvv(out, MakeStr("ta©o©αT"), MakeStr("©αT"),
                             MakeInt(3));
  ExpectInt(out, 4);
}

TEST_F(StringExtFixture, IndexOfUnicodeNoMatchWithPos) {
  // Spec: `'ta©o©αT'.indexOf('©α', 5) == -1`.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vvv(out, MakeStr("ta©o©αT"), MakeStr("©α"),
                             MakeInt(5));
  ExpectInt(out, -1);
}

TEST_F(StringExtFixture, IndexOfFullStringFromZero) {
  // Spec: `'hello wello'.indexOf('hello wello') == 0`.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("hello wello"),
                            MakeStr("hello wello"));
  ExpectInt(out, 0);
}

TEST_F(StringExtFixture, IndexOfPosBeyondHaystackErrors) {
  // cel-cpp's `IndexOf3` returns CEL_ERROR(InvalidArgument) when pos
  // exceeds the haystack BYTE size — not the code-point count.
  uint32_t out = MakeOut();
  cel_string_index_of_at_vvv(out, MakeStr("tacocat"), MakeStr("a"),
                             MakeInt(100));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, IndexOfPosNegativeClamps) {
  // cel-cpp `IndexOf(string, pos)` clamps negative pos to 0.  The
  // pre-flight `IndexOf3` byte-bound check passes for negative pos
  // (it only rejects pos > byte_size, not pos < 0).
  uint32_t out = MakeOut();
  cel_string_index_of_at_vvv(out, MakeStr("abc"), MakeStr("a"), MakeInt(-5));
  ExpectInt(out, 0);
}

TEST_F(StringExtFixture, IndexOfEnvelopeBinary) {
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeError(), MakeStr("a"));
  ExpectKind(out, CEL_ERROR);
}

TEST_F(StringExtFixture, IndexOfKindMismatch) {
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeInt(1), MakeStr("a"));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

// ===============================================================
// lastIndexOf — conformance fixture rows.
// ===============================================================

TEST_F(StringExtFixture, LastIndexOfEmptyHaystack) {
  // Spec: `''.lastIndexOf('@@') == -1`.
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vv(out, MakeStr(""), MakeStr("@@"));
  ExpectInt(out, -1);
}

TEST_F(StringExtFixture, LastIndexOfEmptyNeedleIsSize) {
  // Spec: `'tacocat'.lastIndexOf('') == 7`.  Empty needle matches at
  // every code-point boundary including the trailing one.
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vv(out, MakeStr("tacocat"), MakeStr(""));
  ExpectInt(out, 7);
}

TEST_F(StringExtFixture, LastIndexOfString) {
  // Spec: `'tacocat'.lastIndexOf('at') == 5`.
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vv(out, MakeStr("tacocat"), MakeStr("at"));
  ExpectInt(out, 5);
}

TEST_F(StringExtFixture, LastIndexOfNoMatch) {
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vv(out, MakeStr("tacocat"), MakeStr("none"));
  ExpectInt(out, -1);
}

TEST_F(StringExtFixture, LastIndexOfWithPosEmpty) {
  // Spec: `'tacocat'.lastIndexOf('', 3) == 3`.
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vvv(out, MakeStr("tacocat"), MakeStr(""),
                                  MakeInt(3));
  ExpectInt(out, 3);
}

TEST_F(StringExtFixture, LastIndexOfWithPosChar) {
  // Spec: `'tacocat'.lastIndexOf('a', 3) == 1`.  Search includes
  // code-points [0, 3]; the 'a' at cp=1 is the only match in range.
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vvv(out, MakeStr("tacocat"), MakeStr("a"),
                                  MakeInt(3));
  ExpectInt(out, 1);
}

TEST_F(StringExtFixture, LastIndexOfUnicodeChar) {
  // Spec: `'ta©o©αT'.lastIndexOf('©') == 4`.
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vv(out, MakeStr("ta©o©αT"), MakeStr("©"));
  ExpectInt(out, 4);
}

TEST_F(StringExtFixture, LastIndexOfUnicodeCharWithPos) {
  // Spec: `'ta©o©αT'.lastIndexOf('©', 3) == 2`.
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vvv(out, MakeStr("ta©o©αT"), MakeStr("©"),
                                  MakeInt(3));
  ExpectInt(out, 2);
}

TEST_F(StringExtFixture, LastIndexOfRepeatedString) {
  // Spec: `'bananananana'.lastIndexOf('nana', 7) == 6`.  Overlapping
  // 'nana' matches at cp 2,4,6 — search bound 7 caps to the cp=6
  // match.
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vvv(out, MakeStr("bananananana"), MakeStr("nana"),
                                  MakeInt(7));
  ExpectInt(out, 6);
}

TEST_F(StringExtFixture, LastIndexOfNegativePosErrors) {
  // cel-cpp `LastIndexOf3` rejects negative pos (unlike `IndexOf3`
  // which clamps).
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vvv(out, MakeStr("abc"), MakeStr("a"),
                                  MakeInt(-1));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, LastIndexOfPosBeyondByteSizeErrors) {
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vvv(out, MakeStr("abc"), MakeStr("a"),
                                  MakeInt(100));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

// ===============================================================
// substring — conformance fixture rows.
// ===============================================================

TEST_F(StringExtFixture, SubstringStart) {
  // Spec: `'tacocat'.substring(4) == 'cat'`.
  uint32_t out = MakeOut();
  cel_string_substring_at_vv(out, MakeStr("tacocat"), MakeInt(4));
  ExpectStr(out, "cat");
}

TEST_F(StringExtFixture, SubstringStartAtSize) {
  // Spec: `'tacocat'.substring(7) == ''`.  Start == codepoint_count
  // is the canonical empty-suffix sentinel.
  uint32_t out = MakeOut();
  cel_string_substring_at_vv(out, MakeStr("tacocat"), MakeInt(7));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, SubstringStartZeroIsIdentity) {
  uint32_t out = MakeOut();
  cel_string_substring_at_vv(out, MakeStr("tacocat"), MakeInt(0));
  ExpectStr(out, "tacocat");
}

TEST_F(StringExtFixture, SubstringStartNegativeErrors) {
  uint32_t out = MakeOut();
  cel_string_substring_at_vv(out, MakeStr("abc"), MakeInt(-1));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, SubstringStartPastEndErrors) {
  uint32_t out = MakeOut();
  cel_string_substring_at_vv(out, MakeStr("abc"), MakeInt(10));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, SubstringStartAndEnd) {
  // Spec: `'tacocat'.substring(0, 4) == 'taco'`.
  uint32_t out = MakeOut();
  cel_string_substring_range_at_vvv(out, MakeStr("tacocat"), MakeInt(0),
                                    MakeInt(4));
  ExpectStr(out, "taco");
}

TEST_F(StringExtFixture, SubstringStartAndEndEqual) {
  // Spec: `'tacocat'.substring(4, 4) == ''`.
  uint32_t out = MakeOut();
  cel_string_substring_range_at_vvv(out, MakeStr("tacocat"), MakeInt(4),
                                    MakeInt(4));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, SubstringUnicode) {
  // Spec: `'ta©o©αT'.substring(2, 6) == '©o©α'`.  Walks code-points,
  // resolves byte boundaries — 2 → byte 2, 6 → byte 9.
  uint32_t out = MakeOut();
  cel_string_substring_range_at_vvv(out, MakeStr("ta©o©αT"), MakeInt(2),
                                    MakeInt(6));
  ExpectStr(out, "©o©α");
}

TEST_F(StringExtFixture, SubstringUnicodeAtEnd) {
  // Spec: `'ta©o©αT'.substring(7, 7) == ''`.  cp=7 == codepoint_count
  // is the canonical empty-suffix sentinel.
  uint32_t out = MakeOut();
  cel_string_substring_range_at_vvv(out, MakeStr("ta©o©αT"), MakeInt(7),
                                    MakeInt(7));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, SubstringEndBeforeStartErrors) {
  uint32_t out = MakeOut();
  cel_string_substring_range_at_vvv(out, MakeStr("abc"), MakeInt(2),
                                    MakeInt(1));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, SubstringNegativeStartErrors) {
  uint32_t out = MakeOut();
  cel_string_substring_range_at_vvv(out, MakeStr("abc"), MakeInt(-1),
                                    MakeInt(2));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, SubstringEndPastSizeErrors) {
  uint32_t out = MakeOut();
  cel_string_substring_range_at_vvv(out, MakeStr("abc"), MakeInt(0),
                                    MakeInt(100));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

// ===============================================================
// replace — conformance fixture rows + boundary matrix.
// ===============================================================

TEST_F(StringExtFixture, ReplaceNoMatch) {
  // Spec: `'12 days 12 hours'.replace('{0}', '2') == '12 days 12 hours'`.
  uint32_t out = MakeOut();
  cel_string_replace_at_vvv(out, MakeStr("12 days 12 hours"), MakeStr("{0}"),
                            MakeStr("2"));
  ExpectStr(out, "12 days 12 hours");
}

TEST_F(StringExtFixture, ReplaceAll) {
  // Spec: `'{0} days {0} hours'.replace('{0}', '2') == '2 days 2 hours'`.
  uint32_t out = MakeOut();
  cel_string_replace_at_vvv(out, MakeStr("{0} days {0} hours"), MakeStr("{0}"),
                            MakeStr("2"));
  ExpectStr(out, "2 days 2 hours");
}

TEST_F(StringExtFixture, ReplaceNLimitsCount) {
  // Spec sub-case of `chained`: `'{0} days {0} hours'.replace('{0}', '2', 1)
  // == '2 days {0} hours'`.
  uint32_t out = MakeOut();
  cel_string_replace_n_at_vvvv(out, MakeStr("{0} days {0} hours"),
                               MakeStr("{0}"), MakeStr("2"), MakeInt(1));
  ExpectStr(out, "2 days {0} hours");
}

TEST_F(StringExtFixture, ReplaceUnicode) {
  // Spec: `'1 ©αT taco'.replace('αT', 'o©α') == '1 ©o©α taco'`.
  uint32_t out = MakeOut();
  cel_string_replace_at_vvv(out, MakeStr("1 ©αT taco"), MakeStr("αT"),
                            MakeStr("o©α"));
  ExpectStr(out, "1 ©o©α taco");
}

TEST_F(StringExtFixture, ReplaceZeroLimitReturnsOriginal) {
  // cel-cpp: limit==0 → return original string verbatim.
  uint32_t out = MakeOut();
  cel_string_replace_n_at_vvvv(out, MakeStr("aaa"), MakeStr("a"), MakeStr("X"),
                               MakeInt(0));
  ExpectStr(out, "aaa");
}

TEST_F(StringExtFixture, ReplaceNegativeLimitReplacesAll) {
  // cel-cpp: limit<0 → unlimited replacements (treated as INT64_MAX).
  uint32_t out = MakeOut();
  cel_string_replace_n_at_vvvv(out, MakeStr("aaa"), MakeStr("a"), MakeStr("X"),
                               MakeInt(-1));
  ExpectStr(out, "XXX");
}

TEST_F(StringExtFixture, ReplaceEmptyNeedleInterleaves) {
  // cel-cpp empty-needle path inserts `replacement` BEFORE each code-
  // point + once after the last; with limit=2 we get 2 insertions.
  // For "abc" with limit=2: X + 'a' + X + 'b' + 'c' = "XaXbc".
  uint32_t out = MakeOut();
  cel_string_replace_n_at_vvvv(out, MakeStr("abc"), MakeStr(""), MakeStr("X"),
                               MakeInt(2));
  ExpectStr(out, "XaXbc");
}

TEST_F(StringExtFixture, ReplaceEmptyNeedleUnboundedAppendsTrailing) {
  // Empty needle + unbounded limit: interleaves before each cp AND
  // appends one trailing replacement (cel-cpp's "if limit > 0 after
  // walk" arm).  "ab" → X + 'a' + X + 'b' + X = "XaXbX".
  uint32_t out = MakeOut();
  cel_string_replace_at_vvv(out, MakeStr("ab"), MakeStr(""), MakeStr("X"));
  ExpectStr(out, "XaXbX");
}

TEST_F(StringExtFixture, ReplaceEmptyNeedleEmptyInput) {
  // Empty input + empty needle + unbounded limit: one trailing
  // replacement.
  uint32_t out = MakeOut();
  cel_string_replace_at_vvv(out, MakeStr(""), MakeStr(""), MakeStr("X"));
  ExpectStr(out, "X");
}

TEST_F(StringExtFixture, ReplaceEnvelopeAbsorbsError) {
  uint32_t out = MakeOut();
  cel_string_replace_at_vvv(out, MakeStr("aaa"), MakeError(), MakeStr("X"));
  ExpectKind(out, CEL_ERROR);
}

TEST_F(StringExtFixture, ReplaceKindMismatch) {
  uint32_t out = MakeOut();
  cel_string_replace_at_vvv(out, MakeStr("aaa"), MakeStr("a"), MakeInt(1));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(StringExtFixture, ReplaceNKindMismatchOnN) {
  uint32_t out = MakeOut();
  cel_string_replace_n_at_vvvv(out, MakeStr("a"), MakeStr("a"), MakeStr("b"),
                               MakeStr("1"));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}


// ── anchor-scan boundary matrix ──────────────────────────────────────
//
// IndexOf/LastIndexOf anchor on the needle's first byte
// (`cel_anchor_memchr_`) and bulk-advance code-point counts across
// pure-ASCII blocks; these cases pin the edges that byte-at-a-time
// walks never stressed.

// The skipped span before the match is > one 16-byte block of pure
// ASCII — exercises the bulk code-point advance plus the sub-block
// remainder, and the returned index must still be the code-point
// count.
TEST_F(StringExtFixture, IndexOfAfterLongAsciiRun) {
  const std::string hay = std::string(40, 'x') + "NDL";
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr(hay.c_str()), MakeStr("NDL"));
  ExpectInt(out, 40);
}

// Multi-byte code points before the match: the code-point index (3)
// diverges from the byte offset (6); the scalar decode leg of the
// advance must count sequences, not bytes.
TEST_F(StringExtFixture, IndexOfAfterMultibyteRun) {
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("\xC2\xA9\xC2\xA9\xC2\xA9NDL"),
                            MakeStr("NDL"));
  ExpectInt(out, 3);
}

// The needle's first byte occurs ONLY as the continuation byte of a
// multi-byte sequence (0xA9 inside U+00A9 = C2 A9).  A byte-blind
// search would "find" it; the decode walk never visits non-boundary
// offsets, so the result must be -1.
TEST_F(StringExtFixture, IndexOfAnchorInsideMultibyteSequenceIsNotAMatch) {
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("\xC2\xA9Z"), MakeStr("\xA9Z"));
  ExpectInt(out, -1);
}

// Malformed input parity: a stray continuation byte (0x80 with no
// lead) advances the decode walk by ONE byte and counts as ONE code
// point — the accelerated walk must agree with the plain one.
TEST_F(StringExtFixture, IndexOfStrayContinuationCountsAsOneCodePoint) {
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("\x80NDL"), MakeStr("NDL"));
  ExpectInt(out, 1);
}

// Anchor-dense haystack: every byte matches the needle's first byte,
// so every position is a candidate; only the true match position
// comes back.
TEST_F(StringExtFixture, IndexOfDenseFalseAnchors) {
  uint32_t out = MakeOut();
  cel_string_index_of_at_vv(out, MakeStr("aaaaaaaaaaaaaaaaaaab"),
                            MakeStr("ab"));
  ExpectInt(out, 18);
}

// LastIndexOf across multi-byte spans: three matches; the last one
// sits past a 2-byte code point, so its code-point index (9) is not
// its byte offset (10).
TEST_F(StringExtFixture, LastIndexOfAfterMultibyte) {
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vv(out, MakeStr("NDLxxNDL\xC2\xA9NDL"),
                                 MakeStr("NDL"));
  ExpectInt(out, 9);
}

// Bounded LastIndexOf: matches exist at code points 0, 5, and 9; a
// pos limit of 5 must return the match AT 5 (inclusive), and a limit
// of 4 must fall back to the match at 0.
TEST_F(StringExtFixture, LastIndexOfBoundedPosInclusive) {
  uint32_t out = MakeOut();
  cel_string_last_index_of_at_vvv(out, MakeStr("NDLxxNDL\xC2\xA9NDL"),
                                  MakeStr("NDL"), MakeInt(5));
  ExpectInt(out, 5);
  out = MakeOut();
  cel_string_last_index_of_at_vvv(out, MakeStr("NDLxxNDL\xC2\xA9NDL"),
                                  MakeStr("NDL"), MakeInt(4));
  ExpectInt(out, 0);
}

}  // namespace
}  // namespace celwasm
