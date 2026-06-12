// C ABI over the C++ `celwasm::Compiler` (compiler/compiler.h).
//
// This is the `extern "C"` seam the language bindings build on:
// an N-API native addon (Node) and an emscripten `compiler.wasm`
// (browser) both call through this surface, so neither has to bind
// the C++ class layout directly.  It is a THIN marshalling shim —
// it holds no compile logic; every call forwards into
// `celwasm::Compiler::Builder` / `Compiler::Compile` and maps the
// returned `absl::StatusOr<Program>` onto a C status code plus an
// out-parameter buffer.  No C++ exception or `absl::Status` object
// ever crosses the boundary.
//
// Ownership contract: any non-null `uint8_t*` / `char*` written
// into an out-parameter by a `cel_*` call is heap-allocated by this
// library and MUST be released with `cel_free`.  Opaque handles
// (`CelCompileOpts*`) are released with their matching destructor
// (`cel_compile_opts_free`).
//
// All functions are safe to call with a null library — they perform
// no global initialization.

#ifndef CELWASM_BINDINGS_C_COMPILER_CEL_CAPI_H_
#define CELWASM_BINDINGS_C_COMPILER_CEL_CAPI_H_

// C headers (not <cstddef>/<cstdint>): this is a C-compatible ABI
// header an N-API/emscripten consumer must `#include` from C.
#include <stddef.h>  // NOLINT(modernize-deprecated-headers)
#include <stdint.h>  // NOLINT(modernize-deprecated-headers)

