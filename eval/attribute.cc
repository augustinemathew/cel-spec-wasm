#include "eval/attribute.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

// ————————— AttributeQualifier —————————

AttributeQualifier AttributeQualifier::OfString(std::string value) {
  return AttributeQualifier(std::move(value));
}

absl::StatusOr<std::string> AttributeQualifier::AsCanonicalString() const {
  // Reject embedded NUL / non-printables for diagnostics — they
  // can't be rendered unambiguously without escaping, and the
  // canonical form is meant to be human-readable.
  for (char c : value_) {
    if (c < 0x20 || c == '"') {
      return absl::InvalidArgumentError(
          "AttributeQualifier::AsCanonicalString: string contains "
          "unprintable or quote characters");
    }
  }
  return absl::StrCat("[\"", value_, "\"]");
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
    // Validate renderability (unprintable / quote bytes reject), then
    // use the dotted form (`.foo`) — cleaner than bracketed
    // (`["foo"]`) for the field-like keys the resolver interns.
    if (auto s = q.AsCanonicalString(); !s.ok()) return s.status();
    pieces.push_back(absl::StrCat(".", q.value()));
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

bool IsIdentStart(char c) {
  return absl::ascii_isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool IsIdentCont(char c) {
  return absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The reason a character ends a segment illegally — the suffix of the
// InvalidArgument message (the caller prepends the pattern context).
std::string BadCharReason(char c) {
  if (c == '[' || c == ']') {
    return "has a bracket / index / key qualifier (not supported — the "
           "resolver never interns index/key access)";
  }
  if (absl::ascii_isspace(static_cast<unsigned char>(c))) {
    return "has whitespace in a segment";
  }
  return absl::StrCat("has an unexpected character `", absl::string_view(&c, 1),
                      "` in a segment");
}

// State machine implementing the pattern grammar (the ONLY syntax — see
// the header):
//
//   pattern   := root ( '.' qualifier )*
//   root      := ident
//   qualifier := '*' | ident
//   ident     := [A-Za-z_] [A-Za-z0-9_]*
//
// `*` is the wildcard qualifier; the root and every other qualifier is
// a CEL identifier.  Everything else is rejected with InvalidArgument —
// empty input or segment, leading / trailing / consecutive dot, any
// bracket or index/key form, whitespace, a `*` adjacent to other
// characters or used as the root, and a digit- or punctuation-leading
// segment — because the resolver only interns dotted string `.field`
// qualifiers, so a pattern it cannot match must fail loudly rather than
// silently match nothing.  A hand-split scanner is where this kind of
// mini-language accretes edge-case bugs, hence the explicit FSM.
//
//   kSegStart  — at input start or just after '.': expect an ident
//                start, or '*' for a non-root qualifier.
//   kInIdent   — inside an ident: ident-cont chars, '.', or end.
//   kAfterStar — just consumed a lone '*': only '.' or end may follow.
class PatternParser {
 public:
  explicit PatternParser(absl::string_view dotted) : dotted_(dotted) {}

  absl::StatusOr<AttributePattern> Run() {
    for (size_t i = 0; i < dotted_.size(); ++i) {
      if (auto s = Step(i, dotted_[i]); !s.ok()) return s;
    }
    if (state_ == State::kSegStart) {
      return dotted_.empty() ? Err("is empty") : Err("has a trailing dot");
    }
    Commit(dotted_.size(), /*is_star=*/state_ == State::kAfterStar);
    return AttributePattern(std::move(variable_), std::move(path_));
  }

 private:
  enum class State : uint8_t { kSegStart, kInIdent, kAfterStar };

  absl::Status Err(absl::string_view why) const {
    return absl::InvalidArgumentError(
        absl::StrCat("AttributePattern::Parse: pattern `", dotted_, "` ", why));
  }

  // The first committed segment is the root variable; the rest are
  // qualifiers (`*` -> wildcard, else a string key).
  void Commit(size_t end, bool is_star) {
    absl::string_view seg = dotted_.substr(seg_begin_, end - seg_begin_);
    if (!have_root_) {
      variable_ = std::string(seg);  // root reaches here only as an ident
      have_root_ = true;
    } else if (is_star) {
      path_.push_back(AttributeQualifierPattern::Wildcard());
    } else {
      path_.push_back(AttributeQualifierPattern::OfString(std::string(seg)));
    }
  }

  absl::Status Step(size_t i, char c) {
    switch (state_) {
      case State::kSegStart:
        return StepSegStart(i, c);
      case State::kInIdent:
        return StepInIdent(i, c);
      case State::kAfterStar:
        return StepAfterStar(c);
    }
    ABSL_CHECK(false) << "unreachable PatternParser state";
    return absl::InternalError("unreachable");
  }

  absl::Status StepSegStart(size_t i, char c) {
    if (IsIdentStart(c)) {
      seg_begin_ = i;
      state_ = State::kInIdent;
    } else if (c == '*' && have_root_) {
      seg_begin_ = i;
      state_ = State::kAfterStar;
    } else if (c == '*') {
      return Err("uses `*` as the root (the root must be a variable name)");
    } else if (c == '.') {
      return Err("has an empty segment (leading or consecutive dot)");
    } else {
      return Err(BadCharReason(c));
    }
    return absl::OkStatus();
  }

  absl::Status StepInIdent(size_t i, char c) {
    if (IsIdentCont(c)) return absl::OkStatus();
    if (c == '.') {
      Commit(i, /*is_star=*/false);
      state_ = State::kSegStart;
      return absl::OkStatus();
    }
    return Err(BadCharReason(c));
  }

  absl::Status StepAfterStar(char c) {
    if (c != '.') {
      return Err(
          "has `*` adjacent to other characters (the wildcard is a whole "
          "segment)");
    }
    Commit(seg_begin_ + 1, /*is_star=*/true);  // the lone '*' segment
    state_ = State::kSegStart;
    return absl::OkStatus();
  }

  const absl::string_view dotted_;
  std::string variable_;
  std::vector<AttributeQualifierPattern> path_;
  bool have_root_ = false;
  size_t seg_begin_ = 0;
  State state_ = State::kSegStart;
};

}  // namespace

absl::StatusOr<AttributePattern> AttributePattern::Parse(
    absl::string_view dotted) {
  return PatternParser(dotted).Run();
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
