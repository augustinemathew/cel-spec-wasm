// Activation — per-`Eval` binding set.  Maps variable names (declared
// on the `Compiler` at compile time) to `Value`s supplied by the
// caller.  One Activation per `Instance::Eval` call; safe to reuse
// across back-to-back evals.
//
// Two ways to supply a value: `Bind` hands over a `Value` directly,
// and `BindLazy` registers a callback that produces one on demand.
// `Resolve` is the single lookup entry point and handles both.
//
// Thread-safety: NOT thread-safe.  `Resolve` memoizes lazy values
// through a mutable cache, so an Activation must not be shared across
// concurrent evaluations.  This matches `Instance`, which is
// thread-owned — bind one Activation per worker.

#ifndef CELWASM_EVAL_ACTIVATION_H_
#define CELWASM_EVAL_ACTIVATION_H_

#include <string>

#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "eval/value.h"

namespace celwasm {

class Activation {
 public:
  // Produces a variable's value on demand.  Invoked at most once per
  // evaluation; see `BindLazy`.
  using LazyBinder = absl::AnyInvocable<absl::StatusOr<Value>() const>;

  Activation() = default;

  // Move-only: a lazy binder is move-only, so an Activation cannot be
  // copied once one is registered.  Declared uniformly rather than
  // conditionally so the API shape doesn't depend on what you bound.
  Activation(Activation&&) = default;
  Activation& operator=(Activation&&) = default;
  Activation(const Activation&) = delete;
  Activation& operator=(const Activation&) = delete;

  // ——— Direct binding ———
  // Overwrites any prior binding of `name` — eager or lazy — in this
  // Activation.  Returns *this for fluent construction at Eval sites.
  Activation& Bind(std::string name, Value value);

  // ——— Deferred binding ———
  // Registers a callback that produces `name`'s value on demand,
  // overwriting any prior binding of `name`.
  //
  // `binder` is invoked **at most once per `Eval`**, and only if the
  // compiled program declares `name` and the variable is not blanked
  // by an unknown pattern during a `PartialEval`.  Its result is
  // memoized for the remainder of that evaluation; the next `Eval`
  // invokes it again.  A binder returning a non-OK status aborts the
  // evaluation and that status is propagated verbatim.
  //
  // Note the binder fires on *declaration*, not on first reference:
  // variable slots are marshalled into linear memory before the
  // expression runs, so the runtime never asks for a variable it
  // didn't already receive.  Binding lazily therefore saves the cost
  // of materializing a variable the program never declared — not the
  // cost of one it declares but doesn't reach.
  Activation& BindLazy(std::string name, LazyBinder binder);

  // Lookup.  Returns nullptr when `name` has no binding of either
  // kind, and a non-OK status only when a lazy binder failed.  Used by
  // `Instance::Eval` to populate root-variable slots; callers rarely
  // invoke it directly.
  //
  // The returned pointer is owned by this Activation and is
  // invalidated by `Bind`, `BindLazy`, or `ClearLazyCache`.
  absl::StatusOr<const Value* absl_nullable> Resolve(
      absl::string_view name) const;

  // Drops memoized lazy values so the next evaluation re-invokes its
  // binders.  Called by `Instance` at the start of each evaluation;
  // `const` because evaluation only ever holds a `const Activation&`.
  void ClearLazyCache() const;

 private:
  absl::flat_hash_map<std::string, Value> bindings_;
  absl::flat_hash_map<std::string, LazyBinder> lazy_;
  // Values produced by `lazy_` binders during the current evaluation.
  mutable absl::flat_hash_map<std::string, Value> lazy_cache_;
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_ACTIVATION_H_
