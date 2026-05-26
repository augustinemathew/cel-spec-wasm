// Partial-eval e2e matrix — the cross-product of CEL container shape
// (scalar / list / map / message) and element kind (primitive /
// complex object) against the partial-eval attribute model.
//
// VERIFIED MECHANISM (the load-bearing finding this suite pins).
// Unknowns are PRODUCED at two places, both consulting the same
// `unknown_patterns` set:
//
//   1. The activation marshal (eval/instance.cc BareVariableUnknownId):
//      a declared variable whose BARE attribute (root name + empty
//      qualifier path) is kFull-matched by a pattern gets CEL_UNKNOWN
//      written into its workspace slot instead of a marshaled value.
//      This makes the WHOLE variable opaque — `x + 1`, `xs[0]`,
//      `size(xs)`, `m['k']`, and `c.field` all become unknown, because
//      the runtime's 3VL absorption propagates the slot's unknown
//      through every consumer (arithmetic in cel_runtime.c; index /
//      key / size / field reads in cel_host.cc).  The verdict is
//      independent of binding: a bound value is ignored, and an
//      unknown variable need not be bound at all.
//
//   2. The `cel_get_field` trampoline that backs a `.field` select
//      (eval/internal/cel_host.cc RunFieldPrelude →
//      MatchesAnyUnknownPattern): a select produces CEL_UNKNOWN iff
//      the *effective* attribute (its operand's interned attribute ⊕
//      the field name) is kFull-matched.  This is the FINER lever —
//      it can mark `c.age` unknown while `c.name` stays known, which
//      the whole-variable marshal cannot.
//
// Together: pattern `x` (kFull on bare `x`) → whole var unknown via
// the marshal; pattern `x.foo` (kPartial on bare `x`, kFull only at
// the `.foo` select) → just that field unknown via the trampoline.
//
// What is STILL unexpressible (pinned in §8): per-element / per-key
// granularity.  `[index]` and `['key']` never intern an attribute, so
// you cannot mark `m['a']` unknown while `m['b']` stays known, nor
// `xs[0].age` while `xs[1].age` stays known.  Marking the whole
// container (`m`, `xs`) makes EVERY keyed/indexed read unknown; there
// is no pattern between "all of it" and "one specific field path".  A
// key-qualified pattern is rejected at Parse.
//
// Methodology: every assertion below was run; where the behaviour
// differs from a naive "whole-var unknown propagates" expectation, the
// test asserts the ACTUAL (concrete) result and the comment names why.
// Int leaves / keys / values throughout to avoid the host-arena
// string-marshal gap (e2e/m4_test.cc BoundStringListUnimplemented).

#include <functional>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/attribute.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::absl_testing::StatusIs;
using ::celwasm::testdata::Customer;

// Force generated-pool registration of Customer's descriptor.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<Customer>();
      return 0;
    }();

// ──────────────────────────────────────────────────────────────
//  Harness — mirrors e2e/m2_test.cc.
// ──────────────────────────────────────────────────────────────

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

absl::StatusOr<Compiler> CompilerWithVar(const std::string& name,
                                         const CelType& type) {
  return BuildCompiler([&](Compiler::Builder& b) {
    b.DeclareVariable(name, type);
  });
}

Instance CompilePlan(const Compiler& compiler, absl::string_view source) {
  auto program = compiler.Compile(source);
  ABSL_CHECK_OK(program) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance) << source;
  return *std::move(instance);
}

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
//  1. Simple scalar — `x + 1`.
//
//  Marking the whole scalar var `x` unknown makes `x + 1` UNKNOWN:
//  the activation marshal writes CEL_UNKNOWN into `x`'s slot when a
//  pattern FULL-matches the bare variable (eval/instance.cc
//  BareVariableUnknownId), and the runtime's 3VL absorption then
//  propagates the unknown through the `+`.  This is the bare-variable
//  counterpart to the `.field`-select unknown; both consult the same
//  pattern set.  Per langdef "Partial state".
// ──────────────────────────────────────────────────────────────

