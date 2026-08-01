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
#include "absl/strings/substitute.h"
#include "compiler/celfn/function_library.h"

// ───────────────────────────────────────────────────────────────────
// Raw-string templates.  Each kFoo<n>Tpl is the literal C++ the
// emitter produces for that case — read these directly to see what
// the generated codec.h looks like.  `$0`, `$1`, ... are
// absl::Substitute placeholders filled in at emission time.
//
// Discipline: every emit case maps to a named template + a single
// SubstituteAndAppend call (or StrAppend for the constant cases).
// Keeps the per-case shape obvious and the tests stable against the
// template body, not against a chain of StrCat fragments.
// ───────────────────────────────────────────────────────────────────

namespace celwasm::celfnc_emit {
namespace {

// Author-side struct name for a CelType subtree, mirroring what
// wit-bindgen 0.57 emits (m26 §3.5.1, verified empirically via the
// probe at /tmp/witgen).  Records collapse to a single name per
// kind (kDuration → "duration", kTimestamp → "timestamp") because
// wit-bindgen emits the record type ONCE per interface.
struct StructName {
  // `customfn_list_s64_t` / `customfn_string_t` / `customfn_list_tuple2_…`
  // etc.  Empty for primitives that pass through unwrapped
  // (bool / int / uint / double).
  std::string c_name;
  // True for kProto carriers — they share their wire shape with
  // kBytes (`customfn_list_u8_t`) but the codec emits a TEMPLATE
  // `lift_proto` / `lower_proto` instead of the bytes overload set.
  bool is_proto = false;
};

// Argkind-of-types helper — recurses through the list / map element
// types, mirroring ArgkindSlug but synthesising the C struct name
// instead of the overload-id suffix.
std::string SuffixFor(const CelType& t) {
  using K = CelType::Kind;
  switch (t.kind()) {
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
    // `SuffixFor` is only ever reached for a type in ELEMENT position
    // (list element, map key/value) — `StructFor` answers the bare
    // forms with hard-coded names and never calls in here.  So these
    // must name the element's OWN container, not its inner scalar:
    // bytes is `list<u8>` on the wire, so `list<bytes>` is
    // `list<list<u8>>` -> `customfn_list_list_u8_t`.  Returning "u8"
    // collapsed that to `customfn_list_u8_t`, colliding with bare
    // bytes and emitting two `lift` overloads differing only in
    // return type.
    case K::kBytes:
      return "list_u8";
    case K::kNull:
      return "option_u8";
    case K::kList:
      return absl::StrCat("list_", SuffixFor(t.list_element()));
    case K::kMap:
      // m24 §6: map<K,V> wire shape is list<tuple<K,V>>; the
      // wit-bindgen C struct is `customfn_list_tuple2_<k>_<v>_t`,
      // NOT `customfn_tuple2_<k>_<v>_t` (which is the inner tuple
      // element type, used only as the element type of the outer
      // list).  Empirically verified against the /tmp/witgen probe
      // output during m26 design (m26 §3.5.1).
      return absl::StrCat("list_tuple2_", SuffixFor(t.map_key()), "_",
                          SuffixFor(t.map_value()));
    case K::kMessage:
      return "list_u8";  // serialized bytes: same wire shape as list<u8>
    case K::kDuration:
    case K::kTimestamp:
    case K::kOptional:
    case K::kType:
    case K::kUnknown:
      // Records use a different naming scheme (exports_*_t).
      // optional / type never reach here in v1 — rejected upstream;
      // kUnknown cannot appear in a Builder-finalised decl.
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
StructName StructFor(const CelType& t, absl::string_view exports_prefix) {
  using K = CelType::Kind;
  switch (t.kind()) {
    case K::kBool:
    case K::kInt:
    case K::kUint:
    case K::kDouble:
      return {/*c_name=*/"", false};  // pass-through
    case K::kString:
      return {"customfn_string_t", false};
    case K::kBytes:
      return {"customfn_list_u8_t", false};
    case K::kNull:
      // option<u8> uses customfn_option_u8_t for the struct (but the
      // export adapter uses pointer-as-maybe).  Codec emits an
      // overload for the struct form for completeness; the stub
      // chooses the pointer form.
      return {"customfn_option_u8_t", false};
    case K::kList:
    case K::kMap:
      return {absl::StrCat("customfn_", SuffixFor(t)) + "_t", false};
    case K::kMessage:
      return {"customfn_list_u8_t", true};
    case K::kDuration:
      return {absl::StrCat(exports_prefix, "duration_t"), false};
    case K::kTimestamp:
      return {absl::StrCat(exports_prefix, "timestamp_t"), false};
    case K::kOptional:
    case K::kType:
    case K::kUnknown:
      return {"<unreachable>", false};
  }
  return {"<unreachable>", false};
}

// C++ container-side type the codec lifts to / lowers from.
absl::StatusOr<std::string> CppTypeFor(const CelType& t) {
  using K = CelType::Kind;
  switch (t.kind()) {
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
      // customfn_string_dup_n copy goes to wasm memory).
      return std::string("std::string");  // container form (return type)
    case K::kBytes:
      return std::string("std::vector<uint8_t>");
    case K::kNull:
      return std::string("std::monostate");
    case K::kDuration:
      // google.protobuf.Duration container — m26 §4.  Authors
      // include `<google/protobuf/duration.pb.h>` (the codec.h emits
      // the include conditionally; the wasi-sdk cc_binary needs
      // libprotobuf as a dep when the IDL uses Duration).
      return std::string("::google::protobuf::Duration");
    case K::kTimestamp:
      return std::string("::google::protobuf::Timestamp");
    case K::kList: {
      auto inner = CppTypeFor(t.list_element());
      if (!inner.ok()) return inner.status();
      return absl::StrCat("std::vector<", *inner, ">");
    }
    case K::kMap: {
      auto k = CppTypeFor(t.map_key());
      auto v = CppTypeFor(t.map_value());
      if (!k.ok()) return k.status();
      if (!v.ok()) return v.status();
      return absl::StrCat("std::map<", *k, ", ", *v, ">");
    }
    case K::kMessage:
      // Caller knows the proto type; codec uses lift_proto<M> template.
      return std::string("M");
    case K::kOptional:
    case K::kType:
      return absl::FailedPreconditionError(absl::StrCat(
          "codec emitter saw a permanently-rejected CelType::Kind (",
          (t.kind() == K::kOptional ? "optional" : "type"),
          "); Builder::Build() should have rejected upstream"));
    case K::kUnknown:
      break;
  }
  return absl::InternalError("CppTypeFor: unknown kind");
}

// Walk types, collecting unique non-pass-through types in topological
// order (inner-before-outer).  Uses string-keyed dedup to merge
// repeats across multiple decls.
class TypeCollector {
 public:
  void Visit(const CelType& t) {
    using K = CelType::Kind;
    switch (t.kind()) {
      case K::kList:
        Visit(t.list_element());
        break;
      case K::kMap:
        Visit(t.map_key());
        Visit(t.map_value());
        break;
      default:
        break;
    }
    // Add after inner — topological order.
    if (t.kind() == K::kBool || t.kind() == K::kInt || t.kind() == K::kUint ||
        t.kind() == K::kDouble) {
      return;  // pass-through, no codec needed
    }
    if (t.kind() == K::kOptional || t.kind() == K::kType) {
      // upstream-rejected; let the emit pass surface the error.
      return;
    }
    const std::string key =
        absl::StrCat(static_cast<int>(t.kind()), ":", SuffixFor(t), ":",
                     t.kind() == K::kMessage ? t.message_fully_qualified_name()
                                             : absl::string_view());
    if (seen_.insert(key).second) {
      ordered_.push_back(t);
    }
  }
  const std::vector<CelType>& ordered() const {
    return ordered_;
  }

 private:
  std::set<std::string> seen_;
  std::vector<CelType> ordered_;
};

// ── Per-type emission templates ──
//
// Each kFooTpl is the literal C++ text the emitter produces for
// that case.  `$0`, `$1`, … are absl::Substitute placeholders.

constexpr absl::string_view kStringTpl =
    R"cpp(inline std::string_view lift(const customfn_string_t& s) {
            return {reinterpret_cast<const char*>(s.ptr), s.len};
          }

          inline void lower(customfn_string_t* ret, std::string_view s) {
            customfn_string_dup_n(ret, s.data(), s.size());
          }
    )cpp";

constexpr absl::string_view kBytesTpl =
    R"cpp(inline std::vector<uint8_t> lift(const customfn_list_u8_t& l) {
            return {l.ptr, l.ptr + l.len};
          }

