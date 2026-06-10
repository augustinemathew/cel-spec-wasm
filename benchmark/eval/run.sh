#!/usr/bin/env bash
# Comparison runner.  Builds both benches under -c opt, runs
# celwasm_bench once per link mode (m28 — dynamic AND static) plus
# celcpp_bench once, then hands off to report.sh for the two
# mode-vs-cel-cpp comparison tables.
#
# Usage:
#   benchmark/eval/run.sh          # full run + report
#   benchmark/eval/run.sh smoke    # min_time=0.1s, faster turnaround

set -euo pipefail

cd "$(dirname "$0")/../.."

MIN_TIME="0.5s"
if [[ "${1:-}" == "smoke" ]]; then
  MIN_TIME="0.1s"
fi

WASM_DYNAMIC_JSON=/tmp/benchmark_eval_celwasm_dynamic.json
WASM_STATIC_JSON=/tmp/benchmark_eval_celwasm_static.json
CPP_JSON=/tmp/benchmark_eval_celcpp.json

echo "==> Building benches (-c opt)"
bazel build -c opt \
  //benchmark/eval:celwasm_bench \
  //benchmark/eval:celcpp_bench

run_celwasm() {
  local mode="$1" out_json="$2"
  echo "==> Running celwasm_bench --link_mode=$mode (min_time=$MIN_TIME)"
  bazel-bin/benchmark/eval/celwasm_bench \
    --link_mode="$mode" \
    --benchmark_min_time="$MIN_TIME" \
    --benchmark_format=console \
    --benchmark_out_format=json \
    --benchmark_out="$out_json"
}

run_celwasm dynamic "$WASM_DYNAMIC_JSON"
run_celwasm static "$WASM_STATIC_JSON"

echo "==> Running celcpp_bench (min_time=$MIN_TIME)"
bazel-bin/benchmark/eval/celcpp_bench \
  --benchmark_min_time="$MIN_TIME" \
  --benchmark_format=console \
  --benchmark_out_format=json \
  --benchmark_out="$CPP_JSON"

echo
echo "==> Comparison table: celwasm (link_mode=dynamic) vs cel-cpp"
"$(dirname "$0")/report.sh" "$WASM_DYNAMIC_JSON" "$CPP_JSON"

echo
echo "==> Comparison table: celwasm (link_mode=static) vs cel-cpp"
exec "$(dirname "$0")/report.sh" "$WASM_STATIC_JSON" "$CPP_JSON"
