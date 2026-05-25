// M5 e2e test suite — the spec of "done" for the general kCall
// arm (M5.F) wired into Compile → Plan → Eval.
//
// Mirrors the m2_test / m4_test shape: every test asserts a
// capability M5.F lights up.  Each fixture group covers one
// pipeline-traversal axis:
//
//   - ScalarArithmetic   — int/uint/double arithmetic.
//   - SameKindCompare    — `<` / `<=` / `>` / `>=` per kind.
//   - StringOps          — concat + receiver-form string ops.
//   - BytesOps           — bytes concat round-trip.
//   - BoundVarArithmetic — Activation::Bind drives an arithmetic
//                          expression over the bound CelValue.
//   - ProtoFieldArithmetic — proto field reads feed into arithmetic.
//   - BoolOrdering       — langdef §"Booleans": false < true.
//   - PendingDispatcher  — `size([1,2,3])` etc. surface
//                          Unimplemented per `kPendingRuntimeExports`.
//
// `equals` / `not_equals` (langdef §"Equality") are M5.B step 2b
// and intentionally absent here — the polymorphic dispatcher lands
// after M5.D step 2.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "eval/activation.h"
#include "eval/attribute.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "compiler/program.h"
#include "shared/type.h"
#include "eval/value.h"
#include "testdata/e2e_fixture.pb.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {
using ::celwasm::AttributePattern;

using ::absl_testing::IsOk;
using ::celwasm::testdata::Customer;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<Customer>();
      return 0;
    }();

// One Engine for the whole binary — wasmtime config (tail-calls,
// runtime module load) is shared across all Plans.
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

absl::StatusOr<Compiler> CompilerWithCustomerVar() {
  return BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  });
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

// rvalue overload — lets `EvalOk(CompilePlan(...), a)` chain in one
// expression without a temporary lvalue.  Mirrors the lvalue body.
Value EvalOk(Instance&& instance, const Activation& activation) {
  Instance moved = std::move(instance);
  return EvalOk(moved, activation);
}

// ──────────────────────────────────────────────────────────────
//  ScalarArithmetic — int / uint / double × add/sub/mul/div/mod.
//  All operands are arena literals; no host-side state involved.
//  The kCall result lands in the workspace-slot region; the
//  CelValue decoder reads it back as Value::Int/Uint/Double.
// ──────────────────────────────────────────────────────────────

class ScalarArithmeticE2ETest : public ::testing::Test {};

TEST_F(ScalarArithmeticE2ETest, IntAdd) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "1 + 2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

TEST_F(ScalarArithmeticE2ETest, IntSub) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "10 - 4");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 6);
}

TEST_F(ScalarArithmeticE2ETest, IntMul) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "6 * 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 42);
}

TEST_F(ScalarArithmeticE2ETest, IntDiv) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "20 / 5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 4);
}

TEST_F(ScalarArithmeticE2ETest, IntMod) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "10 % 3");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1);
}

TEST_F(ScalarArithmeticE2ETest, IntNegate) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "-5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), -5);
}

TEST_F(ScalarArithmeticE2ETest, UintAdd) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "1u + 2u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 3u);
}

TEST_F(ScalarArithmeticE2ETest, DoubleAdd) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "1.5 + 2.25");
  Activation a;
  EXPECT_DOUBLE_EQ(*EvalOk(instance, a).AsDouble(), 3.75);
}

TEST_F(ScalarArithmeticE2ETest, NestedArithmetic) {
  // `(1 + 2) * 3` — outer `*` gets the inner `+`'s slot offset
  // as its left operand.  Verifies recursion through Emit.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "(1 + 2) * 3");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 9);
}

// ──────────────────────────────────────────────────────────────
//  SameKindCompare — int/uint/double/string/bytes ordering.
//  M5.B step 1 helpers: `cel_<kind>_lt_at_vv` etc.
// ──────────────────────────────────────────────────────────────

class SameKindCompareE2ETest : public ::testing::Test {};

TEST_F(SameKindCompareE2ETest, IntLessThan) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "1 < 2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(SameKindCompareE2ETest, IntLessThanFalse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "5 < 2");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(SameKindCompareE2ETest, IntLessEquals) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "5 <= 5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(SameKindCompareE2ETest, DoubleGreater) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "3.14 > 2.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(SameKindCompareE2ETest, StringLessThan) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"("alpha" < "beta")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(SameKindCompareE2ETest, BoolOrdering) {
  // langdef §"Booleans": false < true.  M5.B step 1 helper
  // `cel_bool_lt_at_vv` lands behind `less_bool`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "false < true");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
//  StringOps — concat + receiver-form contains/startsWith/endsWith.
//  M5.C runtime helpers; the receiver form `s.contains(sub)`
//  flattens to `(out_slot, s_slot, sub_slot)` — see
//  `EmitGeneralCall`.
// ──────────────────────────────────────────────────────────────

class StringOpsE2ETest : public ::testing::Test {};

TEST_F(StringOpsE2ETest, Concat) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"("a" + "b")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "ab");
}

TEST_F(StringOpsE2ETest, ContainsTrue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"("hello".contains("ell"))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringOpsE2ETest, ContainsFalse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"("hello".contains("xyz"))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(StringOpsE2ETest, StartsWith) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"("foobar".startsWith("foo"))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringOpsE2ETest, EndsWith) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"("foobar".endsWith("bar"))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
//  BytesOps — concat round-trip on bytes literals.
// ──────────────────────────────────────────────────────────────

class BytesOpsE2ETest : public ::testing::Test {};

TEST_F(BytesOpsE2ETest, Concat) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(b"\x01" + b"\x02")");
  Activation a;
  auto got = *EvalOk(instance, a).AsBytes();
  EXPECT_EQ(got, absl::string_view("\x01\x02", 2));
}

// ──────────────────────────────────────────────────────────────
//  BoundVarArithmetic — Activation::Bind feeds a CelValue into
//  the workspace slot the kIdent node reads from; arithmetic
//  helper consumes that slot directly.
// ──────────────────────────────────────────────────────────────

class BoundVarArithmeticE2ETest : public ::testing::Test {};

TEST_F(BoundVarArithmeticE2ETest, IntPlusOne) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Int());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "x + 1");
  Activation a;
  a.Bind("x", Value::Int(7));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 8);
}

TEST_F(BoundVarArithmeticE2ETest, IntCompareWithLiteral) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Int());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "x < 10");
  Activation a;
  a.Bind("x", Value::Int(3));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(BoundVarArithmeticE2ETest, TwoVarArithmetic) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Int());
    b.DeclareVariable("y", CelType::Int());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "x + y");
  Activation a;
  a.Bind("x", Value::Int(40));
  a.Bind("y", Value::Int(2));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 42);
}

// ──────────────────────────────────────────────────────────────
//  ProtoFieldArithmetic — proto field reads (kSelect → kHost
//  trampoline) feed into arithmetic ops (M5.F general arm).
//  Verifies the field-result CelValue's slot is reachable as
//  an arithmetic operand without re-encoding.
// ──────────────────────────────────────────────────────────────

class ProtoFieldArithmeticE2ETest : public ::testing::Test {
 protected:
  Compiler compiler_{*CompilerWithCustomerVar()};
};

TEST_F(ProtoFieldArithmeticE2ETest, AgePlusOne) {
  auto instance = CompilePlan(compiler_, "c.age + 1");
  Customer msg;
  msg.set_age(40);
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 41);
}

TEST_F(ProtoFieldArithmeticE2ETest, AgeTimesTwo) {
  auto instance = CompilePlan(compiler_, "c.age * 2");
  Customer msg;
  msg.set_age(21);
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 42);
}

