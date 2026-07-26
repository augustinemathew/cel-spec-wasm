#include "tools/cel/var_parser.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/util/json_util.h"
#include "shared/type.h"

namespace celwasm::tools::cel {

namespace {

using ::celwasm::CelType;
using ::celwasm::Value;

// ---------- Small cursor primitive ------------------------------------------

struct Cursor {
  absl::string_view src;
  size_t pos = 0;

  bool Eof() const {
    return pos >= src.size();
  }
  char Peek() const {
    return Eof() ? '\0' : src[pos];
  }
  void Skip() {
    while (!Eof() && absl::ascii_isspace(static_cast<unsigned char>(Peek()))) {
      ++pos;
    }
  }
  bool ConsumeChar(char c) {
    Skip();
    if (Eof() || src[pos] != c) return false;
    ++pos;
    return true;
  }
  absl::Status Expect(char c) {
    if (ConsumeChar(c)) return absl::OkStatus();
    return absl::InvalidArgumentError(absl::StrCat(
        "expected '", std::string(1, c), "' at offset ", pos, " in: ", src));
  }
};

// ---------- Type-spec parser ------------------------------------------------

absl::StatusOr<CelType> ParseTypeRec(Cursor& c);

absl::StatusOr<std::string> ParseIdent(Cursor& c) {
  c.Skip();
  const size_t start = c.pos;
  while (!c.Eof()) {
    const char ch = c.src[c.pos];
    if (absl::ascii_isalnum(static_cast<unsigned char>(ch)) || ch == '_' ||
        ch == '.') {
      ++c.pos;
    } else {
      break;
    }
  }
  if (c.pos == start) {
    return absl::InvalidArgumentError(
        absl::StrCat("expected type identifier at offset ", c.pos,
                     " in type spec: ", c.src));
  }
  return std::string(c.src.substr(start, c.pos - start));
}

absl::StatusOr<CelType> ParseListT(Cursor& c) {
  auto s = c.Expect('<');
  if (!s.ok()) return s;
  auto e = ParseTypeRec(c);
  if (!e.ok()) return e.status();
  s = c.Expect('>');
  if (!s.ok()) return s;
  return CelType::List(*e);
}

absl::StatusOr<CelType> ParseMapT(Cursor& c) {
  auto s = c.Expect('<');
  if (!s.ok()) return s;
  auto k = ParseTypeRec(c);
  if (!k.ok()) return k.status();
  s = c.Expect(',');
  if (!s.ok()) return s;
  auto v = ParseTypeRec(c);
  if (!v.ok()) return v.status();
  s = c.Expect('>');
  if (!s.ok()) return s;
  return CelType::Map(*k, *v);
}

absl::StatusOr<CelType> ParseTypeRec(Cursor& c) {
  auto name = ParseIdent(c);
  if (!name.ok()) return name.status();
  const std::string& n = *name;
  if (n == "bool") return CelType::Bool();
  if (n == "int") return CelType::Int();
  if (n == "uint") return CelType::Uint();
  if (n == "double") return CelType::Double();
  if (n == "string") return CelType::String();
  if (n == "bytes") return CelType::Bytes();
  if (n == "duration") return CelType::Duration();
  if (n == "timestamp") return CelType::Timestamp();
  if (n == "list") return ParseListT(c);
  if (n == "map") return ParseMapT(c);
  // Anything else: treat as message FQN.  A bare identifier with no
  // dot is admitted (the checker will reject if no such type exists);
  // we don't second-guess what's a primitive misspell vs an unqualified
  // message name.
  return CelType::Message(n);
}

// ---------- Value-literal helpers ------------------------------------------

absl::StatusOr<std::string> ReadFile(absl::string_view path) {
  std::ifstream in{std::string(path), std::ios::binary};
  if (!in) {
    return absl::NotFoundError(absl::StrCat("cannot open file: ", path));
  }
  std::stringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// String literal: opening quote already at pos.  Supports JSON-style
// escapes; intentionally narrower than CEL's full string grammar (raw
// strings, triple-quoted, hex escapes longer than 2 digits) — the CLI
// is for shells where users hand-write the value; CEL's own
// parser is what the *expression* gets.
absl::StatusOr<std::string> ParseQuotedString(Cursor& c) {
  c.Skip();
  if (c.Eof() || (c.Peek() != '"' && c.Peek() != '\'')) {
    return absl::InvalidArgumentError(absl::StrCat(
        "expected quoted string at offset ", c.pos, " in: ", c.src));
  }
  const char quote = c.Peek();
  ++c.pos;
  std::string out;
  while (!c.Eof()) {
    const char ch = c.src[c.pos++];
    if (ch == quote) {
      return out;
    }
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (c.Eof()) break;
    const char esc = c.src[c.pos++];
    switch (esc) {
      case 'n':
        out.push_back('\n');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '"':
        out.push_back('"');
        break;
      case '\'':
        out.push_back('\'');
        break;
      case '0':
        out.push_back('\0');
        break;
      case 'x': {
        if (c.pos + 2 > c.src.size()) {
          return absl::InvalidArgumentError("truncated \\x escape");
        }
        unsigned v = 0;
        for (int i = 0; i < 2; ++i) {
          const char h = c.src[c.pos++];
          v <<= 4;
          if (h >= '0' && h <= '9')
            v |= (h - '0');
          else if (h >= 'a' && h <= 'f')
            v |= (h - 'a' + 10);
          else if (h >= 'A' && h <= 'F')
            v |= (h - 'A' + 10);
          else
            return absl::InvalidArgumentError(absl::StrCat(
                "invalid hex digit '", std::string(1, h), "' in \\x escape"));
        }
        out.push_back(static_cast<char>(v));
        break;
      }
      default:
        return absl::InvalidArgumentError(absl::StrCat(
            "unknown escape \\", std::string(1, esc), " in string literal"));
    }
  }
  return absl::InvalidArgumentError(
      absl::StrCat("unterminated string literal in: ", c.src));
}

// ---------- Scalar-by-CelType parsers --------------------------------------

absl::StatusOr<Value> ParseBool(Cursor& c) {
  c.Skip();
  const auto rest = c.src.substr(c.pos);
  if (absl::StartsWith(rest, "true")) {
    c.pos += 4;
    return Value::Bool(true);
  }
  if (absl::StartsWith(rest, "false")) {
    c.pos += 5;
    return Value::Bool(false);
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "expected bool literal (true|false) at offset ", c.pos, " in: ", c.src));
}

absl::StatusOr<Value> ParseInt(Cursor& c) {
  c.Skip();
  const size_t start = c.pos;
  if (!c.Eof() && (c.Peek() == '-' || c.Peek() == '+')) ++c.pos;
  while (!c.Eof() &&
         absl::ascii_isdigit(static_cast<unsigned char>(c.Peek()))) {
    ++c.pos;
  }
  if (c.pos == start ||
      (c.pos == start + 1 && (c.src[start] == '-' || c.src[start] == '+'))) {
    return absl::InvalidArgumentError(absl::StrCat(
        "expected integer literal at offset ", start, " in: ", c.src));
  }
  int64_t v;
  if (!absl::SimpleAtoi(c.src.substr(start, c.pos - start), &v)) {
    return absl::InvalidArgumentError(
        absl::StrCat("integer literal out of int64 range: ",
                     c.src.substr(start, c.pos - start)));
  }
  return Value::Int(v);
}

absl::StatusOr<Value> ParseUint(Cursor& c) {
  c.Skip();
  const size_t start = c.pos;
  while (!c.Eof() &&
         absl::ascii_isdigit(static_cast<unsigned char>(c.Peek()))) {
    ++c.pos;
  }
  if (c.pos == start) {
    return absl::InvalidArgumentError(absl::StrCat(
        "expected unsigned integer literal at offset ", start, " in: ", c.src));
  }
  uint64_t v;
  if (!absl::SimpleAtoi(c.src.substr(start, c.pos - start), &v)) {
    return absl::InvalidArgumentError(
        absl::StrCat("uint literal out of uint64 range: ",
                     c.src.substr(start, c.pos - start)));
  }
  // Tolerate trailing 'u' from CEL-style suffix.
  if (!c.Eof() && c.Peek() == 'u') ++c.pos;
  return Value::Uint(v);
}

absl::StatusOr<Value> ParseDouble(Cursor& c) {
  c.Skip();
  const size_t start = c.pos;
  if (!c.Eof() && (c.Peek() == '-' || c.Peek() == '+')) ++c.pos;
  while (!c.Eof() &&
         (absl::ascii_isdigit(static_cast<unsigned char>(c.Peek())) ||
          c.Peek() == '.' || c.Peek() == 'e' || c.Peek() == 'E' ||
          c.Peek() == '-' || c.Peek() == '+')) {
    ++c.pos;
  }
  if (c.pos == start) {
    return absl::InvalidArgumentError(absl::StrCat(
        "expected double literal at offset ", start, " in: ", c.src));
  }
  double v;
  if (!absl::SimpleAtod(c.src.substr(start, c.pos - start), &v)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "malformed double literal: ", c.src.substr(start, c.pos - start)));
  }
  return Value::Double(v);
}

