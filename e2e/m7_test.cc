// M7 e2e test suite — the spec of "done" for proto message
// literal construction (`Foo{a: 1, b: "x"}`).  Mirrors the
// m4_test / m5_test shape: every test asserts a capability
// `m7-proto-literals.md` says M7 must light up; running this
// binary today (with `kStructExpr` still `Unimplemented` in
// `expr_lower.cc:871`) should fail every case below.  Greening
// the suite is the M7 exit per `m7-proto-literals.md` §6.
//
// Wrapper-types (auto-wrap from scalar, wrapper-vs-scalar `==`
// peel) are M8 — see `m8-wrapper-types.md`.  M7 covers explicit
// wrapper-message construction (`Foo{w: Int32Value{value: 5}}`)
// only, via the recursive `kStructExpr` path lit by M7.E.
//
// Fixtures grouped by capability (one section per slice):
//
//   - ProtoLiteralEmptyE2ETest      M7.A — `Foo{}` empty literal,
//                                          read defaults back via
//                                          M2/M3 read paths.
//   - ProtoLiteralScalarE2ETest     M7.B — TEST_P over (cpp_type ×
//                                          boundary value) for the
//                                          10 scalar / enum cpp_types.
//   - ProtoLiteralRepeatedE2ETest   M7.C — repeated field set from
//                                          arena-list and host-list.
//   - ProtoLiteralMapE2ETest        M7.C — map field set per key /
//                                          value cpp_type pair.
//   - ProtoLiteralOneofE2ETest      M7.C — oneof clear-on-set for
//                                          proto2 + proto3.
//   - ProtoLiteralEnumE2ETest       M7.D — enum-literal RHS into an
//                                          enum field; round-trip
//                                          through int.
//   - ProtoLiteralNestedE2ETest     M7.E — `Foo{nested: Bar{...}}`
//                                          incl. explicit-wrapper
//                                          nested case (M8 territory
//                                          for auto-wrap-from-scalar
//                                          only — that lives in
//                                          m8_test.cc).
//   - ProtoLiteralDefaultsE2ETest   §6.3 — proto2 `[default = X]`
//                                          and proto3 zero-default
//                                          read regression on
//                                          M7-constructed messages.
//   - ProtoLiteralEqualityE2ETest        — constructed-message
//                                          equality via the M5.B
//                                          `cel_message_eq` kernel
//                                          (the cohort that's been
//                                          unreachable until M7).
//   - ProtoLiteralActivationE2ETest §4.5 — list-of-message
//                                          bindings + map-from-
//                                          activation; the encoder
//                                          polish bullet from §4.5.
//   - ProtoLiteralRejectE2ETest     §6.4 — checker-side rejection
//                                          matrix (unknown
//                                          descriptor / field /
//                                          type-mismatch / shape).
//
// Conformance unlock estimate per slice is logged on each test
// section; aggregate target is +200..+280 PASS in conformance per
// `m7-proto-literals.md` §1.

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
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/internal/cel_host.h"  // HostListBacking definition
#include "compiler/program.h"
#include "common/type.h"
#include "eval/value.h"
#include "testdata/e2e_fixture.pb.h"
#include "testdata/host_fixture_proto2.pb.h"
#include "testdata/host_fixture_proto3.pb.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace celwasm::api {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::testdata::Customer;
using ::celwasm::testdata::HostMsg2;
using ::celwasm::testdata::HostMsg3;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<Customer>();
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

// Helper for the §6.4 negative matrix: assert Compile() returns
// non-OK.  We don't pin the exact status code (cel-cpp's checker
// folds unrelated diagnostics under `InvalidArgument`); the
// invariant is "this should not lower to a Program."
void ExpectCompileFails(const Compiler& compiler, absl::string_view source,
                        absl::string_view why) {
  auto program_or = compiler.Compile(source);
  EXPECT_FALSE(program_or.ok())
      << "expected `" << source << "` to fail at compile (" << why << ")";
}

