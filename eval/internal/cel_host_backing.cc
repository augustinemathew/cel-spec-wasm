#include "eval/internal/cel_host_backing.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"
#include "eval/error.h"
#include "eval/internal/cel_host_error.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "shared/type.h"

namespace celwasm {

namespace {

// The exported WKT-peel helpers (`UnpackWrapperMessage` /
// `UnpackWellKnownTimeMessage`) are declared in cel_host_backing.h;
// their bodies follow this anonymous namespace, next to the
// file-private peelers they compose with.
//
// M11 Slice E: the simple error-CelValue factories (`FieldNotFound`,
// `MakeError`, `KeyTypeMismatch`, `NoSuchKey`, `IndexOutOfBounds`)
// and the wire-format error encoders (`WriteWireError`,
// `WriteWireBool`, `WriteWireInt`, `WriteInvalidArgumentError`,
// `PoisonCelValue`) plus the 3VL absorbers (`AbsorbUnary`,
// `AbsorbBinary`) moved out to `cel_host_error.{cc,h}` — they're
// included via `<eval/internal/cel_host_error.h>` above
// and remain callable here under the `celwasm::` namespace.

// Forward-declared so `MaybeUnpackWktMessage` (below) can chain it;
// the definition follows after the recursive JSON-peel helpers.
// NOLINTNEXTLINE(readability-redundant-declaration)
std::optional<celwasm::Value> UnpackJsonValueMessage(
    const google::protobuf::Message& sub);

// Chain the well-known-type peelers in a single call site: returns
// the inner-scalar / Timestamp / Duration / JSON-Value value if `sub`
// is one of the recognised WKT message types, otherwise
// `std::nullopt`.  Lets both Any-unwrap and proto-field-read share one
// entry point without duplicating the if-cascade.
inline std::optional<celwasm::Value> MaybeUnpackWktMessage(
    const google::protobuf::Message& sub) {
  if (auto wrap = UnpackWrapperMessage(sub); wrap.has_value()) return wrap;
  if (auto wkt = UnpackWellKnownTimeMessage(sub); wkt.has_value()) return wkt;
  if (auto js = UnpackJsonValueMessage(sub); js.has_value()) return js;
  return std::nullopt;
}

// Strip the standard Any URL prefix and return the bare FQN.
// cel-cpp accepts `type.googleapis.com/` and `type.googleprod.com/`
// (see `internal/well_known_types.cc:1960-1967`); any other URL
// shape is rejected so a malformed type_url surfaces as a clean
// FQN-not-found error instead of silently treating an attacker-
// supplied URL as a pool lookup key.  Empty type_url is handled
// separately by the caller as "Any unset → null".
std::optional<absl::string_view> ExtractAnyFqn(absl::string_view type_url) {
  constexpr absl::string_view kGapisPrefix = "type.googleapis.com/";
  constexpr absl::string_view kGprodPrefix = "type.googleprod.com/";
  if (absl::ConsumePrefix(&type_url, kGapisPrefix)) return type_url;
  if (absl::ConsumePrefix(&type_url, kGprodPrefix)) return type_url;
  return std::nullopt;
}

// Result of peeling exactly one Any layer.  Exactly one of `terminal`
// or `peeled` is set on return:
//   - `terminal` populated → the unwrap is done (Any was unset → null,
//     or an error occurred); caller returns this Value directly.
//   - `peeled` populated → the inner Message was extracted.  Caller
//     checks whether the inner is itself an Any and decides whether
//     to loop.
struct UnpackOneAnyResult {
  std::optional<celwasm::Value> terminal;
  std::unique_ptr<google::protobuf::Message> peeled;
};

// Resolve the Any `type_url` to a descriptor-pool message FQN and
// instantiate + parse the payload bytes into a fresh message.  Returns
// a populated `terminal` on any resolution / parse failure (each an
// error CelValue the caller surfaces directly); on success returns the
// peeled message in `peeled`.  Split from `UnpackOneAnyLayer` so the
// outer helper stays a thin reflection-read + dispatch under the
// function-size gate.  Single call site → inlines back.
UnpackOneAnyResult ResolveAndParseAnyPayload(
    absl::string_view type_url, const std::string& bytes,
    const google::protobuf::DescriptorPool* pool) {
  const auto fqn_opt = ExtractAnyFqn(type_url);
  if (!fqn_opt.has_value()) {
    return {MakeError(celwasm::ErrorCode::kFieldNotFound,
                      absl::StrCat("Any type_url `", type_url,
                                   "` lacks `type.googleapis.com/` or "
                                   "`type.googleprod.com/` prefix")),
            nullptr};
  }
  const absl::string_view fqn = *fqn_opt;
  const google::protobuf::Descriptor* sub_desc =
      pool != nullptr ? pool->FindMessageTypeByName(std::string(fqn)) : nullptr;
  if (sub_desc == nullptr) {
    return {MakeError(celwasm::ErrorCode::kFieldNotFound,
                      absl::StrCat("Any type_url FQN `", fqn,
                                   "` not registered in descriptor pool")),
            nullptr};
  }
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          sub_desc);
  if (prototype == nullptr) {
    return {MakeError(celwasm::ErrorCode::kFieldNotFound,
                      absl::StrCat("Any type `", fqn,
                                   "` has no generated_factory prototype")),
            nullptr};
  }
  std::unique_ptr<google::protobuf::Message> sub(prototype->New());
  if (!sub->ParseFromString(bytes)) {
    return {MakeError(celwasm::ErrorCode::kTypeMismatch,
                      absl::StrCat("Any payload bytes don't parse against `",
                                   fqn, "`")),
            nullptr};
  }
  return {std::nullopt, std::move(sub)};
}

UnpackOneAnyResult UnpackOneAnyLayer(
    const google::protobuf::Message& any,
    const google::protobuf::DescriptorPool* pool) {
  const google::protobuf::Descriptor* any_desc = any.GetDescriptor();
  const google::protobuf::Reflection* any_refl = any.GetReflection();
  const google::protobuf::FieldDescriptor* type_url_fd =
      any_desc->FindFieldByName("type_url");
  const google::protobuf::FieldDescriptor* value_fd =
      any_desc->FindFieldByName("value");
  ABSL_CHECK(any_refl != nullptr && type_url_fd != nullptr &&
             value_fd != nullptr)
      << "UnpackOneAnyLayer: Any descriptor missing type_url/value/reflection";
  std::string url_scratch;
  std::string val_scratch;
  const std::string& type_url =
      any_refl->GetStringReference(any, type_url_fd, &url_scratch);
  if (type_url.empty()) {
    // cel-cpp returns InvalidArgument here (see
    // `third_party/cel-cpp/internal/well_known_types.cc::AdaptAny`
    // — the prefix check fails on an empty string and yields
    // "unable to find descriptor for type URL: ").  Pinned by
    // conformance row `dynamic/any/literal_empty`.  The
    // unset-Any-field path skips this — it short-circuits on
    // `HasField` in `ReadSingularMessageField`.
    return {MakeError(celwasm::ErrorCode::kFieldNotFound,
                      "Any type_url is empty (no descriptor to unpack)"),
            nullptr};
  }
  const std::string& bytes =
      any_refl->GetStringReference(any, value_fd, &val_scratch);
  return ResolveAndParseAnyPayload(type_url, bytes, pool);
}

// Iteratively unwrap an Any (M11 Slice A — fixes the P0 where
// `Any{Any{Int32Value{value:7}}}` previously surfaced as
// `CEL_MESSAGE(google.protobuf.Any)` instead of `int 7`).
// Mirrors cel-cpp's `AdaptAny` (`internal/well_known_types.cc:1943-2007`):
// peel one layer; if the inner descriptor is still `google.protobuf.Any`,
// peel again; otherwise try wrapper / WKT-time peel and return.
//
// Depth `ABSL_CHECK` at 1024 is belt-and-suspenders — wire-size
// implicitly bounds depth in practice, but a malformed Any chain
// shouldn't blow the host stack, and the M7-A design doc explicitly
// recommended this constant.
celwasm::Value UnpackAnyToValueAnonImpl(
    const google::protobuf::Message& any,
    const google::protobuf::DescriptorPool* pool) {
  std::unique_ptr<google::protobuf::Message> owned;
  const google::protobuf::Message* current = &any;
  for (int depth = 0;; ++depth) {
    ABSL_CHECK(depth < 1024)
        << "UnpackAnyToValue: Any-of-Any depth >= 1024 — runaway recursion?";
    UnpackOneAnyResult layer = UnpackOneAnyLayer(*current, pool);
    if (layer.terminal.has_value()) return *std::move(layer.terminal);
    owned = std::move(layer.peeled);
    const google::protobuf::Descriptor* d = owned->GetDescriptor();
    if (d != nullptr && d->full_name() == "google.protobuf.Any") {
      current = owned.get();
      continue;
    }
    if (auto v = MaybeUnpackWktMessage(*owned); v.has_value()) {
      return *std::move(v);
    }
    return celwasm::Value::OwnedMessage(std::move(owned));
  }
}

// Well-known time-type normaliser for proto field reads.  When a
// singular CPPTYPE_MESSAGE field resolves to
// `google.protobuf.Timestamp` / `google.protobuf.Duration`, peel the
// (seconds, nanos) pair via reflection (field numbers 1 and 2 are
// pinned by the well-known type definitions) and return the matching
// celwasm::Value::Timestamp / Duration.  Returns nullopt for any other
// message type — caller falls back to `HostMessage(ProtoBacking)`.
//
// Reflection-based on purpose: works for both generated-class
// messages (via DynamicCastToGenerated downcast) and dynamic
// messages loaded from a runtime descriptor pool.
//
// Split in two so the classified read path (which has already
// resolved the FQN gate and the seconds/nanos sub-fields once, at
// cache-fill time) can call the field-level peel directly:
// `UnpackTimeMessageFields` does the reflection reads only;
// `UnpackWellKnownTimeMessage` keeps the descriptor-walking gate for
// callers with no precomputed classification (the Any-unwrap chain).
std::optional<celwasm::Value> UnpackTimeMessageFields(
    const google::protobuf::Message& sub,
    const google::protobuf::FieldDescriptor& seconds_field,
    const google::protobuf::FieldDescriptor& nanos_field, bool is_timestamp) {
  const google::protobuf::Reflection* refl = sub.GetReflection();
  if (refl == nullptr) return std::nullopt;
  const int64_t s = refl->GetInt64(sub, &seconds_field);
  const int32_t ns = refl->GetInt32(sub, &nanos_field);
  if (is_timestamp) {
    return celwasm::Value::Timestamp(absl::UnixEpoch() + absl::Seconds(s) +
                                     absl::Nanoseconds(ns));
  }
  return celwasm::Value::Duration(absl::Seconds(s) + absl::Nanoseconds(ns));
}

// Forward declarations for the JSON-value peel pair — they recurse
// (a Value can hold a Struct or ListValue, each holding Values;
// `UnpackJsonValueMessage` is declared with the other WKT peelers
// above).
celwasm::Value UnpackJsonStruct(const google::protobuf::Message& s);
celwasm::Value UnpackJsonListValue(const google::protobuf::Message& lv);

// Peel a `google.protobuf.Struct` into a CEL map<string, dyn>.  The
// `fields` map field (number 1) is a map<string, Value>; each entry's
// value is recursively peeled via `UnpackJsonValueMessage`.
celwasm::Value UnpackJsonStruct(const google::protobuf::Message& s) {
  const google::protobuf::Descriptor* d = s.GetDescriptor();
  const google::protobuf::Reflection* refl = s.GetReflection();
  const google::protobuf::FieldDescriptor* fields_fd =
      d != nullptr ? d->FindFieldByNumber(1) : nullptr;
  ABSL_CHECK(refl != nullptr && fields_fd != nullptr && fields_fd->is_map())
      << "UnpackJsonStruct: google.protobuf.Struct missing `fields` map";
  const google::protobuf::FieldDescriptor* key_fd =
      fields_fd->message_type()->FindFieldByNumber(1);
  const google::protobuf::FieldDescriptor* val_fd =
      fields_fd->message_type()->FindFieldByNumber(2);
  std::vector<std::pair<celwasm::Value, celwasm::Value>> entries;
  const int n = refl->FieldSize(s, fields_fd);
  entries.reserve(n);
  for (int i = 0; i < n; ++i) {
    const google::protobuf::Message& entry =
        refl->GetRepeatedMessage(s, fields_fd, i);
    const google::protobuf::Reflection* er = entry.GetReflection();
    std::string scratch;
    celwasm::Value key =
        celwasm::Value::String(er->GetStringReference(entry, key_fd, &scratch));
    auto val = UnpackJsonValueMessage(er->GetMessage(entry, val_fd));
    entries.emplace_back(std::move(key), val.has_value()
                                             ? *std::move(val)
                                             : celwasm::Value::Null());
  }
  return celwasm::Value::Map(std::move(entries));
}

// Peel a `google.protobuf.ListValue` into a CEL list<dyn>.  The
// `values` repeated-Value field (number 1) is peeled element-by-
// element via `UnpackJsonValueMessage`.
celwasm::Value UnpackJsonListValue(const google::protobuf::Message& lv) {
  const google::protobuf::Descriptor* d = lv.GetDescriptor();
  const google::protobuf::Reflection* refl = lv.GetReflection();
  const google::protobuf::FieldDescriptor* values_fd =
      d != nullptr ? d->FindFieldByNumber(1) : nullptr;
  ABSL_CHECK(refl != nullptr && values_fd != nullptr &&
             values_fd->is_repeated())
      << "UnpackJsonListValue: google.protobuf.ListValue missing `values`";
  std::vector<celwasm::Value> elements;
  const int n = refl->FieldSize(lv, values_fd);
  elements.reserve(n);
  for (int i = 0; i < n; ++i) {
    auto v = UnpackJsonValueMessage(refl->GetRepeatedMessage(lv, values_fd, i));
    elements.push_back(v.has_value() ? *std::move(v) : celwasm::Value::Null());
  }
  return celwasm::Value::List(std::move(elements));
}

// JSON well-known-type normaliser for proto field reads.  When a
// CPPTYPE_MESSAGE field resolves to google.protobuf.Value /
// .Struct / .ListValue, peel it into the matching CEL value:
//   Value  → its set kind_case (null/number/string/bool/struct/list).
//   Struct → CEL map<string, dyn>.
//   ListValue → CEL list<dyn>.
// Returns nullopt for any other message type.  Reflection-based so
// it works for generated and dynamic-pool messages alike.
std::optional<celwasm::Value> UnpackJsonValueMessage(
    const google::protobuf::Message& sub) {
  const google::protobuf::Descriptor* d = sub.GetDescriptor();
  if (d == nullptr) return std::nullopt;
  const absl::string_view fqn = d->full_name();
  if (fqn == "google.protobuf.Struct") return UnpackJsonStruct(sub);
  if (fqn == "google.protobuf.ListValue") return UnpackJsonListValue(sub);
  if (fqn != "google.protobuf.Value") return std::nullopt;
  const google::protobuf::Reflection* refl = sub.GetReflection();
  if (refl == nullptr) return std::nullopt;
  const google::protobuf::FieldDescriptor* set =
      refl->GetOneofFieldDescriptor(sub, d->FindOneofByName("kind"));
  // Unset Value (no kind_case set) decodes to null per JSON rules.
  if (set == nullptr) return celwasm::Value::Null();
  switch (set->number()) {
    case 1:  // null_value (a NullValue enum)
      return celwasm::Value::Null();
    case 2:  // number_value (double)
      return celwasm::Value::Double(refl->GetDouble(sub, set));
    case 3: {  // string_value
      std::string scratch;
      return celwasm::Value::String(
          refl->GetStringReference(sub, set, &scratch));
    }
    case 4:  // bool_value
      return celwasm::Value::Bool(refl->GetBool(sub, set));
    case 5:  // struct_value
      return UnpackJsonStruct(refl->GetMessage(sub, set));
    case 6:  // list_value
      return UnpackJsonListValue(refl->GetMessage(sub, set));
    default:
      ABSL_CHECK(false) << "UnpackJsonValueMessage: google.protobuf.Value "
                           "kind_case field number "
                        << set->number() << " is not 1..6";
      return std::nullopt;
  }
}

// Well-known WRAPPER-type normaliser for proto field reads.  Mirror
// of `UnpackWellKnownTimeMessage`: when a singular CPPTYPE_MESSAGE
// field resolves to one of the 9
// google.protobuf.{Bool,Int32,Int64,UInt32,UInt64,Float,Double,
// String,Bytes}Value types, peel the inner `value` field (number 1)
// via reflection and return the matching celwasm::Value scalar.
// Returns std::nullopt for any other message type — caller falls
// back to HostMessage(ProtoBacking).
//
// Reflection-based: works for both generated-class messages
// (DynamicCastToGenerated downcast) and dynamic messages loaded
// from a runtime descriptor pool.
//
// Per langdef §"Dynamic Values" line 479 ("wrapper types |
// converted as eponymous field type"): this helper handles the
// SET case — reading the inner scalar.  The UNSET case
// (field-evaluates-to-null per line 484-486) is gated at the
// caller before this helper is reached.
// String / bytes peel for the WKT wrapper inner `value` field.
// CPPTYPE_STRING covers both string and bytes wire types — the
// distinction comes from `field.type()`, not `cpp_type()`.  Pulled
// out of `UnpackWrapperMessage` so the parent stays under the
// readability-function-size gate (9-branch dispatch + this branch
// would otherwise overflow).
celwasm::Value UnpackWrapperStringOrBytes(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& sub,
    const google::protobuf::FieldDescriptor& vf) {
  std::string scratch;
  std::string s(refl.GetStringReference(sub, &vf, &scratch));
  if (vf.type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
    return celwasm::Value::Bytes(std::move(s));
  }
  return celwasm::Value::String(std::move(s));
}

// Field-level wrapper peel: reads the inner `value` field `vf` of a
// wrapper message `sub` whose wrapper-ness the caller has already
// established (either via `UnpackWrapperMessage`'s FQN gate below or
// via a precomputed `ProtoFieldReadClass::kMessageWrapper`
// classification).  Split out so the classified read path skips the
// per-read FQN string compares and `FindFieldByNumber` walk.
std::optional<celwasm::Value> UnpackWrapperValueField(
    const google::protobuf::Message& sub,
    const google::protobuf::FieldDescriptor& value_field) {
  const google::protobuf::Reflection* refl = sub.GetReflection();
  if (refl == nullptr) return std::nullopt;
  const google::protobuf::FieldDescriptor* vf = &value_field;
  // Dispatch on the inner `value` field's cpp_type — closed set per
  // the 9 wrapper definitions.  The wrapper FQN gate precedes entry,
  // so any other cpp_type here is an invariant violation (corrupted
  // descriptor pool); CHECK at the default arm.
  using FD = google::protobuf::FieldDescriptor;
  switch (vf->cpp_type()) {
    case FD::CPPTYPE_BOOL:
      return celwasm::Value::Bool(refl->GetBool(sub, vf));
    case FD::CPPTYPE_INT32:
      return celwasm::Value::Int(refl->GetInt32(sub, vf));
    case FD::CPPTYPE_INT64:
      return celwasm::Value::Int(refl->GetInt64(sub, vf));
    case FD::CPPTYPE_UINT32:
      return celwasm::Value::Uint(refl->GetUInt32(sub, vf));
    case FD::CPPTYPE_UINT64:
      return celwasm::Value::Uint(refl->GetUInt64(sub, vf));
    case FD::CPPTYPE_FLOAT:
      return celwasm::Value::Double(refl->GetFloat(sub, vf));
    case FD::CPPTYPE_DOUBLE:
      return celwasm::Value::Double(refl->GetDouble(sub, vf));
    case FD::CPPTYPE_STRING:
      return UnpackWrapperStringOrBytes(*refl, sub, *vf);
    default:
      ABSL_CHECK(false)
          << "UnpackWrapperValueField: WKT-wrapper FQN claims an unexpected "
             "inner cpp_type "
          << static_cast<int>(vf->cpp_type());
      return std::nullopt;
  }
}

// Port of v1 `ReadNumericField` — dispatches on the field's
// `cpp_type` to build a `celwasm::Value` of the matching scalar kind.
// Returns `std::nullopt` on non-numeric fields; the caller handles
// string / bytes / message branches.
std::optional<celwasm::Value> ReadNumericField(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      return celwasm::Value::Bool(refl.GetBool(msg, &field));
    case FD::CPPTYPE_INT32:
      return celwasm::Value::Int(refl.GetInt32(msg, &field));
    case FD::CPPTYPE_INT64:
      return celwasm::Value::Int(refl.GetInt64(msg, &field));
    case FD::CPPTYPE_UINT32:
      return celwasm::Value::Uint(refl.GetUInt32(msg, &field));
    case FD::CPPTYPE_UINT64:
      return celwasm::Value::Uint(refl.GetUInt64(msg, &field));
    case FD::CPPTYPE_FLOAT:
      return celwasm::Value::Double(refl.GetFloat(msg, &field));
    case FD::CPPTYPE_DOUBLE:
      return celwasm::Value::Double(refl.GetDouble(msg, &field));
    case FD::CPPTYPE_ENUM:
      // CEL treats proto enum values as ints (langdef §2.4.7).
      return celwasm::Value::Int(refl.GetEnumValue(msg, &field));
    default:
      return std::nullopt;
  }
}

