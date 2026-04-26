#include "compiler_v2/runtime/cel_string_ops.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

// M5.C string + bytes op coverage.  Exercises every helper at the
// happy path, the empty-operand boundary (edge case for substring
// scans), the multi-byte UTF-8 boundary (langdef pins size to byte
// count, NOT code-point count), and the type-mismatch / 3VL
// envelope shared with cel_arith / cel_compare.
//
// The duplication lives WITHIN a helper, not across helpers —
// `contains` / `startsWith` / `endsWith` have distinct semantics
// (full-scan vs prefix vs suffix), so each gets its own
// INSTANTIATE_TEST_SUITE_P table even though the parameter type
// is shared.  Spec-citation rows (UTF-8 byte count, embedded-NUL
// bytes equality, unsigned byte ordering for `bytes`, byte-walk
// verification for concat) stay as focused TEST_F.

namespace celwasm {
namespace {

class StringOpsTest : public ::testing::Test {
 public:  // public so parameterized lambdas can use the helpers.
  void SetUp() override {
    cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  }

  uint32_t MakeOut() {
    return cel_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  const CelValue* At(uint32_t slot) {
    return cel_value_at(slot);
  }

  std::string ReadString(uint32_t slot) {
    EXPECT_EQ(At(slot)->kind, static_cast<uint32_t>(CEL_STRING));
    const CelSpan s = At(slot)->payload.s;
    return std::string(reinterpret_cast<const char*>(cel_mem_base() + s.ptr),
                       s.len);
  }

  bool ReadBool(uint32_t slot) {
    EXPECT_EQ(At(slot)->kind, static_cast<uint32_t>(CEL_BOOL));
    return At(slot)->payload.b != 0;
  }
};

// ── Parameterized: binary string-helpers → bool ───────────────
//
// Shared parameter shape for the helpers that take two string
// operands and produce a bool: contains, startsWith, endsWith,
// string_eq, string_lt.  Each helper gets its own INSTANTIATE
// block so the rows read like a per-helper truth table.

struct BinaryStringBoolCase {
  const char* name;
  void (*helper)(uint32_t, uint32_t, uint32_t);
  uint32_t (*lhs)();
  uint32_t (*rhs)();
  bool expected;
};

class BinaryStringBoolTest
    : public StringOpsTest,
      public ::testing::WithParamInterface<BinaryStringBoolCase> {};

TEST_P(BinaryStringBoolTest, ProducesExpectedBool) {
  const BinaryStringBoolCase& c = GetParam();
  uint32_t out = MakeOut();
  c.helper(out, c.lhs(), c.rhs());
  EXPECT_EQ(ReadBool(out), c.expected) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Contains, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"contains_hit_in_middle",
                             cel_string_contains_at_vv,
                             +[]() {
                               return cel_make_string("abcdef", 6);
                             },
                             +[]() {
                               return cel_make_string("cd", 2);
                             },
                             true},
        BinaryStringBoolCase{"contains_miss", cel_string_contains_at_vv,
                             +[]() {
                               return cel_make_string("abc", 3);
                             },
                             +[]() {
                               return cel_make_string("z", 1);
                             },
                             false},
        // cel-cpp's StringContains follows C's str_view semantics:
        // empty is a substring of everything.
        BinaryStringBoolCase{"contains_empty_substring_in_nonempty",
                             cel_string_contains_at_vv,
                             +[]() {
                               return cel_make_string("abc", 3);
                             },
                             +[]() {
                               return cel_make_string("", 0);
                             },
                             true},
        BinaryStringBoolCase{"contains_empty_in_empty",
                             cel_string_contains_at_vv,
                             +[]() {
                               return cel_make_string("", 0);
                             },
                             +[]() {
                               return cel_make_string("", 0);
                             },
                             true},
        BinaryStringBoolCase{"contains_at_prefix", cel_string_contains_at_vv,
                             +[]() {
                               return cel_make_string("abcdef", 6);
                             },
                             +[]() {
                               return cel_make_string("ab", 2);
                             },
                             true},
        BinaryStringBoolCase{"contains_at_suffix", cel_string_contains_at_vv,
                             +[]() {
                               return cel_make_string("abcdef", 6);
                             },
                             +[]() {
                               return cel_make_string("ef", 2);
                             },
                             true},
        BinaryStringBoolCase{"contains_longer_than_hay",
                             cel_string_contains_at_vv,
                             +[]() {
                               return cel_make_string("abc", 3);
                             },
                             +[]() {
                               return cel_make_string("abcd", 4);
                             },
                             false}),
    [](const auto& info) {
      return std::string(info.param.name);
    });

INSTANTIATE_TEST_SUITE_P(
    StartsWith, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"starts_with_hit", cel_string_starts_with_at_vv,
                             +[]() {
                               return cel_make_string("abcdef", 6);
                             },
                             +[]() {
                               return cel_make_string("abc", 3);
                             },
                             true},
        BinaryStringBoolCase{"starts_with_miss", cel_string_starts_with_at_vv,
                             +[]() {
                               return cel_make_string("abcdef", 6);
                             },
                             +[]() {
                               return cel_make_string("xyz", 3);
                             },
                             false},
        BinaryStringBoolCase{"starts_with_prefix_longer_than_hay",
                             cel_string_starts_with_at_vv,
                             +[]() {
                               return cel_make_string("a", 1);
                             },
                             +[]() {
                               return cel_make_string("ab", 2);
                             },
                             false},
        BinaryStringBoolCase{"starts_with_empty_prefix",
                             cel_string_starts_with_at_vv,
                             +[]() {
                               return cel_make_string("abc", 3);
                             },
                             +[]() {
                               return cel_make_string("", 0);
                             },
                             true}),
    [](const auto& info) {
      return std::string(info.param.name);
    });

