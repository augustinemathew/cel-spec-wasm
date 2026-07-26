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
#include "absl/strings/substitute.h"
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
  kAuthorPtr,    // customfn_T* — pointer carrier
  kRecordPtr,    // exports_<pkg>_<iface>_T* — record pointer
  kProtoPtr,     // customfn_list_u8_t* — proto bytes
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
      return "customfn_string_t";
    case K::kBytes:
      return "customfn_list_u8_t";
    case K::kNull:
      return "customfn_option_u8_t";
    case K::kList: {
      // Reuse the same suffix shape as cpp_codec_emitter.
      auto inner = AuthorCStruct(t.list_element[0]);
      // Replace `customfn_` and trailing `_t` to get the inner suffix.
      absl::string_view innerv = inner;
      if (absl::StartsWith(innerv, "customfn_"))
        innerv.remove_prefix(9);  // strip "customfn_"
      if (absl::EndsWith(innerv, "_t")) innerv.remove_suffix(2);
      return absl::StrCat("customfn_list_", innerv, "_t");
    }
    case K::kMap: {
      auto k = AuthorCStruct(t.map_kv[0]);
      auto v = AuthorCStruct(t.map_kv[1]);
      absl::string_view kv = k;
      if (absl::StartsWith(kv, "customfn_"))
        kv.remove_prefix(9);  // strip "customfn_"
      if (absl::EndsWith(kv, "_t")) kv.remove_suffix(2);
      absl::string_view vv = v;
      if (absl::StartsWith(vv, "customfn_"))
        vv.remove_prefix(9);  // strip "customfn_"
      if (absl::EndsWith(vv, "_t")) vv.remove_suffix(2);
      // For primitive list/map elements AuthorCStruct returns
      // "customfn_<scalar>_t" via a fallthrough below — handle that.
      return absl::StrCat("customfn_list_tuple2_", kv, "_", vv, "_t");
    }
    case K::kBool:
      return "customfn_bool_t";  // unused at top-level; only as list elt
    case K::kInt:
      return "customfn_s64_t";
    case K::kUint:
      return "customfn_u64_t";
    case K::kDouble:
      return "customfn_f64_t";
    case K::kProto:
      return "customfn_list_u8_t";  // same wire shape as bytes
    case K::kDuration:
    case K::kTimestamp:
    case K::kOptional:
    case K::kType:
      return "<unreachable>";
  }
  return "<unreachable>";
}

// Suffix used in customfn_list_<suffix>_t naming for list elements.
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

// ── Raw-string templates ──
//
// Per the m26 §3.5 template-pattern discipline (matches the
// wit + codec emitters): each emitted shape is a named
// `constexpr absl::string_view kFooTpl` raw-string here, and the
// emit fns SubstituteAndAppend with `$0`/`$1`... at the call site,
// so the C++ source mirrors the generated C++ directly.

// File-level preamble.
// $0 = optional block of #include "<extra>" lines (or empty).
constexpr absl::string_view kFilePreambleTpl =
    R"cpp(// Generated by `cel generate` — DO NOT EDIT.
          //
          // generated_stub.cc: routes each wit-bindgen-emitted export
          // through codec.h into the author's user_fns.cc impls.
          // Author never edits this file.

          $0 #include "customfn.h"
#include "codec.h"
#include "user_fns.h"

              extern "C" {
    )cpp";

// Scalar return: pass-through call.
// $0 = scalar C return type (bool / int64_t / uint64_t / double).
// $1 = export name (e.g. exports_cel_customfn_fns_fn_bool_bool).
// $2 = parameter signature ("bool x" / "int64_t a, int64_t b" / "").
// $3 = namespace::CamelFn (with leading "::" if global ns).
// $4 = comma-joined call args.
constexpr absl::string_view kExportScalarTpl =
    "$0 $1($2) {\n"
    "  return $3($4);\n"
    "}\n\n";

// Out-param return through the codec.
// $0 = export name.
// $1 = parameter signature including the trailing "<ret_type>* ret".
// $2 = namespace::CamelFn (with leading "::" if global ns).
// $3 = comma-joined call args.
constexpr absl::string_view kExportCodecTpl =
    "void $0($1) {\n"
    "  $2::codec::lower(ret, $3($4));\n"
    "}\n\n";

// Out-param proto return (lower_proto<M> instead of lower).
// $0 = export name.
// $1 = parameter signature including the trailing
//      "customfn_list_u8_t* ret".
// $2 = author cpp namespace (e.g. "rules").
// $3 = qualified proto message type (e.g. "acme::User").
// $4 = namespace::CamelFn.
// $5 = comma-joined call args.
constexpr absl::string_view kExportProtoTpl =
    "void $0($1) {\n"
    "  $2::codec::lower_proto<$3>(ret, $4($5));\n"
    "}\n\n";

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
        param_decls.push_back(absl::StrCat("customfn_list_u8_t* ", p.name));
        call_args.push_back(
            absl::StrCat(ns, "::codec::lift_proto<",
                         absl::StrReplaceAll(p.type.proto_fqn, {{".", "::"}}),
                         ">(*", p.name, ")"));
        break;
    }
  }

  // Signature.
  const std::string sig_args = absl::StrJoin(param_decls, ", ");
  const std::string called_fn = absl::StrCat(ns, "::", fn_camel);
  const std::string call_arg_list = absl::StrJoin(call_args, ", ");

  std::string out;
  if (ret_c == Carrier::kScalarValue) {
    absl::SubstituteAndAppend(&out, kExportScalarTpl,
                              ScalarCType(d.return_type), export_name, sig_args,
                              called_fn, call_arg_list);
    return out;
  }

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
      ret_type = "customfn_list_u8_t";
      break;
    default:
      ret_type = "<unreachable>";
  }
  const std::string ret_arg =
      sig_args.empty() ? absl::StrCat(ret_type, "* ret")
                       : absl::StrCat(sig_args, ", ", ret_type, "* ret");
  if (ret_c == Carrier::kProtoPtr) {
    const std::string proto_cpp =
        absl::StrReplaceAll(d.return_type.proto_fqn, {{".", "::"}});
    absl::SubstituteAndAppend(&out, kExportProtoTpl, export_name, ret_arg, ns,
                              proto_cpp, called_fn, call_arg_list);
  } else {
    absl::SubstituteAndAppend(&out, kExportCodecTpl, export_name, ret_arg, ns,
                              called_fn, call_arg_list);
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

  std::string extras;
  for (const auto& inc : extra_includes) {
    absl::StrAppend(&extras, "#include \"", inc, "\"\n");
  }
  if (!extra_includes.empty()) absl::StrAppend(&extras, "\n");
  std::string out;
  absl::SubstituteAndAppend(&out, kFilePreambleTpl, extras);

  for (const auto& d : lib.decls()) {
    if (d.backend != CelfnDecl::Backend::kPlugin) continue;
    auto body = EmitOneExport(d, cpp_namespace, exports_prefix);
    if (!body.ok()) return body.status();
    absl::StrAppend(&out, *body);
  }
  absl::StrAppend(&out, "}  // extern \"C\"\n");
  return out;
}

}  // namespace celwasm::celfnc_emit
