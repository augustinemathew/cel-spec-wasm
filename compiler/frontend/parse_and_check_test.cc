#include "compiler/frontend/parse_and_check.h"

#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "common/ast.h"
#include "common/expr.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Look up the annotation for whichever node the checker assigned to the
// *root* expression. Tests assert against this without needing to know the
// internal id the parser hands out.
Repr RootRepr(const TypedAst& ta) {
  int64_t root_id = ta.ast().root_expr().id();
  const NodeAnnotation* ann = ta.annotations().Find(root_id);
  return ann ? ann->repr : Repr::kUnknown;
}

// ---- Positive expression shapes --------------------------------------------

TEST(ParseAndCheckTest, IntegerConstant) {
  auto r = ParseAndCheck("1", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, BoolConstant) {
  auto r = ParseAndCheck("true", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kBool);
}

TEST(ParseAndCheckTest, StringConstant) {
  auto r = ParseAndCheck("\"abc\"", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kString);
}

TEST(ParseAndCheckTest, DoubleConstant) {
  auto r = ParseAndCheck("1.5", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kDouble);
}

TEST(ParseAndCheckTest, NullTypeConstant) {
  auto r = ParseAndCheck("null", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kNull);
}

TEST(ParseAndCheckTest, BytesConstant) {
  auto r = ParseAndCheck("b\"ok\"", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kBytes);
}

TEST(ParseAndCheckTest, UintConstant) {
  auto r = ParseAndCheck("1u", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kUint);
}

TEST(ParseAndCheckTest, ArithmeticProducesInt) {
  auto r = ParseAndCheck("1 + 2", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, ComparisonProducesBool) {
  auto r = ParseAndCheck("1 < 2", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kBool);
}

// ---- Variable specs: every primitive ---------------------------------------

TEST(ParseAndCheckTest, PrimitiveVariableSpecs) {
  struct Row {
    const char* spec;
    const char* expr;
    Repr expected;
  };
  const Row rows[] = {
      {"b:bool", "b", Repr::kBool},      {"i:int", "i", Repr::kInt},
      {"u:uint", "u", Repr::kUint},      {"d:double", "d", Repr::kDouble},
      {"s:string", "s", Repr::kString},  {"by:bytes", "by", Repr::kBytes},
      {"n:null_type", "n", Repr::kNull},
  };
  for (const auto& row : rows) {
    CheckOptions opts;
    opts.variable_specs = {row.spec};
    auto r = ParseAndCheck(row.expr, opts);
    ASSERT_THAT(r, IsOk()) << "spec=" << row.spec;
    EXPECT_EQ(RootRepr(*r), row.expected) << "spec=" << row.spec;
  }
}

// ---- Well-known types ------------------------------------------------------

TEST(ParseAndCheckTest, TimestampVariable) {
  CheckOptions opts;
  opts.variable_specs = {"t:timestamp"};
  auto r = ParseAndCheck("t", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kTimestamp);
}

TEST(ParseAndCheckTest, DurationVariable) {
  CheckOptions opts;
  opts.variable_specs = {"d:duration"};
  auto r = ParseAndCheck("d", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kDuration);
}

TEST(ParseAndCheckTest, AnyVariable) {
  CheckOptions opts;
  opts.variable_specs = {"a:any"};
  auto r = ParseAndCheck("a", opts);
  ASSERT_THAT(r, IsOk());
  // `any` surfaces as a host-owned message at the ABI boundary.
  EXPECT_EQ(RootRepr(*r), Repr::kMessage);
}

// ---- Parameterized types ---------------------------------------------------

TEST(ParseAndCheckTest, ListVariableRoundTrip) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int>"};
  auto r = ParseAndCheck("xs", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kList);
}

TEST(ParseAndCheckTest, ListSizeProducesInt) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int>"};
  auto r = ParseAndCheck("size(xs)", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, NestedListType) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<list<int>>"};
  auto r = ParseAndCheck("xs", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kList);
}

TEST(ParseAndCheckTest, MapVariableRoundTrip) {
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int>"};
  auto r = ParseAndCheck("m", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kMap);
}

