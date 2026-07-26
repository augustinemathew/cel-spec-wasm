#include "abi/celfn_wire.h"

#include <string>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "compiler/celfn/function_library.h"
#include "shared/type.h"

namespace celwasm {

namespace {

using ::celwasm::abi::Type;

// Scalar (non-composite) kind mapping.  Composite kinds (kProto /
// kList / kMap / kOptional) are handled by the caller — reaching
// here with one is an invariant violation.
Type::Kind WireKindForScalar(CelfnType::Kind kind) {
  switch (kind) {
    case CelfnType::Kind::kBool:
      return Type::KIND_BOOL;
    case CelfnType::Kind::kInt:
      return Type::KIND_INT;
    case CelfnType::Kind::kUint:
      return Type::KIND_UINT;
    case CelfnType::Kind::kDouble:
      return Type::KIND_DOUBLE;
    case CelfnType::Kind::kString:
      return Type::KIND_STRING;
    case CelfnType::Kind::kBytes:
      return Type::KIND_BYTES;
    case CelfnType::Kind::kNull:
      return Type::KIND_NULL;
    case CelfnType::Kind::kDuration:
      return Type::KIND_DURATION;
    case CelfnType::Kind::kTimestamp:
      return Type::KIND_TIMESTAMP;
    case CelfnType::Kind::kType:
      return Type::KIND_TYPE;
    case CelfnType::Kind::kProto:
    case CelfnType::Kind::kList:
    case CelfnType::Kind::kMap:
    case CelfnType::Kind::kOptional:
      break;
  }
  ABSL_CHECK(false) << "WireKindForScalar: composite CelfnType kind "
                    << static_cast<int>(kind) << " reached the scalar mapping";
  return Type::KIND_UNSPECIFIED;
}

// Scalar (non-composite) kind mapping over the unified vocabulary.
// Composite kinds (kMessage / kList / kMap / kOptional) are handled
// by the caller — reaching here with one is an invariant violation.
Type::Kind WireKindForCelScalar(CelType::Kind kind) {
  switch (kind) {
    case CelType::Kind::kBool:
      return Type::KIND_BOOL;
    case CelType::Kind::kInt:
      return Type::KIND_INT;
    case CelType::Kind::kUint:
      return Type::KIND_UINT;
    case CelType::Kind::kDouble:
      return Type::KIND_DOUBLE;
    case CelType::Kind::kString:
      return Type::KIND_STRING;
    case CelType::Kind::kBytes:
      return Type::KIND_BYTES;
    case CelType::Kind::kNull:
      return Type::KIND_NULL;
    case CelType::Kind::kDuration:
      return Type::KIND_DURATION;
    case CelType::Kind::kTimestamp:
      return Type::KIND_TIMESTAMP;
    case CelType::Kind::kType:
      return Type::KIND_TYPE;
    case CelType::Kind::kUnknown:
    case CelType::Kind::kMessage:
    case CelType::Kind::kList:
    case CelType::Kind::kMap:
    case CelType::Kind::kOptional:
      break;
  }
  ABSL_CHECK(false) << "WireKindForCelScalar: non-scalar CelType kind `"
                    << CelTypeKindName(kind) << "` reached the scalar mapping";
  return Type::KIND_UNSPECIFIED;
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
Type TypeFromCelType(const CelType& type) {
  Type wire;
  switch (type.kind()) {
    case CelType::Kind::kMessage:
      wire.set_kind(Type::KIND_PROTO);
      wire.set_proto_fqn(std::string(type.message_fully_qualified_name()));
      return wire;
    case CelType::Kind::kList:
      wire.set_kind(Type::KIND_LIST);
      *wire.add_params() = TypeFromCelType(type.list_element());
      return wire;
    case CelType::Kind::kMap:
      wire.set_kind(Type::KIND_MAP);
      *wire.add_params() = TypeFromCelType(type.map_key());
      *wire.add_params() = TypeFromCelType(type.map_value());
      return wire;
    case CelType::Kind::kOptional:
      wire.set_kind(Type::KIND_OPTIONAL);
      *wire.add_params() = TypeFromCelType(type.optional_element());
      return wire;
    case CelType::Kind::kBool:
    case CelType::Kind::kInt:
    case CelType::Kind::kUint:
    case CelType::Kind::kDouble:
    case CelType::Kind::kString:
    case CelType::Kind::kBytes:
    case CelType::Kind::kNull:
    case CelType::Kind::kDuration:
    case CelType::Kind::kTimestamp:
    case CelType::Kind::kType:
      wire.set_kind(WireKindForCelScalar(type.kind()));
      return wire;
    case CelType::Kind::kUnknown:
      break;
  }
  ABSL_CHECK(false) << "TypeFromCelType: kUnknown CelType (default-constructed "
                       "sentinel) has no wire spelling";
  return wire;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::string RenderType(const Type& type) {
  switch (type.kind()) {
    case Type::KIND_BOOL:
      return "bool";
    case Type::KIND_INT:
      return "int";
    case Type::KIND_UINT:
      return "uint";
    case Type::KIND_DOUBLE:
      return "double";
    case Type::KIND_STRING:
      return "string";
    case Type::KIND_BYTES:
      return "bytes";
    case Type::KIND_NULL:
      return "null";
    case Type::KIND_DURATION:
      return "Duration";
    case Type::KIND_TIMESTAMP:
      return "Timestamp";
    case Type::KIND_TYPE:
      return "type";
    case Type::KIND_PROTO:
      return absl::StrCat("proto(", type.proto_fqn(), ")");
    case Type::KIND_LIST:
      return absl::StrCat("list<",
                          type.params_size() > 0 ? RenderType(type.params(0))
                                                 : std::string("?"),
                          ">");
    case Type::KIND_MAP:
      return absl::StrCat("map<",
                          type.params_size() > 0 ? RenderType(type.params(0))
                                                 : std::string("?"),
                          ", ",
                          type.params_size() > 1 ? RenderType(type.params(1))
                                                 : std::string("?"),
                          ">");
    case Type::KIND_OPTIONAL:
      return absl::StrCat("optional<",
                          type.params_size() > 0 ? RenderType(type.params(0))
                                                 : std::string("?"),
                          ">");
    default:
      // Open-set wire data: an unknown kind renders numerically, it
      // is never rejected (this switch is over untrusted wire bytes,
      // so the default is a legitimate arm — see the FormatDirective
      // precedent in eval/host/cel_log.cc).
      return absl::StrCat("<kind ", static_cast<int>(type.kind()), ">");
  }
}

// Public declarations live in celfn_wire.h; clang-tidy's include
// path for the header is incomplete in compile_commands.json and it
// mistakes these for static candidates.
// NOLINTNEXTLINE(misc-use-internal-linkage)
Type TypeFromCelfn(const CelfnType& type) {
  Type wire;
  switch (type.kind) {
    case CelfnType::Kind::kProto:
      wire.set_kind(Type::KIND_PROTO);
      wire.set_proto_fqn(type.proto_fqn);
      return wire;
    case CelfnType::Kind::kList:
      ABSL_CHECK_EQ(type.list_element.size(), 1u)
          << "TypeFromCelfn: kList without exactly one element type";
      wire.set_kind(Type::KIND_LIST);
      *wire.add_params() = TypeFromCelfn(type.list_element[0]);
      return wire;
    case CelfnType::Kind::kMap:
      ABSL_CHECK_EQ(type.map_kv.size(), 2u)
          << "TypeFromCelfn: kMap without exactly [key, value] types";
      wire.set_kind(Type::KIND_MAP);
      *wire.add_params() = TypeFromCelfn(type.map_kv[0]);
      *wire.add_params() = TypeFromCelfn(type.map_kv[1]);
      return wire;
    case CelfnType::Kind::kOptional:
      ABSL_CHECK_EQ(type.optional_element.size(), 1u)
          << "TypeFromCelfn: kOptional without exactly one element type";
      wire.set_kind(Type::KIND_OPTIONAL);
      *wire.add_params() = TypeFromCelfn(type.optional_element[0]);
      return wire;
    case CelfnType::Kind::kBool:
    case CelfnType::Kind::kInt:
    case CelfnType::Kind::kUint:
    case CelfnType::Kind::kDouble:
    case CelfnType::Kind::kString:
    case CelfnType::Kind::kBytes:
    case CelfnType::Kind::kNull:
    case CelfnType::Kind::kDuration:
    case CelfnType::Kind::kTimestamp:
    case CelfnType::Kind::kType:
      wire.set_kind(WireKindForScalar(type.kind));
      return wire;
  }
  ABSL_CHECK(false) << "TypeFromCelfn: unknown CelfnType kind "
                    << static_cast<int>(type.kind);
  return wire;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
bool TypeEquals(const Type& a, const Type& b) {
  if (a.kind() != b.kind()) return false;
  if (a.proto_fqn() != b.proto_fqn()) return false;
  if (a.params_size() != b.params_size()) return false;
  for (int i = 0; i < a.params_size(); ++i) {
    if (!TypeEquals(a.params(i), b.params(i))) return false;
  }
  return true;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
celwasm::abi::RequiredFunction RequiredFunctionFromDecl(const CelfnDecl& decl) {
  celwasm::abi::RequiredFunction row;
  row.set_overload_id(decl.overload_id);
  row.set_fn_name(decl.fn_name);
  switch (decl.backend) {
    case CelfnDecl::Backend::kHost:
      row.set_backend(celwasm::abi::RequiredFunction::HOST);
      break;
    case CelfnDecl::Backend::kPlugin:
      row.set_backend(celwasm::abi::RequiredFunction::PLUGIN);
      break;
    case CelfnDecl::Backend::kCelDefined:
      // kCelDefined decls import under their per-module alias, never
      // `cel_fn` — they have no wire backend and no RequiredFunction
      // row; reaching here is a caller invariant violation.
      ABSL_CHECK(false) << "RequiredFunctionFromDecl: kCelDefined decl `"
                        << decl.overload_id << "` has no cel_fn wire backend";
      break;
  }
  for (const CelfnParam& param : decl.params) {
    *row.add_param_types() = TypeFromCelfn(param.type);
  }
  *row.mutable_return_type() = TypeFromCelfn(decl.return_type);
  row.set_is_receiver(decl.is_receiver);
  return row;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::string RenderSignature(const celwasm::abi::RequiredFunction& fn) {
  std::vector<std::string> params;
  params.reserve(static_cast<size_t>(fn.param_types_size()));
  for (int i = 0; i < fn.param_types_size(); ++i) {
    std::string rendered = RenderType(fn.param_types(i));
    if (i == 0 && fn.is_receiver()) {
      rendered = absl::StrCat("this ", rendered);
    }
    params.push_back(std::move(rendered));
  }
  return absl::StrCat(RenderType(fn.return_type()), " ", fn.fn_name(), "(",
                      absl::StrJoin(params, ", "), ")");
}

}  // namespace celwasm
