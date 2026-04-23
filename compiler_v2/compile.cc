#include "compiler_v2/compile.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "binaryen-c.h"
#include "compiler_v2/codegen/expr_lower.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/module.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/frontend/parse_and_check.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

namespace {

constexpr uint32_t kWasmPageBytes = 64u * 1024u;

// Wasm page count large enough to hold `mem_size_bytes`.
uint32_t PagesForBytes(uint32_t mem_size_bytes) {
  return (mem_size_bytes + kWasmPageBytes - 1) / kWasmPageBytes;
}

// Installs the expr module's host-ABI shape: imports `cel.memory`
// (host-allocated, shared with the cel_runtime.wasm instance), with
// the rodata data segment at `layout.rodata_base` applied to it at
// instantiate-time.  Plus the two runtime function imports the
// `$eval` body (and any future call sites) target.
//
// Memory ownership flipped per
// doc/implementation-plan/rewrite/two-phase-runtime-isolation.md
// §5 Commit F: under the (A) two-phase topology the host
// pre-allocates `cel.memory` and binds it on the linker; both expr
// and cel_runtime.wasm import it.  Active data segments still
// write at module-load time, so expr's .rodata lands in shared
// memory regardless of who owns it.
absl::Status InstallHostAbi(WasmModule& mod, const StaticLayout& layout,
                            uint32_t mem_size_bytes) {
  WasmModule::DataSegment seg{layout.rodata_base, layout.rodata};
  auto s = mod.AddMemoryImport("cel", "memory", PagesForBytes(mem_size_bytes),
                               /*max_pages=*/std::nullopt,
                               absl::MakeConstSpan(&seg, 1));
  if (!s.ok()) return s;

  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType reset_params[2] = {i32, i32};
  mod.AddFunctionImport(kCelResetInternalName, "cel", "cel_reset", reset_params,
                        BinaryenTypeNone());
  const BinaryenType alloc_params[1] = {i32};
  mod.AddFunctionImport("cel_alloc", "cel", "cel_alloc", alloc_params, i32);
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<CompiledArtifact> Compile(absl::string_view expression,
                                         const CompileOptions& opts) {
  auto ast_or = ParseAndCheck(expression, opts.check);
  if (!ast_or.ok()) return ast_or.status();

  auto resolved_or = ResolvePass(*ast_or);
  if (!resolved_or.ok()) return resolved_or.status();

  auto layout_or = LayoutPass(*ast_or, *std::move(resolved_or));
  if (!layout_or.ok()) return layout_or.status();

  CompiledArtifact out{
      /*ast=*/*std::move(ast_or),
      /*layout=*/*std::move(layout_or),
      /*module=*/WasmModule(),
      /*eval_fn=*/{nullptr},
      /*wasm_bytes=*/{},
  };

  auto s = InstallHostAbi(out.module, out.layout, opts.mem_size_bytes);
  if (!s.ok()) return s;

  LoweringOptions lower_opts;
  lower_opts.mem_size_bytes = opts.mem_size_bytes;
  auto lowered_or = LowerToEvalFunction(
      out.ast, out.layout, opts.eval_internal_name, out.module, lower_opts);
  if (!lowered_or.ok()) return lowered_or.status();
  out.eval_fn = *std::move(lowered_or);

  out.module.ExportFunction(opts.eval_internal_name, opts.eval_export_name);

  if (opts.validate) {
    auto v = out.module.Validate();
    if (!v.ok()) return v;
  }

  if (opts.serialize) {
    auto bytes_or = out.module.Serialize();
    if (!bytes_or.ok()) return bytes_or.status();
    out.wasm_bytes = *std::move(bytes_or);
  }

  return out;
}

}  // namespace celwasm
