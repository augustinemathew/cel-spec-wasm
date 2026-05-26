#!/usr/bin/env bash
# regen_conformance_readme.sh — regenerate the auto-managed tables
# in conformance/README.md from a live run_conformance
# invocation.
#
# Two tables are auto-managed (between AUTOGEN markers):
#   1. The per-fixture inventory (pass / skip / fail / pass% +
#      skip-by-category) — fully regenerated.
#   2. The corpus-wide SKIP-totals (Count column only) — Count is
#      regenerated, the Disposition prose column is hand-maintained
#      and preserved across runs.
#
# Usage:
#   scripts/regen_conformance_readme.sh                # rewrite README
#   scripts/regen_conformance_readme.sh --check        # exit 1 if README
#                                                      # would change
#   scripts/regen_conformance_readme.sh --from-log F   # parse F instead
#                                                      # of running bazel
#                                                      # (lets the hook
#                                                      # share a run with
#                                                      # check_conformance
#                                                      # _monotonic.sh)
#
# Designed for the .githooks/pre-push gate (--check) and for
# author-side local fixup (no flag).

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

README="conformance/README.md"
MODE="rewrite"
FROM_LOG=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check) MODE="check" ;;
    --from-log) shift; FROM_LOG="${1:-}" ;;
    --help|-h) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

if [[ ! -f "$README" ]]; then
  echo "$README not found (run from repo root)" >&2
  exit 2
fi

if [[ -n "$FROM_LOG" ]]; then
  if [[ ! -f "$FROM_LOG" ]]; then
    echo "--from-log file not found: $FROM_LOG" >&2
    exit 2
  fi
  TMPOUT="$FROM_LOG"
  trap '' EXIT
else
  if ! command -v bazel >/dev/null 2>&1; then
    echo "bazel not on PATH — required to run the conformance binary" >&2
    exit 2
  fi
  # Run conformance.  -c opt is ~30s warm; cold builds can take
  # 5-10 minutes due to the wasm runtime.
  TMPOUT="$(mktemp -t conformance.XXXXXX)"
  trap 'rm -f "$TMPOUT"' EXIT
  echo "==> Running conformance (slow on a cold bazel cache)..." >&2
  bazel run -c opt //conformance:run_conformance \
      --ui_event_filters=-info,-stdout,-stderr --noshow_progress \
      > "$TMPOUT" 2>/dev/null
fi

# Parse per-fixture blocks.  Each block looks like:
#   <2 spaces>spec/tests/simple/testdata/PATH.textproto
#   <4 spaces>total=N  pass=N  skip=N  fail=N
#   <4 spaces>skip-by-category: cat1=N cat2=N    (omitted when skip=0)
PER_FIXTURE="$(awk '
  /^  spec\/tests\/simple\/testdata\// {
    # Strip leading 2 spaces + the testdata prefix, keep basename.
    n = split($0, a, "/")
    fixture = a[n]
    sub(/^[ \t]+/, "", fixture)
    sub(/[ \t]+$/, "", fixture)
    next
  }
  /^    total=/ {
    total = pass = skip = fail = 0
    for (i = 1; i <= NF; i++) {
      if (split($i, kv, "=") == 2) {
        if (kv[1] == "total") total = kv[2]
        else if (kv[1] == "pass") pass = kv[2]
        else if (kv[1] == "skip") skip = kv[2]
        else if (kv[1] == "fail") fail = kv[2]
      }
    }
    skipcat = ""
    next
  }
  /^    skip-by-category:/ {
    # Everything after the colon, trimmed.
    sub(/^    skip-by-category:[ \t]*/, "")
    skipcat = $0
    # Emit one record per fixture.
    pct = (total > 0) ? int((pass * 100) / total) : -1
    printf "%s\t%d\t%d\t%d\t%d\t%d\t%s\n", fixture, total, pass, skip, fail, pct, skipcat
    fixture = ""
    next
  }
  # Emit a record for fixtures with no skip-by-category line (skip=0).
  /^  spec\/tests\/simple\/testdata\// && fixture != "" {
    # Should not happen — previous block already emitted on skipcat
    # arrival.  But just in case, flush.
  }
  /^summary:/ {
    if (fixture != "") {
      pct = (total > 0) ? int((pass * 100) / total) : -1
      printf "%s\t%d\t%d\t%d\t%d\t%d\t\n", fixture, total, pass, skip, fail, pct
    }
  }