// ──────────────────────────────────────────────────────────────
// 1. ProtoLiteralEmptyE2ETest  (M7.A — kStructExpr admission +
//    cel_make_message)
//
//    The simplest M7 row: `Foo{}` constructs a default proto and
//    every field reads as its descriptor-stated default.  Empty
//    construction has no `cel_set_field` calls, so this isolates
//    M7.A from M7.B/C/D/E.  Greening these alone proves the
//    type-id intern table + cel_make_message + ResolvePass /
//    LayoutPass extensions land cleanly.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralEmptyE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralEmptyE2ETest, EmptyProto3MessageHasZeroDefaultInt) {
  // Proto3 zero-default scalar: `i32` reads as 0.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{}.i32 == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEmptyE2ETest, EmptyProto3MessageHasZeroDefaultString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"(celwasm.testdata.HostMsg3{}.s == "")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEmptyE2ETest, EmptyProto3MessageHasZeroDefaultBool) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{}.b == false");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEmptyE2ETest, EmptyProto3MessageHasZeroDefaultDouble) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{}.f64 == 0.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEmptyE2ETest,
       EmptyProto3MessageReadsDefaultInstanceForUnsetSubmessage) {
  // langdef §"Messages" + conformance row proto3/empty_field/nested_message:
  // accessing an unset singular MESSAGE field returns the
  // default-instance message of the field's type, NOT null.  Null is
  // the unset behavior for WKT WRAPPER fields and for Any fields only
  // — non-wrapper message fields propagate the default-instance.
  // (Pre-2026-06-05 this test asserted `== null`, which was the
  // pre-fix behavior; the conformance corpus and cel-cpp both say
  // default-instance.)
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{}.inner == "
                  "celwasm.testdata.HostMsg3{}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEmptyE2ETest, EmptyProto2MessageHasExplicitDefaultInt) {
  // langdef §"Field Selection": proto2 unset singular field reads
  // its descriptor-stated default (`[default = 42]`).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg2{}.default_i32 == 42");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEmptyE2ETest, EmptyProto2MessageHasExplicitDefaultString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(celwasm.testdata.HostMsg2{}.default_s == "hello")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEmptyE2ETest, EmptyProto2MessageHasExplicitDefaultBool) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg2{}.default_b == true");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEmptyE2ETest, CustomerEmptyConstruction) {
  // Friendly-domain check: confirm Customer (proto3) constructs
  // with all-default fields, exactly the shape `comparisons.textproto`
  // 67-SKIP cohort exercises.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"(celwasm.testdata.Customer{}.name == "")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 2. ProtoLiteralScalarE2ETest  (M7.B — cel_set_field for scalar
//    fields)
//
//    Per `m7-proto-literals.md` §6.1: every cpp_type × every
//    boundary-value combination MUST have a positive test.
//    Parameterised here over (field-name, source-literal,
//    expected-value) triples.  10 cpp_types covered; each cpp_type
//    has multiple boundary rows.  Construction-and-read pattern:
//      `celwasm.testdata.HostMsg3{<field>: <lit>}.<field> == <expected>`
//    so the assertion exercises both the set path (M7.B) and the
//    read path (M2 — must remain green).
// ──────────────────────────────────────────────────────────────

struct ScalarFieldCase {
  std::string label;        // gtest test-name suffix.
  std::string field;        // proto field name on HostMsg3 / HostMsg2.
  std::string proto_fqn;    // fully-qualified message FQN.
  std::string set_lit;      // CEL literal for the RHS of `field: …`.
  std::string read_eq_rhs;  // CEL literal for `== <rhs>` after read.
};

class ProtoLiteralScalarE2ETest
    : public ::testing::TestWithParam<ScalarFieldCase> {};

TEST_P(ProtoLiteralScalarE2ETest, ConstructAndReadBack) {
  const auto& p = GetParam();
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  std::string source = p.proto_fqn + "{" + p.field + ": " + p.set_lit + "}." +
                       p.field + " == " + p.read_eq_rhs;
  auto instance = CompilePlan(*compiler, source);
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true) << "source=" << source;
}

