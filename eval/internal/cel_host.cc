#include "eval/internal/cel_host.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "eval/error.h"
#include "eval/internal/cel_host_error.h"  // M11 Slice E
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_legacy.h"
#include "google/protobuf/message.h"
#include "google/protobuf/util/message_differencer.h"
#include "shared/type.h"

namespace celwasm {

namespace {

// Forward declarations for the WKT-peel helpers used by both
// `UnpackAnyToValue` (Any-of-WKT chain) and `ReadScalarField`
// (singular CPPTYPE_MESSAGE arm).  Bodies are defined below in
// this anonymous namespace alongside the other peelers.
//
// M11 Slice E: the simple error-CelValue factories (`FieldNotFound`,
// `MakeError`, `KeyTypeMismatch`, `NoSuchKey`, `IndexOutOfBounds`)
// and the wire-format error encoders (`WriteWireError`,
// `WriteWireBool`, `WriteWireInt`, `WriteInvalidArgumentError`,
// `PoisonCelValue`) plus the 3VL absorbers (`AbsorbUnary`,
// `AbsorbBinary`) moved out to `cel_host_error.{cc,h}` — they're
// included via `<eval/internal/cel_host_error.h>` above
// and remain callable here under the `celwasm::` namespace.
std::optional<celwasm::Value> UnpackWrapperMessage(
    const google::protobuf::Message& sub);
std::optional<celwasm::Value> UnpackWellKnownTimeMessage(
    const google::protobuf::Message& sub);
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

}  // namespace

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
// `map_keys_equal` in `runtime/cel_runtime.c` so host- and arena-
// built maps lookup identically.  Returns OkStatus on legal compares;
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
// CelMapLookupImpl — Layer 2 entry for `cel_host.cel_map_lookup`.
// Reads map_slot's ref_slot, dereferences to a HostMapBacking,
// decodes key, calls Get(), marshals result back.
// ══════════════════════════════════════════════════════════════════

namespace {

// Decode a scalar CelValue into a celwasm::Value.  Map-key kinds only
// (bool/int/uint/string) — every other kind returns nullopt and the
// caller surfaces a TYPE_MISMATCH error to the wasm side.  Strings
// stay a non-owning view over linear memory: the shared-memory base
// pointer is stable (wasmtime shared memories never remap on grow)
// and the key Value is consumed by `Get` / `ContainsKey` before the
// trampoline returns — no backing stores it past the call.
std::optional<celwasm::Value> DecodeKey(const CelValue& cv,
                                        const MemoryView& mem) {
  switch (cv.kind) {
    case CEL_BOOL:
      return celwasm::Value::Bool(cv.payload.b != 0);
    case CEL_INT:
      return celwasm::Value::Int(cv.payload.i);
    case CEL_UINT:
      return celwasm::Value::Uint(cv.payload.u);
    case CEL_STRING:
      return celwasm::Value::StringView(
          mem.ReadSpan(cv.payload.s.ptr, cv.payload.s.len));
    default:
      return std::nullopt;
  }
}

// `WireErrorCode` / `WriteWireError` moved to cel_host_error.cc (M11 Slice E).

// Encode the (string|bytes) span via the per-eval ArenaAllocator.
absl::Status EncodeSpan(const celwasm::Value& v, CelValue* out,
                        ArenaAllocator& alloc) {
  using K = celwasm::Value::Kind;
  absl::string_view s = v.kind() == K::kString ? *v.AsString() : *v.AsBytes();
  uint32_t off = 0;
  uint8_t* p = alloc.Alloc(s.size(), &off);
  if (p == nullptr && !s.empty()) {
    return absl::ResourceExhaustedError("arena OOM in CelMapLookupImpl");
  }
  if (p != nullptr && !s.empty()) std::memcpy(p, s.data(), s.size());
  out->kind = v.kind() == K::kString ? CEL_STRING : CEL_BYTES;
  out->payload.s.ptr = off;
  out->payload.s.len = static_cast<uint32_t>(s.size());
  return absl::OkStatus();
}

// m7b §3.1 / Probe D — `DecomposeAbslDuration` lives in
// `cel_host.h` (header-inline) so instance.cc shares it.

absl::Status EncodeDurationValue(const celwasm::Value& v, CelValue* out) {
  auto d_or = v.AsDuration();
  if (!d_or.ok()) return d_or.status();
  out->kind = CEL_DURATION;
  DecomposeAbslDuration(*d_or, &out->payload.dur);
  return absl::OkStatus();
}

absl::Status EncodeTimestampValue(const celwasm::Value& v, CelValue* out) {
  auto t_or = v.AsTimestamp();
  if (!t_or.ok()) return t_or.status();
  out->kind = CEL_TIMESTAMP;
  DecomposeAbslDuration(*t_or - absl::UnixEpoch(), &out->payload.ts);
  return absl::OkStatus();
}

// Encode a celwasm::Value into a CelValue, allocating string/bytes
// payloads through the per-eval ArenaAllocator.  Returns non-OK
// Status on infrastructure failure (arena OOM); spec-level errors
// inside the input Value already encode as `{kind:CEL_ERROR, err:…}`.
// Aborts for a Value kind the inline encoder must never see — the
// caller violated the Layer-1/Layer-2 contract.  Never returns.
[[noreturn]] void EncodeValueUnreachable(const celwasm::Value& v,
                                         const char* reason) {
  ABSL_CHECK(false) << "EncodeValue: " << reason << " (kind "
                    << static_cast<int>(v.kind()) << ")";
}

absl::Status EncodeValue(const celwasm::Value& v, CelValue* out,
                         ArenaAllocator& alloc) {
  using K = celwasm::Value::Kind;
  switch (v.kind()) {
    case K::kNull:
      out->kind = CEL_NULL;
      return absl::OkStatus();
    case K::kBool:
      out->kind = CEL_BOOL;
      out->payload.b = *v.AsBool() ? 1 : 0;
      return absl::OkStatus();
    case K::kInt:
      out->kind = CEL_INT;
      out->payload.i = *v.AsInt();
      return absl::OkStatus();
    case K::kUint:
      out->kind = CEL_UINT;
      out->payload.u = *v.AsUint();
      return absl::OkStatus();
    case K::kDouble:
      out->kind = CEL_DOUBLE;
      out->payload.d = *v.AsDouble();
      return absl::OkStatus();
    case K::kString:
    case K::kBytes:
      return EncodeSpan(v, out, alloc);
    case K::kError: {
      const celwasm::ErrorPayload* e = *v.ErrorInfo();
      out->kind = CEL_ERROR;
      out->payload.err = WireErrorCode(e->code);
      return absl::OkStatus();
    }
    case K::kUnknown:
      // Backings don't return unknowns — operand-pair propagation runs
      // before this encoder; PartialEval uses MatchesAnyUnknownPattern.
      EncodeValueUnreachable(v, "kUnknown is unreachable from Layer-1 returns");
    case K::kMessage:
    case K::kMap:
    case K::kList:
      // Aggregate kinds route through EncodeFieldResult /
      // EncodeAggregateIfAny, never the inline path.
      EncodeValueUnreachable(
          v, "aggregate kind must route through EncodeFieldResult");
    case K::kDuration:
      return EncodeDurationValue(v, out);
    case K::kTimestamp:
      return EncodeTimestampValue(v, out);
    case K::kType:
      // type(x) lowers to the cel_type_of_at_v runtime helper, never a
      // Layer-1 backing call (activation side: instance.cc::EncodeType).
      EncodeValueUnreachable(v, "kType is unreachable from Layer-1 backings");
  }
  EncodeValueUnreachable(v, "unhandled kind");
}

// Encode a Layer-1 aggregate (message / map / list) by interning
// the backing into the matching externref namespace and writing the
// resulting ref_slot.  Returns true if `v` is an aggregate kind and
// has been encoded; false if `v` is a scalar (caller falls through
// to the inline EncodeValue path).
absl::StatusOr<bool> EncodeAggregateIfAny(const celwasm::Value& v,
                                          uint32_t out_slot,
                                          const TrampolineContext& ctx) {
  using K = celwasm::Value::Kind;
  CelValue cv{};
  if (v.kind() == K::kMessage) {
    auto sub_or = v.SharedMessageBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = ctx.refs.Intern(*std::move(sub_or));
  } else if (v.kind() == K::kMap) {
    auto sub_or = v.SharedMapBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_MAP_HOST;
    cv.payload.ref_slot = ctx.refs.InternMap(*std::move(sub_or));
  } else if (v.kind() == K::kList) {
    auto sub_or = v.SharedListBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_LIST_HOST;
    cv.payload.ref_slot = ctx.refs.InternList(*std::move(sub_or));
  } else {
    return false;
  }
  ctx.mem.WriteCelValue(out_slot, cv);
  return true;
}

// Marshal a `celwasm::Value` returned by Layer 1 (ReadField / At / Get)
// into the 24-byte CelValue at `out_slot`.  Scalars + null + error
// encode inline / via arena (`EncodeValue`); aggregate kinds intern
// via `EncodeAggregateIfAny`.  Used by every Layer-2 trampoline so
// the wire shape is consistent across surfaces.
absl::Status EncodeFieldResult(const celwasm::Value& v, uint32_t out_slot,
                               const TrampolineContext& ctx) {
  auto encoded_or = EncodeAggregateIfAny(v, out_slot, ctx);
  if (!encoded_or.ok()) return encoded_or.status();
  if (*encoded_or) return absl::OkStatus();
  CelValue cv{};
  if (auto s = EncodeValue(v, &cv, ctx.alloc); !s.ok()) return s;
  ctx.mem.WriteCelValue(out_slot, cv);
  return absl::OkStatus();
}

}  // namespace

absl::Status EncodeValueToSlot(const celwasm::Value& v, uint32_t out_slot,
                               MemoryView& mem, ExternrefTable& refs,
                               ArenaAllocator& alloc) {
  // EncodeFieldResult only ever touches ctx.{mem,refs,alloc}; the
  // bindings span is unused on the encode path, so an empty one is safe.
  const CelHostBindings empty_bindings;
  const TrampolineContext ctx{empty_bindings, mem, refs, alloc};
  return EncodeFieldResult(v, out_slot, ctx);
}

namespace {

// Decodes a list-index CelValue to an int64 per langdef §"Indexing":
// the index is int, or — when dyn-typed (e.g. `[1,2,3][dyn(0.0)]`) — a
// CEL_UINT or an integral CEL_DOUBLE, mirroring `cel_list_at_arena` in
// runtime/cel_runtime.c (pinned by oracle `ListIndex{Double,Uint}Agrees`
// and conformance `lists/index/zero_based_{double,uint}`).  On a
// malformed index writes the wire error to `out_slot` and returns
// nullopt; the returned int64 may still be negative (the caller bounds-
// checks it against the backing).
std::optional<int64_t> DecodeListIndex(const CelValue& idx_cv,
                                       uint32_t out_slot,
                                       const TrampolineContext& ctx) {
  if (idx_cv.kind == CEL_INT) {
    return idx_cv.payload.i;
  }
  if (idx_cv.kind == CEL_UINT) {
    if (idx_cv.payload.u > static_cast<uint64_t>(INT64_MAX)) {
      WriteWireError(CEL_ERR_INDEX_OUT_OF_BOUNDS, out_slot, ctx.mem);
      return std::nullopt;
    }
    return static_cast<int64_t>(idx_cv.payload.u);
  }
  if (idx_cv.kind == CEL_DOUBLE) {
    const double d = idx_cv.payload.d;
    if (!std::isfinite(d) || d > 9.2233720368547758e18 ||
        d < -9.2233720368547758e18) {
      WriteWireError(CEL_ERR_INVALID_ARGUMENT, out_slot, ctx.mem);
      return std::nullopt;
    }
    const auto trunc = static_cast<int64_t>(d);
    if (static_cast<double>(trunc) != d) {  // non-integral
      WriteWireError(CEL_ERR_INVALID_ARGUMENT, out_slot, ctx.mem);
      return std::nullopt;
    }
    return trunc;
  }
  WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
  return std::nullopt;
}

// Materializes a non-empty host list into a fresh arena list at
// `out_slot` (16-byte ArenaListHeader + count*24B element run, two
// arena allocs mirroring `cel_list_create`), snapshotting each element
// via `At` + `EncodeFieldResult`.  Returns false on arena OOM so the
// caller can fall back to an empty list; element-read failures
// propagate as a status.
absl::StatusOr<bool> SnapshotHostListToArena(const HostListBacking& backing,
                                             size_t count, uint32_t out_slot,
                                             const TrampolineContext& ctx) {
  constexpr uint32_t kHeaderBytes = 16u;
  constexpr auto kElemBytes = static_cast<uint32_t>(sizeof(CelValue));
  uint32_t header_off = 0;
  if (ctx.alloc.Alloc(kHeaderBytes, &header_off) == nullptr ||
      header_off == 0) {
    return false;
  }
  uint32_t elements_off = 0;
  const uint32_t elements_bytes = static_cast<uint32_t>(count) * kElemBytes;
  if (ctx.alloc.Alloc(elements_bytes, &elements_off) == nullptr ||
      elements_off == 0) {
    return false;
  }
  // Header layout mirrors `ArenaListHeader` (cel_data.h): count,
  // capacity, elements_offset, _pad.
  ctx.mem.WriteU32(header_off + 0u, static_cast<uint32_t>(count));
  ctx.mem.WriteU32(header_off + 4u, static_cast<uint32_t>(count));
  ctx.mem.WriteU32(header_off + 8u, elements_off);
  ctx.mem.WriteU32(header_off + 12u, 0u);
  // `celwasm::CelType::Int()` is informational only (M4: no element-side
  // narrowing); matches the CelListAtImpl call site.
  for (size_t i = 0; i < count; ++i) {
    auto got = backing.At(i, celwasm::CelType::Int());
    if (!got.ok()) return got.status();
    const uint32_t elem_slot =
        elements_off + (static_cast<uint32_t>(i) * kElemBytes);
    if (auto s = EncodeFieldResult(*got, elem_slot, ctx); !s.ok()) return s;
  }
  CelValue synthetic{};
  synthetic.kind = CEL_LIST_ARENA;
  synthetic.payload.arena_list.header_ptr = header_off;
  ctx.mem.WriteCelValue(out_slot, synthetic);
  return true;
}

}  // namespace

absl::Status CelListAtImpl(uint32_t out_slot, uint32_t list_slot,
                           uint32_t index_slot, const TrampolineContext& ctx) {
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  CelValue idx_cv = ctx.mem.ReadCelValue(index_slot);

  // 3VL on operands — same path the runtime dispatcher uses.  Index
  // first so an unknown index propagates even if the list is also
  // poisoned (matches the runtime fast-path order in
  // cel_list_at_arena).
  if (idx_cv.kind == CEL_UNKNOWN || idx_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, idx_cv);
    return absl::OkStatus();
  }
  if (list_cv.kind == CEL_UNKNOWN || list_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, list_cv);
    return absl::OkStatus();
  }

  // Codegen calls into us only on the kHost arm; defence-in-depth.
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  std::optional<int64_t> decoded = DecodeListIndex(idx_cv, out_slot, ctx);
  if (!decoded.has_value()) return absl::OkStatus();
  int64_t i = *decoded;

  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListAtImpl: list ref_slot ", list_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }

  if (i < 0) {
    WriteWireError(CEL_ERR_INDEX_OUT_OF_BOUNDS, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // backing->At returns a Value::Error(kIndexOutOfBounds) when i >=
  // Size; the encoder maps that to CEL_ERR_INDEX_OUT_OF_BOUNDS via
  // WireErrorCode.  Single round-trip, no host-side double-check.
  auto got = backing->At(static_cast<size_t>(i), celwasm::CelType::Int());
  if (!got.ok()) return got.status();
  return EncodeFieldResult(*got, out_slot, ctx);
}

absl::Status CelListIterOpenImpl(uint32_t out_slot, uint32_t list_slot,
                                 const TrampolineContext& ctx) {
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);

  // Allocate a zero-count ArenaListHeader so the comprehension
  // prologue reads count=0 cleanly (its 2-load shape walks the
  // header pointer at `payload+8`, then reads count at `*header+0`
  // — a zero `header_ptr` would dereference into rodata).  Used
  // for non-host sources, empty host lists, and OOM fallback.
  constexpr uint32_t kHeaderBytes = 16u;
  auto write_empty = [&]() -> absl::Status {
    uint32_t header_off = 0;
    if (ctx.alloc.Alloc(kHeaderBytes, &header_off) == nullptr ||
        header_off == 0) {
      return absl::ResourceExhaustedError(
          "CelListIterOpenImpl: arena OOM allocating empty header");
    }
    ctx.mem.WriteU32(header_off + 0u, 0u);   // count
    ctx.mem.WriteU32(header_off + 4u, 0u);   // capacity
    ctx.mem.WriteU32(header_off + 8u, 0u);   // elements_offset
    ctx.mem.WriteU32(header_off + 12u, 0u);  // _pad
    CelValue empty{};
    empty.kind = CEL_LIST_ARENA;
    empty.payload.arena_list.header_ptr = header_off;
    ctx.mem.WriteCelValue(out_slot, empty);
    return absl::OkStatus();
  };

  if (list_cv.kind == CEL_UNKNOWN || list_cv.kind == CEL_ERROR) {
    // The comprehension prologue's range-absorption guard propagates
    // a poisoned iter_range BEFORE cel_list_arena_view can route it
    // here (expr_lower_comprehension.cc EmitRangeAbsorptionGuard).
    // Reaching this arm means the guard regressed; failing the eval
    // loudly beats silently iterating an empty view — that is the
    // empty-range-identity soundness bug (exists→false, all→true,
    // map/filter→[]) this tripwire pins shut.
    return absl::FailedPreconditionError(
        absl::StrCat("CelListIterOpenImpl: iter_range CelValue is ",
                     list_cv.kind == CEL_UNKNOWN ? "CEL_UNKNOWN" : "CEL_ERROR",
                     " — codegen's comprehension range-absorption guard must "
                     "propagate it before the iterate path runs"));
  }
  if (list_cv.kind != CEL_LIST_HOST) {
    // Codegen contract: cel_list_arena_view only routes us for
    // CEL_LIST_HOST sources.  Defence in depth.
    return write_empty();
  }
  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListIterOpenImpl: list ref_slot ",
                     list_cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  const size_t count = backing->Size();
  if (count == 0) {
    return write_empty();
  }
  // Materialize the host list into the arena; OOM falls back to empty.
  auto done = SnapshotHostListToArena(*backing, count, out_slot, ctx);
  if (!done.ok()) return done.status();
  if (!*done) return write_empty();
  return absl::OkStatus();
}

