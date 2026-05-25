#include "conformance/binding_marshal.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/eval.pb.h"
#include "cel/expr/value.pb.h"
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "shared/type.h"
#include "eval/value.h"
#include "google/protobuf/any.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"

namespace celwasm::conformance {

using ProtoValue = ::cel::expr::Value;
using ProtoExprValue = ::cel::expr::ExprValue;
using ProtoType = ::cel::expr::Type;
using ProtoDecl = ::cel::expr::Decl;

namespace {

// Strip `type.googleapis.com/` (or `type.googleprod.com/`) prefix from
// an Any's `type_url` to get the bare FQN.  Anything before the LAST
// `/` is namespace; the FQN is everything after it.
absl::string_view AnyTypeFqn(absl::string_view type_url) {
  const size_t slash = type_url.rfind('/');
  if (slash == absl::string_view::npos) return type_url;
  return type_url.substr(slash + 1);
}

}  // namespace

// Unpack an `Any`-shaped object_value into a heap-allocated proto.
// Caller's descriptor pool must have the type registered (the
// conformance harness force-links TestAllTypes via
// `ForceLinkFixtureDescriptors`).  Returns `OwnedMessage` ready to
// hand to `Activation::Bind` or `MessageDifferencer::Equals`.
// NOLINTNEXTLINE(misc-use-internal-linkage) — public via header.
absl::StatusOr<std::unique_ptr<google::protobuf::Message>> UnpackAny(
    const google::protobuf::Any& any) {
  const absl::string_view fqn = AnyTypeFqn(any.type_url());
  const google::protobuf::DescriptorPool* pool =
      google::protobuf::DescriptorPool::generated_pool();
  const google::protobuf::Descriptor* desc =
      pool->FindMessageTypeByName(std::string(fqn));
  if (desc == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("binding_marshal: object_value type `", fqn,
                     "` not registered in generated descriptor pool"));
  }
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(desc);
  if (prototype == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "binding_marshal: generated_factory has no prototype for `", fqn, "`"));
  }
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  if (msg == nullptr) {
    return absl::InternalError(absl::StrCat(
        "binding_marshal: prototype->New() returned null for `", fqn, "`"));
  }
  if (!msg->ParseFromString(any.value())) {
    return absl::InvalidArgumentError(absl::StrCat(
        "binding_marshal: failed to parse Any payload for `", fqn, "`"));
  }
  return msg;
}

absl::StatusOr<celwasm::api::Value> ValueFromProto(const ProtoValue& v) {
  switch (v.kind_case()) {
    case ProtoValue::kNullValue:
      return celwasm::api::Value::Null();
    case ProtoValue::kBoolValue:
      return celwasm::api::Value::Bool(v.bool_value());
    case ProtoValue::kInt64Value:
      return celwasm::api::Value::Int(v.int64_value());
    case ProtoValue::kUint64Value:
      return celwasm::api::Value::Uint(v.uint64_value());
    case ProtoValue::kDoubleValue:
      return celwasm::api::Value::Double(v.double_value());
    case ProtoValue::kStringValue:
    case ProtoValue::kBytesValue: {
      // Spec semantics differ (string interns UTF-8, bytes is opaque),
      // but the marshaller treats both as opaque byte payloads.  One
      // arm collapses what `bugprone-branch-clone` would otherwise flag
      // as two identical-shaped `celwasm::api::Value::X(v.X_value())` cases.
      auto kind = v.kind_case();
      const std::string& bytes = (kind == ProtoValue::kStringValue)
                                     ? v.string_value()
                                     : v.bytes_value();
      return (kind == ProtoValue::kStringValue)
                 ? celwasm::api::Value::String(bytes)
                 : celwasm::api::Value::Bytes(bytes);
    }
    case ProtoValue::kEnumValue:
      // langdef §"Enumerated Types": enum values are spec-typed as
      // int.  `InlineConstantReferences` rewrites enum-name
      // resolution into Constant(int) at the AST level;
      // here we marshal an `enum_value`-typed binding the same way.
      return celwasm::api::Value::Int(v.enum_value().value());
    case ProtoValue::kObjectValue: {
      // Unpack the Any-style object_value into a fresh proto
      // wrapped in OwnedProtoBacking.  Same pattern the conformance
      // matcher uses on the read side (CompareMessage in runner.cc).
      auto msg_or = UnpackAny(v.object_value());
      if (!msg_or.ok()) return msg_or.status();
      return celwasm::api::Value::OwnedMessage(*std::move(msg_or));
    }
    case ProtoValue::kMapValue:
      return absl::UnimplementedError(
          "binding_marshal: map_value bindings unimplemented");
    case ProtoValue::kListValue:
      return absl::UnimplementedError(
          "binding_marshal: list_value bindings unimplemented");
    case ProtoValue::kTypeValue:
      // Type-value bindings — proto carries the spec type-name
      // string verbatim (`"int"`, `"bool"`, `"<msg-FQN>"`, ...).
      // No name validation; the read-side comparator does byte-equal
      // matching against the matcher.
      return celwasm::api::Value::Type(v.type_value());
    case ProtoValue::KIND_NOT_SET:
      return absl::InvalidArgumentError(
          "binding_marshal: cel.expr.Value has no kind set");
  }
  // Closed enum: any new oneof tag added upstream that we don't handle
  // is an invariant violation — fail loud.
  ABSL_CHECK(false) << "binding_marshal: unhandled ProtoValue::KindCase = "
                    << static_cast<int>(v.kind_case());
}