absl::StatusOr<Value> ParseString(Cursor& c) {
  auto s = ParseQuotedString(c);
  if (!s.ok()) return s.status();
  return Value::String(*std::move(s));
}

absl::StatusOr<Value> ParseBytes(Cursor& c) {
  c.Skip();
  // `@path.bin` reads raw bytes from a file.
  if (!c.Eof() && c.Peek() == '@') {
    ++c.pos;
    const std::string path(c.src.substr(c.pos));
    c.pos = c.src.size();
    auto buf = ReadFile(path);
    if (!buf.ok()) return buf.status();
    return Value::Bytes(*std::move(buf));
  }
  // Optional `b` prefix matching CEL syntax (`b"..."`).
  if (!c.Eof() && c.Peek() == 'b') {
    ++c.pos;
  }
  auto s = ParseQuotedString(c);
  if (!s.ok()) return s.status();
  return Value::Bytes(*std::move(s));
}

absl::StatusOr<Value> ParseDuration(Cursor& c) {
  auto s = ParseQuotedString(c);
  if (!s.ok()) return s.status();
  absl::Duration d;
  if (!absl::ParseDuration(*s, &d)) {
    return absl::InvalidArgumentError(
        absl::StrCat("malformed duration literal: \"", *s, "\""));
  }
  return Value::Duration(d);
}

