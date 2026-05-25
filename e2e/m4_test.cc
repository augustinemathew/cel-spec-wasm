// M4 e2e test suite — the spec of "done" for the
// list literals + indexing milestone.
//
// Mirrors the m2_test.cc shape: every test asserts a capability
// M4 must land; running this binary before the implementation
// catches up should fail.  Greening the suite is the milestone
// exit per `m4-list-literals.md` §6.2.
//
// Fixtures grouped by capability:
//
//   - ListLiteralE2ETest  — pure literal flows (kArena origin).
//                           Per-element-kind round-trip,
//                           literal-then-index, OOB / negative
//                           index.
//   - ProtoRepeatedE2ETest — proto REPEATED field reads (kHost
//                            origin via `ProtoList` reflection).
//                            Mirrors how `customer.tags[i]`
//                            travels through `cel_host.cel_list_at`.
//
// String/bytes activation marshalling (host bindings of
// `list<string>`) inherits the host-arena gap that `IdentE2ETest`
// kString / kBytes still hit; SKIP at the binding-encoder layer
// until the host-arena work lands.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

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
#include "testdata/host_fixture_proto3.pb.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace celwasm::api {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::testdata::Customer;
using ::celwasm::testdata::HostMsg3;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<Customer>();
      google::protobuf::LinkMessageReflection<HostMsg3>();
      return 0;
    }();

// Shared Engine — same shape as m2_test's GlobalEngine.
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

absl::StatusOr<Compiler> CompilerWithCustomerVar() {
  return BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  });
}

absl::StatusOr<Compiler> CompilerWithHostMsg3Var() {
  return BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("h", CelType::Message("celwasm.testdata.HostMsg3"));
  });
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

// ──────────────────────────────────────────────────────────────
//  List literal E2E (Slice M4.F + M4.H — kArena origin)
//
//  `[…]` constructs in the wasm bump arena via cel_list_create
//  + cel_list_set; codegen routes `[…][i]` through
//  cel_list_at_arena (the pure-wasm fast path).  DecodeArenaListAt
//  walks the elements run on the host side after Eval.
// ──────────────────────────────────────────────────────────────

class ListLiteralE2ETest : public ::testing::Test {};

// ── Per-element-kind matrix.  Per `m4-list-literals.md §6.2`:
// "ListLiteralE2ETest — One TEST per element kind (uniform)."
// Each test indexes into the literal so the round-trip exercises
// both DecodeArenaListAt (for the .Size() probe) AND the
// cel_list_at_arena fast path (for the indexed read).

TEST_F(ListLiteralE2ETest, IntList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10, 20, 30]");
  Activation a;
  Value v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kList);
  EXPECT_EQ((*v.ListBacking())->Size(), 3u);
}

TEST_F(ListLiteralE2ETest, IntListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10, 20, 30][2]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 30);
}

TEST_F(ListLiteralE2ETest, UintListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10u, 20u, 30u][1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 20u);
}

TEST_F(ListLiteralE2ETest, BoolList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[true, false, true]");
  Activation a;
  Value v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kList);
  EXPECT_EQ((*v.ListBacking())->Size(), 3u);
}

TEST_F(ListLiteralE2ETest, BoolListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[true, false, true][1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ListLiteralE2ETest, DoubleList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1.5, 2.5, 3.5]");
  Activation a;
  Value v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kList);
  EXPECT_EQ((*v.ListBacking())->Size(), 3u);
}

TEST_F(ListLiteralE2ETest, DoubleListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1.5, 2.5, 3.5][2]");
  Activation a;
  EXPECT_DOUBLE_EQ(*EvalOk(instance, a).AsDouble(), 3.5);
}

// String / bytes literals live in rodata (StaticMemoryBuilder
// already packs string + bytes payloads at compile time), so a
// list literal of those kinds doesn't hit the host-arena gap that
// `IdentE2ETest::String` SKIPs.  cel_list_set memcpys the rodata
// CelValue (ptr,len) span into the elements arena verbatim.
TEST_F(ListLiteralE2ETest, StringListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(["alpha", "beta", "gamma"][1])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "beta");
}

