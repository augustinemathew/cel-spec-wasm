// Sibling bench to `in_operator_bench.cc` — same expressions, same
// activations, evaluated through the **vendored cel-cpp tree-walking
// evaluator** instead of our wasm pipeline.  Lets us compare our
// codegen-to-wasm Eval steady state directly against cel-cpp's
// tree-walking eval head-to-head.
//
// SCOPE: Eval steady state ONLY.  cel-cpp compile (parse + check) and
// plan (`Runtime::CreateProgram`) happen ONCE outside the benchmark
// loop; only `program->Evaluate(arena, activation)` runs inside the
// timed region.  This mirrors how `in_operator_bench.cc` times only
// `Instance::Eval` for the BM_Eval_* family.
//
// LINKING CONSTRAINT: this TU links cel-cpp's `cel::Value` /
// `cel::Attribute` directly.  That symbol set clashes with our
// pipeline's `cel::Value` / `cel::Attribute` aliases under
// archive-scan-order-sensitive conditions (see testdata/BUILD.bazel
// comment on `cel_cpp_oracle_test`).  The bench is therefore
// **standalone** — it does NOT depend on any `//eval/...`,
// `//compiler:...`, `//shared:...`, or `//abi/...` target.  It pulls
// in cel-cpp's runtime + standard library + the benchmark framework
// and rebuilds the input corpora (int lists, IAM permission strings)
// inline rather than reuse our value builders.
//
// `manual`-tagged — invoke explicitly:
//   bazel run -c opt //bench:in_operator_cel_cpp_bench
//   bazel run -c opt //bench:in_operator_cel_cpp_bench -- \
//       --benchmark_filter=BM_Eval_CelCpp_In_1K_IamPermissions_Bound_Last \
//       --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "common/decl.h"
#include "common/type.h"
#include "common/value.h"
#include "compiler/compiler.h"
#include "compiler/compiler_factory.h"
#include "compiler/standard_library.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"
#include "runtime/activation.h"
#include "runtime/runtime.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"