TEST(ParseAndCheckTest, MapIndexProducesValueType) {
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int>"};
  auto r = ParseAndCheck("m[\"k\"]", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, TypeSpecToleratesWhitespace) {
  // The spec parser should accept `map<K, V>` with spaces around the comma
  // and angle brackets without tripping the "trailing garbage" branch.
  CheckOptions opts;
  opts.variable_specs = {"m: map< string , int > "};
  auto r = ParseAndCheck("m", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kMap);
}

// ---- Message type from the generated pool ----------------------------------

TEST(ParseAndCheckTest, MessageVariableUsesGeneratedDescriptorPool) {
  // `google.protobuf.Empty` is always present in the process-wide generated
  // pool, so no schema file is needed.
  CheckOptions opts;
  opts.variable_specs = {"e:google.protobuf.Empty"};
  auto r = ParseAndCheck("e", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kMessage);
}

// ---- AST-shape pins for each ExprKindCase (M3 entry) -----------------------
//
// The per-ExprKindCase grid in testing-checklist.md claims parser /
// checker / annotations coverage for kIdentExpr, kSelectExpr (field),
// and kCallExpr (member).  The claims were previously implicit — every
// variable-spec test incidentally produces an IdentExpr root, and every
// `size(xs)` produces a global CallExpr — but no test actually asserts
// the AST *shape*.  M3's codegen will descend into these variants for
// the first time, so we pin the shape here before adding the codegen
// that relies on it.  A regression in the vendored cel-cpp parser that
// changed e.g. `msg.field` from a SelectExpr into a CallExpr would be
// caught here rather than manifesting as a confusing codegen ICE.

TEST(ParseAndCheckTest, IdentExprParsesToIdentNode) {
  CheckOptions opts;
  opts.variable_specs = {"x:int"};
  auto r = ParseAndCheck("x", opts);
  ASSERT_THAT(r, IsOk());
  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kIdentExpr);
  EXPECT_EQ(root.ident_expr().name(), "x");
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, SelectExprParsesToFieldAccessOnProtoMessage) {
  // `google.protobuf.DescriptorProto` is in the generated pool and has a
  // plain `string name` field — no well-known semantics get in the way.
  CheckOptions opts;
  opts.variable_specs = {"d:google.protobuf.DescriptorProto"};
  auto r = ParseAndCheck("d.name", opts);
  ASSERT_THAT(r, IsOk());
  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  EXPECT_FALSE(root.select_expr().test_only());
  EXPECT_EQ(root.select_expr().field(), "name");
  ASSERT_EQ(root.select_expr().operand().kind_case(),
            cel::ExprKindCase::kIdentExpr);
  EXPECT_EQ(root.select_expr().operand().ident_expr().name(), "d");
  // The selected field is `string`, so the annotation at the root must
  // be kString — otherwise the annotator skipped the MessageTypeSpec
  // field-type resolution path.
  EXPECT_EQ(RootRepr(*r), Repr::kString);
}

TEST(ParseAndCheckTest, MemberCallExprParsesWithTarget) {
  // A string member-call — the lean-on case for M3's
  // `_.startsWith(_)` / `_.endsWith(_)` / `_.contains(_)` lowering.
  // No variable spec needed: the receiver is a literal.
  auto r = ParseAndCheck(R"("hi".startsWith("h"))", {});
  ASSERT_THAT(r, IsOk());
  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kCallExpr);
  EXPECT_TRUE(root.call_expr().has_target())
      << "member-call form must round-trip with a non-empty target; a bare "
         "CallExpr would mean the parser collapsed the receiver into the "
         "first positional argument";
  EXPECT_EQ(root.call_expr().function(), "startsWith");
  ASSERT_EQ(root.call_expr().target().kind_case(),
            cel::ExprKindCase::kConstant);
  ASSERT_EQ(root.call_expr().args().size(), 1u);
  EXPECT_EQ(root.call_expr().args()[0].kind_case(),
            cel::ExprKindCase::kConstant);
  EXPECT_EQ(RootRepr(*r), Repr::kBool);
}

// ---- has() macro produces a test_only SelectExpr ---------------------------

TEST(ParseAndCheckTest, HasMacroLowersToTestOnlySelectExpr) {
  // The `has(m.k)` macro lowers (in cel-cpp's parser) to a
  // `SelectExpr{operand: m, field: "k", test_only: true}`. The checker
  // types the whole expression as `bool`.
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int>"};
  auto r = ParseAndCheck("has(m.k)", opts);
  ASSERT_THAT(r, IsOk());

  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  EXPECT_TRUE(root.select_expr().test_only());
  EXPECT_EQ(root.select_expr().field(), "k");
  ASSERT_EQ(root.select_expr().operand().kind_case(),
            cel::ExprKindCase::kIdentExpr);
  EXPECT_EQ(root.select_expr().operand().ident_expr().name(), "m");

  // The annotation at the test_only node must be `bool` — anything else
  // means the annotator skipped the presence-test path.
  EXPECT_EQ(RootRepr(*r), Repr::kBool);
}

