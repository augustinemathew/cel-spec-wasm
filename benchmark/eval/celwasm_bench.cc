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
#include <memory>
#include <optional>
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
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"
#include "testdata/host_fixture_proto3.pb.h"

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

// Force generated-pool registration of the proto fixtures that
// message-typed corpus cells may name.  The cells look their types up
// in the generated DescriptorPool by full name at registration time;
// without an explicit reference the linker can drop the generated
// registration objects (same pattern as `bench/pipeline_bench.cc`).
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::HostMsg3>();
      return 0;
    }();

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

// Map a scalar corpus-literal kind to the matching `CelType`.  Used
// directly for scalar activations and for the element type of a
// bound list.  Non-scalar kinds CHECK — the loader only stamps
// scalar kinds into `list_elem_kind`, and scalar activations never
// carry an aggregate kind.
CelType ScalarTypeForKind(CelValueLiteral::Kind kind) {
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
    case CelValueLiteral::Kind::kMessage:
      break;
  }
  ABSL_CHECK(false) << "ScalarTypeForKind: non-scalar literal kind "
                    << static_cast<int>(kind);
  return CelType::Int();
}

// Map a corpus literal to the `CelType` used at variable-declaration
// time.  `kNull` and `kMap` are not used in any current corpus-cell
// activation, so they CHECK — extending to maps requires a key/value
// type pair and is deferred until a cell needs it.
CelType TypeForLiteral(const CelValueLiteral& lit) {
  switch (lit.kind) {
    case CelValueLiteral::Kind::kInt:
    case CelValueLiteral::Kind::kUint:
    case CelValueLiteral::Kind::kDouble:
    case CelValueLiteral::Kind::kBool:
    case CelValueLiteral::Kind::kString:
    case CelValueLiteral::Kind::kBytes:
      return ScalarTypeForKind(lit.kind);
    case CelValueLiteral::Kind::kList:
      // The loader stamps `list_elem_kind` for every kList literal,
      // so the declared `list<T>` is derivable even for empty lists.
      return CelType::List(ScalarTypeForKind(lit.list_elem_kind));
    case CelValueLiteral::Kind::kMessage:
      return CelType::Message(lit.message_type);
    case CelValueLiteral::Kind::kNull:
    case CelValueLiteral::Kind::kMap:
      break;
  }
  ABSL_CHECK(false) << "TypeForLiteral: unsupported literal kind for "
                       "activation; extend when a cell needs it";
  return CelType::Int();
}

// Owns every proto message parsed for a message-typed activation
// entry.  `Value::Message` is non-owning — the parsed message must
// outlive the Instance/Activation that reference it, and bench
// registrations live for the whole process — so the holder is a
// deliberately-leaked process-lifetime singleton (same idiom as
// `GlobalEngine` above).
std::vector<std::unique_ptr<google::protobuf::Message>>&
LeakedActivationMessages() {
  static auto* holder =
      new std::vector<std::unique_ptr<google::protobuf::Message>>();
  return *holder;
}

// Parse a kMessage literal's textproto into a generated-pool message
// instance and wrap it as a (non-owning) `Value::Message`.  Crashes
// naming `ctx` (the BM name) on an unknown type or malformed
// textproto — the loader validated only that both strings are
// non-empty; validity is this binary's job.
Value MessageValueFromLiteral(const CelValueLiteral& lit,
                              absl::string_view ctx) {
  const google::protobuf::Descriptor* desc =
      google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(
          lit.message_type);
  ABSL_CHECK(desc != nullptr)
      << ctx << ": message type `" << lit.message_type
      << "` not in the generated descriptor pool; link its cc_proto and "
         "add it to kDescriptorsLinked";
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(desc);
  ABSL_CHECK(prototype != nullptr)
      << ctx << ": no generated prototype for `" << lit.message_type << "`";
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(lit.string_value,
                                                           msg.get()))
      << ctx << ": textproto for `" << lit.message_type
      << "` failed to parse: " << lit.string_value;
  Value v = Value::Message(*msg);
  LeakedActivationMessages().push_back(std::move(msg));
  return v;
}

