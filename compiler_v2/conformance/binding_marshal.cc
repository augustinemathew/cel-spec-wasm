#include "compiler_v2/conformance/binding_marshal.h"

#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/eval.pb.h"
#include "cel/expr/value.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"

namespace celwasm::conformance {

using ProtoValue = ::cel::expr::Value;
using ProtoExprValue = ::cel::expr::ExprValue;
using ProtoType = ::cel::expr::Type;
using ProtoDecl = ::cel::expr::Decl;

absl::StatusOr<cel::Value> ValueFromProto(const ProtoValue& v) {
  switch (v.kind_case()) {
    case ProtoValue::kNullValue:
      return cel::Value::Null();
    case ProtoValue::kBoolValue:
      return cel::Value::Bool(v.bool_value());
    case ProtoValue::kInt64Value:
      return cel::Value::Int(v.int64_value());
    case ProtoValue::kUint64Value:
      return cel::Value::Uint(v.uint64_value());
    case ProtoValue::kDoubleValue:
      return cel::Value::Double(v.double_value());
    case ProtoValue::kStringValue:
      return cel::Value::String(v.string_value());
    case ProtoValue::kBytesValue:
      return cel::Value::Bytes(v.bytes_value());
    case ProtoValue::kEnumValue:
      // Enum binding is a kInt at the runtime level once a checker
      // resolves the enum's underlying primitive; M7 lights up the
      // proto-enum import path.  Until then, SKIP gracefully.
      return absl::UnimplementedError(
          "binding_marshal: enum_value bindings unimplemented (M7)");
    case ProtoValue::kObjectValue:
      return absl::UnimplementedError(
          "binding_marshal: object_value bindings unimplemented (M7)");
    case ProtoValue::kMapValue:
      return absl::UnimplementedError(
          "binding_marshal: map_value bindings unimplemented (M6)");
    case ProtoValue::kListValue:
      return absl::UnimplementedError(
          "binding_marshal: list_value bindings unimplemented (M6)");
    case ProtoValue::kTypeValue:
      return absl::UnimplementedError(
          "binding_marshal: type_value bindings unimplemented");
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
// with Unimplemented at the call site.
absl::StatusOr<std::string> PrimitiveSpec(ProtoType::PrimitiveType p) {
  switch (p) {
    case ProtoType::BOOL:
      return std::string("bool");
    case ProtoType::INT64:
      return std::string("int");
    case ProtoType::UINT64:
      return std::string("uint");
    case ProtoType::DOUBLE:
      return std::string("double");
    case ProtoType::STRING:
      return std::string("string");
    case ProtoType::BYTES:
      return std::string("bytes");
    case ProtoType::PRIMITIVE_TYPE_UNSPECIFIED:
      return absl::InvalidArgumentError(
          "binding_marshal: PrimitiveType is UNSPECIFIED");
  }
  ABSL_CHECK(false)
      << "binding_marshal: unhandled ProtoType::PrimitiveType = "
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
      return absl::UnimplementedError(
          "binding_marshal: message_type type_env unimplemented (M7)");
    case ProtoType::kFunction:
      return absl::UnimplementedError(
          "binding_marshal: function type_env unimplemented");
    case ProtoType::kAbstractType:
      return absl::UnimplementedError(
          "binding_marshal: abstract_type type_env unimplemented");
    case ProtoType::kTypeParam:
    case ProtoType::kType:
    case ProtoType::kError:
    case ProtoType::kDyn:
      return absl::UnimplementedError(
          "binding_marshal: dyn / type / type_param / error type_env "
          "unimplemented (out of static-subset)");
    case ProtoType::TYPE_KIND_NOT_SET:
      return absl::InvalidArgumentError(
          "binding_marshal: cel.expr.Type has no type_kind set");
  }
  ABSL_CHECK(false) << "binding_marshal: unhandled ProtoType::TypeKindCase = "
                    << static_cast<int>(t.type_kind_case());
}

}  // namespace

absl::StatusOr<std::string> VariableSpecFromDecl(const ProtoDecl& d) {
  if (d.decl_kind_case() != ProtoDecl::kIdent) {
    return absl::UnimplementedError(
        "binding_marshal: function decls unimplemented");
  }
  if (d.name().empty()) {
    return absl::InvalidArgumentError(
        "binding_marshal: Decl has empty name");
  }
  if (!d.ident().has_type()) {
    return absl::InvalidArgumentError(
        "binding_marshal: IdentDecl has no type");
  }
  auto frag = TypeSpecFragment(d.ident().type());
  if (!frag.ok()) return frag.status();
  return absl::StrCat(d.name(), ":", *frag);
}

absl::Status PopulateActivation(
    const cel::expr::conformance::test::SimpleTest& t, cel::Activation& act) {
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
        return absl::UnimplementedError(absl::StrCat(
            "binding_marshal: ExprValue.error binding for `", kv.first,
            "` unimplemented"));
      case ProtoExprValue::kUnknown:
        // Routing this requires a per-test expr-id → AttributeId map
        // the harness doesn't plumb today; future-work item in
        // README.md.
        return absl::UnimplementedError(absl::StrCat(
            "binding_marshal: ExprValue.unknown binding for `", kv.first,
            "` unimplemented"));
      case ProtoExprValue::KIND_NOT_SET:
        return absl::InvalidArgumentError(absl::StrCat(
            "binding_marshal: ExprValue for `", kv.first, "` has no kind set"));
    }
  }
  return absl::OkStatus();
}

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

