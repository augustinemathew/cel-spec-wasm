// e2e tests for CEL optionals: compile → plan → eval and observe the
// runtime Value.  Each test pairs a source expression with the
// expected scalar result.  Cases mirror the conformance rows
// currently FAILing in `tests/simple/testdata/optionals.textproto`.

#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/generated_message_reflection.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

// Force generated-pool registration of descriptors referenced by the
// proto-literal `?field:` tests below.  Runs once at static init;
// mirrors the m8_test shape but scoped to just the proto3
// TestAllTypes needed by the Slice E tests.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<
          ::cel::expr::conformance::proto3::TestAllTypes>();
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

Value EvalSource(absl::string_view source) {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler) << source;
  auto program = compiler->Compile(source);
  ABSL_CHECK_OK(program) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance) << source;
  Activation a;
  auto v = instance->Eval(a);
  ABSL_CHECK_OK(v) << source;
  return *std::move(v);
}

// Status-returning variant for bug probes: a compile rejection or an
// eval-time failure surfaces as a non-OK status rather than aborting the
// binary, so a known-bug test can document *how* an expression fails
// (compile-stage vs eval-stage).
absl::StatusOr<Value> TryEvalSource(absl::string_view source) {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  if (!compiler.ok()) return compiler.status();
  auto program = compiler->Compile(source);
  if (!program.ok()) return program.status();
  auto instance = GlobalEngine().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a;
  return instance->Eval(a);
}

TEST(OptionalE2ETest, ConformanceChaining4ExactSource) {
  // optional_chaining_4 verbatim.
  Value v = EvalSource(
      "optional.of({'c': {'index': 'goodbye'}}).c.index."
      "orValue('default value')");
  ASSERT_EQ(v.kind(), Value::Kind::kString)
      << "kind=" << static_cast<int>(v.kind());
  EXPECT_EQ(*v.AsString(), "goodbye");
}

TEST(OptionalE2ETest, ConformanceChaining9ExactSource) {
  // optional_chaining_9 verbatim.
  Value v = EvalSource(
      "has(optional.of({'c': {'entry': 'hello world'}}).c) && "
      "!has(optional.of({'c': {'entry': 'hello world'}}).c.missing)");
  ASSERT_EQ(v.kind(), Value::Kind::kBool)
      << "kind=" << static_cast<int>(v.kind());
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalE2ETest, OrValueOnSelectIndexChainReturnsResolvedValue) {
  // optional_chaining_4 — uses orValue('default') to surface the
  // inner string when the chain resolves.  Source-level result must
  // be the resolved string, NOT the default and NOT an error.
  Value v = EvalSource(
      "optional.of({'c': {'index': 'goodbye'}}).c.index.orValue('default')");
  ASSERT_EQ(v.kind(), Value::Kind::kString)
      << "kind=" << static_cast<int>(v.kind());
  EXPECT_EQ(*v.AsString(), "goodbye");
}

TEST(OptionalE2ETest, OrValueOnAbsentInnerKeyReturnsDefault) {
  // None propagates through `.missing` to .orValue, which yields
  // the default.
  Value v = EvalSource(
      "optional.of({'c': {'index': 'x'}}).c.missing.orValue('default')");
  ASSERT_EQ(v.kind(), Value::Kind::kString)
      << "kind=" << static_cast<int>(v.kind());
  EXPECT_EQ(*v.AsString(), "default");
}

TEST(OptionalE2ETest, HasOnOptionalSelectReturnsTrueWhenPresent) {
  // optional_chaining_9 (positive half): has(opt.c) where the
  // optional is Some(map) and the inner map has key 'c'.
  Value v = EvalSource("has(optional.of({'c': 1}).c)");
  ASSERT_EQ(v.kind(), Value::Kind::kBool)
      << "kind=" << static_cast<int>(v.kind());
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalE2ETest, HasOnOptionalSelectReturnsFalseWhenAbsent) {
  // optional_chaining_9 (negative half): has(opt.c.missing) — the
  // inner key is absent, so the outer test_only Select sees a None
  // and reports false.
  Value v = EvalSource("has(optional.of({'c': {'entry': 1}}).c.missing)");
  ASSERT_EQ(v.kind(), Value::Kind::kBool)
      << "kind=" << static_cast<int>(v.kind());
  EXPECT_FALSE(*v.AsBool());
}