// Read a singular CPPTYPE_MESSAGE field, applying the langdef
// §"Field Selection" presence rules and the WKT auto-peel chain
// (Any-unwrap, Timestamp / Duration peel, wrapper peel — see
// `doc/implementation-plan/rewrite/cel-host-surface.md` for the
// peel chain spec), dispatching on the precomputed classification
// in `c` (`c.read_class` is one of the kMessage* arms; `c.field` is
// the resolved field on `msg`'s descriptor).
//
// Presence rules:
//   - Wrapper-typed unset field           → Null (langdef line 484-486;
//                                          both proto2 and proto3,
//                                          exception to default-msg).
//   - Any-typed unset field               → Null (cel-cpp parity).
//   - Generic-message unset field         → default-instance message
//                                          (BOTH proto2 and proto3 —
//                                          no implicit-presence null
//                                          shortcut for message-typed
//                                          fields, only for scalars;
//                                          `GetMessage` returns the
//                                          default-instance reference
//                                          for an unset field, pinned
//                                          by conformance row
//                                          `proto3/empty_field/
//                                          nested_message`).
//
// The WKT peels go through the field-level helpers
// (`UnpackWrapperValueField` / `UnpackTimeMessageFields`) using the
// sub-field descriptors cached at classification time — no FQN
// string compares and no `FindFieldByNumber` on this path.  A
// classification whose cached sub-fields are missing (corrupted
// pool) falls back to `HostMessage(ProtoBacking)`, matching the
// pre-classification behaviour where the gated peelers returned
// nullopt and the read fell through.
// kMessageWrapper arm: an unset wrapper field reads as null (langdef
// §"Dynamic Values" line 484-486; cel-cpp parity); a set one peels
// the inner `value` field through the descriptor cached at
// classification time.
celwasm::Value ReadWrapperMessageArm(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const ResolvedFieldCache& c) {
  if (!refl.HasField(msg, &field)) return celwasm::Value::Null();
  const google::protobuf::Message& sub = refl.GetMessage(msg, &field);
  if (c.sub_field1 != nullptr) {
    if (auto v = UnpackWrapperValueField(sub, *c.sub_field1); v.has_value()) {
      return *std::move(v);
    }
  }
  return celwasm::Value::HostMessage(std::make_shared<ProtoBacking>(&sub));
}

