#ifndef CELWASM_COMPILER_V2_CODEGEN_OVERLOAD_TABLE_H_
#define CELWASM_COMPILER_V2_CODEGEN_OVERLOAD_TABLE_H_

// Maps CEL overload ids (e.g. `kAddInt`, `kSizeString`) to the wasm
// import that implements them.  Built-ins come from a frozen
// `kBuiltinSeeds` array; custom host functions are registered by the
// embedder at compile time via `OverloadTableBuilder::RegisterCustom`.
// Both funnel into the same immutable `OverloadTable`.
//
// M1 ships the shape (builder + frozen table + collision rule) with
// `kBuiltinSeeds` empty — M3 fills the seeds.  The table exists now so
// that M3's "turn on the overload set" lands as a data-only change.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"

namespace celwasm {

// Wasm import module a helper comes from.  The expr module imports
// from three modules today (cel, cel_host, cel_env); only the first
// two are overload targets — cel_env is logging-only.
enum class ImportModule : uint8_t {
  kCelRuntime = 0,  // "cel" — runtime .wasm exports (cel_int_add_at_vv, …).
  kCelHost = 1,     // "cel_host" — host trampolines (cel_host_call_custom).
};

// "cel" / "cel_host".  Fails loudly on an unknown value — the enum is
// closed, so a missing case is an invariant violation, not a
// legitimate code path.
absl::string_view ImportModuleName(ImportModule m);

struct OverloadImpl {
  ImportModule module = ImportModule::kCelRuntime;
  // Wasm import name within `module`.
  //   Built-in: "cel_int_add_at_vv" (kCelRuntime).
  //   Custom:   "cel_host_call_custom" (kCelHost).
  // For built-ins this view points at a `constexpr` string in
  // `kBuiltinSeeds`; for customs it points into the frozen table's
  // owned storage (std::deque<std::string>, stable under move).
  absl::string_view name;
  // 0 for built-ins; non-zero for customs — prepended as the first
  // call arg by codegen.
  uint32_t pattern_id = 0;
};

struct Seed {
  absl::string_view overload_id;
  OverloadImpl impl;
};

class OverloadTable;

class OverloadTableBuilder {
 public:
  // Seeds every row in `kBuiltinSeeds` (empty in M1).
  OverloadTableBuilder();

  // Registers a custom host function.  `pattern_id` must be non-zero.
  // Returns `AlreadyExists` if `overload_id` collides with either a
  // built-in (CEL spec forbids shadowing) or a prior custom
  // registration.  Caller-owned string_views are copied into stable
  // storage, so they need not outlive the call.
  ABSL_MUST_USE_RESULT absl::Status RegisterCustom(
      absl::string_view overload_id, ImportModule module,
      absl::string_view helper_name, uint32_t pattern_id);

  OverloadTable Build() &&;

 private:
  std::deque<std::string> custom_ids_;  // owns custom overload-id strings
  std::deque<std::string>
      custom_helper_names_;          // owns custom helper-name strings
  std::vector<OverloadImpl> impls_;  // indexed by (interned_id - 1)
  absl::flat_hash_map<absl::string_view, uint32_t> index_;  // id → interned_id
  absl::flat_hash_set<absl::string_view> builtin_ids_;  // for collision msgs
};

class OverloadTable {
 public:
  OverloadTable(OverloadTable&&) = default;
  OverloadTable& operator=(OverloadTable&&) = default;
  OverloadTable(const OverloadTable&) = delete;
  OverloadTable& operator=(const OverloadTable&) = delete;

  // Returns nullptr if the overload isn't registered — codegen treats
  // this as Unimplemented and aborts the compile with the id in the
  // error message.
  const OverloadImpl* Lookup(absl::string_view overload_id) const;

  // Dense 1-based id for fitting into NodeAnnotation.overload_id.  0
  // is reserved for "unresolved".  Built-ins come first in
  // `kBuiltinSeeds` order; customs follow in registration order.
  // Returns 0 if `overload_id` is not registered.
  uint32_t InternOverloadId(absl::string_view overload_id) const;

  // Reverse of InternOverloadId — called only with ids the builder
  // itself assigned.  CHECKs on out-of-range.
  const OverloadImpl& LookupById(uint32_t interned_id) const;

  // Enumerate (module, helper_name) pairs reached by the compiled
  // expression.  Codegen tracks the set of interned ids it emitted
  // and hands them back here.  Unknown ids are silently skipped.
  std::vector<std::pair<ImportModule, absl::string_view>> UsedImports(
      const absl::flat_hash_set<uint32_t>& used_ids) const;

  size_t size() const {
    return impls_.size();
  }

 private:
  friend class OverloadTableBuilder;
  OverloadTable(std::deque<std::string> custom_ids,
                std::deque<std::string> custom_helper_names,
                std::vector<OverloadImpl> impls,
                absl::flat_hash_map<absl::string_view, uint32_t> index);

  std::deque<std::string> custom_ids_;
  std::deque<std::string> custom_helper_names_;
  std::vector<OverloadImpl> impls_;
  absl::flat_hash_map<absl::string_view, uint32_t> index_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_OVERLOAD_TABLE_H_