// Mirror of `PrimitiveSpec` but producing a `cel::CelType` directly,
// for the `DeclareVariablesOnBuilder` path that doesn't want to
// round-trip through the spec-string parser.  Aggregates / WKTs all
// SKIP — the matching arms in `TypeSpecFragment` already gate them.
absl::StatusOr<cel::CelType> CelTypeFromPrimitive(
    ProtoType::PrimitiveType p) {
  switch (p) {
    case ProtoType::BOOL:
      return cel::CelType::Bool();
    case ProtoType::INT64:
      return cel::CelType::Int();
    case ProtoType::UINT64:
      return cel::CelType::Uint();
    case ProtoType::DOUBLE:
      return cel::CelType::Double();
    case ProtoType::STRING:
      return cel::CelType::String();
    case ProtoType::BYTES:
      return cel::CelType::Bytes();
    case ProtoType::PRIMITIVE_TYPE_UNSPECIFIED:
      return absl::InvalidArgumentError(
          "binding_marshal: PrimitiveType is UNSPECIFIED");
  }
  ABSL_CHECK(false)
      << "binding_marshal: unhandled ProtoType::PrimitiveType = "
      << static_cast<int>(p);
}

absl::StatusOr<cel::CelType> CelTypeFromProtoType(const ProtoType& t) {
  if (t.type_kind_case() == ProtoType::kPrimitive) {
    return CelTypeFromPrimitive(t.primitive());
  }
  // Mirror `TypeSpecFragment`'s rejection list — anything non-scalar
  // SKIPs.  Reusing the same helper would re-serialize then re-parse;
  // instead just consult the same kind tag and return Unimplemented.
  auto frag = TypeSpecFragment(t);
  if (frag.ok()) {
    // Scalar primitive that wasn't kPrimitive — defensive: should not
    // happen given the enumeration in TypeSpecFragment.
    ABSL_CHECK(false) << "binding_marshal: TypeSpecFragment OK for non-"
                         "primitive kind = "
                      << static_cast<int>(t.type_kind_case());
  }
  return frag.status();
}

}  // namespace

absl::Status DeclareVariablesOnBuilder(
    const cel::expr::conformance::test::SimpleTest& t,
    cel::Compiler::Builder& b) {
  for (const auto& d : t.type_env()) {
    if (d.decl_kind_case() != ProtoDecl::kIdent) {
      return absl::UnimplementedError(
          "binding_marshal: function decls unimplemented");
    }
    if (d.name().empty()) {
      return absl::InvalidArgumentError(
          "binding_marshal: Decl has empty name");
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