' "$TMPOUT")"

# The awk above misses fixtures whose total>0 but skip=0 (no
# skip-by-category line follows).  Re-pass to flush those: a
# fixture line followed directly by a "total=" then by another
# fixture line (no skip-by-category in between).
#
# Simpler second pass: emit per fixture from the raw output.
PER_FIXTURE="$(awk '
  function flush() {
    if (fixture == "") return
    pct = (total > 0) ? int((pass * 100) / total) : -1
    printf "%s\t%d\t%d\t%d\t%d\t%d\t%s\n", fixture, total, pass, skip, fail, pct, skipcat
    fixture = ""; total = 0; pass = 0; skip = 0; fail = 0; skipcat = ""
  }
  # The conformance report can appear twice in the captured stream
  # (it reaches both stdout and stderr; the pre-push log captures
  # 2>&1).  The first `summary:` line ends the first run`s per-fixture
  # section — stop there so fixtures are not emitted twice.
  done { next }
  /^  spec\/tests\/simple\/testdata\// {
    flush()
    n = split($0, a, "/")
    fixture = a[n]
    sub(/[ \t]+$/, "", fixture)
    next
  }
  /^    total=/ {
    for (i = 1; i <= NF; i++) {
      if (split($i, kv, "=") == 2) {
        if (kv[1] == "total") total = kv[2]
        else if (kv[1] == "pass") pass = kv[2]
        else if (kv[1] == "skip") skip = kv[2]
        else if (kv[1] == "fail") fail = kv[2]
      }
    }
    next
  }
  /^    skip-by-category:/ {
    line = $0
    sub(/^    skip-by-category:[ \t]*/, "", line)
    skipcat = line
    next
  }
  /^summary:/ { flush(); done = 1 }
  /^skip-by-category \(corpus-wide\):/ { flush() }
' "$TMPOUT")"

# Parse the corpus-wide skip-by-category block.  Format:
#   skip-by-category (corpus-wide):
#     cat = N
#     cat = N
# The runner may emit this identical block more than once per run;
# `captured` guards so we read only the first occurrence — otherwise
# every category row duplicates and the per-key arithmetic below
# (DISABLE_CHECK / STATIC_SUBSET) sees a multi-line value.
CORPUS_SKIP="$(awk '
  /^skip-by-category \(corpus-wide\):/ { if (captured) next; in_block = 1; next }
  in_block && /^  / {
    line = $0
    sub(/^  /, "", line)
    # "cat = N"
    if (split(line, a, " = ") == 2) {
      printf "%s\t%d\n", a[1], a[2]
    }
    next
  }
  in_block { in_block = 0; captured = 1 }
' "$TMPOUT")"

# Sort per-fixture by pass% descending, then by fixture name.  Empty
# fixtures (pct == -1) sort last.
PER_FIXTURE_SORTED="$(echo "$PER_FIXTURE" | awk -F'\t' '
  { pct = ($6 < 0) ? -1 : $6; print pct "\t" $0 }
' | sort -t$'\t' -k1,1nr -k2,2 | cut -f2-)"

# Format the per-fixture markdown table.
#
# Column widths chosen to match the existing README aesthetic
# (right-aligned numerics, fixture name left-padded so the table
# stays vertically aligned even after string_ext drops to 0%).
PER_FIXTURE_MD="$(echo "$PER_FIXTURE_SORTED" | awk -F'\t' '
  function pad(s, w,    n) { n = w - length(s); return (n > 0) ? sprintf("%*s", n, "") : "" }
  BEGIN {
    print "| Fixture | Total | Pass | Skip | Fail | Pass% | Skip categories |"
    print "|---|---:|---:|---:|---:|---:|---|"
  }
  {
    fixture = $1; total = $2; pass = $3; skip = $4; fail = $5; pct = $6; cats = $7
    if (total == 0) {
      cats = (cats == "") ? "(empty fixture)" : cats
      pct_s = " — "
    } else {
      pct_s = sprintf("%d%%", pct)
      if (cats == "") cats = "—"
    }
    # Padding outside backticks so the table stays vertically
    # aligned without putting spaces inside the inline-code spans.
    printf "| `%s`%s | %3d | %3d | %3d | %3d | %3s | %s |\n", \
           fixture, pad(fixture, 26), total, pass, skip, fail, pct_s, cats
  }