          inline void lower(customfn_list_u8_t* ret, const std::vector<uint8_t>& v) {
            ret->len = v.size();
            ret->ptr = static_cast<uint8_t*>(cabi_realloc(NULL, 0, 1, v.size()));
            if (!v.empty()) std::memcpy(ret->ptr, v.data(), v.size());
          }
    )cpp";

constexpr absl::string_view kProtoTpl =
    R"cpp(template <typename M>
          inline M lift_proto(const customfn_list_u8_t& l) {
            M msg;
            if (l.len > 0) msg.ParseFromArray(l.ptr, l.len);
            return msg;
          }

          template <typename M>
          inline void lower_proto(customfn_list_u8_t* ret, const M& msg) {
            std::string buf;
            msg.SerializeToString(&buf);
            ret->len = buf.size();
            ret->ptr = static_cast<uint8_t*>(cabi_realloc(NULL, 0, 1, buf.size()));
            if (!buf.empty()) std::memcpy(ret->ptr, buf.data(), buf.size());
          }
    )cpp";

// $0 = record struct name (e.g. exports_cel_customfn_fns_duration_t).
constexpr absl::string_view kDurationTpl =
    R"cpp(inline ::google::protobuf::Duration lift(const $0& r) {
            ::google::protobuf::Duration d;
            d.set_seconds(r.seconds);
            d.set_nanos(r.nanos);
            return d;
          }

          inline void lower($0* ret, const ::google::protobuf::Duration& d) {
            ret->seconds = d.seconds();
            ret->nanos = d.nanos();
          }
    )cpp";