INSTANTIATE_TEST_SUITE_P(
    EndsWith, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"ends_with_hit", cel_string_ends_with_at_vv,
                             +[]() {
                               return cel_make_string("abcdef", 6);
                             },
                             +[]() {
                               return cel_make_string("def", 3);
                             },
                             true},
        BinaryStringBoolCase{"ends_with_miss", cel_string_ends_with_at_vv,
                             +[]() {
                               return cel_make_string("abcdef", 6);
                             },
                             +[]() {
                               return cel_make_string("ab", 2);
                             },
                             false},
        BinaryStringBoolCase{"ends_with_suffix_longer_than_hay",
                             cel_string_ends_with_at_vv,
                             +[]() {
                               return cel_make_string("a", 1);
                             },
                             +[]() {
                               return cel_make_string("ab", 2);
                             },
                             false},
        BinaryStringBoolCase{"ends_with_empty_suffix",
                             cel_string_ends_with_at_vv,
                             +[]() {
                               return cel_make_string("abc", 3);
                             },
                             +[]() {
                               return cel_make_string("", 0);
                             },
                             true}),
    [](const auto& info) {
      return std::string(info.param.name);
    });

INSTANTIATE_TEST_SUITE_P(
    StringEq, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"string_eq_same", cel_string_eq_at_vv,
                             +[]() {
                               return cel_make_string("hi", 2);
                             },
                             +[]() {
                               return cel_make_string("hi", 2);
                             },
                             true},
        // byte-comparison; case sensitive.
        BinaryStringBoolCase{"string_eq_case_difference", cel_string_eq_at_vv,
                             +[]() {
                               return cel_make_string("hi", 2);
                             },
                             +[]() {
                               return cel_make_string("HI", 2);
                             },
                             false},
        BinaryStringBoolCase{"string_eq_different_length", cel_string_eq_at_vv,
                             +[]() {
                               return cel_make_string("a", 1);
                             },
                             +[]() {
                               return cel_make_string("ab", 2);
                             },
                             false}),
    [](const auto& info) {
      return std::string(info.param.name);
    });