namespace {

// `name:type` spec for the closed set of primitive kinds the M2
// checker accepts.  Aggregate / wrapper / well-known kinds bail
// with Unimplemented at the call site.  Table-driven so that the
// six structurally-identical `case X: return std::string("y")` arms
// — which trip `bugprone-branch-clone` — collapse into one expression.
absl::StatusOr<std::string> PrimitiveSpec(ProtoType::PrimitiveType p) {
  static constexpr struct {
    ProtoType::PrimitiveType kind;
    const char* spec;
  } kTable[] = {
      {ProtoType::BOOL, "bool"},     {ProtoType::INT64, "int"},
      {ProtoType::UINT64, "uint"},   {ProtoType::DOUBLE, "double"},
      {ProtoType::STRING, "string"}, {ProtoType::BYTES, "bytes"},
  };
  for (const auto& row : kTable) {
    if (row.kind == p) return std::string(row.spec);
  }
  if (p == ProtoType::PRIMITIVE_TYPE_UNSPECIFIED) {
    return absl::InvalidArgumentError(
        "binding_marshal: PrimitiveType is UNSPECIFIED");
  }
  ABSL_CHECK(false) << "binding_marshal: unhandled ProtoType::PrimitiveType = "
                    << static_cast<int>(p);
}

// Decode an `IdentDecl.type` into a checker spec fragment (without
// the leading `name:`).  Scalar-only at M2; everything else SKIPs.
absl::StatusOr<std::string> TypeSpecFragment(const ProtoType& t) {
  switch (t.type_kind_case()) {
    case ProtoType::kPrimitive:
      return PrimitiveSpec(t.primitive());
    case ProtoType::kNull:
      // CEL spec carries `null_type` as a checker spec token, but the
      // M2 checker spec parser doesn't accept it as a variable
      // declaration shape.  SKIP.
      return absl::UnimplementedError(
          "binding_marshal: null type_env unimplemented");
    case ProtoType::kWrapper:
      return absl::UnimplementedError(
          "binding_marshal: wrapper type_env unimplemented (M7)");
    case ProtoType::kWellKnown:
      return absl::UnimplementedError(
          "binding_marshal: well_known type_env unimplemented");
    case ProtoType::kListType:
      return absl::UnimplementedError(
          "binding_marshal: list_type type_env unimplemented (M6)");
    case ProtoType::kMapType:
      return absl::UnimplementedError(
          "binding_marshal: map_type type_env unimplemented (M6)");
    case ProtoType::kMessageType:
      // M7: emit `<FQN>` so the checker spec parser routes through
      // ParseMessageType → DescriptorPool::FindMessageTypeByName.
      // Empty FQN is an invariant violation (the checker spec is
      // ill-formed); fail loud so it doesn't manifest as an opaque
      // checker error.
      if (t.message_type().empty()) {
        return absl::InvalidArgumentError(
            "binding_marshal: message_type with empty name");
      }
      return std::string(t.message_type());
    case ProtoType::kFunction:
      return absl::UnimplementedError(
          "binding_marshal: function type_env unimplemented");
    case ProtoType::kAbstractType:
      return absl::UnimplementedError(
          "binding_marshal: abstract_type type_env unimplemented");
    case ProtoType::kType:
      // `type` declared as a variable type — emit `type` as
      // the checker spec-string keyword.  `parse_and_check.cc::
      // ParsePrimitiveType` already maps `"type"` → `cel::TypeType`
      // via the cel-cpp checker's standard library registration.
      return std::string("type");
    case ProtoType::kTypeParam:
    case ProtoType::kError:
    case ProtoType::kDyn:
      return absl::UnimplementedError(
          "binding_marshal: dyn / type_param / error type_env "
          "unimplemented (out of static-subset)");
    case ProtoType::TYPE_KIND_NOT_SET:
      return absl::InvalidArgumentError(
          "binding_marshal: cel.expr.Type has no type_kind set");
  }
  ABSL_CHECK(false) << "binding_marshal: unhandled ProtoType::TypeKindCase = "
                    << static_cast<int>(t.type_kind_case());
}

}  // namespace