constexpr absl::string_view kTimestampTpl =
    R"cpp(inline ::google::protobuf::Timestamp lift(const $0& r) {
            ::google::protobuf::Timestamp t;
            t.set_seconds(r.seconds);
            t.set_nanos(r.nanos);
            return t;
          }

          inline void lower($0* ret, const ::google::protobuf::Timestamp& t) {
            ret->seconds = t.seconds();
            ret->nanos = t.nanos();
          }
    )cpp";

constexpr absl::string_view kNullTpl =
    R"cpp(inline std::monostate lift(const customfn_option_u8_t& /*o*/) {
            return {};
          }

          inline void lower(customfn_option_u8_t* ret, std::monostate) {
            ret->is_some = false;
            ret->val = 0;
          }
    )cpp";

// $0 = cpp container (e.g. std::vector<int64_t>), $1 = author struct.
constexpr absl::string_view kListFlatTpl =
    R"cpp(inline $0 lift(const $1& l) {
            return {l.ptr, l.ptr + l.len};
          }

          inline void lower($1* ret, const $0& v) {
            ret->len = v.size();
            if (v.empty()) {
              ret->ptr = NULL;
              return;
            }
            ret->ptr = static_cast<decltype(ret->ptr)>(
                cabi_realloc(NULL, 0, alignof(decltype(*ret->ptr)),
                             v.size() * sizeof(*ret->ptr)));
            std::memcpy(ret->ptr, v.data(), v.size() * sizeof(*ret->ptr));
          }
    )cpp";

// Recursive list: per-element lift/lower call.  $0 = cpp container,
// $1 = author struct.
constexpr absl::string_view kListRecTpl =
    R"cpp(inline $0 lift(const $1& l) {
            $0 r;
            r.reserve(l.len);
            for (size_t i = 0; i < l.len; ++i) {
              r.emplace_back(lift(l.ptr[i]));
            }
            return r;
          }

          inline void lower($1* ret, const $0& v) {
            ret->len = v.size();
            if (v.empty()) {
              ret->ptr = NULL;
              return;
            }
            ret->ptr = static_cast<decltype(ret->ptr)>(
                cabi_realloc(NULL, 0, alignof(decltype(*ret->ptr)),
                             v.size() * sizeof(*ret->ptr)));
            for (size_t i = 0; i < v.size(); ++i) {
              lower(&ret->ptr[i], v[i]);
            }
          }
    )cpp";

