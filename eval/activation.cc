#include "eval/activation.h"

#include <string>
#include <utility>

#include "absl/base/nullability.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "eval/value.h"

namespace celwasm {

Activation& Activation::Bind(std::string name, Value value) {
  // Drop any lazy binding of the same name, and the value it may
  // already have produced, so the two maps never both answer for one
  // name and a stale memo can't shadow the new value.
  lazy_.erase(name);
  lazy_cache_.erase(name);
  bindings_.insert_or_assign(std::move(name), std::move(value));
  return *this;
}

Activation& Activation::BindLazy(std::string name, LazyBinder binder) {
  bindings_.erase(name);
  lazy_cache_.erase(name);
  lazy_.insert_or_assign(std::move(name), std::move(binder));
  return *this;
}

absl::StatusOr<const Value* absl_nullable> Activation::Resolve(
    absl::string_view name) const {
  if (auto it = bindings_.find(name); it != bindings_.end()) {
    return &it->second;
  }
  if (auto it = lazy_cache_.find(name); it != lazy_cache_.end()) {
    return &it->second;
  }
  auto binder = lazy_.find(name);
  if (binder == lazy_.end()) return nullptr;
  // A failing binder is not memoized: the caller aborts the evaluation
  // on the status, and a retry should get a fresh attempt rather than
  // a cached failure.
  auto produced = binder->second();
  if (!produced.ok()) return produced.status();
  auto [it, inserted] =
      lazy_cache_.insert_or_assign(std::string(name), *std::move(produced));
  return &it->second;
}

void Activation::ClearLazyCache() const {
  lazy_cache_.clear();
}

}  // namespace celwasm
