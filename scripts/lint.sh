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
      'compiler/**/*.cc' 'compiler/**/*.h' 'compiler/**/*.c' \
      'compiler_v2/*.cc' 'compiler_v2/*.h' 'compiler_v2/*.c' \
      'compiler_v2/**/*.cc' 'compiler_v2/**/*.h' 'compiler_v2/**/*.c'
    git diff --name-only --diff-filter=ACMR -- \
      'compiler/*.cc' 'compiler/*.h' 'compiler/*.c' \
      'compiler/**/*.cc' 'compiler/**/*.h' 'compiler/**/*.c' \
      'compiler_v2/*.cc' 'compiler_v2/*.h' 'compiler_v2/*.c' \
      'compiler_v2/**/*.cc' 'compiler_v2/**/*.h' 'compiler_v2/**/*.c'
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

# Filter to *.c/*.cc/*.h before clang-format.  clang-format will happily
# rewrite anything you hand it as if it were C++ — passing a `.bazel`
# or `.sh` here will silently corrupt the file.  Earlier versions of
# this script pre-filtered up the call chain, but explicit guarding
# here is the safer invariant.
declare -a fmt_targets=()
for f in "${targets[@]}"; do
  case "$f" in
    *.c|*.cc|*.h) fmt_targets+=("$f") ;;
  esac
done
if [[ ${#fmt_targets[@]} -gt 0 ]]; then
  echo "lint.sh: formatting ${#fmt_targets[@]} file(s) with $CLANG_FORMAT"
  "$CLANG_FORMAT" -i "${fmt_targets[@]}"
fi

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

# PCH speedup.  Build (or refresh) the precompiled-header up-front; it
# amortises the absl + protobuf header parse across every C++ TU.  Best
# effort — build failures fall back to no-PCH and emit a warning, never
# break the lint.  PCH is C++-only; C TUs ignore it.
PCH_PATH=".lint-cache/lint_pch.h.pch"
declare -a cpp_pch_args=()
if [[ -x scripts/build_lint_pch.sh && -f compile_commands.json ]]; then
  if scripts/build_lint_pch.sh; then
    if [[ -f "$PCH_PATH" ]]; then
      # clang's `-include-pch` takes the path as a separate argument;
      # the `--extra-arg-before=-include-pch=PATH` joined form gets
      # parsed by clang's driver as `-pch=PATH` (the `-include` prefix
      # is stripped before the equals splitter sees it), which then
      # surfaces as `'-pch=...' file not found`.  Pass the flag and
      # its value as two separate --extra-arg-before entries.
      cpp_pch_args+=("--extra-arg-before=-include-pch")
      cpp_pch_args+=("--extra-arg-before=$(pwd)/$PCH_PATH")
    fi
  else
    echo "lint.sh: warning — PCH build failed; continuing without it." >&2
  fi
fi

# Parallelism.  xargs -P fans clang-tidy out across cores, one file
# per process (clang-tidy is itself single-threaded per invocation).
# Override with `LINT_JOBS=N scripts/lint.sh`.
JOBS="${LINT_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null \
                     || nproc 2>/dev/null \
                     || echo 4)}"

pch_label="no"
if [[ ${#cpp_pch_args[@]} -gt 0 ]]; then pch_label="yes"; fi
echo "lint.sh: running $CLANG_TIDY on ${#targets[@]} file(s) — jobs=$JOBS, pch=$pch_label"

# Split targets into C vs C++ groups. Bazel's compile_commands.json uses
# `clang++` as the driver for every entry, even plain C files — which
# errors out as `-std=c11 not allowed with C++` when clang-tidy replays
# the entry. Forcing `-xc` on C inputs sidesteps that. `.h` headers
# under `compiler/runtime/` and `compiler_v2/runtime/` are treated as C because the runtime is a
# C translation unit shared with C++ tests (wrapped in `extern "C"`).
declare -a c_targets=()
declare -a cpp_targets=()
for f in "${targets[@]}"; do
  case "$f" in
    *.c)                          c_targets+=("$f") ;;
    compiler/runtime/*.h)         c_targets+=("$f") ;;
    compiler_v2/runtime/*.h)      c_targets+=("$f") ;;
    *)                            cpp_targets+=("$f") ;;
  esac
done

rc=0
# `--warnings-as-errors=*` so *any* emitted warning becomes a non-zero
# exit. The check set is driven by the repo-root .clang-tidy.  Each
# block fans out via xargs -P so files within a group lint in parallel.
if [[ ${#cpp_targets[@]} -gt 0 ]]; then
  printf '%s\n' "${cpp_targets[@]}" \
    | xargs -n 1 -P "$JOBS" "$CLANG_TIDY" "${tidy_args[@]}" \
        "${cpp_pch_args[@]}" --warnings-as-errors='*' --quiet \
    || rc=$?
fi
# Runtime .h files are freestanding C; don't pass `-p .` for them because
# clang-tidy's basename-matching heuristic can otherwise pick up a C++
# compile entry from a sibling directory (e.g. host/cel_log.cc vs.
# runtime/cel_log.h) and analyze the header in C++ mode, which defeats
# `-xc`.  `.c` translation units still get the DB for include paths.
declare -a c_src_targets=()
declare -a c_hdr_targets=()
for f in "${c_targets[@]}"; do
  case "$f" in
    *.h) c_hdr_targets+=("$f") ;;
    *)   c_src_targets+=("$f") ;;
  esac
done
if [[ ${#c_src_targets[@]} -gt 0 ]]; then
  printf '%s\n' "${c_src_targets[@]}" \
    | xargs -n 1 -P "$JOBS" "$CLANG_TIDY" "${tidy_args[@]}" \
        --extra-arg-before=-xc --warnings-as-errors='*' --quiet \
    || rc=$?
fi
if [[ ${#c_hdr_targets[@]} -gt 0 ]]; then
  # Fixed-compilation-database form (`<file> -- <flags>`) bypasses
  # compile_commands.json entirely; otherwise tidy's auto-detect walks
  # up to the repo root and its basename heuristic can pick up a
  # sibling C++ entry that overrides `-xc`.  -I '{}' templates the
  # filename in so xargs's `--` is consumed by xargs, not clang-tidy.
  printf '%s\n' "${c_hdr_targets[@]}" \
    | xargs -n 1 -P "$JOBS" -I '{}' \
        "$CLANG_TIDY" --warnings-as-errors='*' --quiet \
                      '{}' -- -xc -I. \
    || rc=$?
fi
if [[ $rc -ne 0 ]]; then
  echo "lint.sh: clang-tidy reported warnings — see above." >&2
  exit 1
fi

echo "lint.sh: clean."