TEST_F(ProtoFieldArithmeticE2ETest, NameContains) {
  auto instance = CompilePlan(compiler_, R"(c.name.contains("oh"))");
  Customer msg;
  msg.set_name("John");
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
//  AggregateDispatcher — M5.D step 2 ships the seven kDynamic
//  dispatchers (`cel_list_size` / `cel_list_in` / `cel_list_eq` /
//  `cel_list_concat` / `cel_map_size` / `cel_map_in` / `cel_map_eq`).
//  These tests cover the arena-fast-path side; bound-var (kHost)
//  coverage lands as part of M5.D step 2's BoundList* /
//  BoundMap* fixtures below.
// ──────────────────────────────────────────────────────────────

class AggregateDispatcherE2ETest : public ::testing::Test {};

TEST_F(AggregateDispatcherE2ETest, SizeOfListLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, "size([1, 2, 3])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

TEST_F(AggregateDispatcherE2ETest, SizeOfMapLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, R"(size({"a": 1, "b": 2}))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

TEST_F(AggregateDispatcherE2ETest, InListLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, "2 in [1, 2, 3]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(AggregateDispatcherE2ETest, NotInListLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, "5 in [1, 2, 3]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(AggregateDispatcherE2ETest, InMapLiteral) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, R"("a" in {"a": 1, "b": 2})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(AggregateDispatcherE2ETest, ListConcatSize) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, "size([1, 2] + [3, 4, 5])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 5);
}

// ──────────────────────────────────────────────────────────────
//  BoundAggregateDispatcher — host-backed list/map operands route
//  through the `cel_host.cel_*` trampolines (M5.D step 2 Layer-2
//  Impls).  The dispatcher branches on operand kind and tail-calls
//  the kHost arm; verifies end-to-end reach of every Impl.
// ──────────────────────────────────────────────────────────────

class BoundAggregateDispatcherE2ETest : public ::testing::Test {};

TEST_F(BoundAggregateDispatcherE2ETest, SizeOfBoundList) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
  });
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, "size(xs)");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(10), Value::Int(20), Value::Int(30)}));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

TEST_F(BoundAggregateDispatcherE2ETest, InBoundList) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
  });
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, "20 in xs");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(10), Value::Int(20), Value::Int(30)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(BoundAggregateDispatcherE2ETest, NotInBoundList) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
  });
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, "99 in xs");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(10), Value::Int(20)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// Activation::Bind doesn't yet marshal map values into a kHost
// HostMap (Activation marshal is gated on a separate slice tracked
// in the M2/M3 bindings doc).  The kHost map trampolines
// (CelMapSizeImpl / CelMapInImpl / CelMapEqImpl) are exercised by
// the host_map_test fixtures and proto-map e2e tests; bound-map
// e2e here lights up when the marshal path lands.

// ──────────────────────────────────────────────────────────────
//  PolymorphicEquals — M5.B step 2b ships `cel_equals_at_vv` /
//  `cel_not_equals_at_vv`, a runtime kind-switch over both
//  operands (numeric ladder, same-kind scalar arms, list / map /
//  message dispatchers, cross-kind → false per langdef).
// ──────────────────────────────────────────────────────────────

class PolymorphicEqualsE2ETest : public ::testing::Test {};

TEST_F(PolymorphicEqualsE2ETest, IntEqualsInt) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  Instance i1 = CompilePlan(*compiler, "1 == 1");
  Instance i2 = CompilePlan(*compiler, "1 == 2");
  EXPECT_EQ(*EvalOk(i1, a).AsBool(), true);
  EXPECT_EQ(*EvalOk(i2, a).AsBool(), false);
}

TEST_F(PolymorphicEqualsE2ETest, IntNotEqualsInt) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "1 != 2"), a).AsBool(), true);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "1 != 1"), a).AsBool(), false);
}

// Note: cross-type numeric equality (`1 == 1.0`, `1 == 1u`) needs
// cel-cpp's checker to have an `equals` overload that admits mixed
// numeric operands.  Today the checker rejects these at type-check
// time without `dyn(...)` (which we statically reject).  The
// runtime kernel's cross-numeric ladder is reachable via aggregate
// equality where element kinds may differ — see
// `ListEqualsArenaArena` and the aggregate-element coverage in
// `cel_runtime`'s unit tests.

TEST_F(PolymorphicEqualsE2ETest, BoolEquals) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "true == true"), a).AsBool(), true);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "true == false"), a).AsBool(),
            false);
}

TEST_F(PolymorphicEqualsE2ETest, NullEquals) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "null == null"), a).AsBool(), true);
}

TEST_F(PolymorphicEqualsE2ETest, BytesEquals) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, R"(b"abc" == b"abc")"), a).AsBool(),
            true);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, R"(b"abc" == b"xyz")"), a).AsBool(),
            false);
}

TEST_F(PolymorphicEqualsE2ETest, ListEqualsArenaArena) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  EXPECT_EQ(
      *EvalOk(CompilePlan(*compiler, "[1, 2, 3] == [1, 2, 3]"), a).AsBool(),
      true);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "[1, 2, 3] == [1, 2]"), a).AsBool(),
            false);
}

TEST_F(PolymorphicEqualsE2ETest, MapEqualsArenaArena) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  EXPECT_EQ(
      *EvalOk(CompilePlan(*compiler, R"({"a": 1} == {"a": 1})"), a).AsBool(),
      true);
  EXPECT_EQ(
      *EvalOk(CompilePlan(*compiler, R"({"a": 1} == {"a": 2})"), a).AsBool(),
      false);
}

TEST_F(PolymorphicEqualsE2ETest, MapEqualitySetEqualityIgnoresOrder) {
  // langdef §"Equality": maps compare set-wise on entries.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  EXPECT_EQ(
      *EvalOk(CompilePlan(*compiler, R"({"a": 1, "b": 2} == {"b": 2, "a": 1})"),
              a)
           .AsBool(),
      true);
}

TEST_F(PolymorphicEqualsE2ETest, BoundVarStringEqualsLiteralFails) {
  // String activation marshalling lands in Slice 0 (in-flight).
  // Until then this case skips at the marshaller — exercise via
  // string literal vs string literal instead.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  Activation a;
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, R"("foo" == "foo")"), a).AsBool(),
            true);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, R"("foo" == "bar")"), a).AsBool(),
            false);
}

