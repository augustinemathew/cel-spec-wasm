// M13 Probe 4 — engine-owned module model.
//
// Validates the engine-owned design from the M13 §2.1 refinement
// discussion: the engine holds a map of registered foreign modules
// (alias → instance) and resolves a caller's wasm imports against
// that map at `Plan` time.  Conflicts (alias re-registered, import
// unmet, foreign module missing an expected export) surface at the
// earliest reasonable point.
//
// What this probe proves:
//
//   * AddModule registers a foreign wasm under an alias.  The engine
//     instantiates it, calls `_initialize` if exported, extracts the
//     memory + exports.
//   * Re-registering the same alias is an AlreadyExists error at
//     AddModule time (not deferred to Plan).
//   * Plan walks the caller wasm's import table; each import
//     `(module, helper)` is resolved against the engine's registered
//     modules.  Missing modules fail Plan with a clear message
//     citing the offending import.
//   * Engine-discovered overload-id conflicts (a module exports
//     `allow_string_string`, another module also exports it) are
//     observable from the wasm export tables — no `cel.toolchain`
//     custom section needed.  The probe walks exports and asserts
//     overload-id presence/absence from raw bytes.
//
// Today this is a probe-stage `ProbeEngine` helper class, not the
// production `cel::Engine`.  Slice C will land the same model
// inside the public `Engine` API; this probe locks the design.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/runtime/cel_data.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

using ::testing::HasSubstr;

// ──────────────────────────────────────────────────────────────────
// ProbeEngine — minimal engine-owned model for validating the M13
// design.  Owns the wasmtime engine + store and a map of registered
// foreign modules.  This is intentionally NOT shared with production
// `cel::Engine` (api/engine.cc) — the probe proves the design
// independently.

class ProbeEngine {
 public:
  ProbeEngine() {
    engine_ = wasm_engine_new();
    store_ = wasmtime_store_new(engine_, nullptr, nullptr);
    ctx_ = wasmtime_store_context(store_);
  }
  ProbeEngine(const ProbeEngine&) = delete;
  ProbeEngine& operator=(const ProbeEngine&) = delete;

  ~ProbeEngine() {
    for (auto& [alias, mod] : modules_by_alias_) {
      wasmtime_module_delete(mod.module);
    }
    if (store_ != nullptr) wasmtime_store_delete(store_);
    if (engine_ != nullptr) wasm_engine_delete(engine_);
  }

  // Register `wasm_bytes` under `alias`.  Instantiates against an
  // empty linker (the foreign module must be self-contained for the
  // probe), calls `_initialize` if exported, snapshots the exported
  // set.  Returns AlreadyExists on a duplicate alias.
  absl::Status AddModule(absl::string_view alias,
                         absl::Span<const uint8_t> wasm_bytes) {
    if (modules_by_alias_.find(std::string(alias)) != modules_by_alias_.end()) {
      return absl::AlreadyExistsError(
          absl::StrCat("module alias `", alias, "` already registered"));
    }

    Module m{};
    if (auto err = wasmtime_module_new(engine_, wasm_bytes.data(),
                                       wasm_bytes.size(), &m.module);
        err != nullptr) {
      return WrapWasmtimeError("module_new", err);
    }
    wasmtime_linker_t* linker = wasmtime_linker_new(engine_);
    wasm_trap_t* trap = nullptr;
    auto err = wasmtime_linker_instantiate(linker, ctx_, m.module, &m.instance,
                                           &trap);
    wasmtime_linker_delete(linker);
    if (err != nullptr) {
      wasmtime_module_delete(m.module);
      return WrapWasmtimeError("instantiate", err);
    }
    if (trap != nullptr) {
      wasmtime_module_delete(m.module);
      return WrapWasmtimeTrap("instantiate", trap);
    }

    // Call `_initialize` if the module exports it.
    wasmtime_extern_t init_ext;
    if (wasmtime_instance_export_get(ctx_, &m.instance, "_initialize", 11,
                                     &init_ext) &&
        init_ext.kind == WASMTIME_EXTERN_FUNC) {
      wasmtime_func_t init_fn = init_ext.of.func;
      wasm_trap_t* init_trap = nullptr;
      err = wasmtime_func_call(ctx_, &init_fn, nullptr, 0, nullptr, 0,
                               &init_trap);
      if (err != nullptr) {
        wasmtime_module_delete(m.module);
        return WrapWasmtimeError("_initialize", err);
      }
      if (init_trap != nullptr) {
        wasmtime_module_delete(m.module);
        return WrapWasmtimeTrap("_initialize", init_trap);
      }
    }

    // Snapshot the exported helper names (function exports only).
    // `cel.toolchain` would let us authenticate ABI version; here
    // we just enumerate the function-export names so the engine can
    // answer "does this module fulfill helper X?" without re-running
    // wasmtime export lookups on every query.
    m.helper_exports = SnapshotFunctionExports(m.instance);

    // Extract memory (every foreign module exports one in our v1
    // probe set; production code would handle "imports memory"
    // shape per §4.5.2 too).
    wasmtime_extern_t mem_ext;
    if (wasmtime_instance_export_get(ctx_, &m.instance, "memory", 6,
                                     &mem_ext) &&
        mem_ext.kind == WASMTIME_EXTERN_MEMORY) {
      m.memory_ext = mem_ext;
      m.has_memory = true;
    }

    modules_by_alias_.emplace(std::string(alias), std::move(m));
    return absl::OkStatus();
  }

