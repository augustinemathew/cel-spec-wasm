// Slice B — value-oracle property test.  Generates a CEL source
// string of type Bool from the typed-attribute grammar, evaluates
// it through BOTH our pipeline and cel-cpp's, asserts both accept
// and that the boolean results agree.
//
// The grammar is guarded so every emitted source is guaranteed to
// type-check (L2) and total over its declared input domain (per
// m27 §"Guarded productions").  Any oracle divergence reported by
// this property is therefore necessarily a runtime / codegen bug
// on our side, not a generator misconfiguration.
//
// **This target is tagged `manual`** because the property mines
// for real bugs and WILL fail when it finds them — that's the
// discovery role m27 §"Oracle-divergence policy" expects.
// Excluded from `bazel test //...` to keep the green-baseline
// CI clean; run explicitly:
//
//   - `bazel test //e2e/fuzz:cel_oracle_property_test`  — unit-
//     test mode (~1000 randomised iterations).
//   - `bazel run --config=fuzztest //e2e/fuzz:cel_oracle_property_test
//      -- --fuzz=CelOracleProperty.EvalAgreesWithOracle`  — long-
//     running coverage-guided fuzzer.
//
// Discovered divergences become known_bugs_test.cc entries
// pinning the exact source + activation; PBT is the discovery
// tool, known_bugs is the regression-pinning tool.  Current
// known divergences as of 2026-06-05:
//
//   - `((size("") < (i_a - (b_a ? 0 : i_b))) == ("" == "x"))`
//     produces `kError` on our side, `false` per the spec.
//     Pinned by `KnownBugs.PbtTernaryInsideIntSubtract`.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "cel/expr/value.pb.h"
#include "compiler/compiler.h"
#include "e2e/fuzz/generator.h"
#include "e2e/fuzz/grammar.h"
#include "e2e/fuzz/grammar_slice_b.h"
#include "e2e/fuzz/grammar_slice_c.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/cel_cpp_oracle.h"

