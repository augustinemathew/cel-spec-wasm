// JS-friendly export layer for `compiler.wasm` — the CEL compiler
// cross-compiled to wasm32-wasi so a browser (or any WASI host) can
// compile CEL source to a portable `Program` with no native toolchain.
//
// The module is a wasi-sdk *reactor* (exports functions + memory, no
// `_start`).  These `cew_*` ("cel-wasm") functions wrap the `bindings/c`
// C ABI with signatures that are simple to call from JavaScript — no
// `uint8_t**` / `size_t*` / `char**` out-parameters, which are painful to
// marshal through `WebAssembly.Memory`.  The JS caller:
//
//   1. ptr = cew_alloc(n);  write the NUL-terminated source there (and,
//      optionally, a NUL-terminated, newline-separated list of
//      "name:type" variable declarations).
//   2. len = cew_compile(srcPtr, varDeclsPtr);
//   3. len >= 0  -> read `len` bytes at cew_program()  (the Program wasm)
//      len <  0  -> read the NUL-terminated string at cew_error()
//   4. cew_reset() (or the next cew_compile) releases the stashed result.
//
// Exported by name via `-Wl,--export=` in `bindings/c/BUILD.bazel`; the
// reactor's `_initialize` runs the C++ static constructors (protobuf
// descriptor registration etc.) before any `cew_*` call.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "bindings/c/cel_capi.h"

namespace {

// One-shot result state for the most recent cew_compile.
std::uint8_t* g_program = nullptr;
int g_program_len = 0;
char* g_error = nullptr;

// Duplicates a C string with the C ABI's allocator family (plain malloc),
// so cew_reset can release it uniformly via cel_free.
char* DupError(const char* msg) {
  const std::size_t n = std::strlen(msg) + 1;
  // C-ABI/JS boundary: the string is released by cew_reset via the C ABI's
  // cel_free (plain free).
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
  char* out = static_cast<char*>(std::malloc(n));
  if (out != nullptr) {
    std::memcpy(out, msg, n);
  }
  return out;
}

// Declares each newline-separated "name:type" entry in `var_decls` (a
// mutable, NUL-terminated buffer the caller owns) on `opts`.  Returns
// false and sets g_error on the first malformed declaration.
bool DeclareVars(CelCompileOpts* opts, char* var_decls) {
  if (var_decls == nullptr || var_decls[0] == '\0') {
    return true;
  }
  char* cursor = var_decls;
  while (cursor != nullptr && *cursor != '\0') {
    char* nl = std::strchr(cursor, '\n');
    if (nl != nullptr) {
      *nl = '\0';
    }
    if (cursor[0] != '\0') {
      char* err = nullptr;
      if (cel_compile_opts_declare_var(opts, cursor, &err) != CEL_STATUS_OK) {
        g_error = (err != nullptr) ? err : DupError("variable declaration failed");
        return false;
      }
      cel_free(err);
    }
    cursor = (nl != nullptr) ? nl + 1 : nullptr;
  }
  return true;
}

}  // namespace

extern "C" {

// Allocate / free a buffer in the module's linear memory for the JS caller
// to write input strings into.  (Plain malloc/free; exported so JS has a
// stable allocator that matches what the C ABI frees.)
// NOLINTNEXTLINE(cppcoreguidelines-no-malloc) — the JS host's allocator.
void* cew_alloc(int n) { return std::malloc(static_cast<std::size_t>(n)); }
// NOLINTNEXTLINE(cppcoreguidelines-no-malloc) — frees a cew_alloc buffer.
void cew_free(void* p) { std::free(p); }

const std::uint8_t* cew_program(void) { return g_program; }
int cew_program_len(void) { return g_program_len; }
const char* cew_error(void) { return g_error; }

void cew_reset(void) {
  if (g_program != nullptr) {
    cel_free(g_program);
    g_program = nullptr;
  }
  if (g_error != nullptr) {
    cel_free(g_error);
    g_error = nullptr;
  }
  g_program_len = 0;
}

// Compile `source` (NUL-terminated) with the optional newline-separated
// "name:type" declarations in `var_decls` (NUL-terminated, may be null).
// Returns the Program byte length (>= 0; read the bytes at cew_program()),
// or -1 on failure (read the diagnostic at cew_error()).
int cew_compile(const char* source, char* var_decls) {
  cew_reset();
  if (source == nullptr) {
    g_error = DupError("null source");
    return -1;
  }
  CelCompileOpts* opts = cel_compile_opts_new();
  if (opts == nullptr) {
    g_error = DupError("out of memory");
    return -1;
  }
  if (!DeclareVars(opts, var_decls)) {
    cel_compile_opts_free(opts);
    return -1;
  }

  std::uint8_t* wasm = nullptr;
  std::size_t len = 0;
  char* err = nullptr;
  const CelStatus status = cel_compile(source, opts, &wasm, &len, &err);
  cel_compile_opts_free(opts);

  if (status != CEL_STATUS_OK) {
    g_error = (err != nullptr) ? err : DupError("compile failed");
    return -1;
  }
  cel_free(err);
  g_program = wasm;
  g_program_len = static_cast<int>(len);
  return g_program_len;
}

}  // extern "C"