')"

# Pull existing Disposition prose for each category from the
# committed README.  We re-emit Count from CORPUS_SKIP but keep the
# Disposition column verbatim.  If a new category appears that's
# not yet in the README, emit a placeholder.
DISPOSITION_MAP="$(awk '
  /<!-- BEGIN AUTOGEN skip-totals -->/ { in_block = 1; next }
  /<!-- END AUTOGEN skip-totals -->/   { in_block = 0 }
  in_block && /^\| `/ {
    # | `cat`            | N | Prose... |
    line = $0
    if (match(line, /^\| `[^`]+`/) > 0) {
      cat = substr(line, RSTART + 3, RLENGTH - 4)
      # Trim trailing whitespace so a previous regen that padded
      # inside backticks still produces a lookup key matching the
      # conformance category name.
      gsub(/[ \t]+$/, "", cat)
      n = split(line, parts, "|")
      if (n >= 4) {
        prose = parts[4]
        gsub(/^[ \t]+|[ \t]+$/, "", prose)
        printf "%s\t%s\n", cat, prose
      }
    }
  }
' "$README")"

# Sort corpus skip-by-category by count descending so the largest
# bucket is first (matches the hand-maintained README aesthetic).
CORPUS_SKIP_SORTED="$(echo "$CORPUS_SKIP" | sort -t$'\t' -k2,2nr)"

SKIP_TOTALS_MD="$(echo "$CORPUS_SKIP_SORTED" | DISPMAP="$DISPOSITION_MAP" awk -F'\t' '
  function pad(s, w,    n) { n = w - length(s); return (n > 0) ? sprintf("%*s", n, "") : "" }
  BEGIN {
    n = split(ENVIRON["DISPMAP"], lines, "\n")
    for (i = 1; i <= n; i++) {
      if (split(lines[i], kv, "\t") == 2) disp[kv[1]] = kv[2]
    }
    print "| Category | Count | Disposition |"
    print "|---|---:|---|"
    total = 0
  }
  {
    cat = $1; count = $2
    total += count
    d = (cat in disp) ? disp[cat] : "_New category — fill in disposition prose._"
    printf "| `%s`%s | %3d | %s |\n", cat, pad(cat, 14), count, d
  }
  END {
    printf "| **Total**          | **%d** | |\n", total
  }
')"

# Parse the corpus summary line: `summary: total=N pass=N skip=N fail=N`.
# `-a`: the conformance log can contain non-text bytes, which makes
# grep print "Binary file matches" instead of the line — treat as text.
SUMMARY_LINE="$(grep -aE '^summary:' "$TMPOUT" | head -n1 || true)"
if [[ -z "$SUMMARY_LINE" ]]; then
  echo "error: no 'summary:' line in conformance output" >&2
  exit 2
fi
TOTAL="$(echo "$SUMMARY_LINE" | sed -E 's/.*total=([0-9]+).*/\1/')"
PASS="$(echo  "$SUMMARY_LINE" | sed -E 's/.*pass=([0-9]+).*/\1/')"
SKIP="$(echo  "$SUMMARY_LINE" | sed -E 's/.*skip=([0-9]+).*/\1/')"
FAIL="$(echo  "$SUMMARY_LINE" | sed -E 's/.*fail=([0-9]+).*/\1/')"

