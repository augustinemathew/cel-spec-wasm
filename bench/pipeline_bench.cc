// End-to-end pipeline wall-clock benches — deliverable B of the
// post-M10 bench suite.  Times the public `celwasm::Compiler::Compile` +
// `celwasm::Engine::Plan` + `celwasm::Instance::Eval` surface on a
// representative AST-kind matrix.
//
// Setup discipline (mirrors the e2e fixtures in
// `e2e/m{2,4,5,7,9,10}_test.cc`):
//   - One shared `celwasm::Engine` per process (`GlobalEngine()`) — its
//     construction cost is amortised across thousands of Plans / Evals
//     in real use, and our bench shouldn't double-charge it.
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
//   - BM_Eval_*       — measures Eval only (steady state).  Compiles +
//                       Plans once outside the loop; the loop body
//                       Evals with a pre-built Activation.
//
// Per-AST-kind coverage (one representative per kind, picked to match
// the original brief):
//
//   - kConstantExpr        — literal `42`
//   - kSelectExpr          — `c.name` against a proto fixture
//   - kCallExpr(arith)     — `a + b + c` (3-term)
//   - kCallExpr(compare)   — 20-term `a < b && b < c && ...` chain
//   - kCallExpr(type)      — `type(x) == int`
//   - kCreateList          — `[1, 2, 3, 4, 5]`
//   - kCreateMap           — `{"a": 1, "b": 2}`
//   - kStructExpr          — `celwasm.testdata.Customer{name: "Ada"}`
//   - conversion           — `int(string("123"))`

#include <cstdint>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "benchmark/benchmark.h"
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "compiler/program.h"
#include "shared/type.h"
#include "eval/value.h"
#include "testdata/e2e_fixture.pb.h"
#include "google/protobuf/message.h"

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

Program CompileOrDieAt(const Compiler& c, absl::string_view src,
                       int optimize_level) {
  CompilerOptions opts;
  opts.optimize_level = optimize_level;
  auto p = c.Compile(src, opts);
  ABSL_CHECK_OK(p) << src;
  return *std::move(p);
}

Instance PlanOrDie(const Program& p) {
  auto i = GlobalEngine().Plan(p);
  ABSL_CHECK_OK(i);
  return *std::move(i);
}

// ============================================================
// Compile-time benches.
// ============================================================

