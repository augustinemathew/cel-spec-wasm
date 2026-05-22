#!/usr/bin/env bash
# Regenerate rules.wasm from rules.go via TinyGo.
#
# Run this manually whenever rules.go changes.  The output is
# checked into the tree (a probe artifact, not a build product)
# so the M13 probes can run without depending on TinyGo being
# present in CI / on every contributor's machine.
#
# Requires: TinyGo on PATH (brew tap tinygo-org/tools && brew install tinygo).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

if ! command -v tinygo >/dev/null 2>&1; then
  echo "error: tinygo not on PATH" >&2
  echo "install with: brew tap tinygo-org/tools && brew install tinygo" >&2
  exit 1
fi

echo "Building rules.wasm with $(tinygo version)..."
tinygo build -target=wasm-unknown -no-debug -o rules.wasm .

# Sanity check: confirm the canonical export name lands.  Anything
# else would silently break Probe 2.
if command -v wasm2wat >/dev/null 2>&1; then
  if ! wasm2wat rules.wasm | grep -q '"allow_string_string"'; then
    echo "error: rules.wasm missing allow_string_string export" >&2
    exit 1
  fi
  echo "  ✓ exports allow_string_string"
fi

ls -la rules.wasm
