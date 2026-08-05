#include "compiler/codegen/overload_table.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "common/standard_definitions.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// Overload ids in this file mirror cel-cpp's `MakeOverloadDecl`
// convention (see `third_party/cel-cpp/common/standard_definitions.h`):
// typed-suffix per overload ("add_int", "my_upper_string"), scalars
// named by their CEL type.  Custom functions reuse the overload id as
// the wasm import name, so only one name needs tracking.
namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Built-in seed count.  Rises monotonically as kernels land; update it
// alongside `kBuiltinSeeds`'s std::array size in `overload_table.cc`.
constexpr size_t kBuiltinSeedCount = 271;

TEST(OverloadTableTest, BuildSeedsBuiltinsFromCatalogue) {
  auto table = OverloadTable::Build();
  ASSERT_THAT(table, IsOk());
  EXPECT_EQ(table->impls().size(), kBuiltinSeedCount);

  const OverloadDef* add_int = table->Lookup(cel::StandardOverloadIds::kAddInt);
  ASSERT_NE(add_int, nullptr);
  EXPECT_EQ(add_int->wasm_import_module_type, ImportModuleSource::kCel);
  EXPECT_EQ(ImportModuleName(*add_int), "cel");
  EXPECT_EQ(add_int->wasm_import_function_name, "cel_int_add_at_vv");
  // Arity comes from the runtime catalogue (out_slot + N args).
  EXPECT_GE(add_int->num_args, 1);

  // `size_list` names the kDynamic dispatcher (Option B), not the
  // arena-only fast path.
  const OverloadDef* size_list =
      table->Lookup(cel::StandardOverloadIds::kSizeList);
  ASSERT_NE(size_list, nullptr);
  EXPECT_EQ(size_list->wasm_import_function_name, "cel_list_size");
}

TEST(OverloadTableTest, LookupUnknownReturnsNull) {
  auto table = OverloadTable::Build();
  ASSERT_THAT(table, IsOk());
  EXPECT_EQ(table->Lookup("my_unregistered_fn"), nullptr);
}

TEST(OverloadTableTest, CustomsAppendAfterBuiltinsInOrder) {
  const std::vector<OverloadDef> customs = {
      {"my_log_int", "my_log_int", ImportModuleSource::kCelFn, 2},
      {"my_log_double", "my_log_double", ImportModuleSource::kCelFn, 2},
  };
  auto table = OverloadTable::Build(customs);
  ASSERT_THAT(table, IsOk());
  ASSERT_EQ(table->impls().size(), kBuiltinSeedCount + 2u);
  // Customs land after the built-in seeds, in registration order.
  EXPECT_EQ(table->impls()[kBuiltinSeedCount].wasm_import_function_name,
            "my_log_int");
  EXPECT_EQ(table->impls()[kBuiltinSeedCount + 1].wasm_import_function_name,
            "my_log_double");

  const OverloadDef* impl = table->Lookup("my_log_int");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->wasm_import_module_type, ImportModuleSource::kCelFn);
  EXPECT_EQ(ImportModuleName(*impl), "cel_fn");
  EXPECT_EQ(impl->wasm_import_function_name, "my_log_int");
  EXPECT_EQ(impl->num_args, 2);
}

TEST(OverloadTableTest, CustomShadowingBuiltinIsAlreadyExists) {
  // CEL forbids overriding a standard overload.
  const std::vector<OverloadDef> customs = {
      {"add_int64", "my_add_override", ImportModuleSource::kCelFn, 2}};
  EXPECT_THAT(OverloadTable::Build(customs),
              StatusIs(absl::StatusCode::kAlreadyExists,
                       testing::HasSubstr("add_int64")));
}

TEST(OverloadTableTest, DuplicateCustomIsAlreadyExists) {
  const std::vector<OverloadDef> customs = {
      {"my_upper_string", "my_upper_v1", ImportModuleSource::kCelFn, 2},
      {"my_upper_string", "my_upper_v2", ImportModuleSource::kCelFn, 2}};
  EXPECT_THAT(OverloadTable::Build(customs),
              StatusIs(absl::StatusCode::kAlreadyExists,
                       testing::HasSubstr("my_upper_string")));
}

