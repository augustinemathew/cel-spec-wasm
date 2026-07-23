// C API — sandboxed component functions (`@component.` backend).
//
// The component surface is the one custom-function mechanism that is
// **pure data**: a component is wasm bytes carrying its own `.celfn`
// declarations in a `cel.fns` custom section (embedded by the
// `cel_wasm_component` build macro, mirroring how a Program carries
// `cel.abi`).  No C function pointers cross this API — which makes it
// the custom-function path that survives every FFI the C ABI reaches
// (Go cgo, Rust bindgen, Python ctypes, Node N-API) AND a future
// embedder that is itself wasm: `cel_host_fn` callbacks (cel_eval.h)
// cannot cross a wasm boundary, but component bytes can.
//
// Lifecycle: load once, register on both phases, free after both
// registrations (registration copies what it needs):
//
//   cel_component* scorer = NULL;
//   CEL_TRY(cel_component_load(bytes, len, &scorer));
//   CEL_TRY(cel_compiler_builder_use_component(cb, scorer));  // type-check
//   CEL_TRY(cel_engine_builder_use_component(eb, scorer));    // sandbox
//   cel_component_free(scorer);
//
// The two registrations mirror the C++ `Use(component)` pair and are
// the irreducible floor: compile-time and eval-time are separable
// phases (compile in CI, evaluate in a server that never links the
// compiler), and each side genuinely needs the plugin.  A consumer
// that only compiles links `cel_compiler` alone; only evaluates,
// `cel_eval` alone — this header includes neither and both sides'
// `use_component` entrypoints live with their own libraries.

#ifndef CELWASM_BINDINGS_C_CEL_COMPONENT_H_
#define CELWASM_BINDINGS_C_CEL_COMPONENT_H_

#include <stddef.h>
#include <stdint.h>

#include "bindings/c/cel_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// A loaded, validated component: wasm bytes + the `.celfn`
// declarations parsed out of its `cel.fns` custom section.  Opaque;
// immutable after load; safe to share across threads and register on
// any number of builders.
typedef struct cel_component cel_component;

// Load a self-describing component.  Copies `bytes` (the caller's
// buffer may be freed immediately).  On success writes `*out` and
// returns `NULL`.  InvalidArgument if the bytes are not a wasm
// component, if the `cel.fns` section is missing (see
// `cel_component_load_with_decls` for pre-section components), or if
// the embedded declarations fail to parse; the message names the
// offending declaration.
cel_status* cel_component_load(const uint8_t* bytes, size_t len,
                               cel_component** out);

// Escape hatch for components built without an embedded `cel.fns`
// section: supply the `.celfn` declarations explicitly
// (NUL-terminated source, `@component.` declarations only).  Same
// contract as `cel_component_load` otherwise.  If the component DOES
// carry a section, the explicit text must match it byte-for-byte —
// FailedPrecondition otherwise (two sources of truth drifting is the
// bug class this API exists to end).
cel_status* cel_component_load_with_decls(const uint8_t* bytes, size_t len,
                                          const char* celfn_decls,
                                          cel_component** out);

// ————————————————————— introspection —————————————————————
//
// Enough surface to build a function picker or reference docs from
// the component alone — the same facts `FunctionLibrary::decls()`
// exposes in C++.

// Number of `@component.` declarations the component carries.
size_t cel_component_fn_count(const cel_component* component);

// The i-th declaration as `.celfn` source text (e.g.
// "bool @component.allow(string subject, string action);"),
// NUL-terminated, owned by `component` — valid until
// `cel_component_free`, do not free.  NULL if `i` is out of range.
const char* cel_component_fn_decl(const cel_component* component, size_t i);

// The i-th declaration's doc-comment (`///` text captured at build
// time), or "" if none.  Same ownership as `cel_component_fn_decl`.
const char* cel_component_fn_doc(const cel_component* component, size_t i);

// Content hash of the component (bytes + declarations), suitable for
// cache keys and for correlating a Program with the plugin version it
// was compiled against.  32 bytes, owned by `component`.
const uint8_t* cel_component_hash(const cel_component* component);

// Free the component.  Builders that registered it hold their own
// copies; freeing after registration is always safe.
void cel_component_free(cel_component* component);

// ————————————————————— registration ————————————————————————
//
// Each entrypoint is DECLARED here but exported by its own library
// (`cel_compiler` / `cel_eval`), preserving the one-directional
// layering: this header depends on neither.
//
// The two sides register at different lifecycle points, matching each
// phase's semantics: compile-side declarations are COMPILE CONFIG
// (frozen on the builder, like `declare_variable`), while eval-side
// components are RUNTIME BINDINGS (registered on the built engine,
// like `bind_function` — which is what makes hot-swap possible).

// Forward declarations — the full types live in cel_compiler.h /
// cel_eval.h; a TU that registers on only one side includes only that
// header.
typedef struct cel_compiler_builder cel_compiler_builder;
typedef struct cel_engine cel_engine;

// Compile side: make every declaration callable (call sites
// type-check against the declared signatures).  Copies the
// declarations out of `component`.  AlreadyExists if any function
// name collides with a previously registered declaration.
cel_status* cel_compiler_builder_use_component(
    cel_compiler_builder* builder, const cel_component* component);

// Eval side: instantiate the component in its own sandbox (separate
// linear memory, no host access) and bind every declaration to the
// matching export.  A missing or mis-typed export is rejected HERE,
// at registration — never at eval.  Copies the bytes out of
// `component`.  Configure at startup: NOT thread-safe, and Instances
// planned before this call do not see the component (same contract as
// `cel_engine_bind_function`).
cel_status* cel_engine_use_component(cel_engine* engine,
                                     const cel_component* component);

// Hot-swap: replace a previously registered component with new bytes.
// The replacement is matched to the old component by its DECLARATION
// SET, which must be identical function-for-function — same names,
// same signatures.  NotFound if no registered component carries this
// declaration set; FailedPrecondition if a same-named set differs in
// any signature (a plugin update that changes a signature is a
// compile-side event, not a swap — recompile the expressions).
// Instances already planned keep the old component until re-planned;
// Plans after the swap bind the replacement.  NOT thread-safe with
// concurrent registration; safe relative to concurrent `Eval` on
// existing instances.
cel_status* cel_engine_swap_component(cel_engine* engine,
                                      const cel_component* replacement);

// Optional resource limits for a component's sandbox, applied to the
// NEXT `use`/`swap` of `component` and remembered across swaps.
// Setter style (not a struct) so new limits never break the ABI.
// 0 means the engine default.
cel_status* cel_engine_component_max_memory(cel_engine* engine,
                                            const cel_component* component,
                                            uint64_t max_bytes);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CELWASM_BINDINGS_C_CEL_COMPONENT_H_
