// The `cew_*` ("cel-wasm") export surface of `compiler.wasm` — the
// JS-friendly layer over the `bindings/c/compiler` C ABI.  See
// compiler_wasm_exports.cc for the calling convention; the short form:
//
//   ptr = cew_alloc(n); write a serialized `celwasm.compile.CompileRequest`
//   (compile_request.proto) at ptr; len = cew_compile(ptr, n);
//   len >= 0 -> Program bytes at cew_program(); len < 0 -> cew_error().
//
// This header exists so the native-config test
// (compiler_wasm_exports_test.cc) links the same prototypes the wasm
// binary exports — the JS caller reaches them by export name only.

#ifndef CELWASM_BINDINGS_C_COMPILER_COMPILER_WASM_EXPORTS_H_
#define CELWASM_BINDINGS_C_COMPILER_COMPILER_WASM_EXPORTS_H_

// C headers (not <cstdint>): C-compatible ABI surface, same convention
// as cel_capi.h.
#include <stdint.h>  // NOLINT(modernize-deprecated-headers)

#ifdef __cplusplus
extern "C" {
#endif

// Allocate / free a buffer in the module's linear memory for the caller
// to write the serialized request into.
void* cew_alloc(int n);
void cew_free(void* p);

// Compile the serialized `celwasm.compile.CompileRequest` at
// `request[0, request_len)`.  Returns the Program byte length (>= 0;
// read the bytes at cew_program()), or -1 on failure (read cew_error()).
int cew_compile(const uint8_t* request, int request_len);

// Result accessors for the most recent cew_compile; valid until the
// next cew_compile / cew_reset.
const uint8_t* cew_program(void);
int cew_program_len(void);
const char* cew_error(void);

// Release the stashed result (also done implicitly by the next
// cew_compile).
void cew_reset(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CELWASM_BINDINGS_C_COMPILER_COMPILER_WASM_EXPORTS_H_