class ScalarPartialEvalTest : public ::testing::Test {
 protected:
  Compiler compiler_{*CompilerWithVar("x", CelType::Int())};
};

TEST_F(ScalarPartialEvalTest,
       WholeScalarVarUnknownPropagatesThroughArithmetic) {
  auto instance = CompilePlan(compiler_, "x + 1");
  Activation a;
  a.Bind("x", Value::Int(41));
  AttributePattern patterns[] = {MakePattern("x")};
  auto v = PartialEvalOk(instance, a, patterns);
  // `x` is opaque → `x + 1` is unknown, NOT 42, even though `x` is
  // bound: the unknown pattern wins over the binding.
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "pattern `x` blanks the slot; the `+` 3VL-absorbs it; kind="
      << static_cast<int>(v.kind());
}

// Binding `x` to a concrete value does NOT defeat the unknown pattern:
// marking a variable unknown means "pretend its value isn't known yet,"
// so the present binding is deliberately ignored.
TEST_F(ScalarPartialEvalTest, BoundButUnknownIgnoresTheBinding) {
  auto instance = CompilePlan(compiler_, "x");
  Activation a;
  a.Bind("x", Value::Int(99));  // a value IS supplied …
  AttributePattern patterns[] = {MakePattern("x")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)  // … and still ignored.
      << "kind=" << static_cast<int>(v.kind());
}

// A variable marked unknown need not be bound at all — the marshal
// must NOT raise "declared but not bound" for an unknown variable.
TEST_F(ScalarPartialEvalTest, UnknownVariableNeedNotBeBound) {
  auto instance = CompilePlan(compiler_, "x + 1");
  Activation a;  // `x` intentionally NOT bound.
  AttributePattern patterns[] = {MakePattern("x")};
  auto v = instance.PartialEval(a, patterns);
  ASSERT_TRUE(v.ok()) << v.status();
  EXPECT_EQ(v->kind(), Value::Kind::kUnknown)
      << "kind=" << static_cast<int>(v->kind());
}

TEST_F(ScalarPartialEvalTest, NonMatchingPatternStaysConcrete) {
  auto instance = CompilePlan(compiler_, "x + 1");
  Activation a;
  a.Bind("x", Value::Int(41));
  AttributePattern patterns[] = {MakePattern("y")};  // different var
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 42);
}

// ──────────────────────────────────────────────────────────────
//  2. Array of primitives — `xs[0]`, `size(xs)`.
//
//  Marking the whole list var `xs` unknown propagates through BOTH
//  `[index]` and `size(...)`: the marshal blanks `xs`'s slot to
//  CEL_UNKNOWN, and the host list-at / list-size trampolines 3VL-absorb
//  the unknown operand.  (You cannot mark a single element unknown —
//  see §8 — but the whole list certainly is.)
// ──────────────────────────────────────────────────────────────

class ListPrimitivePartialEvalTest : public ::testing::Test {
 protected:
  Compiler compiler_{*CompilerWithVar("xs", CelType::List(CelType::Int()))};

  Activation BoundList() {
    Activation a;
    a.Bind("xs", Value::List({Value::Int(10), Value::Int(20), Value::Int(30)}));
    return a;
  }
};

