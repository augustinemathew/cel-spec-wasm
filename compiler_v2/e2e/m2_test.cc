// M2 e2e test suite — the spec of "done" for the
// idents / kSelect / has() / partial-eval milestone.
//
// Written TDD-style.  Every test here asserts a capability M2 must
// land; running this binary before the implementation catches up
// should fail / crash.  Greening the suite is the milestone exit.
//
// Scope mirrors `doc/implementation-plan/rewrite/m2-ident-select-unknowns.md`
// §6.2 (E2E tests) plus the corner cases called out across §6.1
// and §9 (risk register).  Grouped by capability:
//
//   - Ident           — local.get lowering, every scalar kind,
//                       unbound / missing-decl / aliasing cases.
//   - Select          — proto field read of every CEL-relevant
//                       wire type, nested hops, proto3 default
//                       semantics.
//   - Has             — test_only dispatch, proto3 implicit
//                       presence, proto2 explicit presence (via
//                       HostMsg2 when landed), unset vs default.
//   - Unknown         — leaf, nested-short-circuit, wildcard,
//                       non-matching-patterns-pass-through,
//                       Eval ↔ PartialEval parity.
//   - EnvelopeError   — MAP / REPEATED field reads produce
//                       CEL_ERR_TYPE_UNSUPPORTED per §2.8.
//
// The tests build real proto fixtures (celwasm.testdata.Customer,
// celwasm.testdata.HostMsg3) — no synthetic WAT shortcuts.  Every
// expression goes through the full pipeline:
//   Compiler::Compile → Engine::Plan → Instance::Eval(activation)
// and, for the unknown cases,
//   Instance::PartialEval(activation, patterns).

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "compiler/testdata/host_fixture_proto3.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::celwasm::testdata::Address;
using ::celwasm::testdata::Customer;
using ::celwasm::testdata::HostMsg3;

// ──────────────────────────────────────────────────────────────
//  Test harness — builds a single Engine shared across tests.
// ──────────────────────────────────────────────────────────────

// Shared Engine — wasm_engine_t + parsed runtime module are
// documented thread-safe; one instance per test binary is plenty.
Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Build a Compiler pre-seeded with a single `c` variable of
// message type, resolving the descriptor through the process
// descriptor pool (Customer is cc_proto_library-linked so its
// descriptor is in generated_pool()).
// Convenience for the named-local / std::move(b).Build() pattern —
// Compiler::Builder is consumed only by Build().  Configure is
// called with the builder by reference; any chained setter's
// return value is ignored.
using ConfigureFn = std::function<void(Compiler::Builder&)>;
absl::StatusOr<Compiler> BuildCompiler(const ConfigureFn& configure) {
  Compiler::Builder b;
  configure(b);
  return std::move(b).Build();
}

absl::StatusOr<Compiler> CompilerWithCustomerVar() {
  return BuildCompiler([](Compiler::Builder& b) {
    b.RegisterMessageType(Customer::descriptor())
        .DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  });
}

absl::StatusOr<Compiler> CompilerWithHostMsg3Var() {
  return BuildCompiler([](Compiler::Builder& b) {
    b.RegisterMessageType(HostMsg3::descriptor())
        .DeclareVariable("h", CelType::Message("celwasm.testdata.HostMsg3"));
  });
}

// Shorthand: single declared variable, typical IdentE2ETest shape.
absl::StatusOr<Compiler> CompilerWithVar(const std::string& name,
                                         const CelType& type) {
  return BuildCompiler([&](Compiler::Builder& b) {
    b.DeclareVariable(name, type);
  });
}

// Compile + Plan in one call — returns a fresh Instance, the
// Program held alive via a leak (test-only; the Engine owns the
// wasm engine).  Asserts each stage is ok() so the caller can
// pattern-match on Eval behaviour alone.
Instance CompilePlan(const Compiler& compiler, absl::string_view source) {
  auto program = compiler.Compile(source);
  ABSL_CHECK_OK(program) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance) << source;
  return *std::move(instance);
}

