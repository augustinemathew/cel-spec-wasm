// `celwasm::api::Compiler` — pure compile-time front end.  Per
// cel-host-surface.md §2.1 (with the role-split correction in
// doc/implementation-plan/rewrite/two-phase-runtime-isolation.md
// §4): holds declarations + the compile-time descriptor pool.
// Immutable after `Builder::Build()`.  No wasmtime dependency.
// One Compiler can produce many Programs.
//
// Programs from this Compiler can be:
//   - serialized (via `Program::wasm_bytes()`) and shipped
//     elsewhere
//   - executed in this process by passing them to
//     `celwasm::api::Engine::Plan(program, bindings)`
//
// The Compiler itself never touches a wasm engine.

#ifndef CELWASM_COMPILER_V2_API_COMPILER_H_
#define CELWASM_COMPILER_V2_API_COMPILER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/celfn/function_library.h"

namespace celwasm::api {

// Pulled in from `namespace cel` (type.h / program.h re-exports
// from celwasm::api but `CelType` still lives in `cel`).  Lets the
// class bodies refer to them unqualified.
using ::celwasm::api::CelType;

// One declared free variable: name + static type.  Introspected via
// `Compiler::declared_variables()`; the checker resolves each ident
// expression against this list at `Compile` time.
//
// Named `VariableDeclaration` rather than `VariableDecl` to avoid an
// ODR collision with `cel::VariableDecl` from cel-cpp's
// `common/decl.h` — both live in `namespace cel::` and a clash causes
// a subtle crash when the checker builder constructs its standard-
// library declarations.
struct VariableDeclaration {
  std::string name;
  CelType type;
};

// Per-compilation tunables.  Per cel-host-surface.md §2.1:
// declarations live on the Compiler/Builder; this struct only tunes
// how a specific expression is lowered.
//
// Scope.  These knobs control the EXPR module (the per-expression
// wasm bytes emitted by `Compile`).  They do NOT control the runtime
// module (`cel_runtime.wasm`), which is compiled once at build time
// with `-O3 -flto` unconditionally — see `compiler_v2/runtime/BUILD.
// bazel` for the rationale.
//
// All three fields below are forwarded into the internal
// `celwasm::CompileOptions` (compile.h); the internal struct has
// additional pipeline-only knobs (`eval_internal_name`,
// `eval_export_name`, `validate`, `serialize`) that public callers
// can't and shouldn't tune.
struct CompilerOptions {
  // Total linear-memory size in bytes, forwarded to the underlying
  // pipeline's `mem_size_bytes`.  Default is two wasm pages
  // (128 KiB) — matches cel_runtime.wasm's `--import-memory` min=2.
  // Raise this when an expression needs a larger arena (e.g. heavy
  // string concatenation or list construction inside a single Eval).
  // Rounded up to the next wasm page (64 KiB) at module-emit time.
  uint32_t mem_size_bytes = 128u * 1024u;

  // Package container used for name resolution (CEL-Go `container` /
  // CEL-Java `container`).  Forwarded verbatim to
  // `CheckOptions::container`.  Empty (the default) means "no
  // container" — every ident must be looked up by its globally-
  // qualified name.  Populating this lets short-form idents inside
  // a namespace resolve against `<container>.<name>` first.
  std::string container;

  // Binaryen optimization level for the emitted EXPR wasm module
  // (the runtime is unconditionally `-O3 -flto`; see struct-level
  // docblock).  Mirrors `wasm-opt -O<n>`:
  //
  //   0 — no-op; byte-identical output to a pre-Optimize build.
  //       Today's default; chosen so existing codegen golden tests
  //       stay byte-identical.
  //   1 — light pass list; fast Compile, modest Eval win.
  //   2 — balanced (canonical `wasm-opt -O2` pipeline); ~2-3×
  //       Compile cost vs level 0, -50% Eval on chain-heavy bodies
  //       (e.g. 20-term comparison chain: 11.2 us → 5.4 us per Eval
  //       on darwin-arm64, see compiler_v2/bench/README.md).  Short
  //       bodies (3-term arith) are a wash on Eval but still pay
  //       the Compile penalty.
  //   3 — aggressive; some passes have superlinear cost.  Rarely
  //       worth it over 2 in practice.
  //
  // Recommended production setting is 2 on the request path —
  // Compile cost amortises across many Eval calls via the
  // Engine::Plan / Instance caching model (compile once, eval many).
  // Use 0 for hot-reload paths where Compile latency dominates.
  // Levels outside [0, 3] are rejected with `InvalidArgument`.
  int optimize_level = 0;
};

class Compiler {
 public:
  class Builder;
  static Builder NewBuilder();

  // Copyable + movable — a Compiler is pure data (declarations +
  // registered descriptors).  Descriptors are non-owning pointers
  // into a `DescriptorPool` the caller manages (typically
  // `generated_pool()`).
  Compiler(const Compiler&) = default;
  Compiler& operator=(const Compiler&) = default;
  Compiler(Compiler&&) noexcept = default;
  Compiler& operator=(Compiler&&) noexcept = default;
  ~Compiler() = default;

