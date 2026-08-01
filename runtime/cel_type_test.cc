#include "runtime/cel_type.h"

#include <cstdint>
#include <string>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_make.h"
#include "runtime/cel_memory.h"
#include "gtest/gtest.h"

// M9.B — type(x) helper coverage.  Spec table cite:
// `doc/langdef.md §"Type Values"` enumerates the 12 spec-defined
// primitive type names.  This file flips one row per kind plus the
// kind-mismatch / 3VL absorb negatives.  The CEL_MESSAGE arm
// dispatches to a host trampoline (M9.C) and is covered end-to-end
// in e2e/type_value_test.cc; here the host-side weak stub
// poisons CEL_ERR_TYPE_MISMATCH so we assert that fall-back.

namespace celwasm {
namespace {

class TypeOfTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }

  uint32_t MakeOut() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  // Allocate + write a CelValue of `kind` at a fresh slot (payload
  // contents irrelevant — type(x) inspects only kind).
  uint32_t MakeOfKind(CelKind kind) {
    uint32_t off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(off);
    v->kind = static_cast<uint32_t>(kind);
    v->payload.i = 0;
    return off;
  }

  std::string TypeNameAt(uint32_t slot) {
    const CelValue* out = cel_value_at(slot);
    EXPECT_EQ(out->kind, static_cast<uint32_t>(CEL_TYPE));
    const char* base = reinterpret_cast<const char*>(cel_mem_base());
    return {base + out->payload.s.ptr, out->payload.s.len};
  }
};

// One TEST_F per spec-defined primitive type-name row — distinct
// stories per kind, parameterising would obscure the type-name
// payload mapping.

TEST_F(TypeOfTest, NullPrimitive) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_NULL));
  EXPECT_EQ(TypeNameAt(out), "null_type");
}

TEST_F(TypeOfTest, BoolPrimitive) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, cel_make_bool(1));
  EXPECT_EQ(TypeNameAt(out), "bool");
}

TEST_F(TypeOfTest, IntPrimitive) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, cel_make_int(-7));
  EXPECT_EQ(TypeNameAt(out), "int");
}

TEST_F(TypeOfTest, UintPrimitive) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, cel_make_uint(7u));
  EXPECT_EQ(TypeNameAt(out), "uint");
}

TEST_F(TypeOfTest, DoublePrimitive) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, cel_make_double(3.14));
  EXPECT_EQ(TypeNameAt(out), "double");
}

TEST_F(TypeOfTest, StringPrimitive) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, cel_make_string("x", 1));
  EXPECT_EQ(TypeNameAt(out), "string");
}

TEST_F(TypeOfTest, BytesPrimitive) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, cel_make_bytes("x", 1));
  EXPECT_EQ(TypeNameAt(out), "bytes");
}

TEST_F(TypeOfTest, ListArenaName) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_LIST_ARENA));
  EXPECT_EQ(TypeNameAt(out), "list");
}

TEST_F(TypeOfTest, ListHostName) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_LIST_HOST));
  EXPECT_EQ(TypeNameAt(out), "list");
}

TEST_F(TypeOfTest, MapArenaName) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_MAP_ARENA));
  EXPECT_EQ(TypeNameAt(out), "map");
}

TEST_F(TypeOfTest, MapHostName) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_MAP_HOST));
  EXPECT_EQ(TypeNameAt(out), "map");
}

TEST_F(TypeOfTest, TypeOfType) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_TYPE));
  EXPECT_EQ(TypeNameAt(out), "type");
}

TEST_F(TypeOfTest, DurationName) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_DURATION));
  EXPECT_EQ(TypeNameAt(out), "google.protobuf.Duration");
}

TEST_F(TypeOfTest, TimestampName) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_TIMESTAMP));
  EXPECT_EQ(TypeNameAt(out), "google.protobuf.Timestamp");
}

// 3VL absorbers — operand ERROR / UNKNOWN propagate verbatim into
// out_slot.  Identical contract to every other slot-out helper; see
// cel_3vl_test.cc for the parameterised matrix.

TEST_F(TypeOfTest, ErrorOperandPropagates) {
  uint32_t in = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* iv = cel_value_at(in);
  iv->kind = CEL_ERROR;
  iv->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, in);
  const CelValue* ov = cel_value_at(out);
  EXPECT_EQ(ov->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(ov->payload.err, static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(TypeOfTest, UnknownOperandPropagates) {
  uint32_t in = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* iv = cel_value_at(in);
  iv->kind = CEL_UNKNOWN;
  iv->payload.unk = 42u;  // arbitrary descriptor offset; copied verbatim.
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, in);
  const CelValue* ov = cel_value_at(out);
  EXPECT_EQ(ov->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(ov->payload.unk, 42u);
}

// CEL_OPTIONAL resolves to the spec type name "optional_type" — see
// `kPrimitiveTypeName[14]` in `cel_type.c`.  Matches cel-cpp
// `OptionalValue::GetRuntimeType()` and the corpus row
// `type(optional.none()) == optional_type`.  An earlier version of
// this test asserted TYPE_MISMATCH back when index 14 was NULL; that
// branch is no longer reachable.
TEST_F(TypeOfTest, OptionalReturnsOptionalType) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_OPTIONAL));
  const CelValue* ov = cel_value_at(out);
  EXPECT_EQ(ov->kind, static_cast<uint32_t>(CEL_TYPE));
  std::string name(
      reinterpret_cast<const char*>(cel_mem_base() + ov->payload.s.ptr),
      ov->payload.s.len);
  EXPECT_EQ(name, "optional_type");
}

// CEL_MESSAGE dispatches to the host trampoline.  In the host build
// the weak stub poisons kTypeMismatch (since no real descriptor
// resolver is wired without the API layer); a stronger override
// lands in cel_host.cc for the wasm path.  Asserting the stub
// behaviour locks the seam in place.
TEST_F(TypeOfTest, MessageDispatchesHostStub) {
  uint32_t out = MakeOut();
  cel_type_of_at_v(out, MakeOfKind(CEL_MESSAGE));
  const CelValue* ov = cel_value_at(out);
  EXPECT_EQ(ov->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(ov->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

}  // namespace
}  // namespace celwasm
