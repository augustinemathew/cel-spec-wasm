#!/usr/bin/env bash
# lint.sh — format + lint changed C/C++ files.
#
# Usage:
#   scripts/lint.sh                    # DEFAULT: working-tree edits only (inner loop, ~5-9s)
#   scripts/lint.sh path/to/file.cc    # inner loop: lint named file(s), ~4.6s
#   scripts/lint.sh --branch           # pre-commit / PR gate: full branch diff vs main (~73s)
#   scripts/lint.sh --dirty            # explicit synonym for the default
#   scripts/lint.sh --all              # lint every project source file
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

# Build the file list.  Scope selection (DEFAULT = working-tree edits):
#
#   (no args)         working-tree edits only (staged + unstaged) — the
#                     inner-loop default.  Lints just the files you're
#                     actively touching (~4.6s/file).  Same as --dirty.
#   --branch          everything changed on this branch vs origin/master,
#                     plus the working tree — the comprehensive pre-commit
#                     / PR gate (~73s on ~20 files).
#   --dirty           explicit synonym for the default.
#   --all             every project source file.
#   <file> [<file>…]  exactly the named files.
#
# Rationale + measured costs: doc/implementation-plan/dev-loop-performance.md
# and CLAUDE.md "Lint & format".  The default is the common case (lint
# while editing); the full-branch sweep is opt-in so the inner loop is
# cheap by default.
declare -a files=()

# C/C++ sources under the project packages, as git pathspecs (reused below).
_cc_globs=(
  ':(glob)compiler/**/*.cc' ':(glob)compiler/**/*.h' ':(glob)compiler/**/*.c'
  ':(glob)eval/**/*.cc' ':(glob)eval/**/*.h' ':(glob)eval/**/*.c'
  ':(glob)common/**/*.cc' ':(glob)common/**/*.h' ':(glob)common/**/*.c'
  ':(glob)abi/**/*.cc' ':(glob)abi/**/*.h' ':(glob)abi/**/*.c'
  ':(glob)runtime/**/*.cc' ':(glob)runtime/**/*.h' ':(glob)runtime/**/*.c'
  ':(glob)tools/**/*.cc' ':(glob)tools/**/*.h' ':(glob)tools/**/*.c'
  ':(glob)conformance/**/*.cc' ':(glob)conformance/**/*.h' ':(glob)conformance/**/*.c'
  ':(glob)e2e/**/*.cc' ':(glob)e2e/**/*.h' ':(glob)e2e/**/*.c'
  ':(glob)benchmark/**/*.cc' ':(glob)benchmark/**/*.h' ':(glob)benchmark/**/*.c'
  ':(glob)testdata/**/*.cc' ':(glob)testdata/**/*.h' ':(glob)testdata/**/*.c'
)

# NOTE: the `:(glob)` prefixes above are load-bearing.  With git's
# DEFAULT pathspec magic, `runtime/**/*.c` requires a literal `/` after
# the `**`, so `runtime/cel_runtime.c` — a file directly under the role
# dir — never matched and was silently skipped by the branch gate.
# `:(glob)` gives `**/` its usual "zero or more directories" meaning.
_dedup_files() {
  if [[ ${#files[@]} -gt 0 ]]; then
    mapfile -t files < <(printf '%s\n' "${files[@]}" | awk 'NF' | sort -u)
  fi
}

case "${1:-}" in
  --all)
    while IFS= read -r -d '' f; do files+=("$f"); done < <(
      find compiler eval common abi runtime tools conformance e2e bench testdata \
        -type f \( -name '*.cc' -o -name '*.h' -o -name '*.c' \) \
        -print0
    )
    ;;
  --branch)
    # Full branch gate: diff vs origin/master (fall back to HEAD if no
    # upstream) PLUS the working tree.
    base_ref="origin/master"
    if ! git rev-parse --verify "$base_ref" >/dev/null 2>&1; then
      base_ref="HEAD"
    fi
    mapfile -t files < <(
      git diff --name-only --diff-filter=ACMR "$base_ref"...HEAD -- "${_cc_globs[@]}"
      git diff --name-only --diff-filter=ACMR -- "${_cc_globs[@]}"
      git diff --name-only --cached --diff-filter=ACMR -- "${_cc_globs[@]}"
    )
    _dedup_files
    ;;
  --dirty | "")
    # DEFAULT + explicit --dirty: working-tree edits only (staged + unstaged).
    mapfile -t files < <(
      git diff --name-only --diff-filter=ACMR -- "${_cc_globs[@]}"
      git diff --name-only --cached --diff-filter=ACMR -- "${_cc_globs[@]}"
    )
    _dedup_files
    ;;
  *)
    files=("$@")
    ;;
