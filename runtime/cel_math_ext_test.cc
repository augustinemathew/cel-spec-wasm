// M16 math_ext kernel coverage — Slice A (scalar family).
//
// Native unit tests for the rounding / predicate / magnitude / sign /
// sqrt kernels.  Semantics are pinned to cel-cpp
// `extensions/math_ext.cc` + the conformance fixture
// `tests/simple/testdata/math_ext.textproto`; spec-citation cases
// (abs(INT64_MIN) overflow, sign(NaN)=NaN, round half-away-from-zero,
// sqrt(neg)=NaN) stay as focused TEST_F so the source of truth is
// explicit at each assertion.

#include "runtime/cel_math_ext.h"

#include <cmath>
#include <cstdint>

#include <map>
#include <vector>

#include "gtest/gtest.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_list.h"
#include "runtime/cel_memory.h"

namespace {

// ref_slot → the elements a host-backed list vends.  The scripted
// `cel_list_iter_open` below materialises these into the arena
// exactly as the production trampoline does
// (eval/internal/cel_host.cc `SnapshotHostListToArena`).
std::map<uint32_t, std::vector<CelValue>>& HostMinMaxTable() {
  static auto* t = new std::map<uint32_t, std::vector<CelValue>>();
  return *t;
}
int g_minmax_iter_open_calls = 0;

}  // namespace

extern "C" {
// Strong override of the weak host-build stub in cel_runtime.c.
void cel_host_cel_list_iter_open(uint32_t out_slot, uint32_t list_slot) {
  ++g_minmax_iter_open_calls;
  const std::vector<CelValue> elements =
      HostMinMaxTable()[cel_value_at(list_slot)->payload.ref_slot];
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
}  // extern "C"

namespace celwasm {
namespace {

class MathExtTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }
  uint32_t Slot() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }
  uint32_t Int(int64_t v) {
    uint32_t s = Slot();
    CelValue* c = cel_value_at(s);
    c->kind = CEL_INT;
    c->payload.i = v;
    return s;
  }
  uint32_t Uint(uint64_t v) {
    uint32_t s = Slot();
    CelValue* c = cel_value_at(s);
    c->kind = CEL_UINT;
    c->payload.u = v;
    return s;
  }
  uint32_t Double(double v) {
    uint32_t s = Slot();
    CelValue* c = cel_value_at(s);
    c->kind = CEL_DOUBLE;
    c->payload.d = v;
    return s;
  }
  uint32_t Bool(int b) {
    uint32_t s = Slot();
    CelValue* c = cel_value_at(s);
    c->kind = CEL_BOOL;
    c->payload.b = b;
    return s;
  }
  uint32_t Err(uint32_t code) {
    uint32_t s = Slot();
    CelValue* c = cel_value_at(s);
    c->kind = CEL_ERROR;
    c->payload.err = code;
    return s;
  }
  uint32_t Unknown() {
    uint32_t s = Slot();
    cel_value_at(s)->kind = CEL_UNKNOWN;
    return s;
  }
  const CelValue* At(uint32_t s) {
    return cel_value_at(s);
  }
};

// ── Rounding: ceil / floor / round / trunc ────────────────────────

TEST_F(MathExtTest, Ceil) {
  uint32_t o = Slot();
  cel_math_ceil_at_v(o, Double(1.2));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, 2.0);
  cel_math_ceil_at_v(o, Double(-1.2));
  EXPECT_EQ(At(o)->payload.d, -1.0);
  cel_math_ceil_at_v(o, Double(3.0));
  EXPECT_EQ(At(o)->payload.d, 3.0);
}

TEST_F(MathExtTest, Floor) {
  uint32_t o = Slot();
  cel_math_floor_at_v(o, Double(1.8));
  EXPECT_EQ(At(o)->payload.d, 1.0);
  cel_math_floor_at_v(o, Double(-1.2));
  EXPECT_EQ(At(o)->payload.d, -2.0);
}

TEST_F(MathExtTest, Trunc) {
  uint32_t o = Slot();
  cel_math_trunc_at_v(o, Double(1.9));
  EXPECT_EQ(At(o)->payload.d, 1.0);
  cel_math_trunc_at_v(o, Double(-1.9));
  EXPECT_EQ(At(o)->payload.d, -1.0);
}

