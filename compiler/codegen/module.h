// Thin RAII wrapper over Binaryen's C-API `BinaryenModuleRef`.
//
// The wrapper exposes only the subset of the Binaryen API that the
// emitted eval-module needs: memory + externref table + imports +
// exports + functions + serialize + validate.  Anything more
// specialised is reachable via `raw()` and the caller can drop straight
// into `binaryen-c.h`.
//
// The goal is to keep celwasmc's codegen free of Binaryen-idiomatic
// C-style setup (malloced C-string arrays for parameter types, manual
// validator-result checks, uint32 sentinel values for "no maximum",
// etc.) so the interesting parts of expr_lower stay small and
// readable.

#ifndef CELWASM_COMPILER_CODEGEN_MODULE_H_
#define CELWASM_COMPILER_CODEGEN_MODULE_H_

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

// Builds a Binaryen function-parameter / result type from a span of
// component types.  Binaryen represents:
//   - 0 components as `BinaryenTypeNone()`.
//   - 1 component as the component itself.
//   - N components as an interned tuple type.
// Tuple types are interned by Binaryen; the caller does not own the
// returned handle and must not dispose of it.
BinaryenType TupleType(absl::Span<const BinaryenType> parts);

class WasmModule {
 public:
  WasmModule();
  ~WasmModule();

  WasmModule(const WasmModule&) = delete;
  WasmModule& operator=(const WasmModule&) = delete;

  WasmModule(WasmModule&& other) noexcept;
  WasmModule& operator=(WasmModule&& other) noexcept;

  // Raw handle for callers that need APIs not yet surfaced here — in
  // particular, expression builders (`BinaryenConst`, `BinaryenCall`,
  // etc.) need the module ref to allocate expressions.
  BinaryenModuleRef absl_nonnull raw() const {
    return module_;
  }

  // Declares the module's (only) memory.  `export_name` is empty to
  // keep the memory module-private; otherwise the memory is exported
  // under that name.  Failure modes surface as `FailedPrecondition`
  // (a memory has already been declared) or `InvalidArgument`
  // (max < initial).
  ABSL_MUST_USE_RESULT absl::Status SetMemory(uint32_t initial_pages,
                                              std::optional<uint32_t> max_pages,
                                              absl::string_view export_name);

  // Adds an externref table under `name` with `initial_slots` entries.
  // The table is initialized to `ref.null externref` at every slot.
  // Typed as externref regardless of initial — this is the `$cel_refs`
  // table from the design doc §7.1.
  ABSL_MUST_USE_RESULT absl::Status AddCelRefsTable(
      absl::string_view name, uint32_t initial_slots,
      std::optional<uint32_t> max_slots);

  // Registers an imported function.  `internal_name` is the identifier
  // codegen uses to refer to the import from inside function bodies
  // (via `BinaryenCall`).  `external_module` / `external_base` are the
  // names the host sees in `(import "mod" "base" ...)`.
  void AddFunctionImport(absl::string_view internal_name,
                         absl::string_view external_module,
                         absl::string_view external_base,
                         absl::Span<const BinaryenType> params,
                         BinaryenType result);

  // Imports a memory from another module.  Every eval module has exactly
  // one memory — the runtime's — imported under the name "cel"/"memory"
  // so every string / bytes offset is interpretable against the same
  // address space.  Fails (FailedPrecondition) if a memory has already
  // been set or imported.
  ABSL_MUST_USE_RESULT absl::Status AddMemoryImport(
      absl::string_view external_module, absl::string_view external_base,
      uint32_t initial_pages, std::optional<uint32_t> max_pages);

  // Adds a function definition.  Binaryen takes ownership of `body` —
  // the caller must not reuse or dispose of the expression after this
  // call.
  void AddFunction(absl::string_view internal_name,
                   absl::Span<const BinaryenType> params, BinaryenType result,
                   absl::Span<const BinaryenType> local_types,
                   BinaryenExpressionRef absl_nonnull body);

  // Exports a previously-declared entity under `external_name`.
  void ExportFunction(absl::string_view internal_name,
                      absl::string_view external_name);
  void ExportMemory(absl::string_view internal_name,
                    absl::string_view external_name);
  void ExportTable(absl::string_view internal_name,
                   absl::string_view external_name);

  // Runs Binaryen's own validator.  Returns OK iff valid.
  // NOTE: Binaryen writes human-readable diagnostics to stderr on
  // failure before this call returns false.  We do not intercept
  // them today; for M2 the validator output is useful during
  // development and is never expected to fire in green CI.
  ABSL_MUST_USE_RESULT absl::Status Validate() const;

  // Serialises the module to its canonical .wasm byte encoding.
  ABSL_MUST_USE_RESULT absl::StatusOr<std::vector<uint8_t>> Serialize() const;

  // For debugging: prints the module in Binaryen's s-expression
  // format to stderr.
  void PrintToStderr() const;

 private:
  BinaryenModuleRef module_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_MODULE_H_
