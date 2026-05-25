// Shared test fixture + per-kind slot builders for the
// `cel_string_ext_*` and `cel_string_format` test TUs.
//
// Per `m12-string-ext.md` §5.3 — splitting the kernel TU per topic
// (see §4.1) means the test TUs split alongside it; this header
// owns the gtest fixture and the `Make*` slot builders so each
// per-topic test file stays focused on its kernel matrix.
//
// Lifetime: arena_init + arena_reset run in `SetUp()`; tests build
// slots with the `Make*` helpers then call kernels through the
// public `cel_string_ext.h` ABI.  Output strings borrow into the
// same arena, so all reads (`StringAt` / `At`) stay valid for the
// duration of a single `TEST_F` body.

#ifndef CELWASM_RUNTIME_STRING_EXT_TEST_HELPERS_H_
#define CELWASM_RUNTIME_STRING_EXT_TEST_HELPERS_H_

#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_make.h"
#include "runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {

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

  uint32_t MakeBytes(const std::string& b) {
    return cel_make_bytes(b.data(), static_cast<uint32_t>(b.size()));
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

  std::string BytesAt(uint32_t slot) {
    const CelValue* v = At(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BYTES));
    if (v->payload.bytes.len == 0) return {};
    return {
        reinterpret_cast<const char*>(cel_mem_base() + v->payload.bytes.ptr),
        v->payload.bytes.len};
  }

  void ExpectBytes(uint32_t slot, const std::string& expected) {
    EXPECT_EQ(BytesAt(slot), expected);
  }

  void ExpectError(uint32_t slot, uint32_t err) {
    const CelValue* v = At(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
    EXPECT_EQ(v->payload.err, err);
  }

  void ExpectKind(uint32_t slot, uint32_t kind) {
    EXPECT_EQ(At(slot)->kind, kind);
  }

  void ExpectInt(uint32_t slot, int64_t expected) {
    const CelValue* v = At(slot);
    ASSERT_EQ(v->kind, static_cast<uint32_t>(CEL_INT));
    EXPECT_EQ(v->payload.i, expected);
  }
};

}  // namespace celwasm

#endif  // CELWASM_RUNTIME_STRING_EXT_TEST_HELPERS_H_
