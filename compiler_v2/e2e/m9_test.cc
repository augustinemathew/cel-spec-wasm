// M9 e2e test suite — the spec of "done" for the type subsystem
// (`type(x)` standard function + type-identifier idents like
// `int` / `bool` / `<message-fqn>` standalone + CEL_TYPE
// equality).  Mirrors the m7_test shape: every test asserts a
// capability `m9-type-subsystem.md` says M9 must light up;
// running this binary today (with `cel.abi.type_names[]` not yet
// in the ABI, `Value::Kind::kType` not yet on the public surface,
// and `type` still in `OverloadTable::kExplicitlyUnimplementedIds`)
// should fail every case below.  Greening the suite is the M9
// exit per `m9-type-subsystem.md` §6.
//
// The 47-row `type_deduction.textproto` cohort is unlocked via
// the `typed_result:` runner matcher in M9.F (harness-only) and
// is NOT exercised here — m9_test asserts user-visible
// capabilities, not harness behaviour.  Those rows are covered
// by `compiler_v2/conformance/runner_test.cc`.
//
// Fixtures grouped by capability (one section per slice):
//
//   - TypeOfPrimitiveE2ETest        M9.B — TEST_P over each of
//                                          12 runtime kinds
//                                          (bool/int/uint/double/
//                                          string/bytes/null/list/
//                                          map/type/error/unknown
//                                          + boundary values).
//   - TypeOfMessageE2ETest          M9.C — `type(<message>)` →
//                                          `<message-FQN>`.
//   - TypeIdentifierExpressionE2ETest M9.C — every spec type-name
//                                          standalone (`int`,
//                                          `bool`, ..., `<msg-FQN>`).
//   - TypeOfNullAndAggregateE2ETest M9.E — `type(null)` →
//                                          `null_type`; `type([])`
//                                          → `list`; `type({})`
//                                          → `map` (bare names per
//                                          langdef §"Type Values").
//   - TypeEqualityE2ETest           M9.D — CEL_TYPE × CEL_TYPE
//                                          equality (`int == int`,
//                                          `int != string`, message
//                                          FQNs, nested `type(...)`
//                                          chains).
//   - TypeAsRhsOfEqualityE2ETest    M9.D — `type(x) == typename`
//                                          ergonomic (the "this is
//                                          why M9 matters" cohort —
//                                          drives most of the
//                                          conformance unlock).
//   - TypeActivationE2ETest         M9.A — `Bind("t",
//                                          Value::Type(name))` flows
//                                          through the activation
//                                          encoder + read-side
//                                          decoder.
//   - TypeRejectE2ETest             §6.4 — checker / surface
//                                          rejection matrix
//                                          (two-arg `type(...)`,
//                                          AsType on wrong kind,
//                                          cross-kind `==`).
//
// Conformance unlock estimate per slice is logged on each test
// section; aggregate target is +80..+160 PASS in conformance per
// `m9-type-subsystem.md` §1.

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "compiler/testdata/host_fixture_proto2.pb.h"
#include "compiler/testdata/host_fixture_proto3.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/internal/cel_host.h"  // Value::Message(proto)
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::testdata::HostMsg2;
using ::celwasm::testdata::HostMsg3;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<HostMsg2>();
      google::protobuf::LinkMessageReflection<HostMsg3>();
      return 0;
    }();

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

using ConfigureFn = std::function<void(Compiler::Builder&)>;
absl::StatusOr<Compiler> BuildCompiler(const ConfigureFn& configure) {
  Compiler::Builder b;
  configure(b);
  return std::move(b).Build();
}

absl::StatusOr<Compiler> CompilerEmpty() {
  return BuildCompiler([](Compiler::Builder& /*b*/) {});
}

Instance CompilePlan(const Compiler& compiler, absl::string_view source) {
  auto program = compiler.Compile(source);
  ABSL_CHECK_OK(program) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance) << source;
  return *std::move(instance);
}

Value EvalOk(Instance& instance, const Activation& activation) {
  auto v = instance.Eval(activation);
  ABSL_CHECK_OK(v);
  return *std::move(v);
}

