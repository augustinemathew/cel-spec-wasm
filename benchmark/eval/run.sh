#!/usr/bin/env bash
# Comparison runner + publisher.  Builds the benches under -c opt, runs
# celwasm_bench per link mode plus celcpp_bench, joins the JSONs via
# report.py, and (full unfiltered runs only) publishes: dated tables
# under benchmark/eval/results/ (committed) + the auto-generated
# Results section of benchmark/README.md.
#
# Usage:
#   benchmark/eval/run.sh                     # full run + publish
#   benchmark/eval/run.sh smoke               # fast correctness+parity:
#                                             #   min_time=0.01s, static
#                                             #   link mode only, stdout
#   benchmark/eval/run.sh [smoke] policies proto …
#                                             # just those surfaces
#                                             # (stdout only — partial
#                                             # runs never publish)
#   BENCH_REPS=3 benchmark/eval/run.sh        # medians of N reps
#
# Surface names: `python3 benchmark/eval/report.py --list-surfaces`.

set -euo pipefail

cd "$(dirname "$0")/../.."

MIN_TIME="0.5s"
PUBLISH=1
MODES=(dynamic static)
if [[ "${1:-}" == "smoke" ]]; then
  shift
  MIN_TIME="0.01s"
  PUBLISH=0
  MODES=(static)
fi

FILTER=""
if [[ $# -gt 0 ]]; then
  FILTER="$(python3 benchmark/eval/report.py --filter-for "$@")"
  PUBLISH=0
  echo "==> Surface filter: $FILTER"
fi

REPS="${BENCH_REPS:-1}"
COMMON_FLAGS=(
  "--benchmark_min_time=$MIN_TIME"
  --benchmark_format=console
  --benchmark_out_format=json
)
if [[ -n "$FILTER" ]]; then
  COMMON_FLAGS+=("--benchmark_filter=$FILTER")
fi
if [[ "$REPS" -gt 1 ]]; then
  COMMON_FLAGS+=(
    "--benchmark_repetitions=$REPS"
    "--benchmark_report_aggregates_only=true"
  )
fi

echo "==> Building benches (-c opt)"
bazel build -c opt \
  //benchmark/eval:celwasm_bench \
  //benchmark/eval:celcpp_bench

# Resolve the opt output tree directly: the `bazel-bin` convenience
# symlink is repointed by ANY concurrent bazel invocation in another
# configuration (a fastbuild test run mid-benchmark left this script
# pointing at a tree without the opt binaries — observed 2026-06-12).
OPT_BIN="$(bazel info -c opt bazel-bin)"

REPORT_ARGS=()

for mode in "${MODES[@]}"; do
  out_json="/tmp/benchmark_eval_celwasm_$mode.json"
  echo "==> celwasm_bench --link_mode=$mode (min_time=$MIN_TIME, reps=$REPS)"
  "$OPT_BIN"/benchmark/eval/celwasm_bench \
    --link_mode="$mode" \
    "${COMMON_FLAGS[@]}" \
    --benchmark_out="$out_json"
  REPORT_ARGS+=(--json "celwasm-$mode=$out_json")
done

CPP_JSON=/tmp/benchmark_eval_celcpp.json
echo "==> celcpp_bench (min_time=$MIN_TIME, reps=$REPS)"
"$OPT_BIN"/benchmark/eval/celcpp_bench \
  "${COMMON_FLAGS[@]}" \
  --benchmark_out="$CPP_JSON"
REPORT_ARGS+=(--json "cel-cpp=$CPP_JSON" --baseline cel-cpp)

if [[ "$PUBLISH" == "1" ]]; then
  STAMP="$(date +%F)-$(hostname -s)"
  RESULTS_DIR=benchmark/eval/results
  mkdir -p "$RESULTS_DIR/raw/$STAMP"
  cp /tmp/benchmark_eval_*.json "$RESULTS_DIR/raw/$STAMP/"
  echo "==> Publishing results ($STAMP)"
  python3 benchmark/eval/report.py "${REPORT_ARGS[@]}" \
    --out-md "$RESULTS_DIR/$STAMP.md" \
    --out-csv "$RESULTS_DIR/$STAMP.csv" \
    --update-readme benchmark/README.md
  echo "==> Done.  Review + commit:"
  echo "      $RESULTS_DIR/$STAMP.md"
  echo "      $RESULTS_DIR/$STAMP.csv"
  echo "      benchmark/README.md (auto-results section)"
else
  echo "==> Report (not archived)"
  python3 benchmark/eval/report.py "${REPORT_ARGS[@]}"
fi
