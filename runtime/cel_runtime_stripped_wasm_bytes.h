// Embedded bytes of the wasi-libc-wrapper-stripped variant of
// cel_runtime.wasm.  Parallel to `cel_runtime_wasm_bytes.h`; used by
// `CompilerOptions::link_mode == kStatic` to merge a self-contained
// runtime into each compiled Program.
//
// Produced at our build time by `//runtime:strip_command_wrappers`
// running over the regular `cel_runtime_wasm.bin`.  See
// `doc/implementation-plan/rewrite/m28-configurable-linking.md` §5.2
// and `runtime/strip_command_wrappers.cc` for the rationale.

#ifndef CELWASM_RUNTIME_CEL_RUNTIME_STRIPPED_WASM_BYTES_H_
#define CELWASM_RUNTIME_CEL_RUNTIME_STRIPPED_WASM_BYTES_H_

namespace celwasm {

extern const unsigned char kCelRuntimeStrippedWasmBytes[];
extern const unsigned int kCelRuntimeStrippedWasmBytesSize;

}  // namespace celwasm

#endif  // CELWASM_RUNTIME_CEL_RUNTIME_STRIPPED_WASM_BYTES_H_