esac

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
# under `runtime/` are treated as C because the runtime is a
# C translation unit shared with C++ tests (wrapped in `extern "C"`).
declare -a c_targets=()
declare -a cpp_targets=()
# `runtime/*_internal.h` and `string_ext_test_helpers.h`
# are C++-only orphan headers (use absl + gtest, no matching `.cc`
# basename clang-tidy could pull a compile entry from).  Skip the
# direct analysis — they're transitively covered by the `.cc` files
# that include them.  Pattern lives here rather than in the
# discovery glob so a forced `--all` run still skips them.
for f in "${targets[@]}"; do
  case "$f" in
    # TUs including vendored cel-cpp's `extensions/*.h` are
    # FORMAT-ONLY: those headers `#include "compiler/compiler.h"`
    # meaning cel-cpp's own header, and clang-tidy runs unsandboxed,
    # so `-iquote .` resolves the name to OUR compiler/compiler.h
    # (different include guard → cel-cpp's never loads →
    # `CompilerLibrary` undeclared → bogus clang-diagnostic-errors on
    # a recovery AST).  The bazel compile is immune only because
    # sandboxing omits our header from the action's declared inputs.
    # No -iquote order fixes both sides — the repos genuinely share
    # the relative path.  Tracked in cleanup-backlog (lint VFS
    # overlay, or renaming our colliding header).  Keep in sync with
    # `grep -rl '#include "extensions/'` over the first-party tree.
    compiler/frontend/parse_and_check.cc) \
      echo "lint.sh: NOTE — $f is format-only (cel-cpp header collision)."; \
      continue ;;
    testdata/cel_cpp_oracle.cc) \
      echo "lint.sh: NOTE — $f is format-only (cel-cpp header collision)."; \
      continue ;;
    runtime/cel_string_ext_internal.h) continue ;;
    runtime/cel_string_format_internal.h) continue ;;
    runtime/string_ext_test_helpers.h) continue ;;
    # C++ orphan header (no matching `.cc` basename → clang-tidy's
    # DB interpolation picks a TU without the wasmtime include path).
    # Transitively covered by memory_grow_stability_test.cc.
    eval/internal/instance_test_peer.h) continue ;;
    # Same orphan-header pattern: shared gtest fixture for the
    # cel_host family tests; interpolation picks a non-test TU with
    # no gmock include path.  Transitively covered by the four
    # cel_host_*_test.cc TUs that include it.
    eval/internal/cel_host_test_harness.h) continue ;;
    # The only first-party TU that includes cel-cpp's
    # `compiler/compiler_factory.h`, which does not parse under the
    # lint clang — `CompilerBuilder` is undeclared at its own
    # declaration site (compiler_factory.h:51), so the AST degrades and
    # the TU reports 15 clang-diagnostic-errors.  Vendored-header vs
    # pinned-toolchain skew, not a defect here: `bazel build
    # //testdata:cel_cpp_oracle` is green.  Tracked in
    # doc/implementation-plan/lint-backlog.md.
    testdata/cel_cpp_oracle.cc) continue ;;
  esac
  case "$f" in
    *.c)                          c_targets+=("$f") ;;
    # C++-only runtime header (namespaced, absl-using, with a matching
    # `.cc` basename in the compile DB) — analyze as C++.  The
    # `runtime/*.h → C` default below is for the extern-C ABI headers.
    runtime/cel_time_canonical.h) cpp_targets+=("$f") ;;
    runtime/*.h)                  c_targets+=("$f") ;;
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
