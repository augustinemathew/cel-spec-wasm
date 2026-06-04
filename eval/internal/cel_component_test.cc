// Per-CEL-type marshaling tests for the Component-Model bridge
// (`eval/internal/cel_component.h`).  Drives the §6 type matrix +
// §10 boundary matrix from m24-foreign-fn-component-backend.md.
//
// CLAUDE.md "tests-first" discipline: the full boundary matrix is
// written up-front, with rows currently unimplemented `GTEST_SKIP()`'d
// against a named verified blocker.  Each blocker maps to a B-slice
// task in the milestone's task list (#6=B.3, #7=B.4, …); deleting the
// skip is the closing edit of that slice.
//
// Boundary discipline (CLAUDE.md "Cover the edge-case matrix"):
//   - Every scalar kind covers `0`, the negative-zero / sentinel
//     value, and both INTn_MIN/MAX (or analogous extremum).
//   - Strings cover empty, embedded NUL, multi-byte UTF-8.
//   - Aggregates cover empty + single + ragged-nested.
//   - Every kind has at least one cross-kind rejection test
//     (CelfnType says X, Value carries Y).

#include "eval/internal/cel_component.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "compiler/celfn/function_library.h"
#include "eval/internal/cel_host.h"
#include "eval/value.h"
#include "gmock/gmock.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"
#include "wasmtime/component/val.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// ── Small helpers ───────────────────────────────────────────────────

CelfnType Prim(CelfnType::Kind k) {
  CelfnType t;
  t.kind = k;
  return t;
}

// Used by GTEST_SKIP'd aggregate-test rows below; the compiler can't
// see the references (skipped bodies never elaborate the construction)
// and would emit -Wunused-function — silence per CLAUDE.md, not via a
// NOLINT.  When B.6/B.7/B.8/B.9 land the references become live and
// the attribute drops.
[[maybe_unused]] CelfnType ListOf(CelfnType elem) {
  CelfnType t;
  t.kind = CelfnType::Kind::kList;
  t.list_element.push_back(std::move(elem));
  return t;
}

[[maybe_unused]] CelfnType MapOf(CelfnType k, CelfnType v) {
  CelfnType t;
  t.kind = CelfnType::Kind::kMap;
  t.map_kv.push_back(std::move(k));
  t.map_kv.push_back(std::move(v));
  return t;
}

[[maybe_unused]] CelfnType OptOf(CelfnType inner) {
  CelfnType t;
  t.kind = CelfnType::Kind::kOptional;
  t.optional_element.push_back(std::move(inner));
  return t;
}

[[maybe_unused]] CelfnType ProtoOf(std::string fqn) {
  CelfnType t;
  t.kind = CelfnType::Kind::kProto;
  t.proto_fqn = std::move(fqn);
  return t;
}

// Sentinel kind for an "uninitialized" wasmtime_component_val_t at the
// start of a test.  The Lift contract treats `out` as overwritten, so
// any kind here is fine — we use BOOL because the union slot for it
// stores no allocation, simplifying the post-test delete.
constexpr uint8_t kSentinelKind = WASMTIME_COMPONENT_BOOL;

// RAII for a wasmtime_component_val_t — ensures every test releases
// its lift output even on assertion failure (otherwise the leak is
// silent under gtest's exit-with-failure path).
class OwnedComponentVal {
 public:
  OwnedComponentVal() {
    val_.kind = kSentinelKind;
    val_.of.boolean = false;
  }
  OwnedComponentVal(const OwnedComponentVal&) = delete;
  OwnedComponentVal& operator=(const OwnedComponentVal&) = delete;
  ~OwnedComponentVal() {
    wasmtime_component_val_delete(&val_);
  }
  wasmtime_component_val_t* get() {
    return &val_;
  }
  const wasmtime_component_val_t& cref() const {
    return val_;
  }

 private:
  wasmtime_component_val_t val_;
};

const CelComponentContext& EmptyCtx() {
  static const CelComponentContext k{};
  return k;
}

// ── bool ────────────────────────────────────────────────────────────

TEST(LiftCelToComponent, BoolFalse) {
  OwnedComponentVal out;
  EXPECT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kBool),
                                 Value::Bool(false), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_BOOL);
  EXPECT_FALSE(out.cref().of.boolean);
}

TEST(LiftCelToComponent, BoolTrue) {
  OwnedComponentVal out;
  EXPECT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kBool),
                                 Value::Bool(true), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_BOOL);
  EXPECT_TRUE(out.cref().of.boolean);
}

TEST(LiftCelToComponent, BoolFromIntValueIsKindMismatch) {
  OwnedComponentVal out;
  EXPECT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kBool), Value::Int(1),
                                 EmptyCtx(), out.get()),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(LowerComponentToCel, BoolRoundTrips) {
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_BOOL;
  in.of.boolean = true;
  Value out;
  ASSERT_THAT(
      LowerComponentToCel(Prim(CelfnType::Kind::kBool), in, EmptyCtx(), &out),
      IsOk());
  ASSERT_EQ(out.kind(), Value::Kind::kBool);
  EXPECT_THAT(out.AsBool(), IsOk());
  EXPECT_TRUE(*out.AsBool());
}

TEST(LowerComponentToCel, BoolKindMismatchRejected) {
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_S64;
  in.of.s64 = 1;
  Value out;
  EXPECT_THAT(
      LowerComponentToCel(Prim(CelfnType::Kind::kBool), in, EmptyCtx(), &out),
      StatusIs(absl::StatusCode::kInvalidArgument));
}

// ── int ─────────────────────────────────────────────────────────────

class IntBoundaryRoundTrip : public ::testing::TestWithParam<int64_t> {};

TEST_P(IntBoundaryRoundTrip, LiftThenInspect) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kInt),
                                 Value::Int(GetParam()), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_S64);
  EXPECT_EQ(out.cref().of.s64, GetParam());
}

TEST_P(IntBoundaryRoundTrip, LowerThenInspect) {
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_S64;
  in.of.s64 = GetParam();
  Value out;
  ASSERT_THAT(
      LowerComponentToCel(Prim(CelfnType::Kind::kInt), in, EmptyCtx(), &out),
      IsOk());
  ASSERT_EQ(out.kind(), Value::Kind::kInt);
  EXPECT_EQ(*out.AsInt(), GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Boundary, IntBoundaryRoundTrip,
    ::testing::Values(int64_t{0}, int64_t{-1}, int64_t{1},
                      std::numeric_limits<int64_t>::min(),
                      std::numeric_limits<int64_t>::max()));

TEST(LiftCelToComponent, IntFromUintValueIsKindMismatch) {
  OwnedComponentVal out;
  EXPECT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kInt), Value::Uint(1),
                                 EmptyCtx(), out.get()),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ── uint ────────────────────────────────────────────────────────────

class UintBoundaryRoundTrip : public ::testing::TestWithParam<uint64_t> {};

TEST_P(UintBoundaryRoundTrip, LiftThenInspect) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(Prim(CelfnType::Kind::kUint), Value::Uint(GetParam()),
                         EmptyCtx(), out.get()),
      IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_U64);
  EXPECT_EQ(out.cref().of.u64, GetParam());
}

TEST_P(UintBoundaryRoundTrip, LowerThenInspect) {
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_U64;
  in.of.u64 = GetParam();
  Value out;
  ASSERT_THAT(
      LowerComponentToCel(Prim(CelfnType::Kind::kUint), in, EmptyCtx(), &out),
      IsOk());
  EXPECT_EQ(*out.AsUint(), GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Boundary, UintBoundaryRoundTrip,
    ::testing::Values(uint64_t{0}, uint64_t{1},
                      std::numeric_limits<uint64_t>::max()));

// ── double ─────────────────────────────────────────────────────────

TEST(LiftCelToComponent, DoublePositiveZero) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDouble),
                                 Value::Double(0.0), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_F64);
  EXPECT_EQ(out.cref().of.f64, 0.0);
  // Signed-zero distinction must be preserved by the bit copy.
  EXPECT_FALSE(std::signbit(out.cref().of.f64));
}

