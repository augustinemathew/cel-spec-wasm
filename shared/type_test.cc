#include "shared/type.h"

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm::api {
namespace {

TEST(CelTypeTest, ScalarFactoriesSetKind) {
  EXPECT_EQ(CelType::Bool().kind(), CelType::Kind::kBool);
  EXPECT_EQ(CelType::Int().kind(), CelType::Kind::kInt);
  EXPECT_EQ(CelType::Uint().kind(), CelType::Kind::kUint);
  EXPECT_EQ(CelType::Double().kind(), CelType::Kind::kDouble);
  EXPECT_EQ(CelType::String().kind(), CelType::Kind::kString);
  EXPECT_EQ(CelType::Bytes().kind(), CelType::Kind::kBytes);
  EXPECT_EQ(CelType::Duration().kind(), CelType::Kind::kDuration);
  EXPECT_EQ(CelType::Timestamp().kind(), CelType::Kind::kTimestamp);
}

TEST(CelTypeTest, DefaultIsUnknown) {
  EXPECT_EQ(CelType().kind(), CelType::Kind::kUnknown);
}

TEST(CelTypeTest, MessageCarriesFullyQualifiedName) {
  auto t = CelType::Message("com.example.Customer");
  EXPECT_EQ(t.kind(), CelType::Kind::kMessage);
  EXPECT_EQ(t.message_fully_qualified_name(), "com.example.Customer");
}

TEST(CelTypeTest, ListCarriesElementType) {
  auto t = CelType::List(CelType::Int());
  EXPECT_EQ(t.kind(), CelType::Kind::kList);
  EXPECT_EQ(t.list_element().kind(), CelType::Kind::kInt);
}

TEST(CelTypeTest, MapCarriesKeyAndValue) {
  auto t = CelType::Map(CelType::String(), CelType::Int());
  EXPECT_EQ(t.kind(), CelType::Kind::kMap);
  EXPECT_EQ(t.map_key().kind(), CelType::Kind::kString);
  EXPECT_EQ(t.map_value().kind(), CelType::Kind::kInt);
}

TEST(CelTypeTest, ScalarEqualityIsKindOnly) {
  EXPECT_EQ(CelType::Int(), CelType::Int());
  EXPECT_NE(CelType::Int(), CelType::Uint());
}

TEST(CelTypeTest, MessageEqualityCheckesName) {
  EXPECT_EQ(CelType::Message("a.B"), CelType::Message("a.B"));
  EXPECT_NE(CelType::Message("a.B"), CelType::Message("a.C"));
}

TEST(CelTypeTest, ListEqualityRecursesIntoElement) {
  EXPECT_EQ(CelType::List(CelType::Int()), CelType::List(CelType::Int()));
  EXPECT_NE(CelType::List(CelType::Int()), CelType::List(CelType::Uint()));
}

TEST(CelTypeTest, MapEqualityRecursesIntoKeyAndValue) {
  EXPECT_EQ(CelType::Map(CelType::String(), CelType::Int()),
            CelType::Map(CelType::String(), CelType::Int()));
  EXPECT_NE(CelType::Map(CelType::String(), CelType::Int()),
            CelType::Map(CelType::Int(), CelType::Int()));
  EXPECT_NE(CelType::Map(CelType::String(), CelType::Int()),
            CelType::Map(CelType::String(), CelType::Uint()));
}

TEST(CelTypeTest, NestedContainersCompose) {
  auto t = CelType::List(CelType::Map(CelType::String(), CelType::Int()));
  EXPECT_EQ(t.kind(), CelType::Kind::kList);
  EXPECT_EQ(t.list_element().kind(), CelType::Kind::kMap);
  EXPECT_EQ(t.list_element().map_key().kind(), CelType::Kind::kString);
  EXPECT_EQ(t.list_element().map_value().kind(), CelType::Kind::kInt);
}

TEST(CelTypeDeathTest, MessageFqnOnNonMessageFires) {
  EXPECT_DEATH(
      { (void)CelType::Int().message_fully_qualified_name(); },
      "message_fully_qualified_name on a int");
}

TEST(CelTypeDeathTest, ListElementOnNonListFires) {
  EXPECT_DEATH(
      { (void)CelType::Int().list_element(); }, "list_element on a int");
}

TEST(CelTypeDeathTest, MapKeyOnNonMapFires) {
  EXPECT_DEATH({ (void)CelType::Int().map_key(); }, "map_key on a int");
}

TEST(CelTypeTest, KindNamesCoverAllKinds) {
  EXPECT_EQ(CelTypeKindName(CelType::Kind::kUnknown), "unknown");
  EXPECT_EQ(CelTypeKindName(CelType::Kind::kBool), "bool");
  EXPECT_EQ(CelTypeKindName(CelType::Kind::kList), "list");
  EXPECT_EQ(CelTypeKindName(CelType::Kind::kMessage), "message");
  EXPECT_EQ(CelTypeKindName(CelType::Kind::kTimestamp), "timestamp");
}

}  // namespace
}  // namespace celwasm::api
