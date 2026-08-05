#!/usr/bin/env bash
# Runs every //examples binary and asserts on a key output line, so
# the examples — and the doc snippets derived from them — cannot
# silently rot.  Mirrors tools/cel/cel_smoke_test.sh's shape.
set -eo pipefail

BIN_DIR="${TEST_SRCDIR}/_main/examples"
fail=0

expect_contains() {
  local label="$1" want="$2" bin="$3"
  local got
  if ! got="$("${BIN_DIR}/${bin}" 2>&1)"; then
    echo "FAIL ${label}: ${bin} exited non-zero"
    echo "  output: ${got}"
    fail=1
    return
  fi
  if [[ "${got}" != *"${want}"* ]]; then
    echo "FAIL ${label}: output mismatch"
    echo "  want substring: $(printf '%q' "${want}")"
    echo "  got:            $(printf '%q' "${got}")"
    fail=1
  fi
}

expect_contains "hello world"      "1 + 2 + 3  =>  6"            01_hello_world
expect_contains "variables true"   "age=25 country=\"US\"  =>  allowed: true"  02_variables
expect_contains "variables false"  "age=15 country=\"US\"  =>  allowed: false" 02_variables
expect_contains "save"             "saved 'price * quantity'"    03_compile_once_run_anywhere
expect_contains "load+eval"        "reloaded and evaluated  =>  210" 03_compile_once_run_anywhere
expect_contains "host fn gold"     "pay 80"                      04_host_functions
expect_contains "host fn silver"   "pay 90"                      04_host_functions
expect_contains "partial shortcut" "kind: bool, allowed: true"   05_partial_eval
expect_contains "partial unknown"  "kind: unknown"               05_partial_eval
expect_contains "proto allow"      "user=alice action=read quantity=10  =>  allowed: true"      06_proto_messages
expect_contains "proto deny"       "user=mallory action=delete quantity=10  =>  allowed: false" 06_proto_messages
expect_contains "compile error"    "compile error:"              07_error_handling
expect_contains "eval error value" "error value:"                07_error_handling
expect_contains "fn plain value"   "quota(\"alice\")  =>  int: 100"     08_function_errors_and_unknowns
expect_contains "fn error value"   "error value: invalid_argument" 08_function_errors_and_unknowns
expect_contains "fn unknown"       "(the unknown was absorbed)"  08_function_errors_and_unknowns
expect_contains "fn status trap"   "Eval failed with status:"    08_function_errors_and_unknowns

if [[ "${fail}" -ne 0 ]]; then
  exit 1
fi
echo "PASS: all examples produced their documented output"
