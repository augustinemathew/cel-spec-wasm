#include "compiler/internal/compile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "abi/cel_abi_emit.h"
#include "abi/runtime_catalogue.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "binaryen-c.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/layout_pass.h"
#include "compiler/codegen/module.h"
#include "compiler/codegen/overload_table.h"
#include "compiler/codegen/resolve_pass.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/typed_ast.h"
#include "compiler/memory_layout.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_runtime_stripped_wasm_bytes.h"

namespace celwasm {

namespace {

// Wasm page count large enough to hold `mem_size_bytes`.
uint32_t PagesForBytes(uint32_t mem_size_bytes) {
  constexpr uint32_t kPageBytes = MemoryLayout::kWasmPageSize;
  return (mem_size_bytes + kPageBytes - 1) / kPageBytes;
}

// Reject an expression whose static region — rodata plus the
// workspace slots the host marshal and `$eval` write — ends past the
// reserved low-memory window.  BOTH link modes share one linear
// memory with the runtime: the runtime is linked with
// `-Wl,--global-base=CELWASM_RESERVED_LOW_MEMORY_BYTES` (so its own
// static data, heap and stack start at that offset), and in kDynamic
// the expr module's `cel.memory` import resolves to the runtime
// instance's exported memory.  A region ending past the boundary
// silently overwrites runtime state — instantiate-time for the rodata
// data segment, marshal/eval-time for workspace slot writes —
// surfacing far downstream as corruption traps (`wasm trap:
// unaligned atomic` from clobbered dlmalloc state).  Overflow is a
// status error, not a CHECK: region size is embedder-input-dependent
// (a big literal list is a legitimate input), and embedder input must
// not crash the process.  See
// doc/implementation-plan/rewrite/m28-configurable-linking.md §5.3.1.
absl::Status ValidateExprStaticRegion(const StaticLayout& layout) {
  const uint64_t rodata_end =
      static_cast<uint64_t>(layout.rodata_base) + layout.rodata.size();
  // `workspace_base >= rodata_end` by LayoutPass construction, so the
  // region end is the workspace end.
  const uint64_t region_end =
      static_cast<uint64_t>(layout.workspace_base) + layout.workspace_bytes;
  if (region_end > CELWASM_RESERVED_LOW_MEMORY_BYTES) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "compile: expression static region ends at byte ", region_end,
        " (rodata ends at ", rodata_end, ", workspace adds ",
        layout.workspace_bytes, "), past the ",
        CELWASM_RESERVED_LOW_MEMORY_BYTES,
        "-byte low-memory window reserved for it below the runtime's "
        "static data; see doc/implementation-plan/rewrite/"
        "m28-configurable-linking.md §5.3.1"));
  }
  return absl::OkStatus();
}

// All `cel_host.*` host-trampoline imports.  Factored out so both
// dynamic-mode (`InstallExprModuleImports`) and static-mode
// (`CompileStatic`) install them on the same code path with the same
// internal names — `expr_lower`'s `BinaryenCall(kCelHost*InternalName)`
// resolves to the import declared here in either mode.
//
// The runtime-side build (`runtime/cel_runtime.wasm`) also declares
// these as imports, but wasm-ld auto-names them `$<module>_<base>`
// (e.g. `$cel_host_cel_get_field`), which does NOT match the
// expected internal names expr_lower uses.  In static mode the
// adopted runtime carries its own `$cel_host_*`-named imports; we
// install a parallel set under the expected names so codegen calls
// resolve.  Wasm allows multiple imports with the same `(module,
// base, type)` triple, and wasmtime resolves them identically at
// instantiate time.
void InstallCelHostImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType host4[4] = {i32, i32, i32, i32};
  const BinaryenType host3[3] = {i32, i32, i32};
  const BinaryenType host2[2] = {i32, i32};

  // Select arm — kSelectExpr.
  mod.AddFunctionImport(std::string(kCelHostGetFieldInternalName), "cel_host",
                        "cel_get_field", host4, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostHasFieldInternalName), "cel_host",
                        "cel_has_field", host4, BinaryenTypeNone());
  // Batched select-chain arm — one host crossing per contiguous
  // message-typed kSelect chain.  `(out_slot, msg_slot, path_ref_id)`;
  // see wat/71_get_field_path.wat.
  mod.AddFunctionImport(std::string(kCelHostGetFieldPathInternalName),
                        "cel_host", "cel_get_field_path", host3,
                        BinaryenTypeNone());

  // Struct arm — kStructExpr (proto / WKT message literals).
  mod.AddFunctionImport(std::string(kCelHostMakeMessageInternalName),
                        "cel_host", "cel_make_message", host2,
                        BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostSetFieldInternalName), "cel_host",
                        "cel_set_field", host3, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostWktUnwrapTimeInternalName),
                        "cel_host", "cel_wkt_unwrap_time", host2,
                        BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostWktUnwrapWrapperInternalName),
                        "cel_host", "cel_wkt_unwrap_wrapper", host3,
                        BinaryenTypeNone());
  // The aggregate-dispatch host arms — `cel_host.cel_map_lookup` /
  // `cel_host.cel_map_in` / `cel_host.cel_list_at` — are installed
  // alongside their arena siblings by `InstallMapAccessImports` and
  // `InstallListImports`.
}

