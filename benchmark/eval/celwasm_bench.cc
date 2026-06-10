// Length-sweep + canonical-pattern comparison bench (celwasm side).
//
// Cells live in YAML under `benchmark/eval/corpus/*.yaml` — this main
// loads the corpus at startup and registers one Google Benchmark per
// cell.  See `benchmark/DESIGN.md` §6 + `benchmark/eval/corpus/
// OPERATORS.md` for the coverage contract.
//
// Cell → BM name: `BM_<bm_prefix(surface)>_<id>`, where the
// (surface → bm_prefix) table is local to this TU (see
// `BmPrefixForSurface` below).  e.g. surface=arithmetic id=intAdd2 →
// `BM_arith_intAdd2`; surface=lists id=5 → `BM_in_list_5`;
// surface=long_strings id=eqLong_N100_match → `BM_str_eqLong_N100_match`.
//
// Compile + Plan happen ONCE outside the timed loop; only Eval runs
// inside the timed region.
//
// Matched cell-for-cell with `celcpp_bench.cc`: the SAME YAML corpus
// drives both binaries, so any cell tagged for the cross-comparator
// matrix runs on both with the same BM name.
//
// Run via `benchmark/eval/run.sh`, or directly:
//   bazel run -c opt //benchmark/eval:celwasm_bench

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "benchmark/eval/corpus_loader.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

