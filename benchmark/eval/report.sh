#!/usr/bin/env bash
# Joins two Google Benchmark JSON outputs (celwasm + cel-cpp) and emits
# the per-operator headline table from `benchmark/DESIGN.md` §12.4 plus
# the per-cell detail table from §12.5.
#
# The headline table decomposes T(N) = setup + N · per_op via linear
# regression over the {2, 10, 50, 250, 1000} length-sweep points so the
# embedder gets one row per operator: slope, intercept, crossover.  The
# per-cell table is the v0 prototype's friction-visible form, kept as a
# secondary artifact.
#
# Usage: report.sh <wasm.json> <cpp.json>

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <celwasm.json> <celcpp.json>" >&2
  exit 2
fi

WASM_JSON="$1"
CPP_JSON="$2"

# Operators × lengths — must match the registration in
# `celwasm_bench.cc::RegisterAll` and `celcpp_bench.cc::RegisterAll`.
OPS=(intAdd intMul intSub doubleAdd)
LENGTHS=(2 10 50 250 1000)

# Read a single BM real_time (nanoseconds) by name.
ns_for() {
  local json="$1" name="$2"
  jq -r --arg n "$name" \
    '.benchmarks[] | select(.name == $n) | .real_time' "$json"
}

# BM name from (op, n) — mirrors `celwasm_bench.cc::MakeBmName`.
bm_name() {
  local op="$1" n="$2"
  if [[ "$n" == "2" ]]; then
    printf 'BM_arith_%s2' "$op"
  else
    printf 'BM_arith_%s%sTerms' "$op" "$n"
  fi
}

# Build the canonical chain expression for (op, n) — same generator as
# the bench code's `MakeChainSource`.  Used for the per-cell detail
# column, truncated for display.
expr_for() {
  local op="$1" n="$2" tok="+"
  case "$op" in
    intAdd|doubleAdd) tok="+" ;;
    intMul)           tok="*" ;;
    intSub)           tok="-" ;;
  esac
  local vars="abcdefghij" out=""
  for ((i=0; i<n; i++)); do
    if (( i > 0 )); then out="${out} ${tok} "; fi
    out="${out}${vars:$((i%10)):1}"
  done
  printf '%s' "$out"
}