// round() is half-AWAY-from-zero (C round / cel-cpp), NOT wasm
// f64.nearest half-to-even.  2.5→3, -2.5→-3, 0.5→1.
TEST_F(MathExtTest, RoundHalfAwayFromZero) {
  uint32_t o = Slot();
  cel_math_round_at_v(o, Double(2.5));
  EXPECT_EQ(At(o)->payload.d, 3.0);
  cel_math_round_at_v(o, Double(-2.5));
  EXPECT_EQ(At(o)->payload.d, -3.0);
  cel_math_round_at_v(o, Double(0.5));
  EXPECT_EQ(At(o)->payload.d, 1.0);
  cel_math_round_at_v(o, Double(1.4));
  EXPECT_EQ(At(o)->payload.d, 1.0);
}

TEST_F(MathExtTest, RoundingRejectsNonDouble) {
  uint32_t o = Slot();
  cel_math_ceil_at_v(o, Int(3));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

// ── Predicates: isInf / isNaN / isFinite ──────────────────────────

TEST_F(MathExtTest, IsInf) {
  uint32_t o = Slot();
  cel_math_is_inf_at_v(o, Double(INFINITY));
  EXPECT_EQ(At(o)->kind, CEL_BOOL);
  EXPECT_EQ(At(o)->payload.b, 1);
  cel_math_is_inf_at_v(o, Double(-INFINITY));
  EXPECT_EQ(At(o)->payload.b, 1);
  cel_math_is_inf_at_v(o, Double(1.0));
  EXPECT_EQ(At(o)->payload.b, 0);
  cel_math_is_inf_at_v(o, Double(NAN));
  EXPECT_EQ(At(o)->payload.b, 0);
}

TEST_F(MathExtTest, IsNaN) {
  uint32_t o = Slot();
  cel_math_is_nan_at_v(o, Double(NAN));
  EXPECT_EQ(At(o)->payload.b, 1);
  cel_math_is_nan_at_v(o, Double(1.0));
  EXPECT_EQ(At(o)->payload.b, 0);
  cel_math_is_nan_at_v(o, Double(INFINITY));
  EXPECT_EQ(At(o)->payload.b, 0);
}

TEST_F(MathExtTest, IsFinite) {
  uint32_t o = Slot();
  cel_math_is_finite_at_v(o, Double(1.0));
  EXPECT_EQ(At(o)->payload.b, 1);
  cel_math_is_finite_at_v(o, Double(INFINITY));
  EXPECT_EQ(At(o)->payload.b, 0);
  cel_math_is_finite_at_v(o, Double(NAN));
  EXPECT_EQ(At(o)->payload.b, 0);
}

// ── abs (int / uint / double) ─────────────────────────────────────

TEST_F(MathExtTest, AbsInt) {
  uint32_t o = Slot();
  cel_math_abs_at_v(o, Int(-11));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 11);
  cel_math_abs_at_v(o, Int(11));
  EXPECT_EQ(At(o)->payload.i, 11);
  cel_math_abs_at_v(o, Int(0));
  EXPECT_EQ(At(o)->payload.i, 0);
}

// abs(INT64_MIN) is unrepresentable → overflow error (cel-cpp parity).
TEST_F(MathExtTest, AbsIntMinOverflows) {
  uint32_t o = Slot();
  cel_math_abs_at_v(o, Int(INT64_MIN));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_OVERFLOW);
}

TEST_F(MathExtTest, AbsUintIsIdentity) {
  uint32_t o = Slot();
  cel_math_abs_at_v(o, Uint(7));
  EXPECT_EQ(At(o)->kind, CEL_UINT);
  EXPECT_EQ(At(o)->payload.u, 7u);
}

TEST_F(MathExtTest, AbsDouble) {
  uint32_t o = Slot();
  cel_math_abs_at_v(o, Double(-11.5));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, 11.5);
  cel_math_abs_at_v(o, Double(-0.0));
  EXPECT_EQ(At(o)->payload.d, 0.0);
  EXPECT_FALSE(std::signbit(At(o)->payload.d));  // fabs(-0.0) = +0.0
}

