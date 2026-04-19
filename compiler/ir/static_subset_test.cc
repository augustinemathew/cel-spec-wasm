#include "compiler/ir/static_subset.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "common/ast.h"
#include "common/ast/metadata.h"
#include "common/expr.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Small helper for writing compact test fixtures: build a cel::Expr of a
// given id, wire a single optional child, and stamp the id into the Ast's
// type_map with a plausible int64 spec. Reused by most tests below so the
// intent of each case stays visible.
struct AstBuilder {
  cel::Ast ast;

  void AddType(int64_t id, cel::TypeSpec spec) {
    ast.mutable_type_map()[id] = std::move(spec);
  }

  // Shorthand: stamp the id with the int64 primitive so the node passes the
  // validator. Callers override to exercise a failure mode.
  void AddOkType(int64_t id) {
    AddType(id, cel::TypeSpec{cel::PrimitiveType::kInt64});
  }

  // Chain a ready-built Expr into the root slot.
  void SetRoot(cel::Expr root) {
    ast.set_is_checked(true);
    ast.mutable_root_expr() = std::move(root);
  }
};

cel::Expr MakeConst(int64_t id) {
  cel::Expr e;
  e.set_id(id);
  e.mutable_const_expr();
  return e;
}

cel::Expr MakeIdent(int64_t id, const char* name) {
  cel::Expr e;
  e.set_id(id);
  e.mutable_ident_expr().set_name(name);
  return e;
}

// ---- Positive tests: every ExprKindCase in its typed form ------------------

