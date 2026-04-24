// Tests for abi_decode.  Covers:
//   - Happy path: a hand-built wasm byte stream with a `cel.abi`
//     custom section round-trips through FindCustomSection +
//     DecodeCelAbiProto into a DecodedCelAbi.
//   - End-to-end: compile `"x"` with `x:int` through the full
//     pipeline, serialize, hand the bytes back to
//     DecodeCelAbiFromWasm, verify the variable entry matches the
//     StaticLayout the compiler emitted.
//   - Error paths: malformed magic / version, missing section,
//     truncated / invalid payload, truncated LEB128.

#include "compiler_v2/api/internal/abi_decode.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/compile.h"
#include "compiler_v2/ir/annotations.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// ──────────────────────────────────────────────────────────────
// Minimal wasm-module byte-stream builder.
//
// The decoder parses raw wasm bytes; driving real codegen from
// every test would slow the suite and couple decoder correctness
// to codegen bugs.  Instead, we hand-build the byte streams here.
// ──────────────────────────────────────────────────────────────

void AppendLeb128U32(std::vector<uint8_t>& out, uint32_t value) {
  while (true) {
    uint8_t b = value & 0x7f;
    value >>= 7;
    if (value == 0) {
      out.push_back(b);
      return;
    }
    out.push_back(b | 0x80u);
  }
}

std::vector<uint8_t> MakeWasmWithCustomSection(absl::string_view name,
                                               absl::Span<const uint8_t> body) {
  std::vector<uint8_t> out;
  // Header.
  out.insert(out.end(), {0x00, 0x61, 0x73, 0x6d});
  uint32_t version = 1;
  const auto* vb = reinterpret_cast<const uint8_t*>(&version);
  out.insert(out.end(), vb, vb + 4);
  // Custom section: id=0 + size + name_len + name + body.
  std::vector<uint8_t> section;
  AppendLeb128U32(section, static_cast<uint32_t>(name.size()));
  section.insert(section.end(), name.begin(), name.end());
  section.insert(section.end(), body.begin(), body.end());
  out.push_back(0);  // section_id
  AppendLeb128U32(out, static_cast<uint32_t>(section.size()));
  out.insert(out.end(), section.begin(), section.end());
  return out;
}

std::vector<uint8_t> SerializeAbi(const celwasm::abi::CelAbi& abi) {
  std::string bytes;
  abi.SerializeToString(&bytes);
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// ──────────────────────────────────────────────────────────────
// Happy-path decode from a hand-built byte stream.
// ──────────────────────────────────────────────────────────────

TEST(AbiDecodeTest, DecodesSingleVariableEntry) {
  celwasm::abi::CelAbi abi;
  abi.set_version(1);
  auto* v = abi.add_variables();
  v->set_name("x");
  v->set_local_index(0);
  v->set_slot_offset(16);
  v->set_repr(static_cast<uint32_t>(Repr::kInt));

  std::vector<uint8_t> payload = SerializeAbi(abi);
  std::vector<uint8_t> wasm = MakeWasmWithCustomSection("cel.abi", payload);

  auto decoded = DecodeCelAbiFromWasm(wasm);
  ASSERT_THAT(decoded, IsOk());
  EXPECT_EQ(decoded->version, 1u);
  ASSERT_EQ(decoded->variables.size(), 1u);
  EXPECT_EQ(decoded->variables[0].name, "x");
  EXPECT_EQ(decoded->variables[0].local_index, 0u);
  EXPECT_EQ(decoded->variables[0].slot_offset, 16u);
  EXPECT_EQ(decoded->variables[0].repr, Repr::kInt);

  // Name index populated.
  auto it = decoded->by_name.find("x");
  ASSERT_NE(it, decoded->by_name.end());
  EXPECT_EQ(it->second, &decoded->variables[0]);
}

TEST(AbiDecodeTest, DecodesMultipleVariablesInOrder) {
  celwasm::abi::CelAbi abi;
  abi.set_version(1);
  for (uint32_t i = 0; i < 4; ++i) {
    auto* v = abi.add_variables();
    v->set_name(absl::StrCat("v", i));
    v->set_local_index(i);
    v->set_slot_offset(16 + i * 24);
    v->set_repr(static_cast<uint32_t>(Repr::kInt));
  }
  std::vector<uint8_t> wasm =
      MakeWasmWithCustomSection("cel.abi", SerializeAbi(abi));
  auto decoded = DecodeCelAbiFromWasm(wasm);
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->variables.size(), 4u);
  for (uint32_t i = 0; i < 4; ++i) {
    EXPECT_EQ(decoded->variables[i].name, absl::StrCat("v", i));
    EXPECT_EQ(decoded->variables[i].local_index, i);
    EXPECT_EQ(decoded->variables[i].slot_offset, 16 + i * 24);
  }
  EXPECT_EQ(decoded->by_name.size(), 4u);
}