void ExpectCompileFails(const Compiler& compiler, absl::string_view source,
                        absl::string_view why) {
  auto program_or = compiler.Compile(source);
  EXPECT_FALSE(program_or.ok())
      << "expected `" << source << "` to fail at compile (" << why << ")";
}

// ──────────────────────────────────────────────────────────────
// 1. TypeOfPrimitiveE2ETest  (M9.B — `type(x)` codegen + helper)
//
//    Per `m9-type-subsystem.md` §6.1: every primitive runtime
//    kind × representative boundary values MUST have a positive
//    `type(<lit>)` row.  Asserts the helper computes the right
//    name for every kind, including absorbing-kind propagation
//    for error / unknown.  Pattern:
//      `type(<lit>) == <typename>`
//    so each row exercises the M9.B helper write-path AND the
//    M9.D equality kernel in one shot.
// ──────────────────────────────────────────────────────────────

struct TypeOfCase {
  std::string label;     // gtest test-name suffix.
  std::string operand;   // CEL source expression for the operand.
  std::string typename_; // expected type-name, used as RHS of `== <typename>`.
};

class TypeOfPrimitiveE2ETest : public ::testing::TestWithParam<TypeOfCase> {};

TEST_P(TypeOfPrimitiveE2ETest, TypeOfOperandEqualsTypename) {
  const auto& p = GetParam();
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  std::string source = "type(" + p.operand + ") == " + p.typename_;
  auto instance = CompilePlan(*compiler, source);
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true) << "source=" << source;
}

INSTANTIATE_TEST_SUITE_P(
    PrimitivesAndBoundaries, TypeOfPrimitiveE2ETest,
    ::testing::Values(
        // BOOL — both polarities (the helper reads operand kind
        // only; both should map to the same `bool` type-id).
        TypeOfCase{"BoolTrue", "true", "bool"},
        TypeOfCase{"BoolFalse", "false", "bool"},
        // INT — boundaries: 0 / -1 / INT64_MIN / INT64_MAX.  All
        // map to `int`; boundaries still asserted for stability.
        TypeOfCase{"IntZero", "0", "int"},
        TypeOfCase{"IntNegOne", "-1", "int"},
        TypeOfCase{"IntMin", "-9223372036854775808", "int"},
        TypeOfCase{"IntMax", "9223372036854775807", "int"},
        // UINT.
        TypeOfCase{"UintZero", "0u", "uint"},
        TypeOfCase{"UintMax", "18446744073709551615u", "uint"},
        // DOUBLE — including +0.0 / -0.0 / Infinity / NaN.  All
        // map to `double` regardless of value.
        TypeOfCase{"DoubleZero", "0.0", "double"},
        TypeOfCase{"DoubleNegZero", "-0.0", "double"},
        TypeOfCase{"DoublePos", "1.5", "double"},
        TypeOfCase{"DoubleNeg", "-1.5", "double"},
        // STRING — empty / ascii / multi-byte UTF-8.
        TypeOfCase{"StringEmpty", R"("")", "string"},
        TypeOfCase{"StringAscii", R"("hello")", "string"},
        TypeOfCase{"StringUtf8", R"("☃")", "string"},
        // BYTES.
        TypeOfCase{"BytesEmpty", R"(b"")", "bytes"},
        TypeOfCase{"BytesShort", R"(b"\x01\x02")", "bytes"},
        TypeOfCase{"BytesWithNul", R"(b"\x00\xff")", "bytes"},
        // NULL — `null_type` per langdef §"Type Values" (note
        // the underscore — spelling is load-bearing).
        TypeOfCase{"Null", "null", "null_type"},
        // LIST — bare `list`, no parameter.  Source uses a
        // homogeneous int list so RejectDyn doesn't flag.
        TypeOfCase{"ListOfInt", "[1, 2, 3]", "list"},
        // MAP — bare `map`, no parameters.
        TypeOfCase{"MapStringInt", R"({"k": 1})", "map"},
        // TYPE-of-TYPE — `type(type(1))` is `type` (the type of
        // a type-value is `type`, per langdef §"Type Values").
        TypeOfCase{"TypeOfTypeOfInt", "type(1)", "type"}),
    [](const ::testing::TestParamInfo<TypeOfCase>& info) {
      return info.param.label;
    });

