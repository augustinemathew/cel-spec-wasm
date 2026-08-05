#include "eval/internal/cel_host_message.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "eval/attribute.h"
#include "eval/internal/cel_host_backing.h"
#include "eval/internal/cel_host_common.h"
#include "eval/internal/cel_host_eq.h"
#include "eval/internal/cel_host_error.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "runtime/cel_data.h"
#include "shared/type.h"

namespace celwasm {

// ══════════════════════════════════════════════════════════════════
// Layer-2 trampoline bodies — `CelGetFieldImpl` / `CelHasFieldImpl`.
//
// Both share the same prelude:
//   1. read msg_cv from `mem` (must precede any out_slot writes so
//      msg_slot == out_slot aliasing works);
//   2. propagate UNKNOWN / ERROR on the input;
//   3. defence: msg_cv.kind != CEL_MESSAGE → kTypeMismatch;
//   4. resolve field_ref_id → (number, name) via
//      `bindings.field_refs`; OOR / sentinel → kFieldNotFound;
//   5. attribute_id != 0 → consult `bindings.unknown_patterns`;
//      a FULL match → CEL_UNKNOWN(attribute_id);
//   6. dereference externref slot → backing pointer;
//      missing → kHostAdapterError.
// They diverge after that: Get calls `ReadField` and marshals the
// returned `celwasm::Value` (scalar inline, span via arena, message
// via Intern); Has calls `HasField` and writes a CEL_BOOL.
//
// Non-OK Status only on infrastructure failure that the wasm side
// can't recover from (memory-out-of-range — handled deeper).  All
// spec-level errors travel inside `out_slot` as CEL_ERROR.
// ══════════════════════════════════════════════════════════════════

const FieldRefEntry* absl_nullable ResolveFieldRef(
    const CelHostBindings& bindings, uint32_t field_ref_id) {
  if (field_ref_id == 0) return nullptr;  // sentinel
  if (field_ref_id >= bindings.field_refs.size()) return nullptr;
  return &bindings.field_refs[field_ref_id];
}

namespace {

// Build an `Attribute` from the AttributeEntry at `attribute_id`.
// AttributeEntry.qualifiers are interned as strings (the CEL paths
// are dotted-only at M2; no array indexing).  Returns nullopt if
// `attribute_id` is the sentinel (0) or OOR.
std::optional<celwasm::Attribute> ResolveAttribute(
    const CelHostBindings& bindings, uint32_t attribute_id) {
  if (attribute_id == 0) return std::nullopt;
  if (attribute_id >= bindings.attributes.size()) return std::nullopt;
  const AttributeEntry& a = bindings.attributes[attribute_id];
  std::vector<celwasm::AttributeQualifier> path;
  path.reserve(a.qualifiers.size());
  for (const std::string& q : a.qualifiers) {
    path.push_back(celwasm::AttributeQualifier::OfString(q));
  }
  return celwasm::Attribute(a.root_variable, std::move(path));
}

// Build the effective attribute for the kSelect being evaluated:
// the operand's attribute (resolved from `attribute_id`) extended
// by one qualifier — the field name being selected.  This is the
// path the kSelect would write into `cel.abi.attributes[]` if every
// node interned its own; M2 only interns the operand and lets the
// trampoline append the leaf qualifier here so the pattern matcher
// sees the full path the user wrote (`c.name`, not just `c`).
celwasm::Attribute EffectiveSelectAttribute(
    const celwasm::Attribute& operand_attr, absl::string_view field_name) {
  std::vector<celwasm::AttributeQualifier> path(
      operand_attr.qualifier_path().begin(),
      operand_attr.qualifier_path().end());
  path.push_back(
      celwasm::AttributeQualifier::OfString(std::string(field_name)));
  return {std::string(operand_attr.variable_name()), std::move(path)};
}

// Returns true iff any pattern in `unknown_patterns` `kFull`-matches
// the *effective* attribute (operand ⊕ field name).  `kPartial`
// means a sub-attribute is unknown but THIS one isn't; we fall
// through and read.  `kFull` means the pattern covers this
// attribute, so it's opaque to PartialEval and we surface UNKNOWN.
bool MatchesAnyUnknownPattern(const CelHostBindings& bindings,
                              uint32_t attribute_id,
                              absl::string_view field_name) {
  if (attribute_id == 0) return false;
  if (bindings.unknown_patterns.empty()) return false;
  auto attr = ResolveAttribute(bindings, attribute_id);
  if (!attr.has_value()) return false;
  const celwasm::Attribute eff = EffectiveSelectAttribute(*attr, field_name);
  return std::any_of(
      bindings.unknown_patterns.begin(), bindings.unknown_patterns.end(),
      [&eff](const celwasm::AttributePattern& pat) {
        return pat.IsMatch(eff) == celwasm::AttributePattern::MatchType::kFull;
      });
}

// Shared prelude for Get / Has.  Returns:
//   - non-OK Status only on infrastructure failure (none today —
//     reads are bounds-checked deeper).
//   - kSentinelHandled = true  → `out_slot` already populated with
//     UNKNOWN / ERROR / wire-error CelValue.  Caller returns OK.
//   - kSentinelHandled = false → resolved a backing + field;
//     populates `*out_backing` + `*out_field` and returns.
struct FieldDispatchPrelude {
  bool sentinel_handled = false;
  const HostMessageBacking* absl_nullable backing = nullptr;
  const FieldRefEntry* absl_nullable field = nullptr;
};

absl::StatusOr<FieldDispatchPrelude> RunFieldPrelude(
    uint32_t out_slot, uint32_t msg_slot, uint32_t field_ref_id,
    uint32_t attribute_id, const TrampolineContext& ctx) {
  CelValue msg_cv = ctx.mem.ReadCelValue(msg_slot);

  // 3VL absorption — same path Get and Has share.
  if (msg_cv.kind == CEL_UNKNOWN || msg_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, msg_cv);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }
  if (msg_cv.kind != CEL_MESSAGE) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }

