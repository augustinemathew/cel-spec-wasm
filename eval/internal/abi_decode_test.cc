// Tests for abi_decode.  Covers:
//   - Happy path: hand-built wasm byte stream with a `cel.abi` custom
//     section parses into a `celwasm::abi::CelAbi` proto.
//   - End-to-end: compile `x` / `c.billing_address.city` through the
//     full pipeline, serialize, re-decode, verify the proto matches
//     the compiler's StaticLayout + LoweredFunction.
//   - Error paths: malformed magic / version, missing section,
//     truncated / invalid payload, truncated LEB128.

#include "eval/internal/abi_decode.h"

#include <cstdint>
#include <string>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "abi/wasm_binary.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/internal/compile.h"
#include "compiler/ir/annotations.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Force generated-pool registration of descriptors referenced by
// tests below.  Runs once at static init per test binary.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::Address>();
      return 0;
    }();

// Build the raw wasm byte stream for a single custom section on a
// core-module preamble, via the shared //abi:wasm_binary framer.
// The decoder parses wasm bytes; driving real codegen from every
// test would slow the suite and couple decoder correctness to
// codegen bugs, so we hand-build the byte streams here.
std::vector<uint8_t> MakeWasmWithCustomSection(absl::string_view name,
                                               absl::Span<const uint8_t> body) {
  std::vector<uint8_t> out = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  const std::vector<uint8_t> section = BuildCustomSection(name, body);
  out.insert(out.end(), section.begin(), section.end());
  return out;
}

std::vector<uint8_t> SerializeAbi(const celwasm::abi::CelAbi& abi) {
  std::string bytes;
  abi.SerializeToString(&bytes);
  return {bytes.begin(), bytes.end()};
}

// --- Happy path (hand-built byte stream) ---------------------------

TEST(AbiDecodeTest, DecodesSingleVariableEntry) {
  celwasm::abi::CelAbi abi;
  abi.set_version(1);
  auto* v = abi.add_variables();
  v->set_name("x");
  v->set_local_index(0);
  v->set_slot_offset(16);
  v->set_repr(static_cast<uint32_t>(Repr::kInt));

  auto decoded = DecodeCelAbiFromWasm(
      MakeWasmWithCustomSection("cel.abi", SerializeAbi(abi)));
  ASSERT_THAT(decoded, IsOk());
  EXPECT_EQ(decoded->version(), 1u);
  ASSERT_EQ(decoded->variables_size(), 1);
  EXPECT_EQ(decoded->variables(0).name(), "x");
  EXPECT_EQ(decoded->variables(0).local_index(), 0u);
  EXPECT_EQ(decoded->variables(0).slot_offset(), 16u);
  EXPECT_EQ(DecodeRepr(decoded->variables(0).repr()), Repr::kInt);
}

TEST(AbiDecodeTest, DecodesMultipleVariablesInOrder) {
  celwasm::abi::CelAbi abi;
  abi.set_version(1);
  for (uint32_t i = 0; i < 4; ++i) {
    auto* v = abi.add_variables();
    v->set_name(absl::StrCat("v", i));
    v->set_local_index(i);
    v->set_slot_offset(16 + (i * 24));
    v->set_repr(static_cast<uint32_t>(Repr::kInt));
  }
  auto decoded = DecodeCelAbiFromWasm(
      MakeWasmWithCustomSection("cel.abi", SerializeAbi(abi)));
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->variables_size(), 4);
  for (uint32_t i = 0; i < 4; ++i) {
    EXPECT_EQ(decoded->variables(i).name(), absl::StrCat("v", i));
    EXPECT_EQ(decoded->variables(i).local_index(), i);
    EXPECT_EQ(decoded->variables(i).slot_offset(), 16 + (i * 24));
  }
}

TEST(AbiDecodeTest, DecodesEveryScalarRepr) {
  struct Case {
    absl::string_view name = "";
    Repr repr = Repr::kUnknown;
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
    v->set_slot_offset(16 + (idx * 24));
    v->set_repr(static_cast<uint32_t>(c.repr));
    ++idx;
  }
  auto decoded = DecodeCelAbiFromWasm(
      MakeWasmWithCustomSection("cel.abi", SerializeAbi(abi)));
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->variables_size(), static_cast<int>(std::size(cases)));
  for (size_t i = 0; i < std::size(cases); ++i) {
    EXPECT_EQ(DecodeRepr(decoded->variables(i).repr()), cases[i].repr)
        << "case " << i << " name=" << cases[i].name;
  }
}

// --- required_functions (field 8) ----------------------------------