// kMessageAny arm.  Unset Any field → null (langdef + cel-cpp
// parity).  An Any whose `type_url` is empty has no descriptor to
// unpack against, but the SET-but-empty case (`Any{}` literal) and
// the UNSET case must distinguish: corpus row `set_null/single_any`
// expects null for the unset path, while `dynamic/any/literal_empty`
// expects an error for the explicit literal.  We rely on HasField
// (only set when the user explicitly assigned the field) to
// disambiguate at the read-side.
celwasm::Value ReadAnyMessageArm(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field) {
  if (!refl.HasField(msg, &field)) return celwasm::Value::Null();
  const google::protobuf::Message& sub = refl.GetMessage(msg, &field);
  return UnpackAnyToValue(sub, field.message_type()->file()->pool());
}

// kMessageTimestamp / kMessageDuration arm: peel (seconds, nanos)
// through the sub-field descriptors cached at classification time.
celwasm::Value ReadTimeMessageArm(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const ResolvedFieldCache& c,
    bool is_timestamp) {
  const google::protobuf::Message& sub = refl.GetMessage(msg, &field);
  if (c.sub_field1 != nullptr && c.sub_field2 != nullptr) {
    if (auto v = UnpackTimeMessageFields(sub, *c.sub_field1, *c.sub_field2,
                                         is_timestamp);
        v.has_value()) {
      return *std::move(v);
    }
  }
  return celwasm::Value::HostMessage(std::make_shared<ProtoBacking>(&sub));
}