absl::StatusOr<Value> ParseTimestamp(Cursor& c) {
  auto s = ParseQuotedString(c);
  if (!s.ok()) return s.status();
  absl::Time t;
  std::string err;
  if (!absl::ParseTime(absl::RFC3339_full, *s, &t, &err)) {
    return absl::InvalidArgumentError(
        absl::StrCat("malformed timestamp literal \"", *s, "\": ", err));
  }
  return Value::Timestamp(t);
}

// Forward declarations for the recursive aggregate parsers.
absl::StatusOr<Value> ParseAtomForType(
    Cursor& c, const CelType& t, const google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory);

absl::StatusOr<Value> ParseList(Cursor& c, const CelType& elem_t,
                                const google::protobuf::DescriptorPool& pool,
                                google::protobuf::DynamicMessageFactory& fac) {
  auto s = c.Expect('[');
  if (!s.ok()) return s;
  std::vector<Value> elements;
  c.Skip();
  if (!c.Eof() && c.Peek() == ']') {
    ++c.pos;
    return Value::List(std::move(elements));
  }
  while (true) {
    auto v = ParseAtomForType(c, elem_t, pool, fac);
    if (!v.ok()) return v.status();
    elements.push_back(*std::move(v));
    c.Skip();
    if (c.ConsumeChar(',')) continue;
    if (c.ConsumeChar(']')) break;
    return absl::InvalidArgumentError(
        absl::StrCat("expected ',' or ']' in list literal at offset ", c.pos,
                     " in: ", c.src));
  }
  return Value::List(std::move(elements));
}

absl::StatusOr<Value> ParseMap(Cursor& c, const CelType& key_t,
                               const CelType& val_t,
                               const google::protobuf::DescriptorPool& pool,
                               google::protobuf::DynamicMessageFactory& fac) {
  auto s = c.Expect('{');
  if (!s.ok()) return s;
  std::vector<std::pair<Value, Value>> entries;
  c.Skip();
  if (!c.Eof() && c.Peek() == '}') {
    ++c.pos;
    return Value::Map(std::move(entries));
  }
  while (true) {
    auto k = ParseAtomForType(c, key_t, pool, fac);
    if (!k.ok()) return k.status();
    s = c.Expect(':');
    if (!s.ok()) return s;
    auto v = ParseAtomForType(c, val_t, pool, fac);
    if (!v.ok()) return v.status();
    entries.emplace_back(*std::move(k), *std::move(v));
    c.Skip();
    if (c.ConsumeChar(',')) continue;
    if (c.ConsumeChar('}')) break;
    return absl::InvalidArgumentError(
        absl::StrCat("expected ',' or '}' in map literal at offset ", c.pos,
                     " in: ", c.src));
  }
  return Value::Map(std::move(entries));
}

