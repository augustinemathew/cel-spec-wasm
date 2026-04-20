#!/bin/bash
# Shell-level test for `celwasmc-eval`.  The deep coverage of every
# Repr / 3VL / partial-eval path lives in //compiler/e2e:eval_test; this
# test only verifies the CLI plumbing reaches the expected stdout.
#
# Platform note: like every other //compiler/host:host_loader consumer,
# this target (and therefore this test) is darwin-arm64-only today.

set -euo pipefail

CLI="${TEST_SRCDIR}/_main/compiler/cli/celwasmc-eval"
PROTO_SRC="${TEST_SRCDIR}/_main/compiler/testdata/e2e_fixture.proto"

die() { echo "FAIL: $*" >&2; exit 1; }

assert_stdout() {
  local expected=$1 actual=$2
  if [[ "${actual}" != "${expected}" ]]; then
    die "stdout mismatch: expected '${expected}', got '${actual}'"
  fi
}

# 1. Nullary scalar: int expression prints its i64 value.
OUT=$("${CLI}" -e "1 + 2 * 3")
assert_stdout "7" "${OUT}"

# 2. Nullary scalar: double expression prints its f64 value.
OUT=$("${CLI}" -e "1.5 + 2.5")
assert_stdout "4" "${OUT}"

# 3. Nullary bool: `true && false` — the 3VL bool lives as an arena
#    offset, so CallNullaryEval's DecodeSlot returns the unboxed i32 and
#    the CLI prints `0` (false) or `1` (true).  Today `true && false`
#    prints `0`.
OUT=$("${CLI}" -e "true && false")
assert_stdout "0" "${OUT}"

OUT=$("${CLI}" -e "true || false")
assert_stdout "1" "${OUT}"

# 4. Partial-eval (the whole point of this slice).  `c.age` is int, and
#    when `c.age` is a host-marked UNKNOWN attribute, `get_field` writes
#    CEL_UNKNOWN into the sret slot; DecodeSlot surfaces this as a
#    non-OK Status whose message contains "UNKNOWN", and the CLI prints
#    `unknown` (with a zero exit — unknown is a valid CEL result).
OUT=$("${CLI}" -e "c.age" \
  --schema="${PROTO_SRC}" \
  --var='c:celwasm.testdata.Customer' \
  --unknown_attrs='c.age')
assert_stdout "unknown" "${OUT}"

# 5. Partial-eval, wildcard pattern: `c.*` is a FULL match for `c.age`
#    (pattern length 2 ≤ attr length 2, all shared qualifiers match via
#    the wildcard).
OUT=$("${CLI}" -e "c.age" \
  --schema="${PROTO_SRC}" \
  --var='c:celwasm.testdata.Customer' \
  --unknown_attrs='c.*')
assert_stdout "unknown" "${OUT}"

# 6. Partial-eval, multiple patterns: first one FULL-matches, wins.
OUT=$("${CLI}" -e "c.age" \
  --schema="${PROTO_SRC}" \
  --var='c:celwasm.testdata.Customer' \
  --unknown_attrs='other.v,c.age')
assert_stdout "unknown" "${OUT}"

# 7. Negative: malformed pattern (empty variable, leading dot) rejected
#    before eval, with a diagnostic that mentions --unknown_attrs.
set +e
"${CLI}" -e "c.age" \
  --schema="${PROTO_SRC}" \
  --var='c:celwasm.testdata.Customer' \
  --unknown_attrs='.leading_dot' \
  > "${TEST_TMPDIR}/err_stdout" 2> "${TEST_TMPDIR}/err_stderr"
STATUS=$?
set -e
[[ ${STATUS} -ne 0 ]] || die "malformed pattern should have failed"
grep -q '\-\-unknown_attrs' "${TEST_TMPDIR}/err_stderr" \
  || die "error should mention --unknown_attrs, got $(cat "${TEST_TMPDIR}/err_stderr")"

# 8. No -e flag → usage on stderr, non-zero exit.
set +e
"${CLI}" > "${TEST_TMPDIR}/usage_stdout" 2> "${TEST_TMPDIR}/usage_stderr"
STATUS=$?
set -e
[[ ${STATUS} -ne 0 ]] || die "missing -e should have failed"
grep -q 'usage:' "${TEST_TMPDIR}/usage_stderr" \
  || die "usage line missing from stderr"

echo "PASS"