TEST(AbiDecodeTest, DecodesEveryScalarRepr) {
  // Locks the Repr u32 <-> enum mapping: one variable per Repr the
  // host marshal knows how to encode at M2.B (scalars + null).
  struct Case {
    absl::string_view name;
    Repr repr;
  };
  const Case cases[] = {
      {"n", Repr::kNull},  {"b", Repr::kBool},       {"i", Repr::kInt},
      {"u", Repr::kUint},  {"d", Repr::kDouble},     {"s", Repr::kString},
      {"y", Repr::kBytes}, {"dur", Repr::kDuration}, {"ts", Repr::kTimestamp},
  };
  celwasm::abi::CelAbi abi;
  abi.set_version(1);
  uint32_t idx = 0;
  for (const auto& c : cases) {
    auto* v = abi.add_variables();
    v->set_name(std::string(c.name));
    v->set_local_index(idx);
    v->set_slot_offset(16 + idx * 24);
    v->set_repr(static_cast<uint32_t>(c.repr));
    ++idx;
  }
  std::vector<uint8_t> wasm =
      MakeWasmWithCustomSection("cel.abi", SerializeAbi(abi));
  auto decoded = DecodeCelAbiFromWasm(wasm);
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->variables.size(), static_cast<size_t>(std::size(cases)));
  for (size_t i = 0; i < std::size(cases); ++i) {
    EXPECT_EQ(decoded->variables[i].repr, cases[i].repr)
        << "case " << i << " name=" << cases[i].name;
  }
}

// ──────────────────────────────────────────────────────────────
// End-to-end: feed real compiler output through the decoder.
// ──────────────────────────────────────────────────────────────

TEST(AbiDecodeTest, RoundTripsCompilerOutput) {
  CompileOptions opts;
  opts.check.variable_specs = {"x:int"};
  auto artifact = Compile("x", opts);
  ASSERT_THAT(artifact, IsOk());

  auto decoded = DecodeCelAbiFromWasm(artifact->wasm_bytes);
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->variables.size(), 1u);
  const auto& v = decoded->variables[0];
  EXPECT_EQ(v.name, "x");
  EXPECT_EQ(v.local_index, 0u);
  EXPECT_EQ(v.slot_offset, artifact->layout.variables[0].slot_offset);
  EXPECT_EQ(v.repr, Repr::kInt);
}

TEST(AbiDecodeTest, RoundTripsMultipleVariablesFromCompiler) {
  // Multi-variable expressions require a kCall root (`b || s == "" ||
  // i > 0`) which expr_lower rejects until M3.  The hand-built
  // DecodesMultipleVariablesInOrder test above already covers the
  // decoder side; skip this scenario until M3 unlocks compiling it
  // end-to-end.
  GTEST_SKIP() << "multi-variable round-trip needs M3 kCall-as-root";
}