TEST_F(MathExtTest, AbsRejectsNonNumeric) {
  uint32_t o = Slot();
  cel_math_abs_at_v(o, Bool(1));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

// ── sign (int / uint / double) ────────────────────────────────────

TEST_F(MathExtTest, SignInt) {
  uint32_t o = Slot();
  cel_math_sign_at_v(o, Int(-11));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, -1);
  cel_math_sign_at_v(o, Int(11));
  EXPECT_EQ(At(o)->payload.i, 1);
  cel_math_sign_at_v(o, Int(0));
  EXPECT_EQ(At(o)->payload.i, 0);
}

TEST_F(MathExtTest, SignUintNeverNegative) {
  uint32_t o = Slot();
  cel_math_sign_at_v(o, Uint(0));
  EXPECT_EQ(At(o)->kind, CEL_UINT);
  EXPECT_EQ(At(o)->payload.u, 0u);
  cel_math_sign_at_v(o, Uint(5));
  EXPECT_EQ(At(o)->payload.u, 1u);
}

TEST_F(MathExtTest, SignDouble) {
  uint32_t o = Slot();
  cel_math_sign_at_v(o, Double(-32.0));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, -1.0);
  cel_math_sign_at_v(o, Double(32.0));
  EXPECT_EQ(At(o)->payload.d, 1.0);
}

// sign(±0.0) = 0.0; sign(NaN) = NaN (cel-cpp parity).
TEST_F(MathExtTest, SignDoubleZeroAndNaN) {
  uint32_t o = Slot();
  cel_math_sign_at_v(o, Double(0.0));
  EXPECT_EQ(At(o)->payload.d, 0.0);
  cel_math_sign_at_v(o, Double(-0.0));
  EXPECT_EQ(At(o)->payload.d, 0.0);
  cel_math_sign_at_v(o, Double(NAN));
  EXPECT_TRUE(std::isnan(At(o)->payload.d));
}

// ── sqrt (int / uint / double → double) ───────────────────────────

TEST_F(MathExtTest, SqrtAlwaysDouble) {
  uint32_t o = Slot();
  cel_math_sqrt_at_v(o, Int(4));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, 2.0);
  cel_math_sqrt_at_v(o, Uint(9));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, 3.0);
  cel_math_sqrt_at_v(o, Double(2.0));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_DOUBLE_EQ(At(o)->payload.d, std::sqrt(2.0));
  cel_math_sqrt_at_v(o, Double(0.0));
  EXPECT_EQ(At(o)->payload.d, 0.0);
}

TEST_F(MathExtTest, SqrtNegativeIsNaN) {
  uint32_t o = Slot();
  cel_math_sqrt_at_v(o, Double(-1.0));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_TRUE(std::isnan(At(o)->payload.d));
  cel_math_sqrt_at_v(o, Int(-4));
  EXPECT_TRUE(std::isnan(At(o)->payload.d));
}

// ── Bitwise: and / or / xor / not ─────────────────────────────────

TEST_F(MathExtTest, BitAndOrXorInt) {
  uint32_t o = Slot();
  cel_math_bit_and_at_vv(o, Int(1), Int(2));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 0);
  cel_math_bit_and_at_vv(o, Int(3), Int(2));
  EXPECT_EQ(At(o)->payload.i, 2);
  cel_math_bit_or_at_vv(o, Int(1), Int(2));
  EXPECT_EQ(At(o)->payload.i, 3);
  cel_math_bit_xor_at_vv(o, Int(1), Int(3));
  EXPECT_EQ(At(o)->payload.i, 2);
}

TEST_F(MathExtTest, BitAndOrXorUint) {
  uint32_t o = Slot();
  cel_math_bit_and_at_vv(o, Uint(6), Uint(2));
  EXPECT_EQ(At(o)->kind, CEL_UINT);
  EXPECT_EQ(At(o)->payload.u, 2u);
  cel_math_bit_or_at_vv(o, Uint(1), Uint(4));
  EXPECT_EQ(At(o)->payload.u, 5u);
  cel_math_bit_xor_at_vv(o, Uint(1), Uint(3));
  EXPECT_EQ(At(o)->payload.u, 2u);
}

