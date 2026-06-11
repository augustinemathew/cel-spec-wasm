#include "e2e/fuzz/oracle_harness.h"

#include <cstdint>
#include <initializer_list>
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
#include "e2e/fuzz/grammar_aggregates.h"
#include "e2e/fuzz/grammar_scalars.h"
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
cel::expr::Value MakeOracleIntList(std::initializer_list<int64_t> xs) {
  cel::expr::Value v;
  auto* list = v.mutable_list_value();
  for (int64_t x : xs) {
    list->add_values()->set_int64_value(x);
  }
  return v;
}
cel::expr::Value MakeOracleStringIntMap(
    std::initializer_list<std::pair<absl::string_view, int64_t>> kvs) {
  cel::expr::Value v;
  auto* map = v.mutable_map_value();
  for (const auto& [k, x] : kvs) {
    auto* entry = map->add_entries();
    entry->mutable_key()->set_string_value(std::string(k));
    entry->mutable_value()->set_int64_value(x);
  }
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

// Compiler declaring every fuzz activation variable.  Shared
// across iterations.
const Compiler& SchemaCompiler() {
  static const Compiler* compiler = [] {
    Compiler::Builder b;
    for (const BoundActivation& bv : BoundActivationEntries()) {
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
  for (const BoundActivation& bv : BoundActivationEntries()) {
    a.Bind(bv.name, bv.ours);
  }
  return a;
}

}  // namespace

namespace {

// The concrete value bound to one ActivationSchema() entry, in
// both our and cel-cpp representations.  The grammar's schema is
// the single source of truth for WHICH variables exist; this is
// the only place that says what they're bound to.  An entry added
// to the schema without a value here CHECK-fails at first use —
// the two can never drift silently (they did once: the property
// test broke the day `xs` landed in a second hand-synced copy).
BoundActivation MakeEntry(const ActivationBinding& v) {
  if (v.name == "i_a") return {v.name, v.type, Value::Int(7), MakeOracleInt(7)};
  if (v.name == "i_b") {
    return {v.name, v.type, Value::Int(11), MakeOracleInt(11)};
  }
  if (v.name == "u_a") {
    return {v.name, v.type, Value::Uint(5), MakeOracleUint(5)};
  }
  if (v.name == "d_a") {
    return {v.name, v.type, Value::Double(3.14), MakeOracleDouble(3.14)};
  }
  if (v.name == "b_a") {
    return {v.name, v.type, Value::Bool(true), MakeOracleBool(true)};
  }
  if (v.name == "s_a") {
    return {v.name, v.type, Value::String("hello"), MakeOracleString("hello")};
  }
  if (v.name == "y_a") {
    return {v.name, v.type, Value::Bytes("hi"), MakeOracleBytes("hi")};
  }
  // Bound aggregates — these reach the host-origin list/map paths
  // that literal aggregates never do.
  if (v.name == "xs") {
    return {v.name, v.type,
            Value::List({Value::Int(1), Value::Int(2), Value::Int(3)}),
            MakeOracleIntList({1, 2, 3})};
  }
  if (v.name == "ms") {
    return {v.name, v.type,
            Value::Map({{Value::String("a"), Value::Int(2)},
                        {Value::String("b"), Value::Int(3)}}),
            MakeOracleStringIntMap({{"a", 2}, {"b", 3}})};
  }
  ABSL_CHECK(false) << "activation schema entry `" << v.name
                    << "` has no bound value — extend MakeEntry in "
                       "oracle_harness.cc alongside ActivationSchema()";
}

}  // namespace

const std::vector<BoundActivation>& BoundActivationEntries() {
  static const auto* kBound = [] {
    auto* out = new std::vector<BoundActivation>();
    for (const ActivationBinding& v : ActivationSchema()) {
      out->push_back(MakeEntry(v));
    }
    return out;
  }();
  return *kBound;
}

std::vector<testdata::OracleVar> MakeOracleVars() {
  std::vector<testdata::OracleVar> out;
  out.reserve(BoundActivationEntries().size());
  for (const BoundActivation& bv : BoundActivationEntries()) {
    out.push_back({bv.name, bv.oracle});
  }
  return out;
}

absl::StatusOr<Value> OurEval(absl::string_view source) {
  auto program = SchemaCompiler().Compile(source);
  if (!program.ok()) return program.status();
  auto instance = GlobalEngine().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a = MakeBoundActivation();
  return instance->Eval(a);
}

GenAndEvalStatus GenAndEvalFull(const CelType& target, uint64_t seed, int depth,
                                GenAndEvalResult& out, std::string* error_out) {
  static const Grammar& grammar = *new Grammar(BuildFullGrammar());

  std::mt19937_64 rng(seed);
  GenCtx ctx = NewGenCtx(depth, rng);
  out.source = GenerateExpr(grammar, target, ctx);

  if (out.source.size() > kMaxSourceBytes) {
    if (error_out != nullptr) {
      *error_out = absl::StrCat("source too large (", out.source.size(), " > ",
                                kMaxSourceBytes, ")");
    }
    return GenAndEvalStatus::kSourceTooLarge;
  }

  auto ours = OurEval(out.source);
  auto oracle = testdata::PartialEvalWithCelCpp(out.source, /*container=*/"",
                                                /*vars=*/MakeOracleVars(),
                                                /*unknown_patterns=*/{});

  if (!ours.ok()) {
    out.our_status = ours.status();
    if (error_out != nullptr) *error_out = ours.status().ToString();
    return GenAndEvalStatus::kOurPipelineRejected;
  }
  if (!oracle.ok()) {
    if (error_out != nullptr) *error_out = oracle.status().ToString();
    return GenAndEvalStatus::kOracleRejected;
  }
  if (oracle->is_error) {
    if (error_out != nullptr) *error_out = oracle->error_message;
    out.ours = *std::move(ours);
    return out.ours.kind() == Value::Kind::kError
               ? GenAndEvalStatus::kBothErrored
               : GenAndEvalStatus::kOracleErrorOnly;
  }
  out.ours = *std::move(ours);
  out.oracle = oracle->value;
  return GenAndEvalStatus::kOk;
}

}  // namespace celwasm::fuzz
