#include "eval/attribute.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

// ————————— AttributeQualifier —————————

AttributeQualifier AttributeQualifier::OfString(std::string value) {
  return AttributeQualifier(
      Variant{std::in_place_type<std::string>, std::move(value)});
}

AttributeQualifier::Kind AttributeQualifier::kind() const {
  switch (value_.index()) {
    case 0:
      return Kind::kInt;
    case 1:
      return Kind::kUint;
    case 2:
      return Kind::kString;
    case 3:
      return Kind::kBool;
  }
  ABSL_CHECK(false) << "unhandled variant index " << value_.index();
}

std::optional<int64_t> AttributeQualifier::AsInt() const {
  if (!std::holds_alternative<int64_t>(value_)) return std::nullopt;
  return std::get<int64_t>(value_);
}
std::optional<uint64_t> AttributeQualifier::AsUint() const {
  if (!std::holds_alternative<uint64_t>(value_)) return std::nullopt;
  return std::get<uint64_t>(value_);
}
std::optional<absl::string_view> AttributeQualifier::AsString() const {
  if (!std::holds_alternative<std::string>(value_)) return std::nullopt;
  return absl::string_view(std::get<std::string>(value_));
}
std::optional<bool> AttributeQualifier::AsBool() const {
  if (!std::holds_alternative<bool>(value_)) return std::nullopt;
  return std::get<bool>(value_);
}

bool AttributeQualifier::MatchesStringKey(absl::string_view key) const {
  auto s = AsString();
  return s.has_value() && *s == key;
}

absl::StatusOr<std::string> AttributeQualifier::AsCanonicalString() const {
  // Internal accessors: we already know the alternative from kind(),
  // so go through std::get on the variant rather than the public
  // AsX() optionals (clang-tidy's unchecked-optional-access check
  // can't prove the optionals are populated here).
  switch (kind()) {
    case Kind::kInt:
      return absl::StrCat("[", std::get<int64_t>(value_), "]");
    case Kind::kUint:
      return absl::StrCat("[", std::get<uint64_t>(value_), "u]");
    case Kind::kString: {
      const auto& s = std::get<std::string>(value_);
      // Reject embedded NUL / non-printables for diagnostics — they
      // can't be rendered unambiguously without escaping, and the
      // canonical form is meant to be human-readable.
      for (char c : s) {
        if (c < 0x20 || c == '"') {
          return absl::InvalidArgumentError(
              "AttributeQualifier::AsCanonicalString: string contains "
              "unprintable or quote characters");
        }
      }
      return absl::StrCat("[\"", s, "\"]");
    }
    case Kind::kBool:
      return std::get<bool>(value_) ? "[true]" : "[false]";
  }
  ABSL_CHECK(false) << "unhandled Kind = " << static_cast<int>(kind());
}

bool AttributeQualifier::operator==(const AttributeQualifier& other) const {
  return value_ == other.value_;
}

bool AttributeQualifier::operator<(const AttributeQualifier& other) const {
  if (kind() != other.kind()) {
    return static_cast<int>(kind()) < static_cast<int>(other.kind());
  }
  switch (kind()) {
    case Kind::kInt:
      return std::get<int64_t>(value_) < std::get<int64_t>(other.value_);
    case Kind::kUint:
      return std::get<uint64_t>(value_) < std::get<uint64_t>(other.value_);
    case Kind::kString:
      return std::get<std::string>(value_) <
             std::get<std::string>(other.value_);
    case Kind::kBool:
      return static_cast<int>(std::get<bool>(value_)) <
             static_cast<int>(std::get<bool>(other.value_));
  }
  ABSL_CHECK(false) << "unhandled Kind = " << static_cast<int>(kind());
}

// ————————— AttributeQualifierPattern —————————

AttributeQualifierPattern AttributeQualifierPattern::Wildcard() {
  return AttributeQualifierPattern(std::nullopt);
}
AttributeQualifierPattern AttributeQualifierPattern::OfString(
    std::string value) {
  return AttributeQualifierPattern(
      AttributeQualifier::OfString(std::move(value)));
}

