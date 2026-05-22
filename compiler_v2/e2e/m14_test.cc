// e2e tests for CEL optionals: compile → plan → eval and observe the
// runtime Value.  Each test pairs a source expression with the
// expected scalar result.  Cases mirror the conformance rows
// currently FAILing in `tests/simple/testdata/optionals.textproto`.

#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

using ::absl_testing::IsOk;

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
  Value v = EvalSource(
      "optional.of({'k': 'v'}).c.orValue('default')");
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
  Value v = EvalSource(
      "{?'k': optional.of('v')}['k']");
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
  Value v = EvalSource(
      "{'k1': 'v1', ?'k2': optional.of('v2')}.size()");
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
  Value v = EvalSource(
      "[?optional.ofNonZeroValue(0), ?optional.of(11)].size()");
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
  Value v = EvalSource(
      "optional.ofNonZeroValue(0).optMap(v, v + 1).orValue(-1)");
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

}  // namespace
}  // namespace cel
