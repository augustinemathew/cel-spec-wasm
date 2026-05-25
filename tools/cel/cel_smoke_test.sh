#!/usr/bin/env bash
# Smoke test for the `cel` CLI.  Schema-less coverage — `--proto` /
# `--descriptor_set` paths are exercised by var_parser_test +
# value_format_test against real descriptor pools.
set -eo pipefail

CEL="${TEST_SRCDIR}/_main/compiler_v2/tools/cel/cel"
if [[ ! -x "${CEL}" ]]; then
  echo "FAIL: cannot find cel binary at ${CEL}" >&2
  ls -la "${TEST_SRCDIR}/_main/compiler_v2/tools/cel/" >&2 || true
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
# test).  Run via `check` because compiler_v2's eval path doesn't
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

# Unknown subcommand → non-zero, usage on stderr.
if "${CEL}" wibble 2>/dev/null; then
  echo "FAIL: unknown subcommand should exit nonzero"
  fail=1
fi

# Type mismatch in eval should produce an ERROR on stderr + nonzero exit.
if out=$("${CEL}" eval "a + 1" --var "a:string=\"hi\"" 2>&1); then
  echo "FAIL: type error should exit nonzero (got: ${out})"
  fail=1
fi

if (( fail != 0 )); then
  exit 1
fi
echo "PASS"
