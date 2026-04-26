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
  kCelHost = 1,     // "cel_host" — host-provided helpers; each custom
                    //   function registers its own named import here.
};

// "cel" / "cel_host".  Fails loudly on an unknown value — the enum is
// closed, so a missing case is an invariant violation, not a
// legitimate code path.
absl::string_view ImportModuleName(ImportModule m);

struct OverloadImpl {
  ImportModule module = ImportModule::kCelRuntime;
  // Wasm import name within `module`.  Built-ins and customs are
  // symmetric: each row names one specific host-visible function.
  //   Built-in: "cel_int_add_at_vv" (kCelRuntime).
  //   Custom:   "my_upper_string"   (kCelHost).
  // For built-ins this view points at a `constexpr` string in
  // `kBuiltinSeeds`; for customs it points into the frozen table's
  // owned storage (std::deque<std::string>, stable under move).
  // NOLINTNEXTLINE(readability-redundant-member-init) — `= {}` silences
  // cppcoreguidelines-pro-type-member-init on the constructor.
  absl::string_view name = {};
};

struct Seed {
  // NOLINTNEXTLINE(readability-redundant-member-init) — see OverloadImpl::name.
  absl::string_view overload_id = {};
  OverloadImpl impl = {};
};

class OverloadTable;

class OverloadTableBuilder {
 public:
  // Seeds every row in `kBuiltinSeeds` (empty in M1).
  OverloadTableBuilder();

  // Registers a custom host function.
  //
  // `overload_id` is the string the cel-cpp checker will stamp onto
  // resolved call nodes — the same value passed to
  // `MakeOverloadDecl` when the embedder declares the function via
  // `TypeCheckerBuilder::AddFunction` (e.g.
  // `MakeOverloadDecl("my_upper_string", StringType(), StringType())`
  // for a `my.upper(string) -> string` function — see
  // `third_party/cel-cpp/checker/type_checker_builder.h` and
  // `third_party/cel-cpp/common/decl.h`).  cel-cpp records this on
  // the `FunctionReference::overloads()` entry in the checked AST's
  // reference map; ResolvePass uses it as the lookup key here.
  //
  // `helper_name` is the wasm import name the expr module will
  // reference (one import per registered custom — no shared
  // trampoline; §4.6.1 of the rewrite design).  By convention it
  // matches `overload_id`, but the embedder may pick any unique
  // name — only `helper_name` is visible in the emitted wasm.
  //
  // Returns `AlreadyExists` if `overload_id` collides with either a
  // built-in (CEL spec forbids shadowing) or a prior custom
  // registration.  Caller-owned string_views are copied into stable
  // storage, so they need not outlive the call.
  ABSL_MUST_USE_RESULT absl::Status RegisterCustom(
      absl::string_view overload_id, ImportModule module,
      absl::string_view helper_name);

  OverloadTable Build() &&;

 private:
  // `= {}` silences cppcoreguidelines-pro-type-member-init on the
  // constructor body; readability-redundant-member-init is the
  // contradicting twin (see NOLINTs on each line).
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::deque<std::string> custom_ids_ = {};  // owns custom overload-id strings
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::deque<std::string> custom_helper_names_ =
      {};  // owns custom helper-name strings
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::vector<OverloadImpl> impls_ = {};  // indexed by (interned_id - 1)
  // NOLINTNEXTLINE(readability-redundant-member-init)
  absl::flat_hash_map<absl::string_view, uint32_t> index_ =
      {};  // id → interned_id
  // NOLINTNEXTLINE(readability-redundant-member-init)
  absl::flat_hash_set<absl::string_view> builtin_ids_ =
      {};  // for collision msgs
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

  // `= {}` silences cppcoreguidelines-pro-type-member-init false
  // positives — the constructor uses an init list that does cover
  // every member, but clang-tidy mis-tracks that with mem-init NOLINTs
  // alone, so prefer brace-init at the declarators.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::deque<std::string> custom_ids_ = {};
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::deque<std::string> custom_helper_names_ = {};
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::vector<OverloadImpl> impls_ = {};
  // NOLINTNEXTLINE(readability-redundant-member-init)
  absl::flat_hash_map<absl::string_view, uint32_t> index_ = {};
};

// Returns true iff `overload_id` matches a `StandardOverloadIds`
// entry that the v2 OverloadTable deliberately doesn't seed.  Three
// reasons land an id here: (1) special-cased in `expr_lower.cc`
// (control flow, `_[_]`, polymorphic `equals` / `not_equals`);
// (2) deferred to a later v2 milestone (cross-type numeric
// ladder, timestamp / duration arithmetic, regex `matches`);
// (3) not on the v2 critical path (type conversions, timestamp
// accessors).  See the `kExplicitlyUnimplementedIds` list in
// `overload_table.cc` for the complete set.  The
// `overload_table_test::CoverageTripwire` test asserts every
// cel-cpp `StandardOverloadIds::k*` value is either resolvable
// here via `OverloadTable::Lookup` or in this set.
bool OverloadTableIsExplicitlyUnimplemented(absl::string_view overload_id);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_OVERLOAD_TABLE_H_