// Convenience for Eval(activation).  ABSL_CHECKs the status — use
// the raw `instance.Eval(activation)` form when inspecting an
// expected failure.
Value EvalOk(Instance& instance, const Activation& activation) {
  auto v = instance.Eval(activation);
  ABSL_CHECK_OK(v);
  return *std::move(v);
}

// Convenience for PartialEval — same contract.
Value PartialEvalOk(Instance& instance, const Activation& activation,
                    absl::Span<const AttributePattern> unknowns) {
  auto v = instance.PartialEval(activation, unknowns);
  ABSL_CHECK_OK(v);
  return *std::move(v);
}

AttributePattern MakePattern(absl::string_view dotted) {
  auto p = AttributePattern::Parse(dotted);
  ABSL_CHECK_OK(p) << dotted;
  return *std::move(p);
}

// ──────────────────────────────────────────────────────────────
//  Ident lowering (Slice M2.B)
//
//  Every scalar kind round-trips a bound Value through:
//    Activation::Bind → cel.abi.variables[] → $eval prelude
//    materialises local → expr_lower `kIdent` arm emits local.get.
// ──────────────────────────────────────────────────────────────

class IdentE2ETest : public ::testing::Test {};

TEST_F(IdentE2ETest, Bool) {
  auto compiler = CompilerWithVar("x", CelType::Bool());
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "x");
  Activation a;
  a.Bind("x", Value::Bool(true));
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_EQ(*v.AsBool(), true);
}

TEST_F(IdentE2ETest, Int) {
  auto compiler = CompilerWithVar("x", CelType::Int());
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "x");
  Activation a;
  a.Bind("x", Value::Int(42));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 42);
}

TEST_F(IdentE2ETest, Uint) {
  auto compiler = CompilerWithVar("x", CelType::Uint());
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "x");
  Activation a;
  a.Bind("x", Value::Uint(9000u));
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 9000u);
}

TEST_F(IdentE2ETest, Double) {
  auto compiler = CompilerWithVar("x", CelType::Double());
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "x");
  Activation a;
  a.Bind("x", Value::Double(3.14));
  EXPECT_DOUBLE_EQ(*EvalOk(instance, a).AsDouble(), 3.14);
}

TEST_F(IdentE2ETest, String) {
  auto compiler = CompilerWithVar("s", CelType::String());
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "s");
  Activation a;
  a.Bind("s", Value::String("hello"));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "hello");
}

TEST_F(IdentE2ETest, Bytes) {
  auto compiler = CompilerWithVar("b", CelType::Bytes());
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "b");
  Activation a;
  a.Bind("b", Value::Bytes(std::string("\x00\x01\x02", 3)));
  auto got = *EvalOk(instance, a).AsBytes();
  EXPECT_EQ(got, absl::string_view("\x00\x01\x02", 3));
}

// Unbound variable should surface as FailedPrecondition, not a
// crash or an uninitialised value.  Plan §2.1: Eval with a
// declared variable missing from the activation → error.
TEST_F(IdentE2ETest, UnboundDeclaredVariableFailsPrecondition) {
  auto compiler = CompilerWithVar("x", CelType::Int());
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "x");
  Activation empty;  // no Bind calls.
  auto v = instance.Eval(empty);
  EXPECT_THAT(v, StatusIs(absl::StatusCode::kFailedPrecondition));
}

// Eval called back-to-back on the same Instance with fresh
// Activations produces fresh results — cel_reset clears the
// arena, BindLazy memoisation clears, local_set overwrites the
// previous binding.
TEST_F(IdentE2ETest, BackToBackEvalRebindsIdent) {
  auto compiler = CompilerWithVar("s", CelType::String());
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "s");

  Activation a1;
  a1.Bind("s", Value::String("first"));
  EXPECT_EQ(*EvalOk(instance, a1).AsString(), "first");

  Activation a2;
  a2.Bind("s", Value::String("second"));
  EXPECT_EQ(*EvalOk(instance, a2).AsString(), "second");
}