TEST_F(ListPrimitivePartialEvalTest, WholeListUnknownPropagatesThroughIndex) {
  auto instance = CompilePlan(compiler_, "xs[0]");
  auto a = BoundList();
  AttributePattern patterns[] = {MakePattern("xs")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "unknown list operand absorbs through `[0]`; kind="
      << static_cast<int>(v.kind());
}

TEST_F(ListPrimitivePartialEvalTest, WholeListUnknownPropagatesThroughSize) {
  auto instance = CompilePlan(compiler_, "size(xs)");
  auto a = BoundList();
  AttributePattern patterns[] = {MakePattern("xs")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "size() of an unknown list is unknown; kind="
      << static_cast<int>(v.kind());
}

// Iteration variable SHADOWING a declared free variable of the same
// name — `x.exists(x, x > 10)` where `x` is declared `list<int>`.
// The two `x`s are distinct bindings in distinct storage: the RANGE
// `x` (the `.exists` receiver) is the declared free variable in a
// workspace slot; the iteration `x` inside the predicate is
// comprehension-scoped (a per-iteration local, set by the loop
// prologue, NOT marshaled from the Activation).  The checker does the
// lexical scoping; resolve_pass stamps attribute_id 0 on the inner
// (scope_id != 0) so it is immune to patterns.  Verified e2e: this
// compiles and the predicate `x > 10` binds `x` to each int ELEMENT
// (it could not typecheck against the list), so [1,2,30] → true.
TEST_F(ListPrimitivePartialEvalTest, IterVarShadowingDeclaredVarBindsElement) {
  auto compiler = CompilerWithVar("x", CelType::List(CelType::Int()));
  ASSERT_THAT(compiler, ::absl_testing::IsOk());
  auto instance = CompilePlan(*compiler, "x.exists(x, x > 10)");
  Activation a;
  a.Bind("x", Value::List({Value::Int(1), Value::Int(2), Value::Int(30)}));
  auto v = instance.Eval(a);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool)
      << "inner `x` is the int element, not the list; kind="
      << static_cast<int>(v->kind());
  EXPECT_TRUE(*v->AsBool()) << "30 > 10 → exists is true";
  // Under PartialEval with pattern `x`, the RANGE `x` slot is blanked
  // (the outer var), so the range is unknown — which routes through the
  // comprehension-over-unknown-range gap (cleanup-backlog #14); the
  // inner predicate `x` stays immune either way.
}

TEST_F(ListPrimitivePartialEvalTest, NonMatchingPatternStaysConcrete) {
  auto instance = CompilePlan(compiler_, "xs[0]");
  auto a = BoundList();
  AttributePattern patterns[] = {MakePattern("other")};
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 10);
}

// SOUNDNESS GAP — a comprehension whose iter_range is UNKNOWN returns
// the empty-range identity (`exists`→false, `all`→true, `map`/`filter`
// →[]) instead of unknown.  `cel_host.cel_list_iter_open` maps any
// non-CEL_LIST_HOST range (including CEL_UNKNOWN/CEL_ERROR) to a
// zero-count arena view (CelListIterOpenImpl write_empty), and the
// comprehension prologue has no 3VL-absorption branch for the range.
// This pre-dates whole-variable unknowns (it was already reachable via
// `c.tags.exists(...)` with pattern `c.tags`); bare-variable unknowns
// just widen the door.  Fix is a WAT-first codegen arm: at the
// comprehension prologue, if iter_range's CelValue is CEL_UNKNOWN /
// CEL_ERROR, write it to the result slot and skip the loop
// (cleanup-backlog #14).
TEST_F(ListPrimitivePartialEvalTest, ComprehensionOverUnknownListIsUnknown) {
  GTEST_SKIP() << "cleanup-backlog #14: comprehension over an UNKNOWN range "
                  "returns the empty-range identity (false), not unknown — "
                  "cel_list_iter_open treats the unknown range as empty and "
                  "the comprehension prologue has no range-absorption branch.";
  auto instance = CompilePlan(compiler_, "xs.exists(e, e > 0)");
  auto a = BoundList();
  AttributePattern patterns[] = {MakePattern("xs")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "exists over an unknown list must be unknown, not false; kind="
      << static_cast<int>(v.kind());
}

// Variable shadowing: the comprehension iteration variable is NOT a
// free-variable attribute root — resolve_pass stamps attribute_id 0 on
// any comprehension-scope ident (scope_id != 0), so a pattern can never
// match it.  Here a pattern named `x` is supplied while the iteration
// variable is also `x`; the literal range `[1,2,3]` is concrete (so
// this dodges the #14 unknown-range gap), and the predicate's `x` is
// the loop variable, immune to the pattern.  Result: a concrete
// `false` (no element > 10), proving the pattern bound to nothing.
TEST_F(ListPrimitivePartialEvalTest, ComprehensionIterVarIsImmuneToPattern) {
  auto instance = CompilePlan(compiler_, "[1, 2, 3].exists(x, x > 10)");
  Activation a;  // no free `x` is referenced; range is a literal.
  AttributePattern patterns[] = {MakePattern("x")};  // shadows iter var
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kBool)
      << "the iteration variable `x` is comprehension-scoped (attribute_id "
         "0), so pattern `x` cannot mark it unknown; kind="
      << static_cast<int>(v.kind());
  EXPECT_FALSE(*v.AsBool());
}

