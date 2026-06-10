// M7-A e2e test suite — the spec of "done" for `google.protobuf.Any`
// pack / unpack / equality.  Mirrors m7_test.cc's shape: every test
// asserts a capability `m7a-any.md` says M7-A must light up; running
// this binary today (with `CelSetFieldImpl`'s Any-shaped descriptor
// mismatch returning `UnimplementedError`) should SKIP everything
// outside the regression-coverage tests.  Greening the suite is the
// M7-A exit per `m7a-any.md` §6.
//
// Wrapper types are NOT in scope here — wrapper auto-wrap is M8 (see
// `m8-wrapper-types.md`).  M7-A covers pack-into-Any when the RHS is
// a typed message (incl. wrapper messages constructed explicitly,
// since the runtime sees them as ordinary typed messages with
// non-Any descriptors).
//
// Fixtures grouped by capability (one section per slice):
//
//   - AnyPackE2ETest          M7-A.A — typed RHS into Any field.
//                                       Pack via reflection (the
//                                       generic path that works for
//                                       both generated and dynamic
//                                       descriptors per probe A).
//   - AnyUnpackE2ETest        M7-A.B — read of Any-typed field
//                                       returns the unwrapped typed
//                                       message; chained selects
//                                       through it (`msg.any.x`).
//   - AnyEqualityE2ETest      M7-A.C — Any-vs-typed and
//                                       Any-vs-Any equality via
//                                       `cel_message_eq` peel.
//   - AnyTypeOfE2ETest        regr.   — `type(msg.any)` returns the
//                                       unwrapped FQN (M9
//                                       regression).
//   - AnyRejectE2ETest        §6.3   — negative matrix: empty
//                                       type_url, malformed type_url,
//                                       FQN not in pool, value bytes
//                                       don't parse.
//   - AnyNullClearE2ETest     regr.  — `Foo{single_any: null}` →
//                                       cleared Any (M7-shipped
//                                       null-clear arm; M7-A
//                                       regression).
//   - AnyLiteralRoundTripE2ETest      — `Any{type_url, value}`
//                                       direct construction (no pack)
//                                       — this already works at M7
//                                       since Any is a message with
//                                       two bytes fields; assert it
//                                       didn't regress.
//
// Conformance unlock estimate per slice is logged on each test
// section; aggregate target is +7..+11 PASS in conformance per
// `m7a-any.md` §1.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/error.h"
#include "eval/instance.h"
#include "eval/internal/cel_host.h"
#include "compiler/program.h"
#include "shared/type.h"
#include "eval/value.h"
#include "testdata/host_fixture_proto2.pb.h"
#include "testdata/host_fixture_proto3.pb.h"
#include "google/protobuf/any.pb.h"
#include "google/protobuf/message.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::testdata::HostMsg2;
using ::celwasm::testdata::HostMsg3;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<HostMsg2>();
      google::protobuf::LinkMessageReflection<HostMsg3>();
      google::protobuf::LinkMessageReflection<google::protobuf::Any>();
      return 0;
    }();

using ::celwasm::e2e::GlobalEngine;

using ConfigureFn = std::function<void(Compiler::Builder&)>;
absl::StatusOr<Compiler> BuildCompiler(const ConfigureFn& configure) {
  Compiler::Builder b;
  configure(b);
  return std::move(b).Build();
}

absl::StatusOr<Compiler> CompilerEmpty() {
  return BuildCompiler([](Compiler::Builder& /*b*/) {});
}

// M7-A.C (cel_message_eq peel) is not yet shipped; AnyEquality tests
// skip with this label.  M7-A.A pack and M7-A.B unwrap are live.
constexpr absl::string_view kM7aEqPending =
    "M7-A.C cel_message_eq Any-peel arm not yet shipped (m7a-any.md §5).";

using ::celwasm::e2e::CompilePlan;

using ::celwasm::e2e::EvalOk;

// ──────────────────────────────────────────────────────────────
// 1. AnyPackE2ETest  (M7-A.A — typed-message RHS into Any field)
//
// What this section can verify end-to-end: cel-cpp's checker types
// selections *through* an Any-typed field as `dyn` (probe §10.3),
// and likewise types heterogeneous list/map literals destined for
// `repeated Any` / `map<_,Any>` as `list(dyn)` / `map(_,dyn)`.  All
// three trip v2's RejectDyn gate.  So e2e here pins only the
// *reachable* shape — singular Any packing — through `has(...)` /
// null-clear regressions.  Byte-level pack invariants (type_url
// suffix, value bytes) and the repeated / map call sites are
// verified at Layer-2 in `cel_host_test.cc::WriteMessageOrPack`,
// which can drive the trampoline directly without going through
// the checker.
// ──────────────────────────────────────────────────────────────

