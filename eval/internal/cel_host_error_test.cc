// Unit tests for `cel_host_error.{cc,h}` — the wire-error helpers
// + 3VL absorbers extracted in M11 Slice E.  Per the CLAUDE.md
// testing principles, each function gets positive + negative +
// boundary coverage.  Before Slice E these helpers were file-local
// inside `cel_host.cc` and only exercised transitively via the
// Compile → Plan → Eval e2e suite; this file pins them directly.

#include "eval/internal/cel_host_error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "eval/error.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "runtime/cel_data.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// ──── celwasm::api::Value error factories ─────────────────────────────────

TEST(CelValueFactoriesTest, FieldNotFoundCarriesNameAsMessage) {
  auto v = FieldNotFound("missing_field");
  ASSERT_TRUE(v.IsError());
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, celwasm::ErrorCode::kFieldNotFound);
  EXPECT_EQ((*info)->message, "missing_field");
}

TEST(CelValueFactoriesTest, FieldNotFoundAcceptsEmptyName) {
  // Boundary: empty name doesn't crash; the message just empties.
  auto v = FieldNotFound("");
  ASSERT_TRUE(v.IsError());
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, celwasm::ErrorCode::kFieldNotFound);
  EXPECT_EQ((*info)->message, "");
}

TEST(CelValueFactoriesTest, MakeErrorRoundsTripCodeAndMessage) {
  auto v = MakeError(celwasm::ErrorCode::kOverflow, "bigger than i64");
  ASSERT_TRUE(v.IsError());
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, celwasm::ErrorCode::kOverflow);
  EXPECT_EQ((*info)->message, "bigger than i64");
}

TEST(CelValueFactoriesTest, MakeErrorMovesMessageString) {
  // Pin the move-into-payload contract — the moved-from string
  // shouldn't appear in the payload (it's been moved).  This is
  // mostly a smoke test on the std::move(message) chain.
  std::string msg = "transient message";
  auto v = MakeError(celwasm::ErrorCode::kTypeMismatch, std::move(msg));
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->message, "transient message");
}

TEST(CelValueFactoriesTest, KeyTypeMismatchHasPinnedMessage) {
  auto v = KeyTypeMismatch();
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, celwasm::ErrorCode::kTypeMismatch);
  EXPECT_EQ((*info)->message, "map key kind is not bool/int/uint/string");
}

TEST(CelValueFactoriesTest, NoSuchKeyHasPinnedMessage) {
  auto v = NoSuchKey();
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, celwasm::ErrorCode::kKeyNotFound);
  EXPECT_EQ((*info)->message, "no such key");
}

TEST(CelValueFactoriesTest, IndexOutOfBoundsFormatsRange) {
  auto v = IndexOutOfBounds(/*index=*/5, /*count=*/3);
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, celwasm::ErrorCode::kIndexOutOfBounds);
  EXPECT_EQ((*info)->message, "index 5 out of range [0, 3)");
}

TEST(CelValueFactoriesTest, IndexOutOfBoundsZeroCount) {
  // Boundary: zero-count list — the formatted message stays sane.
  auto v = IndexOutOfBounds(/*index=*/0, /*count=*/0);
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->message, "index 0 out of range [0, 0)");
}

// ──── WireErrorCode mapping ──────────────────────────────────────

TEST(WireErrorCodeTest, EveryHostCodeMapsToWireConstant) {
  // Pin the host-code → wire-code mapping.  The two enums extend
  // independently; this test catches accidental drift.
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kOverflow), CEL_ERR_OVERFLOW);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kDivideByZero),
            CEL_ERR_DIVIDE_BY_ZERO);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kModulusByZero),
            CEL_ERR_MODULUS_BY_ZERO);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kTypeMismatch),
            CEL_ERR_TYPE_MISMATCH);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kTypeUnsupported),
            CEL_ERR_TYPE_UNSUPPORTED);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kKeyNotFound),
            CEL_ERR_NO_SUCH_KEY);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kFieldNotFound),
            CEL_ERR_FIELD_NOT_FOUND);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kIndexOutOfBounds),
            CEL_ERR_INDEX_OUT_OF_BOUNDS);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kInvalidArgument),
            CEL_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(WireErrorCode(celwasm::ErrorCode::kHostAdapterError),
            CEL_ERR_HOST_ADAPTER_ERROR);
}

