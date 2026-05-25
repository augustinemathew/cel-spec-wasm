#include "compiler_v2/ir/typed_ast.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "common/ast.h"
#include "common/ast/metadata.h"
#include "common/expr.h"
#include "compiler_v2/ir/annotations.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
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

// `optional<T>` is wire-encoded as AbstractType{name="optional_type",
// parameter_types=[inner]} by cel-cpp's checker (probed in
// `compiler_v2/probes/optionals/ast_shape_probe_test.cc` Q8).  ReprOf
// must distinguish it from the generic AbstractTypeIsUnknown path so
// codegen can route through `cel_select_optional_field_at_vv`.
TEST(ReprOfTest, OptionalTypeAbstractIsKOptional) {
  std::vector<cel::TypeSpec> params;
  params.emplace_back(cel::PrimitiveType::kInt64);
  cel::AbstractType opt("optional_type", std::move(params));
  EXPECT_EQ(ReprOf(cel::TypeSpec{std::move(opt)}), Repr::kOptional);
}

TEST(ReprOfTest, OtherAbstractNamedAbstractStaysUnknown) {
  // Confirms the `name == "optional_type"` predicate is exact —
  // a future cel-cpp AbstractType shouldn't accidentally light up
  // the optional Repr path.
  cel::AbstractType abstract("some.Other", {});
  EXPECT_EQ(ReprOf(cel::TypeSpec{std::move(abstract)}), Repr::kUnknown);
}

TEST(ReprOfTest, OptionalOfDynAbstractStillStampsKOptional) {
  // `UnacceptableLabel` recurses through abstract parameters and
  // rejects `optional<dyn>` at the static-subset gate (before codegen
  // runs).  ReprOf stamps Repr::kOptional regardless — the dyn-reject
  // is a different layer.
  std::vector<cel::TypeSpec> params;
  params.emplace_back(cel::DynTypeSpec{});
  cel::AbstractType opt("optional_type", std::move(params));
  EXPECT_EQ(ReprOf(cel::TypeSpec{std::move(opt)}), Repr::kOptional);
}

// ---- PopulateAnnotations + TypedAst plumbing --------------------------------

TEST(PopulateAnnotationsTest, SeedsOneEntryPerTypedNode) {
  cel::Ast ast;
  ast.mutable_type_map()[1] = cel::TypeSpec{cel::PrimitiveType::kBool};
  ast.mutable_type_map()[2] = cel::TypeSpec{cel::PrimitiveType::kInt64};
  ast.mutable_type_map()[3] = cel::TypeSpec{cel::NullTypeSpec{}};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, /*pool=*/nullptr, annotations);

  ASSERT_EQ(annotations.nodes().size(), 3u);
  EXPECT_EQ(annotations.Find(1)->repr, Repr::kBool);
  EXPECT_EQ(annotations.Find(2)->repr, Repr::kInt);
  EXPECT_EQ(annotations.Find(3)->repr, Repr::kNull);
}

TEST(PopulateAnnotationsTest, LeavesUnseenNodesAbsent) {
  cel::Ast ast;
  ast.mutable_type_map()[99] = cel::TypeSpec{cel::PrimitiveType::kString};
  WasmAnnotations annotations;
  PopulateAnnotations(ast, /*pool=*/nullptr, annotations);
  EXPECT_EQ(annotations.Find(1), nullptr);
  ASSERT_NE(annotations.Find(99), nullptr);
  EXPECT_EQ(annotations.Find(99)->repr, Repr::kString);
}

TEST(PopulateAnnotationsTest, OptionalAbstractEntryBecomesKOptional) {
  // End-to-end: an `optional<int>`-typed AST node lands in the
  // annotation map with Repr::kOptional, so downstream codegen
  // can branch on operand repr.
  cel::Ast ast;
  std::vector<cel::TypeSpec> params;
  params.emplace_back(cel::PrimitiveType::kInt64);
  ast.mutable_type_map()[7] =
      cel::TypeSpec{cel::AbstractType("optional_type", std::move(params))};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, /*pool=*/nullptr, annotations);
  ASSERT_NE(annotations.Find(7), nullptr);
  EXPECT_EQ(annotations.Find(7)->repr, Repr::kOptional);
}