TEST_F(ListLiteralE2ETest, BytesListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"([b"\x01", b"\x02", b"\x03"][2])");
  Activation a;
  auto got = *EvalOk(instance, a).AsBytes();
  EXPECT_EQ(got, absl::string_view("\x03", 1));
}

// `null` is its own type at the langdef level, so a `[null,
// null]` literal types as `list<null_type>` — homogeneous, no
// dyn folding.  Locks that null packs into rodata + round-trips.
TEST_F(ListLiteralE2ETest, NullListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[null, null, null][0]");
  Activation a;
  EXPECT_TRUE(EvalOk(instance, a).IsNull());
}

TEST_F(ListLiteralE2ETest, IndexLiteralReturnsElement) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10, 20, 30][1]");
  Activation a;
  Value v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 20);
}

TEST_F(ListLiteralE2ETest, IndexLiteralFirstAndLast) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  {
    auto instance = CompilePlan(*compiler, "[10, 20, 30][0]");
    Activation a;
    EXPECT_EQ(*EvalOk(instance, a).AsInt(), 10);
  }
  {
    auto instance = CompilePlan(*compiler, "[10, 20, 30][2]");
    Activation a;
    EXPECT_EQ(*EvalOk(instance, a).AsInt(), 30);
  }
}

// Out-of-bounds: cel_list_at_arena writes a CEL_ERROR /
// CEL_ERR_INDEX_OUT_OF_BOUNDS CelValue into the result slot.
// Pre-Slice-2, Instance::Eval surfaced this as a top-level decode
// failure (status::InvalidArgument); Slice 2 added a CEL_ERROR
// decoder arm (control-flow operators need to surface ERROR
// through Eval as a Value), so the failure is now a `Value::Error`
// and `Eval()` returns OK.
TEST_F(ListLiteralE2ETest, OutOfBoundsIndexReturnsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10, 20, 30][3]");
  Activation a;
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

TEST_F(ListLiteralE2ETest, NegativeIndexReturnsError) {
  // Per langdef §"Indexing": negative list indices are an error,
  // not Python-style wrap-around.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10, 20, 30][-1]");
  Activation a;
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

// ──────────────────────────────────────────────────────────────
//  Nested aggregates — list of list, list of map.  Lists can
//  hold any homogeneous element type; codegen emits a nested
//  cel_list_create / cel_map_create per inner aggregate, with
//  the outer cel_list_set storing the resulting `{kind, ref}`
//  CelValue.
// ──────────────────────────────────────────────────────────────

TEST_F(ListLiteralE2ETest, NestedListOuterRoundTrip) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[[1, 2], [3, 4], [5, 6]]");
  Activation a;
  Value v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kList);
  EXPECT_EQ((*v.ListBacking())->Size(), 3u);
}

TEST_F(ListLiteralE2ETest, NestedListIndexed) {
  // Outer index produces inner list; need to index again to get
  // a scalar.  `[[1,2],[3,4]][1][0]` — chained `_[_]` calls.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[[1, 2], [3, 4]][1][0]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

TEST_F(ListLiteralE2ETest, ListOfMap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"([{"a": 1, "b": 2}, {"a": 10, "b": 20}])");
  Activation a;
  Value v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kList);
  EXPECT_EQ((*v.ListBacking())->Size(), 2u);
}

TEST_F(ListLiteralE2ETest, ListOfMapChainedIndex) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"([{"a": 1}, {"a": 2}][1]["a"])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

// ──────────────────────────────────────────────────────────────
//  Compile-time rejection — plan §1.3 + §8 risk #1.
//
//  Some shapes that look superficially like list ops should be
//  rejected before reaching codegen.  Each test locks the
//  current behaviour: PASS where rejection lands at compile
//  (preferred), FAIL where the rejection isn't yet wired up
//  (documented with a TODO referencing the M4 future-work list).
// ──────────────────────────────────────────────────────────────