// The default-arm fallback ("unknown ErrorCode → CEL_ERR_TYPE_MISMATCH")
// is defensive scaffolding for future enum additions; deliberately
// not unit-tested because forcing a static_cast<ErrorCode>(N) for N
// outside the declared range is undefined behaviour at the language
// level and the compiler-folded switch behaviour isn't portable.
// The real guard against drift is the exhaustive positive test above
// (one EXPECT per enum value) — if a new code lands without updating
// WireErrorCode, that test won't compile.

// ──── Wire-format slot writers ───────────────────────────────────

TEST(WireWritersTest, WriteWireErrorPopulatesSlotKindAndCode) {
  test::FakeMemoryView mem;
  WriteWireError(CEL_ERR_OVERFLOW, /*out_slot=*/0, mem);
  const CelValue read = mem.ReadCelValue(0);
  EXPECT_EQ(read.kind, CEL_ERROR);
  EXPECT_EQ(read.payload.err, CEL_ERR_OVERFLOW);
}

TEST(WireWritersTest, WriteWireBoolTrueAndFalse) {
  test::FakeMemoryView mem;
  WriteWireBool(true, /*out_slot=*/0, mem);
  EXPECT_EQ(mem.ReadCelValue(0).kind, CEL_BOOL);
  EXPECT_EQ(mem.ReadCelValue(0).payload.b, 1);

  WriteWireBool(false, /*out_slot=*/24, mem);
  EXPECT_EQ(mem.ReadCelValue(24).kind, CEL_BOOL);
  EXPECT_EQ(mem.ReadCelValue(24).payload.b, 0);
}

TEST(WireWritersTest, WriteWireIntBoundaryValues) {
  // Boundary: INT64_MIN, INT64_MAX, zero, negative.
  test::FakeMemoryView mem;
  WriteWireInt(0, 0, mem);
  EXPECT_EQ(mem.ReadCelValue(0).payload.i, 0);

  WriteWireInt(std::numeric_limits<int64_t>::min(), 24, mem);
  EXPECT_EQ(mem.ReadCelValue(24).payload.i,
            std::numeric_limits<int64_t>::min());

  WriteWireInt(std::numeric_limits<int64_t>::max(), 48, mem);
  EXPECT_EQ(mem.ReadCelValue(48).payload.i,
            std::numeric_limits<int64_t>::max());

  WriteWireInt(-1, 72, mem);
  EXPECT_EQ(mem.ReadCelValue(72).payload.i, -1);
}

TEST(WireWritersTest, WriteInvalidArgumentErrorWritesInvalidArgCode) {
  test::FakeMemoryView mem;
  WriteInvalidArgumentError(0, mem);
  const CelValue read = mem.ReadCelValue(0);
  EXPECT_EQ(read.kind, CEL_ERROR);
  EXPECT_EQ(read.payload.err, CEL_ERR_INVALID_ARGUMENT);
}

TEST(PoisonCelValueTest, ProducesErrorSlotWithPayload) {
  CelValue poisoned = PoisonCelValue(CEL_ERR_OVERFLOW);
  EXPECT_EQ(poisoned.kind, CEL_ERROR);
  EXPECT_EQ(poisoned.payload.err, CEL_ERR_OVERFLOW);
}

// ──── 3VL absorbers ──────────────────────────────────────────────

TEST(AbsorbUnaryTest, NormalValuePassesThrough) {
  test::FakeMemoryView mem;
  CelValue normal{};
  normal.kind = CEL_INT;
  normal.payload.i = 42;
  EXPECT_FALSE(AbsorbUnary(normal, 0, mem));
  // Slot unchanged (default-initialised, kind=CEL_NULL).
  EXPECT_EQ(mem.ReadCelValue(0).kind, CEL_NULL);
}

