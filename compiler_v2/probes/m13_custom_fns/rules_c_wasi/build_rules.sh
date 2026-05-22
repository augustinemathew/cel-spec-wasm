#!/usr/bin/env bash
# Build rules.wasm from rules.c using brew's LLVM clang + brew's
# wasi-libc sysroot.  Produces a wasm-32 reactor-mode module —
# what wasi-sdk would emit for a "library, not a CLI app" use case.
#
# Install prereqs:
#   brew install llvm wasi-libc
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

CLANG="/opt/homebrew/opt/llvm/bin/clang"
SYSROOT="/opt/homebrew/opt/wasi-libc/share/wasi-sysroot"

for path in "${CLANG}" "${SYSROOT}"; do
  if [[ ! -e "$path" ]]; then
    echo "error: missing ${path}" >&2
    echo "install with: brew install llvm wasi-libc" >&2
    exit 1
  fi
done

echo "Building rules.wasm with $(${CLANG} --version | head -1) + wasi-libc..."

# -mexec-model=reactor: library mode (no main); emits `_initialize`
#                       instead of `_start`.  Our engine policy is
#                       "call _initialize if exported," so the probe
#                       harness must call it.
# --initial-memory=131072: 2 wasm pages (matches the caller's
#                          (memory 2) import minimum).  WASI defaults
#                          to a larger memory; pinning keeps the
#                          probe deterministic.
# Brew layouts:
#   wasi-libc       — sysroot (headers + libc.a)
#   wasi-runtimes   — compiler-rt builtins at wasm32-unknown-wasip1/
# We tell clang to look in wasi-runtimes' resource-dir for the
# builtins it needs.
WASI_RUNTIMES_RESDIR="$(brew --prefix wasi-runtimes)/share/wasi-runtimes"

"${CLANG}" \
  --target=wasm32-wasip1 \
  --sysroot="${SYSROOT}" \
  -resource-dir="${WASI_RUNTIMES_RESDIR}" \
  -mexec-model=reactor \
  -Wl,--initial-memory=131072 \
  -fvisibility=hidden \
  -O2 \
  -o rules.wasm rules.c

if command -v wasm2wat >/dev/null 2>&1; then
  if ! wasm2wat rules.wasm | grep -q '"allow_string_string"'; then
    echo "error: rules.wasm missing allow_string_string export" >&2
    exit 1
  fi
  if ! wasm2wat rules.wasm | grep -q '"_initialize"'; then
    echo "error: rules.wasm missing _initialize export (reactor mode)" >&2
    exit 1
  fi
  echo "  ✓ exports allow_string_string"
  echo "  ✓ exports _initialize (reactor mode)"
fi

ls -la rules.wasm
