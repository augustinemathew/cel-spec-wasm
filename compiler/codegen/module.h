#ifndef CELWASM_COMPILER_CODEGEN_MODULE_H_
#define CELWASM_COMPILER_CODEGEN_MODULE_H_

// Thin RAII wrapper over Binaryen's C API `BinaryenModuleRef`.  Exposes
// only the surface expr_lower + the module emitter need: memory (defined
// or imported, with optional active data segments), function imports /
// definitions, exports, custom sections (for `cel.abi`), validate, and
// serialize.  Anything else is reachable through `raw()`.
//
// The expr module (emitted per-expression) defines and exports its own
// memory, installs its rodata as an active data segment at offset 16
// (per design doc §6.1), imports `cel.arena_reset` / `cel.arena_alloc`, and
// exports `$eval`.  The runtime module (pre-compiled from
// `runtime/cel_runtime.c`) imports the memory back.  M1's codegen only
// emits the expr side, but `AddMemoryImport` is kept so a future
// codegen-emitted runtime wrapper is a one-liner.

#include <cstdint>
#include <optional>
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

  // One active data segment, referenced by `SetMemory`.  `offset` is an
  // absolute linear-memory byte offset; `bytes` is the payload the
  // instantiator copies into memory at that offset.  The caller must
  // keep `bytes` live across the `SetMemory` call.
  struct DataSegment {
    uint32_t offset = 0;
    absl::Span<const uint8_t> bytes;
  };

  // Defines the module's (only) memory.  `export_name` empty leaves the
  // memory module-private; otherwise the memory is exported under that
  // name.  `segments` are installed as active data segments over the
  // memory and are anonymous (Binaryen auto-names them).  Failure modes:
  // `FailedPrecondition` (memory already set), `InvalidArgument`
  // (max < initial).
  ABSL_MUST_USE_RESULT absl::Status SetMemory(
      uint32_t initial_pages, std::optional<uint32_t> max_pages,
      absl::string_view export_name,
      absl::Span<const DataSegment> segments = {});

  // Imports a memory from `(import external_module external_base memory)`.
  // `segments` are installed as active data segments over the imported
  // memory — the instantiator writes them at module-load time, so expr's
  // `.rodata` ends up in shared memory regardless of who owns it.
  // `shared` mirrors the wasm spec's "shared" flag — required when the
  // runtime exports a `wasm32-wasi-threads` shared linear memory (Phase
  // C and later).  Shared memories MUST have `max_pages` set (wasm spec
  // requirement); we return InvalidArgument if `shared` is true with
  // an empty `max_pages`.  Fails (FailedPrecondition) if `SetMemory` /
  // `AddMemoryImport` has already been called on this module.
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
  void ExportMemory(absl::string_view internal_name,
                    absl::string_view external_name);

  // Adds a raw wasm custom section.  Used for the `cel.abi` header the
  // host loader reads to discover rodata / workspace / arena offsets
  // and the `$eval` return convention.
  void AddCustomSection(absl::string_view name,
                        absl::Span<const uint8_t> bytes);

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
  // Concurrent calls from multiple threads to `Optimize` with
  // different levels would race.  Today every caller is on the
  // Compile thread and serialised by `celwasm::api::Compiler` ownership;
  // surface this here so a future async-compile slice doesn't miss
  // it.  A future-cleaner shape would be `BinaryenModuleRunPasses`
  // with an explicit pass list (no global state); deferred until a
  // caller actually needs it.
  ABSL_MUST_USE_RESULT absl::Status Optimize(int level);

  // Serialises the module to its canonical `.wasm` byte encoding.
  ABSL_MUST_USE_RESULT absl::StatusOr<std::vector<uint8_t>> Serialize() const;

  // Prints the module in Binaryen's s-expression format to stderr.
  void PrintToStderr() const;

 private:
  BinaryenModuleRef module_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_MODULE_H_
