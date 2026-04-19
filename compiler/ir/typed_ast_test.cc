#include "compiler/ir/typed_ast.h"

#include <memory>
#include <utility>
#include <vector>

#include "common/ast.h"
#include "common/ast/metadata.h"
#include "common/expr.h"
#include "compiler/ir/annotations.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// ---- ReprOf coverage: every TypeSpec variant --------------------------------

TEST(ReprOfTest, PrimitivesMapToScalarReprs) {
  struct Row {
    cel::PrimitiveType p;
    Repr expected;
  };
  const Row rows[] = {
      {cel::PrimitiveType::kBool, Repr::kBool},
      {cel::PrimitiveType::kInt64, Repr::kInt},
      {cel::PrimitiveType::kUint64, Repr::kUint},
      {cel::PrimitiveType::kDouble, Repr::kDouble},
      {cel::PrimitiveType::kString, Repr::kString},
      {cel::PrimitiveType::kBytes, Repr::kBytes},
  };
  for (const auto& row : rows) {
    cel::TypeSpec spec{row.p};
    EXPECT_EQ(ReprOf(spec), row.expected)
        << "primitive=" << static_cast<int>(row.p);
  }
}

TEST(ReprOfTest, UnspecifiedPrimitiveIsUnknown) {
  cel::TypeSpec spec{cel::PrimitiveType::kPrimitiveTypeUnspecified};
  EXPECT_EQ(ReprOf(spec), Repr::kUnknown);
}

TEST(ReprOfTest, WrappersReuseUnderlyingPrimitiveRepr) {
  // Wrapper is a nullable-primitive spec — the ABI is still the scalar repr
  // of the wrapped primitive; nullness is tracked elsewhere.
  struct Row {
    cel::PrimitiveType p;
    Repr expected;
  };
  const Row rows[] = {
      {cel::PrimitiveType::kBool, Repr::kBool},
      {cel::PrimitiveType::kInt64, Repr::kInt},
      {cel::PrimitiveType::kUint64, Repr::kUint},
      {cel::PrimitiveType::kDouble, Repr::kDouble},
      {cel::PrimitiveType::kString, Repr::kString},
      {cel::PrimitiveType::kBytes, Repr::kBytes},
  };
  for (const auto& row : rows) {
    cel::TypeSpec spec{cel::PrimitiveTypeWrapper(row.p)};
    EXPECT_EQ(ReprOf(spec), row.expected)
        << "wrapper<" << static_cast<int>(row.p) << ">";
  }
}

TEST(ReprOfTest, WellKnownsMapCorrectly) {
  EXPECT_EQ(ReprOf(cel::TypeSpec{cel::WellKnownTypeSpec::kTimestamp}),
            Repr::kTimestamp);
  EXPECT_EQ(ReprOf(cel::TypeSpec{cel::WellKnownTypeSpec::kDuration}),
            Repr::kDuration);
  // Any travels as an externref (host-owned message); the ABI boundary is
  // the same as for concrete message types.
  EXPECT_EQ(ReprOf(cel::TypeSpec{cel::WellKnownTypeSpec::kAny}),
            Repr::kMessage);
  EXPECT_EQ(
      ReprOf(cel::TypeSpec{cel::WellKnownTypeSpec::kWellKnownTypeUnspecified}),
      Repr::kUnknown);
}

TEST(ReprOfTest, NullTypeSpec) {
  EXPECT_EQ(ReprOf(cel::TypeSpec{cel::NullTypeSpec{}}), Repr::kNull);
}

TEST(ReprOfTest, ListTypeSpec) {
  cel::TypeSpec spec{cel::ListTypeSpec(
      std::make_unique<cel::TypeSpec>(cel::PrimitiveType::kInt64))};
  EXPECT_EQ(ReprOf(spec), Repr::kList);
}

TEST(ReprOfTest, MapTypeSpec) {
  cel::TypeSpec spec{cel::MapTypeSpec(
      std::make_unique<cel::TypeSpec>(cel::PrimitiveType::kString),
      std::make_unique<cel::TypeSpec>(cel::PrimitiveType::kInt64))};
  EXPECT_EQ(ReprOf(spec), Repr::kMap);
}

TEST(ReprOfTest, MessageTypeSpec) {
  cel::TypeSpec spec{cel::MessageTypeSpec("google.example.Request")};
  EXPECT_EQ(ReprOf(spec), Repr::kMessage);
}

TEST(ReprOfTest, TypeOfTypeIsKType) {
  // `type(x)` metatype: the variant alternative is std::unique_ptr<TypeSpec>.
  cel::TypeSpec spec{
      std::make_unique<cel::TypeSpec>(cel::PrimitiveType::kInt64)};
  EXPECT_EQ(ReprOf(spec), Repr::kType);
}