TEST(RejectDynTest, RequiresCheckedAst) {
  cel::Ast ast;
  // is_checked defaults to false; the validator must flag that explicitly
  // rather than silently returning OK against a partially-populated map.
  EXPECT_THAT(RejectDyn(ast), StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST(RejectDynTest, EmptyCheckedAstIsOk) {
  // A zero-id root node is intentionally skipped — id == 0 is the "no
  // meaningful expr" signal. That must not trip the validator.
  AstBuilder b;
  b.SetRoot(cel::Expr{});
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, ConstantIsAcceptedWhenTyped) {
  AstBuilder b;
  b.AddOkType(1);
  b.SetRoot(MakeConst(1));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, IdentExprIsAcceptedWhenTyped) {
  AstBuilder b;
  b.AddOkType(1);
  b.SetRoot(MakeIdent(1, "x"));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, SelectExprIsAcceptedWhenTyped) {
  AstBuilder b;
  b.AddType(1, cel::TypeSpec{cel::MessageTypeSpec("x.Y")});
  b.AddOkType(2);
  cel::Expr root;
  root.set_id(2);
  auto& sel = root.mutable_select_expr();
  sel.set_field("f");
  sel.mutable_operand() = MakeIdent(1, "x");
  b.SetRoot(std::move(root));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, TestOnlySelectExprIsAcceptedWhenOperandTyped) {
  // The `has(x.f)` macro lowers to a SelectExpr with test_only=true.  The
  // walker must recurse into the operand but otherwise treat it like any
  // other select.
  AstBuilder b;
  b.AddType(1,
            cel::TypeSpec{cel::MapTypeSpec(
                std::make_unique<cel::TypeSpec>(cel::PrimitiveType::kString),
                std::make_unique<cel::TypeSpec>(cel::PrimitiveType::kInt64))});
  b.AddType(2, cel::TypeSpec{cel::PrimitiveType::kBool});  // has() → bool
  cel::Expr root;
  root.set_id(2);
  auto& sel = root.mutable_select_expr();
  sel.set_field("k");
  sel.set_test_only(true);
  sel.mutable_operand() = MakeIdent(1, "m");
  b.SetRoot(std::move(root));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, DynOperandInTestOnlySelectIsRejected) {
  // Even with test_only=true, a DYN operand must still be flagged — the
  // presence test produces `bool`, but the thing being probed is DYN.
  AstBuilder b;
  // id 1 (operand) missing → DYN.
  b.AddType(2, cel::TypeSpec{cel::PrimitiveType::kBool});
  cel::Expr root;
  root.set_id(2);
  auto& sel = root.mutable_select_expr();
  sel.set_field("k");
  sel.set_test_only(true);
  sel.mutable_operand() = MakeIdent(1, "m");
  b.SetRoot(std::move(root));
  auto s = RejectDyn(b.ast);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.message().find("id=1"), absl::string_view::npos);
}

TEST(RejectDynTest, CallExprIsAcceptedWithTypedArgs) {
  AstBuilder b;
  b.AddOkType(1);
  b.AddOkType(2);
  b.AddOkType(3);
  cel::Expr root;
  root.set_id(3);
  auto& call = root.mutable_call_expr();
  call.set_function("_+_");
  call.add_args() = MakeConst(1);
  call.add_args() = MakeConst(2);
  b.SetRoot(std::move(root));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, MemberCallExprWalksTarget) {
  AstBuilder b;
  b.AddOkType(1);
  b.AddOkType(2);
  b.AddOkType(3);
  cel::Expr root;
  root.set_id(3);
  auto& call = root.mutable_call_expr();
  call.set_function("startsWith");
  call.mutable_target() = MakeIdent(1, "s");
  call.add_args() = MakeConst(2);
  b.SetRoot(std::move(root));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, ListExprIsAcceptedWhenElementsTyped) {
  AstBuilder b;
  b.AddOkType(1);
  b.AddOkType(2);
  b.AddOkType(3);
  cel::Expr root;
  root.set_id(3);
  auto& list = root.mutable_list_expr();
  list.add_elements().set_expr(MakeConst(1));
  list.add_elements().set_expr(MakeConst(2));
  b.SetRoot(std::move(root));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, StructExprIsAcceptedWithTypedFields) {
  AstBuilder b;
  b.AddOkType(1);
  b.AddOkType(2);
  cel::Expr root;
  root.set_id(2);
  auto& s = root.mutable_struct_expr();
  s.set_name("x.Y");
  auto& f = s.add_fields();
  f.set_name("n");
  f.set_value(MakeConst(1));
  b.SetRoot(std::move(root));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, MapExprIsAcceptedWithTypedEntries) {
  AstBuilder b;
  b.AddOkType(1);
  b.AddOkType(2);
  b.AddOkType(3);
  cel::Expr root;
  root.set_id(3);
  auto& m = root.mutable_map_expr();
  auto& e = m.add_entries();
  e.set_key(MakeConst(1));
  e.set_value(MakeConst(2));
  b.SetRoot(std::move(root));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

TEST(RejectDynTest, ComprehensionExprIsAcceptedWhenAllSubtreesTyped) {
  AstBuilder b;
  for (int64_t id : {1, 2, 3, 4, 5, 6})
    b.AddOkType(id);
  cel::Expr root;
  root.set_id(6);
  auto& c = root.mutable_comprehension_expr();
  c.set_iter_var("x");
  c.set_accu_var("acc");
  c.set_iter_range(MakeIdent(1, "xs"));
  c.set_accu_init(MakeConst(2));
  c.set_loop_condition(MakeConst(3));
  c.set_loop_step(MakeConst(4));
  c.set_result(MakeConst(5));
  b.SetRoot(std::move(root));
  EXPECT_THAT(RejectDyn(b.ast), IsOk());
}

// ---- Negative tests: a DYN or Error node at each position ------------------

TEST(RejectDynTest, MissingTypeMapEntryIsRejectedAsDyn) {
  AstBuilder b;
  // Deliberately do not add a type_map entry for id 1.
  b.SetRoot(MakeConst(1));
  auto s = RejectDyn(b.ast);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.message().find("id=1 is dyn"), absl::string_view::npos);
}

TEST(RejectDynTest, ExplicitDynTypeSpecIsRejected) {
  AstBuilder b;
  b.AddType(1, cel::TypeSpec{cel::DynTypeSpec{}});
  b.SetRoot(MakeConst(1));
  auto s = RejectDyn(b.ast);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.message().find("is dyn"), absl::string_view::npos);
}

TEST(RejectDynTest, ErrorTypeSpecIsRejected) {
  AstBuilder b;
  b.AddType(1, cel::TypeSpec{cel::ErrorTypeSpec::kValue});
  b.SetRoot(MakeConst(1));
  auto s = RejectDyn(b.ast);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.message().find("is error"), absl::string_view::npos);
}

