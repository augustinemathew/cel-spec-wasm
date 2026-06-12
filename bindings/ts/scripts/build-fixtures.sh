#!/usr/bin/env bash
# Regenerate the golden compiled-Program fixtures for the pure-TS eval
# binding from the curated manifest.
#
# For each fixture in eval/fixtures/manifest.json this:
#   1. compiles `expr` (with its `compileVars`) via the native `cel` CLI
#      into eval/fixtures/<name>.wasm — a self-contained STATIC Program
#      (the runtime kernel is bundled in; no separate cel_runtime.wasm is
#      needed to evaluate it);
#   2. cross-checks the answer by running `cel eval` (with `cliVars`) and
#      comparing its printed result to the manifest's `cliCheck`, so a
#      drift between the manifest's hand-authored `expected` and what the
#      compiler actually produces fails loudly here, at generation time.
#
# The manifest is the source of truth; the .wasm files are committed
# build output. Run this only when the fixture set or the C++ wire format
# intentionally changes.
#
# Usage: bindings/ts/scripts/build-fixtures.sh
set -euo pipefail

REPO_ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
FIX_DIR="$REPO_ROOT/bindings/ts/eval/fixtures"
MANIFEST="$FIX_DIR/manifest.json"
CEL="$REPO_ROOT/bazel-bin/tools/cel/cel"

command -v jq >/dev/null || { echo "error: jq is required" >&2; exit 1; }
[ -f "$MANIFEST" ] || { echo "error: manifest not found: $MANIFEST" >&2; exit 1; }

# Build the cel CLI if it's not present (cached if already built).
if [ ! -x "$CEL" ]; then
  echo "cel CLI not found; building //tools/cel:cel ..."
  ( cd "$REPO_ROOT" && bazel build //tools/cel:cel )
fi

count="$(jq '.fixtures | length' "$MANIFEST")"
echo "Generating $count fixtures into $FIX_DIR"

fail=0
i=0
while [ "$i" -lt "$count" ]; do
  name="$(jq -r ".fixtures[$i].name" "$MANIFEST")"
  expr="$(jq -r ".fixtures[$i].expr" "$MANIFEST")"
  expect_cli="$(jq -r ".fixtures[$i].cliCheck" "$MANIFEST")"

  # compileVars -> repeated --var <decl> (decl only; bound at eval time).
  compile_args=()
  while IFS= read -r v; do
    [ -n "$v" ] && compile_args+=(--var "$v")
  done < <(jq -r ".fixtures[$i].compileVars[]?" "$MANIFEST")

  # cliVars -> repeated --var <decl=value> for the eval cross-check.
  eval_args=()
  while IFS= read -r v; do
    [ -n "$v" ] && eval_args+=(--var "$v")
  done < <(jq -r ".fixtures[$i].cliVars[]?" "$MANIFEST")

  # Wrap the expr in parens: semantically transparent for any complete CEL
  # expression, and it stops the CLI's flag parser from reading an expr
  # that begins with '-' (e.g. `-5`) as an unknown flag.
  wrapped="($expr)"

  out="$FIX_DIR/$name.wasm"
  "$CEL" compile "$wrapped" "${compile_args[@]}" --output "$out" >/dev/null

  # Cross-check the result against the manifest. `cel eval` exits non-zero
  # on an eval-error VALUE (e.g. divide_by_zero), which is itself a golden
  # outcome, so capture without tripping `set -e`.
  got="$("$CEL" eval "$wrapped" "${eval_args[@]}" 2>&1 || true)"
  got_trim="$(printf '%s' "$got" | tr -d '\n' | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
  want_trim="$(printf '%s' "$expect_cli" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"

  sz="$(wc -c < "$out" | tr -d ' ')"
  if [ "$got_trim" = "$want_trim" ]; then
    printf '  ok   %-28s %8s B   => %s\n' "$name" "$sz" "$want_trim"
  else
    printf '  FAIL %-28s %8s B   cli=<%s> want=<%s>\n' "$name" "$sz" "$got_trim" "$want_trim"
    fail=1
  fi
  i=$((i + 1))
done

if [ "$fail" -ne 0 ]; then
  echo "FAILED: one or more fixtures disagree with the cel CLI" >&2
  exit 1
fi
echo "All $count fixtures generated and cross-checked against the cel CLI."
