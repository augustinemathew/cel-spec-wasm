// End-to-end evaluation test for the codegen MVP.
//
// Pipeline exercised:
//
//   CEL source string
//       → ParseAndCheck   (compiler/frontend)
//       → LowerToEvalFunction + WasmModule::Serialize
//                         (compiler/codegen)
//       → LoadEval        (compiler/host)  — instantiates runtime
//                           + eval under a shared wasmtime linker
//                           (two-module architecture, design §7.0)
//       → CallNullaryEval — invokes the `eval` export and returns
//                           the scalar result
//
// This is the only test in the repo that actually executes WASM the
// compiler emits against a real runtime.  Everything upstream
// (Binaryen's validator, instruction-shape assertions in
// expr_lower_test) can still pass while the module means nothing
// semantically; this test catches that.

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/host/host_loader.h"
#include "compiler/ir/typed_ast.h"
#include "compiler/runtime/cel_runtime.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Runs `cel_source` through the full pipeline and returns the single
// scalar that `eval()` produced.
absl::StatusOr<wasmtime_val_t> Evaluate(absl::string_view cel_source) {
  auto typed = ParseAndCheck(cel_source, CheckOptions{});
  if (!typed.ok()) return typed.status();

  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  if (auto s = mod.Validate(); !s.ok()) return s;

  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();

  auto loaded = LoadEval(*bytes);
  if (!loaded.ok()) return loaded.status();
  return loaded->CallNullaryEval();
}

// Same as Evaluate, but declares variables via `variable_specs` and
// passes `args` through to the eval function.  The caller is
// responsible for packing args in the same order as specs and using a
// `wasmtime_val_t` whose `kind` matches each variable's ABI (i64 for
// int/uint, i32 for bool, f64 for double, etc.).
absl::StatusOr<wasmtime_val_t> EvaluateWithVars(
    absl::string_view cel_source, std::vector<std::string> variable_specs,
    std::vector<wasmtime_val_t> args) {
  CheckOptions opts;
  opts.variable_specs = std::move(variable_specs);
  auto typed = ParseAndCheck(cel_source, opts);
  if (!typed.ok()) return typed.status();

  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  if (auto s = mod.Validate(); !s.ok()) return s;

  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();

  auto loaded = LoadEval(*bytes);
  if (!loaded.ok()) return loaded.status();
  return loaded->CallEval(args);
}

wasmtime_val_t I64(int64_t v) {
  return wasmtime_val_t{WASMTIME_I64, {.i64 = v}};
}
wasmtime_val_t I32(int32_t v) {
  return wasmtime_val_t{WASMTIME_I32, {.i32 = v}};
}
wasmtime_val_t F64(double v) {
  return wasmtime_val_t{WASMTIME_F64, {.f64 = v}};
}

TEST(EvalE2ETest, IntConstant) {
  auto r = Evaluate("42");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 42);
}

TEST(EvalE2ETest, IntArithmeticPrecedence) {
  auto r = Evaluate("1 + 2 * 3");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 7);
}

TEST(EvalE2ETest, IntSubtractionAndNegation) {
  auto r = Evaluate("-(1 + 2) + 10");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

TEST(EvalE2ETest, UintUnsignedDivision) {
  // If we accidentally used i64.div_s here, the bit pattern of the
  // uint divisor would be the same but the dividend would be read as
  // a signed negative — so exercising with a large uint is the
  // clearest guard.  For plain 10u/3u the two are equivalent; keep
  // the test simple and rely on the module_test opcode inspection
  // for the stronger structural proof.
  auto r = Evaluate("10u / 3u");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(static_cast<uint64_t>(r->of.i64), 3u);
}

TEST(EvalE2ETest, DoubleArithmetic) {
  auto r = Evaluate("1.5 + 2.25 * 4.0");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_F64);
  EXPECT_DOUBLE_EQ(r->of.f64, 1.5 + 2.25 * 4.0);
}

TEST(EvalE2ETest, BoolLiteralTrue) {
  auto r = Evaluate("true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, BoolLiteralFalse) {
  auto r = Evaluate("false");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2ETest, LogicalAndShortCircuit) {
  auto r = Evaluate("true && false");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 0);
}

