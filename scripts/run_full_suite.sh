#!/usr/bin/env bash
# scripts/run_full_suite.sh — run every test that gates a milestone close.
#
# `bazel test $PROJ` excludes targets tagged "manual"
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

# Project-package set (exec-doc §1.0) — `//...` is unusable here (vendored
# third_party/cel-cpp loads @com_github_google_flatbuffers, undeclared in
# MODULE.bazel, so `//...` dies on package loading).  Space-joined for
# `bazel test/build` command lines.
PROJ="//compiler/... //eval/... //common/... //abi/... //runtime/... //tools/... //conformance/... //e2e/... //bench/... //testdata/... //spec/..."

QUICK=0
for arg in "$@"; do
  case "$arg" in
    --quick) QUICK=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.."

echo "==> Default test suite"
bazel test $PROJ

echo "==> Manual-tagged targets"
MANUAL_TARGETS=(
  //eval:instance_test
  //eval:engine_test
  //eval:cel_host_test
  //e2e:m2_test
  //e2e:m4_test
  //e2e:m5_test
  //e2e:optimize_test
  //e2e:program_roundtrip_test
  //runtime:cel_runtime_wasm_test
  //tools/wat_runner:wat_runner_test
)
bazel test "${MANUAL_TARGETS[@]}"

if [[ $QUICK -eq 0 ]]; then
  echo "==> Conformance harness"
  bazel run //conformance:run_conformance
fi

echo "==> Full suite green"
