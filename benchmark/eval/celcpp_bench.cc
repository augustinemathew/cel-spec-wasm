// Length-sweep + canonical-pattern comparison bench (cel-cpp side).
//
// Cells live in YAML under `benchmark/eval/corpus/*.yaml` — this main
// loads the corpus at startup and registers one Google Benchmark per
// cell.  See `benchmark/DESIGN.md` §6 + `benchmark/eval/corpus/
// OPERATORS.md` for the coverage contract.
//
// Cell → BM name: `BM_<bm_prefix(surface)>_<id>`, with the same
// (surface → bm_prefix) table as `celwasm_bench.cc`, so the two
// binaries register the same BM names for the same cell ids and
// `report.sh` can join apples-to-apples.
//
// LINKING CONSTRAINT: this TU links cel-cpp's `cel::Value` /
// `cel::Activation` directly.  Those symbols clash with celwasm's
// `cel::Value` aliases under archive-scan-order-sensitive conditions
// (see `bench/in_operator_cel_cpp_bench.cc` for the original notes).
// This bench is therefore **standalone** — it does NOT depend on any
// `//eval/...`, `//compiler:...`, `//shared:...`, or `//abi/...`
// target.  Only cel-cpp's runtime + compiler + the corpus loader
// (which is comparator-neutral) + the benchmark framework.
//
// Parser depth cap: the 250- and 1000-term chains nest binary ops past
// cel-cpp's default `parser_options.max_recursion_depth = 32`.  We
// raise it to 16384 (matching `bench/in_operator_cel_cpp_bench.cc`)
// inside `CompilePlanOrDie`.
//
// Run via `benchmark/eval/run.sh`, or directly:
//   bazel run -c opt //benchmark/eval:celcpp_bench

#include <cstdint>
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
#include "common/decl.h"
#include "common/type.h"
#include "common/value.h"
#include "compiler/compiler.h"
#include "compiler/compiler_factory.h"
#include "compiler/standard_library.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "runtime/activation.h"
#include "runtime/constant_folding.h"
#include "runtime/runtime.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"
#include "testdata/e2e_fixture.pb.h"
#include "testdata/host_fixture_proto3.pb.h"