// Map-key iteration helpers used by comprehensions over a
// `map(K, V)` source.  Always imported regardless of AST presence
// — per CLAUDE.md "no lazy tracking of runtime imports".  Wire
// shapes pinned by `rewrite/wat/64_comprehension_exists_map.wat`:
//   cel_map_iter_init       (map_slot)              -> i32 handle
//   cel_map_iter_next       (handle)                -> i32 (0|1)
//   cel_map_iter_key_at     (out_slot, handle)      -> void
//   cel_map_iter_value_at   (out_slot, handle)      -> void
void InstallMapIterImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType iter_one_param[1] = {i32};
  const BinaryenType iter_two_params[2] = {i32, i32};
  mod.AddFunctionImport("cel_map_iter_init", "cel", "cel_map_iter_init",
                        iter_one_param, i32);
  mod.AddFunctionImport("cel_map_iter_next", "cel", "cel_map_iter_next",
                        iter_one_param, i32);
  mod.AddFunctionImport("cel_map_iter_key_at", "cel", "cel_map_iter_key_at",
                        iter_two_params, BinaryenTypeNone());
  mod.AddFunctionImport("cel_map_iter_value_at", "cel", "cel_map_iter_value_at",
                        iter_two_params, BinaryenTypeNone());
  // `cel_map_count(map_slot) -> i32` — kind-dispatching count helper
  // for comprehension pre-sizing.  Arena: header.count inline; Host:
  // calls `cel_host.cel_map_size` and unboxes.  See m5b §CCF-8.
  mod.AddFunctionImport("cel_map_count", "cel", "cel_map_count", iter_one_param,
                        i32);
  // `cel_list_arena_view(list_slot) -> i32` — kind-dispatching list
  // iter snapshot.  Arena: returns input slot (identity).  Host:
  // allocates a synthetic CEL_LIST_ARENA CelValue + arena snapshot
  // of elements, returns the new slot offset.  Lets the inline
  // arena prologue walk host lists unchanged.  See m5b §CCF-8
  // Slice 2.
  mod.AddFunctionImport("cel_list_arena_view", "cel", "cel_list_arena_view",
                        iter_one_param, i32);
}

// `cel_map_lookup_*` / `cel_map_in_*` — the three-arm dispatch families
// for value lookup and key-presence.  Each arm is `(out, map, key) ->
// void` (lookup) / `(out, key, map) -> void` (in).  Always-imported per
// CLAUDE.md "no lazy tracking of runtime imports".
void InstallMapAccessImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType map3_params[3] = {i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelMapLookupArenaInternalName), "cel",
                        "cel_map_lookup_arena", map3_params,
                        BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelMapLookupInternalName), "cel",
                        "cel_map_lookup", map3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostMapLookupInternalName), "cel_host",
                        "cel_map_lookup", map3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelMapInArenaInternalName), "cel",
                        "cel_map_in_arena", map3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelMapInInternalName), "cel", "cel_map_in",
                        map3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostMapInInternalName), "cel_host",
                        "cel_map_in", map3_params, BinaryenTypeNone());
}