// A pattern that targets the LOOP variable by name is a no-op, even
// when a DIFFERENT free variable is the range.  Here the range is the
// bound free var `xs` (pattern does NOT name it) and the loop var is
// `e` (pattern DOES name it): `e` is comprehension-scoped, never
// appears in `abi.variables()` for the marshal to blank, and carries
// attribute_id 0 so the select/3VL path can't match it either.  The
// pattern matches nothing → the comprehension runs concretely.  A
// loop variable can NEVER be made unknown by a pattern.
//
// cel-cpp oracle (correct behavior, confirmed against cel-cpp's own
// unknowns e2e suite): in `third_party/cel-cpp/eval/tests/
// unknowns_end_to_end_test.cc` every unknown pattern is rooted at an
// ACTIVATION variable name (`CelAttributePattern("var", {…})`, lines
// 171/196/313/728) — never at a comprehension iter var.  The loop
// variable's unknown-ness there is DERIVED from the range's attribute
// trail (`var[1]['elem1']` unknown → that iteration is unknown; the
// `UnknownsIterAttrTest` cases, ~753-838), not from a pattern naming
// the iter var.  So a pattern on the loop-var NAME matching nothing is
// the same answer cel-cpp gives.  (cel-cpp DOES support per-iteration
// unknown via range index/key qualifiers; we don't — that's the §8
// per-element limitation / #14, a capability gap, not a wrong no-op.)
TEST_F(ListPrimitivePartialEvalTest, PatternTargetingLoopVarIsNoOp) {
  auto instance = CompilePlan(compiler_, "xs.exists(e, e > 10)");
  auto a = BoundList();  // xs = [10, 20, 30]; range is concrete.
  AttributePattern patterns[] = {MakePattern("e")};  // names the loop var
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kBool)
      << "pattern `e` targets the loop var → no-op → concrete; kind="
      << static_cast<int>(v.kind());
  EXPECT_TRUE(*v.AsBool());  // 20 > 10 → exists is true
}

