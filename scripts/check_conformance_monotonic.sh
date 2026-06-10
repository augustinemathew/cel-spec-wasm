#!/usr/bin/env bash
# check_conformance_monotonic.sh — assert the conformance PASS
# count hasn't regressed, in BOTH compiler link modes.
#
# Compiler tests are necessary but not sufficient for correctness;
# the conformance suite is the canonical "does CEL behave per
# spec" gate.  PASS count must rise monotonically on master.
#
# The runner is invoked twice — once per `--link_mode`
# (dynamic, then static) — and each mode gates against its own
# baseline:
#
#   - dynamic: `conformance/.baseline` (the original gate,
#     unchanged behaviour).
#   - static:  `conformance/.baseline_static` (added for m28
#     configurable linking; see
#     doc/implementation-plan/rewrite/m28-configurable-linking.md §7.3).
#
# Each baseline file is a single integer line, the lowest PASS
# count master is allowed to drop to in that mode.  Update only
# when a new milestone closes out (the new baseline = the count
# locked at closeout).
#
# Usage:
#   scripts/check_conformance_monotonic.sh           # run + check both modes
#   scripts/check_conformance_monotonic.sh --update  # bump both baselines
#                                                    # to current
#   scripts/check_conformance_monotonic.sh --baseline N  # set explicit
#                                                        # dynamic baseline
#
# Exit 0 = pass count >= baseline in both modes; exit 1 = regression.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Per-mode baseline files.  The static baseline is a separate file
# so each mode can move (and be `--update`d) independently.  The
# m28 initial static baseline, measured 2026-06-09, is 1899 —
# byte-identical to the dynamic run (same pass/skip/fail rows and
# same failure-detail text across the full 2454-row corpus).
BASELINE_FILE_dynamic="conformance/.baseline"
BASELINE_FILE_static="conformance/.baseline_static"

mode="check"
explicit_baseline=""

for arg in "$@"; do
  case "$arg" in
    --update) mode="update" ;;
    --baseline) shift; explicit_baseline="${1:-}" ;;
    --help|-h)
      sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
  esac
done

# Per-mode log paths.  The dynamic log KEEPS the historical
# un-suffixed name: `.githooks/pre-push` reuses it for the README
# drift gate (`regen_conformance_readme.sh --check --from-log
# /tmp/conformance_last_run.log`), and the README quotes the
# dynamic-mode run.
log_path_for_mode() {
  if [[ "$1" == "dynamic" ]]; then
    echo "/tmp/conformance_last_run.log"
  else
    echo "/tmp/conformance_last_run_$1.log"
  fi
}

run_conformance() {
  # $1 = link mode ("dynamic" | "static"), passed through to the
  # runner's --link_mode flag.
  #
  # `-a`: the log can contain non-text bytes (grep would otherwise
  # print "Binary file matches").  `head -n1`: the runner emits its
  # report on both stdout and stderr, so the merged 2>&1 stream has
  # the summary twice — take the first to keep the count single-line.
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
  local link_mode="$1"
  bazel run //conformance:run_conformance -- --link_mode="$link_mode" 2>&1 \
    | tee "$(log_path_for_mode "$link_mode")" \
    | grep -aE '^summary:' \
    | head -n1 \
    | sed -E 's/.*pass=([0-9]+).*/\1/'
}

# check_mode <link_mode> <baseline_file> <explicit_baseline_or_empty>
# Runs the corpus in the given mode and gates the PASS count
# against the baseline.  Returns 0 on >= baseline, 1 on regression.
check_mode() {
  local link_mode="$1" baseline_file="$2" explicit="$3"
  local current baseline delta

  current=$(run_conformance "$link_mode")
  if [[ -z "$current" ]]; then
    echo "error: could not extract pass count from conformance output ($link_mode)" >&2
    echo "  full log at $(log_path_for_mode "$link_mode")" >&2
    exit 2
  fi
  echo "conformance[$link_mode]: current PASS = $current"

  if [[ "$mode" == "update" ]]; then
    echo "$current" > "$baseline_file"
    echo "baseline[$link_mode] updated to $current at $baseline_file"
    return 0
  fi

  if [[ -n "$explicit" ]]; then
    baseline="$explicit"
  elif [[ -f "$baseline_file" ]]; then
    baseline=$(cat "$baseline_file")
  else
    echo "warn: no baseline at $baseline_file — creating with current count"
    echo "$current" > "$baseline_file"
    return 0
  fi

  echo "conformance[$link_mode]: baseline    = $baseline"
  if [[ "$current" -lt "$baseline" ]]; then
    echo "REGRESSION[$link_mode]: pass count dropped $baseline → $current" >&2
    echo "  diff log: see $(log_path_for_mode "$link_mode") for new failures" >&2
    return 1
  fi
  delta=$((current - baseline))
  if [[ "$delta" -gt 0 ]]; then
    echo "ok[$link_mode]: +$delta PASS above baseline.  Consider:"
    echo "  scripts/check_conformance_monotonic.sh --update"
  fi
  return 0
}

status=0

# Dynamic first — the original gate, identical behaviour to the
# single-mode script (same baseline file, `--baseline N` still
# applies to this mode only).
check_mode dynamic "$BASELINE_FILE_dynamic" "$explicit_baseline" || status=1

# Static second — m28 configurable linking.  Static-mode Compile is
# slower (Binaryen runtime merge per expression), so this leg
# dominates the gate's wall time.  Its baseline is the measured m28
# initial static count (conformance/.baseline_static = 1899 as of
# 2026-06-09, equal to dynamic).
check_mode static "$BASELINE_FILE_static" "" || status=1

exit "$status"