void BM_Compile_Literal(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  for (auto _ : state) {
    auto p = c.Compile("42");
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_Literal);

void BM_Compile_ThreeTermArith(benchmark::State& state) {
  Compiler c = MakeCompilerWithThreeInts();
  for (auto _ : state) {
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
  for (auto _ : state) {
    auto p = c.Compile(src);
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_TwentyTermCompare);

void BM_Compile_TypeOfEqInt(benchmark::State& state) {
  Compiler c = MakeCompilerWithInt("x");
  for (auto _ : state) {
    auto p = c.Compile("type(x) == int");
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_Compile_TypeOfEqInt);

void BM_Compile_IntFromString(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  for (auto _ : state) {
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
  for (auto _ : state) {
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
  for (auto _ : state) {
    auto i = GlobalEngine().Plan(p);
    ABSL_CHECK_OK(i);
    benchmark::DoNotOptimize(i);
  }
}
BENCHMARK(BM_Plan_Literal);

void BM_Plan_ThreeTermArith(benchmark::State& state) {
  Compiler c = MakeCompilerWithThreeInts();
  Program p = CompileOrDie(c, "a + b + c");
  for (auto _ : state) {
    auto i = GlobalEngine().Plan(p);
    ABSL_CHECK_OK(i);
    benchmark::DoNotOptimize(i);
  }
}
BENCHMARK(BM_Plan_ThreeTermArith);

// ============================================================
// Eval steady-state benches.  Compile + Plan once outside the loop;
// the loop body just Evals — matching how a real host runs (compile
// once, eval many).
// ============================================================

void BM_Eval_Literal(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  Instance inst = PlanOrDie(CompileOrDie(c, "42"));
  Activation a;
  for (auto _ : state) {
    auto v = inst.Eval(a);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_Literal);

void BM_Eval_Select(benchmark::State& state) {
  Compiler c = MakeCompilerWithCustomerVar();
  Instance inst = PlanOrDie(CompileOrDie(c, "c.name"));
  celwasm::testdata::Customer msg;
  msg.set_name("Ada");
  Activation a;
  a.Bind("c", Value::Message(msg));
  for (auto _ : state) {
    auto v = inst.Eval(a);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_Select);

void BM_Eval_ThreeTermArith(benchmark::State& state) {
  Compiler c = MakeCompilerWithThreeInts();
  Instance inst = PlanOrDie(CompileOrDie(c, "a + b + c"));
  Activation act;
  act.Bind("a", Value::Int(1));
  act.Bind("b", Value::Int(2));
  act.Bind("c", Value::Int(3));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_ThreeTermArith);

void BM_Eval_TwentyTermCompare(benchmark::State& state) {
  std::string src;
  for (char ch = 'a'; ch < 't'; ++ch) {
    if (!src.empty()) src.append(" && ");
    src.push_back(ch);
    src.append(" < ");
    src.push_back(static_cast<char>(ch + 1));
  }
  Compiler c = MakeCompilerWith20Ints();
  Instance inst = PlanOrDie(CompileOrDie(c, src));
  Activation act;
  int64_t i = 1;
  for (char ch = 'a'; ch <= 't'; ++ch) {
    act.Bind(std::string(1, ch), Value::Int(i++));
  }
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_TwentyTermCompare);

void BM_Eval_TypeOfEqInt(benchmark::State& state) {
  Compiler c = MakeCompilerWithInt("x");
  Instance inst = PlanOrDie(CompileOrDie(c, "type(x) == int"));
  Activation act;
  act.Bind("x", Value::Int(42));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_TypeOfEqInt);

void BM_Eval_CreateList(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  Instance inst = PlanOrDie(CompileOrDie(c, "[1, 2, 3, 4, 5]"));
  Activation act;
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_CreateList);

void BM_Eval_CreateMap(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  Instance inst = PlanOrDie(CompileOrDie(c, R"({"a": 1, "b": 2})"));
  Activation act;
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_CreateMap);

// ─── Arena vs proto-backed list/map crossover ──────────────────
//
// The dispatch-doc §4 keystone: arena origins go through pure-wasm
// fast paths (`cel_list_at_arena` / `cel_map_lookup_arena`), proto
// origins through the host trampolines (`cel_host.cel_list_at` /
// `cel_host.cel_map_lookup`).  These four benches measure both ends
// of that crossover at the e2e level — same operation (index a
// list, look up a map by key), once on an arena literal and once on
// the equivalent proto repeated / proto map field on a `Customer`
// message.

void BM_Eval_ListAt_Arena(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  Instance inst = PlanOrDie(CompileOrDie(c, "[10, 20, 30, 40, 50][2]"));
  Activation act;
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_ListAt_Arena);

void BM_Eval_ListAt_Proto(benchmark::State& state) {
  Compiler c = MakeCompilerWithCustomerVar();
  Instance inst = PlanOrDie(CompileOrDie(c, "c.tags[2]"));
  celwasm::testdata::Customer msg;
  msg.add_tags("alpha");
  msg.add_tags("beta");
  msg.add_tags("gamma");
  msg.add_tags("delta");
  msg.add_tags("epsilon");
  Activation act;
  act.Bind("c", Value::Message(msg));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_ListAt_Proto);

void BM_Eval_MapLookup_Arena(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  Instance inst =
      PlanOrDie(CompileOrDie(c, R"({"a": 1, "b": 2, "c": 3}["b"])"));
  Activation act;
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_MapLookup_Arena);

void BM_Eval_MapLookup_Proto(benchmark::State& state) {
  Compiler c = MakeCompilerWithCustomerVar();
  Instance inst = PlanOrDie(CompileOrDie(c, "c.metadata[\"b\"]"));
  celwasm::testdata::Customer msg;
  (*msg.mutable_metadata())["a"] = "1";
  (*msg.mutable_metadata())["b"] = "2";
  (*msg.mutable_metadata())["c"] = "3";
  Activation act;
  act.Bind("c", Value::Message(msg));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_MapLookup_Proto);

void BM_Eval_StructLiteral(benchmark::State& state) {
  Compiler c = MakeCompilerWithCustomerVar();
  Instance inst =
      PlanOrDie(CompileOrDie(c, "celwasm.testdata.Customer{name: \"Ada\"}"));
  // The expression doesn't read `c`; the variable is declared just to
  // share the compiler with adjacent benches.  An empty Activation is
  // legal — declared variables only need to be bound if the body
  // reads them.
  celwasm::testdata::Customer dummy;
  Activation act;
  act.Bind("c", Value::Message(dummy));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_StructLiteral);

void BM_Eval_IntFromString(benchmark::State& state) {
  Compiler c = MakeBareCompiler();
  Instance inst = PlanOrDie(CompileOrDie(c, "int(string(123))"));
  Activation act;
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_IntFromString);

// ============================================================
// Binaryen-optimization benches.  Pair each `_Opt2` bench with its
// existing unoptimized counterpart so the README can show the
// Compile-up / Eval-down trade-off in side-by-side columns.
// `optimize_level = 2` runs Binaryen's canonical pass list (DCE +
// constant folding + simplify-locals + vacuum + merge-blocks +
// reorder-functions, etc.) before serializing the module.
// ============================================================

void BM_Compile_ThreeTermArith_Opt2(benchmark::State& state) {
  Compiler c = MakeCompilerWithThreeInts();
  for (auto _ : state) {
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
  for (auto _ : state) {
    auto p = c.Compile(src, opts);
    ABSL_CHECK_OK(p);
    benchmark::DoNotOptimize(*p);
  }
}
BENCHMARK(BM_Compile_TwentyTermCompare_Opt2);

void BM_Eval_ThreeTermArith_Opt2(benchmark::State& state) {
  Compiler c = MakeCompilerWithThreeInts();
  Instance inst = PlanOrDie(CompileOrDieAt(c, "a + b + c", /*opt=*/2));
  Activation act;
  act.Bind("a", Value::Int(1));
  act.Bind("b", Value::Int(2));
  act.Bind("c", Value::Int(3));
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_ThreeTermArith_Opt2);

void BM_Eval_TwentyTermCompare_Opt2(benchmark::State& state) {
  Compiler c = MakeCompilerWith20Ints();
  std::string src;
  for (char ch = 'a'; ch < 't'; ++ch) {
    if (!src.empty()) src.append(" + ");
    src.append(1, ch);
  }
  src.append(" == ");
  src.append(1, 't');
  Instance inst = PlanOrDie(CompileOrDieAt(c, src, /*opt=*/2));
  Activation act;
  for (char ch = 'a'; ch <= 't'; ++ch) {
    act.Bind(std::string(1, ch), Value::Int(1));
  }
  for (auto _ : state) {
    auto v = inst.Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Eval_TwentyTermCompare_Opt2);

}  // namespace
}  // namespace celwasm

BENCHMARK_MAIN();
