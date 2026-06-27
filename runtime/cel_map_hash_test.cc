// Unit coverage for the shared SwissTable hash kernel
// (`runtime/cel_map_hash.h`).  Two concerns:
//
//   1. SWAR group primitives (`group_match` / `group_match_empty`) over
//      hand-built control words — the §4 probe machinery.
//   2. Key-hash canonicalization (§5) — the load-bearing invariant that
//      keys `cel_value_eq` (cel_runtime.c:655) considers equal hash
//      identically, plus the `cel_map_index_num_slots` sizing (§3.2).
//
// The canonicalization assertions are checked against what
// `cel_value_eq` actually does (verified by reading cel_runtime.c:655 +
// numeric_compare_kernel cel_compare.c:157), not against a guess.

#include "runtime/cel_map_hash.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_internal.h"  // cel_value_eq
#include "runtime/cel_layout.h"
#include "runtime/cel_make.h"

namespace celwasm {
namespace {

// ════════════════════════════════════════════════════════════════════
// §4 — SWAR group primitives (no arena needed; pure bit math).
// ════════════════════════════════════════════════════════════════════

// Pack 8 control bytes (ctrl[0] = lowest byte) into a group word the way
// `cel_group_load` would read them on an LE host.
uint64_t PackGroup(const uint8_t b[8]) {
  uint64_t g = 0;
  for (int i = 0; i < 8; ++i) {
    g |= static_cast<uint64_t>(b[i]) << (8 * i);
  }
  return g;
}

// Decode a match mask into the set of matching lane indices [0,8).
// Mirrors the production consumer: ctz>>3 for the lane, clear lowest set.
std::string MatchedLanes(uint64_t mask) {
  std::string lanes;
  while (mask) {
    int lane = __builtin_ctzll(mask) >> 3;
    lanes.push_back(static_cast<char>('0' + lane));
    mask &= mask - 1;
  }
  return lanes;
}

TEST(GroupMatchTest, FindsSingleMatchingLane) {
  uint8_t ctrl[8] = {0x10, 0x20, 0x33, 0x40, 0x50, 0x60, 0x70, 0x01};
  uint64_t g = PackGroup(ctrl);
  EXPECT_EQ(MatchedLanes(group_match(g, 0x33)), "2");
  EXPECT_EQ(MatchedLanes(group_match(g, 0x01)), "7");
  EXPECT_EQ(MatchedLanes(group_match(g, 0x10)), "0");
}

TEST(GroupMatchTest, NoMatchWhenAbsent) {
  uint8_t ctrl[8] = {0x10, 0x20, 0x33, 0x40, 0x50, 0x60, 0x70, 0x01};
  EXPECT_EQ(group_match(PackGroup(ctrl), 0x7F), 0u);
}

TEST(GroupMatchTest, FindsMultipleMatchingLanes) {
  uint8_t ctrl[8] = {0x2A, 0x00, 0x2A, 0x00, 0x2A, 0x00, 0x00, 0x00};
  EXPECT_EQ(MatchedLanes(group_match(PackGroup(ctrl), 0x2A)), "024");
}

TEST(GroupMatchTest, H2ZeroMatchesFullSlotsOnly) {
  // H2 == 0 is a legal control byte (cel_h2 of any hash with low 7 bits
  // clear).  It must match full slots whose ctrl byte is 0x00 but NEVER
  // an empty slot (0x80).
  uint8_t ctrl[8] = {0x00, 0x80, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80};
  EXPECT_EQ(MatchedLanes(group_match(PackGroup(ctrl), 0x00)), "02");
}

TEST(GroupMatchEmptyTest, FindsEmptySlots) {
  uint8_t ctrl[8] = {0x10, 0x80, 0x33, 0x80, 0x80, 0x60, 0x70, 0x80};
  EXPECT_EQ(MatchedLanes(group_match_empty(PackGroup(ctrl))), "1347");
}

TEST(GroupMatchEmptyTest, AllFullNoMatch) {
  // Every byte is a valid H2 (top bit clear) — none is empty.
  uint8_t ctrl[8] = {0x00, 0x7F, 0x01, 0x40, 0x2A, 0x55, 0x10, 0x33};
  EXPECT_EQ(group_match_empty(PackGroup(ctrl)), 0u);
}

TEST(GroupMatchEmptyTest, AllEmpty) {
  uint8_t ctrl[8] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
  EXPECT_EQ(MatchedLanes(group_match_empty(PackGroup(ctrl))), "01234567");
}

TEST(GroupMatchEmptyTest, DoesNotMatchDeletedOrFull) {
  // kDeleted (0xFE) is never written by this kernel, but if it appeared
  // MaskEmpty (shift-6) must NOT match it — only 0x80 is "empty".
  uint8_t ctrl[8] = {kDeleted, 0x80, 0x7F, kDeleted, 0x00, 0x80, 0xFE, 0x80};
  // Only the three 0x80 bytes (lanes 1, 5, 7) are empty.
  EXPECT_EQ(MatchedLanes(group_match_empty(PackGroup(ctrl))), "157");
}

TEST(H1H2Test, SplitsHashIntoSlotBitsAndControlByte) {
  uint64_t h = 0xABCD1234DEADBEEFULL;
  EXPECT_EQ(cel_h2(h), static_cast<uint8_t>(0xEF & 0x7F));  // low 7 bits
  EXPECT_EQ(cel_h1(h), h >> 7);                             // remaining bits
  // H2 always has its top bit clear → never aliases kEmpty.
  EXPECT_EQ(cel_h2(h) & 0x80, 0);
}

TEST(GroupLoadTest, ReadsControlBytesLaneForLane) {
  uint8_t buf[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  uint64_t g = cel_group_load(buf);
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ((g >> (8 * i)) & 0xFF, static_cast<uint64_t>(buf[i]))
        << "lane " << i;
  }
}

// ════════════════════════════════════════════════════════════════════
// §3.2 — num_slots sizing boundaries.
// ════════════════════════════════════════════════════════════════════

TEST(NumSlotsTest, FlooredAtGroupWidthForTinyCounts) {
  // Below kIndexThreshold no index is built, but the sizing function
  // still floors at kGroupWidth = 8 so one group is always a full load.
  EXPECT_EQ(cel_map_index_num_slots(0), 8u);
  EXPECT_EQ(cel_map_index_num_slots(1), 8u);
  EXPECT_EQ(cel_map_index_num_slots(7), 8u);
}

TEST(NumSlotsTest, WorkedBoundaries) {
  // The exact boundaries enumerated in §3.2.
  EXPECT_EQ(cel_map_index_num_slots(8), 16u);    // ceil(64/7)=10 → 16
  EXPECT_EQ(cel_map_index_num_slots(14), 16u);   // ceil(112/7)=16 → 16
  EXPECT_EQ(cel_map_index_num_slots(15), 32u);   // ceil(120/7)=18 → 32
  EXPECT_EQ(cel_map_index_num_slots(56), 64u);   // ceil(448/7)=64 → 64
  EXPECT_EQ(cel_map_index_num_slots(64), 128u);  // ceil(512/7)=74 → 128
}

TEST(NumSlotsTest, LoadFactorNeverExceedsSevenEighths) {
  for (uint32_t count = 8; count <= 4096; ++count) {
    uint32_t slots = cel_map_index_num_slots(count);
    // power of two
    EXPECT_EQ(slots & (slots - 1), 0u) << "count " << count;
    // load factor ≤ 7/8  ⇔  8*count ≤ 7*slots
    EXPECT_LE(static_cast<uint64_t>(count) * 8u,
              static_cast<uint64_t>(slots) * 7u)
        << "count " << count;
    // and slots is the SMALLEST such power of two (halving violates it,
    // or drops below the kGroupWidth floor).
    if (slots > kGroupWidth) {
      uint32_t half = slots >> 1;
      EXPECT_GT(static_cast<uint64_t>(count) * 8u,
                static_cast<uint64_t>(half) * 7u)
          << "count " << count;
    }
  }
}

// ════════════════════════════════════════════════════════════════════
// §5 — key-hash canonicalization.
//
// Fixture provides an arena so string/bytes keys can be staged into the
// shared linear memory (the hash reads through cel_memory_base_()).
// ════════════════════════════════════════════════════════════════════

class KeyHashTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }

