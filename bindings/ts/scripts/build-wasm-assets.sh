#!/usr/bin/env bash
# Build the wasm artifacts the TS bindings consume, from the bazel C++/wasm
# build, and copy them into the (git-ignored) locations the dev server, the
# compiler package, and the eval package read:
#
#   //bindings/c:compiler_wasm   -> compiler/wasm/compiler.wasm  (the
#                                   @cel-wasm/compiler package's own
#                                   in-process CEL compiler, the default
#                                   compile backend, ~56 MB) AND
#                                   web/public/compiler.wasm   (the SPA's
#                                   client-side CEL compiler — same bytes,
#                                   fetched over HTTP by the browser)
#   //runtime:cel_runtime_wasm   -> web/public/cel_runtime.wasm AND
#                                   eval/runtime/cel_runtime.wasm  (the
#                                   shared dynamic-link runtime, ~2.8 MB)
#
# These artifacts are NOT checked into git (they bloat history and git
# handles large binaries poorly).  Locally, run this once (the npm `predev`
# hook does it for you) — warm bazel makes it near-instant.  In CI they are
# built once per release and published as GitHub Release assets; the Pages
# build downloads them rather than rebuilding.
set -euo pipefail

# Repo root is two levels up from bindings/ts/scripts/.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TS_ROOT="${REPO_ROOT}/bindings/ts"

# CI (the Pages build) provides the assets out-of-band — downloaded from the
# GitHub Release — and has no bazel/C++ toolchain.  `CEL_SKIP_WASM_BUILD=1`
# (or simply no `bazel` on PATH) means "trust the assets that are already in
# place"; the `predev`/`prebuild` npm hooks can then run unconditionally.
if [[ "${CEL_SKIP_WASM_BUILD:-}" == "1" ]] || ! command -v bazel >/dev/null 2>&1; then
  echo "build-wasm-assets: skipping bazel build (CEL_SKIP_WASM_BUILD set or no" \
       "bazel on PATH); assuming the wasm assets are already in place."
  exit 0
fi

echo "build-wasm-assets: building wasm artifacts via bazel..."
(cd "${REPO_ROOT}" && bazel build //bindings/c:compiler_wasm //runtime:cel_runtime_wasm)

COMPILER_WASM="${REPO_ROOT}/bazel-bin/bindings/c/compiler_wasm.wasm"
RUNTIME_WASM="${REPO_ROOT}/bazel-bin/runtime/cel_runtime_wasm.wasm"

# bazel outputs are read-only; copy then chmod so a re-run can overwrite.
install_asset() {
  local src="$1" dst="$2"
  mkdir -p "$(dirname "${dst}")"
  rm -f "${dst}"
  cp -L "${src}" "${dst}"
  chmod u+w "${dst}"
  echo "  $(basename "${dst}") <- $(basename "${src}")  ($(wc -c < "${dst}" | tr -d ' ') bytes)"
}

install_asset "${COMPILER_WASM}" "${TS_ROOT}/compiler/wasm/compiler.wasm"
install_asset "${COMPILER_WASM}" "${TS_ROOT}/web/public/compiler.wasm"
install_asset "${RUNTIME_WASM}" "${TS_ROOT}/web/public/cel_runtime.wasm"
install_asset "${RUNTIME_WASM}" "${TS_ROOT}/eval/runtime/cel_runtime.wasm"

echo "build-wasm-assets: done."