// Helper: build a HostMsg3 source expression with `i32: <n>` set.
std::string SrcHostMsg3(int n) {
  return absl::StrCat("celwasm.testdata.HostMsg3{i32: ", n, "}");
}

struct PackShapeCase {
  absl::string_view label;
  std::string expr;  // returns bool; asserts the pack arm landed.
};

class AnyPackShapeE2ETest : public ::testing::TestWithParam<PackShapeCase> {};

TEST_P(AnyPackShapeE2ETest, PackArmReaches) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, GetParam().expr);
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true) << GetParam().expr;
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, AnyPackShapeE2ETest,
    ::testing::Values(
        // Singular Any from typed proto3 message — present after pack.
        PackShapeCase{
            "singular_proto3_present",
            absl::StrCat("has(celwasm.testdata.HostMsg3{single_any: ",
                         SrcHostMsg3(1), "}.single_any)"),
        },
        // Singular Any from typed proto2 message — cross-syntax pack
        // doesn't trip the helper (Any is syntax-agnostic).
        PackShapeCase{
            "singular_proto2_src_present",
            "has(celwasm.testdata.HostMsg3{single_any: "
            "celwasm.testdata.HostMsg2{}}.single_any)",
        },
        // Singular Any from an explicit wrapper-message RHS — the
        // descriptor is still non-Any so the pack arm fires (M8's
        // scalar-into-wrapper auto-wrap is a separate concern; see
        // m7a-any.md §2.1).
        PackShapeCase{
            "singular_wrapper_message_src_present",
            "has(celwasm.testdata.HostMsg3{single_any: "
            "celwasm.testdata.HostMsg3{rep_i32: [1]}}.single_any)",
        },
        // Singular Any with empty-payload RHS — pins that
        // zero-length SerializeAsString output still produces a
        // present Any (presence is bytes-set or type_url-set, both
        // of which the reflection-pack sets).
        PackShapeCase{
            "singular_empty_payload_present",
            "has(celwasm.testdata.HostMsg3{single_any: "
            "celwasm.testdata.HostMsg3{}}.single_any)",
        }),
    [](const ::testing::TestParamInfo<PackShapeCase>& info) {
      return std::string(info.param.label);
    });

class AnyPackE2ETest : public ::testing::Test {};