// Classify-on-the-fly adapter for callers that read a singular
// CPPTYPE_MESSAGE field without a per-site cache (map / list element
// reads via `ReadScalarField`).  Same observable behaviour as the
// classified path; the classification cost is paid per call here.
absl::StatusOr<celwasm::Value> ReadSingularMessageField(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field) {
  ResolvedFieldCache local;
  local.field = &field;
  ClassifyResolvedField(field, &local);
  return ReadClassifiedMessageField(refl, msg, field, local);
}

// Read one singular proto field, returning the matching celwasm::Value.
// Non-OK Status is reserved for infrastructure failures (reflection
// missing, descriptor null) — spec-level errors (field not found,
// repeated read at M2) surface as `Value::Error`.
absl::StatusOr<celwasm::Value> ReadScalarField(
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field) {
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) {
    return absl::InternalError(
        "ProtoBacking::ReadField: message has no reflection");
  }
  using FD = google::protobuf::FieldDescriptor;
  if (auto numeric = ReadNumericField(*refl, msg, field); numeric.has_value()) {
    return *std::move(numeric);
  }
  if (field.cpp_type() == FD::CPPTYPE_STRING) {
    std::string scratch;
    const std::string& s = refl->GetStringReference(msg, &field, &scratch);
    const bool is_bytes = field.type() == FD::TYPE_BYTES;
    if (&s == &scratch) {
      // Reflection materialised the payload into the scratch (e.g.
      // cord-backed storage) — the bytes do NOT live in the message,
      // so hand the scratch's ownership to the Value.
      return is_bytes ? celwasm::Value::Bytes(std::move(scratch))
                      : celwasm::Value::String(std::move(scratch));
    }
    // Common case: `s` aliases the message's own storage.  Every
    // caller reads from a message anchored for the rest of the Eval
    // (the ProtoBacking lifetime contract), and the result is
    // consumed before the trampoline returns — return a non-owning
    // view and skip the per-read heap copy; `EncodeSpan` memcpys
    // straight from the proto's storage into the wasm arena.
    return is_bytes ? celwasm::Value::BytesView(s)
                    : celwasm::Value::StringView(s);
  }
  if (field.cpp_type() == FD::CPPTYPE_MESSAGE) {
    return ReadSingularMessageField(*refl, msg, field);
  }
  return absl::InternalError(absl::StrCat(
      "ProtoBacking::ReadField: unhandled cpp_type ",
      static_cast<int>(field.cpp_type()), " on field `", field.name(), "`"));
}