// Convert a corpus literal to an eval-side `Value`.  Mirrors
// `TypeForLiteral` in covered range.  `ctx` names the BM cell for
// crash messages.
Value ValueFromLiteral(const CelValueLiteral& lit, absl::string_view ctx) {
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
    case CelValueLiteral::Kind::kList: {
      std::vector<Value> elems;
      elems.reserve(lit.list_value.size());
      for (const CelValueLiteral& e : lit.list_value) {
        elems.push_back(ValueFromLiteral(e, ctx));
      }
      return Value::List(std::move(elems));
    }
    case CelValueLiteral::Kind::kMessage:
      return MessageValueFromLiteral(lit, ctx);
    case CelValueLiteral::Kind::kMap:
      break;
  }
  ABSL_CHECK(false) << ctx
                    << ": ValueFromLiteral: map activation values not yet "
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
          {"literals", "lit"},
          {"logic", "logic"},
          {"long_strings", "str"},
          {"maps", "map"},
          {"policies", "policy"},
          {"proto", "proto"},
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
    b.DeclareVariable(e.name, TypeForLiteral(e.value));
  }
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  return PlanOrDie(CompileOrDie(*std::move(compiler), source));
}

// Bind every corpus activation entry into a fresh Activation.  `ctx`
// names the BM cell for crash messages.
Activation BuildActivation(const std::vector<ActivationEntry>& act_entries,
                           absl::string_view ctx) {
  Activation act;
  for (const ActivationEntry& e : act_entries) {
    act.Bind(e.name, ValueFromLiteral(e.value, ctx));
  }
  return act;
}

// Single-entry per-cell runtime cache.  Under --benchmark_repetitions
// Google Benchmark re-invokes the bench function once per repetition;
// without this cache every repetition re-pays Compile (Binaryen -O2
// over the whole module — dominant in static link mode, where the
// runtime is linked into every cell's module) + Plan + activation
// build.  Repetitions of one cell run back-to-back, so a single slot
// keyed by BM name gets a (reps-1)/reps hit rate while keeping exactly
// one Instance alive at a time.
struct CellRuntime {
  std::string key;
  Instance inst;
  Activation act;
};

std::optional<CellRuntime>& CachedCellRuntime() {
  static auto* slot = new std::optional<CellRuntime>();
  return *slot;
}

// Returns the cached runtime for `bm_name`, (re)building it on miss.
// Hoisted out of the registration lambda so the lambda body stays a
// two-liner (and the clang-analyzer leak suppression on
// RegisterBenchmark keeps anchoring to the call site).
CellRuntime& EnsureCellRuntime(const std::string& bm_name,
                               const std::string& source,
                               const std::vector<ActivationEntry>& entries) {
  auto& slot = CachedCellRuntime();
  if (!slot.has_value() || slot->key != bm_name) {
    slot.reset();
    Instance inst = PrepareInstance(source, entries);
    Activation act = BuildActivation(entries, bm_name);
    slot.emplace(CellRuntime{bm_name, std::move(inst), std::move(act)});
  }
  return *slot;
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
  benchmark::RegisterBenchmark(bm_name, [bm_name, source = std::move(source),
                                         act_entries = std::move(act_entries)](
                                            benchmark::State& state) {
    CellRuntime& rt = EnsureCellRuntime(bm_name, source, act_entries);
    RunCorpusBench(state, rt.inst, rt.act);
  })->Unit(benchmark::kNanosecond);
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
    "benchmark/eval/corpus/literals.yaml",
    "benchmark/eval/corpus/logic.yaml",
    "benchmark/eval/corpus/long_strings.yaml",
    "benchmark/eval/corpus/maps.yaml",
    "benchmark/eval/corpus/policies.yaml",
    "benchmark/eval/corpus/proto.yaml",
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
