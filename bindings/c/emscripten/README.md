# `bindings/c/emscripten` — browser `compiler.wasm` backend (stretch)

This dir holds the build for the **emscripten** backend of the TS compiler
binding: the C ABI (`bindings/c/cel_capi.*`, owned by WI-2.1) + the C++
`Compiler` + cel-cpp + Binaryen cross-compiled to a `compiler.wasm` so a
browser can compile CEL→Program with no server round-trip.

**Status: feasibility spike done, full backend NOT yet built.** See
`doc/implementation-plan/rewrite/m29-wi24-emscripten-spike.md` for the
go/no-go. Short version: the Binaryen (codegen+optimize) half is proven
feasible under emscripten today; the parse/check half (protobuf + ANTLR4 +
cel-cpp) is a multi-day toolchain port that is a standalone follow-up, not
on the v1 critical path. The browser demo ships v1 on the N-API native
backend + a thin local compile endpoint.

emscripten is an **optional, out-of-tree** dependency — it is deliberately
not part of the hermetic bazel build (the core build forbids
platform-specific deps). An embedder pursuing the browser backend installs
emsdk + cmake locally.

## `build_binaryen_probe.sh`

The reproducible feasibility probe for the de-risked half. It builds
`libbinaryen.a` under emscripten and runs the `binaryen-c.h` API (build +
validate + optimize + serialize a wasm module) inside a Node-hosted wasm
module.

```bash
# obtain emsdk (self-contained: its own clang + node, no system toolchain)
git clone https://github.com/emscripten-core/emscripten emsdk   # = emsdk repo
cd emsdk && ./emsdk install latest && ./emsdk activate latest && cd ..

# Binaryen source, pinned to MODULE.bazel's version_129
curl -sL https://github.com/WebAssembly/binaryen/archive/refs/tags/version_129.tar.gz \
  | tar xz

EMSDK_DIR="$PWD/emsdk" BINARYEN_SRC="$PWD/binaryen-version_129" \
  bindings/c/emscripten/build_binaryen_probe.sh
# => "emitted wasm bytes = 37" / "OK: Binaryen (codegen half) is emscripten-feasible."
```