absl::Status CelMapLookupImpl(uint32_t out_slot, uint32_t map_slot,
                              uint32_t key_slot, const TrampolineContext& ctx) {
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  CelValue key_cv = ctx.mem.ReadCelValue(key_slot);

  // 3VL on operands — same path the runtime dispatcher uses.
  if (key_cv.kind == CEL_UNKNOWN || key_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, key_cv);
    return absl::OkStatus();
  }
  if (map_cv.kind == CEL_UNKNOWN || map_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, map_cv);
    return absl::OkStatus();
  }

  // Codegen calls into us only on the kHost arm; defence-in-depth
  // for codegen drift.
  if (map_cv.kind != CEL_MAP_HOST) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }

  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapLookupImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }

  std::optional<celwasm::Value> key = DecodeKey(key_cv, ctx.mem);
  if (!key.has_value()) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }

  // expected_value_type is informational at M3 (no implicit
  // coercion); HostMap ignores it.  Pass an arbitrary scalar — the
  // type catalogue exposes no `Dyn` factory yet, and any choice
  // here is observed only by future backings that opt into typed
  // narrowing.
  auto got = backing->Get(*key, celwasm::CelType::Int());
  if (!got.ok()) return got.status();
  // EncodeFieldResult handles scalar + every aggregate kind
  // uniformly — nested map/list/message values from Get land
  // through the matching externref namespace.
  return EncodeFieldResult(*got, out_slot, ctx);
}

namespace {

// MapIterState field offsets (mirror the struct in cel_runtime.c).
constexpr uint32_t kMapIterKindOff = 0u;
constexpr uint32_t kMapIterCursorOff = 4u;
constexpr uint32_t kMapIterPayloadOff = 8u;
constexpr uint32_t kMapIterCountOff = 12u;
constexpr uint32_t kMapIterHostKind = 1u;  // MAP_ITER_KIND_HOST

// Stamp an empty (count=0) host map-iter state.  The runtime's
// `cel_map_iter_init` reads count and collapses count=0 to the empty
// iter handle.
void WriteEmptyMapIterState(uint32_t state_offset,
                            const TrampolineContext& ctx) {
  ctx.mem.WriteU32(state_offset + kMapIterKindOff, kMapIterHostKind);
  ctx.mem.WriteU32(state_offset + kMapIterCursorOff, 0u);
  ctx.mem.WriteU32(state_offset + kMapIterPayloadOff, 0u);
  ctx.mem.WriteU32(state_offset + kMapIterCountOff, 0u);
}

// Allocate a `count * 48` byte arena snapshot (key 24B immediately
// followed by value 24B — the stride `cel_map_iter_{key,value}_at`
// index by), encode each entry pair via `EncodeFieldResult`, and stamp
// the iter state to point at it.  Returns false on arena OOM so the
// caller can fall back to an empty state.
absl::StatusOr<bool> SnapshotMapEntriesToArena(
    const std::vector<std::pair<celwasm::Value, celwasm::Value>>& entries,
    uint32_t state_offset, const TrampolineContext& ctx) {
  constexpr uint32_t kPerEntry = 2u * sizeof(CelValue);
  const uint32_t snapshot_bytes =
      static_cast<uint32_t>(entries.size()) * kPerEntry;
  uint32_t snapshot_off = 0;
  ctx.alloc.Alloc(snapshot_bytes, &snapshot_off);
  if (snapshot_off == 0) return false;
  for (size_t i = 0; i < entries.size(); ++i) {
    const uint32_t key_off =
        snapshot_off + (static_cast<uint32_t>(i) * kPerEntry);
    const uint32_t val_off = key_off + sizeof(CelValue);
    if (auto s = EncodeFieldResult(entries[i].first, key_off, ctx); !s.ok()) {
      return s;
    }
    if (auto s = EncodeFieldResult(entries[i].second, val_off, ctx); !s.ok()) {
      return s;
    }
  }
  ctx.mem.WriteU32(state_offset + kMapIterKindOff, kMapIterHostKind);
  ctx.mem.WriteU32(state_offset + kMapIterCursorOff, 0u);
  ctx.mem.WriteU32(state_offset + kMapIterPayloadOff, snapshot_off);
  ctx.mem.WriteU32(state_offset + kMapIterCountOff,
                   static_cast<uint32_t>(entries.size()));
  return true;
}

}  // namespace