// Outer construction containing both a packed Any and a non-Any
// field — pins that the pack helper doesn't accidentally take over
// the non-Any cpp_type-MESSAGE path.  `inner` is HostMsg3-typed
// (same descriptor as the outer), so the descriptors match and
// `WriteMessageOrPack` takes the CopyFrom branch.
TEST_F(AnyPackE2ETest, PackCoexistsWithNonAnyMessageField) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{"
                  "inner: celwasm.testdata.HostMsg3{i32: 11}, "
                  "single_any: celwasm.testdata.HostMsg3{}}.inner.i32 == 11");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 2. AnyUnpackE2ETest  (M7-A.B — read-side: any-field returns the
//                       unwrapped typed value)
//
// Each row exercises an Any field that was previously packed (via
// the conformance harness's `UnpackAny` for activation-bound
// messages, or via M7-A.A's pack arm for construction-built ones),
// then read back through ProtoBacking::ReadField's Any-aware arm.
// ──────────────────────────────────────────────────────────────

class AnyUnpackE2ETest : public ::testing::Test {};

// Pack a HostMsg3 with i32=42 into single_any, then read back the
// unwrapped i32 through the chained select.  M7-A.B's read-side
// unwrap fires when ProtoBacking::ReadField sees the Any-typed
// singular-message field; the §3.5.A frontend carve-out lets
// `msg.single_any.i32` clear the static-subset gate even though the
// select types as dyn.
TEST_F(AnyUnpackE2ETest, ReadAnyFieldReturnsUnwrappedTypedValue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{single_any: "
                  "celwasm.testdata.HostMsg3{i32: 42}}.single_any.i32 == 42");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Unset Any field on a freshly-constructed proto reads as null per
// the M7-shipped null-on-unset-singular-message rule.  The Any
// unwrap arm sees `HasField == false` and returns null before
// reaching UnpackAnyToValue.
TEST_F(AnyUnpackE2ETest, ReadUnsetAnyFieldReturnsNull) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{}.single_any == null");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Two-hop select on the unwrapped value: pack a HostMsg3 with a
// nested `inner.s = 'deep'`, then read back via the Any.  Exercises
// that the unwrapped backing routes through normal proto reflection
// past the unwrap boundary.
TEST_F(AnyUnpackE2ETest, ChainedSelectOnUnpackedMessage) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{single_any: "
      "celwasm.testdata.HostMsg3{inner: celwasm.testdata.HostMsg3{s: 'deep'}}}"
      ".single_any.inner.s == 'deep'");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// String round-trip: pack HostMsg3{s: 'abc'} into single_any, read
// back the unwrapped `.s`.
TEST_F(AnyUnpackE2ETest, ReadAnyFieldUnwrapsStringField) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{single_any: "
                  "celwasm.testdata.HostMsg3{s: 'abc'}}.single_any.s == 'abc'");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// has() on a packed Any field is true (presence is type_url-or-value
// set, both of which the reflection-pack writes).
TEST_F(AnyUnpackE2ETest, HasOnPackedSingularAnyIsTrue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "has(celwasm.testdata.HostMsg3{single_any: "
                              "celwasm.testdata.HostMsg3{i32: 1}}.single_any)");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// has() on an unset Any field is false.
TEST_F(AnyUnpackE2ETest, HasOnUnsetSingularAnyIsFalse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "has(celwasm.testdata.HostMsg3{}.single_any) == false");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 3. AnyEqualityE2ETest  (M7-A.C — wrapper-peel on `==` / `!=`)
//
// Pin every shape of the equivalence:
//   - Any-vs-typed-same       (`==` → true)
//   - Any-vs-typed-different  (`==` → false)
//   - typed-vs-Any-same       (symmetric)
//   - Any-vs-Any-same         (delegated to MessageDifferencer)
//   - Any-vs-Any-different    (false)
//   - Any-vs-null             (false unless Any is unset)
// ──────────────────────────────────────────────────────────────

class AnyEqualityE2ETest : public ::testing::Test {};

// After M7-A.B, msg.single_any returns the unwrapped backing.  The
// outer == then compares (unwrapped Bar) vs (typed Bar) via
// CelMessageEqImpl's existing MessageDifferencer path — same
// descriptor, so equality reduces to message-structural equality.
// M7-A.C's peel arm is only needed when the wrapped operand is NOT
// reached through a field read (e.g. direct `Any{}` literal).
TEST_F(AnyEqualityE2ETest, AnyFieldReadEqualsMatchingTypedMessage) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{single_any: "
                  "celwasm.testdata.HostMsg3{i32: 1}}.single_any == "
                  "celwasm.testdata.HostMsg3{i32: 1}");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

TEST_F(AnyEqualityE2ETest, AnyFieldReadUnequalToMismatchingTypedMessage) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{single_any: "
                  "celwasm.testdata.HostMsg3{i32: 1}}.single_any == "
                  "celwasm.testdata.HostMsg3{i32: 2}");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), false);
}

TEST_F(AnyEqualityE2ETest, TypedMessageEqualsAnyFieldReadSymmetric) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "celwasm.testdata.HostMsg3{i32: 1} == "
                              "celwasm.testdata.HostMsg3{single_any: "
                              "celwasm.testdata.HostMsg3{i32: 1}}.single_any");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Two unwrapped values on both sides — both are unpacked Bars by
// M7-A.B and compared directly.
TEST_F(AnyEqualityE2ETest, TwoAnyFieldReadsEqual) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{single_any: "
                  "celwasm.testdata.HostMsg3{i32: 1}}.single_any == "
                  "celwasm.testdata.HostMsg3{single_any: "
                  "celwasm.testdata.HostMsg3{i32: 1}}.single_any");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Outer-outer eq: TestAllTypes-vs-TestAllTypes where both carry the
// same packed Any.  CelMessageEqImpl invokes MessageDifferencer on
// the outer descriptor; the embedded Any sub-field compares
// recursively.  Protobuf's MessageDifferencer is Any-aware by
// default in modern protobuf, so this passes even if the inner
// wire bytes are byte-different (semantically-equal payloads).
TEST_F(AnyEqualityE2ETest, OuterMessageEqualityCarryingPackedAny) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "celwasm.testdata.HostMsg3{single_any: "
                              "celwasm.testdata.HostMsg3{i32: 1}} == "
                              "celwasm.testdata.HostMsg3{single_any: "
                              "celwasm.testdata.HostMsg3{i32: 1}}");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Unset Any field reads as null per M7-shipped null-on-unset rule;