TEST_F(MathExtTest, BitNot) {
  uint32_t o = Slot();
  cel_math_bit_not_at_v(o, Int(1));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, ~static_cast<int64_t>(1));  // -2
  cel_math_bit_not_at_v(o, Uint(0));
  EXPECT_EQ(At(o)->kind, CEL_UINT);
  EXPECT_EQ(At(o)->payload.u, ~static_cast<uint64_t>(0));  // UINT64_MAX
}

TEST_F(MathExtTest, BitMismatchedKindsPoison) {
  uint32_t o = Slot();
  cel_math_bit_and_at_vv(o, Int(1), Uint(2));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

// ── Bitwise: shifts ───────────────────────────────────────────────

TEST_F(MathExtTest, ShiftLeftRight) {
  uint32_t o = Slot();
  cel_math_bit_shift_left_at_vv(o, Int(1), Int(2));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 4);
  cel_math_bit_shift_right_at_vv(o, Int(1024), Int(2));
  EXPECT_EQ(At(o)->payload.i, 256);
  cel_math_bit_shift_left_at_vv(o, Uint(1), Int(2));
  EXPECT_EQ(At(o)->kind, CEL_UINT);
  EXPECT_EQ(At(o)->payload.u, 4u);
  cel_math_bit_shift_right_at_vv(o, Uint(1024), Int(2));
  EXPECT_EQ(At(o)->payload.u, 256u);
}

// Shift count > 63 saturates to 0 (cel-cpp parity).
TEST_F(MathExtTest, ShiftLargeCountIsZero) {
  uint32_t o = Slot();
  cel_math_bit_shift_left_at_vv(o, Int(-1), Int(200));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 0);
  cel_math_bit_shift_right_at_vv(o, Uint(12345), Int(64));
  EXPECT_EQ(At(o)->payload.u, 0u);
}

// bitShiftRight on a negative int is LOGICAL (no sign extension):
// -1 >> 1 == INT64_MAX, not -1.
TEST_F(MathExtTest, ShiftRightIntIsLogical) {
  uint32_t o = Slot();
  cel_math_bit_shift_right_at_vv(o, Int(-1), Int(1));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, INT64_MAX);
}

// Negative offset → CEL_ERR_INVALID_ARGUMENT ("negative offset").
TEST_F(MathExtTest, ShiftNegativeOffsetErrors) {
  uint32_t o = Slot();
  cel_math_bit_shift_left_at_vv(o, Uint(1), Int(-1));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_INVALID_ARGUMENT);
  cel_math_bit_shift_right_at_vv(o, Int(1), Int(-5));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_INVALID_ARGUMENT);
}

// ── min / max: binary (post-macro math.@min / math.@max) ──────────

TEST_F(MathExtTest, MinMaxBinarySameType) {
  uint32_t o = Slot();
  cel_math_min_at_vv(o, Int(3), Int(7));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 3);
  cel_math_max_at_vv(o, Int(3), Int(7));
  EXPECT_EQ(At(o)->payload.i, 7);
  cel_math_min_at_vv(o, Uint(9), Uint(4));
  EXPECT_EQ(At(o)->kind, CEL_UINT);
  EXPECT_EQ(At(o)->payload.u, 4u);
  cel_math_max_at_vv(o, Double(1.5), Double(2.5));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, 2.5);
}

// Cross-type: the winning operand is returned verbatim, preserving its
// kind (cel-cpp MinNumber/MaxNumber).  greatest(1, 1.0) == 1 (int).
TEST_F(MathExtTest, MinMaxBinaryCrossType) {
  uint32_t o = Slot();
  cel_math_max_at_vv(o, Int(1), Double(1.0));  // equal → first operand
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 1);
  cel_math_min_at_vv(o, Int(5), Uint(2));  // 2u < 5 → uint wins
  EXPECT_EQ(At(o)->kind, CEL_UINT);
  EXPECT_EQ(At(o)->payload.u, 2u);
  cel_math_max_at_vv(o, Uint(3), Double(10.5));  // 10.5 > 3 → double wins
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, 10.5);
}