TEST(EvalE2ETest, LogicalOrShortCircuit) {
  auto r = Evaluate("false || true");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, LogicalNot) {
  auto r = Evaluate("!false");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, IntComparisons) {
  EXPECT_EQ(Evaluate("1 < 2")->of.i32, 1);
  EXPECT_EQ(Evaluate("2 < 2")->of.i32, 0);
  EXPECT_EQ(Evaluate("2 <= 2")->of.i32, 1);
  EXPECT_EQ(Evaluate("3 > 2")->of.i32, 1);
  EXPECT_EQ(Evaluate("2 >= 3")->of.i32, 0);
  EXPECT_EQ(Evaluate("2 == 2")->of.i32, 1);
  EXPECT_EQ(Evaluate("2 != 2")->of.i32, 0);
}

TEST(EvalE2ETest, SignedIntComparisonTreatsNegativeCorrectly) {
  // If we had used unsigned comparison by mistake, -1 would read as
  // UINT64_MAX and the answer would flip.
  auto r = Evaluate("-1 < 0");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, DoubleComparisons) {
  // Per-op coverage mirroring IntComparisons.  A regression that used
  // int opcodes on f64 operands would produce either validator failure
  // or numerically wrong results on the ordered ops.
  EXPECT_EQ(Evaluate("1.0 < 2.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("2.0 < 2.0")->of.i32, 0);
  EXPECT_EQ(Evaluate("2.0 <= 2.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("3.0 > 2.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("2.0 >= 3.0")->of.i32, 0);
  EXPECT_EQ(Evaluate("1.0 == 1.0")->of.i32, 1);
  EXPECT_EQ(Evaluate("2.0 != 2.0")->of.i32, 0);
}

TEST(EvalE2ETest, UintComparisons) {
  // The important row is the ordered compares: if codegen used the signed
  // opcode, the answer for a very large uint would flip.  Using a value
  // above `INT64_MAX` makes that lurking bug visible.
  EXPECT_EQ(Evaluate("1u < 2u")->of.i32, 1);
  EXPECT_EQ(Evaluate("2u < 2u")->of.i32, 0);
  EXPECT_EQ(Evaluate("2u <= 2u")->of.i32, 1);
  EXPECT_EQ(Evaluate("3u > 2u")->of.i32, 1);
  EXPECT_EQ(Evaluate("2u >= 3u")->of.i32, 0);
  EXPECT_EQ(Evaluate("2u == 2u")->of.i32, 1);
  EXPECT_EQ(Evaluate("2u != 2u")->of.i32, 0);
  // 18446744073709551615u == 2^64 - 1; would be read as -1 if signed
  // compares leaked in.
  EXPECT_EQ(Evaluate("18446744073709551615u > 1u")->of.i32, 1);
}

TEST(EvalE2ETest, DoubleNegate) {
  // `-(x + y)` forces a non-constant negate so constant folding in the
  // parser can't short-circuit the codegen path.
  auto r = Evaluate("-(1.5 + 2.25)");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_F64);
  EXPECT_DOUBLE_EQ(r->of.f64, -(1.5 + 2.25));
}

TEST(EvalE2ETest, Ternary) {
  EXPECT_EQ(Evaluate("true ? 1 : 2")->of.i64, 1);
  EXPECT_EQ(Evaluate("false ? 1 : 2")->of.i64, 2);
}

TEST(EvalE2ETest, MixedArithAndConditional) {
  // (1+2)*3 == 9  →  true  →  returns 42.  Also exercises short-circuit.
  auto r = Evaluate("(1 + 2) * 3 == 9 ? 42 : -1");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 42);
}

TEST(EvalE2ETest, IntModulo) {
  auto r = Evaluate("10 % 3");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 1);
}

