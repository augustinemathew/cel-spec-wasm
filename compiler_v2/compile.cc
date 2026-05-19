#include "compiler_v2/compile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
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
#include "compiler_v2/codegen/overload_table.h"
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

// `cel_host.cel_get_field` + `cel_host.cel_has_field` trampolines.
// Always imported — the runtime links the cel_host module
// unconditionally (see CLAUDE.md "no lazy tracking of runtime
// imports").
void InstallSelectImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType host_params[4] = {i32, i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelHostGetFieldInternalName), "cel_host",
                        "cel_get_field", host_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostHasFieldInternalName), "cel_host",
                        "cel_has_field", host_params, BinaryenTypeNone());
}

// `cel_host.cel_make_message` — `(type_id, out_slot)` → ().
// `cel_host.cel_set_field` — `(msg_slot, field_ref_id, value_slot)`
// → ().  Both always imported (no lazy tracking — CLAUDE.md);
// codegen emits direct calls in the kStructExpr arm.
// `cel_host.cel_wkt_unwrap_time` — `(out_slot, msg_slot)` → ().
// Conditional emit at the kStructExpr tail for WKT
// Timestamp/Duration literals.
// `cel_host.cel_wkt_unwrap_wrapper` —
// `(out_slot, msg_slot, wrapper_kind)` → ().  Conditional emit at
// the kStructExpr tail for the 9 wrapper FQNs.  Uninstalled
// imports validate fine — keep them always installed per
// CLAUDE.md "no lazy tracking" rule.  See
// `rewrite/m7-proto-literals.md` and `rewrite/m8-wrapper-types.md`.
void InstallStructImports(WasmModule& mod) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType make_params[2] = {i32, i32};
  mod.AddFunctionImport(std::string(kCelHostMakeMessageInternalName),
                        "cel_host", "cel_make_message", make_params,
                        BinaryenTypeNone());
  const BinaryenType set_params[3] = {i32, i32, i32};
  mod.AddFunctionImport(std::string(kCelHostSetFieldInternalName), "cel_host",
                        "cel_set_field", set_params, BinaryenTypeNone());
  // 2-arg `(out_slot, msg_slot)` — `make_params` happens to match.
  mod.AddFunctionImport(std::string(kCelHostWktUnwrapTimeInternalName),
                        "cel_host", "cel_wkt_unwrap_time", make_params,
                        BinaryenTypeNone());
  // 3-arg `(out_slot, msg_slot, wrapper_kind)` — `set_params` happens
  // to match (same i32, i32, i32 shape).
  mod.AddFunctionImport(std::string(kCelHostWktUnwrapWrapperInternalName),
                        "cel_host", "cel_wkt_unwrap_wrapper", set_params,
                        BinaryenTypeNone());
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
  mod.AddFunctionImport(std::string(kCelMapLookupArenaInternalName), "cel",
                        "cel_map_lookup_arena", map3_params,
                        BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelMapLookupInternalName), "cel",
                        "cel_map_lookup", map3_params, BinaryenTypeNone());
  mod.AddFunctionImport(std::string(kCelHostMapLookupInternalName), "cel_host",
                        "cel_map_lookup", map3_params, BinaryenTypeNone());
  // Map-key iteration helpers used by comprehensions over a
  // `map(K, V)` source.  Always imported regardless of AST presence
  // — per CLAUDE.md "no lazy tracking of runtime imports".  Wire
  // shapes pinned by `rewrite/wat/64_comprehension_exists_map.wat`:
  //   cel_map_iter_init       (map_slot)              -> i32 handle
  //   cel_map_iter_next       (handle)                -> i32 (0|1)
  //   cel_map_iter_key_at     (out_slot, handle)      -> void
  //   cel_map_iter_value_at   (out_slot, handle)      -> void
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
// Returns the helper arity inferred from the overload-helper name:
// `_at_vv` → 3-arg, `_at_v` → 2-arg, aggregate-op dispatchers
// (`cel_list_size`/etc.) carry no suffix and use a hand-rolled
// table.  Returns 0 for "not a helper we install here".
int OverloadHelperArity(absl::string_view name) {
  if (name.size() >= 6 && name.substr(name.size() - 6) == "_at_vv") {
    return 3;
  }
  if (name.size() >= 5 && name.substr(name.size() - 5) == "_at_v") {
    return 2;
  }
  static constexpr struct {
    absl::string_view name;
    int arity;
  } kDispatchers[] = {
      {"cel_list_size", 2},
      {"cel_list_in", 3},
      {"cel_list_eq", 3},
      {"cel_list_concat", 3},
      {"cel_map_size", 2},
      {"cel_map_in", 3},
      {"cel_map_eq", 3},
      // 3VL / control-flow helpers.  cel_and / cel_or are 3-arg;
      // cel_not is 2-arg.  cel_unknown_merge is reachable only
      // through cel_and / cel_or internally so it doesn't need an
      // expr-side import; cel_copy_slot is installed unconditionally
      // by InstallOverloadImports below for the ternary lowering.
      {"cel_and", 3},
      {"cel_or", 3},
      {"cel_not", 2},
      // `cel_copy_slot` is the kernel for identity conversions
      // (`bool(bool)` / `int(int)` / ...).  Its `(dst, src) → void`
      // signature is 2-arg; it doesn't follow the `_at_v` suffix
      // convention because the ternary lowering emits it directly,
      // not via OverloadTable seeding.
      {"cel_copy_slot", 2},
      // cel_host parse + format trampolines.  Names match the host
      // ABI canonical form (`cel_timestamp_parse` etc., not
      // `cel_timestamp_parse_at_v`) — the `_at_v` suffix is a
      // runtime-side convention; host trampolines stay unsuffixed
      // for consistency with cel_get_field / cel_make_message /
      // cel_map_lookup.
      {"cel_timestamp_parse", 2},
      {"cel_duration_parse", 2},
      {"cel_timestamp_format", 2},
      {"cel_duration_format", 2},
  };
  for (const auto& d : kDispatchers) {
    if (d.name == name) return d.arity;
  }
  return 0;
}

