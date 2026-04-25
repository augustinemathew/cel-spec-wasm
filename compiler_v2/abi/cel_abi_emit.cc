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

// Public declaration lives in cel_abi_emit.h; clang-tidy's include
// path for the header is incomplete in compile_commands.json and it
// mistakes this for a static candidate.
// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::StatusOr<celwasm::abi::CelAbi> BuildCelAbi(
    const StaticLayout& layout, absl::Span<const FieldRefRow> field_refs) {
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

  // fields[]: one row per kSelect (M2.C).  Index 0 is the sentinel
  // (zero-initialised FieldRefRow); emit it too so the host-side
  // table's indices line up 1:1 with field_ref_id used in
  // `cel_host.cel_get_field` calls.
  abi.mutable_fields()->Reserve(static_cast<int>(field_refs.size()));
  for (uint32_t i = 0; i < field_refs.size(); ++i) {
    const FieldRefRow& row = field_refs[i];
    celwasm::abi::FieldEntry* entry = abi.add_fields();
    entry->set_id(i);
    entry->set_field_number(row.field_number);
    entry->set_name(row.name);
    entry->set_owner_fqn(row.owner_fqn);
  }

  // attributes[]: one row per distinct attribute path (M2.E).
  // Index 0 is the sentinel (empty row).
  abi.mutable_attributes()->Reserve(static_cast<int>(layout.attributes.size()));
  for (uint32_t i = 0; i < layout.attributes.size(); ++i) {
    const AttributeEntryRow& row = layout.attributes[i];
    celwasm::abi::AttributeEntry* entry = abi.add_attributes();
    entry->set_id(i);
    entry->set_variable(row.root_variable);
    for (const std::string& q : row.qualifiers) {
      entry->add_qualifiers(q);
    }
  }

  return abi;
}

}  // namespace celwasm