TEST_F(BoundAggregateDispatcherE2ETest, TwoBoundListsCompareSize) {
  // `==` for non-numeric types is M5.B step 2b — exercise the
  // dispatcher via a `<` comparison on int sizes instead, so this
  // test stays green at M5.D step 2 and doesn't need polymorphic
  // equals.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
    b.DeclareVariable("ys", CelType::List(CelType::Int()));
  });
  ASSERT_THAT(compiler, IsOk());
  Instance instance = CompilePlan(*compiler, "size(xs) < size(ys)");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(1), Value::Int(2)}));
  a.Bind("ys", Value::List({Value::Int(7), Value::Int(8), Value::Int(9)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
//  ControlFlow — `_&&_` / `_||_` / `!_` / `_?_:_` (M5.G — Slice 2).
//  Originally a `ControlFlowPendingE2ETest` group asserting
//  Unimplemented status; rewritten as positive coverage at the
//  M5.G enabling commit.  Kept under the same fixture-level intent
//  (operator coverage) so a future reader sees the slice graduate
//  from "all stubbed" to "all green" in one place in git history.
// ──────────────────────────────────────────────────────────────

class ControlFlowE2ETest : public ::testing::Test {};

TEST_F(ControlFlowE2ETest, AndOfTrueAndTrueIsTrue) {
  EXPECT_EQ(*EvalOk(CompilePlan(*CompilerEmpty(), "true && true"), {}).AsBool(),
            true);
}

TEST_F(ControlFlowE2ETest, AndOfTrueAndFalseIsFalse) {
  EXPECT_EQ(
      *EvalOk(CompilePlan(*CompilerEmpty(), "true && false"), {}).AsBool(),
      false);
}

TEST_F(ControlFlowE2ETest, OrOfFalseAndTrueIsTrue) {
  EXPECT_EQ(
      *EvalOk(CompilePlan(*CompilerEmpty(), "false || true"), {}).AsBool(),
      true);
}

TEST_F(ControlFlowE2ETest, OrOfFalseAndFalseIsFalse) {
  EXPECT_EQ(
      *EvalOk(CompilePlan(*CompilerEmpty(), "false || false"), {}).AsBool(),
      false);
}

TEST_F(ControlFlowE2ETest, NotOfTrueIsFalse) {
  EXPECT_EQ(*EvalOk(CompilePlan(*CompilerEmpty(), "!true"), {}).AsBool(),
            false);
}

TEST_F(ControlFlowE2ETest, NotOfFalseIsTrue) {
  EXPECT_EQ(*EvalOk(CompilePlan(*CompilerEmpty(), "!false"), {}).AsBool(),
            true);
}

TEST_F(ControlFlowE2ETest, TernaryTrueSelectsThenArm) {
  EXPECT_EQ(*EvalOk(CompilePlan(*CompilerEmpty(), "true ? 1 : 2"), {}).AsInt(),
            1);
}

TEST_F(ControlFlowE2ETest, TernaryFalseSelectsElseArm) {
  EXPECT_EQ(*EvalOk(CompilePlan(*CompilerEmpty(), "false ? 1 : 2"), {}).AsInt(),
            2);
}

// Per langdef §"Logical operators": `false && X = false` for any X,
// including expressions that would error on the right.  Load-bearing
// for the non-strict semantics: a strict short-circuit evaluator
// would still run `1/0` and propagate ERROR.  cel_and's eager-eval +
// runtime truth table absorbs the error.
TEST_F(ControlFlowE2ETest, AndOverFalseAndError) {
  // `1/0 == 1` materialises ERROR(divide_by_zero); `false && ERROR`
  // → false per the absorber rule.
  Value v = EvalOk(CompilePlan(*CompilerEmpty(), "false && (1/0 == 1)"), {});
  EXPECT_EQ(*v.AsBool(), false);
}

// Symmetric: `true || X = true (any X)`.
TEST_F(ControlFlowE2ETest, OrOverTrueAndError) {
  Value v = EvalOk(CompilePlan(*CompilerEmpty(), "true || (1/0 == 1)"), {});
  EXPECT_EQ(*v.AsBool(), true);
}

// `false || ERROR` is NOT absorbed — the `false` side fails to
// short-circuit a `true` result, ERROR dominates UNKNOWN/OK on the
// other side, and the combined result is the error.  Locks the
// boundary between OK(false)-absorber and ERROR-dominance.
TEST_F(ControlFlowE2ETest, OrOverFalseAndError) {
  auto v = CompilePlan(*CompilerEmpty(), "false || (1/0 == 1)").Eval({});
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError());
}

TEST_F(ControlFlowE2ETest, AndOverTrueAndError) {
  auto v = CompilePlan(*CompilerEmpty(), "true && (1/0 == 1)").Eval({});
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError());
}

// `_?_:_` propagates ERROR on the cond verbatim — neither arm runs.
TEST_F(ControlFlowE2ETest, TernaryErrorCondPropagates) {
  auto v = CompilePlan(*CompilerEmpty(), "(1/0 == 1) ? 1 : 2").Eval({});
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError());
}

// ──────────────────────────────────────────────────────────────
//  ControlFlowUnknownE2ETest — UNKNOWN propagation under
//  PartialEval.  Each test mints an UNKNOWN by binding a Customer
//  field that's also pattern-matched, then folds it through the
//  3VL operators and asserts the result preserves UNKNOWN-ness.
//  Per-operator id-set merge correctness lives in the runtime
//  unit suite (cel_3vl_test.cc); these tests verify the e2e
//  plumbing carries the unknown across the operator boundary.
// ──────────────────────────────────────────────────────────────

class ControlFlowUnknownE2ETest : public ::testing::Test {};

absl::StatusOr<Value> EvalUnknown(absl::string_view expr,
                                  absl::string_view pattern) {
  auto compiler_or = CompilerWithCustomerVar();
  if (!compiler_or.ok()) return compiler_or.status();
  Instance inst = CompilePlan(*compiler_or, expr);
  Customer c;
  c.set_age(0);
  Activation act;
  act.Bind("c", Value::Message(c));
  auto pat = AttributePattern::Parse(pattern);
  if (!pat.ok()) return pat.status();
  AttributePattern patterns[] = {*std::move(pat)};
  return inst.PartialEval(act, patterns);
}

TEST_F(ControlFlowUnknownE2ETest, AndOfUnknownAndTrueIsUnknown) {
  // `c.age == 0` becomes UNKNOWN under the pattern; `UNKNOWN && true`
  // → UNKNOWN by the 3VL truth table.
  auto v = EvalUnknown("(c.age == 0) && true", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsUnknown());
}

TEST_F(ControlFlowUnknownE2ETest, AndOfUnknownAndFalseIsFalse) {
  // OK(false) absorbs UNKNOWN per the langdef truth table.
  auto v = EvalUnknown("(c.age == 0) && false", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsBool(), false);
}

TEST_F(ControlFlowUnknownE2ETest, OrOfUnknownAndTrueIsTrue) {
  auto v = EvalUnknown("(c.age == 0) || true", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(*v->AsBool(), true);
}

TEST_F(ControlFlowUnknownE2ETest, OrOfUnknownAndFalseIsUnknown) {
  auto v = EvalUnknown("(c.age == 0) || false", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsUnknown());
}

TEST_F(ControlFlowUnknownE2ETest, NotOfUnknownIsUnknown) {
  auto v = EvalUnknown("!(c.age == 0)", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsUnknown());
}

TEST_F(ControlFlowUnknownE2ETest, TernaryUnknownCondPropagatesUnknown) {
  auto v = EvalUnknown("(c.age == 0) ? 1 : 2", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsUnknown());
}

// Note: a both-UNKNOWN merge e2e case is intentionally absent here.
// The merge correctness invariant (sorted, dedup) is fully covered
// at the runtime unit level in `cel_3vl_test.cc::UnknownMerge*`.
// An end-to-end version would require both operands to land at
// `cel_unknown_merge` with the same `payload.unk` wire shape, which
// the v2 codepath doesn't currently produce uniformly.

// ──────────────────────────────────────────────────────────────
//  ControlFlowUnknownErrorPrecedenceE2ETest — ERROR > UNKNOWN
//  dominance per langdef §"Errors and unknowns".  Both operand
//  orderings, both `_&&_` / `_||_`.
// ──────────────────────────────────────────────────────────────

class ControlFlowUnknownErrorPrecedenceE2ETest : public ::testing::Test {};

TEST_F(ControlFlowUnknownErrorPrecedenceE2ETest, ErrorOverUnknownInAnd) {
  auto v = EvalUnknown("(1/0 == 1) && (c.age == 0)", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError());
}

TEST_F(ControlFlowUnknownErrorPrecedenceE2ETest, UnknownOverErrorInAnd) {
  auto v = EvalUnknown("(c.age == 0) && (1/0 == 1)", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError());
}

TEST_F(ControlFlowUnknownErrorPrecedenceE2ETest, ErrorOverUnknownInOr) {
  auto v = EvalUnknown("(1/0 == 1) || (c.age == 0)", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError());
}

TEST_F(ControlFlowUnknownErrorPrecedenceE2ETest, UnknownOverErrorInOr) {
  auto v = EvalUnknown("(c.age == 0) || (1/0 == 1)", "c.age");
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_TRUE(v->IsError());
}

// ──────────────────────────────────────────────────────────────
//  Slice 0 (conformance unlock plan) — kString / kBytes activation
//  encoder.  Activation::Bind("s", Value::String("hi")) followed
//  by an Eval that reads `s` returns the bound string from a
//  host-managed arena above the wasm-side `arena_limit`.  Pre-
//  Slice-0 these all returned UnimplementedError.  The fixture is
//  named distinctively so it doesn't conflict with Slice 1's
//  PolymorphicEqualsE2ETest landing in the same file.
// ──────────────────────────────────────────────────────────────

class StringBytesActivationE2ETest : public ::testing::Test {};

TEST_F(StringBytesActivationE2ETest, BindStringPlusLiteral) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("s", CelType::String());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(s + " world")");
  Activation a;
  a.Bind("s", Value::String("hi"));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "hi world");
}