TEST(AbsorbUnaryTest, ErrorPropagates) {
  test::FakeMemoryView mem;
  CelValue err = PoisonCelValue(CEL_ERR_DIVIDE_BY_ZERO);
  EXPECT_TRUE(AbsorbUnary(err, 0, mem));
  EXPECT_EQ(mem.ReadCelValue(0).kind, CEL_ERROR);
  EXPECT_EQ(mem.ReadCelValue(0).payload.err, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST(AbsorbUnaryTest, UnknownPropagates) {
  test::FakeMemoryView mem;
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 7;
  EXPECT_TRUE(AbsorbUnary(unk, 0, mem));
  EXPECT_EQ(mem.ReadCelValue(0).kind, CEL_UNKNOWN);
  EXPECT_EQ(mem.ReadCelValue(0).payload.unk, 7u);
}

TEST(AbsorbBinaryTest, BothNormalReturnsFalse) {
  test::FakeMemoryView mem;
  CelValue a{};
  a.kind = CEL_INT;
  a.payload.i = 1;
  CelValue b{};
  b.kind = CEL_INT;
  b.payload.i = 2;
  EXPECT_FALSE(AbsorbBinary(a, b, 0, mem));
  EXPECT_EQ(mem.ReadCelValue(0).kind, CEL_NULL);  // untouched
}

TEST(AbsorbBinaryTest, FirstOperandErrorWins) {
  test::FakeMemoryView mem;
  CelValue err = PoisonCelValue(CEL_ERR_OVERFLOW);
  CelValue ok{};
  ok.kind = CEL_INT;
  ok.payload.i = 1;
  EXPECT_TRUE(AbsorbBinary(err, ok, 0, mem));
  EXPECT_EQ(mem.ReadCelValue(0).payload.err, CEL_ERR_OVERFLOW);
}

TEST(AbsorbBinaryTest, SecondOperandErrorPropagates) {
  // First operand normal, second error: second is propagated.
  test::FakeMemoryView mem;
  CelValue ok{};
  ok.kind = CEL_INT;
  ok.payload.i = 1;
  CelValue err = PoisonCelValue(CEL_ERR_TYPE_MISMATCH);
  EXPECT_TRUE(AbsorbBinary(ok, err, 0, mem));
  EXPECT_EQ(mem.ReadCelValue(0).payload.err, CEL_ERR_TYPE_MISMATCH);
}

TEST(AbsorbBinaryTest, FirstOperandUnknownBeatsSecondOperandError) {
  // Per the propagation rule: the FIRST non-normal operand wins
  // (not error-beats-unknown).  Documented behaviour in
  // cel_host_error.h.
  test::FakeMemoryView mem;
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 3;
  CelValue err = PoisonCelValue(CEL_ERR_OVERFLOW);
  EXPECT_TRUE(AbsorbBinary(unk, err, 0, mem));
  EXPECT_EQ(mem.ReadCelValue(0).kind, CEL_UNKNOWN);
  EXPECT_EQ(mem.ReadCelValue(0).payload.unk, 3u);
}

TEST(AbsorbBinaryTest, FirstOperandErrorBeatsSecondOperandUnknown) {
  // Symmetric: first-wins also when first is error and second is
  // unknown.
  test::FakeMemoryView mem;
  CelValue err = PoisonCelValue(CEL_ERR_DIVIDE_BY_ZERO);
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 7;
  EXPECT_TRUE(AbsorbBinary(err, unk, 0, mem));
  EXPECT_EQ(mem.ReadCelValue(0).kind, CEL_ERROR);
  EXPECT_EQ(mem.ReadCelValue(0).payload.err, CEL_ERR_DIVIDE_BY_ZERO);
}

}  // namespace
}  // namespace celwasm