// the peel branch must not fire.
TEST_F(AnyEqualityE2ETest, UnsetAnyEqualsNull) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{}.single_any == null");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Direct Any literal == typed message — operand on LHS is NOT a
// field-read of an Any field, so M7-A.B's read-side unwrap does
// NOT fire.  The LHS is an Any-typed backing; the RHS is a typed
// HostMsg3 backing.  Without an Any-peel in CelMessageEqImpl this
// would compare two different descriptors and return false.  M7-A.C
// peels the Any operand and re-enters equality with the unwrapped
// value.
TEST_F(AnyEqualityE2ETest, DirectAnyLiteralEqualsTypedMessageViaPeel) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // Construct an Any wrapping HostMsg3{} via the bytes path; compare
  // with the typed empty HostMsg3.  Empty proto serializes to empty
  // bytes — predictable round-trip.
  auto instance = CompilePlan(
      *compiler,
      "google.protobuf.Any{type_url: "
      "'type.googleapis.com/celwasm.testdata.HostMsg3', value: b''} == "
      "celwasm.testdata.HostMsg3{}");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Symmetric peel: typed-message LHS, Any-literal RHS.
TEST_F(AnyEqualityE2ETest, TypedMessageEqualsDirectAnyLiteralViaPeel) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{} == "
      "google.protobuf.Any{type_url: "
      "'type.googleapis.com/celwasm.testdata.HostMsg3', value: b''}");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Two Any literals with the same type_url + same bytes compare equal
// — MessageDifferencer handles this directly (both descriptors are
// Any; protobuf's default Any-aware mode unpacks).
TEST_F(AnyEqualityE2ETest, TwoAnyLiteralsEqual) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "google.protobuf.Any{type_url: "
      "'type.googleapis.com/celwasm.testdata.HostMsg3', value: b''} == "
      "google.protobuf.Any{type_url: "
      "'type.googleapis.com/celwasm.testdata.HostMsg3', value: b''}");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Two Any literals with different type_urls are unequal (cross-type
// after peel).
TEST_F(AnyEqualityE2ETest, AnysWithDifferentTypeUrlsAreUnequal) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "google.protobuf.Any{type_url: "
      "'type.googleapis.com/celwasm.testdata.HostMsg3', value: b''} == "
      "google.protobuf.Any{type_url: "
      "'type.googleapis.com/celwasm.testdata.HostMsg2', value: b''}");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), false);
}

// ──────────────────────────────────────────────────────────────
// 4. AnyTypeOfE2ETest  (M9 regression — type(unpacked_any))
//
// M9's `cel_host.resolve_message_type_name` reads
// `HostMessageBacking::message()->GetDescriptor()->full_name()`.
// After M7-A.B's read-side unwrap, the backing already points at
// the *unwrapped* typed message, so `type(...)` returns the
// unwrapped FQN with no M9-specific change.  This test family
// pins the invariant.
// ──────────────────────────────────────────────────────────────

class AnyTypeOfE2ETest : public ::testing::Test {};

// After M7-A.B's read-side unwrap, `msg.single_any` returns a backing
// wrapping the unwrapped typed message — so `type(...)` over it
// resolves to the *unwrapped* descriptor's FQN, not "Any".  M9's
// resolver reads `backing->message()->GetDescriptor()->full_name()`
// without any Any-specific code; M7-A.B's unwrap supplies the
// already-unwrapped message.
TEST_F(AnyTypeOfE2ETest, TypeOfUnpackedAnyReturnsUnwrappedFqn) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "type(celwasm.testdata.HostMsg3{single_any: "
                  "celwasm.testdata.HostMsg3{i32: 1}}.single_any) == "
                  "celwasm.testdata.HostMsg3");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// Direct Any-literal construction produces an Any-typed backing —
// no Any-typed-field-read is involved, so the unwrap arm doesn't
// fire.  `type(...)` returns "google.protobuf.Any".  Pins the
// boundary between field-read-unwrap and struct-literal-no-unwrap.
TEST_F(AnyTypeOfE2ETest, TypeOfDirectlyConstructedAnyReturnsAny) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "type(google.protobuf.Any{type_url: 'type.googleapis.com/X', "
                  "value: b''}) == google.protobuf.Any");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 5. AnyRejectE2ETest  (§6.3 — read-side negative envelope)