INSTANTIATE_TEST_SUITE_P(
    StringLt, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"string_lt_lexicographic", cel_string_lt_at_vv,
                             +[]() {
                               return cel_make_string("a", 1);
                             },
                             +[]() {
                               return cel_make_string("b", 1);
                             },
                             true},
        // shorter-equal-prefix < longer.
        BinaryStringBoolCase{"string_lt_prefix_shorter_than_longer",
                             cel_string_lt_at_vv,
                             +[]() {
                               return cel_make_string("ab", 2);
                             },
                             +[]() {
                               return cel_make_string("abc", 3);
                             },
                             true},
        BinaryStringBoolCase{"string_lt_false_when_greater",
                             cel_string_lt_at_vv,
                             +[]() {
                               return cel_make_string("b", 1);
                             },
                             +[]() {
                               return cel_make_string("a", 1);
                             },
                             false}),
    [](const auto& info) {
      return std::string(info.param.name);
    });

// ── String le / gt / ge (M5.B step 2) ─────────────────────────
//
// Each ordering tail op is `span_lt` re-expressed with the
// language identities (a<=b ↔ !(b<a); a>b ↔ b<a; a>=b ↔ !(a<b))
// — verify each direction at the equal / shorter-prefix boundary
// where the inequality flips.

INSTANTIATE_TEST_SUITE_P(
    StringLe, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"string_le_strictly_less", cel_string_le_at_vv,
                             +[]() { return cel_make_string("a", 1); },
                             +[]() { return cel_make_string("b", 1); }, true},
        BinaryStringBoolCase{"string_le_equal", cel_string_le_at_vv,
                             +[]() { return cel_make_string("ab", 2); },
                             +[]() { return cel_make_string("ab", 2); }, true},
        BinaryStringBoolCase{"string_le_strictly_greater", cel_string_le_at_vv,
                             +[]() { return cel_make_string("b", 1); },
                             +[]() { return cel_make_string("a", 1); }, false}),
    [](const auto& info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(
    StringGt, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"string_gt_strictly_greater", cel_string_gt_at_vv,
                             +[]() { return cel_make_string("b", 1); },
                             +[]() { return cel_make_string("a", 1); }, true},
        BinaryStringBoolCase{"string_gt_equal_false", cel_string_gt_at_vv,
                             +[]() { return cel_make_string("ab", 2); },
                             +[]() { return cel_make_string("ab", 2); }, false},
        BinaryStringBoolCase{"string_gt_strictly_less_false",
                             cel_string_gt_at_vv,
                             +[]() { return cel_make_string("a", 1); },
                             +[]() { return cel_make_string("b", 1); }, false}),
    [](const auto& info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(
    StringGe, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"string_ge_strictly_greater", cel_string_ge_at_vv,
                             +[]() { return cel_make_string("b", 1); },
                             +[]() { return cel_make_string("a", 1); }, true},
        BinaryStringBoolCase{"string_ge_equal", cel_string_ge_at_vv,
                             +[]() { return cel_make_string("ab", 2); },
                             +[]() { return cel_make_string("ab", 2); }, true},
        BinaryStringBoolCase{"string_ge_strictly_less_false",
                             cel_string_ge_at_vv,
                             +[]() { return cel_make_string("a", 1); },
                             +[]() { return cel_make_string("b", 1); }, false}),
    [](const auto& info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(
    BytesLe, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"bytes_le_unsigned_byte_order", cel_bytes_le_at_vv,
                             +[]() { return cel_make_bytes("\x7F", 1); },
                             +[]() { return cel_make_bytes("\x80", 1); }, true},
        BinaryStringBoolCase{"bytes_le_equal", cel_bytes_le_at_vv,
                             +[]() { return cel_make_bytes("\x01\x02", 2); },
                             +[]() { return cel_make_bytes("\x01\x02", 2); },
                             true},
        BinaryStringBoolCase{"bytes_le_strictly_greater_false",
                             cel_bytes_le_at_vv,
                             +[]() { return cel_make_bytes("\x80", 1); },
                             +[]() { return cel_make_bytes("\x7F", 1); },
                             false}),
    [](const auto& info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(
    BytesGt, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"bytes_gt_unsigned_byte_order", cel_bytes_gt_at_vv,
                             +[]() { return cel_make_bytes("\x80", 1); },
                             +[]() { return cel_make_bytes("\x7F", 1); }, true},
        BinaryStringBoolCase{"bytes_gt_equal_false", cel_bytes_gt_at_vv,
                             +[]() { return cel_make_bytes("\x01", 1); },
                             +[]() { return cel_make_bytes("\x01", 1); },
                             false},
        BinaryStringBoolCase{"bytes_gt_shorter_prefix_false",
                             cel_bytes_gt_at_vv,
                             +[]() { return cel_make_bytes("\x01", 1); },
                             +[]() { return cel_make_bytes("\x01\x02", 2); },
                             false}),
    [](const auto& info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(
    BytesGe, BinaryStringBoolTest,
    ::testing::Values(
        BinaryStringBoolCase{"bytes_ge_strictly_greater", cel_bytes_ge_at_vv,
                             +[]() { return cel_make_bytes("\x80", 1); },
                             +[]() { return cel_make_bytes("\x7F", 1); }, true},
        BinaryStringBoolCase{"bytes_ge_equal", cel_bytes_ge_at_vv,
                             +[]() { return cel_make_bytes("\x01\x02", 2); },
                             +[]() { return cel_make_bytes("\x01\x02", 2); },
                             true},
        BinaryStringBoolCase{"bytes_ge_strictly_less_false",
                             cel_bytes_ge_at_vv,
                             +[]() { return cel_make_bytes("\x7F", 1); },
                             +[]() { return cel_make_bytes("\x80", 1); },
                             false}),
    [](const auto& info) { return std::string(info.param.name); });

// ── Parameterized: string concat happy/empty ──────────────────

struct StringConcatCase {
  const char* name;
  uint32_t (*lhs)();
  uint32_t (*rhs)();
  const char* expected;
  uint32_t expected_len;
};

class StringConcatTest
    : public StringOpsTest,
      public ::testing::WithParamInterface<StringConcatCase> {};

TEST_P(StringConcatTest, ProducesExpectedString) {
  const StringConcatCase& c = GetParam();
  uint32_t out = MakeOut();
  cel_string_concat_at_vv(out, c.lhs(), c.rhs());
  EXPECT_EQ(ReadString(out), std::string(c.expected, c.expected_len)) << c.name;
  EXPECT_EQ(At(out)->payload.s.len, c.expected_len) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    All, StringConcatTest,
    ::testing::Values(StringConcatCase{"concat_happy_path",
                                       +[]() {
                                         return cel_make_string("abc", 3);
                                       },
                                       +[]() {
                                         return cel_make_string("def", 3);
                                       },
                                       "abcdef", 6},
                      StringConcatCase{"concat_empty_lhs",
                                       +[]() {
                                         return cel_make_string("", 0);
                                       },
                                       +[]() {
                                         return cel_make_string("only", 4);
                                       },
                                       "only", 4},
                      StringConcatCase{"concat_both_empty",
                                       +[]() {
                                         return cel_make_string("", 0);
                                       },
                                       +[]() {
                                         return cel_make_string("", 0);
                                       },
                                       "", 0}),
    [](const auto& info) {
      return std::string(info.param.name);
    });

// ── Parameterized: unary size helpers (string / bytes) ────────
//
// size() is byte count, not code-point count — that spec
// citation lives in its own focused TEST_F below; this table
// covers the same-shape happy-path / empty-operand pair for
// both string and bytes.

struct SizeIntCase {
  const char* name;
  void (*helper)(uint32_t, uint32_t);
  uint32_t (*v)();
  int64_t expected;
};

class StringSizeTest : public StringOpsTest,
                       public ::testing::WithParamInterface<SizeIntCase> {};

TEST_P(StringSizeTest, ProducesExpectedByteCount) {
  const SizeIntCase& c = GetParam();
  uint32_t out = MakeOut();
  c.helper(out, c.v());
  ASSERT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_INT)) << c.name;
  EXPECT_EQ(At(out)->payload.i, c.expected) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    All, StringSizeTest,
    ::testing::Values(SizeIntCase{"string_size_empty", cel_string_size_at_v,
                                  +[]() {
                                    return cel_make_string("", 0);
                                  },
                                  0},
                      SizeIntCase{"bytes_size_three", cel_bytes_size_at_v,
                                  +[]() {
                                    return cel_make_bytes("\x00\x01\x02", 3);
                                  },
                                  3},
                      SizeIntCase{"bytes_size_empty", cel_bytes_size_at_v,
                                  +[]() {
                                    return cel_make_bytes("", 0);
                                  },
                                  0}),
    [](const auto& info) {
      return std::string(info.param.name);
    });