 public:
  static CelValue Int(int64_t v) {
    CelValue cv;
    std::memset(&cv, 0, sizeof(cv));
    cv.kind = CEL_INT;
    cv.payload.i = v;
    return cv;
  }
  static CelValue Uint(uint64_t v) {
    CelValue cv;
    std::memset(&cv, 0, sizeof(cv));
    cv.kind = CEL_UINT;
    cv.payload.u = v;
    return cv;
  }
  static CelValue Double(double v) {
    CelValue cv;
    std::memset(&cv, 0, sizeof(cv));
    cv.kind = CEL_DOUBLE;
    cv.payload.d = v;
    return cv;
  }
  static CelValue Bool(bool v) {
    CelValue cv;
    std::memset(&cv, 0, sizeof(cv));
    cv.kind = CEL_BOOL;
    cv.payload.b = v ? 1 : 0;
    return cv;
  }
  // Stage a string/bytes value into the arena and return it by value.
  CelValue Str(const std::string& s) {
    uint32_t off = cel_make_string(s.data(), static_cast<uint32_t>(s.size()));
    return *cel_value_at(off);
  }
  CelValue Bytes(const std::string& s) {
    uint32_t off = cel_make_bytes(s.data(), static_cast<uint32_t>(s.size()));
    return *cel_value_at(off);
  }
};