TEST(LiftCelToComponent, DoubleNegativeZero) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDouble),
                                 Value::Double(-0.0), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_TRUE(std::signbit(out.cref().of.f64));
}

TEST(LiftCelToComponent, DoubleNaN) {
  OwnedComponentVal out;
  const double nan = std::nan("");
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDouble),
                                 Value::Double(nan), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_TRUE(std::isnan(out.cref().of.f64));
}

TEST(LiftCelToComponent, DoublePositiveInfinity) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(Prim(CelfnType::Kind::kDouble),
                         Value::Double(std::numeric_limits<double>::infinity()),
                         EmptyCtx(), out.get()),
      IsOk());
  EXPECT_TRUE(std::isinf(out.cref().of.f64));
  EXPECT_GT(out.cref().of.f64, 0.0);
}

TEST(LiftCelToComponent, DoubleNegativeInfinity) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(
                  Prim(CelfnType::Kind::kDouble),
                  Value::Double(-std::numeric_limits<double>::infinity()),
                  EmptyCtx(), out.get()),
              IsOk());
  EXPECT_TRUE(std::isinf(out.cref().of.f64));
  EXPECT_LT(out.cref().of.f64, 0.0);
}

TEST(LiftCelToComponent, DoubleMinAndMax) {
  for (double v :
       {std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
        -std::numeric_limits<double>::max()}) {
    OwnedComponentVal out;
    ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDouble),
                                   Value::Double(v), EmptyCtx(), out.get()),
                IsOk())
        << "v=" << v;
    EXPECT_EQ(out.cref().of.f64, v);
  }
}

// ── null ────────────────────────────────────────────────────────────

TEST(LiftCelToComponent, NullEncodesAsOptionNone) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kNull), Value::Null(),
                                 EmptyCtx(), out.get()),
              IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_OPTION);
  EXPECT_EQ(out.cref().of.option, nullptr);  // none == nullptr payload
}

TEST(LowerComponentToCel, NullFromOptionNone) {
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_OPTION;
  in.of.option = nullptr;
  Value out = Value::Int(99);  // pre-poison
  ASSERT_THAT(
      LowerComponentToCel(Prim(CelfnType::Kind::kNull), in, EmptyCtx(), &out),
      IsOk());
  EXPECT_TRUE(out.IsNull());
}

// ── string ─────────────────────────────────────────────────────────

// Helper: round-trip a string Value through Lift + Lower, asserting
// the resulting Value equals the input byte-for-byte.  Sized parameter
// keeps `embedded NUL` from terminating early.
void LiftLowerStringRoundTrip(absl::string_view payload) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kString),
                                 Value::String(std::string(payload)),
                                 EmptyCtx(), out.get()),
              IsOk());
  ASSERT_EQ(out.cref().kind, WASMTIME_COMPONENT_STRING);
  ASSERT_EQ(out.cref().of.string.size, payload.size());
  EXPECT_EQ(0, std::memcmp(out.cref().of.string.data, payload.data(),
                           payload.size()));

  Value back;
  ASSERT_THAT(LowerComponentToCel(Prim(CelfnType::Kind::kString), out.cref(),
                                  EmptyCtx(), &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kString);
  EXPECT_EQ(*back.AsString(), payload);
}

TEST(LiftCelToComponent, StringEmpty) {
  LiftLowerStringRoundTrip("");
}
TEST(LiftCelToComponent, StringAscii) {
  LiftLowerStringRoundTrip("hello world");
}
TEST(LiftCelToComponent, StringWithEmbeddedNul) {
  // 5 bytes: "a\0b\0c" — embedded NULs must survive because the
  // wasm_name_t form uses size, not a NUL terminator.
  LiftLowerStringRoundTrip(absl::string_view("a\0b\0c", 5));
}
TEST(LiftCelToComponent, StringMultiByteUtf8) {
  // "héllo 世界 ✓" — mixes 2-, 3-, and 4-byte (BMP) UTF-8.
  LiftLowerStringRoundTrip(
      "h\xc3\xa9llo \xe4\xb8\x96\xe7\x95\x8c "
      "\xe2\x9c\x93");
}
TEST(LiftCelToComponent, StringLong) {
  // 5 KB to exercise the heap-alloc path of wasm_byte_vec_new.
  std::string s(5 * 1024, 'x');
  LiftLowerStringRoundTrip(s);
}
TEST(LowerComponentToCel, StringRoundTripsEmbeddedNul) {
  // Lower-direction explicit pin: construct a component val with an
  // embedded-NUL payload from scratch and verify Lower copies the
  // full size.
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_STRING;
  const char payload[] = {'x', '\0', 'y'};
  wasm_byte_vec_new(&in.of.string, 3, payload);
  Value out;
  ASSERT_THAT(
      LowerComponentToCel(Prim(CelfnType::Kind::kString), in, EmptyCtx(), &out),
      IsOk());
  ASSERT_EQ(out.kind(), Value::Kind::kString);
  EXPECT_EQ(out.AsString()->size(), 3u);
  EXPECT_EQ(out.AsString()->at(1), '\0');
  wasmtime_component_val_delete(&in);
}

// ── bytes ──────────────────────────────────────────────────────────

void LiftLowerBytesRoundTrip(absl::string_view payload) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kBytes),
                                 Value::Bytes(std::string(payload)), EmptyCtx(),
                                 out.get()),
              IsOk());
  ASSERT_EQ(out.cref().kind, WASMTIME_COMPONENT_LIST);
  ASSERT_EQ(out.cref().of.list.size, payload.size());
  for (size_t i = 0; i < payload.size(); ++i) {
    EXPECT_EQ(out.cref().of.list.data[i].kind, WASMTIME_COMPONENT_U8);
    EXPECT_EQ(out.cref().of.list.data[i].of.u8,
              static_cast<uint8_t>(payload[i]))
        << "byte " << i;
  }
  Value back;
  ASSERT_THAT(LowerComponentToCel(Prim(CelfnType::Kind::kBytes), out.cref(),
                                  EmptyCtx(), &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kBytes);
  EXPECT_EQ(*back.AsBytes(), payload);
}

TEST(LiftCelToComponent, BytesEmpty) {
  LiftLowerBytesRoundTrip("");
}
TEST(LiftCelToComponent, BytesWithNul) {
  // Single 0x00 byte — bytes payload, NUL is data not a terminator.
  LiftLowerBytesRoundTrip(absl::string_view("\x00", 1));
}
TEST(LiftCelToComponent, BytesFFRun) {
  // Verifies no signed-widening when copying through char →
  // uint8_t.  Six 0xFF bytes appear as decimal 255, not -1.
  LiftLowerBytesRoundTrip("\xff\xff\xff\xff\xff\xff");
}
TEST(LiftCelToComponent, BytesLong) {
  std::string b(2048, '\x42');
  LiftLowerBytesRoundTrip(b);
}

TEST(LowerComponentToCel, BytesElementKindMismatchNamesIndex) {
  // Cross-element invariant break: index-1 element is s64, not u8.
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_LIST;
  wasmtime_component_vallist_new_uninit(&in.of.list, 2);
  in.of.list.data[0].kind = WASMTIME_COMPONENT_U8;
  in.of.list.data[0].of.u8 = 0x10;
  in.of.list.data[1].kind = WASMTIME_COMPONENT_S64;
  in.of.list.data[1].of.s64 = 99;
  Value out;
  auto s =
      LowerComponentToCel(Prim(CelfnType::Kind::kBytes), in, EmptyCtx(), &out);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("index 1"));
  wasmtime_component_val_delete(&in);
}

