#!/usr/bin/env bash
# check_conformance_monotonic.sh — assert the conformance PASS
# count hasn't regressed.
#
# Compiler tests are necessary but not sufficient for correctness;
# the conformance suite is the canonical "does CEL behave per
# spec" gate.  PASS count must rise monotonically on master.
#
# Baseline lives at `conformance/.baseline` — a single
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

BASELINE_FILE="conformance/.baseline"
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
  # `-a`: the log can contain non-text bytes (grep would otherwise
  # print "Binary file matches").  `head -n1`: the runner emits its
  # report on both stdout and stderr, so the merged 2>&1 stream has
  # the summary twice — take the first to keep `current` single-line.
  #
  # Runs in the DEFAULT (fastbuild) config, NOT `-c opt`.  This is the
  # same configuration the dev loop and `bazel test` use, so the gate
  # reuses the warm dev build tree instead of forcing a second,
  # opt-config compile of cel-cpp (which made every push a ~10 min
  # cold rebuild).  Pass count is identical across configs (verified
  # 1774==1774); the gate checks correctness, not eval throughput, so
  # the slower fastbuild eval is the right trade.  `-c opt` is reserved
  # for benchmarks (//bench) and CI.  See
  # doc/implementation-plan/dev-loop-performance.md.
  bazel run //conformance:run_conformance 2>&1 \
    | tee /tmp/conformance_last_run.log \
    | grep -aE '^summary:' \
    | head -n1 \
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