// NaN comparisons are false (cel-cpp), so a NaN operand keeps the
// first/current operand.
TEST_F(MathExtTest, MinMaxBinaryNaN) {
  uint32_t o = Slot();
  cel_math_min_at_vv(o, Double(5.0), Double(NAN));
  EXPECT_EQ(At(o)->payload.d, 5.0);
  cel_math_max_at_vv(o, Double(5.0), Double(NAN));
  EXPECT_EQ(At(o)->payload.d, 5.0);
}

TEST_F(MathExtTest, MinMaxRejectsNonNumeric) {
  uint32_t o = Slot();
  cel_math_min_at_vv(o, Int(1), Bool(1));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

// ── min / max: list fold (collapsed 3+ args / list literal) ────────

TEST_F(MathExtTest, MinMaxListInt) {
  uint32_t list = Slot();
  cel_list_create(list, 3);
  cel_list_append_at(list, Int(3));
  cel_list_append_at(list, Int(1));
  cel_list_append_at(list, Int(2));
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, list);
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 1);
  cel_math_max_list_at_v(o, list);
  EXPECT_EQ(At(o)->payload.i, 3);
}

// Mixed-kind list (the dyn-typed checker case): winner returned with
// its own kind.  min([5.4, 10, 3u, -5.0]) == -5.0 (double).
TEST_F(MathExtTest, MinMaxListMixed) {
  uint32_t list = Slot();
  cel_list_create(list, 4);
  cel_list_append_at(list, Double(5.4));
  cel_list_append_at(list, Int(10));
  cel_list_append_at(list, Uint(3));
  cel_list_append_at(list, Double(-5.0));
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, list);
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, -5.0);
  cel_math_max_list_at_v(o, list);
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 10);
}

TEST_F(MathExtTest, MinMaxListSingleElement) {
  uint32_t list = Slot();
  cel_list_create(list, 1);
  cel_list_append_at(list, Int(42));
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, list);
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 42);
}

TEST_F(MathExtTest, MinMaxListNonNumericElementPoisons) {
  uint32_t list = Slot();
  cel_list_create(list, 2);
  cel_list_append_at(list, Int(1));
  cel_list_append_at(list, Bool(1));
  uint32_t o = Slot();
  cel_math_max_list_at_v(o, list);
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(MathExtTest, MinMaxEmptyArenaListIsInvalidArgument) {
  // cel-cpp `extensions/math_ext.cc:106` / `:152`:
  // "math.@min argument must not be empty".
  uint32_t list = Slot();
  cel_list_create(list, 0);
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, list);
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_INVALID_ARGUMENT);
  cel_math_max_list_at_v(o, list);
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(MathExtTest, MinMaxNonListOperandPoisons) {
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, Int(1));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

// ── min / max over a HOST-BACKED list ─────────────────────────────
//
// `math.least(boundList)` reaches the kernel with a CEL_LIST_HOST
// operand — its elements exist only on the host side until
// `cel_list_arena_view` snapshots them.  Before that lift was wired
// every case here returned CEL_ERR_TYPE_MISMATCH, indistinguishable
// from a genuine type error.  The 3-arg macro form is always
// rewritten by cel-cpp into an arena literal, which is why only the
// single-list-argument form over a bound variable reaches this arm.

class HostListMinMaxFixture : public MathExtTest {
 protected:
  void SetUp() override {
    MathExtTest::SetUp();
    HostMinMaxTable().clear();
    g_minmax_iter_open_calls = 0;
  }

  // A CEL_LIST_HOST slot whose scripted backing vends `elements`.
  uint32_t HostList(uint32_t ref_slot, const std::vector<CelValue>& elements) {
    HostMinMaxTable()[ref_slot] = elements;
    uint32_t s = Slot();
    CelValue* c = cel_value_at(s);
    c->kind = CEL_LIST_HOST;
    c->payload.ref_slot = ref_slot;
    return s;
  }
  CelValue IntVal(int64_t v) {
    CelValue c{};
    c.kind = CEL_INT;
    c.payload.i = v;
    return c;
  }
  CelValue UintVal(uint64_t v) {
    CelValue c{};
    c.kind = CEL_UINT;
    c.payload.u = v;
    return c;
  }
  CelValue DoubleVal(double v) {
    CelValue c{};
    c.kind = CEL_DOUBLE;
    c.payload.d = v;
    return c;
  }
  CelValue BoolVal(int b) {
    CelValue c{};
    c.kind = CEL_BOOL;
    c.payload.b = b;
    return c;
  }
};