TEST(PopulateAnnotationsTest, DynEntriesBecomeKUnknown) {
  // Explicit DynTypeSpec entry (some checkers do emit these). The annotation
  // is still written as kUnknown so downstream passes see it as a poisoned
  // node rather than silently missing.
  cel::Ast ast;
  ast.mutable_type_map()[5] = cel::TypeSpec{cel::DynTypeSpec{}};
  WasmAnnotations annotations;
  PopulateAnnotations(ast, /*pool=*/nullptr, annotations);
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

// ---- G2: proto field-number resolution for SelectExpr ----------------------

// Builds a self-contained descriptor pool exposing one message
// `celwasm.testg2.G2Msg` with a field of every proto wire-representable
// type.  Field numbers are deliberately non-contiguous so an off-by-one
// bug in the resolver cannot silently pass — and the table below pairs
// each field name with its expected number so we can read coverage at a
// glance.
class G2PoolFixture {
 public:
  G2PoolFixture() {
    google::protobuf::FileDescriptorProto file;
    file.set_name("celwasm_testg2.proto");
    file.set_package("celwasm.testg2");
    file.set_syntax("proto2");

    google::protobuf::DescriptorProto* outer = file.add_message_type();
    outer->set_name("G2Msg");
    AddKindEnum(outer);
    AddScalarFields(outer);
    AddKindField(outer);

    pool_ = std::make_unique<google::protobuf::DescriptorPool>();
    const auto* added = pool_->BuildFile(file);
    ABSL_CHECK(added != nullptr) << "BuildFile failed for G2 fixture";
  }

  const google::protobuf::DescriptorPool* pool() const {
    return pool_.get();
  }

 private:
  static void AddKindEnum(google::protobuf::DescriptorProto* outer) {
    google::protobuf::EnumDescriptorProto* kind_enum = outer->add_enum_type();
    kind_enum->set_name("Kind");
    google::protobuf::EnumValueDescriptorProto* kind_zero =
        kind_enum->add_value();
    kind_zero->set_name("KIND_UNSPECIFIED");
    kind_zero->set_number(0);
    google::protobuf::EnumValueDescriptorProto* kind_one =
        kind_enum->add_value();
    kind_one->set_name("KIND_A");
    kind_one->set_number(1);
  }

  static void AddScalarFields(google::protobuf::DescriptorProto* outer) {
    using Field = google::protobuf::FieldDescriptorProto;
    AddField(outer, "i64_field", 1, Field::TYPE_INT64, Field::LABEL_OPTIONAL);
    AddField(outer, "b_field", 2, Field::TYPE_BOOL, Field::LABEL_OPTIONAL);
    AddField(outer, "s_field", 3, Field::TYPE_STRING, Field::LABEL_OPTIONAL);
    AddField(outer, "i32_field", 4, Field::TYPE_INT32, Field::LABEL_OPTIONAL);
    AddMessageField(outer, "inner", 5, ".celwasm.testg2.G2Msg",
                    Field::LABEL_OPTIONAL);
    AddField(outer, "u32_field", 6, Field::TYPE_UINT32, Field::LABEL_OPTIONAL);
    AddField(outer, "float_field", 7, Field::TYPE_FLOAT, Field::LABEL_OPTIONAL);
    AddField(outer, "double_field", 8, Field::TYPE_DOUBLE,
             Field::LABEL_OPTIONAL);
    AddField(outer, "bytes_field", 9, Field::TYPE_BYTES, Field::LABEL_OPTIONAL);
    AddField(outer, "u64_field", 10, Field::TYPE_UINT64, Field::LABEL_OPTIONAL);
    AddField(outer, "sint32_field", 11, Field::TYPE_SINT32,
             Field::LABEL_OPTIONAL);
    AddField(outer, "fixed32_field", 12, Field::TYPE_FIXED32,
             Field::LABEL_OPTIONAL);
    AddField(outer, "repeated_i32", 13, Field::TYPE_INT32,
             Field::LABEL_REPEATED);
    AddField(outer, "fixed64_field", 16, Field::TYPE_FIXED64,
             Field::LABEL_OPTIONAL);
    AddField(outer, "sfixed32_field", 17, Field::TYPE_SFIXED32,
             Field::LABEL_OPTIONAL);
    AddField(outer, "sfixed64_field", 18, Field::TYPE_SFIXED64,
             Field::LABEL_OPTIONAL);
    AddField(outer, "sint64_field", 19, Field::TYPE_SINT64,
             Field::LABEL_OPTIONAL);
  }

  static void AddKindField(google::protobuf::DescriptorProto* outer) {
    using Field = google::protobuf::FieldDescriptorProto;
    Field* kind_f = outer->add_field();
    kind_f->set_name("kind");
    kind_f->set_number(15);
    kind_f->set_type(Field::TYPE_ENUM);
    kind_f->set_type_name(".celwasm.testg2.G2Msg.Kind");
    kind_f->set_label(Field::LABEL_OPTIONAL);
  }

  static void AddField(google::protobuf::DescriptorProto* msg,
                       absl::string_view name, int number,
                       google::protobuf::FieldDescriptorProto::Type type,
                       google::protobuf::FieldDescriptorProto::Label label) {
    google::protobuf::FieldDescriptorProto* f = msg->add_field();
    f->set_name(std::string(name));
    f->set_number(number);
    f->set_type(type);
    f->set_label(label);
  }

  static void AddMessageField(
      google::protobuf::DescriptorProto* msg, absl::string_view name,
      int number, absl::string_view type_name,
      google::protobuf::FieldDescriptorProto::Label label) {
    google::protobuf::FieldDescriptorProto* f = msg->add_field();
    f->set_name(std::string(name));
    f->set_number(number);
    f->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
    f->set_type_name(std::string(type_name));
    f->set_label(label);
  }

  std::unique_ptr<google::protobuf::DescriptorPool> pool_;
};

cel::Ast BuildSelectAst(int64_t operand_id, int64_t select_id,
                        absl::string_view operand_name,
                        absl::string_view field_name, bool test_only = false) {
  cel::Expr root;
  root.set_id(select_id);
  cel::SelectExpr& sel = root.mutable_select_expr();
  sel.set_field(std::string(field_name));
  sel.set_test_only(test_only);
  cel::Expr operand;
  operand.set_id(operand_id);
  operand.mutable_ident_expr().set_name(std::string(operand_name));
  sel.set_operand(std::move(operand));
  cel::Ast ast;
  ast.mutable_root_expr() = std::move(root);
  return ast;
}

struct FieldCase {
  const char* label;
  const char* field_name;
  uint32_t expected_number;
};

TEST(PopulateAnnotationsG2Test, ResolvesEveryProtoFieldKind) {
  G2PoolFixture fx;
  const FieldCase cases[] = {
      {"int64", "i64_field", 1u},
      {"bool", "b_field", 2u},
      {"string", "s_field", 3u},
      {"int32", "i32_field", 4u},
      {"message", "inner", 5u},
      {"uint32", "u32_field", 6u},
      {"float", "float_field", 7u},
      {"double", "double_field", 8u},
      {"bytes", "bytes_field", 9u},
      {"uint64", "u64_field", 10u},
      {"sint32", "sint32_field", 11u},
      {"fixed32", "fixed32_field", 12u},
      {"repeated_int32", "repeated_i32", 13u},
      {"enum", "kind", 15u},
      {"fixed64", "fixed64_field", 16u},
      {"sfixed32", "sfixed32_field", 17u},
      {"sfixed64", "sfixed64_field", 18u},
      {"sint64", "sint64_field", 19u},
  };

  for (const auto& row : cases) {
    cel::Ast ast = BuildSelectAst(/*operand_id=*/1, /*select_id=*/2, "msg",
                                  row.field_name);
    ast.mutable_type_map()[1] =
        cel::TypeSpec{cel::MessageTypeSpec("celwasm.testg2.G2Msg")};

    WasmAnnotations annotations;
    PopulateAnnotations(ast, fx.pool(), annotations);

    ASSERT_NE(annotations.Find(2), nullptr) << "row=" << row.label;
    EXPECT_EQ(annotations.Find(2)->field_number, row.expected_number)
        << "row=" << row.label << " field=" << row.field_name;
  }
}

TEST(PopulateAnnotationsG2Test, ResolvesTestOnlySelect) {
  G2PoolFixture fx;
  cel::Ast ast = BuildSelectAst(/*operand_id=*/20, /*select_id=*/21, "msg",
                                "s_field", /*test_only=*/true);
  ast.mutable_type_map()[20] =
      cel::TypeSpec{cel::MessageTypeSpec("celwasm.testg2.G2Msg")};
  ast.mutable_type_map()[21] = cel::TypeSpec{cel::PrimitiveType::kBool};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, fx.pool(), annotations);

  EXPECT_EQ(annotations.Find(21)->field_number, 3u);
}