// Message body parsing.  Accepts:
//   @path.txtpb  / .textproto  → TextFormat::Parse
//   @path.json                  → util::JsonStringToMessage
//   @path.pb / .bin             → Message::ParseFromString
//   txtpb:<inline>              → TextFormat::Parse (inline body)
//   json:<inline>               → util::JsonStringToMessage (inline body)
//   pb:@path                    → ParseFromString
//
// Extension-driven dispatch on @path; explicit `prefix:` overrides.
enum class MsgFormat : std::uint8_t { kTextproto, kJson, kBinary };

absl::StatusOr<MsgFormat> InferFormatFromPath(absl::string_view path) {
  if (absl::EndsWith(path, ".txtpb") || absl::EndsWith(path, ".textproto") ||
      absl::EndsWith(path, ".pbtxt") || absl::EndsWith(path, ".prototxt")) {
    return MsgFormat::kTextproto;
  }
  if (absl::EndsWith(path, ".json")) {
    return MsgFormat::kJson;
  }
  if (absl::EndsWith(path, ".pb") || absl::EndsWith(path, ".bin")) {
    return MsgFormat::kBinary;
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "cannot infer proto format from path `", path,
      "`; use explicit txtpb:/json:/pb: prefix or rename to .txtpb/.json/.pb"));
}

absl::Status PopulateMessage(google::protobuf::Message& m, MsgFormat fmt,
                             absl::string_view body) {
  switch (fmt) {
    case MsgFormat::kTextproto: {
      if (!google::protobuf::TextFormat::ParseFromString(std::string(body),
                                                         &m)) {
        return absl::InvalidArgumentError(
            absl::StrCat("TextFormat::Parse failed for message ",
                         m.GetDescriptor()->full_name()));
      }
      return absl::OkStatus();
    }
    case MsgFormat::kJson: {
      auto s =
          google::protobuf::util::JsonStringToMessage(std::string(body), &m);
      if (!s.ok()) {
        return absl::InvalidArgumentError(
            absl::StrCat("JsonStringToMessage failed for message ",
                         m.GetDescriptor()->full_name(), ": ", s.message()));
      }
      return absl::OkStatus();
    }
    case MsgFormat::kBinary: {
      if (!m.ParseFromString(std::string(body))) {
        return absl::InvalidArgumentError(
            absl::StrCat("binary ParseFromString failed for message ",
                         m.GetDescriptor()->full_name()));
      }
      return absl::OkStatus();
    }
  }
  ABSL_CHECK(false) << "unhandled MsgFormat: " << static_cast<int>(fmt);
}

absl::StatusOr<Value> ParseMessage(
    Cursor& c, const CelType& t, const google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
  const std::string fqn(t.message_fully_qualified_name());
  const google::protobuf::Descriptor* desc = pool.FindMessageTypeByName(fqn);
  if (desc == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("message type `", fqn,
                     "` not found in descriptor pool — "
                     "did you pass --proto or --descriptor_set?"));
  }
  const google::protobuf::Message* proto = factory.GetPrototype(desc);
  if (proto == nullptr) {
    return absl::InternalError(absl::StrCat(
        "DynamicMessageFactory returned no prototype for `", fqn, "`"));
  }

  c.Skip();
  absl::string_view rest = c.src.substr(c.pos);
  c.pos = c.src.size();

  // Strip explicit format prefix if present.
  std::optional<MsgFormat> explicit_fmt;
  if (absl::ConsumePrefix(&rest, "txtpb:"))
    explicit_fmt = MsgFormat::kTextproto;
  else if (absl::ConsumePrefix(&rest, "json:"))
    explicit_fmt = MsgFormat::kJson;
  else if (absl::ConsumePrefix(&rest, "pb:"))
    explicit_fmt = MsgFormat::kBinary;

  // File reference vs inline body.
  std::string body;
  MsgFormat fmt;
  if (!rest.empty() && rest.front() == '@') {
    const std::string path(rest.substr(1));
    if (explicit_fmt.has_value()) {
      fmt = *explicit_fmt;
    } else {
      auto inferred = InferFormatFromPath(path);
      if (!inferred.ok()) return inferred.status();
      fmt = *inferred;
    }
    auto buf = ReadFile(path);
    if (!buf.ok()) return buf.status();
    body = *std::move(buf);
  } else {
    if (!explicit_fmt.has_value()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "inline message body must use txtpb:/json:/pb: prefix, got `", rest,
          "`"));
    }
    fmt = *explicit_fmt;
    body = std::string(rest);
  }

  auto m = std::unique_ptr<google::protobuf::Message>(proto->New());
  if (auto s = PopulateMessage(*m, fmt, body); !s.ok()) return s;
  return Value::OwnedMessage(std::move(m));
}