#ifdef __cplusplus
extern "C" {
#endif

// Status codes returned by `cel_compile` and the opts builders.
// Mirrors the `absl::StatusCode` values the compiler pipeline
// surfaces (compiler/compiler.h documents the mapping), collapsed
// to the subset a caller acts on.  Any non-zero value means the
// call failed and (where the function takes an `out_err`) a
// diagnostic string was written.
// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-use-enum-class,performance-enum-size):
// this is a C-compatible ABI header — `enum class`, `using`, and a
// non-`int` enum base are not valid C, and an N-API/emscripten
// consumer must be able to `#include` it from C.
typedef enum CelStatus {
  CEL_STATUS_OK = 0,
  // Bad input the caller can fix: parse failure, type-check
  // failure, a static-subset violation, an out-of-range optimize
  // level, or a malformed variable declaration.
  CEL_STATUS_INVALID_ARGUMENT = 1,
  // An AST shape the current compiler does not lower yet.
  CEL_STATUS_UNIMPLEMENTED = 2,
  // A compiler-internal invariant failure (should not escape; file
  // a regression).  Also covers a null required argument.
  CEL_STATUS_INTERNAL = 3,
} CelStatus;

// How the runtime helpers are linked into the emitted Program.wasm.
// Mirrors `celwasm::CompilerOptions::LinkMode`.
typedef enum CelLinkMode {
  CEL_LINK_MODE_DYNAMIC = 0,
  CEL_LINK_MODE_STATIC = 1,  // The default; self-contained Program.wasm.
} CelLinkMode;

// Opaque compile-options handle.  Accumulates variable declarations
// and the per-compilation tunables (container, optimize level, link
// mode).  Construct with `cel_compile_opts_new`, configure with the
// setters, pass to `cel_compile`, then release with
// `cel_compile_opts_free`.  Not thread-safe; one handle per builder.
typedef struct CelCompileOpts CelCompileOpts;
// NOLINTEND(modernize-use-using,cppcoreguidelines-use-enum-class,performance-enum-size)

// Allocate a fresh options handle with the compiler defaults
// (empty container, optimize level 0, static link mode, no declared
// variables).  Never returns null except on allocation failure.
CelCompileOpts* cel_compile_opts_new(void);

// Release an options handle.  Safe to call with null.
void cel_compile_opts_free(CelCompileOpts* opts);

// Declare a free variable visible to the compiled expression.
// `decl` is a `name:Type` string using the same type-spec grammar
// as the `cel` CLI's `--var` flag (e.g. "x:int", "items:list<int>",
// "m:map<string,int>", "req:com.example.Req").  No `=value` suffix:
// the C ABI declares types only; activation binding is the eval
// binding's job.
//
// Returns CEL_STATUS_OK on success.  On a malformed `decl` (missing
// ':', empty name, or an unparsable type spec) returns
// CEL_STATUS_INVALID_ARGUMENT and, if `out_err` is non-null, writes
// a heap-allocated diagnostic into `*out_err` (release with
// `cel_free`).  On success `*out_err` is set to null.
CelStatus cel_compile_opts_declare_var(CelCompileOpts* opts, const char* decl,
                                       char** out_err);

// Declare a custom host function available to the expression, given
// as a `.celfn` IDL declaration string (e.g.
// "string @host.upper(this string s);").  The declaration is staged
// on the builder; a parse error surfaces from `cel_compile`, not
// here (it mirrors `Compiler::Builder::AddFunction`'s deferred-error
// contract).  Host functions are the only custom-fn mechanism in
// scope for the bindings.
//
// Returns CEL_STATUS_OK once staged (always, barring a null
// argument); the real validation happens at compile time.
CelStatus cel_compile_opts_declare_host_fn(CelCompileOpts* opts,
                                           const char* celfn_decl);

// Set the name-resolution container (CEL `container`).  Empty or
// null clears it.  Copied into the handle.
void cel_compile_opts_set_container(CelCompileOpts* opts,
                                    const char* container);

// Set the Binaryen optimize level for the emitted expr module.
// Accepted range is [0, 3]; an out-of-range value is not rejected
// here but surfaces as CEL_STATUS_INVALID_ARGUMENT from
// `cel_compile`.
void cel_compile_opts_set_optimize_level(CelCompileOpts* opts, int level);

// Set the runtime link mode (default CEL_LINK_MODE_STATIC).
void cel_compile_opts_set_link_mode(CelCompileOpts* opts, CelLinkMode mode);

// Supply a binary-serialized `google.protobuf.FileDescriptorSet` (the bytes
// `protoc --descriptor_set_out` emits) describing the message types a
// proto-typed expression / declaration references.  The bytes are parsed
// and built into a descriptor pool layered over the process-wide generated
// pool (so well-known types resolve), copied into `opts`; the caller may
// free `fds` after the call.  A null `fds` or non-positive `len` clears any
// previously-supplied set (resolution falls back to the generated pool).
// Returns CEL_STATUS_INVALID_ARGUMENT if the bytes are not a valid
// FileDescriptorSet (or contain a duplicate file name).
CelStatus cel_compile_opts_set_descriptor_set(CelCompileOpts* opts,
                                              const uint8_t* fds, int len);

// Compile a CEL `source` expression to a wasm Program.
//
// `opts` may be null (uses the compiler defaults).  On success the
// function returns CEL_STATUS_OK, writes a heap-allocated copy of
// the Program wasm bytes into `*out_wasm` with its length in
// `*out_len`, and sets `*out_err` to null.  Release `*out_wasm`
// with `cel_free`.
//
// On failure it returns a non-zero `CelStatus`, leaves `*out_wasm`
// null and `*out_len` zero, and — if `out_err` is non-null — writes
// a heap-allocated diagnostic (the `absl::Status` message) into
// `*out_err` (release with `cel_free`).
//
// `out_wasm`, `out_len`, and `source` must be non-null; a null one
// yields CEL_STATUS_INTERNAL.  `out_err` may be null if the caller
// does not want the diagnostic.
CelStatus cel_compile(const char* source, const CelCompileOpts* opts,
                      uint8_t** out_wasm, size_t* out_len, char** out_err);

// Release a buffer or string previously returned through a `cel_*`
// out-parameter (`*out_wasm`, `*out_err`).  Safe to call with null.
void cel_free(void* ptr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CELWASM_BINDINGS_C_COMPILER_CEL_CAPI_H_