// ── Spec-citation focused tests (TEST_F) ──────────────────────

// langdef §"String / bytes": size() returns BYTE count, not
// code-point count.  The 4-byte UTF-8 sequence for U+1F600
// (😀) must report size 4, not 1.  Distinct-story citation —
// stays out of the SizeIntCase table so the rationale is
// explicit at the assertion.
TEST_F(StringOpsTest, StringSizeIsByteCountNotCodepointCount) {
  static const char kEmoji[] = "\xF0\x9F\x98\x80";  // U+1F600
  uint32_t v = cel_make_string(kEmoji, sizeof(kEmoji) - 1);
  uint32_t out = MakeOut();
  cel_string_size_at_v(out, v);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(At(out)->payload.i, 4);
}

// Verifies the concatenated bytes' byte-walk: indices 0..2
// must read back 0x01, 0x02, 0x03.  Distinct-story: locks the
// positional layout post-concat; the size assertion alone
// wouldn't catch a swapped-operand regression.
TEST_F(StringOpsTest, BytesConcatPreservesByteOrder) {
  uint32_t a = cel_make_bytes("\x01\x02", 2);
  uint32_t b = cel_make_bytes("\x03", 1);
  uint32_t out = MakeOut();
  cel_bytes_concat_at_vv(out, a, b);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(At(out)->payload.s.len, 3u);
  const uint8_t* dst = cel_mem_base() + At(out)->payload.s.ptr;
  EXPECT_EQ(dst[0], 0x01);
  EXPECT_EQ(dst[1], 0x02);
  EXPECT_EQ(dst[2], 0x03);
}