// Map literal + indexing runtime entry points.  `cel_map_*` come
// from the runtime module; `cel_host.cel_map_lookup` is the host
// trampoline arm of the kDynamic dispatcher (see
// `rewrite/map-list-dispatch.md` §3 / §5).
void InstallMapImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType map_create_params[2] = {i32, i32};
  mod.AddFunctionImport(std::string(kCelMapCreateInternalName), "cel",
                        "cel_map_create", map_create_params,
                        BinaryenTypeNone());
  const BinaryenType map3_params[3] = {i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelMapInsertInternalName), "cel",
                        "cel_map_insert", map3_params, BinaryenTypeNone());
  // Dynamic-map insert for transformMap accumulators.
  mod.AddFunctionImport("cel_map_insert_at", "cel", "cel_map_insert_at",
                        map3_params, BinaryenTypeNone());
  // 3VL predicate-gated map insert.
  // `(map_slot, pred_slot, key_slot, value_slot) -> void`.
  const BinaryenType map4_params[4] = {i32, i32, i32, i32};
  mod.AddFunctionImport("cel_map_insert_at_if_bool", "cel",
                        "cel_map_insert_at_if_bool", map4_params,
                        BinaryenTypeNone());
  // Optional-payload predicate-gated insert for `{?key: opt_v}` map
  // entries.  `(map_slot, key_slot, opt_value_slot) -> void`.
  mod.AddFunctionImport(std::string(kCelMapInsertAtIfPresentInternalName),
                        "cel", "cel_map_insert_at_if_present", map3_params,
                        BinaryenTypeNone());
  // Map value-lookup / key-presence dispatch arms (`cel_map_lookup*`
  // / `cel_map_in*`, including the `cel_host.cel_map_lookup` host-rep
  // arm) and the comprehension map/list iteration helpers.  Both
  // installed for both link modes, per CLAUDE.md "no lazy tracking of
  // runtime imports".
  InstallMapAccessImports(mod);
  InstallMapIterImports(mod);
}

// List literal + indexing runtime entry points.  Same shape as
// maps; unused imports are harmless (Binaryen drops them at
// validate).
void InstallListImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType list_create_params[2] = {i32, i32};
  mod.AddFunctionImport(std::string(kCelListCreateInternalName), "cel",
                        "cel_list_create", list_create_params,
                        BinaryenTypeNone());
  const BinaryenType list3_params[3] = {i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelListAtArenaInternalName), "cel",
                        "cel_list_at_arena", list3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelListAtInternalName), "cel",
                        "cel_list_at", list3_params, BinaryenTypeNone());
  // `cel_host.cel_list_at` — the host-representation dispatch arm,
  // installed alongside its arena sibling for both link modes.
  mod.AddFunctionImport(std::string(kCelHostListAtInternalName), "cel_host",
                        "cel_list_at", list3_params, BinaryenTypeNone());
  // Universal append for arena lists — used by both literal codegen
  // (N appends per literal, in index order) and comprehension accu
  // codegen (per-iter for map / filter / transformList).
  // `(list_slot, value_slot) -> void`.
  const BinaryenType append_params[2] = {i32, i32};
  mod.AddFunctionImport("cel_list_append_at", "cel", "cel_list_append_at",
                        append_params, BinaryenTypeNone());
  // Predicate-gated append for filter / conditional-map.
  // `(list_slot, pred_slot, value_slot) -> void`.
  mod.AddFunctionImport("cel_list_append_at_if_bool", "cel",
                        "cel_list_append_at_if_bool", list3_params,
                        BinaryenTypeNone());
  // Optional-payload predicate-gated append for `[?elem]` list
  // entries.  `(list_slot, opt_value_slot) -> void`.
  mod.AddFunctionImport(std::string(kCelListAppendAtIfPresentInternalName),
                        "cel", "cel_list_append_at_if_present", append_params,
                        BinaryenTypeNone());
  // Optional-payload predicate-gated proto-field set for
  // `Foo{?field: opt_value}` entries.  Wasm-side unwrap → delegate to
  // `cel_host.cel_set_field` on Some; no-op on None.
  // `(msg_slot, field_ref_id, opt_value_slot) -> void`.  Kept with
  // the list / map sibling at-if-present helpers — same arity, same
  // wasm-side dispatch pattern.
  const BinaryenType set_params[3] = {i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelSetFieldAtIfPresentInternalName), "cel",
                        "cel_set_field_at_if_present", set_params,
                        BinaryenTypeNone());
}