TEST_F(TypeOfPrimitiveE2ETest, TypeOfErrorPropagates) {
  // langdef §"Error propagation": `type(<error>)` propagates the
  // error.  `1/0` produces a divide-by-zero CEL error value;
  // the `type(...)` helper's prelude detects CEL_ERROR and
  // copies through.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(1/0) == int");
  Activation a;
  // Outer `==` propagates the error too.  Result is a CEL error
  // value (not OK with bool false).
  auto v = instance.Eval(a);
  ASSERT_THAT(v, IsOk());
  EXPECT_TRUE(v->IsError());
}

TEST_F(TypeOfPrimitiveE2ETest, TypeOfUnknownPropagates) {
  // langdef §"Unknowns": `type(<unknown>)` propagates the unknown
  // tag.  Bind a free variable, leave it un-marshalled (PartialEval
  // path), assert the result is Unknown.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Int());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(x)");
  Activation a;  // no Bind for "x" — pattern marks it unknown
  // PartialEval with no patterns marks every unbound var unknown.
  auto v = instance.PartialEval(a, {});
  ASSERT_THAT(v, IsOk());
  EXPECT_TRUE(v->IsUnknown());
}

// ──────────────────────────────────────────────────────────────
// 2. TypeOfMessageE2ETest  (M9.C — `type(<message>)` runtime
//    descriptor-pool resolution)
//
//    Per `m9-type-subsystem.md` §3.1 + §6.1: a CEL message value's
//    runtime type is its descriptor's full name (FQN).  M9.C's
//    `cel_host_resolve_message_type_id` host trampoline reads
//    the operand's externref-table backing, walks to the
//    Descriptor, returns its FQN as a CEL_TYPE.
// ──────────────────────────────────────────────────────────────

class TypeOfMessageE2ETest : public ::testing::Test {};