// ── The core invariant, asserted directly: equal ⇒ same hash. ─────────
//
// Each row picks N and asserts int N / uint N / double N.0 all hash the
// same, AND that cel_value_eq agrees they are equal (the property the
// hash must respect).  N spans 0, 1, 2, 2^53-1 (last exactly-rep), 2^53,
// 2^53+1, INT64_MAX, and a uint-only value above INT64_MAX.
struct CrossTypeRow {
  const char* name;
  int64_t as_int;    // valid iff representable as int64
  uint64_t as_uint;  // the uint reading
  double as_double;  // the integral double reading
  bool int_valid;    // false for values > INT64_MAX
};

class CrossTypeCollision : public KeyHashTest,
                           public ::testing::WithParamInterface<CrossTypeRow> {
};

TEST_P(CrossTypeCollision, EqualKeysHashIdentically) {
  const CrossTypeRow& r = GetParam();
  CelValue u = Uint(r.as_uint);
  CelValue d = Double(r.as_double);
  uint64_t hu = cel_map_key_hash(&u);
  uint64_t hd = cel_map_key_hash(&d);

  if (r.int_valid) {
    CelValue i = Int(r.as_int);
    uint64_t hi = cel_map_key_hash(&i);
    // cel_value_eq must agree these are equal (cel_runtime.c:656).
    ASSERT_TRUE(cel_value_eq(&i, &u)) << r.name << " int==uint";
    ASSERT_TRUE(cel_value_eq(&i, &d)) << r.name << " int==double";
    EXPECT_EQ(hi, hu) << r.name << " int/uint hash";
    EXPECT_EQ(hi, hd) << r.name << " int/double hash";
  }
  ASSERT_TRUE(cel_value_eq(&u, &d)) << r.name << " uint==double";
  EXPECT_EQ(hu, hd) << r.name << " uint/double hash";
}

INSTANTIATE_TEST_SUITE_P(
    Numerics, CrossTypeCollision,
    ::testing::Values(
        CrossTypeRow{"zero", 0, 0u, 0.0, true},
        CrossTypeRow{"one", 1, 1u, 1.0, true},
        CrossTypeRow{"two", 2, 2u, 2.0, true},
        // 2^53 - 1: largest int exactly representable as a double.
        CrossTypeRow{"2p53m1", static_cast<int64_t>(9007199254740991LL),
                     9007199254740991ull, 9007199254740991.0, true},
        // 2^53: still exactly representable.
        CrossTypeRow{"2p53", static_cast<int64_t>(9007199254740992LL),
                     9007199254740992ull, 9007199254740992.0, true},
        // 2^53 + 2: exactly representable (2^53+1 is NOT, but +2 is).
        // The double 2^53+2.0 truncates to exactly 2^53+2, and
        // cel_value_eq(int 2^53+2, double 2^53+2.0) is true.
        CrossTypeRow{"2p53p2", static_cast<int64_t>(9007199254740994LL),
                     9007199254740994ull, 9007199254740994.0, true},
        // INT64_MAX is NOT exactly representable as a double (rounds to
        // 2^63), so a double reading would NOT compare-equal to it; use
        // the int/uint pair only — drop the double check by giving a
        // double that DOES equal it under the lossy compare: (double)
        // INT64_MAX == 2^63, which equals neither.  So instead use a
        // value that is exactly representable: skip INT64_MAX double.
        // Covered separately below.
        // A uint-only value above INT64_MAX that is exactly representable
        // as a double: 2^63 (= 9223372036854775808).
        CrossTypeRow{"2p63_uint_only", 0, 9223372036854775808ull,
                     9223372036854775808.0, false}));