  // Compile a CEL source string to a Program (wasm bytes).  Runs the
  // full pipeline (parse → check → resolve → layout → module → lower
  // → assemble → optionally optimize).  No wasmtime involvement; the
  // Program is just bytes + ABI.
  //
  // Status mapping flows through from the underlying pipeline:
  //
  //   - InvalidArgument:    parse failure, type-check failure, a
  //                         static-subset violation (DYN / unbound
  //                         function / type-param), or an
  //                         `optimize_level` outside [0, 3].
  //   - Unimplemented:      AST shape the current milestone doesn't
  //                         handle yet (e.g. comprehensions pre-M11).
  //   - FailedPrecondition: Binaryen validate failure (compiler bug —
  //                         should never escape; file a regression).
  //
  // See `compiler_v2/README.md` for the high-level compile / plan /
  // eval lifecycle and the runtime build-time flags that frame
  // these compile-time knobs.
  ABSL_MUST_USE_RESULT absl::StatusOr<Program> Compile(
      absl::string_view source, const CompilerOptions& opts = {}) const;

  // Introspection — declarations visible to this Compiler.
  absl::Span<const VariableDeclaration> declared_variables() const {
    return declared_variables_;
  }

  // Introspection — custom function libraries plugged into this
  // Compiler via `Builder::AddLibrary` / `Builder::AddFunction`.
  // Each library's `decls()` enumerates the available custom fns.
  absl::Span<const celwasm::FunctionLibrary> function_libraries() const {
    return function_libraries_;
  }

 private:
  friend class Builder;
  Compiler() = default;  // Builder constructs.

  std::vector<VariableDeclaration> declared_variables_;
  // M13 Slice C.2 — custom-fn libraries accumulated by the Builder.
  // Each library is the typed parsed form of a `.celfn` file (or a
  // programmatic `Builder` construction).  Held by value; Compiler
  // owns them.
  //
  // **Storage only until Slice C.3.**  `Compiler::Compile` does not
  // yet consume this field — the wiring into the cel-cpp
  // `TypeCheckerBuilder` (call-site resolution) and the codegen
  // `OverloadTableBuilder::RegisterCustom` (per decl, so the
  // `cel_fn.<overload_id>` imports are emitted) lands in Slice C.3.
  // Until C.3 lands, a CEL source that references a registered
  // custom fn fails at the checker with "undeclared reference to
  // 'fn_name'" — the same failure mode as if the library had never
  // been registered.  See
  // `doc/implementation-plan/rewrite/m13-custom-fns.md` §12.
  std::vector<celwasm::FunctionLibrary> function_libraries_;
};

class Compiler::Builder {
 public:
  Builder() = default;

  ~Builder() = default;
  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;
  Builder(Builder&&) noexcept = default;
  Builder& operator=(Builder&&) noexcept = default;

  // Declare a free variable available to every expression this
  // Compiler compiles.  The checker resolves each ident against this
  // list; a reference to an undeclared variable fails at compile
  // time with `InvalidArgument`.  Duplicate names on the same Builder
  // are rejected by Build() with `InvalidArgument`, not silently
  // deduped.
  //
  // Args are taken by const reference and copied into the internal
  // decl vector.  Declaration is not a hot path; a handful of string
  // copies per Build() is well below the noise floor.
  //
  // Returns `*this` so calls chain:
  //     Compiler::Builder b;
  //     b.DeclareVariable("x", CelType::Int())
  //      .DeclareVariable("s", CelType::String());
  //     auto c = std::move(b).Build();
  // The ONLY consuming method on Builder is `Build()`; every setter
  // mutates `*this` in place and returns an lvalue reference.
  Builder& DeclareVariable(const std::string& name, const CelType& type);

  // M13 Slice C.2 — register a `FunctionLibrary` of custom CEL
  // function declarations.  All decls in the library become
  // visible to every expression this Compiler compiles.  The
  // library is taken by value and stored on the Compiler.  Mixing
  // multiple `AddLibrary` calls (e.g. one from a `.celfn` file +
  // one constructed programmatically via Builder) is supported;
  // decls accumulate across calls.  Collisions (same overload-id
  // declared twice) are caught at `Build()` time.
  Builder& AddLibrary(celwasm::FunctionLibrary library);

  // M13 Slice C.2 — register a single custom-fn declaration from a
  // source string.  Convenience over `AddLibrary(ParseCelfnSource(s))`.
  // Accepts any valid `.celfn` source — typically a one-liner like
  //   `string @host.upper(this string s);`
  // but multi-decl strings work too.  Returns `*this` even on
  // parse failure; the deferred error surfaces at `Build()`.
  Builder& AddFunction(absl::string_view celfn_source);

  // Materialises the Compiler.  Consumes the Builder — chain from a
  // named local with `std::move(b).Build()`.
  //
  // Message-typed declarations are resolved against the process-wide
  // `google::protobuf::DescriptorPool::generated_pool()`; any
  // statically-linked `cc_proto_library` descriptor is reachable
  // there automatically.  For dynamic schemas (source `.proto` /
  // `FileDescriptorSet`) see the internal `CheckOptions::schema`
  // plumbing in `parse_and_check.cc`.
  //
  // Returns InvalidArgument on:
  //   - duplicate variable names declared on this Builder
  //   - a variable declared with CelType::Kind::kUnknown
  //   - a variable of Message type whose FQN is empty
  ABSL_MUST_USE_RESULT absl::StatusOr<Compiler> Build() &&;

 private:
  std::vector<VariableDeclaration> declared_variables_;
  std::vector<celwasm::FunctionLibrary> function_libraries_;
  // Deferred parse error from `AddFunction(string)` — surfaced
  // at `Build()` if non-OK.  Subsequent `AddFunction` /
  // `AddLibrary` calls overwrite this only if the prior status
  // was OK, so the FIRST failure wins.
  absl::Status deferred_status_;
};

}  // namespace celwasm::api

#endif  // CELWASM_COMPILER_V2_API_COMPILER_H_
