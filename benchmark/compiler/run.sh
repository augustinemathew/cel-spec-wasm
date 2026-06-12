#!/usr/bin/env bash
# Compiler-phase bench runner.  Builds the three timed benches under
# -c opt, runs each with console output plus JSON written to
# /tmp/benchmark_compiler_<name>.json.
#
# Usage:
#   benchmark/compiler/run.sh          # full run (min_time=0.5s)
#   benchmark/compiler/run.sh smoke    # min_time=0.1s, faster turnaround

set -euo pipefail

cd "$(dirname "$0")/../.."

MIN_TIME="0.5s"
if [[ "${1:-}" == "smoke" ]]; then
  MIN_TIME="0.1s"
fi

BENCHES=(pipeline_bench stage_bench in_operator_compile_bench)

echo "==> Building benches (-c opt)"
bazel build -c opt \
  //benchmark/compiler:pipeline_bench \
  //benchmark/compiler:stage_bench \
  //benchmark/compiler:in_operator_compile_bench

for name in "${BENCHES[@]}"; do
  out_json="/tmp/benchmark_compiler_${name}.json"
  echo "==> Running ${name} (min_time=$MIN_TIME)"
  "bazel-bin/benchmark/compiler/${name}" \
    --benchmark_min_time="$MIN_TIME" \
    --benchmark_format=console \
    --benchmark_out_format=json \
    --benchmark_out="$out_json"
  echo "    JSON written to $out_json"
done
