#!/usr/bin/env bash
# Feasibility probe for the emscripten compiler.wasm backend.
#
# This builds the *codegen + optimizer half* of the CEL compiler under
# emscripten: Binaryen (the C API the codegen lowers into) compiled to a
# static wasm-side library, linked into a tiny program that drives the
# binaryen-c.h API to emit + optimize a wasm module, and run in Node.
#
# It does NOT yet build the parse/check half (cel-cpp + protobuf + ANTLR4 +
# RE2 + absl) — that is the unfinished, multi-day part of the port. See
# doc/implementation-plan/rewrite/m29-wi24-emscripten-spike.md for the
# go/no-go and the remaining work.
#
# What an embedder needs (none of this is in the repo's hermetic build —
# emscripten is an OPTIONAL stretch backend, deliberately out-of-tree to
# honor the "no platform-specific deps in the core build" rule):
#   - emsdk (https://github.com/emscripten-core/emscripten — git clone +
#     `./emsdk install latest && ./emsdk activate latest`); provides its own
#     pinned clang + node, so no system toolchain dependency.
#   - cmake (for Binaryen, which builds via its own CMakeLists.txt).
#
# Usage:
#   EMSDK_DIR=/path/to/emsdk BINARYEN_SRC=/path/to/binaryen-version_129 \
#     bindings/c/emscripten/build_binaryen_probe.sh
#
# Both env vars are required; the script makes no assumption about where the
# embedder put emsdk or the Binaryen source (the version is pinned in
# MODULE.bazel to version_129 — match it).
set -euo pipefail

: "${EMSDK_DIR:?set EMSDK_DIR to your emsdk checkout}"
: "${BINARYEN_SRC:?set BINARYEN_SRC to the extracted binaryen-version_129 dir}"

OUT_DIR="${OUT_DIR:-/tmp/cel-wi24-emscripten}"
mkdir -p "${OUT_DIR}"

# shellcheck disable=SC1091
source "${EMSDK_DIR}/emsdk_env.sh" >/dev/null 2>&1

echo "== emcc =="
emcc --version | head -1

echo "== configure + build Binaryen static lib (emscripten) =="
BUILD_DIR="${OUT_DIR}/binaryen-build"
mkdir -p "${BUILD_DIR}"
emcmake cmake -S "${BINARYEN_SRC}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=OFF \
  -DBUILD_TOOLS=OFF \
  -DBUILD_STATIC_LIB=ON \
  -DENABLE_WERROR=OFF
emmake make -C "${BUILD_DIR}" binaryen -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

LIB="${BUILD_DIR}/lib/libbinaryen.a"
test -f "${LIB}" || { echo "FAIL: ${LIB} not produced"; exit 1; }
echo "built: ${LIB} ($(wc -c <"${LIB}") bytes)"

echo "== link the binaryen-c.h driver probe =="
PROBE_SRC="${OUT_DIR}/binaryen_probe.cc"
cat >"${PROBE_SRC}" <<'CC'
#include <cstdio>
#include "binaryen-c.h"
// Mirror what compiler/codegen does: build (i32.add 1 2) as $eval, validate,
// optimize at -O2, serialize. This is the codegen+optimize half of the
// compiler, running inside an emscripten wasm module.
extern "C" int run_compile() {
  BinaryenModuleRef m = BinaryenModuleCreate();
  BinaryenExpressionRef body = BinaryenBinary(
      m, BinaryenAddInt32(),
      BinaryenConst(m, BinaryenLiteralInt32(1)),
      BinaryenConst(m, BinaryenLiteralInt32(2)));
  BinaryenAddFunction(m, "eval", BinaryenTypeNone(), BinaryenTypeInt32(),
                      nullptr, 0, body);
  BinaryenAddFunctionExport(m, "eval", "eval");
  bool ok = BinaryenModuleValidate(m);
  BinaryenSetOptimizeLevel(2);
  BinaryenModuleOptimize(m);
  BinaryenModuleAllocateAndWriteResult out =
      BinaryenModuleAllocateAndWrite(m, nullptr);
  int n = static_cast<int>(out.binaryBytes);
  free(out.binary);
  BinaryenModuleDispose(m);
  return ok ? n : -1;
}
int main() {
  printf("emitted wasm bytes = %d\n", run_compile());
  return 0;
}
CC

em++ -O2 -std=c++17 \
  -I "${BINARYEN_SRC}/src" \
  "${PROBE_SRC}" "${LIB}" \
  -o "${OUT_DIR}/binaryen_probe.js"

echo "== run the probe in Node =="
node "${OUT_DIR}/binaryen_probe.js"

echo "OK: Binaryen (codegen half) is emscripten-feasible."
