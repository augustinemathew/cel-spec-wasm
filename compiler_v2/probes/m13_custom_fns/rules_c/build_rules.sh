#!/usr/bin/env bash
# Build rules.wasm from rules.c via brew's LLVM clang.
#
# Apple's bundled clang has the wasm32 backend but its lld doesn't
# accept --target=wasm32 the way brew's LLVM lld does.  So we
# explicitly pick the brew LLVM toolchain.  Install with:
#
#   brew install llvm
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

CLANG="/opt/homebrew/opt/llvm/bin/clang"
if [[ ! -x "${CLANG}" ]]; then
  echo "error: ${CLANG} not found" >&2
  echo "install with: brew install llvm" >&2
  exit 1
fi

echo "Building rules.wasm with $(${CLANG} --version | head -1)..."

# -nostdlib: no libc, no startup code, no _initialize
# -Wl,--no-entry: no _start; we're not an app, just exports
# -Wl,--export-dynamic: keep all `export_name`-annotated symbols
# -fvisibility=hidden + explicit export_name attrs keep the export
#   list minimal (just `allow_string_string` and the toolchain-
#   defaults `memory` + `__stack_pointer`).
"${CLANG}" --target=wasm32 \
  -nostdlib \
  -Wl,--no-entry \
  -Wl,--export-dynamic \
  -Wl,--initial-memory=131072 \
  -fvisibility=hidden \
  -O2 \
  -o rules.wasm rules.c
# --initial-memory=131072 = 2 wasm pages × 64 KiB.  The caller
# WAT imports `(memory 2)` (min 2 pages); a 1-page memory fails
# to link.  Matches probe 2's TinyGo default.

if command -v wasm2wat >/dev/null 2>&1; then
  if ! wasm2wat rules.wasm | grep -q '"allow_string_string"'; then
    echo "error: rules.wasm missing allow_string_string export" >&2
    exit 1
  fi
  echo "  ✓ exports allow_string_string"
fi

ls -la rules.wasm