// ──────────────────────────────────────────────────────────────
//  Select — proto field reads (Slice M2.C)
//
//  Every CEL-relevant scalar wire type through ProtoBacking::
//  ReadField, plus nested single-hop and two-hop chains.  The
//  Customer fixture covers the bread-and-butter domain shape;
//  HostMsg3 extends coverage to the full scalar matrix in the
//  smoke test (§6.1.1) — e2e here samples the interesting arms.
// ──────────────────────────────────────────────────────────────

class SelectE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto c = CompilerWithCustomerVar();
    ABSL_CHECK_OK(c);
    compiler_ = *std::move(c);
  }

  Compiler compiler_{Compiler::NewBuilder().Build().value()};
};

TEST_F(SelectE2ETest, SelectString) {
  auto instance = CompilePlan(compiler_, "c.name");
  Customer msg;
  msg.set_name("Ada");
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "Ada");
}

TEST_F(SelectE2ETest, SelectInt32) {
  auto instance = CompilePlan(compiler_, "c.age");
  Customer msg;
  msg.set_age(30);
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 30);
}

TEST_F(SelectE2ETest, SelectInt64) {
  auto instance = CompilePlan(compiler_, "c.user_id");
  Customer msg;
  msg.set_user_id(9'000'000'000'000LL);
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 9'000'000'000'000LL);
}

TEST_F(SelectE2ETest, SelectUint32) {
  auto instance = CompilePlan(compiler_, "c.priority");
  Customer msg;
  msg.set_priority(5u);
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 5u);
}

TEST_F(SelectE2ETest, SelectUint64) {
  auto instance = CompilePlan(compiler_, "c.balance_cents");
  Customer msg;
  msg.set_balance_cents(1'000'000uLL);
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 1'000'000uLL);
}

TEST_F(SelectE2ETest, SelectDouble) {
  auto instance = CompilePlan(compiler_, "c.credit_score");
  Customer msg;
  msg.set_credit_score(750.5);
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_DOUBLE_EQ(*EvalOk(instance, a).AsDouble(), 750.5);
}

TEST_F(SelectE2ETest, SelectBool) {
  auto instance = CompilePlan(compiler_, "c.is_premium");
  Customer msg;
  msg.set_is_premium(true);
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(SelectE2ETest, SelectBytes) {
  auto instance = CompilePlan(compiler_, "c.session_token");
  Customer msg;
  msg.set_session_token(std::string("\xde\xad\xbe\xef", 4));
  Activation a;
  a.Bind("c", Value::Message(msg));
  auto got = *EvalOk(instance, a).AsBytes();
  EXPECT_EQ(got, absl::string_view("\xde\xad\xbe\xef", 4));
}

// Two-hop select chain: c.billing_address.city.
// Exercises nested-message intern-at-first-hop + second-hop read.
TEST_F(SelectE2ETest, SelectNestedMessageField) {
  auto instance = CompilePlan(compiler_, "c.billing_address.city");
  Customer msg;
  msg.mutable_billing_address()->set_city("Seattle");
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "Seattle");
}

// Proto3 implicit-presence: an unset scalar string returns the
// default ("").  The select must NOT crash or return UNKNOWN.
TEST_F(SelectE2ETest, SelectUnsetProto3StringReturnsDefault) {
  auto instance = CompilePlan(compiler_, "c.name");
  Customer msg;  // default-constructed; name unset.
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "");
}

// Same Instance, two different messages: lifetime of the bound
// message ends after Eval returns, so back-to-back calls with
// different arguments must work.
TEST_F(SelectE2ETest, BackToBackEvalWithDifferentMessages) {
  auto instance = CompilePlan(compiler_, "c.name");
  Customer a_msg;
  Customer b_msg;
  a_msg.set_name("Alice");
  b_msg.set_name("Bob");
  {
    Activation act;
    act.Bind("c", Value::Message(a_msg));
    EXPECT_EQ(*EvalOk(instance, act).AsString(), "Alice");
  }
  {
    Activation act;
    act.Bind("c", Value::Message(b_msg));
    EXPECT_EQ(*EvalOk(instance, act).AsString(), "Bob");
  }
}

