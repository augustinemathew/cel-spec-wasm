#ifndef CELWASM_COMPILER_INTERNAL_COMPILE_H_
#define CELWASM_COMPILER_INTERNAL_COMPILE_H_

// Top-level pipeline facade.  Stitches parse → check → resolve → layout →
// module-setup → LowerToEvalFunction into one call so downstream callers
// (CLI, host_loader-driven e2e) don't each re-wire the passes.
//
// The emitted module has the M1 shape:
//   - memory: one wasm page, exported as "memory", with an active data
//     segment at `layout.rodata_base` holding the packed rodata bytes.
//   - imports from module "cel": `arena_reset(i32,i32)->()` and
//     `arena_alloc(i32)->i32`.  `arena_alloc` is unused at M1 but installed
//     unconditionally — codegen always links the runtime fully, never
//     gates imports on AST inspection (see CLAUDE.md memory notes).
//   - function: `$eval : () -> i32`, exported under `opts.eval_export_name`.
//
// Error mapping (status codes flow through unchanged):
//   - Parser / checker failure  →  InvalidArgument
//   - Static-subset violation   →  InvalidArgument (from RejectDyn)
//   - Non-kConst root at M1     →  Unimplemented (from expr_lower)
//   - Binaryen validate failure →  FailedPrecondition

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/layout_pass.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

struct CompileOptions {
  // Forwarded to ParseAndCheck — carries schema + variable specs +
  // container + source description.
  CheckOptions check;

  // Total linear-memory size in bytes.  Flows to
  // `LoweringOptions.mem_size_bytes` (the second arg of the `arena_reset`
  // call emitted at the top of every `$eval` body) and to `SetMemory`'s
  // page count (rounded up to the next wasm page).  Default is two
  // pages (128 KiB): the runtime `.wasm` is cross-compiled with
  // `min: 2` on its imported memory, so a single-page expr module can't
  // pair with it.  Raise this when the expression needs a larger arena.
  uint32_t mem_size_bytes =
      MemoryLayout::kInitialMemoryPages * MemoryLayout::kWasmPageSize;

  // Internal wasm name the function is registered under inside the module.
  // `Binaryen` uses this to resolve `BinaryenCall` targets and in exports.
  std::string eval_internal_name = "$eval";

  // External name the function is exported as.  Host loaders call
  // `wasmtime_instance_export_get(..., "eval")` by default.
  std::string eval_export_name = "eval";

  // When true, run Binaryen's validator after emission.  Disabling is
  // useful only for tests that exercise the validate-failure path.
  bool validate = true;

  // Binaryen optimization level run on the emitted module before
  // serializing.  Mirrors `wasm-opt -O<n>` semantics:
  //   0 — no-op (today's default; preserves byte-identical output).
  //   1 — light; fast compile, modest perf win.
  //   2 — balanced (the canonical pipeline); ~2-5× the unoptimized
  //       Compile cost, single-digit-percent Eval speed-up on
  //       arithmetic-heavy expressions, larger on chains.
  //   3 — aggressive; some passes have superlinear cost.
  // Default 0 keeps existing codegen golden tests byte-identical.
  // Recommended setting for production is 2.
  int optimize_level = 0;

  // When true, serialize the module into `wasm_bytes` in the result.
  // False keeps the artifact alive as a Binaryen IR handle for callers
  // that only need to inspect / transform the module.
  bool serialize = true;

  // M13 Slice C.3 — custom-fn libraries to register with the cel-cpp
  // checker (call-site resolution) AND the `OverloadTableBuilder`
  // (codegen import emission).  Each `FunctionLibrary::decls()`
  // contributes one `OverloadDecl` to the checker and one
  // `RegisterCustom` call to the OverloadTable, keyed by the
  // synthesised `overload_id`.  Empty (default) → built-ins only.
  // Cross-library overload-id uniqueness is the public Compiler
  // API's responsibility; this layer trusts the upstream filter.
  std::vector<FunctionLibrary> function_libraries;
};

// One bundled CEL-defined-fn library wasm module — produced by
// `celwasm::CompileLibraryBodies` for each `FunctionLibrary` that
// carries at least one `kCelDefined` decl.  `name` is the library's
// `Module foo;` directive; `bytes` is the serialised wasm.  Empty
// when no library carries CEL-defined decls.
struct LibraryModuleBytes {
  std::string name;
  std::vector<uint8_t> bytes;
};

struct CompiledArtifact {
  TypedAst ast;
  StaticLayout layout;
  WasmModule module;
  LoweredFunction eval_fn;
  // Populated iff `CompileOptions.serialize == true`.
  std::vector<uint8_t> wasm_bytes;
  // M13 Slice D — bundled CEL-defined-fn libraries, one entry per
  // `opts.function_libraries[i]` that carries `kCelDefined` decls.
  // `Engine::Plan` walks this vector before instantiating the expr
  // module and registers each entry under its `name`.
  std::vector<LibraryModuleBytes> library_modules;
};

// Runs the full compile pipeline on `expression`.  Returns the bundled
// artifact on success or the first failing stage's status on failure.
ABSL_MUST_USE_RESULT absl::StatusOr<CompiledArtifact> Compile(
    absl::string_view expression, const CompileOptions& opts = {});

// Installs the standard expr-module imports (`cel.memory`,
// `cel.arena_reset`, `cel.arena_alloc`, `cel_host.*` trampolines,
// `cel.cel_map_*` / `cel.cel_list_*`) onto `mod`.  Shared with
// `compiler/celfn/library_module.cc` so a CEL-defined-fn library
// module's wasm imports the same surface the expr module does.
//
// `layout.rodata` is installed as an active data segment on the
// shared memory at `layout.rodata_base`; multiple modules using the
// same memory need disjoint rodata ranges (see
// `LayoutOptions::rodata_base_override`).
ABSL_MUST_USE_RESULT absl::Status InstallExprModuleImports(
    WasmModule& mod, const StaticLayout& layout, uint32_t mem_size_bytes);

// Installs one wasm function import per `OverloadTable` row whose
// runtime export is shipped today (built-ins) plus the M13 custom
// rows.  Shared with `library_module.cc` so CEL-defined-fn bodies
// can call built-ins + sibling helpers via the same import shape
// the expr module uses.  Idempotent against the dedup set the
// caller passes in.
void InstallOverloadImportsExport(WasmModule& mod,
                                  const OverloadTable& overload_table);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_INTERNAL_COMPILE_H_