TEST(AbiDecodeTest, DecodesRequiredFunctions) {
  celwasm::abi::CelAbi abi;
  abi.set_version(1);
  auto* host_fn = abi.add_required_functions();
  host_fn->set_overload_id("discount_pct_string");
  host_fn->set_fn_name("discount_pct");
  host_fn->set_backend(celwasm::abi::RequiredFunction::HOST);
  host_fn->add_param_types()->set_kind(celwasm::abi::Type::KIND_STRING);
  host_fn->mutable_return_type()->set_kind(celwasm::abi::Type::KIND_INT);
  auto* plugin_fn = abi.add_required_functions();
  plugin_fn->set_overload_id("is_adult_message_acme_User");
  plugin_fn->set_fn_name("is_adult");
  plugin_fn->set_backend(celwasm::abi::RequiredFunction::PLUGIN);
  auto* param = plugin_fn->add_param_types();
  param->set_kind(celwasm::abi::Type::KIND_LIST);
  auto* elem = param->add_params();
  elem->set_kind(celwasm::abi::Type::KIND_PROTO);
  elem->set_proto_fqn("acme.User");
  plugin_fn->mutable_return_type()->set_kind(celwasm::abi::Type::KIND_BOOL);
  plugin_fn->set_is_receiver(true);

  auto decoded = DecodeCelAbiFromWasm(
      MakeWasmWithCustomSection("cel.abi", SerializeAbi(abi)));
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->required_functions_size(), 2);
  const auto& h = decoded->required_functions(0);
  EXPECT_EQ(h.overload_id(), "discount_pct_string");
  EXPECT_EQ(h.fn_name(), "discount_pct");
  EXPECT_EQ(h.backend(), celwasm::abi::RequiredFunction::HOST);
  ASSERT_EQ(h.param_types_size(), 1);
  EXPECT_EQ(h.param_types(0).kind(), celwasm::abi::Type::KIND_STRING);
  EXPECT_EQ(h.return_type().kind(), celwasm::abi::Type::KIND_INT);
  EXPECT_FALSE(h.is_receiver());
  const auto& p = decoded->required_functions(1);
  EXPECT_EQ(p.overload_id(), "is_adult_message_acme_User");
  EXPECT_EQ(p.backend(), celwasm::abi::RequiredFunction::PLUGIN);
  ASSERT_EQ(p.param_types_size(), 1);
  ASSERT_EQ(p.param_types(0).params_size(), 1);
  EXPECT_EQ(p.param_types(0).params(0).proto_fqn(), "acme.User");
  EXPECT_TRUE(p.is_receiver());
}

// Open-set wire contract: unknown Type kinds and unknown Backend
// values decode without rejection and survive numerically — a
// program emitted by a future compiler must not fail decode on an
// older engine (the signature compare treats them numerically).
TEST(AbiDecodeTest, DecodesUnknownFnKindAndBackendWithoutRejection) {
  celwasm::abi::CelAbi abi;
  abi.set_version(1);
  auto* fn = abi.add_required_functions();
  fn->set_overload_id("future_fn");
  fn->set_backend(static_cast<celwasm::abi::RequiredFunction::Backend>(7));
  fn->mutable_return_type()->set_kind(
      static_cast<celwasm::abi::Type::Kind>(99));

  auto decoded = DecodeCelAbiFromWasm(
      MakeWasmWithCustomSection("cel.abi", SerializeAbi(abi)));
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->required_functions_size(), 1);
  EXPECT_EQ(static_cast<int>(decoded->required_functions(0).backend()), 7);
  EXPECT_EQ(
      static_cast<int>(decoded->required_functions(0).return_type().kind()),
      99);
}

// --- End-to-end (real compiler output) -----------------------------

TEST(AbiDecodeTest, RoundTripsCompilerOutput) {
  CompileOptions opts;
  opts.check.variable_specs = {"x:int"};
  auto artifact = Compile("x", opts);
  ASSERT_THAT(artifact, IsOk());
  auto decoded = DecodeCelAbiFromWasm(artifact->wasm_bytes);
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->variables_size(), 1);
  EXPECT_EQ(decoded->variables(0).name(), "x");
  EXPECT_EQ(decoded->variables(0).local_index(), 0u);
  EXPECT_EQ(decoded->variables(0).slot_offset(),
            artifact->layout.variables[0].slot_offset);
  EXPECT_EQ(DecodeRepr(decoded->variables(0).repr()), Repr::kInt);
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
    ASSERT_EQ(decoded->variables_size(), 1) << spec;
    EXPECT_EQ(decoded->variables(0).name(), "x") << spec;
    EXPECT_EQ(decoded->variables(0).slot_offset(),
              artifact->layout.variables[0].slot_offset)
        << spec;
  }
}

TEST(AbiDecodeTest, RoundTripsWithLiteralOnlyProgramEmitsSentinelOnly) {
  // `42` — no variables.  LoweredFunction always emits the sentinel
  // field-ref row so the trampoline's bounds check stays uniform.
  auto artifact = Compile("42", {});
  ASSERT_THAT(artifact, IsOk());
  auto decoded = DecodeCelAbiFromWasm(artifact->wasm_bytes);
  ASSERT_THAT(decoded, IsOk());
  EXPECT_EQ(decoded->version(), 1u);
  EXPECT_EQ(decoded->variables_size(), 0);
  ASSERT_EQ(decoded->fields_size(), 1);
  EXPECT_EQ(decoded->fields(0).field_number(), 0u);
  EXPECT_EQ(decoded->fields(0).name(), "");
}

