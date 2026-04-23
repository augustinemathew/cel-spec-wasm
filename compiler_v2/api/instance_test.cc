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
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

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

}  // namespace
}  // namespace cel
