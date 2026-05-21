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

#include "compiler_v2/runtime/cel_string_format_internal.h"

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