// Declared in `binding_marshal.h` and called from
// `binding_marshal_test.cc` + `runner.cc`; `misc-use-internal-linkage`
// can't see the cross-TU callers and would otherwise hide this from
// the header API.
// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::StatusOr<std::string> VariableSpecFromDecl(const ProtoDecl& d) {
  if (d.decl_kind_case() != ProtoDecl::kIdent) {
    return absl::UnimplementedError(
        "binding_marshal: function decls unimplemented");
  }
  if (d.name().empty()) {
    return absl::InvalidArgumentError("binding_marshal: Decl has empty name");
  }
  if (!d.ident().has_type()) {
    return absl::InvalidArgumentError("binding_marshal: IdentDecl has no type");
  }
  auto frag = TypeSpecFragment(d.ident().type());
  if (!frag.ok()) return frag.status();
  return absl::StrCat(d.name(), ":", *frag);
}

absl::Status PopulateActivation(
    const cel::expr::conformance::test::SimpleTest& t,
    celwasm::api::Activation& act) {
  for (const auto& kv : t.bindings()) {
    const ProtoExprValue& ev = kv.second;
    switch (ev.kind_case()) {
      case ProtoExprValue::kValue: {
        auto val_or = ValueFromProto(ev.value());
        if (!val_or.ok()) return val_or.status();
        act.Bind(kv.first, *std::move(val_or));
        break;
      }
      case ProtoExprValue::kError:
        return absl::UnimplementedError(
            absl::StrCat("binding_marshal: ExprValue.error binding for `",
                         kv.first, "` unimplemented"));
      case ProtoExprValue::kUnknown:
        // Routing this requires a per-test expr-id → AttributeId map
        // the harness doesn't plumb today; future-work item in
        // README.md.
        return absl::UnimplementedError(
            absl::StrCat("binding_marshal: ExprValue.unknown binding for `",
                         kv.first, "` unimplemented"));
      case ProtoExprValue::KIND_NOT_SET:
        return absl::InvalidArgumentError(absl::StrCat(
            "binding_marshal: ExprValue for `", kv.first, "` has no kind set"));
    }
  }
  return absl::OkStatus();
}