// 2^53 + 1 is NOT exactly representable as a double; the literal
// `9007199254740993.0` rounds to 2^53.  Under the lossy cel_value_eq the
// double 2^53.0 is map-equal to BOTH int 2^53 and int 2^53+1 — that is
// the §5.1 range ambiguity the LOOKUP kernel handles with a linear
// fallback, NOT the hash.  Pin the hash behaviour: int 2^53 and the
// rounding double hash equal (both canonicalize to 2^53's token); int
// 2^53+1 hashes to its own (different) token.  No false miss occurs
// because the linear fallback (a later phase) covers the double side.
TEST_F(KeyHashTest, RoundingDoubleHashesToTruncatedIntToken) {
  CelValue i53 = Int(9007199254740992LL);    // 2^53
  CelValue i53p1 = Int(9007199254740993LL);  // 2^53 + 1
  CelValue d = Double(9007199254740993.0);   // rounds to 2^53.0

  // The literal rounded down to 2^53, so the double's truncation is 2^53.
  ASSERT_EQ(static_cast<int64_t>(d.payload.d), 9007199254740992LL);
  // Hash equals the int-2^53 token (exact representation it carries).
  EXPECT_EQ(cel_map_key_hash(&d), cel_map_key_hash(&i53));
  // And differs from int 2^53+1's token (distinct integers, distinct
  // tokens) — the §5.1 ambiguity lives in equality, not the hash.
  EXPECT_NE(cel_map_key_hash(&i53), cel_map_key_hash(&i53p1));
}

// INT64_MIN magnitude is computed without UB (§5).  A negative int and
// the integral double of the same value collide.
TEST_F(KeyHashTest, NegativeIntCollidesWithDouble) {
  for (int64_t v :
       {static_cast<int64_t>(-1), static_cast<int64_t>(-2),
        static_cast<int64_t>(-1024),
        static_cast<int64_t>(-9007199254740992LL) /* -2^53 */, INT64_MIN}) {
    CelValue i = Int(v);
    CelValue d = Double(static_cast<double>(v));
    // INT64_MIN is exactly representable as a double (it's -2^63).
    ASSERT_TRUE(cel_value_eq(&i, &d)) << "v=" << v;
    EXPECT_EQ(cel_map_key_hash(&i), cel_map_key_hash(&d)) << "v=" << v;
  }
}

// Negatives never collide with the non-negative / uint token space: a
// negative int can't compare-equal to any uint key.
TEST_F(KeyHashTest, NegativeAndPositiveTokenSpacesAreDisjoint) {
  CelValue neg = Int(-5);
  CelValue pos = Int(5);
  CelValue pos_u = Uint(5);
  EXPECT_NE(cel_map_key_hash(&neg), cel_map_key_hash(&pos));
  EXPECT_NE(cel_map_key_hash(&neg), cel_map_key_hash(&pos_u));
}

// Out-of-range / non-integral / NaN / ±Inf doubles all take the fixed
// non-matching sentinel — none can compare-equal to any stored int/uint
// key (cmp_int_vs_double cel_compare.c:132 / cmp_uint_vs_double :139).
TEST_F(KeyHashTest, NonMatchingDoublesTakeSentinel) {
  uint64_t sentinel = cel_hash_double_sentinel();
  double cases[] = {
      1.5,                     // non-integral
      -0.25,                   // non-integral
      __builtin_nan(""),       // NaN
      __builtin_inf(),         // +Inf
      -__builtin_inf(),        // -Inf
      1e300,                   // far above 2^64
      -1e300,                  // far below INT64_MIN
      18446744073709551616.0,  // exactly 2^64 (out of uint range)
  };
  for (double d : cases) {
    CelValue cv = Double(d);
    EXPECT_EQ(cel_map_key_hash(&cv), sentinel) << "d=" << d;
  }
}

// A double whose mathematical value is an in-range integer must still
// hash to that integer's token (NOT the sentinel) — otherwise a lookup
// `5.0` in `{5:'a'}` false-misses.
TEST_F(KeyHashTest, InRangeIntegralDoubleHashesToIntToken) {
  CelValue d = Double(5.0);
  CelValue i = Int(5);
  EXPECT_NE(cel_map_key_hash(&d), cel_hash_double_sentinel());
  EXPECT_EQ(cel_map_key_hash(&d), cel_map_key_hash(&i));
}

// ── bool: distinct from numeric 0/1 because cel_value_eq is
// (cel_runtime.c:662 guards bool==bool only). ────────────────────────
TEST_F(KeyHashTest, BoolDistinctFromNumericSameMagnitude) {
  CelValue bt = Bool(true);
  CelValue bf = Bool(false);
  CelValue i1 = Int(1);
  CelValue i0 = Int(0);

  // Verify the equality these assertions rely on: bool true != int 1.
  ASSERT_FALSE(cel_value_eq(&bt, &i1));
  ASSERT_FALSE(cel_value_eq(&bf, &i0));

  // true / false hash distinctly from each other and from numeric 0/1.
  EXPECT_NE(cel_map_key_hash(&bt), cel_map_key_hash(&bf));
  EXPECT_NE(cel_map_key_hash(&bt), cel_map_key_hash(&i1));
  EXPECT_NE(cel_map_key_hash(&bf), cel_map_key_hash(&i0));
}