TEST(PopulateAnnotationsG2Test, LeavesNonSelectNodesAtZero) {
  G2PoolFixture fx;
  cel::Expr root;
  root.set_id(42);
  root.mutable_ident_expr().set_name("msg");
  cel::Ast ast;
  ast.mutable_root_expr() = std::move(root);
  ast.mutable_type_map()[42] =
      cel::TypeSpec{cel::MessageTypeSpec("celwasm.testg2.G2Msg")};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, fx.pool(), annotations);

  ASSERT_NE(annotations.Find(42), nullptr);
  EXPECT_EQ(annotations.Find(42)->field_number, 0u);
}

TEST(PopulateAnnotationsG2Test, UnknownFieldLeavesZero) {
  G2PoolFixture fx;
  cel::Ast ast = BuildSelectAst(/*operand_id=*/1, /*select_id=*/2, "msg",
                                "does_not_exist");
  ast.mutable_type_map()[1] =
      cel::TypeSpec{cel::MessageTypeSpec("celwasm.testg2.G2Msg")};
  ast.mutable_type_map()[2] = cel::TypeSpec{cel::PrimitiveType::kInt64};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, fx.pool(), annotations);

  ASSERT_NE(annotations.Find(2), nullptr);
  EXPECT_EQ(annotations.Find(2)->field_number, 0u);
}