// ── duration / timestamp ───────────────────────────────────────────

// Helper: assert a lifted record has shape {seconds:s64=, nanos:s32=}
// for `expected_seconds`, `expected_nanos`.
void ExpectSecondsNanosRecord(const wasmtime_component_val_t& v,
                              int64_t expected_seconds,
                              int32_t expected_nanos) {
  ASSERT_EQ(v.kind, WASMTIME_COMPONENT_RECORD);
  ASSERT_EQ(v.of.record.size, 2u);
  // Field order is implementation-defined; check by name.
  int found_secs = -1, found_nanos = -1;
  for (size_t i = 0; i < v.of.record.size; ++i) {
    absl::string_view nm(v.of.record.data[i].name.data,
                         v.of.record.data[i].name.size);
    if (nm == "seconds") found_secs = static_cast<int>(i);
    if (nm == "nanos") found_nanos = static_cast<int>(i);
  }
  ASSERT_NE(found_secs, -1);
  ASSERT_NE(found_nanos, -1);
  EXPECT_EQ(v.of.record.data[found_secs].val.kind, WASMTIME_COMPONENT_S64);
  EXPECT_EQ(v.of.record.data[found_secs].val.of.s64, expected_seconds);
  EXPECT_EQ(v.of.record.data[found_nanos].val.kind, WASMTIME_COMPONENT_S32);
  EXPECT_EQ(v.of.record.data[found_nanos].val.of.s32, expected_nanos);
}

TEST(LiftCelToComponent, DurationZero) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDuration),
                                 Value::Duration(absl::ZeroDuration()),
                                 EmptyCtx(), out.get()),
              IsOk());
  ExpectSecondsNanosRecord(out.cref(), 0, 0);
}

TEST(LiftCelToComponent, DurationOneNanosecondPositive) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDuration),
                                 Value::Duration(absl::Nanoseconds(1)),
                                 EmptyCtx(), out.get()),
              IsOk());
  ExpectSecondsNanosRecord(out.cref(), 0, 1);
}

TEST(LiftCelToComponent, DurationOneNanosecondNegative) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDuration),
                                 Value::Duration(absl::Nanoseconds(-1)),
                                 EmptyCtx(), out.get()),
              IsOk());
  // -1 ns: seconds=0, nanos=-1 (IDivDuration truncates toward zero).
  ExpectSecondsNanosRecord(out.cref(), 0, -1);
}

TEST(LiftCelToComponent, DurationNanosBoundary999_999_999) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(Prim(CelfnType::Kind::kDuration),
                         Value::Duration(absl::Nanoseconds(999'999'999)),
                         EmptyCtx(), out.get()),
      IsOk());
  ExpectSecondsNanosRecord(out.cref(), 0, 999'999'999);
}

TEST(LiftCelToComponent, DurationOneSecondExactly) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDuration),
                                 Value::Duration(absl::Seconds(1)), EmptyCtx(),
                                 out.get()),
              IsOk());
  ExpectSecondsNanosRecord(out.cref(), 1, 0);
}

TEST(LiftCelToComponent, DurationNegativeMixed) {
  // -1.5 s split: seconds=-1, nanos=-500_000_000 (truncate-toward-zero
  // semantics — both halves share sign).
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDuration),
                                 Value::Duration(absl::Milliseconds(-1500)),
                                 EmptyCtx(), out.get()),
              IsOk());
  ExpectSecondsNanosRecord(out.cref(), -1, -500'000'000);
}

TEST(LowerComponentToCel, DurationRoundTrips) {
  for (absl::Duration d :
       {absl::ZeroDuration(), absl::Seconds(1), absl::Milliseconds(-1500),
        absl::Nanoseconds(999'999'999)}) {
    OwnedComponentVal out;
    ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kDuration),
                                   Value::Duration(d), EmptyCtx(), out.get()),
                IsOk())
        << absl::FormatDuration(d);
    Value back;
    ASSERT_THAT(LowerComponentToCel(Prim(CelfnType::Kind::kDuration),
                                    out.cref(), EmptyCtx(), &back),
                IsOk());
    EXPECT_EQ(*back.AsDuration(), d);
  }
}

TEST(LowerComponentToCel, DurationNanosOutOfRangeRejected) {
  // Construct a malformed record with nanos = 1_000_000_000 (langdef
  // §Duration mandates |nanos| < 1e9).  Lower must refuse rather than
  // silently produce a degenerate Duration.
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_RECORD;
  wasmtime_component_valrecord_new_uninit(&in.of.record, 2);
  wasm_name_new_from_string(&in.of.record.data[0].name, "seconds");
  in.of.record.data[0].val.kind = WASMTIME_COMPONENT_S64;
  in.of.record.data[0].val.of.s64 = 0;
  wasm_name_new_from_string(&in.of.record.data[1].name, "nanos");
  in.of.record.data[1].val.kind = WASMTIME_COMPONENT_S32;
  in.of.record.data[1].val.of.s32 = 1'000'000'000;
  Value out;
  EXPECT_THAT(LowerComponentToCel(Prim(CelfnType::Kind::kDuration), in,
                                  EmptyCtx(), &out),
              StatusIs(absl::StatusCode::kOutOfRange));
  wasmtime_component_val_delete(&in);
}

TEST(LowerComponentToCel, TimestampNanosMustBeNonNegative) {
  // google.protobuf.Timestamp invariant: nanos ∈ [0, 1e9).  Lower for
  // timestamp rejects nanos<0 even though duration accepts it.
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_RECORD;
  wasmtime_component_valrecord_new_uninit(&in.of.record, 2);
  wasm_name_new_from_string(&in.of.record.data[0].name, "seconds");
  in.of.record.data[0].val.kind = WASMTIME_COMPONENT_S64;
  in.of.record.data[0].val.of.s64 = 0;
  wasm_name_new_from_string(&in.of.record.data[1].name, "nanos");
  in.of.record.data[1].val.kind = WASMTIME_COMPONENT_S32;
  in.of.record.data[1].val.of.s32 = -1;
  Value out;
  EXPECT_THAT(LowerComponentToCel(Prim(CelfnType::Kind::kTimestamp), in,
                                  EmptyCtx(), &out),
              StatusIs(absl::StatusCode::kOutOfRange));
  wasmtime_component_val_delete(&in);
}

TEST(LowerComponentToCel, DurationRecordMissingFieldNamedRejected) {
  // {seconds, foo} record — Lower must name the unexpected field.
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_RECORD;
  wasmtime_component_valrecord_new_uninit(&in.of.record, 2);
  wasm_name_new_from_string(&in.of.record.data[0].name, "seconds");
  in.of.record.data[0].val.kind = WASMTIME_COMPONENT_S64;
  in.of.record.data[0].val.of.s64 = 0;
  wasm_name_new_from_string(&in.of.record.data[1].name, "foo");
  in.of.record.data[1].val.kind = WASMTIME_COMPONENT_S32;
  in.of.record.data[1].val.of.s32 = 0;
  Value out;
  auto s = LowerComponentToCel(Prim(CelfnType::Kind::kDuration), in, EmptyCtx(),
                               &out);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("foo"));
  wasmtime_component_val_delete(&in);
}

TEST(LiftCelToComponent, TimestampZero) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kTimestamp),
                                 Value::Timestamp(absl::FromUnixSeconds(0)),
                                 EmptyCtx(), out.get()),
              IsOk());
  ExpectSecondsNanosRecord(out.cref(), 0, 0);
}