namespace celwasm {

// Link mode used for every corpus-cell Compile in this binary.
// Settable from `main` via `--link_mode=dynamic|static` (stripped from
// argv before Google Benchmark sees it).  Defaults to kDynamic so the
// emitted numbers stay comparable with the historical baselines in
// `doc/implementation-plan/rewrite/m28-wrapper-overhead-findings.md`
// regardless of what `CompilerOptions`' own default is.  Function-local
// static (vs a global) so there's a named hook `main` can reach from
// outside the unnamed namespace below.
static CompilerOptions::LinkMode& BenchLinkMode() {
  static CompilerOptions::LinkMode mode = CompilerOptions::LinkMode::kDynamic;
  return mode;
}

namespace {

using ::celbench::ActivationEntry;
using ::celbench::Cell;
using ::celbench::CelValueLiteral;
using ::celbench::LoadCorpus;

// Single engine across all benches; engine construction is amortised
// in real use and shouldn't be double-charged here.
//
// When `CELWASM_BENCH_PERFMAP=1` is set in the environment, wasmtime
// writes a `/tmp/perf-<pid>.map` symbol file describing the JIT'd
// runtime + expr wasm.  Lets `samply` (or Linux `perf`) symbolicate
// the otherwise-anonymous JIT addresses when profiling the bench.
Engine& GlobalEngine() {
  static Engine* engine = [] {
    const char* p = std::getenv("CELWASM_BENCH_PERFMAP");
    const bool enable_perfmap = (p != nullptr && p[0] == '1' && p[1] == '\0');
    auto e = Engine::NewBuilder().EnableJitPerfMap(enable_perfmap).Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

// Benches measure production-shape numbers — Binaryen at its highest
// wired-up optimization level (see CLAUDE.md "Benchmark configuration").
constexpr int kBenchOptimizeLevel = 2;

Program CompileOrDie(const Compiler& c, absl::string_view src) {
  CompilerOptions opts;
  opts.optimize_level = kBenchOptimizeLevel;
  opts.link_mode = BenchLinkMode();
  auto p = c.Compile(src, opts);
  ABSL_CHECK_OK(p) << "compile failed (size=" << src.size() << "): " << src;
  return *std::move(p);
}

Instance PlanOrDie(const Program& p) {
  auto i = GlobalEngine().Plan(p);
  ABSL_CHECK_OK(i);
  return *std::move(i);
}

// Map a corpus literal's `kind` to the matching `CelType` used at
// variable-declaration time.  Aggregates (`kList`, `kMap`) and `kNull`
// are not used in any current corpus-cell activation, so they CHECK —
// extending to those types requires deciding on a concrete element
// type for lists (CelType::List takes an element type) and is deferred
// until a cell needs it.
CelType TypeForKind(CelValueLiteral::Kind kind) {
  switch (kind) {
    case CelValueLiteral::Kind::kInt:
      return CelType::Int();
    case CelValueLiteral::Kind::kUint:
      return CelType::Uint();
    case CelValueLiteral::Kind::kDouble:
      return CelType::Double();
    case CelValueLiteral::Kind::kBool:
      return CelType::Bool();
    case CelValueLiteral::Kind::kString:
      return CelType::String();
    case CelValueLiteral::Kind::kBytes:
      return CelType::Bytes();
    case CelValueLiteral::Kind::kNull:
    case CelValueLiteral::Kind::kList:
    case CelValueLiteral::Kind::kMap:
      break;
  }
  ABSL_CHECK(false) << "TypeForKind: unsupported literal kind for activation; "
                       "extend when a cell needs it";
  return CelType::Int();
}

// Convert a corpus literal to an eval-side `Value`.  Mirrors
// `TypeForKind` in covered range.
Value ValueFromLiteral(const CelValueLiteral& lit) {
  switch (lit.kind) {
    case CelValueLiteral::Kind::kInt:
      return Value::Int(lit.int_value);
    case CelValueLiteral::Kind::kUint:
      return Value::Uint(lit.uint_value);
    case CelValueLiteral::Kind::kDouble:
      return Value::Double(lit.double_value);
    case CelValueLiteral::Kind::kBool:
      return Value::Bool(lit.bool_value);
    case CelValueLiteral::Kind::kString:
      return Value::String(lit.string_value);
    case CelValueLiteral::Kind::kBytes:
      return Value::Bytes(lit.string_value);
    case CelValueLiteral::Kind::kNull:
      return Value::Null();
    case CelValueLiteral::Kind::kList:
    case CelValueLiteral::Kind::kMap:
      break;
  }
  ABSL_CHECK(false) << "ValueFromLiteral: list/map activation values not yet "
                       "supported; extend when a cell needs them";
  return Value::Null();
}

// Stamp the produced value as a Label so the runner can mechanically
// diff celwasm's number against the cel-cpp side.  Kept format-
// identical with `celcpp_bench.cc::SetResultLabel`; string/bytes
// payloads go through `AbbreviateForLabel` so long results stay
// diffable without megabyte labels.
void SetResultLabel(benchmark::State& state, const Value& v) {
  if (auto iv = v.AsInt(); iv.ok()) {
    state.SetLabel(absl::StrCat("result=", *iv, " (int)"));
  } else if (auto uv = v.AsUint(); uv.ok()) {
    state.SetLabel(absl::StrCat("result=", *uv, " (uint)"));
  } else if (auto dv = v.AsDouble(); dv.ok()) {
    state.SetLabel(absl::StrCat("result=", *dv, " (double)"));
  } else if (auto bv = v.AsBool(); bv.ok()) {
    state.SetLabel(absl::StrCat("result=", *bv ? "true" : "false", " (bool)"));
  } else if (auto sv = v.AsString(); sv.ok()) {
    state.SetLabel(absl::StrCat("result=\"", celbench::AbbreviateForLabel(*sv),
                                "\" (string)"));
  } else if (auto bytes = v.AsBytes(); bytes.ok()) {
    state.SetLabel(absl::StrCat(
        "result=b\"", celbench::AbbreviateForLabel(*bytes), "\" (bytes)"));
  } else {
    state.SetLabel("result=<non-numeric>");
  }
}

// Surface → BM prefix.  Keeps the BM names emitted by the YAML-driven
// path byte-identical to the hand-coded forms the prior bench
// registered (`BM_arith_*`, `BM_str_*`, `BM_in_list_*`, `BM_cmp_*`).
absl::string_view BmPrefixForSurface(absl::string_view surface) {
  static constexpr std::pair<absl::string_view, absl::string_view>
      kSurfacePrefixes[] = {
          {"arithmetic", "arith"},
          {"comparisons", "cmp"},
          {"comprehensions", "compr"},
          {"conversions", "conv"},
          {"index", "idx"},
          {"lists", "in_list"},
          {"logic", "logic"},
          {"long_strings", "str"},
          {"maps", "map"},
          {"size", "size"},
          {"strings", "str"},
          {"ternary", "ternary"},
          {"time", "time"},
      };
  for (const auto& [name, prefix] : kSurfacePrefixes) {
    if (surface == name) return prefix;
  }
  ABSL_CHECK(false) << "BmPrefixForSurface: no prefix wired for surface '"
                    << surface << "'; add one when a new corpus file lands";
  return "";
}

// True iff any of the cell's tags starts with `prefix`.  Used for the
// `celwasm-skip-<reason>` tag family: every such tag means "this cell
// cannot run through celwasm today for <reason>; celcpp_bench still
// runs it" (rodata cap, heterogeneous-equality checker gap, the
// map-dot-field Select gap, …).  The per-cell reason lives in the
// corpus YAML next to the cell.
bool HasTagWithPrefix(const Cell& cell, absl::string_view prefix) {
  return std::any_of(cell.tags.begin(), cell.tags.end(),
                     [prefix](const std::string& t) {
                       return absl::StartsWith(t, prefix);
                     });
}

// Build the typed Compiler + planned Instance for a cell's source +
// activation shape.  Pulled out of the bench-registration lambda so
// the inner function stays under the function-size lint cap.
Instance PrepareInstance(absl::string_view source,
                         const std::vector<ActivationEntry>& act_entries) {
  Compiler::Builder b;
  for (const ActivationEntry& e : act_entries) {
    b.DeclareVariable(e.name, TypeForKind(e.value.kind));
  }
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  return PlanOrDie(CompileOrDie(*std::move(compiler), source));
}

// Bind every corpus activation entry into a fresh Activation.
Activation BuildActivation(const std::vector<ActivationEntry>& act_entries) {
  Activation act;
  for (const ActivationEntry& e : act_entries) {
    act.Bind(e.name, ValueFromLiteral(e.value));
  }
  return act;
}

// The actual bench-loop body — separated so RegisterCorpusCell stays
// under the function-size cap.  Eval runs both once outside the loop
// (for the result-label parity stamp) and again inside the timed
// loop; production-shape Eval is the only thing measured here.
void RunCorpusBench(benchmark::State& state, Instance& inst,
                    const Activation& act) {
  {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    SetResultLabel(state, *v);
  }
  for ([[maybe_unused]] auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}

// Register one corpus cell as a Google Benchmark on this binary.
// Compiler + Instance are built once, captured into the bench lambda;
// only Eval runs in the timed region.
void RegisterCorpusCell(const Cell& cell) {
  // Cells tagged celwasm-skip-<reason> cannot run through celwasm
  // today (rodata cap, checker gaps, known eval bugs — the reason is
  // documented next to the tag in the corpus YAML).  Skip here —
  // celcpp_bench still runs them, and OPERATORS.md records each
  // skipped cell.
  if (HasTagWithPrefix(cell, "celwasm-skip")) return;

  std::string bm_name =
      absl::StrCat("BM_", BmPrefixForSurface(cell.surface), "_", cell.id);

  // Take an owning copy of the source + activation so the lambda
  // outlives the loop-local Cell reference.
  std::string source = cell.source;
  std::vector<ActivationEntry> act_entries = cell.activation;

  // Google Benchmark's RegisterBenchmark allocates the cell heap-side
  // via `new`; the framework owns the lifetime, but clang-analyzer
  // sees the unowned pointer as a leak.  False positive — suppressed
  // at the call site in every TU that registers cells.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  benchmark::RegisterBenchmark(bm_name, [source = std::move(source),
                                         act_entries = std::move(act_entries)](
                                            benchmark::State& state) {
    Instance inst = PrepareInstance(source, act_entries);
    Activation act = BuildActivation(act_entries);
    RunCorpusBench(state, inst, act);
  })->Unit(benchmark::kNanosecond);
}

// Register the abc-abc 6-term `a + b + c + a + b + c` cell with `a..c`
// bound in activation (variable cell — the companion to the literal
// cell below; the pair isolates activation-marshaling cost on an
// identical call-graph shape).
void RegisterAbcAbcVarsToday() {
  // Google Benchmark's RegisterBenchmark allocates the cell heap-side
  // via `new`; the framework owns the lifetime, but clang-analyzer
  // sees the unowned pointer as a leak.  False positive — suppressed
  // at the call site in every TU that registers cells.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  benchmark::RegisterBenchmark("BM_arith_intAdd_AbcAbcShape_VarsToday",
                               [](benchmark::State& state) {
                                 Compiler::Builder b;
                                 b.DeclareVariable("a", CelType::Int());
                                 b.DeclareVariable("b", CelType::Int());
                                 b.DeclareVariable("c", CelType::Int());
                                 auto c = std::move(b).Build();
                                 ABSL_CHECK_OK(c);
                                 Instance inst = PlanOrDie(CompileOrDie(
                                     *std::move(c), "a + b + c + a + b + c"));
                                 Activation act;
                                 act.Bind("a", Value::Int(1));
                                 act.Bind("b", Value::Int(2));
                                 act.Bind("c", Value::Int(3));
                                 RunCorpusBench(state, inst, act);
                               })
      ->Unit(benchmark::kNanosecond);
}

// `1 + 2 + 3 + 1 + 2 + 3` — same call-graph shape as the var cell
// above, with literals.
void RegisterAbcAbcLitToday() {
  // Google Benchmark's RegisterBenchmark allocates the cell heap-side
  // via `new`; the framework owns the lifetime, but clang-analyzer
  // sees the unowned pointer as a leak.  False positive — suppressed
  // at the call site in every TU that registers cells.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  benchmark::RegisterBenchmark("BM_arith_intAdd_AbcAbcShape_LitToday",
                               [](benchmark::State& state) {
                                 Compiler::Builder b;
                                 auto c = std::move(b).Build();
                                 ABSL_CHECK_OK(c);
                                 Instance inst = PlanOrDie(CompileOrDie(
                                     *std::move(c), "1 + 2 + 3 + 1 + 2 + 3"));
                                 Activation act;
                                 RunCorpusBench(state, inst, act);
                               })
      ->Unit(benchmark::kNanosecond);
}

// Cell-bearing YAML files under benchmark/eval/corpus/.  The bench
// main is the single point that names them — adding a new YAML means
// (a) drop the file under corpus/, (b) add it here, (c) add the
// surface name to `BmPrefixForSurface`.
constexpr const char* kCorpusFiles[] = {
    "benchmark/eval/corpus/arithmetic.yaml",
    "benchmark/eval/corpus/comparisons.yaml",
    "benchmark/eval/corpus/comprehensions.yaml",
    "benchmark/eval/corpus/conversions.yaml",
    "benchmark/eval/corpus/index.yaml",
    "benchmark/eval/corpus/lists.yaml",
    "benchmark/eval/corpus/logic.yaml",
    "benchmark/eval/corpus/long_strings.yaml",
    "benchmark/eval/corpus/maps.yaml",
    "benchmark/eval/corpus/size.yaml",
    "benchmark/eval/corpus/strings.yaml",
    "benchmark/eval/corpus/ternary.yaml",
    "benchmark/eval/corpus/time.yaml",
};

void RegisterAll() {
  std::vector<std::string> paths;
  paths.reserve(sizeof(kCorpusFiles) / sizeof(kCorpusFiles[0]));
  for (const char* p : kCorpusFiles) {
    paths.emplace_back(p);
  }

  auto cells = LoadCorpus(paths);
  ABSL_CHECK_OK(cells) << "LoadCorpus failed (running from "
                       << std::getenv("PWD") << ")";

  for (const Cell& cell : *cells) {
    RegisterCorpusCell(cell);
  }

  // Hand-coded cells the YAML loader can't represent yet (the vars/lit
  // pair needs a way to express "same shape, two activation forms"
  // adjacently).
  RegisterAbcAbcVarsToday();
  RegisterAbcAbcLitToday();
}

}  // namespace
}  // namespace celwasm