AttributeQualifierPattern::AttributeQualifierPattern(
    AttributeQualifier qualifier)
    : value_(std::move(qualifier)) {}

bool AttributeQualifierPattern::IsMatch(
    const AttributeQualifier& qualifier) const {
  // IsWildcard() returns !value_.has_value(); falling through here
  // means value_ holds a qualifier.
  if (IsWildcard()) return true;
  ABSL_CHECK(value_.has_value());
  return *value_ == qualifier;
}

bool AttributeQualifierPattern::IsMatch(absl::string_view key) const {
  if (IsWildcard()) return true;
  ABSL_CHECK(value_.has_value());
  return value_->MatchesStringKey(key);
}

// ————————— Attribute —————————

Attribute::Attribute(std::string variable_name)
    : Attribute(std::move(variable_name), {}) {}

Attribute::Attribute(std::string variable_name,
                     std::vector<AttributeQualifier> qualifier_path)
    : impl_(std::make_shared<Impl>(std::move(variable_name),
                                   std::move(qualifier_path))) {}

absl::string_view Attribute::variable_name() const {
  return impl_->variable_name;
}
bool Attribute::has_variable_name() const {
  return !impl_->variable_name.empty();
}
absl::Span<const AttributeQualifier> Attribute::qualifier_path() const {
  return impl_->qualifier_path;
}

bool Attribute::operator==(const Attribute& other) const {
  return impl_->variable_name == other.impl_->variable_name &&
         impl_->qualifier_path == other.impl_->qualifier_path;
}

bool Attribute::operator<(const Attribute& other) const {
  if (impl_->variable_name != other.impl_->variable_name) {
    return impl_->variable_name < other.impl_->variable_name;
  }
  const auto& a = impl_->qualifier_path;
  const auto& b = other.impl_->qualifier_path;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    if (a[i] < b[i]) return true;
    if (b[i] < a[i]) return false;
  }
  return a.size() < b.size();
}

absl::StatusOr<std::string> Attribute::AsString() const {
  std::vector<std::string> pieces;
  pieces.reserve(1 + impl_->qualifier_path.size());
  pieces.push_back(impl_->variable_name);
  for (const auto& q : impl_->qualifier_path) {
    auto s = q.AsCanonicalString();
    if (!s.ok()) return s.status();
    // String keys in dotted form (`.foo`) are cleaner than bracketed
    // (`["foo"]`) for the common field-like case.
    if (q.kind() == AttributeQualifier::Kind::kString) {
      auto key = q.AsString();
      ABSL_CHECK(key.has_value());
      pieces.push_back(absl::StrCat(".", *key));
    } else {
      pieces.push_back(*s);
    }
  }
  return absl::StrJoin(pieces, "");
}

// ————————— AttributePattern —————————

AttributePattern::AttributePattern(
    std::string variable, std::vector<AttributeQualifierPattern> qualifier_path)
    : variable_(std::move(variable)),
      qualifier_path_(std::move(qualifier_path)) {}

absl::string_view AttributePattern::variable() const {
  return variable_;
}
absl::Span<const AttributeQualifierPattern> AttributePattern::qualifier_path()
    const {
  return qualifier_path_;
}