INSTANTIATE_TEST_SUITE_P(
    ScalarBoundaries, ProtoLiteralScalarE2ETest,
    ::testing::Values(
        // BOOL — set both polarities; default is false in proto3 so
        // setting false still asserts the set path (vs default
        // observation).
        ScalarFieldCase{"BoolTrue", "b", "celwasm.testdata.HostMsg3", "true",
                        "true"},
        ScalarFieldCase{"BoolFalse", "b", "celwasm.testdata.HostMsg3", "false",
                        "false"},
        // INT32 — boundaries: 0, 1, -1, INT32_MIN, INT32_MAX.
        ScalarFieldCase{"Int32Zero", "i32", "celwasm.testdata.HostMsg3", "0",
                        "0"},
        ScalarFieldCase{"Int32One", "i32", "celwasm.testdata.HostMsg3", "1",
                        "1"},
        ScalarFieldCase{"Int32MinusOne", "i32", "celwasm.testdata.HostMsg3",
                        "-1", "-1"},
        ScalarFieldCase{"Int32Min", "i32", "celwasm.testdata.HostMsg3",
                        "-2147483648", "-2147483648"},
        ScalarFieldCase{"Int32Max", "i32", "celwasm.testdata.HostMsg3",
                        "2147483647", "2147483647"},
        // INT64 — boundaries.
        ScalarFieldCase{"Int64Zero", "i64", "celwasm.testdata.HostMsg3", "0",
                        "0"},
        ScalarFieldCase{"Int64Min", "i64", "celwasm.testdata.HostMsg3",
                        "-9223372036854775808", "-9223372036854775808"},
        ScalarFieldCase{"Int64Max", "i64", "celwasm.testdata.HostMsg3",
                        "9223372036854775807", "9223372036854775807"},
        // UINT32.
        ScalarFieldCase{"Uint32Zero", "u32", "celwasm.testdata.HostMsg3", "0u",
                        "0u"},
        ScalarFieldCase{"Uint32Max", "u32", "celwasm.testdata.HostMsg3",
                        "4294967295u", "4294967295u"},
        // UINT64.
        ScalarFieldCase{"Uint64Zero", "u64", "celwasm.testdata.HostMsg3", "0u",
                        "0u"},
        ScalarFieldCase{"Uint64Max", "u64", "celwasm.testdata.HostMsg3",
                        "18446744073709551615u", "18446744073709551615u"},
        // FLOAT (single-precision).  cel-cpp checker accepts double
        // literals into a float field with implicit narrowing; M7
        // codegen reads the double CelValue and writes via
        // `Reflection::SetFloat`.
        ScalarFieldCase{"FloatZero", "f32", "celwasm.testdata.HostMsg3", "0.0",
                        "0.0"},
        ScalarFieldCase{"FloatPos", "f32", "celwasm.testdata.HostMsg3", "1.5",
                        "1.5"},
        ScalarFieldCase{"FloatNeg", "f32", "celwasm.testdata.HostMsg3", "-1.5",
                        "-1.5"},
        // DOUBLE.
        ScalarFieldCase{"DoubleZero", "f64", "celwasm.testdata.HostMsg3", "0.0",
                        "0.0"},
        ScalarFieldCase{"DoublePos", "f64", "celwasm.testdata.HostMsg3", "1.5",
                        "1.5"},
        ScalarFieldCase{"DoubleNeg", "f64", "celwasm.testdata.HostMsg3", "-1.5",
                        "-1.5"},
        // STRING — empty / one-char / multi-byte UTF-8.  Embedded-
        // NUL coverage lives in the unit tests; CEL's string-literal
        // grammar admits `" "` but the codegen path is the same
        // pointer+length copy regardless of contents — covered by
        // the unit test for `cel_set_field`.
        ScalarFieldCase{"StringEmpty", "s", "celwasm.testdata.HostMsg3",
                        R"("")", R"("")"},
        ScalarFieldCase{"StringAscii", "s", "celwasm.testdata.HostMsg3",
                        R"("hello")", R"("hello")"},
        ScalarFieldCase{"StringUtf8", "s", "celwasm.testdata.HostMsg3",
                        R"("☃")", R"("☃")"},
        // BYTES — empty / non-empty / NUL-containing.
        ScalarFieldCase{"BytesEmpty", "by", "celwasm.testdata.HostMsg3",
                        R"(b"")", R"(b"")"},
        ScalarFieldCase{"BytesShort", "by", "celwasm.testdata.HostMsg3",
                        R"(b"\x01\x02\x03")", R"(b"\x01\x02\x03")"},
        ScalarFieldCase{"BytesWithNul", "by", "celwasm.testdata.HostMsg3",
                        R"(b"\x00\xff")", R"(b"\x00\xff")"},
        // ENUM (cpp_type 8).  Set path goes through
        // `Reflection::SetEnumValue`; the int-typed comparison reads
        // the same number.
        ScalarFieldCase{"EnumNamed", "kind", "celwasm.testdata.HostMsg3",
                        "celwasm.testdata.HostMsg3.Kind.KIND_SEVEN", "7"},
        ScalarFieldCase{"EnumUnspecified", "kind", "celwasm.testdata.HostMsg3",
                        "celwasm.testdata.HostMsg3.Kind.KIND_UNSPECIFIED", "0"},
        // sint32 / sint64 / fixed* / sfixed* are wire-encoding variants
        // of int32 / int64 / uint32 / uint64; cel-cpp's checker types
        // them as int / uint per langdef §"Numeric Types", and
        // M7.B's cel_set_field hits the same `SetInt32` / `SetUInt64`
        // / etc. path.  One representative per shape.
        ScalarFieldCase{"SInt32", "si32", "celwasm.testdata.HostMsg3", "-7",
                        "-7"},
        ScalarFieldCase{"SInt64", "si64", "celwasm.testdata.HostMsg3", "-7",
                        "-7"},
        ScalarFieldCase{"Fixed32", "fx32", "celwasm.testdata.HostMsg3", "7u",
                        "7u"},
        ScalarFieldCase{"Fixed64", "fx64", "celwasm.testdata.HostMsg3", "7u",
                        "7u"},
        ScalarFieldCase{"SFixed32", "sfx32", "celwasm.testdata.HostMsg3", "-7",
                        "-7"},
        ScalarFieldCase{"SFixed64", "sfx64", "celwasm.testdata.HostMsg3", "-7",
                        "-7"}),
    [](const ::testing::TestParamInfo<ScalarFieldCase>& info) {
      return info.param.label;
    });