namespace celwasm_bench_celcpp {
namespace {

// ─── Plan / runtime setup helpers (one-shot, outside the timed loop) ──

// Build a cel-cpp runtime with the standard library.  Matches the
// minimal subset of testdata/cel_cpp_oracle.cc's BuildRuntime that
// matters for plain (non-proto, non-partial-eval) `in` expressions.
// No reference resolver, no enum registration — these benches don't
// need either.
std::unique_ptr<const cel::Runtime> BuildRuntimeOrDie() {
  const google::protobuf::DescriptorPool* pool =
      google::protobuf::DescriptorPool::generated_pool();
  cel::RuntimeOptions opts;
  opts.enable_qualified_type_identifiers = true;
  opts.enable_heterogeneous_equality = true;
  auto builder = cel::CreateStandardRuntimeBuilder(pool, opts);
  ABSL_CHECK_OK(builder);
  auto runtime = std::move(*builder).Build();
  ABSL_CHECK_OK(runtime);
  return std::move(*runtime);
}

const cel::Runtime& GlobalRuntime() {
  static const cel::Runtime* r = BuildRuntimeOrDie().release();
  return *r;
}

// Compile (parse + type-check) `source`, declaring each (name, type)
// in `vars` so it resolves.  Returns a planned `cel::Program`.  Both
// the parse/check and the runtime plan happen here — outside the
// bench loop.
struct TypedVar {
  std::string name;
  cel::Type type;
};

std::unique_ptr<cel::Program> CompilePlanOrDie(
    absl::string_view source, const std::vector<TypedVar>& vars) {
  const google::protobuf::DescriptorPool* pool =
      google::protobuf::DescriptorPool::generated_pool();
  cel::CompilerOptions copts;
  copts.parser_options.max_recursion_depth = 16384;
  auto compiler_builder = cel::NewCompilerBuilder(pool, copts);
  ABSL_CHECK_OK(compiler_builder);
  ABSL_CHECK_OK(
      (*compiler_builder)->AddLibrary(cel::StandardCompilerLibrary()));
  auto& checker = (*compiler_builder)->GetCheckerBuilder();
  for (const TypedVar& v : vars) {
    ABSL_CHECK_OK(checker.AddVariable(cel::MakeVariableDecl(v.name, v.type)));
  }
  auto compiler = std::move(**compiler_builder).Build();
  ABSL_CHECK_OK(compiler);
  auto compiled = (*compiler)->Compile(source);
  ABSL_CHECK_OK(compiled);
  ABSL_CHECK(compiled->IsValid()) << "compile/check failed";
  auto ast = compiled->ReleaseAst();
  ABSL_CHECK_OK(ast);
  auto program = GlobalRuntime().CreateProgram(*std::move(ast));
  ABSL_CHECK_OK(program);
  return std::move(*program);
}

// ─── Source-text + value builders (mirror in_operator_bench.cc) ───────

std::string MakeIntListInSource(int n, absl::string_view var = "x") {
  std::string src = absl::StrCat(var, " in [");
  src.reserve(static_cast<size_t>(n) * 8);
  for (int i = 0; i < n; ++i) {
    if (i > 0) src.append(", ");
    absl::StrAppend(&src, i);
  }
  src.push_back(']');
  return src;
}

// Build a `cel::ListValue` of length `n` containing `[0, 1, ..., n-1]`.
// Lives in `arena` for the lifetime of the bench.
cel::ListValue MakeIntListValue(google::protobuf::Arena* arena, int n) {
  auto builder = cel::NewListValueBuilder(arena);
  builder->Reserve(n);
  for (int i = 0; i < n; ++i) {
    builder->UnsafeAdd(cel::Value(cel::IntValue(i)));
  }
  return std::move(*builder).Build();
}

// Realistic-looking GCP IAM permission strings, padded to exactly
// `target_len` bytes.  Copied verbatim from in_operator_bench.cc so
// the corpora are byte-identical.
std::vector<std::string> MakeIamPermissions(size_t n, size_t target_len = 50) {
  static const char* const kBases[] = {
      "aiplatform.batchPredictionJobs.cancel",
      "aiplatform.featurestores.entityTypes.viewMetric",
      "bigquery.connections.delegate",
      "bigquery.datasets.create",
      "bigquery.tables.getIamPolicy",
      "bigquerydatatransfer.transferConfigs.scheduleRuns",
      "cloudasset.assets.searchAllResources",
      "cloudbuild.builds.create",
      "cloudkms.cryptoKeyVersions.useToSignBytes",
      "cloudresourcemanager.organizations.setIamPolicy",
      "cloudresourcemanager.projects.searchOrgPolicies",
      "compute.instances.startWithEncryptionKey",
      "compute.regionInstanceGroupManagers.delete",
      "containeranalysis.notes.attachOccurrence",
      "dataproc.clusters.update",
      "dialogflow.intents.batchUpdate",
      "iam.serviceAccounts.actAs",
      "logging.logEntries.create",
      "monitoring.timeSeries.create",
      "pubsub.subscriptions.consume",
      "secretmanager.versions.access",
      "serviceusage.services.batchEnable",
      "spanner.databases.beginPartitionedDmlTransaction",
      "storage.objects.setIamPolicy",
      "workflows.workflows.create",
  };
  constexpr size_t kNumBases = sizeof(kBases) / sizeof(kBases[0]);

  std::vector<std::string> out;
  out.reserve(n);
  char idx_buf[16];
  for (size_t i = 0; i < n; ++i) {
    std::string p(kBases[i % kNumBases]);
    p.append(".v");
    std::snprintf(idx_buf, sizeof(idx_buf), "%zu", i);
    p.append(idx_buf);
    if (p.size() < target_len) {
      p.append(target_len - p.size(), '0');
    } else if (p.size() > target_len) {
      p.resize(target_len);
    }
    out.push_back(std::move(p));
  }
  return out;
}

std::string MakeStringListInSource(const std::vector<std::string>& items,
                                   absl::string_view var = "perm") {
  std::string src = absl::StrCat(var, " in [");
  src.reserve(items.size() * 55 + 16);
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) src.append(", ");
    src.push_back('"');
    src.append(items[i]);
    src.push_back('"');
  }
  src.push_back(']');
  return src;
}

