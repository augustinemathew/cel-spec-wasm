#include "abi/celfn_wire.h"

#include <string>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "compiler/celfn/function_library.h"

namespace celwasm {

namespace {

using ::celwasm::abi::FnType;

// Scalar (non-composite) kind mapping.  Composite kinds (kProto /
// kList / kMap / kOptional) are handled by the caller — reaching
// here with one is an invariant violation.
FnType::Kind WireKindForScalar(CelfnType::Kind kind) {
  switch (kind) {
    case CelfnType::Kind::kBool:
      return FnType::FN_KIND_BOOL;
    case CelfnType::Kind::kInt:
      return FnType::FN_KIND_INT;
    case CelfnType::Kind::kUint:
      return FnType::FN_KIND_UINT;
    case CelfnType::Kind::kDouble:
      return FnType::FN_KIND_DOUBLE;
    case CelfnType::Kind::kString:
      return FnType::FN_KIND_STRING;
    case CelfnType::Kind::kBytes:
      return FnType::FN_KIND_BYTES;
    case CelfnType::Kind::kNull:
      return FnType::FN_KIND_NULL;
    case CelfnType::Kind::kDuration:
      return FnType::FN_KIND_DURATION;
    case CelfnType::Kind::kTimestamp:
      return FnType::FN_KIND_TIMESTAMP;
    case CelfnType::Kind::kType:
      return FnType::FN_KIND_TYPE;
    case CelfnType::Kind::kProto:
    case CelfnType::Kind::kList:
    case CelfnType::Kind::kMap:
    case CelfnType::Kind::kOptional:
      break;
  }
  ABSL_CHECK(false) << "WireKindForScalar: composite CelfnType kind "
                    << static_cast<int>(kind)
                    << " reached the scalar mapping";
  return FnType::FN_KIND_UNSPECIFIED;
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::string RenderFnType(const FnType& type) {
  switch (type.kind()) {
    case FnType::FN_KIND_BOOL:
      return "bool";
    case FnType::FN_KIND_INT:
      return "int";
    case FnType::FN_KIND_UINT:
      return "uint";
    case FnType::FN_KIND_DOUBLE:
      return "double";
    case FnType::FN_KIND_STRING:
      return "string";
    case FnType::FN_KIND_BYTES:
      return "bytes";
    case FnType::FN_KIND_NULL:
      return "null";
    case FnType::FN_KIND_DURATION:
      return "Duration";
    case FnType::FN_KIND_TIMESTAMP:
      return "Timestamp";
    case FnType::FN_KIND_TYPE:
      return "type";
    case FnType::FN_KIND_PROTO:
      return absl::StrCat("proto(", type.proto_fqn(), ")");
    case FnType::FN_KIND_LIST:
      return absl::StrCat("list<",
                          type.params_size() > 0 ? RenderFnType(type.params(0))
                                                 : std::string("?"),
                          ">");
    case FnType::FN_KIND_MAP:
      return absl::StrCat(
          "map<",
          type.params_size() > 0 ? RenderFnType(type.params(0))
                                 : std::string("?"),
          ", ",
          type.params_size() > 1 ? RenderFnType(type.params(1))
                                 : std::string("?"),
          ">");
    case FnType::FN_KIND_OPTIONAL:
      return absl::StrCat("optional<",
                          type.params_size() > 0 ? RenderFnType(type.params(0))
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
FnType FnTypeFromCelfn(const CelfnType& type) {
  FnType wire;
  switch (type.kind) {
    case CelfnType::Kind::kProto:
      wire.set_kind(FnType::FN_KIND_PROTO);
      wire.set_proto_fqn(type.proto_fqn);
      return wire;
    case CelfnType::Kind::kList:
      ABSL_CHECK_EQ(type.list_element.size(), 1u)
          << "FnTypeFromCelfn: kList without exactly one element type";
      wire.set_kind(FnType::FN_KIND_LIST);
      *wire.add_params() = FnTypeFromCelfn(type.list_element[0]);
      return wire;
    case CelfnType::Kind::kMap:
      ABSL_CHECK_EQ(type.map_kv.size(), 2u)
          << "FnTypeFromCelfn: kMap without exactly [key, value] types";
      wire.set_kind(FnType::FN_KIND_MAP);
      *wire.add_params() = FnTypeFromCelfn(type.map_kv[0]);
      *wire.add_params() = FnTypeFromCelfn(type.map_kv[1]);
      return wire;
    case CelfnType::Kind::kOptional:
      ABSL_CHECK_EQ(type.optional_element.size(), 1u)
          << "FnTypeFromCelfn: kOptional without exactly one element type";
      wire.set_kind(FnType::FN_KIND_OPTIONAL);
      *wire.add_params() = FnTypeFromCelfn(type.optional_element[0]);
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
  ABSL_CHECK(false) << "FnTypeFromCelfn: unknown CelfnType kind "
                    << static_cast<int>(type.kind);
  return wire;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
bool FnTypeEquals(const FnType& a, const FnType& b) {
  if (a.kind() != b.kind()) return false;
  if (a.proto_fqn() != b.proto_fqn()) return false;
  if (a.params_size() != b.params_size()) return false;
  for (int i = 0; i < a.params_size(); ++i) {
    if (!FnTypeEquals(a.params(i), b.params(i))) return false;
  }
  return true;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
celwasm::abi::RequiredFunction RequiredFunctionFromDecl(
    const CelfnDecl& decl) {
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
    *row.add_param_types() = FnTypeFromCelfn(param.type);
  }
  *row.mutable_return_type() = FnTypeFromCelfn(decl.return_type);
  row.set_is_receiver(decl.is_receiver);
  return row;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::string RenderSignature(const celwasm::abi::RequiredFunction& fn) {
  std::vector<std::string> params;
  params.reserve(static_cast<size_t>(fn.param_types_size()));
  for (int i = 0; i < fn.param_types_size(); ++i) {
    std::string rendered = RenderFnType(fn.param_types(i));
    if (i == 0 && fn.is_receiver()) {
      rendered = absl::StrCat("this ", rendered);
    }
    params.push_back(std::move(rendered));
  }
  return absl::StrCat(RenderFnType(fn.return_type()), " ", fn.fn_name(), "(",
                      absl::StrJoin(params, ", "), ")");
}

}  // namespace celwasm
