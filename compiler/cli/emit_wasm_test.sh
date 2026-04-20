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

# 3. Positive case: a string constant also round-trips.  Since M3 slice
#    A, string literals lower to a block that allocates via the runtime
#    and calls `cel_make_string_view`; the eval module declares the
#    runtime imports, and the two-module loader (compiler/host) wires
#    them.  We only check the file here; round-trip execution lives in
#    //compiler/host:host_loader_test and //compiler/e2e:eval_test.
OUT3="${TEST_TMPDIR}/string.wasm"
"${CLI}" -e '"hi"' --emit_wasm="${OUT3}" > /dev/null
[[ -s "${OUT3}" ]] || die "string .wasm is empty"
MAGIC3=$(xxd -p -l 8 "${OUT3}")
[[ "${MAGIC3}" == "0061736d01000000" ]] || die "string bad magic: ${MAGIC3}"

# 4. Negative case: a list expression is still outside the MVP subset.
#    Codegen should fail with a diagnostic and exit non-zero; no output
#    file should be produced.
OUT4="${TEST_TMPDIR}/list.wasm"
set +e
"${CLI}" -e '[1, 2, 3]' --emit_wasm="${OUT4}" > "${TEST_TMPDIR}/list_stdout" \
                                               2> "${TEST_TMPDIR}/list_stderr"
STATUS=$?
set -e
[[ ${STATUS} -ne 0 ]] || die "list expr should have failed"
[[ ! -s "${OUT4}" ]] || die "list expr should not produce output"
grep -q "codegen error" "${TEST_TMPDIR}/list_stderr" \
  || die "missing 'codegen error' diagnostic in stderr"

# 5. Positive case: `--schema <file.proto>` feeds a textual proto source
#    into the checker.  Uses the shared `e2e_fixture.proto` that lives
#    under `compiler/testdata/`.  We run in `--check` mode only so this
#    test stays a flag-plumbing check rather than a full codegen run
#    (codegen of `c.name` is covered end-to-end by
#    `//compiler/e2e:eval_test`).
PROTO_SRC="${TEST_SRCDIR}/_main/compiler/testdata/e2e_fixture.proto"
[[ -r "${PROTO_SRC}" ]] || die "missing proto fixture at ${PROTO_SRC}"
"${CLI}" -e 'c.name' --check \
  --schema="${PROTO_SRC}" \
  --var='c:celwasm.testdata.Customer' \
  > "${TEST_TMPDIR}/schema_stdout" 2>&1 \
  || die "--schema failed: $(cat "${TEST_TMPDIR}/schema_stdout")"
grep -q 'string' "${TEST_TMPDIR}/schema_stdout" \
  || die "--schema: expected string annotation, got $(cat "${TEST_TMPDIR}/schema_stdout")"

# 6. Negative case: passing both `--schema` and `--schema-descriptorset`
#    should fail fast with an InvalidArgument error.
set +e
"${CLI}" -e 'c.name' --check \
  --schema="${PROTO_SRC}" \
  --schema_descriptorset="${PROTO_SRC}" \
  --var='c:celwasm.testdata.Customer' \
  > "${TEST_TMPDIR}/both_stdout" 2> "${TEST_TMPDIR}/both_stderr"
BOTH_STATUS=$?
set -e
[[ ${BOTH_STATUS} -ne 0 ]] || die "dual-schema should have failed"
grep -q 'at most one of' "${TEST_TMPDIR}/both_stderr" \
  || die "dual-schema diagnostic missing: $(cat "${TEST_TMPDIR}/both_stderr")"

echo "PASS"
