// Throwaway baseline benchmark: cel-cpp interpreter on the
// google.protobuf.Timestamp / google.protobuf.Duration cohort drawn
// from the conformance corpus.  This is M7B prep work — we want a
// reference number for the interpreter we are about to compete
// against in pure-wasm + host-trampoline form.
//
// Scope and lifetime
//   - One-off: do NOT generalise, do NOT promote into kernel_bench
//     or pipeline_bench.  When M7B ships and the wasm side gets a
//     paired bench, this file is deletable.
//   - Cases are baked as string literals.  Each case carries a
//     comment naming the conformance row(s) it derives from (file
//     + section/test name) so the matrix is traceable.
//
// What is measured (per case, three independent BENCHMARK rows)
//   - BM_Compile_<case>  — Compiler::Compile (parse + check).  Fresh
//                          compiler reused across iterations; the
//                          loop body just compiles the source.
//   - BM_Plan_<case>     — CreateAstFromCheckedExpr +
//                          Runtime::CreateProgram.  CheckedExpr
//                          proto pre-staged once outside the loop.
//   - BM_Eval_<case>     — Program::Evaluate on a pre-built
//                          Activation, with a fresh Arena per
//                          iteration (mirrors the
//                          one-arena-per-eval real-world pattern).
//
// Compile + Plan are one-shot in practice; Eval is the steady-state
// hot path and the column that matters most for M7B comparison.
//
// Reading the output
//   - All times in nanoseconds, single-threaded.
//   - "/manual_time" not used — wall clock is fine for relative
//     comparison; absolute numbers are darwin-laptop dependent.
//   - Eval times include arena alloc + Activation lookup overhead;
//     that's representative of how a real host calls the
//     interpreter, so we don't try to amortise it out.
//
// Build & run
//
//   bazel run -c opt //compiler_v2/bench:cel_cpp_ts_dur_bench -- \
//       --benchmark_min_time=0.1s
//
// (Use 0.5s+ for publishable numbers; 0.1s is enough to spot 2x
// regressions and avoids sitting at a terminal for minutes.)

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"

#include "cel/expr/checked.pb.h"
#include "checker/validation_result.h"
#include "common/ast.h"
#include "common/ast_proto.h"
#include "common/value.h"
#include "compiler/compiler.h"
#include "compiler/compiler_factory.h"
#include "compiler/standard_library.h"
#include "runtime/activation.h"
#include "runtime/runtime.h"
#include "runtime/runtime_builder.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/descriptor.h"