void InstallOverloadImports(WasmModule& mod,
                            const OverloadTable& overload_table) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType vv_params[3] = {i32, i32, i32};
  const BinaryenType v_params[2] = {i32, i32};
  absl::flat_hash_set<std::string> installed;
  for (uint32_t id = 1; id <= overload_table.size(); ++id) {
    const OverloadImpl& impl = overload_table.LookupById(id);
    // kCelHost seeds install with import_module="cel_host" (host
    // trampolines).  The binaryen function symbol matches the
    // helper name regardless of module — helper names are globally
    // unique (kCelHost trampoline names start with `cel_timestamp_`
    // / `cel_duration_`, kCelRuntime helpers with `cel_int_` /
    // `cel_string_` / etc).
    const char* module_name = nullptr;
    switch (impl.module) {
      case ImportModule::kCelRuntime:
        module_name = "cel";
        break;
      case ImportModule::kCelHost:
        module_name = "cel_host";
        break;
    }
    if (module_name == nullptr) continue;
    const std::string name(impl.name);
    if (installed.contains(name)) continue;
    const int arity = OverloadHelperArity(impl.name);
    if (arity == 3) {
      mod.AddFunctionImport(name, module_name, name, vv_params,
                            BinaryenTypeNone());
    } else if (arity == 2) {
      mod.AddFunctionImport(name, module_name, name, v_params,
                            BinaryenTypeNone());
    }
    installed.insert(name);
  }

  // `cel_copy_slot` is emitted directly by expr_lower's ternary
  // lowering (BinaryenIf branches that copy the chosen arm into
  // out_slot).  Not seeded in OverloadTable —
  // the table maps cel-cpp overload ids to helpers, and ternary's
  // overload id `conditional` is special-cased above.  Install
  // unconditionally so every emitted module imports it; the no-lazy-
  // imports rule (CLAUDE.md) applies — better to import once and
  // never use than to gate on AST inspection.
  if (!installed.contains("cel_copy_slot")) {
    mod.AddFunctionImport("cel_copy_slot", "cel", "cel_copy_slot", v_params,
                          BinaryenTypeNone());
    installed.insert("cel_copy_slot");
  }
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
  // Phase C: the runtime (cel_runtime.wasm) is built on
  // wasm32-wasi-threads and exports its memory as shared.  The expr
  // module's `(import "cel" "memory")` must therefore declare a
  // matching shared memory with a max page count.  Pick 1024 pages
  // (64 MiB) to mirror the runtime's `-Wl,--max-memory=67108864`.
  constexpr uint32_t kSharedMaxPages = 1024;
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
  InstallSelectImports(mod);
  InstallMapImports(mod);
  InstallListImports(mod);
  InstallStructImports(mod);
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

  // Build the OverloadTable (built-ins only; embedder custom
  // functions land via `RegisterFunction` — see
  // `rewrite/m-custom-fns.md`) and install one wasm import per
  // shipped helper before lowering.  expr_lower's
  // general-arm `BinaryenCall` references these imports by
  // internal name; if an import is missing the module won't
  // validate.
  OverloadTable overload_table = OverloadTableBuilder().Build();
  InstallOverloadImports(out.module, overload_table);

  LoweringOptions lower_opts;
  lower_opts.mem_size_bytes = opts.mem_size_bytes;
  auto lowered_or =
      LowerToEvalFunction(out.ast, out.layout, opts.eval_internal_name,
                          out.module, overload_table, lower_opts);
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
