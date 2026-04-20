// wasmtime glue for the `cel_host.*` host ABI imports.  The pure
// host-side logic lives in `cel_host.{h,cc}` and is runtime-agnostic;
// this file is the thin trampoline layer that unwraps wasmtime's
// externref / i32 / memory types, calls into `ReadField` / `HasField` /
// `MessageEq`, and wires the resulting callbacks onto a
// `wasmtime_linker_t` under the module namespace `"cel_host"`.
//
// Registration order in the embedder:
//
//   1. Compile + instantiate the runtime module (as host_loader.cc already
//      does) — this gives you `wasmtime_instance_t runtime`.
//   2. Construct a `CelHostEnv` against that runtime; it caches the few
//      runtime exports the trampolines need (`cel_alloc`, `cel_mem_base`,
//      `cel_ref_intern`, the exported `memory`).
//   3. Call `env.Register(linker)` *before* `linker.instantiate(eval)`.
//   4. Instantiate the eval module through the linker.  The eval module's
//      `(import "cel_host" "get_field" ...)` etc. resolve against the
//      trampolines.
//
// `CelHostEnv` is non-copyable and non-movable so that the pointer
// passed to `wasmtime_func_new` as callback-data stays valid for the
// life of the store.  Keep one alive per `LoadedEval`.

#ifndef CELWASM_COMPILER_HOST_CEL_HOST_WASMTIME_H_
#define CELWASM_COMPILER_HOST_CEL_HOST_WASMTIME_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "compiler/host/attribute.h"
#include "wasmtime.h"

namespace celwasm {

// Resolved field-intern entry.  `field_number == 0` is the sentinel
// for "not proto-resolvable" (forward-compat for future JSON / map
// backings); the host then falls back to name-based field lookup.
struct FieldTableEntry {
  uint32_t field_number;
  std::string name;
};

// Resolved attribute-intern entry: the full rooted path the
// `cel_host.get_field` call site refers to.  `variable` is empty for
// non-ident-rooted paths — those never FULL-match any pattern and
// the host treats them as "always resolve normally".
struct AttributeTableEntry {
  std::string variable;
  std::vector<std::string> qualifiers;
};

// Holds the runtime-instance handles the `cel_host.*` trampolines need,
// plus the wasmtime store context.  Must outlive the
// `wasmtime_linker_t` it registers on.  Not copyable / movable (the
// address is captured as `void*` callback-data).
class CelHostEnv {
 public:
  CelHostEnv() = default;

  // Looks up `cel_alloc`, `cel_mem_base`, and the `memory` export on
  // `runtime` and stashes them on `*this`.  Fails with NotFound if any
  // required export is missing or the wrong kind.  Borrows `ctx` — the
  // caller is responsible for keeping the store alive for as long as
  // the env lives.
  //
  // `cel_ref_intern` is *not* looked up here: it is an eval-module
  // export (emitted by `AddCelRefsTableAndHelpers`), not a runtime
  // export.  For G2 flat selects nothing calls it; for G4 nested
  // selects the caller runs `BindEvalInterner` after eval
  // instantiation to install it.
  //
  // Must be called before `Register`; calling `Register` on a
  // default-constructed env is a programming error.
  ABSL_MUST_USE_RESULT absl::Status Init(wasmtime_context_t* absl_nonnull ctx,
                                         const wasmtime_instance_t& runtime);

  // Looks up `cel_ref_intern` on `eval` so `get_field` can intern
  // sub-messages into `$cel_refs`.  Only required for eval modules
  // that read message-typed proto fields (G4+).  Eval modules that
  // don't touch messages omit the export; callers may skip this call
  // in that case.
  ABSL_MUST_USE_RESULT absl::Status BindEvalInterner(
      const wasmtime_instance_t& eval);

  CelHostEnv(const CelHostEnv&) = delete;
  CelHostEnv& operator=(const CelHostEnv&) = delete;
  CelHostEnv(CelHostEnv&&) = delete;
  CelHostEnv& operator=(CelHostEnv&&) = delete;

