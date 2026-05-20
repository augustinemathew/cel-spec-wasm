// M12 Slice A — unit coverage for `cel_string_ext.h` runtime kernels.
//
// Test discipline mirrors `cel_matches_test.cc` /
// `cel_string_ops_test.cc`: focused TEST_F coverage for the 3VL +
// kind-mismatch envelope; parameterised TEST_P over the conformance-
// shape admit set (the rows in
// `tests/simple/testdata/string_ext.textproto` for the same
// function).
//
// CEL spec citation is in the per-test comment block where the
// behaviour is spec-mandated (`langdef.md §"String / bytes"`,
// `cel-cpp StringValue::CharAt`, …).

#include "compiler_v2/runtime/cel_string_ext.h"

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

class StringExtFixture : public ::testing::Test {
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

  uint32_t MakeInt(int64_t i) {
    uint32_t slot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_INT;
    v->payload.i = i;
    return slot;
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

  const CelValue* At(uint32_t slot) {
    return cel_value_at(slot);
  }

  std::string StringAt(uint32_t slot) {
    const CelValue* v = At(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
    if (v->payload.s.len == 0) return {};
    return {reinterpret_cast<const char*>(cel_mem_base() + v->payload.s.ptr),
            v->payload.s.len};
  }

  void ExpectStr(uint32_t slot, const std::string& expected) {
    EXPECT_EQ(StringAt(slot), expected);
  }

  void ExpectError(uint32_t slot, uint32_t err) {
    const CelValue* v = At(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
    EXPECT_EQ(v->payload.err, err);
  }

  void ExpectKind(uint32_t slot, uint32_t kind) {
    EXPECT_EQ(At(slot)->kind, kind);
  }
};

// ===============================================================
// 3VL + kind-mismatch envelope — shared across all 5 kernels.
// Each kernel is one helper per slot.
// ===============================================================

TEST_F(StringExtFixture, CharAtAbsorbsErrorOnString) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeError(), MakeInt(0));
  ExpectKind(out, CEL_ERROR);
}

TEST_F(StringExtFixture, CharAtAbsorbsErrorOnIndex) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("abc"), MakeError());
  ExpectKind(out, CEL_ERROR);
}

TEST_F(StringExtFixture, CharAtAbsorbsUnknownOnString) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeUnknown(), MakeInt(0));
  ExpectKind(out, CEL_UNKNOWN);
}

TEST_F(StringExtFixture, CharAtAbsorbsUnknownOnIndex) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("abc"), MakeUnknown());
  ExpectKind(out, CEL_UNKNOWN);
}

TEST_F(StringExtFixture, CharAtKindMismatchStringNotString) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeInt(1), MakeInt(0));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(StringExtFixture, CharAtKindMismatchIndexNotInt) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("abc"), MakeStr("0"));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

// Unary kernels — 3VL + kind-mismatch are identical across the 4
// `_at_v` shapes (lowerAscii / upperAscii / trim / reverse).
// Parameterise so the matrix is explicit rather than copied 4x.
using UnaryKernel = void (*)(uint32_t, uint32_t);

struct UnaryEnvelopeRow {
  const char* label;
  UnaryKernel fn;
};

class UnaryEnvelopeTable
    : public StringExtFixture,
      public ::testing::WithParamInterface<UnaryEnvelopeRow> {};

TEST_P(UnaryEnvelopeTable, AbsorbsError) {
  uint32_t out = MakeOut();
  GetParam().fn(out, MakeError());
  ExpectKind(out, CEL_ERROR);
}

TEST_P(UnaryEnvelopeTable, AbsorbsUnknown) {
  uint32_t out = MakeOut();
  GetParam().fn(out, MakeUnknown());
  ExpectKind(out, CEL_UNKNOWN);
}

