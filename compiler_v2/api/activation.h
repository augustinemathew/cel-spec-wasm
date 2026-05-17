// Activation — per-`Eval` binding set.  Maps variable names (declared
// on the `Compiler` at compile time) to `Value`s supplied by the
// caller.  One Activation per `Instance::Eval` call; safe to reuse
// across back-to-back evals.
//
// M1 scope: direct `Bind(name, value)` + `Find(name)`.  `BindLazy`
// (deferred-evaluation binding) and `OverrideFunction` (per-call
// function-impl override) are signature-final stubs — declared so
// the surface shape doesn't change later, bodies `ABSL_CHECK`
// until the milestone that lights them up.

#ifndef CELWASM_COMPILER_V2_API_ACTIVATION_H_
#define CELWASM_COMPILER_V2_API_ACTIVATION_H_

#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/value.h"

namespace celwasm::api {

// Forward: `FunctionImpl` lives in `api/function.h`.  Declared via
// duplicate `using` so this header doesn't pull function.h into its
// transitive set (tests exercise activation without caring about
// custom fns).
using FunctionImpl =
    absl::AnyInvocable<Value(absl::Span<const Value> args) const>;

class Activation {
 public:
  Activation() = default;

  // ——— Direct binding ———
  // Overwrites any prior binding of `name` in this Activation.
  // Returns *this for fluent construction at Eval sites.
  Activation& Bind(std::string name, Value value);

  // ——— Deferred binding (M2+; stub at M1) ———
  // Binder is invoked at most once per Eval, the first time the
  // expression references `name`.  Result is cached for the rest of
  // that Eval; cleared on Instance::Reset.  Stub body CHECKs.
  Activation& BindLazy(
      std::string name,
      absl::AnyInvocable<absl::StatusOr<Value>() const> binder);

  // ——— Per-call function override (M5+; stub at M1) ———
  // Allows a single Eval to use a different impl for a declared
  // overload_id without rebuilding RuntimeBindings.  Stub body CHECKs.
  Activation& OverrideFunction(std::string overload_id, FunctionImpl impl);

  // Lookup.  Returns nullptr if not bound.  Used internally by
  // `Instance::Eval` to populate root-variable slots; users rarely
  // call this directly.
  const Value* absl_nullable Find(absl::string_view name) const;

 private:
  absl::flat_hash_map<std::string, Value> bindings_;
};

}  // namespace celwasm::api

// Backward-compat aliases — see value.h for the rationale (avoiding
// `cel::Activation` ODR collision with cel-cpp `common/activation.h`).
namespace cel {
using ::celwasm::api::Activation;
using ::celwasm::api::FunctionImpl;
}  // namespace cel

#endif  // CELWASM_COMPILER_V2_API_ACTIVATION_H_
