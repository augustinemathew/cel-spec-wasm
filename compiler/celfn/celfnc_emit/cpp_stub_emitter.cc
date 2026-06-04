// celfnc_emit/cpp_stub_emitter — see header for scope.

#include "compiler/celfn/celfnc_emit/cpp_stub_emitter.h"

#include <cctype>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::celfnc_emit {
namespace {

// Same package normalization rule the codec emitter uses
// (m26 §3.5.1): `cel:customfn` → `cel_customfn`.
std::string NormalizePkg(absl::string_view p) {
  return absl::StrReplaceAll(p, {{":", "_"}});
}

// Top-level argument/return categories.  Drives signature shape.
enum class Carrier {
  kScalarValue,  // bool / int64 / uint64 / double — pass by value
  kAuthorPtr,    // author_T* — pointer carrier
  kRecordPtr,    // exports_<pkg>_<iface>_T* — record pointer
  kProtoPtr,     // author_list_u8_t* — proto bytes
};

Carrier CarrierFor(const CelfnType& t) {
  using K = CelfnType::Kind;
  switch (t.kind) {
    case K::kBool:
    case K::kInt:
    case K::kUint:
    case K::kDouble:
      return Carrier::kScalarValue;
    case K::kDuration:
    case K::kTimestamp:
      return Carrier::kRecordPtr;
    case K::kProto:
      return Carrier::kProtoPtr;
    default:
      return Carrier::kAuthorPtr;
  }
}

// C type of an arg/return for a scalar-passing carrier.
absl::string_view ScalarCType(const CelfnType& t) {
  using K = CelfnType::Kind;
  switch (t.kind) {
    case K::kBool:
      return "bool";
    case K::kInt:
      return "int64_t";
    case K::kUint:
      return "uint64_t";
    case K::kDouble:
      return "double";
    default:
      return "<unreachable>";
  }
}

// C struct name for an aggregate/string/bytes/list/map carrier.
// Mirrors cpp_codec_emitter's StructFor.
std::string AuthorCStruct(const CelfnType& t) {
  using K = CelfnType::Kind;
  switch (t.kind) {
    case K::kString:
      return "author_string_t";
    case K::kBytes:
      return "author_list_u8_t";
    case K::kNull:
      return "author_option_u8_t";
    case K::kList: {
      // Reuse the same suffix shape as cpp_codec_emitter.
      auto inner = AuthorCStruct(t.list_element[0]);
      // Replace `author_` and trailing `_t` to get the inner suffix.
      absl::string_view innerv = inner;
      if (absl::StartsWith(innerv, "author_")) innerv.remove_prefix(7);
      if (absl::EndsWith(innerv, "_t")) innerv.remove_suffix(2);
      return absl::StrCat("author_list_", innerv, "_t");
    }
    case K::kMap: {
      auto k = AuthorCStruct(t.map_kv[0]);
      auto v = AuthorCStruct(t.map_kv[1]);
      absl::string_view kv = k;
      if (absl::StartsWith(kv, "author_")) kv.remove_prefix(7);
      if (absl::EndsWith(kv, "_t")) kv.remove_suffix(2);
      absl::string_view vv = v;
      if (absl::StartsWith(vv, "author_")) vv.remove_prefix(7);
      if (absl::EndsWith(vv, "_t")) vv.remove_suffix(2);
      // For primitive list/map elements AuthorCStruct returns
      // "author_<scalar>_t" via a fallthrough below — handle that.
      return absl::StrCat("author_list_tuple2_", kv, "_", vv, "_t");
    }
    case K::kBool:
      return "author_bool_t";  // unused at top-level; only as list elt
    case K::kInt:
      return "author_s64_t";
    case K::kUint:
      return "author_u64_t";
    case K::kDouble:
      return "author_f64_t";
    case K::kProto:
      return "author_list_u8_t";  // same wire shape as bytes
    case K::kDuration:
    case K::kTimestamp:
    case K::kOptional:
    case K::kType:
      return "<unreachable>";
  }
  return "<unreachable>";
}

// Suffix used in author_list_<suffix>_t naming for list elements.
// For scalars, returns "s64" / "u64" / etc.  For complex types,
// mirrors cpp_codec_emitter's SuffixFor (string→"string",
// list<X>→"list_<X>", map<K,V>→"tuple2_<K>_<V>"... wait, the map
// inside list uses "tuple2_..." not "list_tuple2_..." because it's
// already wrapped in list<...>).
//
// Centralised here to avoid duplicating with codec; we deliberately
// inline it for clarity given the surface is small.

std::string RecordCType(const CelfnType& t, absl::string_view exports_prefix) {
  using K = CelfnType::Kind;
  switch (t.kind) {
    case K::kDuration:
      return absl::StrCat(exports_prefix, "duration_t");
    case K::kTimestamp:
      return absl::StrCat(exports_prefix, "timestamp_t");
    default:
      return "<unreachable>";
  }
}

// One emitted export-fn body.
absl::StatusOr<std::string> EmitOneExport(const CelfnDecl& d,
                                          absl::string_view ns,
                                          absl::string_view exports_prefix) {
  using K = CelfnType::Kind;
  if (d.return_type.kind == K::kOptional || d.return_type.kind == K::kType) {
    return absl::FailedPreconditionError(
        absl::StrCat("stub emitter saw permanently-rejected return kind for `",
                     d.fn_name, "`"));
  }
  for (const auto& p : d.params) {
    if (p.type.kind == K::kOptional || p.type.kind == K::kType) {
      return absl::FailedPreconditionError(
          absl::StrCat("stub emitter saw permanently-rejected param kind for `",
                       d.fn_name, "`.", p.name));
    }
  }

  const std::string fn_camel = SnakeToCamel(d.fn_name);
  const std::string export_name = absl::StrCat(exports_prefix, d.overload_id);
  const Carrier ret_c = CarrierFor(d.return_type);

  // Argument list pieces.
  std::vector<std::string> param_decls;
  std::vector<std::string> call_args;  // codec::lift(*p) or p
  param_decls.reserve(d.params.size());
  call_args.reserve(d.params.size());
  for (const auto& p : d.params) {
    Carrier c = CarrierFor(p.type);
    switch (c) {
      case Carrier::kScalarValue:
        param_decls.push_back(absl::StrCat(ScalarCType(p.type), " ", p.name));
        call_args.push_back(std::string(p.name));
        break;
      case Carrier::kAuthorPtr:
        param_decls.push_back(
            absl::StrCat(AuthorCStruct(p.type), "* ", p.name));
        call_args.push_back(absl::StrCat(ns, "::codec::lift(*", p.name, ")"));
        break;
      case Carrier::kRecordPtr:
        param_decls.push_back(
            absl::StrCat(RecordCType(p.type, exports_prefix), "* ", p.name));
        call_args.push_back(absl::StrCat(ns, "::codec::lift(*", p.name, ")"));
        break;
      case Carrier::kProtoPtr:
        param_decls.push_back(absl::StrCat("author_list_u8_t* ", p.name));
        call_args.push_back(
            absl::StrCat(ns, "::codec::lift_proto<",
                         absl::StrReplaceAll(p.type.proto_fqn, {{".", "::"}}),
                         ">(*", p.name, ")"));
        break;
    }
  }

  // Signature.
  std::string out;
  std::string sig_args = absl::StrJoin(param_decls, ", ");
  // Return shape.
  if (ret_c == Carrier::kScalarValue) {
    absl::StrAppend(&out, ScalarCType(d.return_type), " ", export_name, "(",
                    sig_args, ") {\n");
    absl::StrAppend(&out, "  return ", ns, "::", fn_camel, "(",
                    absl::StrJoin(call_args, ", "), ");\n}\n\n");
  } else {
    // Out-param shape.
    std::string ret_type;
    switch (ret_c) {
      case Carrier::kAuthorPtr:
        ret_type = AuthorCStruct(d.return_type);
        break;
      case Carrier::kRecordPtr:
        ret_type = RecordCType(d.return_type, exports_prefix);
        break;
      case Carrier::kProtoPtr:
        ret_type = "author_list_u8_t";
        break;
      default:
        ret_type = "<unreachable>";
    }
    const std::string ret_arg =
        sig_args.empty() ? absl::StrCat(ret_type, "* ret")
                         : absl::StrCat(sig_args, ", ", ret_type, "* ret");
    absl::StrAppend(&out, "void ", export_name, "(", ret_arg, ") {\n");
    if (ret_c == Carrier::kProtoPtr) {
      absl::StrAppend(
          &out, "  ", ns, "::codec::lower_proto<",
          absl::StrReplaceAll(d.return_type.proto_fqn, {{".", "::"}}),
          ">(ret, ", ns, "::", fn_camel, "(", absl::StrJoin(call_args, ", "),
          "));\n}\n\n");
    } else {
      absl::StrAppend(&out, "  ", ns, "::codec::lower(ret, ", ns,
                      "::", fn_camel, "(", absl::StrJoin(call_args, ", "),
                      "));\n}\n\n");
    }
  }
  return out;
}

}  // namespace