// Install one wasm function import per OverloadTable seed whose
// runtime export is shipped today.  Per CLAUDE.md "no lazy
// tracking of runtime imports" — every helper the table can name
// gets imported regardless of whether the AST happens to reach it.
//
// Helper signatures: names ending in `_at_vv` take 3 args
// (`out_slot, a_slot, b_slot`); names ending in `_at_v` take 2
// (`out_slot, v_slot`).  The seven aggregate-op dispatchers
// (`cel_list_size` / `cel_list_in` / `cel_list_eq` /
// `cel_list_concat` / `cel_map_size` / `cel_map_in` / `cel_map_eq`)
// don't follow the `_at_*` suffix convention; they get special-
// cased below.
//
// Sets are deduplicated by helper-name: two cel-cpp overload ids
// can resolve to the same wasm helper (every cross-type numeric
// `less_*` resolves to `cel_numeric_lt_at_vv`); we only install
// the import once.
// Install a single overload helper as a wasm import with the
// correct arity-derived parameter shape.  Arity comes from
// `OverloadImpl::num_args` (pre-populated by the OverloadTable
// builder for built-ins via `InferHelperArity`, and by
// `RegisterCustom` for customs).  Returns true if an import was
// installed; false if `num_args` is 0 (caller skips installation —
// matches pre-M13 "unknown arity ⇒ silently skip" behavior).
bool InstallOverloadImport(WasmModule& mod, absl::string_view name,
                           absl::string_view module_name, uint8_t num_args) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType one_param[1] = {i32};
  const BinaryenType v_params[2] = {i32, i32};
  const BinaryenType vv_params[3] = {i32, i32, i32};
  const BinaryenType vvv_params[4] = {i32, i32, i32, i32};
  const BinaryenType vvvv_params[5] = {i32, i32, i32, i32, i32};
  const std::string nstr(name);
  const std::string mstr(module_name);
  switch (num_args) {
    case 5:
      mod.AddFunctionImport(nstr, mstr, nstr, vvvv_params, BinaryenTypeNone());
      return true;
    case 4:
      mod.AddFunctionImport(nstr, mstr, nstr, vvv_params, BinaryenTypeNone());
      return true;
    case 3:
      mod.AddFunctionImport(nstr, mstr, nstr, vv_params, BinaryenTypeNone());
      return true;
    case 2:
      mod.AddFunctionImport(nstr, mstr, nstr, v_params, BinaryenTypeNone());
      return true;
    case 1:
      // 0 value-args + out_slot.  `cel_optional_none_at(out_slot)` is
      // the canonical 1-arg kernel — the 0-input cousin of `_at_v`.
      mod.AddFunctionImport(nstr, mstr, nstr, one_param, BinaryenTypeNone());
      return true;
    default:
      return false;
  }
}

}  // namespace