// Full field-read dispatch on a precomputed classification.  `field`
// must be resolved on `msg`'s descriptor and match `c` (callers pass
// `*c.field` after a successful resolve; the reference parameter
// keeps the non-null contract explicit).  This is the per-read half
// of the resolve/classify split: no descriptor-pool walks, no FQN
// string compares — one enum switch, then straight into reflection.
absl::StatusOr<celwasm::Value> ReadFieldClassified(
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const ResolvedFieldCache& c) {
  using RC = ProtoFieldReadClass;
  switch (c.read_class) {
    // Map fields land as `Value::HostMap(ProtoMap{…})` — the
    // trampoline interns the backing into the ExternrefTable and
    // hands a `CEL_MAP_HOST` slot back to wasm.  REPEATED (non-map)
    // fields land as `Value::HostList(ProtoList{…})` — same intern
    // path, separate ExternrefTable namespace.
    case RC::kMap:
      return celwasm::Value::HostMap(std::make_shared<ProtoMap>(&msg, &field));
    case RC::kRepeated:
      return celwasm::Value::HostList(
          std::make_shared<ProtoList>(&msg, &field));
    case RC::kScalar:
      return ReadScalarField(msg, field);
    case RC::kMessageWrapper:
    case RC::kMessageAny:
    case RC::kMessageTimestamp:
    case RC::kMessageDuration:
    case RC::kMessageJson:
    case RC::kMessagePlain: {
      const google::protobuf::Reflection* refl = msg.GetReflection();
      if (refl == nullptr) {
        return absl::InternalError(
            "ProtoBacking::ReadField: message has no reflection");
      }
      return ReadClassifiedMessageField(*refl, msg, field, c);
    }
  }
  ABSL_CHECK(false) << "ReadFieldClassified: unhandled read class "
                    << static_cast<int>(c.read_class);
  return absl::InternalError("unreachable");
}

}  // namespace

std::optional<celwasm::Value> UnpackWellKnownTimeMessage(
    const google::protobuf::Message& sub) {
  const google::protobuf::Descriptor* d = sub.GetDescriptor();
  if (d == nullptr) return std::nullopt;
  const absl::string_view fqn = d->full_name();
  const bool is_timestamp = (fqn == "google.protobuf.Timestamp");
  const bool is_duration = (fqn == "google.protobuf.Duration");
  if (!is_timestamp && !is_duration) return std::nullopt;
  const google::protobuf::FieldDescriptor* sf = d->FindFieldByNumber(1);
  const google::protobuf::FieldDescriptor* nf = d->FindFieldByNumber(2);
  if (sf == nullptr || nf == nullptr) return std::nullopt;
  return UnpackTimeMessageFields(sub, *sf, *nf, is_timestamp);
}

// Closed set of 9 google.protobuf wrapper FQNs.  Shared between the
// unset-field-null gate in `ReadScalarField` and the peel-the-inner-
// scalar `UnpackWrapperMessage` below (and chained from
// `UnpackAnyToValue` for Any-of-wrapper).  Per langdef line 484-486
// the unset-wrapper-field-evaluates-to-null exception applies
// regardless of proto syntax (proto2 and proto3 agree).
bool IsWrapperFqn(absl::string_view fqn) {
  return fqn == "google.protobuf.BoolValue" ||
         fqn == "google.protobuf.Int32Value" ||
         fqn == "google.protobuf.Int64Value" ||
         fqn == "google.protobuf.UInt32Value" ||
         fqn == "google.protobuf.UInt64Value" ||
         fqn == "google.protobuf.FloatValue" ||
         fqn == "google.protobuf.DoubleValue" ||
         fqn == "google.protobuf.StringValue" ||
         fqn == "google.protobuf.BytesValue";
}

// Descriptor-gated wrapper peel for callers with no precomputed
// classification (the Any-unwrap chain, map/list element reads).
std::optional<celwasm::Value> UnpackWrapperMessage(
    const google::protobuf::Message& sub) {
  const google::protobuf::Descriptor* d = sub.GetDescriptor();
  if (d == nullptr) return std::nullopt;
  if (!IsWrapperFqn(d->full_name())) return std::nullopt;
  const google::protobuf::FieldDescriptor* vf = d->FindFieldByNumber(1);
  if (vf == nullptr) return std::nullopt;
  return UnpackWrapperValueField(sub, *vf);
}

// Precompute the read-dispatch classification for a resolved field
// into `out` (which must already carry `out->field == &field`).
// This is the once-per-cache-fill half of the field-read split: all
// the `is_map()` / `is_repeated()` / WKT `full_name()` string
// compares and inner-sub-field `FindFieldByNumber` walks happen
// here; the per-read half (`ReadFieldClassified` below) dispatches
// on the resulting enum only.
void ClassifyResolvedField(const google::protobuf::FieldDescriptor& field,
                           ResolvedFieldCache* absl_nonnull out) {
  using RC = ProtoFieldReadClass;
  // `is_map()` first — every map field is also `is_repeated()` per
  // descriptor.proto.
  if (field.is_map()) {
    out->read_class = RC::kMap;
    return;
  }
  if (field.is_repeated()) {
    out->read_class = RC::kRepeated;
    return;
  }
  if (field.cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    out->read_class = RC::kScalar;
    return;
  }
  const google::protobuf::Descriptor* mt = field.message_type();
  if (mt == nullptr) {  // defensive — CPPTYPE_MESSAGE implies non-null
    out->read_class = RC::kMessagePlain;
    return;
  }
  const absl::string_view fqn = mt->full_name();
  if (IsWrapperFqn(fqn)) {
    out->read_class = RC::kMessageWrapper;
    out->sub_field1 = mt->FindFieldByNumber(1);  // inner `value`
    return;
  }
  if (fqn == "google.protobuf.Any") {
    out->read_class = RC::kMessageAny;
    return;
  }
  if (fqn == "google.protobuf.Timestamp" || fqn == "google.protobuf.Duration") {
    out->read_class = fqn == "google.protobuf.Timestamp" ? RC::kMessageTimestamp
                                                         : RC::kMessageDuration;
    out->sub_field1 = mt->FindFieldByNumber(1);  // seconds
    out->sub_field2 = mt->FindFieldByNumber(2);  // nanos
    return;
  }
  if (fqn == "google.protobuf.Value" || fqn == "google.protobuf.Struct" ||
      fqn == "google.protobuf.ListValue") {
    out->read_class = RC::kMessageJson;
    return;
  }
  out->read_class = RC::kMessagePlain;
}

absl::StatusOr<celwasm::Value> ReadClassifiedMessageField(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const ResolvedFieldCache& c) {
  using RC = ProtoFieldReadClass;
  switch (c.read_class) {
    case RC::kMessageWrapper:
      return ReadWrapperMessageArm(refl, msg, field, c);
    case RC::kMessageAny:
      return ReadAnyMessageArm(refl, msg, field);
    case RC::kMessageTimestamp:
    case RC::kMessageDuration:
      return ReadTimeMessageArm(
          refl, msg, field, c,
          /*is_timestamp=*/c.read_class == RC::kMessageTimestamp);
    case RC::kMessageJson: {
      const google::protobuf::Message& sub = refl.GetMessage(msg, &field);
      if (auto v = UnpackJsonValueMessage(sub); v.has_value()) {
        return *std::move(v);
      }
      return celwasm::Value::HostMessage(std::make_shared<ProtoBacking>(&sub));
    }
    case RC::kMessagePlain: {
      const google::protobuf::Message& sub = refl.GetMessage(msg, &field);
      return celwasm::Value::HostMessage(std::make_shared<ProtoBacking>(&sub));
    }
    case RC::kScalar:
    case RC::kMap:
    case RC::kRepeated:
      break;  // unreachable — caller dispatched those before reflection
  }
  ABSL_CHECK(false) << "ReadClassifiedMessageField: non-message read class "
                    << static_cast<int>(c.read_class) << " on field `"
                    << field.name() << "`";
  return absl::InternalError("unreachable");
}

