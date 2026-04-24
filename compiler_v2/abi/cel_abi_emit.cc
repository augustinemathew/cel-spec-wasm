#include "compiler_v2/abi/cel_abi_emit.h"

#include <cstdint>

#include "absl/status/statusor.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/codegen/layout_pass.h"

namespace celwasm {

namespace {

// Current wire version.  Bumped on any field removal or semantics
// change.  Additive changes (new fields, new enum values) keep the
// version the same — hosts consuming older versions just ignore the
// unknown fields per proto3 semantics.
constexpr uint32_t kCelAbiVersion = 1;

}  // namespace

absl::StatusOr<celwasm::abi::CelAbi> BuildCelAbi(const StaticLayout& layout) {
  celwasm::abi::CelAbi abi;
  abi.set_version(kCelAbiVersion);

  abi.mutable_variables()->Reserve(static_cast<int>(layout.variables.size()));
  for (const LaidOutVariable& v : layout.variables) {
    celwasm::abi::VariableEntry* entry = abi.add_variables();
    entry->set_name(v.name);
    entry->set_local_index(v.local_index);
    entry->set_slot_offset(v.slot_offset);
    entry->set_repr(static_cast<uint32_t>(v.repr));
  }

  // fields[] + attributes[] stay empty at M2.B — populated by
  // M2.C (kSelect field intern table) and M2.E (kIdent/kSelect
  // attribute intern table) respectively.

  return abi;
}

}  // namespace celwasm
