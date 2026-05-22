#!/usr/bin/env bash
# Build rules.wasm from rules.rs via rustup-managed rustc.
#
# Requires:
#   - rustup (brew install rustup; then rustup-init)
#   - `rustup target add wasm32-unknown-unknown`
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Locate the rustup-managed stable rustc.  Brew's package-manager
# Rust doesn't ship the wasm32 std crate; we must use rustup's.
RUSTC=""
for cand in \
    "$HOME/.rustup/toolchains/stable-aarch64-apple-darwin/bin/rustc" \
    "$HOME/.rustup/toolchains/stable-x86_64-apple-darwin/bin/rustc"; do
  if [[ -x "$cand" ]]; then
    RUSTC="$cand"
    break
  fi
done

if [[ -z "$RUSTC" ]]; then
  echo "error: rustup-managed rustc not found" >&2
  echo "install with:" >&2
  echo "  brew install rustup" >&2
  echo "  rustup-init -y --default-toolchain stable" >&2
  echo "  rustup target add wasm32-unknown-unknown" >&2
  exit 1
fi

echo "Building rules.wasm with $($RUSTC --version)..."

"$RUSTC" \
  --target=wasm32-unknown-unknown \
  --crate-type=cdylib \
  -C opt-level=z \
  -C strip=symbols \
  -o rules.wasm rules.rs

if command -v wasm2wat >/dev/null 2>&1; then
  if ! wasm2wat rules.wasm | grep -q '"allow_string_string"'; then
    echo "error: rules.wasm missing allow_string_string export" >&2
    exit 1
  fi
  echo "  ✓ exports allow_string_string"
fi

ls -la rules.wasm