TEST(LiftCelToComponent, TimestampLargePositive) {
  // 2026-01-01 00:00:00 UTC: 1767225600 seconds since epoch.  No
  // sub-second part → nanos=0.
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(Prim(CelfnType::Kind::kTimestamp),
                         Value::Timestamp(absl::FromUnixSeconds(1'767'225'600)),
                         EmptyCtx(), out.get()),
      IsOk());
  ExpectSecondsNanosRecord(out.cref(), 1'767'225'600, 0);
}

TEST(LowerComponentToCel, TimestampRoundTrips) {
  const absl::Time t =
      absl::FromUnixSeconds(1'767'225'600) + absl::Nanoseconds(123'456'789);
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kTimestamp),
                                 Value::Timestamp(t), EmptyCtx(), out.get()),
              IsOk());
  Value back;
  ASSERT_THAT(LowerComponentToCel(Prim(CelfnType::Kind::kTimestamp), out.cref(),
                                  EmptyCtx(), &back),
              IsOk());
  EXPECT_EQ(*back.AsTimestamp(), t);
}

// ── list<T> ────────────────────────────────────────────────────────

TEST(LiftCelToComponent, ListIntEmpty) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(ListOf(Prim(CelfnType::Kind::kInt)),
                                 Value::List({}), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_LIST);
  EXPECT_EQ(out.cref().of.list.size, 0u);
}

TEST(LiftCelToComponent, ListIntSingleElement) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(ListOf(Prim(CelfnType::Kind::kInt)),
                         Value::List({Value::Int(42)}), EmptyCtx(), out.get()),
      IsOk());
  ASSERT_EQ(out.cref().of.list.size, 1u);
  EXPECT_EQ(out.cref().of.list.data[0].kind, WASMTIME_COMPONENT_S64);
  EXPECT_EQ(out.cref().of.list.data[0].of.s64, 42);
}

TEST(LiftCelToComponent, ListIntContainsBoundary) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(
          ListOf(Prim(CelfnType::Kind::kInt)),
          Value::List({Value::Int(std::numeric_limits<int64_t>::min()),
                       Value::Int(0),
                       Value::Int(std::numeric_limits<int64_t>::max())}),
          EmptyCtx(), out.get()),
      IsOk());
  ASSERT_EQ(out.cref().of.list.size, 3u);
  EXPECT_EQ(out.cref().of.list.data[0].of.s64,
            std::numeric_limits<int64_t>::min());
  EXPECT_EQ(out.cref().of.list.data[1].of.s64, 0);
  EXPECT_EQ(out.cref().of.list.data[2].of.s64,
            std::numeric_limits<int64_t>::max());
}

TEST(LiftCelToComponent, ListListIntRaggedNesting) {
  // [[1,2], [], [3]] — ragged inner lengths.
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(
                  ListOf(ListOf(Prim(CelfnType::Kind::kInt))),
                  Value::List({Value::List({Value::Int(1), Value::Int(2)}),
                               Value::List({}), Value::List({Value::Int(3)})}),
                  EmptyCtx(), out.get()),
              IsOk());
  ASSERT_EQ(out.cref().of.list.size, 3u);
  EXPECT_EQ(out.cref().of.list.data[0].of.list.size, 2u);
  EXPECT_EQ(out.cref().of.list.data[1].of.list.size, 0u);
  EXPECT_EQ(out.cref().of.list.data[2].of.list.size, 1u);
  EXPECT_EQ(out.cref().of.list.data[0].of.list.data[1].of.s64, 2);
  EXPECT_EQ(out.cref().of.list.data[2].of.list.data[0].of.s64, 3);
}

TEST(LiftCelToComponent, ListIntCrossKindElementErrorIncludesIndex) {
  // index 1 has wrong inner kind (kUint, not kInt).
  OwnedComponentVal out;
  auto s = LiftCelToComponent(
      ListOf(Prim(CelfnType::Kind::kInt)),
      Value::List({Value::Int(0), Value::Uint(1), Value::Int(2)}), EmptyCtx(),
      out.get());
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("element 1"));
}

TEST(LowerComponentToCel, ListIntRoundTrip) {
  OwnedComponentVal lifted;
  ASSERT_THAT(LiftCelToComponent(
                  ListOf(Prim(CelfnType::Kind::kInt)),
                  Value::List({Value::Int(-7), Value::Int(0), Value::Int(7)}),
                  EmptyCtx(), lifted.get()),
              IsOk());
  Value back;
  ASSERT_THAT(LowerComponentToCel(ListOf(Prim(CelfnType::Kind::kInt)),
                                  lifted.cref(), EmptyCtx(), &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kList);
  auto backing_or = back.ListBacking();
  ASSERT_THAT(backing_or, IsOk());
  ASSERT_EQ((*backing_or)->Size(), 3u);
  const CelType placeholder = CelType::Int();
  auto e0 = (*backing_or)->At(0, placeholder);
  auto e2 = (*backing_or)->At(2, placeholder);
  ASSERT_THAT(e0, IsOk());
  ASSERT_THAT(e2, IsOk());
  EXPECT_EQ(*e0->AsInt(), -7);
  EXPECT_EQ(*e2->AsInt(), 7);
}

// ── map<K,V> ───────────────────────────────────────────────────────

TEST(LiftCelToComponent, MapStringIntEmpty) {
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(MapOf(Prim(CelfnType::Kind::kString),
                                       Prim(CelfnType::Kind::kInt)),
                                 Value::Map({}), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_LIST);
  EXPECT_EQ(out.cref().of.list.size, 0u);
}

TEST(LiftCelToComponent, MapAllKeyKindsBool) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(
          MapOf(Prim(CelfnType::Kind::kBool), Prim(CelfnType::Kind::kInt)),
          Value::Map({{Value::Bool(true), Value::Int(1)},
                      {Value::Bool(false), Value::Int(0)}}),
          EmptyCtx(), out.get()),
      IsOk());
  ASSERT_EQ(out.cref().of.list.size, 2u);
  EXPECT_EQ(out.cref().of.list.data[0].kind, WASMTIME_COMPONENT_TUPLE);
  EXPECT_EQ(out.cref().of.list.data[0].of.tuple.data[0].kind,
            WASMTIME_COMPONENT_BOOL);
}

TEST(LiftCelToComponent, MapAllKeyKindsInt) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(
          MapOf(Prim(CelfnType::Kind::kInt), Prim(CelfnType::Kind::kString)),
          Value::Map({{Value::Int(7), Value::String("a")}}), EmptyCtx(),
          out.get()),
      IsOk());
  ASSERT_EQ(out.cref().of.list.size, 1u);
  EXPECT_EQ(out.cref().of.list.data[0].of.tuple.data[0].kind,
            WASMTIME_COMPONENT_S64);
  EXPECT_EQ(out.cref().of.list.data[0].of.tuple.data[0].of.s64, 7);
}

TEST(LiftCelToComponent, MapAllKeyKindsUint) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(
          MapOf(Prim(CelfnType::Kind::kUint), Prim(CelfnType::Kind::kInt)),
          Value::Map({{Value::Uint(42), Value::Int(-1)}}), EmptyCtx(),
          out.get()),
      IsOk());
  EXPECT_EQ(out.cref().of.list.data[0].of.tuple.data[0].kind,
            WASMTIME_COMPONENT_U64);
}

TEST(LiftCelToComponent, MapAllKeyKindsString) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(
          MapOf(Prim(CelfnType::Kind::kString), Prim(CelfnType::Kind::kInt)),
          Value::Map({{Value::String("k"), Value::Int(9)}}), EmptyCtx(),
          out.get()),
      IsOk());
  EXPECT_EQ(out.cref().of.list.data[0].of.tuple.data[0].kind,
            WASMTIME_COMPONENT_STRING);
}

TEST(LiftCelToComponent, MapDoubleKeyRejectedAtType) {
  // Pre-construct: a map<double, int> shape is structurally rejected
  // even though the host Value::Map could carry double keys.
  OwnedComponentVal out;
  auto s = LiftCelToComponent(
      MapOf(Prim(CelfnType::Kind::kDouble), Prim(CelfnType::Kind::kInt)),
      Value::Map({}), EmptyCtx(), out.get());
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("double"));
}