class ListRejectionE2ETest : public ::testing::Test {};

// Per langdef §"Indexing": numeric list indices are `int` only.
// `[1u, 2u, 3u]` types as `list<uint>` (homogeneous) but
// indexing it with an `int` should still be the only legal form;
// indexing with a `uint` literal should be a checker rejection.
TEST_F(ListRejectionE2ETest, UintIndexRejectedAtCompile) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto program_or = compiler->Compile("[1, 2, 3][1u]");
  EXPECT_FALSE(program_or.ok())
      << "uint index on a list should be a static-subset rejection";
}

// Same family: double / bool / string indices on a list are
// rejected by the type-checker (the `_[_]` overload set has no
// list⟨_⟩-by-double / -by-bool / -by-string member).
TEST_F(ListRejectionE2ETest, DoubleIndexRejectedAtCompile) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto program_or = compiler->Compile("[1, 2, 3][1.0]");
  EXPECT_FALSE(program_or.ok());
}

TEST_F(ListRejectionE2ETest, BoolIndexRejectedAtCompile) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto program_or = compiler->Compile("[1, 2, 3][true]");
  EXPECT_FALSE(program_or.ok());
}

TEST_F(ListRejectionE2ETest, StringIndexOnIntListRejectedAtCompile) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto program_or = compiler->Compile(R"([1, 2, 3]["x"])");
  EXPECT_FALSE(program_or.ok());
}

// Bare empty literal `[]` — cel-cpp's checker types it as
// `list<dyn>` (no element type to infer; dyn is the top type).
// Our static-subset `RejectDyn` gate rejects it because the
// recursive walk added in M5.A traverses `list_type().elem_type()`
// and finds `dyn`.
TEST_F(ListRejectionE2ETest, BareEmptyListLiteralRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto program_or = compiler->Compile("[]");
  EXPECT_FALSE(program_or.ok())
      << "bare `[]` types as list<dyn>; RejectDyn must catch the "
         "implicit dyn (M5.A — m5-kcall-comprehensions.md §5)";
}

// Heterogeneous list `[1, "two"]` — cel-cpp's checker types it
// as `list<dyn>` (CEL semantics permit it; `dyn` is the top
// type).  Our static-subset `RejectDyn` gate rejects it because
// dyn travels under the hood (M5.A — recursive walk of
// list_type().elem_type()).
TEST_F(ListRejectionE2ETest, HeterogeneousListRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto program_or = compiler->Compile(R"([1, "two"])");
  EXPECT_FALSE(program_or.ok())
      << "heterogeneous list literal types as list<dyn>; RejectDyn "
         "must catch the implicit dyn (M5.A — "
         "m5-kcall-comprehensions.md §5)";
}

// ──────────────────────────────────────────────────────────────
//  ListBindingE2ETest — Activation::Bind list with each element
//  kind.  Per `m4-list-literals.md §6.2`: "Activation::Bind('xs',
//  Value::List({...})) round-trips through the kHost arm.  Per
//  element kind."  This exercises EncodeList → InternList →
//  cel_host.cel_list_at → ProtoList/HostList::At → host-side
//  decode.
// ──────────────────────────────────────────────────────────────

class ListBindingE2ETest : public ::testing::Test {};

TEST_F(ListBindingE2ETest, BoundIntListIndexed) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "xs[1]");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(10), Value::Int(20), Value::Int(30)}));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 20);
}

TEST_F(ListBindingE2ETest, BoundUintListIndexed) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Uint()));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "xs[0]");
  Activation a;
  a.Bind("xs", Value::List({Value::Uint(7u), Value::Uint(8u)}));
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 7u);
}

TEST_F(ListBindingE2ETest, BoundDoubleListIndexed) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Double()));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "xs[1]");
  Activation a;
  a.Bind("xs", Value::List({Value::Double(1.5), Value::Double(2.5)}));
  EXPECT_DOUBLE_EQ(*EvalOk(instance, a).AsDouble(), 2.5);
}