// ---- G2: SelectExpr field-number resolution (Option B) ----------------------
//
// `PopulateAnnotations` walks every `SelectExpr` while the descriptor
// pool is still live and writes the resolved proto field number into
// `NodeAnnotation::field_number`.  The unit tests in
// `typed_ast_test.cc` cover the visitor directly with a hand-built
// pool.  The tests here exercise the *real* plumbing — LoadDescriptorPool
// through ParseAndCheck — against the generated pool.  A regression
// where the pool is dropped before annotation seeding, or where a future
// IR rewrite renumbers expr ids without re-running the visitor, shows up
// here rather than as a silent `field_number = 0` at codegen time.
//
// We lean on `google.protobuf.DescriptorProto` (already present in the
// generated pool): its `name` field is number 1 and its `options` field
// is number 7 of type `google.protobuf.MessageOptions`, whose
// `deprecated` field is number 3.  That gives us a real nested-select
// chain without introducing a test-only .proto.

TEST(ParseAndCheckTest, SelectExprAnnotationCarriesFieldNumber) {
  CheckOptions opts;
  opts.variable_specs = {"d:google.protobuf.DescriptorProto"};
  auto r = ParseAndCheck("d.name", opts);
  ASSERT_THAT(r, IsOk());

  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  const NodeAnnotation* ann = r->annotations().Find(root.id());
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->repr, Repr::kString);
  // `DescriptorProto.name` is proto field number 1.
  EXPECT_EQ(ann->field_number, 1u);
}

TEST(ParseAndCheckTest, NestedSelectExprResolvesEachHop) {
  // `d.options.deprecated` — inner select is `d.options`, outer is
  // `<inner>.deprecated`.  Both must carry a populated field_number.
  CheckOptions opts;
  opts.variable_specs = {"d:google.protobuf.DescriptorProto"};
  auto r = ParseAndCheck("d.options.deprecated", opts);
  ASSERT_THAT(r, IsOk());

  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  EXPECT_EQ(root.select_expr().field(), "deprecated");

  const NodeAnnotation* outer_ann = r->annotations().Find(root.id());
  ASSERT_NE(outer_ann, nullptr);
  EXPECT_EQ(outer_ann->repr, Repr::kBool);
  // `MessageOptions.deprecated` is proto field number 3.
  EXPECT_EQ(outer_ann->field_number, 3u);

  const auto& inner = root.select_expr().operand();
  ASSERT_EQ(inner.kind_case(), cel::ExprKindCase::kSelectExpr);
  EXPECT_EQ(inner.select_expr().field(), "options");
  const NodeAnnotation* inner_ann = r->annotations().Find(inner.id());
  ASSERT_NE(inner_ann, nullptr);
  // `DescriptorProto.options` is proto field number 7.
  EXPECT_EQ(inner_ann->field_number, 7u);
}

TEST(ParseAndCheckTest, HasMacroSelectExprCarriesFieldNumber) {
  // `has(d.name)` on a proto message — test_only select must still have
  // `field_number` populated; G3 codegen will need it for `has_field`.
  CheckOptions opts;
  opts.variable_specs = {"d:google.protobuf.DescriptorProto"};
  auto r = ParseAndCheck("has(d.name)", opts);
  ASSERT_THAT(r, IsOk());

  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  EXPECT_TRUE(root.select_expr().test_only());

  const NodeAnnotation* ann = r->annotations().Find(root.id());
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->field_number, 1u);
}

TEST(ParseAndCheckTest, RepeatedFieldSelectResolvesFieldNumber) {
  // Selecting a `repeated` field yields a SelectExpr whose type is
  // `list<T>`.  M3 codegen will reject this as unsupported-in-slice, but
  // the annotation resolver must still populate `field_number` — else
  // the eventual codegen error can't cite the specific field.
  CheckOptions opts;
  opts.variable_specs = {"d:google.protobuf.DescriptorProto"};
  auto r = ParseAndCheck("d.field", opts);
  ASSERT_THAT(r, IsOk());

  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  const NodeAnnotation* ann = r->annotations().Find(root.id());
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->repr, Repr::kList);
  // `DescriptorProto.field` is proto field number 2.
  EXPECT_EQ(ann->field_number, 2u);
}

TEST(ParseAndCheckTest, HasOnMapKeyLeavesFieldNumberZero) {
  // `has(m.k)` on a `map<string,int>` compiles to a SelectExpr whose
  // operand type is a map, not a message.  The visitor must skip it and
  // leave `field_number` at 0 — codegen for G3 will special-case the
  // map-presence path.
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int>"};
  auto r = ParseAndCheck("has(m.k)", opts);
  ASSERT_THAT(r, IsOk());

  const auto& root = r->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  EXPECT_TRUE(root.select_expr().test_only());

  const NodeAnnotation* ann = r->annotations().Find(root.id());
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->field_number, 0u);
}

