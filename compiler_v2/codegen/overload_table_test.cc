#include "compiler_v2/codegen/overload_table.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// Overload ids in this file mirror cel-cpp's `MakeOverloadDecl`
// convention (see `third_party/cel-cpp/checker/type_checker_builder_factory_test.cc`
// and `third_party/cel-cpp/common/standard_definitions.h`):
//   - Typed-suffix form per overload:     "add_int", "my_upper_string".
//   - Scalars named by their CEL type:    int / uint / double / string / bytes.
//   - Custom functions typically reuse    the overload id as the wasm
//     import name so only one name needs  tracking.
// Using realistic names keeps the test readable as a usage example for
// future embedders consulting `RegisterCustom`'s docstring.
namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

TEST(ImportModuleNameTest, MapsEveryEnumerator) {
  EXPECT_EQ(ImportModuleName(ImportModule::kCelRuntime), "cel");
  EXPECT_EQ(ImportModuleName(ImportModule::kCelHost), "cel_host");
}

TEST(OverloadTableTest, EmptyBuiltinSeedsYieldsEmptyTable) {
  OverloadTableBuilder builder;
  OverloadTable table = std::move(builder).Build();
  EXPECT_EQ(table.size(), 0u);
  EXPECT_EQ(table.Lookup("add_int"), nullptr);
  EXPECT_EQ(table.InternOverloadId("add_int"), 0u);
}

TEST(OverloadTableTest, RegisterCustomAppendsAndInternsFromOne) {
  // Mirrors an embedder declaring `my.upper(string) -> string` via
  //   MakeFunctionDecl("my.upper",
  //     MakeOverloadDecl("my_upper_string", StringType(), StringType()))
  // and wiring its compile-time implementation through cel_host.
  OverloadTableBuilder builder;
  EXPECT_THAT(builder.RegisterCustom("my_upper_string", ImportModule::kCelHost,
                                     "my_upper_string"),
              IsOk());
  OverloadTable table = std::move(builder).Build();

  EXPECT_EQ(table.size(), 1u);
  const OverloadImpl* impl = table.Lookup("my_upper_string");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->module, ImportModule::kCelHost);
  EXPECT_EQ(impl->name, "my_upper_string");

  const uint32_t id = table.InternOverloadId("my_upper_string");
  EXPECT_EQ(id, 1u);
  EXPECT_EQ(&table.LookupById(id), impl);
}

TEST(OverloadTableTest, DuplicateCustomRegistrationIsAlreadyExists) {
  // cel-cpp's FunctionRegistry rejects double-registration of the
  // same overload id; OverloadTable enforces the same rule so the
  // failure is caught at compile time with the id in the message.
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("my_upper_string", ImportModule::kCelHost,
                                     "my_upper_v1"),
              IsOk());
  EXPECT_THAT(builder.RegisterCustom("my_upper_string", ImportModule::kCelHost,
                                     "my_upper_v2"),
              StatusIs(absl::StatusCode::kAlreadyExists,
                       testing::HasSubstr("my_upper_string")));
}

TEST(OverloadTableTest, InternUnknownReturnsZero) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("my_upper_string", ImportModule::kCelHost,
                                     "my_upper_string"),
              IsOk());
  OverloadTable table = std::move(builder).Build();
  EXPECT_EQ(table.InternOverloadId("my_unregistered_fn"), 0u);
  EXPECT_EQ(table.Lookup("my_unregistered_fn"), nullptr);
}