  // Does the engine know about a module exporting `helper_name`?
  // Used by Plan + by direct overload-id-conflict checks.
  bool ModuleExportsHelper(absl::string_view alias,
                           absl::string_view helper_name) const {
    auto it = modules_by_alias_.find(std::string(alias));
    if (it == modules_by_alias_.end()) return false;
    for (const auto& e : it->second.helper_exports) {
      if (e == helper_name) return true;
    }
    return false;
  }

  // Discover overload-id conflicts ACROSS already-registered modules.
  // Returns the set of helper-names that appear in 2+ modules' export
  // tables (excluding ABI noise like `_initialize`, `__data_end`,
  // memory pseudo-exports).  Empty result == no conflict.
  //
  // This is what the design's "look at the wasm exports" approach
  // gives us for free — no custom section needed.
  std::vector<std::string> CrossModuleOverloadConflicts() const {
    absl::flat_hash_map<std::string, std::vector<std::string>> by_name;
    for (const auto& [alias, mod] : modules_by_alias_) {
      for (const auto& e : mod.helper_exports) {
        by_name[e].push_back(alias);
      }
    }
    std::vector<std::string> conflicts;
    for (const auto& [name, aliases] : by_name) {
      if (aliases.size() >= 2) conflicts.push_back(name);
    }
    return conflicts;
  }

