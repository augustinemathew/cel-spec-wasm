#ifndef CELWASM_COMPILER_CODEGEN_OVERLOAD_TABLE_H_
#define CELWASM_COMPILER_CODEGEN_OVERLOAD_TABLE_H_

// Maps a cel-cpp overload id (e.g. "add_int64") to the wasm import that
// implements it.  Built-in operators come from a frozen seed table that
// pairs each overload id with a `cel_runtime.wasm` helper name; the
// helper's import module ("cel") and arity are taken from the runtime
// catalogue (`abi/runtime_catalogue.h`) — the single source of truth for
// built-in functions.  Embedder-registered custom functions are layered
// on top.  The result is an immutable `OverloadTable` with two queries:
//
//   - `Lookup(overload_id)` — resolve to the def codegen calls.
//   - `impls()`             — enumerate every def, for import install.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

// Which wasm import module an overload's function lives under.  The first
// three resolve to fixed module strings; `kUser` uses the per-overload
// alias in `OverloadDef::wasm_import_module_name`.
enum class ImportModuleSource : uint8_t {
  kCel = 0,      // "cel"      — cel_runtime.wasm exports (built-in seeds)
  kCelHost = 1,  // "cel_host" — host trampolines
  kCelFn = 2,    // "cel_fn"   — host-backed custom functions
  kUser = 3,     // uses OverloadDef::wasm_import_module_name
};

// One overload: a CEL overload id paired with the wasm import that
// implements it.  Built-in standard overloads and embedder-declared
// custom functions all flow through this one type, in two distinct
// shapes — every field of each shown below.
//
//   1) built-in — a CEL spec standard overload, e.g. `1 + 2`.  Build()
//      fills the function name + num_args from the runtime catalogue.
//        overload_id               = "add_int64"   (the CEL spec id)
//        wasm_import_function_name = "cel_int_add_at_vv"
//        wasm_import_module_type   = kCel
//        wasm_import_module_name   = ""             (module name → "cel")
//        num_args                  = 3              (out_slot + 2 args)
//
//   2) @host custom — a trusted C++ lambda the embedder binds at Plan
//      time, e.g. `myorg.up(s)`.
//        overload_id               = "up_str"
//        wasm_import_function_name = "up_str"
//        wasm_import_module_type   = kCelFn
//        wasm_import_module_name   = ""             (module name → "cel_fn")
//        num_args                  = 2              (out_slot + 1 arg)
struct OverloadDef {
  // The table's lookup key, and a CEL spec concept rather than a local
  // one: it is the spec's canonical name for one specific typed overload
  // of a function (e.g. "add_int64" for `int + int`, "size_string" for
  // `size(string)`), defined by the CEL standard environment.  It is the
  // exact same string the type-checker writes onto the call's AST node —
  // the checked AST's `cel.expr.Reference.overload_id`.  Used verbatim.
  std::string overload_id;
  std::string wasm_import_function_name;  // the imported wasm function
  ImportModuleSource wasm_import_module_type = ImportModuleSource::kCel;
  // The wasm import module string when `wasm_import_module_type == kUser`;
  // empty otherwise (the name is then derived from the type).
  std::string wasm_import_module_name;
  uint8_t num_args = 0;  // i32 param count (out_slot + args); always >= 1
};

// The wasm import-module string for `def`: the fixed name for
// kCel/kCelHost/kCelFn, or `def.wasm_import_module_name` for kUser.
absl::string_view ImportModuleName(const OverloadDef& def);

class OverloadTable {
 public:
  OverloadTable(OverloadTable&&) = default;
  OverloadTable& operator=(OverloadTable&&) = default;
  OverloadTable(const OverloadTable&) = delete;
  OverloadTable& operator=(const OverloadTable&) = delete;

  // Builds the immutable table: the built-in seed set (source kCel, arity
  // from the runtime catalogue) plus `customs`, in that order.  Returns
  // `AlreadyExists` if a custom's `overload_id` collides with a built-in
  // (CEL forbids shadowing) or an earlier custom.  CHECK-fails if a
  // seed's helper is missing from the catalogue, a custom has
  // `num_args == 0` or `wasm_import_module_type == kCel`, or a custom's
  // `wasm_import_module_name` is set inconsistently with the kUser type.
  static absl::StatusOr<OverloadTable> Build(
      absl::Span<const OverloadDef> customs = {});

  // Resolve a cel-cpp overload id to its def, or nullptr — codegen
  // treats nullptr as Unimplemented and aborts the compile with the id
  // in the error message.  Valid for the lifetime of the table.
  const OverloadDef* Lookup(absl::string_view overload_id) const;

  // Every def, built-ins first then customs in registration order — the
  // set of wasm imports an emitted module installs.  Valid for the
  // table's lifetime.
  absl::Span<const OverloadDef> impls() const {
    return impls_;
  }

 private:
  OverloadTable() = default;

  std::vector<OverloadDef> impls_;
  // overload_id -> index into `impls_`.  Keys are owned copies, so the
  // map is self-contained and `impls_` can reallocate during Build.
  absl::flat_hash_map<std::string, size_t> index_;
};

// True iff `overload_id` is a cel-cpp `StandardOverloadIds` entry the
// built-in seed set deliberately omits.  Two reasons land an id here:
// (1) special-cased in `expr_lower.cc` (control flow `_?_:_`, indexing
// `_[_]`, the comprehension-internal `not_strictly_false`); (2) the
// dyn-passthrough `to_dyn`.  See `kExplicitlyUnimplementedIds` in
// `overload_table.cc`.  The `overload_table_test::CoverageTripwire` test
// asserts every cel-cpp `StandardOverloadIds::k*` value is either
// resolvable via `Lookup` or in this set.
bool OverloadTableIsExplicitlyUnimplemented(absl::string_view overload_id);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_OVERLOAD_TABLE_H_