TEST(OverloadTableTest, LookupSurvivesTableMove) {
  const std::vector<OverloadDef> customs = {
      {"my_upper_string", "my_upper_string", ImportModuleSource::kCelFn, 2}};
  auto built = OverloadTable::Build(customs);
  ASSERT_THAT(built, IsOk());
  OverloadTable a = *std::move(built);
  OverloadTable b = std::move(a);
  const OverloadDef* impl = b.Lookup("my_upper_string");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->wasm_import_function_name, "my_upper_string");
}

TEST(OverloadTableTest, CustomInputsNeedNotOutliveBuild) {
  // OverloadDef owns its strings, and the table copies them; the
  // caller's source strings can be destroyed before any lookup.
  auto table = [] {
    std::string id = "my_upper_string";
    std::vector<OverloadDef> customs = {
        {id, id, ImportModuleSource::kCelFn, 2}};
    return OverloadTable::Build(customs);
  }();
  ASSERT_THAT(table, IsOk());
  const OverloadDef* impl = table->Lookup("my_upper_string");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->wasm_import_function_name, "my_upper_string");
}

// ── coverage tripwire ─────────────────────────────────────────
// Asserts every cel-cpp `StandardOverloadIds::k*` constant is classified
// by the OverloadTable as either resolvable (a `kBuiltinSeeds` row, so
// `Lookup` is non-null) or explicitly unimplemented
// (`OverloadTableIsExplicitlyUnimplemented` returns true).  A failure
// means cel-cpp added a new overload id that nothing here classifies —
// it MUST land in `kBuiltinSeeds` (with a runtime helper) or
// `kExplicitlyUnimplementedIds` (with a reason).

