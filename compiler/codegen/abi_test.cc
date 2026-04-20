#include "compiler/codegen/abi.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "cel/expr/checked.pb.h"
#include "compiler/codegen/cel_abi.pb.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/typed_ast.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Minimal wasm custom-section parser.
//
// The .wasm binary format is:
//   magic (4 bytes) + version (4 bytes) + section*
//   section = 1 byte id + ULEB128 length + <length bytes>
//
// Custom sections have id = 0 and the payload starts with a
// name: ULEB128 length + UTF-8 name bytes, then the payload.
// See https://webassembly.github.io/spec/core/binary/modules.html.
//
// We only need the custom-section path here.  Returns the payload
// (after the name) for the first custom section named `section_name`,
// or nullopt if none matches.
// Reads a ULEB128 from `wasm` starting at `*pos`, advancing `*pos`.
// Returns false on truncation.
bool ReadUleb(const std::vector<uint8_t>& wasm, size_t* pos, uint64_t* out) {
  uint64_t value = 0;
  int shift = 0;
  while (*pos < wasm.size()) {
    const uint8_t b = wasm.at((*pos)++);
    value |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      *out = value;
      return true;
    }
    shift += 7;
  }
  return false;
}

std::optional<std::vector<uint8_t>> FindCustomSection(
    const std::vector<uint8_t>& wasm, absl::string_view section_name) {
  if (wasm.size() < 8) return std::nullopt;
  size_t pos = 8;  // skip magic + version
  while (pos < wasm.size()) {
    const uint8_t id = wasm.at(pos++);
    uint64_t section_len = 0;
    if (!ReadUleb(wasm, &pos, &section_len)) return std::nullopt;
    if (pos + section_len > wasm.size()) return std::nullopt;
    const size_t section_start = pos;
    const size_t section_end = section_start + section_len;
    if (id == 0) {  // custom section
      size_t p = section_start;
      uint64_t name_len = 0;
      if (!ReadUleb(wasm, &p, &name_len)) return std::nullopt;
      if (p + name_len > section_end) return std::nullopt;
      const absl::string_view this_name(
          reinterpret_cast<const char*>(wasm.data() + p), name_len);
      p += name_len;
      if (this_name == section_name) {
        return std::vector<uint8_t>(wasm.begin() + p,
                                    wasm.begin() + section_end);
      }
    }
    pos = section_end;
  }
  return std::nullopt;
}

// Builds a `TypedAst` by running the same pipeline codegen uses.
TypedAst CheckOrDie(absl::string_view source) {
  auto typed = ParseAndCheck(source, CheckOptions{});
  CHECK_OK(typed);
  return *std::move(typed);
}

TEST(CelAbiTest, BuildPopulatesVersionSourceAndCheckedExpr) {
  TypedAst typed = CheckOrDie("1 + 2");
  auto abi_or = BuildCelAbi(typed, "1 + 2");
  ASSERT_THAT(abi_or, IsOk());
  const CelAbi& abi = *abi_or;

  EXPECT_EQ(abi.version(), kCelAbiVersion);
  EXPECT_EQ(abi.cel_source(), "1 + 2");
  EXPECT_TRUE(abi.has_checked());
  // The root call is `_+_` on two int literals.  We don't pin the
  // exact IDs the cel-cpp checker assigns, just that the tree is
  // non-empty and carries type info.
  EXPECT_TRUE(abi.checked().has_expr());
  EXPECT_GT(abi.checked().type_map_size(), 0);
}

TEST(CelAbiTest, BuildDefaultsLayoutAndFunctionSet) {
  TypedAst typed = CheckOrDie("true");
  auto abi_or = BuildCelAbi(typed, "true");
  ASSERT_THAT(abi_or, IsOk());
  const CelAbi& abi = *abi_or;

  EXPECT_TRUE(abi.has_function_set());
  EXPECT_EQ(abi.function_set().required_imports_size(), 0);
  EXPECT_TRUE(abi.has_layout());
  EXPECT_EQ(abi.layout().initial_pages(), 1u);
  EXPECT_EQ(abi.layout().max_pages(), 0u);
  // No field selects in `true` — the intern table stays empty.
  EXPECT_EQ(abi.fields_size(), 0);
}

TEST(CelAbiTest, AttachCelAbiSectionRoundTripsThroughSerializedWasm) {
  const std::string source = "1 + 2 * 3";
  TypedAst typed = CheckOrDie(source);
  auto abi_or = BuildCelAbi(typed, source);
  ASSERT_THAT(abi_or, IsOk());

  WasmModule mod;
  BinaryenExpressionRef body = BinaryenNop(mod.raw());
  mod.AddFunction("nop", {}, BinaryenTypeNone(), {}, body);
  ASSERT_THAT(AttachCelAbiSection(mod, *abi_or), IsOk());
  ASSERT_THAT(mod.Validate(), IsOk());

  auto bytes_or = mod.Serialize();
  ASSERT_THAT(bytes_or, IsOk());

  auto payload = FindCustomSection(*bytes_or, "cel.abi");
  ASSERT_TRUE(payload.has_value())
      << "serialized .wasm missing `cel.abi` custom section";
  std::vector<uint8_t> payload_bytes;
  if (payload.has_value()) payload_bytes = *std::move(payload);

  CelAbi decoded;
  ASSERT_TRUE(decoded.ParseFromArray(payload_bytes.data(),
                                     static_cast<int>(payload_bytes.size())));
  EXPECT_EQ(decoded.version(), kCelAbiVersion);
  EXPECT_EQ(decoded.cel_source(), source);
  EXPECT_TRUE(decoded.has_checked());
  EXPECT_TRUE(decoded.checked().has_expr());
  EXPECT_EQ(decoded.layout().initial_pages(), 1u);
}

TEST(CelAbiTest, BuildPopulatesFieldTableForSelectExpressions) {
  (void)celwasm::testdata::Customer::descriptor();  // pin into pool.
  CheckOptions opts;
  opts.variable_specs.emplace_back("c:celwasm.testdata.Customer");
  auto typed = ParseAndCheck("c.billing_address.city", opts);
  ASSERT_THAT(typed, IsOk());
  auto abi_or = BuildCelAbi(*typed, "c.billing_address.city");
  ASSERT_THAT(abi_or, IsOk());
  const CelAbi& abi = *abi_or;

  // Two distinct selects → two entries.  Pre-order walk: outer
  // `.city` is visited before its operand `.billing_address`, so
  // `city` lands at id 0 and `billing_address` at id 1.
  ASSERT_EQ(abi.fields_size(), 2);
  EXPECT_EQ(abi.fields(0).id(), 0u);
  EXPECT_EQ(abi.fields(0).name(), "city");
  EXPECT_GT(abi.fields(0).field_number(), 0u);
  EXPECT_EQ(abi.fields(1).id(), 1u);
  EXPECT_EQ(abi.fields(1).name(), "billing_address");
  EXPECT_GT(abi.fields(1).field_number(), 0u);
}

TEST(CelAbiTest, SectionNameConstantMatchesDesignDoc) {
  // The string `cel.abi` is part of the host contract.  A typo in
  // this constant would break every host silently — pin it.
  EXPECT_EQ(kCelAbiSectionName, "cel.abi");
}

}  // namespace
}  // namespace celwasm
