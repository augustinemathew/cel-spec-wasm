#ifndef CELWASM_COMPILER_CODEGEN_MODULE_H_
#define CELWASM_COMPILER_CODEGEN_MODULE_H_

// Thin RAII wrapper over Binaryen's C API `BinaryenModuleRef`.  Exposes
// only the surface expr_lower + the module emitter need: memory (defined
// or imported, with optional active data segments), function imports /
// definitions, exports, custom sections (for `cel.abi`), validate, and
// serialize.  Anything else is reachable through `raw()`.
//
// The expr module is emitted per expression: it imports the runtime
// helpers it calls (`cel.arena_reset`, the `cel_host.*` trampolines, the
// overload-table helpers) and exports `$eval`.  Where its linear memory
// comes from depends on the link mode (see
// `rewrite/m28-configurable-linking.md`): DYNAMIC imports the runtime's
// shared memory (`AddMemoryImport`) with rodata installed as active data
// segments over it; STATIC emits `$eval` into the adopted runtime module
// (`Adopt`) with rodata appended via `AddActiveDataSegment`.  Rodata
// lands at offset 16 — past the two reserved low slots (see
// `rewrite/design.md` §6).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "binaryen-c.h"

namespace celwasm {

// Builds a Binaryen parameter / result type from `parts`.  Binaryen
// represents 0 components as `BinaryenTypeNone()`, 1 component as the
// component itself, N components as an interned tuple type.  The
// returned handle is Binaryen-owned; callers must not dispose it.
BinaryenType TupleType(absl::Span<const BinaryenType> parts);

class WasmModule {
 public:
  WasmModule();
  ~WasmModule();

  // Adopts an existing `BinaryenModuleRef` (e.g. one returned by
  // `BinaryenModuleRead`).  The adopted module's features are reset to
  // the same set our default constructor installs, so downstream
  // codegen sees a uniform feature surface regardless of how the host
  // module was assembled.  Used by `Compile()`'s `kStatic` link mode
  // to load the pre-built, wrapper-stripped cel_runtime wasm as the
  // base module that `$eval` is emitted into.  Takes ownership: on
  // destruction, the adopted module is `BinaryenModuleDispose`'d.
  static WasmModule Adopt(BinaryenModuleRef absl_nonnull existing);

  WasmModule(const WasmModule&) = delete;
  WasmModule& operator=(const WasmModule&) = delete;
  WasmModule(WasmModule&& other) noexcept;
  WasmModule& operator=(WasmModule&& other) noexcept;

  // Raw handle for callers that need Binaryen APIs not surfaced here —
  // in particular, expression builders (`BinaryenConst`, `BinaryenCall`,
  // …) need the module ref to allocate expressions.
  BinaryenModuleRef absl_nonnull raw() const {
    return module_;
  }

  // One active data segment, referenced by `AddMemoryImport`.  `offset` is an
  // absolute linear-memory byte offset; `bytes` is the payload the
  // instantiator copies into memory at that offset.  The caller must
  // keep `bytes` live across the `SetMemory` call.
  struct DataSegment {
    uint32_t offset = 0;
    absl::Span<const uint8_t> bytes;
  };

  // Imports a memory from `(import external_module external_base memory)`.
  // `segments` are installed as active data segments over the imported
  // memory — the instantiator writes them at module-load time, so expr's
  // `.rodata` ends up in shared memory regardless of who owns it.
  // `shared` mirrors the wasm spec's "shared" flag — required when the
  // runtime exports a `wasm32-wasi-threads` shared linear memory (Phase
  // C and later).  Shared memories MUST have `max_pages` set (wasm spec
  // requirement); we return InvalidArgument if `shared` is true with
  // an empty `max_pages`.  Fails (FailedPrecondition) if a memory
  // has already been installed on this module.
  ABSL_MUST_USE_RESULT absl::Status AddMemoryImport(
      absl::string_view external_module, absl::string_view external_base,
      uint32_t initial_pages, std::optional<uint32_t> max_pages,
      absl::Span<const DataSegment> segments = {}, bool shared = false);