TEST(AbiDecodeTest, RoundTripsEveryScalarReprFromCompiler) {
  for (absl::string_view spec :
       {"x:bool", "x:int", "x:uint", "x:double", "x:string", "x:bytes"}) {
    CompileOptions opts;
    opts.check.variable_specs = {std::string(spec)};
    auto artifact = Compile("x", opts);
    ASSERT_THAT(artifact, IsOk()) << spec;
    auto decoded = DecodeCelAbiFromWasm(artifact->wasm_bytes);
    ASSERT_THAT(decoded, IsOk()) << spec;
    ASSERT_EQ(decoded->variables.size(), 1u) << spec;
    EXPECT_EQ(decoded->variables[0].name, "x") << spec;
    EXPECT_EQ(decoded->variables[0].slot_offset,
              artifact->layout.variables[0].slot_offset)
        << spec;
  }
}

TEST(AbiDecodeTest, RoundTripsWithLiteralOnlyProgramEmitsEmptyAbi) {
  // `42` — no variables.  ABI section is still emitted (an empty
  // CelAbi); decoder should return a DecodedCelAbi with version=1
  // and variables.empty().
  auto artifact = Compile("42", {});
  ASSERT_THAT(artifact, IsOk());
  auto decoded = DecodeCelAbiFromWasm(artifact->wasm_bytes);
  ASSERT_THAT(decoded, IsOk());
  EXPECT_EQ(decoded->version, 1u);
  EXPECT_TRUE(decoded->variables.empty());
}

// ──────────────────────────────────────────────────────────────
// Error paths.
// ──────────────────────────────────────────────────────────────

TEST(AbiDecodeErrorTest, FailsOnTooShortStream) {
  std::vector<uint8_t> too_short = {0x00, 0x61, 0x73};  // 3 bytes
  EXPECT_THAT(DecodeCelAbiFromWasm(too_short),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, FailsOnBadMagic) {
  std::vector<uint8_t> bad_magic = {0xff, 0xff, 0xff, 0xff,
                                    0x01, 0x00, 0x00, 0x00};
  EXPECT_THAT(DecodeCelAbiFromWasm(bad_magic),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, FailsOnUnsupportedVersion) {
  std::vector<uint8_t> bad_ver = {0x00, 0x61, 0x73, 0x6d,
                                  0x02, 0x00, 0x00, 0x00};
  EXPECT_THAT(DecodeCelAbiFromWasm(bad_ver),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, ReturnsNotFoundWhenSectionIsMissing) {
  // Valid wasm header but no sections — decoder walks to EOF
  // without finding cel.abi.
  std::vector<uint8_t> empty_module = {0x00, 0x61, 0x73, 0x6d,
                                       0x01, 0x00, 0x00, 0x00};
  EXPECT_THAT(DecodeCelAbiFromWasm(empty_module),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(AbiDecodeErrorTest, ReturnsNotFoundWhenOtherCustomSectionsPresent) {
  std::vector<uint8_t> other = {'n', 'o', 't'};
  std::vector<uint8_t> wasm = MakeWasmWithCustomSection("name", other);
  EXPECT_THAT(DecodeCelAbiFromWasm(wasm),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(AbiDecodeErrorTest, FailsOnMalformedAbiPayload) {
  std::vector<uint8_t> garbage = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  std::vector<uint8_t> wasm = MakeWasmWithCustomSection("cel.abi", garbage);
  EXPECT_THAT(DecodeCelAbiFromWasm(wasm),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, FailsOnTruncatedSection) {
  // Hand-craft a module whose first section claims size=100 but the
  // bytes run out after 5.  FindCustomSection should fail with
  // InvalidArgument, not crash or silently skip.
  std::vector<uint8_t> wasm = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  wasm.push_back(0);    // section_id = 0 (custom)
  wasm.push_back(100);  // LEB128 100 — claims 100 bytes follow
  // ...but only 5 follow.
  for (int i = 0; i < 5; ++i)
    wasm.push_back(0);
  EXPECT_THAT(DecodeCelAbiFromWasm(wasm),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, FailsOnOversizeLeb128) {
  // LEB128 encoding of a section size that keeps the continuation
  // bit set for more than 5 bytes.
  std::vector<uint8_t> wasm = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  wasm.push_back(0);  // section_id
  for (int i = 0; i < 6; ++i)
    wasm.push_back(0x80);  // all continuation
  EXPECT_THAT(DecodeCelAbiFromWasm(wasm),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace celwasm
