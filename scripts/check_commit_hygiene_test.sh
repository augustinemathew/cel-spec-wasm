#!/usr/bin/env bash
# Regression test for scripts/check_commit_hygiene.sh.
#
# A gate that stops firing is worse than no gate: it reads as "clean"
# forever.  Each case below pins one rule against a real commit in this
# repo's history, so a refactor that breaks the greps fails here.
set -uo pipefail
cd "$(git rev-parse --show-toplevel)"

fail=0
# `want_hit`: the range must produce a finding.  `want_clean`: it must not.
want_hit() {
  local label="$1" range="$2" needle="$3"
  local out
  out="$(scripts/check_commit_hygiene.sh --range "${range}" 2>&1)"
  if ! printf '%s' "${out}" | grep -qF "${needle}"; then
    echo "FAIL ${label}: expected a finding matching '${needle}'"
    echo "  got: ${out:-<empty>}"
    fail=1
  fi
}
want_clean() {
  local label="$1" range="$2"
  local out
  out="$(scripts/check_commit_hygiene.sh --range "${range}" 2>&1)"
  if [[ -n "${out}" ]]; then
    echo "FAIL ${label}: expected no findings, got:"
    echo "  ${out}"
    fail=1
  fi
}

# 07d15d0 added CompilerOptions::link_mode — a public header, no docs.
want_hit "public header without docs" "07d15d0~1..07d15d0" "public header changed"

# a5ee99a changed tools/cel AND shipped both tellings + docs: must be quiet.
want_clean "CLI change with both tellings" "a5ee99a~1..a5ee99a"

# c5483d1 deleted mem_size_bytes across headers and docs together.
want_clean "public API change with docs" "c5483d1~1..c5483d1"

# Strict mode turns a finding into a non-zero exit.
if CEL_HOOK_STRICT=1 scripts/check_commit_hygiene.sh \
     --range 07d15d0~1..07d15d0 >/dev/null 2>&1; then
  echo "FAIL strict mode: expected non-zero exit on a finding"
  fail=1
fi

# Advisory mode must never block, even when it finds something.
if ! scripts/check_commit_hygiene.sh \
     --range 07d15d0~1..07d15d0 >/dev/null 2>&1; then
  echo "FAIL advisory mode: expected exit 0 despite a finding"
  fail=1
fi

(( fail != 0 )) && exit 1
echo "PASS"