TEST(PopulateAnnotationsG2Test, UnknownMessageTypeLeavesZero) {
  G2PoolFixture fx;
  cel::Ast ast =
      BuildSelectAst(/*operand_id=*/1, /*select_id=*/2, "msg", "i64_field");
  ast.mutable_type_map()[1] =
      cel::TypeSpec{cel::MessageTypeSpec("celwasm.testg2.NotInPool")};
  ast.mutable_type_map()[2] = cel::TypeSpec{cel::PrimitiveType::kInt64};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, fx.pool(), annotations);

  EXPECT_EQ(annotations.Find(2)->field_number, 0u);
}

TEST(PopulateAnnotationsG2Test, NonMessageOperandLeavesZero) {
  G2PoolFixture fx;
  cel::Ast ast =
      BuildSelectAst(/*operand_id=*/1, /*select_id=*/2, "n", "i64_field");
  ast.mutable_type_map()[1] = cel::TypeSpec{cel::PrimitiveType::kInt64};
  ast.mutable_type_map()[2] = cel::TypeSpec{cel::PrimitiveType::kInt64};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, fx.pool(), annotations);

  EXPECT_EQ(annotations.Find(2)->field_number, 0u);
}

TEST(PopulateAnnotationsG2Test, ResolvesNestedSelectChain) {
  G2PoolFixture fx;
  cel::Expr root;
  root.set_id(3);
  cel::SelectExpr& outer_sel = root.mutable_select_expr();
  outer_sel.set_field("i64_field");
  cel::Expr inner;
  inner.set_id(2);
  cel::SelectExpr& inner_sel = inner.mutable_select_expr();
  inner_sel.set_field("inner");
  cel::Expr ident;
  ident.set_id(1);
  ident.mutable_ident_expr().set_name("msg");
  inner_sel.set_operand(std::move(ident));
  outer_sel.set_operand(std::move(inner));

  cel::Ast ast;
  ast.mutable_root_expr() = std::move(root);
  ast.mutable_type_map()[1] =
      cel::TypeSpec{cel::MessageTypeSpec("celwasm.testg2.G2Msg")};
  ast.mutable_type_map()[2] =
      cel::TypeSpec{cel::MessageTypeSpec("celwasm.testg2.G2Msg")};
  ast.mutable_type_map()[3] = cel::TypeSpec{cel::PrimitiveType::kInt64};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, fx.pool(), annotations);

  EXPECT_EQ(annotations.Find(2)->field_number, 5u);
  EXPECT_EQ(annotations.Find(3)->field_number, 1u);
}

TEST(PopulateAnnotationsG2Test, NullPoolSkipsFieldNumberResolution) {
  cel::Ast ast =
      BuildSelectAst(/*operand_id=*/1, /*select_id=*/2, "msg", "i64_field");
  ast.mutable_type_map()[1] =
      cel::TypeSpec{cel::MessageTypeSpec("celwasm.testg2.G2Msg")};
  ast.mutable_type_map()[2] = cel::TypeSpec{cel::PrimitiveType::kInt64};

  WasmAnnotations annotations;
  PopulateAnnotations(ast, /*pool=*/nullptr, annotations);

  ASSERT_NE(annotations.Find(2), nullptr);
  EXPECT_EQ(annotations.Find(2)->repr, Repr::kInt);
  EXPECT_EQ(annotations.Find(2)->field_number, 0u);
}

}  // namespace
}  // namespace celwasm