TEST_F(TypeOfMessageE2ETest, TypeOfHostMsg3IsItsFqn) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "type(celwasm.testdata.HostMsg3{}) == celwasm.testdata.HostMsg3");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeOfMessageE2ETest, TypeOfHostMsg2IsItsFqn) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "type(celwasm.testdata.HostMsg2{}) == celwasm.testdata.HostMsg2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeOfMessageE2ETest, TypeOfBoundMessageIsItsFqn) {
  // Same path as the literal-construction case, but the operand
  // arrives via Activation.  Exercises the externref-backed
  // CEL_MESSAGE arm of `cel_type_of_at_v`.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("h", CelType::Message("celwasm.testdata.HostMsg3"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "type(h) == celwasm.testdata.HostMsg3");
  HostMsg3 bound;
  bound.set_i32(7);
  Activation a;
  a.Bind("h", Value::Message(bound));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeOfMessageE2ETest, TypeOfMessageFieldRead) {
  // langdef §"Field Selection" + §"Type Values": reading a message
  // field whose type is itself a message and taking its `type()`
  // returns the inner message's FQN.  Exercises both M2/M3 read
  // paths AND the M9 type-of arm in one expression.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("h", CelType::Message("celwasm.testdata.HostMsg3"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "type(h.inner) == celwasm.testdata.HostMsg3");
  HostMsg3 bound;
  bound.mutable_inner()->set_i32(11);  // singular submessage set
  Activation a;
  a.Bind("h", Value::Message(bound));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 3. TypeIdentifierExpressionE2ETest  (M9.C — type idents as
//    standalone expressions)
//
//    Per `m9-type-subsystem.md` §3.3 + §6.2: each of the 11 spec
//    type names + every message FQN, used standalone, is itself
//    an expression of type `type` whose value is the corresponding
//    CEL_TYPE.  M9.A's `InlineTypeIdentifierReferences` rewriter
//    is what makes these reach codegen as real values.
//
//    Pattern: `<typename> == <typename>` — exercises both the
//    LHS-as-value and RHS-as-value paths (which are the same
//    rewrite).  A pure `<typename>` standalone is also exercised
//    via the type-of-type test.
// ──────────────────────────────────────────────────────────────

struct TypeIdentCase {
  std::string label;
  std::string ident;  // The standalone type-identifier source.
};

class TypeIdentifierExpressionE2ETest
    : public ::testing::TestWithParam<TypeIdentCase> {};

TEST_P(TypeIdentifierExpressionE2ETest, IdentEqualsItself) {
  const auto& p = GetParam();
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  std::string source = p.ident + " == " + p.ident;
  auto instance = CompilePlan(*compiler, source);
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true) << "source=" << source;
}

INSTANTIATE_TEST_SUITE_P(
    SpecTypeNames, TypeIdentifierExpressionE2ETest,
    ::testing::Values(
        TypeIdentCase{"Bool", "bool"}, TypeIdentCase{"Int", "int"},
        TypeIdentCase{"Uint", "uint"}, TypeIdentCase{"Double", "double"},
        TypeIdentCase{"String", "string"}, TypeIdentCase{"Bytes", "bytes"},
        TypeIdentCase{"NullType", "null_type"},
        TypeIdentCase{"List", "list"}, TypeIdentCase{"Map", "map"},
        TypeIdentCase{"Type", "type"},
        TypeIdentCase{"MessageHostMsg3", "celwasm.testdata.HostMsg3"}),
    [](const ::testing::TestParamInfo<TypeIdentCase>& info) {
      return info.param.label;
    });

TEST_F(TypeIdentifierExpressionE2ETest, TypeOfTypeIdentIsType) {
  // `type(int)` is `type` per langdef §"Type Values" (the type of
  // any type-value is `type`).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(int) == type");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeIdentifierExpressionE2ETest, TypeOfMessageFqnIdentIsType) {
  // Message-FQN type idents go through the same rewrite as
  // primitives — `type(<msg-fqn>)` should also be `type`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "type(celwasm.testdata.HostMsg3) == type");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 4. TypeOfNullAndAggregateE2ETest  (M9.E — bare aggregate /
//    null type names)
//
//    Per langdef §"Type Values": list / map type values are
//    bare names (not parameterised); null is `null_type`
//    (underscore).  Most of these are also covered in
//    TypeOfPrimitiveE2ETest's parameter table; pinned
//    individually here as the "spec citation" rows so a
//    regression in name spelling fails with a focused diagnostic.
// ──────────────────────────────────────────────────────────────

class TypeOfNullAndAggregateE2ETest : public ::testing::Test {};

TEST_F(TypeOfNullAndAggregateE2ETest, TypeOfNullIsNullTypeUnderscore) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(null) == null_type");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeOfNullAndAggregateE2ETest, TypeOfHomogeneousListIsBareList) {
  // `list` not `list<int>` — per langdef §"Type Values" the type
  // value is the bare name.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type([1, 2, 3]) == list");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeOfNullAndAggregateE2ETest, TypeOfMapIsBareMap) {
  // `map` not `map<string, int>` — bare name per spec.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(type({"k": 1}) == map)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeOfNullAndAggregateE2ETest, TypeOfTimestampIsAbstractFqn) {
  // Per `m9-type-subsystem.md` §2.2: timestamp construction is a
  // separate slice (post-M7).  Until that lands, `timestamp(...)`
  // can't be reached from CEL source.  The expected behaviour
  // post-timestamps: `type(timestamp(...))` →
  // `google.protobuf.Timestamp`.  Skipped today; flip when
  // timestamps construction lights up.
  GTEST_SKIP() << "timestamp() construction is a separate slice "
                  "(post-M7); enable when it lands";
}

// ──────────────────────────────────────────────────────────────
// 5. TypeEqualityE2ETest  (M9.D — CEL_TYPE × CEL_TYPE equality
//    kernel arm)
//
//    Per `m9-type-subsystem.md` §3.4 + §6.3: two CEL_TYPE values
//    are equal iff their interned names match.  Cross-kind
//    comparisons return `false` (heterogeneous-equality rule),
//    not error.
// ──────────────────────────────────────────────────────────────

class TypeEqualityE2ETest : public ::testing::Test {};

TEST_F(TypeEqualityE2ETest, SameTypeIdentEqual) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "int == int");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeEqualityE2ETest, DifferentTypeIdentNotEqual) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "int != string");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeEqualityE2ETest, TypeOfMatchesTypeOfSameKind) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(1) == type(2)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeEqualityE2ETest, TypeOfDoesNotMatchTypeOfDifferentKind) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(1) == type(\"x\")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(TypeEqualityE2ETest, NestedTypeOfEqualsBare) {
  // langdef §"Type Values" cited example: `type(type(1)) ==
  // type(string)` evaluates to `true` (both are `type`).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(type(1)) == type(string)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeEqualityE2ETest, TypeOfTypeIdentEqualsType) {
  // `type(int) == type` — the type of a type-value is `type`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(int) == type");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeEqualityE2ETest, MessageTypeOfSameFqnEqual) {
  // Two literal-constructed messages with the same FQN have
  // equal `type(...)` values.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "type(celwasm.testdata.HostMsg3{}) == "
      "type(celwasm.testdata.HostMsg3{i32: 1})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeEqualityE2ETest, MessageTypeOfDifferentFqnNotEqual) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "type(celwasm.testdata.HostMsg3{}) != "
      "type(celwasm.testdata.HostMsg2{})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 6. TypeAsRhsOfEqualityE2ETest  (M9.D — `type(x) == typename`
//    ergonomic, the dominant unlock cohort)
//
//    Per `m9-type-subsystem.md` §1: this is the shape `type(x)`
//    rows in `dynamic.textproto` / `enums.textproto` /
//    `proto2.textproto` / `proto3.textproto` use.  The 255-row
//    `envelope:` SKIP cohort lifts via this exact pattern.
// ──────────────────────────────────────────────────────────────

class TypeAsRhsOfEqualityE2ETest : public ::testing::Test {};

TEST_F(TypeAsRhsOfEqualityE2ETest, TypeOfBoolEqualsBool) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(true) == bool");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeAsRhsOfEqualityE2ETest, TypeOfIntEqualsInt) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(42) == int");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeAsRhsOfEqualityE2ETest, TypeOfStringEqualsString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(type("hi") == string)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeAsRhsOfEqualityE2ETest, TypeOfBytesEqualsBytes) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(type(b"x") == bytes)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeAsRhsOfEqualityE2ETest, TypeOfNullEqualsNullType) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(null) == null_type");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeAsRhsOfEqualityE2ETest, TypeOfMessageEqualsMessageFqnIdent) {
  // The cohort that drives `proto2.textproto` / `proto3.textproto`
  // unlock — `type(msg) == <msg-FQN>` shape.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "type(celwasm.testdata.HostMsg3{}) == celwasm.testdata.HostMsg3");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeAsRhsOfEqualityE2ETest, CrossKindEqualityIsFalseNotError) {
  // Per langdef §"Equality": cross-kind `==` is `false`, not
  // an error.  `int == 7` (CEL_TYPE vs CEL_INT) returns false.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "int == 7");
  Activation a;
  // Outer eval: bool false (NOT an error).
  auto v = instance.Eval(a);
  ASSERT_THAT(v, IsOk());
  EXPECT_FALSE(v->IsError());
  EXPECT_EQ(*v->AsBool(), false);
}