// None propagation through Select-on-optional: when the inner-key
// lookup misses, the kernel surfaces None.  Bare `optional.none()`
// can't drive this end-to-end (the static-subset gate rejects
// `optional<dyn>`); the inner-key-missing path exercises the same
// kernel None branch via `is_absent_error`.

TEST(OptionalE2ETest, SelectOnOptionalMissingKeyOrValueReturnsDefault) {
  // `optional.of({'k': 'v'}).c` — inner key 'c' is absent in the
  // wrapped map, so the kernel produces None.  `.orValue('default')`
  // resolves to the default.
  Value v = EvalSource("optional.of({'k': 'v'}).c.orValue('default')");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "default");
}

// `_[?_]` AST shape end-to-end: `m[?k]` returns Some(v) when present,
// None when absent.

TEST(OptionalE2ETest, MapOptIndexPresentKeyHasValueIsTrue) {
  Value v = EvalSource("{'c': 'v'}[?'c'].hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalE2ETest, MapOptIndexMissingKeyHasValueIsFalse) {
  Value v = EvalSource("{'c': 'v'}[?'missing'].hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalE2ETest, ListOptIndexInBoundsHasValueIsTrue) {
  Value v = EvalSource("[10, 20, 30][?1].hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalE2ETest, ListOptIndexOutOfBoundsHasValueIsFalse) {
  Value v = EvalSource("[10, 20, 30][?99].hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v.AsBool());
}

// `_[_]` on optional<list> operand — exercises the `optional<list>`
// shape that the codegen-only test `IndexOnOptionalListEmits...`
// covers structurally.

TEST(OptionalE2ETest, IndexOnOptionalListInBoundsReturnsSome) {
  Value v = EvalSource("optional.of([10, 20, 30])[1].orValue(-1)");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 20);
}

TEST(OptionalE2ETest, IndexOnOptionalListOutOfBoundsReturnsNone) {
  Value v = EvalSource("optional.of([10, 20, 30])[99].orValue(-1)");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), -1);
}

// `{?key: opt_v}` map-literal entries — when the optional is Some(v)
// the entry materialises; when None, the entry is omitted.  The
// runtime kernel is `cel_map_insert_at_if_present`.

TEST(OptionalE2ETest, MapLiteralOptionalEntryPresentMaterialises) {
  Value v = EvalSource("{?'k': optional.of('v')}['k']");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "v");
}