TEST(EvalE2ETest, IntVariableIsReadFromFirstParam) {
  // `x + 1` with x=41 exercises the full ident path: parse+check assigns
  // `x:int` an i64 Repr, codegen lays it out as param 0, and the runtime
  // call hands `41` in.  If the param-index assignment or the
  // `local.get` type ever drift, this test catches it.
  auto r = EvaluateWithVars("x + 1", {"x:int"}, {I64(41)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 42);
}

TEST(EvalE2ETest, BoolVariableInTernary) {
  auto r = EvaluateWithVars("flag ? 7 : 9", {"flag:bool"}, {I32(1)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
  auto r2 = EvaluateWithVars("flag ? 7 : 9", {"flag:bool"}, {I32(0)});
  ASSERT_THAT(r2.status(), IsOk());
  EXPECT_EQ(r2->of.i64, 9);
}

TEST(EvalE2ETest, DoubleVariableArithmetic) {
  auto r = EvaluateWithVars("x * 2.0", {"x:double"}, {F64(1.5)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_F64);
  EXPECT_DOUBLE_EQ(r->of.f64, 3.0);
}

TEST(EvalE2ETest, UintVariableUnsignedComparison) {
  // Signed-vs-unsigned matters here: if the compare opcode leaks in
  // signed, 2^63 would read as negative and this comparison would
  // flip.
  uint64_t big = 1ULL << 63;
  auto r =
      EvaluateWithVars("x > 1u", {"x:uint"}, {I64(static_cast<int64_t>(big))});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_EQ(r->of.i32, 1);
}

TEST(EvalE2ETest, TwoVariablesReadInDeclarationOrder) {
  // `x:int, y:int` then expr `x - y`.  With x=10, y=3 the result is 7.
  // If the codegen ever swapped the param indices, this would return
  // -7 (because `y - x`), which is a very visible regression.
  auto r = EvaluateWithVars("x - y", {"x:int", "y:int"}, {I64(10), I64(3)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

TEST(EvalE2ETest, UnreferencedVariableStillOccupiesParamSlot) {
  // Declaring `y` but referring only to `x` must still produce a
  // 2-param signature so the host ABI is deterministic.  Passing a
  // dummy value for `y` exercises the slot; the expression value
  // shouldn't depend on it.
  auto r = EvaluateWithVars("x + 1", {"x:int", "y:int"}, {I64(5), I64(999)});
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 6);
}

// Reads a string `CelValue` back out of the runtime's linear memory.
// The eval function returns an i32 offset into the shared memory; we
// need to pull the 24-byte CelValue header and the span's bytes from
// wasmtime to assert on the semantic result, not just the offset.
//
// Keeps the test coupled to the on-wire layout (the struct defined in
// cel_runtime.h) on purpose: a layout bump would break this read in a
// visible way, which is what we want — a layout change that silently
// works on the host but not in the runtime would be much worse.
struct DecodedString {
  uint32_t kind = 0;
  std::string payload;
};
absl::StatusOr<DecodedString> DecodeStringAt(LoadedEval& loaded,
                                             int32_t offset) {
  wasmtime_context_t* ctx = loaded.context();
  wasmtime_extern_t mem_ext;
  if (!wasmtime_instance_export_get(ctx, &loaded.runtime_instance(), "memory",
                                    6, &mem_ext)) {
    return absl::InternalError("runtime does not export `memory`");
  }
  if (mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
    return absl::InternalError("`memory` export is not a memory");
  }
  const uint8_t* base = wasmtime_memory_data(ctx, &mem_ext.of.memory);
  size_t size = wasmtime_memory_data_size(ctx, &mem_ext.of.memory);

  // CelValue offsets returned by the runtime are relative to the
  // runtime's `g_memory` array, not to wasm linear memory.  Ask the
  // runtime where g_memory lives and translate.
  wasmtime_extern_t mem_base_ext;
  if (!wasmtime_instance_export_get(ctx, &loaded.runtime_instance(),
                                    "cel_mem_base", std::strlen("cel_mem_base"),
                                    &mem_base_ext)) {
    return absl::InternalError("runtime does not export `cel_mem_base`");
  }
  wasmtime_val_t base_off{};
  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_func_call(
        ctx, &mem_base_ext.of.func, /*args=*/nullptr, /*nargs=*/0, &base_off,
        /*nresults=*/1, &trap);
    if (err != nullptr || trap != nullptr) {
      return absl::InternalError("cel_mem_base call failed");
    }
  }
  const uint32_t mem_base = static_cast<uint32_t>(base_off.of.i32);
  const uint64_t abs_cv =
      static_cast<uint64_t>(mem_base) + static_cast<uint64_t>(offset);
  if (offset <= 0 || abs_cv + sizeof(CelValue) > size) {
    return absl::OutOfRangeError("CelValue offset is outside memory bounds");
  }
  CelValue v;
  std::memcpy(&v, base + abs_cv, sizeof(v));
  DecodedString out;
  out.kind = v.kind;
  const uint32_t ptr = v.payload.s.ptr;
  const uint32_t len = v.payload.s.len;
  if (len > 0) {
    const uint64_t abs_bytes =
        static_cast<uint64_t>(mem_base) + static_cast<uint64_t>(ptr);
    if (abs_bytes + len > size) {
      return absl::OutOfRangeError(
          "string payload span falls outside runtime memory");
    }
    out.payload.assign(reinterpret_cast<const char*>(base + abs_bytes), len);
  }
  return out;
}

// Runs `cel_source` through the full pipeline, invokes `eval`, decodes
// the resulting i32 offset as a CEL string, and hands the decoded
// {kind, bytes} pair back.  The `kind` lives on the struct so callers
// can assert `CEL_STRING` (a codegen regression that returned a bool
// or an int offset would otherwise surface as surprising but passing
// bytes comparisons — better to pin the kind explicitly).
absl::StatusOr<DecodedString> EvaluateToString(absl::string_view cel_source) {
  auto typed = ParseAndCheck(cel_source, CheckOptions{});
  if (!typed.ok()) return typed.status();
  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  if (auto s = mod.Validate(); !s.ok()) return s;
  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();
  auto loaded = LoadEval(*bytes);
  if (!loaded.ok()) return loaded.status();
  auto result = loaded->CallNullaryEval();
  if (!result.ok()) return result.status();
  if (result->kind != WASMTIME_I32) {
    return absl::InternalError(
        "expected eval to return an i32 CelValue offset");
  }
  return DecodeStringAt(*loaded, result->of.i32);
}

TEST(EvalE2ETest, StringLiteralReturnsCelValueOffset) {
  // Strings travel as i32 offsets of CelValues in the runtime's arena.
  // The runtime instantiation populates the static singletons below
  // offset kStaticStart+4*sizeof(CelValue); our string's CelValue will
  // land beyond that.  Just assert positivity here — the
  // runtime-level semantics (offsets point to a CEL_STRING CelValue
  // whose bytes are "hello") are covered by compiler/runtime tests.
  auto r = Evaluate("'hello'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I32);
  EXPECT_GT(r->of.i32, 0);
}

TEST(EvalE2ETest, StringLiteralRoundTripsThroughMemory) {
  // The strongest literal-path assertion: a string constant survives
  // the `cel_alloc` + `i32.store8` + `cel_make_string_view` chain and
  // can be read back byte-identical via the runtime's shared memory.
  auto r = EvaluateToString("'hello'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(r->payload, "hello");
}

TEST(EvalE2ETest, EmptyStringRoundTrips) {
  // cel_alloc(0), zero store8s, cel_make_string_view(ptr, 0).  Must
  // produce a valid CelValue whose span is len=0.  Regressions where
  // the empty-path returns 0 (null offset) or the wrong kind would
  // cause DecodeStringAt to fail cleanly rather than silently pass.
  auto r = EvaluateToString("''");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(r->payload, "");
}

TEST(EvalE2ETest, StringConcatenationProducesJoinedBytes) {
  auto r = EvaluateToString("'hi' + 'there'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(r->payload, "hithere");
}

TEST(EvalE2ETest, StringConcatenationEmptyLhs) {
  // cel_string_concat's la=0 branch is the one that would regress if
  // someone reordered the memcpy guards.  Exercise it explicitly.
  auto r = EvaluateToString("'' + 'xy'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->payload, "xy");
}

TEST(EvalE2ETest, StringEqualityPositiveAndNegative) {
  EXPECT_EQ(Evaluate("'hi' == 'hi'")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hi' == 'bye'")->of.i32, 0);
  // Length-different operands hit the early-exit branch in span_eq;
  // same-length-different-bytes hits the memcmp branch.  Cover both.
  EXPECT_EQ(Evaluate("'ab' == 'abc'")->of.i32, 0);
  EXPECT_EQ(Evaluate("'ab' == 'ac'")->of.i32, 0);
  EXPECT_EQ(Evaluate("'' == ''")->of.i32, 1);
}

TEST(EvalE2ETest, StringInequalityInvertsEquality) {
  EXPECT_EQ(Evaluate("'hi' != 'hi'")->of.i32, 0);
  EXPECT_EQ(Evaluate("'hi' != 'bye'")->of.i32, 1);
}

TEST(EvalE2ETest, SizeOfAsciiStringIsByteCount) {
  // For ASCII, code-point count == byte count, so the observable
  // answer is the same.  The UTF-8 test below is where the codepoint
  // semantics actually matter.
  auto r = Evaluate("size('hello')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 5);
}

TEST(EvalE2ETest, SizeOfEmptyStringIsZero) {
  EXPECT_EQ(Evaluate("size('')")->of.i64, 0);
}

TEST(EvalE2ETest, SizeOfUtf8StringCountsCodepointsNotBytes) {
  // "héllo" is 6 bytes (`é` encodes as C3 A9) but 5 code points.
  // cel_string_size is defined to count code points (CEL §1110); a
  // regression that counted bytes instead would return 6 here.
  auto r = Evaluate("size('h\\u00e9llo')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 5);
}

TEST(EvalE2ETest, SizeOfConcatenatedString) {
  // End-to-end compose: concat(...) must produce a CEL_STRING whose
  // size the runtime can subsequently compute.  Exercises the path
  // that consumes a freshly-allocated concat result — a regression
  // that stashed the concat result in the wrong arena slot would
  // surface here as a wrong count or a trap.
  auto r = Evaluate("size('hi' + 'there')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

// M3 slice E: string member calls.  All three helpers are i32-returning
// runtime imports; these tests confirm the eval module lowers the CEL
// method syntax into a runtime call that honors the spec's edge cases
// (empty needle is true, longer needle than haystack is false).

TEST(EvalE2ETest, StartsWithPositiveAndNegative) {
  EXPECT_EQ(Evaluate("'hello'.startsWith('he')")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hello'.startsWith('lo')")->of.i32, 0);
}

TEST(EvalE2ETest, StartsWithEmptyPrefixIsTrue) {
  // CEL §9: every string starts with the empty string.  The runtime's
  // la=0 early-return guards this; a regression that dropped the guard
  // would memcmp zero bytes and still return 1, but if the zero-length
  // guard moved around after the allocation check the wrong branch
  // could return 0.
  EXPECT_EQ(Evaluate("'hello'.startsWith('')")->of.i32, 1);
}

TEST(EvalE2ETest, StartsWithLongerPrefixIsFalse) {
  EXPECT_EQ(Evaluate("'hi'.startsWith('hello')")->of.i32, 0);
}

TEST(EvalE2ETest, EndsWithPositiveAndNegative) {
  EXPECT_EQ(Evaluate("'hello'.endsWith('lo')")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hello'.endsWith('he')")->of.i32, 0);
}

TEST(EvalE2ETest, EndsWithEmptySuffixIsTrue) {
  EXPECT_EQ(Evaluate("'hello'.endsWith('')")->of.i32, 1);
}

TEST(EvalE2ETest, ContainsPositivePrefixMiddleSuffix) {
  // Three cases exercise the three search positions so a regression in
  // the search-loop bounds (off-by-one on the `last` computation)
  // surfaces in at least one assertion.
  EXPECT_EQ(Evaluate("'hello'.contains('he')")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hello'.contains('ell')")->of.i32, 1);
  EXPECT_EQ(Evaluate("'hello'.contains('lo')")->of.i32, 1);
}

TEST(EvalE2ETest, ContainsNegative) {
  EXPECT_EQ(Evaluate("'hello'.contains('xyz')")->of.i32, 0);
}

TEST(EvalE2ETest, ContainsEmptyNeedleIsTrue) {
  EXPECT_EQ(Evaluate("'hello'.contains('')")->of.i32, 1);
}

TEST(EvalE2ETest, ContainsLongerNeedleIsFalse) {
  EXPECT_EQ(Evaluate("'hi'.contains('hello')")->of.i32, 0);
}

// M3 slice F: bytes literals and operators.  The `DecodedString` helper
// reads out the CelValue regardless of kind, so bytes tests reuse the
// same decode path and assert `kind == CEL_BYTES` explicitly so a
// regression that swapped the span constructor would surface as a
// mismatched kind rather than silently-passing byte comparisons.  CEL
// bytes literals use the `b''` prefix; concatenation uses `+` like
// strings, and `size()` is a byte count (not a codepoint count).

TEST(EvalE2ETest, BytesLiteralRoundTripsThroughMemory) {
  auto r = EvaluateToString("b'hi'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(r->payload, "hi");
}

TEST(EvalE2ETest, EmptyBytesRoundTrips) {
  auto r = EvaluateToString("b''");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(r->payload, "");
}

TEST(EvalE2ETest, BytesLiteralPreservesHighBits) {
  // `b'\xff\x00\xfe'` — non-ASCII + an embedded NUL.  The store-per-byte
  // loop's `i32.const byte` must survive the sign-extension dance in
  // Binaryen; a regression that let a u8 get sign-extended through i32
  // would surface as a mismatched byte here (0xff would become -1 and
  // then zero-extend differently on the other side).
  auto r = EvaluateToString("b'\\xff\\x00\\xfe'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  ASSERT_EQ(r->payload.size(), 3u);
  EXPECT_EQ(static_cast<uint8_t>(r->payload[0]), 0xffu);
  EXPECT_EQ(static_cast<uint8_t>(r->payload[1]), 0x00u);
  EXPECT_EQ(static_cast<uint8_t>(r->payload[2]), 0xfeu);
}

TEST(EvalE2ETest, BytesConcatenationProducesJoinedBytes) {
  auto r = EvaluateToString("b'ab' + b'cd'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(r->payload, "abcd");
}

TEST(EvalE2ETest, BytesConcatenationEmptyLhs) {
  auto r = EvaluateToString("b'' + b'xy'");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(r->payload, "xy");
}

TEST(EvalE2ETest, BytesEqualityPositiveAndNegative) {
  EXPECT_EQ(Evaluate("b'hi' == b'hi'")->of.i32, 1);
  EXPECT_EQ(Evaluate("b'hi' == b'bye'")->of.i32, 0);
  EXPECT_EQ(Evaluate("b'ab' == b'abc'")->of.i32, 0);
  EXPECT_EQ(Evaluate("b'' == b''")->of.i32, 1);
}

TEST(EvalE2ETest, BytesInequalityInvertsEquality) {
  EXPECT_EQ(Evaluate("b'hi' != b'hi'")->of.i32, 0);
  EXPECT_EQ(Evaluate("b'hi' != b'bye'")->of.i32, 1);
}

TEST(EvalE2ETest, SizeOfBytesIsByteCount) {
  // The same 4-byte UTF-8 sequence whose string size is 1 codepoint
  // must report 4 here — `size(bytes)` is byte count per CEL §1110.
  // This is the semantic difference between `cel_string_size` and
  // `cel_bytes_size` that the codegen dispatch must honour.
  auto r = Evaluate("size(b'\\xf0\\x9f\\x98\\x80')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->kind, WASMTIME_I64);
  EXPECT_EQ(r->of.i64, 4);
}

TEST(EvalE2ETest, SizeOfEmptyBytesIsZero) {
  EXPECT_EQ(Evaluate("size(b'')")->of.i64, 0);
}

TEST(EvalE2ETest, SizeOfConcatenatedBytes) {
  // Mirror SizeOfConcatenatedString: the concat result's CelValue must
  // be usable as an input to size() in the same eval.  A regression
  // where the concat output landed in an arena slot that `cel_bytes_size`
  // couldn't read (stale `g_memory` pointer, for instance) would trap
  // here rather than return the wrong count.
  auto r = Evaluate("size(b'hi' + b'there')");
  ASSERT_THAT(r.status(), IsOk());
  EXPECT_EQ(r->of.i64, 7);
}

}  // namespace
}  // namespace celwasm