absl::Status CelMapIterOpenImpl(uint32_t state_offset, uint32_t map_slot,
                                const TrampolineContext& ctx) {
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  if (map_cv.kind == CEL_UNKNOWN || map_cv.kind == CEL_ERROR) {
    // Same tripwire as CelListIterOpenImpl: the comprehension
    // prologue's range-absorption guard propagates poisoned ranges
    // before any iterate path runs; an empty iter here would be the
    // silent empty-range-identity wrong answer.
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapIterOpenImpl: iter_range CelValue is ",
                     map_cv.kind == CEL_UNKNOWN ? "CEL_UNKNOWN" : "CEL_ERROR",
                     " — codegen's comprehension range-absorption guard must "
                     "propagate it before the iterate path runs"));
  }
  if (map_cv.kind != CEL_MAP_HOST) {
    // Codegen contract: cel_map_iter_init only calls us for
    // CEL_MAP_HOST sources.  Defence in depth — leave empty.
    WriteEmptyMapIterState(state_offset, ctx);
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapIterOpenImpl: map ref_slot ",
                     map_cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  // `ForEach` is the only positional-agnostic accessor on
  // `HostMapBacking`; iter callers need by-index lookup, so snapshot the
  // entries once up front (the snapshot also lives in the arena —
  // count*48B — for the iter's lifetime; a streaming variant is future
  // work if huge-host-map comprehensions become hot).
  std::vector<std::pair<celwasm::Value, celwasm::Value>> entries;
  entries.reserve(backing->Size());
  backing->ForEach([&](const celwasm::Value& k, const celwasm::Value& v) {
    entries.emplace_back(k, v);
  });
  if (entries.empty()) {
    WriteEmptyMapIterState(state_offset, ctx);
    return absl::OkStatus();
  }
  auto done = SnapshotMapEntriesToArena(entries, state_offset, ctx);
  if (!done.ok()) return done.status();
  if (!*done) WriteEmptyMapIterState(state_offset, ctx);
  return absl::OkStatus();
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

namespace {

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

}  // namespace

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

namespace {

// Resolve `field_ref_id` against the `bindings.field_refs` table.
// Returns nullptr on OOR / sentinel; caller writes
// CEL_ERR_FIELD_NOT_FOUND.
const FieldRefEntry* absl_nullable ResolveFieldRef(
    const CelHostBindings& bindings, uint32_t field_ref_id) {
  if (field_ref_id == 0) return nullptr;  // sentinel
  if (field_ref_id >= bindings.field_refs.size()) return nullptr;
  return &bindings.field_refs[field_ref_id];
}

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

}  // namespace

absl::Status EncodeUnknownSet(absl::Span<const uint32_t> ids,
                              ArenaAllocator& alloc,
                              CelValue* absl_nonnull out) {
  out->kind = CEL_UNKNOWN;
  if (ids.empty()) {
    out->payload.unk = 0;  // legal empty UnknownSet, no allocation
    return absl::OkStatus();
  }
  // Canonical id-array form: sorted ascending, deduplicated —
  // the same invariant `cel_unknown_merge`'s merge walk relies on.
  std::vector<uint32_t> sorted(ids.begin(), ids.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  // One allocation: descriptor words at [0, 8), ids at [8, ...).
  const size_t bytes = (2 + sorted.size()) * sizeof(uint32_t);
  uint32_t off = 0;
  uint8_t* p = alloc.Alloc(bytes, &off);
  if (p == nullptr) {
    return absl::ResourceExhaustedError(
        "arena OOM minting an UnknownSet descriptor");
  }
  const uint32_t desc[2] = {off + (2 * static_cast<uint32_t>(sizeof(uint32_t))),
                            static_cast<uint32_t>(sorted.size())};
  std::memcpy(p, desc, sizeof(desc));
  std::memcpy(p + sizeof(desc), sorted.data(),
              sorted.size() * sizeof(uint32_t));
  out->payload.unk = off;
  return absl::OkStatus();
}

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

// ══════════════════════════════════════════════════════════════════
// Aggregate-op kHost trampolines.
//
// The seven dispatchers in `cel_runtime.c` (`cel_list_size` /
// `cel_list_in` / `cel_list_eq` / `cel_list_concat` / `cel_map_size`
// / `cel_map_in` / `cel_map_eq`) tail-call here when the operand
// origin is `CEL_LIST_HOST` or `CEL_MAP_HOST`.  Each Impl reads its
// operand backing(s) via `ctx.refs.LookupList` / `LookupMap`, runs
// the corresponding spec-level operation, and writes the result
// CelValue into `out_slot`.  Element/value equality reuses a
// scalar-only matcher (`HostScalarValueEq`) consistent with the
// arena fast paths in `cel_runtime.c::cel_value_eq` —
// nested-aggregate equality (lists of lists, maps of messages, …)
// returns false here for now; see
// `doc/implementation-plan/rewrite/cel-host-surface.md` for the
// scope boundary.
// ══════════════════════════════════════════════════════════════════

namespace {

// Scalar-equality matcher mirroring `cel_runtime.c::cel_value_eq`
// + `map_keys_equal`: cross-type numeric per langdef §"Equality"
// for int/uint/double; structural for bool / string / bytes / null.
// Aggregate kinds (CEL_LIST_*, CEL_MAP_*, CEL_MESSAGE) return false —
// the host arms only need scalar equality for `in` / `eq` element
// matching.  Span operands ReadSpan via the MemoryView since both
// arena spans (literal-built) and encoded backing-element spans
// (allocated via the trampoline's ArenaAllocator) live in linear
// memory.
bool HostScalarSpanEq(const CelValue& a, const CelValue& b,
                      const MemoryView& mem) {
  if (a.payload.s.len != b.payload.s.len) return false;
  absl::string_view sa = mem.ReadSpan(a.payload.s.ptr, a.payload.s.len);
  absl::string_view sb = mem.ReadSpan(b.payload.s.ptr, b.payload.s.len);
  return sa == sb;
}

bool HostScalarSameKindEq(const CelValue& a, const CelValue& b,
                          const MemoryView& mem) {
  switch (a.kind) {
    case CEL_NULL:
      return true;
    case CEL_BOOL:
      return a.payload.b == b.payload.b;
    case CEL_INT:
      return a.payload.i == b.payload.i;
    case CEL_UINT:
      return a.payload.u == b.payload.u;
    case CEL_DOUBLE:
      return a.payload.d == b.payload.d;
    case CEL_STRING:
    case CEL_BYTES:
      return HostScalarSpanEq(a, b, mem);
    case CEL_DURATION:
      return a.payload.dur.seconds == b.payload.dur.seconds &&
             a.payload.dur.nanos == b.payload.dur.nanos;
    case CEL_TIMESTAMP:
      return a.payload.ts.seconds == b.payload.ts.seconds &&
             a.payload.ts.nanos == b.payload.ts.nanos;
    default:
      return false;
  }
}

bool HostNumericCrossEq(const CelValue& a, const CelValue& b) {
  // langdef §"Equality": int/uint/double compare by mathematical
  // value across the type ladder.  Unrepresentable cross-type
  // (e.g. int<0 vs uint) → false.
  auto get_d = [](const CelValue& v, double* out) {
    switch (v.kind) {
      case CEL_INT:
        *out = static_cast<double>(v.payload.i);
        return true;
      case CEL_UINT:
        *out = static_cast<double>(v.payload.u);
        return true;
      case CEL_DOUBLE:
        *out = v.payload.d;
        return true;
      default:
        return false;
    }
  };
  double da = 0;
  double db = 0;
  if (!get_d(a, &da) || !get_d(b, &db)) return false;
  return da == db;
}

bool HostScalarValueEq(const CelValue& a, const CelValue& b,
                       const MemoryView& mem) {
  if (a.kind == b.kind) return HostScalarSameKindEq(a, b, mem);
  return HostNumericCrossEq(a, b);
}

// Read the i-th element of a CEL_LIST_ARENA via the MemoryView.
// `cv` must be CEL_LIST_ARENA; caller has verified.
CelValue ReadArenaListElement(const CelValue& cv, uint32_t i,
                              const MemoryView& mem) {
  ArenaListHeader hdr{};
  // ReadCelValue is also what reads ArenaListHeader-shaped runs —
  // it's just memcpy(24) which truncates to the 16-byte header.
  // Use a typed memcpy through ReadSpan for clarity.
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_list.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  return mem.ReadCelValue(hdr.elements_offset +
                          (i * static_cast<uint32_t>(kCelListEntryStride)));
}

uint32_t ReadArenaListCount(const CelValue& cv, const MemoryView& mem) {
  ArenaListHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_list.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  return hdr.count;
}

// Encode a backing-returned celwasm::Value into a CelValue.  Aggregate
// returns POISON since aggregate element equality is out of scope
// here (mirrors arena fast path).
absl::StatusOr<CelValue> EncodeBackingScalar(const celwasm::Value& v,
                                             ArenaAllocator& alloc) {
  using K = celwasm::Value::Kind;
  if (v.kind() == K::kMessage || v.kind() == K::kMap || v.kind() == K::kList) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    return err;
  }
  CelValue cv{};
  if (auto s = EncodeValue(v, &cv, alloc); !s.ok()) return s;
  return cv;
}

// `WriteWireBool` / `WriteWireInt` / `AbsorbUnary` / `AbsorbBinary`
// moved to cel_host_error.cc (M11 Slice E).

}  // namespace

absl::Status CelListSizeImpl(uint32_t out_slot, uint32_t list_slot,
                             const TrampolineContext& ctx) {
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  if (AbsorbUnary(list_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListSizeImpl: list ref_slot ",
                     list_cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  WriteWireInt(static_cast<int64_t>(backing->Size()), out_slot, ctx.mem);
  return absl::OkStatus();
}

// Direct equality of a backing-side `celwasm::Value` against the
// already-decoded wire query `query_cv`.  Skips the per-element
// `EncodeBackingScalar` arena allocation that the legacy At()-loop
// path took — for a 1000-element 50-byte-string scan that allocation
// alone was ~50us / scan (measured 2026-06-03; see
// `BM_Eval_In_IamPermissions_Bound_Last/1000` vs the cel-cpp sibling).
//
// Same-kind comparisons cover scalar / string / bytes / temporal,
// matching `HostScalarSameKindEq` semantics.  Aggregate kinds on
// the backing side (kMessage / kList / kMap) return false — `in` /
// `eq` element matching is scalar-only per the m11 contract above.
// Cross-numeric (int / uint / double) routes through
// `HostNumericCrossEq` against a synthesised CelValue prototype so
// the langdef §"Equality" mathematical-value rule holds for
// `1 in [1u, 2u]`.
// Compare a numeric backing scalar (int / uint / double) against a
// wire query CelValue.  Same-kind compares hit the direct payload
// path; cross-kind routes through `HostNumericCrossEq` against a
// synthesised prototype so the langdef §"Equality" mathematical-value
// rule holds (`1 in [1u]`).  Single call site per numeric arm so the
// optimizer folds it back into `BackingValueEqualsQuery`.
static bool NumericBackingEqualsQuery(uint32_t same_kind, const CelValue& proto,
                                      const CelValue& query_cv) {
  if (query_cv.kind == same_kind) {
    switch (same_kind) {
      case CEL_INT:
        return query_cv.payload.i == proto.payload.i;
      case CEL_UINT:
        return query_cv.payload.u == proto.payload.u;
      default:  // CEL_DOUBLE
        return query_cv.payload.d == proto.payload.d;
    }
  }
  return HostNumericCrossEq(proto, query_cv);
}

// Compare a string/bytes backing scalar against a wire query CelValue
// of the matching wire kind, reading the query bytes from linear
// memory.  Returns false for any non-matching query kind.  Single
// call site per arm so it inlines back into the caller.
static bool SpanBackingEqualsQuery(absl::string_view backing,
                                   uint32_t want_kind, const CelValue& query_cv,
                                   const MemoryView& mem) {
  if (query_cv.kind != want_kind) return false;
  if (query_cv.payload.s.len != backing.size()) return false;
  absl::string_view q =
      mem.ReadSpan(query_cv.payload.s.ptr, query_cv.payload.s.len);
  return q == backing;
}

static bool BackingValueEqualsQuery(const celwasm::Value& bv,
                                    const CelValue& query_cv,
                                    const MemoryView& mem) {
  using K = celwasm::Value::Kind;
  switch (bv.kind()) {
    case K::kBool:
      if (query_cv.kind != CEL_BOOL) return false;
      return *bv.AsBool() == (query_cv.payload.b != 0);
    case K::kInt: {
      CelValue proto{};
      proto.kind = CEL_INT;
      proto.payload.i = *bv.AsInt();
      return NumericBackingEqualsQuery(CEL_INT, proto, query_cv);
    }
    case K::kUint: {
      CelValue proto{};
      proto.kind = CEL_UINT;
      proto.payload.u = *bv.AsUint();
      return NumericBackingEqualsQuery(CEL_UINT, proto, query_cv);
    }
    case K::kDouble: {
      CelValue proto{};
      proto.kind = CEL_DOUBLE;
      proto.payload.d = *bv.AsDouble();
      return NumericBackingEqualsQuery(CEL_DOUBLE, proto, query_cv);
    }
    case K::kString:
      return SpanBackingEqualsQuery(*bv.AsString(), CEL_STRING, query_cv, mem);
    case K::kBytes:
      return SpanBackingEqualsQuery(*bv.AsBytes(), CEL_BYTES, query_cv, mem);
    case K::kNull:
      return query_cv.kind == CEL_NULL;
    case K::kDuration: {
      if (query_cv.kind != CEL_DURATION) return false;
      const absl::Duration d = *bv.AsDuration();
      return absl::ToInt64Seconds(d) == query_cv.payload.dur.seconds &&
             absl::ToInt64Nanoseconds(d -
                                      absl::Seconds(absl::ToInt64Seconds(d))) ==
                 query_cv.payload.dur.nanos;
    }
    case K::kTimestamp: {
      if (query_cv.kind != CEL_TIMESTAMP) return false;
      const absl::Time t = *bv.AsTimestamp();
      const int64_t sec = absl::ToUnixSeconds(t);
      const int64_t nanos =
          absl::ToInt64Nanoseconds(t - absl::FromUnixSeconds(sec));
      return sec == query_cv.payload.ts.seconds &&
             nanos == query_cv.payload.ts.nanos;
    }
    default:
      // Aggregates + error / unknown / type aren't matchable against a
      // scalar query in the m11 `in` / `eq` contract.
      return false;
  }
}

absl::Status CelListInImpl(uint32_t out_slot, uint32_t value_slot,
                           uint32_t list_slot, const TrampolineContext& ctx) {
  CelValue value_cv = ctx.mem.ReadCelValue(value_slot);
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  if (AbsorbBinary(value_cv, list_cv, out_slot, ctx.mem)) {
    return absl::OkStatus();
  }
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListInImpl: list ref_slot ", list_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  // Use ForEach + a direct compare against the pre-decoded wire query
  // CelValue.  Avoids the per-element `At()` -> `StatusOr<Value>` and
  // the per-element `EncodeBackingScalar` arena allocation.  We track
  // `found` to short-circuit; `ForEach` itself can't break, so the
  // post-found iterations just skip the compare.
  bool found = false;
  backing->ForEach([&](const celwasm::Value& v) {
    if (found) return;
    if (BackingValueEqualsQuery(v, value_cv, ctx.mem)) found = true;
  });
  WriteWireBool(found, out_slot, ctx.mem);
  return absl::OkStatus();
}

namespace {

// Tri-state outcome of proto-message equality (Any peel + descriptor
// check + MessageDifferencer).  kNotComparable covers operands
// without an underlying proto (custom non-proto backings) and Any
// payloads that fail to unpack.  Defined with the message-equality
// trampoline below; shared with the list-equality walk so message
// elements compare with full proto semantics.
enum class ProtoMessageEqOutcome : uint8_t { kEqual, kUnequal, kNotComparable };
ProtoMessageEqOutcome CompareProtoMessages(
    const google::protobuf::Message* absl_nullable a,
    const google::protobuf::Message* absl_nullable b);

// Returns the count of `cv` whether arena or host.  Caller has
// already verified kind ∈ {CEL_LIST_ARENA, CEL_LIST_HOST}.
absl::StatusOr<size_t> ListLength(const CelValue& cv,
                                  const TrampolineContext& ctx) {
  if (cv.kind == CEL_LIST_ARENA) {
    return static_cast<size_t>(ReadArenaListCount(cv, ctx.mem));
  }
  const HostListBacking* backing = ctx.refs.LookupList(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "list ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  return backing->Size();
}

// One list element normalized across origins for the equality walk —
// the same normalizing-accessor bridge as SnapshotMapEntries below
// uses for maps.  Scalar elements carry a wire CelValue (nested
// lists/maps encode to a CEL_ERROR placeholder that compares
// unequal — nested-aggregate equality stays out of scope per the
// trampoline block header above); message elements resolve to the
// underlying proto so the compare goes through the same Any-peel +
// MessageDifferencer core as `cel_host.cel_message_eq`.
// `keepalive` pins the host-side backing (ProtoList::At returns a
// fresh ProtoBacking per call) so `msg` stays valid for the compare.
struct ListEqElement {
  CelValue wire{};
  bool is_message = false;
  const google::protobuf::Message* absl_nullable msg = nullptr;
  std::shared_ptr<const HostMessageBacking> keepalive;
};

// Reads the i-th element of an arena-origin list into the normalized
// form.  CEL_MESSAGE wire values must reference an interned
// msg_slot; a dangling slot is interner/codegen drift → non-OK.
absl::StatusOr<ListEqElement> ReadArenaListEqElement(
    const CelValue& cv, size_t i, const TrampolineContext& ctx) {
  ListEqElement e;
  e.wire = ReadArenaListElement(cv, static_cast<uint32_t>(i), ctx.mem);
  if (e.wire.kind == CEL_MESSAGE) {
    const HostMessageBacking* backing =
        ctx.refs.Lookup(e.wire.payload.msg_slot);
    if (backing == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("list element msg_slot ", e.wire.payload.msg_slot,
                       " not found in ExternrefTable"));
    }
    e.is_message = true;
    e.msg = backing->message();  // null for non-proto custom backings
  }
  return e;
}

// Reads the i-th element of a host-origin list into the normalized
// form.  Caller has verified `cv.kind == CEL_LIST_HOST`.
absl::StatusOr<ListEqElement> ReadHostListEqElement(
    const CelValue& cv, size_t i, const TrampolineContext& ctx) {
  const HostListBacking* backing = ctx.refs.LookupList(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "list ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  auto got = backing->At(i, celwasm::CelType::Int());
  if (!got.ok()) return got.status();
  ListEqElement e;
  if (got->kind() == celwasm::Value::Kind::kMessage) {
    auto shared_or = got->SharedMessageBacking();
    if (!shared_or.ok()) return shared_or.status();
    e.is_message = true;
    e.keepalive = *std::move(shared_or);
    e.msg = e.keepalive->message();  // null for non-proto custom backings
    return e;
  }
  auto enc_or = EncodeBackingScalar(*got, ctx.alloc);
  if (!enc_or.ok()) return enc_or.status();
  e.wire = *enc_or;
  return e;
}

absl::StatusOr<ListEqElement> ReadListEqElementAt(
    const CelValue& cv, size_t i, const TrampolineContext& ctx) {
  if (cv.kind == CEL_LIST_ARENA) {
    return ReadArenaListEqElement(cv, i, ctx);
  }
  return ReadHostListEqElement(cv, i, ctx);
}

// Equality of two normalized elements.  Message pairs route through
// CompareProtoMessages — kNotComparable (non-proto backing,
// Any-unpack failure) compares UNEQUAL, matching the walk's
// established nested-aggregate contract; message-vs-scalar is the
// langdef cross-kind `false`, not an error.
bool ListEqElementEquals(const ListEqElement& a, const ListEqElement& b,
                         const MemoryView& mem) {
  if (a.is_message != b.is_message) return false;
  if (a.is_message) {
    return CompareProtoMessages(a.msg, b.msg) == ProtoMessageEqOutcome::kEqual;
  }
  return HostScalarValueEq(a.wire, b.wire, mem);
}

// Element-wise equality walk for two list operands of any origin
// pair (arena+arena, arena+host, host+host).  Caller has already
// verified both kinds are list-shaped and verified equal lengths.
// Returns OkStatus + `*equal` set; non-OK Status only on
// infrastructure failure (bad ref_slot, backing->At error).
absl::Status WalkListEq(const CelValue& a_cv, const CelValue& b_cv, size_t n,
                        const TrampolineContext& ctx, bool* equal) {
  for (size_t i = 0; i < n; ++i) {
    auto ea_or = ReadListEqElementAt(a_cv, i, ctx);
    if (!ea_or.ok()) return ea_or.status();
    auto eb_or = ReadListEqElementAt(b_cv, i, ctx);
    if (!eb_or.ok()) return eb_or.status();
    if (!ListEqElementEquals(*ea_or, *eb_or, ctx.mem)) {
      *equal = false;
      return absl::OkStatus();
    }
  }
  *equal = true;
  return absl::OkStatus();
}

}  // namespace

absl::Status CelListEqImpl(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
                           const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  const bool a_ok = (a_cv.kind == CEL_LIST_ARENA || a_cv.kind == CEL_LIST_HOST);
  const bool b_ok = (b_cv.kind == CEL_LIST_ARENA || b_cv.kind == CEL_LIST_HOST);
  if (!a_ok || !b_ok) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  auto na_or = ListLength(a_cv, ctx);
  if (!na_or.ok()) return na_or.status();
  auto nb_or = ListLength(b_cv, ctx);
  if (!nb_or.ok()) return nb_or.status();
  if (*na_or != *nb_or) {
    WriteWireBool(false, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  bool equal = true;
  if (auto s = WalkListEq(a_cv, b_cv, *na_or, ctx, &equal); !s.ok()) return s;
  WriteWireBool(equal, out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelListConcatImpl(uint32_t out_slot, uint32_t a_slot,
                               uint32_t b_slot, const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  // ── Materialisation strategy (DESIGN, follow-up impl) ────────
  //
  // The shipping behaviour for mixed-origin / both-host list
  // concat is to MATERIALISE the host operand(s) into the arena
  // and then run the arena+arena fast path.  Concretely:
  //
  //   1. Allocate a fresh ArenaListHeader + elements run via
  //      `arena_alloc`, sized `a_size + b_size`.  ArenaAllocator's
  //      `Alloc` already reenters wasm for `arena_alloc`, so this
  //      works from inside a host trampoline.
  //   2. For each operand:
  //        - If CEL_LIST_ARENA: memcpy the elements run into the
  //          new run at the right offset.
  //        - If CEL_LIST_HOST: walk `backing->ForEach`, encode each
  //          `celwasm::Value` into a CelValue (via `EncodeBackingScalar`
  //          extended for aggregates — pending work item), and
  //          write into the destination run.
  //   3. Write `{kind:CEL_LIST_ARENA, arena_list.header_ptr=hdr_off}`
  //      into `out_slot`.  The result is observably an arena list,
  //      which keeps downstream codegen on the fast path.
  //
  // This same strategy applies to any future operator that needs to
  // walk both operands as one origin: lift host into arena, then run
  // the arena fast path.  (Map equality instead normalizes both
  // operands into host-side snapshots — see CelMapEqImpl — since a
  // read-only walk doesn't need the arena materialisation.)
  // Documented in
  // `doc/implementation-plan/rewrite/m5-kcall-comprehensions.md`
  // §"Cross-origin materialisation" and
  // `doc/implementation-plan/rewrite/map-list-dispatch.md` §6.
  //
  // Current ship state: nested-aggregate elements + the re-entrant
  // arena allocation aren't fully exercised yet, so mixed-origin
  // concat POISONs with TYPE_MISMATCH; follow-up work flips this to
  // actual materialisation.
  WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelMapSizeImpl(uint32_t out_slot, uint32_t map_slot,
                            const TrampolineContext& ctx) {
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  if (AbsorbUnary(map_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (map_cv.kind != CEL_MAP_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapSizeImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  WriteWireInt(static_cast<int64_t>(backing->Size()), out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelMapInImpl(uint32_t out_slot, uint32_t key_slot,
                          uint32_t map_slot, const TrampolineContext& ctx) {
  CelValue key_cv = ctx.mem.ReadCelValue(key_slot);
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  if (AbsorbBinary(key_cv, map_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (map_cv.kind != CEL_MAP_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapInImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  std::optional<celwasm::Value> key = DecodeKey(key_cv, ctx.mem);
  if (!key.has_value()) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  WriteWireBool(backing->ContainsKey(*key), out_slot, ctx.mem);
  return absl::OkStatus();
}

namespace {

// Read the ArenaMapHeader of a CEL_MAP_ARENA operand via the
// MemoryView (mirrors ReadArenaListCount above).  `cv` must be
// CEL_MAP_ARENA; caller has verified.
ArenaMapHeader ReadArenaMapHeader(const CelValue& cv, const MemoryView& mem) {
  ArenaMapHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_map.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  return hdr;
}

// Returns the entry count of `cv` whether arena or host.  Caller has
// already verified kind ∈ {CEL_MAP_ARENA, CEL_MAP_HOST}.
absl::StatusOr<size_t> MapEntryCount(const CelValue& cv,
                                     const TrampolineContext& ctx) {
  if (cv.kind == CEL_MAP_ARENA) {
    return static_cast<size_t>(ReadArenaMapHeader(cv, ctx.mem).count);
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "map ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  return backing->Size();
}

// Snapshot every entry of `cv` as wire-format (key, value) CelValue
// pairs, regardless of origin — the normalizing accessor that lets
// the equality walk below treat arena and host operands uniformly
// (same bridge shape as ReadListEqElementAt for lists).  Arena entries
// are read straight out of linear memory; host entries are encoded
// via EncodeBackingScalar (aggregate values encode to a CEL_ERROR
// placeholder, which compares unequal — nested-aggregate equality is
// out of scope here, matching the scalar-only contract documented at
// the trampoline block header above).
absl::Status SnapshotMapEntries(
    const CelValue& cv, const TrampolineContext& ctx,
    std::vector<std::pair<CelValue, CelValue>>* out) {
  if (cv.kind == CEL_MAP_ARENA) {
    const ArenaMapHeader hdr = ReadArenaMapHeader(cv, ctx.mem);
    out->reserve(hdr.count);
    for (uint32_t i = 0; i < hdr.count; ++i) {
      const uint32_t entry_off =
          hdr.entries_offset + (i * static_cast<uint32_t>(kCelMapEntryStride));
      out->emplace_back(
          ctx.mem.ReadCelValue(entry_off),
          ctx.mem.ReadCelValue(entry_off +
                               static_cast<uint32_t>(sizeof(CelValue))));
    }
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "map ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  absl::Status status = absl::OkStatus();
  out->reserve(backing->Size());
  backing->ForEach([&](const celwasm::Value& k, const celwasm::Value& v) {
    if (!status.ok()) return;
    auto ek_or = EncodeBackingScalar(k, ctx.alloc);
    if (!ek_or.ok()) {
      status = ek_or.status();
      return;
    }
    auto ev_or = EncodeBackingScalar(v, ctx.alloc);
    if (!ev_or.ok()) {
      status = ev_or.status();
      return;
    }
    out->emplace_back(*ek_or, *ev_or);
  });
  return status;
}

// Returns true iff some entry of `b` has a key equal to `entry`'s
// key AND a value equal to `entry`'s value.  Key equality goes
// through HostScalarValueEq, so numeric keys match across the
// int/uint/double ladder (langdef §"Equality") — same polymorphic
// rule as the arena kernel's `map_keys_equal`.  Mirrors
// `cel_runtime.c::arena_map_entry_matches`.
bool NormalizedMapEntryMatches(
    const std::pair<CelValue, CelValue>& entry,
    const std::vector<std::pair<CelValue, CelValue>>& b,
    const MemoryView& mem) {
  for (const auto& [kb, vb] : b) {
    if (HostScalarValueEq(entry.first, kb, mem)) {
      return HostScalarValueEq(entry.second, vb, mem);
    }
  }
  return false;
}

// Set-equality of two map operands of any origin pair, via the
// normalized snapshots above (langdef §"Equality" — map order is
// irrelevant).  Caller has verified both kinds are map-shaped.
// Returns the boolean answer; non-OK Status only on infrastructure
// failure (bad ref_slot, backing error).
absl::StatusOr<bool> NormalizedMapEq(const CelValue& a_cv, const CelValue& b_cv,
                                     const TrampolineContext& ctx) {
  auto na_or = MapEntryCount(a_cv, ctx);
  if (!na_or.ok()) return na_or.status();
  auto nb_or = MapEntryCount(b_cv, ctx);
  if (!nb_or.ok()) return nb_or.status();
  if (*na_or != *nb_or) return false;
  std::vector<std::pair<CelValue, CelValue>> a_entries;
  std::vector<std::pair<CelValue, CelValue>> b_entries;
  if (auto s = SnapshotMapEntries(a_cv, ctx, &a_entries); !s.ok()) return s;
  if (auto s = SnapshotMapEntries(b_cv, ctx, &b_entries); !s.ok()) return s;
  for (const auto& ea : a_entries) {
    if (!NormalizedMapEntryMatches(ea, b_entries, ctx.mem)) return false;
  }
  return true;
}

}  // namespace

absl::Status CelMapEqImpl(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
                          const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  // Any origin pair is admitted (host+host, host+arena, arena+host;
  // arena+arena normally short-circuits in the runtime's arena fast
  // path but compares correctly here too).  Both operands are
  // snapshotted through the same normalizing accessor, then compared
  // with a set-equality walk.
  const bool a_ok = (a_cv.kind == CEL_MAP_ARENA || a_cv.kind == CEL_MAP_HOST);
  const bool b_ok = (b_cv.kind == CEL_MAP_ARENA || b_cv.kind == CEL_MAP_HOST);
  if (!a_ok || !b_ok) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  auto eq_or = NormalizedMapEq(a_cv, b_cv, ctx);
  if (!eq_or.ok()) return eq_or.status();
  WriteWireBool(*eq_or, out_slot, ctx.mem);
  return absl::OkStatus();
}

// If `m` is a google.protobuf.Any, unpack its type_url + value
// against the Any descriptor's own pool and return a fresh
// typed-message clone (stashed in `owner` so the caller can keep it
// alive).  Returns the original `m` when it's not an Any; returns
// nullptr on Any-unpack failure (malformed type_url / unknown FQN /
// corrupt bytes).  Used by CelMessageEqImpl to compare an Any
// against either a typed message or another Any uniformly.
static const google::protobuf::Message* absl_nullable PeelAnyForEq(
    const google::protobuf::Message* m,
    std::unique_ptr<google::protobuf::Message>& owner) {
  if (m->GetDescriptor() == nullptr ||
      m->GetDescriptor()->full_name() != "google.protobuf.Any") {
    return m;
  }
  celwasm::Value peeled =
      UnpackAnyToValue(*m, m->GetDescriptor()->file()->pool());
  if (peeled.kind() != celwasm::Value::Kind::kMessage) return nullptr;
  auto backing_or = peeled.MessageBacking();
  if (!backing_or.ok() || (*backing_or)->message() == nullptr) return nullptr;
  const google::protobuf::Message* src = (*backing_or)->message();
  owner.reset(src->New());
  owner->CopyFrom(*src);
  return owner.get();
}

namespace {

// Proto-message equality core, shared by `cel_host.cel_message_eq`
// and the list-equality element walk (declared above ListLength).
// Peels google.protobuf.Any operands, then compares by descriptor +
// MessageDifferencer.  Null inputs (non-proto custom backings) and
// Any-unpack failures are kNotComparable — the caller decides
// whether that surfaces as a type error (direct `msg == msg`) or as
// unequal (list element walk).
ProtoMessageEqOutcome CompareProtoMessages(
    const google::protobuf::Message* absl_nullable a,
    const google::protobuf::Message* absl_nullable b) {
  if (a == nullptr || b == nullptr) {
    return ProtoMessageEqOutcome::kNotComparable;
  }
  // Peel either operand if it's a google.protobuf.Any (typical
  // shape: a direct `Any{...}` literal that didn't pass through
  // ProtoBacking::ReadField's unwrap arm).  The peeled owners live
  // for the duration of this call.
  std::unique_ptr<google::protobuf::Message> a_owner;
  std::unique_ptr<google::protobuf::Message> b_owner;
  const google::protobuf::Message* a_cmp = PeelAnyForEq(a, a_owner);
  const google::protobuf::Message* b_cmp = PeelAnyForEq(b, b_owner);
  if (a_cmp == nullptr || b_cmp == nullptr) {
    // Any with malformed type_url / unknown FQN / corrupt bytes —
    // equality is undefined.
    return ProtoMessageEqOutcome::kNotComparable;
  }
  if (a_cmp->GetDescriptor() != b_cmp->GetDescriptor()) {
    // Cross-descriptor mismatch after peel → unequal, not error.
    return ProtoMessageEqOutcome::kUnequal;
  }
  return google::protobuf::util::MessageDifferencer::Equals(*a_cmp, *b_cmp)
             ? ProtoMessageEqOutcome::kEqual
             : ProtoMessageEqOutcome::kUnequal;
}

}  // namespace

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

// ══════════════════════════════════════════════════════════════════
// `cel_host.cel_set_field(msg_slot, field_ref_id, value_slot)`.
//
// Per-cpp_type dispatch on the resolved FieldDescriptor.  The
// OwnedProtoBacking wrapping the constructed message exposes a
// non-const `Message*` via `mutable_message()`; reflection's
// `Set...` family writes through that pointer.
//
// Repeated + map fields walk the source list/map (arena or host)
// and per element call `Reflection::Add...` for repeated, or build
// a fresh MapEntry submessage via `Reflection::AddMessage` for map.
// Per-cpp_type dispatch shares structure with the scalar path
// (singular `Set...` ↔ repeated `Add...`); element-of-message and
// map-value-of-message route through `CopyFrom` on a fresh
// reflection-allocated submessage.
// ══════════════════════════════════════════════════════════════════

namespace {

// Read a CelValue's scalar payload as int64 — used for INT32/INT64/ENUM
// dispatch.  Caller has verified `cv.kind == CEL_INT`.
int64_t ReadInt64(const CelValue& cv) {
  return cv.payload.i;
}
uint64_t ReadUInt64(const CelValue& cv) {
  return cv.payload.u;
}
double ReadDouble(const CelValue& cv) {
  return cv.payload.d;
}

// Range-check an int64 CEL value before narrowing to an int32 proto
// field.  cel-cpp surfaces an out-of-range field assignment as an
// eval error ("range error"); we return OutOfRange so the caller
// propagates it rather than silently truncating.  Shared by the bare
// int32 field arm and the Int32Value-wrapper arm.
absl::Status CheckInt32Range(int64_t v, absl::string_view field_name) {
  if (v < std::numeric_limits<int32_t>::min() ||
      v > std::numeric_limits<int32_t>::max()) {
    return absl::OutOfRangeError(absl::StrCat(
        "CelSetFieldImpl: field `", field_name, "` int32 range error: ", v));
  }
  return absl::OkStatus();
}

// Range-check a uint64 CEL value before narrowing to a uint32 proto
// field.  Same contract as `CheckInt32Range`.
absl::Status CheckUint32Range(uint64_t v, absl::string_view field_name) {
  if (v > std::numeric_limits<uint32_t>::max()) {
    return absl::OutOfRangeError(absl::StrCat(
        "CelSetFieldImpl: field `", field_name, "` uint32 range error: ", v));
  }
  return absl::OkStatus();
}

// Read a string/bytes payload via the MemoryView's Span reader.
// Caller has verified the value kind is CEL_STRING or CEL_BYTES.
std::string ReadSpanString(const CelValue& cv, const MemoryView& mem) {
  absl::string_view sv = mem.ReadSpan(cv.payload.s.ptr, cv.payload.s.len);
  return std::string(sv);
}

// Write `src` into `dst` (a CPPTYPE_MESSAGE slot the caller already
// resolved via `MutableMessage` or `AddMessage`).  Three shapes:
//   (1) dst descriptor == src descriptor    → CopyFrom.
//   (2) dst is google.protobuf.Any          → reflection-pack.
//   (3) other descriptor mismatch           → InvalidArgument (see
//                                             tail below).
// The reflection path (vs the typed `Any::PackFrom`) is required for
// portability across generated and dynamic descriptor pools.  Shared
// across every cpp_type-MESSAGE caller (singular set, repeated
// append, map-entry value).
absl::Status WriteMessageOrPack(google::protobuf::Message* dst,
                                const google::protobuf::Message& src) {
  using FD = google::protobuf::FieldDescriptor;
  const google::protobuf::Descriptor* dst_desc = dst->GetDescriptor();
  const google::protobuf::Descriptor* src_desc = src.GetDescriptor();
  if (src_desc == dst_desc) {
    dst->CopyFrom(src);
    return absl::OkStatus();
  }
  if (dst_desc->full_name() == "google.protobuf.Any") {
    const google::protobuf::Reflection* refl = dst->GetReflection();
    const google::protobuf::FieldDescriptor* type_url_fd =
        dst_desc->FindFieldByName("type_url");
    const google::protobuf::FieldDescriptor* value_fd =
        dst_desc->FindFieldByName("value");
    if (refl == nullptr || type_url_fd == nullptr ||
        type_url_fd->cpp_type() != FD::CPPTYPE_STRING || value_fd == nullptr ||
        value_fd->cpp_type() != FD::CPPTYPE_STRING) {
      return absl::InternalError(
          "WriteMessageOrPack: Any descriptor missing type_url/value fields");
    }
    refl->SetString(
        dst, type_url_fd,
        absl::StrCat("type.googleapis.com/", src_desc->full_name()));
    refl->SetString(dst, value_fd, src.SerializeAsString());
    return absl::OkStatus();
  }
  // The remaining descriptor-mismatch path (dst is some non-Any
  // non-same-descriptor target) is reachable only if codegen /
  // Activation handed us a CEL_MESSAGE with the wrong descriptor.
  // Wrapper tail-unwrap and `SetWrapperFieldFromScalar` both route
  // scalar values through their own paths before reaching here;
  // only proper-message-vs-mismatched-message lands here.  Surface
  // as Invalid rather than CHECK — embedder error, not codegen bug.
  return absl::InvalidArgumentError(
      absl::StrCat("WriteMessageOrPack: dst `", dst_desc->full_name(),
                   "` ≠ src `", src_desc->full_name(),
                   "` — descriptor mismatch on singular-message "
                   "field write"));
}

// Build the `SetWrapperInnerValue` kind-mismatch error for a wrapper
// whose inner `value` field rejects the supplied CelValue kind.
absl::Status WrapperInnerMismatch(
    const google::protobuf::Descriptor& wrapper_desc,
    absl::string_view expected, const CelValue& value) {
  return absl::InvalidArgumentError(absl::StrCat(
      "SetWrapperInnerValue: `", wrapper_desc.full_name(), "` expects ",
      expected, " but value kind is ", static_cast<int>(value.kind)));
}

// Integer / bool arms of `SetWrapperInnerValue` (CPPTYPE_BOOL ..
// CPPTYPE_UINT64).  Returns the set Status for a handled cpp_type, or
// `std::nullopt` for FLOAT / DOUBLE / STRING (the caller handles
// those).  INT32 / UINT32 range-check before narrowing.  Single call
// site → inlines back.
std::optional<absl::Status> SetWrapperIntegerValue(
    const google::protobuf::Reflection& wr, google::protobuf::Message& wrapper,
    const google::protobuf::FieldDescriptor& vf,
    const google::protobuf::Descriptor& wrapper_desc, const CelValue& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (vf.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (value.kind != CEL_BOOL) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_BOOL", value);
      }
      wr.SetBool(&wrapper, &vf, value.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_INT32:
      if (value.kind != CEL_INT) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_INT", value);
      }
      if (auto s = CheckInt32Range(ReadInt64(value), wrapper_desc.full_name());
          !s.ok()) {
        return s;
      }
      wr.SetInt32(&wrapper, &vf, static_cast<int32_t>(ReadInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (value.kind != CEL_INT) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_INT", value);
      }
      wr.SetInt64(&wrapper, &vf, ReadInt64(value));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (value.kind != CEL_UINT) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_UINT", value);
      }
      if (auto s =
              CheckUint32Range(ReadUInt64(value), wrapper_desc.full_name());
          !s.ok()) {
        return s;
      }
      wr.SetUInt32(&wrapper, &vf, static_cast<uint32_t>(ReadUInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (value.kind != CEL_UINT) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_UINT", value);
      }
      wr.SetUInt64(&wrapper, &vf, ReadUInt64(value));
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// Write the inner `value` field of a freshly-allocated wrapper
// message from a matching scalar CelValue.  9-way cpp_type dispatch,
// mirror of the read-side `UnpackWrapperMessage`.  Extracted from
// `SetWrapperFieldFromScalar` to keep the parent under the
// readability-function-size gate.
absl::Status SetWrapperInnerValue(
    const google::protobuf::Reflection& wr, google::protobuf::Message& wrapper,
    const google::protobuf::FieldDescriptor& vf,
    const google::protobuf::Descriptor& wrapper_desc, const CelValue& value,
    const MemoryView& mem) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = SetWrapperIntegerValue(wr, wrapper, vf, wrapper_desc, value);
      s.has_value()) {
    return *std::move(s);
  }
  switch (vf.cpp_type()) {
    case FD::CPPTYPE_FLOAT:
      if (value.kind != CEL_DOUBLE) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_DOUBLE", value);
      }
      wr.SetFloat(&wrapper, &vf, static_cast<float>(ReadDouble(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (value.kind != CEL_DOUBLE) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_DOUBLE", value);
      }
      wr.SetDouble(&wrapper, &vf, ReadDouble(value));
      return absl::OkStatus();
    case FD::CPPTYPE_STRING:
      if (vf.type() == FD::TYPE_BYTES) {
        if (value.kind != CEL_BYTES) {
          return WrapperInnerMismatch(wrapper_desc, "CEL_BYTES", value);
        }
      } else if (value.kind != CEL_STRING) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_STRING", value);
      }
      wr.SetString(&wrapper, &vf, ReadSpanString(value, mem));
      return absl::OkStatus();
    default:
      ABSL_CHECK(false)
          << "SetWrapperInnerValue: WKT-wrapper FQN claims unexpected inner "
             "cpp_type "
          << static_cast<int>(vf.cpp_type());
      return absl::InternalError("unreachable");
  }
}

// Synthesise a wrapper-message proto from a matching scalar
// CelValue and assign it to a wrapper-typed singular-message field
// on the outer message.  Called by `SetScalarField`'s CPPTYPE_MESSAGE
// arm when the field's `message_type()` FQN is one of the 9 wrapper
// FQNs.  Mirror of the read-side `UnpackWrapperMessage` shape.
absl::Status SetWrapperFieldFromScalar(
    const google::protobuf::Reflection& outer_refl,
    google::protobuf::Message& outer,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Descriptor& wrapper_desc, const CelValue& value,
    const MemoryView& mem) {
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          &wrapper_desc);
  if (prototype == nullptr) {
    return absl::InternalError(absl::StrCat(
        "SetWrapperFieldFromScalar: no generated_factory prototype for `",
        wrapper_desc.full_name(), "`"));
  }
  std::unique_ptr<google::protobuf::Message> wrapper(prototype->New());
  const google::protobuf::Reflection* wr = wrapper->GetReflection();
  const google::protobuf::FieldDescriptor* vf =
      wrapper_desc.FindFieldByNumber(1);
  if (wr == nullptr || vf == nullptr) {
    return absl::InternalError(
        absl::StrCat("SetWrapperFieldFromScalar: `", wrapper_desc.full_name(),
                     "` missing reflection or value-field descriptor"));
  }
  if (auto s =
          SetWrapperInnerValue(*wr, *wrapper, *vf, wrapper_desc, value, mem);
      !s.ok()) {
    return s;
  }
  outer_refl.MutableMessage(&outer, &field)->CopyFrom(*wrapper);
  return absl::OkStatus();
}

// Forward declarations for the WKT pack dispatch invoked from
// `SetScalarField`'s CPPTYPE_MESSAGE arm and the arena-walker
// helpers the JSON-Value packer recurses through (both defined
// below, after the repeated/map walkers).
std::optional<absl::Status> MaybeSetWktMessageField(
    google::protobuf::Message& outer,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs);
std::optional<absl::Status> MaybePackWktMessage(
    google::protobuf::Message& target, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs);
// Forward-declared so the Struct / ListValue packers can recurse into
// it before its definition; the body lives below those helpers.
// NOLINTNEXTLINE(readability-redundant-declaration)
absl::Status PackCelValueIntoJsonValue(
    const CelValue& value, google::protobuf::Message& out,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs);

// Resolve a CEL_MESSAGE source through `refs` and copy/pack it into a
// freshly-mutable submessage of `field` on `msg`.  Tail of
// `SetSingularMessageField` (reached once the null / wrapper / WKT
// gates ahead of it miss); split out so the parent stays under the
// function-size gate.  Single call site → inlines back.
absl::Status SetNestedSingularMessage(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const ExternrefTable* absl_nullable refs) {
  if (value.kind != CEL_MESSAGE) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: field `", field.name(),
        "` is MESSAGE but value kind is ", static_cast<int>(value.kind)));
  }
  if (refs == nullptr) {
    return absl::InternalError(
        absl::StrCat("CelSetFieldImpl: field `", field.name(),
                     "` is MESSAGE but no ExternrefTable supplied "
                     "(nested-message call-site bug)"));
  }
  const HostMessageBacking* src = refs->Lookup(value.payload.msg_slot);
  if (src == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: field `", field.name(),
                     "` source has no externref entry"));
  }
  const google::protobuf::Message* src_msg = src->message();
  if (src_msg == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: field `", field.name(),
                     "` source backing has no proto message"));
  }
  // Descriptor-aware dst-write: descriptors match → CopyFrom;
  // dst is google.protobuf.Any → reflection-pack; other mismatch
  // → InvalidArgument (wrapper auto-wrap is gated above).
  google::protobuf::Message* dst = refl.MutableMessage(&msg, &field);
  return WriteMessageOrPack(dst, *src_msg);
}

// CPPTYPE_MESSAGE arm of `SetScalarField` — singular message-field
// assignment.  Handles, in spec order: `null` clears the field (or
// packs an explicit `null_value` for a `google.protobuf.Value`
// target); a scalar source onto a wrapper-typed field auto-wraps; a
// scalar/aggregate onto a WKT message field (Duration / Timestamp /
// Value / Struct / ListValue / Any) packs through the shared WKT
// dispatch; otherwise the source must be a CEL_MESSAGE resolved
// through `refs` and copied/packed via `WriteMessageOrPack`.  Split
// from `SetScalarField` so the cpp_type switch stays under the
// function-size gate; single call site → inlines back.
absl::Status SetSingularMessageField(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs) {
  // langdef §"Field Selection" + cel-cpp behaviour: assigning
  // `null` to a singular message field clears it (equivalent
  // to leaving it unset).  `Foo{m: null} == Foo{}` per the
  // conformance corpus's `set_null/*` rows.  For wrapper-typed
  // fields, langdef line 484-486's unset-reads-as-null rule
  // makes this round-trip with the read-side wrapper peel.
  // A `null` assigned to a `google.protobuf.Value` field packs as
  // an explicit `null_value` (NOT a cleared field) — langdef JSON
  // semantics, `value_null/field_assign_*` corpus rows.  Every
  // other message type clears on null.
  if (value.kind == CEL_NULL) {
    const google::protobuf::Descriptor* nmt = field.message_type();
    if (nmt != nullptr && nmt->full_name() == "google.protobuf.Value") {
      return PackCelValueIntoJsonValue(
          value, *refl.MutableMessage(&msg, &field), mem, refs);
    }
    refl.ClearField(&msg, &field);
    return absl::OkStatus();
  }
  // Wrapper-typed field with scalar source — synthesise the
  // wrapper proto and assign.  `Foo{single_int32_wrapper: 5}`
  // sees scalar CEL_INT here because typed_ast.cc:56 stamps the
  // value as `Int32` Repr; the auto-wrap below is the boundary
  // where the scalar becomes an `Int32Value{value: 5}` proto.
  const google::protobuf::Descriptor* mt = field.message_type();
  if (mt != nullptr && IsWrapperFqn(mt->full_name()) &&
      value.kind != CEL_MESSAGE) {
    return SetWrapperFieldFromScalar(refl, msg, field, *mt, value, mem);
  }
  // Duration / Timestamp / Value / Struct / ListValue / Any from
  // a non-message CelValue — pack the scalar/aggregate into the
  // matching well-known message.  Mirror of the read-side
  // `MaybeUnpackWktMessage` / `UnpackJsonValueMessage` peelers.
  if (mt != nullptr && value.kind != CEL_MESSAGE) {
    if (auto s = MaybeSetWktMessageField(msg, field, value, mem, refs);
        s.has_value()) {
      return *std::move(s);
    }
  }
  // Nested singular message — `Foo{nested: Bar{...}}`.  The
  // outer kStructExpr lowering recursively built `Bar{...}` into
  // a fresh OwnedProtoBacking and wrote a CEL_MESSAGE CelValue
  // at value_slot.  Resolve that backing and CopyFrom into a
  // freshly-mutable submessage of the outer field — same pattern
  // as repeated-of-message in `AppendRepeatedFromCelValue`.
  return SetNestedSingularMessage(refl, msg, field, value, refs);
}

// Build the singular-scalar kind-mismatch error (a checker regression
// surfaced as InvalidArgument).  Shared across the scalar set arms.
absl::Status ScalarKindMismatch(const google::protobuf::FieldDescriptor& field,
                                absl::string_view ty, const CelValue& value) {
  return absl::InvalidArgumentError(
      absl::StrCat("CelSetFieldImpl: field `", field.name(), "` is ", ty,
                   " but value kind is ", static_cast<int>(value.kind)));
}

// Integer / bool scalar arms of `SetScalarField` (CPPTYPE_BOOL ..
// CPPTYPE_UINT64).  Returns the set Status for a handled cpp_type, or
// `std::nullopt` for FLOAT / DOUBLE / STRING / ENUM / MESSAGE.  INT32 /
// UINT32 range-check before narrowing.  Single call site → inlines
// back.
std::optional<absl::Status> SetScalarIntegerField(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (value.kind != CEL_BOOL)
        return ScalarKindMismatch(field, "BOOL", value);
      refl.SetBool(&msg, &field, value.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_INT32:
      if (value.kind != CEL_INT)
        return ScalarKindMismatch(field, "INT32", value);
      if (auto s = CheckInt32Range(ReadInt64(value), field.name()); !s.ok()) {
        return s;
      }
      refl.SetInt32(&msg, &field, static_cast<int32_t>(ReadInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (value.kind != CEL_INT)
        return ScalarKindMismatch(field, "INT64", value);
      refl.SetInt64(&msg, &field, ReadInt64(value));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (value.kind != CEL_UINT) {
        return ScalarKindMismatch(field, "UINT32", value);
      }
      if (auto s = CheckUint32Range(ReadUInt64(value), field.name()); !s.ok()) {
        return s;
      }
      refl.SetUInt32(&msg, &field, static_cast<uint32_t>(ReadUInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (value.kind != CEL_UINT) {
        return ScalarKindMismatch(field, "UINT64", value);
      }
      refl.SetUInt64(&msg, &field, ReadUInt64(value));
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// FLOAT / DOUBLE scalar arms of `SetScalarField`.  Both accept
// CEL_DOUBLE (CEL has one floating type); FLOAT narrows.  Single call
// site → inlines back.
std::optional<absl::Status> SetScalarFloatField(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_FLOAT:
      if (value.kind != CEL_DOUBLE)
        return ScalarKindMismatch(field, "FLOAT", value);
      refl.SetFloat(&msg, &field, static_cast<float>(ReadDouble(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (value.kind != CEL_DOUBLE) {
        return ScalarKindMismatch(field, "DOUBLE", value);
      }
      refl.SetDouble(&msg, &field, ReadDouble(value));
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// STRING / BYTES and ENUM scalar arms of `SetScalarField`.  String
// fields accept CEL_STRING, bytes fields (same CPPTYPE_STRING slot,
// distinguished by `field.type()`) accept CEL_BYTES; enum values flow
// as CEL_INT (langdef §"Enumerated Types") and range-check into int32
// before assignment.  Single call site → inlines back.
absl::Status SetStringOrEnumField(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem) {
  using FD = google::protobuf::FieldDescriptor;
  if (field.cpp_type() == FD::CPPTYPE_STRING) {
    if (field.type() == FD::TYPE_BYTES) {
      if (value.kind != CEL_BYTES) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is BYTES but value kind is ", static_cast<int>(value.kind)));
      }
    } else if (value.kind != CEL_STRING) {
      return absl::InvalidArgumentError(absl::StrCat(
          "CelSetFieldImpl: field `", field.name(),
          "` is STRING but value kind is ", static_cast<int>(value.kind)));
    }
    refl.SetString(&msg, &field, ReadSpanString(value, mem));
    return absl::OkStatus();
  }
  // CPPTYPE_ENUM: cel-cpp's checker resolves `Foo.SOME_VALUE` to a
  // Constant int64; codegen flows that as CEL_INT here.
  if (value.kind != CEL_INT) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: field `", field.name(),
        "` is ENUM but value kind is ", static_cast<int>(value.kind)));
  }
  // Enum values narrow to int32 on the wire; an out-of-range
  // assignment is a CEL error in cel-cpp (struct_value_builder.cc
  // `CPPTYPE_ENUM` returns TypeConversionError), surfaced here via
  // the poison contract rather than a silent truncation.
  if (auto s = CheckInt32Range(ReadInt64(value), field.name()); !s.ok()) {
    return s;
  }
  refl.SetEnumValue(&msg, &field, static_cast<int>(ReadInt64(value)));
  return absl::OkStatus();
}

// Set a scalar singular field on `msg` per `field`'s cpp_type.  Returns
// non-OK Status on cpp_type / value-kind mismatches that the cel-cpp
// checker should have rejected pre-codegen — surfaces as a wasm trap so
// a checker regression fails the row loudly.  Repeated and map fields
// are NOT routed here; the caller checks `is_map() || is_repeated()`
// and returns Unimplemented before reaching this dispatch.
absl::Status SetScalarField(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs = nullptr) {
  using FD = google::protobuf::FieldDescriptor;
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) {
    return absl::InternalError("CelSetFieldImpl: message has no reflection");
  }
  if (auto s = SetScalarIntegerField(*refl, msg, field, value); s.has_value()) {
    return *std::move(s);
  }
  if (auto s = SetScalarFloatField(*refl, msg, field, value); s.has_value()) {
    return *std::move(s);
  }
  switch (field.cpp_type()) {
    case FD::CPPTYPE_STRING:
    case FD::CPPTYPE_ENUM:
      return SetStringOrEnumField(*refl, msg, field, value, mem);
    case FD::CPPTYPE_MESSAGE:
      return SetSingularMessageField(*refl, msg, field, value, mem, refs);
    default:
      break;
  }
  ABSL_CHECK(false) << "CelSetFieldImpl: unknown cpp_type "
                    << static_cast<int>(field.cpp_type());
  return absl::InternalError("unreachable");
}

// ──── Repeated + map field set helpers ──────────────────────────

// Walk an arena-list CelValue's elements, calling `visit(elem, i)`
// per element (read via the MemoryView).  Caller has verified
// `cv.kind == CEL_LIST_ARENA`.
void ForEachArenaListElement(
    const CelValue& cv, const MemoryView& mem,
    absl::FunctionRef<void(const CelValue&, uint32_t)> visit) {
  ArenaListHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_list.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  for (uint32_t i = 0; i < hdr.count; ++i) {
    CelValue elem = mem.ReadCelValue(
        hdr.elements_offset + (i * static_cast<uint32_t>(kCelListEntryStride)));
    visit(elem, i);
  }
}

// Walk an arena-map CelValue's entries, calling `visit(key, val, i)`
// per entry.  Caller has verified `cv.kind == CEL_MAP_ARENA`.
// Each entry is a 48-byte (key, val) pair at
// `entries_offset + i * kCelMapEntryStride`.
void ForEachArenaMapEntry(
    const CelValue& cv, const MemoryView& mem,
    absl::FunctionRef<void(const CelValue&, const CelValue&, uint32_t)> visit) {
  ArenaMapHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_map.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  for (uint32_t i = 0; i < hdr.count; ++i) {
    const uint32_t entry_off =
        hdr.entries_offset + (i * static_cast<uint32_t>(kCelMapEntryStride));
    CelValue k = mem.ReadCelValue(entry_off);
    CelValue v = mem.ReadCelValue(entry_off +
                                  static_cast<uint32_t>(kCelListEntryStride));
    visit(k, v, i);
  }
}

// ──── Well-known-type pack helpers (write side) ─────────────────
//
// Inverse of the read-side `MaybeUnpackWktMessage` /
// `UnpackJsonValueMessage` peelers: synthesise a Duration /
// Timestamp / Value / Struct / ListValue / Any message from a
// CelValue and assign it to a singular-message field.

// Allocate a fresh prototype instance of the WKT message `desc`
// describes.  Returns null on a missing generated_factory prototype
// (an InternalError the caller surfaces).
std::unique_ptr<google::protobuf::Message> NewWktMessage(
    const google::protobuf::Descriptor& desc) {
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          &desc);
  if (prototype == nullptr) return nullptr;
  return std::unique_ptr<google::protobuf::Message>(prototype->New());
}

// Set the (seconds, nanos) pair on a freshly-built Duration /
// Timestamp message from a CEL_DURATION / CEL_TIMESTAMP payload.
// `dur_ts` is `value.payload.dur` or `.ts` (both `CelDurTs`).
absl::Status PackDurationOrTimestamp(google::protobuf::Message& wkt,
                                     const CelDurTs& dur_ts) {
  const google::protobuf::Descriptor* d = wkt.GetDescriptor();
  const google::protobuf::Reflection* refl = wkt.GetReflection();
  const google::protobuf::FieldDescriptor* sf =
      d != nullptr ? d->FindFieldByNumber(1) : nullptr;
  const google::protobuf::FieldDescriptor* nf =
      d != nullptr ? d->FindFieldByNumber(2) : nullptr;
  if (refl == nullptr || sf == nullptr || nf == nullptr) {
    return absl::InternalError(
        "PackDurationOrTimestamp: WKT time message missing seconds/nanos");
  }
  refl->SetInt64(&wkt, sf, dur_ts.seconds);
  refl->SetInt32(&wkt, nf, dur_ts.nanos);
  return absl::OkStatus();
}

// `PackCelValueIntoJsonValue` (the recursive CelValue →
// google.protobuf.Value packer) is forward-declared with the other
// WKT-pack helpers above; its definition follows below.

// Populate a `google.protobuf.Struct`'s `fields` map from a CEL map
// CelValue.  Keys must be CEL_STRING (JSON object keys); each value
// is recursively packed into a `google.protobuf.Value`.
absl::Status PackStruct(const CelValue& map_cv,
                        google::protobuf::Message& strct, const MemoryView& mem,
                        const ExternrefTable* absl_nullable refs) {
  const google::protobuf::Descriptor* d = strct.GetDescriptor();
  const google::protobuf::Reflection* refl = strct.GetReflection();
  const google::protobuf::FieldDescriptor* fields_fd =
      d != nullptr ? d->FindFieldByNumber(1) : nullptr;
  if (refl == nullptr || fields_fd == nullptr || !fields_fd->is_map()) {
    return absl::InternalError("PackStruct: Struct missing `fields` map");
  }
  const google::protobuf::FieldDescriptor* key_fd =
      fields_fd->message_type()->FindFieldByNumber(1);
  const google::protobuf::FieldDescriptor* val_fd =
      fields_fd->message_type()->FindFieldByNumber(2);
  absl::Status status = absl::OkStatus();
  auto add_entry = [&](const CelValue& k, const CelValue& v) {
    if (!status.ok()) return;
    if (k.kind != CEL_STRING) {
      status = absl::InvalidArgumentError(
          "PackStruct: google.protobuf.Struct key is not a string");
      return;
    }
    google::protobuf::Message* entry = refl->AddMessage(&strct, fields_fd);
    const google::protobuf::Reflection* er = entry->GetReflection();
    er->SetString(entry, key_fd, ReadSpanString(k, mem));
    status = PackCelValueIntoJsonValue(v, *er->MutableMessage(entry, val_fd),
                                       mem, refs);
  };
  if (map_cv.kind == CEL_MAP_ARENA) {
    ForEachArenaMapEntry(
        map_cv, mem, [&](const CelValue& k, const CelValue& v, uint32_t /*i*/) {
          add_entry(k, v);
        });
    return status;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("PackStruct: source kind=", static_cast<int>(map_cv.kind),
                   " (expected CEL_MAP_ARENA)"));
}

// Populate a `google.protobuf.ListValue`'s `values` from a CEL list
// CelValue.  Each element is recursively packed into a
// `google.protobuf.Value`.
absl::Status PackListValue(const CelValue& list_cv,
                           google::protobuf::Message& lv, const MemoryView& mem,
                           const ExternrefTable* absl_nullable refs) {
  const google::protobuf::Descriptor* d = lv.GetDescriptor();
  const google::protobuf::Reflection* refl = lv.GetReflection();
  const google::protobuf::FieldDescriptor* values_fd =
      d != nullptr ? d->FindFieldByNumber(1) : nullptr;
  if (refl == nullptr || values_fd == nullptr || !values_fd->is_repeated()) {
    return absl::InternalError("PackListValue: ListValue missing `values`");
  }
  absl::Status status = absl::OkStatus();
  if (list_cv.kind == CEL_LIST_ARENA) {
    ForEachArenaListElement(
        list_cv, mem, [&](const CelValue& elem, uint32_t /*i*/) {
          if (!status.ok()) return;
          status = PackCelValueIntoJsonValue(
              elem, *refl->AddMessage(&lv, values_fd), mem, refs);
        });
    return status;
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "PackListValue: source kind=", static_cast<int>(list_cv.kind),
      " (expected CEL_LIST_ARENA)"));
}

absl::Status PackCelValueIntoJsonValue(
    const CelValue& value, google::protobuf::Message& out,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs) {
  const google::protobuf::Descriptor* d = out.GetDescriptor();
  const google::protobuf::Reflection* refl = out.GetReflection();
  // kind_case field numbers in google.protobuf.Value: 1 null_value,
  // 2 number_value, 3 string_value, 4 bool_value, 5 struct_value,
  // 6 list_value.
  auto field = [&](int n) {
    return d->FindFieldByNumber(n);
  };
  switch (value.kind) {
    case CEL_NULL:
      refl->SetEnumValue(&out, field(1), 0);  // NULL_VALUE
      return absl::OkStatus();
    case CEL_BOOL:
      refl->SetBool(&out, field(4), value.payload.b != 0);
      return absl::OkStatus();
    case CEL_INT:
      refl->SetDouble(&out, field(2), static_cast<double>(value.payload.i));
      return absl::OkStatus();
    case CEL_UINT:
      refl->SetDouble(&out, field(2), static_cast<double>(value.payload.u));
      return absl::OkStatus();
    case CEL_DOUBLE:
      refl->SetDouble(&out, field(2), value.payload.d);
      return absl::OkStatus();
    case CEL_STRING:
      refl->SetString(&out, field(3), ReadSpanString(value, mem));
      return absl::OkStatus();
    case CEL_LIST_ARENA:
      return PackListValue(value, *refl->MutableMessage(&out, field(6)), mem,
                           refs);
    case CEL_MAP_ARENA:
      return PackStruct(value, *refl->MutableMessage(&out, field(5)), mem,
                        refs);
    default:
      return absl::InvalidArgumentError(absl::StrCat(
          "PackCelValueIntoJsonValue: unsupported value kind ",
          static_cast<int>(value.kind), " for google.protobuf.Value"));
  }
}

// Pick the well-known inner FQN a CelValue is packed into before
// being wrapped in an Any.  Mirrors cel-cpp's to-Any conversion:
//
//   - int / uint / bytes → the matching wrapper message, so the value
//     round-trips as its CEL kind through the read-side wrapper peel
//     (a bare JSON Value would lose int/uint to double, and JSON has
//     no bytes type).
//   - list → google.protobuf.ListValue, map → google.protobuf.Struct
//     packed DIRECTLY (not nested inside a Value) — the corpus's
//     `complex/any_list_map` row pins `type_url=.../ListValue`.
//   - everything else (bool / double / string / null) → a bare
//     google.protobuf.Value.
absl::string_view AnyInnerFqn(uint32_t kind) {
  switch (kind) {
    case CEL_INT:
      return "google.protobuf.Int64Value";
    case CEL_UINT:
      return "google.protobuf.UInt64Value";
    case CEL_BYTES:
      return "google.protobuf.BytesValue";
    case CEL_LIST_ARENA:
    case CEL_LIST_HOST:
      return "google.protobuf.ListValue";
    case CEL_MAP_ARENA:
    case CEL_MAP_HOST:
      return "google.protobuf.Struct";
    default:
      return "google.protobuf.Value";
  }
}

// True for the inner FQNs that are filled by their dedicated packer
// rather than `SetWrapperInnerValue`'s value-field set.
bool IsAggregateInnerFqn(absl::string_view fqn) {
  return fqn == "google.protobuf.ListValue" ||
         fqn == "google.protobuf.Struct" || fqn == "google.protobuf.Value";
}

// Pack a non-message CelValue into the already-allocated `Any`
// message `any`.  int / uint / bytes wrap into the matching wrapper
// type (so the value round-trips as its CEL kind through the
// read-side wrapper peel); everything else wraps into a
// `google.protobuf.Value`.  Matches cel-cpp's to-Any conversion.
absl::Status PackAnyFromScalar(google::protobuf::Message& any,
                               const CelValue& value, const MemoryView& mem,
                               const ExternrefTable* absl_nullable refs) {
  const google::protobuf::DescriptorPool* pool =
      any.GetDescriptor()->file()->pool();
  const absl::string_view inner_fqn = AnyInnerFqn(value.kind);
  const google::protobuf::Descriptor* inner_desc =
      pool->FindMessageTypeByName(std::string(inner_fqn));
  if (inner_desc == nullptr) {
    return absl::InternalError(absl::StrCat("PackAnyFromScalar: ", inner_fqn,
                                            " not in descriptor pool"));
  }
  std::unique_ptr<google::protobuf::Message> inner = NewWktMessage(*inner_desc);
  if (inner == nullptr) {
    return absl::InternalError(
        absl::StrCat("PackAnyFromScalar: no prototype for ", inner_fqn));
  }
  if (IsAggregateInnerFqn(inner_fqn)) {
    // List → ListValue, map → Struct, scalar JSON kind → Value: route
    // through the shared WKT packer for the chosen inner type.
    if (auto s = MaybePackWktMessage(*inner, value, mem, refs);
        s.has_value() && !s->ok()) {
      return *std::move(s);
    }
  } else {
    // Wrapper: set the inner `value` field (number 1) from the scalar.
    const google::protobuf::Reflection* wr = inner->GetReflection();
    const google::protobuf::FieldDescriptor* vf =
        inner_desc->FindFieldByNumber(1);
    if (wr == nullptr || vf == nullptr) {
      return absl::InternalError(
          "PackAnyFromScalar: wrapper missing value field");
    }
    if (auto s =
            SetWrapperInnerValue(*wr, *inner, *vf, *inner_desc, value, mem);
        !s.ok()) {
      return s;
    }
  }
  return WriteMessageOrPack(&any, *inner);
}

// Dispatch on `target`'s descriptor FQN to pack a non-message
// CelValue into an already-allocated WKT message `target`.  Returns
// `nullopt` if `target` is not a packable WKT — the caller then
// falls through to its mismatch error.  Shared by the singular,
// repeated, and map message-set paths; each resolves `target` from
// the appropriate reflection mutator (MutableMessage / AddMessage).
std::optional<absl::Status> MaybePackWktMessage(
    google::protobuf::Message& target, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs) {
  const absl::string_view fqn = target.GetDescriptor()->full_name();
  const bool is_duration = fqn == "google.protobuf.Duration";
  const bool is_timestamp = fqn == "google.protobuf.Timestamp";
  if (is_duration || is_timestamp) {
    if ((is_duration && value.kind != CEL_DURATION) ||
        (is_timestamp && value.kind != CEL_TIMESTAMP)) {
      return std::nullopt;
    }
    return PackDurationOrTimestamp(
        target, is_duration ? value.payload.dur : value.payload.ts);
  }
  if (fqn == "google.protobuf.Value") {
    return PackCelValueIntoJsonValue(value, target, mem, refs);
  }
  if (fqn == "google.protobuf.Struct") {
    return PackStruct(value, target, mem, refs);
  }
  if (fqn == "google.protobuf.ListValue") {
    return PackListValue(value, target, mem, refs);
  }
  if (fqn == "google.protobuf.Any") {
    return PackAnyFromScalar(target, value, mem, refs);
  }
  return std::nullopt;
}

// Singular-field WKT pack: resolve a mutable submessage on the outer
// field and delegate to `MaybePackWktMessage`.  `nullopt` propagates
// (the field's message type isn't a packable WKT) — but only after
// confirming via FQN so we don't mutate the field for a non-match.
std::optional<absl::Status> MaybeSetWktMessageField(
    google::protobuf::Message& outer,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs) {
  const google::protobuf::Descriptor* mt = field.message_type();
  const absl::string_view fqn = mt != nullptr ? mt->full_name() : "";
  if (fqn != "google.protobuf.Duration" && fqn != "google.protobuf.Timestamp" &&
      fqn != "google.protobuf.Value" && fqn != "google.protobuf.Struct" &&
      fqn != "google.protobuf.ListValue" && fqn != "google.protobuf.Any") {
    return std::nullopt;
  }
  google::protobuf::Message* sub =
      outer.GetReflection()->MutableMessage(&outer, &field);
  return MaybePackWktMessage(*sub, value, mem, refs);
}

// Numeric / bool / string / enum arms of `AppendRepeatedFromCelValue`
// (every non-MESSAGE cpp_type).  Returns the append Status for a
// handled cpp_type, or `std::nullopt` for CPPTYPE_MESSAGE (caller
// handles).  Mirror of `SetScalarNumericField` + the string/enum arm
// but uses the `Add...` reflection family.  Single call site →
// inlines back.
std::optional<absl::Status> AppendRepeatedScalar(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const CelValue& cv,
    const MemoryView& mem) {
  using FD = google::protobuf::FieldDescriptor;
  auto mismatch = [&](absl::string_view ty) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: repeated `", field.name(), "` ", ty,
                     " element kind=", static_cast<int>(cv.kind)));
  };
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (cv.kind != CEL_BOOL) return mismatch("BOOL");
      refl.AddBool(&msg, &field, cv.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_INT32:
      if (cv.kind != CEL_INT) return mismatch("INT32");
      refl.AddInt32(&msg, &field, static_cast<int32_t>(cv.payload.i));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (cv.kind != CEL_INT) return mismatch("INT64");
      refl.AddInt64(&msg, &field, cv.payload.i);
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (cv.kind != CEL_UINT) return mismatch("UINT32");
      refl.AddUInt32(&msg, &field, static_cast<uint32_t>(cv.payload.u));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (cv.kind != CEL_UINT) return mismatch("UINT64");
      refl.AddUInt64(&msg, &field, cv.payload.u);
      return absl::OkStatus();
    case FD::CPPTYPE_FLOAT:
      if (cv.kind != CEL_DOUBLE) return mismatch("FLOAT");
      refl.AddFloat(&msg, &field, static_cast<float>(cv.payload.d));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (cv.kind != CEL_DOUBLE) return mismatch("DOUBLE");
      refl.AddDouble(&msg, &field, cv.payload.d);
      return absl::OkStatus();
    case FD::CPPTYPE_STRING: {
      const bool want_bytes = field.type() == FD::TYPE_BYTES;
      if (want_bytes ? cv.kind != CEL_BYTES : cv.kind != CEL_STRING) {
        return mismatch("STRING/BYTES");
      }
      refl.AddString(&msg, &field, ReadSpanString(cv, mem));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_ENUM:
      if (cv.kind != CEL_INT) return mismatch("ENUM");
      if (auto s = CheckInt32Range(cv.payload.i, field.name()); !s.ok()) {
        return s;
      }
      refl.AddEnumValue(&msg, &field, static_cast<int>(cv.payload.i));
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// CPPTYPE_MESSAGE arm of `AppendRepeatedFromCelValue`.  A `null`
// element is PRUNED (skipped) per the `set_null/repeated_*` corpus
// rows; a non-message element targeting a WKT field packs via the
// shared dispatch; otherwise the source must be a CEL_MESSAGE resolved
// through `refs` and copied/packed into a fresh AddMessage element.
// Single call site → inlines back.
absl::Status AppendRepeatedMessage(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const CelValue& cv,
    const MemoryView& mem, const ExternrefTable& refs) {
  // A `null` element of a message-typed repeated field is PRUNED
  // (skipped), not appended — `[timestamp(1), null]` round-trips
  // as `[timestamp(1)]` per the `set_null/repeated_*` corpus rows.
  if (cv.kind == CEL_NULL) {
    return absl::OkStatus();
  }
  // Non-message element targeting a WKT message field (Duration /
  // Timestamp / Value / Struct / ListValue / Any) — pack into a
  // fresh AddMessage element via the shared WKT dispatch.
  if (cv.kind != CEL_MESSAGE) {
    if (auto s =
            MaybePackWktMessage(*refl.AddMessage(&msg, &field), cv, mem, &refs);
        s.has_value()) {
      return *std::move(s);
    }
  }
  // Repeated-of-message: source element is CEL_MESSAGE pointing
  // at a HostMessageBacking that exposes its underlying Message*.
  // We CopyFrom into a fresh `AddMessage` submessage so the
  // outer field carries an independent copy (the source backing
  // may be freed at ExternrefTable::Reset between Evals).
  if (cv.kind != CEL_MESSAGE) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: repeated message `", field.name(),
                     "` element kind=", static_cast<int>(cv.kind)));
  }
  const HostMessageBacking* src = refs.Lookup(cv.payload.msg_slot);
  if (src == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: repeated message `", field.name(),
                     "` element has no externref entry"));
  }
  const google::protobuf::Message* src_msg = src->message();
  if (src_msg == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: repeated message `", field.name(),
                     "` element backing has no proto message"));
  }
  google::protobuf::Message* dst = refl.AddMessage(&msg, &field);
  return WriteMessageOrPack(dst, *src_msg);
}

// Append one element to a repeated field from an arena-source
// CelValue.  Per-cpp_type dispatch mirrors `SetScalarField` but
// uses the `Add...` reflection family instead of `Set...`.
// CPPTYPE_MESSAGE elements expect the source to be a CEL_MESSAGE
// pointing at a HostMessageBacking — we CopyFrom into a fresh
// `AddMessage` submessage.
absl::Status AppendRepeatedFromCelValue(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const CelValue& cv,
    const MemoryView& mem, const ExternrefTable& refs) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = AppendRepeatedScalar(msg, field, refl, cv, mem); s.has_value()) {
    return *std::move(s);
  }
  if (field.cpp_type() == FD::CPPTYPE_MESSAGE) {
    return AppendRepeatedMessage(msg, field, refl, cv, mem, refs);
  }
  ABSL_CHECK(false) << "AppendRepeatedFromCelValue: unknown cpp_type "
                    << static_cast<int>(field.cpp_type());
  return absl::InternalError("unreachable");
}

// Non-MESSAGE arms of `AppendRepeatedFromHostListValue` — reads each
// element through `celwasm::Value`'s typed accessors (a failed
// accessor propagates its Status).  Returns `std::nullopt` for
// CPPTYPE_MESSAGE (caller handles).  Single call site → inlines back.
std::optional<absl::Status> AppendRepeatedHostScalar(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const celwasm::Value& v) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL: {
      auto b = v.AsBool();
      if (!b.ok()) return b.status();
      refl.AddBool(&msg, &field, *b);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT32: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      refl.AddInt32(&msg, &field, static_cast<int32_t>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT64: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      refl.AddInt64(&msg, &field, *i);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT32: {
      auto u = v.AsUint();
      if (!u.ok()) return u.status();
      refl.AddUInt32(&msg, &field, static_cast<uint32_t>(*u));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT64: {
      auto u = v.AsUint();
      if (!u.ok()) return u.status();
      refl.AddUInt64(&msg, &field, *u);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_FLOAT: {
      auto d = v.AsDouble();
      if (!d.ok()) return d.status();
      refl.AddFloat(&msg, &field, static_cast<float>(*d));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_DOUBLE: {
      auto d = v.AsDouble();
      if (!d.ok()) return d.status();
      refl.AddDouble(&msg, &field, *d);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_STRING: {
      auto s = (field.type() == FD::TYPE_BYTES) ? v.AsBytes() : v.AsString();
      if (!s.ok()) return s.status();
      refl.AddString(&msg, &field, std::string(*s));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_ENUM: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      if (auto s = CheckInt32Range(*i, field.name()); !s.ok()) {
        return s;
      }
      refl.AddEnumValue(&msg, &field, static_cast<int>(*i));
      return absl::OkStatus();
    }
    default:
      return std::nullopt;
  }
}

// Same as `AppendRepeatedFromCelValue` but the source element comes
// from a host-list backing as a `celwasm::Value` (Activation::Bind
// path).  Per-cpp_type dispatch reads via celwasm::Value's typed
// accessors instead of CelValue payloads + MemoryView.
absl::Status AppendRepeatedFromHostListValue(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const celwasm::Value& v) {
  using FD = google::protobuf::FieldDescriptor;
  using K = celwasm::Value::Kind;
  if (auto s = AppendRepeatedHostScalar(msg, field, refl, v); s.has_value()) {
    return *std::move(s);
  }
  if (field.cpp_type() == FD::CPPTYPE_MESSAGE) {
    if (v.kind() != K::kMessage) {
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: host-list repeated message `",
                       field.name(), "` element kind != kMessage"));
    }
    auto backing_or = v.MessageBacking();
    if (!backing_or.ok()) return backing_or.status();
    const google::protobuf::Message* src_msg = (*backing_or)->message();
    if (src_msg == nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: host-list repeated message `",
                       field.name(), "` backing has no proto message"));
    }
    google::protobuf::Message* dst = refl.AddMessage(&msg, &field);
    return WriteMessageOrPack(dst, *src_msg);
  }
  ABSL_CHECK(false) << "AppendRepeatedFromHostListValue: unknown cpp_type "
                    << static_cast<int>(field.cpp_type());
  return absl::InternalError("unreachable");
}

absl::Status SetRepeatedField(google::protobuf::Message& msg,
                              const google::protobuf::FieldDescriptor& field,
                              const CelValue& source, const MemoryView& mem,
                              const ExternrefTable& refs) {
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) {
    return absl::InternalError(
        "CelSetFieldImpl: repeated set: message has no reflection");
  }
  if (source.kind == CEL_LIST_ARENA) {
    absl::Status status = absl::OkStatus();
    ForEachArenaListElement(
        source, mem, [&](const CelValue& elem, uint32_t /*i*/) {
          if (!status.ok()) return;
          status =
              AppendRepeatedFromCelValue(msg, field, *refl, elem, mem, refs);
        });
    return status;
  }
  if (source.kind == CEL_LIST_HOST) {
    const HostListBacking* backing = refs.LookupList(source.payload.ref_slot);
    if (backing == nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                       "` host source has no externref entry"));
    }
    absl::Status status = absl::OkStatus();
    backing->ForEach([&](const celwasm::Value& v) {
      if (!status.ok()) return;
      status = AppendRepeatedFromHostListValue(msg, field, *refl, v);
    });
    return status;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                   "` source kind=", static_cast<int>(source.kind),
                   " (expected CEL_LIST_ARENA or CEL_LIST_HOST)"));
}

// Build one map entry submessage on `msg`'s map field.  The entry's
// key + value sub-fields are populated by recursive `SetScalarField`
// calls — each map field's entry message is a synthetic 2-field
// proto (descriptor.proto §"map_entry"); both sub-fields are
// singular scalars (or singular message for value).  Reusing
// `SetScalarField` for the dispatch shares the cpp_type table with
// the singular path; the key path naturally rejects map keys typed
// as message (proto disallows map<message,_>) at the descriptor
// level.
// Set the value sub-field of an arena-map entry whose value type is a
// message.  A non-message source either packs into a WKT value
// submessage (Duration / Timestamp / Value / Struct / ListValue / Any)
// or errors; a CEL_MESSAGE source is resolved through `refs` and
// copied/packed via `WriteMessageOrPack`.  Split from
// `InsertArenaMapEntry` so the parent stays under the function-size
// gate; single call site → inlines back.
absl::Status SetArenaMapEntryMessageValue(
    google::protobuf::Message& entry,
    const google::protobuf::FieldDescriptor& key_fd,
    const google::protobuf::FieldDescriptor& val_fd, const CelValue& val_cv,
    const MemoryView& mem, const ExternrefTable& refs) {
  // Non-message value targeting a WKT map value (Duration /
  // Timestamp / Value / Struct / ListValue / Any) — pack via the
  // shared WKT dispatch onto the entry's mutable value submessage.
  google::protobuf::Message* dst =
      entry.GetReflection()->MutableMessage(&entry, &val_fd);
  if (val_cv.kind != CEL_MESSAGE) {
    if (auto s = MaybePackWktMessage(*dst, val_cv, mem, &refs); s.has_value()) {
      return *std::move(s);
    }
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: map<", key_fd.name(), ", message `", val_fd.name(),
        "`> value kind=", static_cast<int>(val_cv.kind)));
  }
  const HostMessageBacking* src = refs.Lookup(val_cv.payload.msg_slot);
  if (src == nullptr || src->message() == nullptr) {
    return absl::InvalidArgumentError(
        "CelSetFieldImpl: map message-value source has no backing");
  }
  return WriteMessageOrPack(dst, *src->message());
}

absl::Status InsertArenaMapEntry(google::protobuf::Message& msg,
                                 const google::protobuf::FieldDescriptor& field,
                                 const google::protobuf::Reflection& refl,
                                 const CelValue& key_cv, const CelValue& val_cv,
                                 const MemoryView& mem,
                                 const ExternrefTable& refs) {
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(field, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(field, 2);
  const bool val_is_message =
      val_fd->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE;
  // A `null` value of a message-typed map field is PRUNED — the whole
  // entry is skipped, not inserted with a default value.  Matches the
  // `set_null/map_*_null_pruned` corpus rows.  Check before AddMessage
  // so no stray empty entry is left behind.
  if (val_is_message && val_cv.kind == CEL_NULL) {
    return absl::OkStatus();
  }
  google::protobuf::Message* entry = refl.AddMessage(&msg, &field);
  // Recursive scalar-set on the entry submessage.  For a value
  // typed as message we route through a `CopyFrom` via the same
  // CEL_MESSAGE-source path repeated-of-message uses.
  if (auto s = SetScalarField(*entry, *key_fd, key_cv, mem, &refs); !s.ok()) {
    return s;
  }
  if (val_is_message) {
    return SetArenaMapEntryMessageValue(*entry, *key_fd, *val_fd, val_cv, mem,
                                        refs);
  }
  return SetScalarField(*entry, *val_fd, val_cv, mem, &refs);
}

// Set the key sub-field of a host-map entry from a `celwasm::Value`.
// Map keys are a closed set (bool / int / uint / string) per
// descriptor.proto; any other cpp_type is rejected (defence — the
// descriptor wouldn't have legalised the field).  A failed typed
// accessor propagates its Status.  Single call site → inlines back.
absl::Status SetHostMapEntryKey(google::protobuf::Message& entry,
                                const google::protobuf::Reflection& entry_refl,
                                const google::protobuf::FieldDescriptor& key_fd,
                                const celwasm::Value& key) {
  using FD = google::protobuf::FieldDescriptor;
  switch (key_fd.cpp_type()) {
    case FD::CPPTYPE_INT32: {
      auto i = key.AsInt();
      if (!i.ok()) return i.status();
      entry_refl.SetInt32(&entry, &key_fd, static_cast<int32_t>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT64: {
      auto i = key.AsInt();
      if (!i.ok()) return i.status();
      entry_refl.SetInt64(&entry, &key_fd, *i);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT32: {
      auto u = key.AsUint();
      if (!u.ok()) return u.status();
      entry_refl.SetUInt32(&entry, &key_fd, static_cast<uint32_t>(*u));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT64: {
      auto u = key.AsUint();
      if (!u.ok()) return u.status();
      entry_refl.SetUInt64(&entry, &key_fd, *u);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_BOOL: {
      auto b = key.AsBool();
      if (!b.ok()) return b.status();
      entry_refl.SetBool(&entry, &key_fd, *b);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_STRING: {
      auto s = key.AsString();
      if (!s.ok()) return s.status();
      entry_refl.SetString(&entry, &key_fd, std::string(*s));
      return absl::OkStatus();
    }
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: map key cpp_type ",
                       static_cast<int>(key_fd.cpp_type()), " not allowed"));
  }
}

// Set the value sub-field of a host-map entry from a `celwasm::Value`.
// Every cpp_type is allowed (per descriptor.proto); message values
// resolve their backing and copy/pack via `WriteMessageOrPack`.  A
// failed typed accessor propagates its Status.  Single call site →
// inlines back.
absl::Status SetHostMapEntryValue(
    google::protobuf::Message& entry,
    const google::protobuf::Reflection& entry_refl,
    const google::protobuf::FieldDescriptor& val_fd,
    const celwasm::Value& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (val_fd.cpp_type()) {
    case FD::CPPTYPE_BOOL: {
      auto b = value.AsBool();
      if (!b.ok()) return b.status();
      entry_refl.SetBool(&entry, &val_fd, *b);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT32: {
      auto i = value.AsInt();
      if (!i.ok()) return i.status();
      entry_refl.SetInt32(&entry, &val_fd, static_cast<int32_t>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT64: {
      auto i = value.AsInt();
      if (!i.ok()) return i.status();
      entry_refl.SetInt64(&entry, &val_fd, *i);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT32: {
      auto u = value.AsUint();
      if (!u.ok()) return u.status();
      entry_refl.SetUInt32(&entry, &val_fd, static_cast<uint32_t>(*u));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT64: {
      auto u = value.AsUint();
      if (!u.ok()) return u.status();
      entry_refl.SetUInt64(&entry, &val_fd, *u);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_FLOAT: {
      auto d = value.AsDouble();
      if (!d.ok()) return d.status();
      entry_refl.SetFloat(&entry, &val_fd, static_cast<float>(*d));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_DOUBLE: {
      auto d = value.AsDouble();
      if (!d.ok()) return d.status();
      entry_refl.SetDouble(&entry, &val_fd, *d);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_STRING: {
      auto s = (val_fd.type() == FD::TYPE_BYTES) ? value.AsBytes()
                                                 : value.AsString();
      if (!s.ok()) return s.status();
      entry_refl.SetString(&entry, &val_fd, std::string(*s));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_ENUM: {
      auto i = value.AsInt();
      if (!i.ok()) return i.status();
      if (auto s = CheckInt32Range(*i, val_fd.name()); !s.ok()) {
        return s;
      }
      entry_refl.SetEnumValue(&entry, &val_fd, static_cast<int>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_MESSAGE: {
      auto backing_or = value.MessageBacking();
      if (!backing_or.ok()) return backing_or.status();
      const google::protobuf::Message* src_msg = (*backing_or)->message();
      if (src_msg == nullptr) {
        return absl::InvalidArgumentError(
            "CelSetFieldImpl: map message-value backing has no proto");
      }
      google::protobuf::Message* dst =
          entry_refl.MutableMessage(&entry, &val_fd);
      return WriteMessageOrPack(dst, *src_msg);
    }
  }
  ABSL_CHECK(false) << "InsertHostMapEntry: unknown value cpp_type "
                    << static_cast<int>(val_fd.cpp_type());
  return absl::InternalError("unreachable");
}

absl::Status InsertHostMapEntry(google::protobuf::Message& msg,
                                const google::protobuf::FieldDescriptor& field,
                                const google::protobuf::Reflection& refl,
                                const celwasm::Value& key,
                                const celwasm::Value& value) {
  google::protobuf::Message* entry = refl.AddMessage(&msg, &field);
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(field, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(field, 2);
  // The entry submessage's key/value sub-fields are *Set* (singular),
  // not *Add* (repeated) — so we dispatch through the two host-value
  // helpers rather than reusing `AppendRepeatedFromHostListValue`.
  // (Converting celwasm::Value → CelValue + reusing SetScalarField
  // would need arena bytes for strings, which we don't have here.)
  const google::protobuf::Reflection* entry_refl = entry->GetReflection();
  if (auto s = SetHostMapEntryKey(*entry, *entry_refl, *key_fd, key); !s.ok()) {
    return s;
  }
  return SetHostMapEntryValue(*entry, *entry_refl, *val_fd, value);
}

absl::Status SetMapField(google::protobuf::Message& msg,
                         const google::protobuf::FieldDescriptor& field,
                         const CelValue& source, const MemoryView& mem,
                         const ExternrefTable& refs) {
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) {
    return absl::InternalError(
        "CelSetFieldImpl: map set: message has no reflection");
  }
  if (source.kind == CEL_MAP_ARENA) {
    absl::Status status = absl::OkStatus();
    ForEachArenaMapEntry(
        source, mem, [&](const CelValue& k, const CelValue& v, uint32_t /*i*/) {
          if (!status.ok()) return;
          status = InsertArenaMapEntry(msg, field, *refl, k, v, mem, refs);
        });
    return status;
  }
  if (source.kind == CEL_MAP_HOST) {
    const HostMapBacking* backing = refs.LookupMap(source.payload.ref_slot);
    if (backing == nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: map `", field.name(),
                       "` host source has no externref entry"));
    }
    absl::Status status = absl::OkStatus();
    backing->ForEach([&](const celwasm::Value& k, const celwasm::Value& v) {
      if (!status.ok()) return;
      status = InsertHostMapEntry(msg, field, *refl, k, v);
    });
    return status;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("CelSetFieldImpl: map `", field.name(),
                   "` source kind=", static_cast<int>(source.kind),
                   " (expected CEL_MAP_ARENA or CEL_MAP_HOST)"));
}

}  // namespace

// Resolve the mutable `Message*` a `cel_set_field` call targets from
// its `msg_slot` CelValue.  On success `*out_msg` is the mutable proto
// and the returned optional is empty; a non-empty optional carries the
// terminal Status the trampoline returns directly (OkStatus for the
// poison short-circuit, InvalidArgument for a kind / backing / mutability
// mismatch).  Split from `CelSetFieldImpl` so the resolve preamble and
// the per-kind set dispatch live in separately-reviewable functions;
// single call site → inlines back.
std::optional<absl::Status> ResolveOwnedSetTarget(
    const CelValue& msg_cv, const ExternrefTable& refs,
    google::protobuf::Message** absl_nonnull out_msg) {
  // Poison propagation: a prior field-set on this same message slot
  // overflowed and wrote a CEL_ERROR there.  Leave it untouched and
  // no-op so the error rides the construction's result slot out to the
  // expression value — cel-cpp likewise short-circuits a struct
  // constructor once one argument errors.  This is the read side of the
  // poison contract; see the out-of-range tail below.
  if (msg_cv.kind == CEL_ERROR) {
    return absl::OkStatus();
  }
  if (msg_cv.kind != CEL_MESSAGE) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: msg_slot kind is ",
                     static_cast<int>(msg_cv.kind), " (expected CEL_MESSAGE)"));
  }
  const HostMessageBacking* backing = refs.Lookup(msg_cv.payload.msg_slot);
  if (backing == nullptr) {
    return absl::InvalidArgumentError(
        "CelSetFieldImpl: msg_slot has no externref entry");
  }
  // Only OwnedProtoBacking is mutable through this path — the
  // host-bound `ProtoBacking` (Activation-bound messages) wraps a
  // non-const `const Message*` and must not be mutated.  A
  // dynamic_cast distinguishes; mismatch is a checker regression
  // (a `Foo{...}` literal must construct a fresh OwnedProtoBacking,
  // never feed an Activation::Bind binding through here).
  //
  // The const-cast is required because `ExternrefTable::Lookup`
  // returns a `const HostMessageBacking*` for read-side use, but
  // `cel_set_field` owns mutating writes to OwnedProtoBacking's
  // wrapped proto.  Lookup is the only API surface that hands out
  // the backing; widening the table return type to non-const would
  // leak mutability into every read site.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto* owned_backing = const_cast<OwnedProtoBacking*>(
      dynamic_cast<const OwnedProtoBacking*>(backing));
  if (owned_backing == nullptr) {
    return absl::InvalidArgumentError(
        "CelSetFieldImpl: msg_slot points at a non-owned message backing "
        "(can't mutate host-bound proto messages through cel_set_field)");
  }
  google::protobuf::Message* msg = owned_backing->mutable_message();
  ABSL_CHECK(msg != nullptr)
      << "CelSetFieldImpl: OwnedProtoBacking has null msg";
  *out_msg = msg;
  return std::nullopt;
}

absl::Status CelSetFieldImpl(uint32_t msg_slot, uint32_t field_ref_id,
                             uint32_t value_slot,
                             const TrampolineContext& ctx) {
  const CelValue msg_cv = ctx.mem.ReadCelValue(msg_slot);
  google::protobuf::Message* msg = nullptr;
  if (auto terminal = ResolveOwnedSetTarget(msg_cv, ctx.refs, &msg);
      terminal.has_value()) {
    return *std::move(terminal);
  }

  const FieldRefEntry* field_ref = ResolveFieldRef(ctx.bindings, field_ref_id);
  if (field_ref == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: field_ref_id=", field_ref_id, " out of range"));
  }
  const google::protobuf::FieldDescriptor* field = ResolveFieldDescriptor(
      *msg, field_ref->field_number, field_ref->field_name);
  if (field == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: field `", field_ref->field_name,
                     "` not found on descriptor"));
  }

  const CelValue value_cv = ctx.mem.ReadCelValue(value_slot);

  // Route map / repeated source kinds through dedicated walkers.
  // Map check precedes repeated because every proto map field is
  // also `is_repeated()` per descriptor.proto.
  absl::Status set_status = [&]() -> absl::Status {
    if (field->is_map()) {
      return SetMapField(*msg, *field, value_cv, ctx.mem, ctx.refs);
    }
    if (field->is_repeated()) {
      return SetRepeatedField(*msg, *field, value_cv, ctx.mem, ctx.refs);
    }
    if (value_cv.kind == CEL_UNKNOWN || value_cv.kind == CEL_ERROR) {
      // 3VL on `Foo{a: <unknown>}` is unaddressed; surfacing as a
      // clean trap matches the "trust the checker" stance — a
      // properly-typed CEL program won't pass an Unknown / Error to
      // a typed scalar field.  Revisit when partial-eval ×
      // construction is exercised by a fixture row.
      return absl::UnimplementedError(absl::StrCat(
          "CelSetFieldImpl: 3VL value kind=", static_cast<int>(value_cv.kind),
          " on field set not yet supported"));
    }
    return SetScalarField(*msg, *field, value_cv, ctx.mem, &ctx.refs);
  }();

  // Poison-on-range-error (the write side of the poison contract).  An
  // out-of-range scalar/enum assignment is a CEL value-level error —
  // cel-cpp returns an ErrorValue, so the expression result is a CEL
  // error, not a host trap.  The field-write helpers signal this with
  // `kOutOfRange`; convert it to a CEL_ERROR poison stamped into
  // msg_slot, overwriting the partially-built message, and report
  // success.  The construction's trailing `(i32.const out_slot)` then
  // naturally carries the error.  Every other non-OK status is an
  // internal invariant violation (kind mismatch, missing reflection,
  // unsupported 3VL) and stays non-OK → trap, per the "a release build
  // that miscompiles silently is worse than one that crashes" rule.
  if (set_status.code() == absl::StatusCode::kOutOfRange) {
    WriteWireError(CEL_ERR_OVERFLOW, msg_slot, ctx.mem);
    return absl::OkStatus();
  }
  return set_status;
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

// ══════════════════════════════════════════════════════════════════
// Timestamp / duration parse + format kernels are now self-hosted in
// `runtime/cel_time_parse.cc`; codegen routes the four
// ids there directly.  See
// `doc/implementation-plan/rewrite/phase-c-plan.md` §4.

// `WriteInvalidArgumentError` moved to cel_host_error.cc (M11 Slice E).

// With-TZ accessor dispatch trampoline.
// ══════════════════════════════════════════════════════════════════
//
// Single host import absorbs all 10 with-TZ accessor surfaces; the
// per-accessor shims in cel_time.c supply the `accessor_kind`
// constant.  Wire enum lives in cel_time.h (`CelTzAccessorKind`)
// and is mirrored here — keep them in lockstep.  Rationale ("1
// dispatch trampoline vs 10 named trampolines": ABI surface count
// savings > switch-branch cost) lives in
// `doc/implementation-plan/rewrite/m7b-duration-timestamp.md`.

namespace {

// Mirrors `CelTzAccessorKind` in cel_time.h.  Append-only.
enum class TzAccessorKind : uint8_t {
  kYear = 0,
  kMonth = 1,
  kDayOfMonth1 = 2,
  kDayOfMonth = 3,
  kDayOfYear = 4,
  kDayOfWeek = 5,
  kHours = 6,
  kMinutes = 7,
  kSeconds = 8,
  kMilliseconds = 9,
};

int64_t ProjectCivilField(const absl::CivilSecond& cs, absl::Weekday weekday,
                          int day_of_year, int64_t ns_in_second,
                          TzAccessorKind kind) {
  switch (kind) {
    case TzAccessorKind::kYear:
      return cs.year();
    case TzAccessorKind::kMonth:
      return cs.month() - 1;  // cel-cpp 0-based
    case TzAccessorKind::kDayOfMonth1:
      return cs.day();  // 1-based
    case TzAccessorKind::kDayOfMonth:
      return cs.day() - 1;  // 0-based
    case TzAccessorKind::kDayOfYear:
      return day_of_year - 1;  // absl 1-based → cel-cpp 0-based
    case TzAccessorKind::kDayOfWeek:
      // absl::Weekday: monday=0..sunday=6.  cel-cpp: sunday=0..saturday=6.
      return (static_cast<int>(weekday) + 1) % 7;
    case TzAccessorKind::kHours:
      return cs.hour();
    case TzAccessorKind::kMinutes:
      return cs.minute();
    case TzAccessorKind::kSeconds:
      return cs.second();
    case TzAccessorKind::kMilliseconds: {
      // Sub-second within the civil second.  Sign-correlated nanos
      // get unix-floor-shifted to match cel-cpp's
      // `ToInt64Milliseconds(t - FloorToSecond(t))`.
      int64_t n = ns_in_second;
      if (n < 0) n += 1'000'000'000;
      return n / 1'000'000;
    }
  }
  return 0;  // unreachable; codegen never emits unknown kinds.
}

}  // namespace

namespace {

// Resolve a TZ string to an `absl::TimeZone`.  Three shapes:
//   - "UTC" / "Z" → UTC.
//   - "+HH:MM" / "-HH:MM" → fixed offset; absl::LoadTimeZone doesn't
//     parse these inline so we do it ourselves (plan §4.3).
//   - IANA name → absl::LoadTimeZone walks the host tzdata.
// Returns false on parse failure or unknown IANA name.
bool ResolveTimeZone(absl::string_view name, absl::TimeZone* out) {
  if (name == "UTC" || name == "Z") {
    *out = absl::UTCTimeZone();
    return true;
  }
  // Fixed offset: `+HH:MM` / `-HH:MM` / `HH:MM` (no sign = +).
  // cel-cpp admits the unsigned form per
  // `runtime/standard/time_functions.cc`.  Trim the sign prefix
  // if present, then validate HH:MM digit layout.
  int sign = 1;
  absl::string_view rest = name;
  if (!rest.empty() && (rest[0] == '+' || rest[0] == '-')) {
    sign = rest[0] == '+' ? 1 : -1;
    rest.remove_prefix(1);
  }
  if (rest.size() == 5 && rest[2] == ':' &&
      std::isdigit(static_cast<unsigned char>(rest[0])) &&
      std::isdigit(static_cast<unsigned char>(rest[1])) &&
      std::isdigit(static_cast<unsigned char>(rest[3])) &&
      std::isdigit(static_cast<unsigned char>(rest[4]))) {
    const int hours = ((rest[0] - '0') * 10) + (rest[1] - '0');
    const int minutes = ((rest[3] - '0') * 10) + (rest[4] - '0');
    if (hours > 23 || minutes > 59) return false;
    *out = absl::FixedTimeZone(sign * ((hours * 3600) + (minutes * 60)));
    return true;
  }
  return absl::LoadTimeZone(std::string(name), out);
}

// 3VL absorb + operand kind guards for the TZ-accessor trampoline.
// Returns true (and writes the result CelValue) if the call has
// already been short-circuited; false to continue.
bool TzAccessorPrelude(uint32_t out_slot, const CelValue& ts_cv,
                       const CelValue& tz_cv, uint32_t accessor_kind,
                       MemoryView& mem) {
  if (ts_cv.kind == CEL_ERROR || ts_cv.kind == CEL_UNKNOWN) {
    mem.WriteCelValue(out_slot, ts_cv);
    return true;
  }
  if (tz_cv.kind == CEL_ERROR || tz_cv.kind == CEL_UNKNOWN) {
    mem.WriteCelValue(out_slot, tz_cv);
    return true;
  }
  if (ts_cv.kind != CEL_TIMESTAMP || tz_cv.kind != CEL_STRING ||
      accessor_kind > static_cast<uint32_t>(TzAccessorKind::kMilliseconds)) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    mem.WriteCelValue(out_slot, err);
    return true;
  }
  return false;
}

}  // namespace

absl::Status CelTimestampTzAccessorImpl(uint32_t out_slot, uint32_t ts_slot,
                                        uint32_t tz_slot,
                                        uint32_t accessor_kind,
                                        const TrampolineContext& ctx) {
  CelValue ts_cv = ctx.mem.ReadCelValue(ts_slot);
  CelValue tz_cv = ctx.mem.ReadCelValue(tz_slot);
  if (TzAccessorPrelude(out_slot, ts_cv, tz_cv, accessor_kind, ctx.mem)) {
    return absl::OkStatus();
  }
  const absl::string_view tz_name =
      ctx.mem.ReadSpan(tz_cv.payload.s.ptr, tz_cv.payload.s.len);
  absl::TimeZone tz;
  if (!ResolveTimeZone(tz_name, &tz)) {
    WriteInvalidArgumentError(out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const absl::Time t = absl::UnixEpoch() +
                       absl::Seconds(ts_cv.payload.ts.seconds) +
                       absl::Nanoseconds(ts_cv.payload.ts.nanos);
  const absl::TimeZone::CivilInfo info = tz.At(t);
  const int day_of_year = absl::GetYearDay(absl::CivilDay(info.cs));
  const absl::Weekday weekday = absl::GetWeekday(absl::CivilDay(info.cs));
  const int64_t result =
      ProjectCivilField(info.cs, weekday, day_of_year, ts_cv.payload.ts.nanos,
                        static_cast<TzAccessorKind>(accessor_kind));
  CelValue out{};
  out.kind = CEL_INT;
  out.payload.i = result;
  ctx.mem.WriteCelValue(out_slot, out);
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

namespace celwasm {

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
