// Probe E3 — does a bazel cc_toolchain for wasm32-wasi produce a
// wasm artifact?  Minimal canary: a single exported `add` function.
//
// The toolchain config in cc_toolchain_config.bzl wires:
//   - tool_paths to @wasi_sdk//:clang++ / clang / llvm-ar / llvm-ranlib
//   - compile_flags --target=wasm32-wasi + the sysroot
//   - link_flags -nostartfiles -Wl,--no-entry -Wl,--export=add

extern "C" {

__attribute__((export_name("add")))
int add(int a, int b) {
  return a + b;
}

}  // extern "C"