// $0 = cpp std::map<...>, $1 = customfn_list_tuple2_*_t,
// $2 = key lift expression (uses m.ptr[i].f0),
// $3 = value lift expression (uses m.ptr[i].f1),
// $4 = the whole lower-loop body (key write, value write, ++i) —
//      ONE substitution because clang-format reformats raw-string
//      contents and would otherwise join adjacent statement slots.
constexpr absl::string_view kMapTpl =
    R"cpp(inline $0 lift(const $1& m) {
            $0 r;
            for (size_t i = 0; i < m.len; ++i) {
              r.emplace($2, $3);
            }
            return r;
          }

          inline void lower($1* ret, const $0& m) {
            ret->len = m.size();
            if (m.empty()) {
              ret->ptr = NULL;
              return;
            }
            ret->ptr = static_cast<decltype(ret->ptr)>(
                cabi_realloc(NULL, 0, alignof(decltype(*ret->ptr)),
                             m.size() * sizeof(*ret->ptr)));
            size_t i = 0;
            for (const auto& kv : m) {
              $4
            }
          }
    )cpp";

// Emit the map arm's lift + lower.  The per-element key / value
// expressions vary by kind — string keys route through the string
// codec, scalar values assign directly — so they are inlined into
// the shared template rather than templated further.
void EmitMap(const CelType& t, absl::string_view cpp, absl::string_view c_name,
             std::string* out) {
  using K = CelType::Kind;
  const bool key_string = t.map_key().kind() == K::kString;
  const std::string key_expr =
      key_string
          ? "std::string(reinterpret_cast<const char*>(m.ptr[i].f0.ptr), "
            "m.ptr[i].f0.len)"
          : "m.ptr[i].f0";
  const K vk = t.map_value().kind();
  const bool val_scalar =
      vk == K::kBool || vk == K::kInt || vk == K::kUint || vk == K::kDouble;
  const std::string val_expr = val_scalar ? "m.ptr[i].f1" : "lift(m.ptr[i].f1)";
  const std::string lower_body =
      absl::StrCat(key_string ? "lower(&ret->ptr[i].f0, kv.first); "
                              : "ret->ptr[i].f0 = kv.first; ",
                   val_scalar ? "ret->ptr[i].f1 = kv.second; "
                              : "lower(&ret->ptr[i].f1, kv.second); ",
                   "++i;");
  absl::SubstituteAndAppend(out, kMapTpl, cpp, c_name, key_expr, val_expr,
                            lower_body);
}

// The kinds whose codec is a fixed template with no substitution
// (string / bytes / proto / null) plus the two record kinds, whose
// only substitution is the struct name.  Returns false for the
// kinds that need per-type expression building (list / map) or need
// no codec at all (the scalars).
bool EmitFixedTemplate(const CelType& t, const StructName& s,
                       std::string* out) {
  using K = CelType::Kind;
  switch (t.kind()) {
    case K::kString:
      absl::StrAppend(out, kStringTpl);
      return true;
    case K::kBytes:
      absl::StrAppend(out, kBytesTpl);
      return true;
    case K::kMessage:
      absl::StrAppend(out, kProtoTpl);
      return true;
    case K::kNull:
      absl::StrAppend(out, kNullTpl);
      return true;
    case K::kDuration:
      absl::SubstituteAndAppend(out, kDurationTpl, s.c_name);
      return true;
    case K::kTimestamp:
      absl::SubstituteAndAppend(out, kTimestampTpl, s.c_name);
      return true;
    default:
      return false;
  }
}

// Emit lift+lower for a single type that needs them.
absl::Status EmitOne(const CelType& t, absl::string_view exports_prefix,
                     std::string* out) {
  using K = CelType::Kind;
  StructName s = StructFor(t, exports_prefix);
  auto cpp_or = CppTypeFor(t);
  if (!cpp_or.ok()) return cpp_or.status();
  const std::string& cpp = *cpp_or;

  if (EmitFixedTemplate(t, s, out)) return absl::OkStatus();

  switch (t.kind()) {
    case K::kList: {
      const auto& inner = t.list_element();
      const bool scalar_inner =
          inner.kind() == K::kBool || inner.kind() == K::kInt ||
          inner.kind() == K::kUint || inner.kind() == K::kDouble;
      const absl::string_view tpl = scalar_inner ? kListFlatTpl : kListRecTpl;
      absl::SubstituteAndAppend(out, tpl, cpp, s.c_name);
      return absl::OkStatus();
    }

    case K::kMap:
      EmitMap(t, cpp, s.c_name, out);
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

    case K::kString:
    case K::kBytes:
    case K::kMessage:
    case K::kNull:
    case K::kDuration:
    case K::kTimestamp:
      ABSL_CHECK(false) << "EmitOne: EmitFixedTemplate already handled kind "
                        << static_cast<int>(t.kind());
      break;
    case K::kUnknown:
      break;
  }
  return absl::InternalError("EmitOne: unknown kind");
}

