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
  EXPECT_EQ(table.Lookup("anything"), nullptr);
  EXPECT_EQ(table.InternOverloadId("anything"), 0u);
}

TEST(OverloadTableTest, RegisterCustomAppendsAndInternsFromOne) {
  OverloadTableBuilder builder;
  EXPECT_THAT(builder.RegisterCustom("my_func", ImportModule::kCelHost,
                                     "cel_host_call_custom", /*pattern_id=*/7),
              IsOk());
  OverloadTable table = std::move(builder).Build();

  EXPECT_EQ(table.size(), 1u);
  const OverloadImpl* impl = table.Lookup("my_func");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->module, ImportModule::kCelHost);
  EXPECT_EQ(impl->name, "cel_host_call_custom");
  EXPECT_EQ(impl->pattern_id, 7u);

  const uint32_t id = table.InternOverloadId("my_func");
  EXPECT_EQ(id, 1u);
  EXPECT_EQ(&table.LookupById(id), impl);
}

TEST(OverloadTableTest, DuplicateCustomRegistrationIsAlreadyExists) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("my_func", ImportModule::kCelHost,
                                     "cel_host_call_custom", 1),
              IsOk());
  EXPECT_THAT(builder.RegisterCustom("my_func", ImportModule::kCelHost,
                                     "cel_host_call_other", 2),
              StatusIs(absl::StatusCode::kAlreadyExists,
                       testing::HasSubstr("my_func")));
}

TEST(OverloadTableTest, ZeroPatternIdIsRejected) {
  OverloadTableBuilder builder;
  EXPECT_THAT(builder.RegisterCustom("my_func", ImportModule::kCelHost,
                                     "cel_host_call_custom", /*pattern_id=*/0),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("pattern_id")));
}

TEST(OverloadTableTest, InternUnknownReturnsZero) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("known", ImportModule::kCelHost, "k", 1),
              IsOk());
  OverloadTable table = std::move(builder).Build();
  EXPECT_EQ(table.InternOverloadId("unknown"), 0u);
  EXPECT_EQ(table.Lookup("unknown"), nullptr);
}

TEST(OverloadTableTest, InternsAssignedInRegistrationOrder) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("first", ImportModule::kCelHost, "a", 1),
              IsOk());
  ASSERT_THAT(builder.RegisterCustom("second", ImportModule::kCelHost, "b", 2),
              IsOk());
  ASSERT_THAT(builder.RegisterCustom("third", ImportModule::kCelHost, "c", 3),
              IsOk());
  OverloadTable table = std::move(builder).Build();
  EXPECT_EQ(table.InternOverloadId("first"), 1u);
  EXPECT_EQ(table.InternOverloadId("second"), 2u);
  EXPECT_EQ(table.InternOverloadId("third"), 3u);
  EXPECT_EQ(table.LookupById(1).pattern_id, 1u);
  EXPECT_EQ(table.LookupById(2).pattern_id, 2u);
  EXPECT_EQ(table.LookupById(3).pattern_id, 3u);
}

TEST(OverloadTableTest, RegisterCustomCopiesIdAndHelperName) {
  // The string_view inputs must be safe to dangle after the call.  We
  // pass in heap-constructed strings and then destroy them before any
  // lookup on the frozen table.
  OverloadTableBuilder builder;
  {
    std::string id = "ephemeral_id";
    std::string name = "ephemeral_name";
    ASSERT_THAT(builder.RegisterCustom(id, ImportModule::kCelHost, name, 42),
                IsOk());
  }
  OverloadTable table = std::move(builder).Build();
  const OverloadImpl* impl = table.Lookup("ephemeral_id");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->name, "ephemeral_name");
}

TEST(OverloadTableTest, LookupSurvivesOuterTableMove) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("abc", ImportModule::kCelHost, "h", 5),
              IsOk());
  OverloadTable a = std::move(builder).Build();
  OverloadTable b = std::move(a);
  const OverloadImpl* impl = b.Lookup("abc");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->name, "h");
  EXPECT_EQ(impl->pattern_id, 5u);
  EXPECT_EQ(b.InternOverloadId("abc"), 1u);
}

TEST(OverloadTableTest, UsedImportsFiltersToRequestedIds) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("x", ImportModule::kCelHost, "hx", 1),
              IsOk());
  ASSERT_THAT(builder.RegisterCustom("y", ImportModule::kCelHost, "hy", 2),
              IsOk());
  ASSERT_THAT(builder.RegisterCustom("z", ImportModule::kCelHost, "hz", 3),
              IsOk());
  OverloadTable table = std::move(builder).Build();
  const absl::flat_hash_set<uint32_t> used = {1u, 3u};
  std::vector<std::pair<ImportModule, absl::string_view>> imports =
      table.UsedImports(used);
  ASSERT_EQ(imports.size(), 2u);
  // Hash-set iteration order is unspecified; sort results before
  // comparing.
  std::sort(imports.begin(), imports.end(), [](const auto& a, const auto& b) {
    return a.second < b.second;
  });
  EXPECT_EQ(imports[0].first, ImportModule::kCelHost);
  EXPECT_EQ(imports[0].second, "hx");
  EXPECT_EQ(imports[1].first, ImportModule::kCelHost);
  EXPECT_EQ(imports[1].second, "hz");
}

TEST(OverloadTableTest, UsedImportsSilentlySkipsUnknownIds) {
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("x", ImportModule::kCelHost, "hx", 1),
              IsOk());
  OverloadTable table = std::move(builder).Build();
  const absl::flat_hash_set<uint32_t> used = {0u, 99u};
  const auto imports = table.UsedImports(used);
  EXPECT_TRUE(imports.empty());
}

}  // namespace
}  // namespace celwasm
