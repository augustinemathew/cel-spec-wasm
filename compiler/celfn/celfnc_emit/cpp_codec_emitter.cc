// celfnc_emit/cpp_codec_emitter — see header for scope.

#include "compiler/celfn/celfnc_emit/cpp_codec_emitter.h"

#include <set>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::celfnc_emit {
namespace {

// Author-side struct name for a CelfnType subtree, mirroring what
// wit-bindgen 0.57 emits (m26 §3.5.1, verified empirically via the
// probe at /tmp/witgen).  Records collapse to a single name per
// kind (kDuration → "duration", kTimestamp → "timestamp") because
// wit-bindgen emits the record type ONCE per interface.
struct StructName {
  // `author_list_s64_t` / `author_string_t` / `author_list_tuple2_…`
  // etc.  Empty for primitives that pass through unwrapped
  // (bool / int / uint / double).
  std::string c_name;
  // True for kProto carriers — they share their wire shape with
  // kBytes (`author_list_u8_t`) but the codec emits a TEMPLATE
  // `lift_proto` / `lower_proto` instead of the bytes overload set.
  bool is_proto = false;
};

// Argkind-of-types helper — recurses through list_element / map_kv,
// mirroring CelfnType::Argkind but synthesising the C struct name
// instead of the overload-id suffix.
std::string SuffixFor(const CelfnType& t) {
  using K = CelfnType::Kind;
  switch (t.kind) {
    case K::kBool:
      return "bool";
    case K::kInt:
      return "s64";
    case K::kUint:
      return "u64";
    case K::kDouble:
      return "f64";
    case K::kString:
      return "string";
    case K::kBytes:
      return "u8";  // list<u8>
    case K::kNull:
      return "u8";  // option<u8>
    case K::kList:
      return absl::StrCat("list_", SuffixFor(t.list_element[0]));
    case K::kMap:
      // m24 §6: map<K,V> wire shape is list<tuple<K,V>>; the
      // wit-bindgen C struct is `author_list_tuple2_<k>_<v>_t`,
      // NOT `author_tuple2_<k>_<v>_t` (which is the inner tuple
      // element type, used only as the element type of the outer
      // list).  Empirically verified against the /tmp/witgen probe
      // output during m26 design (m26 §3.5.1).
      return absl::StrCat("list_tuple2_", SuffixFor(t.map_kv[0]), "_",
                          SuffixFor(t.map_kv[1]));
    case K::kProto:
      return "u8";  // same shape as list<u8>
    case K::kDuration:
    case K::kTimestamp:
    case K::kOptional:
    case K::kType:
      // Records use a different naming scheme (exports_*_t).
      // optional / type never reach here in v1 — rejected upstream.
      return "<unreachable>";
  }
  return "<unreachable>";
}

// The WIT package-name normalization that wit-bindgen 0.57 uses for
// the exports_*_t prefix: replace `:` with `_`.  `cel:customfn` ->
// `cel_customfn`.
std::string NormalizePkgForExportsPrefix(absl::string_view pkg) {
  return absl::StrReplaceAll(pkg, {{":", "_"}});
}

// C struct name for a top-level type used by a decl.  Recursion happens
// in SuffixFor; this just wraps with the right prefix.
StructName StructFor(const CelfnType& t, absl::string_view exports_prefix) {
  using K = CelfnType::Kind;
  switch (t.kind) {
    case K::kBool:
    case K::kInt:
    case K::kUint:
    case K::kDouble:
      return {/*c_name=*/"", false};  // pass-through
    case K::kString:
      return {"author_string_t", false};
    case K::kBytes:
      return {"author_list_u8_t", false};
    case K::kNull:
      // option<u8> uses author_option_u8_t for the struct (but the
      // export adapter uses pointer-as-maybe).  Codec emits an
      // overload for the struct form for completeness; the stub
      // chooses the pointer form.
      return {"author_option_u8_t", false};
    case K::kList:
    case K::kMap:
      return {absl::StrCat("author_", SuffixFor(t)) + "_t", false};
    case K::kProto:
      return {"author_list_u8_t", true};
    case K::kDuration:
      return {absl::StrCat(exports_prefix, "duration_t"), false};
    case K::kTimestamp:
      return {absl::StrCat(exports_prefix, "timestamp_t"), false};
    case K::kOptional:
    case K::kType:
      return {"<unreachable>", false};
  }
  return {"<unreachable>", false};
}

// C++ container-side type the codec lifts to / lowers from.
absl::StatusOr<std::string> CppTypeFor(const CelfnType& t) {
  using K = CelfnType::Kind;
  switch (t.kind) {
    case K::kBool:
      return std::string("bool");
    case K::kInt:
      return std::string("int64_t");
    case K::kUint:
      return std::string("uint64_t");
    case K::kDouble:
      return std::string("double");
    case K::kString:
      // String lifts as string_view (zero-copy view onto component
      // memory); lowers from string_view too (a fresh
      // author_string_dup_n copy goes to wasm memory).
      return std::string("std::string");  // container form (return type)
    case K::kBytes:
      return std::string("std::vector<uint8_t>");
    case K::kNull:
      return std::string("std::monostate");
    case K::kDuration:
      return std::string("absl::Duration");
    case K::kTimestamp:
      return std::string("absl::Time");
    case K::kList: {
      auto inner = CppTypeFor(t.list_element[0]);
      if (!inner.ok()) return inner.status();
      return absl::StrCat("std::vector<", *inner, ">");
    }
    case K::kMap: {
      auto k = CppTypeFor(t.map_kv[0]);
      auto v = CppTypeFor(t.map_kv[1]);
      if (!k.ok()) return k.status();
      if (!v.ok()) return v.status();
      return absl::StrCat("std::map<", *k, ", ", *v, ">");
    }
    case K::kProto:
      // Caller knows the proto type; codec uses lift_proto<M> template.
      return std::string("M");
    case K::kOptional:
    case K::kType:
      return absl::FailedPreconditionError(absl::StrCat(
          "codec emitter saw a permanently-rejected CelfnType::Kind (",
          (t.kind == K::kOptional ? "optional" : "type"),
          "); Builder::Build() should have rejected upstream"));
  }
  return absl::InternalError("CppTypeFor: unknown kind");
}

// Walk types, collecting unique non-pass-through types in topological
// order (inner-before-outer).  Uses string-keyed dedup to merge
// repeats across multiple decls.
class TypeCollector {
 public:
  void Visit(const CelfnType& t) {
    using K = CelfnType::Kind;
    switch (t.kind) {
      case K::kList:
        if (!t.list_element.empty()) Visit(t.list_element[0]);
        break;
      case K::kMap:
        if (t.map_kv.size() == 2) {
          Visit(t.map_kv[0]);
          Visit(t.map_kv[1]);
        }
        break;
      default:
        break;
    }
    // Add after inner — topological order.
    if (t.kind == K::kBool || t.kind == K::kInt || t.kind == K::kUint ||
        t.kind == K::kDouble) {
      return;  // pass-through, no codec needed
    }
    if (t.kind == K::kOptional || t.kind == K::kType) {
      // upstream-rejected; let the emit pass surface the error.
      return;
    }
    const std::string key = absl::StrCat(static_cast<int>(t.kind), ":",
                                         SuffixFor(t), ":", t.proto_fqn);
    if (seen_.insert(key).second) {
      ordered_.push_back(t);
    }
  }
  const std::vector<CelfnType>& ordered() const {
    return ordered_;
  }

