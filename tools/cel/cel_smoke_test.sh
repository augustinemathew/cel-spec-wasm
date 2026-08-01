#!/usr/bin/env bash
# Smoke test for the `cel` CLI.  Covers the schema-less paths plus
# `--descriptor_set` against a real FileDescriptorSet (the
# proto_library by-product declared in this target's `data`);
# `--proto` source loading is exercised by var_parser_test +
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

# Assert a command's exit status.  See the exit-code contract below.
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

# --- compile -> inspect -> run round trip ---------------------------------
# The loop the whole AOT design exists for: compile once, describe the
# artifact, evaluate it later with no recompile.
rt_wasm="${TEST_TMPDIR:-/tmp}/roundtrip.wasm"
"${CEL}" compile "a * b + 1" --var "a:int" --var "b:int" \
    --output "${rt_wasm}" >/dev/null 2>&1

expect "inspect reports declared vars" \
  "vars:       a:int, b:int" \
  bash -c "\"${CEL}\" inspect \"${rt_wasm}\" | head -1"

# A plain expression requires no custom functions; both rows read
# `none` and nothing warns about runnability.
expect "inspect reports no required fns" "plugin fns: none" \
  bash -c "\"${CEL}\" inspect \"${rt_wasm}\" | sed -n 2p"
expect "inspect reports no host fns" "host fns:   none" \
  bash -c "\"${CEL}\" inspect \"${rt_wasm}\" | sed -n 3p"

expect "run evaluates a precompiled program" "43" \
  "${CEL}" run "${rt_wasm}" --var "a=6" --var "b=7"

expect "run accepts the explicit typed form too" "43" \
  "${CEL}" run "${rt_wasm}" --var "a:int=6" --var "b:int=7"

# Aggregates bind by value now that cel.abi carries the full type —
# no `name:Type=value` re-declaration needed.
agg_wasm="${TEST_TMPDIR:-/tmp}/agg.wasm"
"${CEL}" compile "size(xs)" --var "xs:list<int>" \
    --output "${agg_wasm}" >/dev/null 2>&1
expect "inspect shows the full element type" "vars:       xs:list<int>" \
  bash -c "\"${CEL}\" inspect \"${agg_wasm}\" | head -1"
expect "run binds an aggregate by value alone" "3" \
  "${CEL}" run "${agg_wasm}" --var "xs=[1, 2, 3]"

expect_exit "run: undeclared --var"        2 \
  "${CEL}" run "${rt_wasm}" --var "a=6" --var "b=7" --var "nope=1"
expect_exit "run: unbound declared var"    2 "${CEL}" run "${rt_wasm}" --var "a=6"
expect_exit "run: bad value for repr"      2 \
  "${CEL}" run "${rt_wasm}" --var "a=notanint" --var "b=7"
expect_exit "run: missing file"            2 "${CEL}" run /nonexistent.wasm
expect_exit "inspect: missing file"        2 "${CEL}" inspect /nonexistent.wasm
expect_exit "inspect: not a wasm module"   2 "${CEL}" inspect "$0"

# --- --descriptor_set: schema loading from a FileDescriptorSet ------------
# The path is the proto_library's descriptor-set by-product, resolved
# relative to the runfiles root.
FDS="${TEST_SRCDIR}/_main/testdata/e2e_fixture_proto-descriptor-set.proto.bin"
if [[ ! -s "${FDS}" ]]; then
  echo "FAIL: descriptor set not in runfiles at ${FDS}"
  fail=1
else
  # A message-typed variable resolves against the loaded set.
  expect "descriptor_set: message var checks" "OK" \
    "${CEL}" check "c.name" --var "c:celwasm.testdata.Customer" \
      --descriptor_set "${FDS}"
  # A message NOT in the set is still rejected.
  expect_exit "descriptor_set: unknown message rejected" 1 \
    "${CEL}" check "c.name" --var "c:no.such.Message" \
      --descriptor_set "${FDS}"
fi

expect_exit "descriptor_set: missing file"      2 \
  "${CEL}" check "1" --descriptor_set /nonexistent.fds
expect_exit "descriptor_set: not an FDS"        2 \
  "${CEL}" check "1" --descriptor_set "$0"
