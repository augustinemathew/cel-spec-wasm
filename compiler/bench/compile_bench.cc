// Google Benchmark harness for the compile path.
//
// Each iteration runs ParseAndCheck + Lower + Serialize + LoadEval for
// a single expression.  LoadEval is included because users of the
// library invoke the full pipeline, and the wasmtime engine/store
// creation dominates for tiny expressions — tracking it here makes
// "compile" honest.
//
// Parser default recursion depth is 32, so AddChain sweeps stop at 28.
// Run with `bazel run -c opt //compiler/bench:compile_bench`.

#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "compiler/bench/bench_fixture.h"

namespace celwasm::bench {
namespace {

std::string BuildAddChain(int n) {
  std::string out = "1";
  for (int i = 1; i < n; ++i) {
    out += " + 1";
  }
  return out;
}

std::string AsciiFill(int len) {
  std::string s;
  s.reserve(len);
  for (int i = 0; i < len; ++i) {
    s.push_back('a' + (i % 26));
  }
  return s;
}

void CompileOnce(benchmark::State& state, absl::string_view source,
                 const std::vector<std::string>& specs) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(_);
    auto loaded = Precompile(source, specs);
    benchmark::DoNotOptimize(loaded);
    if (!loaded.ok()) {
      state.SkipWithError(loaded.status().ToString());
      return;
    }
  }
}

void BM_CompileIntLiteral(benchmark::State& state) {
  CompileOnce(state, "42", {});
}
BENCHMARK(BM_CompileIntLiteral);

void BM_CompileArithmetic(benchmark::State& state) {
  CompileOnce(state, "(1 + 2) * 3 - 4 / 2", {});
}
BENCHMARK(BM_CompileArithmetic);

void BM_CompileBoolLogic(benchmark::State& state) {
  CompileOnce(state, "true && (false || true) && !false", {});
}
BENCHMARK(BM_CompileBoolLogic);

void BM_CompileTernary(benchmark::State& state) {
  CompileOnce(state, "(1 + 2) * 3 == 9 ? 42 : -1", {});
}
BENCHMARK(BM_CompileTernary);

void BM_CompileAddChainInt(benchmark::State& state) {
  std::string src = BuildAddChain(state.range(0));
  CompileOnce(state, src, {});
}
BENCHMARK(BM_CompileAddChainInt)->Arg(1)->Arg(4)->Arg(16)->Arg(28);

void BM_CompileStringLiteral(benchmark::State& state) {
  std::string src = absl::StrCat("'", AsciiFill(state.range(0)), "'");
  CompileOnce(state, src, {});
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CompileStringLiteral)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

void BM_CompileStringEq(benchmark::State& state) {
  std::string s = AsciiFill(state.range(0));
  std::string src = absl::StrCat("'", s, "' == '", s, "'");
  CompileOnce(state, src, {});
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CompileStringEq)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

void BM_CompileWithVariables(benchmark::State& state) {
  CompileOnce(state, "x + y * 2", {"x:int", "y:int"});
}
BENCHMARK(BM_CompileWithVariables);

void BM_CompileCustomerFieldEq(benchmark::State& state) {
  CompileOnce(state, "c.name == 'Ada' && c.age > 18",
              {"c:celwasm.testdata.Customer"});
}
BENCHMARK(BM_CompileCustomerFieldEq);

}  // namespace
}  // namespace celwasm::bench
