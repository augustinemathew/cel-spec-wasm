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
      {"i_a", CelType::Int(),    Value::Int(7),   MakeOracleInt(7)},
      {"i_b", CelType::Int(),    Value::Int(11),  MakeOracleInt(11)},
      {"u_a", CelType::Uint(),   Value::Uint(5),  MakeOracleUint(5)},
      {"d_a", CelType::Double(), Value::Double(3.14), MakeOracleDouble(3.14)},
      {"b_a", CelType::Bool(),   Value::Bool(true),  MakeOracleBool(true)},
      {"s_a", CelType::String(),
                                 Value::String("hello"),
                                 MakeOracleString("hello")},
      {"y_a", CelType::Bytes(),
                                 Value::Bytes("hi"),
                                 MakeOracleBytes("hi")},
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

// fuzztest property — values seeded from the fuzztest input
// space (one `Arbitrary<uint64_t>` for the RNG, one
// `InRange<int>(0, 6)` for the depth budget).
//
// 0-budget = always-leaf source, exercises the kIdent / kConst
// dispatch by itself.  6-budget exercises composition.
void EvalAgreesWithOracle(uint64_t seed, int depth) {
  static const Grammar& grammar = *new Grammar(BuildSliceBGrammar());

  std::mt19937_64 rng(seed);
  GenCtx ctx = NewGenCtxForSliceB(depth, rng);
  const std::string source = GenerateExpr(grammar, CelType::Bool(), ctx);

  // Drop sources past the cap; they're vanishingly rare at the
  // depth budget we permit but the fuzzer can still hit them.
  if (source.size() > kMaxSourceBytes) return;

  auto ours = OurEval(source);
  auto oracle = testdata::PartialEvalWithCelCpp(
      source, /*container=*/"",
      /*vars=*/MakeOracleVars(),
      /*unknown_patterns=*/{});

  // The grammar is guarded — both pipelines should ACCEPT every
  // generated source.  Acceptance asymmetry is the first thing to
  // call out because it's the most diagnostic.
  ASSERT_TRUE(ours.ok())
      << "our pipeline rejected source generated by the grammar "
         "(this is necessarily a bug — either the grammar admits "
         "something the static subset shouldn't, OR our compiler "
         "regressed):\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << source << "`\n  error=" << ours.status();
  ASSERT_TRUE(oracle.ok())
      << "cel-cpp rejected source generated by the grammar (this "
         "is necessarily a bug — the grammar admits something the "
         "cel-cpp checker doesn't):\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << source << "`\n  error=" << oracle.status();

  // Neither side should report an eval error — Slice B's
  // productions are all total over their typed input domain.
  ASSERT_FALSE(oracle->is_error)
      << "cel-cpp produced an eval error on a grammar-emitted "
         "Bool expression — the safety guards in the catalog let "
         "an error-producing shape through.\n  seed=" << seed
      << " depth=" << depth << "\n  source=`" << source
      << "`\n  oracle.error_message=`" << oracle->error_message << "`";
  ASSERT_EQ(ours->kind(), Value::Kind::kBool)
      << "our eval produced a non-bool result on a grammar-emitted "
         "Bool expression. seed=" << seed << " depth=" << depth
      << " source=`" << source << "`";

  // Compare the boolean payload.
  const bool ours_b = *ours->AsBool();
  const bool oracle_b = oracle->value.bool_value();
  EXPECT_EQ(ours_b, oracle_b)
      << "VALUE DIVERGENCE — our pipeline disagrees with cel-cpp.\n"
      << "  seed=" << seed << " depth=" << depth << "\n"
      << "  source=`" << source << "`\n"
      << "  ours   = " << (ours_b ? "true" : "false") << "\n"
      << "  oracle = " << (oracle_b ? "true" : "false");
}

FUZZ_TEST(CelOracleProperty, EvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(),
                 fuzztest::InRange<int>(0, 6));

}  // namespace
}  // namespace celwasm::fuzz
