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
#include "common/standard_definitions.h"
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

// Snapshot of the M5.E builtin seed count.  Used by a few tests to
// compute the first available custom id and to assert the seed
// table didn't unexpectedly grow / shrink.  Update with the seed
// list itself.
// M9.B: 85 → 86 — added `type` seed pointing at `cel_type_of_at_v`.
// M10.A: 86 → 92 — added 6 identity-conversion seeds
// (`<kind>_to_<kind>` for bool/int64/uint64/double/string/bytes),
// all pointing at `cel_copy_slot`.
// M10.B: 92 → 98 — added 6 numeric inter-conversion seeds
// (`uint64_to_int64` / `double_to_int64` / `int64_to_uint64` /
// `double_to_uint64` / `int64_to_double` / `uint64_to_double`).
// M10.C: 98 → 102 — added 4 string-parse seeds
// (`string_to_int64` / `string_to_uint64` / `string_to_double` /
// `string_to_bool`).
// M10.D: 102 → 106 — added 4 number/bool→string seeds
// (`int64_to_string` / `uint64_to_string` / `bool_to_string` /
// `double_to_string`).
// M10.E: 106 → 108 — added 2 bytes/string interconversion seeds
// (`string_to_bytes` / `bytes_to_string`).
// M7B.B: 108 → 122 — added 14 timestamp/duration seeds (6 arithmetic
// helpers + 8 ordering helpers).
// M7B.C: 122 → 136 — added 14 accessor seeds (10 ts UTC + 4 dur
// accessors).
// M7B.D: 136 → 146 — added 10 conversion seeds (4 int<->ts/dur
// pure-wasm + 2 identities via cel_copy_slot + 4 host parse/format
// trampolines).
// M7B.E: 146 → 156 — added 10 with-TZ accessor shim seeds; all
// route through the single `cel_host.cel_timestamp_tz_accessor`
// trampoline with a per-shim `accessor_kind` constant.
constexpr size_t kBuiltinSeedCount = 156;

TEST(OverloadTableTest, BuiltinSeedsArePopulated) {
  // M5.E populated `kBuiltinSeeds` with the cel-cpp standard
  // overload ids the v2 runtime supports.  The empty-table phase
  // is over; this test pins the count and a representative
  // arithmetic + container helper resolution.
  OverloadTableBuilder builder;
  OverloadTable table = std::move(builder).Build();
  EXPECT_EQ(table.size(), kBuiltinSeedCount);

  const OverloadImpl* add_int = table.Lookup(cel::StandardOverloadIds::kAddInt);
  ASSERT_NE(add_int, nullptr);
  EXPECT_EQ(add_int->module, ImportModule::kCelRuntime);
  EXPECT_EQ(add_int->name, "cel_int_add_at_vv");

  // `size_list` names the kDynamic dispatcher (Option B — see
  // `overload_table.cc` design comment near `kBuiltinSeeds`).  The
  // dispatcher itself ships in M5.D step 2; this assertion locks
  // that codegen will NOT emit the arena-only fast path through
  // the table.
  const OverloadImpl* size_list =
      table.Lookup(cel::StandardOverloadIds::kSizeList);
  ASSERT_NE(size_list, nullptr);
  EXPECT_EQ(size_list->name, "cel_list_size");
}

TEST(OverloadTableTest, RegisterCustomAppendsPastBuiltinSeeds) {
  // Mirrors an embedder declaring `my.upper(string) -> string` via
  //   MakeFunctionDecl("my.upper",
  //     MakeOverloadDecl("my_upper_string", StringType(), StringType()))
  // and wiring its compile-time implementation through cel_host.
  // Customs intern at `kBuiltinSeedCount + 1` (1-based, after the
  // builtins).
  OverloadTableBuilder builder;
  EXPECT_THAT(builder.RegisterCustom("my_upper_string", ImportModule::kCelHost,
                                     "my_upper_string"),
              IsOk());
  OverloadTable table = std::move(builder).Build();

  EXPECT_EQ(table.size(), kBuiltinSeedCount + 1u);
  const OverloadImpl* impl = table.Lookup("my_upper_string");
  ASSERT_NE(impl, nullptr);
  EXPECT_EQ(impl->module, ImportModule::kCelHost);
  EXPECT_EQ(impl->name, "my_upper_string");

  const uint32_t id = table.InternOverloadId("my_upper_string");
  EXPECT_EQ(id, kBuiltinSeedCount + 1u);
  EXPECT_EQ(&table.LookupById(id), impl);
}

