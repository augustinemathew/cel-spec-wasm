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
PROJ="//compiler/... //eval/... //shared/... //abi/... //runtime/... //tools/... //conformance/... //e2e/... //benchmark/... //testdata/... //spec/..."

QUICK=0
for arg in "$@"; do
  case "$arg" in
    --quick) QUICK=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.."

echo "==> Default test suite"
bazel test --test_output=errors $PROJ

echo "==> Manual-tagged targets"
# Query-driven: a hardcoded list rotted the first time targets were
# renamed (the dual-link-mode macro split every e2e test into
# <name>_dynamic + <name>_static).  Enumerate every manual-tagged
# test in the project instead, so renames and additions are picked
# up automatically.
MANUAL_TARGETS=$(bazel query 'attr(tags, "manual", tests(//...))' 2>/dev/null)
if [[ -z "$MANUAL_TARGETS" ]]; then
  echo "run_full_suite.sh: manual-target query returned nothing" >&2
  exit 1
fi
echo "$MANUAL_TARGETS" | wc -l | xargs echo "    manual targets:"
# shellcheck disable=SC2086
bazel test --test_output=errors $MANUAL_TARGETS

if [[ $QUICK -eq 0 ]]; then
  echo "==> Conformance gate (both link modes, monotonic baselines)"
  scripts/check_conformance_monotonic.sh
fi

echo "==> Full suite green"