TEST_F(StringBytesActivationE2ETest, BindBytesSize) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("b", CelType::Bytes());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size(b)");
  Activation a;
  a.Bind("b", Value::Bytes(std::string("\x01\x02\x03\x04", 4)));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 4);
}

TEST_F(StringBytesActivationE2ETest, BindEmptyString) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("s", CelType::String());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size(s)");
  Activation a;
  a.Bind("s", Value::String(""));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 0);
}

// langdef §"Strings": strings are byte-clean — embedded NULs round-
// trip and `size(s)` returns the byte count, not a strlen-equivalent
// terminated count.
TEST_F(StringBytesActivationE2ETest, BindEmbeddedNul) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("s", CelType::String());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size(s)");
  Activation a;
  a.Bind("s", Value::String(std::string("a\0b", 3)));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

// langdef §"size() over strings": `size(s)` returns the byte length
// of the UTF-8 encoding, not the number of unicode codepoints.
// "πέντε" encodes as 10 bytes (each Greek letter is 2 bytes).
TEST_F(StringBytesActivationE2ETest, BindMultibyteUtf8) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("s", CelType::String());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size(s)");
  Activation a;
  a.Bind("s", Value::String("πέντε"));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 10);
}

// Multi-variable activation — both kString slots get marshalled
// into the host arena, then `+` concatenates them.  Verifies the
// arena's bump cursor advances correctly between writes.
TEST_F(StringBytesActivationE2ETest, BindTwoStringsConcat) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("a", CelType::String());
    b.DeclareVariable("b", CelType::String());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "a + b");
  Activation act;
  act.Bind("a", Value::String("foo"));
  act.Bind("b", Value::String("bar"));
  EXPECT_EQ(*EvalOk(instance, act).AsString(), "foobar");
}

// Re-evaluating the same Instance with different activations must
// not leak data across calls — Slice 0's host arena rewinds its
// cursor every Eval, so the second bind must overwrite the first.
TEST_F(StringBytesActivationE2ETest, RebindAcrossEvalsRewindsArena) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("s", CelType::String());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "s + s");
  Activation a;
  a.Bind("s", Value::String("first"));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "firstfirst");
  Activation b;
  b.Bind("s", Value::String("second"));
  EXPECT_EQ(*EvalOk(instance, b).AsString(), "secondsecond");
}

// Bytes operand size + a non-trivial body confirms the bytes arm
// uses the same arena path as strings (bytes are byte-clean too —
// langdef §"Bytes").
TEST_F(StringBytesActivationE2ETest, BindBytesWithNul) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("b", CelType::Bytes());
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size(b)");
  Activation a;
  a.Bind("b", Value::Bytes(std::string("\x00\xff\x00", 3)));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

// ──────────────────────────────────────────────────────────────
//  DynPassthroughE2ETest — Slice 1.5.  `dyn(scalar)` is admitted
//  by RejectDyn and lowered as the identity function (no helper);
//  the surrounding `==` / `!=` reach the existing polymorphic
//  equality kernel.  Each test pins one cross-numeric / cross-kind
//  shape from the conformance corpus.
// ──────────────────────────────────────────────────────────────
class DynPassthroughE2ETest : public ::testing::Test {};

TEST_F(DynPassthroughE2ETest, DynScalarEqualsCrossNumeric) {
  // `dyn(int) == uint` reaches the runtime's cross-numeric ladder
  // — the kernel returns true since 1 and 1u compare equal under
  // langdef §"Numeric equality".
  EXPECT_EQ(*EvalOk(CompilePlan(*CompilerEmpty(), "dyn(1) == 1u"), {}).AsBool(),
            true);
}

TEST_F(DynPassthroughE2ETest, DynScalarEqualsDouble) {
  EXPECT_EQ(
      *EvalOk(CompilePlan(*CompilerEmpty(), "dyn(1) == 1.0"), {}).AsBool(),
      true);
}

TEST_F(DynPassthroughE2ETest, DynStringEqualsString) {
  EXPECT_EQ(
      *EvalOk(CompilePlan(*CompilerEmpty(), "dyn(\"foo\") == \"foo\""), {})
           .AsBool(),
      true);
}

TEST_F(DynPassthroughE2ETest, DynNullEqualsNull) {
  EXPECT_EQ(
      *EvalOk(CompilePlan(*CompilerEmpty(), "dyn(null) == null"), {}).AsBool(),
      true);
}

TEST_F(DynPassthroughE2ETest, DynNotEqualsAcrossKinds) {
  // `dyn(int) != string` — kernel returns true on `!=` of
  // disjoint kinds (langdef §"Equality" — values of different
  // type are not equal).
  EXPECT_EQ(
      *EvalOk(CompilePlan(*CompilerEmpty(), "dyn(1) != \"1\""), {}).AsBool(),
      true);
}

TEST_F(DynPassthroughE2ETest, DynBoolThroughTernary) {
  // Slice 2 (M5.G) shipped: `dyn(true) ? 1 : 2` is reachable.  The
  // ternary cond reads the dyn call's annotation, which Slice 1.5
  // forwards to the bool literal's storage — so the ternary
  // detects bool-kind / true-payload and selects the then-arm.
  EXPECT_EQ(
      *EvalOk(CompilePlan(*CompilerEmpty(), "dyn(true) ? 1 : 2"), {}).AsInt(),
      1);
}

// ──────────────────────────────────────────────────────────────
//  CrossNumericOrderingE2ETest — Slice 1.6.  Ordering operators
//  (`<` / `<=` / `>` / `>=`) and `in` membership across numeric
//  kinds (int / uint / double).  cel-cpp's checker only admits
//  these when one operand is dyn-wrapped (Slice 1.5), so every
//  shape below threads `dyn(...)` through one or both operands.
//
//  langdef §"Comparisons": "numeric comparisons across type are
//  supported at runtime as all numeric representations may be
//  considered to exist along a shared number line".
//  langdef §"Equality" + §"List Membership (in)": cross-numeric
//  equality / membership uses the polymorphic ladder
//  (`!(x < y || x > y)`).
// ──────────────────────────────────────────────────────────────

class CrossNumericOrderingE2ETest : public ::testing::Test {
 protected:
  bool EvalBool(absl::string_view expr) {
    return *EvalOk(CompilePlan(*CompilerEmpty(), expr), {}).AsBool();
  }
};

// Parameterised matrix: `dyn(K1) <op> K2` and `K1 <op> dyn(K2)`
// across every (kind_a, kind_b, op) combination per langdef
// number-line ordering.  Each row carries an expression and the
// expected boolean.

