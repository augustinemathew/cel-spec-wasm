#include "e2e/fuzz/targets.h"

#include <optional>
#include <vector>

#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm::fuzz {
namespace {

// Pins the canonical mineable-target list. This is the drift guard:
// the same 13 names appear in `mine_divergences`'s CLI doc, the
// README "Targets:" line, and `scripts/fuzz.sh`'s ALL_TARGETS sweep.
// If a target is added/removed/reordered, update all four AND this
// expectation in the same change.
TEST(Targets, CanonicalListIsPinned) {
  std::vector<absl::string_view> names;
  for (const NamedTarget& t : AllTargets()) {
    names.push_back(t.name);
  }
  EXPECT_EQ(names,
            (std::vector<absl::string_view>{
                "bool", "int", "uint", "double", "string", "bytes", "list_int",
                "list_bool", "list_double", "list_string", "map_string_int",
                "list_list_int", "map_string_list_int"}));
}

TEST(Targets, ParseRoundTripsEveryName) {
  // Compare the optional directly (no deref) so a nullopt fails loudly
  // rather than UB-dereferencing.
  for (const NamedTarget& t : AllTargets()) {
    EXPECT_EQ(ParseTarget(t.name), t.type) << t.name;
  }
}

TEST(Targets, ParseScalarTypesAreCorrect) {
  EXPECT_EQ(ParseTarget("int"), CelType::Int());
  EXPECT_EQ(ParseTarget("uint"), CelType::Uint());
  EXPECT_EQ(ParseTarget("double"), CelType::Double());
  EXPECT_EQ(ParseTarget("bool"), CelType::Bool());
  EXPECT_EQ(ParseTarget("string"), CelType::String());
  EXPECT_EQ(ParseTarget("bytes"), CelType::Bytes());
}

TEST(Targets, ParseNestedTypesAreCorrect) {
  EXPECT_EQ(ParseTarget("list_int"), CelType::List(CelType::Int()));
  EXPECT_EQ(ParseTarget("map_string_int"),
            CelType::Map(CelType::String(), CelType::Int()));
  EXPECT_EQ(ParseTarget("list_list_int"),
            CelType::List(CelType::List(CelType::Int())));
  EXPECT_EQ(ParseTarget("map_string_list_int"),
            CelType::Map(CelType::String(), CelType::List(CelType::Int())));
}

TEST(Targets, ParseUnknownReturnsNullopt) {
  EXPECT_EQ(ParseTarget("nope"), std::nullopt);
  EXPECT_EQ(ParseTarget(""), std::nullopt);
  EXPECT_EQ(ParseTarget("list_uint"), std::nullopt);  // plausible but absent
}

}  // namespace
}  // namespace celwasm::fuzz
