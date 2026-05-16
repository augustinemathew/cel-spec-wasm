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
#include "absl/strings/string_view.h"
#include "compiler/testdata/host_fixture_proto2.pb.h"
#include "compiler/testdata/host_fixture_proto3.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/internal/cel_host.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/any.pb.h"
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
      google::protobuf::LinkMessageReflection<google::protobuf::Any>();
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

// Used by AnyPack tests: the HostMsg3 fixture doesn't yet have an
// `Any single_any` field (see §"Test fixture extension" in
// m7a-any.md §11).  Until the fixture is extended, AnyPack tests
// route through `cel.expr.conformance.proto3.TestAllTypes` via the
// conformance descriptor pool — but the conformance pool is not on
// the e2e path's BUILD graph today.  Tests in this file therefore
// `GTEST_SKIP()` with a pointer to the fixture-extension item.
constexpr absl::string_view kFixtureExtensionPending =
    "M7-A test fixture: HostMsg3 needs `google.protobuf.Any single_any` "
    "field (see m7a-any.md §11.1).  Until that lands the AnyPack /  "
    "AnyUnpack rows route only through the conformance harness, not "
    "through this e2e suite.";

constexpr absl::string_view kM7aPackPending =
    "M7-A.A pack arm not yet shipped (m7a-any.md §5).";
constexpr absl::string_view kM7aUnpackPending =
    "M7-A.B unpack arm not yet shipped (m7a-any.md §5).";
constexpr absl::string_view kM7aEqPending =
    "M7-A.C cel_message_eq Any-peel arm not yet shipped (m7a-any.md §5).";

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

// ──────────────────────────────────────────────────────────────
// 1. AnyPackE2ETest  (M7-A.A — typed-message RHS into Any field)
//
// Each row exercises a different field shape (singular / repeated /
// map) and a different RHS descriptor.  The runtime path:
//   1. CelSetFieldImpl resolves field_ref_id → FieldDescriptor.
//   2. cpp_type is MESSAGE, message_type() is `google.protobuf.Any`.
//   3. AssignMessageOrPack helper detects the Any-shaped mismatch and
//      packs: SetString(type_url, "type.googleapis.com/<src.fqn>")
//      + SetString(value, src.SerializeAsString()).
//
// Test cases skip until M7-A.A ships.
// ──────────────────────────────────────────────────────────────

class AnyPackE2ETest : public ::testing::Test {};