// Shadowing UNDER a matching pattern: `x.exists(x, x > 10)` with
// pattern `x`, where `x` is a declared `list<int>`.  The pattern hits
// the FREE variable `x` (the range), blanking its slot → the range is
// unknown → this routes through the comprehension-over-unknown-range
// gap (#14).  The inner loop `x` is immune regardless; the pattern
// never targets it.  Once #14 lands this is unknown, not false.
TEST_F(ListPrimitivePartialEvalTest, ShadowedRangeVarUnknownIsUnknown) {
  GTEST_SKIP() << "cleanup-backlog #14: pattern `x` blanks the RANGE `x` "
                  "(the free var) → comprehension over an unknown range "
                  "returns the empty-range identity (false) not unknown.";
  auto compiler = CompilerWithVar("x", CelType::List(CelType::Int()));
  ABSL_CHECK_OK(compiler);
  auto instance = CompilePlan(*compiler, "x.exists(x, x > 10)");
  Activation a;
  a.Bind("x", Value::List({Value::Int(1), Value::Int(2), Value::Int(30)}));
  AttributePattern patterns[] = {MakePattern("x")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "the range `x` is unknown → exists must be unknown; kind="
      << static_cast<int>(v.kind());
}

// ──────────────────────────────────────────────────────────────
//  3. Array of complex objects — `xs[0].age`.
//
//  Marking `xs` unknown makes `xs[0].age` unknown: `xs[0]` reads the
//  blanked (unknown) list slot and absorbs, then the `.age` select
//  3VL-absorbs the unknown operand in RunFieldPrelude.  You still
//  cannot mark ONLY `xs[0].age` (per-element) unknown — the index
//  interns no attribute (§8) — but the whole list being unknown
//  reaches the element field.
// ──────────────────────────────────────────────────────────────

class ListOfMessagePartialEvalTest : public ::testing::Test {
 protected:
  // `Value::Message` holds a NON-owning pointer to the proto (the host
  // owns the message for the Eval's lifetime — see
  // `Value::Message(const Message&)` in cel_host.cc).  The bound
  // messages MUST therefore outlive every Eval/PartialEval that reads
  // them: they live as fixture members, NOT as stack locals in
  // `BoundList()` (an earlier helper returned the Activation holding
  // pointers into destroyed stack messages — a use-after-free that
  // crashed as a "PartialEval segfault", cleanup-backlog #13).
  ListOfMessagePartialEvalTest() {
    c0_.set_age(30);
    c1_.set_age(41);
  }

  Compiler compiler_{*CompilerWithVar(
      "xs", CelType::List(CelType::Message("celwasm.testdata.Customer")))};

  Activation BoundList() {
    Activation a;
    a.Bind("xs", Value::List({Value::Message(c0_), Value::Message(c1_)}));
    return a;
  }

  Customer c0_;
  Customer c1_;
};

TEST_F(ListOfMessagePartialEvalTest, WholeListUnknownReachesElementField) {
  auto instance = CompilePlan(compiler_, "xs[0].age");
  auto a = BoundList();
  AttributePattern patterns[] = {MakePattern("xs")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "unknown list → `xs[0]` unknown → `.age` select absorbs; kind="
      << static_cast<int>(v.kind());
}

TEST_F(ListOfMessagePartialEvalTest, NonMatchingPatternStaysConcrete) {
  auto instance = CompilePlan(compiler_, "xs[1].age");
  auto a = BoundList();
  AttributePattern patterns[] = {MakePattern("other")};
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 41);
}

// ──────────────────────────────────────────────────────────────
//  4. Array of maps — `xs[0]['k']`.
//
//  Marking `xs` unknown propagates through index-then-key: `xs[0]`
//  absorbs the unknown list, and the `['k']` lookup absorbs the
//  unknown operand in turn.
// ──────────────────────────────────────────────────────────────

class ListOfMapPartialEvalTest : public ::testing::Test {
 protected:
  Compiler compiler_{*CompilerWithVar(
      "xs", CelType::List(CelType::Map(CelType::String(), CelType::Int())))};

  Activation BoundList() {
    Activation a;
    a.Bind("xs", Value::List({
                     Value::Map({{Value::String("k"), Value::Int(7)}}),
                     Value::Map({{Value::String("k"), Value::Int(9)}}),
                 }));
    return a;
  }
};

TEST_F(ListOfMapPartialEvalTest,
       WholeListUnknownPropagatesThroughIndexThenKey) {
  auto instance = CompilePlan(compiler_, "xs[0]['k']");
  auto a = BoundList();
  AttributePattern patterns[] = {MakePattern("xs")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "unknown list absorbs through `[0]` then `['k']`; kind="
      << static_cast<int>(v.kind());
}

TEST_F(ListOfMapPartialEvalTest, NonMatchingPatternStaysConcrete) {
  auto instance = CompilePlan(compiler_, "xs[1]['k']");
  auto a = BoundList();
  AttributePattern patterns[] = {MakePattern("other")};
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 9);
}

// ──────────────────────────────────────────────────────────────
//  5. Map of primitives — `m['k']`.
//
//  Marking the whole map var `m` unknown propagates through `['key']`:
//  the blanked map slot is absorbed by the host map-lookup trampoline.
// ──────────────────────────────────────────────────────────────

class MapPrimitivePartialEvalTest : public ::testing::Test {
 protected:
  Compiler compiler_{
      *CompilerWithVar("m", CelType::Map(CelType::String(), CelType::Int()))};

  Activation BoundMap() {
    Activation a;
    a.Bind("m", Value::Map({{Value::String("k"), Value::Int(5)},
                            {Value::String("j"), Value::Int(6)}}));
    return a;
  }
};

TEST_F(MapPrimitivePartialEvalTest, WholeMapUnknownPropagatesThroughKey) {
  auto instance = CompilePlan(compiler_, "m['k']");
  auto a = BoundMap();
  AttributePattern patterns[] = {MakePattern("m")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "unknown map operand absorbs through `['k']`; kind="
      << static_cast<int>(v.kind());
}

TEST_F(MapPrimitivePartialEvalTest, NonMatchingPatternStaysConcrete) {
  auto instance = CompilePlan(compiler_, "m['k']");
  auto a = BoundMap();
  AttributePattern patterns[] = {MakePattern("other")};
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 5);
}

// ──────────────────────────────────────────────────────────────
//  6. Map of arrays of complex objects — `m['k'][0].age`.
//
//  Marking `m` unknown reaches the leaf: `m['k']` absorbs the unknown
//  map, `[0]` absorbs the unknown list, and the `.age` select absorbs
//  the unknown element — three propagation hops from one blanked slot.
// ──────────────────────────────────────────────────────────────

class MapOfListOfMessagePartialEvalTest : public ::testing::Test {
 protected:
  // Bound messages are non-owning (see ListOfMessagePartialEvalTest);
  // they live as fixture members so they outlive every Eval.
  MapOfListOfMessagePartialEvalTest() {
    c0_.set_age(33);
    c1_.set_age(44);
  }

  Compiler compiler_{*CompilerWithVar(
      "m", CelType::Map(CelType::String(), CelType::List(CelType::Message(
                                               "celwasm.testdata.Customer"))))};

  Activation BoundMap() {
    Activation a;
    a.Bind("m", Value::Map({
                    {Value::String("k"),
                     Value::List({Value::Message(c0_), Value::Message(c1_)})},
                }));
    return a;
  }

  Customer c0_;
  Customer c1_;
};

TEST_F(MapOfListOfMessagePartialEvalTest, WholeMapUnknownReachesLeaf) {
  auto instance = CompilePlan(compiler_, "m['k'][0].age");
  auto a = BoundMap();
  AttributePattern patterns[] = {MakePattern("m")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "unknown map → key → index → field, all absorb; kind="
      << static_cast<int>(v.kind());
}

TEST_F(MapOfListOfMessagePartialEvalTest, NonMatchingPatternStaysConcrete) {
  auto instance = CompilePlan(compiler_, "m['k'][1].age");
  auto a = BoundMap();
  AttributePattern patterns[] = {MakePattern("other")};
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 44);
}

// ──────────────────────────────────────────────────────────────
//  7. Message field paths — the granularity that DOES work.
//
//  `.field` selects intern a string qualifier, so a pattern can name a
//  specific field path (`c.age`), a parent path (`c.billing_address`
//  covering `.city`), or a wildcard mid-path (`c.*.city`).  This is
//  the ONLY shape in the matrix where marking an attribute unknown
//  actually produces a kUnknown — because here the matched attribute
//  is the operand of a `.field` select.
// ──────────────────────────────────────────────────────────────

class MessagePartialEvalTest : public ::testing::Test {
 protected:
  Compiler compiler_{
      *CompilerWithVar("c", CelType::Message("celwasm.testdata.Customer"))};
};

TEST_F(MessagePartialEvalTest, ExactFieldPathUnknown) {
  auto instance = CompilePlan(compiler_, "c.age");
  Customer msg;
  msg.set_age(30);
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.age")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown);
}

TEST_F(MessagePartialEvalTest, WholeMessageVarUnknownCoversAnyFieldSelect) {
  // Pattern `c` kFull-matches the bare variable, so the marshal blanks
  // `c`'s slot; the `.age` select then 3VL-absorbs the unknown message
  // operand.  (Even without the marshal lever this would still fire at
  // the select, since `c` is also a prefix of the `c.age` attribute —
  // belt and suspenders.)
  auto instance = CompilePlan(compiler_, "c.age");
  Customer msg;
  msg.set_age(30);
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown);
}

