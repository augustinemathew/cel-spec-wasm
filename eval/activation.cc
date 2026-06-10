#include "eval/activation.h"

#include <string>
#include <utility>

#include "absl/base/nullability.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "eval/value.h"

namespace celwasm {

Activation& Activation::Bind(std::string name, Value value) {
  bindings_.insert_or_assign(std::move(name), std::move(value));
  return *this;
}

// Unimplemented surfaces — signature matches cel-host-surface.md
// §2.6; move demonstrates the sink semantics + satisfies the
// unused-value-param lint.  No milestone currently owns either body
// (cleanup-backlog #44); the CHECK keeps any caller loud instead of
// silently misbinding.
Activation& Activation::BindLazy(
    std::string name,
    absl::AnyInvocable<absl::StatusOr<Value>() const> binder) {
  [[maybe_unused]] auto name_sink = std::move(name);
  [[maybe_unused]] auto binder_sink = std::move(binder);
  ABSL_CHECK(false) << "Activation::BindLazy is unimplemented (lazy variable "
                       "binding; cleanup-backlog #44)";
}

Activation& Activation::OverrideFunction(std::string overload_id,
                                         FunctionImpl impl) {
  [[maybe_unused]] auto id_sink = std::move(overload_id);
  [[maybe_unused]] auto impl_sink = std::move(impl);
  ABSL_CHECK(false) << "Activation::OverrideFunction is unimplemented "
                       "(per-eval function override; cleanup-backlog #44)";
}

const Value* absl_nullable Activation::Find(absl::string_view name) const {
  auto it = bindings_.find(name);
  if (it == bindings_.end()) return nullptr;
  return &it->second;
}

}  // namespace celwasm