TEST(LiftCelToComponent, MapWithListValue) {
  // {"k": []} — value is an empty list; verifies the value type-witness
  // recurses through the value side too.
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(MapOf(Prim(CelfnType::Kind::kString),
                               ListOf(Prim(CelfnType::Kind::kInt))),
                         Value::Map({{Value::String("k"), Value::List({})}}),
                         EmptyCtx(), out.get()),
      IsOk());
  ASSERT_EQ(out.cref().of.list.size, 1u);
  EXPECT_EQ(out.cref().of.list.data[0].of.tuple.data[1].kind,
            WASMTIME_COMPONENT_LIST);
  EXPECT_EQ(out.cref().of.list.data[0].of.tuple.data[1].of.list.size, 0u);
}

TEST(LowerComponentToCel, MapStringIntRoundTrip) {
  OwnedComponentVal lifted;
  ASSERT_THAT(
      LiftCelToComponent(
          MapOf(Prim(CelfnType::Kind::kString), Prim(CelfnType::Kind::kInt)),
          Value::Map({{Value::String("alpha"), Value::Int(1)},
                      {Value::String("beta"), Value::Int(2)}}),
          EmptyCtx(), lifted.get()),
      IsOk());
  Value back;
  ASSERT_THAT(LowerComponentToCel(MapOf(Prim(CelfnType::Kind::kString),
                                        Prim(CelfnType::Kind::kInt)),
                                  lifted.cref(), EmptyCtx(), &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kMap);
  auto backing_or = back.MapBacking();
  ASSERT_THAT(backing_or, IsOk());
  EXPECT_EQ((*backing_or)->Size(), 2u);
}

TEST(LowerComponentToCel, MapTupleArityMismatchRejected) {
  // Construct a malformed list<tuple> where one tuple has 3 elements.
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_LIST;
  wasmtime_component_vallist_new_uninit(&in.of.list, 1);
  in.of.list.data[0].kind = WASMTIME_COMPONENT_TUPLE;
  wasmtime_component_valtuple_new_uninit(&in.of.list.data[0].of.tuple, 3);
  for (size_t i = 0; i < 3; ++i) {
    in.of.list.data[0].of.tuple.data[i].kind = WASMTIME_COMPONENT_S64;
    in.of.list.data[0].of.tuple.data[i].of.s64 = 0;
  }
  Value out;
  auto s = LowerComponentToCel(
      MapOf(Prim(CelfnType::Kind::kInt), Prim(CelfnType::Kind::kInt)), in,
      EmptyCtx(), &out);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("arity 3"));
  wasmtime_component_val_delete(&in);
}

// ── optional<T> ────────────────────────────────────────────────────
//
// optional<T> as a declarable argument / return shape was dropped from
// v1 of the component-backend (user direction, 2026-06-03).  CEL
// `null` still encodes as WIT `option<unit>` on the wire; that is a
// wire detail and does not require a user-facing `optional` type.
// Lift for any kOptional CelfnType must permanently refuse, not stub.

TEST(LiftCelToComponent, OptionalArgIsPermanentlyRejected) {
  CelfnType opt;
  opt.kind = CelfnType::Kind::kOptional;
  CelfnType inner;
  inner.kind = CelfnType::Kind::kInt;
  opt.optional_element.push_back(inner);
  OwnedComponentVal out;
  // Value side doesn't carry an optional kind — any Value mismatches.
  // The Lift dispatcher must refuse on the type alone, not silently
  // proceed.
  auto s = LiftCelToComponent(opt, Value::Int(7), EmptyCtx(), out.get());
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("optional"));
}

// ── proto(fqn) ─────────────────────────────────────────────────────

// Force-link the Customer descriptor into the generated pool so the
// Lower side's MessageFactory::GetPrototype lookup resolves.  Without
// this, a binary that doesn't directly reference Customer would not
// have it registered with the generated pool.
[[maybe_unused]] const int kProtoDescriptorsLinked = [] {
  google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
  return 0;
}();

CelfnType ProtoOfFqn(absl::string_view fqn) {
  CelfnType t;
  t.kind = CelfnType::Kind::kProto;
  t.proto_fqn = std::string(fqn);
  return t;
}

TEST(LiftCelToComponent, ProtoEmptyMessage) {
  // Lift an empty Customer; bytes should be zero-length.
  celwasm::testdata::Customer c;
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(ProtoOfFqn("celwasm.testdata.Customer"),
                                 Value::Message(c), EmptyCtx(), out.get()),
              IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_LIST);
  // An empty proto serialises to zero bytes.
  EXPECT_EQ(out.cref().of.list.size, 0u);
}

TEST(LiftCelToComponent, ProtoAllFieldsSet) {
  // A populated Customer round-trips through Lift+Lower preserving
  // all set fields.
  celwasm::testdata::Customer c;
  c.set_name("Alice");
  c.set_user_id(42);
  c.set_age(33);
  OwnedComponentVal lifted;
  ASSERT_THAT(LiftCelToComponent(ProtoOfFqn("celwasm.testdata.Customer"),
                                 Value::Message(c), EmptyCtx(), lifted.get()),
              IsOk());
  EXPECT_EQ(lifted.cref().kind, WASMTIME_COMPONENT_LIST);
  EXPECT_GT(lifted.cref().of.list.size, 0u);

  CelComponentContext ctx;
  ctx.pool = google::protobuf::DescriptorPool::generated_pool();
  Value back;
  ASSERT_THAT(LowerComponentToCel(ProtoOfFqn("celwasm.testdata.Customer"),
                                  lifted.cref(), ctx, &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kMessage);
  auto backing_or = back.MessageBacking();
  ASSERT_THAT(backing_or, IsOk());
  const auto* m = (*backing_or)->message();
  ASSERT_NE(m, nullptr);
  const auto* c2 = dynamic_cast<const celwasm::testdata::Customer*>(m);
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c2->user_id(), 42);
  EXPECT_EQ(c2->name(), "Alice");
  EXPECT_EQ(c2->age(), 33);
}

TEST(LowerComponentToCel, ProtoMissingPoolRejected) {
  // No pool in context → InvalidArgument naming the fqn.
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_LIST;
  wasmtime_component_vallist_new_uninit(&in.of.list, 0);
  Value out;
  auto s = LowerComponentToCel(ProtoOfFqn("celwasm.testdata.Customer"), in,
                               EmptyCtx() /* pool=nullptr */, &out);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("pool"));
  wasmtime_component_val_delete(&in);
}

TEST(LowerComponentToCel, ProtoUnknownFqnRejected) {
  CelComponentContext ctx;
  ctx.pool = google::protobuf::DescriptorPool::generated_pool();
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_LIST;
  wasmtime_component_vallist_new_uninit(&in.of.list, 0);
  Value out;
  auto s = LowerComponentToCel(ProtoOfFqn("does.not.Exist"), in, ctx, &out);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("does.not.Exist"));
  wasmtime_component_val_delete(&in);
}

TEST(LowerComponentToCel, ProtoBadBytesRejectedWithDescriptorName) {
  // Construct a list<u8> with garbage bytes; ParseFromString rejects.
  CelComponentContext ctx;
  ctx.pool = google::protobuf::DescriptorPool::generated_pool();
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_LIST;
  // 0xFF 0xFF — invalid varint header, well-formed tag would expect
  // continuation.
  wasmtime_component_vallist_new_uninit(&in.of.list, 2);
  in.of.list.data[0].kind = WASMTIME_COMPONENT_U8;
  in.of.list.data[0].of.u8 = 0xFF;
  in.of.list.data[1].kind = WASMTIME_COMPONENT_U8;
  in.of.list.data[1].of.u8 = 0xFF;
  Value out;
  auto s = LowerComponentToCel(ProtoOfFqn("celwasm.testdata.Customer"), in, ctx,
                               &out);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("celwasm.testdata.Customer"));
  wasmtime_component_val_delete(&in);
}