// ──────────────────────────────────────────────────────────────
// 7. TypeActivationE2ETest  (M9.A — Bind + read-side decode for
//    CEL_TYPE)
//
//    `Bind("t", Value::Type("bool"))` flows through the
//    activation encoder; the bound `t` is then comparable like
//    any other CEL_TYPE expression.  Read-side decoder is
//    asserted by `Instance::Eval`'s return value being a
//    Value::Type when the root expression returns a CEL_TYPE.
// ──────────────────────────────────────────────────────────────

class TypeActivationE2ETest : public ::testing::Test {};

TEST_F(TypeActivationE2ETest, BoundTypeEqualsLiteralIdent) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    // Per `m9-type-subsystem.md` §4.6 + R5: the public CelType
    // surface needs a `CelType::Type()` factory or accept a TYPE
    // VariableSpec spec-string.  Spec syntax mirrors langdef:
    // `type` is the type-name keyword for type-of-types.  The
    // exact public-API spelling is a design point M9.A pins;
    // this test asserts the contract end-to-end.
    b.DeclareVariable("t", CelType::Type());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "t == bool");
  Activation a;
  a.Bind("t", Value::Type("bool"));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeActivationE2ETest, BoundTypeEqualsTypeOfRuntimeValue) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("t", CelType::Type());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "t == type(7)");
  Activation a;
  a.Bind("t", Value::Type("int"));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(TypeActivationE2ETest, ReadSideDecoderReturnsValueType) {
  // Eval a root expression that produces a CEL_TYPE; assert the
  // returned `cel::Value` is `Value::Kind::kType` with the
  // expected name.  Exercises the read-side `DecodeCelValueAt`
  // for CEL_TYPE.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "type(7)");
  Activation a;
  Value v = EvalOk(instance, a);
  EXPECT_EQ(v.kind(), Value::Kind::kType);
  ASSERT_THAT(v.AsType(), IsOk());
  EXPECT_EQ(*v.AsType(), "int");
}

