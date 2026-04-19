#!/usr/bin/env bash
# refresh_compile_db.sh — regenerate compile_commands.json for clang-tidy.
#
# clang-tidy needs an entry per translation unit (compiler flags,
# include paths, language standard) so it can parse each .cc the same
# way bazel does. We emit it via Hedron's `hedron_compile_commands`
# bazel extension, which inspects every cc_library / cc_test / cc_binary
# in //compiler/... and writes the database to the repo root.
#
# First time use (one-off, not yet wired into MODULE.bazel):
#
#   1. Add to MODULE.bazel:
#        bazel_dep(name = "hedron_compile_commands", version = "...")
#      OR (vendor via git_override) per Hedron's README:
#        https://github.com/hedronvision/bazel-compile-commands-extractor
#
#   2. Run this script to emit compile_commands.json.
#
# Without the bazel dep wired up, clang-tidy can still run in "syntax-
# only" mode via scripts/lint.sh (which falls back to a curated set of
# -I flags discovered from `bazel info output_base`).  The compile DB
# is strongly preferred for accurate analysis.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

if ! grep -q "hedron_compile_commands" MODULE.bazel 2>/dev/null; then
  echo "warning: hedron_compile_commands is not declared in MODULE.bazel." >&2
  echo "  See this script's header for setup instructions." >&2
  echo "  Falling back to Bazel's aquery-based generator (slower)." >&2
  # Aquery-based fallback: dumps every compile action bazel would run
  # for //compiler/... and reformats it as compile_commands.json. This
  # works without any MODULE.bazel changes but is noticeably slower.
  bazel build --config=lint //compiler/... 2>/dev/null || true
  bazel aquery --output=jsonproto 'mnemonic("CppCompile", //compiler/...)' \
    > /tmp/celwasm_aquery.json
  python3 scripts/_aquery_to_compdb.py /tmp/celwasm_aquery.json \
    > compile_commands.json
  echo "Wrote compile_commands.json via aquery fallback."
  exit 0
fi

bazel run @hedron_compile_commands//:refresh_all
echo "Wrote compile_commands.json (via hedron_compile_commands)."
