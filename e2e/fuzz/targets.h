#ifndef CELWASM_E2E_FUZZ_TARGETS_H_
#define CELWASM_E2E_FUZZ_TARGETS_H_

#include <optional>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// A named fuzz target: the CLI string and the CelType the generator
// roots its expression at.  This is the SINGLE source of truth for the
// mineable target set — `mine_divergences` and `dump_samples` both
// derive their `ParseTarget` from it, and `targets_test` pins the list
// so the two CLIs (and the `scripts/fuzz.sh` sweep) cannot drift the
// way they did before this was consolidated.
struct NamedTarget {
  absl::string_view name;
  CelType type;
};

// All mineable targets, in canonical (CLI-documented) order.
absl::Span<const NamedTarget> AllTargets();

// The CelType for `name`, or nullopt if `name` is not a known target.
std::optional<CelType> ParseTarget(absl::string_view name);

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_TARGETS_H_
