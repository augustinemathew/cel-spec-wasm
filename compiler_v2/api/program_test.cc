// M1 — Program is a pure data type: bytes + (future) ABI.  No
// wasmtime, no Compiler dep, no Engine dep.  Tests cover the
// trivial properties of the type.
//
// Compile / Plan integration is covered by compiler_test.cc and
// (later) engine_test.cc respectively.

#include "compiler_v2/api/program.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace cel {
namespace {

TEST(ProgramTest, ConstructFromBytes) {
  std::vector<uint8_t> bytes{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  Program p(bytes);
  ASSERT_EQ(p.wasm_bytes().size(), bytes.size());
  for (size_t i = 0; i < bytes.size(); ++i) {
    EXPECT_EQ(p.wasm_bytes()[i], bytes[i]);
  }
}

TEST(ProgramTest, MoveConstructionTransfersBytes) {
  std::vector<uint8_t> bytes{0x00, 0x61, 0x73, 0x6d};
  Program a(bytes);
  Program b = std::move(a);
  EXPECT_EQ(b.wasm_bytes().size(), bytes.size());
}

TEST(ProgramTest, CopyConstructionDuplicatesBytes) {
  std::vector<uint8_t> bytes{0x00, 0x61, 0x73, 0x6d};
  Program a(bytes);
  Program b = a;  // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(a.wasm_bytes().size(), bytes.size());
  EXPECT_EQ(b.wasm_bytes().size(), bytes.size());
}

}  // namespace
}  // namespace cel