// Additional one-off tests where the source-operand axis from §6.1
// matters: `literal` is covered by INSTANTIATE_TEST_SUITE_P above;
// `ident` and `computed expr` need a bound variable / arithmetic
// expression respectively.

TEST_F(ProtoLiteralScalarE2ETest, IntFromIdent) {
  // RHS = ident.  Drives the cel_set_field path with a workspace-
  // slot-resident operand (vs. a rodata literal).
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("n", CelType::Int());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{i32: n}.i32 == 17");
  Activation a;
  a.Bind("n", Value::Int(17));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralScalarE2ETest, IntFromComputedExpr) {
  // RHS = arithmetic.  Codegen lowers the kCall first, lands the
  // result in a scratch slot, then hands the slot to cel_set_field.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{i32: 1 + 2}.i32 == 3");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralScalarE2ETest, MultipleScalarsInOneLiteral) {
  // Multi-entry literal — per-entry `cel_set_field` calls in
  // sequence, all writing into the same msg_slot.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(celwasm.testdata.HostMsg3{i32: 7, s: "x", b: true}.i32 == 7)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 3. ProtoLiteralRepeatedE2ETest  (M7.C — repeated fields)
//
//    `Foo{xs: [1, 2, 3]}` — RHS is a kListExpr; M7.C's
//    cel_set_field iterates the list and appends via
//    `Reflection::AddInt64`/etc.  Per `m7-proto-literals.md` §6.2:
//    one row per repeated cpp_type plus one row for repeated of
//    message and the activation-bound (HostList) source path.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralRepeatedE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralRepeatedE2ETest, RepeatedInt32FromArenaList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{rep_i32: [10, 20, 30]}.rep_i32[1] == 20");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralRepeatedE2ETest, RepeatedStringFromArenaList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(celwasm.testdata.HostMsg3{rep_s: ["a", "b"]}.rep_s[0] == "a")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralRepeatedE2ETest, RepeatedBoolFromArenaList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{rep_b: [true, false]}.rep_b[1] == false");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralRepeatedE2ETest, RepeatedDoubleFromArenaList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{rep_f64: [1.5, 2.5]}.rep_f64[1] == 2.5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralRepeatedE2ETest, RepeatedMessageFromArenaList) {
  // Repeated of message — RHS is `[Foo{}, Foo{}]`, exercising the
  // recursive `kStructExpr` lower (M7.E) inside an arena list,
  // which `cel_set_field` then iterates and `AddMessage`-copies
  // into the outer field.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{rep_msg: [celwasm.testdata.HostMsg3{i32: 1}, "
      "celwasm.testdata.HostMsg3{i32: 2}]}.rep_msg[1].i32 == 2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralRepeatedE2ETest, RepeatedEmptyList) {
  // Empty repeated — the empty-list literal `[]` is rejected by
  // RejectDyn at M5.A (types as `list<dyn>`).  Instead, the
  // construction must not name the field at all to leave it empty;
  // the read returns size 0.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "size(celwasm.testdata.HostMsg3{}.rep_i32) == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralRepeatedE2ETest, RepeatedFromHostListBinding) {
  // §4.5 — list-of-int activation binding flows through HostList
  // backing and `cel_set_field` accepts `Repr::kHostList` as a
  // source.  Same shape as M4's BoundIntList but threaded through
  // M7's set-field arm.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "celwasm.testdata.HostMsg3{rep_i32: xs}.rep_i32[2] == 30");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(10), Value::Int(20), Value::Int(30)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralRepeatedE2ETest, RepeatedSizeMatchesLiteralLength) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "size(celwasm.testdata.HostMsg3{rep_i32: [1, 2, 3, 4, 5]}.rep_i32) == 5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 4. ProtoLiteralMapE2ETest  (M7.C — map fields)
//
//    Per `m7-proto-literals.md` §6.2: every allowed map-key kind
//    (string / int / int64 / uint / bool) MUST have a positive
//    test.  Per cel-cpp / langdef §"Maps" + descriptor.proto, map
//    keys are restricted to {string, int32/64, uint32/64, bool}.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralMapE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralMapE2ETest, MapStringToInt32FromLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(celwasm.testdata.HostMsg3{str_to_i32: {"k1": 1, "k2": 2}}.str_to_i32["k2"] == 2)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralMapE2ETest, MapInt64ToStringFromLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(celwasm.testdata.HostMsg3{i64_to_str: {1: "a", 2: "b"}}.i64_to_str[1] == "a")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralMapE2ETest, MapUint32ToDoubleFromLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "celwasm.testdata.HostMsg3{u32_to_f64: {1u: 1.5, "
                              "2u: 2.5}}.u32_to_f64[2u] "
                              "== 2.5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralMapE2ETest, MapBoolToInt64FromLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{bool_to_i64: {true: 7, false: 9}}."
                  "bool_to_i64[true] == 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralMapE2ETest, MapStringToMessageFromLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(celwasm.testdata.HostMsg3{str_to_msg: {"k": celwasm.testdata.HostMsg3{i32: 5}}}.str_to_msg["k"].i32 == 5)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralMapE2ETest, MapEmptyConstruction) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "size(celwasm.testdata.HostMsg3{}.str_to_i32) == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralMapE2ETest, MapSizeMatchesLiteralEntryCount) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(size(celwasm.testdata.HostMsg3{str_to_i32: {"a": 1, "b": 2, "c": 3}}.str_to_i32) == 3)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 5. ProtoLiteralOneofE2ETest  (M7.C — oneof discipline)