void InstallOverloadImportsExport(WasmModule& mod,
                                  const OverloadTable& overload_table) {
  // Dedup by (module_name, helper_name) pair — two foreign
  // backends could legitimately export the same helper name under
  // different aliases (`rules.allow` vs `policy.allow`), and we
  // want both imports installed; only same-module same-helper
  // duplicates should be folded.
  //
  // Also skip if the module already has a function under the
  // expected internal name.  Under `LinkMode::kStatic` the adopted
  // runtime module carries every cel.* helper as a defined function;
  // installing a same-named import would conflict.  Custom (cel_fn)
  // imports aren't present in the adopted module, so they still get
  // installed.
  //
  // Cost of the per-entry probe: `BinaryenGetFunction` is a single
  // `std::unordered_map<Name, Function*>` lookup (Binaryen
  // `Module::getFunctionOrNull`), and the table is a fixed few
  // hundred entries (built-in seeds + embedder customs) walked once
  // per Compile — microseconds total, noise next to parse/check and
  // Binaryen IR construction.  Not worth gating on link mode.
  absl::flat_hash_set<std::pair<std::string, std::string>> installed;
  for (uint32_t id = 1; id <= overload_table.size(); ++id) {
    const OverloadImpl& impl = overload_table.LookupById(id);
    const absl::string_view module_name = ImportModuleName(impl);
    const std::string helper_name(impl.name);
    auto key = std::make_pair(std::string(module_name), helper_name);
    if (installed.contains(key)) continue;
    if (BinaryenGetFunction(mod.raw(), helper_name.c_str()) != nullptr) {
      installed.insert(std::move(key));
      continue;
    }
    InstallOverloadImport(mod, helper_name, module_name, impl.num_args);
    installed.insert(std::move(key));
  }

  // `cel_copy_slot` is emitted directly by expr_lower's ternary
  // lowering (BinaryenIf branches that copy the chosen arm into
  // out_slot).  Not seeded in OverloadTable — the table maps
  // cel-cpp overload ids to helpers, and ternary's overload id
  // `conditional` is special-cased above.  Install unconditionally
  // so every emitted module imports it; the no-lazy-imports rule
  // (CLAUDE.md) applies — better to import once and never use than
  // to gate on AST inspection.  Arity from the ABI catalogue.
  auto copy_key =
      std::make_pair(std::string("cel"), std::string("cel_copy_slot"));
  if (!installed.contains(copy_key) &&
      BinaryenGetFunction(mod.raw(), "cel_copy_slot") == nullptr) {
    const auto* helper =
        abi::FindBuiltinHelper(abi::AbiModule::kCelRuntime, "cel_copy_slot");
    ABSL_CHECK(helper != nullptr)
        << "cel_copy_slot missing from ABI catalogue — see "
           "abi/runtime_catalogue.cc";
    InstallOverloadImport(mod, "cel_copy_slot", "cel", helper->num_args());
    installed.insert(std::move(copy_key));
  }
}

namespace {

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
}  // namespace

absl::Status InstallExprModuleImports(WasmModule& mod,
                                      const StaticLayout& layout,
                                      uint32_t mem_size_bytes) {
  WasmModule::DataSegment seg{layout.rodata_base, layout.rodata};
  // Phase C: the runtime (cel_runtime.wasm) is built on
  // wasm32-wasi-threads and exports its memory as shared.  The expr
  // module's `(import "cel" "memory")` must therefore declare a
  // matching shared memory with a max page count, derived from the
  // runtime's `-Wl,--max-memory=` (mirrored as
  // `MemoryLayout::kMaxMemoryBytes`).
  constexpr uint32_t kSharedMaxPages =
      MemoryLayout::kMaxMemoryBytes / MemoryLayout::kWasmPageSize;
  auto s = mod.AddMemoryImport("cel", "memory", PagesForBytes(mem_size_bytes),
                               /*max_pages=*/kSharedMaxPages,
                               absl::MakeConstSpan(&seg, 1),
                               /*shared=*/true);
  if (!s.ok()) return s;

  const BinaryenType i32 = BinaryenTypeInt32();
  mod.AddFunctionImport(kArenaResetInternalName, "cel", "arena_reset",
                        absl::Span<const BinaryenType>{}, BinaryenTypeNone());
  const BinaryenType alloc_params[1] = {i32};
  mod.AddFunctionImport("arena_alloc", "cel", "arena_alloc", alloc_params, i32);
  InstallCelHostImports(mod);
  InstallMapImports(mod);
  InstallListImports(mod);
  return absl::OkStatus();
}