TEST(AbiDecodeTest, RoundTripsFieldRefsFromCompiler) {
  CompileOptions opts;
  opts.check.variable_specs = {"c:celwasm.testdata.Customer"};
  auto artifact = Compile("c.billing_address.city", opts);
  ASSERT_THAT(artifact, IsOk());
  auto decoded = DecodeCelAbiFromWasm(artifact->wasm_bytes);
  ASSERT_THAT(decoded, IsOk());
  // Sentinel + billing_address + city = 3 rows (post-order walk).
  ASSERT_EQ(decoded->fields_size(), 3);
  EXPECT_EQ(decoded->fields(0).field_number(), 0u);
  EXPECT_EQ(decoded->fields(1).name(), "billing_address");
  EXPECT_EQ(decoded->fields(1).field_number(), 9u);
  EXPECT_EQ(decoded->fields(1).owner_fqn(), "celwasm.testdata.Customer");
  EXPECT_EQ(decoded->fields(2).name(), "city");
  EXPECT_EQ(decoded->fields(2).field_number(), 1u);
  EXPECT_EQ(decoded->fields(2).owner_fqn(), "celwasm.testdata.Address");
}

TEST(AbiDecodeTest, RoundTripsRequiredFunctionsFromCompiler) {
  // Emit → decode equality for the required-functions table: compile
  // a program calling one of two declared plugin fns at O2 (the
  // unused import is dropped), decode the emitted bytes, and assert
  // the surviving row's full shape.
  CelfnType bool_t;
  bool_t.kind = CelfnType::Kind::kBool;
  CelfnType string_t;
  string_t.kind = CelfnType::Kind::kString;
  CompileOptions opts;
  opts.optimize_level = 2;
  opts.check.variable_specs = {"u:string"};
  opts.function_libraries = {
      *FunctionLibrary::Builder()
           .AddPlugin("allow", bool_t,
                      {CelfnParam{/*is_receiver=*/false, string_t, "u"}})
           .AddPlugin("deny", bool_t,
                      {CelfnParam{/*is_receiver=*/false, string_t, "u"}})
           .Build()};
  opts.check.function_libraries = opts.function_libraries;
  auto artifact = Compile("allow(u)", opts);
  ASSERT_THAT(artifact, IsOk()) << artifact.status();
  auto decoded = DecodeCelAbiFromWasm(artifact->wasm_bytes);
  ASSERT_THAT(decoded, IsOk());
  ASSERT_EQ(decoded->required_functions_size(), 1);
  const auto& row = decoded->required_functions(0);
  EXPECT_EQ(row.overload_id(), "allow_string");
  EXPECT_EQ(row.fn_name(), "allow");
  EXPECT_EQ(row.backend(), celwasm::abi::RequiredFunction::PLUGIN);
  ASSERT_EQ(row.param_types_size(), 1);
  EXPECT_EQ(row.param_types(0).kind(), celwasm::abi::Type::KIND_STRING);
  EXPECT_EQ(row.return_type().kind(), celwasm::abi::Type::KIND_BOOL);
  EXPECT_FALSE(row.is_receiver());
}

// --- Error paths ---------------------------------------------------

TEST(AbiDecodeErrorTest, FailsOnTooShortStream) {
  EXPECT_THAT(DecodeCelAbiFromWasm(std::vector<uint8_t>{0x00, 0x61, 0x73}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, FailsOnBadMagic) {
  EXPECT_THAT(DecodeCelAbiFromWasm(std::vector<uint8_t>{
                  0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, FailsOnUnsupportedVersion) {
  EXPECT_THAT(DecodeCelAbiFromWasm(std::vector<uint8_t>{
                  0x00, 0x61, 0x73, 0x6d, 0x02, 0x00, 0x00, 0x00}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, ReturnsNotFoundWhenSectionIsMissing) {
  EXPECT_THAT(DecodeCelAbiFromWasm(std::vector<uint8_t>{
                  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(AbiDecodeErrorTest, ReturnsNotFoundWhenOtherCustomSectionsPresent) {
  EXPECT_THAT(DecodeCelAbiFromWasm(MakeWasmWithCustomSection(
                  "name", std::vector<uint8_t>{'n', 'o', 't'})),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(AbiDecodeErrorTest, FailsOnMalformedAbiPayload) {
  EXPECT_THAT(
      DecodeCelAbiFromWasm(MakeWasmWithCustomSection(
          "cel.abi", std::vector<uint8_t>{0xff, 0xff, 0xff, 0xff, 0xff, 0xff})),
      StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, FailsOnTruncatedSection) {
  std::vector<uint8_t> wasm = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  wasm.push_back(0);    // custom-section id
  wasm.push_back(100);  // claims 100 bytes follow
  for (int i = 0; i < 5; ++i) {
    wasm.push_back(0);
  }
  EXPECT_THAT(DecodeCelAbiFromWasm(wasm),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AbiDecodeErrorTest, FailsOnOversizeLeb128) {
  std::vector<uint8_t> wasm = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  wasm.push_back(0);
  for (int i = 0; i < 6; ++i) {
    wasm.push_back(0x80);  // all continuation
  }
  EXPECT_THAT(DecodeCelAbiFromWasm(wasm),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace celwasm
