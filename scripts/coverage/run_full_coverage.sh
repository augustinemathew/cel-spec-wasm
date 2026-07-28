#!/usr/bin/env bash
# run_full_coverage.sh — the whole coverage pipeline, one command.
#
#   scripts/coverage/run_full_coverage.sh [<out_dir>]
#
# Produces <out_dir>/report.{json,html} (default: /tmp/celwasm-coverage):
# the combined Compiler / Eval / CelRuntime report with per-workload
# attribution, e2e-only vs all-tests split, and native branch data.
#
# Stages (each incremental — a warm tree re-runs in minutes):
#   1. wasm side: build every dynamic e2e binary + the wasm-instantiating
#      eval manual suites + the conformance runner with
#      --//runtime:instrument_wasm, run each with its own
#      CELWASM_WASM_GCOV_DIR (collect_wasm_gcov.sh), then the
#      conformance corpus + engine_test via `bazel run` (they need
#      runfiles).
#   2. native side: `bazel coverage` in LLVM source-based mode over
#      every test target (llvm_gcov.sh shim), which leaves per-target
#      merged .profdata in bazel-testlogs.
#   3. join: native_cov_report.py exports per-target lcov (llvm-cov)
#      and merges the wasm layer into one report.
#
# Requirements: brew llvm (llvm-cov/llvm-profdata 18+) at
# /opt/homebrew/opt/llvm (override with LLVM_BIN).

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

OUT=${1:-/tmp/celwasm-coverage}
WASM_COV="$OUT/wasm-cov"
LLVM_BIN=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
INSTR_FLAGS=(--//runtime:instrument_wasm --collect_code_coverage
             '--instrumentation_filter=^//runtime[/:]')
NATIVE_FILTER='^//(compiler|eval|shared|abi|tools|conformance)[/:]'

echo "── 1/3 wasm side ──────────────────────────────────────────────"
# Dynamic-mode e2e binaries + the wasm-instantiating eval manual
# suites.  engine_test is run via `bazel run` below (runfiles).
E2E_BINS=$(bazel query 'kind("cc_test|cc_binary", //e2e:all)' \
    | grep -v '_static$' | grep -v repro_pbt_bug)
EVAL_BINS="//eval:instance_test_dynamic //eval:memory_grow_stability_test_dynamic"
bazel build "${INSTR_FLAGS[@]}" $E2E_BINS $EVAL_BINS \
    //eval:engine_test_dynamic //conformance:run_conformance

scripts/coverage/collect_wasm_gcov.sh "$WASM_COV" \
    $(echo "$E2E_BINS $EVAL_BINS" | tr ' ' '\n' \
      | sed 's|//|bazel-bin/|; s|:|/|' | tr '\n' ' ')

for RUNFILES_WORKLOAD in "engine_test_dynamic //eval:engine_test_dynamic" \
                         "conformance //conformance:run_conformance -- --link_mode=dynamic"; do
  name=${RUNFILES_WORKLOAD%% *}
  target=${RUNFILES_WORKLOAD#* }
  dir="$WASM_COV/raw/$name"
  rm -rf "$dir" && mkdir -p "$dir"
  CELWASM_WASM_GCOV_DIR="$dir" bazel run "${INSTR_FLAGS[@]}" $target \
      >"$dir/run.log" 2>&1 || echo "WARN: $name non-zero (see run.log)"
done
# Decode the two bazel-run workloads with the same gcno pairing.
scripts/coverage/collect_wasm_gcov.sh --decode-only "$WASM_COV" 2>/dev/null || true

echo "── 2/3 native side ────────────────────────────────────────────"
TARGETS=$(bazel query 'tests(//...) except //e2e/fuzz:cel_oracle_property_test')
bazel coverage \
    --repo_env=BAZEL_USE_LLVM_NATIVE_COVERAGE=1 \
    --repo_env=BAZEL_LLVM_PROFDATA="$LLVM_BIN/llvm-profdata" \
    --repo_env=BAZEL_LLVM_COV="$LLVM_BIN/llvm-cov" \
    --repo_env=GCOV="$PWD/scripts/coverage/llvm_gcov.sh" \
    "--instrumentation_filter=$NATIVE_FILTER" \
    $TARGETS

echo "── 3/3 report ─────────────────────────────────────────────────"
# Keep the previous report for diffing (one generation of history).
if [[ -f "$OUT/report.json" ]]; then
  mkdir -p "$OUT/prev"
  cp -f "$OUT/report.json" "$OUT/prev/report.json"
  cp -f "$OUT/report.html" "$OUT/prev/report.html" 2>/dev/null || true
fi
python3 scripts/coverage/native_cov_report.py \
    --repo-root . --out "$OUT" --wasm-cov-root "$WASM_COV"
if [[ -f "$OUT/prev/report.json" ]]; then
  python3 scripts/coverage/report_diff.py \
      "$OUT/prev/report.json" "$OUT/report.json" || true
fi
echo "report: $OUT/report.html"