  // Registers three functions on `linker` under module `"cel_host"`:
  //   get_field   (externref, i32, i32, i32) -> ()
  //   has_field   (externref, i32)           -> i32
  //   message_eq  (externref, externref)     -> i32
  // The second `i32` in `get_field` / `has_field` is a field
  // intern-id; the trampoline resolves it against `field_table_`
  // (seeded via `SetFieldTable`) to produce the `(field_number,
  // field_name)` pair passed to the pure host helpers.
  // Must be called before `wasmtime_linker_instantiate(eval_mod)`.
  ABSL_MUST_USE_RESULT absl::Status Register(
      wasmtime_linker_t* absl_nonnull linker);

  // Installs the field intern-id → `(field_number, field_name)`
  // lookup table parsed out of the eval module's `cel.abi` custom
  // section.  Entry `i` maps intern-id `i` to the corresponding
  // resolved field.  Must be called before the first call into a
  // trampoline that consumes intern-ids (`get_field` / `has_field`).
  // Loading an eval module with no selects is fine — the table stays
  // empty.
  void SetFieldTable(std::vector<FieldTableEntry> table);

  // Resolves an intern-id to its field table row.  Returns nullptr
  // when `id` is out of range (treated as CEL_ERROR by the caller).
  // Public so the trampolines (free functions in the .cc) can call it;
  // treat as internal.
  const FieldTableEntry* absl_nullable LookupField(uint32_t intern_id) const;

  // Installs the attribute intern-id → `(variable, qualifiers[])`
  // lookup table parsed out of the eval module's `cel.abi.attributes`
  // section.  Entry `i` maps attr-id `i` to the select call-site's
  // full attribute path.  Loading an eval module with no selects is
  // fine — the table stays empty.
  void SetAttributeTable(std::vector<AttributeTableEntry> table);

  // Resolves an attr-id to its attribute-table row.  Returns nullptr
  // when `id` is out of range.  When no table has been installed
  // (e.g. pre-E2a.1 eval modules without the `attributes` section)
  // every call returns nullptr and the trampoline skips unknown-
  // pattern matching, resolving fields normally.
  const AttributeTableEntry* absl_nullable LookupAttribute(
      uint32_t attr_id) const;

  // Installs the set of attribute patterns the embedder has marked as
  // unknown.  The trampoline checks each `cel_host.get_field` call
  // site's attribute path against every pattern via
  // `AttributePattern::IsMatch`; on a FULL match it writes
  // `CelValue{CEL_UNKNOWN, attr_id}` into the sret slot and skips
  // `ReadField`.  Overrides any previous set.
  void SetUnknownPatterns(std::vector<AttributePattern> patterns);

  // Runs every configured pattern against `attr` and returns true iff
  // any match is FULL.  PARTIAL / NONE results do not short-circuit
  // the field read — a FULL match on a deeper select will trigger the
  // UNKNOWN there.  Public for the trampoline's sake; treat as
  // internal.
  bool AttributeIsFullyUnknown(const Attribute& attr) const;

  // Accessors used by the trampolines.  Public only because the
  // trampolines are free functions in the .cc; treat as internal.
  wasmtime_context_t* absl_nullable ctx() const {
    return ctx_;
  }
  const wasmtime_func_t& cel_alloc() const {
    return cel_alloc_;
  }
  const wasmtime_func_t& cel_mem_base() const {
    return cel_mem_base_;
  }
  const wasmtime_func_t& cel_ref_intern() const {
    return cel_ref_intern_;
  }
  const wasmtime_memory_t& memory() const {
    return memory_;
  }

 private:
  wasmtime_context_t* absl_nullable ctx_ = nullptr;
  wasmtime_func_t cel_alloc_{};
  wasmtime_func_t cel_mem_base_{};
  wasmtime_func_t cel_ref_intern_{};
  wasmtime_memory_t memory_{};
  std::vector<FieldTableEntry> field_table_;
  std::vector<AttributeTableEntry> attribute_table_;
  std::vector<AttributePattern> unknown_patterns_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_HOST_CEL_HOST_WASMTIME_H_