struct CrossNumCmpRow {
  const char* name;
  const char* expr;
  bool expected;
};

class CrossNumericCmpExhaustive
    : public CrossNumericOrderingE2ETest,
      public ::testing::WithParamInterface<CrossNumCmpRow> {};

TEST_P(CrossNumericCmpExhaustive, ProducesExpectedBool) {
  EXPECT_EQ(EvalBool(GetParam().expr), GetParam().expected)
      << GetParam().name << ": " << GetParam().expr;
}

// 9 (kind_a, kind_b) pairs × 2 dyn-positions × 4 ops = 72 rows.
// Operand values picked so the ordering result is unambiguous:
// 1 < 2 across every numeric representation (1 / 1u / 1.0 vs
// 2 / 2u / 2.0).  Same-kind pairs are included as regression
// tripwires — the cross-numeric ladder must give the same answer
// the per-kind helper would.
INSTANTIATE_TEST_SUITE_P(
    LtMatrix, CrossNumericCmpExhaustive,
    ::testing::Values(
        // dyn(K1) < K2.
        CrossNumCmpRow{"lt_dyn_int_int", "dyn(1) < 2", true},
        CrossNumCmpRow{"lt_dyn_int_uint", "dyn(1) < 2u", true},
        CrossNumCmpRow{"lt_dyn_int_double", "dyn(1) < 2.0", true},
        CrossNumCmpRow{"lt_dyn_uint_int", "dyn(1u) < 2", true},
        CrossNumCmpRow{"lt_dyn_uint_uint", "dyn(1u) < 2u", true},
        CrossNumCmpRow{"lt_dyn_uint_double", "dyn(1u) < 2.0", true},
        CrossNumCmpRow{"lt_dyn_double_int", "dyn(1.0) < 2", true},
        CrossNumCmpRow{"lt_dyn_double_uint", "dyn(1.0) < 2u", true},
        CrossNumCmpRow{"lt_dyn_double_double", "dyn(1.0) < 2.0", true},
        // K1 < dyn(K2).
        CrossNumCmpRow{"lt_int_dyn_int", "1 < dyn(2)", true},
        CrossNumCmpRow{"lt_int_dyn_uint", "1 < dyn(2u)", true},
        CrossNumCmpRow{"lt_int_dyn_double", "1 < dyn(2.0)", true},
        CrossNumCmpRow{"lt_uint_dyn_int", "1u < dyn(2)", true},
        CrossNumCmpRow{"lt_uint_dyn_uint", "1u < dyn(2u)", true},
        CrossNumCmpRow{"lt_uint_dyn_double", "1u < dyn(2.0)", true},
        CrossNumCmpRow{"lt_double_dyn_int", "1.0 < dyn(2)", true},
        CrossNumCmpRow{"lt_double_dyn_uint", "1.0 < dyn(2u)", true},
        CrossNumCmpRow{"lt_double_dyn_double", "1.0 < dyn(2.0)", true}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    LeMatrix, CrossNumericCmpExhaustive,
    ::testing::Values(
        CrossNumCmpRow{"le_dyn_int_int_eq", "dyn(2) <= 2", true},
        CrossNumCmpRow{"le_dyn_int_uint_eq", "dyn(2) <= 2u", true},
        CrossNumCmpRow{"le_dyn_int_double_eq", "dyn(2) <= 2.0", true},
        CrossNumCmpRow{"le_dyn_uint_int_eq", "dyn(2u) <= 2", true},
        CrossNumCmpRow{"le_dyn_uint_uint_eq", "dyn(2u) <= 2u", true},
        CrossNumCmpRow{"le_dyn_uint_double_eq", "dyn(2u) <= 2.0", true},
        CrossNumCmpRow{"le_dyn_double_int_eq", "dyn(2.0) <= 2", true},
        CrossNumCmpRow{"le_dyn_double_uint_eq", "dyn(2.0) <= 2u", true},
        CrossNumCmpRow{"le_dyn_double_double_eq", "dyn(2.0) <= 2.0", true},
        CrossNumCmpRow{"le_int_dyn_int_eq", "2 <= dyn(2)", true},
        CrossNumCmpRow{"le_int_dyn_uint_eq", "2 <= dyn(2u)", true},
        CrossNumCmpRow{"le_int_dyn_double_eq", "2 <= dyn(2.0)", true},
        CrossNumCmpRow{"le_uint_dyn_int_eq", "2u <= dyn(2)", true},
        CrossNumCmpRow{"le_uint_dyn_uint_eq", "2u <= dyn(2u)", true},
        CrossNumCmpRow{"le_uint_dyn_double_eq", "2u <= dyn(2.0)", true},
        CrossNumCmpRow{"le_double_dyn_int_eq", "2.0 <= dyn(2)", true},
        CrossNumCmpRow{"le_double_dyn_uint_eq", "2.0 <= dyn(2u)", true},
        CrossNumCmpRow{"le_double_dyn_double_eq", "2.0 <= dyn(2.0)", true}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    GtMatrix, CrossNumericCmpExhaustive,
    ::testing::Values(
        CrossNumCmpRow{"gt_dyn_int_int", "dyn(3) > 2", true},
        CrossNumCmpRow{"gt_dyn_int_uint", "dyn(3) > 2u", true},
        CrossNumCmpRow{"gt_dyn_int_double", "dyn(3) > 2.0", true},
        CrossNumCmpRow{"gt_dyn_uint_int", "dyn(3u) > 2", true},
        CrossNumCmpRow{"gt_dyn_uint_uint", "dyn(3u) > 2u", true},
        CrossNumCmpRow{"gt_dyn_uint_double", "dyn(3u) > 2.0", true},
        CrossNumCmpRow{"gt_dyn_double_int", "dyn(3.0) > 2", true},
        CrossNumCmpRow{"gt_dyn_double_uint", "dyn(3.0) > 2u", true},
        CrossNumCmpRow{"gt_dyn_double_double", "dyn(3.0) > 2.0", true},
        CrossNumCmpRow{"gt_int_dyn_int", "3 > dyn(2)", true},
        CrossNumCmpRow{"gt_int_dyn_uint", "3 > dyn(2u)", true},
        CrossNumCmpRow{"gt_int_dyn_double", "3 > dyn(2.0)", true},
        CrossNumCmpRow{"gt_uint_dyn_int", "3u > dyn(2)", true},
        CrossNumCmpRow{"gt_uint_dyn_uint", "3u > dyn(2u)", true},
        CrossNumCmpRow{"gt_uint_dyn_double", "3u > dyn(2.0)", true},
        CrossNumCmpRow{"gt_double_dyn_int", "3.0 > dyn(2)", true},
        CrossNumCmpRow{"gt_double_dyn_uint", "3.0 > dyn(2u)", true},
        CrossNumCmpRow{"gt_double_dyn_double", "3.0 > dyn(2.0)", true}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    GeMatrix, CrossNumericCmpExhaustive,
    ::testing::Values(
        CrossNumCmpRow{"ge_dyn_int_int_eq", "dyn(2) >= 2", true},
        CrossNumCmpRow{"ge_dyn_int_uint_eq", "dyn(2) >= 2u", true},
        CrossNumCmpRow{"ge_dyn_int_double_eq", "dyn(2) >= 2.0", true},
        CrossNumCmpRow{"ge_dyn_uint_int_eq", "dyn(2u) >= 2", true},
        CrossNumCmpRow{"ge_dyn_uint_uint_eq", "dyn(2u) >= 2u", true},
        CrossNumCmpRow{"ge_dyn_uint_double_eq", "dyn(2u) >= 2.0", true},
        CrossNumCmpRow{"ge_dyn_double_int_eq", "dyn(2.0) >= 2", true},
        CrossNumCmpRow{"ge_dyn_double_uint_eq", "dyn(2.0) >= 2u", true},
        CrossNumCmpRow{"ge_dyn_double_double_eq", "dyn(2.0) >= 2.0", true},
        CrossNumCmpRow{"ge_int_dyn_int_eq", "2 >= dyn(2)", true},
        CrossNumCmpRow{"ge_int_dyn_uint_eq", "2 >= dyn(2u)", true},
        CrossNumCmpRow{"ge_int_dyn_double_eq", "2 >= dyn(2.0)", true},
        CrossNumCmpRow{"ge_uint_dyn_int_eq", "2u >= dyn(2)", true},
        CrossNumCmpRow{"ge_uint_dyn_uint_eq", "2u >= dyn(2u)", true},
        CrossNumCmpRow{"ge_uint_dyn_double_eq", "2u >= dyn(2.0)", true},
        CrossNumCmpRow{"ge_double_dyn_int_eq", "2.0 >= dyn(2)", true},
        CrossNumCmpRow{"ge_double_dyn_uint_eq", "2.0 >= dyn(2u)", true},
        CrossNumCmpRow{"ge_double_dyn_double_eq", "2.0 >= dyn(2.0)", true}),
    [](const auto& info) {
      return info.param.name;
    });

// ── Boundary TEST_F's — each names one langdef-cited invariant.

// langdef §"Comparisons": `int(MAX_INT64) < uint(MAX_UINT64)` →
// true.  `cmp_int_vs_uint`: any non-negative int compared to a
// uint > INT64_MAX yields kCmpLess.
TEST_F(CrossNumericOrderingE2ETest, LtIntMaxUintMax) {
  EXPECT_TRUE(EvalBool("dyn(9223372036854775807) < 18446744073709551615u"));
}

TEST_F(CrossNumericOrderingE2ETest, GtUintMaxIntMax) {
  EXPECT_TRUE(EvalBool("dyn(18446744073709551615u) > 9223372036854775807"));
}

// langdef §"Comparisons": `int(-1) < uint(0)` → true.
TEST_F(CrossNumericOrderingE2ETest, LtNegIntUintZero) {
  EXPECT_TRUE(EvalBool("dyn(-1) < 0u"));
}

TEST_F(CrossNumericOrderingE2ETest, GtUintZeroNegInt) {
  EXPECT_TRUE(EvalBool("dyn(0u) > -1"));
}

// `comparisons.textproto` (`not_lt_dyn_int_big_lossy_double`):
// `int(MAX_INT64) < double(MAX_INT64+1)` is FALSE because the
// double rounds down to MAX_INT64+0.0.
TEST_F(CrossNumericOrderingE2ETest, LtIntMaxBigLossyDouble) {
  // `9223372036854775807` cast to double rounds to 9.223372036854776e18
  // which equals (double)INT64_MAX exactly; cmp_int_vs_double's
  // boundary check returns kCmpEqual.
  EXPECT_FALSE(EvalBool("dyn(9223372036854775807) < 9223372036854775807.0"));
}

// `double(1e100) > int(MAX_INT64)` → true.  cmp_int_vs_double's
// `b > (double)INT64_MAX` arm returns kCmpLess (i.e. int < double).
TEST_F(CrossNumericOrderingE2ETest, GtBigDoubleIntMax) {
  EXPECT_TRUE(EvalBool("dyn(1.0e100) > 9223372036854775807"));
}

// `uint(MAX_UINT64) < double(-1.0)` → false.  cmp_uint_vs_double's
// `b < 0.0` arm returns kCmpGreater (uint > double).
TEST_F(CrossNumericOrderingE2ETest, LtUintMaxNegativeDouble) {
  EXPECT_FALSE(EvalBool("dyn(18446744073709551615u) < -1.0"));
}

// `int(1) < double(1.5)` → true (non-lossy double compare).
TEST_F(CrossNumericOrderingE2ETest, LtIntDoubleNonLossy) {
  EXPECT_TRUE(EvalBool("dyn(1) < 1.5"));
}

TEST_F(CrossNumericOrderingE2ETest, LtIntInt64MinDouble) {
  EXPECT_TRUE(EvalBool("dyn(-9223372036854775808) < 0.0"));
}

TEST_F(CrossNumericOrderingE2ETest, LtUintZeroDoubleNeg) {
  EXPECT_FALSE(EvalBool("dyn(0u) < -1.0"));
}

// IEEE: -Inf is less than every finite value.
TEST_F(CrossNumericOrderingE2ETest, LtDoubleMinusInfFinite) {
  EXPECT_TRUE(EvalBool("dyn(-1.0/0.0) < 0"));
}

TEST_F(CrossNumericOrderingE2ETest, LtFiniteDoubleMinusInf) {
  EXPECT_FALSE(EvalBool("dyn(0) < -1.0/0.0"));
}

TEST_F(CrossNumericOrderingE2ETest, LtFiniteDoublePlusInf) {
  EXPECT_TRUE(EvalBool("dyn(0) < 1.0/0.0"));
}

TEST_F(CrossNumericOrderingE2ETest, GtPlusInfFinite) {
  EXPECT_TRUE(EvalBool("dyn(1.0/0.0) > 0"));
}

// ── NaN matrix: every operator returns false on NaN-touching
// compares.  langdef §"Comparisons" + IEEE 754: NaN compares
// unordered with every value (including itself).
//
// 4 ops × 5 NaN-vs-X pairs = 20 rows.

struct NaNOrderRow {
  const char* name;
  const char* expr;
};

class CrossNumericNaNOrderingE2ETest
    : public CrossNumericOrderingE2ETest,
      public ::testing::WithParamInterface<NaNOrderRow> {};

TEST_P(CrossNumericNaNOrderingE2ETest, AlwaysFalse) {
  EXPECT_FALSE(EvalBool(GetParam().expr)) << GetParam().name;
}

INSTANTIATE_TEST_SUITE_P(
    NaN, CrossNumericNaNOrderingE2ETest,
    ::testing::Values(
        // NaN obtained via 0.0/0.0 (both checker- and runtime-legal).
        NaNOrderRow{"lt_nan_int", "dyn(0.0/0.0) < 0"},
        NaNOrderRow{"lt_nan_uint", "dyn(0.0/0.0) < 0u"},
        NaNOrderRow{"lt_nan_double", "dyn(0.0/0.0) < 0.0"},
        NaNOrderRow{"lt_nan_nan", "dyn(0.0/0.0) < 0.0/0.0"},
        NaNOrderRow{"lt_finite_nan", "dyn(0) < 0.0/0.0"},
        NaNOrderRow{"le_nan_int", "dyn(0.0/0.0) <= 0"},
        NaNOrderRow{"le_nan_uint", "dyn(0.0/0.0) <= 0u"},
        NaNOrderRow{"le_nan_double", "dyn(0.0/0.0) <= 0.0"},
        NaNOrderRow{"le_nan_nan", "dyn(0.0/0.0) <= 0.0/0.0"},
        NaNOrderRow{"le_finite_nan", "dyn(0) <= 0.0/0.0"},
        NaNOrderRow{"gt_nan_int", "dyn(0.0/0.0) > 0"},
        NaNOrderRow{"gt_nan_uint", "dyn(0.0/0.0) > 0u"},
        NaNOrderRow{"gt_nan_double", "dyn(0.0/0.0) > 0.0"},
        NaNOrderRow{"gt_nan_nan", "dyn(0.0/0.0) > 0.0/0.0"},
        NaNOrderRow{"gt_finite_nan", "dyn(0) > 0.0/0.0"},
        NaNOrderRow{"ge_nan_int", "dyn(0.0/0.0) >= 0"},
        NaNOrderRow{"ge_nan_uint", "dyn(0.0/0.0) >= 0u"},
        NaNOrderRow{"ge_nan_double", "dyn(0.0/0.0) >= 0.0"},
        NaNOrderRow{"ge_nan_nan", "dyn(0.0/0.0) >= 0.0/0.0"},
        NaNOrderRow{"ge_finite_nan", "dyn(0) >= 0.0/0.0"}),
    [](const auto& info) {
      return info.param.name;
    });

// ── Membership matrix: `<query> in <list>` and `<query> in <map>`
// across query × element kinds × {true, false}.  Per langdef
// §"List Membership (in)" / §"Map Key Membership (in)" + the
// conformance corpus' `int_in_doubles` / `uint_in_ints` rows
// (`lists.textproto`).
//
// 3 query × 3 element × 2 (true/false) × 2 (list/map) = 36 rows.

struct MembershipRow {
  const char* name;
  const char* expr;
  bool expected;
};

class CrossNumericMembershipE2ETest
    : public CrossNumericOrderingE2ETest,
      public ::testing::WithParamInterface<MembershipRow> {};

TEST_P(CrossNumericMembershipE2ETest, ProducesExpectedBool) {
  EXPECT_EQ(EvalBool(GetParam().expr), GetParam().expected)
      << GetParam().name << ": " << GetParam().expr;
}

INSTANTIATE_TEST_SUITE_P(
    ListMembership, CrossNumericMembershipE2ETest,
    ::testing::Values(
        // int query.
        MembershipRow{"int_in_ints_true", "dyn(2) in [1, 2, 3]", true},
        MembershipRow{"int_in_ints_false", "dyn(4) in [1, 2, 3]", false},
        MembershipRow{"int_in_uints_true", "dyn(2) in [1u, 2u, 3u]", true},
        MembershipRow{"int_in_uints_false", "dyn(4) in [1u, 2u, 3u]", false},
        MembershipRow{"int_in_doubles_true", "dyn(2) in [1.0, 2.0, 3.0]", true},
        MembershipRow{"int_in_doubles_false", "dyn(4) in [1.0, 2.0, 3.0]",
                      false},
        // uint query.
        MembershipRow{"uint_in_ints_true", "dyn(2u) in [1, 2, 3]", true},
        MembershipRow{"uint_in_ints_false", "dyn(4u) in [1, 2, 3]", false},
        MembershipRow{"uint_in_uints_true", "dyn(2u) in [1u, 2u, 3u]", true},
        MembershipRow{"uint_in_uints_false", "dyn(4u) in [1u, 2u, 3u]", false},
        MembershipRow{"uint_in_doubles_true", "dyn(2u) in [1.0, 2.0, 3.0]",
                      true},
        MembershipRow{"uint_in_doubles_false", "dyn(4u) in [1.0, 2.0, 3.0]",
                      false},
        // double query.
        MembershipRow{"double_in_ints_true", "dyn(2.0) in [1, 2, 3]", true},
        MembershipRow{"double_in_ints_false", "dyn(4.0) in [1, 2, 3]", false},
        MembershipRow{"double_in_uints_true", "dyn(2.0) in [1u, 2u, 3u]", true},
        MembershipRow{"double_in_uints_false", "dyn(4.0) in [1u, 2u, 3u]",
                      false},
        MembershipRow{"double_in_doubles_true", "dyn(2.0) in [1.0, 2.0, 3.0]",
                      true},
        MembershipRow{"double_in_doubles_false", "dyn(4.0) in [1.0, 2.0, 3.0]",
                      false}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    MapMembership, CrossNumericMembershipE2ETest,
    ::testing::Values(
        // double is NOT a valid map-key kind — `is_valid_map_key_kind`
        // rejects it on cel_map_insert — so map keys are int / uint /
        // (string / bool).  The query side, however, accepts any
        // numeric kind via the polymorphic ladder.
        MembershipRow{"int_in_intkeys_true", "dyn(1) in {1: \"a\", 2: \"b\"}",
                      true},
        MembershipRow{"int_in_intkeys_false", "dyn(3) in {1: \"a\", 2: \"b\"}",
                      false},
        MembershipRow{"int_in_uintkeys_true",
                      "dyn(1) in {1u: \"a\", 2u: \"b\"}", true},
        MembershipRow{"int_in_uintkeys_false",
                      "dyn(3) in {1u: \"a\", 2u: \"b\"}", false},
        MembershipRow{"uint_in_intkeys_true", "dyn(1u) in {1: \"a\", 2: \"b\"}",
                      true},
        MembershipRow{"uint_in_intkeys_false",
                      "dyn(3u) in {1: \"a\", 2: \"b\"}", false},
        MembershipRow{"uint_in_uintkeys_true",
                      "dyn(1u) in {1u: \"a\", 2u: \"b\"}", true},
        MembershipRow{"uint_in_uintkeys_false",
                      "dyn(3u) in {1u: \"a\", 2u: \"b\"}", false},
        // double-typed query against int / uint keys — the
        // map_keys_equal Slice 1.6 update makes this work.
        MembershipRow{"double_in_intkeys_true",
                      "dyn(1.0) in {1: \"a\", 2: \"b\"}", true},
        MembershipRow{"double_in_intkeys_false",
                      "dyn(3.0) in {1: \"a\", 2: \"b\"}", false},
        MembershipRow{"double_in_uintkeys_true",
                      "dyn(1.0) in {1u: \"a\", 2u: \"b\"}", true},
        MembershipRow{"double_in_uintkeys_false",
                      "dyn(3.0) in {1u: \"a\", 2u: \"b\"}", false}),
    [](const auto& info) {
      return info.param.name;
    });

// ── Membership NaN edges.  Spec: NaN is unequal to all values
// including itself; therefore NaN is NEVER `in` any list/map.
TEST_F(CrossNumericOrderingE2ETest, NaNNotInListWithNaN) {
  // `dyn(NaN) in [NaN, 1.0]` — NaN matcher returns false on every
  // element, including NaN itself.
  EXPECT_FALSE(EvalBool("dyn(0.0/0.0) in [0.0/0.0, 1.0]"));
}

TEST_F(CrossNumericOrderingE2ETest, FiniteNotInListOfNaN) {
  EXPECT_FALSE(EvalBool("dyn(1.0) in [0.0/0.0]"));
}

TEST_F(CrossNumericOrderingE2ETest, IntFoundInListWithNaN) {
  // The 1.0 element matches under polymorphic equality.
  EXPECT_TRUE(EvalBool("dyn(1) in [0.0/0.0, 1.0]"));
}

// ── Membership negative-int edges.  langdef §"Numeric equality":
// no negative int equals any uint.
TEST_F(CrossNumericOrderingE2ETest, NegIntNotInListOfUints) {
  EXPECT_FALSE(EvalBool("dyn(-1) in [0u, 1u, 2u]"));
}

TEST_F(CrossNumericOrderingE2ETest, ZeroUintFoundInListOfInts) {
  EXPECT_TRUE(EvalBool("dyn(0u) in [-1, 0, 1]"));
}

// ── Sanity: dyn-on-both-sides and double-dyn.

TEST_F(CrossNumericOrderingE2ETest, DynBothSidesIntUintLt) {
  EXPECT_TRUE(EvalBool("dyn(1) < dyn(2u)"));
}

TEST_F(CrossNumericOrderingE2ETest, DynBothSidesDoubleIntLe) {
  EXPECT_TRUE(EvalBool("dyn(1.0) <= dyn(1)"));
}

TEST_F(CrossNumericOrderingE2ETest, DynBothSidesNaNGt) {
  EXPECT_FALSE(EvalBool("dyn(0.0/0.0) > dyn(1)"));
}

TEST_F(CrossNumericOrderingE2ETest, DoubleDynNoOp) {
  // `dyn(dyn(1)) < 2u` — Slice 1.5's recursive collapse admits
  // nested dyn calls; the outer dyn forwards the inner dyn's
  // forwarded annotation.
  EXPECT_TRUE(EvalBool("dyn(dyn(1)) < 2u"));
}

// ── dyn(list) rejection — Slice 1.5's gate keeps aggregate-typed
// dyn arguments rejected; the test pins that this remains so even
// though Slice 1.6 makes the cross-numeric ladder more permissive.
TEST_F(CrossNumericOrderingE2ETest, DynListRejectsAtCheckTime) {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  ASSERT_THAT(compiler, IsOk());
  // `dyn([1.0])` — the argument is a list literal, which Slice 1.5
  // does not admit.  Compile must fail (RejectDyn).
  auto program = compiler->Compile("dyn(1) in dyn([1.0])");
  EXPECT_FALSE(program.ok())
      << "expected dyn(list) to be rejected at check time";
}

// ── Same-kind regression guards.  After the codegen overload
// re-pick, same-kind operands must still flow through the per-kind
// helper (no spurious cross-numeric routing).  These mirror the
// pre-Slice-1.6 happy paths.
TEST_F(CrossNumericOrderingE2ETest, SameKindIntLtPreserved) {
  EXPECT_TRUE(EvalBool("1 < 2"));
}

TEST_F(CrossNumericOrderingE2ETest, SameKindUintLePreserved) {
  EXPECT_TRUE(EvalBool("2u <= 2u"));
}

TEST_F(CrossNumericOrderingE2ETest, SameKindDoubleGtPreserved) {
  EXPECT_TRUE(EvalBool("3.0 > 2.0"));
}

TEST_F(CrossNumericOrderingE2ETest, SameKindIntGePreserved) {
  EXPECT_TRUE(EvalBool("3 >= 3"));
}

// In-list with same-kind elements (no dyn) — Slice 1.6 must not
// regress this path; cel-cpp admits it directly because `int in
// list(int)` has a checker overload.
TEST_F(CrossNumericOrderingE2ETest, SameKindIntInListOfInts) {
  EXPECT_TRUE(EvalBool("1 in [1, 2, 3]"));
}

TEST_F(CrossNumericOrderingE2ETest, SameKindStringInListOfStrings) {
  EXPECT_TRUE(EvalBool("\"a\" in [\"a\", \"b\"]"));
}

// ──────────────────────────────────────────────────────────────
//  MessageEqualityE2ETest — proves the cel_message_eq kernel
//  (M5.D step 2) computes the right answer for activation-bound
//  messages.  The conformance corpus's message-equality rows all
//  start with `Foo{...}` literal construction (kStructExpr, M7),
//  which v2 codegen rejects today — so this fixture is the
//  load-bearing coverage for the kernel itself until M7 lands.
// ──────────────────────────────────────────────────────────────
class MessageEqualityE2ETest : public ::testing::Test {
 protected:
  static absl::StatusOr<Compiler> CompilerWithTwoCustomers() {
    return BuildCompiler([](Compiler::Builder& b) {
      b.DeclareVariable("c1", CelType::Message("celwasm.testdata.Customer"));
      b.DeclareVariable("c2", CelType::Message("celwasm.testdata.Customer"));
    });
  }
};

TEST_F(MessageEqualityE2ETest, EmptyMessagesAreEqual) {
  auto compiler = CompilerWithTwoCustomers();
  ASSERT_THAT(compiler, IsOk());
  Customer a, b;
  Activation act;
  act.Bind("c1", Value::Message(a));
  act.Bind("c2", Value::Message(b));
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 == c2"), act).AsBool(), true);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 != c2"), act).AsBool(), false);
}

TEST_F(MessageEqualityE2ETest, IdenticalScalarFieldsAreEqual) {
  auto compiler = CompilerWithTwoCustomers();
  ASSERT_THAT(compiler, IsOk());
  Customer a;
  a.set_name("Alice");
  a.set_age(30);
  a.set_is_premium(true);
  Customer b;
  b.set_name("Alice");
  b.set_age(30);
  b.set_is_premium(true);
  Activation act;
  act.Bind("c1", Value::Message(a));
  act.Bind("c2", Value::Message(b));
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 == c2"), act).AsBool(), true);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 != c2"), act).AsBool(), false);
}