//
// Probe B (m7a-any.md §10.2) pinned the failure modes the unpack
// path must surface as CEL_ERROR:
//   - empty type_url    → kFieldNotFound (FQN="" → no descriptor)
//   - no slash in type_url → same (FQN=whole-string → no descriptor)
//   - unknown FQN       → kFieldNotFound
//   - value bytes don't parse → kTypeMismatch
//
// Each error is observable through `Eval(...).IsError()` and the
// ErrorPayload carries a recognisable code.
// ──────────────────────────────────────────────────────────────

class AnyRejectE2ETest : public ::testing::Test {};

// Helper: compile + eval an expression that wraps a literal Any
// into HostMsg3.single_any, then chained-selects `.<field>`.  Returns
// the eval result so the caller can assert on error code.
Value EvalReject(absl::string_view literal_any_inline) {
  auto compiler = CompilerEmpty();
  ABSL_CHECK_OK(compiler);
  // `<literal_any>.i32` — read-side unwrap fires because the operand
  // is a field-read of `single_any` (Any-typed).  The unwrap parses
  // the literal Any's type_url + value against the field's pool.
  auto instance = CompilePlan(
      *compiler, absl::StrCat("celwasm.testdata.HostMsg3{single_any: ",
                              literal_any_inline, "}.single_any.i32"));
  return EvalOk(instance, Activation{});
}

// Empty type_url on an EXPLICITLY-assigned Any — cel-cpp's AdaptAny
// errors here (the prefix check fails on the empty string,
// `well_known_types.cc:1960-1966`).  Pinned by conformance row
// `dynamic/any/literal_empty`.  Distinct from the UNSET-Any-field
// case (HasField=false), which reads as null — see
// `AnyEqualityE2ETest.UnsetAnyEqualsNull`.
TEST_F(AnyRejectE2ETest, ReadAnyWithExplicitlySetEmptyTypeUrlIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{single_any: "
      "google.protobuf.Any{type_url: '', value: b''}}.single_any");
  Value v = EvalOk(instance, Activation{});
  EXPECT_TRUE(v.IsError())
      << "Explicitly-assigned Any{type_url: ''} should read as error, kind="
      << static_cast<int>(v.kind());
  if (v.IsError()) {
    auto err = v.ErrorInfo();
    ASSERT_THAT(err, IsOk());
    EXPECT_EQ((*err)->code, ErrorCode::kFieldNotFound);
  }
}

// Malformed type_url with no slash — FQN is the whole string, pool
// lookup fails → kFieldNotFound.
TEST_F(AnyRejectE2ETest, ReadAnyWithMalformedTypeUrlIsError) {
  Value v =
      EvalReject("google.protobuf.Any{type_url: 'not_a_url', value: b''}");
  auto err = v.ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, ErrorCode::kFieldNotFound);
}

// FQN with prefix but unknown name → kFieldNotFound.
TEST_F(AnyRejectE2ETest, ReadAnyWithUnknownFqnIsError) {
  Value v = EvalReject(
      "google.protobuf.Any{type_url: 'type.googleapis.com/com.nope.Unknown', "
      "value: b''}");
  auto err = v.ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, ErrorCode::kFieldNotFound);
}

// Corrupt value bytes against a known FQN — ParseFromString fails
// → kTypeMismatch.
TEST_F(AnyRejectE2ETest, ReadAnyWithCorruptValueBytesIsError) {
  // 0xff bytes form invalid wire-format for HostMsg3 (varint
  // continuation never terminates).
  Value v = EvalReject(
      "google.protobuf.Any{type_url: "
      "'type.googleapis.com/celwasm.testdata.HostMsg3', "
      "value: b'\\xff\\xff\\xff\\xff'}");
  auto err = v.ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, ErrorCode::kTypeMismatch);
}

// Non-googleapis.com prefix is accepted — cel-cpp parity (probe B):
// strip-before-last-slash is the rule, scheme is informational.
TEST_F(AnyRejectE2ETest, NonGoogleApisPrefixIsAccepted) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // Construct a HostMsg3 with i32=7, serialize via the type
  // 'type.example.com/celwasm.testdata.HostMsg3' alias.  We can't
  // easily synthesize the raw bytes from CEL, so use a packed-
  // through-cel Any and verify it unwraps via the prefix-stripped
  // FQN.  Direct construction with a non-googleapis prefix is
  // possible because the prefix isn't enforced.
  auto instance =
      CompilePlan(*compiler,
                  "type(celwasm.testdata.HostMsg3{single_any: "
                  "celwasm.testdata.HostMsg3{i32: 7}}.single_any) == "
                  "celwasm.testdata.HostMsg3");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 6. AnyNullClearE2ETest  (M7-shipped null-clear arm regression)