// The fixed file header + include block.  The two proto includes are
// conditional so authors whose IDL has no Duration / Timestamp pay no
// protobuf dep at the wasm cc_binary step.
void EmitPreambleAndIncludes(bool needs_proto_duration,
                             bool needs_proto_timestamp, std::string* out) {
  absl::StrAppend(out,
                  "// Generated by `cel generate` — DO NOT EDIT.\n"
                  "//\n"
                  "// codec.h: lift / lower between wit-bindgen customfn_* "
                  "structs\n"
                  "// and std:: containers.  Author never includes this — "
                  "consumed\n"
                  "// only by generated_stub.cc.\n//\n"
                  "// Ownership rule: `lower(*ret, ...)` populates the "
                  "out-param;\n"
                  "// the canonical-ABI runtime calls `cabi_post_*` to free.\n"
                  "// The author NEVER calls _free on a return value.\n\n");
  absl::StrAppend(out, "#pragma once\n");
  absl::StrAppend(out, "#include <cstdint>\n");
  absl::StrAppend(out, "#include <cstring>\n");
  absl::StrAppend(out, "#include <map>\n");
  absl::StrAppend(out, "#include <string>\n");
  absl::StrAppend(out, "#include <string_view>\n");
  absl::StrAppend(out, "#include <variant>  // std::monostate\n");
  absl::StrAppend(out, "#include <vector>\n\n");
  if (needs_proto_duration) {
    absl::StrAppend(out, "#include \"google/protobuf/duration.pb.h\"\n");
  }
  if (needs_proto_timestamp) {
    absl::StrAppend(out, "#include \"google/protobuf/timestamp.pb.h\"\n");
  }
  absl::StrAppend(out, "#include \"customfn.h\"\n\n");
}

// Visit every type reachable from the library's PLUGIN decls (the
// only backend whose args/returns cross the canonical ABI).
void CollectPluginTypes(const FunctionLibrary& lib, TypeCollector* tc) {
  for (const auto& d : lib.decls()) {
    if (d.backend != CelfnDecl::Backend::kPlugin) continue;
    tc->Visit(d.return_type);
    for (const auto& p : d.params) {
      tc->Visit(p.type);
    }
  }
}

}  // namespace

absl::StatusOr<std::string> EmitCodecH(const FunctionLibrary& lib,
                                       absl::string_view cpp_namespace,
                                       absl::string_view wit_package_name) {
  TypeCollector tc;
  CollectPluginTypes(lib, &tc);

  const std::string exports_prefix = absl::StrCat(
      "exports_", NormalizePkgForExportsPrefix(wit_package_name), "_fns_");

  // Only pull google.protobuf.Duration / Timestamp into codec.h when
  // the IDL actually declares Duration / Timestamp args or returns.
  // Authors who don't use these CEL types pay no protobuf dep at the
  // wasm cc_binary step.
  bool needs_proto_duration = false;
  bool needs_proto_timestamp = false;
  for (const auto& t : tc.ordered()) {
    if (t.kind() == CelType::Kind::kDuration) needs_proto_duration = true;
    if (t.kind() == CelType::Kind::kTimestamp) needs_proto_timestamp = true;
  }

  std::string out;
  EmitPreambleAndIncludes(needs_proto_duration, needs_proto_timestamp, &out);

  // wit-bindgen's customfn.c defines `cabi_realloc` as a weak,
  // export-named symbol but customfn.h does NOT declare it.  The
  // codec.h's lower(...) bodies call `cabi_realloc(...)` for every
  // list / bytes / proto path — emit the forward decl here so we
  // don't depend on header changes from wit-bindgen.
  absl::StrAppend(&out,
                  "extern \"C\" void* cabi_realloc(void* ptr, size_t old_size, "
                  "size_t align, size_t new_size);\n\n");

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
