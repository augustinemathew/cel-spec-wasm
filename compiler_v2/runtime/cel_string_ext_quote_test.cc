// M12 Slice D — unit tests for `cel_string_quote_at_v`.  Covers
// every escape sequence in cel-cpp's `AppendQuoteCodePoint` switch
// plus verbatim ASCII / multi-byte UTF-8 / 3VL envelope / kind-
// mismatch.  Conformance fixture rows are taken verbatim from
// `tests/simple/testdata/string_ext.textproto::quote`.

#include <cstring>
#include <string>

#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_string_ext.h"
#include "compiler_v2/runtime/string_ext_test_helpers.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class QuoteTest : public StringExtFixture {};

// ───────────────────────────────────────────────────────────────
// Named-escape matrix — one row per cel-cpp `AppendQuoteCodePoint`
// case.  Verifies the two-byte `\<c>` form is emitted exactly.
// ───────────────────────────────────────────────────────────────

TEST_F(QuoteTest, EscapesBell) {
  const uint32_t in = MakeStrLen("bell\a", 5);
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"bell\\a\"");
}

TEST_F(QuoteTest, EscapesBackspace) {
  const uint32_t in = MakeStrLen("\bbackspace", 10);
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"\\bbackspace\"");
}

TEST_F(QuoteTest, EscapesFormFeed) {
  const uint32_t in = MakeStrLen("\fform feed", 10);
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"\\fform feed\"");
}

TEST_F(QuoteTest, EscapesNewline) {
  const uint32_t in = MakeStr("first\nsecond");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"first\\nsecond\"");
}

TEST_F(QuoteTest, EscapesCarriageReturn) {
  const uint32_t in = MakeStr("carriage \r return");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"carriage \\r return\"");
}

TEST_F(QuoteTest, EscapesHorizontalTab) {
  const uint32_t in = MakeStr("horizontal tab\t");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"horizontal tab\\t\"");
}

TEST_F(QuoteTest, EscapesVerticalTab) {
  const uint32_t in = MakeStr("vertical \v tab");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"vertical \\v tab\"");
}

TEST_F(QuoteTest, EscapesBackslash) {
  // Source literal: `double \\ slash` (4 bytes for `\\`); expected
  // output doubles every backslash, so 8 backslashes round-trip.
  const uint32_t in = MakeStr("double \\\\ slash");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"double \\\\\\\\ slash\"");
}

TEST_F(QuoteTest, EscapesDoubleQuote) {
  const uint32_t in = MakeStr("mid string \" quote");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"mid string \\\" quote\"");
}

// ───────────────────────────────────────────────────────────────
// Verbatim — no escapes needed.  Output is `"<input>"`.
// ───────────────────────────────────────────────────────────────

TEST_F(QuoteTest, VerbatimAscii) {
  const uint32_t in = MakeStr("verbatim");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"verbatim\"");
}

TEST_F(QuoteTest, VerbatimEmpty) {
  const uint32_t in = MakeStr("");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"\"");
}

TEST_F(QuoteTest, VerbatimSingleSpace) {
  const uint32_t in = MakeStr(" ");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\" \"");
}

// Multi-byte UTF-8: 2-byte (ÿ = C3 BF), 3-byte (α = CE B1), 4-byte
// (🐱 = F0 9F 90 B1).  Each passes through verbatim — cel-cpp's
// `AppendQuoteCodePoint` default arm Utf8-encodes the code point,
// which for any ≥ 0x80 code point round-trips the original bytes.
TEST_F(QuoteTest, Verbatim2ByteUtf8) {
  const uint32_t in = MakeStr("ÿ");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"ÿ\"");
}

TEST_F(QuoteTest, Verbatim3ByteUtf8) {
  const uint32_t in = MakeStr("πέντε");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"πέντε\"");
}

TEST_F(QuoteTest, Verbatim4ByteUtf8) {
  const uint32_t in = MakeStr("printable unicode😀");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"printable unicode😀\"");
}

TEST_F(QuoteTest, MixedUnicode) {
  const uint32_t in = MakeStr("ta©o©αT");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"ta©o©αT\"");
}

// Boundary: backslash at start / end.  Conformance rows
// `starts_with` / `ends_with`.
TEST_F(QuoteTest, EscapesBackslashAtStart) {
  const uint32_t in = MakeStr("\\ starts with");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"\\\\ starts with\"");
}

TEST_F(QuoteTest, EscapesBackslashAtEnd) {
  const uint32_t in = MakeStr("ends with \\");
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectStr(out, "\"ends with \\\\\"");
}

// `cel-cpp`'s `AppendQuoteCodePoint` does NOT special-case NUL
// (`\0`); the default arm passes through the raw byte.  Conformance
// fixture doesn't exercise this directly but we lock the behavior
// against any drift.
TEST_F(QuoteTest, VerbatimEmbeddedNul) {
  const char src[] = {'a', '\0', 'b'};
  const uint32_t in = MakeStrLen(src, 3);
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  const CelValue* v = At(out);
  ASSERT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
  ASSERT_EQ(v->payload.s.len, 5u);
  const char* p = reinterpret_cast<const char*>(cel_mem_base()) +
                  v->payload.s.ptr;
  EXPECT_EQ(p[0], '"');
  EXPECT_EQ(p[1], 'a');
  EXPECT_EQ(p[2], '\0');
  EXPECT_EQ(p[3], 'b');
  EXPECT_EQ(p[4], '"');
}

// ───────────────────────────────────────────────────────────────
// Envelope: 3VL absorb + kind-mismatch.
// ───────────────────────────────────────────────────────────────

TEST_F(QuoteTest, AbsorbError) {
  const uint32_t in = MakeError();
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(QuoteTest, AbsorbUnknown) {
  const uint32_t in = MakeUnknown();
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectKind(out, CEL_UNKNOWN);
}

TEST_F(QuoteTest, KindMismatchInt) {
  const uint32_t in = MakeInt(42);
  const uint32_t out = MakeOut();
  cel_string_quote_at_v(out, in);
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

}  // namespace
}  // namespace celwasm
