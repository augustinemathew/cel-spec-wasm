// Compile/Plan-phase cost of large-list `in`-operator expressions —
// IAM-style authorization shapes with the list inline in the source.
//
// Two real-world LITERAL shapes:
//
//   1. `x in [1, 2, ..., N]`  (int membership)
//   2. `perm in [<IAM permission strings, exactly 50 bytes each>]`
//
// The list is inline in the CEL source; codegen materialises it into
// the static-memory arena.  The cel-cpp parser caps source at
// `expression_size_codepoint_limit = 100_000` codepoints (see
// third_party/cel-cpp/parser/options.h:37), so the literal benches cap
// at 10 000 ints / 1000 strings, both of which fit well under the cap
// (a 10 k int list compiles to ~98 kB of source; a 1 k 50-byte string
// list to ~55 kB).
//
// Each shape is timed at two pipeline boundaries:
//   - BM_Compile_* — parse + type-check + codegen + cel.abi serialize.
//   - BM_Plan_*    — wasmtime module link + static-memory snapshot.
// The Eval steady state of the same shapes is covered by the
// corpus-driven benches under `benchmark/eval/`.
//
// `manual`-tagged — invoke explicitly:
//   bazel run -c opt //benchmark/compiler:in_operator_compile_bench

#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "shared/type.h"

namespace celwasm {
namespace {

// One shared engine across all benches; engine construction is amortised
// in real use and shouldn't be double-charged here.
Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

Compiler MakeCompilerWith(absl::string_view name, const CelType& ty) {
  Compiler::Builder b;
  b.DeclareVariable(std::string(name), ty);
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

// All benches compile at the highest Binaryen optimization level
// (CLAUDE.md "Benchmark configuration"); a bench number from an
// unoptimised expr module isn't representative of what production runs.
constexpr int kBenchOptimizeLevel = 2;

Program CompileOrDie(const Compiler& c, absl::string_view src) {
  CompilerOptions opts;
  opts.optimize_level = kBenchOptimizeLevel;
  auto p = c.Compile(src, opts);
  ABSL_CHECK_OK(p) << "compile failed (source size = " << src.size() << ")";
  return *std::move(p);
}

// ─── Source-text builders ─────────────────────────────────────────────

// Builds the CEL source `x in [0, 1, 2, ..., n-1]`.  Size scales as
// ~8 bytes per element.
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

// Realistic-looking GCP IAM permission strings, padded to exactly
// `target_len` bytes.  Base names are real permissions; the `.v<i>`
// suffix is artificial but plausible (some IAM permissions DO carry a
// version-like suffix, e.g. ".v1beta").  Trailing '0' digits pad to
// the fixed length so every entry has the same strcmp cost.
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
  src.reserve((items.size() * 55) + 16);
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) src.append(", ");
    src.push_back('"');
    src.append(items[i]);
    src.push_back('"');
  }
  src.push_back(']');
  return src;
}

// ══════════════════════════════════════════════════════════════════════
// Scenario 1: int LITERAL list `x in [0..N-1]`.
//
// Capped at 10 000 by the parser's 100 k-codepoint limit
// (~10 k ints = ~98 kB of source; 11 k overflows).
// ══════════════════════════════════════════════════════════════════════

void BM_Compile_In_IntList_Literal(benchmark::State& state) {
  const int n = static_cast<int>(state.range(0));
  const std::string src = MakeIntListInSource(n);
  state.SetLabel(absl::StrCat("source=", src.size(), "B"));
  Compiler c = MakeCompilerWith("x", CelType::Int());
  CompilerOptions opts;
  opts.optimize_level = kBenchOptimizeLevel;
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile(src, opts);
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_In_IntList_Literal)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Unit(benchmark::kMillisecond);

void BM_Plan_In_IntList_Literal(benchmark::State& state) {
  const int n = static_cast<int>(state.range(0));
  Compiler c = MakeCompilerWith("x", CelType::Int());
  Program prog = CompileOrDie(c, MakeIntListInSource(n));
  for ([[maybe_unused]] auto _ : state) {
    auto inst = GlobalEngine().Plan(prog);
    ABSL_CHECK_OK(inst);
    benchmark::DoNotOptimize(inst);
  }
}
BENCHMARK(BM_Plan_In_IntList_Literal)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Unit(benchmark::kMillisecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 2: IAM permission LITERAL list `perm in ["p0",...,"pN-1"]`.
//
// Each permission is exactly 50 bytes; ~55 bytes per list entry
// including the `"…", ` framing.  1000 entries ≈ 55 kB of source,
// well under the 100 k parser cap.
// ══════════════════════════════════════════════════════════════════════

void BM_Compile_In_IamPermissions_Literal(benchmark::State& state) {
  const auto n = static_cast<size_t>(state.range(0));
  const auto perms = MakeIamPermissions(n);
  const std::string src = MakeStringListInSource(perms);
  state.SetLabel(absl::StrCat("source=", src.size(), "B"));
  Compiler c = MakeCompilerWith("perm", CelType::String());
  CompilerOptions opts;
  opts.optimize_level = kBenchOptimizeLevel;
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile(src, opts);
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_In_IamPermissions_Literal)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMillisecond);

void BM_Plan_In_IamPermissions_Literal(benchmark::State& state) {
  const auto n = static_cast<size_t>(state.range(0));
  const auto perms = MakeIamPermissions(n);
  Compiler c = MakeCompilerWith("perm", CelType::String());
  Program prog = CompileOrDie(c, MakeStringListInSource(perms));
  for ([[maybe_unused]] auto _ : state) {
    auto inst = GlobalEngine().Plan(prog);
    ABSL_CHECK_OK(inst);
    benchmark::DoNotOptimize(inst);
  }
}
BENCHMARK(BM_Plan_In_IamPermissions_Literal)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace celwasm

BENCHMARK_MAIN();