// Self-recursive message — exercises intern-on-message at depth
// ≥ 2 (h.inner.b on a HostMsg3 with a populated inner).  The
// recursion boundary is a classic off-by-one site for the
// ExternrefTable.
TEST_F(SelectE2ETest, SelectSelfRecursiveInnerField) {
  auto c = CompilerWithHostMsg3Var();
  ASSERT_THAT(c, IsOk());
  auto instance = CompilePlan(*c, "h.inner.b");
  HostMsg3 msg;
  msg.mutable_inner()->set_b(true);
  Activation a;
  a.Bind("h", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// Three-hop self-recursive chain — proves the intern-on-message
// path works at arbitrary depth (h.inner.inner.b).
TEST_F(SelectE2ETest, SelectThreeHopSelfRecursive) {
  auto c = CompilerWithHostMsg3Var();
  ASSERT_THAT(c, IsOk());
  auto instance = CompilePlan(*c, "h.inner.inner.b");
  HostMsg3 msg;
  msg.mutable_inner()->mutable_inner()->set_b(true);
  Activation a;
  a.Bind("h", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
//  has() — test-only select dispatch (Slice M2.D)
//
//  `has(msg.field)` becomes a Select(..., test_only=true) from
//  cel-cpp's macro expander, not a kCall.  expr_lower dispatches
//  test_only to cel_host.cel_has_field, which returns a bool
//  per langdef's proto2/proto3 rules.
// ──────────────────────────────────────────────────────────────

class HasE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto c = CompilerWithCustomerVar();
    ABSL_CHECK_OK(c);
    compiler_ = *std::move(c);
  }
  Compiler compiler_{Compiler::NewBuilder().Build().value()};
};

// Proto3 singular scalar: set → true.  This is the ONLY case
// where proto3 has() can differ from "field has non-default
// value" on the cel-go / cel-java side; cel-cpp treats proto3
// singular-scalar has() as "not default" per langdef 3.2.
TEST_F(HasE2ETest, StringFieldSetReturnsTrue) {
  auto instance = CompilePlan(compiler_, "has(c.name)");
  Customer msg;
  msg.set_name("Ada");
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// Proto3 singular scalar: unset (default value) → false.
TEST_F(HasE2ETest, StringFieldUnsetReturnsFalse) {
  auto instance = CompilePlan(compiler_, "has(c.name)");
  Customer msg;  // default-constructed.
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// Nested message: billing_address set (even empty) → true.
// Proto3 messages have explicit presence (unlike scalars).
TEST_F(HasE2ETest, NestedMessageSetReturnsTrue) {
  auto instance = CompilePlan(compiler_, "has(c.billing_address)");
  Customer msg;
  (void)msg.mutable_billing_address();  // set, even if empty.
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// Nested message unset → false.
TEST_F(HasE2ETest, NestedMessageUnsetReturnsFalse) {
  auto instance = CompilePlan(compiler_, "has(c.billing_address)");
  Customer msg;  // billing_address unset.
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// Two-hop has: has(c.billing_address.city).  The first hop reads
// the submessage, second hop tests the field.
TEST_F(HasE2ETest, TwoHopHasSet) {
  auto instance = CompilePlan(compiler_, "has(c.billing_address.city)");
  Customer msg;
  msg.mutable_billing_address()->set_city("Seattle");
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(HasE2ETest, TwoHopHasUnsetLeafReturnsFalse) {
  auto instance = CompilePlan(compiler_, "has(c.billing_address.city)");
  Customer msg;
  (void)msg.mutable_billing_address();  // submessage set, city unset.
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// ──────────────────────────────────────────────────────────────
//  Unknown propagation (Slice M2.E)
//
//  PartialEval short-circuits any select whose resolved
//  AttributeId matches an unknown pattern.  Nested chains absorb
//  at the first unknown hop (all-or-nothing — no partial proto
//  reads if a parent is unknown).
// ──────────────────────────────────────────────────────────────

class UnknownE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto c = CompilerWithCustomerVar();
    ABSL_CHECK_OK(c);
    compiler_ = *std::move(c);
  }
  Compiler compiler_{Compiler::NewBuilder().Build().value()};
};

// Leaf unknown: --unknown_attrs "c.name" → UNKNOWN(c.name).
TEST_F(UnknownE2ETest, LeafUnknownShortCircuits) {
  auto instance = CompilePlan(compiler_, "c.name");
  Customer msg;
  msg.set_name("Ada");  // concrete value — but declared unknown.
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.name")};
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kUnknown);
  auto attr = v.UnknownAttribute();
  ASSERT_THAT(attr, IsOk());
  EXPECT_NE(attr->id, 0u) << "unknown attr id must be a real slot";
}

// Nested select short-circuits at first unknown hop:
// --unknown_attrs "c.billing_address" evaluating
// `c.billing_address.city` → UNKNOWN (no descriptor walk for
// `.city`, no crash even if billing_address is unset).
TEST_F(UnknownE2ETest, NestedChainAbsorbsAtFirstUnknownHop) {
  auto instance = CompilePlan(compiler_, "c.billing_address.city");
  Customer msg;  // billing_address unset — should not crash.
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.billing_address")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown);
}

// Wildcard: --unknown_attrs "c.*.city" matches
// `c.billing_address.city`.
TEST_F(UnknownE2ETest, WildcardMidPathMatches) {
  auto instance = CompilePlan(compiler_, "c.billing_address.city");
  Customer msg;
  msg.mutable_billing_address()->set_city("Seattle");
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.*.city")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown);
}

// Non-matching pattern: --unknown_attrs "c.age" evaluating
// `c.name` → concrete string, not unknown.
TEST_F(UnknownE2ETest, NonMatchingPatternsPassThrough) {
  auto instance = CompilePlan(compiler_, "c.name");
  Customer msg;
  msg.set_name("Ada");
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.age")};
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "Ada");
}

// Eval ↔ PartialEval parity: identical concrete result when no
// unknowns are declared.  Locks the design.md §10.1.3 invariant
// "codegen stays oblivious to partial eval".
TEST_F(UnknownE2ETest, EvalVsPartialEvalParityWithNoPatterns) {
  auto instance = CompilePlan(compiler_, "c.billing_address.city");
  Customer msg;
  msg.mutable_billing_address()->set_city("Seattle");
  Activation a;
  a.Bind("c", Value::Message(msg));

  auto concrete = EvalOk(instance, a);
  auto partial = PartialEvalOk(instance, a, /*unknowns=*/{});
  EXPECT_TRUE(concrete.StructurallyEquals(partial));
}

// has() absorbs unknowns: has(c.billing_address) under a pattern
// that matches billing_address returns UNKNOWN (not false).  The
// spec rule: has() against an unknown attribute cannot decide
// presence, so the outcome propagates as unknown.
TEST_F(UnknownE2ETest, HasAbsorbsUnknownAtTarget) {
  auto instance = CompilePlan(compiler_, "has(c.billing_address)");
  Customer msg;  // billing_address unset.
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.billing_address")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown);
}

// Ident-rooted unknown: declaring the root variable unknown
// short-circuits every select against it.
TEST_F(UnknownE2ETest, RootIdentUnknownShortCircuitsSelect) {
  auto instance = CompilePlan(compiler_, "c.name");
  Customer msg;
  msg.set_name("Ada");
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown);
}

// ──────────────────────────────────────────────────────────────
//  AttributePattern::Parse — dotted-path + wildcard
// ──────────────────────────────────────────────────────────────

TEST(AttributePatternParseTest, SingleSegmentVariable) {
  auto p = AttributePattern::Parse("x");
  ASSERT_THAT(p, IsOk());
  EXPECT_EQ(p->variable(), "x");
  EXPECT_TRUE(p->qualifier_path().empty());
}

TEST(AttributePatternParseTest, DottedPath) {
  auto p = AttributePattern::Parse("c.billing_address.city");
  ASSERT_THAT(p, IsOk());
  EXPECT_EQ(p->variable(), "c");
  ASSERT_EQ(p->qualifier_path().size(), 2u);
  EXPECT_TRUE(p->qualifier_path()[0].IsMatch("billing_address"));
  EXPECT_TRUE(p->qualifier_path()[1].IsMatch("city"));
}

TEST(AttributePatternParseTest, WildcardMidPath) {
  auto p = AttributePattern::Parse("c.*.city");
  ASSERT_THAT(p, IsOk());
  ASSERT_EQ(p->qualifier_path().size(), 2u);
  EXPECT_TRUE(p->qualifier_path()[0].IsWildcard());
  EXPECT_FALSE(p->qualifier_path()[1].IsWildcard());
}

TEST(AttributePatternParseTest, WildcardTrailing) {
  auto p = AttributePattern::Parse("c.billing_address.*");
  ASSERT_THAT(p, IsOk());
  ASSERT_EQ(p->qualifier_path().size(), 2u);
  EXPECT_FALSE(p->qualifier_path()[0].IsWildcard());
  EXPECT_TRUE(p->qualifier_path()[1].IsWildcard());
}

// Array/map index forms also parse — relevant once lists and maps
// are evaluable (M6), but the Parse surface is shared now.
TEST(AttributePatternParseTest, ArrayIndexAndMapKey) {
  auto p = AttributePattern::Parse("request.messages[3].text");
  ASSERT_THAT(p, IsOk());
  ASSERT_EQ(p->qualifier_path().size(), 3u);
  EXPECT_TRUE(p->qualifier_path()[1].IsMatch(AttributeQualifier::OfInt(3)));
}

TEST(AttributePatternParseTest, EmptyInputIsInvalid) {
  auto p = AttributePattern::Parse("");
  EXPECT_THAT(p, StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, LeadingDotIsInvalid) {
  auto p = AttributePattern::Parse(".x");
  EXPECT_THAT(p, StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, TrailingDotIsInvalid) {
  auto p = AttributePattern::Parse("x.");
  EXPECT_THAT(p, StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, ConsecutiveDotsIsInvalid) {
  auto p = AttributePattern::Parse("x..y");
  EXPECT_THAT(p, StatusIs(absl::StatusCode::kInvalidArgument));
}

// ──────────────────────────────────────────────────────────────
//  Envelope boundary — MAP/REPEATED → CEL_ERR_TYPE_UNSUPPORTED
//
//  §2.8 / §6.1.1 row: ProtoBacking::ReadField on a REPEATED
//  field returns Value::Error(CEL_ERR_TYPE_UNSUPPORTED) until
//  M6 swaps in ProtoRepeatedBacking.  The M2→M6 graduation
//  contract: this test starts green at M2 and must be flipped
//  (new expected value) when M6 lands.
// ──────────────────────────────────────────────────────────────

TEST(EnvelopeBoundaryE2ETest, SelectRepeatedFieldReturnsUnsupportedError) {
  auto c = CompilerWithHostMsg3Var();
  ASSERT_THAT(c, IsOk());
  // Even selecting a repeated field — without operating on it —
  // triggers the envelope: ReadField returns CEL_ERROR.
  auto instance = CompilePlan(*c, "h.rep_i32");
  HostMsg3 msg;
  msg.add_rep_i32(1);
  msg.add_rep_i32(2);
  Activation a;
  a.Bind("h", Value::Message(msg));
  auto v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kError);
  auto err = v.ErrorInfo();
  ASSERT_THAT(err, IsOk());
  // CEL_ERR_TYPE_UNSUPPORTED — plan §2.4.1 / §2.8 row.
  EXPECT_EQ(static_cast<int>((*err)->code),
            /*CEL_ERR_TYPE_UNSUPPORTED=*/20);
}

}  // namespace
}  // namespace cel