TEST(OverloadTableTest, InternsAssignedInRegistrationOrder) {
  // Mirrors an embedder registering overloads of a polymorphic
  // `add` function:
  //   MakeFunctionDecl("add",
  //     MakeOverloadDecl("add_int",    IntType(), IntType(), IntType()),
  //     MakeOverloadDecl("add_double", DoubleType(), DoubleType(), DoubleType()))
  // plus a single custom `my_reverse_string`.
  OverloadTableBuilder builder;
  ASSERT_THAT(
      builder.RegisterCustom("add_int", ImportModule::kCelHost, "add_int"),
      IsOk());
  ASSERT_THAT(builder.RegisterCustom("add_double", ImportModule::kCelHost,
                                     "add_double"),
              IsOk());
  ASSERT_THAT(
      builder.RegisterCustom("my_reverse_string", ImportModule::kCelHost,
                             "my_reverse_string"),
      IsOk());
  OverloadTable table = std::move(builder).Build();
  EXPECT_EQ(table.InternOverloadId("add_int"), 1u);
  EXPECT_EQ(table.InternOverloadId("add_double"), 2u);
  EXPECT_EQ(table.InternOverloadId("my_reverse_string"), 3u);
  EXPECT_EQ(table.LookupById(1).name, "add_int");
  EXPECT_EQ(table.LookupById(2).name, "add_double");
  EXPECT_EQ(table.LookupById(3).name, "my_reverse_string");
}

TEST(OverloadTableTest, RegisterCustomCopiesIdAndHelperName) {
  // The string_view inputs must be safe to dangle after the call.
  // We pass in heap-constructed strings and then destroy them before
  // any lookup on the frozen table.
  OverloadTableBuilder builder;
  {
    std::string id = "my_upper_string";
    std::string name = "my_upper_string";
    ASSERT_THAT(builder.RegisterCustom(id, ImportModule::kCelHost, name),
                IsOk());
  }
  OverloadTable table = std::move(builder).Build();
  const OverloadImpl* impl = table.Lookup("my_upper_string");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->name, "my_upper_string");
}

TEST(OverloadTableTest, LookupSurvivesOuterTableMove) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("my_upper_string", ImportModule::kCelHost,
                                     "my_upper_string"),
              IsOk());
  OverloadTable a = std::move(builder).Build();
  OverloadTable b = std::move(a);
  const OverloadImpl* impl = b.Lookup("my_upper_string");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->name, "my_upper_string");
  EXPECT_EQ(b.InternOverloadId("my_upper_string"), 1u);
}

TEST(OverloadTableTest, UsedImportsFiltersToRequestedIds) {
  // An expression that references `my.upper(x)` and `my.trim(z)` but
  // not `my.lower(y)` should produce two wasm imports, not three —
  // unused customs stay off the module's import list.
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("my_upper_string", ImportModule::kCelHost,
                                     "my_upper_string"),
              IsOk());
  ASSERT_THAT(builder.RegisterCustom("my_lower_string", ImportModule::kCelHost,
                                     "my_lower_string"),
              IsOk());
  ASSERT_THAT(builder.RegisterCustom("my_trim_string", ImportModule::kCelHost,
                                     "my_trim_string"),
              IsOk());
  OverloadTable table = std::move(builder).Build();
  const absl::flat_hash_set<uint32_t> used = {
      table.InternOverloadId("my_upper_string"),
      table.InternOverloadId("my_trim_string"),
  };
  std::vector<std::pair<ImportModule, absl::string_view>> imports =
      table.UsedImports(used);
  ASSERT_EQ(imports.size(), 2u);
  // Hash-set iteration order is unspecified; sort results before
  // comparing.
  std::sort(imports.begin(), imports.end(), [](const auto& a, const auto& b) {
    return a.second < b.second;
  });
  EXPECT_EQ(imports[0].first, ImportModule::kCelHost);
  EXPECT_EQ(imports[0].second, "my_trim_string");
  EXPECT_EQ(imports[1].first, ImportModule::kCelHost);
  EXPECT_EQ(imports[1].second, "my_upper_string");
}

TEST(OverloadTableTest, UsedImportsSilentlySkipsUnknownIds) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("my_upper_string", ImportModule::kCelHost,
                                     "my_upper_string"),
              IsOk());
  OverloadTable table = std::move(builder).Build();
  const absl::flat_hash_set<uint32_t> used = {0u, 99u};
  const auto imports = table.UsedImports(used);
  EXPECT_TRUE(imports.empty());
}

}  // namespace
}  // namespace celwasm
