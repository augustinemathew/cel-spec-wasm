// `celwasm::Engine` — runtime side of the host surface.  Owns the
// wasm execution machinery shared across all evaluations: the
// `wasm_engine_t` and the parsed `cel_runtime.wasm` module.
//
// Per doc/implementation-plan/rewrite/two-phase-runtime-isolation.md
// §4 (revised): role-separated from `celwasm::Compiler`.  Compiler is
// pure compile-time and has no wasmtime dependency.  Engine is
// pure runtime and has no compile-time dependency.  A Program
// (bytes + ABI) is the serialization boundary between them.
//
// Lifecycle:
//
//   celwasm::Compiler  ─Compile(source)─►  celwasm::Program
//                                            │
//                                            ▼
//                  celwasm::Engine ─Plan(program, bindings)─►  celwasm::Instance
//
// The Engine is process-shared (or per-tenant in multi-tenant
// hosts).  `wasm_engine_t` and `wasmtime_module_t` are documented
// thread-safe for concurrent access; many threads safely call
// `Engine::Plan(...)` against the same Engine.
//
// Bench-justified: caching the parsed cel_runtime.wasm on the
// Engine gives a ~34x per-Plan speedup over re-parsing it (186us
// → 5.5us); sharing the engine across the process gives another
// ~64x cold-to-hot (351us → 5.5us).  See §2 of the plan doc for
// raw numbers.
//
// Plan() lands in a later commit; this one only stands up the
// engine + parsed runtime fixture and the Builder.

#ifndef CELWASM_EVAL_ENGINE_H_
#define CELWASM_EVAL_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "abi/plugin.h"
#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "compiler/program.h"
#include "eval/host_callback.h"
#include "eval/instance.h"
#include "eval/typed_function.h"

namespace celwasm {
struct WasmtimeEngineState;
}  // namespace celwasm

namespace celwasm {

// `HostCallback` (the impl for a single `@host.<name>` declaration) is
// defined in `eval/host_callback.h` as
// `std::function<absl::Status(HostCallContext&)>`.  The engine invokes
// it when a Planned program's wasm imports `cel_fn.<overload_id>` and
// that import is reached during `Instance::Eval`; the typed
// `HostCallContext` (eval/host_call_context.h) gives the callback
// kind-checked accessors over the argument slots and setters for the
// result slot.  Returning a non-OK status traps the wasm execution.

class Engine {
 public:
  class Builder;
  static Builder NewBuilder();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) noexcept;
  Engine& operator=(Engine&&) noexcept;
  ~Engine();

  // Build an Instance from a Program, ready for evaluation.  M1
  // re-parses the program's wasm bytes via wasmtime_module_new on
  // every Plan; future commits add an Engine-side cache keyed by
  // program identity if profiles demand it.
  //
  // Link-mode transparency: Plan accepts BOTH Program shapes that
  // `Compiler::Compile` can produce (see `CompilerOptions::LinkMode`
  // in compiler/compiler.h for the tradeoff table) and routes
  // automatically by inspecting the compiled module's import list
  // (`eval/internal/module_imports.h`):
  //
  //   - dynamic Programs (import from `"cel"`): a standalone
  //     cel_runtime.wasm instance is created in the same store and
  //     its exports bound on the linker before the expr module
  //     instantiates.
  //   - static Programs (no `"cel"` imports — runtime merged in at
  //     Compile time): no separate runtime instance; the Program
  //     instance itself is the helpers source, and its
  //     `__wasm_call_ctors` runs once at instantiate.
  //
  // The same Instance API (Eval / PartialEval) results either way;
  // callers never branch on link mode.  Per
  // `doc/implementation-plan/rewrite/m28-configurable-linking.md`.
  //
  // Wiring (per Plan §5.4): host-allocates a 2-page wasmtime_memory_t
  // in a fresh store; binds it as cel.memory on a fresh linker;
  // installs cel_env.cel_log; instantiates cel_runtime.wasm against
  // those imports; binds the runtime's arena_reset / arena_alloc exports
  // back onto the linker as cel.arena_reset / cel.arena_alloc; parses
  // and instantiates the expr module against the now-complete linker;
  // looks up the eval export.
  //
  // FailedPrecondition on:
  //   - module_new(expr): malformed bytes
  //   - instantiate failures (missing imports, etc.)
  //   - missing `eval` export
  //   - wasmtime trap during instantiation
  //
  // Plan is safe to call concurrently: each call creates a fresh
  // store + linker + memory, sharing only the engine + parsed
  // runtime module via the shared_ptr.  wasmtime engines and modules
  // are documented thread-safe.
  ABSL_MUST_USE_RESULT absl::StatusOr<Instance> Plan(
      const Program& program) const;

