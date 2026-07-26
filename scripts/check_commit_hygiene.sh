#!/usr/bin/env bash
# check_commit_hygiene.sh — fast, deterministic checks over the staged
# diff.  No network, no model, well under a second.
#
# These are the mechanical half of the commit gate: rules from
# CLAUDE.md that a grep can decide.  The judgement half — "is this doc
# update actually true?" — lives in scripts/review_staged_change.sh.
#
# Exit status:
#   0  no findings, or findings in advisory mode (the default)
#   1  findings AND CEL_HOOK_STRICT=1
#
# Advisory by default on purpose: every rule here has a plausible
# false positive (a refactor that moves a public symbol without
# changing its contract, a comment that legitimately cites a
# milestone doc).  A gate that cries wolf gets bypassed with
# --no-verify and then catches nothing at all.
#
#   scripts/check_commit_hygiene.sh            # staged changes
#   scripts/check_commit_hygiene.sh --range A..B

set -uo pipefail
cd "$(git rev-parse --show-toplevel)"

if [[ "${1:-}" == "--range" && -n "${2:-}" ]]; then
  DIFF_ARGS=("$2")
  NAME_ARGS=("$2")
else
  DIFF_ARGS=(--cached)
  NAME_ARGS=(--cached)
fi

staged_names="$(git diff --name-only "${NAME_ARGS[@]}" -- 2>/dev/null || true)"
[[ -z "${staged_names}" ]] && exit 0

# Added lines only (drop the +++ header), restricted to first-party code.
added_code="$(git diff "${DIFF_ARGS[@]}" -- \
  'compiler/**' 'eval/**' 'shared/**' 'abi/**' 'runtime/**' 'tools/**' \
  2>/dev/null | grep -E '^\+' | grep -vE '^\+\+\+' || true)"

findings=0
note() {
  printf '  • %s\n' "$1"
  findings=$((findings + 1))
}

has() { echo "${staged_names}" | grep -qE "$1"; }

# ---- A. public API changed without a user-facing doc ------------------------
# CLAUDE.md "Docs ship with the change".
PUBLIC_HDRS='^(compiler/(compiler|program)\.h|eval/(engine|instance|activation|value|error|attribute|host_call_context|typed_function)\.h|shared/type\.h|abi/[^/]+\.h|runtime/[^/]+\.h)$'
if has "${PUBLIC_HDRS}"; then
  if ! has '^doc/' && ! has 'README\.md$'; then
    note "public header changed but no doc/ or README staged — see CLAUDE.md
    \"Docs ship with the change\".  Changed: $(echo "${staged_names}" |
      grep -E "${PUBLIC_HDRS}" | tr '\n' ' ')"
  fi
fi

# ---- B. CLI changed without BOTH tellings ----------------------------------
# docs_dir is doc/, so tools/cel/README.md is GitHub-only and
# doc/user-guide/ is the site.  Updating one is not updating the other.
if has '^tools/cel/.*\.(cc|h|sh)$'; then
  has '^tools/cel/README\.md$' || \
    note "tools/cel changed but tools/cel/README.md is not staged (the
    GitHub-facing telling)"
  has '^doc/user-guide/' || \
    note "tools/cel changed but no doc/user-guide/ page is staged (the
    published telling — mkdocs docs_dir is doc/)"
fi

# ---- C. new milestone references in comments -------------------------------
# Grandfathered where they already exist; forbidden in new lines.  The
# sanctioned `... is a stub until <milestone>` CHECK message is exempt.
ms_hits="$(echo "${added_code}" \
  | grep -E '^\+\s*(//|\*|/\*)' \
  | grep -vE 'is a stub until' \
  | grep -EI '\bM[0-9]{1,2}\b|\bSlice [A-Z]\b' || true)"
if [[ -n "${ms_hits}" ]]; then
  note "new comment(s) cite a milestone — CLAUDE.md forbids these in new
    code (cite a design-doc path instead):"
  echo "${ms_hits}" | head -3 | sed 's/^/      /'
fi

# ---- D. ABSL_CHECK(false) with no message ----------------------------------
# Unreachable-default and stub CHECKs must name the offending value or
# the owning milestone; a bare one gives the eventual debugger nothing.
if echo "${added_code}" | grep -qE 'ABSL_CHECK\(false\)\s*;'; then
  note "ABSL_CHECK(false) added with no '<< message' — name the offending
    value or the owning milestone"
fi

# ---- E. wasm binary-format knowledge outside abi/ --------------------------
# feature-pipeline-checklist §2.7: //abi:wasm_binary is THE binary-format
# layer; a magic constant or LEB codec elsewhere is a review finding.
framing="$(git diff "${DIFF_ARGS[@]}" -- \
  'compiler/**' 'eval/**' 'tools/**' 'runtime/**' 2>/dev/null \
  | grep -E '^\+' | grep -vE '^\+\+\+' \
  | grep -EI '0x61,\s*0x73,\s*0x6d|leb128' || true)"
if [[ -n "${framing}" ]]; then
  note "wasm framing knowledge (magic bytes / LEB codec) added outside
    abi/ — belongs in //abi:wasm_binary (feature-pipeline-checklist §2.7)"
fi

# ---- F. new source file without a paired test ------------------------------
new_srcs="$(git diff --name-only --diff-filter=A "${NAME_ARGS[@]}" -- \
  'compiler/**' 'eval/**' 'shared/**' 'abi/**' 'tools/**' 2>/dev/null \
  | grep -E '\.(cc|h)$' | grep -vE '_test\.(cc|h)$|test_.*\.h$' || true)"
for f in ${new_srcs}; do
  base="${f%.*}"
  if ! has "^${base}_test\.cc$"; then
    note "new source ${f} has no paired ${base}_test.cc staged — CLAUDE.md
    \"every individual source file gets its own _test.cc\""
  fi
done

if (( findings == 0 )); then
  exit 0
fi

echo
if [[ "${CEL_HOOK_STRICT:-0}" == "1" ]]; then
  echo "commit-hygiene: ${findings} finding(s), CEL_HOOK_STRICT=1 — blocking." >&2
  exit 1
fi
echo "commit-hygiene: ${findings} finding(s) above (advisory)." >&2
echo "  block on these with CEL_HOOK_STRICT=1; silence with CEL_HOOK_SKIP=1" >&2
exit 0