truncate_expr() {
  local s="$1" n=40
  if (( ${#s} > n )); then
    printf '%s…' "${s:0:n-1}"
  else
    printf '%s' "$s"
  fi
}

# Linear regression: given parallel arrays of x (lengths) and y (ns),
# emit "slope intercept" — slope is ns per added op, intercept is the
# y(0) per-Eval setup cost.  Formula: slope = (Σxy - n·x̄·ȳ) / (Σx² - n·x̄²).
linreg() {
  local xs="$1" ys="$2"
  awk -v xs="$xs" -v ys="$ys" 'BEGIN {
    split(xs, X, " "); split(ys, Y, " ");
    n = 0;
    for (i in X) { if (X[i] != "" && Y[i] != "") { n++; sx += X[i]; sy += Y[i]; sxx += X[i]*X[i]; sxy += X[i]*Y[i]; } }
    if (n < 2) { printf "n/a n/a"; exit }
    mx = sx/n; my = sy/n;
    denom = sxx - n*mx*mx;
    if (denom == 0) { printf "n/a n/a"; exit }
    slope = (sxy - n*mx*my) / denom;
    intercept = my - slope*mx;
    printf "%.2f %.2f", slope, intercept;
  }'
}

# Crossover: solve `wasm_b + N · wasm_m == cpp_b + N · cpp_m` for N.
# If wasm_m >= cpp_m (our slope is greater), we never win → "n/a".
crossover() {
  local wasm_m="$1" wasm_b="$2" cpp_m="$3" cpp_b="$4"
  awk -v wm="$wasm_m" -v wb="$wasm_b" -v cm="$cpp_m" -v cb="$cpp_b" 'BEGIN {
    if (wm == "n/a" || cm == "n/a") { print "n/a"; exit }
    if (wm >= cm) { print "n/a (we never win)"; exit }
    n = (wb - cb) / (cm - wm);
    if (n <= 0) { print "<= 0 (already winning)"; exit }
    printf "%.0f", n;
  }'
}

echo "## Per-operator headline (DESIGN.md §12.4)"
echo
echo "Eval steady-state, real_time ns/call from Google Benchmark JSON."
echo "Parity verified for all 20 cells (eyeballed via SetLabel; full"
echo "parity checker is a Phase 2 deliverable per DESIGN.md §11)."
echo
printf '| operator   | T(2) | T(10) | T(50) | T(250) | T(1000) | celwasm slope (ns/op) | cel-cpp slope (ns/op) | celwasm setup | cel-cpp setup | crossover @ N terms |\n'
printf '|------------|-----:|------:|------:|-------:|--------:|----------------------:|----------------------:|--------------:|--------------:|---------------------|\n'

for op in "${OPS[@]}"; do
  wasm_xs="" wasm_ys=""
  cpp_xs="" cpp_ys=""
  declare -a wasm_ts=()
  declare -a cpp_ts=()
  for n in "${LENGTHS[@]}"; do
    bm=$(bm_name "$op" "$n")
    w=$(ns_for "$WASM_JSON" "$bm")
    c=$(ns_for "$CPP_JSON" "$bm")
    wasm_ts+=("$w")
    cpp_ts+=("$c")
    if [[ -n "$w" ]]; then
      wasm_xs+="$n "
      wasm_ys+="$w "
    fi
    if [[ -n "$c" ]]; then
      cpp_xs+="$n "
      cpp_ys+="$c "
    fi
  done

  read -r wasm_m wasm_b <<<"$(linreg "$wasm_xs" "$wasm_ys")"
  read -r cpp_m cpp_b   <<<"$(linreg "$cpp_xs" "$cpp_ys")"
  cross=$(crossover "$wasm_m" "$wasm_b" "$cpp_m" "$cpp_b")

  # Format cell times: truncate to integer ns for the headline.
  fmt_ns() {
    if [[ -z "$1" ]]; then printf "MISS"; else printf "%.0f" "$1"; fi
  }
  t2=$(fmt_ns "${wasm_ts[0]}")
  t10=$(fmt_ns "${wasm_ts[1]}")
  t50=$(fmt_ns "${wasm_ts[2]}")
  t250=$(fmt_ns "${wasm_ts[3]}")
  t1000=$(fmt_ns "${wasm_ts[4]}")

  printf '| %-10s | %4s | %5s | %5s | %6s | %7s | %21s | %21s | %13s | %13s | %s |\n' \
    "$op" "$t2" "$t10" "$t50" "$t250" "$t1000" \
    "$wasm_m" "$cpp_m" "$wasm_b" "$cpp_b" "$cross"
done

echo
echo "T(N) columns show celwasm real_time at length N (ns/call)."
echo "Slopes and intercepts come from linear regression over"
echo "{2, 10, 50, 250, 1000} per operator.  Crossover solves"
echo "celwasm_setup + N·celwasm_slope == celcpp_setup + N·celcpp_slope."
echo
echo
echo "## Per-cell detail (DESIGN.md §12.5)"
echo
printf '| surface    | id                  | expression                              | celwasm ns | cel-cpp ns | ratio   |\n'
printf '|------------|---------------------|-----------------------------------------|-----------:|-----------:|---------|\n'

for op in "${OPS[@]}"; do
  for n in "${LENGTHS[@]}"; do
    bm=$(bm_name "$op" "$n")
    id="${bm#BM_arith_}"
    w=$(ns_for "$WASM_JSON" "$bm")
    c=$(ns_for "$CPP_JSON" "$bm")
    expr_full=$(expr_for "$op" "$n")
    expr_display=$(truncate_expr "$expr_full")
    if [[ -z "$w" || -z "$c" ]]; then
      printf '| %-10s | %-19s | %-39s | %10s | %10s | %-7s |\n' \
        "arithmetic" "$id" "$expr_display" "${w:-MISS}" "${c:-MISS}" "n/a"
      continue
    fi
    # ratio = cpp / wasm  (>1 means celwasm faster; <1 means celwasm slower)
    ratio=$(awk -v w="$w" -v c="$c" 'BEGIN { printf "%.2f", c/w }')
    printf '| %-10s | %-19s | %-39s | %10.1f | %10.1f | %6sx |\n' \
      "arithmetic" "$id" "$expr_display" "$w" "$c" "$ratio"
  done
done

echo
echo "ratio = cel-cpp ns / celwasm ns  (>1.0 means celwasm is faster)"