  const FieldRefEntry* field = ResolveFieldRef(ctx.bindings, field_ref_id);
  if (field == nullptr) {
    WriteWireError(CEL_ERR_FIELD_NOT_FOUND, out_slot, ctx.mem);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }

  if (MatchesAnyUnknownPattern(ctx.bindings, attribute_id, field->field_name)) {
    // Mint a 1-element UnknownSet descriptor carrying the matched
    // attribute id (never the raw id — `payload.unk` is a descriptor
    // offset; see EncodeUnknownSet).
    CelValue unk{};
    const uint32_t ids[] = {attribute_id};
    if (auto s = EncodeUnknownSet(ids, ctx.alloc, &unk); !s.ok()) return s;
    ctx.mem.WriteCelValue(out_slot, unk);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }

  const HostMessageBacking* backing = ctx.refs.Lookup(msg_cv.payload.msg_slot);
  if (backing == nullptr) {
    WriteWireError(CEL_ERR_HOST_ADAPTER_ERROR, out_slot, ctx.mem);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }

  return FieldDispatchPrelude{/*sentinel_handled=*/false, backing, field};
}

// Fast-path encoder for aggregate-shaped proto field reads (plain
// nested message / map field / repeated field).  Skips the
// intermediate `celwasm::Value` and its per-read
// `make_shared<Proto{Backing,Map,List}>` by interning through the
// proto-identity ExternrefTable entry points, which may dedup — a
// repeat hop over the same sub-object within an Eval reuses the
// already-issued slot with no allocation.  Returns nullopt when the
// classification is not one of the three aggregate classes (or the
// message lacks reflection); the caller falls through to
// `ReadFieldClassified`, whose kMessagePlain / kMap / kRepeated arms
// remain the semantics of record for non-trampoline callers.
std::optional<absl::Status> TryEncodeAggregateFieldFast(
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const ResolvedFieldCache& c,
    uint32_t out_slot, const TrampolineContext& ctx) {
  using RC = ProtoFieldReadClass;
  CelValue cv{};
  switch (c.read_class) {
    case RC::kMessagePlain: {
      const google::protobuf::Reflection* refl = msg.GetReflection();
      if (refl == nullptr) return std::nullopt;
      const google::protobuf::Message& sub = refl->GetMessage(msg, &field);
      cv.kind = CEL_MESSAGE;
      cv.payload.msg_slot = ctx.refs.InternProtoMessage(&sub);
      break;
    }
    case RC::kMap:
      cv.kind = CEL_MAP_HOST;
      cv.payload.ref_slot = ctx.refs.InternProtoMapField(&msg, &field);
      break;
    case RC::kRepeated:
      cv.kind = CEL_LIST_HOST;
      cv.payload.ref_slot = ctx.refs.InternProtoListField(&msg, &field);
      break;
    case RC::kScalar:
    case RC::kMessageWrapper:
    case RC::kMessageAny:
    case RC::kMessageTimestamp:
    case RC::kMessageDuration:
    case RC::kMessageJson:
      return std::nullopt;
  }
  ctx.mem.WriteCelValue(out_slot, cv);
  return absl::OkStatus();
}

