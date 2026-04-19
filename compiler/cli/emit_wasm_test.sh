#!/bin/bash
# Shell-level test for `celwasmc --emit_wasm`.  We cannot meaningfully
# unit-test the CLI's flag-parsing wrapper (it's a thin shim around
# absl::ParseCommandLine), so this test runs the binary and inspects
# the bytes it writes.  The fact that the .wasm validates and runs is
# covered exhaustively by //compiler/e2e:eval_test; this test only
# verifies that the CLI plumbing — file open, serialize, write —
# reaches the expected output on disk.

set -euo pipefail

CLI="${TEST_SRCDIR}/_main/compiler/cli/celwasmc"
OUT="${TEST_TMPDIR}/out.wasm"

die() { echo "FAIL: $*" >&2; exit 1; }

# 1. Positive case: a plain integer expression produces a .wasm file.
"${CLI}" -e "1 + 2 * 3" --emit_wasm="${OUT}" > "${TEST_TMPDIR}/stdout" 2>&1
[[ -s "${OUT}" ]] || die "output .wasm is empty"

# Magic bytes: \0asm then version 1.
MAGIC=$(xxd -p -l 8 "${OUT}")
[[ "${MAGIC}" == "0061736d01000000" ]] || die "bad magic: ${MAGIC}"

# Stdout should mention the path we wrote.
grep -qF "${OUT}" "${TEST_TMPDIR}/stdout" || die "stdout missing path"

# 2. Positive case: a boolean expression also round-trips.
OUT2="${TEST_TMPDIR}/bool.wasm"
"${CLI}" -e "true && false" --emit_wasm="${OUT2}" > /dev/null
[[ -s "${OUT2}" ]] || die "bool .wasm is empty"
MAGIC2=$(xxd -p -l 8 "${OUT2}")
[[ "${MAGIC2}" == "0061736d01000000" ]] || die "bool bad magic: ${MAGIC2}"

# 3. Negative case: a string constant is outside the M2 MVP subset.
#    Codegen should fail with a diagnostic and exit non-zero; no
#    output file should be produced.
OUT3="${TEST_TMPDIR}/string.wasm"
set +e
"${CLI}" -e '"hi"' --emit_wasm="${OUT3}" > "${TEST_TMPDIR}/str_stdout" \
                                          2> "${TEST_TMPDIR}/str_stderr"
STATUS=$?
set -e
[[ ${STATUS} -ne 0 ]] || die "string expr should have failed"
[[ ! -s "${OUT3}" ]] || die "string expr should not produce output"
grep -q "codegen error" "${TEST_TMPDIR}/str_stderr" \
  || die "missing 'codegen error' diagnostic in stderr"

echo "PASS"