TEST(OverloadTableTest, RegisterCustomCollidingWithBuiltinIsAlreadyExists) {
  // M5.E: customs cannot shadow built-ins.  CEL spec forbids
  // overriding standard overloads; OverloadTable enforces this at
  // the builder layer with the failing id in the error message.
  OverloadTableBuilder builder;
  EXPECT_THAT(builder.RegisterCustom("add_int64", ImportModule::kCelHost,
                                     "my_add_int_override"),
              StatusIs(absl::StatusCode::kAlreadyExists,
                       testing::HasSubstr("add_int64")));
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
  // Customs are appended after the built-in seeds and intern
  // contiguously in registration order.  Use distinct overload ids
  // (not in `kBuiltinSeeds`) so the `RegisterCustom` calls don't
  // collide with the standard names.
  OverloadTableBuilder builder;
  ASSERT_THAT(builder.RegisterCustom("my_log_int", ImportModule::kCelHost,
                                     "my_log_int"),
              IsOk());
  ASSERT_THAT(builder.RegisterCustom("my_log_double", ImportModule::kCelHost,
                                     "my_log_double"),
              IsOk());
  ASSERT_THAT(
      builder.RegisterCustom("my_reverse_string", ImportModule::kCelHost,
                             "my_reverse_string"),
      IsOk());
  OverloadTable table = std::move(builder).Build();
  const uint32_t base = kBuiltinSeedCount;
  EXPECT_EQ(table.InternOverloadId("my_log_int"), base + 1u);
  EXPECT_EQ(table.InternOverloadId("my_log_double"), base + 2u);
  EXPECT_EQ(table.InternOverloadId("my_reverse_string"), base + 3u);
  EXPECT_EQ(table.LookupById(base + 1u).name, "my_log_int");
  EXPECT_EQ(table.LookupById(base + 2u).name, "my_log_double");
  EXPECT_EQ(table.LookupById(base + 3u).name, "my_reverse_string");
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
  EXPECT_EQ(b.InternOverloadId("my_upper_string"), kBuiltinSeedCount + 1u);
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
  // M10.B note: id `99` used to be a safe "unknown" placeholder when
  // kBuiltinSeeds was at 92 (one custom registered → id 93, leaving
  // 99 unused).  M10.B grew kBuiltinSeeds to 98, so 99 now names the
  // first custom and the test would fail.  Use a far-out value
  // (`kBuiltinSeedCount + 1000`) to stay unknown as more seeds land.
  const absl::flat_hash_set<uint32_t> used = {
      0u, static_cast<uint32_t>(kBuiltinSeedCount + 1000)};
  const auto imports = table.UsedImports(used);
  EXPECT_TRUE(imports.empty());
}

// ── M5.E coverage tripwire ────────────────────────────────────
// Asserts every cel-cpp `StandardOverloadIds::k*` constant is
// classified by the v2 OverloadTable as either:
//   - resolvable (a row in `kBuiltinSeeds` returns non-null
//     from `OverloadTable::Lookup`), or
//   - explicitly unimplemented (`OverloadTableIsExplicitlyUnimplemented`
//     returns true).
//
// Failing this test means cel-cpp added a new overload id that the
// v2 OverloadTable doesn't know about — every new id MUST land in
// either `kBuiltinSeeds` (with a runtime helper) or
// `kExplicitlyUnimplementedIds` (with a comment naming the milestone
// it'll land in or why it's deferred).  This is the forcing
// function `m5-kcall-comprehensions.md §2.2 / design.md §4.5`
// describes — silent additions to cel-cpp's standard library no
// longer go unnoticed.

// Returns every `static constexpr absl::string_view k*` member of
// `cel::StandardOverloadIds` in source-file order — see
// `third_party/cel-cpp/common/standard_definitions.h`.  Pulled out
// of the tripwire test body so the test stays under the lint
// function-size threshold.  When cel-cpp adds a new id, append it
// here and classify it in `overload_table.cc`.
//
// `EveryStandardOverloadId` *is* the enumeration; splitting it into
// chunked helpers would obscure the "every standard id, in source-
// file order" invariant that is the whole point of the tripwire.
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
  OverloadTable table = OverloadTableBuilder().Build();
  for (absl::string_view id : EveryStandardOverloadId()) {
    const bool resolvable = table.Lookup(id) != nullptr;
    const bool unimplemented = OverloadTableIsExplicitlyUnimplemented(id);
    EXPECT_TRUE(resolvable || unimplemented)
        << "cel-cpp StandardOverloadIds id `" << id << "` is unclassified — "
        << "add it to `kBuiltinSeeds` (with a runtime helper) or to "
        << "`kExplicitlyUnimplementedIds` (with a comment naming the deferral "
        << "milestone) in compiler_v2/codegen/overload_table.cc.";
    EXPECT_FALSE(resolvable && unimplemented)
        << "id `" << id << "` is BOTH resolvable AND in "
        << "kExplicitlyUnimplementedIds; remove from one of the two sets.";
  }
}

}  // namespace
}  // namespace celwasm