// Resolve `site`'s field against `msg` through the per-access-site
// single-entry cache (`FieldRefEntry::resolved` — see the seam
// rationale on that member in cel_host.h).  Hit: pointer-compare the
// message's `Descriptor*` against the cached owner, return the cached
// `FieldDescriptor*`.  Miss: resolve + classify once, overwrite the
// entry.  Failed resolutions are NOT cached — a dynamic pool can gain
// extensions between reads, and the not-found path is cold anyway.
//
// Mutates `site.resolved` without locks: an Instance is
// single-threaded per Eval (the `HostExternrefTable` interning on
// this same path already assumes it).
const google::protobuf::FieldDescriptor* absl_nullable
ResolveFieldThroughSiteCache(const google::protobuf::Message& msg,
                             const FieldRefEntry& site) {
  const google::protobuf::Descriptor* d = msg.GetDescriptor();
  if (d == nullptr) return nullptr;
  ResolvedFieldCache& cache = site.resolved;
  if (cache.owner == d) return cache.field;
  const google::protobuf::FieldDescriptor* field = ResolveFieldDescriptor(
      msg, static_cast<int>(site.field_number), site.field_name);
  if (field == nullptr) return nullptr;
  ResolvedFieldCache fresh;
  fresh.field = field;
  ClassifyResolvedField(*field, &fresh);
  fresh.owner = d;
  cache = fresh;
  return field;
}

}  // namespace

// Marshals a proto string/bytes field's payload into the per-Eval arena
// and stamps the resulting span (offset + length) onto `cv`.  The wasm
// guest only sees linear memory, so the payload is copied in — the same
// copy `EncodeSpan` does, minus the `celwasm::Value` round-trip.
ABSL_ATTRIBUTE_ALWAYS_INLINE static absl::Status
WriteProtoStringFieldToCelValue(const google::protobuf::Message& msg,
                                const google::protobuf::FieldDescriptor& field,
                                ArenaAllocator& alloc, CelValue* cv) {
  using FD = google::protobuf::FieldDescriptor;
  std::string scratch;
  absl::string_view s =
      msg.GetReflection()->GetStringReference(msg, &field, &scratch);
  uint32_t off = 0;
  uint8_t* p = alloc.Alloc(s.size(), &off);
  if (p == nullptr && !s.empty()) {
    return absl::ResourceExhaustedError(
        "arena OOM marshalling proto string/bytes field");
  }
  if (!s.empty()) std::memcpy(p, s.data(), s.size());
  cv->kind = field.type() == FD::TYPE_BYTES ? CEL_BYTES : CEL_STRING;
  cv->payload.s.ptr = off;
  cv->payload.s.len = static_cast<uint32_t>(s.size());
  return absl::OkStatus();
}