TEST_F(MessagePartialEvalTest, ParentPathUnknownCoversNestedField) {
  auto instance = CompilePlan(compiler_, "c.billing_address.city");
  Customer msg;  // billing_address unset — must not crash.
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.billing_address")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "marking the parent path unknown absorbs the nested .city read";
}

TEST_F(MessagePartialEvalTest, WildcardMidPathUnknown) {
  auto instance = CompilePlan(compiler_, "c.billing_address.city");
  Customer msg;
  msg.mutable_billing_address()->set_city("Seattle");
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.*.city")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "c.*.city wildcard matches c.billing_address.city";
}

TEST_F(MessagePartialEvalTest, SiblingFieldPatternStaysConcrete) {
  // Pattern names the sibling field `c.name` while we evaluate the int
  // `c.age`; distinct field paths, so the read stays concrete.
  auto instance = CompilePlan(compiler_, "c.age");
  Customer msg;
  msg.set_age(30);
  Activation a;
  a.Bind("c", Value::Message(msg));
  AttributePattern patterns[] = {MakePattern("c.name")};  // sibling
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kInt)
      << "a sibling-field pattern leaves c.age concrete";
  EXPECT_EQ(*v.AsInt(), 30);
}

// ──────────────────────────────────────────────────────────────
//  7b. Container-typed FIELD marked unknown — `c.tags[0]`,
//      `c.tier_quotas[5]`.
//
//  This is the genuinely useful composite: a message field that is
//  itself a list / map.  The `.tags` / `.tier_quotas` SELECT produces
//  the unknown (its effective attribute `c.tags` / `c.tier_quotas`
//  kFull-matches the pattern), and the following `[index]` / `['key']`
//  3VL-absorbs it.  No bare-variable marshal blanking is involved —
//  `c` itself stays a concrete message (pattern `c.tags` is kPartial
//  on bare `c`).  This is the field-path lever reaching INTO a
//  container, the case the per-element negative (§8) contrasts with.
//
//  Int-keyed map + the list value never read past the unknown, so no
//  string-arena marshal is needed (cf. the file-header int-leaf rule).
// ──────────────────────────────────────────────────────────────