  // Plan: assemble `caller_wat`, walk its imports, wire them from
  // the engine's registered modules + return the instantiated
  // caller.  Returns FailedPrecondition if an import is unmet.
  struct Planned {
    wasmtime_instance_t instance;
    wasmtime_memory_t memory;  // the canonical shared memory
  };
  absl::StatusOr<Planned> Plan(absl::string_view caller_wat) {
    wasm_byte_vec_t out;
    if (auto err = wasmtime_wat2wasm(caller_wat.data(), caller_wat.size(),
                                     &out);
        err != nullptr) {
      return WrapWasmtimeError("wat2wasm", err);
    }
    wasmtime_module_t* caller_module = nullptr;
    auto err = wasmtime_module_new(engine_,
                                   reinterpret_cast<const uint8_t*>(out.data),
                                   out.size, &caller_module);
    const std::vector<uint8_t> caller_bytes(
        reinterpret_cast<const uint8_t*>(out.data),
        reinterpret_cast<const uint8_t*>(out.data) + out.size);
    wasm_byte_vec_delete(&out);
    if (err != nullptr) return WrapWasmtimeError("module_new(caller)", err);

    // Walk caller imports.  For each (module_name, item_name) the
    // engine resolves: if module_name is a registered foreign
    // module's alias, look up `item_name` from that module's
    // exports; if module_name is "cel" + item_name is "memory",
    // resolve to the first registered module's exported memory.
    wasmtime_linker_t* linker = wasmtime_linker_new(engine_);

    wasm_importtype_vec_t imports;
    wasmtime_module_imports(caller_module, &imports);
    absl::Status link_status = absl::OkStatus();
    for (size_t i = 0; i < imports.size; ++i) {
      const wasm_name_t* mod_name = wasm_importtype_module(imports.data[i]);
      const wasm_name_t* item_name = wasm_importtype_name(imports.data[i]);
      const absl::string_view mod_sv(mod_name->data, mod_name->size);
      const absl::string_view item_sv(item_name->data, item_name->size);

      // `cel.memory` → the canonical shared memory.  Pick the first
      // registered module that exports memory.  (Conflict resolution
      // for multi-memory-defining modules is a Plan-time policy; for
      // the probe, FIFO is fine — the probe only registers one.)
      if (mod_sv == "cel" && item_sv == "memory") {
        const Module* mem_module = FirstModuleWithMemory();
        if (mem_module == nullptr) {
          link_status = absl::FailedPreconditionError(absl::StrCat(
              "caller imports `cel.memory` but no registered foreign module "
              "exports memory"));
          break;
        }
        wasmtime_extern_t mem_ext = mem_module->memory_ext;
        if (auto e = wasmtime_linker_define(linker, ctx_, mod_sv.data(),
                                            mod_sv.size(), item_sv.data(),
                                            item_sv.size(), &mem_ext);
            e != nullptr) {
          link_status = WrapWasmtimeError("linker.define(cel.memory)", e);
          break;
        }
        continue;
      }

      // Otherwise resolve `<alias>.<helper>` against registered modules.
      auto it = modules_by_alias_.find(std::string(mod_sv));
      if (it == modules_by_alias_.end()) {
        link_status = absl::FailedPreconditionError(absl::StrCat(
            "caller imports `", mod_sv, ".", item_sv,
            "` but no module registered under alias `", mod_sv, "`"));
        break;
      }
      wasmtime_extern_t ext;
      if (!wasmtime_instance_export_get(ctx_, &it->second.instance,
                                        item_sv.data(), item_sv.size(),
                                        &ext)) {
        link_status = absl::FailedPreconditionError(absl::StrCat(
            "module `", mod_sv, "` does not export `", item_sv, "`"));
        break;
      }
      if (auto e = wasmtime_linker_define(linker, ctx_, mod_sv.data(),
                                          mod_sv.size(), item_sv.data(),
                                          item_sv.size(), &ext);
          e != nullptr) {
        link_status =
            WrapWasmtimeError(absl::StrCat("linker.define(", mod_sv, ".",
                                           item_sv, ")"),
                              e);
        break;
      }
    }
    wasm_importtype_vec_delete(&imports);

    if (!link_status.ok()) {
      wasmtime_linker_delete(linker);
      wasmtime_module_delete(caller_module);
      return link_status;
    }

    Planned planned{};
    wasm_trap_t* trap = nullptr;
    err = wasmtime_linker_instantiate(linker, ctx_, caller_module,
                                      &planned.instance, &trap);
    wasmtime_linker_delete(linker);
    wasmtime_module_delete(caller_module);
    if (err != nullptr) return WrapWasmtimeError("instantiate(caller)", err);
    if (trap != nullptr) return WrapWasmtimeTrap("instantiate(caller)", trap);

    const Module* mem_module = FirstModuleWithMemory();
    if (mem_module != nullptr) {
      planned.memory = mem_module->memory_ext.of.memory;
    }
    return planned;
  }

  wasmtime_context_t* ctx() { return ctx_; }

 private:
  struct Module {
    wasmtime_module_t* module = nullptr;
    wasmtime_instance_t instance{};
    std::vector<std::string> helper_exports;
    wasmtime_extern_t memory_ext{};
    bool has_memory = false;
  };

  // Walk the instance's exports and return function-export names
  // that look like overload-ids (not toolchain noise).  Heuristic:
  // skip leading-underscore names (`_initialize`, `__data_end`,
  // `__heap_base`, `__stack_pointer`, …) — those are toolchain
  // artifacts, not CEL overloads.
  std::vector<std::string> SnapshotFunctionExports(
      const wasmtime_instance_t& inst) {
    std::vector<std::string> out;
    size_t i = 0;
    char* name = nullptr;
    size_t name_len = 0;
    wasmtime_extern_t ext;
    while (wasmtime_instance_export_nth(ctx_, &inst, i, &name, &name_len,
                                        &ext)) {
      const absl::string_view nm(name, name_len);
      if (ext.kind == WASMTIME_EXTERN_FUNC && !nm.empty() && nm[0] != '_') {
        out.emplace_back(nm);
      }
      ++i;
    }
    return out;
  }

