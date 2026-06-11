// WASI entry point for `compiler.wasm` — the CEL compiler cross-compiled
// to wasm32-wasi so a browser (or any WASI host) can compile CEL source
// to a portable `Program` with no native toolchain.  It is a thin CLI
// over the `bindings/c` C ABI, deliberately mirroring the I/O contract of
// the native `cel compile` subcommand so the TypeScript `compileWasm()`
// backend is structurally identical to the subprocess backend: source +
// variable declarations in via argv, Program wasm bytes out via stdout,
// diagnostics out via stderr, non-zero exit on failure.
//
//   compiler.wasm <expr> [name:type ...]
//
// Build via `//bindings/c:compiler_wasm` (a wasm32-wasi `cc_binary` under
// a transition that enables C++ exceptions/RTTI for the cel-cpp + Binaryen
// dependencies — see `third_party/wasi_sdk/wasm_cc_binary.bzl`).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "bindings/c/cel_capi.h"

namespace {

// Frees an `out_err` string from the C ABI and returns `code` so callers
// can `return Fail(err, 1)` without leaking.
int Fail(char* err, const char* context, int code) {
  std::fprintf(stderr, "%s: %s\n", context, err != nullptr ? err : "(no detail)");
  cel_free(err);
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: compiler.wasm <expr> [name:type ...]\n");
    return 2;
  }

  CelCompileOpts* opts = cel_compile_opts_new();
  if (opts == nullptr) {
    std::fprintf(stderr, "out of memory allocating compile options\n");
    return 1;
  }

  // argv[2..] are `name:type` variable declarations (the same grammar the
  // native CLI's `--var name:Type` accepts, declaration-only here).
  for (int i = 2; i < argc; ++i) {
    char* err = nullptr;
    if (cel_compile_opts_declare_var(opts, argv[i], &err) != CEL_STATUS_OK) {
      cel_compile_opts_free(opts);
      return Fail(err, "variable declaration", 1);
    }
    cel_free(err);
  }

  std::uint8_t* wasm = nullptr;
  std::size_t len = 0;
  char* err = nullptr;
  const CelStatus status = cel_compile(argv[1], opts, &wasm, &len, &err);
  cel_compile_opts_free(opts);

  if (status != CEL_STATUS_OK) {
    return Fail(err, "compile", 1);
  }
  cel_free(err);

  if (std::fwrite(wasm, 1, len, stdout) != len) {
    cel_free(wasm);
    std::fprintf(stderr, "failed to write %zu Program bytes to stdout\n", len);
    return 1;
  }
  cel_free(wasm);
  return 0;
}
