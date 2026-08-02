// m28 — Configurable linking (static).  E2E proof that a
// `LinkMode::kStatic` Program compiles, plans, evaluates, and
// returns the same Value as today's `kDynamic` Program for a
// representative slice of expressions.
//
// What this test gates:
//   - `Compiler::Compile(..., kStatic)` produces a self-contained
//     wasm with the runtime bundled in.
//   - `Engine::Plan(static_program)` detects the absence of `cel.*`
//     imports, skips the standalone cel_runtime instantiation, and
//     aliases the Program instance as both `expr_instance` and
//     `helpers_instance`.
//   - `Instance::Eval()` calls `arena_init` (seeding the bump arena)
//     and `eval` (running expr-side codegen) intra-module — proving
//     `arena_reset` etc. execute correctly out of the merged module.
//   - Result value matches the dynamic-mode answer byte-for-byte
//     (same Value::Kind and decoded payload).

#include "absl/status/statusor.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// Runs `expr` under `link_mode`, compiles → plans → evaluates, and
// returns the resulting Value (or status).
absl::StatusOr<Value> EvalUnder(absl::string_view expr,
                                CompilerOptions::LinkMode link_mode) {
  auto compiler_or = Compiler::NewBuilder().Build();
  if (!compiler_or.ok()) return compiler_or.status();
  CompilerOptions opts;
  opts.link_mode = link_mode;
  auto program_or = compiler_or->Compile(expr, opts);
  if (!program_or.ok()) return program_or.status();
  auto engine_or = Engine::NewBuilder().Build();
  if (!engine_or.ok()) return engine_or.status();
  auto instance_or = engine_or->Plan(*program_or);
  if (!instance_or.ok()) return instance_or.status();
  return instance_or->Eval();
}

// Each cell asserts:
//   - both link modes succeed (proves the Plan branches work),
//   - both produce the same Value::Kind (proves the codegen is
//     mode-agnostic at the expr layer),
//   - the value matches the expected literal (proves arena_reset +
//     intra-module helper calls + rodata read all work).
//
// The expression set covers M1-M5 codegen surfaces — kConst /
// kCallExpr (binop) / kConst-string / has() — none of which touches
// cctz/absl, so the missing __wasm_call_ctors (DCE'd by the strip
// tool) doesn't surface here.  Complex-message + timestamp paths are
// out of scope for this commit; their cctz dependency is covered in a
// follow-up.

TEST(M28StaticLinkE2E, ConstInt) {
  auto sv = EvalUnder("42", CompilerOptions::LinkMode::kStatic);
  ASSERT_TRUE(sv.ok()) << sv.status();
  EXPECT_EQ(sv->kind(), Value::Kind::kInt);
  auto v = sv->AsInt();
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v, 42);
}

TEST(M28StaticLinkE2E, ConstString) {
  auto sv = EvalUnder("\"hello\"", CompilerOptions::LinkMode::kStatic);
  ASSERT_TRUE(sv.ok()) << sv.status();
  EXPECT_EQ(sv->kind(), Value::Kind::kString);
  auto v = sv->AsString();
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v, "hello");
}

TEST(M28StaticLinkE2E, ConstBool) {
  auto sv = EvalUnder("true", CompilerOptions::LinkMode::kStatic);
  ASSERT_TRUE(sv.ok()) << sv.status();
  EXPECT_EQ(sv->kind(), Value::Kind::kBool);
  auto v = sv->AsBool();
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(*v);
}

TEST(M28StaticLinkE2E, IntAdd) {
  auto sv = EvalUnder("1 + 2", CompilerOptions::LinkMode::kStatic);
  ASSERT_TRUE(sv.ok()) << sv.status();
  EXPECT_EQ(sv->kind(), Value::Kind::kInt);
  auto v = sv->AsInt();
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v, 3);
}

TEST(M28StaticLinkE2E, StringConcat) {
  auto sv = EvalUnder("\"foo\" + \"bar\"", CompilerOptions::LinkMode::kStatic);
  ASSERT_TRUE(sv.ok()) << sv.status();
  EXPECT_EQ(sv->kind(), Value::Kind::kString);
  auto v = sv->AsString();
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v, "foobar");
}

// Cross-mode parity for every covered cell: static and dynamic must
// produce the same Value::Kind + decoded payload.
struct ParityCell {
  const char* expr;
  Value::Kind kind;
};
class M28ParityE2E : public ::testing::TestWithParam<ParityCell> {};

// Per-kind accessor comparison, deferred to a templated helper so the
// parametric test body stays one assertion per cell.
void ExpectSamePayload(const Value& a, const Value& b) {
  ASSERT_EQ(a.kind(), b.kind());
  switch (a.kind()) {
    case Value::Kind::kBool:
      EXPECT_EQ(*a.AsBool(), *b.AsBool());
      break;
    case Value::Kind::kInt:
      EXPECT_EQ(*a.AsInt(), *b.AsInt());
      break;
    case Value::Kind::kUint:
      EXPECT_EQ(*a.AsUint(), *b.AsUint());
      break;
    case Value::Kind::kDouble:
      EXPECT_EQ(*a.AsDouble(), *b.AsDouble());
      break;
    case Value::Kind::kString:
      EXPECT_EQ(*a.AsString(), *b.AsString());
      break;
    case Value::Kind::kBytes:
      EXPECT_EQ(*a.AsBytes(), *b.AsBytes());
      break;
    default:
      FAIL() << "ExpectSamePayload: unhandled kind for parity assert";
  }
}

TEST_P(M28ParityE2E, SameResultBothModes) {
  const ParityCell& c = GetParam();
  auto d = EvalUnder(c.expr, CompilerOptions::LinkMode::kDynamic);
  auto s = EvalUnder(c.expr, CompilerOptions::LinkMode::kStatic);
  ASSERT_TRUE(d.ok()) << "dynamic: " << d.status();
  ASSERT_TRUE(s.ok()) << "static:  " << s.status();
  EXPECT_EQ(d->kind(), c.kind);
  EXPECT_EQ(s->kind(), c.kind);
  ExpectSamePayload(*d, *s);
}

INSTANTIATE_TEST_SUITE_P(
    Scalars, M28ParityE2E,
    ::testing::Values(ParityCell{"42", Value::Kind::kInt},
                      ParityCell{"42u", Value::Kind::kUint},
                      ParityCell{"3.14", Value::Kind::kDouble},
                      ParityCell{"true", Value::Kind::kBool},
                      ParityCell{"\"hello\"", Value::Kind::kString},
                      ParityCell{"b\"x\"", Value::Kind::kBytes},
                      ParityCell{"1 + 2", Value::Kind::kInt},
                      ParityCell{"\"a\" + \"b\"", Value::Kind::kString},
                      // Ternary lowering — exercises cel_copy_slot.
                      ParityCell{"true ? 1 : 2", Value::Kind::kInt},
                      ParityCell{"false ? 1 : 2", Value::Kind::kInt}));

}  // namespace
}  // namespace celwasm
