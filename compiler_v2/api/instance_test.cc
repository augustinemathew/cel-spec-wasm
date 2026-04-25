// M1 — Instance::Eval end-to-end through the full Compiler →
// Engine → Plan → Eval pipeline.  Per Plan §5.5: ports the per-
// scalar-kind round-trip tests previously in
// host_loader_test.cc onto the new public api/.  Each test
// compiles a literal, plans it, evals once, asserts the resulting
// Value matches.
//
// These tests depend on Commit F's codegen flip — expr now imports
// cel.memory rather than defining it, which is what Engine::Plan
// expects.  Pre-flip these would fail at instantiate-time.

#include <cstdint>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/internal/cel_host.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

// Force generated-pool registration of descriptors referenced by
// tests below.  Runs once at static init per test binary.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::Address>();
      return 0;
    }();

// Compiler + Engine are reused across tests via a fixture — both
// are immutable after Build (Compiler) / immutable after Build
// + thread-safe shared (Engine).  Cuts ~167us of per-test
// engine+runtime-module setup that would otherwise dominate.
class InstanceEvalTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    auto compiler_or = Compiler::NewBuilder().Build();
    ABSL_CHECK_OK(compiler_or);
    compiler_ = new Compiler(*std::move(compiler_or));
    auto engine_or = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(engine_or);
    engine_ = new Engine(*std::move(engine_or));
  }
  static void TearDownTestSuite() {
    delete engine_;
    delete compiler_;
    engine_ = nullptr;
    compiler_ = nullptr;
  }

  // Shorthand: compile + plan + eval once, return decoded Value.
  static Value EvalLiteral(absl::string_view source) {
    auto prog_or = compiler_->Compile(source);
    ABSL_CHECK_OK(prog_or) << "Compile(" << source << ")";
    auto inst_or = engine_->Plan(*prog_or);
    ABSL_CHECK_OK(inst_or) << "Plan(" << source << ")";
    Instance inst = *std::move(inst_or);
    auto val_or = inst.Eval();
    ABSL_CHECK_OK(val_or) << "Eval(" << source << ")";
    return *std::move(val_or);
  }

  static Compiler* compiler_;
  static Engine* engine_;
};
Compiler* InstanceEvalTest::compiler_ = nullptr;
Engine* InstanceEvalTest::engine_ = nullptr;

// ——— Per-scalar-kind round trips (port from host_loader_test). ———

TEST_F(InstanceEvalTest, EvalsIntLiteral) {
  Value v = EvalLiteral("42");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  auto i = v.AsInt();
  ASSERT_TRUE(i.ok());
  EXPECT_EQ(*i, 42);
}

TEST_F(InstanceEvalTest, EvalsNegIntLiteral) {
  Value v = EvalLiteral("-42");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  auto i = v.AsInt();
  ASSERT_TRUE(i.ok());
  EXPECT_EQ(*i, -42);
}

TEST_F(InstanceEvalTest, EvalsUintLiteral) {
  Value v = EvalLiteral("42u");
  ASSERT_EQ(v.kind(), Value::Kind::kUint);
  auto u = v.AsUint();
  ASSERT_TRUE(u.ok());
  EXPECT_EQ(*u, 42u);
}

TEST_F(InstanceEvalTest, EvalsBoolLiteralTrue) {
  Value v = EvalLiteral("true");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  auto b = v.AsBool();
  ASSERT_TRUE(b.ok());
  EXPECT_TRUE(*b);
}

TEST_F(InstanceEvalTest, EvalsBoolLiteralFalse) {
  Value v = EvalLiteral("false");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  auto b = v.AsBool();
  ASSERT_TRUE(b.ok());
  EXPECT_FALSE(*b);
}

TEST_F(InstanceEvalTest, EvalsDoubleLiteral) {
  Value v = EvalLiteral("3.14");
  ASSERT_EQ(v.kind(), Value::Kind::kDouble);
  auto d = v.AsDouble();
  ASSERT_TRUE(d.ok());
  EXPECT_DOUBLE_EQ(*d, 3.14);
}

TEST_F(InstanceEvalTest, EvalsNullLiteral) {
  Value v = EvalLiteral("null");
  EXPECT_TRUE(v.IsNull());
}