// Resolve the FieldDescriptor on a message preferring the wire field
// number when non-zero; falling back to a by-name lookup.  The
// fallback is the forward-compat path for non-proto backings (JSON /
// map) that emit `field_number = 0`.
const google::protobuf::FieldDescriptor* absl_nullable ResolveFieldDescriptor(
    const google::protobuf::Message& msg, int field_number,
    absl::string_view field_name) {
  const google::protobuf::Descriptor* d = msg.GetDescriptor();
  if (d == nullptr) return nullptr;
  if (field_number != 0) {
    const google::protobuf::FieldDescriptor* f =
        d->FindFieldByNumber(field_number);
    if (f != nullptr) return f;
    // proto2 extension fields aren't direct fields of the containing
    // message — `FindFieldByNumber` won't find them, so fall through
    // to the by-name extension lookup below.
  }
  const std::string name_str(field_name);
  if (const google::protobuf::FieldDescriptor* f = d->FindFieldByName(name_str);
      f != nullptr) {
    return f;
  }
  // Extension fields are addressed by their fully-qualified name
  // (e.g. `cel.expr.conformance.proto2.int32_ext`) in CEL.  Mirrors
  // cel-cpp's `proto_message_type_adapter.cc::GetFieldImpl` (lines
  // 176-180): when the by-name lookup misses, try reflection's
  // known-extension table.  Requires the extension descriptor to be
  // linked in (the conformance harness pulls in
  // `test_all_types_extensions.pb.h` for the side-effect of
  // registering it on the generated pool).
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) return nullptr;
  return refl->FindKnownExtensionByName(name_str);
}

// has(msg.field) on an already-resolved descriptor — the shared tail
// of `ProtoBacking::HasField` and the cached trampoline path.
bool ProtoHasFieldResolved(const google::protobuf::Message& msg,
                           const google::protobuf::FieldDescriptor& field) {
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) return false;
  if (field.is_repeated()) {
    return refl->FieldSize(msg, &field) > 0;
  }
  // Singular field: proto2 uses explicit presence (HasField
  // returns true iff the bit is set); proto3 implicit-presence
  // scalars report HasField based on the default-value comparison.
  // Reflection's HasField handles both cases correctly.
  return refl->HasField(msg, &field);
}

// Public re-export of the anonymous-namespace Any unpacker so
// `Instance::Eval` (instance.cc) can auto-unpack a top-level
// `google.protobuf.Any` result.  The body lives inside the
// anonymous namespace because it threads through
// `UnpackOneAnyLayer` and `MaybeUnpackWktMessage`, both of which
// are file-private helpers; this two-line wrapper is the
// dependency injection.  Header: `cel_host.h`.
celwasm::Value UnpackAnyToValue(const google::protobuf::Message& any,
                                const google::protobuf::DescriptorPool* pool) {
  return UnpackAnyToValueAnonImpl(any, pool);
}

// ══════════════════════════════════════════════════════════════════
// OwnedProtoBacking — Layer 1 over an owned `unique_ptr<Message>`.
// ══════════════════════════════════════════════════════════════════
//
// Constructed by `CelMakeMessageImpl` for proto literals built inside
// the wasm module; owns the heap-allocated default-proto so the
// `ExternrefTable::Reset()` between Evals frees it.  Reads delegate
// to a composed `ProtoBacking` over the owned message — same
// reflection path used for host-bound messages, no duplicated logic.

OwnedProtoBacking::OwnedProtoBacking(
    std::unique_ptr<google::protobuf::Message> msg)
    : msg_(std::move(msg)), inner_(msg_.get()) {
  ABSL_CHECK(msg_ != nullptr) << "OwnedProtoBacking: null message";
}

absl::StatusOr<celwasm::Value> OwnedProtoBacking::ReadField(
    int field_number, absl::string_view field_name,
    const celwasm::CelType& expected_type) const {
  return inner_.ReadField(field_number, field_name, expected_type);
}

bool OwnedProtoBacking::HasField(int field_number,
                                 absl::string_view field_name) const {
  return inner_.HasField(field_number, field_name);
}

// ══════════════════════════════════════════════════════════════════
// ProtoBacking — Layer 1 over google::protobuf::Message.
// ══════════════════════════════════════════════════════════════════

absl::StatusOr<celwasm::Value> ProtoBacking::ReadField(
    int field_number, absl::string_view field_name,
    const celwasm::CelType& /*expected_type*/) const {
  ABSL_CHECK(msg_ != nullptr) << "ProtoBacking::ReadField: null message";
  const google::protobuf::FieldDescriptor* field =
      ResolveFieldDescriptor(*msg_, field_number, field_name);
  if (field == nullptr) return FieldNotFound(field_name);
  // Classify-on-the-fly: this virtual entry point has no per-site
  // cache slot (the cached path lives in `CelGetFieldImpl`, keyed by
  // the access site's `FieldRefEntry`), so the classification cost
  // is paid per call here — same behaviour, one shared dispatch.
  ResolvedFieldCache local;
  local.field = field;
  ClassifyResolvedField(*field, &local);
  return ReadFieldClassified(*msg_, *field, local);
}

bool ProtoBacking::HasField(int field_number,
                            absl::string_view field_name) const {
  ABSL_CHECK(msg_ != nullptr) << "ProtoBacking::HasField: null message";
  const google::protobuf::FieldDescriptor* field =
      ResolveFieldDescriptor(*msg_, field_number, field_name);
  if (field == nullptr) return false;
  return ProtoHasFieldResolved(*msg_, *field);
}

// ══════════════════════════════════════════════════════════════════
// HostMap — vector-backed `HostMapBacking` for user bindings.
// ══════════════════════════════════════════════════════════════════

// File-scope helpers shared by HostMap (Layer-1, vector-backed) and
// ProtoMap (reflection-backed).  TU-internal via `static` so the
// symbols don't escape this translation unit.

// langdef §"Equality" / §"Map keys": cross-type numeric equality
// (int ≡ uint by mathematical value; negative int never equals any
// uint), structural same-kind for bool / string.  Mirrors
// `cel_map_key_eq` in `runtime/cel_compare.c` so host- and arena-
// built maps lookup identically.  There is no double arm because
// `DecodeKey` admits only bool / int / uint / string; the int↔uint
// comparison below is exact, which is what the map-key rule requires.  Returns OkStatus on legal compares;
// returns false for any non-key-kind operand (caller should already
// have rejected; this is defence-in-depth).
static bool MapKeysEqual(const celwasm::Value& a, const celwasm::Value& b) {
  using K = celwasm::Value::Kind;
  const K ka = a.kind();
  const K kb = b.kind();
  if (ka == K::kInt && kb == K::kInt) {
    return *a.AsInt() == *b.AsInt();
  }
  if (ka == K::kUint && kb == K::kUint) {
    return *a.AsUint() == *b.AsUint();
  }
  if (ka == K::kInt && kb == K::kUint) {
    int64_t ai = *a.AsInt();
    return ai >= 0 && static_cast<uint64_t>(ai) == *b.AsUint();
  }
  if (ka == K::kUint && kb == K::kInt) {
    int64_t bi = *b.AsInt();
    return bi >= 0 && *a.AsUint() == static_cast<uint64_t>(bi);
  }
  if (ka == K::kBool && kb == K::kBool) {
    return *a.AsBool() == *b.AsBool();
  }
  if (ka == K::kString && kb == K::kString) {
    return *a.AsString() == *b.AsString();
  }
  return false;
}