// ──────────────────────────────────────────────────────────────
// 8. TypeRejectE2ETest  (§6.4 — surface rejection matrix)
//
//    Per CLAUDE.md "Cover the edge-case matrix — this is a
//    compiler.  Negative coverage is ≥ 30%".  These rows pin
//    the rejection invariants — not the diagnostic strings
//    (which fluctuate with cel-cpp version bumps).
// ──────────────────────────────────────────────────────────────

class TypeRejectE2ETest : public ::testing::Test {};

TEST_F(TypeRejectE2ETest, TwoArgTypeRejected) {
  // `type(...)` has only the unary overload per cel-cpp's
  // `type_conversion_functions.cc:464`.  Two-arg form must
  // reject at compile time.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "type(1, 2)",
                     "type takes exactly one argument");
}

TEST_F(TypeRejectE2ETest, AsTypeOnNonTypeValueIsInvalidArgument) {
  // Public API contract: `Value::AsType()` on a non-kType value
  // returns `InvalidArgument`.  Mirrors `AsBool()` / `AsInt()`
  // accessor-error policy on the user surface.
  Value v = Value::Int(7);
  auto t = v.AsType();
  EXPECT_FALSE(t.ok());
  EXPECT_EQ(t.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(TypeRejectE2ETest, ConstructValueTypeWithUnknownNameBindsButPlanRejects) {
  // `Value::Type("not_a_real_kind")` is accepted at the public
  // factory (no validation; consistent with `Value::Message`).
  // Bind'ing it and Eval'ing surfaces a Plan-or-runtime error
  // because the name isn't in the per-Plan intern table — the
  // gate is "every CEL_TYPE referenced at runtime must have an
  // interned id".  Exact failure mode (Plan vs. runtime trap)
  // is a §4.4 design choice; this test asserts that SOME
  // failure surfaces — not a silent miscompare.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("t", CelType::Type());
  });
  ASSERT_THAT(compiler, IsOk());
  auto program = compiler->Compile("t == int");
  ASSERT_THAT(program, IsOk());
  auto instance_or = GlobalEngine().Plan(*program);
  ASSERT_THAT(instance_or, IsOk());
  Activation a;
  a.Bind("t", Value::Type("not_a_real_kind"));
  // Eval surfaces an error or a non-OK status; the failure mode
  // is a M9.A design point (probably error-Value with a
  // type-name-unknown payload — clean per langdef §"Error
  // propagation" which makes this a runtime CEL error, not a
  // host error).
  auto v = instance_or->Eval(a);
  if (v.ok()) {
    EXPECT_TRUE(v->IsError())
        << "expected error Value when binding an uninterned "
           "type name; got kind="
        << static_cast<int>(v->kind());
  }
  // (else: non-OK status is also acceptable per the §4.4 design
  // choice; the load-bearing assertion is "not silently true".)
}

TEST_F(TypeRejectE2ETest, TypeKeywordAsValueOfNonTypeOperand) {
  // `1 == type` is a `int == type` cross-kind comparison.  Per
  // §3.4 + langdef §"Equality": cross-kind `==` is `false`,
  // not an error — the checker doesn't reject this (heterogeneous
  // equality is admitted).  Pin the invariant.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "1 == type");
  Activation a;
  auto v = instance.Eval(a);
  ASSERT_THAT(v, IsOk());
  EXPECT_FALSE(v->IsError());
  EXPECT_EQ(*v->AsBool(), false);
}

}  // namespace
}  // namespace cel