class ContainerFieldPartialEvalTest : public ::testing::Test {
 protected:
  ContainerFieldPartialEvalTest() {
    c_.add_tags("alpha");
    c_.add_tags("beta");
    (*c_.mutable_tier_quotas())[5] = 99;
  }

  Compiler compiler_{
      *CompilerWithVar("c", CelType::Message("celwasm.testdata.Customer"))};

  Activation BoundCustomer() {
    Activation a;
    a.Bind("c", Value::Message(c_));
    return a;
  }

  Customer c_;
};

TEST_F(ContainerFieldPartialEvalTest, UnknownListFieldPropagatesThroughIndex) {
  auto instance = CompilePlan(compiler_, "c.tags[0]");
  auto a = BoundCustomer();
  AttributePattern patterns[] = {MakePattern("c.tags")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "`.tags` select is unknown → `[0]` absorbs; kind="
      << static_cast<int>(v.kind());
}

TEST_F(ContainerFieldPartialEvalTest, UnknownMapFieldPropagatesThroughKey) {
  auto instance = CompilePlan(compiler_, "c.tier_quotas[5]");
  auto a = BoundCustomer();
  AttributePattern patterns[] = {MakePattern("c.tier_quotas")};
  auto v = PartialEvalOk(instance, a, patterns);
  EXPECT_EQ(v.kind(), Value::Kind::kUnknown)
      << "`.tier_quotas` select is unknown → `[5]` absorbs; kind="
      << static_cast<int>(v.kind());
}

TEST_F(ContainerFieldPartialEvalTest, SiblingContainerFieldStaysConcrete) {
  // Mark the list field unknown but READ the map field: distinct field
  // paths, and bare `c` is only kPartial-matched, so `c.tier_quotas[5]`
  // reads its concrete value.
  auto instance = CompilePlan(compiler_, "c.tier_quotas[5]");
  auto a = BoundCustomer();
  AttributePattern patterns[] = {MakePattern("c.tags")};  // sibling field
  auto v = PartialEvalOk(instance, a, patterns);
  ASSERT_EQ(v.kind(), Value::Kind::kInt)
      << "sibling-field pattern leaves `c.tier_quotas[5]` concrete; kind="
      << static_cast<int>(v.kind());
  EXPECT_EQ(*v.AsInt(), 99);
}

// ──────────────────────────────────────────────────────────────
//  8. Negative / by-design — per-element / per-key cannot be singled
//     out, and a key-qualified pattern is rejected at Parse.
// ──────────────────────────────────────────────────────────────

class PerKeyNegativePartialEvalTest : public ::testing::Test {
 protected:
  // map<string,list<Customer>> so a `.field` select exists to carry an
  // unknown — but the field path on the map ROOT (`m.size` etc.) isn't
  // what's read; we use the message-field lever to show a working
  // unknown still can't separate keys.  For the primitive-map case the
  // root simply can't be made unknown (see §5), so the separation
  // point is demonstrated against a key-qualified Parse rejection.
  Compiler compiler_{
      *CompilerWithVar("m", CelType::Map(CelType::String(), CelType::Int()))};

  Activation BoundMap() {
    Activation a;
    a.Bind("m", Value::Map({{Value::String("a"), Value::Int(1)},
                            {Value::String("b"), Value::Int(2)}}));
    return a;
  }
};

// You cannot mark `m['a']` unknown while `m['b']` stays known: the key
// is never interned, so there is no pattern between "all of `m`" and a
// specific field path.  The only lever that touches a keyed read is the
// whole-map pattern `m`, and it makes BOTH keys unknown — you can blank
// everything or nothing, never one key while its sibling stays known.
TEST_F(PerKeyNegativePartialEvalTest, KeysCannotBeSeparated) {
  auto inst_a = CompilePlan(compiler_, "m['a']");
  auto inst_b = CompilePlan(compiler_, "m['b']");
  auto act = BoundMap();
  AttributePattern patterns[] = {MakePattern("m")};
  auto va = PartialEvalOk(inst_a, act, patterns);
  auto vb = PartialEvalOk(inst_b, act, patterns);
  // Whole-map pattern blanks BOTH keys — the separation a per-key
  // pattern would give is impossible by construction.
  EXPECT_EQ(va.kind(), Value::Kind::kUnknown);
  EXPECT_EQ(vb.kind(), Value::Kind::kUnknown)
      << "no pattern marks one key unknown while the sibling stays known";
}

// A key-qualified pattern is rejected at Parse (cross-ref deliverable
// 1 / eval/attribute.cc): the key is never interned, so we don't
// accept a pattern we can't honor rather than silently match nothing.
TEST_F(PerKeyNegativePartialEvalTest, KeyQualifiedPatternRejectedAtParse) {
  EXPECT_THAT(AttributePattern::Parse("m['a']"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(AttributePattern::Parse("m[\"a\"]"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace celwasm