// Convenience for Get/ContainsKey: caller didn't pre-validate the
// key's kind; emit a kTypeMismatch error value if it's not a legal
// map key.  Mirrors the runtime's `is_valid_map_key_kind` gate.
static bool IsValidMapKeyKind(celwasm::Value::Kind k) {
  return k == celwasm::Value::Kind::kBool || k == celwasm::Value::Kind::kInt ||
         k == celwasm::Value::Kind::kUint || k == celwasm::Value::Kind::kString;
}

// `KeyTypeMismatch` / `NoSuchKey` moved to cel_host_error.cc (M11 Slice E).

HostMap::HostMap(std::vector<std::pair<celwasm::Value, celwasm::Value>> entries)
    : entries_(std::move(entries)) {}

size_t HostMap::Size() const {
  return entries_.size();
}

absl::StatusOr<celwasm::Value> HostMap::Get(
    const celwasm::Value& key,
    const celwasm::CelType& /*expected_value_type*/) const {
  if (!IsValidMapKeyKind(key.kind())) {
    return KeyTypeMismatch();
  }
  for (const auto& [k, v] : entries_) {
    if (MapKeysEqual(k, key)) return v;
  }
  return NoSuchKey();
}

bool HostMap::ContainsKey(const celwasm::Value& key) const {
  if (!IsValidMapKeyKind(key.kind())) return false;
  return std::any_of(entries_.begin(), entries_.end(), [&](const auto& kv) {
    return MapKeysEqual(kv.first, key);
  });
}

void HostMap::ForEach(
    absl::FunctionRef<void(const celwasm::Value&, const celwasm::Value&)> visit)
    const {
  for (const auto& [k, v] : entries_) {
    visit(k, v);
  }
}

// ══════════════════════════════════════════════════════════════════
// ProtoMap — proto-reflection over a single map field.
//
// Proto map fields serialise on the wire as `repeated MapEntry`,
// where MapEntry is a synthesized message with fields
// `key=1, value=2` matching the user-declared key/value types.
// `Reflection::FieldSize` + `GetRepeatedMessage(*owner_, field_, i)`
// walk the entries; each entry's reflection lets us read the key
// and value sub-fields with the existing `ReadScalarField` helper.
//
// Linear scan on lookup mirrors the wasm-side `cel_map_lookup_arena`
// semantics — host- and arena-built maps must agree under langdef
// map-key equality so user-visible behaviour is identical.
// ══════════════════════════════════════════════════════════════════

// FieldDescriptor for the synthetic key/value sub-field on a proto
// map's entry message type.  Pinned via field number (1=key, 2=value)
// per descriptor.proto.
const google::protobuf::FieldDescriptor* absl_nonnull MapEntryField(
    const google::protobuf::FieldDescriptor& field, int number) {
  const google::protobuf::Descriptor* entry = field.message_type();
  ABSL_CHECK(entry != nullptr)
      << "ProtoMap: map field `" << field.name()
      << "` has no entry message_type — invariant violation";
  const google::protobuf::FieldDescriptor* sub =
      entry->FindFieldByNumber(number);
  ABSL_CHECK(sub != nullptr)
      << "ProtoMap: entry of `" << field.name() << "` missing field " << number;
  return sub;
}

ProtoMap::ProtoMap(const google::protobuf::Message* absl_nonnull owner,
                   const google::protobuf::FieldDescriptor* absl_nonnull field)
    : owner_(owner), field_(field) {
  ABSL_CHECK(field->is_map())
      << "ProtoMap: field `" << field->name() << "` is not a map";
}

size_t ProtoMap::Size() const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  ABSL_CHECK(refl != nullptr) << "ProtoMap::Size: no reflection";
  return static_cast<size_t>(refl->FieldSize(*owner_, field_));
}