TEST_F(ListBindingE2ETest, BoundBoolListIndexed) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Bool()));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "xs[2]");
  Activation a;
  a.Bind("xs", Value::List(
                   {Value::Bool(true), Value::Bool(false), Value::Bool(true)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// String / bytes binding hits the same host-arena gap that
// IdentE2ETest::String SKIPs — span payloads need persistent
// host-side memory across arena_reset.  The element-type encoder
// itself doesn't fire for kHost lists (the list backing carries
// the payloads), but the element ENCODER on read-back through
// EncodeFieldResult does need the arena.  Lock the gap here.
TEST_F(ListBindingE2ETest, BoundStringListUnimplemented) {
  GTEST_SKIP() << "list<string> binding inherits the host-arena gap "
                  "from IdentE2ETest::String; deferred to host-arena "
                  "milestone";
}

TEST_F(ListBindingE2ETest, OutOfBoundsOnBoundListFails) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
  });
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "xs[5]");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(1), Value::Int(2)}));
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

// ──────────────────────────────────────────────────────────────
//  DispatcherE2ETest — Plan §6.2 / §1.1.  `(cond ? [1,2] : xs)
//  [0]` — the `?:` operand is mixed-origin (kArena from the
//  literal arm, kHost from the bound list arm), so
//  ListOriginVisitor coalesces to kDynamic and codegen routes
//  through `cel_list_at` (the runtime tail-call dispatcher).
//
//  M5 introduces the `?:` codegen arm; M4 doesn't lower it.
//  Lock the current Unimplemented status here — flips green
//  when M5 ships kCall arithmetic + ternary.
// ──────────────────────────────────────────────────────────────

class DispatcherE2ETest : public ::testing::Test {};

TEST_F(DispatcherE2ETest, ConditionalListOperandLowersGreen) {
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("xs", CelType::List(CelType::Int()));
    b.DeclareVariable("cond", CelType::Bool());
  });
  ASSERT_THAT(compiler, IsOk());
  // M5.G (Slice 2) lit up `_?_:_`.  The ternary's two arms are
  // mixed-origin (kArena from the literal, kHost from the bound
  // list); ListOriginVisitor coalesces to kDynamic and `_[_]`
  // routes through `cel_list_at`.  Rewritten from
  // `ConditionalListOperandUnimplementedAtM4` at the M5.G enabling
  // commit.
  auto program_or = compiler->Compile("(cond ? [1, 2] : xs)[0]");
  ASSERT_THAT(program_or, IsOk());
}

// ──────────────────────────────────────────────────────────────
//  Proto REPEATED field E2E (Slice M4.G + M4.F + M4.H — kHost
//  origin via ProtoList)
//
//  `customer.tags[i]` reads the REPEATED field via
//  ProtoBacking::ReadField → Value::HostList(ProtoList{...}),
//  routes the index call through cel_host.cel_list_at, which
//  invokes ProtoList::At under the hood and encodes the result
//  back into the wasm result slot.
// ──────────────────────────────────────────────────────────────

class ProtoRepeatedE2ETest : public ::testing::Test {
 protected:
  Compiler compiler_{*CompilerWithCustomerVar()};
};

TEST_F(ProtoRepeatedE2ETest, IndexZeroReturnsFirstElement) {
  auto instance = CompilePlan(compiler_, "c.tags[0]");
  Customer msg;
  msg.add_tags("alpha");
  msg.add_tags("beta");
  msg.add_tags("gamma");
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "alpha");
}

TEST_F(ProtoRepeatedE2ETest, IndexMidReturnsMiddleElement) {
  auto instance = CompilePlan(compiler_, "c.tags[1]");
  Customer msg;
  msg.add_tags("alpha");
  msg.add_tags("beta");
  msg.add_tags("gamma");
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "beta");
}

