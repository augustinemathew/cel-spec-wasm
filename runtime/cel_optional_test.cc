// M14 Slice A — per-TU tests for `cel_optional.{h,c}`.
//
// Coverage matrix per CLAUDE.md §"Testing principles":
//
//   - Constructors: of (positive + 3VL propagation), of_non_zero
//     (per-kind zero predicate), none.
//   - Accessors:    has_value (Some / None / kind-mismatch), value
//     (Some / None ⇒ INVALID_ARGUMENT / kind-mismatch).
//   - Or / orValue: both branches (LHS Some / LHS None) + kind-mismatch.
//   - select_optional_field: unwrap + map lookup, absent key, error
//     propagation, type-mismatch source.
//   - cel_equals_at_vv arm: None==None, present mismatch, Some(==),
//     Some(!=) with inner recursion.
//
// The corpus row `optional.ofNonZeroValue(<HOST_MESSAGE>)` is out of
// scope for Slice A — `is_zero_value` traps on CEL_LIST_HOST /
// CEL_MAP_HOST / CEL_MESSAGE until the host trampoline lands in
// Slice B.  Negative coverage for those kinds is the trap itself
// (CLAUDE.md "Unimplemented features" — crash at the call site).

#include "runtime/cel_optional.h"

#include <cstdint>
#include <cstring>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_runtime.h"
#include "gtest/gtest.h"

extern "C" {
// `cel_equals_at_vv` is wasm-exported by `cel_runtime.c` but not
// re-declared in any umbrella header (intentional: host code goes
// through the polymorphic ladder via specific kernels, not the
// dispatcher).  Forward-declare locally so the M14 equality-arm
// tests can call it.
void cel_equals_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
// `cel_map_create` / `cel_map_insert` declarations live in
// `cel_map.h` (already pulled in transitively).  No extra decls
// needed.

// Strong override of the weak `cel_host_cel_set_field` declared in
// `cel_optional.c` for the host build.  Recording the invocation
// args is the load-bearing assertion for the proto `_if_present`
// short-circuit: the None-path tests verify this counter stays at
// 0 (proves the wrapper didn't reach the host).  Reset in `SetUp`.
struct HostSetFieldCall {
  uint32_t msg_slot;
  uint32_t field_ref_id;
  uint32_t value_slot;
};
}
namespace {
HostSetFieldCall g_host_set_field_last;
int g_host_set_field_calls = 0;
}  // namespace
extern "C" {
void cel_host_cel_set_field(uint32_t msg_slot, uint32_t field_ref_id,
                            uint32_t value_slot) {
  g_host_set_field_last = {msg_slot, field_ref_id, value_slot};
  ++g_host_set_field_calls;
}
}

namespace celwasm {
namespace {

class OptionalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
    g_host_set_field_calls = 0;
    g_host_set_field_last = {};
  }

  // Allocates a fresh 24-byte CelValue slot in the arena and writes
  // the given CelValue into it.  Returns the slot's byte offset.
  uint32_t MakeSlot() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }
  uint32_t MakeSlotInt(int64_t i) {
    uint32_t s = MakeSlot();
    CelValue* v = cel_value_at(s);
    v->kind = CEL_INT;
    v->payload.i = i;
    return s;
  }
  uint32_t MakeSlotBool(int b) {
    uint32_t s = MakeSlot();
    CelValue* v = cel_value_at(s);
    v->kind = CEL_BOOL;
    v->payload.b = b ? 1 : 0;
    return s;
  }
  uint32_t MakeSlotNull() {
    uint32_t s = MakeSlot();
    cel_value_at(s)->kind = CEL_NULL;
    return s;
  }
  uint32_t MakeSlotError(uint32_t err) {
    uint32_t s = MakeSlot();
    CelValue* v = cel_value_at(s);
    v->kind = CEL_ERROR;
    v->payload.err = err;
    return s;
  }
  uint32_t MakeSlotUnknown(uint32_t attr) {
    uint32_t s = MakeSlot();
    CelValue* v = cel_value_at(s);
    v->kind = CEL_UNKNOWN;
    v->payload.unk = attr;
    return s;
  }
  uint32_t MakeSlotString(const char* bytes, uint32_t len) {
    // Allocate the byte payload in the arena, then a CelValue slot
    // pointing at it.  Span offsets are arena-relative; cel_string
    // helpers consume the same convention.
    uint32_t bytes_off = arena_alloc(len > 0 ? len : 1);
    if (len > 0) {
      std::memcpy(cel_mem_base() + bytes_off, bytes, len);
    }
    uint32_t s = MakeSlot();
    CelValue* v = cel_value_at(s);
    v->kind = CEL_STRING;
    v->payload.s.ptr = bytes_off;
    v->payload.s.len = len;
    return s;
  }

  const CelValue* At(uint32_t slot) {
    return cel_value_at(slot);
  }
  const OptionalCell* CellOf(uint32_t opt_slot) {
    const CelValue* v = cel_value_at(opt_slot);
    return reinterpret_cast<const OptionalCell*>(cel_mem_base() +
                                                 v->payload.opt);
  }
};

