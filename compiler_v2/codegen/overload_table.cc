#include "compiler_v2/codegen/overload_table.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace celwasm {

absl::string_view ImportModuleName(ImportModule m) {
  switch (m) {
    case ImportModule::kCelRuntime:
      return "cel";
    case ImportModule::kCelHost:
      return "cel_host";
  }
  ABSL_CHECK(false) << "unknown ImportModule=" << static_cast<int>(m);
  return {};
}

namespace {

// M1 ships with no built-in overloads — M3 fills this.  Using
// `std::array` so a zero-length declaration is legal C++.  Every row
// added here must name `ImportModule::kCelRuntime` explicitly; the
// `kCelHost` module only appears via `RegisterCustom`.
constexpr std::array<Seed, 0> kBuiltinSeeds{};

}  // namespace

OverloadTableBuilder::OverloadTableBuilder() {
  for (const Seed& s : kBuiltinSeeds) {
    // Built-ins use `constexpr` string_views — they point at stable
    // module-lifetime storage, so no copy is needed here.
    const uint32_t interned_id = static_cast<uint32_t>(impls_.size()) + 1u;
    impls_.push_back(s.impl);
    auto [it, inserted] = index_.emplace(s.overload_id, interned_id);
    ABSL_CHECK(inserted) << "kBuiltinSeeds duplicate: " << s.overload_id;
    builtin_ids_.insert(s.overload_id);
  }
}

absl::Status OverloadTableBuilder::RegisterCustom(
    absl::string_view overload_id, ImportModule module,
    absl::string_view helper_name) {
  if (builtin_ids_.contains(overload_id)) {
    return absl::AlreadyExistsError(absl::StrCat(
        "'", overload_id, "' is a standard built-in and cannot be overridden"));
  }
  if (index_.contains(overload_id)) {
    return absl::AlreadyExistsError(
        absl::StrCat("overload '", overload_id, "' already registered"));
  }

  // Commit: push stable storage, then the parallel arrays.
  const std::string& stored_id = custom_ids_.emplace_back(overload_id);
  const std::string& stored_name =
      custom_helper_names_.emplace_back(helper_name);
  const OverloadImpl impl{module, absl::string_view(stored_name)};
  const uint32_t interned_id = static_cast<uint32_t>(impls_.size()) + 1u;
  impls_.push_back(impl);
  index_.emplace(absl::string_view(stored_id), interned_id);
  return absl::OkStatus();
}

OverloadTable OverloadTableBuilder::Build() && {
  return {std::move(custom_ids_), std::move(custom_helper_names_),
          std::move(impls_), std::move(index_)};
}

OverloadTable::OverloadTable(
    std::deque<std::string> custom_ids,
    std::deque<std::string> custom_helper_names,
    std::vector<OverloadImpl> impls,
    absl::flat_hash_map<absl::string_view, uint32_t> index)
    : custom_ids_(std::move(custom_ids)),
      custom_helper_names_(std::move(custom_helper_names)),
      impls_(std::move(impls)),
      index_(std::move(index)) {}

const OverloadImpl* OverloadTable::Lookup(absl::string_view overload_id) const {
  auto it = index_.find(overload_id);
  if (it == index_.end()) return nullptr;
  return &impls_[it->second - 1];
}

uint32_t OverloadTable::InternOverloadId(absl::string_view overload_id) const {
  auto it = index_.find(overload_id);
  if (it == index_.end()) return 0;
  return it->second;
}

const OverloadImpl& OverloadTable::LookupById(uint32_t interned_id) const {
  ABSL_CHECK_GE(interned_id, 1u);
  ABSL_CHECK_LE(static_cast<size_t>(interned_id), impls_.size());
  return impls_[interned_id - 1];
}

std::vector<std::pair<ImportModule, absl::string_view>>
OverloadTable::UsedImports(
    const absl::flat_hash_set<uint32_t>& used_ids) const {
  std::vector<std::pair<ImportModule, absl::string_view>> out;
  out.reserve(used_ids.size());
  for (const uint32_t id : used_ids) {
    if (id == 0 || id > impls_.size()) continue;
    const OverloadImpl& impl = impls_[id - 1];
    out.emplace_back(impl.module, impl.name);
  }
  return out;
}

}  // namespace celwasm
