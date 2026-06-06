#include "e2e/fuzz/oracle_harness.h"

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/value.pb.h"
#include "compiler/compiler.h"
#include "e2e/fuzz/generator.h"
#include "e2e/fuzz/grammar.h"
#include "e2e/fuzz/grammar_slice_c.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"
#include "testdata/cel_cpp_oracle.h"

namespace celwasm::fuzz {
namespace {

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

// Process-wide engine — instantiating one per iteration would
// dominate the per-eval cost.
Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Compiler declaring every Slice B activation variable.  Shared
// across iterations.
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

Activation MakeBoundActivation() {
  Activation a;
  for (const BoundActivation& bv : SliceBBoundActivation()) {
    a.Bind(bv.name, bv.ours);
  }
  return a;
}

}  // namespace

const std::vector<BoundActivation>& SliceBBoundActivation() {
  static const auto* kBound = new std::vector<BoundActivation>{
      {"i_a", CelType::Int(),    Value::Int(7),       MakeOracleInt(7)},
      {"i_b", CelType::Int(),    Value::Int(11),      MakeOracleInt(11)},
      {"u_a", CelType::Uint(),   Value::Uint(5),      MakeOracleUint(5)},
      {"d_a", CelType::Double(), Value::Double(3.14), MakeOracleDouble(3.14)},
      {"b_a", CelType::Bool(),   Value::Bool(true),   MakeOracleBool(true)},
      {"s_a", CelType::String(), Value::String("hello"),
                                 MakeOracleString("hello")},
      {"y_a", CelType::Bytes(),  Value::Bytes("hi"),
                                 MakeOracleBytes("hi")},
  };
  return *kBound;
}

std::vector<testdata::OracleVar> MakeOracleVars() {
  std::vector<testdata::OracleVar> out;
  out.reserve(SliceBBoundActivation().size());
  for (const BoundActivation& bv : SliceBBoundActivation()) {
    out.push_back({bv.name, bv.oracle});
  }
  return out;
}

absl::StatusOr<Value> OurEval(absl::string_view source) {
  auto program = SliceBCompiler().Compile(source);
  if (!program.ok()) return program.status();
  auto instance = GlobalEngine().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a = MakeBoundActivation();
  return instance->Eval(a);
}

GenAndEvalStatus GenAndEvalSliceC(const CelType& target, uint64_t seed,
                                  int depth, GenAndEvalResult& out,
                                  std::string* error_out) {
  static const Grammar& grammar = *new Grammar(BuildSliceCGrammar());

  std::mt19937_64 rng(seed);
  GenCtx ctx = NewGenCtxForSliceB(depth, rng);
  out.source = GenerateExpr(grammar, target, ctx);

  if (out.source.size() > kMaxSourceBytes) {
    if (error_out != nullptr) {
      *error_out = absl::StrCat("source too large (",
                                out.source.size(), " > ",
                                kMaxSourceBytes, ")");
    }
    return GenAndEvalStatus::kSourceTooLarge;
  }

  auto ours = OurEval(out.source);
  auto oracle = testdata::PartialEvalWithCelCpp(
      out.source, /*container=*/"", /*vars=*/MakeOracleVars(),
      /*unknown_patterns=*/{});

  if (!ours.ok()) {
    if (error_out != nullptr) *error_out = ours.status().ToString();
    return GenAndEvalStatus::kOurPipelineRejected;
  }
  if (!oracle.ok()) {
    if (error_out != nullptr) *error_out = oracle.status().ToString();
    return GenAndEvalStatus::kOracleRejected;
  }
  if (oracle->is_error) {
    if (error_out != nullptr) *error_out = oracle->error_message;
    return GenAndEvalStatus::kOracleErrorValue;
  }
  out.ours = *std::move(ours);
  out.oracle = oracle->value;
  return GenAndEvalStatus::kOk;
}

}  // namespace celwasm::fuzz