// ── Constructors ──────────────────────────────────────────

TEST_F(OptionalTest, NoneProducesPresentZeroCell) {
  uint32_t out = MakeSlot();
  cel_optional_none_at(out);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 0u);
}

TEST_F(OptionalTest, OfWrapsInner) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotInt(42);
  cel_optional_of_at_v(out, v);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  const OptionalCell* cell = CellOf(out);
  EXPECT_EQ(cell->present, 1u);
  EXPECT_EQ(cell->inner.kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(cell->inner.payload.i, 42);
}

TEST_F(OptionalTest, OfPropagatesUnknown) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotUnknown(7);
  cel_optional_of_at_v(out, v);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(out)->payload.unk, 7u);
}

TEST_F(OptionalTest, OfPropagatesError) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotError(CEL_ERR_DIVIDE_BY_ZERO);
  cel_optional_of_at_v(out, v);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

// ── ofNonZeroValue: zero-predicate matrix ─────────────────

TEST_F(OptionalTest, OfNonZeroValueZeroIntProducesNone) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotInt(0);
  cel_optional_of_non_zero_at_v(out, v);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 0u);
}

TEST_F(OptionalTest, OfNonZeroValueNonZeroIntProducesSome) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotInt(7);
  cel_optional_of_non_zero_at_v(out, v);
  EXPECT_EQ(CellOf(out)->present, 1u);
  EXPECT_EQ(CellOf(out)->inner.payload.i, 7);
}

TEST_F(OptionalTest, OfNonZeroValueNullProducesNone) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotNull();
  cel_optional_of_non_zero_at_v(out, v);
  EXPECT_EQ(CellOf(out)->present, 0u);
}

TEST_F(OptionalTest, OfNonZeroValueFalseProducesNone) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotBool(0);
  cel_optional_of_non_zero_at_v(out, v);
  EXPECT_EQ(CellOf(out)->present, 0u);
}

TEST_F(OptionalTest, OfNonZeroValueTrueProducesSome) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotBool(1);
  cel_optional_of_non_zero_at_v(out, v);
  EXPECT_EQ(CellOf(out)->present, 1u);
}

TEST_F(OptionalTest, OfNonZeroValueEmptyStringProducesNone) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotString("", 0);
  cel_optional_of_non_zero_at_v(out, v);
  EXPECT_EQ(CellOf(out)->present, 0u);
}

TEST_F(OptionalTest, OfNonZeroValueNonEmptyStringProducesSome) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotString("hi", 2);
  cel_optional_of_non_zero_at_v(out, v);
  EXPECT_EQ(CellOf(out)->present, 1u);
}

TEST_F(OptionalTest, OfNonZeroValueNestedNoneProducesNone) {
  uint32_t out = MakeSlot();
  uint32_t inner = MakeSlot();
  cel_optional_none_at(inner);
  cel_optional_of_non_zero_at_v(out, inner);
  EXPECT_EQ(CellOf(out)->present, 0u);
}

TEST_F(OptionalTest, OfNonZeroValueNestedSomeNonZeroProducesSome) {
  uint32_t out = MakeSlot();
  uint32_t inner_opt = MakeSlot();
  uint32_t inner_v = MakeSlotInt(1);
  cel_optional_of_at_v(inner_opt, inner_v);
  cel_optional_of_non_zero_at_v(out, inner_opt);
  EXPECT_EQ(CellOf(out)->present, 1u);
}

// ── hasValue / value ──────────────────────────────────────

TEST_F(OptionalTest, HasValueOnSomeIsTrue) {
  uint32_t opt = MakeSlot();
  uint32_t v = MakeSlotInt(1);
  cel_optional_of_at_v(opt, v);
  uint32_t out = MakeSlot();
  cel_optional_has_value_at_v(out, opt);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(At(out)->payload.b, 1);
}

TEST_F(OptionalTest, HasValueOnNoneIsFalse) {
  uint32_t opt = MakeSlot();
  cel_optional_none_at(opt);
  uint32_t out = MakeSlot();
  cel_optional_has_value_at_v(out, opt);
  EXPECT_EQ(At(out)->payload.b, 0);
}

