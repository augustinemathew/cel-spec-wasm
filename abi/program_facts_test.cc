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
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "testdata/host_fixture_proto3.pb.h"
#include "tools/cel/program_report.h"

namespace celwasm::abi {
namespace {

// Force generated-pool registration so a message-typed var declaration
// resolves.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::HostMsg3>();
      return 0;
    }();

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
  for (const auto& [name, type] : vars) {
    b.DeclareVariable(name, type);
  }
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  CompilerOptions opts;
  opts.link_mode = mode;
  auto program = compiler->Compile(source, opts);
  ABSL_CHECK_OK(program) << source;
  const auto bytes = program->wasm_bytes();
  return {bytes.begin(), bytes.end()};
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
    rendered.push_back(v.name + ":" + v.type_spec);
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

// The full declared type reaches the wire, so a consumer sees the
// element / key / value types rather than the bare kind.
TEST(DescribeProgramTest, AggregateVarReportsItsFullType) {
  const auto bytes =
      CompileBytes({{"xs", CelType::List(CelType::Int())}}, "size(xs) > 0");
  auto facts = DescribeProgram(bytes);
  ASSERT_THAT(facts, IsOk());
  ASSERT_EQ(facts->vars.size(), 1u);
  EXPECT_EQ(facts->vars[0].name, "xs");
  EXPECT_EQ(facts->vars[0].repr, Repr::kList);
  EXPECT_TRUE(facts->vars[0].has_full_type);
  EXPECT_EQ(facts->vars[0].type_spec, "list<int>");
}

TEST(DescribeProgramTest, MapVarReportsKeyAndValueTypes) {
  const auto bytes = CompileBytes(
      {{"m", CelType::Map(CelType::String(), CelType::Int())}}, "m['k'] > 0");
  auto facts = DescribeProgram(bytes);
  ASSERT_THAT(facts, IsOk());
  ASSERT_EQ(facts->vars.size(), 1u);
  EXPECT_EQ(facts->vars[0].type_spec, "map<string,int>");
}

TEST(DescribeProgramTest, ScalarVarsCarryTheirTypeToo) {
  auto facts = DescribeProgram(CompileBytes(
      {{"a", CelType::Int()}, {"s", CelType::String()}}, "a > 0 && s != ''"));
  ASSERT_THAT(facts, IsOk());
  for (const auto& v : facts->vars) {
    EXPECT_TRUE(v.has_full_type) << v.name;
    EXPECT_EQ(v.type_spec, v.name == "a" ? "int" : "string");
  }
}

// The type renders in the `--var` grammar, not the `.celfn` one, so it
// can be pasted straight back into a binding.  `proto(acme.User)`
// would not parse as a --var type.
TEST(DescribeProgramTest, TypeSpecUsesTheVarGrammarNotCelfn) {
  auto facts = DescribeProgram(CompileBytes(
      {{"xs", CelType::List(CelType::Duration())}}, "size(xs) > 0"));
  ASSERT_THAT(facts, IsOk());
  ASSERT_EQ(facts->vars.size(), 1u);
  EXPECT_EQ(facts->vars[0].type_spec, "list<duration>")
      << "the .celfn grammar would spell this list<Duration>";
}

// Every declarable var type renders in the `--var` grammar.  One row
// per `abi::Type` kind the renderer switches on, so a new kind that
// falls through to the `<kind N>` fallback shows up here rather than
// in a pasted-back binding that fails to parse.
TEST(DescribeProgramTest, RendersEveryDeclarableVarTypeSpec) {
  struct Row {
    CelType type;
    absl::string_view source;
    absl::string_view want;
  };
  const Row rows[] = {
      {CelType::Bool(), "v", "bool"},
      {CelType::Uint(), "v > 0u", "uint"},
      {CelType::Double(), "v > 0.0", "double"},
      {CelType::Bytes(), "size(v) > 0", "bytes"},
      {CelType::Duration(), "v > duration('0s')", "duration"},
      {CelType::Timestamp(), "v > timestamp(0)", "timestamp"},
      {CelType::Message("celwasm.testdata.HostMsg3"), "v.i32 > 0",
       "celwasm.testdata.HostMsg3"},
      {CelType::Map(CelType::String(), CelType::Int()), "size(v) > 0",
       "map<string,int>"},
      {CelType::List(CelType::List(CelType::Bool())), "size(v) > 0",
       "list<list<bool>>"},
  };
  for (const Row& r : rows) {
    auto facts = DescribeProgram(CompileBytes({{"v", r.type}}, r.source));
    ASSERT_THAT(facts, IsOk()) << r.want;
    ASSERT_EQ(facts->vars.size(), 1u) << r.want;
    EXPECT_EQ(facts->vars[0].type_spec, r.want);
  }
}

// Back-compat: an entry with no `type` on the wire keeps the bare
// repr and must not claim a full type — a guessed element type would
// parse a literal wrongly.
TEST(TypeSpecForBindingTest, FallsBackToReprWhenTheWireCarriesNoType) {
  DeclaredVar scalar{"a", Repr::kInt, "int", /*has_full_type=*/false};
  auto spec = TypeSpecForBinding(scalar);
  ASSERT_THAT(spec, IsOk());
  EXPECT_EQ(*spec, "int");

  DeclaredVar aggregate{"xs", Repr::kList, "list", /*has_full_type=*/false};
  auto agg = TypeSpecForBinding(aggregate);
  EXPECT_THAT(agg, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_TRUE(absl::StrContains(agg.status().message(), "--var xs:<Type>="));
}

TEST(TypeSpecForBindingTest, PrefersTheWireTypeWhenPresent) {
  DeclaredVar v{"xs", Repr::kList, "list<int>", /*has_full_type=*/true};
  auto spec = TypeSpecForBinding(v);
  ASSERT_THAT(spec, IsOk());
  EXPECT_EQ(*spec, "list<int>");
}

TEST(DescribeProgramTest, ModuleWithoutAbiSectionIsDescribableNotAnError) {
  // A bare wasm preamble: valid module, no sections at all.
  const std::vector<uint8_t> bare = {0x00, 0x61, 0x73, 0x6d,
                                     0x01, 0x00, 0x00, 0x00};
  auto facts = DescribeProgram(bare);
  ASSERT_THAT(facts, IsOk());
  EXPECT_FALSE(facts->has_abi_section);
  EXPECT_TRUE(facts->vars.empty());
  EXPECT_TRUE(absl::StrContains(
      ::celwasm::tools::cel::FormatProgramFacts(*facts), "no cel.abi"));
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

  auto facts =
      DescribeProgram(std::vector<uint8_t>(bytes.begin(), bytes.end()));
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
  EXPECT_TRUE(absl::StrContains(
      ::celwasm::tools::cel::FormatProgramFacts(*facts), "vars:"));
  EXPECT_TRUE(absl::StrContains(
      ::celwasm::tools::cel::FormatProgramFacts(*facts), "none"));
}

// ---------- Repr → type spec ------------------------------------------------

TEST(TypeSpecForBindingTest, MapsEveryScalarReprWhenNoWireType) {
  const std::vector<std::pair<Repr, std::string>> cases = {
      {Repr::kBool, "bool"},         {Repr::kInt, "int"},
      {Repr::kUint, "uint"},         {Repr::kDouble, "double"},
      {Repr::kString, "string"},     {Repr::kBytes, "bytes"},
      {Repr::kDuration, "duration"}, {Repr::kTimestamp, "timestamp"},
  };
  for (const auto& [repr, want] : cases) {
    auto spec = TypeSpecForBinding(
        DeclaredVar{"v", repr, want, /*has_full_type=*/false});
    ASSERT_THAT(spec, IsOk()) << want;
    EXPECT_EQ(*spec, want);
  }
}

// The reprs whose full type is not on the wire must refuse, and the
// message must name the escape hatch — this is the only guidance a
// user gets when `--var m={...}` fails.
TEST(TypeSpecForBindingTest, RejectsAggregateReprsWithNoWireType) {
  for (Repr repr : {Repr::kList, Repr::kMap, Repr::kMessage, Repr::kEnum,
                    Repr::kType, Repr::kUnknown}) {
    auto spec = TypeSpecForBinding(
        DeclaredVar{"m", repr, "?", /*has_full_type=*/false});
    EXPECT_THAT(spec, StatusIs(absl::StatusCode::kInvalidArgument));
    EXPECT_TRUE(absl::StrContains(spec.status().message(), "--var m:<Type>="))
        << "repr=" << static_cast<int>(repr)
        << " message=" << spec.status().message();
  }
}

}  // namespace
}  // namespace celwasm::abi
