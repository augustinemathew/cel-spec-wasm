#ifndef CELWASM_COMPILER_CODEGEN_OVERLOAD_TABLE_H_
#define CELWASM_COMPILER_CODEGEN_OVERLOAD_TABLE_H_

// Maps CEL overload ids (e.g. `kAddInt`, `kSizeString`) to the wasm
// import that implements them.  Built-ins come from a frozen
// `kBuiltinSeeds` array; custom host functions are registered by the
// embedder at compile time via `OverloadTableBuilder::RegisterCustom`.
// Both funnel into the same immutable `OverloadTable`.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"

namespace celwasm {

// Wasm import module a helper comes from.  The expr module imports
// from several modules today:
//
//   kCelRuntime — "cel" — runtime .wasm exports (cel_int_add_at_vv, …).
//   kCelHost   — "cel_host" — built-in host trampolines (cel_get_field, …).
//   kCelFn     — "cel_fn"   — host-backed custom fns.
//   kUserModule — <impl.module_name> — foreign-wasm-backed custom fns
//                                       or CEL-defined backend exports.
//
// Built-in seeds use only kCelRuntime and kCelHost; kCelFn and
// kUserModule carry custom functions.  See `m13-custom-fns.md` §5.3
// for the design context.
enum class ImportModule : uint8_t {
  kCelRuntime = 0,
  kCelHost = 1,
  kCelFn = 2,
  kUserModule = 3,
};

// Returns the fixed wasm-import-module string for the three closed-set
// variants (`"cel"`, `"cel_host"`, `"cel_fn"`).  CHECKs on
// `kUserModule` — that variant's name is per-`OverloadImpl`, not
// per-kind, so use the overload below.
absl::string_view ImportModuleName(ImportModule m);

struct OverloadImpl {
  ImportModule module = ImportModule::kCelRuntime;
  // Wasm import name within `module`.  Built-ins and customs are
  // symmetric: each row names one specific host-visible function.
  //   Built-in: "cel_int_add_at_vv" (kCelRuntime).
  //   Custom:   "my_upper_string"   (kCelHost / kCelFn / kUserModule).
  // For built-ins this view points at a `constexpr` string in
  // `kBuiltinSeeds`; for customs it points into the frozen table's
  // owned storage (std::deque<std::string>, stable under move).
  absl::string_view name;
  // Number of i32 wasm function parameters this helper takes (one
  // out_slot + N arg slots, so a unary helper has num_args=2).
  // Populated for every built-in seed at Build() time from the ABI
  // catalogue (`abi/runtime_catalogue.h`), and for customs at
  // `RegisterCustom` time.  Always >= 1 (out_slot is mandatory): the
  // catalogue lookup CHECK-fails for a missing built-in, and
  // RegisterCustom requires num_args >= 1.
  uint8_t num_args = 0;
  // Only populated when `module == kUserModule`.  The wasm import
  // module string — declared alias from `<alias>.<fnname>(...)`.
  // For customs this view points into the frozen table's owned
  // storage; empty otherwise.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  absl::string_view module_name = {};
};

// Returns the wasm-import-module string for a specific row — handles
// `kUserModule` by returning `impl.module_name`.  Use this at every
// site that emits wasm imports; the bare-enum overload above is only
// for log messages / fixed-variant checks.
absl::string_view ImportModuleName(const OverloadImpl& impl);

// `InferHelperArity` was removed 2026-05-22.  Helper arities now
// come from the ABI catalogue in `abi/runtime_catalogue.h`
// — the single source of truth across codegen, the engine's
// runtime-export allowlist, and the wasm linker's `--export=` set.
// Callers that need arity for a built-in helper name use
// `abi::FindBuiltinHelper(module, name)->num_args`; callers that
// need it for a custom-fn helper read `OverloadImpl::num_args`
// (populated by `RegisterCustom`).

struct Seed {
  // NOLINTNEXTLINE(readability-redundant-member-init) — see OverloadImpl::name.
  absl::string_view overload_id = {};
  OverloadImpl impl = {};
};

class OverloadTable;

class OverloadTableBuilder {
 public:
  // Seeds every row in `kBuiltinSeeds`.
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
  // `module` selects the wasm import namespace:
  //   kCelHost     — legacy host trampoline.
  //   kCelFn       — host-backed customs.
  //   kUserModule  — foreign-wasm-backed or CEL-defined backend.
  // kCelRuntime is reserved for built-in seeds and CHECKs here.
  //
  // `module_name` is required when `module == kUserModule` (the
  // alias from `<alias>.<fnname>(...)` in the .celfn IDL).  For
  // kCelHost / kCelFn it MUST be empty — the import-module name is
  // fixed by the kind.
  //
  // `helper_name` is the wasm import name the expr module will
  // reference.  By convention it matches `overload_id`, but the
  // embedder may pick any unique name — only `helper_name` is
  // visible in the emitted wasm.
  //
  // `num_args` is the wasm function arity: 1 (out_slot) plus the
  // number of CEL arguments.  E.g. a `bool isAdmin(User)` custom is
  // num_args=2; `bool allow(User, string)` is num_args=3.  Must be
  // ≥ 1 (out_slot is required); CHECKs otherwise.
  //
  // Returns `AlreadyExists` if `overload_id` collides with either a
  // built-in (CEL spec forbids shadowing) or a prior custom
  // registration.  Caller-owned string_views are copied into stable
  // storage, so they need not outlive the call.
  ABSL_MUST_USE_RESULT absl::Status RegisterCustom(
      absl::string_view overload_id, ImportModule module,
      absl::string_view module_name, absl::string_view helper_name,
      uint8_t num_args);

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
  std::deque<std::string> custom_module_names_ =
      {};  // owns kUserModule alias strings (e.g. "rules")
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

  // Enumerate the `OverloadImpl` rows reached by the compiled
  // expression.  Codegen tracks the set of interned ids it emitted
  // and hands them back here.  Unknown ids are silently skipped.
  // Returned pointers reference the table's internal storage and
  // are valid for the lifetime of the table.
  //
  // Each row carries enough information to emit a wasm import:
  // `ImportModuleName(*p)` resolves the wasm import-module string
  // (handles `kUserModule` correctly via `module_name`), and
  // `p->num_args` gives the import's arity.
  std::vector<const OverloadImpl*> UsedImports(
      const absl::flat_hash_set<uint32_t>& used_ids) const;

  size_t size() const {
    return impls_.size();
  }

 private:
  friend class OverloadTableBuilder;
  OverloadTable(std::deque<std::string> custom_ids,
                std::deque<std::string> custom_helper_names,
                std::deque<std::string> custom_module_names,
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
  std::deque<std::string> custom_module_names_ = {};
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

#endif  // CELWASM_COMPILER_CODEGEN_OVERLOAD_TABLE_H_
