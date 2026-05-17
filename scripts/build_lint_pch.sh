#!/usr/bin/env bash
# build_lint_pch.sh — build the precompiled-header used by lint.sh.
#
# Reads flags from compile_commands.json (the first non-test
# compiler_v2 .cc entry — gtest's `-I` is absent from non-test TUs,
# so the resulting PCH is valid for both test and non-test files),
# substitutes brew clang as the compiler, and emits
# scripts/lint_pch.h → .lint-cache/lint_pch.h.pch.
#
# Idempotent: skips rebuild if the PCH is newer than both lint_pch.h
# and compile_commands.json.  scripts/lint.sh calls this transparently.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

PCH_HEADER="scripts/lint_pch.h"
PCH_OUT=".lint-cache/lint_pch.h.pch"

if [[ ! -f compile_commands.json ]]; then
  echo "build_lint_pch.sh: no compile_commands.json — skipping PCH build." >&2
  exit 0
fi

# Keep execroot/_main/external/<repo> symlinks live for clang-tidy's
# per-TU `-iquote external/...` resolution.  Bazel populates external
# symlinks lazily; a recent narrow `bazel test` evicts repos that
# target doesn't depend on.  When those symlinks vanish, clang-tidy
# hits header-not-found AND cascades into a flood of error-recovery
# false positives (non-const-parameter on `std::vector<>*` params,
# pro-type-member-init on aggregate structs, misc-use-internal-linkage
# on extern entry points).  Build the superset that
# compile_commands.json's -iquote paths reference.  Runs BEFORE the
# PCH-fresh gate below — the symlinks decay independently of the PCH
# inputs, so the gate would skip this step on warm cache.  Sub-2s on
# warm bazel cache.
bazel build //compiler_v2/... //compiler_v2/bench:kernel_bench \
  >/dev/null 2>&1 || true

if [[ -f "$PCH_OUT" \
      && "$PCH_OUT" -nt "$PCH_HEADER" \
      && "$PCH_OUT" -nt compile_commands.json ]]; then
  exit 0  # PCH is fresh.
fi

mkdir -p "$(dirname "$PCH_OUT")"
PCH_HEADER_ABS="$(pwd)/$PCH_HEADER"
PCH_OUT_ABS="$(pwd)/$PCH_OUT"

# Brew llvm — must match the compiler clang-tidy uses, otherwise the
# PCH's serialized AST is rejected.
CLANG="${CLANG:-/opt/homebrew/opt/llvm/bin/clang++}"
if [[ ! -x "$CLANG" ]]; then
  echo "build_lint_pch.sh: $CLANG not executable — skipping PCH build." >&2
  exit 0
fi

echo "build_lint_pch.sh: building $PCH_OUT"

python3 - "$PCH_HEADER_ABS" "$PCH_OUT_ABS" "$CLANG" <<'PY'
import json, os, subprocess, sys

header_abs, pch_out_abs, clang = sys.argv[1:4]

with open('compile_commands.json') as f:
    db = json.load(f)

# Pick a non-test compiler_v2 .cc entry whose `-iquote`/`-I` set
# actually pulls in absl + protobuf — otherwise the PCH-build clang
# can't resolve the absl headers and the PCH never produces.  Bazel
# generates many tiny TUs that don't depend on absl (e.g. proto-only
# emitters); skip those.
entry = None
for e in db:
    f = e.get('file', '')
    if 'compiler_v2' not in f or not f.endswith('.cc'):
        continue
    if '_test.cc' in f or '/test_' in f:
        continue
    args_blob = ' '.join(e['arguments'])
    if 'abseil-cpp~' not in args_blob or 'protobuf~' not in args_blob:
        continue
    entry = e
    break
if entry is None:
    sys.exit('no non-test compiler_v2 .cc entry with absl+protobuf deps')

args = list(entry['arguments'])
out = []
i = 0
while i < len(args):
    a = args[i]
    # Drop output / dependency / per-file flags that don't apply to PCH.
    if a in ('-c', '-MD'):
        i += 1; continue
    if a in ('-MF', '-o'):
        i += 2; continue
    if a.startswith('-frandom-seed='):
        i += 1; continue
    # Drop any source-file positional (compile_commands records
    # exactly one .cc, but be defensive — clang errors with
    # "cannot specify -o when generating multiple output files"
    # if any leak through).
    if a.endswith(('.cc', '.cpp', '.cxx', '.c', '.m', '.mm')):
        i += 1; continue
    out.append(a)
    i += 1

# Replace bazel's recorded compiler with brew clang++ (must match
# clang-tidy's clang).  Append PCH-build directives.
out[0] = clang
cmd = out + ['-x', 'c++-header', header_abs, '-o', pch_out_abs]

# compile_commands records `directory` as the bazel exec root; its
# `-iquote .`, `-iquote bazel-out/...`, `-Ibazel-out/...` paths
# resolve from there.  Run with that as cwd so include resolution
# matches what clang-tidy will see at lint time.
os.chdir(entry['directory'])
subprocess.check_call(cmd)
PY

size=$(stat -f%z "$PCH_OUT" 2>/dev/null || stat -c%s "$PCH_OUT")
echo "build_lint_pch.sh: ok ($size bytes)"