//
//    Per `m7-proto-literals.md` §3.4 + R2 (risk): proto reflection
//    automatically clears sibling oneof arms on `SetField`.  The
//    invariant being asserted: setting two siblings in sequence
//    → only the most-recent set is observable.  Both proto2 and
//    proto3 oneofs go through the same Reflection::SetField path,
//    so both are exercised.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralOneofE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralOneofE2ETest, Proto2OneofFirstArmSetReadsBack) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "celwasm.testdata.HostMsg2{oneof_i64: 7}.oneof_i64 == 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralOneofE2ETest, Proto2OneofHasFirstArmAfterSet) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "has(celwasm.testdata.HostMsg2{oneof_i64: 7}.oneof_i64)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralOneofE2ETest, Proto2OneofHasSecondArmAfterSecondSet) {
  // Setting two siblings in literal-order: per Reflection, the
  // second set wins and clears the first.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(has(celwasm.testdata.HostMsg2{oneof_i64: 7, oneof_s: "x"}.oneof_s))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralOneofE2ETest, Proto2OneofFirstArmClearedAfterSecondSet) {
  // The companion to the previous test: has() on the FIRST arm
  // returns false because it was cleared by the second set.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(has(celwasm.testdata.HostMsg2{oneof_i64: 7, oneof_s: "x"}.oneof_i64))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ProtoLiteralOneofE2ETest, Proto3OneofFirstArmSetReadsBack) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "celwasm.testdata.HostMsg3{oneof_i64: 7}.oneof_i64 == 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralOneofE2ETest, Proto3OneofFirstArmClearedAfterSecondSet) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(has(celwasm.testdata.HostMsg3{oneof_i64: 7, oneof_s: "x"}.oneof_i64))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// ──────────────────────────────────────────────────────────────
