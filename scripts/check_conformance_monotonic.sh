#!/usr/bin/env bash
# check_conformance_monotonic.sh — assert the conformance PASS
# count hasn't regressed.
#
# Compiler tests are necessary but not sufficient for correctness;
# the conformance suite is the canonical "does CEL behave per
# spec" gate.  PASS count must rise monotonically on master.
#
# Baseline lives at `compiler_v2/conformance/.baseline` — a single
# integer line, the lowest PASS count master is allowed to drop
# to.  Update only when a new milestone closes out (the new
# baseline = the count locked at closeout).
#
# Usage:
#   scripts/check_conformance_monotonic.sh           # run + check
#   scripts/check_conformance_monotonic.sh --update  # bump baseline
#                                                    # to current
#   scripts/check_conformance_monotonic.sh --baseline N  # set
#                                                        # explicit
#
# Exit 0 = pass count >= baseline; exit 1 = regression.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

BASELINE_FILE="compiler_v2/conformance/.baseline"
mode="check"
explicit_baseline=""

for arg in "$@"; do
  case "$arg" in
    --update) mode="update" ;;
    --baseline) shift; explicit_baseline="${1:-}" ;;
    --help|-h)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
  esac
done

run_conformance() {
  bazel run -c opt //compiler_v2/conformance:run_conformance 2>&1 \
    | tee /tmp/conformance_last_run.log \
    | grep -E '^summary:' \
    | sed -E 's/.*pass=([0-9]+).*/\1/'
}

current=$(run_conformance)
if [[ -z "$current" ]]; then
  echo "error: could not extract pass count from conformance output" >&2
  echo "  full log at /tmp/conformance_last_run.log" >&2
  exit 2
fi
echo "conformance: current PASS = $current"

if [[ "$mode" == "update" ]]; then
  echo "$current" > "$BASELINE_FILE"
  echo "baseline updated to $current at $BASELINE_FILE"
  exit 0
fi

if [[ -n "$explicit_baseline" ]]; then
  baseline="$explicit_baseline"
elif [[ -f "$BASELINE_FILE" ]]; then
  baseline=$(cat "$BASELINE_FILE")
else
  echo "warn: no baseline at $BASELINE_FILE — creating with current count"
  echo "$current" > "$BASELINE_FILE"
  exit 0
fi

echo "conformance: baseline    = $baseline"
if [[ "$current" -lt "$baseline" ]]; then
  echo "REGRESSION: pass count dropped $baseline → $current" >&2
  echo "  diff log: see /tmp/conformance_last_run.log for new failures" >&2
  exit 1
fi
delta=$((current - baseline))
if [[ "$delta" -gt 0 ]]; then
  echo "ok: +$delta PASS above baseline.  Consider:"
  echo "  scripts/check_conformance_monotonic.sh --update"
fi
exit 0