TEST(OptionalE2ETest, MapLiteralOptionalEntryAbsentOmitted) {
  // Both entries optional<string>; first is None (empty-string is
  // zero), second is Some.  Final map has 1 entry.
  Value v = EvalSource(
      "{?'k1': optional.ofNonZeroValue(''), "
      " ?'k2': optional.of('v')}.size()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 1);
}

TEST(OptionalE2ETest, MapLiteralMixedOptionalAndUnconditionalEntries) {
  // First entry unconditional, second optional Some: both materialise.
  Value v = EvalSource("{'k1': 'v1', ?'k2': optional.of('v2')}.size()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 2);
}

TEST(OptionalE2ETest, MapLiteralAllOptionalAllNoneIsEmpty) {
  // Both entries Optional None — final map is empty.  Same inner
  // type to keep the map value-type concrete (`map<string,string>`).
  Value v = EvalSource(
      "{?'k1': optional.ofNonZeroValue(''), "
      " ?'k2': optional.ofNonZeroValue('')}.size()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 0);
}

// `[?elem]` list-literal entries — symmetric to the map case.

TEST(OptionalE2ETest, ListLiteralOptionalEntryPresentMaterialises) {
  Value v = EvalSource("[?optional.of(7), ?optional.of(11)].size()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 2);
}

TEST(OptionalE2ETest, ListLiteralOptionalEntryAbsentOmitted) {
  Value v =
      EvalSource("[?optional.ofNonZeroValue(0), ?optional.of(11)].size()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 1);
}

TEST(OptionalE2ETest, ListLiteralMixedOptionalAndUnconditionalEntries) {
  Value v = EvalSource("[1, ?optional.of(2), 3].size()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 3);
}

TEST(OptionalE2ETest, ListLiteralAllOptionalAllNoneIsEmpty) {
  Value v = EvalSource(
      "[?optional.ofNonZeroValue(0), "
      " ?optional.ofNonZeroValue(0)].size()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 0);
}

// `optMap` / `optFlatMap` macros expand to a `_?_:_(hasValue(target),
// optional.of(<Shape-C cel.bind>), optional.none())` ternary (probe Q5);
// they should ride the existing comprehension Shape-C path with zero
// new codegen.

TEST(OptionalE2ETest, OptMapOnSomeAppliesBodyAndWraps) {
  // `optional.of(1).optMap(v, v + 1)` → optional.of(2).
  Value v = EvalSource("optional.of(1).optMap(v, v + 1).orValue(-1)");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 2);
}

TEST(OptionalE2ETest, OptMapOnNoneShortCircuitsToNone) {
  // `optional.ofNonZeroValue(0).optMap(v, v + 1)` → optional.none() →
  // orValue surfaces the default.
  Value v =
      EvalSource("optional.ofNonZeroValue(0).optMap(v, v + 1).orValue(-1)");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), -1);
}

TEST(OptionalE2ETest, OptFlatMapOnSomeReturnsBody) {
  // `optFlatMap` body already returns optional<T>; the macro threads
  // it through without re-wrapping.  `optional.of(1).optFlatMap(v,
  // optional.of(v + 1))` → optional.of(2).
  Value v = EvalSource(
      "optional.of(1).optFlatMap(v, optional.of(v + 1)).orValue(-1)");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 2);
}

TEST(OptionalE2ETest, OptFlatMapBodyMayReturnNone) {
  // Body returning None propagates through; orValue surfaces default.
  Value v = EvalSource(
      "optional.of(1).optFlatMap(v, optional.ofNonZeroValue(0))"
      ".orValue(-1)");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), -1);
}

// `Foo{?field: opt_value}` proto-literal optional entries.  Some
// materialises the field; None leaves it unset (matches proto
// semantics — `has()` returns false on an unset field).  Runtime
// kernel: `cel_set_field_at_if_present`.  All three rows use
// `cel.expr.conformance.proto3.TestAllTypes` which is registered
// in the Compiler builder via the same descriptor pool m8_test
// uses.

TEST(OptionalE2ETest, ProtoLiteralOptionalFieldSomeMaterialises) {
  Value v = EvalSource(
      "cel.expr.conformance.proto3.TestAllTypes{"
      "  ?single_int32: optional.of(5)"
      "}.single_int32");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 5);
}

TEST(OptionalE2ETest, ProtoLiteralOptionalFieldNoneLeavesUnset) {
  // `optional.ofNonZeroValue(0)` on int → None, so the field stays
  // unset.  `has()` on an unset scalar field returns false in
  // proto3.
  Value v = EvalSource(
      "has(cel.expr.conformance.proto3.TestAllTypes{"
      "  ?single_int32: optional.ofNonZeroValue(0)"
      "}.single_int32)");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalE2ETest, ProtoLiteralMixedOptionalAndUnconditionalEntries) {
  // Unconditional + optional entries in one literal; both
  // materialise (the optional resolves Some).
  Value v = EvalSource(
      "cel.expr.conformance.proto3.TestAllTypes{"
      "  single_int32: 7, "
      "  ?single_string: optional.of('v')"
      "}.single_string");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "v");
}