TEST(ParseAndCheckTest, RejectsSelectOfUnknownFieldAtCheckTime) {
  // Negative: the checker rejects selects against unknown field names
  // before codegen ever runs.  This pins the invariant that
  // `PopulateAnnotations` never sees an unresolvable proto field in the
  // success path — so the only way `field_number` comes out 0 is via
  // non-message operand or an intentionally-unresolvable map operand.
  CheckOptions opts;
  opts.variable_specs = {"d:google.protobuf.DescriptorProto"};
  EXPECT_THAT(ParseAndCheck("d.does_not_exist", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ---- Annotations contract --------------------------------------------------

TEST(ParseAndCheckTest, AnnotationsAreSeededForEveryTypedNode) {
  CheckOptions opts;
  opts.variable_specs = {"a:int", "b:int"};
  auto r = ParseAndCheck("a + b", opts);
  ASSERT_THAT(r, IsOk());
  // There must be at least three typed nodes: two operand idents and one
  // call expression for `_+_`.
  EXPECT_GE(r->annotations().nodes().size(), 3u);
  for (const auto& [id, ann] : r->annotations().nodes()) {
    EXPECT_NE(ann.repr, Repr::kUnknown)
        << "expr id " << id << " ended up kUnknown in the annotation map";
  }
}

// ---- Negative: variable-spec parser errors ---------------------------------

TEST(ParseAndCheckTest, RejectsSpecWithoutColon) {
  CheckOptions opts;
  opts.variable_specs = {"bogus"};
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsSpecWithEmptyName) {
  CheckOptions opts;
  opts.variable_specs = {":int"};
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsUnknownTypeName) {
  CheckOptions opts;
  opts.variable_specs = {"x:NotAType"};
  auto s = ParseAndCheck("1", opts);
  ASSERT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.status().message().find("unknown type"), absl::string_view::npos);
}

TEST(ParseAndCheckTest, RejectsUnbalancedList) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int"};
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsUnbalancedMap) {
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int"};
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsTrailingGarbageInSpec) {
  CheckOptions opts;
  opts.variable_specs = {"x:int garbage"};
  auto s = ParseAndCheck("1", opts);
  ASSERT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.status().message().find("trailing garbage"),
            absl::string_view::npos);
}

TEST(ParseAndCheckTest, RejectsSchemaDescriptorSetNotFound) {
  CheckOptions opts;
  opts.schema = SchemaDescriptorSet{"/does/not/exist.fds"};
  EXPECT_THAT(ParseAndCheck("1", opts), StatusIs(absl::StatusCode::kNotFound));
}

TEST(ParseAndCheckTest, RejectsProtoSourceNotFound) {
  CheckOptions opts;
  opts.schema = SchemaProtoSource{"/does/not/exist.proto"};
  EXPECT_THAT(ParseAndCheck("1", opts), StatusIs(absl::StatusCode::kNotFound));
}

// Positive: loading a proto source file registers its messages and the
// checker resolves field selects through the user-supplied schema.
TEST(ParseAndCheckTest, ProtoSourceSchemaRegistersMessage) {
  CheckOptions opts;
  opts.schema = SchemaProtoSource{"testdata/e2e_fixture.proto"};
  opts.variable_specs = {"c:celwasm.testdata.Customer"};
  auto ta = ParseAndCheck("c.name", opts);
  ASSERT_THAT(ta, IsOk());
  EXPECT_EQ(RootRepr(*ta), Repr::kString);
}

TEST(ParseAndCheckTest, ProtoSourceSchemaResolvesNestedMessageField) {
  CheckOptions opts;
  opts.schema = SchemaProtoSource{"testdata/e2e_fixture.proto"};
  opts.variable_specs = {"c:celwasm.testdata.Customer"};
  auto ta = ParseAndCheck("c.billing_address.city", opts);
  ASSERT_THAT(ta, IsOk());
  EXPECT_EQ(RootRepr(*ta), Repr::kString);
}