// Reads a scalar proto field via reflection and writes the CelValue
// straight into `out_slot`, skipping the `celwasm::Value` variant
// middleman.  Profiling showed ~75% of a field read's cost was the
// Value build + StatusOr move + EncodeValue visitation, NOT the
// reflection (~3ns) or the crossing (~3ns); writing the CelValue
// directly removes it.  Every scalar read is total (proto3 unset ->
// default; no error path), so the direct write is exact.  Precondition:
// `field` is `kScalar` (its cpp_type is never MESSAGE — message fields
// classify to a kMessage* class and go through
// `ReadClassifiedMessageField`).  Force-inlined into its sole caller
// (`CelGetFieldImpl`) so extracting it for readability does not
// re-introduce a per-field-read call: the codegen matches the original
// inline switch (verified by the proto field-read benches).
ABSL_ATTRIBUTE_ALWAYS_INLINE static absl::Status WriteScalarFieldToSlot(
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, uint32_t out_slot,
    const TrampolineContext& ctx) {
  using FD = google::protobuf::FieldDescriptor;
  const google::protobuf::Reflection* refl = msg.GetReflection();
  CelValue cv{};
  switch (field.cpp_type()) {
    case FD::CPPTYPE_INT32:
      cv.kind = CEL_INT;
      cv.payload.i = refl->GetInt32(msg, &field);
      break;
    case FD::CPPTYPE_INT64:
      cv.kind = CEL_INT;
      cv.payload.i = refl->GetInt64(msg, &field);
      break;
    case FD::CPPTYPE_UINT32:
      cv.kind = CEL_UINT;
      cv.payload.u = refl->GetUInt32(msg, &field);
      break;
    case FD::CPPTYPE_UINT64:
      cv.kind = CEL_UINT;
      cv.payload.u = refl->GetUInt64(msg, &field);
      break;
    case FD::CPPTYPE_BOOL:
      cv.kind = CEL_BOOL;
      cv.payload.b = refl->GetBool(msg, &field) ? 1 : 0;
      break;
    case FD::CPPTYPE_DOUBLE:
      cv.kind = CEL_DOUBLE;
      cv.payload.d = refl->GetDouble(msg, &field);
      break;
    case FD::CPPTYPE_FLOAT:
      cv.kind = CEL_DOUBLE;
      cv.payload.d = refl->GetFloat(msg, &field);
      break;
    case FD::CPPTYPE_ENUM:
      // CEL surfaces an enum field as its int value.
      cv.kind = CEL_INT;
      cv.payload.i = refl->GetEnumValue(msg, &field);
      break;
    case FD::CPPTYPE_STRING:
      if (auto s = WriteProtoStringFieldToCelValue(msg, field, ctx.alloc, &cv);
          !s.ok()) {
        return s;
      }
      break;
    case FD::CPPTYPE_MESSAGE:
      ABSL_CHECK(false) << "WriteScalarFieldToSlot: message field `"
                        << field.name() << "` is not a scalar";
  }
  ctx.mem.WriteCelValue(out_slot, cv);
  return absl::OkStatus();
}