TEST_F(HostListMinMaxFixture, MinMaxHostIntList) {
  uint32_t list = HostList(1, {IntVal(3), IntVal(1), IntVal(2)});
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, list);
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 1);
  cel_math_max_list_at_v(o, list);
  EXPECT_EQ(At(o)->payload.i, 3);
  EXPECT_EQ(g_minmax_iter_open_calls, 2);
}

TEST_F(HostListMinMaxFixture, MinMaxHostUintAndDoubleLists) {
  uint32_t o = Slot();
  cel_math_max_list_at_v(o, HostList(1, {UintVal(1), UintVal(2)}));
  EXPECT_EQ(At(o)->kind, CEL_UINT);
  EXPECT_EQ(At(o)->payload.u, 2u);
  cel_math_min_list_at_v(o, HostList(2, {DoubleVal(1.5), DoubleVal(0.5)}));
  EXPECT_EQ(At(o)->kind, CEL_DOUBLE);
  EXPECT_EQ(At(o)->payload.d, 0.5);
}

TEST_F(HostListMinMaxFixture, MinMaxHostListBoundaryIntegers) {
  constexpr int64_t kMin = -9223372036854775807LL - 1;
  constexpr int64_t kMax = 9223372036854775807LL;
  uint32_t list = HostList(1, {IntVal(kMax), IntVal(kMin)});
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, list);
  EXPECT_EQ(At(o)->payload.i, kMin);
  cel_math_max_list_at_v(o, list);
  EXPECT_EQ(At(o)->payload.i, kMax);
}

TEST_F(HostListMinMaxFixture, MinMaxHostSingleElementList) {
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, HostList(1, {IntVal(42)}));
  EXPECT_EQ(At(o)->kind, CEL_INT);
  EXPECT_EQ(At(o)->payload.i, 42);
}

TEST_F(HostListMinMaxFixture, MinMaxEmptyHostListIsInvalidArgument) {
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, HostList(1, {}));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_INVALID_ARGUMENT);
}

TEST_F(HostListMinMaxFixture, MinMaxHostListNonNumericElementPoisons) {
  uint32_t o = Slot();
  cel_math_max_list_at_v(o, HostList(1, {IntVal(1), BoolVal(1)}));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(HostListMinMaxFixture, MinMaxHostListPoisonedElementPropagates) {
  CelValue poisoned{};
  poisoned.kind = CEL_ERROR;
  poisoned.payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, HostList(1, {IntVal(1), poisoned}));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(HostListMinMaxFixture, ArenaListDoesNotTripTheHostArm) {
  // Arena operands pass through `cel_list_arena_view` as identity —
  // no host trip, same answer as before the lift was wired.
  uint32_t list = Slot();
  cel_list_create(list, 2);
  cel_list_append_at(list, Int(5));
  cel_list_append_at(list, Int(4));
  uint32_t o = Slot();
  cel_math_min_list_at_v(o, list);
  EXPECT_EQ(At(o)->payload.i, 4);
  EXPECT_EQ(g_minmax_iter_open_calls, 0);
}

// ── 3VL absorption (representative across the family) ─────────────

TEST_F(MathExtTest, AbsorbsErrorAndUnknown) {
  uint32_t o = Slot();
  cel_math_ceil_at_v(o, Err(CEL_ERR_DIVIDE_BY_ZERO));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_DIVIDE_BY_ZERO);

  cel_math_abs_at_v(o, Unknown());
  EXPECT_EQ(At(o)->kind, CEL_UNKNOWN);

  cel_math_sqrt_at_v(o, Err(CEL_ERR_OVERFLOW));
  EXPECT_EQ(At(o)->kind, CEL_ERROR);
  EXPECT_EQ(At(o)->payload.err, CEL_ERR_OVERFLOW);
}

}  // namespace
}  // namespace celwasm