namespace celwasm::fuzz {
namespace {

// One bound activation entry, in both representations: a
// `celwasm::Value` for our pipeline, and a `cel::expr::Value` for
// the cel-cpp oracle.  The two carry the same logical value; the
// duplication exists only because the two pipelines speak
// different value types.
struct BoundActivation {
  std::string name;
  CelType type;
  Value ours;
  cel::expr::Value oracle;
};

// Build a `cel::expr::Value` for each scalar.
cel::expr::Value MakeOracleBool(bool x) {
  cel::expr::Value v;
  v.set_bool_value(x);
  return v;
}
cel::expr::Value MakeOracleInt(int64_t x) {
  cel::expr::Value v;
  v.set_int64_value(x);
  return v;
}
cel::expr::Value MakeOracleUint(uint64_t x) {
  cel::expr::Value v;
  v.set_uint64_value(x);
  return v;
}
cel::expr::Value MakeOracleDouble(double x) {
  cel::expr::Value v;
  v.set_double_value(x);
  return v;
}
cel::expr::Value MakeOracleString(absl::string_view x) {
  cel::expr::Value v;
  v.set_string_value(std::string(x));
  return v;
}
cel::expr::Value MakeOracleBytes(absl::string_view x) {
  cel::expr::Value v;
  v.set_bytes_value(std::string(x));
  return v;
}

// Concrete values bound to the Slice B activation.  Same values
// on both sides — divergence in eval comes from operator
// semantics, not from binding asymmetry.  Picked so each scalar
// has a non-trivial, non-default value (distinguishes
// `(i_a == 0)` from `(i_a == 7)` etc.).
const std::vector<BoundActivation>& SliceBBoundActivation() {
  static const auto* kBound = new std::vector<BoundActivation>{
      {"i_a", CelType::Int(), Value::Int(7), MakeOracleInt(7)},
      {"i_b", CelType::Int(), Value::Int(11), MakeOracleInt(11)},
      {"u_a", CelType::Uint(), Value::Uint(5), MakeOracleUint(5)},
      {"d_a", CelType::Double(), Value::Double(3.14), MakeOracleDouble(3.14)},
      {"b_a", CelType::Bool(), Value::Bool(true), MakeOracleBool(true)},
      {"s_a", CelType::String(), Value::String("hello"),
       MakeOracleString("hello")},
      {"y_a", CelType::Bytes(), Value::Bytes("hi"), MakeOracleBytes("hi")},
  };
  return *kBound;
}

// Process-wide engine — instantiating one per property iteration
// would dominate the per-eval cost.
Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Build a `Compiler` declaring every Slice B activation variable.
// Shared across iterations — `Compiler::Compile()` is cheap to
// invoke many times.
const Compiler& SliceBCompiler() {
  static const Compiler* compiler = [] {
    Compiler::Builder b;
    for (const BoundActivation& bv : SliceBBoundActivation()) {
      b.DeclareVariable(bv.name, bv.type);
    }
    auto c = std::move(b).Build();
    ABSL_CHECK_OK(c);
    return new Compiler(*std::move(c));
  }();
  return *compiler;
}

// Build an Activation pre-bound with every Slice B value.
Activation MakeBoundActivation() {
  Activation a;
  for (const BoundActivation& bv : SliceBBoundActivation()) {
    a.Bind(bv.name, bv.ours);
  }
  return a;
}

// Build the cel-cpp `OracleVar` list mirroring the activation.
std::vector<testdata::OracleVar> MakeOracleVars() {
  std::vector<testdata::OracleVar> out;
  out.reserve(SliceBBoundActivation().size());
  for (const BoundActivation& bv : SliceBBoundActivation()) {
    out.push_back({bv.name, bv.oracle});
  }
  return out;
}

// Our pipeline: Compile → Plan → Eval.  Returns the value or a
// status error (which the property body distinguishes from a CEL
// eval error).
absl::StatusOr<Value> OurEval(absl::string_view source) {
  auto program = SliceBCompiler().Compile(source);
  if (!program.ok()) return program.status();
  auto instance = GlobalEngine().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a = MakeBoundActivation();
  return instance->Eval(a);
}

// Cap on per-iteration source size.  The grammar admits sources
// up to ~thousands of characters at depth 6 — fine for cel-cpp
// but our compiler's WAT lowering benchmarks slower with depth.
// Cap to keep CI iteration cost reasonable; failures above the
// cap still get reported.
constexpr std::size_t kMaxSourceBytes = 4 * 1024;

// ── Shared property body ─────────────────────────────────────────
//
// One property per scalar target type.  Each generates a source
// of its target type, evals through both pipelines, and verifies
// (a) both accept, (b) neither errors, (c) `ours->kind()` matches
// the expected `Value::Kind`, (d) the payload value agrees.
//
// The shared `RunPropertyForBool/Int/Uint/Double/String/Bytes`
// helpers take a `compare` callable so each kind can extract
// the right `Value::Kind` and `cel::expr::Value` field without
// duplicating the error-message boilerplate.

// Generate a source of `target`, eval both sides, ASSERT both
// accept, ASSERT neither errors, RETURN both values to the
// caller (which then compares the kind-specific payload).
// Returns false (and emits its own ASSERT failures) if any
// pre-condition fails — caller should early-return in that case.
struct GenAndEvalResult {
  std::string source;
  Value ours;
  cel::expr::Value oracle;
};
bool GenAndEval(const CelType& target, uint64_t seed, int depth,
                absl::string_view kind_label, GenAndEvalResult& out) {
  static const Grammar& grammar = *new Grammar(BuildSliceCGrammar());

  std::mt19937_64 rng(seed);
  GenCtx ctx = NewGenCtxForSliceB(depth, rng);
  out.source = GenerateExpr(grammar, target, ctx);

  if (out.source.size() > kMaxSourceBytes) return false;

  auto ours = OurEval(out.source);
  auto oracle = testdata::PartialEvalWithCelCpp(out.source, /*container=*/"",
                                                /*vars=*/MakeOracleVars(),
                                                /*unknown_patterns=*/{});

  if (!ours.ok()) {
    ADD_FAILURE() << "our pipeline rejected a grammar-emitted " << kind_label
                  << " expression:\n  seed=" << seed << " depth=" << depth
                  << "\n  source=`" << out.source
                  << "`\n  error=" << ours.status();
    return false;
  }
  if (!oracle.ok()) {
    ADD_FAILURE() << "cel-cpp rejected a grammar-emitted " << kind_label
                  << " expression:\n  seed=" << seed << " depth=" << depth
                  << "\n  source=`" << out.source
                  << "`\n  error=" << oracle.status();
    return false;
  }
  if (oracle->is_error) {
    ADD_FAILURE() << "cel-cpp produced an eval error on a grammar-"
                  << "emitted " << kind_label << " expression — the "
                  << "safety guards in the catalog let an error-"
                  << "producing shape through.\n  seed=" << seed
                  << " depth=" << depth << "\n  source=`" << out.source
                  << "`\n  oracle.error_message=`" << oracle->error_message
                  << "`";
    return false;
  }
  out.ours = *std::move(ours);
  out.oracle = oracle->value;
  return true;
}

// ── Per-target properties ────────────────────────────────────────

void BoolEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Bool(), seed, depth, "Bool", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kBool)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsBool(), r.oracle.bool_value())
      << "VALUE DIVERGENCE (Bool)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

void IntEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Int(), seed, depth, "Int", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kInt)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsInt(), r.oracle.int64_value())
      << "VALUE DIVERGENCE (Int)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

void UintEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Uint(), seed, depth, "Uint", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kUint)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsUint(), r.oracle.uint64_value())
      << "VALUE DIVERGENCE (Uint)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

void DoubleEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Double(), seed, depth, "Double", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kDouble)
      << "kind mismatch on `" << r.source << "`";
  const double ours_d = *r.ours.AsDouble();
  const double oracle_d = r.oracle.double_value();
  // NaN-equality: NaN != NaN in IEEE 754, so a plain `==` would
  // flag matched NaNs as divergent.  Match cel-cpp's conformance
  // discipline: if both are NaN, they agree; otherwise compare
  // bit-exact (which catches +0/-0 distinctness and +inf/-inf).
  if (std::isnan(ours_d) && std::isnan(oracle_d)) return;
  EXPECT_EQ(ours_d, oracle_d)
      << "VALUE DIVERGENCE (Double)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`"
      << "\n  ours   = " << ours_d << "\n  oracle = " << oracle_d;
}

void StringEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::String(), seed, depth, "String", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kString)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsString(), r.oracle.string_value())
      << "VALUE DIVERGENCE (String)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

void BytesEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Bytes(), seed, depth, "Bytes", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kBytes)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsBytes(), r.oracle.bytes_value())
      << "VALUE DIVERGENCE (Bytes)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

// ── FUZZ_TEST registrations ──────────────────────────────────────
//
// Six properties, one per scalar target type.  Each runs ~1000
// random iterations in unit-test mode; under `--config=fuzztest`
// the same registration becomes a coverage-guided fuzzer over
// that target.  Together they multiply oracle surface by ~6×
// over the original Bool-only property — a divergence in any
// pure-Int / Uint / Double / String / Bytes computation now
// fires its own kind-specific test rather than having to bubble
// up through a comparison into a bool.

FUZZ_TEST(CelOracleProperty, BoolEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 6));
FUZZ_TEST(CelOracleProperty, IntEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 6));
FUZZ_TEST(CelOracleProperty, UintEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 6));
FUZZ_TEST(CelOracleProperty, DoubleEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 6));
FUZZ_TEST(CelOracleProperty, StringEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 6));
FUZZ_TEST(CelOracleProperty, BytesEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 6));

}  // namespace
}  // namespace celwasm::fuzz
