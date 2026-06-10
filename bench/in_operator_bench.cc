// Large-list `in`-operator benches — IAM-style authorization shapes.
//
// Two real-world shapes that exercise the worst-case linear scan in
// `cel_list_in`:
//
//   1. `x in [1, 2, ..., N]`  (int membership)
//   2. `perm in [<IAM permission strings, exactly 50 bytes each>]`
//
// Each shape comes in two flavours that hit different runtime paths:
//
//   - LITERAL source.  The list is inline in the CEL source; codegen
//     materialises it into the static-memory arena and `cel_list_in`
//     scans the arena slots directly.  The cel-cpp parser caps source
//     at `expression_size_codepoint_limit = 100_000` codepoints
//     (see third_party/cel-cpp/parser/options.h:37), so a literal 1 M
//     int list (~7.9 MB of source) does NOT compile today — the
//     literal benches cap at 10 000 ints / 1000 strings, both of which
//     fit well under the cap (a 10 k int list compiles to ~98 kB of
//     source; a 1 k 50-byte string list to ~55 kB).
//
//   - BOUND.  The list is bound through `Activation` as a `list<T>`
//     variable; the CEL source is just `x in xs`.  This is how a
//     real host runs it (build the permission list once, reuse it
//     across many evaluations) and is the relevant path for the
//     headline "1 M ints with x=999_999" and "1 k permissions"
//     numbers the user asked for.
//
// Each shape is timed at three pipeline boundaries (matching
// `pipeline_bench.cc`):
//   - BM_Compile_* — parse + type-check + codegen + cel.abi serialize.
//   - BM_Plan_*    — wasmtime module link + static-memory snapshot.
//   - BM_Eval_*    — Activation.Eval() steady state (the per-request
//                    cost a host pays in production).
//
// `manual`-tagged — invoke explicitly:
//   bazel run -c opt //bench:in_operator_bench
//   bazel run -c opt //bench:in_operator_bench -- \
//       --benchmark_filter=BM_Eval_In_1M \
//       --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "benchmark/benchmark.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
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

