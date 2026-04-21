// Google Benchmark harness for the CEL eval path.
//
// Every benchmark pre-compiles its expression outside `for (auto _ :
// state)` so only `CallEval` is timed.  The matrix below sweeps the
// dimensions we care most about (see Slice F / M4 docs):
//
//   - Arithmetic depth (AddChain, DeepPrecedenceTree) — measures
//     per-opcode eval cost + sret arena traffic.
//   - Boolean / logical chains — measures `cel_and` / `cel_or`
//     short-circuit cost vs. the i32-fast-path left.
//   - Ternary ladders — measures nested sret copy + 3VL early-return
//     dispatch.
//   - String length (L) — `size(s)` is O(1), `==` is O(L); we benchmark
//     both so a regression in `cel_string_eq_v` (or in cel_alloc of a
//     long literal) surfaces separately from `size` changes.
//   - String match position (P) — worst-case for `contains`-style ops
//     is a match right at the tail; tests both "match at front" and
//     "match at end".
//
// Default parser recursion depth is 32 (cel-cpp ParserOptions), so
// depth sweeps stop at N=28 for left-folded chains.  Balanced
// expressions can go deeper once we bump that option; not worth doing
// now.
//
// Run a filtered subset:
//   bazel run -c opt //compiler/bench:eval_bench -- \
//       --benchmark_filter=BM_StringEq.*L_1000 --benchmark_out=bench.json

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "compiler/bench/bench_fixture.h"
#include "compiler/host/host_loader.h"
#include "wasmtime.h"