  // Register a C++ callback as the impl for a `@host.<name>`
  // declaration.  `overload_id` matches the synthesised id from
  // `FunctionLibrary::Builder::AddHost(...)` (e.g. `upper_string`,
  // `is_admin_message_acme_User`).  `num_args` is the total wasm
  // function arity — `params.size() + 1` (the +1 is the out_slot
  // every callback receives).  Matches `OverloadDef::num_args` 1:1.
  //
  // Conflict checks:
  //   - `overload_id` already registered → AlreadyExists
  //
  // Plan-time verification is ARITY-ONLY for this path: a raw
  // callback carries no declared parameter/return types, so the
  // required-function check (`Engine::Plan`, m35-plugin-ergonomics.md
  // §5.3) can only compare the wasm arity.  Register through
  // `BindFunction` to get the full recursive signature compare.
  //
  // **NOT thread-safe** — same contract as `AddFunction`.
  ABSL_MUST_USE_RESULT absl::Status AddFunction(absl::string_view overload_id,
                                                uint8_t num_args,
                                                HostCallback impl);

  // Register a Plugin as the sandboxed backend for its
  // declarations — the eval-side half of the one-noun flow.  Wraps
  // the AddPlugin internals and adds:
  //   - a STATIC export check: the interface
  //     (`plugin.wit_interface()`) and every decl's kebab-case
  //     export are resolved against the parsed component — a
  //     missing interface/export fails HERE, FailedPrecondition,
  //     naming it.  No instantiation.
  //   - the plugin's content hash is retained for Plan-time
  //     diagnostics (names which plugin mismatched).
  // Collisions (vs AddFunction/BindFunction and prior plugins)
  // -> AlreadyExists.  NOT thread-safe; startup-only, same contract
  // as the rest of the registration family.
  ABSL_MUST_USE_RESULT absl::Status Use(const Plugin& plugin);

  // Register a plugin (a Component-Model wasm binary) as the backend
  // for every `kPlugin` decl in `lib` — the explicit-decls escape
  // hatch (pure-WAT tests, pre-`cel.fns` artifacts).  Prefer
  // `Use(plugin)`, which derives `lib` from the artifact itself and
  // adds a registration-time static export check.
  //
  // Registration validates ONLY:
  //   - Any `overload_id` from `lib`'s kPlugin decls already
  //     registered → AlreadyExists.
  //   - `plugin_bytes` empty → InvalidArgument; failing to parse as
  //     a Component-Model component → FailedPrecondition (the
  //     wasmtime parse error, passed through).
  //
  // Export ↔ decl resolution is Plan-time-only on this path: each
  // Plan instantiates the component and looks up every decl's
  // kebab-case export, so a missing export surfaces as
  // FailedPrecondition from `Plan`, not here (pinned by
  // e2e/plugin_dispatch_test.cc MissingExportFailsAtPlanNotAddPlugin).
  // No `FuncType`-vs-decl signature validation exists on either
  // path — the wasmtime C API's component type introspection is too
  // thin — so a wrong-arity export surfaces only as a call-time
  // trap (`Use` narrows this to export *existence*, checked
  // statically at registration).
  //
  // **NOT thread-safe** — same contract as `AddFunction`.
  //
  // See `examples/09_plugin_functions.cc` for an end-to-end embed.
  ABSL_MUST_USE_RESULT absl::Status AddPlugin(
      absl::Span<const uint8_t> plugin_bytes, const FunctionLibrary& lib);

  // Typed sugar over `AddFunction` (host-call adapter Layer 2,
  // eval/typed_function.h): adapts a plain typed lambda into a
  // `HostCallback`, deriving the wasm arity from the lambda's parameter
  // count.  Only canonical CEL parameter / return spellings compile —
  // see typed_function.h for the closed type set and the rationale.
  //
  //   engine.AddTypedFunction("double_it_int",
  //       [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; });
  //
  // Same conflict / thread-safety contract as AddFunction — and the
  // same ARITY-ONLY Plan-time verification: without a parsed decl the
  // required-function check cannot compare CEL types (the C++
  // parameter kinds are canonical spellings, not CEL declarations).
  // Use `BindFunction` for the full signature compare.
  template <typename Fn>
  ABSL_MUST_USE_RESULT absl::Status AddTypedFunction(
      absl::string_view overload_id, Fn fn) {
    TypedFunction tf = BindTypedFunction(std::move(fn));
    return AddFunction(overload_id, tf.num_args, std::move(tf.callback));
  }