TEST_F(AnyPackE2ETest, PackTypedMessageIntoSingularAny) {
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyPackE2ETest, PackEmptyMessageIntoSingularAny) {
  // Pack `HostMsg3{}` (a default-constructed proto with zero set fields)
  // into an Any field.  Verifies that the empty-payload-bytes path
  // (value_size==0) round-trips: `Any.value == ""` but `type_url`
  // is still set.
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyPackE2ETest, PackTypedMessageIntoRepeatedAnyArenaSource) {
  // `Foo{repeated_any: [Bar{x:1}, Baz{y:2}]}` — repeated field of
  // Any with arena-list source.  Two different inner types in the
  // same list (Any is heterogeneous).
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyPackE2ETest, PackTypedMessageIntoRepeatedAnyHostSource) {
  // Same shape but the list is bound via Activation::Bind(Value::List).
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyPackE2ETest, PackTypedMessageIntoMapValueAny) {
  // `Foo{map_any: {"a": Bar{}, "b": Baz{}}}` — map<string, Any>.
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyPackE2ETest, PackAnyIntoAnyIsCopyFromNotDoubleWrap) {
  // RHS is itself an Any: descriptor matches; existing M7 CopyFrom
  // path handles it.  Regression-test that M7-A.A's `AssignMessageOrPack`
  // helper takes the CopyFrom branch, not the pack branch (which
  // would double-wrap the Any).
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyPackE2ETest, PackWrapperMessageIntoAnyDoesNotInvokeM8AutoWrap) {
  // `Foo{single_any: Int32Value{value: 5}}` — RHS is a typed message
  // (Int32Value with one field set).  The runtime should pack it as
  // an ordinary typed RHS: the WrapperMessage descriptor != Any
  // descriptor, so M7-A.A's Any branch fires.  M8's wrapper auto-wrap
  // does NOT come into play (this would only fire for a scalar RHS
  // into a wrapper-typed *field*, not a wrapper-message RHS into an
  // Any *field*).
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyPackE2ETest, PackProto2MessageIntoProto3AnyField) {
  // Cross-syntax: source is a proto2 message, destination Any field
  // belongs to a proto3 outer.  Any is syntax-agnostic; the pack
  // must succeed regardless.
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
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

TEST_F(AnyUnpackE2ETest, ReadAnyFieldReturnsUnwrappedTypedValue) {
  // `msg.single_any.x == 1` where `single_any` was packed at
  // construction with `TestAllTypes{single_int32: 1}`.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyUnpackE2ETest, ReadAnyFieldOnActivationBoundMessage) {
  // The embedder packs an Any field on a proto and binds it via
  // `Activation::Bind(Value::Message(proto))`.  Reading
  // `bound.single_any.x` exercises the unpack path on an
  // activation-rooted backing (i.e. `ProtoBacking` not
  // `OwnedProtoBacking`).
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyUnpackE2ETest, ReadUnsetAnyFieldReturnsNull) {
  // `TestAllTypes{}.single_any == null` — M7-shipped null-on-unset-
  // singular-message rule.  M7-A.B's unpack arm must NOT trigger
  // when the Any field is unset (type_url == "").  Regression-test
  // that M7-A.B preserves M7's null-clear behaviour.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyUnpackE2ETest, ReadRepeatedAnyFieldUnwrapsEachElement) {
  // `msg.repeated_any[0].x` reads the first packed Any.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyUnpackE2ETest, ReadMapValueAnyUnwrapsLookup) {
  // `msg.map_any["k"].x` reads the value at key "k", which is itself
  // a packed Any.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyUnpackE2ETest, ChainedSelectOnUnpackedMessage) {
  // `msg.single_any.nested.x` — the Any unwraps to a typed message,
  // then we chained-select through it.  Tests that the unwrapped
  // backing routes through ProtoBacking's normal kSelect path with
  // no Any-specific awareness past the boundary.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
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

TEST_F(AnyEqualityE2ETest, AnyEqualsMatchingTypedMessage) {
  // `Foo{single_any: Bar{x:1}}.single_any == Bar{x:1}` → true.
  GTEST_SKIP() << kM7aEqPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyEqualityE2ETest, AnyEqualsMismatchingTypedMessageIsFalse) {
  // `Foo{single_any: Bar{x:1}}.single_any == Baz{}` → false (not
  // error).  Different typed payloads compare unequal.
  GTEST_SKIP() << kM7aEqPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyEqualityE2ETest, TypedMessageEqualsAnySymmetric) {
  // `Bar{x:1} == Foo{single_any: Bar{x:1}}.single_any` — same as
  // above with operands swapped.  M7-A.C's peel must fire on either
  // side.
  GTEST_SKIP() << kM7aEqPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyEqualityE2ETest, AnyEqualsAnyDelegatesToMessageDifferencer) {
  // `Foo{single_any: Bar{x:1}}.single_any ==
  //  Foo{single_any: Bar{x:1}}.single_any` → true.  After M7-A.B's
  // unwrap, both sides become typed `Bar`; M5.B step 2b's
  // `cel_message_eq` handles the rest.
  GTEST_SKIP() << kM7aEqPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyEqualityE2ETest, AnysWithDifferentTypeUrlsAreUnequal) {
  // `Any{type_url: "...Bar", value: ...} ==
  //  Any{type_url: "...Baz", value: <same bytes>}` → false even if
  // value bytes happen to match.  After unwrap the operands have
  // different runtime types; cross-type equality is false (not
  // error).
  GTEST_SKIP() << kM7aEqPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyEqualityE2ETest, UnsetAnyEqualsNull) {
  // `TestAllTypes{}.single_any == null` — unset Any reads as null
  // (M7-shipped); the peel branch must not fire here.
  GTEST_SKIP() << kM7aEqPending << " — " << kFixtureExtensionPending;
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

TEST_F(AnyTypeOfE2ETest, TypeOfUnpackedAnyReturnsUnwrappedFqn) {
  // `type(Foo{single_any: Bar{x:1}}.single_any) ==
  //  "celwasm.testdata.Bar"` (or similar) → true.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyTypeOfE2ETest, TypeOfDirectlyConstructedAnyReturnsAny) {
  // `type(google.protobuf.Any{type_url: ..., value: ...}) ==
  //  google.protobuf.Any` → true.  When the Any is constructed
  // via the `google.protobuf.Any{type_url, value}` literal (M7-shipped,
  // since Any is just a 2-field proto), the resulting Value carries
  // the Any descriptor; reading `type(...)` of it returns "Any".  Only
  // a *field-read* of an Any-typed field triggers M7-A.B's unwrap.
  // Pin the boundary.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
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

TEST_F(AnyRejectE2ETest, ReadAnyWithEmptyTypeUrlIsError) {
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyRejectE2ETest, ReadAnyWithMalformedTypeUrlIsError) {
  // `type_url = "not_a_type_url"` (no slash) → empty FQN → error.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyRejectE2ETest, ReadAnyWithUnknownFqnIsError) {
  // `type_url = "type.googleapis.com/com.nope.Unknown"` — FQN not
  // in the descriptor pool.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyRejectE2ETest, ReadAnyWithCorruptValueBytesIsError) {
  // `value = b"\xff\xff\xff\xff"` against a known FQN — bytes don't
  // parse as the resolved message.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyRejectE2ETest, NonGoogleApisPrefixIsAccepted) {
  // Probe B finding: stripping before the last `/` is the rule;
  // `type.example.com/<FQN>` is accepted equivalently to
  // `type.googleapis.com/<FQN>`.  Pin this — cel-cpp parity.
  GTEST_SKIP() << kM7aUnpackPending << " — " << kFixtureExtensionPending;
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

TEST_F(AnyNullClearE2ETest, NullSetOnSingularAnyClearsField) {
  // `Foo{single_any: null}.single_any == null` → true.  Tests the
  // M7-shipped null-clear; no M7-A code involved, but if M7-A.A
  // regresses the ordering this fails.
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
}

TEST_F(AnyNullClearE2ETest, NullSetOnSingularAnyHasFieldIsFalse) {
  // `has(Foo{single_any: null}.single_any) == false` — proto3
  // singular message presence rule (M7-shipped).
  GTEST_SKIP() << kM7aPackPending << " — " << kFixtureExtensionPending;
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

void ExpectCompileFails(const Compiler& compiler, absl::string_view source,
                        absl::string_view why) {
  auto program_or = compiler.Compile(source);
  EXPECT_FALSE(program_or.ok())
      << "expected `" << source << "` to fail at compile (" << why << ")";
}

TEST_F(AnyLiteralRoundTripE2ETest, DirectAnyLiteralCurrentlyDynRejected) {
  // Documents the as-shipped behaviour: direct Any-literal
  // construction is rejected by the static-subset gate.  M7-A does
  // NOT change this; cel-cpp checker types Any{...} as dyn-shaped.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(
      *compiler,
      R"(google.protobuf.Any{type_url: 'type.googleapis.com/X', )"
      R"(value: b'hello'}.type_url == 'type.googleapis.com/X')",
      "Any literal currently hits RejectDyn gate (m7a-any.md §10.3)");
}

TEST_F(AnyLiteralRoundTripE2ETest, DirectAnyLiteralOnBytesAlsoDynRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(
      *compiler,
      R"(google.protobuf.Any{type_url: 'type.googleapis.com/X', )"
      R"(value: b'hello'}.value == b'hello')",
      "Any literal currently hits RejectDyn gate (m7a-any.md §10.3)");
}

}  // namespace
}  // namespace cel