// langdef: bytes are arbitrary byte sequences — equality must
// compare every byte including embedded NULs.  A naive strcmp
// would stop at the NUL and call the operands equal.
TEST_F(StringOpsTest, BytesEqWithEmbeddedNul) {
  uint32_t a = cel_make_bytes(
      "\x00"
      "hi",
      3);
  uint32_t b = cel_make_bytes(
      "\x00"
      "hi",
      3);
  uint32_t c = cel_make_bytes(
      "\x00"
      "hj",
      3);
  uint32_t out = MakeOut();
  cel_bytes_eq_at_vv(out, a, b);
  EXPECT_TRUE(ReadBool(out));
  cel_bytes_eq_at_vv(out, a, c);
  EXPECT_FALSE(ReadBool(out));
}

// langdef: bytes order as UNSIGNED bytes.  0x80 > 0x7F — a
// signed-char comparator would invert this.
TEST_F(StringOpsTest, BytesLtUnsignedByteOrder) {
  uint32_t a = cel_make_bytes("\x80", 1);  // > 0x7F as unsigned
  uint32_t b = cel_make_bytes("\x7F", 1);
  uint32_t out = MakeOut();
  cel_bytes_lt_at_vv(out, b, a);
  EXPECT_TRUE(ReadBool(out));
  cel_bytes_lt_at_vv(out, a, b);
  EXPECT_FALSE(ReadBool(out));
}

// ── Type-mismatch + 3VL envelope (one example each) ───────────

TEST_F(StringOpsTest, StringConcatTypeMismatchPoisons) {
  uint32_t a = cel_make_string("a", 1);
  uint32_t b = cel_make_int(1);
  uint32_t out = MakeOut();
  cel_string_concat_at_vv(out, a, b);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(StringOpsTest, BytesConcatMixedKindRejected) {
  uint32_t a = cel_make_bytes("a", 1);
  uint32_t b = cel_make_string("a", 1);
  uint32_t out = MakeOut();
  cel_bytes_concat_at_vv(out, a, b);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(StringOpsTest, ContainsAbsorbsError) {
  uint32_t err_off = cel_alloc(sizeof(CelValue));
  CelValue* err = cel_value_at(err_off);
  err->kind = CEL_ERROR;
  err->payload.err = CEL_ERR_OVERFLOW;
  uint32_t out = MakeOut();
  cel_string_contains_at_vv(out, err_off, cel_make_string("a", 1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

}  // namespace
}  // namespace celwasm
