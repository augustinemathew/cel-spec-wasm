#include "abi/program_facts.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/match.h"
#include "compiler/compiler.h"
#include "compiler/ir/annotations.h"
#include "compiler/program.h"
#include "tools/cel/program_report.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm::abi {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Compile `source` with the given declarations and hand back the
// program bytes, so each case describes a real artifact rather than a
// hand-built section.
std::vector<uint8_t> CompileBytes(
    const std::vector<std::pair<std::string, CelType>>& vars,
    absl::string_view source,
    CompilerOptions::LinkMode mode = CompilerOptions::LinkMode::kStatic) {
  auto b = Compiler::NewBuilder();
  for (const auto& [name, type] : vars)
    b.DeclareVariable(name, type);
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  CompilerOptions opts;
  opts.link_mode = mode;
  auto program = compiler->Compile(source, opts);
  ABSL_CHECK_OK(program) << source;
  const auto bytes = program->wasm_bytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

TEST(DescribeProgramTest, ReportsDeclaredScalarVars) {
  const auto bytes = CompileBytes(
      {{"a", CelType::Int()}, {"s", CelType::String()}}, "a > 0 && s != ''");
  auto facts = DescribeProgram(bytes);
  ASSERT_THAT(facts, IsOk());
  EXPECT_TRUE(facts->has_abi_section);
  ASSERT_EQ(facts->vars.size(), 2u);
  // Declaration order follows cel.abi's local_index, not the source.
  std::vector<std::string> rendered;
  for (const auto& v : facts->vars) {
    rendered.push_back(v.name + ":" + v.type_name);
  }
  EXPECT_THAT(rendered, ::testing::UnorderedElementsAre("a:int", "s:string"));
}

TEST(DescribeProgramTest, ReportsNoVarsForAConstantExpression) {
  auto facts = DescribeProgram(CompileBytes({}, "1 + 2"));
  ASSERT_THAT(facts, IsOk());
  EXPECT_TRUE(facts->has_abi_section);
  EXPECT_TRUE(facts->vars.empty());
}

TEST(DescribeProgramTest, ReportsLinkModeAndAbiVersions) {
  auto stat = DescribeProgram(CompileBytes({}, "1"));
  ASSERT_THAT(stat, IsOk());
  EXPECT_TRUE(stat->static_linked);
  EXPECT_GT(stat->runtime_abi_version, 0u);

  auto dyn = DescribeProgram(
      CompileBytes({}, "1", CompilerOptions::LinkMode::kDynamic));
  ASSERT_THAT(dyn, IsOk());
  EXPECT_FALSE(dyn->static_linked);
}

// An aggregate variable is reported by its bare repr — the wire has no
// element/key/value types to print.
TEST(DescribeProgramTest, AggregateVarReportsBareRepr) {
  const auto bytes =
      CompileBytes({{"xs", CelType::List(CelType::Int())}}, "size(xs) > 0");
  auto facts = DescribeProgram(bytes);
  ASSERT_THAT(facts, IsOk());
  ASSERT_EQ(facts->vars.size(), 1u);
  EXPECT_EQ(facts->vars[0].name, "xs");
  EXPECT_EQ(facts->vars[0].repr, Repr::kList);
  EXPECT_EQ(facts->vars[0].type_name, "list")
      << "cel.abi carries no element type; it must not be invented";
}

TEST(DescribeProgramTest, ModuleWithoutAbiSectionIsDescribableNotAnError) {
  // A bare wasm preamble: valid module, no sections at all.
  const std::vector<uint8_t> bare = {0x00, 0x61, 0x73, 0x6d,
                                     0x01, 0x00, 0x00, 0x00};
  auto facts = DescribeProgram(bare);
  ASSERT_THAT(facts, IsOk());
  EXPECT_FALSE(facts->has_abi_section);
  EXPECT_TRUE(facts->vars.empty());
  EXPECT_TRUE(absl::StrContains(::celwasm::tools::cel::FormatProgramFacts(*facts), "no cel.abi"));
}

TEST(DescribeProgramTest, RejectsBytesThatAreNotWasm) {
  const std::vector<uint8_t> junk = {'n', 'o', 't', 'w', 'a', 's', 'm'};
  EXPECT_THAT(DescribeProgram(junk),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ---------- Required functions (cel.abi field 8) ----------------------------

// A program that calls an @host custom fn records it in
// `required_functions`, which is what tells an operator the artifact
// is not runnable by the stock CLI.
TEST(DescribeProgramTest, ReportsRequiredHostFunction) {
  auto b = Compiler::NewBuilder();
  b.DeclareVariable("s", CelType::String());
  b.AddFunction("string @host.upper(this string s);");
  auto compiler = std::move(b).Build();
  ASSERT_THAT(compiler, IsOk());
  auto program = compiler->Compile("s.upper()");
  ASSERT_THAT(program, IsOk());
  const auto bytes = program->wasm_bytes();

  auto facts = DescribeProgram(std::vector<uint8_t>(bytes.begin(), bytes.end()));
  ASSERT_THAT(facts, IsOk());
  ASSERT_EQ(facts->required_fns.size(), 1u);
  EXPECT_EQ(facts->required_fns[0].name, "upper");
  EXPECT_TRUE(facts->required_fns[0].is_host);
  EXPECT_FALSE(facts->required_fns[0].signature.empty());

  const std::string out = ::celwasm::tools::cel::FormatProgramFacts(*facts);
  EXPECT_TRUE(absl::StrContains(out, "upper")) << out;
  EXPECT_TRUE(absl::StrContains(out, "not runnable by `cel run`")) << out;
}

TEST(DescribeProgramTest, ReportsNoRequiredFunctionsForAPlainExpression) {
  auto facts = DescribeProgram(CompileBytes({}, "1 + 2"));
  ASSERT_THAT(facts, IsOk());
  EXPECT_TRUE(facts->required_fns.empty());
  const std::string out = ::celwasm::tools::cel::FormatProgramFacts(*facts);
  EXPECT_TRUE(absl::StrContains(out, "host fns:")) << out;
  EXPECT_TRUE(absl::StrContains(out, "none")) << out;
  EXPECT_FALSE(absl::StrContains(out, "not runnable")) << out;
}

// ---------- Rendering -------------------------------------------------------

TEST(FormatProgramFactsTest, RendersVarsAndLinkLine) {
  auto facts = DescribeProgram(CompileBytes({{"a", CelType::Int()}}, "a + 1"));
  ASSERT_THAT(facts, IsOk());
  const std::string out = ::celwasm::tools::cel::FormatProgramFacts(*facts);
  EXPECT_TRUE(absl::StrContains(out, "a:int")) << out;
  EXPECT_TRUE(absl::StrContains(out, "link:")) << out;
  EXPECT_TRUE(absl::StrContains(out, "static")) << out;
}

TEST(FormatProgramFactsTest, RendersNoneWhenNoVarsAreDeclared) {
  auto facts = DescribeProgram(CompileBytes({}, "1 + 2"));
  ASSERT_THAT(facts, IsOk());
  EXPECT_TRUE(absl::StrContains(::celwasm::tools::cel::FormatProgramFacts(*facts), "vars:"));
  EXPECT_TRUE(absl::StrContains(::celwasm::tools::cel::FormatProgramFacts(*facts), "none"));
}

// ---------- Repr → type spec ------------------------------------------------

TEST(ScalarTypeSpecForReprTest, MapsEveryScalarRepr) {
  const std::vector<std::pair<Repr, std::string>> cases = {
      {Repr::kBool, "bool"},         {Repr::kInt, "int"},
      {Repr::kUint, "uint"},         {Repr::kDouble, "double"},
      {Repr::kString, "string"},     {Repr::kBytes, "bytes"},
      {Repr::kDuration, "duration"}, {Repr::kTimestamp, "timestamp"},
  };
  for (const auto& [repr, want] : cases) {
    auto spec = ScalarTypeSpecForRepr(repr, "v");
    ASSERT_THAT(spec, IsOk()) << want;
    EXPECT_EQ(*spec, want);
  }
}

// The reprs whose full type is not on the wire must refuse, and the
// message must name the escape hatch — this is the only guidance a
// user gets when `--var m={...}` fails.
TEST(ScalarTypeSpecForReprTest, RejectsReprsWithNoCompleteWireType) {
  for (Repr repr : {Repr::kList, Repr::kMap, Repr::kMessage, Repr::kEnum,
                    Repr::kType, Repr::kUnknown}) {
    auto spec = ScalarTypeSpecForRepr(repr, "m");
    EXPECT_THAT(spec, StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_TRUE(absl::StrContains(spec.status().message(), "--var m:<Type>="))
        << "repr=" << static_cast<int>(repr)
        << " message=" << spec.status().message();
  }
}

}  // namespace
}  // namespace celwasm::abi