TEST(ParseAndCheckTest, ProtoSourceSchemaRejectsSyntaxError) {
  CheckOptions opts;
  // Point at a file that exists but isn't a valid .proto source.
  opts.schema = SchemaProtoSource{"compiler/frontend/parse_and_check.h"};
  opts.variable_specs = {"c:celwasm.testdata.Customer"};
  EXPECT_THAT(ParseAndCheck("c.name", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ---- Negative: expression-level failures -----------------------------------

TEST(ParseAndCheckTest, RejectsSyntaxError) {
  // `parser::Parse` returns InvalidArgument on a malformed source.
  EXPECT_THAT(ParseAndCheck("1 +", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsUndeclaredVariable) {
  // No variable_specs and an ident → the checker flags it.
  EXPECT_THAT(ParseAndCheck("undeclared", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsTypeMismatch) {
  CheckOptions opts;
  opts.variable_specs = {"s:string", "i:int"};
  // The checker must reject string + int.
  EXPECT_THAT(ParseAndCheck("s + i", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ---- Slice 1.5: dyn(scalar) passthrough ------------------------------------

TEST(ParseAndCheckTest, RejectDynAdmitsDynScalarLiteral) {
  // `dyn(1)` must pass RejectDyn — the call site is `dyn`-typed by the
  // checker, but the underlying argument is a scalar `int`.  Slice 1.5
  // recurses into the arg only.
  auto r = ParseAndCheck("dyn(1)", {});
  ASSERT_THAT(r, IsOk());
}

TEST(ParseAndCheckTest, RejectDynAdmitsDynIdent) {
  CheckOptions opts;
  opts.variable_specs = {"x:int"};
  auto r = ParseAndCheck("dyn(x)", opts);
  ASSERT_THAT(r, IsOk());
}

TEST(ParseAndCheckTest, RejectDynAdmitsNestedDyn) {
  // `dyn(dyn(1))` — the outer call recurses into the inner call, which
  // recurses into the literal.  Both `dyn(...)` nodes are themselves
  // typed `dyn`; only the final scalar literal carries an admissible
  // type entry.
  auto r = ParseAndCheck("dyn(dyn(1))", {});
  ASSERT_THAT(r, IsOk());
}

TEST(ParseAndCheckTest, RejectDynAdmitsDynScalarAcrossEqual) {
  // The motivating shape: `dyn(int) == uint`.  Without Slice 1.5 the
  // checker types the LHS as `dyn` and our gate rejects it; with the
  // passthrough the LHS recurses to the int literal, the `_==_` call
  // resolves cross-numeric, and the whole expression admits.
  auto r = ParseAndCheck("dyn(1) == 1u", {});
  ASSERT_THAT(r, IsOk());
}

TEST(ParseAndCheckTest, RejectDynStillRejectsDynVariable) {
  // A variable whose declared type IS `dyn` — there is no `dyn(...)`
  // wrapper to recurse through, so the ident itself fails the gate.
  CheckOptions opts;
  opts.variable_specs = {"x:dyn"};
  EXPECT_THAT(ParseAndCheck("x", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectDynStillRejectsDynList) {
  // `dyn([1,2,3])` — list-typed argument is not a scalar; admission
  // criterion fails, the call site falls through to the standard
  // UnacceptableLabel dispatch, which rejects on `dyn` typing of the
  // call site itself.
  EXPECT_THAT(ParseAndCheck("dyn([1, 2, 3])", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectDynStillRejectsDynMap) {
  EXPECT_THAT(ParseAndCheck("dyn({\"a\": 1})", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectDynStillRejectsDynMessage) {
  // `dyn(msg)` — message-typed argument is not a scalar.  Admitting
  // it would invite `dyn(msg).field` (M7-surface late-bound field
  // reads that runtime cannot dispatch today).
  CheckOptions opts;
  opts.variable_specs = {"c:celwasm.testdata.Customer"};
  opts.schema = SchemaProtoSource{"testdata/e2e_fixture.proto"};
  EXPECT_THAT(ParseAndCheck("dyn(c)", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectDynStillRejectsDynFieldAccess) {
  // `dyn(msg).field` — the inner `dyn(msg)` already fails (above),
  // and the outer Select node carries `dyn` typing.  Either gate
  // catches the rejection.
  CheckOptions opts;
  opts.variable_specs = {"c:celwasm.testdata.Customer"};
  opts.schema = SchemaProtoSource{"testdata/e2e_fixture.proto"};
  EXPECT_THAT(ParseAndCheck("dyn(c).name", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectDynAdmitsScalarSelect) {
  // `dyn(c.name)` — the argument is a scalar select (string); admits
  // per Risk #1 in the plan (the field read is itself admissible).
  CheckOptions opts;
  opts.variable_specs = {"c:celwasm.testdata.Customer"};
  opts.schema = SchemaProtoSource{"testdata/e2e_fixture.proto"};
  auto r = ParseAndCheck("dyn(c.name)", opts);
  ASSERT_THAT(r, IsOk());
}

}  // namespace
}  // namespace celwasm