expect_exit "descriptor_set: with --proto"      2 \
  "${CEL}" check "1" --descriptor_set "${FDS}" --proto testdata/e2e_fixture.proto

# Malformed --var TYPE SPECS — the reject arms of the compiler-side
# spec grammar (parse_and_check.cc TypeParser / ParseVariableSpec):
# unclosed list<>, missing map comma / '>', unknown type name, empty
# element type, trailing garbage, missing name, missing colon.
expect_exit "spec: unclosed list<"          2 \
  "${CEL}" check "1" --var "x:list<int"
expect_exit "spec: list missing <"          2 \
  "${CEL}" check "1" --var "x:list int>"
expect_exit "spec: map missing '<'"         2 \
  "${CEL}" check "1" --var "x:map int>"
expect_exit "spec: map missing comma"       2 \
  "${CEL}" check "1" --var "x:map<int int>"
expect_exit "spec: map unclosed"            2 \
  "${CEL}" check "1" --var "x:map<int, int"
# Unknown type names surface at the check stage (exit 1, a
# diagnostic), not argv parsing (exit 2) — the name only resolves
# against the descriptor pool inside ParseAndCheck.
expect_exit "spec: unknown type name"       1 \
  "${CEL}" check "1" --var "x:no.such.Type"
expect_exit "spec: empty element type"      2 \
  "${CEL}" check "1" --var "x:list<>"
expect_exit "spec: trailing garbage"        2 \
  "${CEL}" check "1" --var "x:int garbage"
expect_exit "spec: empty name"              2 \
  "${CEL}" check "1" --var ":int=1"
expect_exit "spec: missing colon"           2 \
  "${CEL}" check "1" --var "xint"

# `--plugin` is repeatable on `run` but a single absl flag on
# `embed-decls`.  Peeling it out of argv for every subcommand would
# consume embed-decls' value before absl saw it — this pins that the
# extraction stays scoped to `run`.
embed_out="$("${CEL}" embed-decls --plugin "${rt_wasm}" \
  --idl /nonexistent.idl --out /tmp/ignored.wasm 2>&1 || true)"
case "${embed_out}" in
  *"--plugin is required"*)
    echo "FAIL: embed-decls lost its --plugin value to run's extractor"
    fail=1
    ;;
esac

# A CEL error from a precompiled program follows the same contract as
# `eval`: stderr, exit 1.
dz_wasm="${TEST_TMPDIR:-/tmp}/divzero.wasm"
"${CEL}" compile "1 / 0" --output "${dz_wasm}" >/dev/null 2>&1
expect_exit "run: CEL error exits 1"       1 "${CEL}" run "${dz_wasm}"

# --- Plugin flow ------------------------------------------------------------
# A plugin is needed at BOTH compile (its decls resolve the call site)
# and run (its artifact satisfies the import).  Skipped when the demo
# fixture is not in the runfiles — the smoke test must stay runnable
# without building the plugin toolchain.
demo_plugin="${TEST_SRCDIR}/_main/e2e/plugin_fixtures/cel_wasm_plugin_demo/demo_plugin.wasm"
if [[ -f "${demo_plugin}" ]]; then
  expect "eval calls a plugin function" "5" \
    "${CEL}" eval "add(2, 3)" --plugin "${demo_plugin}"

  pl_wasm="${TEST_TMPDIR:-/tmp}/plugin_prog.wasm"
  "${CEL}" compile "add(a, b)" --plugin "${demo_plugin}" \
      --var "a:int" --var "b:int" --output "${pl_wasm}" >/dev/null 2>&1
  expect "run a plugin-backed program" "42" \
    "${CEL}" run "${pl_wasm}" --plugin "${demo_plugin}" \
      --var "a=20" --var "b=22"

  # Omitting --plugin is a usage error that names what is missing,
  # not a wasm link failure.
  expect_exit "run without --plugin" 2 \
    "${CEL}" run "${pl_wasm}" --var "a=1" --var "b=2"
else
  # Loud, not silent: a runfiles change that drops the fixture would
  # otherwise remove this coverage without anyone noticing.
  echo "WARN: demo plugin fixture absent — plugin flow NOT covered" >&2
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
