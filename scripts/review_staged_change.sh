#!/usr/bin/env bash
# review_staged_change.sh — ask an agent whether the staged change
# leaves the repo's own claims true.
#
# This is the judgement half of the commit gate; the mechanical half
# (greps over the diff) is scripts/check_commit_hygiene.sh.  The
# questions here are the ones a grep cannot decide: is the doc update
# accurate, or merely present?  Did this make a published sentence
# false somewhere the diff doesn't touch?
#
# It exists because the failure it targets is real and recurring: a
# 2026-07-25 audit found `mem_size_bytes` documented as an arena knob
# in four places when it had no effect at all, a `BindLazy` contract
# promising behaviour the eager marshal could not deliver, and a
# PROPOSALS.md claiming the repo had no CI months after CI landed.
# Each shipped because code moved and prose did not.
#
# NEVER blocks a commit.  It needs the network and a model; making a
# commit depend on either is how a gate gets bypassed permanently.
# Findings print, and you decide.
#
#   CEL_COMMIT_REVIEW=0   skip entirely
#   CEL_REVIEW_TIMEOUT=N  seconds to wait (default 120)
#
#   scripts/review_staged_change.sh            # staged changes
#   scripts/review_staged_change.sh --range A..B

set -uo pipefail
cd "$(git rev-parse --show-toplevel)"

[[ "${CEL_COMMIT_REVIEW:-1}" == "0" ]] && exit 0
command -v claude >/dev/null 2>&1 || exit 0

if [[ "${1:-}" == "--range" && -n "${2:-}" ]]; then
  diff_text="$(git diff "$2" 2>/dev/null || true)"
  names="$(git diff --name-only "$2" 2>/dev/null || true)"
else
  diff_text="$(git diff --cached 2>/dev/null || true)"
  names="$(git diff --cached --name-only 2>/dev/null || true)"
fi
[[ -z "${diff_text}" ]] && exit 0

# A diff too large to read carefully is one the model will skim; say so
# rather than pretend it was reviewed.
lines=$(printf '%s\n' "${diff_text}" | wc -l | tr -d ' ')
if (( lines > 4000 )); then
  echo "commit-review: diff is ${lines} lines — too large to review" >&2
  echo "  reliably; run scripts/review_staged_change.sh by hand if wanted." >&2
  exit 0
fi

secs="${CEL_REVIEW_TIMEOUT:-90}"

# Portable timeout.  macOS ships no `timeout(1)` — relying on it meant
# the bound silently did not apply and a hook call ran for three
# minutes, which is precisely the behaviour this guard exists to
# prevent.  Poll for the child instead of trusting a binary to exist.
run_bounded() {
  local limit="$1" out_file="$2"
  shift 2
  "$@" >"${out_file}" 2>/dev/null &
  local pid=$! waited=0
  while kill -0 "${pid}" 2>/dev/null; do
    if (( waited >= limit )); then
      kill -TERM "${pid}" 2>/dev/null
      sleep 1
      kill -KILL "${pid}" 2>/dev/null
      return 124
    fi
    sleep 1
    waited=$(( waited + 1 ))
  done
  wait "${pid}" 2>/dev/null
}

read -r -d '' prompt <<'PROMPT'
You are reviewing a staged git change in the celwasmc repo (a CEL →
WebAssembly AOT compiler) for ONE thing only: does this change leave
the repository's own claims true?

You are NOT looking for bugs, style, or design opinions. Report only:

1. A claim this change makes false and does not fix — a comment, a
   header contract, a doc page, a README table, a status line. Include
   claims in files the diff does NOT touch, if the diff falsifies them.
2. A public-API or CLI change whose user-facing docs were not updated
   in the same change. There are two separate tellings and both count:
   `doc/**` is the published site (mkdocs docs_dir is doc/), while
   `tools/cel/README.md` and the root `README.md` are GitHub-only.
3. Something shipped here that a tracking doc still lists as open —
   PROPOSALS.md, doc/implementation-plan/cleanup-backlog.md,
   CLEANUP_PLAN.md, or a milestone doc's status header.
4. A promise the code cannot keep — a documented contract the
   implementation does not actually provide.

Rules: be specific, cite file:line, and prefer silence to speculation.
If you are unsure whether something is wrong, leave it out. If the
change is self-consistent, reply with exactly: CLEAN

Keep the whole reply under 200 words. No preamble.
PROMPT

payload="$(printf '%s\n\nFiles changed:\n%s\n\nDiff:\n%s\n' \
  "${prompt}" "${names}" "${diff_text}")"

echo "commit-review: asking an agent to check the change against the repo's claims… (${secs}s max)" >&2
tmp_out="$(mktemp -t celreview)"
trap 'rm -f "${tmp_out}"' EXIT
rc=0
run_bounded "${secs}" "${tmp_out}" claude -p "${payload}" || rc=$?
out="$(cat "${tmp_out}" 2>/dev/null || true)"

if (( rc == 124 )); then
  echo "commit-review: exceeded ${secs}s — skipped (raise CEL_REVIEW_TIMEOUT)." >&2
  exit 0
fi
if [[ -z "${out}" ]]; then
  echo "commit-review: no response (offline or not logged in) — skipped." >&2
  exit 0
fi
# The CLI reports auth/quota problems on stdout with a success status,
# so an unguarded read prints "401 API key is invalid" as though it were
# a review finding.  Recognise failure text as failure.
case "${out}" in
  *"API Error"* | *"Failed to authenticate"* | *"Invalid API key"* | \
  *"credit balance"* | *"Please run /login"* | *"Usage limit"*)
    printf 'commit-review: agent unavailable (%s) — skipped.\n' \
      "$(printf '%s' "${out}" | head -1 | cut -c1-60)" >&2
    exit 0
    ;;
esac
if [[ "${out}" == *CLEAN* && ${#out} -lt 40 ]]; then
  echo "commit-review: clean." >&2
  exit 0
fi

echo >&2
echo "commit-review findings (advisory — the commit still proceeds):" >&2
printf '%s\n' "${out}" | sed 's/^/  /' >&2
echo >&2
exit 0