 private:
  std::set<std::string> seen_;
  std::vector<CelfnType> ordered_;
};

// Emit lift+lower for a single type that needs them.
absl::Status EmitOne(const CelfnType& t, absl::string_view exports_prefix,
                     std::string* out) {
  using K = CelfnType::Kind;
  StructName s = StructFor(t, exports_prefix);
  auto cpp_or = CppTypeFor(t);
  if (!cpp_or.ok()) return cpp_or.status();
  const std::string& cpp = *cpp_or;

  switch (t.kind) {
    case K::kString:
      absl::StrAppend(out,
                      "inline std::string_view lift(const author_string_t& "
                      "s) {\n  return {reinterpret_cast<const "
                      "char*>(s.ptr), s.len};\n}\n\n");
      absl::StrAppend(out,
                      "inline void lower(author_string_t* ret, "
                      "std::string_view s) {\n"
                      "  author_string_dup_n(ret, s.data(), s.size());\n"
                      "}\n\n");
      return absl::OkStatus();

    case K::kBytes:
      absl::StrAppend(out,
                      "inline std::vector<uint8_t> lift(const "
                      "author_list_u8_t& l) {\n"
                      "  return {l.ptr, l.ptr + l.len};\n}\n\n");
      absl::StrAppend(
          out,
          "inline void lower(author_list_u8_t* ret, const "
          "std::vector<uint8_t>& v) {\n"
          "  ret->len = v.size();\n"
          "  ret->ptr = static_cast<uint8_t*>(cabi_realloc(\n"
          "      NULL, 0, 1, v.size()));\n"
          "  if (!v.empty()) std::memcpy(ret->ptr, v.data(), v.size());\n"
          "}\n\n");
      return absl::OkStatus();

    case K::kProto:
      // Template helpers, emitted once.  Caller (stub) provides M.
      absl::StrAppend(
          out,
          "template <typename M>\n"
          "inline M lift_proto(const author_list_u8_t& l) {\n"
          "  M msg;\n"
          "  if (l.len > 0) msg.ParseFromArray(l.ptr, l.len);\n"
          "  return msg;\n}\n\n");
      absl::StrAppend(
          out,
          "template <typename M>\n"
          "inline void lower_proto(author_list_u8_t* ret, const M& msg) {\n"
          "  std::string buf;\n"
          "  msg.SerializeToString(&buf);\n"
          "  ret->len = buf.size();\n"
          "  ret->ptr = static_cast<uint8_t*>(cabi_realloc(\n"
          "      NULL, 0, 1, buf.size()));\n"
          "  if (!buf.empty()) std::memcpy(ret->ptr, buf.data(), "
          "buf.size());\n"
          "}\n\n");
      return absl::OkStatus();

    case K::kDuration:
      absl::StrAppend(
          out, "inline absl::Duration lift(const ", s.c_name,
          "& r) {\n  return absl::Seconds(r.seconds) + "
          "absl::Nanoseconds(r.nanos);\n}\n\n");
      absl::StrAppend(
          out,
          "inline void lower(", s.c_name,
          "* ret, absl::Duration d) {\n"
          "  const int64_t sec = absl::ToInt64Seconds(d);\n"
          "  ret->seconds = sec;\n"
          "  ret->nanos = static_cast<int32_t>(\n"
          "      absl::ToInt64Nanoseconds(d - absl::Seconds(sec)));\n"
          "}\n\n");
      return absl::OkStatus();

    case K::kTimestamp:
      absl::StrAppend(
          out, "inline absl::Time lift(const ", s.c_name,
          "& r) {\n  return absl::FromUnixSeconds(r.seconds) + "
          "absl::Nanoseconds(r.nanos);\n}\n\n");
      absl::StrAppend(
          out,
          "inline void lower(", s.c_name,
          "* ret, absl::Time t) {\n"
          "  const int64_t sec = absl::ToUnixSeconds(t);\n"
          "  ret->seconds = sec;\n"
          "  ret->nanos = static_cast<int32_t>(\n"
          "      absl::ToInt64Nanoseconds(t - absl::FromUnixSeconds(sec)));\n"
          "}\n\n");
      return absl::OkStatus();

    case K::kList: {
      // list<T> → std::vector<C++ T>
      auto inner_cpp = CppTypeFor(t.list_element[0]);
      if (!inner_cpp.ok()) return inner_cpp.status();
      const auto& inner = t.list_element[0];
      if (inner.kind == K::kBool || inner.kind == K::kInt ||
          inner.kind == K::kUint || inner.kind == K::kDouble) {
        // Trivial flat copy.
        absl::StrAppend(out, "inline ", cpp, " lift(const ", s.c_name,
                        "& l) {\n  return {l.ptr, l.ptr + l.len};\n}\n\n");
        absl::StrAppend(out, "inline void lower(", s.c_name, "* ret, const ",
                        cpp,
                        "& v) {\n  ret->len = v.size();\n  ret->ptr = "
                        "static_cast<decltype(ret->ptr)>(cabi_realloc(\n      "
                        "NULL, 0, alignof(decltype(*ret->ptr)),\n      "
                        "v.size() * sizeof(*ret->ptr)));\n  if (!v.empty()) "
                        "std::memcpy(ret->ptr, v.data(),\n              "
                        "v.size() * sizeof(*ret->ptr));\n}\n\n");
      } else {
        // Recursive: call lift/lower for each element.
        absl::StrAppend(out, "inline ", cpp, " lift(const ", s.c_name,
                        "& l) {\n  ", cpp,
                        " r;\n  r.reserve(l.len);\n  for (size_t i = 0; i < "
                        "l.len; ++i) {\n    r.emplace_back(lift(l.ptr[i]));\n  "
                        "}\n  return r;\n}\n\n");
        absl::StrAppend(out, "inline void lower(", s.c_name, "* ret, const ",
                        cpp,
                        "& v) {\n  ret->len = v.size();\n  ret->ptr = "
                        "static_cast<decltype(ret->ptr)>(cabi_realloc(\n      "
                        "NULL, 0, alignof(decltype(*ret->ptr)),\n      "
                        "v.size() * sizeof(*ret->ptr)));\n  for (size_t i = 0;"
                        " i < v.size(); ++i) {\n    lower(&ret->ptr[i], "
                        "v[i]);\n  }\n}\n\n");
      }
      return absl::OkStatus();
    }

    case K::kMap: {
      auto k_cpp = CppTypeFor(t.map_kv[0]);
      auto v_cpp = CppTypeFor(t.map_kv[1]);
      if (!k_cpp.ok()) return k_cpp.status();
      if (!v_cpp.ok()) return v_cpp.status();
      // The map is a list<tuple<K, V>> at the WIT level.
      absl::StrAppend(
          out, "inline ", cpp, " lift(const ", s.c_name, "& m) {\n  ", cpp,
          " r;\n  for (size_t i = 0; i < m.len; ++i) {\n    r.emplace(\n      "
          "  ");
      // Key lift
      if (t.map_kv[0].kind == K::kString) {
        absl::StrAppend(out,
                        "std::string(reinterpret_cast<const "
                        "char*>(m.ptr[i].f0.ptr), m.ptr[i].f0.len)");
      } else {
        absl::StrAppend(out, "m.ptr[i].f0");
      }
      absl::StrAppend(out, ",\n        ");
      // Value lift
      if (t.map_kv[1].kind == K::kBool || t.map_kv[1].kind == K::kInt ||
          t.map_kv[1].kind == K::kUint || t.map_kv[1].kind == K::kDouble) {
        absl::StrAppend(out, "m.ptr[i].f1");
      } else {
        absl::StrAppend(out, "lift(m.ptr[i].f1)");
      }
      absl::StrAppend(out, ");\n  }\n  return r;\n}\n\n");
      // Lower for maps is a future-work item — we currently support
      // map RETURNS via direct stub authoring (maps as returns are
      // less common than as args).  Emit a comment placeholder so
      // the missing overload is obvious if a generator user hits it.
      absl::StrAppend(out, "// TODO(m26): lower(", s.c_name, "*, const ", cpp,
                      "&) not yet emitted; declare manually if needed.\n\n");
      return absl::OkStatus();
    }

    case K::kNull:
      absl::StrAppend(out,
                      "inline std::monostate lift(const author_option_u8_t&"
                      " /*o*/) {\n  return {};\n}\n\n");
      absl::StrAppend(out,
                      "inline void lower(author_option_u8_t* ret, "
                      "std::monostate) {\n"
                      "  ret->is_some = false;\n  ret->val = 0;\n}\n\n");
      return absl::OkStatus();

    case K::kBool:
    case K::kInt:
    case K::kUint:
    case K::kDouble:
      // Pass-through; no codec needed.
      return absl::OkStatus();

    case K::kOptional:
    case K::kType:
      return absl::FailedPreconditionError(
          "codec emit reached a permanently-rejected kind");
  }
  return absl::InternalError("EmitOne: unknown kind");
}

}  // namespace