namespace {

// Attach the `cel.abi` custom section to the module.  Payload is a
// serialised `celwasm.abi.CelAbi` proto populated from the
// compile-time layout.  Engine::Plan reads it at load time to build
// the runtime lookup tables Instance::Eval(Activation) needs.
// `link_mode` is the wire-format marker for the mode this module was
// emitted under — embedder-tooling metadata, not an engine routing
// input (see doc/implementation-plan/rewrite/m28-configurable-linking.md
// §6).
absl::Status AttachCelAbiSection(WasmModule& module, const StaticLayout& layout,
                                 absl::Span<const FieldRefRow> field_refs,
                                 absl::Span<const PathRefRow> path_refs,
                                 celwasm::abi::LinkMode link_mode) {
  auto abi_or = BuildCelAbi(layout, field_refs, path_refs, link_mode);
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
// function under the function-size lint threshold.  `layout_opts`
// flows to LayoutPass — the kStatic merge path uses it to shift
// rodata above the runtime's own static-data range (see
// `CompileStatic`).
absl::StatusOr<CompiledArtifact> RunFrontAndLayout(
    absl::string_view expression, const CompileOptions& opts,
    const LayoutOptions& layout_opts = {}) {
  auto ast_or = ParseAndCheck(expression, opts.check);
  if (!ast_or.ok()) return ast_or.status();
  auto resolved_or = ResolvePass(*ast_or);
  if (!resolved_or.ok()) return resolved_or.status();
  auto layout_or = LayoutPass(*ast_or, *std::move(resolved_or), layout_opts);
  if (!layout_or.ok()) return layout_or.status();
  // The default layout places the static region at the bottom of
  // linear memory, inside the reserved low-memory window; validate it
  // fits.  A caller passing `rodata_base_override` is deliberately
  // relocating the region (e.g. above the runtime's static-data
  // range) and owns its own budget.
  if (layout_opts.rodata_base_override == 0) {
    if (auto s = ValidateExprStaticRegion(*layout_or); !s.ok()) return s;
  }
  return CompiledArtifact{
      /*ast=*/*std::move(ast_or),
      /*layout=*/*std::move(layout_or),
      /*module=*/WasmModule(),
      /*eval_fn=*/{nullptr},
      /*wasm_bytes=*/{},
  };
}

// Validate + optionally optimize + optionally serialize the emitted
// module.  Back half of the pipeline.  Order matters: validate FIRST
// (proves the module is well-formed before the optimizer touches it),
// then optimize (mutates IR — would invalidate validator state),
// then serialize.
absl::Status FinaliseModule(CompiledArtifact& out, const CompileOptions& opts) {
  if (opts.validate) {
    auto v = out.module.Validate();
    if (!v.ok()) return v;
  }
  if (opts.optimize_level > 0) {
    auto o = out.module.Optimize(opts.optimize_level);
    if (!o.ok()) return o;
  }
  if (opts.serialize) {
    auto bytes_or = out.module.Serialize();
    if (!bytes_or.ok()) return bytes_or.status();
    out.wasm_bytes = *std::move(bytes_or);
  }
  return absl::OkStatus();
}

// Build the OverloadTable from the built-in seeds plus the
// embedder's `function_libraries`.  Each library decl maps to one
// `RegisterCustom` row:
//   - `overload_id`         = decl's synthesised id (the cel-cpp
//     checker stamps this on resolved call nodes; ResolvePass uses
//     it as the OverloadTable lookup key).
//   - `module`              = the wasm import-module routing:
//                             host-callback path (`cel_fn`) for
//                             `kHost` and `kForeignComponent`
//                             (Component-Model backend dispatched
//                             via a host callback — m24 §2-§3
//                             "a component fn is a host fn at the
//                             call site"); per-module `kUserModule`
//                             for `kCelDefined`.
//   - `module_name`         = the per-decl module string for kUserModule
//                             (empty for kCelFn — OverloadTable hardcodes
//                             the import-module string for kCelFn).
//   - `helper_name`         = same as `overload_id` (one wasm
//     import per decl; the IDL guarantees uniqueness).
//   - `num_args`            = wasm function arity (1 out_slot + N
//     CEL args, as recorded on `CelfnDecl::num_args`).
bool DispatchesViaCelFn(CelfnDecl::Backend backend) {
  return backend == CelfnDecl::Backend::kHost ||
         backend == CelfnDecl::Backend::kForeignComponent;
}

absl::StatusOr<OverloadTable> BuildOverloadTable(
    const std::vector<FunctionLibrary>& libraries) {
  OverloadTableBuilder builder;
  for (const auto& lib : libraries) {
    for (const auto& decl : lib.decls()) {
      const bool via_cel_fn = DispatchesViaCelFn(decl.backend);
      const ImportModule import_module =
          via_cel_fn ? ImportModule::kCelFn : ImportModule::kUserModule;
      const absl::string_view module_name =
          via_cel_fn ? absl::string_view("") : decl.module_name;
      if (auto s = builder.RegisterCustom(decl.overload_id, import_module,
                                          module_name, decl.overload_id,
                                          decl.num_args);
          !s.ok()) {
        return s;
      }
    }
  }
  return std::move(builder).Build();
}

// Adopt the wrapper-stripped runtime wasm as the base Binaryen
// module for static-mode emission.
absl::StatusOr<WasmModule> AdoptStrippedRuntime() {
  // Cast away the `const` on the byte pointer — Binaryen's API
  // spelling takes a mutable `char*` but does not mutate the input on
  // the deserialize path (it copies into its own arena before any
  // in-place rewriting).
  auto* raw_bytes = reinterpret_cast<char*>(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      const_cast<unsigned char*>(kCelRuntimeStrippedWasmBytes));
  BinaryenModuleRef adopted =
      BinaryenModuleRead(raw_bytes, kCelRuntimeStrippedWasmBytesSize);
  if (adopted == nullptr) {
    return absl::InternalError(
        "CompileStatic: BinaryenModuleRead failed on embedded "
        "cel_runtime_stripped_wasm bytes");
  }
  return WasmModule::Adopt(adopted);
}

// Install expr's rodata as an active data segment on the adopted
// runtime's memory.  The runtime is linked with
// `-Wl,--global-base=CELWASM_RESERVED_LOW_MEMORY_BYTES` (8 KiB), so
// the bottom 8 KiB of linear memory is reserved by design for the
// expr module's static region; the runtime's own wasi-libc static
// data + stack + heap live above that.  `ValidateExprStaticRegion`
// (run by `RunFrontAndLayout` in both link modes) already rejected
// any layout ending past the boundary; the CHECK here is the
// tripwire against that gate regressing — a segment installed past
// the window would silently overwrite the runtime's static data at
// instantiate time.
//
// The runtime declares its memory with the clang/wasi-libc-emitted
// internal name `"0"` (single-char index name); our own freshly-built
// modules use `"memory"`.
void InstallExprRodataSegment(WasmModule& mod, const StaticLayout& layout) {
  const uint64_t rodata_end =
      static_cast<uint64_t>(layout.rodata_base) + layout.rodata.size();
  ABSL_CHECK_LE(rodata_end, CELWASM_RESERVED_LOW_MEMORY_BYTES)
      << "expression rodata past the reserved low-memory window reached "
         "segment install — ValidateExprStaticRegion gate regressed";
  mod.AddActiveDataSegment(layout.rodata_base, layout.rodata,
                           /*memory_name=*/"0");
}

// Shared back half of both link-mode arms, run after the arm's
// mode-specific module bootstrap (fresh module + full import surface
// for kDynamic; adopted stripped runtime + rodata segment + cel_host
// imports for kStatic).  Builds the OverloadTable (built-ins +
// embedder customs — `LowerToEvalFunction` needs it to map cel-cpp
// overload-ids to wasm helper names), installs any overload imports
// the module doesn't already define (`InstallOverloadImportsExport`
// self-skips helpers that are defined functions in the adopted
// runtime, so under kStatic only custom `cel_fn.*` imports land),
// lowers `$eval`, exports it, attaches the `cel.abi` section stamped
// with `link_mode`, and finalises (validate / optimize / serialize).
// Factored into one helper so the two arms can't silently diverge.
absl::StatusOr<CompiledArtifact> LowerExportAndFinalise(
    CompiledArtifact out, const CompileOptions& opts,
    celwasm::abi::LinkMode link_mode) {
  auto overload_table_or = BuildOverloadTable(opts.function_libraries);
  if (!overload_table_or.ok()) return overload_table_or.status();
  OverloadTable overload_table = *std::move(overload_table_or);
  InstallOverloadImportsExport(out.module, overload_table);

  LoweringOptions lower_opts;
  lower_opts.mem_size_bytes = opts.mem_size_bytes;
  auto lowered_or =
      LowerToEvalFunction(out.ast, out.layout, opts.eval_internal_name,
                          out.module, overload_table, lower_opts);
  if (!lowered_or.ok()) return lowered_or.status();
  out.eval_fn = *std::move(lowered_or);

  out.module.ExportFunction(opts.eval_internal_name, opts.eval_export_name);

  if (auto s = AttachCelAbiSection(
          out.module, out.layout, absl::MakeConstSpan(out.eval_fn.field_refs),
          absl::MakeConstSpan(out.eval_fn.path_refs), link_mode);
      !s.ok()) {
    return s;
  }
  if (auto s = FinaliseModule(out, opts); !s.ok()) return s;
  return out;
}

// kStatic link mode.  Loads the wrapper-stripped runtime wasm as the
// base Binaryen module, then emits `$eval` against that module so the
// expression's calls to runtime helpers (`arena_reset`, `cel_int_add_at_vv`,
// `cel_list_at_arena`, …) bind intra-module rather than via imports.
// Skips `InstallExprModuleImports` entirely — every name it installs
// is already a defined function in the adopted module, and adding
// duplicate imports would conflict.
absl::StatusOr<CompiledArtifact> CompileStatic(absl::string_view expression,
                                               const CompileOptions& opts) {
  auto out_or = RunFrontAndLayout(expression, opts);
  if (!out_or.ok()) return out_or.status();
  CompiledArtifact out = *std::move(out_or);

  auto adopted_or = AdoptStrippedRuntime();
  if (!adopted_or.ok()) return adopted_or.status();
  out.module = *std::move(adopted_or);

  InstallExprRodataSegment(out.module, out.layout);

  // Install `cel_host.*` trampoline imports under the internal names
  // `expr_lower` references via `BinaryenCall(kCelHost*InternalName)`.
  // The adopted runtime already imports the same `(cel_host, X)`
  // pairs — but under wasm-ld-assigned internal names like
  // `$cel_host_cel_get_field` that codegen never targets.  Wasm allows
  // multiple imports under the same `(module, base, type)` triple;
  // wasmtime resolves both to the same host trampoline at instantiate
  // time, so the parallel set is harmless to the runtime side.
  InstallCelHostImports(out.module);

  return LowerExportAndFinalise(std::move(out), opts,
                                celwasm::abi::LINK_MODE_STATIC);
}

}  // namespace

absl::StatusOr<CompiledArtifact> Compile(absl::string_view expression,
                                         const CompileOptions& opts) {
  if (opts.link_mode == CompileOptions::LinkMode::kStatic) {
    return CompileStatic(expression, opts);
  }
  auto out_or = RunFrontAndLayout(expression, opts);
  if (!out_or.ok()) return out_or.status();
  CompiledArtifact out = *std::move(out_or);

  if (auto s =
          InstallExprModuleImports(out.module, out.layout, opts.mem_size_bytes);
      !s.ok()) {
    return s;
  }

  return LowerExportAndFinalise(std::move(out), opts,
                                celwasm::abi::LINK_MODE_DYNAMIC);
}

}  // namespace celwasm