namespace celwasm_bench {
namespace {

// ---------------------------------------------------------------
// Shared infra: one compiler, one runtime per process.
// ---------------------------------------------------------------

const cel::Compiler& GlobalCompiler() {
  static const cel::Compiler* const compiler = [] {
    auto builder = cel::NewCompilerBuilder(
        google::protobuf::DescriptorPool::generated_pool());
    ABSL_CHECK_OK(builder.status());
    ABSL_CHECK_OK((*builder)->AddLibrary(cel::StandardCompilerLibrary()));
    auto compiler_or = (*builder)->Build();
    ABSL_CHECK_OK(compiler_or.status());
    return compiler_or->release();
  }();
  return *compiler;
}

const cel::Runtime& GlobalRuntime() {
  static const cel::Runtime* const runtime = [] {
    cel::RuntimeOptions opts;
    auto builder = cel::CreateStandardRuntimeBuilder(
        google::protobuf::DescriptorPool::generated_pool(), opts);
    ABSL_CHECK_OK(builder.status());
    auto rt = std::move(*builder).Build();
    ABSL_CHECK_OK(rt.status());
    return rt->release();
  }();
  return *runtime;
}

// Pre-stages a CheckedExpr proto for an expression.  Used by Plan
// and Eval benches to avoid double-charging the compile cost.
cel::expr::CheckedExpr CheckedFor(absl::string_view src) {
  auto vr = GlobalCompiler().Compile(src);
  ABSL_CHECK_OK(vr.status()) << src;
  ABSL_CHECK(vr->IsValid()) << src << ": " << vr->FormatError();
  cel::expr::CheckedExpr checked;
  ABSL_CHECK_OK(cel::AstToCheckedExpr(*vr->GetAst(), &checked));
  return checked;
}

std::unique_ptr<cel::Program> PlanFor(const cel::expr::CheckedExpr& checked) {
  auto ast = cel::CreateAstFromCheckedExpr(checked);
  ABSL_CHECK_OK(ast.status());
  auto prog = GlobalRuntime().CreateProgram(std::move(*ast));
  ABSL_CHECK_OK(prog.status());
  return std::move(*prog);
}

// ---------------------------------------------------------------
// Curated case table.
// ---------------------------------------------------------------
//
// Each row: (slug, source, provenance comment).  Source is baked
// as a string literal; provenance is in a //-comment so the user
// can trace back to the conformance corpus.

struct Case {
  const char* slug;
  const char* expr;
};

// timestamps.textproto = "tests/simple/testdata/timestamps.textproto"
// conversions.textproto = "tests/simple/testdata/conversions.textproto"
constexpr Case kCases[] = {
    // --- (1) Pure parse: admit shapes ---
    // timestamps.textproto::timestamp_conversions/toString_timestamp
    // (UTC base, no fractional).
    {"parse_ts_utc", "timestamp('2009-02-13T23:31:30Z')"},
    // timestamps.textproto::timestamp_conversions/toString_timestamp_nanos
    // (Y9999 + 9-digit fractional).
    {"parse_ts_nanos_max",
     "timestamp('9999-12-31T23:59:59.999999999Z')"},
    // langdef.md timestamp section / paraphrased from conversions
    // (fixed offset, not Z).
    {"parse_ts_offset", "timestamp('2009-02-13T18:31:30-05:00')"},
    // timestamps.textproto::duration_conversions/toString_duration
    // (whole seconds).
    {"parse_dur_seconds", "duration('1000000s')"},
    // timestamps.textproto::timestamp_arithmetic
    //   /add_time_to_duration_nanos_positive (sub-second, ns unit).
    {"parse_dur_nanos", "duration('999999999ns')"},

    // --- (1b) Pure parse: reject shapes (measure error path) ---
    // timestamps.textproto::timestamp_range/from_string_under
    // (year 0000, out of range).
    {"reject_ts_underflow", "timestamp('0000-01-01T00:00:00Z')"},
    // timestamps.textproto::timestamp_range/from_string_over
    // (year 10000, out of range).
    {"reject_ts_overflow", "timestamp('10000-01-01T00:00:00Z')"},

    // --- (2) Pure arithmetic ---
    // timestamps.textproto::timestamp_arithmetic/add_duration_to_duration.
    {"arith_dur_plus_dur",
     "duration('600s') + duration('50s') == duration('650s')"},
    // timestamps.textproto::timestamp_arithmetic/subtract_time_from_time.
    {"arith_ts_minus_ts",
     "timestamp('2009-02-13T23:31:00Z') - timestamp('2009-02-13T23:29:00Z') "
     "== duration('120s')"},
    // langdef.md worked example shape:
    //   ts + dur == ts (carry across the second boundary).
    {"arith_ts_plus_dur",
     "timestamp('2009-02-13T23:00:00Z') + duration('240s') == "
     "timestamp('2009-02-13T23:04:00Z')"},
    // timestamps.textproto::timestamp_range/add_duration_over
    // (overflow on add).
    {"arith_overflow",
     "timestamp('9999-12-31T23:59:59Z') + duration('1s')"},

    // --- (3) UTC accessors ---
    // timestamps.textproto::timestamp_selectors/getFullYear.
    {"utc_getFullYear",
     "timestamp('2009-02-13T23:31:30Z').getFullYear()"},
    // timestamps.textproto::timestamp_selectors/getDayOfYear
    // (non-leap year, day 43).
    {"utc_getDayOfYear",
     "timestamp('2009-02-13T23:31:30Z').getDayOfYear()"},
    // Variant on getMilliseconds row: sub-second fractional path.
    // timestamps.textproto::timestamp_selectors/getMilliseconds.
    {"utc_getMilliseconds",
     "timestamp('2009-02-13T23:31:20.123456789Z').getMilliseconds()"},

    // --- (4) With-TZ accessors (absl::TimeZone::Load slow path) ---
    // timestamps.textproto::timestamp_selectors_tz/getDate
    //   (IANA name: Australia/Sydney).
    {"tz_iana_getDate",
     "timestamp('2009-02-13T23:31:30Z').getDate('Australia/Sydney')"},
    // timestamps.textproto::timestamp_selectors_tz
    //   /getDayOfMonth_numerical_pos (fixed +11:00 offset).
    {"tz_offset_getDayOfMonth",
     "timestamp('2009-02-13T23:31:30Z').getDayOfMonth('+11:00')"},
    // timestamps.textproto::timestamp_selectors_tz/getMinutes
    //   (Asia/Kathmandu — half-hour offset, exercises non-integral
    //   hour TZ math).
    {"tz_iana_kathmandu_getMinutes",
     "timestamp('2009-02-13T23:31:30Z').getMinutes('Asia/Kathmandu')"},

    // --- (5) Format / string conversion ---
    // timestamps.textproto::timestamp_conversions/toString_timestamp.
    {"fmt_string_ts",
     "string(timestamp('2009-02-13T23:31:30Z'))"},
    // timestamps.textproto::duration_conversions/toString_duration.
    {"fmt_string_dur", "string(duration('1000000s'))"},
    // conversions.textproto: "int(timestamp('2004-09-16T23:59:59Z'))".
    {"fmt_int_ts", "int(timestamp('2004-09-16T23:59:59Z'))"},

    // --- (6) Compound expression: chains parse + accessor +
    //         arithmetic + boolean (the shape a real policy writes). ---
    {"compound_policy",
     "timestamp('2009-02-13T23:31:30Z').getFullYear() == 2009 && "
     "(timestamp('2009-02-13T23:31:30Z') - timestamp('2009-02-13T00:00:00Z')) "
     "> duration('3600s')"},

    // --- (7) Cross-form equivalence: parsed-string ts compared to
    //         a struct-literal Timestamp (m7b §3.4 / §6.5). ---
    // Shape from timestamps.textproto::timestamp_equality coupled
    // with the well-known-type literal form.
    {"crossform_ts_eq_struct",
     "timestamp('1970-01-01T00:00:01Z') == "
     "google.protobuf.Timestamp{seconds: 1}"},
};

constexpr int kNumCases = sizeof(kCases) / sizeof(kCases[0]);

// ---------------------------------------------------------------
// Benchmark bodies.  One Compile/Plan/Eval triple per case,
// registered manually so each row is distinct in the output.
// ---------------------------------------------------------------

void DoCompile(benchmark::State& state, const char* expr) {
  const cel::Compiler& compiler = GlobalCompiler();
  for (auto _ : state) {
    auto vr = compiler.Compile(expr);
    benchmark::DoNotOptimize(vr);
    ABSL_CHECK_OK(vr.status());
  }
}

void DoPlan(benchmark::State& state, const char* expr) {
  cel::expr::CheckedExpr checked = CheckedFor(expr);
  const cel::Runtime& runtime = GlobalRuntime();
  for (auto _ : state) {
    auto ast = cel::CreateAstFromCheckedExpr(checked);
    ABSL_CHECK_OK(ast.status());
    auto prog = runtime.CreateProgram(std::move(*ast));
    benchmark::DoNotOptimize(prog);
    ABSL_CHECK_OK(prog.status());
  }
}

void DoEval(benchmark::State& state, const char* expr) {
  cel::expr::CheckedExpr checked = CheckedFor(expr);
  std::unique_ptr<cel::Program> program = PlanFor(checked);
  cel::Activation activation;  // empty — all of our cases are
                               // closed expressions.
  for (auto _ : state) {
    google::protobuf::Arena arena;
    auto v = program->Evaluate(&arena, activation);
    benchmark::DoNotOptimize(v);
    // For the reject cases, Evaluate returns ok-status with an
    // ErrorValue; only a Plan/Compile failure would surface as a
    // non-ok Status here.  So we check status, not value kind.
    ABSL_CHECK_OK(v.status());
  }
}

// Manually register each case.  benchmark::RegisterBenchmark
// returns a Benchmark*; we ignore it (the registry owns the
// lifetime).  Done in a static initialiser to keep the case
// table the single source of truth.
int RegisterAll() {
  for (int i = 0; i < kNumCases; ++i) {
    const Case& c = kCases[i];
    const char* slug = c.slug;
    const char* expr = c.expr;
    benchmark::RegisterBenchmark(
        (std::string("BM_Compile_") + slug).c_str(),
        [expr](benchmark::State& s) { DoCompile(s, expr); });
    benchmark::RegisterBenchmark(
        (std::string("BM_Plan_") + slug).c_str(),
        [expr](benchmark::State& s) { DoPlan(s, expr); });
    benchmark::RegisterBenchmark(
        (std::string("BM_Eval_") + slug).c_str(),
        [expr](benchmark::State& s) { DoEval(s, expr); });
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterAll();

}  // namespace
}  // namespace celwasm_bench

BENCHMARK_MAIN();
