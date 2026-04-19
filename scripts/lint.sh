#!/usr/bin/env bash
# lint.sh — format + lint changed C/C++ files.
#
# Usage:
#   scripts/lint.sh                    # lint files that differ from main
#   scripts/lint.sh --all              # lint every compiler/ source file
#   scripts/lint.sh path/to/file.cc    # lint the named files only
#
# Behaviour:
#   1. Run clang-format -i on each target file (in-place rewrite).
#   2. Run clang-tidy on each target file, using compile_commands.json
#      if present. Any clang-tidy warning → non-zero exit.
#   3. third_party/ and bazel-*/ are always skipped.
#
# Install prereqs:
#   brew install llvm
#   # put clang-format / clang-tidy on PATH, e.g.
#   export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Prefer brew llvm if PATH doesn't already have a modern clang-tidy.
if [[ -x /opt/homebrew/opt/llvm/bin/clang-format ]]; then
  export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
fi

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"

for tool in "$CLANG_FORMAT" "$CLANG_TIDY"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: $tool not on PATH. See scripts/lint.sh for install steps." >&2
    exit 2
  fi
done

# Build the file list.
declare -a files=()
if [[ $# -ge 1 && "$1" == "--all" ]]; then
  while IFS= read -r -d '' f; do files+=("$f"); done < <(
    find compiler -type f \( -name '*.cc' -o -name '*.h' -o -name '*.c' \) \
      -print0
  )
elif [[ $# -ge 1 ]]; then
  files=("$@")
else
  # Files that differ from origin/master (fall back to HEAD if no upstream).
  base_ref="origin/master"
  if ! git rev-parse --verify "$base_ref" >/dev/null 2>&1; then
    base_ref="HEAD"
  fi
  mapfile -t files < <(
    git diff --name-only --diff-filter=ACMR "$base_ref"...HEAD -- \
      'compiler/*.cc' 'compiler/*.h' 'compiler/*.c' \
      'compiler/**/*.cc' 'compiler/**/*.h' 'compiler/**/*.c'
    git diff --name-only --diff-filter=ACMR -- \
      'compiler/*.cc' 'compiler/*.h' 'compiler/*.c' \
      'compiler/**/*.cc' 'compiler/**/*.h' 'compiler/**/*.c'
  )
  # Dedup and strip empties.
  if [[ ${#files[@]} -gt 0 ]]; then
    mapfile -t files < <(printf '%s\n' "${files[@]}" | awk 'NF' | sort -u)
  fi
fi

# Filter out third_party/ and bazel-*/ defensively.
declare -a targets=()
for f in "${files[@]:-}"; do
  [[ -z "$f" ]] && continue
  case "$f" in
    third_party/*|bazel-*/*) continue ;;
  esac
  [[ -f "$f" ]] || continue
  targets+=("$f")
done

if [[ ${#targets[@]} -eq 0 ]]; then
  echo "lint.sh: no changed C/C++ files to lint."
  exit 0
fi

echo "lint.sh: formatting ${#targets[@]} file(s) with $CLANG_FORMAT"
"$CLANG_FORMAT" -i "${targets[@]}"

# clang-tidy. If compile_commands.json is missing we still try, but warn;
# analysis without the DB is partial (missing include paths → many
# spurious 'file not found' diagnostics).
tidy_args=()
if [[ -f compile_commands.json ]]; then
  tidy_args+=("-p" ".")
else
  echo "lint.sh: warning — compile_commands.json not found." >&2
  echo "  Run scripts/refresh_compile_db.sh before committing." >&2
fi

echo "lint.sh: running $CLANG_TIDY on ${#targets[@]} file(s)"
# `--warnings-as-errors=*` so *any* emitted warning becomes a non-zero
# exit. The check set is driven by the repo-root .clang-tidy.
if ! "$CLANG_TIDY" "${tidy_args[@]}" --warnings-as-errors='*' \
    --quiet "${targets[@]}"; then
  echo "lint.sh: clang-tidy reported warnings — see above." >&2
  exit 1
fi

echo "lint.sh: clean."