  // Registers an imported function.  `internal_name` is the identifier
  // codegen uses to refer to the import from inside function bodies
  // (via `BinaryenCall`).  `external_module` / `external_base` are the
  // names the host sees in `(import "mod" "base" ...)`.
  void AddFunctionImport(absl::string_view internal_name,
                         absl::string_view external_module,
                         absl::string_view external_base,
                         absl::Span<const BinaryenType> params,
                         BinaryenType result);

  // Adds a function definition.  Binaryen takes ownership of `body`;
  // callers must not reuse or dispose the expression after this call.
  void AddFunction(absl::string_view internal_name,
                   absl::Span<const BinaryenType> params, BinaryenType result,
                   absl::Span<const BinaryenType> local_types,
                   BinaryenExpressionRef absl_nonnull body);

  // Exports a previously-declared entity under `external_name`.
  void ExportFunction(absl::string_view internal_name,
                      absl::string_view external_name);

  // Adds a raw wasm custom section.  Used for the `cel.abi` header the
  // host loader reads to discover rodata / workspace / arena offsets
  // and the `$eval` return convention.
  void AddCustomSection(absl::string_view name,
                        absl::Span<const uint8_t> bytes);

  // Appends an active data segment to an EXISTING memory.  Unlike the
  // `segments` arrays threaded through `SetMemory` / `AddMemoryImport`,
  // this lands on a memory whose other init data has already been
  // committed — useful for `Compile()`'s `kStatic` path, where the
  // memory comes from an adopted runtime module and the expr-side
  // rodata needs to land alongside the runtime's own .data segments.
  // `memory_name` is the internal name (Binaryen defaults memories to
  // `"memory"`).  The caller must keep `bytes` live across the call
  // (Binaryen copies into its own arena).
  void AddActiveDataSegment(uint32_t offset, absl::Span<const uint8_t> bytes,
                            absl::string_view memory_name = "memory");

  // One function import's external name pair — the names the host
  // sees in `(import "module" "base" (func ...))`.
  struct FunctionImportName {
    std::string module;
    std::string base;
  };

  // Lists the module's function imports (external module + base
  // names), in module function-index order.  Defined functions are
  // excluded.  Read-only — does not mutate the module.  Callers that
  // need the POST-optimize import surface (e.g. the cel.abi
  // required-functions table) must call this after `Optimize`, which
  // drops unused imports at level >= 1.
  std::vector<FunctionImportName> ListFunctionImports() const;

  // Runs Binaryen's own validator.  Returns OK iff valid.  Binaryen
  // writes human-readable diagnostics to stderr on failure before this
  // call returns; we do not intercept them.
  ABSL_MUST_USE_RESULT absl::Status Validate() const;

  // Runs the default Binaryen optimization pipeline (DCE + constant
  // folding + simplify-locals + vacuum + merge-blocks + reorder-
  // functions, etc.) at the given level.  `level` mirrors Binaryen's
  // CLI `-O` flag: 0 → no-op; 1 → light; 2 → balanced (the canonical
  // `wasm-opt -O2` pipeline); 3 → aggressive.  ShrinkLevel is held at
  // 0 because the expr module's perf wins more than its bytes:
  // Cranelift JITs once at instantiate time, so a 20 KB → 18 KB
  // module-size cut is irrelevant, but fewer locals + folded
  // constants directly tighten the native code Cranelift produces.
  //
  // **Thread-safety caveat.**  Binaryen's optimization knobs are
  // process-global state — `BinaryenSetOptimizeLevel` and
  // `BinaryenSetShrinkLevel` mutate static variables in the library.
  // `Compiler` is copyable pure data and does NOT serialise anything:
  // two Compilers (or two copies of one) calling `Compile` with
  // `optimize_level > 0` on different threads race on these globals.
  // Embedders must serialise `Compile` process-wide when
  // `optimize_level > 0` (see doc/design/00-architecture.md).  A
  // future-cleaner shape would be `BinaryenModuleRunPasses` with an
  // explicit pass list (no global state); deferred until a caller
  // actually needs it.
  ABSL_MUST_USE_RESULT absl::Status Optimize(int level);

  // Serialises the module to its canonical `.wasm` byte encoding.
  ABSL_MUST_USE_RESULT absl::StatusOr<std::vector<uint8_t>> Serialize() const;

 private:
  BinaryenModuleRef module_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_MODULE_H_