std::string SnakeToCamel(absl::string_view snake) {
  std::string r;
  r.reserve(snake.size());
  bool cap = true;
  for (char c : snake) {
    if (c == '_') {
      cap = true;
      continue;
    }
    r.push_back(cap ? static_cast<char>(std::toupper(c)) : c);
    cap = false;
  }
  return r;
}

absl::StatusOr<std::string> EmitStubCc(
    const FunctionLibrary& lib, absl::string_view cpp_namespace,
    absl::string_view wit_package_name,
    const std::vector<std::string>& extra_includes) {
  const std::string exports_prefix =
      absl::StrCat("exports_", NormalizePkg(wit_package_name), "_fns_");

  std::string out;
  absl::StrAppend(
      &out,
      "// Generated by `cel generate` — DO NOT EDIT.\n"
      "//\n"
      "// generated_stub.cc: routes each wit-bindgen-emitted export\n"
      "// through codec.h into the author's user_fns.cc impls.\n"
      "// Author never edits this file.\n\n");
  for (const auto& inc : extra_includes) {
    absl::StrAppend(&out, "#include \"", inc, "\"\n");
  }
  if (!extra_includes.empty()) absl::StrAppend(&out, "\n");
  absl::StrAppend(&out, "#include \"author.h\"\n");
  absl::StrAppend(&out, "#include \"codec.h\"\n");
  absl::StrAppend(&out, "#include \"user_fns.h\"\n\n");

  // The exports are extern "C" linkage so wit-bindgen's C-shaped
  // declarations link cleanly to our C++ bodies.
  absl::StrAppend(&out, "extern \"C\" {\n\n");

  for (const auto& d : lib.decls()) {
    if (d.backend != CelfnDecl::Backend::kForeignComponent) continue;
    auto body = EmitOneExport(d, cpp_namespace, exports_prefix);
    if (!body.ok()) return body.status();
    absl::StrAppend(&out, *body);
  }
  absl::StrAppend(&out, "}  // extern \"C\"\n");
  return out;
}

}  // namespace celwasm::celfnc_emit
