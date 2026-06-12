#!/usr/bin/env bash
# fuzz.sh — the one entry point for the CEL differential fuzzer.
#
# The fuzzer ('e2e/fuzz/') generates type-checked CEL source and
# evaluates it through both our compiler and the cel-cpp oracle; any
# value/error divergence is a real bug.  This wraps the common
# invocations so you don't memorise miner flags or chase stray
# processes.
#
#   fuzz.sh validate                 — L1/L2/L3 grammar checks (run first)
#   fuzz.sh mine <target> [seeds] [depth] [stop_after]
#                                    — mine one target; exits non-zero on divergence
#   fuzz.sh sweep [seeds] [depth]    — mine every target; fails if any diverges
#   fuzz.sh repro <target> <seed> <depth>
#                                    — re-run ONE seed, print source + both sides
#   fuzz.sh samples <target> <depth> <count>
#                                    — print generated sources (eyeball the grammar)
#   fuzz.sh kill                     — kill any stray miner processes
#
# Defaults: seeds=500, depth=6, stop_after=10.  Exit code is the
# number of divergences (0 = clean), so it gates CI directly.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

MINER=//e2e/fuzz:mine_divergences
DUMP=//e2e/fuzz:dump_samples
BIN_MINER=bazel-bin/e2e/fuzz/mine_divergences
BIN_DUMP=bazel-bin/e2e/fuzz/dump_samples

# Every mineable target.  Source of truth: AllTargets() in
# e2e/fuzz/targets.cc (pinned by //e2e/fuzz:targets_test).  Keep this
# list in sync with that test's canonical expectation.
ALL_TARGETS=(
  bool int uint double string bytes
  list_int list_bool list_double list_string
  map_string_int list_list_int map_string_list_int
)

# Kill leftover miners — long sweeps from a previous run compete for
# CPU and skew per-seed timings.  Always run before a fresh sweep.
kill_miners() {
  pkill -9 -f mine_divergences 2>/dev/null || true
}

build_miner() { bazel build "$MINER" >/dev/null 2>&1; }

cmd_validate() {
  echo "fuzz.sh: L1/L2/L3 grammar validation…"
  bazel test //e2e/fuzz:grammar_test //e2e/fuzz:generator_test \
    //e2e/fuzz:compare_test
}

cmd_mine() {
  local target=${1:?usage: fuzz.sh mine <target> [seeds] [depth] [stop_after]}
  local seeds=${2:-500} depth=${3:-6} stop=${4:-10}
  build_miner
  kill_miners
  "$BIN_MINER" "$target" "$seeds" "$depth" "$stop"
}

cmd_sweep() {
  local seeds=${1:-500} depth=${2:-6}
  build_miner
  kill_miners
  local failures=0 t rc
  for t in "${ALL_TARGETS[@]}"; do
    echo "=== sweep: $t (seeds=$seeds depth=$depth) ==="
    # stop_after=1: a sweep wants a fast "is it clean" answer per
    # target, not an exhaustive divergence list.
    if "$BIN_MINER" "$t" "$seeds" "$depth" 1; then :; else
      rc=$?
      echo "  >> $t DIVERGED (exit $rc)"
      failures=$((failures + 1))
    fi
  done
  echo "=== sweep done: $failures/${#ALL_TARGETS[@]} targets diverged ==="
  return "$failures"
}

cmd_repro() {
  local target=${1:?usage: fuzz.sh repro <target> <seed> <depth>}
  local seed=${2:?need seed} depth=${3:?need depth}
  build_miner
  kill_miners
  # Run exactly the single seed: start at `seed`, one iteration, never
  # early-stop.  The miner walks 1..max so we pass max=seed and rely on
  # the DIVERGE/summary print; for a precise single-seed view use the
  # property test's shrinker.  Here we re-run [1..seed] and grep the seed.
  "$BIN_MINER" "$target" "$seed" "$depth" 999999 | \
    grep -A3 "seed=$seed\b" || echo "(no divergence at seed=$seed)"
}

cmd_samples() {
  local target=${1:?usage: fuzz.sh samples <target> <depth> <count>}
  local depth=${2:?need depth} count=${3:?need count}
  bazel build "$DUMP" >/dev/null 2>&1
  "$BIN_DUMP" "$target" "$depth" "$count"
}

case "${1:-}" in
  validate) shift; cmd_validate "$@" ;;
  mine)     shift; cmd_mine "$@" ;;
  sweep)    shift; cmd_sweep "$@" ;;
  repro)    shift; cmd_repro "$@" ;;
  samples)  shift; cmd_samples "$@" ;;
  kill)     kill_miners; echo "fuzz.sh: stray miners killed" ;;
  *)
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