TEST_F(InstanceEvalTest, EvalsStringLiteral) {
  Value v = EvalLiteral(R"("hello")");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  auto s = v.AsString();
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(*s, "hello");
}

TEST_F(InstanceEvalTest, EvalsBytesLiteral) {
  Value v = EvalLiteral(R"(b"\x00\x01\x02")");
  ASSERT_EQ(v.kind(), Value::Kind::kBytes);
  auto b = v.AsBytes();
  ASSERT_TRUE(b.ok());
  ASSERT_EQ(b->size(), 3u);
  EXPECT_EQ(static_cast<uint8_t>((*b)[0]), 0x00);
  EXPECT_EQ(static_cast<uint8_t>((*b)[1]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>((*b)[2]), 0x02);
}

// ——— Determinism + isolation ———

TEST_F(InstanceEvalTest, EvalIsDeterministicAcrossManyCalls) {
  // Re-evaluating the same Instance many times must produce the
  // same Value — $eval's first instruction is a baked-in cel_reset
  // call so the arena is fresh every time.
  auto prog_or = compiler_->Compile(R"("hello")");
  ASSERT_TRUE(prog_or.ok());
  auto inst_or = engine_->Plan(*prog_or);
  ASSERT_TRUE(inst_or.ok());
  Instance inst = *std::move(inst_or);
  for (int i = 0; i < 16; ++i) {
    auto v_or = inst.Eval();
    ASSERT_TRUE(v_or.ok()) << "iter " << i << ": " << v_or.status();
    ASSERT_EQ(v_or->kind(), Value::Kind::kString) << "iter " << i;
    auto s = v_or->AsString();
    ASSERT_TRUE(s.ok()) << "iter " << i;
    EXPECT_EQ(*s, "hello") << "iter " << i;
  }
}

TEST_F(InstanceEvalTest, TwoInstancesEvaluateIndependently) {
  // Two Instances from the same Program (or two different Programs)
  // each have their own host-allocated memory; eval'ing one
  // doesn't perturb the other.  This is the smoke-test invariant
  // re-verified at the api/ level.
  auto p_a = compiler_->Compile("42");
  auto p_b = compiler_->Compile(R"("world")");
  ASSERT_TRUE(p_a.ok());
  ASSERT_TRUE(p_b.ok());
  auto a_or = engine_->Plan(*p_a);
  auto b_or = engine_->Plan(*p_b);
  ASSERT_TRUE(a_or.ok());
  ASSERT_TRUE(b_or.ok());
  Instance inst_a = *std::move(a_or);
  Instance inst_b = *std::move(b_or);

  // Eval interleaved.
  auto v_a1 = inst_a.Eval();
  auto v_b1 = inst_b.Eval();
  auto v_a2 = inst_a.Eval();
  ASSERT_TRUE(v_a1.ok() && v_b1.ok() && v_a2.ok());
  ASSERT_EQ(v_a1->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v_a1->AsInt(), 42);
  ASSERT_EQ(v_b1->kind(), Value::Kind::kString);
  EXPECT_EQ(*v_b1->AsString(), "world");
  ASSERT_EQ(v_a2->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v_a2->AsInt(), 42);
}

// --- M2.C.5 kSelect e2e: Customer.age read through Layer 3 ---------
//
// First green path where wasm calls back into host: compile
// `c.age` → Engine::Plan wires cel_host.cel_get_field →
// Instance::Eval(activation) marshals Customer backing into the
// externref table → $eval hits the trampoline, ProtoBacking reads
// the int field, result surfaces as a Value::Int.

// Helper: compile + plan `expr` against a Customer-declaring
// compiler.  Returns the live Instance ready for Eval(activation).
// Customer/Address lookup goes through the process-wide generated
// descriptor pool; `google::protobuf::LinkMessageReflection<T>()`
// in the caller forces static-init registration of T.
Instance PlanAgainstCustomer(Engine& engine, absl::string_view expr) {
  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  auto compiler_or = std::move(builder).Build();
  ABSL_CHECK_OK(compiler_or);
  auto prog_or = compiler_or->Compile(expr);
  ABSL_CHECK_OK(prog_or) << expr;
  auto inst_or = engine.Plan(*prog_or);
  ABSL_CHECK_OK(inst_or) << expr;
  return *std::move(inst_or);
}

TEST(InstanceSelectEvalTest, IntFieldOnMessageRoundTrips) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "c.age");

  celwasm::testdata::Customer c;
  c.set_age(42);
  Activation act;
  act.Bind("c", Value::Message(c));
  auto val_or = inst.Eval(act);
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  EXPECT_EQ(*val_or->AsInt(), 42);
}