absl::Status CelGetFieldImpl(uint32_t out_slot, uint32_t msg_slot,
                             uint32_t field_ref_id, uint32_t attribute_id,
                             const TrampolineContext& ctx) {
  auto prelude_or =
      RunFieldPrelude(out_slot, msg_slot, field_ref_id, attribute_id, ctx);
  if (!prelude_or.ok()) return prelude_or.status();
  if (prelude_or->sentinel_handled) return absl::OkStatus();

  // Proto fast path: backings exposing a real proto message
  // (`ProtoBacking` / `OwnedProtoBacking` — `message()` non-null)
  // read through the per-access-site resolved-field cache instead of
  // re-resolving the FieldDescriptor and re-classifying WKT shapes on
  // every read.  Same precedent as `CelMessageIsZeroImpl` /
  // `CelMessageEqImpl`, which already reach proto reflection through
  // `message()` rather than per-backing virtuals.  Non-proto custom
  // backings (JSON, …) keep the virtual `ReadField` contract below.
  if (const google::protobuf::Message* msg = prelude_or->backing->message();
      msg != nullptr) {
    const google::protobuf::FieldDescriptor* field =
        ResolveFieldThroughSiteCache(*msg, *prelude_or->field);
    if (field == nullptr) {
      return EncodeFieldResult(FieldNotFound(prelude_or->field->field_name),
                               out_slot, ctx);
    }
    // Dispatch on the resolved read class.  Map/list intern a host
    // backing; scalars (numerics/bool/enum/string/bytes) write their
    // CelValue straight into the slot via `WriteScalarFieldToSlot`,
    // skipping the celwasm::Value variant; only message/WKT classes
    // need the Value path (`ReadClassifiedMessageField` + encode), since
    // their decode (Any unpack, timestamp/duration, wrapper peel) is
    // genuinely more than a scalar read.
    const ResolvedFieldCache& rc = prelude_or->field->resolved;
    if (auto fast =
            TryEncodeAggregateFieldFast(*msg, *field, rc, out_slot, ctx);
        fast.has_value()) {
      return *std::move(fast);  // kMap / kRepeated
    }
    if (rc.read_class == ProtoFieldReadClass::kScalar) {
      return WriteScalarFieldToSlot(*msg, *field, out_slot, ctx);
    }
    const google::protobuf::Reflection* refl = msg->GetReflection();
    if (refl == nullptr) {
      return absl::InternalError("CelGetFieldImpl: message has no reflection");
    }
    auto v_or = ReadClassifiedMessageField(*refl, *msg, *field, rc);
    if (!v_or.ok()) return v_or.status();
    return EncodeFieldResult(*v_or, out_slot, ctx);
  }

  // expected_type informational at M2 — ProtoBacking dispatches
  // on descriptor cpp_type, not on this hint.  Pass an arbitrary
  // scalar; real plumb-through arrives with the typed-narrowing
  // milestone.
  auto v_or = prelude_or->backing->ReadField(prelude_or->field->field_number,
                                             prelude_or->field->field_name,
                                             celwasm::CelType::Int());
  if (!v_or.ok()) return v_or.status();
  return EncodeFieldResult(*v_or, out_slot, ctx);
}

absl::Status CelHasFieldImpl(uint32_t out_slot, uint32_t msg_slot,
                             uint32_t field_ref_id, uint32_t attribute_id,
                             const TrampolineContext& ctx) {
  auto prelude_or =
      RunFieldPrelude(out_slot, msg_slot, field_ref_id, attribute_id, ctx);
  if (!prelude_or.ok()) return prelude_or.status();
  if (prelude_or->sentinel_handled) return absl::OkStatus();

  // Same proto fast path as CelGetFieldImpl: resolve through the
  // per-access-site cache, then probe presence on the resolved
  // descriptor.  Unresolvable field → false, matching
  // `ProtoBacking::HasField`.
  bool present = false;
  if (const google::protobuf::Message* msg = prelude_or->backing->message();
      msg != nullptr) {
    const google::protobuf::FieldDescriptor* field =
        ResolveFieldThroughSiteCache(*msg, *prelude_or->field);
    present = field != nullptr && ProtoHasFieldResolved(*msg, *field);
  } else {
    present = prelude_or->backing->HasField(prelude_or->field->field_number,
                                            prelude_or->field->field_name);
  }
  CelValue out{};
  out.kind = CEL_BOOL;
  out.payload.b = present ? 1 : 0;
  ctx.mem.WriteCelValue(out_slot, out);
  return absl::OkStatus();
}

