#!/usr/bin/env bash
# check_doc_drift.sh — flag planning docs that name code that no
# longer exists.
#
# Planning docs accrete references to files, BUILD targets, and
# specific function/class symbols.  Code moves; docs lag.  This
# script greps for those references and reports the ones that
# no longer resolve in the working tree — the cheapest mechanical
# tripwire against the most common form of doc bitrot.
#
# Two reference classes:
#
#   1. **Backtick-quoted paths**:  `compiler_v2/foo/bar.cc`,
#      `doc/implementation-plan/rewrite/baz.md` — checked via
#      `[[ -e <path> ]]`.
#
#   2. **Backtick-quoted symbol-shaped strings**:  `MyFunction`,
#      `cel_host_codec`, `MakeProtoMessage()` — checked via grep
#      against the tracked source tree.  Heuristic — best-effort,
#      not exhaustive, and tuned to err on the side of false
#      negatives (missing a stale ref) rather than false positives
#      (flagging a sentence that happens to look like a symbol).
#
# Usage:
#   scripts/check_doc_drift.sh                # scan doc/implementation-plan/
#   scripts/check_doc_drift.sh path/to/doc.md # scan named doc(s)
#   scripts/check_doc_drift.sh --strict       # exit non-zero on findings
#
# Output: human-readable list of (doc-file, line, missing-ref).
# Default exit code is 0 (advisory) so CI doesn't break on a
# transient lag; `--strict` is the gating mode for merge gates.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

strict=0
declare -a docs=()
for arg in "$@"; do
  case "$arg" in
    --strict) strict=1 ;;
    --help|-h)
      sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) docs+=("$arg") ;;
  esac
done

if [[ ${#docs[@]} -eq 0 ]]; then
  while IFS= read -r -d '' f; do docs+=("$f"); done < <(
    find doc/implementation-plan -type f -name '*.md' -print0
  )
fi

# Symbols / paths to ignore even if missing.  These are intentional
# forward references (planned-but-not-yet-shipped) or external names.
declare -a ignore_patterns=(
  # External tooling / vendored projects.
  'protoc'
  'cel-cpp'
  'absl::'
  'std::'
  'google::protobuf'
  'wasm32-'
  'wasi_'
  'wasi-'
  'wasm-ld'
  'wasm-as'
  'clang-'
  'compile_commands.json'
  # CEL language semantics — not symbols in our tree.
  'cel.expr'
  'CEL_'
  '.textproto'
)

is_ignored() {
  local s="$1"
  for p in "${ignore_patterns[@]}"; do
    [[ "$s" == *"$p"* ]] && return 0
  done
  return 1
}

# Build a one-shot source-tree corpus for symbol existence checks.
# Two-line grep: identifier-ish tokens in tracked source files.
src_corpus=$(mktemp)
trap 'rm -f "$src_corpus"' EXIT
git ls-files 'compiler_v2/**/*.cc' 'compiler_v2/**/*.h' 'compiler_v2/**/*.c' \
              'compiler/**/*.cc' 'compiler/**/*.h' 2>/dev/null \
  | xargs -I{} grep -hoE '\b[A-Za-z_][A-Za-z0-9_]+\b' {} 2>/dev/null \
  | sort -u > "$src_corpus"

# Also enumerate file paths git knows about — used for path checks
# when a backtick wraps a path-shaped string.
path_corpus=$(mktemp)
trap 'rm -f "$src_corpus" "$path_corpus"' EXIT
git ls-files > "$path_corpus"

findings=0
for doc in "${docs[@]}"; do
  [[ -f "$doc" ]] || { echo "skip: $doc (not a file)"; continue; }

  # Extract backtick-quoted refs.  awk pattern reads one line at a
  # time, prints `lineno\tref` for each backtick span.
  awk '
    {
      n = split($0, parts, "`");
      for (i = 2; i <= n; i += 2) {
        if (parts[i] != "" && length(parts[i]) < 100) {
          print NR "\t" parts[i];
        }
      }
    }
  ' "$doc" | while IFS=$'\t' read -r lineno ref; do
    # Ignore patterns we don't care about.
    is_ignored "$ref" && continue

    # Path-shaped: contains a / and a . (e.g. "foo/bar.cc").
    if [[ "$ref" == *"/"*"."* && "$ref" != *" "* ]]; then
      # Strip any trailing :line / line range suffix.
      bare="${ref%%:*}"
      bare="${bare%% *}"
      if [[ -n "$bare" && ! -e "$bare" ]] && ! grep -qxF "$bare" "$path_corpus"; then
        echo "$doc:$lineno: missing path \`$ref\`"
        findings=$((findings + 1))
      fi
      continue
    fi

    # Symbol-shaped: matches /^[A-Za-z_][A-Za-z0-9_]*$/, no spaces.
    # Plain identifier (no namespace, no parens, no operators).
    if [[ "$ref" =~ ^[A-Za-z_][A-Za-z0-9_]+$ ]]; then
      # Identifiers shorter than 6 chars are too common to verify
      # without false positives (every "foo" might be a real
      # identifier somewhere).
      [[ ${#ref} -lt 6 ]] && continue
      if ! grep -qxF "$ref" "$src_corpus"; then
        echo "$doc:$lineno: missing symbol \`$ref\`"
        findings=$((findings + 1))
      fi
    fi
  done
done

if [[ "$findings" -gt 0 ]]; then
  echo
  echo "check_doc_drift.sh: $findings missing reference(s)"
  [[ "$strict" -eq 1 ]] && exit 1
fi
exit 0
