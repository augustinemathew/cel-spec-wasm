#include "compiler_v2/tools/cel/value_format.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/internal/cel_host.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/util/json_util.h"

namespace celwasm::tools::cel {

namespace {

using ::celwasm::api::Value;

absl::StatusOr<std::string> ToCelLiteral(const Value& v);

std::string QuoteString(absl::string_view s) {
  // Match the literal grammar the var parser accepts (JSON-ish).
  std::string out = "\"";
  for (const char ch : s) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          absl::StrAppendFormat(&out, "\\x%02x",
                                static_cast<unsigned>(ch) & 0xFFu);
        } else {
          out.push_back(ch);
        }
    }
  }
  out += "\"";
  return out;
}

std::string QuoteBytes(absl::string_view b) {
  std::string out = "b\"";
  for (const char ch : b) {
    const auto u = static_cast<unsigned char>(ch);
    if (u >= 0x20 && u < 0x7f && ch != '\\' && ch != '"') {
      out.push_back(ch);
    } else {
      absl::StrAppendFormat(&out, "\\x%02x", static_cast<unsigned>(u));
    }
  }
  out += "\"";
  return out;
}

absl::StatusOr<std::string> FormatListBacking(
    const celwasm::HostListBacking& l) {
  std::string out = "[";
  bool first = true;
  absl::Status err = absl::OkStatus();
  for (std::size_t i = 0; i < l.Size(); ++i) {
    auto e = l.At(i, ::celwasm::api::CelType{});
    if (!e.ok()) {
      err = e.status();
      break;
    }
    auto lit = ToCelLiteral(*e);
    if (!lit.ok()) {
      err = lit.status();
      break;
    }
    if (!first) out += ", ";
    first = false;
    out += *lit;
  }
  if (!err.ok()) return err;
  out += "]";
  return out;
}

absl::StatusOr<std::string> FormatMapBacking(const celwasm::HostMapBacking& m) {
  std::string out = "{";
  bool first = true;
  absl::Status err = absl::OkStatus();
  m.ForEach([&](const Value& k, const Value& v) {
    if (!err.ok()) return;
    auto klit = ToCelLiteral(k);
    if (!klit.ok()) {
      err = klit.status();
      return;
    }
    auto vlit = ToCelLiteral(v);
    if (!vlit.ok()) {
      err = vlit.status();
      return;
    }
    if (!first) out += ", ";
    first = false;
    out += *klit;
    out += ": ";
    out += *vlit;
  });
  if (!err.ok()) return err;
  out += "}";
  return out;
}

absl::StatusOr<std::string> MessageToTextproto(
    const google::protobuf::Message& m) {
  std::string out;
  google::protobuf::TextFormat::Printer p;
  p.SetExpandAny(true);
  if (!p.PrintToString(m, &out)) {
    return absl::InternalError(
        absl::StrCat("TextFormat::PrintToString failed for message ",
                     m.GetDescriptor()->full_name()));
  }
  return out;
}

absl::StatusOr<std::string> MessageToJson(const google::protobuf::Message& m) {
  std::string out;
  google::protobuf::util::JsonPrintOptions opts;
  opts.add_whitespace = false;
  opts.preserve_proto_field_names = true;
  auto s = google::protobuf::util::MessageToJsonString(m, &out, opts);
  if (!s.ok()) {
    return absl::InternalError(
        absl::StrCat("MessageToJsonString failed for message ",
                     m.GetDescriptor()->full_name(), ": ", s.message()));
  }
  return out;
}

absl::StatusOr<std::string> MessageToCelLiteral(
    const google::protobuf::Message& m) {
  // No canonical CEL-literal renderer exists in the repo (see
  // `acme.User{...}` example in cel-spec); compose one shallowly
  // from set fields.  Sufficient for the CLI's --format=cel output
  // on simple shapes; nested messages recurse.
  const auto* desc = m.GetDescriptor();
  const auto* refl = m.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  refl->ListFields(m, &fields);
  std::string out = absl::StrCat(desc->full_name(), "{");
  bool first = true;
  for (const auto* f : fields) {
    if (!first) out += ", ";
    first = false;
    absl::StrAppend(&out, f->name(), ": ");
    if (f->is_repeated()) {
      out += "[...]";  // Aggregate rendering left to textproto / json.
      continue;
    }
    switch (f->cpp_type()) {
      case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        absl::StrAppend(&out, refl->GetInt32(m, f));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
        absl::StrAppend(&out, refl->GetInt64(m, f));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        absl::StrAppend(&out, refl->GetUInt32(m, f), "u");
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
        absl::StrAppend(&out, refl->GetUInt64(m, f), "u");
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
        absl::StrAppend(&out, refl->GetDouble(m, f));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
        absl::StrAppend(&out, refl->GetFloat(m, f));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
        absl::StrAppend(&out, refl->GetBool(m, f) ? "true" : "false");
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
        std::string scratch;
        const std::string& s = refl->GetStringReference(m, f, &scratch);
        absl::StrAppend(
            &out, f->type() == google::protobuf::FieldDescriptor::TYPE_BYTES
                      ? QuoteBytes(s)
                      : QuoteString(s));
        break;
      }
      case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
        absl::StrAppend(&out, refl->GetEnum(m, f)->name());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
        auto inner = MessageToCelLiteral(refl->GetMessage(m, f));
        if (!inner.ok()) return inner.status();
        out += *inner;
        break;
      }
    }
  }
  out += "}";
  return out;
}