absl::StatusOr<std::string> EmitCodecH(const FunctionLibrary& lib,
                                       absl::string_view cpp_namespace,
                                       absl::string_view wit_package_name) {
  TypeCollector tc;
  for (const auto& d : lib.decls()) {
    if (d.backend != CelfnDecl::Backend::kForeignComponent) continue;
    tc.Visit(d.return_type);
    for (const auto& p : d.params) {
      tc.Visit(p.type);
    }
  }

  const std::string exports_prefix = absl::StrCat(
      "exports_", NormalizePkgForExportsPrefix(wit_package_name), "_fns_");

  std::string out;
  absl::StrAppend(&out,
                  "// Generated by `cel generate` — DO NOT EDIT.\n"
                  "//\n"
                  "// codec.h: lift / lower between wit-bindgen author_* "
                  "structs\n"
                  "// and std:: containers.  Author never includes this — "
                  "consumed\n"
                  "// only by generated_stub.cc.\n//\n"
                  "// Ownership rule: `lower(*ret, ...)` populates the "
                  "out-param;\n"
                  "// the canonical-ABI runtime calls `cabi_post_*` to free.\n"
                  "// The author NEVER calls _free on a return value.\n\n");
  absl::StrAppend(&out, "#pragma once\n");
  absl::StrAppend(&out, "#include <cstdint>\n");
  absl::StrAppend(&out, "#include <cstring>\n");
  absl::StrAppend(&out, "#include <map>\n");
  absl::StrAppend(&out, "#include <string>\n");
  absl::StrAppend(&out, "#include <string_view>\n");
  absl::StrAppend(&out, "#include <variant>  // std::monostate\n");
  absl::StrAppend(&out, "#include <vector>\n\n");
  absl::StrAppend(&out, "#include \"absl/time/time.h\"\n");
  absl::StrAppend(&out, "#include \"author.h\"\n\n");

  if (!cpp_namespace.empty()) {
    absl::StrAppend(&out, "namespace ", cpp_namespace, "::codec {\n\n");
  } else {
    absl::StrAppend(&out, "namespace codec {\n\n");
  }

  for (const auto& t : tc.ordered()) {
    if (auto s = EmitOne(t, exports_prefix, &out); !s.ok()) return s;
  }

  if (!cpp_namespace.empty()) {
    absl::StrAppend(&out, "}  // namespace ", cpp_namespace, "::codec\n");
  } else {
    absl::StrAppend(&out, "}  // namespace codec\n");
  }
  return out;
}

}  // namespace celwasm::celfnc_emit