TEST_P(UnaryEnvelopeTable, KindMismatchNotString) {
  uint32_t out = MakeOut();
  GetParam().fn(out, MakeInt(1));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

INSTANTIATE_TEST_SUITE_P(
    AllUnaryKernels, UnaryEnvelopeTable,
    ::testing::Values(
        UnaryEnvelopeRow{"lower_ascii", cel_string_lower_ascii_at_v},
        UnaryEnvelopeRow{"upper_ascii", cel_string_upper_ascii_at_v},
        UnaryEnvelopeRow{"trim", cel_string_trim_at_v},
        UnaryEnvelopeRow{"reverse", cel_string_reverse_at_v}),
    [](const ::testing::TestParamInfo<UnaryEnvelopeRow>& info) {
      return std::string(info.param.label);
    });

// ===============================================================
// charAt — boundary + Unicode matrix.  Spec rows live in
// `string_ext.textproto::char_at`.
// ===============================================================

TEST_F(StringExtFixture, CharAtMiddleAscii) {
  // string_ext.textproto::char_at/middle_index.  cel-cpp returns
  // the code-point at index 3 of "tacocat" → "o".
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("tacocat"), MakeInt(3));
  ExpectStr(out, "o");
}

TEST_F(StringExtFixture, CharAtEndIndexReturnsEmpty) {
  // string_ext.textproto::char_at/end_index.  `i == size()` is the
  // canonical end-of-string sentinel — empty string, not error
  // (StringValue::CharAt line ~1483).
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("tacocat"), MakeInt(7));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, CharAtPastEndErrors) {
  // `i > size()` is `InvalidArgumentError` per StringValue::CharAt.
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("tacocat"), MakeInt(8));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, CharAtNegativeErrors) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("abc"), MakeInt(-1));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, CharAtIntMaxErrors) {
  // INT64_MAX is well past any reasonable string size — must error.
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("abc"),
                           MakeInt(static_cast<int64_t>(INT64_MAX)));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(StringExtFixture, CharAtMultiByteCodePoints) {
  // string_ext.textproto::char_at/multiple.  "©αT" is 3 code points
  // with byte sizes (2, 2, 1).  Each charAt returns the full UTF-8
  // sequence for that code point.
  const char* s = "©αT";  // 0xC2 0xA9 0xCE 0xB1 0x54
  uint32_t out0 = MakeOut();
  cel_string_char_at_at_vv(out0, MakeStr(s), MakeInt(0));
  ExpectStr(out0, "©");
  uint32_t out1 = MakeOut();
  cel_string_char_at_at_vv(out1, MakeStr(s), MakeInt(1));
  ExpectStr(out1, "α");
  uint32_t out2 = MakeOut();
  cel_string_char_at_at_vv(out2, MakeStr(s), MakeInt(2));
  ExpectStr(out2, "T");
}

TEST_F(StringExtFixture, CharAtSmpFourByte) {
  // U+1F600 GRINNING FACE encodes as F0 9F 98 80 (4 bytes / 1 code
  // point).  Locking the 4-byte arm of the UTF-8 decoder.
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr("\xF0\x9F\x98\x80"), MakeInt(0));
  ExpectStr(out, "\xF0\x9F\x98\x80");
}

