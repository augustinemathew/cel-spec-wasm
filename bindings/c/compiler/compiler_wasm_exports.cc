// JS-friendly export layer for `compiler.wasm` — the CEL compiler
// cross-compiled to wasm32-wasi so a browser (or any WASI host) can
// compile CEL source to a portable `Program` with no native toolchain.
//
// The module is a wasi-sdk *reactor* (exports functions + memory, no
// `_start`).  These `cew_*` ("cel-wasm") functions wrap the
// `bindings/c/compiler` C ABI with signatures that are simple to call from
// JavaScript — no
// `uint8_t**` / `size_t*` / `char**` out-parameters, which are painful to
// marshal through `WebAssembly.Memory`.  The JS caller:
//
//   1. ptr = cew_alloc(n);  write a serialized `celwasm.compile.
//      CompileRequest` (bindings/c/compiler/compile_request.proto) there —
//      the source plus every compile option in one length-delimited
//      buffer.
//   2. len = cew_compile(ptr, n);
//   3. len >= 0  -> read `len` bytes at cew_program()  (the Program wasm)
//      len <  0  -> read the NUL-terminated string at cew_error()
//   4. cew_reset() (or the next compile) releases the stashed result.
//
// Because the request (and the `source` field inside it) is
// length-delimited, an expression carrying an embedded NUL byte (a CEL
// `b'\x00'` byte literal) compiles correctly — there is no
// NUL-terminated `const char*` crossing for it to truncate.
//
// Exported by name via `-Wl,--export=` in
// `bindings/c/compiler/BUILD.bazel`; the reactor's `_initialize` runs the
// C++ static constructors (protobuf
// descriptor registration etc.) before any `cew_*` call.

#include "bindings/c/compiler/compiler_wasm_exports.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bindings/c/compiler/cel_capi.h"
#include "bindings/c/compiler/compile_request.pb.h"

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

// Applies every option field of the decoded request onto the C-ABI
// options builder.  Returns false and sets g_error on a declaration the
// builder rejects, an invalid descriptor set, or a link mode this build
// does not know (open-set wire data from a newer schema).
bool ApplyRequest(const celwasm::compile::CompileRequest& request,
                  CelCompileOpts* opts) {
  for (const auto& var : request.variables()) {
    const std::string decl = var.name() + ":" + var.type();
    char* err = nullptr;
    if (cel_compile_opts_declare_var(opts, decl.c_str(), &err) !=
        CEL_STATUS_OK) {
      g_error = (err != nullptr) ? err : DupError("variable decl failed");
      return false;
    }
    cel_free(err);
  }
  for (const std::string& fn : request.fns()) {
    if (cel_compile_opts_declare_host_fn(opts, fn.c_str()) != CEL_STATUS_OK) {
      g_error = DupError("function declaration failed");
      return false;
    }
  }
  cel_compile_opts_set_container(opts, request.container().c_str());
  cel_compile_opts_set_optimize_level(
      opts, static_cast<int>(request.optimize_level()));
  switch (request.link_mode()) {
    case celwasm::compile::LINK_MODE_DYNAMIC:
      cel_compile_opts_set_link_mode(opts, CEL_LINK_MODE_DYNAMIC);
      break;
    case celwasm::compile::LINK_MODE_STATIC:
      cel_compile_opts_set_link_mode(opts, CEL_LINK_MODE_STATIC);
      break;
    default:
      // Open wire data — a newer schema's mode this build can't honour.
      g_error = DupError("compile request: unsupported link_mode");
      return false;
  }
  if (!request.descriptor_set().empty()) {
    if (cel_compile_opts_set_descriptor_set(
            opts,
            reinterpret_cast<const std::uint8_t*>(
                request.descriptor_set().data()),
            static_cast<int>(request.descriptor_set().size())) !=
        CEL_STATUS_OK) {
      g_error = DupError("compile request: invalid descriptor set");
      return false;
    }
  }
  return true;
}

}  // namespace

extern "C" {

// Allocate / free a buffer in the module's linear memory for the JS caller
// to write input bytes into.  (Plain malloc/free; exported so JS has a
// stable allocator that matches what the C ABI frees.)
void* cew_alloc(int n) {
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) — the JS host's allocator.
  return std::malloc(static_cast<std::size_t>(n));
}
void cew_free(void* p) {
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) — frees a cew_alloc buffer.
  std::free(p);
}

const std::uint8_t* cew_program(void) {
  return g_program;
}
int cew_program_len(void) {
  return g_program_len;
}
const char* cew_error(void) {
  return g_error;
}

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

// Compile the serialized `celwasm.compile.CompileRequest` at
// `request[0, request_len)` (bindings/c/compiler/compile_request.proto):
// the CEL source plus every compile option in one proto message.
// Returns the Program byte length (>= 0; read the bytes at
// cew_program()), or -1 on failure (read cew_error()).
int cew_compile(const std::uint8_t* request, int request_len) {
  cew_reset();
  if (request == nullptr || request_len <= 0) {
    g_error = DupError("null compile request");
    return -1;
  }
  celwasm::compile::CompileRequest req;
  if (!req.ParseFromArray(request, request_len)) {
    g_error = DupError("compile request: malformed CompileRequest proto");
    return -1;
  }
  CelCompileOpts* opts = cel_compile_opts_new();
  if (opts == nullptr) {
    g_error = DupError("out of memory");
    return -1;
  }
  if (!ApplyRequest(req, opts)) {
    cel_compile_opts_free(opts);
    return -1;
  }

  std::uint8_t* wasm = nullptr;
  std::size_t len = 0;
  char* err = nullptr;
  // cel_compile_n, not cel_compile: the source is length-delimited so an
  // embedded NUL byte inside it survives the crossing.
  const CelStatus status = cel_compile_n(
      req.source().data(), req.source().size(), opts, &wasm, &len, &err);
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