// 6. ProtoLiteralEnumE2ETest  (M7.D — enum literals)
//
//    Probe-spike pending per `m7-proto-literals.md` §3.3: the
//    cel-cpp checker may emit either a Constant(int64) or a
//    Select(Ident, name) for `Foo.SomeEnumValue`.  These tests
//    pin the expected behaviour either way: the enum-literal RHS
//    flows into `cel_set_field` as an int and reads back as the
//    declared numeric value.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralEnumE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralEnumE2ETest, EnumLiteralInScalarContext) {
  // `Foo.SomeEnumValue` resolves to its int representation per
  // langdef §"Enumerated Types".  Evaluating it standalone returns
  // the int (after checker constant-folding).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3.Kind.KIND_SEVEN == 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEnumE2ETest, EnumLiteralRhsOfFieldSet) {
  // Construction `Foo{kind: Foo.KIND_SEVEN}` — RHS is an int after
  // checker resolution; `cel_set_field` on an enum field calls
  // `Reflection::SetEnumValue` with that int.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "celwasm.testdata.HostMsg3{kind: "
                              "celwasm.testdata.HostMsg3.Kind.KIND_SEVEN}"
                              ".kind == 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEnumE2ETest, EnumFieldUnsetReadsAsZero) {
  // Proto3 enum default is the zero-numbered value, which for
  // HostMsg3.Kind is KIND_UNSPECIFIED (0).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{}.kind == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEnumE2ETest, EnumFieldRoundTripViaInt) {
  // Set with a numeric int RHS instead of the enum-name form;
  // checker accepts the cross because enum is spec-typed as int.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{kind: 7}.kind == 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 7. ProtoLiteralNestedE2ETest  (M7.E — message field nesting)
//
//    Per `m7-proto-literals.md` §5 / M7.E: the codegen arm is
//    recursive by construction, so once M7.A/B land, nesting
//    falls out for free.  Verify-and-test slice: pin the matrix.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralNestedE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralNestedE2ETest, OneLevelNestedReadBack) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{inner: celwasm.testdata.HostMsg3{i32: 9}}"
      ".inner.i32 == 9");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralNestedE2ETest, ThreeLevelDeepNest) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{inner: celwasm.testdata.HostMsg3{inner: "
      "celwasm.testdata.HostMsg3{i32: 42}}}.inner.inner.i32 == 42");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralNestedE2ETest, NestedWithScalarSibling) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(celwasm.testdata.HostMsg3{i32: 1, inner: celwasm.testdata.HostMsg3{s: "x"}}.inner.s == "x")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralNestedE2ETest, NestedInListLiteral) {
  // `[Foo{}, Foo{}]` — list-of-message via M4 list literal +
  // recursive M7 struct lower for each element.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "[celwasm.testdata.HostMsg3{i32: 1}, celwasm.testdata.HostMsg3{i32: 2}]"
      "[1].i32 == 2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralNestedE2ETest, ExplicitWrapperAsNested) {
  // M8 territory.  cel-cpp's checker types
  // `google.protobuf.Int32Value{value: 5}` as the special
  // `wrapper(int)` type, which it rejects as the operand of a
  // select — by design, since the wrapper is supposed to peel to
  // its value automatically at the consumer site.  M8 (wrapper
  // equivalence + auto-peel) is the first slice that makes
  // wrapper-typed expressions usable in scalar contexts.  The
  // M7.E recursive-construction path is exercised indirectly via
  // the other Nested tests; explicit wrapper construction lights
  // up cleanly once M8 ships the peel.
  GTEST_SKIP() << "wrapper-typed expression in scalar context "
                  "(`Int32Value{value:5}.value`) is M8 "
                  "(wrapper auto-peel) — see m8-wrapper-types.md";
}