TEST(InstanceSelectEvalTest, BoolFieldOnMessageRoundTrips) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "c.is_premium");

  celwasm::testdata::Customer c;
  c.set_is_premium(true);
  Activation act;
  act.Bind("c", Value::Message(c));
  auto val_or = inst.Eval(act);
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  EXPECT_EQ(*val_or->AsBool(), true);
}

// has(c.billing_address) dispatches via cel_host.cel_has_field.
// The SelectExpr's test_only flag routes expr_lower to the
// has-field trampoline instead of get-field; Layer 2's HasField
// calls ProtoBacking::HasField which applies proto3 presence.
TEST(InstanceSelectEvalTest, HasMessageFieldSetReturnsTrue) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "has(c.billing_address)");

  celwasm::testdata::Customer c;
  c.mutable_billing_address()->set_city("Seattle");  // presence
  Activation act;
  act.Bind("c", Value::Message(c));
  auto val_or = inst.Eval(act);
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  EXPECT_EQ(*val_or->AsBool(), true);
}

TEST(InstanceSelectEvalTest, HasMessageFieldUnsetReturnsFalse) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "has(c.billing_address)");

  celwasm::testdata::Customer c;  // default-constructed; no billing_address
  Activation act;
  act.Bind("c", Value::Message(c));
  auto val_or = inst.Eval(act);
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  EXPECT_EQ(*val_or->AsBool(), false);
}

// Nested select: c.billing_address.city reads a nested sub-backing.
// The inner cel_get_field interns a fresh ProtoBacking (the
// address); the outer reads `city` off it.  String marshal exercises
// WasmtimeArenaAllocator via the runtime's cel_alloc.
TEST(InstanceSelectEvalTest, NestedSelectReadsSubBackingString) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "c.billing_address.city");

  celwasm::testdata::Customer c;
  c.mutable_billing_address()->set_city("Seattle");
  Activation act;
  act.Bind("c", Value::Message(c));
  auto val_or = inst.Eval(act);
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  ASSERT_EQ(val_or->kind(), Value::Kind::kString);
  EXPECT_EQ(*val_or->AsString(), "Seattle");
}

// --- M2.E PartialEval: unknown-pattern matching --------------------

TEST(InstancePartialEvalTest, MatchingPatternAbsorbsSelectToUnknown) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "c.billing_address");

  celwasm::testdata::Customer c;
  c.mutable_billing_address()->set_city("Seattle");
  Activation act;
  act.Bind("c", Value::Message(c));

  auto pat = AttributePattern::Parse("c.billing_address");
  ASSERT_TRUE(pat.ok()) << pat.status();
  AttributePattern patterns[] = {*std::move(pat)};
  auto val_or = inst.PartialEval(act, patterns);
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  EXPECT_TRUE(val_or->IsUnknown());
}

TEST(InstancePartialEvalTest, NonMatchingPatternFallsThroughToRealValue) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "c.age");

  celwasm::testdata::Customer c;
  c.set_age(30);
  Activation act;
  act.Bind("c", Value::Message(c));

  // Pattern matches a different field (name), not age — so c.age
  // reads normally and returns the bound value.
  auto pat = AttributePattern::Parse("c.name");
  ASSERT_TRUE(pat.ok()) << pat.status();
  AttributePattern patterns[] = {*std::move(pat)};
  auto val_or = inst.PartialEval(act, patterns);
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  EXPECT_EQ(*val_or->AsInt(), 30);
}

TEST(InstancePartialEvalTest, WildcardPatternMatchesAnyFieldUnderRoot) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "c.age");

  celwasm::testdata::Customer c;
  c.set_age(30);
  Activation act;
  act.Bind("c", Value::Message(c));

  auto pat = AttributePattern::Parse("c.*");
  ASSERT_TRUE(pat.ok()) << pat.status();
  AttributePattern patterns[] = {*std::move(pat)};
  EXPECT_TRUE(inst.PartialEval(act, patterns)->IsUnknown());
}

