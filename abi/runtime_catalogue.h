// Single source of truth for the wasm imports an emitted expr
// module declares.  Three namespaces:
//
//   `cel`       — runtime helpers exported by `cel_runtime.wasm`
//                 (arithmetic kernels, comparison, aggregate ops,
//                 3VL, type conversions, comprehension iter,
//                 timestamp/duration helpers, arena primitives).
//   `cel_host`  — host-side trampolines registered by the wasmtime
//                 layer (`cel_get_field`, `cel_list_at`, the iter
//                 snapshots, `cel_make_message`, etc.).  Take wasm
//                 slots + arena bytes and call into the C++ host
//                 (proto reflection, externref dereference, …).
//   `cel_env`   — host-side environment primitives (`cel_log`).
//                 Conceptually like `cel_host` but reserved for
//                 cross-cutting concerns (diagnostics, eventually
//                 metrics / tracing).
//
// And one user-facing namespace for M13 custom fns:
//
//   `cel_fn`    — user-supplied function implementations.  Always
//                 imported under this exact module name regardless
//                 of how the user registered them on the Engine;
//                 the loader aliases the user's wasm exports into
//                 this namespace at AddModule time.
//
// This catalogue replaces three pre-existing hand-maintained
// surfaces that drifted independently:
//
//   1. `kRuntimeExports` in `eval/engine.cc` (the
//      allowlist binding runtime exports onto the linker).
//   2. `-Wl,--export=...` flags in `runtime/BUILD.bazel`
//      (the wasm-link export list).
//   3. `InferHelperArity` + the 15-entry exception table in
//      `compiler/codegen/overload_table.cc` (the name-suffix
//      sniff that recovered arity at OverloadTable::Build time).
//
// The `cel`-module set (`CelRuntimeHelpers()`) is itself DERIVED
// from the `cel_runtime` C source: every codegen-imported helper is
// marked with a `// cel:codegen-export` comment at its declaration,
// and `//bazel:gen_runtime_catalogue` reads the marker (membership)
// plus the `void`/`uint32_t` signature (arity + return shape) into a
// `CelRuntimeCatalogue` textproto that this TU embeds and parses at
// first use.  The `cel_host` / `cel_env` import sets stay
// hand-maintained (they describe host imports, which have no
// `cel_runtime.wasm` export to derive from).
//
// ABI versioning.  `kRuntimeAbiVersion` is bumped on any breaking
// change to the catalogue (renamed helper, changed arity, dropped
// helper).  The `cel.abi` custom section the expr module emits
// carries this constant at compile time; the engine checks it
// against the runtime's at instantiate.  Mismatch → clear
// FailedPrecondition with both versions, rather than wasmtime's
// opaque "type mismatch on call $cel_..." trap.

#ifndef CELWASM_ABI_RUNTIME_CATALOGUE_H_
#define CELWASM_ABI_RUNTIME_CATALOGUE_H_

#include <cstdint>

#include "abi/cel_abi.pb.h"
#include "abi/runtime_catalogue.pb.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm::abi {

enum class AbiModule : uint8_t {
  kCelRuntime,  // "cel"      — pure-wasm helpers in cel_runtime.wasm.
  kCelHost,     // "cel_host" — wasmtime host trampolines.
  kCelEnv,      // "cel_env"  — host environment helpers (cel_log).
  kCelFn,       // "cel_fn"   — user-supplied custom-fn impls.
};

absl::string_view AbiModuleName(AbiModule m);

// The catalogue-entry type is the GENERATED proto message
// `celwasm::abi::CelRuntimeFunction` (`runtime_catalogue.proto`) — there
// is no hand-defined POD; the proto IS the type used everywhere.  Its
// accessors:
//   - `name()`        the exported symbol.
//   - `module()`      the `CelRuntimeModule` enum (`CEL` / `CEL_HOST` /
//                     `CEL_ENV`); mirrors the C++ `AbiModule` below.
//   - `num_args()`    exact i32 parameter count (no out-slot semantics
//                     layered on; the wasm signature is just i32×N).
//   - `returns_i32()` true iff the wasm function returns one i32 (arena
//                     helpers, iter handles, count helpers); false for
//                     the void-returning majority (every `_at_*` kernel
//                     and every host trampoline writes through an
//                     out-slot in linear memory).
//
// The i32-only signature shape is LOAD-BEARING, not incidental: it is
// what licenses registering host trampolines through wasmtime's
// unchecked (raw, unboxed) call ABI, which has no safe story for
// GC'd reference types (see benchmark/ANALYSIS.md, candidate P0).
// Values cross the boundary through linear-memory CelValue slots and
// host-side handle-table indices — never as wasm reference types.
// Extending this schema with a non-i32 parameter type is therefore an
// ABI-design event, not a field addition: it must revisit the
// unchecked registration in eval/internal/cel_host_wasmtime.cc.

// Current ABI version.  Bumped on any change to the helper
// catalogues below.  The cel.abi custom section in every emitted
// expr module carries this constant; the engine checks it against
// the runtime's at instantiate time.
// v3: reserved low-memory window raised 8 KiB → 256 KiB
// (`--global-base=262144`) so large constant aggregates materialize
// into rodata (m31).
constexpr uint32_t kRuntimeAbiVersion = 3;

// All helpers exported by `cel_runtime.wasm` (module name "cel").
// Loaded once from the generated, embedded `CelRuntimeCatalogue`
// textproto; the returned span has process lifetime.
absl::Span<const CelRuntimeFunction> CelRuntimeHelpers();

// All host trampolines registered by the wasmtime layer (module
// name "cel_host").
absl::Span<const CelRuntimeFunction> CelHostFunctions();

// All host environment helpers (module name "cel_env").  Currently
// just `cel_log`.
absl::Span<const CelRuntimeFunction> CelEnvFunctions();

// Lookup a helper by name in a specific namespace.  Returns
// nullptr if not found.  Used by codegen's import-installation
// pass to recover arity given a `(name, module)` pair — the
// OverloadTable seeds carry `{cel_id, helper_name, module}`.
//
// Namespaces are independent: `cel.cel_list_at` (the kDynamic
// dispatcher) and `cel_host.cel_list_at` (the host trampoline
// it tail-calls) coexist as distinct entries.
//
// `kCelFn` is rejected — custom fns aren't in the catalogue;
// their arity comes from `Compiler::Builder::AddFunction`.
const CelRuntimeFunction* absl_nullable FindBuiltinHelper(
    AbiModule module, absl::string_view name);

// Version-check policy for the `runtime_abi_version` field in a
// decoded `cel.abi` section.  Used by `Engine::Plan` (Slice E).
//
// Rules:
//   - `prog_v == engine_v` (`kRuntimeAbiVersion`)  →  OK.
//   - `prog_v == 0` AND the section is otherwise empty (no
//     variables / fields / attributes / types)  →  OK.  Minimal
//     synthetic fixtures and pre-Slice-E modules that happen to
//     declare no surface still load.
//   - `prog_v == 0` with a non-empty surface  →  FailedPrecondition
//     "predates ABI versioning; recompile".
//   - otherwise  →  FailedPrecondition naming both versions.
//
// Returning a status keeps the call site at `Engine::Plan` trivial
// and lets the policy be unit-tested without spinning up an engine.
absl::Status CheckRuntimeAbiVersion(const CelAbi& abi);

}  // namespace celwasm::abi

#endif  // CELWASM_ABI_RUNTIME_CATALOGUE_H_