TEST(RejectDynTest, FunctionTypeSpecIsRejected) {
  AstBuilder b;
  auto fn = cel::FunctionTypeSpec(
      std::make_unique<cel::TypeSpec>(cel::PrimitiveType::kBool),
      std::vector<cel::TypeSpec>{});
  b.AddType(1, cel::TypeSpec{std::move(fn)});
  b.SetRoot(MakeConst(1));
  EXPECT_THAT(RejectDyn(b.ast), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(RejectDynTest, ParamTypeSpecIsRejected) {
  AstBuilder b;
  b.AddType(1, cel::TypeSpec{cel::ParamTypeSpec("T")});
  b.SetRoot(MakeConst(1));
  EXPECT_THAT(RejectDyn(b.ast), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(RejectDynTest, UnsetTypeSpecIsRejected) {
  AstBuilder b;
  b.AddType(1, cel::TypeSpec{});  // default-constructed = UnsetTypeSpec.
  b.SetRoot(MakeConst(1));
  auto s = RejectDyn(b.ast);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.message().find("is unset"), absl::string_view::npos);
}

TEST(RejectDynTest, DynChildInCallIsRejected) {
  // Parent typed, second argument DYN — validator must flag the child.
  AstBuilder b;
  b.AddOkType(1);
  // id 2 intentionally missing → DYN.
  b.AddOkType(3);
  cel::Expr root;
  root.set_id(3);
  auto& call = root.mutable_call_expr();
  call.set_function("_+_");
  call.add_args() = MakeConst(1);
  call.add_args() = MakeConst(2);
  b.SetRoot(std::move(root));
  auto s = RejectDyn(b.ast);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.message().find("id=2"), absl::string_view::npos);
}

TEST(RejectDynTest, DynOperandInSelectIsRejected) {
  AstBuilder b;
  // Missing type for id 1 (the operand).
  b.AddOkType(2);
  cel::Expr root;
  root.set_id(2);
  auto& sel = root.mutable_select_expr();
  sel.set_field("f");
  sel.mutable_operand() = MakeIdent(1, "x");
  b.SetRoot(std::move(root));
  auto s = RejectDyn(b.ast);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.message().find("id=1"), absl::string_view::npos);
}

TEST(RejectDynTest, DynElementInListIsRejected) {
  AstBuilder b;
  b.AddOkType(1);
  // id 2 deliberately DYN.
  b.AddOkType(3);
  cel::Expr root;
  root.set_id(3);
  auto& list = root.mutable_list_expr();
  list.add_elements().set_expr(MakeConst(1));
  list.add_elements().set_expr(MakeConst(2));
  b.SetRoot(std::move(root));
  auto s = RejectDyn(b.ast);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.message().find("id=2"), absl::string_view::npos);
}

TEST(RejectDynTest, DynFieldValueInStructIsRejected) {
  AstBuilder b;
  // id 1 (field value) deliberately DYN.
  b.AddOkType(2);
  cel::Expr root;
  root.set_id(2);
  auto& s = root.mutable_struct_expr();
  s.set_name("x.Y");
  auto& f = s.add_fields();
  f.set_name("n");
  f.set_value(MakeConst(1));
  b.SetRoot(std::move(root));
  auto st = RejectDyn(b.ast);
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(st.message().find("id=1"), absl::string_view::npos);
}

TEST(RejectDynTest, DynKeyInMapIsRejected) {
  AstBuilder b;
  // Missing id 1 → DYN key.
  b.AddOkType(2);
  b.AddOkType(3);
  cel::Expr root;
  root.set_id(3);
  auto& m = root.mutable_map_expr();
  auto& e = m.add_entries();
  e.set_key(MakeConst(1));
  e.set_value(MakeConst(2));
  b.SetRoot(std::move(root));
  auto st = RejectDyn(b.ast);
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(st.message().find("id=1"), absl::string_view::npos);
}

