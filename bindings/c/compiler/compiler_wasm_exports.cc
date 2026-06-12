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
//   1. srcPtr  = cew_alloc(n);  write the NUL-terminated source there.
//      optsPtr = cew_alloc(m);  write the compile-option records blob
//      there (variable / function declarations, container, optimize
//      level, link mode — see ApplyOptions for the format; may be 0/null
//      for a no-option default-static compile).
//   2. len = cew_compile_opts(srcPtr, optsPtr, m);
//   3. len >= 0  -> read `len` bytes at cew_program()  (the Program wasm)
//      len <  0  -> read the NUL-terminated string at cew_error()
//   4. cew_reset() (or the next compile) releases the stashed result.
//
// Exported by name via `-Wl,--export=` in
// `bindings/c/compiler/BUILD.bazel`; the reactor's `_initialize` runs the
// C++ static constructors (protobuf
// descriptor registration etc.) before any `cew_*` call.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bindings/c/compiler/cel_capi.h"

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

// Applies one compile-option record of `kind` whose value is `val[0,rlen)`
// to `opts`. String kinds ('v'/'f'/'c') carry UTF-8 bytes (NOT
// NUL-terminated — `rlen` delimits, so multi-line `.celfn` sources are
// fine); 'o' is a 1-byte level (0..3); 'l' is a 1-byte link mode (0 =
// dynamic, non-zero = static). Returns false and sets g_error on an
// unknown kind or a declaration the builder rejects.
bool ApplyOneOption(CelCompileOpts* opts, std::uint8_t kind,
                    const std::uint8_t* val, std::uint32_t rlen) {
  switch (kind) {
    case 'v': {
      const std::string decl(reinterpret_cast<const char*>(val), rlen);
      char* err = nullptr;
      if (cel_compile_opts_declare_var(opts, decl.c_str(), &err) !=
          CEL_STATUS_OK) {
        g_error = (err != nullptr) ? err : DupError("variable decl failed");
        return false;
      }
      cel_free(err);
      return true;
    }
    case 'f': {
      const std::string decl(reinterpret_cast<const char*>(val), rlen);
      if (cel_compile_opts_declare_host_fn(opts, decl.c_str()) !=
          CEL_STATUS_OK) {
        g_error = DupError("function declaration failed");
        return false;
      }
      return true;
    }
    case 'c': {
      const std::string container(reinterpret_cast<const char*>(val), rlen);
      cel_compile_opts_set_container(opts, container.c_str());
      return true;
    }
    case 'o':
      if (rlen >= 1) {
        cel_compile_opts_set_optimize_level(opts, val[0]);
      }
      return true;
    case 'l':
      if (rlen >= 1) {
        cel_compile_opts_set_link_mode(
            opts, val[0] != 0 ? CEL_LINK_MODE_STATIC : CEL_LINK_MODE_DYNAMIC);
      }
      return true;
    case 'd':
      // A binary FileDescriptorSet for proto-typed expressions; the C ABI
      // builds a pool over the generated pool from these bytes.
      if (cel_compile_opts_set_descriptor_set(
              opts, val, static_cast<int>(rlen)) != CEL_STATUS_OK) {
        g_error = DupError("compile options: invalid descriptor set");
        return false;
      }
      return true;
    default:
      g_error = DupError("compile options: unknown record kind");
      return false;
  }
}

// Applies a sequence of compile-option records to `opts`. Each record is
//   u8  kind     'v' var | 'f' fn | 'c' container | 'o' optimize | 'l' link |
//                'd' descriptor-set (binary FileDescriptorSet bytes)
//   u32 len      (little-endian) — the value byte count
//   u8  value[len]
// Returns false and sets g_error on a malformed record or a declaration
// the builder rejects. (A single structured blob beats a multi-call
// builder marshalled across the wasm boundary, and covers fns + container
// + optimize + link, not just variables.)
bool ApplyOptions(CelCompileOpts* opts, const std::uint8_t* options, int len) {
  int pos = 0;
  while (pos < len) {
    if (pos + 5 > len) {
      g_error = DupError("compile options: truncated record header");
      return false;
    }
    const std::uint8_t kind = options[pos];
    const std::uint32_t rlen =
        static_cast<std::uint32_t>(options[pos + 1]) |
        (static_cast<std::uint32_t>(options[pos + 2]) << 8) |
        (static_cast<std::uint32_t>(options[pos + 3]) << 16) |
        (static_cast<std::uint32_t>(options[pos + 4]) << 24);
    pos += 5;
    if (rlen > static_cast<std::uint32_t>(len - pos)) {
      g_error = DupError("compile options: record overruns buffer");
      return false;
    }
    if (!ApplyOneOption(opts, kind, options + pos, rlen)) {
      return false;
    }
    pos += static_cast<int>(rlen);
  }
  return true;
}

}  // namespace

extern "C" {

// Allocate / free a buffer in the module's linear memory for the JS caller
// to write input strings into.  (Plain malloc/free; exported so JS has a
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

// Compile `source` (NUL-terminated) with the compile-option records in
// `options` (`options_len` bytes; see ApplyOptions for the format — may be
// null/0 for no options, which compiles a no-variable expression in the
// default STATIC link mode). Returns the Program byte length (>= 0; read
// the bytes at cew_program()), or -1 on failure (read cew_error()).
int cew_compile_opts(const char* source, const std::uint8_t* options,
                     int options_len) {
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
  if (options != nullptr && options_len > 0 &&
      !ApplyOptions(opts, options, options_len)) {
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

// Legacy wrapper: a newline-separated "name:type" var_decls string + a
// `dynamic` flag (0 = static, non-zero = dynamic). Builds an options blob
// and delegates to cew_compile_opts. Prefer cew_compile_opts for new code
// (it also carries function declarations, container, and optimize level).
int cew_compile(const char* source, const char* var_decls, int dynamic) {
  std::string blob;
  const auto push = [&blob](char kind, const char* data, std::uint32_t n) {
    blob.push_back(kind);
    blob.push_back(static_cast<char>(n & 0xff));
    blob.push_back(static_cast<char>((n >> 8) & 0xff));
    blob.push_back(static_cast<char>((n >> 16) & 0xff));
    blob.push_back(static_cast<char>((n >> 24) & 0xff));
    blob.append(data, n);
  };
  if (var_decls != nullptr) {
    const char* cur = var_decls;
    while (*cur != '\0') {
      const char* nl = std::strchr(cur, '\n');
      const std::uint32_t n =
          (nl != nullptr) ? static_cast<std::uint32_t>(nl - cur)
                          : static_cast<std::uint32_t>(std::strlen(cur));
      if (n > 0) {
        push('v', cur, n);
      }
      cur = (nl != nullptr) ? nl + 1 : cur + n;
    }
  }
  const char link_mode = dynamic != 0 ? 0 : 1;
  push('l', &link_mode, 1);
  return cew_compile_opts(source,
                          reinterpret_cast<const std::uint8_t*>(blob.data()),
                          static_cast<int>(blob.size()));
}

}  // extern "C"
