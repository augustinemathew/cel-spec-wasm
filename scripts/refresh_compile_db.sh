#!/usr/bin/env bash
# refresh_compile_db.sh — regenerate compile_commands.json for clang-tidy.
#
# clang-tidy needs an entry per translation unit (compiler flags,
# include paths, language standard) so it can parse each .cc the same
# way bazel does. We emit it via Hedron's `hedron_compile_commands`
# bazel extension, which inspects every cc_library / cc_test / cc_binary
# in the project packages and writes the database to the repo root.
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
#
# Both paths include `manual`-tagged C++ targets (bench binaries,
# wasmtime-gated tests, conformance runner, …).  The aquery fallback
# enumerates them explicitly via `bazel query` and unions them into
# the build + aquery target patterns; see the inline comment by the
# `manual_cc=` line below.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Project-package set (exec-doc §1.0) — `//...` is unusable (vendored
# third_party/cel-cpp loads an undeclared repo).  Two forms: space-joined
# for `bazel build` command lines, `+`-joined for aquery/query union
# strings (Q3).
PROJ="//compiler/... //eval/... //shared/... //abi/... //runtime/... //tools/... //conformance/... //e2e/... //bench/... //benchmark/... //testdata/... //spec/..."
PROJ_UNION="//compiler/... + //eval/... + //shared/... + //abi/... + //runtime/... + //tools/... + //conformance/... + //e2e/... + //bench/... + //benchmark/... + //testdata/... + //spec/..."

if ! grep -q "hedron_compile_commands" MODULE.bazel 2>/dev/null; then
  echo "warning: hedron_compile_commands is not declared in MODULE.bazel." >&2
  echo "  See this script's header for setup instructions." >&2
  echo "  Falling back to Bazel's aquery-based generator (slower)." >&2
  # Aquery-based fallback: dumps every compile action bazel would run
  # for the project packages and reformats it as compile_commands.json. This
  # works without any MODULE.bazel changes but is noticeably slower.
  #
  # `manual`-tagged targets (e.g. //bench:kernel_bench,
  # //conformance:run_conformance, the wasmtime-gated
  # //eval:instance_test, …) are excluded by `:all` /
  # `...` wildcard expansion at the build/test level.  aquery's
  # wildcard expansion technically traverses manual deps, but only
  # the compile actions whose outputs sit on disk get returned
  # reliably — so a manual target that has never been built shows
  # up in `bazel query` but not in `bazel aquery $PROJ`.
  # The net effect is that clangd sees "file not found" errors for
  # every TU under a manual target.
  #
  # Fix: enumerate manual cc_* targets explicitly via `bazel query`
  # and union them into both the precursor `bazel build` line (so
  # generated headers / proto outputs land on disk) and the aquery
  # target pattern (so the action set is exhaustive).  Non-manual
  # entries are unchanged — the union is idempotent.
  manual_cc=$(bazel query \
    "attr(tags, \"\\bmanual\\b\", ${PROJ_UNION}) intersect kind(\"cc_.*\", ${PROJ_UNION})" \
    2>/dev/null | paste -sd '+' -)
  if [[ -n "${manual_cc}" ]]; then
    aquery_pattern="${PROJ_UNION} + ${manual_cc}"
    # `bazel build` takes space-separated labels, not '+'-unioned.
    manual_cc_space=${manual_cc//+/ }
    # shellcheck disable=SC2086
    bazel build --config=lint $PROJ ${manual_cc_space} 2>/dev/null || true
  else
    aquery_pattern="${PROJ_UNION}"
    # shellcheck disable=SC2086
    bazel build --config=lint $PROJ 2>/dev/null || true
  fi
  bazel aquery --output=jsonproto \
    "mnemonic(\"CppCompile\", ${aquery_pattern})" \
    > /tmp/celwasm_aquery.json
  python3 scripts/_aquery_to_compdb.py /tmp/celwasm_aquery.json \
    > compile_commands.json
  echo "Wrote compile_commands.json via aquery fallback."
  exit 0
fi

bazel run @hedron_compile_commands//:refresh_all
echo "Wrote compile_commands.json (via hedron_compile_commands)."