namespace celwasm::bench {
namespace {

// --- expression builders --------------------------------------------------

std::string BuildAddChain(int n) {
  // `1 + 1 + 1 + … + 1` with `n` operands.  Left-associative; parser
  // recursion depth equals n, so stop short of 32 at the caller.
  std::string out = "1";
  for (int i = 1; i < n; ++i) {
    out += " + 1";
  }
  return out;
}

std::string BuildMulChain(int n) {
  std::string out = "2";
  for (int i = 1; i < n; ++i) {
    out += " * 2";
  }
  return out;
}

std::string BuildAndChain(int n) {
  std::string out = "true";
  for (int i = 1; i < n; ++i) {
    out += " && true";
  }
  return out;
}

std::string BuildOrChainFalseHeavy(int n) {
  // `false || false || … || true` — worst-case for `||`: every operand
  // except the last must evaluate to `false` before short-circuit.
  std::string out = "false";
  for (int i = 1; i < n - 1; ++i) {
    out += " || false";
  }
  if (n > 1) {
    out += " || true";
  }
  return out;
}

std::string BuildTernaryLadder(int n) {
  // `true ? 1 : (true ? 1 : (… : 0))`; right-nested so parse depth is n.
  std::string out = "0";
  for (int i = 0; i < n; ++i) {
    out = absl::StrCat("true ? 1 : (", out, ")");
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

// --- driver ----------------------------------------------------------------

void RunCompiled(benchmark::State& state, PrecompiledEval& eval) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(_);
    auto r = eval.loaded.CallEval(eval.args);
    benchmark::DoNotOptimize(r);
    if (!r.ok()) {
      state.SkipWithError(r.status().ToString());
      return;
    }
  }
}

PrecompiledEval MustPrecompile(absl::string_view source,
                               std::vector<std::string> specs,
                               benchmark::State& state) {
  auto loaded = Precompile(source, std::move(specs));
  PrecompiledEval out;
  if (!loaded.ok()) {
    state.SkipWithError(loaded.status().ToString());
    return out;
  }
  out.loaded = *std::move(loaded);
  return out;
}

// --- literals + arithmetic -------------------------------------------------

void BM_IntLiteral(benchmark::State& state) {
  auto eval = MustPrecompile("42", {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_IntLiteral);

void BM_DoubleLiteral(benchmark::State& state) {
  auto eval = MustPrecompile("3.14", {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_DoubleLiteral);

void BM_AddChainInt(benchmark::State& state) {
  auto eval = MustPrecompile(BuildAddChain(state.range(0)), {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_AddChainInt)->Arg(1)->Arg(4)->Arg(16)->Arg(28);

void BM_MulChainInt(benchmark::State& state) {
  auto eval = MustPrecompile(BuildMulChain(state.range(0)), {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_MulChainInt)->Arg(1)->Arg(4)->Arg(16)->Arg(28);

void BM_AddChainDouble(benchmark::State& state) {
  std::string src = "1.0";
  for (int i = 1; i < state.range(0); ++i) {
    src += " + 1.0";
  }
  auto eval = MustPrecompile(src, {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_AddChainDouble)->Arg(1)->Arg(4)->Arg(16)->Arg(28);

// --- boolean / logical -----------------------------------------------------

void BM_AndChain(benchmark::State& state) {
  auto eval = MustPrecompile(BuildAndChain(state.range(0)), {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_AndChain)->Arg(1)->Arg(4)->Arg(16)->Arg(28);

void BM_OrChainFalseHeavy(benchmark::State& state) {
  auto eval = MustPrecompile(BuildOrChainFalseHeavy(state.range(0)), {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_OrChainFalseHeavy)->Arg(2)->Arg(4)->Arg(16)->Arg(28);

void BM_TernaryLadder(benchmark::State& state) {
  auto eval = MustPrecompile(BuildTernaryLadder(state.range(0)), {}, state);
  RunCompiled(state, eval);
}
// Capped at 24 because nested-ternary parse depth equals N and
// cel-cpp's ParserOptions.max_recursion_depth defaults to 32.  A
// benchmark that exceeds the shipping parser limit would measure a
// failure path, not the ternary itself.
BENCHMARK(BM_TernaryLadder)->Arg(1)->Arg(4)->Arg(16)->Arg(24);

// --- strings: literal operands --------------------------------------------

void BM_StringLiteralPassThrough(benchmark::State& state) {
  std::string src = absl::StrCat("'", AsciiFill(state.range(0)), "'");
  auto eval = MustPrecompile(src, {}, state);
  RunCompiled(state, eval);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_StringLiteralPassThrough)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);

void BM_SizeOfLiteralString(benchmark::State& state) {
  std::string src = absl::StrCat("size('", AsciiFill(state.range(0)), "')");
  auto eval = MustPrecompile(src, {}, state);
  RunCompiled(state, eval);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SizeOfLiteralString)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

// Equality on literal strings.  Both operands identical → equality op
// must walk all `L` bytes; pinned here as the worst case for `==`.
void BM_StringEqLiteralMatch(benchmark::State& state) {
  std::string s = AsciiFill(state.range(0));
  std::string src = absl::StrCat("'", s, "' == '", s, "'");
  auto eval = MustPrecompile(src, {}, state);
  RunCompiled(state, eval);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_StringEqLiteralMatch)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

// Mismatch at position P (early mismatch = fast return).  P axis is
// fraction of operand length: 0 (front), L/2 (middle), L-1 (tail).
void BM_StringEqLiteralMismatchAtFront(benchmark::State& state) {
  std::string a = AsciiFill(state.range(0));
  std::string b = a;
  if (!b.empty()) {
    b.front() = 'Z';
  }
  auto eval = MustPrecompile(absl::StrCat("'", a, "' == '", b, "'"), {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_StringEqLiteralMismatchAtFront)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);

void BM_StringEqLiteralMismatchAtTail(benchmark::State& state) {
  std::string a = AsciiFill(state.range(0));
  std::string b = a;
  if (!b.empty()) {
    b.back() = 'Z';
  }
  auto eval = MustPrecompile(absl::StrCat("'", a, "' == '", b, "'"), {}, state);
  RunCompiled(state, eval);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_StringEqLiteralMismatchAtTail)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);

// --- strings: concat -------------------------------------------------------

void BM_StringConcatLiteral(benchmark::State& state) {
  std::string a = AsciiFill(state.range(0));
  auto eval = MustPrecompile(absl::StrCat("'", a, "' + '", a, "'"), {}, state);
  RunCompiled(state, eval);
  state.SetBytesProcessed(state.iterations() * state.range(0) * 2);
}
BENCHMARK(BM_StringConcatLiteral)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

void BM_SizeOfConcatenatedString(benchmark::State& state) {
  std::string a = AsciiFill(state.range(0));
  auto eval =
      MustPrecompile(absl::StrCat("size('", a, "' + '", a, "')"), {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_SizeOfConcatenatedString)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);

// --- bytes -----------------------------------------------------------------

void BM_BytesLiteralPassThrough(benchmark::State& state) {
  std::string src = absl::StrCat("b'", AsciiFill(state.range(0)), "'");
  auto eval = MustPrecompile(src, {}, state);
  RunCompiled(state, eval);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_BytesLiteralPassThrough)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

void BM_SizeOfLiteralBytes(benchmark::State& state) {
  std::string src = absl::StrCat("size(b'", AsciiFill(state.range(0)), "')");
  auto eval = MustPrecompile(src, {}, state);
  RunCompiled(state, eval);
}
BENCHMARK(BM_SizeOfLiteralBytes)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

}  // namespace
}  // namespace celwasm::bench