absl::StatusOr<Value> ParseAtomForType(
    Cursor& c, const CelType& t, const google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
  switch (t.kind()) {
    case CelType::Kind::kBool:
      return ParseBool(c);
    case CelType::Kind::kInt:
      return ParseInt(c);
    case CelType::Kind::kUint:
      return ParseUint(c);
    case CelType::Kind::kDouble:
      return ParseDouble(c);
    case CelType::Kind::kString:
      return ParseString(c);
    case CelType::Kind::kBytes:
      return ParseBytes(c);
    case CelType::Kind::kDuration:
      return ParseDuration(c);
    case CelType::Kind::kTimestamp:
      return ParseTimestamp(c);
    case CelType::Kind::kList:
      return ParseList(c, t.list_element(), pool, factory);
    case CelType::Kind::kMap:
      return ParseMap(c, t.map_key(), t.map_value(), pool, factory);
    case CelType::Kind::kMessage:
      return ParseMessage(c, t, pool, factory);
    case CelType::Kind::kType:
    case CelType::Kind::kUnknown:
    case CelType::Kind::kNull:
    case CelType::Kind::kOptional:
      return absl::InvalidArgumentError(
          absl::StrCat("--var: cannot bind a value of type `",
                       ::celwasm::CelTypeKindName(t.kind()), "`"));
  }
  ABSL_CHECK(false) << "ParseAtomForType: unhandled CelType::Kind = "
                    << static_cast<int>(t.kind());
}

}  // namespace

// ---------- Public entry points ---------------------------------------------

absl::StatusOr<CelType> ParseTypeSpec(absl::string_view spec) {
  Cursor c{spec};
  auto t = ParseTypeRec(c);
  if (!t.ok()) return t.status();
  c.Skip();
  if (!c.Eof()) {
    return absl::InvalidArgumentError(
        absl::StrCat("unexpected trailing characters in type spec at offset ",
                     c.pos, ": `", spec, "`"));
  }
  return t;
}

absl::StatusOr<ParsedVar> ParseVarFlag(
    absl::string_view flag, const google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
  const size_t colon = flag.find(':');
  if (colon == absl::string_view::npos) {
    return absl::InvalidArgumentError(absl::StrCat(
        "--var: expected `name:Type=value`, missing ':' in: ", flag));
  }
  std::string name(flag.substr(0, colon));
  if (name.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("--var: empty variable name in: ", flag));
  }
  const size_t eq = flag.find('=', colon + 1);
  absl::string_view type_spec;
  absl::string_view value_lit;
  if (eq == absl::string_view::npos) {
    type_spec = flag.substr(colon + 1);
    value_lit = {};
  } else {
    type_spec = flag.substr(colon + 1, eq - colon - 1);
    value_lit = flag.substr(eq + 1);
  }
  auto t = ParseTypeSpec(type_spec);
  if (!t.ok()) {
    return absl::Status(t.status().code(), absl::StrCat("--var ", name, ": ",
                                                        t.status().message()));
  }
  ParsedVar out;
  out.name = std::move(name);
  out.type = *std::move(t);
  if (eq == absl::string_view::npos) {
    // Declaration-only form (used by `cel check` and `cel compile`).
    out.has_value = false;
    return out;
  }
  Cursor vc{value_lit};
  auto v = ParseAtomForType(vc, out.type, pool, factory);
  if (!v.ok()) {
    return absl::Status(
        v.status().code(),
        absl::StrCat("--var ", out.name, ": ", v.status().message()));
  }
  vc.Skip();
  if (!vc.Eof()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "--var ", out.name, ": unexpected trailing characters at offset ",
        vc.pos, " in value: ", value_lit));
  }
  out.value = *std::move(v);
  out.has_value = true;
  return out;
}

}  // namespace celwasm::tools::cel