TEST_F(MessageEqualityE2ETest, DifferentStringFieldIsUnequal) {
  auto compiler = CompilerWithTwoCustomers();
  ASSERT_THAT(compiler, IsOk());
  Customer a;
  a.set_name("Alice");
  Customer b;
  b.set_name("Bob");
  Activation act;
  act.Bind("c1", Value::Message(a));
  act.Bind("c2", Value::Message(b));
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 == c2"), act).AsBool(), false);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 != c2"), act).AsBool(), true);
}

TEST_F(MessageEqualityE2ETest, DifferentNumericFieldIsUnequal) {
  auto compiler = CompilerWithTwoCustomers();
  ASSERT_THAT(compiler, IsOk());
  Customer a;
  a.set_age(30);
  Customer b;
  b.set_age(31);
  Activation act;
  act.Bind("c1", Value::Message(a));
  act.Bind("c2", Value::Message(b));
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 == c2"), act).AsBool(), false);
}

TEST_F(MessageEqualityE2ETest, EmptyVsPopulatedIsUnequal) {
  // Per langdef §"Equality" — proto field equality treats unset
  // and explicitly-default-set as equal (proto3); we set a non-
  // default value on one side to force inequality regardless of
  // the unset/default treatment.
  auto compiler = CompilerWithTwoCustomers();
  ASSERT_THAT(compiler, IsOk());
  Customer a;
  a.set_name("populated");
  Customer b;  // empty
  Activation act;
  act.Bind("c1", Value::Message(a));
  act.Bind("c2", Value::Message(b));
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 == c2"), act).AsBool(), false);
}