absl::Status CelMessageEqImpl(uint32_t out_slot, uint32_t a_slot,
                              uint32_t b_slot, const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (a_cv.kind != CEL_MESSAGE || b_cv.kind != CEL_MESSAGE) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMessageBacking* a_backing = ctx.refs.Lookup(a_cv.payload.msg_slot);
  const HostMessageBacking* b_backing = ctx.refs.Lookup(b_cv.payload.msg_slot);
  if (a_backing == nullptr || b_backing == nullptr) {
    return absl::FailedPreconditionError(
        "CelMessageEqImpl: message msg_slot not found in ExternrefTable");
  }
  // HostMessageBacking exposes its underlying Message* via the
  // virtual `message()` so both `ProtoBacking` (host-bound) and
  // `OwnedProtoBacking` (proto-literal-built) participate uniformly.
  // Custom non-proto backings return nullptr from `message()`,
  // making the pair kNotComparable, which surfaces here as
  // kTypeMismatch (proto-vs-non-proto eq is a spec error per
  // langdef §"Equality") — same for Any-unpack failures.
  switch (CompareProtoMessages(a_backing->message(), b_backing->message())) {
    case ProtoMessageEqOutcome::kEqual:
      WriteWireBool(true, out_slot, ctx.mem);
      return absl::OkStatus();
    case ProtoMessageEqOutcome::kUnequal:
      WriteWireBool(false, out_slot, ctx.mem);
      return absl::OkStatus();
    case ProtoMessageEqOutcome::kNotComparable:
      WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
      return absl::OkStatus();
  }
  ABSL_CHECK(false) << "CompareProtoMessages returned an out-of-enum value";
  return absl::InternalError("unreachable");
}