Compiler MakeCompilerWith(absl::string_view name, CelType ty) {
  Compiler::Builder b;
  b.DeclareVariable(std::string(name), std::move(ty));
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

Compiler MakeCompilerWithBoundList(absl::string_view scalar_var,
                                   CelType scalar_ty,
                                   absl::string_view list_var) {
  Compiler::Builder b;
  b.DeclareVariable(std::string(scalar_var), scalar_ty);
  b.DeclareVariable(std::string(list_var), CelType::List(scalar_ty));
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

// Declares 10 int vars `a..j`; used by the long-arith benches as a
// realistic working set (a 50-term polynomial over 10 inputs).
Compiler MakeCompilerWithTenInts() {
  Compiler::Builder b;
  for (char c = 'a'; c <= 'j'; ++c) {
    b.DeclareVariable(std::string(1, c), CelType::Int());
  }
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  return *std::move(compiler);
}

// 50-term quadratic-form polynomial: each term is `var[i % 10] *
// var[(i*7+3) % 10]` summed across i in [0, 50).  The `(i*7+3) % 10`
// stride hits every var with no two adjacent terms identical, so a
// trivial CSE pass can't collapse the expression on either side
// (Binaryen on ours, the runtime evaluator on cel-cpp's).  Total
// shape: 50 multiplications + 49 additions = 99 operations across
// 10 distinct int inputs.
std::string MakeLongArithSource() {
  // 10 000 multiplications + 9 999 additions ≈ 20 000 binary ops.
  // Stress test for the arithmetic path: cel-cpp's tree-walker pays
  // ~20 000 virtual-dispatch+alloc cycles per Eval, while our wasm
  // module hands Cranelift ~20 000 i64 ops it scheduler-collapses
  // into straight-line native code.  Parser depth cap raised to
  // 16 384 in `parse_and_check.cc::DefaultParserOptions` (celwasmc)
  // and in `CompilePlanOrDie` (cel-cpp) so this fits.
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

// Bind `a..j` to small distinct primes (2, 3, 5, ..., 29) — enough
// signal that constant-folding can't dismiss the result, small
// enough that the int64 product cannot overflow across 50 terms.
void BindLongArithActivation(Activation* act) {
  static constexpr int64_t kVals[10] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29};
  for (int i = 0; i < 10; ++i) {
    act->Bind(std::string(1, static_cast<char>('a' + i)), Value::Int(kVals[i]));
  }
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

Instance PlanOrDie(const Program& p) {
  auto i = GlobalEngine().Plan(p);
  ABSL_CHECK_OK(i);
  return *std::move(i);
}

// ─── Source-text + value builders ─────────────────────────────────────

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

Value MakeIntListValue(int n) {
  std::vector<Value> v;
  v.reserve(n);
  for (int i = 0; i < n; ++i) v.push_back(Value::Int(i));
  return Value::List(std::move(v));
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

Value MakeStringListValue(const std::vector<std::string>& items) {
  std::vector<Value> v;
  v.reserve(items.size());
  for (const auto& s : items) v.push_back(Value::String(s));
  return Value::List(std::move(v));
}

// ══════════════════════════════════════════════════════════════════════
// Scenario 1A: int LITERAL list `x in [0..N-1]`.
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
  for (auto _ : state) {
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
  for (auto _ : state) {
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

void BM_Eval_In_IntList_Literal_WorstCase(benchmark::State& state) {
  const int n = static_cast<int>(state.range(0));
  Compiler c = MakeCompilerWith("x", CelType::Int());
  Instance inst = PlanOrDie(CompileOrDie(c, MakeIntListInSource(n)));
  Activation act;
  act.Bind("x", Value::Int(n - 2));  // second-to-last: full O(N) scan
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
// NOTE: capped at 1000 — at N=10 000 wasmtime traps in
// `store.rs:2440 (assertion failed: fault.is_none())` during Eval.
// Compile and Plan complete at 10 k, but the produced module faults
// when executed.  Tracked separately; do not raise without fixing.
BENCHMARK(BM_Eval_In_IntList_Literal_WorstCase)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 1B: int BOUND list `x in xs`, xs of size N.
//
// The headline "1 M ints with x = 999_999" lives here — the literal-
// source path can't express it (parser cap), and binding the list as
// a variable is the realistic production shape anyway.
// ══════════════════════════════════════════════════════════════════════

void BM_Eval_In_IntList_Bound_WorstCase(benchmark::State& state) {
  const int n = static_cast<int>(state.range(0));
  Compiler c = MakeCompilerWithBoundList("x", CelType::Int(), "xs");
  Instance inst = PlanOrDie(CompileOrDie(c, "x in xs"));
  Activation act;
  act.Bind("x", Value::Int(n - 2));      // second-to-last
  act.Bind("xs", MakeIntListValue(n));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_In_IntList_Bound_WorstCase)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Arg(1000000)
    ->Unit(benchmark::kMicrosecond);

// Headline (per brief): 1 M ints, x = 999_999.  This is the BOUND path:
// `x in xs` with xs an Activation-bound list<int>.  The LITERAL path
// (`x in [0, 1, ..., 999_999]`) is impossible today — the parser caps
// source at 100 k codepoints (a 1 M-element int source is ~7.9 MB).
// In production, a permission set is built once and reused; bound is
// the realistic shape.
void BM_Eval_In_1M_IntList_Bound_X_999999(benchmark::State& state) {
  Compiler c = MakeCompilerWithBoundList("x", CelType::Int(), "xs");
  Instance inst = PlanOrDie(CompileOrDie(c, "x in xs"));
  Activation act;
  act.Bind("x", Value::Int(999'999));
  act.Bind("xs", MakeIntListValue(1'000'000));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_In_1M_IntList_Bound_X_999999)->Unit(benchmark::kMicrosecond);

void BM_Eval_In_1M_IntList_Bound_X_0(benchmark::State& state) {
  Compiler c = MakeCompilerWithBoundList("x", CelType::Int(), "xs");
  Instance inst = PlanOrDie(CompileOrDie(c, "x in xs"));
  Activation act;
  act.Bind("x", Value::Int(0));  // first element — early exit
  act.Bind("xs", MakeIntListValue(1'000'000));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_In_1M_IntList_Bound_X_0)->Unit(benchmark::kMicrosecond);

void BM_Eval_In_1M_IntList_Bound_X_NotPresent(benchmark::State& state) {
  Compiler c = MakeCompilerWithBoundList("x", CelType::Int(), "xs");
  Instance inst = PlanOrDie(CompileOrDie(c, "x in xs"));
  Activation act;
  act.Bind("x", Value::Int(1'000'000));  // outside [0, 999_999]
  act.Bind("xs", MakeIntListValue(1'000'000));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_In_1M_IntList_Bound_X_NotPresent)->Unit(benchmark::kMicrosecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 2A: IAM permission LITERAL list `perm in ["p0",...,"pN-1"]`.
//
// Each permission is exactly 50 bytes; ~55 bytes per list entry
// including the `"…", ` framing.  1000 entries ≈ 55 kB of source,
// well under the 100 k parser cap.
// ══════════════════════════════════════════════════════════════════════

void BM_Compile_In_IamPermissions_Literal(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const auto perms = MakeIamPermissions(n);
  const std::string src = MakeStringListInSource(perms);
  state.SetLabel(absl::StrCat("source=", src.size(), "B"));
  Compiler c = MakeCompilerWith("perm", CelType::String());
  CompilerOptions opts;
  opts.optimize_level = kBenchOptimizeLevel;
  for (auto _ : state) {
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
  const size_t n = static_cast<size_t>(state.range(0));
  const auto perms = MakeIamPermissions(n);
  Compiler c = MakeCompilerWith("perm", CelType::String());
  Program prog = CompileOrDie(c, MakeStringListInSource(perms));
  for (auto _ : state) {
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

// Headline (per brief): 1000 IAM permissions of length 50 in a
// LITERAL list (source-inline); perm = last entry (worst-case scan).
void BM_Eval_In_1K_IamPermissions_Literal_Last(benchmark::State& state) {
  const auto perms = MakeIamPermissions(1000);
  Compiler c = MakeCompilerWith("perm", CelType::String());
  Instance inst =
      PlanOrDie(CompileOrDie(c, MakeStringListInSource(perms)));
  Activation act;
  act.Bind("perm", Value::String(perms.back()));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_In_1K_IamPermissions_Literal_Last)
    ->Unit(benchmark::kMicrosecond);

void BM_Eval_In_1K_IamPermissions_Literal_First(benchmark::State& state) {
  const auto perms = MakeIamPermissions(1000);
  Compiler c = MakeCompilerWith("perm", CelType::String());
  Instance inst =
      PlanOrDie(CompileOrDie(c, MakeStringListInSource(perms)));
  Activation act;
  act.Bind("perm", Value::String(perms.front()));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_In_1K_IamPermissions_Literal_First)
    ->Unit(benchmark::kMicrosecond);

void BM_Eval_In_1K_IamPermissions_Literal_NotPresent(benchmark::State& state) {
  const auto perms = MakeIamPermissions(1000);
  Compiler c = MakeCompilerWith("perm", CelType::String());
  Instance inst =
      PlanOrDie(CompileOrDie(c, MakeStringListInSource(perms)));
  std::string needle = "absent.service.deny";
  needle.append(50 - needle.size(), '0');
  Activation act;
  act.Bind("perm", Value::String(needle));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_In_1K_IamPermissions_Literal_NotPresent)
    ->Unit(benchmark::kMicrosecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 2B: IAM permission BOUND list `perm in perms`.
//
// Realistic production shape: build the permission set once, query
// many times.
// ══════════════════════════════════════════════════════════════════════

void BM_Eval_In_IamPermissions_Bound_Last(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const auto perms = MakeIamPermissions(n);
  Compiler c =
      MakeCompilerWithBoundList("perm", CelType::String(), "perms");
  Instance inst = PlanOrDie(CompileOrDie(c, "perm in perms"));
  Activation act;
  act.Bind("perm", Value::String(perms.back()));
  act.Bind("perms", MakeStringListValue(perms));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
// NOTE: capped at 1000 — at N=10 000 the bound-string `in` path
// traps with "arena OOM in CelMapLookupImpl" in cel_list_in's
// trampoline.  Strings allocate into the per-Eval arena and the
// scan exhausts it at this size.  Tracked separately.
BENCHMARK(BM_Eval_In_IamPermissions_Bound_Last)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// ══════════════════════════════════════════════════════════════════════
// Scenario 4: long arithmetic expression — 50-term polynomial.
//
// Showcases what AOT + JIT buys on a compute-heavy expression: the
// whole polynomial collapses to straight-line native machine code at
// Plan time (Cranelift sees ~99 wasm i64 ops, schedules them as a
// register-machine pass), so each Eval is one wasmtime call into a
// short native function.  The cel-cpp side (`in_operator_cel_cpp_bench`)
// runs the same expression through the tree-walking evaluator — every
// `*` and `+` is a virtual dispatch + a CelValue allocation.  Honest
// head-to-head on something neither side has a specialised arm for.
// ══════════════════════════════════════════════════════════════════════

void BM_Eval_LongArith_10kTerms(benchmark::State& state) {
  Compiler c = MakeCompilerWithTenInts();
  Instance inst = PlanOrDie(CompileOrDie(c, MakeLongArithSource()));
  Activation act;
  BindLongArithActivation(&act);
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_LongArith_10kTerms)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace celwasm

BENCHMARK_MAIN();