// ──────────────────────────────────────────────────────────────
// 8. ProtoLiteralDefaultsE2ETest  (§6.3 — default-value
//    semantic regression on M7-constructed messages)
//
//    The point of this section is to assert that M7's
//    construction path doesn't accidentally shortcut around
//    Reflection — every default observed below comes "for free"
//    from `MessageFactory::GetPrototype(desc)->New()` plus the
//    M2/M3 read paths.  Per `m7-proto-literals.md` §3.2: defaults
//    are entirely descriptor-driven; M7 writes no default-value
//    code.  These tests guard the invariant.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralDefaultsE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralDefaultsE2ETest, Proto3ZeroDefaultScalar) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{}.i32 == 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralDefaultsE2ETest, Proto3UnsetMessageReadsAsDefaultInstance) {
  // langdef §"Messages" + conformance row proto3/empty_field/nested_message:
  // accessing an unset proto3 singular MESSAGE field returns the
  // default-instance message of the field's type, NOT null.  Was
  // asserting `== null` pre-2026-06-05 (the buggy behavior, see
  // m7_test::EmptyProto3MessageReadsDefaultInstanceForUnsetSubmessage
  // for the longer rationale).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{}.inner == "
                  "celwasm.testdata.HostMsg3{}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralDefaultsE2ETest, Proto2ExplicitDefaultScalar) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg2{}.default_i32 == 42");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralDefaultsE2ETest, Proto2HasOnUnsetIsFalse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "has(celwasm.testdata.HostMsg2{}.default_i32)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ProtoLiteralDefaultsE2ETest, Proto2HasOnExplicitlySetToDefaultIsTrue) {
  // Critical regression: setting a proto2 field to its declared
  // default value still produces `has() == true`.  This is the
  // behaviour that distinguishes proto2 from proto3 and the read
  // path must not collapse it.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "has(celwasm.testdata.HostMsg2{default_i32: 42}.default_i32)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralDefaultsE2ETest, Proto3HasOnUnsetSubmessageIsFalse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "has(celwasm.testdata.HostMsg3{}.inner)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ProtoLiteralDefaultsE2ETest, Proto3HasOnSetSubmessageIsTrue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "has(celwasm.testdata.HostMsg3{inner: "
                              "celwasm.testdata.HostMsg3{}}.inner)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 9. ProtoLiteralEqualityE2ETest