// Declared in `binding_marshal.h` and called from `runner.cc`; see
// `VariableSpecFromDecl` above for the NOLINT rationale.
// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::Status PopulateVariableSpecs(
    const cel::expr::conformance::test::SimpleTest& t,
    std::vector<std::string>& out) {
  out.reserve(out.size() + t.type_env_size());
  for (const auto& d : t.type_env()) {
    auto spec_or = VariableSpecFromDecl(d);
    if (!spec_or.ok()) return spec_or.status();
    out.push_back(*std::move(spec_or));
  }
  return absl::OkStatus();
}

namespace {

// Mirror of `PrimitiveSpec` but producing a `celwasm::api::CelType` directly,
// for the `DeclareVariablesOnBuilder` path that doesn't want to
// round-trip through the spec-string parser.  Aggregates / WKTs all
// SKIP — the matching arms in `TypeSpecFragment` already gate them.
absl::StatusOr<celwasm::api::CelType> CelTypeFromPrimitive(
    ProtoType::PrimitiveType p) {
  switch (p) {
    case ProtoType::BOOL:
      return celwasm::api::CelType::Bool();
    case ProtoType::INT64:
      return celwasm::api::CelType::Int();
    case ProtoType::UINT64:
      return celwasm::api::CelType::Uint();
    case ProtoType::DOUBLE:
      return celwasm::api::CelType::Double();
    case ProtoType::STRING:
      return celwasm::api::CelType::String();
    case ProtoType::BYTES:
      return celwasm::api::CelType::Bytes();
    case ProtoType::PRIMITIVE_TYPE_UNSPECIFIED:
      return absl::InvalidArgumentError(
          "binding_marshal: PrimitiveType is UNSPECIFIED");
  }
  ABSL_CHECK(false) << "binding_marshal: unhandled ProtoType::PrimitiveType = "
                    << static_cast<int>(p);
}

absl::StatusOr<celwasm::api::CelType> CelTypeFromProtoType(const ProtoType& t) {
  if (t.type_kind_case() == ProtoType::kPrimitive) {
    return CelTypeFromPrimitive(t.primitive());
  }
  // M7: declare a message-typed variable through the typed CelType
  // surface — same FQN that `TypeSpecFragment` returns for the spec-
  // string path.
  if (t.type_kind_case() == ProtoType::kMessageType) {
    if (t.message_type().empty()) {
      return absl::InvalidArgumentError(
          "binding_marshal: message_type with empty name");
    }
    return celwasm::api::CelType::Message(t.message_type());
  }
  // `type` declarable as a variable type.  No payload to
  // unpack — the type-of-types is uninhabited as a distinct shape.
  if (t.type_kind_case() == ProtoType::kType) {
    return celwasm::api::CelType::Type();
  }
  // Anything else (list_type / map_type / wrapper / well_known / dyn /
  // ...) is still SKIP territory.  Re-use `TypeSpecFragment`'s
  // Unimplemented status for a single source of truth.
  auto frag = TypeSpecFragment(t);
  if (frag.ok()) {
    // Defensive: TypeSpecFragment returned OK for a kind we didn't
    // route here — invariant violation.
    ABSL_CHECK(false) << "binding_marshal: TypeSpecFragment OK for "
                         "unrouted kind = "
                      << static_cast<int>(t.type_kind_case());
  }
  return frag.status();
}

}  // namespace

absl::Status DeclareVariablesOnBuilder(
    const cel::expr::conformance::test::SimpleTest& t,
    celwasm::api::Compiler::Builder& b) {
  for (const auto& d : t.type_env()) {
    if (d.decl_kind_case() != ProtoDecl::kIdent) {
      return absl::UnimplementedError(
          "binding_marshal: function decls unimplemented");
    }
    if (d.name().empty()) {
      return absl::InvalidArgumentError("binding_marshal: Decl has empty name");
    }
    if (!d.ident().has_type()) {
      return absl::InvalidArgumentError(
          "binding_marshal: IdentDecl has no type");
    }
    auto ct_or = CelTypeFromProtoType(d.ident().type());
    if (!ct_or.ok()) return ct_or.status();
    b.DeclareVariable(d.name(), *ct_or);
  }
  return absl::OkStatus();
}

}  // namespace celwasm::conformance