absl::Status CelMessageIsZeroImpl(uint32_t out_slot, uint32_t msg_slot,
                                  const TrampolineContext& ctx) {
  const CelValue msg_cv = ctx.mem.ReadCelValue(msg_slot);
  if (AbsorbUnary(msg_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (msg_cv.kind != CEL_MESSAGE) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMessageBacking* backing = ctx.refs.Lookup(msg_cv.payload.msg_slot);
  const google::protobuf::Message* msg_ptr =
      backing == nullptr ? nullptr : backing->message();
  if (msg_ptr == nullptr) {
    // Unmapped slot is an adapter bug; a non-proto custom backing has
    // no reflection to walk (cel-cpp requires custom struct values to
    // implement IsZeroValue themselves — `HostMessageBacking` has no
    // such hook).  Either way: clean poison, not a trap.  The wasm
    // caller treats any non-BOOL result as "non-zero" so the operand
    // propagates instead of vanishing.
    WriteWireError(CEL_ERR_HOST_ADAPTER_ERROR, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const google::protobuf::Message& msg = *msg_ptr;
  const google::protobuf::Reflection* reflection = msg.GetReflection();
  // cel-cpp parity — `ParsedMessageValue::IsZeroValue()`
  // (third_party/cel-cpp/common/values/parsed_message_value.cc:78):
  // zero iff the unknown-field set is empty AND no fields are set.
  bool is_zero = reflection->GetUnknownFields(msg).empty();
  if (is_zero) {
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    reflection->ListFields(msg, &fields);
    is_zero = fields.empty();
  }
  WriteWireBool(is_zero, out_slot, ctx.mem);
  return absl::OkStatus();
}

// ══════════════════════════════════════════════════════════════════
// `cel_host.cel_make_message(type_id, out_slot)`.
//
// Resolves type_id → Descriptor* against the per-Plan
// `bindings.message_types` lookup (populated from `cel.abi.types[]`
// at `Engine::Plan` time).  Allocates a default-constructed proto
// via `MessageFactory::generated_factory()->GetPrototype(desc)
// ->New()`, wraps in `OwnedProtoBacking`, interns into the
// ExternrefTable, and writes a CEL_MESSAGE CelValue with the
// interned slot to `out_slot`.
//
// Spec-level errors (sentinel id, OOR, unknown FQN, prototype-
// missing) write CEL_ERROR to out_slot and return OK; non-OK
// Status is reserved for true infrastructure failures (none today
// — the lookup table is bounds-checked, descriptor null surfaces
// as a clean error).
// ══════════════════════════════════════════════════════════════════

absl::Status CelMakeMessageImpl(uint32_t type_id, uint32_t out_slot,
                                const TrampolineContext& ctx) {
  if (type_id == 0 || type_id >= ctx.bindings.message_types.size()) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const MessageTypeEntry& entry = ctx.bindings.message_types[type_id];
  if (entry.descriptor == nullptr) {
    // FQN was not resolvable against the pool at Plan time — treat
    // as a spec-level type error rather than crashing the eval.
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          entry.descriptor);
  if (prototype == nullptr) {
    // Generated factory doesn't know about this descriptor — most
    // likely a dynamic descriptor loaded via SchemaProtoSource.
    // Dynamic-descriptor support is a follow-up tied to the
    // conformance harness's descriptor mode.  Surface as a clean
    // spec error.
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  if (msg == nullptr) {
    return absl::ResourceExhaustedError(
        "CelMakeMessageImpl: prototype->New() returned null");
  }
  auto backing = std::make_shared<OwnedProtoBacking>(std::move(msg));
  const uint32_t slot = ctx.refs.Intern(std::move(backing));
  CelValue out{};
  out.kind = CEL_MESSAGE;
  out.payload.msg_slot = slot;
  ctx.mem.WriteCelValue(out_slot, out);
  return absl::OkStatus();
}

// `cel_host.resolve_message_type_name` — descriptor-FQN resolver
// for `type(<message>)`.
//
//   1. Read CEL_MESSAGE at in_slot; look up `payload.msg_slot` in
//      `ctx.refs` → `HostMessageBacking*`.
//   2. Backing's `Message()` → `proto*`; `proto->GetDescriptor()
//      ->full_name()` → FQN std::string.
//   3. Allocate FQN bytes in the per-Eval arena via `ctx.alloc`.
//   4. Stamp `{kind: CEL_TYPE, payload.s: {arena_off, len}}` into
//      out_slot.
absl::Status CelResolveMessageTypeNameImpl(uint32_t out_slot, uint32_t in_slot,
                                           const TrampolineContext& ctx) {
  // Read the CEL_MESSAGE CelValue at `in_slot`; defence-in-depth the
  // kind check (the runtime helper already routes on kind, but a
  // direct caller — e.g. tests — could reach here with a wrong-kind
  // operand).
  const CelValue in_cv = ctx.mem.ReadCelValue(in_slot);
  if (in_cv.kind != CEL_MESSAGE) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // Dereference the externref backing.  A miss here is a host-
  // adapter bug — the runtime should not have produced a CEL_MESSAGE
  // CelValue whose msg_slot is unmapped.
  const HostMessageBacking* backing = ctx.refs.Lookup(in_cv.payload.msg_slot);
  if (backing == nullptr) {
    WriteWireError(CEL_ERR_HOST_ADAPTER_ERROR, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const google::protobuf::Message* msg = backing->message();
  if (msg == nullptr) {
    // Non-proto backing — no descriptor.  Treat as adapter error;
    // a future milestone may surface a host-supplied type-name on
    // HostMessageBacking instead.
    WriteWireError(CEL_ERR_HOST_ADAPTER_ERROR, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // `Descriptor::full_name()` returns `absl::string_view` in modern
  // protobuf — copy out into a string_view we own for the alloc below.
  const absl::string_view fqn = msg->GetDescriptor()->full_name();
  // Allocate the FQN bytes in the per-Eval arena and stamp the
  // CelSpan into out_slot.  Same lifetime model as the runtime
  // helper's primitive-name path: bytes outlive `arena_reset` because
  // the arena is reset only at the start of the NEXT Eval, and
  // user-visible Values copy the bytes out via the read-side
  // decoder before the arena resets.
  uint32_t off = 0;
  uint8_t* dst = ctx.alloc.Alloc(fqn.size(), &off);
  if (dst == nullptr && !fqn.empty()) {
    return absl::ResourceExhaustedError(
        absl::StrCat("CelResolveMessageTypeNameImpl: arena OOM (need ",
                     fqn.size(), " bytes for FQN `", fqn, "`)"));
  }
  if (dst != nullptr && !fqn.empty()) {
    std::memcpy(dst, fqn.data(), fqn.size());
  }
  CelValue out_cv{};
  out_cv.kind = CEL_TYPE;
  out_cv.payload.s.ptr = off;
  out_cv.payload.s.len = static_cast<uint32_t>(fqn.size());
  ctx.mem.WriteCelValue(out_slot, out_cv);
  return absl::OkStatus();
}

// `PoisonCelValue` moved to cel_host_error.cc (M11 Slice E); still
// used by `CelWktUnwrapWrapperImpl` below via the new header.

// kStructExpr tail-unwrap for the 9 wrapper FQNs.  Reads
// the CEL_MESSAGE at `msg_slot`, peels the inner `value` field via
// the shared `UnpackWrapperMessage` helper (also used by the
// read-side auto-peel in `ReadSingularMessageField`), and writes
// the matching scalar CelValue at `out_slot`.  Cross-checks the
// produced kind against the caller-supplied `wrapper_kind` (1..6
// per CelKind); mismatch surfaces as `CEL_ERR_TYPE_MISMATCH`
// (codegen-regression tripwire).  Mirrors `CelWktUnwrapTimeImpl`.
absl::Status CelWktUnwrapWrapperImpl(uint32_t out_slot, uint32_t msg_slot,
                                     uint32_t wrapper_kind,
                                     const TrampolineContext& ctx) {
  const CelValue in = ctx.mem.ReadCelValue(msg_slot);
  if (in.kind == CEL_ERROR || in.kind == CEL_UNKNOWN) {
    ctx.mem.WriteCelValue(out_slot, in);
    return absl::OkStatus();
  }
  if (in.kind != CEL_MESSAGE) {
    ctx.mem.WriteCelValue(out_slot, PoisonCelValue(CEL_ERR_TYPE_MISMATCH));
    return absl::OkStatus();
  }
  const HostMessageBacking* backing = ctx.refs.Lookup(in.payload.msg_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelWktUnwrapWrapperImpl: msg_slot ", in.payload.msg_slot,
                     " not found in ExternrefTable"));
  }
  const google::protobuf::Message* msg = backing->message();
  if (msg == nullptr) {
    ctx.mem.WriteCelValue(out_slot, PoisonCelValue(CEL_ERR_TYPE_MISMATCH));
    return absl::OkStatus();
  }
  auto wrap = UnpackWrapperMessage(*msg);
  if (!wrap.has_value()) {
    ctx.mem.WriteCelValue(out_slot, PoisonCelValue(CEL_ERR_TYPE_MISMATCH));
    return absl::OkStatus();
  }
  CelValue out_cv{};
  if (auto s = EncodeValue(*wrap, &out_cv, ctx.alloc); !s.ok()) return s;
  // Codegen-regression tripwire: produced inner-scalar kind must
  // match the caller-supplied wrapper_kind (1..6 per CelKind).
  if (out_cv.kind != wrapper_kind) {
    ctx.mem.WriteCelValue(out_slot, PoisonCelValue(CEL_ERR_TYPE_MISMATCH));
    return absl::OkStatus();
  }
  ctx.mem.WriteCelValue(out_slot, out_cv);
  return absl::OkStatus();
}

absl::Status CelWktUnwrapTimeImpl(uint32_t out_slot, uint32_t msg_slot,
                                  const TrampolineContext& ctx) {
  const CelValue in = ctx.mem.ReadCelValue(msg_slot);
  if (in.kind == CEL_ERROR || in.kind == CEL_UNKNOWN) {
    ctx.mem.WriteCelValue(out_slot, in);
    return absl::OkStatus();
  }
  if (in.kind != CEL_MESSAGE) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }
  const HostMessageBacking* backing = ctx.refs.Lookup(in.payload.msg_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelWktUnwrapTimeImpl: msg_slot ", in.payload.msg_slot,
                     " not found in ExternrefTable"));
  }
  const google::protobuf::Message* msg = backing->message();
  if (msg == nullptr) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }
  // Reuse `UnpackWellKnownTimeMessage` — same helper the field-read
  // normaliser uses.  Returns nullopt if descriptor doesn't match
  // WKT Timestamp/Duration (which shouldn't happen — codegen only
  // emits this for matching s.name() — but defence-in-depth).
  auto wkt = UnpackWellKnownTimeMessage(*msg);
  if (!wkt.has_value()) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }
  CelValue out_cv{};
  if (auto s = EncodeValue(*wkt, &out_cv, ctx.alloc); !s.ok()) return s;
  ctx.mem.WriteCelValue(out_slot, out_cv);
  return absl::OkStatus();
}

}  // namespace celwasm