cel::ListValue MakeStringListValue(google::protobuf::Arena* arena,
                                   const std::vector<std::string>& items) {
  auto builder = cel::NewListValueBuilder(arena);
  builder->Reserve(items.size());
  for (const auto& s : items) {
    builder->UnsafeAdd(cel::Value(cel::StringValue(arena, s)));
  }
  return std::move(*builder).Build();
}

// ─── Eval-loop helper ─────────────────────────────────────────────────

// Runs `program->Evaluate(&arena, activation)` once per iteration and
// validates the result is the expected boolean `want`.  Mirrors the
// shape of `Instance::Eval(act)` in in_operator_bench.cc, with the
// extra assertion that the cel-cpp result is correct (so a misbuilt
// activation surfaces as a bench failure rather than a noisy number).
void RunEvalLoop(benchmark::State& state, const cel::Program& program,
                 google::protobuf::Arena* arena,
                 const cel::Activation& activation, bool want) {
  // One pre-loop sanity check: confirm cel-cpp returns the expected
  // boolean on this activation.  If it doesn't, fail loudly instead
  // of reporting noise.
  {
    auto v = program.Evaluate(arena, activation);
    ABSL_CHECK_OK(v);
    auto b = v->AsBool();
    ABSL_CHECK(b.has_value()) << "cel-cpp returned a non-bool result";
    ABSL_CHECK_EQ(b->NativeValue(), want)
        << "cel-cpp returned the wrong bool — bench inputs are wrong";
  }
  for (auto _ : state) {
    auto v = program.Evaluate(arena, activation);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}

// ══════════════════════════════════════════════════════════════════════
// Scenario 1A: int LITERAL list `x in [0..N-1]`.
//
// Same N matrix as in_operator_bench.cc:
//   BM_Eval_In_IntList_Literal_WorstCase: 100, 1000.
// ══════════════════════════════════════════════════════════════════════

void BM_Eval_CelCpp_In_IntList_Literal_WorstCase(benchmark::State& state) {
  const int n = static_cast<int>(state.range(0));
  google::protobuf::Arena arena;
  auto program = CompilePlanOrDie(MakeIntListInSource(n),
                                  {{"x", cel::IntType()}});
  cel::Activation act;
  act.InsertOrAssignValue("x", cel::Value(cel::IntValue(n - 2)));
  RunEvalLoop(state, *program, &arena, act, /*want=*/true);
}
BENCHMARK(BM_Eval_CelCpp_In_IntList_Literal_WorstCase)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 1B: int BOUND list `x in xs`, xs of size N.
//
// Headline pairings:
//   BM_Eval_In_IntList_Bound_WorstCase  @ 100..1M
//   BM_Eval_In_1M_IntList_Bound_X_999999
//   BM_Eval_In_1M_IntList_Bound_X_0
//   BM_Eval_In_1M_IntList_Bound_X_NotPresent
// ══════════════════════════════════════════════════════════════════════

void BM_Eval_CelCpp_In_IntList_Bound_WorstCase(benchmark::State& state) {
  const int n = static_cast<int>(state.range(0));
  google::protobuf::Arena arena;
  auto program = CompilePlanOrDie(
      "x in xs",
      {{"x", cel::IntType()},
       {"xs", cel::ListType(&arena, cel::IntType())}});
  cel::Activation act;
  act.InsertOrAssignValue("x", cel::Value(cel::IntValue(n - 2)));
  act.InsertOrAssignValue("xs", cel::Value(MakeIntListValue(&arena, n)));
  RunEvalLoop(state, *program, &arena, act, /*want=*/true);
}
BENCHMARK(BM_Eval_CelCpp_In_IntList_Bound_WorstCase)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Arg(1000000)
    ->Unit(benchmark::kMicrosecond);

void BM_Eval_CelCpp_In_1M_IntList_Bound_X_999999(benchmark::State& state) {
  google::protobuf::Arena arena;
  auto program = CompilePlanOrDie(
      "x in xs",
      {{"x", cel::IntType()},
       {"xs", cel::ListType(&arena, cel::IntType())}});
  cel::Activation act;
  act.InsertOrAssignValue("x", cel::Value(cel::IntValue(999'999)));
  act.InsertOrAssignValue("xs",
                          cel::Value(MakeIntListValue(&arena, 1'000'000)));
  RunEvalLoop(state, *program, &arena, act, /*want=*/true);
}
BENCHMARK(BM_Eval_CelCpp_In_1M_IntList_Bound_X_999999)
    ->Unit(benchmark::kMicrosecond);

void BM_Eval_CelCpp_In_1M_IntList_Bound_X_0(benchmark::State& state) {
  google::protobuf::Arena arena;
  auto program = CompilePlanOrDie(
      "x in xs",
      {{"x", cel::IntType()},
       {"xs", cel::ListType(&arena, cel::IntType())}});
  cel::Activation act;
  act.InsertOrAssignValue("x", cel::Value(cel::IntValue(0)));
  act.InsertOrAssignValue("xs",
                          cel::Value(MakeIntListValue(&arena, 1'000'000)));
  RunEvalLoop(state, *program, &arena, act, /*want=*/true);
}
BENCHMARK(BM_Eval_CelCpp_In_1M_IntList_Bound_X_0)
    ->Unit(benchmark::kMicrosecond);

void BM_Eval_CelCpp_In_1M_IntList_Bound_X_NotPresent(benchmark::State& state) {
  google::protobuf::Arena arena;
  auto program = CompilePlanOrDie(
      "x in xs",
      {{"x", cel::IntType()},
       {"xs", cel::ListType(&arena, cel::IntType())}});
  cel::Activation act;
  act.InsertOrAssignValue("x", cel::Value(cel::IntValue(1'000'000)));
  act.InsertOrAssignValue("xs",
                          cel::Value(MakeIntListValue(&arena, 1'000'000)));
  RunEvalLoop(state, *program, &arena, act, /*want=*/false);
}
BENCHMARK(BM_Eval_CelCpp_In_1M_IntList_Bound_X_NotPresent)
    ->Unit(benchmark::kMicrosecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 2A: IAM permission LITERAL list `perm in ["p0",...,"pN-1"]`.
//
// Headline pairings (N=1000, fixed):
//   BM_Eval_In_1K_IamPermissions_Literal_Last
//   BM_Eval_In_1K_IamPermissions_Literal_First
//   BM_Eval_In_1K_IamPermissions_Literal_NotPresent
// ══════════════════════════════════════════════════════════════════════

void BM_Eval_CelCpp_In_1K_IamPermissions_Literal_Last(benchmark::State& state) {
  google::protobuf::Arena arena;
  const auto perms = MakeIamPermissions(1000);
  auto program = CompilePlanOrDie(MakeStringListInSource(perms),
                                  {{"perm", cel::StringType()}});
  cel::Activation act;
  act.InsertOrAssignValue(
      "perm", cel::Value(cel::StringValue(&arena, perms.back())));
  RunEvalLoop(state, *program, &arena, act, /*want=*/true);
}
BENCHMARK(BM_Eval_CelCpp_In_1K_IamPermissions_Literal_Last)
    ->Unit(benchmark::kMicrosecond);

void BM_Eval_CelCpp_In_1K_IamPermissions_Literal_First(
    benchmark::State& state) {
  google::protobuf::Arena arena;
  const auto perms = MakeIamPermissions(1000);
  auto program = CompilePlanOrDie(MakeStringListInSource(perms),
                                  {{"perm", cel::StringType()}});
  cel::Activation act;
  act.InsertOrAssignValue(
      "perm", cel::Value(cel::StringValue(&arena, perms.front())));
  RunEvalLoop(state, *program, &arena, act, /*want=*/true);
}
BENCHMARK(BM_Eval_CelCpp_In_1K_IamPermissions_Literal_First)
    ->Unit(benchmark::kMicrosecond);

void BM_Eval_CelCpp_In_1K_IamPermissions_Literal_NotPresent(
    benchmark::State& state) {
  google::protobuf::Arena arena;
  const auto perms = MakeIamPermissions(1000);
  auto program = CompilePlanOrDie(MakeStringListInSource(perms),
                                  {{"perm", cel::StringType()}});
  std::string needle = "absent.service.deny";
  needle.append(50 - needle.size(), '0');
  cel::Activation act;
  act.InsertOrAssignValue("perm",
                          cel::Value(cel::StringValue(&arena, needle)));
  RunEvalLoop(state, *program, &arena, act, /*want=*/false);
}
BENCHMARK(BM_Eval_CelCpp_In_1K_IamPermissions_Literal_NotPresent)
    ->Unit(benchmark::kMicrosecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 2B: IAM permission BOUND list `perm in perms`.
//
// Same N matrix as in_operator_bench.cc (capped at 1000):
//   BM_Eval_In_IamPermissions_Bound_Last @ 100, 1000.
// ══════════════════════════════════════════════════════════════════════

void BM_Eval_CelCpp_In_IamPermissions_Bound_Last(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  google::protobuf::Arena arena;
  const auto perms = MakeIamPermissions(n);
  auto program = CompilePlanOrDie(
      "perm in perms",
      {{"perm", cel::StringType()},
       {"perms", cel::ListType(&arena, cel::StringType())}});
  cel::Activation act;
  act.InsertOrAssignValue(
      "perm", cel::Value(cel::StringValue(&arena, perms.back())));
  act.InsertOrAssignValue("perms",
                          cel::Value(MakeStringListValue(&arena, perms)));
  RunEvalLoop(state, *program, &arena, act, /*want=*/true);
}
BENCHMARK(BM_Eval_CelCpp_In_IamPermissions_Bound_Last)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 4: long arithmetic expression — 50-term polynomial.
//
// Byte-for-byte the same source string as
// `BM_Eval_LongArith_10kTerms` in `in_operator_bench.cc`, evaluated
// through cel-cpp's tree-walking runtime so the delta is the
// architecture choice (AOT-wasm + Cranelift vs interpreted tree walk),
// not the workload.
// ══════════════════════════════════════════════════════════════════════

// Mirror of `MakeLongArithSource()` on the celwasmc side — same
// pseudo-random stride so the two sources are textually identical.
std::string MakeLongArithSource() {
  constexpr int kTerms = 1000;
  static constexpr char kVars[] = "abcdefghij";
  std::string s;
  s.reserve(kTerms * 5);
  for (int i = 0; i < kTerms; ++i) {
    if (i > 0) s.append(" + ");
    s.push_back(kVars[i % 10]);
    s.push_back('*');
    s.push_back(kVars[(i * 7 + 3) % 10]);
  }
  return s;
}

void BM_Eval_CelCpp_LongArith_10kTerms(benchmark::State& state) {
  std::vector<TypedVar> vars;
  vars.reserve(10);
  for (char ch = 'a'; ch <= 'j'; ++ch) {
    vars.push_back({std::string(1, ch), cel::IntType()});
  }
  auto program = CompilePlanOrDie(MakeLongArithSource(), vars);

  static constexpr int64_t kVals[10] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29};
  google::protobuf::Arena arena;
  cel::Activation act;
  for (int i = 0; i < 10; ++i) {
    act.InsertOrAssignValue(std::string(1, static_cast<char>('a' + i)),
                            cel::Value(cel::IntValue(kVals[i])));
  }

  for (auto _ : state) {
    auto v = program->Evaluate(&arena, act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_CelCpp_LongArith_10kTerms)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace celwasm_bench_celcpp

BENCHMARK_MAIN();