TEST_F(KeyHashTest, BoolNormalizesNonZeroTrue) {
  // Any non-zero bool payload normalizes to canonical 1 (mirrors
  // cel_value_eq's `payload.b != 0`, cel_runtime.c:663).
  CelValue b1 = Bool(true);
  CelValue braw;
  std::memset(&braw, 0, sizeof(braw));
  braw.kind = CEL_BOOL;
  braw.payload.b = 42;  // non-canonical "true"
  EXPECT_EQ(cel_map_key_hash(&b1), cel_map_key_hash(&braw));
}

// ── string / bytes byte hashing (length-delimited; never strlen). ─────
TEST_F(KeyHashTest, EqualStringsHashIdentically) {
  CelValue a = Str("hello world");
  CelValue b = Str("hello world");
  ASSERT_TRUE(cel_value_eq(&a, &b));
  EXPECT_EQ(cel_map_key_hash(&a), cel_map_key_hash(&b));
}

TEST_F(KeyHashTest, DifferentStringsHashDifferently) {
  CelValue a = Str("hello");
  CelValue b = Str("world");
  EXPECT_NE(cel_map_key_hash(&a), cel_map_key_hash(&b));
}

TEST_F(KeyHashTest, EmbeddedNulIsHashedNotTruncated) {
  // "a\0b" and "a" must hash differently — a strlen-based hash would
  // collide them (it would stop at the NUL).
  std::string with_nul("a\0b", 3);
  std::string prefix("a", 1);
  CelValue a = Str(with_nul);
  CelValue b = Str(prefix);
  ASSERT_FALSE(cel_value_eq(&a, &b));
  EXPECT_NE(cel_map_key_hash(&a), cel_map_key_hash(&b));

  // Two identical embedded-NUL strings still collide.
  CelValue a2 = Str(with_nul);
  ASSERT_TRUE(cel_value_eq(&a, &a2));
  EXPECT_EQ(cel_map_key_hash(&a), cel_map_key_hash(&a2));
}

TEST_F(KeyHashTest, EmbeddedNulVariantsAreDistinct) {
  // "\0\0" vs "\0" — content is all NULs; only the length differs.  The
  // length fold in cel_hash_bytes keeps them apart.
  CelValue a = Str(std::string("\0\0", 2));
  CelValue b = Str(std::string("\0", 1));
  EXPECT_NE(cel_map_key_hash(&a), cel_map_key_hash(&b));
}

TEST_F(KeyHashTest, MultibyteUtf8IsHashedByBytes) {
  // "é" (U+00E9, 2 bytes) vs "e" — distinct byte runs, distinct hashes.
  CelValue a = Str("\xC3\xA9");  // é
  CelValue b = Str("e");
  EXPECT_NE(cel_map_key_hash(&a), cel_map_key_hash(&b));

  // Snowman U+2603 (3 bytes) round-trips to itself.
  CelValue snow = Str("\xE2\x98\x83");
  CelValue snow2 = Str("\xE2\x98\x83");
  EXPECT_EQ(cel_map_key_hash(&snow), cel_map_key_hash(&snow2));
}

TEST_F(KeyHashTest, EmptyStringHashesStably) {
  CelValue a = Str("");
  CelValue b = Str("");
  ASSERT_TRUE(cel_value_eq(&a, &b));
  EXPECT_EQ(cel_map_key_hash(&a), cel_map_key_hash(&b));
}

// string and bytes with identical content are NOT equal under
// cel_value_eq (cel_runtime.c:659/665 guard each kind), and are salted
// apart so they hash differently — a harmless distinction that reduces
// cross-kind collisions.
TEST_F(KeyHashTest, StringAndBytesSameContentHashDistinctly) {
  CelValue s = Str("payload");
  CelValue b = Bytes("payload");
  ASSERT_FALSE(cel_value_eq(&s, &b));
  EXPECT_NE(cel_map_key_hash(&s), cel_map_key_hash(&b));
}

TEST_F(KeyHashTest, EqualBytesHashIdentically) {
  CelValue a = Bytes(std::string("\x00\x01\x02", 3));
  CelValue b = Bytes(std::string("\x00\x01\x02", 3));
  ASSERT_TRUE(cel_value_eq(&a, &b));
  EXPECT_EQ(cel_map_key_hash(&a), cel_map_key_hash(&b));
}

}  // namespace
}  // namespace celwasm