TEST_F(ProtoRepeatedE2ETest, IndexLastReturnsLastElement) {
  auto instance = CompilePlan(compiler_, "c.tags[2]");
  Customer msg;
  msg.add_tags("alpha");
  msg.add_tags("beta");
  msg.add_tags("gamma");
  Activation a;
  a.Bind("c", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "gamma");
}

TEST_F(ProtoRepeatedE2ETest, OutOfBoundsIndexFails) {
  auto instance = CompilePlan(compiler_, "c.tags[3]");
  Customer msg;
  msg.add_tags("alpha");
  msg.add_tags("beta");
  msg.add_tags("gamma");
  Activation a;
  a.Bind("c", Value::Message(msg));
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

TEST_F(ProtoRepeatedE2ETest, NegativeIndexFails) {
  auto instance = CompilePlan(compiler_, "c.tags[-1]");
  Customer msg;
  msg.add_tags("alpha");
  Activation a;
  a.Bind("c", Value::Message(msg));
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

TEST_F(ProtoRepeatedE2ETest, EmptyRepeatedFieldFailsOnIndexZero) {
  auto instance = CompilePlan(compiler_, "c.tags[0]");
  Customer msg;  // tags unset.
  Activation a;
  a.Bind("c", Value::Message(msg));
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

// Repeated-int-typed field (rep_i32 on HostMsg3) — exercises the
// integer arm of ProtoList::At, distinct from string above.
TEST(ProtoRepeatedHostMsg3E2ETest, RepeatedInt32Indexing) {
  auto compiler = CompilerWithHostMsg3Var();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "h.rep_i32[1]");
  HostMsg3 msg;
  msg.add_rep_i32(100);
  msg.add_rep_i32(200);
  msg.add_rep_i32(300);
  Activation a;
  a.Bind("h", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 200);
}

TEST(ProtoRepeatedHostMsg3E2ETest, RepeatedDoubleIndexing) {
  auto compiler = CompilerWithHostMsg3Var();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "h.rep_f64[0]");
  HostMsg3 msg;
  msg.add_rep_f64(1.5);
  msg.add_rep_f64(2.5);
  Activation a;
  a.Bind("h", Value::Message(msg));
  EXPECT_DOUBLE_EQ(*EvalOk(instance, a).AsDouble(), 1.5);
}

TEST(ProtoRepeatedHostMsg3E2ETest, RepeatedBoolIndexing) {
  auto compiler = CompilerWithHostMsg3Var();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "h.rep_b[1]");
  HostMsg3 msg;
  msg.add_rep_b(true);
  msg.add_rep_b(false);
  Activation a;
  a.Bind("h", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// String repeated field — exercises the kString arm of
// ProtoList::At and the EncodeFieldResult span-encoding path.
// (Customer.tags above is the proto3 `string` shape; HostMsg3's
// `rep_s` uses a different field number / namespace.  Keeping
// both keeps the e2e matrix covered.)
TEST(ProtoRepeatedHostMsg3E2ETest, RepeatedStringIndexing) {
  auto compiler = CompilerWithHostMsg3Var();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "h.rep_s[0]");
  HostMsg3 msg;
  msg.add_rep_s("hello");
  msg.add_rep_s("world");
  Activation a;
  a.Bind("h", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "hello");
}

// Repeated message — element type is itself a HostMsg3 backing.
// Reads `rep_msg[0].b` chain: outer kSelect on the repeated
// field gives a kHost list, kCallExpr `_[_]` invokes
// cel_host.cel_list_at which interns the inner message and
// returns CEL_MESSAGE; the inner kSelect on `.b` then runs the
// usual cel_get_field path.
TEST(ProtoRepeatedHostMsg3E2ETest, RepeatedMessageIndexedThenField) {
  auto compiler = CompilerWithHostMsg3Var();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "h.rep_msg[1].b");
  HostMsg3 msg;
  msg.add_rep_msg();
  msg.add_rep_msg()->set_b(true);  // index 1 — bool true.
  Activation a;
  a.Bind("h", Value::Message(msg));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

}  // namespace
}  // namespace celwasm::api