// ── type ───────────────────────────────────────────────────────────

TEST(LiftCelToComponent, TypeAsString) {
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(Prim(CelfnType::Kind::kType), Value::Type("acme.User"),
                         EmptyCtx(), out.get()),
      IsOk());
  EXPECT_EQ(out.cref().kind, WASMTIME_COMPONENT_STRING);
  EXPECT_EQ(std::string(out.cref().of.string.data, out.cref().of.string.size),
            "acme.User");
}

TEST(LiftCelToComponent, TypeFromIntValueIsKindMismatch) {
  OwnedComponentVal out;
  EXPECT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kType), Value::Int(0),
                                 EmptyCtx(), out.get()),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ── cross-kind defence-in-depth ────────────────────────────────────

TEST(LowerComponentToCel, IntKindMismatchNamesBothSides) {
  // Defence in depth: even if FuncType validation at AddComponent
  // missed a mismatch, Lower must refuse rather than miscompile.
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_F64;
  in.of.f64 = 3.0;
  Value out;
  auto s =
      LowerComponentToCel(Prim(CelfnType::Kind::kInt), in, EmptyCtx(), &out);
  ASSERT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(s.message()), HasSubstr("int"));
}

TEST(LowerComponentToCel, UintKindMismatchNamesBothSides) {
  wasmtime_component_val_t in{};
  in.kind = WASMTIME_COMPONENT_S64;
  in.of.s64 = -1;
  Value out;
  EXPECT_THAT(
      LowerComponentToCel(Prim(CelfnType::Kind::kUint), in, EmptyCtx(), &out),
      StatusIs(absl::StatusCode::kInvalidArgument));
}

// ── Large-payload boundary matrix (variable-length types) ──────────
//
// The KB-scale "Long" cases above exercise the heap-alloc path of
// wasm_byte_vec_new / vallist_new_uninit, but only just.  These cases
// drive payloads at MiB / 10⁴–10⁵ scale to surface bugs that only
// appear under genuine memory pressure:
//
//   - signed-width / size_t-overflow assumptions baked into the
//     canonical-ABI copy paths (e.g. `int` length counters);
//   - allocator pathologies — an O(n²) inner copy or a quadratic
//     descriptor walk that's invisible at 5 KB but timeouts here;
//   - element-walk loops that touch every node by index — a missing
//     `+= 1` lap would deadlock at this scale;
//   - regression-pin against re-introducing a per-element host
//     callback that ate Lift cost (m23 §5 measured ~500 ns/node for
//     the value-resource path; the v1 typed-WIT path is one crossing
//     + one O(n) memcpy, so 10⁵ ints stays bounded).
//
// Scales picked to balance coverage vs test wall-time: 256 KiB for
// blob types (strings, bytes), 100 000 for flat collections, and
// nested-aggregate cases bounded at 10 000 leaf cells.  The patterns
// chosen (counters with a non-trivial varying prefix) defeat any
// run-length-style accidental optimisation that would mask a bug.

// Scale knobs — central so a future tuning bump touches one place.
constexpr size_t kLargeBlobBytes = 256 * 1024;  // 256 KiB
constexpr size_t kLargeListInts = 100'000;      // 10⁵ scalars
constexpr size_t kLargeStrings = 1024;          // strings in a list
constexpr size_t kLargeStringLen = 1024;        // 1 KiB each → 1 MiB total
constexpr size_t kLargeMapEntries = 10'000;     // 10⁴ map entries
constexpr size_t kLargeProtoRepeats = 10'000;   // 10⁴ repeated strings
constexpr size_t kNestedOuter = 100;            // 100 × 100 = 10⁴ leaves
constexpr size_t kNestedInner = 100;

// Build a deterministic, non-uniform payload so a bug that zeros or
// runs-length-collapses elements is detectable.  Each byte is its index
// XOR'd with a small prime, then mixed back into the low bits to keep
// the value varying every byte.  Used for `string` (raw / CEL-side,
// not proto3 wire), `bytes`, and bytes-field proto cases.
std::string MakeLargeBlob(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) {
    s[i] = static_cast<char>((i * 1103515245u + 12345u) ^ (i >> 3));
  }
  return s;
}

// Same shape, but constrained to printable ASCII.  Proto3 `string`
// fields require valid UTF-8 — raw bytes from `MakeLargeBlob` would
// fail SerializeToString's UTF-8 check.
std::string MakeLargeAsciiBlob(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) {
    // ASCII printable [32, 126] — 95 distinct values, non-trivial spread.
    s[i] = static_cast<char>(32 + ((i * 1103515245u + 12345u) % 95));
  }
  return s;
}

TEST(LiftCelToComponent, LargeStringRoundTripsAtMiBScale) {
  const std::string payload = MakeLargeBlob(kLargeBlobBytes);
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kString),
                                 Value::String(payload), EmptyCtx(), out.get()),
              IsOk());
  ASSERT_EQ(out.cref().kind, WASMTIME_COMPONENT_STRING);
  ASSERT_EQ(out.cref().of.string.size, payload.size());
  // Byte-exact match (not just length): catches a memcpy that copies
  // n-1 bytes / silently truncates / pads with zeros.
  EXPECT_EQ(0, std::memcmp(out.cref().of.string.data, payload.data(),
                           payload.size()));

  Value back;
  ASSERT_THAT(LowerComponentToCel(Prim(CelfnType::Kind::kString), out.cref(),
                                  EmptyCtx(), &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kString);
  EXPECT_EQ(back.AsString()->size(), payload.size());
  EXPECT_EQ(*back.AsString(), payload);
}

