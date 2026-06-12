#include "e2e/fuzz/targets.h"

#include <optional>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "shared/type.h"

namespace celwasm::fuzz {

absl::Span<const NamedTarget> AllTargets() {
  // Leak-on-exit (the codebase convention, e.g. Grammar::Rules) avoids
  // an exit-time destructor on the CelType members.
  static const std::vector<NamedTarget>* kTargets = [] {
    const CelType i = CelType::Int();
    const CelType s = CelType::String();
    const CelType b = CelType::Bool();
    const CelType d = CelType::Double();
    return new std::vector<NamedTarget>{
        {"bool", b},
        {"int", i},
        {"uint", CelType::Uint()},
        {"double", d},
        {"string", s},
        {"bytes", CelType::Bytes()},
        {"list_int", CelType::List(i)},
        {"list_bool", CelType::List(b)},
        {"list_double", CelType::List(d)},
        {"list_string", CelType::List(s)},
        {"map_string_int", CelType::Map(s, i)},
        {"list_list_int", CelType::List(CelType::List(i))},
        {"map_string_list_int", CelType::Map(s, CelType::List(i))},
    };
  }();
  return *kTargets;
}

std::optional<CelType> ParseTarget(absl::string_view name) {
  for (const NamedTarget& t : AllTargets()) {
    if (t.name == name) return t.type;
  }
  return std::nullopt;
}

}  // namespace celwasm::fuzz