TEST(RejectDynTest, DynInComprehensionAccuInitIsRejected) {
  AstBuilder b;
  b.AddOkType(1);
  // id 2 (accu_init) missing → DYN.
  b.AddOkType(3);
  b.AddOkType(4);
  b.AddOkType(5);
  b.AddOkType(6);
  cel::Expr root;
  root.set_id(6);
  auto& c = root.mutable_comprehension_expr();
  c.set_iter_var("x");
  c.set_accu_var("acc");
  c.set_iter_range(MakeIdent(1, "xs"));
  c.set_accu_init(MakeConst(2));
  c.set_loop_condition(MakeConst(3));
  c.set_loop_step(MakeConst(4));
  c.set_result(MakeConst(5));
  b.SetRoot(std::move(root));
  auto st = RejectDyn(b.ast);
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(st.message().find("id=2"), absl::string_view::npos);
}

// ---- Per-type acceptance: every concrete CEL type passes RejectDyn --------
//
// The per-type column of `testing-checklist.md` needs a positive witness that
// a node typed T is not flagged as DYN.  A parameterized table keeps the
// coverage explicit; a future regression that special-cases one primitive
// would fail the matching row here rather than silently skipping.

TEST(RejectDynTest, AcceptsEveryPrimitiveAtRoot) {
  struct Row {
    const char* name;
    cel::PrimitiveType p;
  };
  const Row rows[] = {
      {"bool", cel::PrimitiveType::kBool},
      {"int", cel::PrimitiveType::kInt64},
      {"uint", cel::PrimitiveType::kUint64},
      {"double", cel::PrimitiveType::kDouble},
      {"string", cel::PrimitiveType::kString},
      {"bytes", cel::PrimitiveType::kBytes},
  };
  for (const auto& row : rows) {
    AstBuilder b;
    b.AddType(1, cel::TypeSpec{row.p});
    b.SetRoot(MakeConst(1));
    EXPECT_THAT(RejectDyn(b.ast), IsOk()) << "primitive=" << row.name;
  }
}

TEST(RejectDynTest, AcceptsEveryPrimitiveWrapperAtRoot) {
  struct Row {
    const char* name;
    cel::PrimitiveType p;
  };
  const Row rows[] = {
      {"BoolValue", cel::PrimitiveType::kBool},
      {"Int64Value", cel::PrimitiveType::kInt64},
      {"UInt64Value", cel::PrimitiveType::kUint64},
      {"DoubleValue", cel::PrimitiveType::kDouble},
      {"StringValue", cel::PrimitiveType::kString},
      {"BytesValue", cel::PrimitiveType::kBytes},
  };
  for (const auto& row : rows) {
    AstBuilder b;
    b.AddType(1, cel::TypeSpec{cel::PrimitiveTypeWrapper(row.p)});
    b.SetRoot(MakeConst(1));
    EXPECT_THAT(RejectDyn(b.ast), IsOk()) << "wrapper=" << row.name;
  }
}

TEST(RejectDynTest, AcceptsNullTimestampDurationAny) {
  struct Row {
    const char* name;
    cel::TypeSpec spec;
  };
  Row rows[] = {
      {"null_type", cel::TypeSpec{cel::NullTypeSpec{}}},
      {"timestamp", cel::TypeSpec{cel::WellKnownTypeSpec::kTimestamp}},
      {"duration", cel::TypeSpec{cel::WellKnownTypeSpec::kDuration}},
      {"any", cel::TypeSpec{cel::WellKnownTypeSpec::kAny}},
  };
  for (auto& row : rows) {
    AstBuilder b;
    b.AddType(1, std::move(row.spec));
    b.SetRoot(MakeConst(1));
    EXPECT_THAT(RejectDyn(b.ast), IsOk()) << "type=" << row.name;
  }
}

TEST(RejectDynTest, ReportsAllViolationsInOneMessage) {
  // Two DYN nodes at once — the error message should mention both ids so
  // the user fixes them together instead of iterating one per run.
  AstBuilder b;
  // ids 1 and 2 missing from type_map → both DYN.
  b.AddOkType(3);
  cel::Expr root;
  root.set_id(3);
  auto& call = root.mutable_call_expr();
  call.set_function("_+_");
  call.add_args() = MakeConst(1);
  call.add_args() = MakeConst(2);
  b.SetRoot(std::move(root));
  auto st = RejectDyn(b.ast);
  ASSERT_THAT(st, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(st.message().find("id=1"), absl::string_view::npos);
  EXPECT_NE(st.message().find("id=2"), absl::string_view::npos);
}

}  // namespace
}  // namespace celwasm
