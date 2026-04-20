#include "compiler/host/attribute.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace celwasm {
namespace {

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

bool IsIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

absl::StatusOr<std::string> ReadIdent(absl::string_view text, size_t* pos) {
  const size_t start = *pos;
  if (start >= text.size() || !IsIdentStart(text.at(start))) {
    return absl::InvalidArgumentError(
        absl::StrCat("unknown-attrs: expected identifier at offset ", start,
                     " in `", text, "`"));
  }
  size_t end = start + 1;
  while (end < text.size() && IsIdentChar(text.at(end))) {
    ++end;
  }
  *pos = end;
  return std::string(text.substr(start, end - start));
}

}  // namespace

AttributePattern::MatchType AttributePattern::IsMatch(
    const Attribute& attr) const {
  if (attr.variable() != variable_) return MatchType::kNone;
  // cel-cpp's convention: if the pattern is longer than the attribute,
  // the pattern *might* describe something nested inside the attribute
  // — kPartial.  Otherwise the pattern covers the attribute (and any
  // children) — kFull.
  const auto pat_len = path_.size();
  const auto qualifier_path = attr.qualifier_path();
  const auto attr_len = qualifier_path.size();
  const MatchType result =
      pat_len > attr_len ? MatchType::kPartial : MatchType::kFull;
  const size_t cmp_len = pat_len < attr_len ? pat_len : attr_len;
  auto attr_it = qualifier_path.begin();
  for (size_t i = 0; i < cmp_len; ++i, ++attr_it) {
    if (!path_.at(i).IsMatch(*attr_it)) return MatchType::kNone;
  }
  return result;
}

absl::StatusOr<AttributePattern> ParseUnknownAttributePattern(
    absl::string_view text) {
  if (text.empty()) {
    return absl::InvalidArgumentError(
        "unknown-attrs: pattern is empty (expected `var.field.*` shape)");
  }
  size_t pos = 0;
  auto variable = ReadIdent(text, &pos);
  if (!variable.ok()) return variable.status();
  std::vector<AttributeQualifierPattern> path;
  while (pos < text.size()) {
    if (text.at(pos) != '.') {
      return absl::InvalidArgumentError(absl::StrCat(
          "unknown-attrs: expected `.` at offset ", pos, " in `", text, "`"));
    }
    ++pos;  // consume '.'
    if (pos < text.size() && text.at(pos) == '*') {
      path.push_back(AttributeQualifierPattern::Wildcard());
      ++pos;
      continue;
    }
    auto q = ReadIdent(text, &pos);
    if (!q.ok()) return q.status();
    path.push_back(AttributeQualifierPattern::OfName(*std::move(q)));
  }
  return AttributePattern(*std::move(variable), std::move(path));
}

}  // namespace celwasm
