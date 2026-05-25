#include "abi/cel_abi_emit.h"

#include <cstdint>

#include "absl/status/statusor.h"
#include "abi/cel_abi.pb.h"
#include "abi/runtime_catalogue.h"
#include "compiler/codegen/layout_pass.h"

namespace celwasm {

namespace {

// Current wire version.  Bumped on any field removal or semantics
// change.  Additive changes (new fields, new enum values) keep the
// version the same — hosts consuming older versions just ignore the
// unknown fields per proto3 semantics.
constexpr uint32_t kCelAbiVersion = 1;

// Populates `cel.abi.variables[]` from the layout's free-variable
// entries.  Comprehension-scope locals (iter / accu / index) are
// excluded — they're bound by the comprehension's loop prologue
// at comprehension entry, not by `Activation::Bind`.
void EmitVariables(const StaticLayout& layout, celwasm::abi::CelAbi& abi) {
  abi.mutable_variables()->Reserve(static_cast<int>(layout.variables.size()));
  for (const LaidOutVariable& v : layout.variables) {
    if (v.kind != ResolvedVariableKind::kFreeVariable) continue;
    celwasm::abi::VariableEntry* entry = abi.add_variables();
    entry->set_name(v.name);
    entry->set_local_index(v.local_index);
    entry->set_slot_offset(v.slot_offset);
    entry->set_repr(static_cast<uint32_t>(v.repr));
  }
}

}  // namespace

// Public declaration lives in cel_abi_emit.h; clang-tidy's include
// path for the header is incomplete in compile_commands.json and it
// mistakes this for a static candidate.
// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::StatusOr<celwasm::abi::CelAbi> BuildCelAbi(
    const StaticLayout& layout, absl::Span<const FieldRefRow> field_refs) {
  celwasm::abi::CelAbi abi;
  abi.set_version(kCelAbiVersion);
  // Runtime catalogue version the program is being compiled
  // against.  Engine::Plan compares this against its own
  // kRuntimeAbiVersion and rejects mismatches.  See
  // doc/implementation-plan/rewrite/abi-refactor.md §5 (Slice E).
  abi.set_runtime_abi_version(celwasm::abi::kRuntimeAbiVersion);

  EmitVariables(layout, abi);

  // fields[]: one row per kSelect.  Index 0 is the sentinel
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

  // attributes[]: one row per distinct attribute path.
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

  // types[]: one row per distinct kStructExpr message FQN.
  // Index 0 is the sentinel (empty FQN); rows [1..N] are the ids
  // codegen stamps via `NodeAnnotation::message_type_id` and the
  // emitted `cel_make_message` calls reference.
  abi.mutable_types()->Reserve(static_cast<int>(layout.message_types.size()));
  for (uint32_t i = 0; i < layout.message_types.size(); ++i) {
    const MessageTypeRow& row = layout.message_types[i];
    celwasm::abi::TypeEntry* entry = abi.add_types();
    entry->set_id(i);
    entry->set_fully_qualified_name(row.fully_qualified_name);
  }

  return abi;
}

}  // namespace celwasm