TEST_F(StringExtFixture, CharAtEmptyStringPosZeroIsEmpty) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr(""), MakeInt(0));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, CharAtEmptyStringPosOneErrors) {
  uint32_t out = MakeOut();
  cel_string_char_at_at_vv(out, MakeStr(""), MakeInt(1));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

// ===============================================================
// lowerAscii / upperAscii — fold matrix.
// ===============================================================

TEST_F(StringExtFixture, LowerAsciiMixed) {
  uint32_t out = MakeOut();
  cel_string_lower_ascii_at_v(out, MakeStr("TacoCat"));
  ExpectStr(out, "tacocat");
}

TEST_F(StringExtFixture, LowerAsciiAlreadyLowerReturnsSame) {
  uint32_t out = MakeOut();
  cel_string_lower_ascii_at_v(out, MakeStr("tacocat"));
  ExpectStr(out, "tacocat");
}

TEST_F(StringExtFixture, LowerAsciiEmpty) {
  uint32_t out = MakeOut();
  cel_string_lower_ascii_at_v(out, MakeStr(""));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, LowerAsciiNonAsciiPassesThrough) {
  // Per spec: ASCII-only fold; non-ASCII bytes pass through
  // verbatim.  cel-cpp's `LowerAsciiImpl` uses `absl::ascii_tolower`
  // which is byte-level and only affects 'A'..'Z'.
  uint32_t out = MakeOut();
  cel_string_lower_ascii_at_v(out, MakeStr("ABCñΩ"));
  ExpectStr(out, "abcñΩ");
}

TEST_F(StringExtFixture, UpperAsciiMixed) {
  uint32_t out = MakeOut();
  cel_string_upper_ascii_at_v(out, MakeStr("TacoCat"));
  ExpectStr(out, "TACOCAT");
}

TEST_F(StringExtFixture, UpperAsciiAlreadyUpperReturnsSame) {
  uint32_t out = MakeOut();
  cel_string_upper_ascii_at_v(out, MakeStr("TACOCAT"));
  ExpectStr(out, "TACOCAT");
}

TEST_F(StringExtFixture, UpperAsciiNonAsciiPassesThrough) {
  uint32_t out = MakeOut();
  cel_string_upper_ascii_at_v(out, MakeStr("abcñΩ"));
  ExpectStr(out, "ABCñΩ");
}

TEST_F(StringExtFixture, UpperAsciiEmbeddedNul) {
  // Embedded NUL must survive — strings are byte arrays, not C
  // strings.  Locks a class of bug where a fold helper uses
  // strlen() instead of the span length.
  const char raw[] = {'a', '\0', 'b'};
  uint32_t out = MakeOut();
  cel_string_upper_ascii_at_v(out, MakeStrLen(raw, 3));
  EXPECT_EQ(StringAt(out), std::string(std::string("A\0B", 3)));
}

// ===============================================================
// trim — Unicode whitespace matrix.  Spec rows live in
// `string_ext.textproto::trim`.
// ===============================================================

TEST_F(StringExtFixture, TrimAsciiBlanks) {
  // string_ext.textproto::trim/blank_spaces_escaped_chars:
  // " \f\n\r\t\vtext  " → "text".
  uint32_t out = MakeOut();
  cel_string_trim_at_v(out, MakeStr(" \f\n\r\t\vtext  "));
  ExpectStr(out, "text");
}

TEST_F(StringExtFixture, TrimUnicodeLeading) {
  // string_ext.textproto::trim/unicode_space_chars_1:
  // U+0085 + U+00A0 + U+1680 + "text" → "text".
  uint32_t out = MakeOut();
  cel_string_trim_at_v(out, MakeStr("\xC2\x85\xC2\xA0\xE1\x9A\x80text"));
  ExpectStr(out, "text");
}

TEST_F(StringExtFixture, TrimUnicodeTrailing) {
  // string_ext.textproto::trim/unicode_space_chars_2:
  // "text" + U+2000..U+2009 → "text".  Truncated to one
  // representative trailing run.
  uint32_t out = MakeOut();
  cel_string_trim_at_v(out, MakeStr("text\xE2\x80\x80\xE2\x80\x81"));
  ExpectStr(out, "text");
}

TEST_F(StringExtFixture, TrimUnicodeNotInWhitespaceSetPasses) {
  // string_ext.textproto::trim/unicode_no_trim.  U+180E and U+200B
  // are NOT in cel-cpp's whitespace set; they pass through.
  uint32_t out = MakeOut();
  cel_string_trim_at_v(out, MakeStr("\xE1\xA0\x8Etext\xE2\x80\x8B"));
  ExpectStr(out, "\xE1\xA0\x8Etext\xE2\x80\x8B");
}

TEST_F(StringExtFixture, TrimAllWhitespaceProducesEmpty) {
  uint32_t out = MakeOut();
  cel_string_trim_at_v(out, MakeStr("   \t\n  "));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, TrimEmptyInput) {
  uint32_t out = MakeOut();
  cel_string_trim_at_v(out, MakeStr(""));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, TrimNoChangeReturnsOriginal) {
  uint32_t out = MakeOut();
  cel_string_trim_at_v(out, MakeStr("text"));
  ExpectStr(out, "text");
}

TEST_F(StringExtFixture, TrimSingleCharNonWhitespace) {
  uint32_t out = MakeOut();
  cel_string_trim_at_v(out, MakeStr("a"));
  ExpectStr(out, "a");
}

// Parameterised matrix over every code point cel-cpp considers
// whitespace.  Each row pads the marker `X` on both sides with one
// instance of the code point and asserts trim narrows back to "X".
// Lifted verbatim from `IsUnicodeWhitespace` in
// `common/values/string_value.cc`.
struct WsCharRow {
  const char* label;
  const char* utf8;  // 1-3 byte UTF-8 encoding.
};

class TrimWhitespaceTable : public StringExtFixture,
                            public ::testing::WithParamInterface<WsCharRow> {};

TEST_P(TrimWhitespaceTable, NarrowsToCore) {
  const WsCharRow& row = GetParam();
  std::string padded = std::string(row.utf8) + "X" + row.utf8;
  uint32_t out = MakeOut();
  cel_string_trim_at_v(
      out, MakeStrLen(padded.data(), static_cast<uint32_t>(padded.size())));
  ExpectStr(out, "X");
}

INSTANTIATE_TEST_SUITE_P(
    EveryWhitespaceCodePoint, TrimWhitespaceTable,
    ::testing::Values(WsCharRow{"space", " "}, WsCharRow{"tab", "\t"},
                      WsCharRow{"lf", "\n"}, WsCharRow{"vt", "\v"},
                      WsCharRow{"ff", "\f"}, WsCharRow{"cr", "\r"},
                      WsCharRow{"u0085_nel", "\xC2\x85"},
                      WsCharRow{"u00a0_nbsp", "\xC2\xA0"},
                      WsCharRow{"u1680_ogham", "\xE1\x9A\x80"},
                      WsCharRow{"u2000_en_quad", "\xE2\x80\x80"},
                      WsCharRow{"u200a_hair", "\xE2\x80\x8A"},
                      WsCharRow{"u2028_line_sep", "\xE2\x80\xA8"},
                      WsCharRow{"u2029_para_sep", "\xE2\x80\xA9"},
                      WsCharRow{"u202f_nnbsp", "\xE2\x80\xAF"},
                      WsCharRow{"u205f_mmsp", "\xE2\x81\x9F"},
                      WsCharRow{"u3000_ideographic", "\xE3\x80\x80"}),
    [](const ::testing::TestParamInfo<WsCharRow>& info) {
      return std::string(info.param.label);
    });

// ===============================================================
// reverse — code-point reversal matrix.  Spec rows live in
// `string_ext.textproto::reverse`.
// ===============================================================

TEST_F(StringExtFixture, ReverseEmpty) {
  uint32_t out = MakeOut();
  cel_string_reverse_at_v(out, MakeStr(""));
  ExpectStr(out, "");
}

TEST_F(StringExtFixture, ReverseAscii) {
  uint32_t out = MakeOut();
  cel_string_reverse_at_v(out, MakeStr("abcdef"));
  ExpectStr(out, "fedcba");
}

TEST_F(StringExtFixture, ReversePalindrome) {
  // "tacocat" reversed is itself — locks-in that we copy bytes
  // forward rather than reflecting them with a stride bug.
  uint32_t out = MakeOut();
  cel_string_reverse_at_v(out, MakeStr("tacocat"));
  ExpectStr(out, "tacocat");
}

TEST_F(StringExtFixture, ReverseTwoByte) {
  // "©α" (2 + 2 bytes) reversed should be "α©" (2 + 2 bytes), NOT
  // byte-reversed garbage.
  uint32_t out = MakeOut();
  cel_string_reverse_at_v(out, MakeStr("©α"));
  ExpectStr(out, "α©");
}

TEST_F(StringExtFixture, ReverseMixedWidth) {
  // Mixed 1/2/3-byte code points: "a©αT" (1+2+2+1).  Reversed:
  // "Tα©a".  Spec citation: cel-cpp Reverse iterates code points.
  uint32_t out = MakeOut();
  cel_string_reverse_at_v(out, MakeStr("a©αT"));
  ExpectStr(out, "Tα©a");
}

TEST_F(StringExtFixture, ReverseSmpFourByte) {
  // Two SMP code points (4 bytes each).  Reversed.
  uint32_t out = MakeOut();
  cel_string_reverse_at_v(out, MakeStr("\xF0\x9F\x90\xB1\xF0\x9F\x98\x80"));
  ExpectStr(out, "\xF0\x9F\x98\x80\xF0\x9F\x90\xB1");
}

TEST_F(StringExtFixture, ReverseSingleAsciiChar) {
  uint32_t out = MakeOut();
  cel_string_reverse_at_v(out, MakeStr("x"));
  ExpectStr(out, "x");
}

TEST_F(StringExtFixture, ReverseEmbeddedNul) {
  const char raw[] = {'a', '\0', 'b'};
  uint32_t out = MakeOut();
  cel_string_reverse_at_v(out, MakeStrLen(raw, 3));
  EXPECT_EQ(StringAt(out), std::string("b\0a", 3));
}

}  // namespace
}  // namespace celwasm