// Google Benchmark's APIs (RegisterBenchmark, Initialize,
// RunSpecifiedBenchmarks) are declared without `noexcept` and could
// in theory throw; in practice they only throw on misuse the bench
// would crash on anyway.  An uncaught exception in main aborts via
// the default terminate handler, which is the right behaviour here —
// we don't want to mask a malformed configuration with a cryptic
// recovery path.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
  // `--link_mode=dynamic|static` is ours, not Google Benchmark's —
  // consume it (compacting argv) before benchmark::Initialize, which
  // treats unrecognized flags as a hard error.  Same dual-mode story
  // as `conformance/runner.cc`: one binary, the harness (`run.sh`)
  // invokes it once per mode and joins the reports.
  int kept = 1;
  for (int i = 1; i < argc; ++i) {
    const absl::string_view arg = argv[i];
    if (arg == "--link_mode=dynamic") {
      celwasm::BenchLinkMode() = celwasm::CompilerOptions::LinkMode::kDynamic;
    } else if (arg == "--link_mode=static") {
      celwasm::BenchLinkMode() = celwasm::CompilerOptions::LinkMode::kStatic;
    } else if (arg.rfind("--link_mode", 0) == 0) {
      std::fprintf(stderr,
                   "unrecognized --link_mode value in '%s' "
                   "(expected --link_mode=dynamic or --link_mode=static)\n",
                   argv[i]);
      return 2;
    } else {
      argv[kept++] = argv[i];
    }
  }
  argc = kept;
  celwasm::RegisterAll();
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
