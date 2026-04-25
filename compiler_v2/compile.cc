#include "compiler_v2/compile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "binaryen-c.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/abi/cel_abi_emit.h"
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

// `cel_host.cel_get_field` + `cel_host.cel_has_field` trampolines
// (M2.C / M2.D).  Always imported — the runtime links the cel_host
// module unconditionally (see memory "No lazy tracking of runtime
// imports").
void InstallSelectImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType host_params[4] = {i32, i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelHostGetFieldInternalName), "cel_host",
                        "cel_get_field", host_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostHasFieldInternalName), "cel_host",
                        "cel_has_field", host_params, BinaryenTypeNone());
}

// M3.F: map literal + indexing runtime entry points.  `cel_map_*`
// come from the runtime module; `cel_host.cel_map_lookup` is the
// host trampoline arm of the kDynamic dispatcher (see
// map-list-dispatch.md §3 + §5).
void InstallMapImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType map_create_params[2] = {i32, i32};
  mod.AddFunctionImport(std::string(kCelMapCreateInternalName), "cel",
                        "cel_map_create", map_create_params,
                        BinaryenTypeNone());
  const BinaryenType map3_params[3] = {i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelMapInsertInternalName), "cel",
                        "cel_map_insert", map3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelMapLookupArenaInternalName), "cel",
                        "cel_map_lookup_arena", map3_params,
                        BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelMapLookupInternalName), "cel",
                        "cel_map_lookup", map3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostMapLookupInternalName), "cel_host",
                        "cel_map_lookup", map3_params, BinaryenTypeNone());
}

// M4.F: list literal + indexing runtime entry points.  Same shape
// as maps; unused imports are harmless (Binaryen drops them at
// validate).
void InstallListImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType list_create_params[2] = {i32, i32};
  mod.AddFunctionImport(std::string(kCelListCreateInternalName), "cel",
                        "cel_list_create", list_create_params,
                        BinaryenTypeNone());
  const BinaryenType list3_params[3] = {i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelListSetInternalName), "cel",
                        "cel_list_set", list3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelListAtArenaInternalName), "cel",
                        "cel_list_at_arena", list3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelListAtInternalName), "cel",
                        "cel_list_at", list3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostListAtInternalName), "cel_host",
                        "cel_list_at", list3_params, BinaryenTypeNone());
}

// Installs the expr module's host-ABI shape: imports `cel.memory`
// (host-allocated, shared with the cel_runtime.wasm instance), with
// the rodata data segment at `layout.rodata_base` applied to it at
// instantiate-time.  Plus the runtime function imports the `$eval`
// body (and any future call sites) target.
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
  InstallSelectImports(mod);
  InstallMapImports(mod);
  InstallListImports(mod);
  return absl::OkStatus();
}

// Attach the `cel.abi` custom section to the module.  Payload is a
// serialised `celwasm.abi.CelAbi` proto populated from the
// compile-time layout.  Engine::Plan reads it at load time to build
// the runtime lookup tables Instance::Eval(Activation) needs.
absl::Status AttachCelAbiSection(WasmModule& module, const StaticLayout& layout,
                                 absl::Span<const FieldRefRow> field_refs) {
  auto abi_or = BuildCelAbi(layout, field_refs);
  if (!abi_or.ok()) return abi_or.status();
  std::string abi_bytes;
  if (!abi_or->SerializeToString(&abi_bytes)) {
    return absl::InternalError("compile: failed to serialize cel.abi proto");
  }
  module.AddCustomSection(
      "cel.abi",
      absl::MakeConstSpan(reinterpret_cast<const uint8_t*>(abi_bytes.data()),
                          abi_bytes.size()));
  return absl::OkStatus();
}

// Parse + check + resolve + layout.  The expensive half of the
// pipeline; returns the populated front-half of a CompiledArtifact
// (ast + layout).  Extracted from Compile() to keep the top-level
// function under the function-size lint threshold.
absl::StatusOr<CompiledArtifact> RunFrontAndLayout(absl::string_view expression,
                                                   const CompileOptions& opts) {
  auto ast_or = ParseAndCheck(expression, opts.check);
  if (!ast_or.ok()) return ast_or.status();
  auto resolved_or = ResolvePass(*ast_or);
  if (!resolved_or.ok()) return resolved_or.status();
  auto layout_or = LayoutPass(*ast_or, *std::move(resolved_or));
  if (!layout_or.ok()) return layout_or.status();
  return CompiledArtifact{
      /*ast=*/*std::move(ast_or),
      /*layout=*/*std::move(layout_or),
      /*module=*/WasmModule(),
      /*eval_fn=*/{nullptr},
      /*wasm_bytes=*/{},
  };
}

// Validate + optionally serialize the emitted module.  Back half of
// the pipeline.
absl::Status FinaliseModule(CompiledArtifact& out, const CompileOptions& opts) {
  if (opts.validate) {
    auto v = out.module.Validate();
    if (!v.ok()) return v;
  }
  if (opts.serialize) {
    auto bytes_or = out.module.Serialize();
    if (!bytes_or.ok()) return bytes_or.status();
    out.wasm_bytes = *std::move(bytes_or);
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<CompiledArtifact> Compile(absl::string_view expression,
                                         const CompileOptions& opts) {
  auto out_or = RunFrontAndLayout(expression, opts);
  if (!out_or.ok()) return out_or.status();
  CompiledArtifact out = *std::move(out_or);

  if (auto s = InstallHostAbi(out.module, out.layout, opts.mem_size_bytes);
      !s.ok()) {
    return s;
  }

  LoweringOptions lower_opts;
  lower_opts.mem_size_bytes = opts.mem_size_bytes;
  auto lowered_or = LowerToEvalFunction(
      out.ast, out.layout, opts.eval_internal_name, out.module, lower_opts);
  if (!lowered_or.ok()) return lowered_or.status();
  out.eval_fn = *std::move(lowered_or);

  out.module.ExportFunction(opts.eval_internal_name, opts.eval_export_name);

  if (auto s = AttachCelAbiSection(out.module, out.layout,
                                   absl::MakeConstSpan(out.eval_fn.field_refs));
      !s.ok()) {
    return s;
  }
  if (auto s = FinaliseModule(out, opts); !s.ok()) return s;
  return out;
}

}  // namespace celwasm
