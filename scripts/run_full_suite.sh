#!/usr/bin/env bash
# scripts/run_full_suite.sh — run every test that gates a milestone close.
#
# `bazel test //compiler_v2/...` excludes targets tagged "manual"
# (wasmtime-driven e2e, wasm32-cross-compiled runtime tests, fixture-
# heavy integration suites).  Those carry the load-bearing assertions.
# This script bundles them all into one invocation so the closeout
# checklist in per-component-test-coverage.md §5 is one command.
#
# Usage:
#   scripts/run_full_suite.sh             # run everything
#   scripts/run_full_suite.sh --quick     # skip conformance harness (slow)
#
# Exits non-zero on the first failing target.

set -euo pipefail

QUICK=0
for arg in "$@"; do
  case "$arg" in
    --quick) QUICK=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.."

echo "==> Default test suite"
bazel test //compiler_v2/...

echo "==> Manual-tagged targets"
MANUAL_TARGETS=(
  //compiler_v2/api:instance_test
  //compiler_v2/api:engine_test
  //compiler_v2/api:cel_host_test
  //compiler_v2/e2e:m2_test
  //compiler_v2/e2e:m4_test
  //compiler_v2/runtime:cel_runtime_wasm_test
  //compiler_v2/tools/wat_runner:wat_runner_test
)
bazel test "${MANUAL_TARGETS[@]}"

if [[ $QUICK -eq 0 ]]; then
  echo "==> Conformance harness"
  bazel run //compiler_v2/conformance:run_conformance
fi

echo "==> Full suite green"