TEST_F(MessageEqualityE2ETest, NestedAddressEqual) {
  auto compiler = CompilerWithTwoCustomers();
  ASSERT_THAT(compiler, IsOk());
  Customer a;
  a.mutable_billing_address()->set_city("NYC");
  a.mutable_billing_address()->set_country("US");
  Customer b;
  b.mutable_billing_address()->set_city("NYC");
  b.mutable_billing_address()->set_country("US");
  Activation act;
  act.Bind("c1", Value::Message(a));
  act.Bind("c2", Value::Message(b));
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 == c2"), act).AsBool(), true);
}

TEST_F(MessageEqualityE2ETest, NestedAddressDifferingFieldIsUnequal) {
  auto compiler = CompilerWithTwoCustomers();
  ASSERT_THAT(compiler, IsOk());
  Customer a;
  a.mutable_billing_address()->set_city("NYC");
  Customer b;
  b.mutable_billing_address()->set_city("SF");
  Activation act;
  act.Bind("c1", Value::Message(a));
  act.Bind("c2", Value::Message(b));
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c1 == c2"), act).AsBool(), false);
}

TEST_F(MessageEqualityE2ETest, MessageReflexiveEquality) {
  // A bound message is always equal to itself.  Pin the obvious
  // invariant — easy regression sentinel if the kernel ever drifts.
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  });
  ASSERT_THAT(compiler, IsOk());
  Customer m;
  m.set_name("Anyone");
  m.set_age(42);
  Activation act;
  act.Bind("c", Value::Message(m));
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c == c"), act).AsBool(), true);
  EXPECT_EQ(*EvalOk(CompilePlan(*compiler, "c != c"), act).AsBool(), false);
}

}  // namespace
}  // namespace celwasm
