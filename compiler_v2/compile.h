#ifndef CELWASM_COMPILER_V2_COMPILE_H_
#define CELWASM_COMPILER_V2_COMPILE_H_

// Top-level pipeline facade.  Stitches parse → check → resolve → layout →
// module-setup → LowerToEvalFunction into one call so downstream callers
// (CLI, host_loader-driven e2e) don't each re-wire the passes.
//
// The emitted module has the M1 shape:
//   - memory: one wasm page, exported as "memory", with an active data
//     segment at `layout.rodata_base` holding the packed rodata bytes.
//   - imports from module "cel": `cel_reset(i32,i32)->()` and
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
#include "compiler_v2/codegen/expr_lower.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/module.h"
#include "compiler_v2/frontend/parse_and_check.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

struct CompileOptions {
  // Forwarded to ParseAndCheck — carries schema + variable specs +
  // container + source description.
  CheckOptions check;

  // Total linear-memory size in bytes.  Flows to
  // `LoweringOptions.mem_size_bytes` (the second arg of the `cel_reset`
  // call emitted at the top of every `$eval` body) and to `SetMemory`'s
  // page count (rounded up to the next wasm page).  Default is two
  // pages (128 KiB): the runtime `.wasm` is cross-compiled with
  // `min: 2` on its imported memory, so a single-page expr module can't
  // pair with it.  Raise this when the expression needs a larger arena.
  uint32_t mem_size_bytes = 128u * 1024u;

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
};

struct CompiledArtifact {
  TypedAst ast;
  StaticLayout layout;
  WasmModule module;
  LoweredFunction eval_fn;
  // Populated iff `CompileOptions.serialize == true`.
  std::vector<uint8_t> wasm_bytes;
};

// Runs the full compile pipeline on `expression`.  Returns the bundled
// artifact on success or the first failing stage's status on failure.
ABSL_MUST_USE_RESULT absl::StatusOr<CompiledArtifact> Compile(
    absl::string_view expression, const CompileOptions& opts = {});

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_COMPILE_H_