absl::StatusOr<celwasm::Value> ProtoMap::Get(
    const celwasm::Value& key,
    const celwasm::CelType& /*expected_value_type*/) const {
  if (!IsValidMapKeyKind(key.kind())) {
    return KeyTypeMismatch();
  }
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  if (refl == nullptr) {
    return absl::InternalError("ProtoMap::Get: no reflection");
  }
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(*field_, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(*field_, 2);
  const int n = refl->FieldSize(*owner_, field_);
  for (int i = 0; i < n; ++i) {
    const google::protobuf::Message& entry =
        refl->GetRepeatedMessage(*owner_, field_, i);
    auto k_or = ReadScalarField(entry, *key_fd);
    if (!k_or.ok()) return k_or.status();
    if (MapKeysEqual(*k_or, key)) {
      return ReadScalarField(entry, *val_fd);
    }
  }
  return NoSuchKey();
}

bool ProtoMap::ContainsKey(const celwasm::Value& key) const {
  if (!IsValidMapKeyKind(key.kind())) return false;
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  if (refl == nullptr) return false;
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(*field_, 1);
  const int n = refl->FieldSize(*owner_, field_);
  for (int i = 0; i < n; ++i) {
    const google::protobuf::Message& entry =
        refl->GetRepeatedMessage(*owner_, field_, i);
    auto k_or = ReadScalarField(entry, *key_fd);
    if (!k_or.ok()) return false;
    if (MapKeysEqual(*k_or, key)) return true;
  }
  return false;
}

void ProtoMap::ForEach(
    absl::FunctionRef<void(const celwasm::Value&, const celwasm::Value&)> visit)
    const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  ABSL_CHECK(refl != nullptr) << "ProtoMap::ForEach: no reflection";
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(*field_, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(*field_, 2);
  const int n = refl->FieldSize(*owner_, field_);
  for (int i = 0; i < n; ++i) {
    const google::protobuf::Message& entry =
        refl->GetRepeatedMessage(*owner_, field_, i);
    auto k_or = ReadScalarField(entry, *key_fd);
    auto v_or = ReadScalarField(entry, *val_fd);
    if (!k_or.ok() || !v_or.ok()) continue;
    visit(*k_or, *v_or);
  }
}

// ══════════════════════════════════════════════════════════════════
// HostList — vector-backed `HostListBacking` for user bindings.
// ══════════════════════════════════════════════════════════════════

// `IndexOutOfBounds` moved to cel_host_error.cc (M11 Slice E).

HostList::HostList(std::vector<celwasm::Value> elements)
    : elements_(std::move(elements)) {}

size_t HostList::Size() const {
  return elements_.size();
}

absl::StatusOr<celwasm::Value> HostList::At(
    size_t index, const celwasm::CelType& /*expected_element_type*/) const {
  if (index >= elements_.size()) {
    return IndexOutOfBounds(index, elements_.size());
  }
  return elements_[index];
}

void HostList::ForEach(
    absl::FunctionRef<void(const celwasm::Value&)> visit) const {
  for (const celwasm::Value& v : elements_) {
    visit(v);
  }
}

// ══════════════════════════════════════════════════════════════════
// ProtoList — proto reflection over a single REPEATED (non-map)
// field.  Element reads delegate to `ReadScalarField` against a
// synthesized FieldDescriptor view of the i-th element — proto's
// `GetRepeated{Bool,Int32,…}()` family does the type dispatch.
// ══════════════════════════════════════════════════════════════════

namespace {

// Read the i-th element of a REPEATED field of the given cpp_type
// into a celwasm::Value.  Mirrors `ReadNumericField` + the
// string/bytes/message branches of `ReadScalarField`, but reads the
// repeated-element accessors instead of the singular ones.  Returns
// non-OK Status on infrastructure failure (no reflection); the
// caller surfaces spec-level errors as Value::Error.
absl::StatusOr<celwasm::Value> ReadRepeatedElement(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, int i) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      return celwasm::Value::Bool(refl.GetRepeatedBool(msg, &field, i));
    case FD::CPPTYPE_INT32:
      return celwasm::Value::Int(refl.GetRepeatedInt32(msg, &field, i));
    case FD::CPPTYPE_INT64:
      return celwasm::Value::Int(refl.GetRepeatedInt64(msg, &field, i));
    case FD::CPPTYPE_UINT32:
      return celwasm::Value::Uint(refl.GetRepeatedUInt32(msg, &field, i));
    case FD::CPPTYPE_UINT64:
      return celwasm::Value::Uint(refl.GetRepeatedUInt64(msg, &field, i));
    case FD::CPPTYPE_FLOAT:
      return celwasm::Value::Double(refl.GetRepeatedFloat(msg, &field, i));
    case FD::CPPTYPE_DOUBLE:
      return celwasm::Value::Double(refl.GetRepeatedDouble(msg, &field, i));
    case FD::CPPTYPE_ENUM:
      return celwasm::Value::Int(refl.GetRepeatedEnumValue(msg, &field, i));
    case FD::CPPTYPE_STRING: {
      std::string scratch;
      const std::string& s =
          refl.GetRepeatedStringReference(msg, &field, i, &scratch);
      const bool is_bytes = field.type() == FD::TYPE_BYTES;
      if (&s == &scratch) {
        // Bytes were materialised into the scratch — hand over
        // ownership (same contract as `ReadScalarField`).
        return is_bytes ? celwasm::Value::Bytes(std::move(scratch))
                        : celwasm::Value::String(std::move(scratch));
      }
      // View into the anchored message's element storage — see the
      // `ReadScalarField` string arm for the lifetime argument.
      return is_bytes ? celwasm::Value::BytesView(s)
                      : celwasm::Value::StringView(s);
    }
    case FD::CPPTYPE_MESSAGE: {
      const google::protobuf::Message& sub =
          refl.GetRepeatedMessage(msg, &field, i);
      // Mirror `ReadSingularMessageField`'s WKT peel chain: Any-unwrap,
      // Timestamp/Duration, wrapper, and JSON-Value peels apply to
      // repeated message elements too.
      const google::protobuf::Descriptor* mt = field.message_type();
      if (mt != nullptr && mt->full_name() == "google.protobuf.Any") {
        return UnpackAnyToValue(sub, mt->file()->pool());
      }
      if (auto v = MaybeUnpackWktMessage(sub); v.has_value()) {
        return *std::move(v);
      }
      return celwasm::Value::HostMessage(std::make_shared<ProtoBacking>(&sub));
    }
  }
  return absl::InternalError(absl::StrCat("ProtoList::At: unhandled cpp_type ",
                                          static_cast<int>(field.cpp_type()),
                                          " on field `", field.name(), "`"));
}

}  // namespace

ProtoList::ProtoList(
    const google::protobuf::Message* absl_nonnull owner,
    const google::protobuf::FieldDescriptor* absl_nonnull field)
    : owner_(owner), field_(field) {
  ABSL_CHECK(field->is_repeated())
      << "ProtoList: field `" << field->name() << "` is not repeated";
  ABSL_CHECK(!field->is_map()) << "ProtoList: field `" << field->name()
                               << "` is a map; use ProtoMap instead";
}

size_t ProtoList::Size() const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  ABSL_CHECK(refl != nullptr) << "ProtoList::Size: no reflection";
  return static_cast<size_t>(refl->FieldSize(*owner_, field_));
}

absl::StatusOr<celwasm::Value> ProtoList::At(
    size_t index, const celwasm::CelType& /*expected_element_type*/) const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  if (refl == nullptr) {
    return absl::InternalError("ProtoList::At: no reflection");
  }
  const auto count = static_cast<size_t>(refl->FieldSize(*owner_, field_));
  if (index >= count) {
    return IndexOutOfBounds(index, count);
  }
  return ReadRepeatedElement(*refl, *owner_, *field_, static_cast<int>(index));
}

void ProtoList::ForEach(
    absl::FunctionRef<void(const celwasm::Value&)> visit) const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  ABSL_CHECK(refl != nullptr) << "ProtoList::ForEach: no reflection";
  const int n = refl->FieldSize(*owner_, field_);
  for (int i = 0; i < n; ++i) {
    auto v_or = ReadRepeatedElement(*refl, *owner_, *field_, i);
    if (v_or.ok()) visit(*v_or);
  }
}

// ══════════════════════════════════════════════════════════════════
// celwasm::Value::Message(const google::protobuf::Message&)
// celwasm::Value::Map(...) / celwasm::Value::HostMap(...)
//
// Defined in this TU — not in value.cc — so value.cc doesn't need
// to know about ProtoBacking / HostMap.  The dependency is one-way:
// cel_host depends on value; value never depends on cel_host (only
// forward-declares the abstract bases for the shared_ptr slots in
// its variant).
// ══════════════════════════════════════════════════════════════════

Value Value::Message(const google::protobuf::Message& m) {
  return Value::HostMessage(std::make_shared<celwasm::ProtoBacking>(&m));
}

Value Value::OwnedMessage(std::unique_ptr<google::protobuf::Message> m) {
  ABSL_CHECK(m != nullptr) << "Value::OwnedMessage: message must not be null";
  return Value::HostMessage(
      std::make_shared<celwasm::OwnedProtoBacking>(std::move(m)));
}

Value Value::Map(std::vector<std::pair<Value, Value>> entries) {
  return Value::HostMap(std::make_shared<celwasm::HostMap>(std::move(entries)));
}

Value Value::HostMap(std::shared_ptr<celwasm::HostMapBacking> backing) {
  ABSL_CHECK(backing != nullptr) << "Value::HostMap: backing must not be null";
  Value r;
  r.kind_ = Kind::kMap;
  r.payload_ = std::move(backing);
  return r;
}

Value Value::List(std::vector<Value> elements) {
  return Value::HostList(
      std::make_shared<celwasm::HostList>(std::move(elements)));
}

Value Value::HostList(std::shared_ptr<celwasm::HostListBacking> backing) {
  ABSL_CHECK(backing != nullptr) << "Value::HostList: backing must not be null";
  Value r;
  r.kind_ = Kind::kList;
  r.payload_ = std::move(backing);
  return r;
}

}  // namespace celwasm