namespace {

// Split a dotted pattern into atomic identifier segments.  Only the
// dotted `.field` form (with `*` as the wildcard shorthand) is
// supported: bracket / index / key qualifiers are rejected because
// the resolver never interns them (index access breaks the attribute
// chain), so a pattern naming one could never match an attribute.
absl::StatusOr<std::vector<absl::string_view>> Tokenize(
    absl::string_view dotted) {
  std::vector<absl::string_view> out;
  size_t i = 0;
  // Variable name — up to the first `.`.  Any `[` here is an
  // unsupported qualifier form (see the function comment).  May not
  // be empty (the bare-empty case is handled by the caller).
  size_t var_end = dotted.find('.');
  if (var_end == absl::string_view::npos) var_end = dotted.size();
  if (var_end == 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("AttributePattern::Parse: pattern `", dotted,
                     "` has an empty variable segment"));
  }
  absl::string_view var = dotted.substr(0, var_end);
  if (var.find_first_of("[]") != absl::string_view::npos) {
    return absl::InvalidArgumentError(
        absl::StrCat("AttributePattern::Parse: pattern `", dotted,
                     "` has an unexpected character (bracket / index / key "
                     "qualifiers are not supported)"));
  }
  out.push_back(var);
  i = var_end;
  while (i < dotted.size()) {
    ABSL_CHECK_EQ(dotted[i], '.');
    ++i;
    // Dotted identifier chunk up to next `.` or end.
    size_t end = dotted.find('.', i);
    if (end == absl::string_view::npos) end = dotted.size();
    if (end == i) {
      return absl::InvalidArgumentError(
          absl::StrCat("AttributePattern::Parse: pattern `", dotted,
                       "` has an empty dotted segment"));
    }
    absl::string_view seg = dotted.substr(i, end - i);
    if (seg.find_first_of("[]") != absl::string_view::npos) {
      return absl::InvalidArgumentError(
          absl::StrCat("AttributePattern::Parse: pattern `", dotted,
                       "` has an unexpected character (bracket / index / key "
                       "qualifiers are not supported)"));
    }
    out.push_back(seg);
    i = end;
  }
  return out;
}

}  // namespace

absl::StatusOr<AttributePattern> AttributePattern::Parse(
    absl::string_view dotted) {
  if (dotted.empty()) {
    return absl::InvalidArgumentError(
        "AttributePattern::Parse: pattern is empty");
  }
  if (dotted.front() == '.' || dotted.back() == '.') {
    return absl::InvalidArgumentError(
        absl::StrCat("AttributePattern::Parse: pattern `", dotted,
                     "` has a leading or trailing dot"));
  }
  auto tokens_or = Tokenize(dotted);
  if (!tokens_or.ok()) return tokens_or.status();
  const auto& tokens = *tokens_or;
  ABSL_CHECK(!tokens.empty());
  // The first segment is the root variable.  Remaining segments are
  // qualifier patterns: dotted identifiers become string-keyed
  // qualifiers, with `*` as the wildcard shorthand.
  std::string variable(tokens.front());
  std::vector<AttributeQualifierPattern> path;
  path.reserve(tokens.size() - 1);
  for (size_t idx = 1; idx < tokens.size(); ++idx) {
    absl::string_view tok = tokens[idx];
    if (tok == "*") {
      path.push_back(AttributeQualifierPattern::Wildcard());
    } else {
      path.push_back(AttributeQualifierPattern::OfString(std::string(tok)));
    }
  }
  return AttributePattern(std::move(variable), std::move(path));
}

AttributePattern::MatchType AttributePattern::IsMatch(
    const Attribute& attribute) const {
  if (attribute.variable_name() != variable_) return MatchType::kNone;

  const auto attr_path = attribute.qualifier_path();
  size_t max_index = qualifier_path_.size();
  MatchType result = MatchType::kFull;
  // Pattern longer than attribute → at best PARTIAL (pattern names
  // something nested below the attribute).
  if (qualifier_path_.size() > attr_path.size()) {
    max_index = attr_path.size();
    result = MatchType::kPartial;
  }
  for (size_t i = 0; i < max_index; ++i) {
    if (!qualifier_path_[i].IsMatch(attr_path[i])) return MatchType::kNone;
  }
  return result;
}

absl::string_view AttributePatternMatchTypeName(AttributePattern::MatchType m) {
  switch (m) {
    case AttributePattern::MatchType::kNone:
      return "none";
    case AttributePattern::MatchType::kPartial:
      return "partial";
    case AttributePattern::MatchType::kFull:
      return "full";
  }
  ABSL_CHECK(false) << "unhandled MatchType = " << static_cast<int>(m);
}

}  // namespace celwasm