// ============================================================
// `optional.ofNonZeroValue` per-kind zero-predicate matrix
// ============================================================
//
// cel-cpp parity (`runtime/optional_types.cc`): zero ⇒ None;
// non-zero ⇒ Some(v).  m14-optionals.md §3.4 enumerates the
// per-kind definition of "zero" the kernel honors.  Verifying
// each row at the e2e level proves the kernel + ABI + frontend
// agree end-to-end — unit tests in `cel_optional_test.cc`
// cover the kernel in isolation; these tests catch
// integration-level breakage.

TEST(OptionalOfNonZeroE2ETest, ZeroIntIsNone) {
  Value v = EvalSource("optional.ofNonZeroValue(0).hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NonZeroIntIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue(5).hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NegativeIntIsSome) {
  // Sign isn't part of the zero predicate — only `payload.i == 0`
  // counts as zero.  Negative ints wrap as Some.
  Value v = EvalSource("optional.ofNonZeroValue(-1).hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, ZeroUintIsNone) {
  Value v = EvalSource("optional.ofNonZeroValue(0u).hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NonZeroUintIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue(5u).hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, ZeroDoubleIsNone) {
  Value v = EvalSource("optional.ofNonZeroValue(0.0).hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NegativeZeroDoubleIsNone) {
  // IEEE-754 -0.0 == +0.0 by `==`, matching cel-cpp's
  // `DoubleValue::IsZeroValue` which uses `== 0.0`.
  Value v = EvalSource("optional.ofNonZeroValue(-0.0).hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NonZeroDoubleIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue(1.5).hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, FalseBoolIsNone) {
  Value v = EvalSource("optional.ofNonZeroValue(false).hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, TrueBoolIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue(true).hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, EmptyStringIsNone) {
  Value v = EvalSource("optional.ofNonZeroValue('').hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NonEmptyStringIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue('x').hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, EmptyBytesIsNone) {
  Value v = EvalSource("optional.ofNonZeroValue(b'').hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NonEmptyBytesIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue(b'x').hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, EmptyListIsNone) {
  // List value-type must be concrete or RejectDyn fires upstream.
  // `[1].filter(...)` produces a concrete-typed empty list.
  Value v =
      EvalSource("optional.ofNonZeroValue([1].filter(x, false)).hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NonEmptyListIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue([1, 2, 3]).hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NonEmptyMapIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue({'k': 'v'}).hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, ZeroDurationIsNone) {
  Value v = EvalSource("optional.ofNonZeroValue(duration('0s')).hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NonZeroDurationIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue(duration('1s')).hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NestedNoneIsNone) {
  // optional.ofNonZeroValue(optional.<none-of-int>) → None per
  // cel-cpp `OptionalValue::IsZeroValue` (recursive descent).
  // Needs typed-None to avoid RejectDyn — use ofNonZeroValue(0)
  // which produces optional<int>.None.
  Value v = EvalSource(
      "optional.ofNonZeroValue(optional.ofNonZeroValue(0)).hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NestedSomeNonZeroIsSome) {
  Value v = EvalSource("optional.ofNonZeroValue(optional.of(5)).hasValue()");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalOfNonZeroE2ETest, NestedSomeZeroInnerIsNone) {
  // Inner Some(0) — outer ofNonZeroValue recurses, finds inner
  // is_zero → outer None.  Matches cel-cpp parity.
  Value v = EvalSource("optional.ofNonZeroValue(optional.of(0)).hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

// ============================================================
// `value()` / `hasValue()` on Some / None
// ============================================================

TEST(OptionalValueE2ETest, ValueOnSomeUnwrapsInner) {
  Value v = EvalSource("optional.of(42).value()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 42);
}

TEST(OptionalValueE2ETest, ValueOnNoneReturnsError) {
  // cel-cpp `OptionalValueInterface::Value` on None returns
  // `kInvalidArgument`; our kernel poisons with
  // `CEL_ERR_INVALID_ARGUMENT`.  Wrapping in `has(... )` short-
  // circuits the error, surfacing it as a boolean — for direct
  // observation we read the resulting kind.
  Value v = EvalSource("optional.ofNonZeroValue(0).value() == 0 ? 1 : 2");
  // CEL's `_?_:_` is short-circuit: the error on the condition
  // propagates without evaluating either branch.  Result is
  // CEL_ERROR.
  EXPECT_EQ(v.kind(), Value::Kind::kError);
}

TEST(OptionalValueE2ETest, HasValueOnSomeIsTrue) {
  Value v = EvalSource("optional.of(42).hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalValueE2ETest, HasValueOnNoneIsFalse) {
  Value v = EvalSource("optional.ofNonZeroValue(0).hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v.AsBool());
}

// ============================================================
// `or` (preserves optional-ness) / `orValue` (unwraps to bare T)
// ============================================================

TEST(OptionalOrE2ETest, OrLhsSomeKeepsLhs) {
  Value v = EvalSource("optional.of(5).or(optional.of(99)).value()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 5);
}

TEST(OptionalOrE2ETest, OrLhsNoneTakesRhs) {
  Value v =
      EvalSource("optional.ofNonZeroValue(0).or(optional.of(99)).value()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 99);
}

TEST(OptionalOrE2ETest, OrBothNoneStaysNone) {
  // Result type is `optional<int>`; observability via hasValue.
  Value v = EvalSource(
      "optional.ofNonZeroValue(0)"
      ".or(optional.ofNonZeroValue(0)).hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalOrE2ETest, OrChainResolvesFirstSome) {
  // Left-to-right or-chain; first Some wins.
  Value v = EvalSource(
      "optional.ofNonZeroValue(0)"
      ".or(optional.of(7))"
      ".or(optional.of(99))"
      ".value()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 7);
}

TEST(OptionalOrValueE2ETest, OrValueLhsSomeUnwrapsLhs) {
  Value v = EvalSource("optional.of(5).orValue(99)");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 5);
}

TEST(OptionalOrValueE2ETest, OrValueLhsNoneTakesDefault) {
  Value v = EvalSource("optional.ofNonZeroValue(0).orValue(99)");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 99);
}

TEST(OptionalOrValueE2ETest, OrValueReturnsBareInnerType) {
  // orValue's result kind is the inner type, NOT optional<T>.
  // (Contrast with `or` which preserves optional-ness.)
  Value v = EvalSource("optional.of('hello').orValue('default')");
  EXPECT_EQ(v.kind(), Value::Kind::kString);
}

// ============================================================
// Optional equality
// ============================================================

TEST(OptionalEqE2ETest, BothNoneIsEqual) {
  // optional.none() types as optional<dyn> which RejectDyn
  // refuses.  Use ofNonZeroValue zero values for concrete-typed
  // None alternatives.
  Value v =
      EvalSource("optional.ofNonZeroValue(0) == optional.ofNonZeroValue(0)");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalEqE2ETest, BothSomeWithEqualInnerIsEqual) {
  Value v = EvalSource("optional.of(5) == optional.of(5)");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalEqE2ETest, BothSomeWithDifferentInnerIsUnequal) {
  Value v = EvalSource("optional.of(5) == optional.of(7)");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalEqE2ETest, SomeVsNoneIsUnequal) {
  Value v = EvalSource("optional.of(5) == optional.ofNonZeroValue(0)");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalEqE2ETest, NotEqualsBothNoneIsFalse) {
  Value v =
      EvalSource("optional.ofNonZeroValue(0) != optional.ofNonZeroValue(0)");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalEqE2ETest, NotEqualsSomeVsNoneIsTrue) {
  Value v = EvalSource("optional.of(5) != optional.ofNonZeroValue(0)");
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalEqE2ETest, NestedSomeOfSameValueIsEqual) {
  Value v =
      EvalSource("optional.of(optional.of(5)) == optional.of(optional.of(5))");
  EXPECT_TRUE(*v.AsBool());
}

// ============================================================
// Deep chains — Select / Index on optional propagates None all
// the way through.  Each hop is a separate kernel call; chains
// of length 3+ stress the chain's compositional 3VL.
// ============================================================

TEST(OptionalChainE2ETest, ThreeDeepChainAllPresent) {
  Value v = EvalSource("optional.of({'a': {'b': {'c': 42}}}).a.b.c.value()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 42);
}

TEST(OptionalChainE2ETest, ThreeDeepChainMissingMiddleIsNone) {
  // Inner-map key 'b' is absent; the chain at .b produces None
  // and the trailing .c yields None as well (None propagates).
  Value v = EvalSource("optional.of({'a': {'x': {'c': 42}}}).a.b.c.hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalChainE2ETest, ChainOrValueSurfacesDefaultOnAnyMiss) {
  Value v = EvalSource(
      "optional.of({'a': {'b': {'c': 'v'}}}).a.missing.c.orValue('def')");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "def");
}

TEST(OptionalChainE2ETest, HasOnDeepChainTrueWhenAllPresent) {
  Value v = EvalSource("has(optional.of({'a': {'b': 1}}).a.b)");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalChainE2ETest, HasOnDeepChainFalseWhenAnyMissing) {
  Value v = EvalSource("has(optional.of({'a': {'b': 1}}).a.x)");
  EXPECT_FALSE(*v.AsBool());
}

TEST(OptionalChainE2ETest, OptIndexThenSelectInBoundsResolves) {
  // List of maps; [?i] → optional<map>; then .key resolves Some.
  Value v = EvalSource("[{'k': 'v0'}, {'k': 'v1'}][?1].k.orValue('def')");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "v1");
}

TEST(OptionalChainE2ETest, OptIndexThenSelectOutOfBoundsIsNone) {
  Value v = EvalSource("[{'k': 'v0'}, {'k': 'v1'}][?99].k.orValue('def')");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "def");
}

// ============================================================
// Nested optionals (`optional<optional<T>>`)
// ============================================================

TEST(OptionalNestedE2ETest, ValueOnNestedSomeUnwrapsOuterOnly) {
  // optional<optional<int>>::value() returns optional<int>, NOT
  // the inner int.  Each `.value()` peels one layer.
  Value v = EvalSource("optional.of(optional.of(5)).value().value()");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 5);
}

TEST(OptionalNestedE2ETest, HasValueOnOuterSomeOfNoneIsTrue) {
  // Outer optional is Some(None); hasValue reads the OUTER
  // present flag → true.  The fact that the inner is None
  // doesn't propagate through hasValue (only through chained
  // selects).
  Value v = EvalSource("optional.of(optional.ofNonZeroValue(0)).hasValue()");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(OptionalNestedE2ETest, OuterValueInnerHasValueOnSomeOfNone) {
  // Peel outer (Some), then call hasValue on the inner None.
  Value v =
      EvalSource("optional.of(optional.ofNonZeroValue(0)).value().hasValue()");
  EXPECT_FALSE(*v.AsBool());
}

// ============================================================
// Proto `?field:` edge cases
// ============================================================

TEST(ProtoOptionalFieldE2ETest, WrapperFieldSomeMaterialises) {
  // `single_int32_wrapper` is `google.protobuf.Int32Value`; cel-
  // cpp unwraps wrappers as the underlying scalar.  With
  // ?single_int32_wrapper: opt.of(5), the wrapper materialises
  // and reads back as int(5).
  Value v = EvalSource(
      "cel.expr.conformance.proto3.TestAllTypes{"
      "  ?single_int32_wrapper: optional.of(5)"
      "}.single_int32_wrapper");
  ASSERT_EQ(v.kind(), Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 5);
}

TEST(ProtoOptionalFieldE2ETest, WrapperFieldNoneLeavesUnset) {
  // Unset wrapper reads as null (cel-cpp wrapper semantics).
  Value v = EvalSource(
      "cel.expr.conformance.proto3.TestAllTypes{"
      "  ?single_int32_wrapper: optional.ofNonZeroValue(0)"
      "}.single_int32_wrapper == null");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_TRUE(*v.AsBool());
}

TEST(ProtoOptionalFieldE2ETest, OptionalFromMapOptIndexSourcesField) {
  // ?field: opt-from-some-other-expression.  Map opt-index returns
  // optional<V>; piping that into a proto field exercises the
  // wasm-side composition (not just optional.of literal).
  Value v = EvalSource(
      "cel.expr.conformance.proto3.TestAllTypes{"
      "  ?single_string: {'k': 'piped'}[?'k']"
      "}.single_string");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "piped");
}

TEST(ProtoOptionalFieldE2ETest, OptionalFromMissingKeyLeavesUnset) {
  // opt-index miss → None → field stays unset → reading the
  // scalar gives proto3's default (empty string for `single_string`).
  Value v = EvalSource(
      "cel.expr.conformance.proto3.TestAllTypes{"
      "  ?single_string: {'k': 'piped'}[?'missing']"
      "}.single_string");
  ASSERT_EQ(v.kind(), Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "");
}

TEST(ProtoOptionalFieldE2ETest, HasOnUnsetOptionalFieldIsFalse) {
  Value v = EvalSource(
      "has(cel.expr.conformance.proto3.TestAllTypes{"
      "  ?single_string: optional.ofNonZeroValue('')"
      "}.single_string)");
  ASSERT_EQ(v.kind(), Value::Kind::kBool);
  EXPECT_FALSE(*v.AsBool());
}

TEST(ProtoOptionalFieldE2ETest, HasOnSetOptionalFieldIsTrueForWrapper) {
  // Wrapper field: `has(msg.wrapper_field)` returns true iff the
  // wrapper message is *present* on the parent.  Setting via
  // ?field: opt.of(v) makes it present → has() = true.
  Value v = EvalSource(
      "has(cel.expr.conformance.proto3.TestAllTypes{"
      "  ?single_int32_wrapper: optional.of(7)"
      "}.single_int32_wrapper)");
  EXPECT_TRUE(*v.AsBool());
}

// KNOWN BUG (eval-stage): `optional.ofNonZeroValue(<message>)` traps.
// The expression compiles and plans (the proto-`?field:` gate was lifted,
// so the static subset admits a message operand), then traps at EVAL:
// `is_zero_value` (compiler_v2/runtime/cel_optional.c) has no CEL_MESSAGE
// arm — proto zero-ness needs reflection (cel-cpp parity:
// ParsedMessageValue::IsZeroValue), so it `__builtin_trap()`s.  Verified
// reproducing at the M14 Slice E closeout conformance run, where
// `optional_ofNonZeroValue_struct_…` flipped SKIP→FAIL.  Tracked as
// cleanup-backlog #10 (fix: a `cel_host.cel_message_is_zero` trampoline).
//
// Running unskipped TRAPS the process (non-OK Eval status that
// TryEvalSource would surface, but the wasm trap aborts first), so this
// stays GTEST_SKIP'd until the trampoline lands — delete the skip then,
// and the assertion below becomes the live regression guard.
TEST(ProtoOptionalFieldE2ETest, OfNonZeroValueOnNonZeroMessageHasValue) {
  GTEST_SKIP() << "KNOWN BUG (verified at M14 Slice E closeout: eval traps "
                  "__builtin_trap, want hasValue()==true): is_zero_value has "
                  "no CEL_MESSAGE arm (cel_optional.c) — needs a proto-"
                  "reflection host trampoline. cleanup-backlog #10. Running "
                  "unskipped TRAPS the process — fix first, then unskip.";
  auto v = TryEvalSource(
      "optional.ofNonZeroValue("
      "cel.expr.conformance.proto3.TestAllTypes{single_int32: 1})"
      ".hasValue()");
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kBool) << static_cast<int>(v->kind());
  // A non-zero message is non-zero -> ofNonZeroValue yields a present
  // optional -> hasValue() is true.
  EXPECT_TRUE(*v->AsBool())
      << "ofNonZeroValue on a non-zero message should be a present optional";
}

}  // namespace
}  // namespace cel
