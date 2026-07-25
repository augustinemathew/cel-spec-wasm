#!/usr/bin/env bash
# Smoke test for the `cel` CLI.  Schema-less coverage — `--proto` /
# `--descriptor_set` paths are exercised by var_parser_test +
# value_format_test against real descriptor pools.
set -eo pipefail

CEL="${TEST_SRCDIR}/_main/tools/cel/cel"
if [[ ! -x "${CEL}" ]]; then
  echo "FAIL: cannot find cel binary at ${CEL}" >&2
  ls -la "${TEST_SRCDIR}/_main/tools/cel/" >&2 || true
  exit 1
fi

fail=0
expect() {
  local label="$1"; shift
  local want="$1"; shift
  local got
  if ! got="$("$@" 2>&1)"; then
    echo "FAIL ${label}: command failed: $*"
    echo "  output: ${got}"
    fail=1
    return
  fi
  if [[ "${got}" != "${want}" ]]; then
    echo "FAIL ${label}: stdout mismatch"
    echo "  cmd:  $*"
    echo "  want: $(printf '%q' "${want}")"
    echo "  got:  $(printf '%q' "${got}")"
    fail=1
  fi
}

expect "scalar literal" "6" \
  "${CEL}" eval "1 + 2 + 3"

expect "scalar + vars" "42" \
  "${CEL}" eval "a * b" --var "a:int=6" --var "b:int=7"

expect "string concat" '"Ada (36)"' \
  "${CEL}" eval 's + " (" + string(n) + ")"' --var 's:string="Ada"' --var "n:int=36"

expect "bool result" "true" \
  "${CEL}" eval "a > 5" --var "a:int=10"

expect "duration cmp" "true" \
  "${CEL}" eval 'd > duration("1s")' --var 'd:duration="2s"'

# --var values containing commas (the CLI-handling concern under
# test).  Run via `check` because celwasm's eval path doesn't
# yet support comprehensions over activation-bound lists or any
# activation-bound map (codebase-level "later milestone" gates) —
# `check` exercises the CLI's argv extraction + value parsing
# without depending on those eval-side features.
expect "list (commas in value) parses" "OK" \
  "${CEL}" check "xs.exists(x, x > 5)" --var "xs:list<int>=[1, 3, 5, 7]"

expect "map (commas in value) parses" "OK" \
  "${CEL}" check 'm["us"]' --var 'm:map<string,int>={"us": 1, "ca": 2}'

# Literal list comprehension + literal map lookup — same shapes,
# but inlined in the source so they exercise the eval path too.
expect "literal list comprehension" "true" \
  "${CEL}" eval "[1, 3, 5, 7].exists(x, x > 5)"

expect "literal map lookup" "1" \
  "${CEL}" eval '{"us": 1, "ca": 2}["us"]'

expect "check ok" "OK" \
  "${CEL}" check "a * b + 1" --var "a:int" --var "b:int"

# Compile to a tempfile and verify the file starts with the wasm magic.
out_wasm="${TEST_TMPDIR:-/tmp}/out.wasm"
"${CEL}" compile "1 + 1" --output "${out_wasm}" >/dev/null 2>&1
if [[ ! -s "${out_wasm}" ]]; then
  echo "FAIL compile: ${out_wasm} not written"
  fail=1
else
  # `od` (coreutils) not `xxd` — xxd ships by default on macOS but is
  # absent from a minimal Linux (e.g. ubuntu container / CI image),
  # where it lives in a separate `xxd`/`vim-common` package.  `od` is
  # always present on both, so the test needs no extra install.
  magic=$(head -c 4 "${out_wasm}" | od -An -tx1 | tr -d ' \n')
  if [[ "${magic}" != "0061736d" ]]; then
    echo "FAIL compile: wasm magic mismatch (got ${magic})"
    fail=1
  fi
fi

# --- Exit-code contract -----------------------------------------------------
# Pins the three codes declared by the kExit* constants in cel.cc and
# documented in tools/cel/README.md:
#   0  success
#   1  the expression or program failed (diagnostics, or a CEL error /
#      unknown result)
#   2  usage (bad subcommand, flag, --var syntax, positional count)
# A CEL error used to print on stdout and exit 0, which made the CLI
# unsafe to branch on from a script; these cases lock that shut.
expect_exit() {
  local label="$1"; shift
  local want="$1"; shift
  local got rc
  got="$("$@" 2>&1)" && rc=0 || rc=$?
  if [[ "${rc}" != "${want}" ]]; then
    echo "FAIL ${label}: want exit ${want}, got ${rc}"
    echo "  cmd:    $*"
    echo "  output: ${got}"
    fail=1
  fi
}

expect_exit "success"                0 "${CEL}" eval "1 + 1"
expect_exit "help"                   0 "${CEL}" --help
expect_exit "help after subcommand"  0 "${CEL}" eval --help

expect_exit "divide by zero"         1 "${CEL}" eval "1 / 0"
expect_exit "modulus by zero"        1 "${CEL}" eval "1 % 0"
expect_exit "no such key"            1 "${CEL}" eval '{"a": 1}["b"]'
expect_exit "index out of range"     1 "${CEL}" eval '[1, 2][5]'
expect_exit "int overflow"           1 "${CEL}" eval "9223372036854775807 + 1"
expect_exit "check type error"       1 "${CEL}" check "1 + \"s\""
expect_exit "eval type error"        1 "${CEL}" eval "a + 1" --var 'a:string="hi"'
expect_exit "compile type error"     1 "${CEL}" compile "1 + \"s\"" --output /dev/null

expect_exit "unknown subcommand"     2 "${CEL}" wibble
expect_exit "unknown flag"           2 "${CEL}" eval "1 + 1" --bogus_flag
expect_exit "malformed --var"        2 "${CEL}" eval "a" --var "a=5"
expect_exit "bad --var value"        2 "${CEL}" eval "a" --var "a:int=notanint"
expect_exit "bad --format"           2 "${CEL}" eval "1 + 1" --format wibble
expect_exit "missing positional"     2 "${CEL}" eval
expect_exit "too many positionals"   2 "${CEL}" eval "1 + 1" "2 + 2"

# A CEL error must go to stderr, leaving stdout empty — otherwise
# `x=$(cel eval ...)` captures the error text as if it were a result.
err_stdout="$("${CEL}" eval "1 / 0" 2>/dev/null || true)"
if [[ -n "${err_stdout}" ]]; then
  echo "FAIL: CEL error leaked to stdout: $(printf '%q' "${err_stdout}")"
  fail=1
fi
# Captured rather than piped: `set -o pipefail` would surface the
# intentional exit 1 as a pipeline failure and mask the real assertion.
err_stderr="$("${CEL}" eval "1 / 0" 2>&1 >/dev/null || true)"
case "${err_stderr}" in
  *divide_by_zero*) ;;
  *)
    echo "FAIL: CEL error not reported on stderr"
    echo "  stderr: $(printf '%q' "${err_stderr}")"
    fail=1
    ;;
esac

if (( fail != 0 )); then
  exit 1
fi
echo "PASS"