TEST_F(OptionalTest, HasValueOnNonOptionalIsTypeMismatch) {
  uint32_t out = MakeSlot();
  uint32_t v = MakeSlotInt(42);
  cel_optional_has_value_at_v(out, v);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(OptionalTest, ValueOnSomeUnwraps) {
  uint32_t opt = MakeSlot();
  uint32_t v = MakeSlotInt(99);
  cel_optional_of_at_v(opt, v);
  uint32_t out = MakeSlot();
  cel_optional_value_at_v(out, opt);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(At(out)->payload.i, 99);
}

TEST_F(OptionalTest, ValueOnNoneIsInvalidArgument) {
  uint32_t opt = MakeSlot();
  cel_optional_none_at(opt);
  uint32_t out = MakeSlot();
  cel_optional_value_at_v(out, opt);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_INVALID_ARGUMENT));
}

// ── or / orValue ──────────────────────────────────────────

TEST_F(OptionalTest, OrLhsSomeKeepsLhs) {
  uint32_t lhs = MakeSlot();
  uint32_t v_lhs = MakeSlotInt(1);
  cel_optional_of_at_v(lhs, v_lhs);
  uint32_t rhs = MakeSlot();
  uint32_t v_rhs = MakeSlotInt(7);
  cel_optional_of_at_v(rhs, v_rhs);
  uint32_t out = MakeSlot();
  cel_optional_or_at_vv(out, lhs, rhs);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 1u);
  EXPECT_EQ(CellOf(out)->inner.payload.i, 1);
}

TEST_F(OptionalTest, OrLhsNoneTakesRhs) {
  uint32_t lhs = MakeSlot();
  cel_optional_none_at(lhs);
  uint32_t rhs = MakeSlot();
  uint32_t v_rhs = MakeSlotInt(7);
  cel_optional_of_at_v(rhs, v_rhs);
  uint32_t out = MakeSlot();
  cel_optional_or_at_vv(out, lhs, rhs);
  EXPECT_EQ(CellOf(out)->inner.payload.i, 7);
}

TEST_F(OptionalTest, OrValueLhsSomeUnwrapsLhs) {
  uint32_t lhs = MakeSlot();
  uint32_t v_lhs = MakeSlotInt(11);
  cel_optional_of_at_v(lhs, v_lhs);
  uint32_t dflt = MakeSlotInt(0);
  uint32_t out = MakeSlot();
  cel_optional_or_value_at_vv(out, lhs, dflt);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(At(out)->payload.i, 11);
}

TEST_F(OptionalTest, OrValueLhsNoneTakesDefault) {
  uint32_t lhs = MakeSlot();
  cel_optional_none_at(lhs);
  uint32_t dflt = MakeSlotString("fallback", 8);
  uint32_t out = MakeSlot();
  cel_optional_or_value_at_vv(out, lhs, dflt);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(At(out)->payload.s.len, 8u);
}

TEST_F(OptionalTest, OrValueTypeMismatchOnLhs) {
  uint32_t lhs = MakeSlotInt(0);  // not an optional
  uint32_t dflt = MakeSlotInt(0);
  uint32_t out = MakeSlot();
  cel_optional_or_value_at_vv(out, lhs, dflt);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// ── select_optional_field (arena map) ─────────────────────

TEST_F(OptionalTest, SelectFieldOnMapPresentKeyProducesSome) {
  // Build map {'c': 'v'}.
  uint32_t m = MakeSlot();
  cel_map_create(m, 1);
  uint32_t k = MakeSlotString("c", 1);
  uint32_t v = MakeSlotString("v", 1);
  cel_map_insert(m, k, v);

  uint32_t out = MakeSlot();
  uint32_t query = MakeSlotString("c", 1);
  cel_select_optional_field_at_vv(out, m, query);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 1u);
  EXPECT_EQ(CellOf(out)->inner.kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(CellOf(out)->inner.payload.s.len, 1u);
}

TEST_F(OptionalTest, SelectFieldOnMapAbsentKeyProducesNone) {
  uint32_t m = MakeSlot();
  cel_map_create(m, 1);
  uint32_t k = MakeSlotString("k", 1);
  uint32_t v = MakeSlotInt(1);
  cel_map_insert(m, k, v);

  uint32_t out = MakeSlot();
  uint32_t query = MakeSlotString("missing", 7);
  cel_select_optional_field_at_vv(out, m, query);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 0u);
}

TEST_F(OptionalTest, SelectFieldOnOptionalMapNoneStaysNone) {
  // optional.none() then .c should propagate None.
  uint32_t opt = MakeSlot();
  cel_optional_none_at(opt);
  uint32_t out = MakeSlot();
  uint32_t query = MakeSlotString("c", 1);
  cel_select_optional_field_at_vv(out, opt, query);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 0u);
}