TEST(ReprOfTest, DynIsUnknownSoValidatorCanReject) {
  EXPECT_EQ(ReprOf(cel::TypeSpec{cel::DynTypeSpec{}}), Repr::kUnknown);
}

TEST(ReprOfTest, ErrorTypeIsUnknown) {
  EXPECT_EQ(ReprOf(cel::TypeSpec{cel::ErrorTypeSpec::kValue}), Repr::kUnknown);
}

TEST(ReprOfTest, FunctionTypeIsUnknown) {
  auto fn = cel::FunctionTypeSpec(
      std::make_unique<cel::TypeSpec>(cel::PrimitiveType::kBool),
      std::vector<cel::TypeSpec>{});
  EXPECT_EQ(ReprOf(cel::TypeSpec{std::move(fn)}), Repr::kUnknown);
}

TEST(ReprOfTest, ParamTypeIsUnknown) {
  EXPECT_EQ(ReprOf(cel::TypeSpec{cel::ParamTypeSpec("T")}), Repr::kUnknown);
}

TEST(ReprOfTest, UnsetIsUnknown) {
  EXPECT_EQ(ReprOf(cel::TypeSpec{}), Repr::kUnknown);
}

TEST(ReprOfTest, AbstractTypeIsUnknown) {
  cel::AbstractType abstract("my.Abstract", {});
  EXPECT_EQ(ReprOf(cel::TypeSpec{std::move(abstract)}), Repr::kUnknown);
}

// ---- PopulateAnnotations + TypedAst plumbing --------------------------------

TEST(PopulateAnnotationsTest, SeedsOneEntryPerTypedNode) {
  cel::Ast ast;
  ast.mutable_type_map()[1] = cel::TypeSpec{cel::PrimitiveType::kBool};
  ast.mutable_type_map()[2] = cel::TypeSpec{cel::PrimitiveType::kInt64};
  ast.mutable_type_map()[3] = cel::TypeSpec{cel::NullTypeSpec{}};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, annotations);

  ASSERT_EQ(annotations.nodes().size(), 3u);
  EXPECT_EQ(annotations.Find(1)->repr, Repr::kBool);
  EXPECT_EQ(annotations.Find(2)->repr, Repr::kInt);
  EXPECT_EQ(annotations.Find(3)->repr, Repr::kNull);
}

TEST(PopulateAnnotationsTest, LeavesUnseenNodesAbsent) {
  cel::Ast ast;
  ast.mutable_type_map()[99] = cel::TypeSpec{cel::PrimitiveType::kString};
  WasmAnnotations annotations;
  PopulateAnnotations(ast, annotations);
  EXPECT_EQ(annotations.Find(1), nullptr);
  ASSERT_NE(annotations.Find(99), nullptr);
  EXPECT_EQ(annotations.Find(99)->repr, Repr::kString);
}

TEST(PopulateAnnotationsTest, DynEntriesBecomeKUnknown) {
  // Explicit DynTypeSpec entry (some checkers do emit these). The annotation
  // is still written as kUnknown so downstream passes see it as a poisoned
  // node rather than silently missing.
  cel::Ast ast;
  ast.mutable_type_map()[5] = cel::TypeSpec{cel::DynTypeSpec{}};
  WasmAnnotations annotations;
  PopulateAnnotations(ast, annotations);
  ASSERT_NE(annotations.Find(5), nullptr);
  EXPECT_EQ(annotations.Find(5)->repr, Repr::kUnknown);
}

TEST(TypedAstTest, DefaultIsEmpty) {
  TypedAst ta;
  EXPECT_FALSE(ta.has_ast());
  EXPECT_EQ(ta.annotations().nodes().size(), 0u);
}

TEST(TypedAstTest, OwnsAstAndAnnotations) {
  auto ast = std::make_unique<cel::Ast>();
  ast->mutable_type_map()[1] = cel::TypeSpec{cel::PrimitiveType::kBool};
  WasmAnnotations annotations;
  annotations[1].repr = Repr::kBool;

  TypedAst ta(std::move(ast), std::move(annotations));
  ASSERT_TRUE(ta.has_ast());
  EXPECT_EQ(ta.ast().type_map().size(), 1u);
  ASSERT_NE(ta.annotations().Find(1), nullptr);
  EXPECT_EQ(ta.annotations().Find(1)->repr, Repr::kBool);
}

TEST(TypedAstTest, MoveTransfersOwnership) {
  auto ast = std::make_unique<cel::Ast>();
  ast->mutable_type_map()[7] = cel::TypeSpec{cel::PrimitiveType::kInt64};
  WasmAnnotations annotations;
  annotations[7].repr = Repr::kInt;
  TypedAst original(std::move(ast), std::move(annotations));

  TypedAst moved = std::move(original);
  ASSERT_TRUE(moved.has_ast());
  EXPECT_EQ(moved.annotations().Find(7)->repr, Repr::kInt);
}

}  // namespace
}  // namespace celwasm