  const Module* FirstModuleWithMemory() const {
    for (const auto& [alias, mod] : modules_by_alias_) {
      if (mod.has_memory) return &mod;
    }
    return nullptr;
  }

  static absl::Status WrapWasmtimeError(absl::string_view ctx,
                                        wasmtime_error_t* err) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    absl::Status s = absl::InternalError(
        absl::StrCat(ctx, ": ", std::string(msg.data, msg.size)));
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return s;
  }
  static absl::Status WrapWasmtimeTrap(absl::string_view ctx,
                                       wasm_trap_t* trap) {
    wasm_byte_vec_t msg;
    wasm_trap_message(trap, &msg);
    absl::Status s = absl::InternalError(
        absl::StrCat(ctx, ": ", std::string(msg.data, msg.size)));
    wasm_byte_vec_delete(&msg);
    wasm_trap_delete(trap);
    return s;
  }

  wasm_engine_t* engine_ = nullptr;
  wasmtime_store_t* store_ = nullptr;
  wasmtime_context_t* ctx_ = nullptr;
  std::map<std::string, Module> modules_by_alias_;
};

// ──────────────────────────────────────────────────────────────────
// Test helpers.

std::string ReadWorkspaceFile(absl::string_view path) {
  std::ifstream in(std::string{path}, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ──────────────────────────────────────────────────────────────────
// Tests.

TEST(M13Probe4Engine, AddModuleRegistersAndExposesExports) {
  ProbeEngine engine;
  const std::string rules_bytes = ReadWorkspaceFile(
      "compiler_v2/probes/m13_custom_fns/rules/rules.wasm");
  ASSERT_FALSE(rules_bytes.empty());

  ASSERT_TRUE(engine.AddModule(
                    "rules",
                    absl::MakeConstSpan(
                        reinterpret_cast<const uint8_t*>(rules_bytes.data()),
                        rules_bytes.size()))
                  .ok());
  EXPECT_TRUE(
      engine.ModuleExportsHelper("rules", "allow_string_string"));
  EXPECT_FALSE(engine.ModuleExportsHelper("rules", "no_such_export"));
}

TEST(M13Probe4Engine, AddModuleDuplicateAliasErrors) {
  ProbeEngine engine;
  const std::string rules_bytes = ReadWorkspaceFile(
      "compiler_v2/probes/m13_custom_fns/rules/rules.wasm");
  const absl::Span<const uint8_t> bytes(
      reinterpret_cast<const uint8_t*>(rules_bytes.data()), rules_bytes.size());

  ASSERT_TRUE(engine.AddModule("rules", bytes).ok());
  auto second = engine.AddModule("rules", bytes);
  EXPECT_FALSE(second.ok());
  EXPECT_EQ(second.code(), absl::StatusCode::kAlreadyExists);
  EXPECT_THAT(std::string(second.message()), HasSubstr("rules"));
}

TEST(M13Probe4Engine, CrossModuleOverloadConflictDetectableViaExports) {
  // Register the same TinyGo `rules.wasm` under TWO aliases (`rules`
  // and `policy`).  Both modules export `allow_string_string` — that's
  // a conflict at the CEL-level (cel-cpp can't tell which `allow` you
  // mean).  Engine should detect it by walking exports.
  ProbeEngine engine;
  const std::string rules_bytes = ReadWorkspaceFile(
      "compiler_v2/probes/m13_custom_fns/rules/rules.wasm");
  const absl::Span<const uint8_t> bytes(
      reinterpret_cast<const uint8_t*>(rules_bytes.data()), rules_bytes.size());

  ASSERT_TRUE(engine.AddModule("rules", bytes).ok());
  ASSERT_TRUE(engine.AddModule("policy", bytes).ok());

  auto conflicts = engine.CrossModuleOverloadConflicts();
  EXPECT_THAT(conflicts, ::testing::Contains("allow_string_string"))
      << "engine should detect that both `rules` and `policy` export the "
         "same helper";
}

TEST(M13Probe4Engine, PlanResolvesImportsAgainstRegisteredModules) {
  ProbeEngine engine;
  const std::string rules_bytes = ReadWorkspaceFile(
      "compiler_v2/probes/m13_custom_fns/rules/rules.wasm");
  ASSERT_TRUE(engine
                  .AddModule("rules",
                             absl::MakeConstSpan(
                                 reinterpret_cast<const uint8_t*>(
                                     rules_bytes.data()),
                                 rules_bytes.size()))
                  .ok());

  const std::string caller_wat = ReadWorkspaceFile(
      "doc/implementation-plan/rewrite/wat/m13_p1_caller.wat");
  ASSERT_FALSE(caller_wat.empty());

  auto planned_or = engine.Plan(caller_wat);
  ASSERT_TRUE(planned_or.ok()) << planned_or.status();

  // Call eval and verify result — same shape as Probe 2.
  wasmtime_extern_t eval_ext;
  ASSERT_TRUE(wasmtime_instance_export_get(
      engine.ctx(), &planned_or->instance, "eval", 4, &eval_ext));
  wasmtime_val_t result{};
  wasmtime_func_t eval_fn = eval_ext.of.func;
  wasm_trap_t* trap = nullptr;
  auto err = wasmtime_func_call(engine.ctx(), &eval_fn, nullptr, 0, &result, 1,
                                &trap);
  ASSERT_EQ(err, nullptr);
  ASSERT_EQ(trap, nullptr);
  ASSERT_EQ(result.kind, WASMTIME_I32);

  const uint32_t out_offset = static_cast<uint32_t>(result.of.i32);
  const uint8_t* mem =
      wasmtime_memory_data(engine.ctx(), &planned_or->memory);
  CelValue out{};
  std::memcpy(&out, mem + out_offset, sizeof(out));
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(out.payload.b, 1);
}

TEST(M13Probe4Engine, PlanFailsWhenForeignAliasNotRegistered) {
  ProbeEngine engine;
  // Register a non-rules module so memory resolves, then try to Plan
  // a caller that references the (still-unregistered) `rules` alias.
  // Plan should fail with a message citing the unmet import.
  const std::string rules_bytes = ReadWorkspaceFile(
      "compiler_v2/probes/m13_custom_fns/rules/rules.wasm");
  ASSERT_TRUE(engine
                  .AddModule("other",
                             absl::MakeConstSpan(
                                 reinterpret_cast<const uint8_t*>(
                                     rules_bytes.data()),
                                 rules_bytes.size()))
                  .ok());

  const std::string caller_wat = ReadWorkspaceFile(
      "doc/implementation-plan/rewrite/wat/m13_p1_caller.wat");
  auto planned_or = engine.Plan(caller_wat);
  ASSERT_FALSE(planned_or.ok());
  EXPECT_EQ(planned_or.status().code(),
            absl::StatusCode::kFailedPrecondition);
  EXPECT_THAT(std::string(planned_or.status().message()),
              HasSubstr("rules"));
}

TEST(M13Probe4Engine, PlanFailsWhenModuleMissingExpectedExport) {
  // Register a module under `rules` that doesn't export the helper
  // the caller WAT expects.  The probe builds a tiny stub WAT inline
  // that exports a *different* helper.
  const std::string mismatch_wat = R"(
    (module
      (memory (export "memory") 2)
      (func (export "some_other_helper") (param i32 i32 i32))))";

  // Convert it to wasm bytes via wat2wasm.
  wasm_byte_vec_t out_bytes;
  ASSERT_EQ(wasmtime_wat2wasm(mismatch_wat.data(), mismatch_wat.size(),
                              &out_bytes),
            nullptr);
  std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t*>(out_bytes.data),
                             reinterpret_cast<const uint8_t*>(out_bytes.data) +
                                 out_bytes.size);
  wasm_byte_vec_delete(&out_bytes);

  ProbeEngine engine;
  ASSERT_TRUE(engine.AddModule("rules", bytes).ok());

  const std::string caller_wat = ReadWorkspaceFile(
      "doc/implementation-plan/rewrite/wat/m13_p1_caller.wat");
  auto planned_or = engine.Plan(caller_wat);
  ASSERT_FALSE(planned_or.ok());
  EXPECT_THAT(std::string(planned_or.status().message()),
              HasSubstr("allow_string_string"));
}

}  // namespace
}  // namespace celwasm