TEST_F(OptionalTest, SelectFieldOnOptionalMapSomeRecurses) {
  // optional.of({'c': 'v'}) then .c should produce optional.of('v').
  uint32_t m = MakeSlot();
  cel_map_create(m, 1);
  uint32_t k = MakeSlotString("c", 1);
  uint32_t v = MakeSlotString("v", 1);
  cel_map_insert(m, k, v);

  uint32_t opt = MakeSlot();
  cel_optional_of_at_v(opt, m);
  uint32_t out = MakeSlot();
  uint32_t query = MakeSlotString("c", 1);
  cel_select_optional_field_at_vv(out, opt, query);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 1u);
  EXPECT_EQ(CellOf(out)->inner.kind, static_cast<uint32_t>(CEL_STRING));
}

// Chained Select on optional: `optional.of({'c': {'index': 'v'}}).c.index`
// — two sequential `cel_select_optional_field_at_vv` calls, with the
// second's src_slot being the first's out_slot.  This is the exact
// shape the conformance row `optional_chaining_4` produces at codegen.
TEST_F(OptionalTest, ChainedSelectFieldOnOptionalNestedMapsRecurses) {
  // Build inner map {'index': 'v'} at slot inner_m.
  uint32_t inner_m = MakeSlot();
  cel_map_create(inner_m, 1);
  cel_map_insert(inner_m, MakeSlotString("index", 5), MakeSlotString("v", 1));
  // Build outer map {'c': inner_m} at slot outer_m.
  uint32_t outer_m = MakeSlot();
  cel_map_create(outer_m, 1);
  cel_map_insert(outer_m, MakeSlotString("c", 1), inner_m);

  // Wrap in optional.
  uint32_t opt = MakeSlot();
  cel_optional_of_at_v(opt, outer_m);

  // Step 1: `.c` on optional<map<string, map<string, string>>>.
  uint32_t after_c = MakeSlot();
  cel_select_optional_field_at_vv(after_c, opt, MakeSlotString("c", 1));
  ASSERT_EQ(At(after_c)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  ASSERT_EQ(CellOf(after_c)->present, 1u);
  ASSERT_EQ(CellOf(after_c)->inner.kind, static_cast<uint32_t>(CEL_MAP_ARENA))
      << "inner of .c must be the unwrapped inner_map";

  // Step 2: `.index` on optional<map<string, string>>.
  uint32_t out = MakeSlot();
  cel_select_optional_field_at_vv(out, after_c, MakeSlotString("index", 5));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 1u);
  EXPECT_EQ(CellOf(out)->inner.kind, static_cast<uint32_t>(CEL_STRING));
}

// 3VL absorption — `cel_select_optional_field_at_vv` propagates
// CEL_ERROR / CEL_UNKNOWN on either operand without invoking the
// inner lookup.  Spec ref: `langdef.md` §"Errors and unknowns".