absl::StatusOr<std::string> ToCelLiteral(const Value& v) {
  switch (v.kind()) {
    case Value::Kind::kNull:
      return std::string("null");
    case Value::Kind::kBool:
      return std::string(*v.AsBool() ? "true" : "false");
    case Value::Kind::kInt:
      return absl::StrCat(*v.AsInt());
    case Value::Kind::kUint:
      return absl::StrCat(*v.AsUint(), "u");
    case Value::Kind::kDouble: {
      return absl::StrFormat("%g", *v.AsDouble());
    }
    case Value::Kind::kString:
      return QuoteString(*v.AsString());
    case Value::Kind::kBytes:
      return QuoteBytes(*v.AsBytes());
    case Value::Kind::kDuration:
      return absl::StrCat("duration(\"", absl::FormatDuration(*v.AsDuration()),
                          "\")");
    case Value::Kind::kTimestamp:
      return absl::StrCat("timestamp(\"",
                          absl::FormatTime(absl::RFC3339_full, *v.AsTimestamp(),
                                           absl::UTCTimeZone()),
                          "\")");
    case Value::Kind::kType:
      return absl::StrCat("type(", *v.AsType(), ")");
    case Value::Kind::kList: {
      auto backing = v.ListBacking();
      if (!backing.ok()) return backing.status();
      return FormatListBacking(**backing);
    }
    case Value::Kind::kMap: {
      auto backing = v.MapBacking();
      if (!backing.ok()) return backing.status();
      return FormatMapBacking(**backing);
    }
    case Value::Kind::kMessage: {
      auto backing = v.MessageBacking();
      if (!backing.ok()) return backing.status();
      const google::protobuf::Message* m = (*backing)->message();
      if (m == nullptr) {
        return absl::InvalidArgumentError(
            "kMessage Value without a proto::Message backing");
      }
      return MessageToCelLiteral(*m);
    }
    case Value::Kind::kUnknown: {
      auto attr = v.UnknownAttribute();
      return absl::StrCat("<unknown:", attr.ok() ? attr->id : 0u, ">");
    }
    case Value::Kind::kError: {
      auto e = v.ErrorInfo();
      if (!e.ok()) return absl::StrCat("error: <opaque>");
      return absl::StrCat("error: ", ::celwasm::ErrorCodeName((*e)->code), " ",
                          (*e)->message);
    }
  }
  ABSL_CHECK(false) << "ToCelLiteral: unhandled Value::Kind = "
                    << static_cast<int>(v.kind());
}

}  // namespace

absl::string_view FormatName(Format f) {
  switch (f) {
    case Format::kTextproto:
      return "textproto";
    case Format::kJson:
      return "json";
    case Format::kCel:
      return "cel";
  }
  ABSL_CHECK(false) << "FormatName: unhandled Format = " << static_cast<int>(f);
}

absl::StatusOr<Format> ParseFormatName(absl::string_view name) {
  if (name == "textproto" || name == "txtpb" || name == "pbtxt") {
    return Format::kTextproto;
  }
  if (name == "json") return Format::kJson;
  if (name == "cel") return Format::kCel;
  return absl::InvalidArgumentError(
      absl::StrCat("--format: expected textproto|json|cel, got `", name, "`"));
}

absl::StatusOr<std::string> FormatScalar(const Value& v) {
  return ToCelLiteral(v);
}

absl::StatusOr<std::string> FormatMessage(const Value& v,
                                          const std::vector<Format>& formats) {
  if (v.kind() != Value::Kind::kMessage) {
    return absl::InvalidArgumentError(
        "FormatMessage called on a non-message Value");
  }
  auto backing = v.MessageBacking();
  if (!backing.ok()) return backing.status();
  const google::protobuf::Message* m = (*backing)->message();
  if (m == nullptr) {
    return absl::InvalidArgumentError(
        "kMessage Value without a proto::Message backing");
  }

  std::vector<Format> effective = formats;
  if (effective.empty()) effective.push_back(Format::kTextproto);
  const bool labeled = effective.size() > 1;
  std::string out;
  for (std::size_t i = 0; i < effective.size(); ++i) {
    if (labeled) {
      if (i > 0) out += "\n";
      absl::StrAppend(&out, "--- ", FormatName(effective[i]), " ---\n");
    }
    absl::StatusOr<std::string> body;
    switch (effective[i]) {
      case Format::kTextproto:
        body = MessageToTextproto(*m);
        break;
      case Format::kJson:
        body = MessageToJson(*m);
        break;
      case Format::kCel:
        body = MessageToCelLiteral(*m);
        break;
    }
    if (!body.ok()) return body.status();
    out += *body;
    // TextFormat appends a trailing newline; JSON / CEL do not.
    if (!out.empty() && out.back() != '\n') out += "\n";
  }
  return out;
}

}  // namespace celwasm::tools::cel