# Pull disable_check + static_subset from corpus-wide SKIP for the
# "out-of-scope by design" arithmetic in the addressable-prose stamp.
DISABLE_CHECK="$(echo "$CORPUS_SKIP" | awk -F'\t' '$1=="disable_check"{print $2}')"
STATIC_SUBSET="$(echo "$CORPUS_SKIP" | awk -F'\t' '$1=="static_subset"{print $2}')"
DISABLE_CHECK="${DISABLE_CHECK:-0}"
STATIC_SUBSET="${STATIC_SUBSET:-0}"
OOS_TOTAL=$((DISABLE_CHECK + STATIC_SUBSET))
SHIPPABLE_SKIP=$((SKIP - OOS_TOTAL))
ADDRESSABLE=$((TOTAL - OOS_TOTAL))

# Compute percentages with awk (printf "%.1f") so locale-safe.
pct() { awk -v num="$1" -v den="$2" 'BEGIN { if (den == 0) print "—"; else printf "%.1f", (num*100)/den }'; }
ipct() { awk -v num="$1" -v den="$2" 'BEGIN { if (den == 0) print "—"; else printf "%d", int((num*100)/den + 0.5) }'; }

PASS_PCT="$(pct "$PASS" "$TOTAL")"
SKIP_PCT="$(pct "$SKIP" "$TOTAL")"
FAIL_PCT="$(pct "$FAIL" "$TOTAL")"
ADDR_PCT="$(ipct "$PASS" "$ADDRESSABLE")"

HEADLINE_MD="$(printf '```\ntotal=%d  pass=%d (%s%%)  skip=%d (%s%%)  fail=%d (%s%%)\n```' \
  "$TOTAL" "$PASS" "$PASS_PCT" "$SKIP" "$SKIP_PCT" "$FAIL" "$FAIL_PCT")"

ADDR_PROSE_MD="$(cat <<EOF
Of the $SKIP SKIPs: ~$OOS_TOTAL are out-of-scope by design
(\`disable_check\` + \`static_subset\`); the rest ($SHIPPABLE_SKIP) are
scope-not-yet-shipped capabilities a future milestone will
graduate.  Effective pass rate against the addressable corpus
($TOTAL - $OOS_TOTAL = $ADDRESSABLE) is **${ADDR_PCT}%**.
EOF
)"

# Stamp the regenerated content between markers.  Four AUTOGEN
# regions: headline, per-fixture, skip-totals, addressable-prose.
# Pass the (multi-line) content via env vars — awk -v cannot carry
# newlines.
NEW_README="$(HEADMD="$HEADLINE_MD" FIXMD="$PER_FIXTURE_MD" \
              SKIPMD="$SKIP_TOTALS_MD" PROMD="$ADDR_PROSE_MD" \
  awk '
  /<!-- BEGIN AUTOGEN headline -->/ { print; print ENVIRON["HEADMD"]; in_head = 1; next }
  /<!-- END AUTOGEN headline -->/   { in_head = 0; print; next }
  /<!-- BEGIN AUTOGEN per-fixture -->/ { print; print ENVIRON["FIXMD"]; in_fix = 1; next }
  /<!-- END AUTOGEN per-fixture -->/   { in_fix = 0; print; next }
  /<!-- BEGIN AUTOGEN skip-totals -->/ { print; print ENVIRON["SKIPMD"]; in_skip = 1; next }
  /<!-- END AUTOGEN skip-totals -->/   { in_skip = 0; print; next }
  /<!-- BEGIN AUTOGEN addressable-prose -->/ { print; print ENVIRON["PROMD"]; in_pro = 1; next }
  /<!-- END AUTOGEN addressable-prose -->/   { in_pro = 0; print; next }
  !in_head && !in_fix && !in_skip && !in_pro { print }
' "$README")"

if [[ "$MODE" == "check" ]]; then
  if ! diff -u "$README" <(echo "$NEW_README") >/dev/null; then
    echo "" >&2
    echo "conformance README is stale.  Regenerate with:" >&2
    echo "  scripts/regen_conformance_readme.sh" >&2
    echo "and commit the result." >&2
    echo "" >&2
    diff -u "$README" <(echo "$NEW_README") | head -60 >&2
    exit 1
  fi
  echo "conformance README in sync." >&2
  exit 0
fi

echo "$NEW_README" > "$README"
echo "==> Rewrote $README" >&2