TEST(InstancePartialEvalTest, EmptyPatternSetBehavesLikeEval) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanAgainstCustomer(*engine_or, "c.age");

  celwasm::testdata::Customer c;
  c.set_age(30);
  Activation act;
  act.Bind("c", Value::Message(c));

  auto val_or = inst.PartialEval(act, {});
  ASSERT_TRUE(val_or.ok()) << val_or.status();
  EXPECT_EQ(*val_or->AsInt(), 30);
}

// ────────────────────────────────────────────────────────────────────
// M3 — `Instance::Eval` decoder grew a `CEL_MAP_ARENA` arm.  These
// tests exercise the decoder by evaluating a map-producing
// expression and asserting the host-side `cel::Value::Map` round-
// trips correctly: size matches, key kind matches, value matches
// per entry.  Both the literal/index path (scalar value) and the
// pure-literal path (map value) flow through `DecodeArenaMapAt`.
// ────────────────────────────────────────────────────────────────────

TEST_F(InstanceEvalTest, EvalsEmptyMapLiteralReturnsEmptyMap) {
  Value v = EvalLiteral("{}");
  ASSERT_EQ(v.kind(), Value::Kind::kMap);
  auto bk_or = v.MapBacking();
  ASSERT_TRUE(bk_or.ok()) << bk_or.status();
  EXPECT_EQ((*bk_or)->Size(), 0u);
}

TEST_F(InstanceEvalTest, EvalsScalarMapLiteralRoundTrips) {
  Value v = EvalLiteral(R"({"a": 1, "b": 2})");
  ASSERT_EQ(v.kind(), Value::Kind::kMap);
  auto bk_or = v.MapBacking();
  ASSERT_TRUE(bk_or.ok()) << bk_or.status();
  const auto* backing = *bk_or;
  EXPECT_EQ(backing->Size(), 2u);

  // Order-agnostic key-by-key probe — `cel_map_insert` preserves
  // insertion order in the arena, but the decoder doesn't promise
  // ordering on the host side, so iterate via ForEach and bucket
  // by key.
  int hit_a = -1;
  int hit_b = -1;
  backing->ForEach([&](const Value& k, const Value& val) {
    if (k.kind() == Value::Kind::kString && *k.AsString() == "a") {
      ASSERT_EQ(val.kind(), Value::Kind::kInt);
      hit_a = static_cast<int>(*val.AsInt());
    } else if (k.kind() == Value::Kind::kString && *k.AsString() == "b") {
      ASSERT_EQ(val.kind(), Value::Kind::kInt);
      hit_b = static_cast<int>(*val.AsInt());
    }
  });
  EXPECT_EQ(hit_a, 1);
  EXPECT_EQ(hit_b, 2);
}

TEST_F(InstanceEvalTest, EvalsLiteralMapIndexingProducesScalarValue) {
  Value v = EvalLiteral(R"({"a": 1, "b": 2}["b"])");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 2);
}

TEST_F(InstanceEvalTest, EvalsMapLiteralOverEveryAllowedKeyKind) {
  // langdef map keys: bool / int / uint / string.  Index a literal
  // map of each key kind by the matching literal and assert the
  // round-trip lands.  Cross-type numeric (int↔uint) is exercised
  // by `host_map_test.cc` + the cross-type runtime test; here we
  // only pin same-kind hits to make sure the codegen + decoder
  // agrees on the wire form per key kind.
  struct Case {
    const char* expr;
    int64_t expected;
  };
  for (const Case& c : {
           Case{R"({1: 10, 2: 20}[2])", 20},
           Case{R"({1u: 10, 2u: 20}[2u])", 20},
           Case{R"({"x": 10, "y": 20}["y"])", 20},
           Case{R"({true: 10, false: 20}[false])", 20},
       }) {
    Value v = EvalLiteral(c.expr);
    ASSERT_EQ(v.kind(), Value::Kind::kInt) << c.expr;
    EXPECT_EQ(*v.AsInt(), c.expected) << c.expr;
  }
}

}  // namespace
}  // namespace cel