  // Declaration-first registration (host-call adapter Layer 3, sugar
  // over `AddTypedFunction`): takes the SAME `.celfn` declaration
  // string the compiler side takes via `Compiler::Builder::
  // AddFunction`, parses it with the same parser, and registers `fn`
  // under the overload-id the parse synthesises — so the embedder
  // never hand-spells the overload-id mangling or the wasm arity:
  //
  //   builder.AddFunction("int @host.discount_pct(string tier);");
  //   engine.BindFunction("int @host.discount_pct(string tier);",
  //       [](absl::string_view tier) -> absl::StatusOr<int64_t> {
  //         return tier == "gold" ? 20 : 5;
  //       });
  //
  // The callable's signature is validated against the declaration at
  // registration time.  InvalidArgument when:
  //   - `celfn_decl` fails to parse as `.celfn` source;
  //   - it holds anything other than exactly ONE declaration;
  //   - the declaration's backend is not `@host.`;
  //   - the callable's parameter count differs from the declared one;
  //   - a parameter's canonical C++ type is incompatible with the CEL
  //     type declared at the same position (the message names the
  //     param index, the declared CEL type, and the provided C++
  //     type).  Compatibility: bool↔bool, int64_t↔int, uint64_t↔uint,
  //     double↔double, absl::string_view↔string or bytes,
  //     absl::Duration↔Duration, absl::Time↔Timestamp,
  //     HostListView↔list<...>, HostMapView↔map<...>, `const M&` /
  //     `const google::protobuf::Message*`↔proto(...), and Value↔any
  //     declared type.
  //
  // Conflict + threading contract is `AddFunction`'s verbatim: an
  // already-registered overload-id → AlreadyExists, and the call is
  // **NOT thread-safe** with concurrent calls to itself or the other
  // setup methods — configure once at startup, then `Plan` from many
  // threads.
  //
  // Because this path parses a full declaration, the registration
  // captures the declared signature: `Engine::Plan`'s
  // required-function check compares it recursively (param types,
  // return type, receiver-ness — protos by FQN) against what the
  // Program was compiled with, not just the wasm arity that raw
  // `AddFunction` / `AddTypedFunction` registrations are limited to.
  template <typename Fn>
  ABSL_MUST_USE_RESULT absl::Status BindFunction(absl::string_view celfn_decl,
                                                 Fn fn) {
    return BindParsedFunction(celfn_decl, BindTypedFunction(std::move(fn)));
  }

 private:
  friend class Builder;

  // Non-template back half of `BindFunction`: `.celfn` parsing +
  // signature validation + registration through `AddFunction`.
  // Defined in engine.cc.
  ABSL_MUST_USE_RESULT absl::Status BindParsedFunction(
      absl::string_view celfn_decl, TypedFunction fn);

  // Built only by Builder::Build(); the shared_ptr's control
  // block is set up there so destruction here works without
  // WasmtimeEngineState being a complete type.
  explicit Engine(std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime);

  // shared_ptr (vs unique_ptr) so future Instances minted by Plan
  // can hold a back-reference to the engine state and outlive the
  // Engine handle the user originally constructed.  Internal
  // detail; not exposed.
  std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime_;
};

class Engine::Builder {
 public:
  Builder() = default;

  ~Builder() = default;
  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;
  Builder(Builder&&) noexcept = default;
  Builder& operator=(Builder&&) noexcept = default;

  // Enable wasmtime's "perfmap" JIT profiling strategy: the engine
  // writes a `/tmp/perf-<pid>.map` symbol file describing JIT'd code
  // (the runtime + every Planned expr module) so sampling profilers
  // (`samply`, Linux `perf`) can symbolicate otherwise-anonymous JIT
  // addresses.  Off by default — the map file is pure overhead
  // outside a profiling session.  Used by `benchmark/eval/
  // celwasm_bench` behind its `CELWASM_BENCH_PERFMAP=1` env knob.
  Builder& EnableJitPerfMap(bool enable) & {
    jit_perf_map_ = enable;
    return *this;
  }
  Builder&& EnableJitPerfMap(bool enable) && {
    jit_perf_map_ = enable;
    return std::move(*this);
  }



  // Allocate the wasm engine + parse `cel_runtime.wasm` into a
  // module.  Returns Internal on wasmtime allocation failure.
  // Single-use: && enforces consumption at the call site (const so
  // the build reads, never mutates, the configured tunables).
  ABSL_MUST_USE_RESULT absl::StatusOr<Engine> Build() const&&;

 private:
  bool jit_perf_map_ = false;
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_ENGINE_H_