TEST(LiftCelToComponent, LargeBytesRoundTripsAtMiBScale) {
  const std::string payload = MakeLargeBlob(kLargeBlobBytes);
  OwnedComponentVal out;
  ASSERT_THAT(LiftCelToComponent(Prim(CelfnType::Kind::kBytes),
                                 Value::Bytes(payload), EmptyCtx(), out.get()),
              IsOk());
  ASSERT_EQ(out.cref().kind, WASMTIME_COMPONENT_LIST);
  ASSERT_EQ(out.cref().of.list.size, payload.size());
  // Bytes encode as list<u8>; verify a handful of bytes (full sweep
  // would dominate test runtime — head + middle + tail catches the
  // off-by-one + truncation cases).
  EXPECT_EQ(out.cref().of.list.data[0].kind, WASMTIME_COMPONENT_U8);
  EXPECT_EQ(out.cref().of.list.data[0].of.u8, static_cast<uint8_t>(payload[0]));
  const size_t mid = payload.size() / 2;
  EXPECT_EQ(out.cref().of.list.data[mid].of.u8,
            static_cast<uint8_t>(payload[mid]));
  EXPECT_EQ(out.cref().of.list.data[payload.size() - 1].of.u8,
            static_cast<uint8_t>(payload.back()));

  Value back;
  ASSERT_THAT(LowerComponentToCel(Prim(CelfnType::Kind::kBytes), out.cref(),
                                  EmptyCtx(), &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kBytes);
  EXPECT_EQ(*back.AsBytes(), payload);
}

TEST(LiftCelToComponent, LargeListIntRoundTripsAt100kElements) {
  std::vector<Value> elems;
  elems.reserve(kLargeListInts);
  for (size_t i = 0; i < kLargeListInts; ++i) {
    // Non-monotone pattern: alternating sign, varying magnitude.  A
    // collapsed-to-zero allocator would fail head + tail checks; a
    // signed-vs-unsigned ladder bug would diverge here.
    elems.push_back(
        Value::Int(static_cast<int64_t>(i) * ((i % 2 == 0) ? 1 : -1)));
  }
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(ListOf(Prim(CelfnType::Kind::kInt)),
                         Value::List(std::move(elems)), EmptyCtx(), out.get()),
      IsOk());
  ASSERT_EQ(out.cref().kind, WASMTIME_COMPONENT_LIST);
  ASSERT_EQ(out.cref().of.list.size, kLargeListInts);

  // Spot-check head, middle, tail (full sweep would dominate runtime).
  EXPECT_EQ(out.cref().of.list.data[0].of.s64, 0);
  EXPECT_EQ(out.cref().of.list.data[1].of.s64, -1);
  const size_t mid = kLargeListInts / 2;
  EXPECT_EQ(out.cref().of.list.data[mid].of.s64,
            static_cast<int64_t>(mid) * ((mid % 2 == 0) ? 1 : -1));
  const size_t last = kLargeListInts - 1;
  EXPECT_EQ(out.cref().of.list.data[last].of.s64,
            static_cast<int64_t>(last) * ((last % 2 == 0) ? 1 : -1));

  Value back;
  ASSERT_THAT(LowerComponentToCel(ListOf(Prim(CelfnType::Kind::kInt)),
                                  out.cref(), EmptyCtx(), &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kList);
  auto backing_or = back.ListBacking();
  ASSERT_THAT(backing_or, IsOk());
  ASSERT_EQ((*backing_or)->Size(), kLargeListInts);
  const CelType placeholder = CelType::Int();
  // Sparse check — same head/mid/tail discipline.
  for (size_t i : {size_t{0}, size_t{1}, mid, last}) {
    auto e = (*backing_or)->At(i, placeholder);
    ASSERT_THAT(e, IsOk()) << "i=" << i;
    EXPECT_EQ(*e->AsInt(), static_cast<int64_t>(i) * ((i % 2 == 0) ? 1 : -1))
        << "i=" << i;
  }
}

TEST(LiftCelToComponent, LargeListOfLargeStringsLiftsAtMiBScale) {
  // 1024 strings × 1 KiB each ≈ 1 MiB of payload across a non-trivial
  // outer list shape.  Catches a bug that allocates outer correctly
  // but per-inner-element under-allocates.
  std::vector<Value> elems;
  elems.reserve(kLargeStrings);
  for (size_t i = 0; i < kLargeStrings; ++i) {
    std::string s(kLargeStringLen, 'a' + static_cast<char>(i % 26));
    // Stamp the index at the front so a swapped/duplicated element is
    // detectable without sweeping every byte.
    s[0] = static_cast<char>((i >> 8) & 0xff);
    s[1] = static_cast<char>(i & 0xff);
    elems.push_back(Value::String(std::move(s)));
  }
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(ListOf(Prim(CelfnType::Kind::kString)),
                         Value::List(std::move(elems)), EmptyCtx(), out.get()),
      IsOk());
  ASSERT_EQ(out.cref().of.list.size, kLargeStrings);
  // Spot-check that head + tail elements have the right length AND the
  // right index-stamp — a sliced / duplicated element would still have
  // length kLargeStringLen but wrong stamp bytes.
  EXPECT_EQ(out.cref().of.list.data[0].kind, WASMTIME_COMPONENT_STRING);
  EXPECT_EQ(out.cref().of.list.data[0].of.string.size, kLargeStringLen);
  EXPECT_EQ(static_cast<uint8_t>(out.cref().of.list.data[0].of.string.data[0]),
            0u);
  EXPECT_EQ(static_cast<uint8_t>(out.cref().of.list.data[0].of.string.data[1]),
            0u);
  const size_t last = kLargeStrings - 1;
  EXPECT_EQ(out.cref().of.list.data[last].of.string.size, kLargeStringLen);
  EXPECT_EQ(
      static_cast<uint8_t>(out.cref().of.list.data[last].of.string.data[0]),
      (last >> 8) & 0xff);
  EXPECT_EQ(
      static_cast<uint8_t>(out.cref().of.list.data[last].of.string.data[1]),
      last & 0xff);
}

TEST(LiftCelToComponent, LargeNestedListLiftsAt10kLeafCells) {
  // list<list<int>>: 100 outer × 100 inner = 10 000 leaves.  Stress the
  // recursive Lift path with shape genuinely nested (B.6's ragged case
  // was 3×3 — too small to catch a recursion-depth allocator bug).
  std::vector<Value> outer;
  outer.reserve(kNestedOuter);
  for (size_t i = 0; i < kNestedOuter; ++i) {
    std::vector<Value> inner;
    inner.reserve(kNestedInner);
    for (size_t j = 0; j < kNestedInner; ++j) {
      inner.push_back(Value::Int(static_cast<int64_t>(i * kNestedInner + j)));
    }
    outer.push_back(Value::List(std::move(inner)));
  }
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(ListOf(ListOf(Prim(CelfnType::Kind::kInt))),
                         Value::List(std::move(outer)), EmptyCtx(), out.get()),
      IsOk());
  ASSERT_EQ(out.cref().of.list.size, kNestedOuter);
  for (size_t i : {size_t{0}, kNestedOuter / 2, kNestedOuter - 1}) {
    ASSERT_EQ(out.cref().of.list.data[i].kind, WASMTIME_COMPONENT_LIST);
    ASSERT_EQ(out.cref().of.list.data[i].of.list.size, kNestedInner);
    // Head leaf of row i should be i * kNestedInner; tail leaf
    // (i+1)*kNestedInner - 1.
    EXPECT_EQ(out.cref().of.list.data[i].of.list.data[0].of.s64,
              static_cast<int64_t>(i * kNestedInner));
    EXPECT_EQ(out.cref().of.list.data[i].of.list.data[kNestedInner - 1].of.s64,
              static_cast<int64_t>(i * kNestedInner + kNestedInner - 1));
  }
}