// Every `static constexpr absl::string_view k*` member of
// `cel::StandardOverloadIds`, in source-file order — see
// `third_party/cel-cpp/common/standard_definitions.h`.  When cel-cpp
// adds a new id, append it here and classify it in `overload_table.cc`.
//
// `EveryStandardOverloadId` *is* the enumeration; chunking it would
// obscure the "every standard id, in source order" invariant.
std::vector<absl::string_view>
EveryStandardOverloadId() {  // NOLINT(readability-function-size)
  using S = cel::StandardOverloadIds;
  return {
      // Add operator
      S::kAddInt,
      S::kAddUint,
      S::kAddDouble,
      S::kAddDurationDuration,
      S::kAddDurationTimestamp,
      S::kAddTimestampDuration,
      S::kAddString,
      S::kAddBytes,
      S::kAddList,
      // Subtract operator
      S::kSubtractInt,
      S::kSubtractUint,
      S::kSubtractDouble,
      S::kSubtractDurationDuration,
      S::kSubtractTimestampDuration,
      S::kSubtractTimestampTimestamp,
      // Multiply / divide / modulo / negate
      S::kMultiplyInt,
      S::kMultiplyUint,
      S::kMultiplyDouble,
      S::kDivideInt,
      S::kDivideUint,
      S::kDivideDouble,
      S::kModuloInt,
      S::kModuloUint,
      S::kNegateInt,
      S::kNegateDouble,
      // Logical + comprehension
      S::kNot,
      S::kAnd,
      S::kOr,
      S::kConditional,
      S::kNotStrictlyFalse,
      S::kNotStrictlyFalseDeprecated,
      // Equality
      S::kEquals,
      S::kNotEquals,
      // Less
      S::kLessBool,
      S::kLessString,
      S::kLessBytes,
      S::kLessDuration,
      S::kLessTimestamp,
      S::kLessInt,
      S::kLessIntUint,
      S::kLessIntDouble,
      S::kLessDouble,
      S::kLessDoubleInt,
      S::kLessDoubleUint,
      S::kLessUint,
      S::kLessUintInt,
      S::kLessUintDouble,
      // Greater
      S::kGreaterBool,
      S::kGreaterString,
      S::kGreaterBytes,
      S::kGreaterDuration,
      S::kGreaterTimestamp,
      S::kGreaterInt,
      S::kGreaterIntUint,
      S::kGreaterIntDouble,
      S::kGreaterDouble,
      S::kGreaterDoubleInt,
      S::kGreaterDoubleUint,
      S::kGreaterUint,
      S::kGreaterUintInt,
      S::kGreaterUintDouble,
      // GreaterEquals
      S::kGreaterEqualsBool,
      S::kGreaterEqualsString,
      S::kGreaterEqualsBytes,
      S::kGreaterEqualsDuration,
      S::kGreaterEqualsTimestamp,
      S::kGreaterEqualsInt,
      S::kGreaterEqualsIntUint,
      S::kGreaterEqualsIntDouble,
      S::kGreaterEqualsDouble,
      S::kGreaterEqualsDoubleInt,
      S::kGreaterEqualsDoubleUint,
      S::kGreaterEqualsUint,
      S::kGreaterEqualsUintInt,
      S::kGreaterEqualsUintDouble,
      // LessEquals
      S::kLessEqualsBool,
      S::kLessEqualsString,
      S::kLessEqualsBytes,
      S::kLessEqualsDuration,
      S::kLessEqualsTimestamp,
      S::kLessEqualsInt,
      S::kLessEqualsIntUint,
      S::kLessEqualsIntDouble,
      S::kLessEqualsDouble,
      S::kLessEqualsDoubleInt,
      S::kLessEqualsDoubleUint,
      S::kLessEqualsUint,
      S::kLessEqualsUintInt,
      S::kLessEqualsUintDouble,
      // Container operators
      S::kIndexList,
      S::kIndexMap,
      S::kInList,
      S::kInMap,
      S::kSizeBytes,
      S::kSizeList,
      S::kSizeMap,
      S::kSizeString,
      S::kSizeBytesMember,
      S::kSizeListMember,
      S::kSizeMapMember,
      S::kSizeStringMember,
      // String functions
      S::kContainsString,
      S::kEndsWithString,
      S::kStartsWithString,
      // Regex
      S::kMatches,
      S::kMatchesMember,
      // Timestamp / duration accessors
      S::kTimestampToYear,
      S::kTimestampToYearWithTz,
      S::kTimestampToMonth,
      S::kTimestampToMonthWithTz,
      S::kTimestampToDayOfYear,
      S::kTimestampToDayOfYearWithTz,
      S::kTimestampToDayOfMonth,
      S::kTimestampToDayOfMonthWithTz,
      S::kTimestampToDayOfWeek,
      S::kTimestampToDayOfWeekWithTz,
      S::kTimestampToDate,
      S::kTimestampToDateWithTz,
      S::kTimestampToHours,
      S::kTimestampToHoursWithTz,
      S::kDurationToHours,
      S::kTimestampToMinutes,
      S::kTimestampToMinutesWithTz,
      S::kDurationToMinutes,
      S::kTimestampToSeconds,
      S::kTimestampToSecondsWithTz,
      S::kDurationToSeconds,
      S::kTimestampToMilliseconds,
      S::kTimestampToMillisecondsWithTz,
      S::kDurationToMilliseconds,
      // Type conversions
      S::kToDyn,
      S::kUintToUint,
      S::kDoubleToUint,
      S::kIntToUint,
      S::kStringToUint,
      S::kUintToInt,
      S::kDoubleToInt,
      S::kIntToInt,
      S::kStringToInt,
      S::kTimestampToInt,
      S::kDurationToInt,
      S::kDoubleToDouble,
      S::kUintToDouble,
      S::kIntToDouble,
      S::kStringToDouble,
      S::kBoolToBool,
      S::kStringToBool,
      S::kBytesToBytes,
      S::kStringToBytes,
      S::kStringToString,
      S::kBytesToString,
      S::kBoolToString,
      S::kDoubleToString,
      S::kIntToString,
      S::kUintToString,
      S::kDurationToString,
      S::kTimestampToString,
      S::kTimestampToTimestamp,
      S::kIntToTimestamp,
      S::kStringToTimestamp,
      S::kDurationToDuration,
      S::kIntToDuration,
      S::kStringToDuration,
      S::kToType,
  };
}

TEST(OverloadTableTest, CoverageTripwireClassifiesEveryStandardId) {
  auto table = OverloadTable::Build();
  ASSERT_THAT(table, IsOk());
  for (absl::string_view id : EveryStandardOverloadId()) {
    const bool resolvable = table->Lookup(id) != nullptr;
    const bool unimplemented = OverloadTableIsExplicitlyUnimplemented(id);
    EXPECT_TRUE(resolvable || unimplemented)
        << "cel-cpp StandardOverloadIds id `" << id << "` is unclassified — "
        << "add it to `kBuiltinSeeds` (with a runtime helper) or to "
        << "`kExplicitlyUnimplementedIds` (with a reason) in "
        << "compiler/codegen/overload_table.cc.";
    EXPECT_FALSE(resolvable && unimplemented)
        << "id `" << id << "` is BOTH resolvable AND in "
        << "kExplicitlyUnimplementedIds; remove it from one of the two sets.";
  }
}

}  // namespace
}  // namespace celwasm