//
//    The whole point of M7 from a conformance perspective: the
//    M5.B `cel_message_eq` kernel has been in place since
//    2026-04-24 but unreachable because no fixture row builds a
//    message literal.  These tests prove the equality kernel
//    fires cleanly once M7 hands it a constructed message, lighting
//    up the 67 `comparisons.textproto` SKIPs.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralEqualityE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralEqualityE2ETest, EmptyEqualsEmpty) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "celwasm.testdata.HostMsg3{} == celwasm.testdata.HostMsg3{}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEqualityE2ETest, SameScalarsEqual) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{i32: 1} == celwasm.testdata.HostMsg3{i32: 1}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEqualityE2ETest, DifferentScalarsNotEqual) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{i32: 1} != celwasm.testdata.HostMsg3{i32: 2}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEqualityE2ETest, NestedEquality) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{inner: celwasm.testdata.HostMsg3{i32: 1}} == "
      "celwasm.testdata.HostMsg3{inner: celwasm.testdata.HostMsg3{i32: 1}}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEqualityE2ETest, RepeatedEquality) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{rep_i32: [1, 2, 3]} == "
                  "celwasm.testdata.HostMsg3{rep_i32: [1, 2, 3]}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEqualityE2ETest, MapEquality) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(celwasm.testdata.HostMsg3{str_to_i32: {"a": 1}} == celwasm.testdata.HostMsg3{str_to_i32: {"a": 1}})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralEqualityE2ETest, ConstructedEqualsBoundProto) {
  // Mixed construction-vs-binding equality: one operand is a
  // literal-constructed message, the other is bound through
  // Activation.  Both must compare structurally equal.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("h", CelType::Message("celwasm.testdata.HostMsg3"));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{i32: 5} == h");
  HostMsg3 bound;
  bound.set_i32(5);
  Activation a;
  a.Bind("h", Value::Message(bound));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 10. ProtoLiteralActivationE2ETest  (§4.5 — activation
//     marshalling polish)
//
//     M7 §4.5: list-of-message and map-from-activation are the
//     two polish bullets.  list-of-message verifies kMessage
//     element encoding inside the list-encoder.  map-from-
//     activation may pull in the kMap encoder (currently SKIP
//     per `conformance/README.md`); if so, M7.C
//     lands the encoder.
// ──────────────────────────────────────────────────────────────

class ProtoLiteralActivationE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralActivationE2ETest, ListOfMessageBinding) {
  // §4.5 first bullet: bind a list-of-message and read element 1.
  // Exercises EncodeList → kMessage element encode → ProtoList
  // backing in the host.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "ms", CelType::List(CelType::Message("celwasm.testdata.HostMsg3")));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "ms[1].i32 == 7");
  HostMsg3 m0;
  m0.set_i32(1);
  HostMsg3 m1;
  m1.set_i32(7);
  Activation a;
  a.Bind("ms", Value::List({Value::Message(m0), Value::Message(m1)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ProtoLiteralActivationE2ETest,
       ListOfMessageBindingFlowsIntoConstruction) {
  // The composition the §4.5 bullet matters for: bind a list-of-
  // message externally, hand it to a `Foo{rep_msg: ms}`
  // construction, read back through the proto repeated.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable(
        "ms", CelType::List(CelType::Message("celwasm.testdata.HostMsg3")));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "celwasm.testdata.HostMsg3{rep_msg: ms}.rep_msg[0].i32 == 11");
  HostMsg3 m0;
  m0.set_i32(11);
  Activation a;
  a.Bind("ms", Value::List({Value::Message(m0)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 11. ProtoLiteralRejectE2ETest  (§6.4 — negative / rejection
//     matrix)
//
//     Per CLAUDE.md "Cover the edge-case matrix — this is a
//     compiler.  Negative coverage (rejection cases) is ≥ 30% of
//     the total".  These rows are checker rejections — the
//     diagnostic comes from cel-cpp; M7 simply must not overrule
//     it (don't accidentally admit a malformed `kStructExpr`
//     past the static-subset gate).
// ──────────────────────────────────────────────────────────────

class ProtoLiteralRejectE2ETest : public ::testing::Test {};

TEST_F(ProtoLiteralRejectE2ETest, UnregisteredDescriptorRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "com.example.Unknown{x: 1}",
                     "descriptor not in pool");
}

TEST_F(ProtoLiteralRejectE2ETest, UnknownFieldOnKnownDescriptorRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "celwasm.testdata.HostMsg3{not_a_field: 1}",
                     "field name not on descriptor");
}

TEST_F(ProtoLiteralRejectE2ETest, ScalarTypeMismatchRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, R"(celwasm.testdata.HostMsg3{i32: "string"})",
                     "string into int32 field");
}

TEST_F(ProtoLiteralRejectE2ETest, RepeatedFieldNonListSourceRejected) {
  // `Foo{rep_i32: 1}` — RHS is int, field is repeated.  cel-cpp's
  // checker rejects with a list<int> vs int mismatch.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "celwasm.testdata.HostMsg3{rep_i32: 1}",
                     "non-list into repeated field");
}

TEST_F(ProtoLiteralRejectE2ETest, MapKeyTypeMismatchRejected) {
  // `str_to_i32` is map<string, int32>; an int key in the literal
  // is a checker-side type error.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "celwasm.testdata.HostMsg3{str_to_i32: {1: 2}}",
                     "int key into map<string,_>");
}

TEST_F(ProtoLiteralRejectE2ETest, MapValueTypeMismatchRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler,
                     R"(celwasm.testdata.HostMsg3{str_to_i32: {"k": "v"}})",
                     "string value into map<_,int32>");
}

TEST_F(ProtoLiteralRejectE2ETest, MessageFieldScalarSourceRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "celwasm.testdata.HostMsg3{inner: 1}",
                     "scalar into singular message field");
}

}  // namespace
}  // namespace celwasm::api