TEST(LiftCelToComponent, LargeMapStringIntRoundTripsAt10kEntries) {
  std::vector<std::pair<Value, Value>> entries;
  entries.reserve(kLargeMapEntries);
  for (size_t i = 0; i < kLargeMapEntries; ++i) {
    // Keys: "k_<padded-decimal>" so all distinct + a non-trivial
    // length spread (the 1-digit vs 5-digit boundaries cross at i=10
    // and i=10000).
    entries.emplace_back(Value::String(absl::StrCat("k_", i)),
                         Value::Int(static_cast<int64_t>(i)));
  }
  OwnedComponentVal lifted;
  ASSERT_THAT(
      LiftCelToComponent(
          MapOf(Prim(CelfnType::Kind::kString), Prim(CelfnType::Kind::kInt)),
          Value::Map(std::move(entries)), EmptyCtx(), lifted.get()),
      IsOk());
  ASSERT_EQ(lifted.cref().kind, WASMTIME_COMPONENT_LIST);
  ASSERT_EQ(lifted.cref().of.list.size, kLargeMapEntries);

  // Spot-check: every tuple is shape <STRING, S64>; a corrupt-write
  // bug at a fixed index (typically 0 or last) is caught here.
  for (size_t i : {size_t{0}, kLargeMapEntries / 2, kLargeMapEntries - 1}) {
    ASSERT_EQ(lifted.cref().of.list.data[i].kind, WASMTIME_COMPONENT_TUPLE);
    ASSERT_EQ(lifted.cref().of.list.data[i].of.tuple.size, 2u);
    EXPECT_EQ(lifted.cref().of.list.data[i].of.tuple.data[0].kind,
              WASMTIME_COMPONENT_STRING);
    EXPECT_EQ(lifted.cref().of.list.data[i].of.tuple.data[1].kind,
              WASMTIME_COMPONENT_S64);
  }

  Value back;
  ASSERT_THAT(LowerComponentToCel(MapOf(Prim(CelfnType::Kind::kString),
                                        Prim(CelfnType::Kind::kInt)),
                                  lifted.cref(), EmptyCtx(), &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kMap);
  auto backing_or = back.MapBacking();
  ASSERT_THAT(backing_or, IsOk());
  EXPECT_EQ((*backing_or)->Size(), kLargeMapEntries);
}

TEST(LiftCelToComponent, LargeMapStringListIntLiftsAt10kLeafCells) {
  // map<string, list<int>>: 100 keys × 100-element lists = 10⁴ leaves
  // through a non-trivial value-type recursion.  Catches a value-side
  // type-witness bug that B.7's single MapWithListValue (one entry,
  // empty list) wouldn't.
  std::vector<std::pair<Value, Value>> entries;
  entries.reserve(kNestedOuter);
  for (size_t i = 0; i < kNestedOuter; ++i) {
    std::vector<Value> inner;
    inner.reserve(kNestedInner);
    for (size_t j = 0; j < kNestedInner; ++j) {
      inner.push_back(Value::Int(static_cast<int64_t>(i * kNestedInner + j)));
    }
    entries.emplace_back(Value::String(absl::StrCat("g_", i)),
                         Value::List(std::move(inner)));
  }
  OwnedComponentVal out;
  ASSERT_THAT(
      LiftCelToComponent(MapOf(Prim(CelfnType::Kind::kString),
                               ListOf(Prim(CelfnType::Kind::kInt))),
                         Value::Map(std::move(entries)), EmptyCtx(), out.get()),
      IsOk());
  ASSERT_EQ(out.cref().of.list.size, kNestedOuter);
  // Spot-check one tuple's inner-list shape + magnitudes.
  ASSERT_EQ(out.cref().of.list.data[0].of.tuple.data[1].kind,
            WASMTIME_COMPONENT_LIST);
  ASSERT_EQ(out.cref().of.list.data[0].of.tuple.data[1].of.list.size,
            kNestedInner);
  EXPECT_EQ(out.cref().of.list.data[0].of.tuple.data[1].of.list.data[0].of.s64,
            0);
  EXPECT_EQ(out.cref()
                .of.list.data[kNestedOuter - 1]
                .of.tuple.data[1]
                .of.list.data[kNestedInner - 1]
                .of.s64,
            static_cast<int64_t>((kNestedOuter - 1) * kNestedInner +
                                 kNestedInner - 1));
}

TEST(LiftCelToComponent, LargeProtoLargeStringFieldLiftsAtMiBScale) {
  // Customer.name = 256 KiB.  Proto serialisation has its own
  // varint-length-prefixed copy path; this catches a length-prefix
  // overflow or a per-byte zigzag bug at the SerializePartialToString
  // boundary.  Uses an ASCII payload because proto3 `string` is
  // UTF-8 — see `MakeLargeAsciiBlob`; binary-payload coverage at the
  // proto layer is in `LargeProtoLargeBytesFieldLiftsAtMiBScale`.
  celwasm::testdata::Customer c;
  c.set_name(MakeLargeAsciiBlob(kLargeBlobBytes));
  c.set_user_id(7);
  OwnedComponentVal lifted;
  ASSERT_THAT(LiftCelToComponent(ProtoOfFqn("celwasm.testdata.Customer"),
                                 Value::Message(c), EmptyCtx(), lifted.get()),
              IsOk());
  EXPECT_EQ(lifted.cref().kind, WASMTIME_COMPONENT_LIST);
  // Serialised size dominated by the 256 KiB string payload.
  EXPECT_GE(lifted.cref().of.list.size, kLargeBlobBytes);

  CelComponentContext ctx;
  ctx.pool = google::protobuf::DescriptorPool::generated_pool();
  Value back;
  ASSERT_THAT(LowerComponentToCel(ProtoOfFqn("celwasm.testdata.Customer"),
                                  lifted.cref(), ctx, &back),
              IsOk());
  ASSERT_EQ(back.kind(), Value::Kind::kMessage);
  auto backing_or = back.MessageBacking();
  ASSERT_THAT(backing_or, IsOk());
  const auto* m = (*backing_or)->message();
  ASSERT_NE(m, nullptr);
  const auto* c2 = dynamic_cast<const celwasm::testdata::Customer*>(m);
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c2->name().size(), kLargeBlobBytes);
  EXPECT_EQ(c2->name(), c.name());
  EXPECT_EQ(c2->user_id(), 7);
}

TEST(LiftCelToComponent, LargeProtoLargeBytesFieldLiftsAtMiBScale) {
  // Customer.session_token is `bytes` (field 8) — accepts arbitrary
  // binary, which lets us stress the full byte-range payload that
  // the `string` field's UTF-8 constraint forbids.  Pins that
  // proto's bytes-field length-prefix + the eval-side proto→list<u8>
  // walk both handle a MiB-scale unconstrained payload.
  celwasm::testdata::Customer c;
  c.set_session_token(MakeLargeBlob(kLargeBlobBytes));
  c.set_user_id(9);
  OwnedComponentVal lifted;
  ASSERT_THAT(LiftCelToComponent(ProtoOfFqn("celwasm.testdata.Customer"),
                                 Value::Message(c), EmptyCtx(), lifted.get()),
              IsOk());
  EXPECT_GE(lifted.cref().of.list.size, kLargeBlobBytes);

  CelComponentContext ctx;
  ctx.pool = google::protobuf::DescriptorPool::generated_pool();
  Value back;
  ASSERT_THAT(LowerComponentToCel(ProtoOfFqn("celwasm.testdata.Customer"),
                                  lifted.cref(), ctx, &back),
              IsOk());
  const auto* c2 = dynamic_cast<const celwasm::testdata::Customer*>(
      (*back.MessageBacking())->message());
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c2->session_token().size(), kLargeBlobBytes);
  EXPECT_EQ(c2->session_token(), c.session_token());
  EXPECT_EQ(c2->user_id(), 9);
}

TEST(LiftCelToComponent, LargeProtoRepeatedStringLiftsAt10kEntries) {
  // Customer.tags is `repeated string` (field 12).  10⁴ tags exercise
  // the proto serialiser's repeated-field encoder + the Lower's
  // ParseFromString in a regime where per-element overhead dominates.
  celwasm::testdata::Customer c;
  c.mutable_tags()->Reserve(kLargeProtoRepeats);
  for (size_t i = 0; i < kLargeProtoRepeats; ++i) {
    c.add_tags(absl::StrCat("tag_", i));
  }
  OwnedComponentVal lifted;
  ASSERT_THAT(LiftCelToComponent(ProtoOfFqn("celwasm.testdata.Customer"),
                                 Value::Message(c), EmptyCtx(), lifted.get()),
              IsOk());
  EXPECT_EQ(lifted.cref().kind, WASMTIME_COMPONENT_LIST);
  EXPECT_GT(lifted.cref().of.list.size, kLargeProtoRepeats);  // ≥ 1 byte/tag

  CelComponentContext ctx;
  ctx.pool = google::protobuf::DescriptorPool::generated_pool();
  Value back;
  ASSERT_THAT(LowerComponentToCel(ProtoOfFqn("celwasm.testdata.Customer"),
                                  lifted.cref(), ctx, &back),
              IsOk());
  const auto* c2 = dynamic_cast<const celwasm::testdata::Customer*>(
      (*back.MessageBacking())->message());
  ASSERT_NE(c2, nullptr);
  ASSERT_EQ(c2->tags_size(), static_cast<int>(kLargeProtoRepeats));
  // Spot-check: head + middle + tail keep their value.
  EXPECT_EQ(c2->tags(0), "tag_0");
  EXPECT_EQ(c2->tags(static_cast<int>(kLargeProtoRepeats / 2)),
            absl::StrCat("tag_", kLargeProtoRepeats / 2));
  EXPECT_EQ(c2->tags(static_cast<int>(kLargeProtoRepeats - 1)),
            absl::StrCat("tag_", kLargeProtoRepeats - 1));
}

}  // namespace
}  // namespace celwasm
