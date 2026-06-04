// celfnc_emit/wit_emitter — see header for scope.

#include "compiler/celfn/celfnc_emit/wit_emitter.h"

#include <set>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::celfnc_emit {
namespace {

// Records that appear in the WIT (declared inside `interface fns`).
// Today only `duration` and `timestamp` carry record types; the set is
// closed at the WIT level — list / map / proto carriers map directly
// to WIT structural types.
enum class RecordKind { kDuration, kTimestamp };

// Render a CEL type as its WIT spelling, walking through nested
// list_element / map_kv / optional_element / proto_fqn entries.
// Side-effect: stamps `records_needed` for every record-carrier we
// encounter so the emitter can declare them once inside the interface.
absl::StatusOr<std::string> WitTypeText(
    const CelfnType& t, std::set<RecordKind>* records_needed) {
  using K = CelfnType::Kind;
  switch (t.kind) {
    case K::kBool:
      return std::string("bool");
    case K::kInt:
      return std::string("s64");
    case K::kUint:
      return std::string("u64");
    case K::kDouble:
      return std::string("f64");
    case K::kNull:
      // CEL `null` rides on the wire as `option<u8>` — wit-bindgen
      // 0.57 rejects option<unit> at the C generator; option must
      // wrap a concrete type.  We pick `u8` as the conventional
      // carrier; the canonical-ABI value is always "none" so the
      // payload byte is never read.
      return std::string("option<u8>");
    case K::kString:
      return std::string("string");
    case K::kBytes:
      return std::string("list<u8>");
    case K::kDuration:
      records_needed->insert(RecordKind::kDuration);
      return std::string("duration");
    case K::kTimestamp:
      records_needed->insert(RecordKind::kTimestamp);
      return std::string("timestamp");
    case K::kList: {
      if (t.list_element.empty()) {
        return absl::InternalError("list<> with empty element type");
      }
      auto inner = WitTypeText(t.list_element[0], records_needed);
      if (!inner.ok()) return inner.status();
      return absl::StrCat("list<", *inner, ">");
    }
    case K::kMap: {
      if (t.map_kv.size() != 2) {
        return absl::InternalError("map<> with wrong kv arity");
      }
      auto k = WitTypeText(t.map_kv[0], records_needed);
      if (!k.ok()) return k.status();
      auto v = WitTypeText(t.map_kv[1], records_needed);
      if (!v.ok()) return v.status();
      // m24 §6: map<K,V> → list<tuple<wit K, wit V>>.
      return absl::StrCat("list<tuple<", *k, ", ", *v, ">>");
    }
    case K::kProto:
      // m24 §8: proto crosses as serialized bytes (list<u8>).  The
      // fqn is host-side metadata (the codec uses it for
      // ParseFromString); it does NOT appear in the WIT signature.
      return std::string("list<u8>");
    case K::kType:
    case K::kOptional:
      // m24 §14: permanently rejected.  Builder::Build() refuses
      // these upstream; if one reaches us, treat it as a regression
      // and surface a status the integration test will see.
      return absl::FailedPreconditionError(absl::StrCat(
          "wit emitter saw a permanently-rejected CelfnType::Kind (",
          (t.kind == K::kType ? "type" : "optional"),
          "); FunctionLibrary::Builder::Build() should have rejected this "
          "decl upstream (m24 §A.4 / §14)"));
  }
  return absl::InternalError("WitTypeText: unknown CelfnType::Kind");
}

absl::StatusOr<std::string> EmitOneFn(const CelfnDecl& d,
                                      std::set<RecordKind>* records_needed) {
  ABSL_CHECK_EQ(d.backend, CelfnDecl::Backend::kForeignComponent);
  std::vector<std::string> param_parts;
  param_parts.reserve(d.params.size());
  for (const auto& p : d.params) {
    auto wit = WitTypeText(p.type, records_needed);
    if (!wit.ok()) return wit.status();
    param_parts.push_back(absl::StrCat(p.name, ": ", *wit));
  }
  auto ret = WitTypeText(d.return_type, records_needed);
  if (!ret.ok()) return ret.status();
  return absl::StrCat("  ", SnakeToKebab(d.overload_id), ": func(",
                      absl::StrJoin(param_parts, ", "), ") -> ", *ret, ";");
}

std::string RecordLine(RecordKind k) {
  switch (k) {
    case RecordKind::kDuration:
      return "  record duration { seconds: s64, nanos: s32 }";
    case RecordKind::kTimestamp:
      return "  record timestamp { seconds: s64, nanos: s32 }";
  }
  ABSL_CHECK(false) << "RecordKind switch fell through";
}

}  // namespace

std::string SnakeToKebab(absl::string_view snake) {
  return absl::StrReplaceAll(snake, {{"_", "-"}});
}

absl::StatusOr<std::string> EmitWit(const FunctionLibrary& lib,
                                    absl::string_view package_name,
                                    absl::string_view package_version) {
  std::set<RecordKind> records_needed;
  std::vector<std::string> fn_lines;
  for (const auto& d : lib.decls()) {
    if (d.backend != CelfnDecl::Backend::kForeignComponent) continue;
    auto line = EmitOneFn(d, &records_needed);
    if (!line.ok()) return line.status();
    fn_lines.push_back(*std::move(line));
  }

  std::string out;
  absl::StrAppend(&out, "// Generated by `cel generate` — DO NOT EDIT.\n");
  absl::StrAppend(&out, "//\n");
  absl::StrAppend(
      &out,
      "// Re-run `cel generate --language=cpp --idl=<your.idl>` to refresh.\n");
  absl::StrAppend(&out, "\n");
  if (package_version.empty()) {
    absl::StrAppend(&out, "package ", package_name, ";\n");
  } else {
    absl::StrAppend(&out, "package ", package_name, "@", package_version,
                    ";\n");
  }
  absl::StrAppend(&out, "\n");
  absl::StrAppend(&out, "interface fns {\n");
  // Records first, then functions.  wit-bindgen 0.57 accepts either
  // order inside an interface; emitting records first keeps the file
  // readable when a parameter references a record by name.
  for (RecordKind k : records_needed) {
    absl::StrAppend(&out, RecordLine(k), "\n");
  }
  if (!records_needed.empty() && !fn_lines.empty()) {
    absl::StrAppend(&out, "\n");
  }
  for (const auto& line : fn_lines) {
    absl::StrAppend(&out, line, "\n");
  }
  absl::StrAppend(&out, "}\n");
  absl::StrAppend(&out, "\n");
  absl::StrAppend(&out, "world author { export fns; }\n");
  absl::StrAppend(&out, "world host   { import fns; }\n");
  return out;
}

}  // namespace celwasm::celfnc_emit