namespace celwasm_bench_celcpp_proto {
namespace {

using ::celbench::ActivationEntry;
using ::celbench::Cell;
using ::celbench::CelValueLiteral;
using ::celbench::LoadCorpus;

// Force generated-pool registration of the proto fixtures that
// message-typed corpus cells may name.  Mirrors
// `celwasm_bench.cc::kDescriptorsLinked` — both binaries resolve
// `message_type` in the generated DescriptorPool by full name, and
// without an explicit reference the linker can drop the generated
// registration objects.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::HostMsg3>();
      return 0;
    }();

std::unique_ptr<const cel::Runtime> BuildRuntimeOrDie() {
  const google::protobuf::DescriptorPool* pool =
      google::protobuf::DescriptorPool::generated_pool();
  cel::RuntimeOptions opts;
  opts.enable_qualified_type_identifiers = true;
  opts.enable_heterogeneous_equality = true;
  auto builder = cel::CreateStandardRuntimeBuilder(pool, opts);
  ABSL_CHECK_OK(builder);
  // Fairness toggle: CELCPP_FOLD=1 enables cel-cpp's plan-time constant
  // folding, so constant subexpressions (a whole `size([…])`, or the
  // `[…]` operand of `x in […]`) are precomputed once instead of rebuilt
  // every Eval — the cel-cpp analogue of our compile-time materialization.
  if (std::getenv("CELCPP_FOLD") != nullptr) {
    ABSL_CHECK_OK(cel::extensions::EnableConstantFolding(*builder));
  }
  auto runtime = std::move(*builder).Build();
  ABSL_CHECK_OK(runtime);
  return std::move(*runtime);
}

const cel::Runtime& GlobalRuntime() {
  static const cel::Runtime* r = BuildRuntimeOrDie().release();
  return *r;
}

struct TypedVar {
  std::string name;
  cel::Type type;
};

std::unique_ptr<cel::Compiler> BuildCompilerOrDie(
    const std::vector<TypedVar>& vars) {
  const google::protobuf::DescriptorPool* pool =
      google::protobuf::DescriptorPool::generated_pool();
  cel::CompilerOptions copts;
  // 250- and 1000-term linear chains exceed cel-cpp's default parser
  // recursion depth of 32; raise to match `bench/in_operator_cel_cpp_bench.cc`.
  copts.parser_options.max_recursion_depth = 16384;
  // The corpus's cross-numeric cells (`int == double`, `int < double`)
  // are spec-admitted but gated behind this checker option in cel-cpp;
  // the runtime side already enables heterogeneous equality (see
  // BuildRuntimeOrDie).  celwasm's checker rejects these shapes today,
  // so the cells run celcpp-only (tagged `celwasm-skip-het-eq`).
  copts.checker_options.enable_cross_numeric_comparisons = true;
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
  return std::move(*compiler);
}

std::unique_ptr<cel::Program> CompilePlanOrDie(
    absl::string_view source, const std::vector<TypedVar>& vars) {
  auto compiler = BuildCompilerOrDie(vars);
  auto compiled = compiler->Compile(source);
  ABSL_CHECK_OK(compiled);
  ABSL_CHECK(compiled->IsValid()) << "compile/check failed: " << source;
  auto ast = compiled->ReleaseAst();
  ABSL_CHECK_OK(ast);
  auto program = GlobalRuntime().CreateProgram(*std::move(ast));
  ABSL_CHECK_OK(program);
  return std::move(*program);
}

// One pre-loop validation: print result via SetLabel so cross-checking
// against the celwasm bench output is mechanical.  Kept format-
// identical with `celwasm_bench.cc::SetResultLabel`; string/bytes
// payloads go through `AbbreviateForLabel` so long results stay
// diffable without megabyte labels.
void SetResultLabel(benchmark::State& state, const cel::Value& v) {
  if (auto iv = v.AsInt(); iv.has_value()) {
    state.SetLabel(absl::StrCat("result=", iv->NativeValue(), " (int)"));
  } else if (auto uv = v.AsUint(); uv.has_value()) {
    state.SetLabel(absl::StrCat("result=", uv->NativeValue(), " (uint)"));
  } else if (auto dv = v.AsDouble(); dv.has_value()) {
    state.SetLabel(absl::StrCat("result=", dv->NativeValue(), " (double)"));
  } else if (auto bv = v.AsBool(); bv.has_value()) {
    state.SetLabel(absl::StrCat("result=", bv->NativeValue() ? "true" : "false",
                                " (bool)"));
  } else if (auto sv = v.AsString(); sv.has_value()) {
    state.SetLabel(absl::StrCat("result=\"",
                                celbench::AbbreviateForLabel(sv->ToString()),
                                "\" (string)"));
  } else if (auto bytes = v.AsBytes(); bytes.has_value()) {
    state.SetLabel(absl::StrCat("result=b\"",
                                celbench::AbbreviateForLabel(bytes->ToString()),
                                "\" (bytes)"));
  } else {
    state.SetLabel("result=<non-numeric>");
  }
}

// Map a scalar corpus-literal kind to cel-cpp's `cel::Type`.  Used
// directly for scalar activations and for the element type of a
// bound list.  Non-scalar kinds CHECK — the loader only stamps
// scalar kinds into `list_elem_kind`.
cel::Type ScalarTypeForKind(CelValueLiteral::Kind kind) {
  switch (kind) {
    case CelValueLiteral::Kind::kInt:
      return cel::IntType();
    case CelValueLiteral::Kind::kUint:
      return cel::UintType();
    case CelValueLiteral::Kind::kDouble:
      return cel::DoubleType();
    case CelValueLiteral::Kind::kBool:
      return cel::BoolType();
    case CelValueLiteral::Kind::kString:
      return cel::StringType();
    case CelValueLiteral::Kind::kBytes:
      return cel::BytesType();
    case CelValueLiteral::Kind::kNull:
    case CelValueLiteral::Kind::kList:
    case CelValueLiteral::Kind::kMap:
    case CelValueLiteral::Kind::kMessage:
      break;
  }
  ABSL_CHECK(false) << "ScalarTypeForKind: non-scalar literal kind "
                    << static_cast<int>(kind);
  return cel::IntType();
}

// Look up a corpus `message_type` in the generated descriptor pool.
// Crashes naming `ctx` (the BM name) when the type isn't linked in.
const google::protobuf::Descriptor* FindMessageDescriptorOrDie(
    absl::string_view message_type, absl::string_view ctx) {
  const google::protobuf::Descriptor* desc =
      google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(
          message_type);
  ABSL_CHECK(desc != nullptr)
      << ctx << ": message type `" << message_type
      << "` not in the generated descriptor pool; link its cc_proto and "
         "add it to kDescriptorsLinked";
  return desc;
}

// Map a corpus literal to the `cel::Type` used at variable-declaration
// time.  `kNull` and `kMap` are not used by any current corpus-cell
// activation; extending to maps requires a key/value type pair.
cel::Type TypeForLiteral(const CelValueLiteral& lit,
                         google::protobuf::Arena* arena,
                         absl::string_view ctx) {
  switch (lit.kind) {
    case CelValueLiteral::Kind::kInt:
    case CelValueLiteral::Kind::kUint:
    case CelValueLiteral::Kind::kDouble:
    case CelValueLiteral::Kind::kBool:
    case CelValueLiteral::Kind::kString:
    case CelValueLiteral::Kind::kBytes:
      return ScalarTypeForKind(lit.kind);
    case CelValueLiteral::Kind::kList:
      return cel::ListType(arena, ScalarTypeForKind(lit.list_elem_kind));
    case CelValueLiteral::Kind::kMessage:
      // cel::MessageType(descriptor) — common/types/message_type.h.
      return cel::MessageType(
          FindMessageDescriptorOrDie(lit.message_type, ctx));
    case CelValueLiteral::Kind::kNull:
    case CelValueLiteral::Kind::kMap:
      break;
  }
  ABSL_CHECK(false) << ctx
                    << ": TypeForLiteral: unsupported literal kind for "
                       "activation; extend when a cell needs it";
  return cel::IntType();
}

cel::Value ValueFromLiteral(const CelValueLiteral& lit,
                            google::protobuf::Arena* arena,
                            absl::string_view ctx);

// Build a `cel::ListValue` from a kList literal's (scalar) elements.
// Lives in `arena`, which the bench lambda keeps alive across the
// whole timed region.
cel::Value ListValueFromLiteral(const CelValueLiteral& lit,
                                google::protobuf::Arena* arena,
                                absl::string_view ctx) {
  auto builder = cel::NewListValueBuilder(arena);
  builder->Reserve(lit.list_value.size());
  for (const CelValueLiteral& e : lit.list_value) {
    builder->UnsafeAdd(ValueFromLiteral(e, arena, ctx));
  }
  return {std::move(*builder).Build()};
}

// Parse a kMessage literal's textproto into an arena-allocated
// generated-pool message and wrap it via `cel::Value::WrapMessage`
// (common/value.h) — borrowed, arena-owned, alive for the bench.
cel::Value MessageValueFromLiteral(const CelValueLiteral& lit,
                                   google::protobuf::Arena* arena,
                                   absl::string_view ctx) {
  const google::protobuf::Descriptor* desc =
      FindMessageDescriptorOrDie(lit.message_type, ctx);
  google::protobuf::MessageFactory* factory =
      google::protobuf::MessageFactory::generated_factory();
  const google::protobuf::Message* prototype = factory->GetPrototype(desc);
  ABSL_CHECK(prototype != nullptr)
      << ctx << ": no generated prototype for `" << lit.message_type << "`";
  google::protobuf::Message* msg = prototype->New(arena);
  ABSL_CHECK(
      google::protobuf::TextFormat::ParseFromString(lit.string_value, msg))
      << ctx << ": textproto for `" << lit.message_type
      << "` failed to parse: " << lit.string_value;
  return cel::Value::WrapMessage(
      msg, google::protobuf::DescriptorPool::generated_pool(), factory, arena);
}

// Convert a corpus literal to a cel-cpp `cel::Value`.  `arena` is the
// allocation home for string/bytes/list/message payloads; `ctx` names
// the BM cell for crash messages.
cel::Value ValueFromLiteral(const CelValueLiteral& lit,
                            google::protobuf::Arena* arena,
                            absl::string_view ctx) {
  switch (lit.kind) {
    case CelValueLiteral::Kind::kInt:
      return {cel::IntValue(lit.int_value)};
    case CelValueLiteral::Kind::kUint:
      return {cel::UintValue(lit.uint_value)};
    case CelValueLiteral::Kind::kDouble:
      return {cel::DoubleValue(lit.double_value)};
    case CelValueLiteral::Kind::kBool:
      return {cel::BoolValue(lit.bool_value)};
    // The kString / kBytes arms construct DIFFERENT types
    // (cel::StringValue vs cel::BytesValue) from the same payload
    // field; clang-tidy's branch-clone check flags them as identical
    // only because the lint environment cannot resolve
    // `benchmark/benchmark.h` for this TU (pre-existing compile-DB
    // gap shared with bench/pipeline_bench.cc) and the degraded AST
    // collapses both constructors into recovery expressions.
    // NOLINTNEXTLINE(bugprone-branch-clone)
    case CelValueLiteral::Kind::kString:
      return {cel::StringValue(arena, lit.string_value)};
    case CelValueLiteral::Kind::kBytes:
      return {cel::BytesValue(arena, lit.string_value)};
    case CelValueLiteral::Kind::kNull:
      return {cel::NullValue()};
    case CelValueLiteral::Kind::kList:
      return ListValueFromLiteral(lit, arena, ctx);
    case CelValueLiteral::Kind::kMessage:
      return MessageValueFromLiteral(lit, arena, ctx);
    case CelValueLiteral::Kind::kMap:
      break;
  }
  ABSL_CHECK(false) << ctx
                    << ": ValueFromLiteral: map activation values not yet "
                       "supported; extend when a cell needs them";
  return {cel::NullValue()};
}

// Surface → BM prefix.  MUST match `celwasm_bench.cc::BmPrefixForSurface`
// — the joined report keys on identical BM names across the two
// binaries (DESIGN.md §12).
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

// True iff any of the cell's tags starts with `prefix`.  Mirrors
// `celwasm_bench.cc::HasTagWithPrefix`.
bool HasTagWithPrefix(const Cell& cell, absl::string_view prefix) {
  return std::any_of(cell.tags.begin(), cell.tags.end(),
                     [prefix](const std::string& t) {
                       return absl::StartsWith(t, prefix);
                     });
}

// Single-entry per-cell runtime cache.  Mirrors
// `celwasm_bench.cc::CachedCellRuntime`: under --benchmark_repetitions
// Google Benchmark re-invokes the bench function once per repetition,
// and rebuilding parse + check + CreateProgram (plus the arena-backed
// activation) per repetition dominates wall clock.  Repetitions of one
// cell run back-to-back, so a single slot keyed by BM name gets a
// (reps-1)/reps hit rate while keeping one program alive at a time.
// The arena lives behind a unique_ptr because google::protobuf::Arena
// is neither movable nor copyable.
struct CellRuntime {
  std::string key;
  std::unique_ptr<google::protobuf::Arena> arena;
  std::unique_ptr<cel::Program> program;
  cel::Activation act;
};

std::optional<CellRuntime>& CachedCellRuntime() {
  static auto* slot = new std::optional<CellRuntime>();
  return *slot;
}

// (Re)fills the cache slot for one cell: declares the typed variables,
// compiles + plans the program, and binds the activation in place.
// `source` feeds CompilePlanOrDie below; the misc-unused-parameters
// suppression is for the lint environment only, where this TU's AST
// degrades (benchmark.h unresolvable — the same pre-existing
// compile-DB gap as the branch-clone note in ValueFromLiteral) and
// the CompilePlanOrDie call goes unparsed.
// NOLINTNEXTLINE(misc-unused-parameters)
void PrepareCellRuntime(const std::string& bm_name, const std::string& source,
                        const std::vector<ActivationEntry>& act_entries) {
  auto& slot = CachedCellRuntime();
  slot.reset();
  slot.emplace();
  slot->key = bm_name;
  slot->arena = std::make_unique<google::protobuf::Arena>();
  std::vector<TypedVar> vars;
  vars.reserve(act_entries.size());
  for (const ActivationEntry& e : act_entries) {
    vars.push_back(
        {e.name, TypeForLiteral(e.value, slot->arena.get(), bm_name)});
  }
  slot->program = CompilePlanOrDie(source, vars);
  for (const ActivationEntry& e : act_entries) {
    slot->act.InsertOrAssignValue(
        e.name, ValueFromLiteral(e.value, slot->arena.get(), bm_name));
  }
}

// Register one corpus cell as a Google Benchmark.  Compiler + Program
// are built once per cell (cached across repetitions); only Evaluate
// runs in the timed region.
void RegisterCorpusCell(const Cell& cell) {
  // celwasm-skip-* tags are celwasm-only constraints — cel-cpp runs
  // those cells.  celcpp-skip-* tags mark the (rare) cells cel-cpp's
  // checker rejects as configured (heterogeneous `==` shapes like
  // `1 == null`, which cel-cpp only admits at runtime via dyn);
  // the per-cell reason lives in the corpus YAML next to the tag.
  if (HasTagWithPrefix(cell, "celcpp-skip")) return;

  std::string bm_name =
      absl::StrCat("BM_", BmPrefixForSurface(cell.surface), "_", cell.id);

  std::string source = cell.source;
  std::vector<ActivationEntry> act_entries = cell.activation;

  benchmark::RegisterBenchmark(bm_name, [bm_name, source = std::move(source),
                                         act_entries = std::move(act_entries)](
                                            benchmark::State& state) {
    auto& slot = CachedCellRuntime();
    if (!slot.has_value() || slot->key != bm_name) {
      PrepareCellRuntime(bm_name, source, act_entries);
    }
    google::protobuf::Arena* arena = slot->arena.get();
    cel::Program& program = *slot->program;
    const cel::Activation& act = slot->act;

    {
      auto v = program.Evaluate(arena, act);
      ABSL_CHECK_OK(v);
      SetResultLabel(state, *v);
    }
    for (auto _ : state) {
      auto v = program.Evaluate(arena, act);
      ABSL_CHECK_OK(v);
      benchmark::DoNotOptimize(v);
    }
  })->Unit(benchmark::kNanosecond);
}

// Cell-bearing YAML files under benchmark/eval/corpus/.  Kept in
// lockstep with `celwasm_bench.cc::kCorpusFiles` so both binaries
// register the same surface set.
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
}  // namespace celwasm_bench_celcpp_proto

// See `celwasm_bench.cc` for rationale; Google Benchmark's setup APIs
// aren't `noexcept` but an uncaught throw in bench main aborts loudly,
// which is the right behaviour.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
  celwasm_bench_celcpp_proto::RegisterAll();
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
