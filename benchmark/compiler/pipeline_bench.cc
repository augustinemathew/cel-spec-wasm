// Compile-phase and Plan-phase wall-clock benches.  Times the public
// `celwasm::Compiler::Compile` and `celwasm::Engine::Plan` surface on a
// representative AST-kind matrix.  Eval steady-state numbers live in
// the corpus-driven benches under `benchmark/eval/`.
//
// Setup discipline (mirrors the e2e fixtures in
// `e2e/m{2,4,5,7,9,10}_test.cc`):
//   - One shared `celwasm::Engine` per process (`GlobalEngine()`) — its
//     construction cost is amortised across thousands of Plans in real
//     use, and our bench shouldn't double-charge it.
//   - One `celwasm::Compiler` per benchmark — declared variables matter for
//     compile-time, but constructing the compiler itself isn't the
//     interesting number.
//
// Bench shapes:
//
//   - BM_Compile_*    — measures Compile only.  Builds a fresh compiler
//                       once outside the loop; the loop body just
//                       compiles the source.
//   - BM_Plan_*       — measures Plan only.  Pre-compiles the Program
//                       once outside the loop; the loop body Plans.
//
// Per-AST-kind coverage (one representative per kind):
//
//   - kConstantExpr        — literal `42`
//   - kCallExpr(arith)     — `a + b + c` (3-term)
//   - kCallExpr(compare)   — 20-term `a < b && b < c && ...` chain
//   - kCallExpr(type)      — `type(x) == int`
//   - kStructExpr          — `celwasm.testdata.Customer{name: "Ada"}`
//   - conversion           — `int(string(123))`

#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "google/protobuf/message.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

// Force generated-pool registration of descriptors used below.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      return 0;
    }();

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

Compiler MakeBareCompiler() {
  auto c = Compiler::Builder().Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

Compiler MakeCompilerWithInt(const std::string& name) {
  Compiler::Builder b;
  b.DeclareVariable(name, CelType::Int());
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

Compiler MakeCompilerWithThreeInts() {
  Compiler::Builder b;
  b.DeclareVariable("a", CelType::Int());
  b.DeclareVariable("b", CelType::Int());
  b.DeclareVariable("c", CelType::Int());
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

Compiler MakeCompilerWith20Ints() {
  Compiler::Builder b;
  for (char ch = 'a'; ch <= 't'; ++ch) {
    b.DeclareVariable(std::string(1, ch), CelType::Int());
  }
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

Compiler MakeCompilerWithCustomerVar() {
  Compiler::Builder b;
  b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

Program CompileOrDie(const Compiler& c, absl::string_view src) {
  auto p = c.Compile(src);
  ABSL_CHECK_OK(p) << src;
  return *std::move(p);
}

// ============================================================
// Compile-time benches.
// ============================================================

void BM_Compile_Literal(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile("42");
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_Literal);

void BM_Compile_ThreeTermArith(benchmark::State& state) {
  Compiler c = MakeCompilerWithThreeInts();
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile("a + b + c");
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_ThreeTermArith);

void BM_Compile_TwentyTermCompare(benchmark::State& state) {
  // 20-term comparison chain: a < b && b < c && ... && s < t.
  std::string src;
  for (char ch = 'a'; ch < 't'; ++ch) {
    if (!src.empty()) src.append(" && ");
    src.push_back(ch);
    src.append(" < ");
    src.push_back(static_cast<char>(ch + 1));
  }
  Compiler c = MakeCompilerWith20Ints();
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile(src);
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_TwentyTermCompare);

void BM_Compile_TypeOfEqInt(benchmark::State& state) {
  Compiler c = MakeCompilerWithInt("x");
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile("type(x) == int");
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_TypeOfEqInt);

void BM_Compile_IntFromString(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile("int(string(123))");
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_IntFromString);

void BM_Compile_StructLiteral(benchmark::State& state) {
  Compiler c = MakeCompilerWithCustomerVar();
  // Reference the declared variable so the compiler builds against
  // the right schema.  The struct literal alone parses but the
  // compose-with-existing-variable shape is the realistic one.
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile("celwasm.testdata.Customer{name: \"Ada\"}");
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_StructLiteral);

// ============================================================
// Plan-time benches.
// ============================================================

void BM_Plan_Literal(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  Program p = CompileOrDie(c, "42");
  for ([[maybe_unused]] auto _ : state) {
    auto i = GlobalEngine().Plan(p);
    ABSL_CHECK_OK(i);
    benchmark::DoNotOptimize(i);
  }
}
BENCHMARK(BM_Plan_Literal);

void BM_Plan_ThreeTermArith(benchmark::State& state) {
  Compiler c = MakeCompilerWithThreeInts();
  Program p = CompileOrDie(c, "a + b + c");
  for ([[maybe_unused]] auto _ : state) {
    auto i = GlobalEngine().Plan(p);
    ABSL_CHECK_OK(i);
    benchmark::DoNotOptimize(i);
  }
}
BENCHMARK(BM_Plan_ThreeTermArith);

// ============================================================
// Binaryen-optimization benches.  Pair each `_Opt2` bench with its
// existing unoptimized counterpart so the README can show the
// Compile-time cost of optimization in side-by-side columns.
// `optimize_level = 2` runs Binaryen's canonical pass list (DCE +
// constant folding + simplify-locals + vacuum + merge-blocks +
// reorder-functions, etc.) before serializing the module.
// ============================================================

void BM_Compile_ThreeTermArith_Opt2(benchmark::State& state) {
  Compiler c = MakeCompilerWithThreeInts();
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile("a + b + c", [] {
      CompilerOptions o;
      o.optimize_level = 2;
      return o;
    }());
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(*p);
  }
}
BENCHMARK(BM_Compile_ThreeTermArith_Opt2);

void BM_Compile_TwentyTermCompare_Opt2(benchmark::State& state) {
  Compiler c = MakeCompilerWith20Ints();
  std::string src;
  for (char ch = 'a'; ch < 't'; ++ch) {
    if (!src.empty()) src.append(" + ");
    src.append(1, ch);
  }
  src.append(" == ");
  src.append(1, 't');
  CompilerOptions opts;
  opts.optimize_level = 2;
  for ([[maybe_unused]] auto _ : state) {
    auto p = c.Compile(src, opts);
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(*p);
  }
}
BENCHMARK(BM_Compile_TwentyTermCompare_Opt2);

}  // namespace
}  // namespace celwasm

BENCHMARK_MAIN();