TEST_F(OptionalTest, SelectFieldErrorSrcPropagatesError) {
  uint32_t src = MakeSlotError(CEL_ERR_DIVIDE_BY_ZERO);
  uint32_t out = MakeSlot();
  cel_select_optional_field_at_vv(out, src, MakeSlotString("c", 1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(OptionalTest, SelectFieldUnknownSrcPropagatesUnknown) {
  uint32_t src = MakeSlotUnknown(7u);
  uint32_t out = MakeSlot();
  cel_select_optional_field_at_vv(out, src, MakeSlotString("c", 1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(out)->payload.unk, 7u);
}

TEST_F(OptionalTest, SelectFieldErrorKeyPropagatesError) {
  // Real-world shape: the key sub-expression poisoned at runtime
  // (e.g. integer overflow on a computed index).  The optional
  // kernel must surface the poison directly.
  uint32_t m = MakeSlot();
  cel_map_create(m, 0);
  uint32_t key = MakeSlotError(CEL_ERR_OVERFLOW);
  uint32_t out = MakeSlot();
  cel_select_optional_field_at_vv(out, m, key);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(OptionalTest, SelectFieldUnknownKeyPropagatesUnknown) {
  uint32_t m = MakeSlot();
  cel_map_create(m, 0);
  uint32_t key = MakeSlotUnknown(42u);
  uint32_t out = MakeSlot();
  cel_select_optional_field_at_vv(out, m, key);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(out)->payload.unk, 42u);
}

TEST_F(OptionalTest, SelectFieldOnNonContainerIsTypeMismatch) {
  uint32_t src = MakeSlotInt(7);  // not a map/list/message/optional
  uint32_t out = MakeSlot();
  uint32_t query = MakeSlotString("x", 1);
  cel_select_optional_field_at_vv(out, src, query);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// ── cel_equals_at_vv arm for CEL_OPTIONAL ─────────────────

TEST_F(OptionalTest, EqualsBothNoneIsTrue) {
  uint32_t a = MakeSlot();
  uint32_t b = MakeSlot();
  cel_optional_none_at(a);
  cel_optional_none_at(b);
  uint32_t out = MakeSlot();
  cel_equals_at_vv(out, a, b);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(At(out)->payload.b, 1);
}

TEST_F(OptionalTest, EqualsPresentMismatchIsFalse) {
  uint32_t a = MakeSlot();
  uint32_t b = MakeSlot();
  uint32_t v = MakeSlotInt(1);
  cel_optional_of_at_v(a, v);
  cel_optional_none_at(b);
  uint32_t out = MakeSlot();
  cel_equals_at_vv(out, a, b);
  EXPECT_EQ(At(out)->payload.b, 0);
}

TEST_F(OptionalTest, EqualsBothSomeWithEqualInnerIsTrue) {
  uint32_t a = MakeSlot();
  uint32_t b = MakeSlot();
  uint32_t va = MakeSlotInt(5);
  uint32_t vb = MakeSlotInt(5);
  cel_optional_of_at_v(a, va);
  cel_optional_of_at_v(b, vb);
  uint32_t out = MakeSlot();
  cel_equals_at_vv(out, a, b);
  EXPECT_EQ(At(out)->payload.b, 1);
}

TEST_F(OptionalTest, EqualsBothSomeWithDifferentInnerIsFalse) {
  uint32_t a = MakeSlot();
  uint32_t b = MakeSlot();
  uint32_t va = MakeSlotInt(5);
  uint32_t vb = MakeSlotInt(6);
  cel_optional_of_at_v(a, va);
  cel_optional_of_at_v(b, vb);
  uint32_t out = MakeSlot();
  cel_equals_at_vv(out, a, b);
  EXPECT_EQ(At(out)->payload.b, 0);
}

// ── A4-A7: load-bearing gaps flagged in 2026-05-21 Slice A review ──

// A4: `optional.of(1).or(<UNKNOWN>)` must return `optional.of(1)`,
// not propagate the UNKNOWN — `cel_optional_or_at_vv` short-circuits
// when LHS is Some (kernel returns at the `*out = *opt;` arm before
// touching RHS).  langdef §"Optional types" matches cel-cpp
// `OptionalValue::Or` (third_party/cel-cpp/runtime/optional_types.cc).
TEST_F(OptionalTest, OrLhsSomeShortCircuitsPoisonRhs) {
  uint32_t lhs = MakeSlot();
  uint32_t v_lhs = MakeSlotInt(1);
  cel_optional_of_at_v(lhs, v_lhs);
  uint32_t rhs = MakeSlotUnknown(42u);  // would poison if dereferenced
  uint32_t out = MakeSlot();
  cel_optional_or_at_vv(out, lhs, rhs);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 1u);
  EXPECT_EQ(CellOf(out)->inner.kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(CellOf(out)->inner.payload.i, 1);
}

// A5: `cel_select_optional_field` on a CEL_LIST_ARENA source with an
// in-bounds CEL_INT key dispatches to `cel_list_at_arena` and wraps
// the result in `optional.of`.  Kernel branch at cel_optional.c:280.
TEST_F(OptionalTest, SelectFieldOnArenaListInBoundsProducesSome) {
  uint32_t list = MakeSlot();
  cel_list_create(list, 2);
  uint32_t v0 = MakeSlotString("a", 1);
  uint32_t v1 = MakeSlotString("b", 1);
  cel_list_append_at(list, v0);
  cel_list_append_at(list, v1);

  uint32_t out = MakeSlot();
  uint32_t idx = MakeSlotInt(1);
  cel_select_optional_field_at_vv(out, list, idx);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 1u);
  EXPECT_EQ(CellOf(out)->inner.kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(CellOf(out)->inner.payload.s.len, 1u);
}

// A6: out-of-bounds index on a CEL_LIST_ARENA source returns None
// (NOT an error) — `cel_list_at_arena` poisons with
// CEL_ERR_INDEX_OUT_OF_BOUNDS into the scratch cell; `is_absent_error`
// (cel_optional.c:261) reinterprets it as `optional.none()`.
// Symmetric to the map-absent-key branch already covered by
// `SelectFieldOnMapAbsentKeyProducesNone`.
TEST_F(OptionalTest, SelectFieldOnArenaListOutOfBoundsProducesNone) {
  uint32_t list = MakeSlot();
  cel_list_create(list, 1);
  uint32_t v0 = MakeSlotInt(7);
  cel_list_append_at(list, v0);

  uint32_t out = MakeSlot();
  uint32_t idx = MakeSlotInt(99);  // out of bounds
  cel_select_optional_field_at_vv(out, list, idx);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_OPTIONAL));
  EXPECT_EQ(CellOf(out)->present, 0u);
}

// A7: `cel_equals_at_vv` on cross-kind `optional(_) == non-optional`
// returns `false`, NOT an error — per langdef §"Equality" the
// polymorphic equality is total: distinct types always compare
// unequal rather than raising.  This is the load-bearing check that
// `equality_kernel`'s cross-kind fall-through arm does NOT poison.
TEST_F(OptionalTest, EqualsOptionalVsNonOptionalIsFalse) {
  uint32_t opt = MakeSlot();
  uint32_t v = MakeSlotInt(1);
  cel_optional_of_at_v(opt, v);
  uint32_t bare = MakeSlotInt(1);  // same value, different kind
  uint32_t out = MakeSlot();
  cel_equals_at_vv(out, opt, bare);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(At(out)->payload.b, 0);
}

// ── Predicate-gated insert / append (`{?k: v}` / `[?e]`) ──

// Helper: arena-decode the ArenaListHeader pointed at by a CelValue
// at `list_slot` (kind must be CEL_LIST_ARENA).
const ArenaListHeader* ListHeaderOf(uint32_t list_slot) {
  const CelValue* v = cel_value_at(list_slot);
  return reinterpret_cast<const ArenaListHeader*>(
      cel_mem_base() + v->payload.arena_list.header_ptr);
}
const ArenaMapHeader* MapHeaderOf(uint32_t map_slot) {
  const CelValue* v = cel_value_at(map_slot);
  return reinterpret_cast<const ArenaMapHeader*>(
      cel_mem_base() + v->payload.arena_map.header_ptr);
}

TEST_F(OptionalTest, ListAppendIfPresentSomeAppendsInner) {
  uint32_t list = MakeSlot();
  cel_list_create(list, 2);
  uint32_t opt = MakeSlot();
  cel_optional_of_at_v(opt, MakeSlotInt(7));
  cel_list_append_at_if_present(list, opt);
  ASSERT_EQ(At(list)->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(ListHeaderOf(list)->count, 1u);
}

TEST_F(OptionalTest, ListAppendIfPresentNoneIsNoOp) {
  uint32_t list = MakeSlot();
  cel_list_create(list, 2);
  uint32_t opt = MakeSlot();
  cel_optional_none_at(opt);
  cel_list_append_at_if_present(list, opt);
  EXPECT_EQ(At(list)->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(ListHeaderOf(list)->count, 0u);
}

TEST_F(OptionalTest, ListAppendIfPresentErrorPropagatesIntoList) {
  uint32_t list = MakeSlot();
  cel_list_create(list, 2);
  uint32_t err = MakeSlotError(CEL_ERR_DIVIDE_BY_ZERO);
  cel_list_append_at_if_present(list, err);
  EXPECT_EQ(At(list)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(list)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(OptionalTest, ListAppendIfPresentUnknownPropagatesIntoList) {
  uint32_t list = MakeSlot();
  cel_list_create(list, 2);
  uint32_t unk = MakeSlotUnknown(11u);
  cel_list_append_at_if_present(list, unk);
  EXPECT_EQ(At(list)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(list)->payload.unk, 11u);
}

TEST_F(OptionalTest, ListAppendIfPresentNonOptionalIsTypeMismatch) {
  uint32_t list = MakeSlot();
  cel_list_create(list, 2);
  uint32_t bare = MakeSlotInt(1);  // not CEL_OPTIONAL
  cel_list_append_at_if_present(list, bare);
  EXPECT_EQ(At(list)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(list)->payload.err,
            static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(OptionalTest, ListAppendIfPresentPoisonedListIsNoOp) {
  // Symmetric to cel_list_append_at_if_bool: if the list slot is
  // already poisoned (kind != CEL_LIST_ARENA), the call must not
  // overwrite the existing poison.  Important because earlier
  // entries' 3VL absorption may have stamped the list before the
  // `_if_present` call.
  uint32_t list = MakeSlot();
  CelValue* lv = cel_value_at(list);
  lv->kind = CEL_ERROR;
  lv->payload.err = CEL_ERR_OVERFLOW;
  uint32_t opt = MakeSlot();
  cel_optional_of_at_v(opt, MakeSlotInt(7));
  cel_list_append_at_if_present(list, opt);
  EXPECT_EQ(At(list)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(list)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(OptionalTest, MapInsertIfPresentSomeInsertsInner) {
  uint32_t map = MakeSlot();
  cel_map_create(map, 2);
  uint32_t key = MakeSlotString("k1", 2);
  uint32_t opt = MakeSlot();
  cel_optional_of_at_v(opt, MakeSlotString("v1", 2));
  cel_map_insert_at_if_present(map, key, opt);
  ASSERT_EQ(At(map)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapHeaderOf(map)->count, 1u);
}

TEST_F(OptionalTest, MapInsertIfPresentNoneIsNoOp) {
  uint32_t map = MakeSlot();
  cel_map_create(map, 2);
  uint32_t key = MakeSlotString("k1", 2);
  uint32_t opt = MakeSlot();
  cel_optional_none_at(opt);
  cel_map_insert_at_if_present(map, key, opt);
  EXPECT_EQ(At(map)->kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(MapHeaderOf(map)->count, 0u);
}

TEST_F(OptionalTest, MapInsertIfPresentErrorPropagatesIntoMap) {
  uint32_t map = MakeSlot();
  cel_map_create(map, 2);
  uint32_t key = MakeSlotString("k", 1);
  uint32_t err = MakeSlotError(CEL_ERR_DIVIDE_BY_ZERO);
  cel_map_insert_at_if_present(map, key, err);
  EXPECT_EQ(At(map)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(map)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(OptionalTest, MapInsertIfPresentUnknownPropagatesIntoMap) {
  uint32_t map = MakeSlot();
  cel_map_create(map, 2);
  uint32_t key = MakeSlotString("k", 1);
  uint32_t unk = MakeSlotUnknown(13u);
  cel_map_insert_at_if_present(map, key, unk);
  EXPECT_EQ(At(map)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(map)->payload.unk, 13u);
}

TEST_F(OptionalTest, MapInsertIfPresentNonOptionalIsTypeMismatch) {
  uint32_t map = MakeSlot();
  cel_map_create(map, 2);
  uint32_t key = MakeSlotString("k", 1);
  uint32_t bare = MakeSlotInt(1);  // not CEL_OPTIONAL
  cel_map_insert_at_if_present(map, key, bare);
  EXPECT_EQ(At(map)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(map)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(OptionalTest, MapInsertIfPresentPoisonedMapIsNoOp) {
  uint32_t map = MakeSlot();
  CelValue* mv = cel_value_at(map);
  mv->kind = CEL_ERROR;
  mv->payload.err = CEL_ERR_OVERFLOW;
  uint32_t key = MakeSlotString("k", 1);
  uint32_t opt = MakeSlot();
  cel_optional_of_at_v(opt, MakeSlotInt(7));
  cel_map_insert_at_if_present(map, key, opt);
  EXPECT_EQ(At(map)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(map)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

// Mixed Some + None — exact shape that `{?k1: opt.of(v), ?k2: opt.none()}`
// produces.  After two calls only the Some entry should remain.
TEST_F(OptionalTest, MapInsertIfPresentMixedSomeNonePreservesOnlySome) {
  uint32_t map = MakeSlot();
  cel_map_create(map, 2);
  uint32_t opt_some = MakeSlot();
  cel_optional_of_at_v(opt_some, MakeSlotString("v1", 2));
  uint32_t opt_none = MakeSlot();
  cel_optional_none_at(opt_none);
  cel_map_insert_at_if_present(map, MakeSlotString("k1", 2), opt_some);
  cel_map_insert_at_if_present(map, MakeSlotString("k2", 2), opt_none);
  EXPECT_EQ(MapHeaderOf(map)->count, 1u);
}

TEST_F(OptionalTest, ListAppendIfPresentMixedSomeNonePreservesOnlySome) {
  uint32_t list = MakeSlot();
  cel_list_create(list, 2);
  uint32_t opt_some = MakeSlot();
  cel_optional_of_at_v(opt_some, MakeSlotInt(7));
  uint32_t opt_none = MakeSlot();
  cel_optional_none_at(opt_none);
  cel_list_append_at_if_present(list, opt_some);
  cel_list_append_at_if_present(list, opt_none);
  EXPECT_EQ(ListHeaderOf(list)->count, 1u);
}

// ── Proto `Foo{?field: opt_v}` predicate-gated set ────────

// Helper: build a CEL_MESSAGE CelValue at a fresh slot.  The test
// override of `cel_host_cel_set_field` ignores the payload — only
// the kind matters for the kernel's pre-check.
uint32_t MakeSlotMessage(uint32_t msg_slot_payload) {
  uint32_t s = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* v = cel_value_at(s);
  v->kind = CEL_MESSAGE;
  v->payload.msg_slot = msg_slot_payload;
  return s;
}

TEST_F(OptionalTest, SetFieldIfPresentSomeForwardsToHostWithInnerOffset) {
  uint32_t msg = MakeSlotMessage(/*msg_slot_payload=*/1);
  uint32_t opt = MakeSlot();
  cel_optional_of_at_v(opt, MakeSlotInt(5));
  cel_set_field_at_if_present(msg, /*field_ref_id=*/42, opt);
  ASSERT_EQ(g_host_set_field_calls, 1);
  EXPECT_EQ(g_host_set_field_last.msg_slot, msg);
  EXPECT_EQ(g_host_set_field_last.field_ref_id, 42u);
  // value_slot points at OptionalCell.inner (8 bytes past the cell
  // base).  This is the load-bearing contract delta vs. previous
  // `cel_set_field` callers, which passed standalone workspace slots.
  uint32_t expected_inner_off =
      At(opt)->payload.opt +
      static_cast<uint32_t>(offsetof(OptionalCell, inner));
  EXPECT_EQ(g_host_set_field_last.value_slot, expected_inner_off);
  // The host stub treats msg_slot as opaque; the kernel must not
  // have mutated the message slot's kind.
  EXPECT_EQ(At(msg)->kind, static_cast<uint32_t>(CEL_MESSAGE));
}

TEST_F(OptionalTest, SetFieldIfPresentNoneShortCircuitsBeforeHost) {
  // Load-bearing correctness assertion: a None-path call MUST NOT
  // reach the host trampoline.  If it did, the proto field would
  // get unset → set to zero value, which violates proto semantics
  // (the field should stay unset so `has()` returns false).
  uint32_t msg = MakeSlotMessage(/*msg_slot_payload=*/1);
  uint32_t opt = MakeSlot();
  cel_optional_none_at(opt);
  cel_set_field_at_if_present(msg, /*field_ref_id=*/42, opt);
  EXPECT_EQ(g_host_set_field_calls, 0)
      << "None-path call must NOT reach cel_host_cel_set_field";
  EXPECT_EQ(At(msg)->kind, static_cast<uint32_t>(CEL_MESSAGE));
}

TEST_F(OptionalTest, SetFieldIfPresentErrorPropagatesIntoMessage) {
  uint32_t msg = MakeSlotMessage(1);
  uint32_t err = MakeSlotError(CEL_ERR_DIVIDE_BY_ZERO);
  cel_set_field_at_if_present(msg, /*field_ref_id=*/42, err);
  EXPECT_EQ(g_host_set_field_calls, 0);
  EXPECT_EQ(At(msg)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(msg)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(OptionalTest, SetFieldIfPresentUnknownPropagatesIntoMessage) {
  uint32_t msg = MakeSlotMessage(1);
  uint32_t unk = MakeSlotUnknown(17u);
  cel_set_field_at_if_present(msg, /*field_ref_id=*/42, unk);
  EXPECT_EQ(g_host_set_field_calls, 0);
  EXPECT_EQ(At(msg)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(msg)->payload.unk, 17u);
}

TEST_F(OptionalTest, SetFieldIfPresentNonOptionalIsTypeMismatch) {
  uint32_t msg = MakeSlotMessage(1);
  uint32_t bare = MakeSlotInt(5);  // not CEL_OPTIONAL
  cel_set_field_at_if_present(msg, /*field_ref_id=*/42, bare);
  EXPECT_EQ(g_host_set_field_calls, 0);
  EXPECT_EQ(At(msg)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(msg)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(OptionalTest, SetFieldIfPresentPoisonedMessageIsNoOp) {
  // Earlier 3VL absorption (a prior entry's optional was ERROR)
  // stamped the message; preserve the poison, do nothing.
  uint32_t msg = MakeSlot();
  CelValue* mv = cel_value_at(msg);
  mv->kind = CEL_ERROR;
  mv->payload.err = CEL_ERR_OVERFLOW;
  uint32_t opt = MakeSlot();
  cel_optional_of_at_v(opt, MakeSlotInt(5));
  cel_set_field_at_if_present(msg, /*field_ref_id=*/42, opt);
  EXPECT_EQ(g_host_set_field_calls, 0);
  EXPECT_EQ(At(msg)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(msg)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(OptionalTest, SetFieldIfPresentMixedSomeNoneCallsHostOnce) {
  // The canonical `Foo{?single_int32: opt.of(5), ?single_str: opt.none()}`
  // shape: two entries, one materialises, one short-circuits.  Host
  // stub fires exactly once with the Some entry's args.
  uint32_t msg = MakeSlotMessage(1);
  uint32_t opt_some = MakeSlot();
  cel_optional_of_at_v(opt_some, MakeSlotInt(5));
  uint32_t opt_none = MakeSlot();
  cel_optional_none_at(opt_none);
  cel_set_field_at_if_present(msg, /*field_ref_id=*/42, opt_some);
  cel_set_field_at_if_present(msg, /*field_ref_id=*/43, opt_none);
  EXPECT_EQ(g_host_set_field_calls, 1);
  // The recorded args belong to the Some call (field_ref_id=42),
  // not the None call (43) — proves order + which call reached host.
  EXPECT_EQ(g_host_set_field_last.field_ref_id, 42u);
}

}  // namespace
}  // namespace celwasm