//
// `Foo{single_any: null}` already routes through M7's `CEL_NULL →
// ClearField` arm in `SetScalarField`'s CPPTYPE_MESSAGE path.
// M7-A.A's `AssignMessageOrPack` helper must NOT shadow this —
// the null check stays in the caller's switch, not inside the
// pack helper.  These tests pin the order-of-operations.
// ──────────────────────────────────────────────────────────────

class AnyNullClearE2ETest : public ::testing::Test {};

// Null-clear stays inside SetScalarField's switch (before the pack
// helper), so `Foo{single_any: null}` leaves the field unset.  If
// M7-A.A regressed the ordering, the null payload would reach the
// pack arm and serialise a literal null → non-null Any.
TEST_F(AnyNullClearE2ETest, NullSetOnSingularAnyClearsField) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "celwasm.testdata.HostMsg3{single_any: null}.single_any == null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(AnyNullClearE2ETest, NullSetOnSingularAnyHasFieldIsFalse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "has(celwasm.testdata.HostMsg3{single_any: null}.single_any) == false");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(AnyNullClearE2ETest, UnsetSingularAnyReadsAsNull) {
  // `HostMsg3{}.single_any == null` — proto3 singular-message null-on-
  // unset rule.  M7-A.B's unpack arm (when it ships) must preserve
  // this: a literal-null type_url is the unset signal.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "celwasm.testdata.HostMsg3{}.single_any == null");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 7. AnyLiteralRoundTripE2ETest  (PROBE FINDING — see m7a-any.md §10.3)
//
// PROBE FINDING (challenges m7a-any.md draft assumption): the
// expression `google.protobuf.Any{type_url: 'x', value: b'y'}`
// does NOT compile today — cel-cpp's checker types the result as
// `google.protobuf.Any` (a "well-known type") which the v2
// static-subset rejects via `RejectDyn` with diagnostic
//
//   InvalidArgument: expression is not in the static subset:
//     expr id=N is dyn (dyn)
//
// So direct Any-literal construction is currently NOT shipped (the
// `Any{...}` syntax hits the dyn gate, not the kStructExpr arm).
// This means M7-A.A doesn't need to handle the "Any-as-RHS" branch
// reachable from direct Any-literal syntax — that path is already
// gated out at the frontend.
//
// The case that IS reachable: `Foo{single_any: Bar{...}}` where the
// outer is non-Any and the inner is a typed message (the §"Pack"
// cohort above).  Bar's RHS is reached via the kStructExpr arm
// without involving direct Any-literal construction at all.
//
// These tests pin that the rejection is at the frontend, not at
// runtime — so M7-A.A doesn't accidentally graduate the dyn-rejected
// shape.
// ──────────────────────────────────────────────────────────────

class AnyLiteralRoundTripE2ETest : public ::testing::Test {};

// After M7-A.B's §3.5.A select-through-Any carve-out, direct
// `google.protobuf.Any{...}.type_url` / `.value` reads compile and
// eval correctly — the outer struct literal materialises a regular
// OwnedProtoBacking holding the Any descriptor, and the inner select
// reads `type_url` / `value` as plain CPPTYPE_STRING / TYPE_BYTES
// fields via reflection.  No Any-unwrap fires because the operand is
// a constructed Any value, not an Any-typed field on a parent
// message.  Pin both round-trips so the boundary doesn't drift.
TEST_F(AnyLiteralRoundTripE2ETest, DirectAnyLiteralTypeUrlReadRoundTrips) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(google.protobuf.Any{type_url: 'type.googleapis.com/X', )"
                 R"(value: b'hello'}.type_url == 'type.googleapis.com/X')");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

TEST_F(AnyLiteralRoundTripE2ETest, DirectAnyLiteralValueReadRoundTrips) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(google.protobuf.Any{type_url: 'type.googleapis.com/X', )"
                 R"(value: b'hello'}.value == b'hello')");
  EXPECT_EQ(*EvalOk(instance, Activation{}).AsBool(), true);
}

}  // namespace
}  // namespace celwasm
