#!/usr/bin/env bash
# collect_wasm_gcov.sh — per-workload wasm-side gcov collection.
#
# Runs each given e2e binary (built with `--//runtime:instrument_wasm
# --collect_code_coverage`, see m38-wasm-gcov-coverage.md §4) with its
# own CELWASM_WASM_GCOV_DIR, so every workload gets an attributable
# set of .gcda counters for the runtime/*.c TUs; pairs each set with
# the build's .gcno notes files and decodes it with `llvm-cov gcov`.
#
# Output layout under <out_root>:
#   raw/<workload>/*.gcda        merged counters for that workload
#   raw/<workload>/*.gcno        paired notes (copied from bazel-out)
#   raw/<workload>/*.gcov        intermediate-format records (`gcov -i`:
#                                function:<line>,<count>,<name> +
#                                lcount:<line>,<count>) — the input
#                                wasm_gcov_report.py parses
#   raw/<workload>/summary.txt   per-function/-file percentages (human)
#   raw/<workload>/run.log       the workload's own output
#
# Workloads run SEQUENTIALLY — the sink has no file lock (see
# eval/internal/wasm_gcov.h).
#
# Usage:
#   scripts/coverage/collect_wasm_gcov.sh <out_root> <binary> [...]
#
# Env:
#   LLVM_COV  path to llvm-cov (default: /opt/homebrew/opt/llvm/bin/llvm-cov,
#             falling back to `xcrun llvm-cov`).

set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <out_root> <binary> [<binary>...]" >&2
  exit 2
fi

OUT_ROOT=$1
shift

if [[ -z "${LLVM_COV:-}" ]]; then
  if [[ -x /opt/homebrew/opt/llvm/bin/llvm-cov ]]; then
    LLVM_COV=/opt/homebrew/opt/llvm/bin/llvm-cov
  else
    LLVM_COV="xcrun llvm-cov"
  fi
fi

# The wasm objs dirs are in the ST-suffixed coverage config.  The
# runtime wasm links several cc_library targets, each with its own
# _objs dir — gather EVERY runtime .gcno (anchoring on cel_time.gcno
# to find the config), or decoding silently drops whole TUs
# (cel_base64_ext, cel_math_ext, the string_ext splits, …).
output_path=$(bazel info output_path 2>/dev/null)
gcno_probe=$(find "$output_path" -path '*cel_runtime_wasm.bin*' -name 'cel_time.gcno' 2>/dev/null | head -1)
if [[ -z "$gcno_probe" ]]; then
  echo "ERROR: no cel_time.gcno under $output_path — build with" >&2
  echo "  bazel build --//runtime:instrument_wasm --collect_code_coverage \\" >&2
  echo "      '--instrumentation_filter=^//runtime[/:]' <targets>" >&2
  exit 1
fi
RUNTIME_OBJS_ROOT=${gcno_probe%%/_objs/*}/_objs
echo "gcno root: $RUNTIME_OBJS_ROOT"

# Copy notes for every runtime TU into $1.  Some TUs are compiled by
# more than one target (the `cel_runtime` cc_library AND the
# `cel_runtime_wasm.bin` cc_binary); the .gcda stamps must pair with
# the objects that were LINKED, so the binary's own notes are copied
# last and win any basename collision.
copy_gcno() {
  # `cp -f` throughout: bazel outputs are mode r-x, and a plain cp
  # preserves that — the later, higher-priority copy would then fail
  # (Permission denied) and silently leave the WRONG notes in place.
  find "$RUNTIME_OBJS_ROOT" -name '*.gcno' \
      ! -path '*/_objs/cel_runtime_wasm.bin/*' \
      ! -path '*/_objs/cel_runtime/*' -exec cp -f {} "$1/" \;
  cp -f "$RUNTIME_OBJS_ROOT"/cel_runtime/*.gcno "$1/" 2>/dev/null || true
  cp -f "$RUNTIME_OBJS_ROOT"/cel_runtime_wasm.bin/*.gcno "$1/" 2>/dev/null || true
}

for bin in "$@"; do
  name=$(basename "$bin")
  dir="$OUT_ROOT/raw/$name"
  rm -rf "$dir"
  mkdir -p "$dir"
  echo "=== $name"
  # Workload failures are recorded but don't abort the sweep — a
  # red test still produced counters worth attributing.
  if ! CELWASM_WASM_GCOV_DIR="$dir" "$bin" >"$dir/run.log" 2>&1; then
    echo "WARN: $name exited non-zero (see $dir/run.log)" >&2
  fi
  shopt -s nullglob
  gcda=("$dir"/*.gcda)
  shopt -u nullglob
  if [[ ${#gcda[@]} -eq 0 ]]; then
    echo "WARN: $name produced no .gcda (not instrumented? crashed early?)" >&2
    continue
  fi
  copy_gcno "$dir"
  # Two decodes: `-n -b -f` prints the human per-function/-file summary
  # (no .gcov files); `-i` writes machine-parsable intermediate-format
  # .gcov files for wasm_gcov_report.py.
  (cd "$dir" && $LLVM_COV gcov -n -b -f ./*.gcda >summary.txt 2>gcov.err && \
       $LLVM_COV gcov -i ./*.gcda >>gcov.err 2>&1) || \
    echo "WARN: llvm-cov gcov failed for $name (see $dir/gcov.err)" >&2
done

echo "collected: $(ls "$OUT_ROOT/raw" | wc -l | tr -d ' ') workload dirs under $OUT_ROOT/raw"
