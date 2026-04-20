// Minimal port of cel-cpp's `AttributePattern` machinery, trimmed to
// the subset the wasm host needs for Slice E2a.1 partial-eval:
//   - only string qualifiers (field selects; map lookups and list
//     indexing land with M5 collections);
//   - wildcards as `*` segments in a dotted path;
//   - `MatchType { NONE, PARTIAL, FULL }` matching an `Attribute`
//     against a pattern, with the same semantics as cel-cpp
//     `AttributePattern::IsMatch` (pattern-shorter-or-equal-to-attribute
//     → FULL, pattern-longer → PARTIAL, any qualifier mismatch → NONE).
//
// When a select-site's attribute path matches a host-configured
// unknown pattern at FULL, the `cel_host.get_field` trampoline
// produces `CelValue{CEL_UNKNOWN}` instead of resolving the field, and
// the 3VL plumbing elsewhere carries it to the eval result.  PARTIAL
// matches do NOT produce UNKNOWN on their own — they resolve
// normally; a deeper select under the partial match will FULL-match
// and produce UNKNOWN there.
//
// The parser accepts dotted paths of the form
//   variable ( '.' qualifier )*
// where `qualifier` is either a bare identifier or a literal `*`
// (wildcard).  Example: `request.user.*`.  The parser rejects leading
// dots, trailing dots, consecutive dots, and empty input.

#ifndef CELWASM_COMPILER_HOST_ATTRIBUTE_H_
#define CELWASM_COMPILER_HOST_ATTRIBUTE_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

// One resolved qualifier segment.  Slice E2a.1 only uses string
// qualifiers (proto / struct field names).  Keyed map lookups and
// list indices land in M5 — when they do, flip this to a variant.
class AttributeQualifier {
 public:
  explicit AttributeQualifier(std::string name) : name_(std::move(name)) {}

  absl::string_view name() const { return name_; }

  bool operator==(const AttributeQualifier& other) const {
    return name_ == other.name_;
  }

 private:
  std::string name_;
};

// One segment of a pattern.  Either a concrete name or a wildcard
// (matches any qualifier value).  Wildcard only matches at its
// position, not across the path (e.g. `a.*` matches `a.x` but not
// `a.x.y` — length rules below handle longer attributes via
// MatchType::FULL).
class AttributeQualifierPattern {
 public:
  static AttributeQualifierPattern OfName(std::string name) {
    return AttributeQualifierPattern(
        std::optional<AttributeQualifier>(AttributeQualifier(std::move(name))));
  }
  static AttributeQualifierPattern Wildcard() {
    return AttributeQualifierPattern(std::nullopt);
  }

  bool is_wildcard() const { return !value_.has_value(); }

  bool IsMatch(const AttributeQualifier& q) const {
    if (is_wildcard()) return true;
    return value_.value() == q;
  }

 private:
  explicit AttributeQualifierPattern(std::optional<AttributeQualifier> v)
      : value_(std::move(v)) {}
  std::optional<AttributeQualifier> value_;
};

// A resolved attribute path: root variable + qualifier chain.  The
// host loader builds one of these per `CelAbi.AttributeEntry` parsed
// out of the `cel.abi` custom section, and hands it to the trampoline
// through `CelHostEnv::LookupAttribute`.
class Attribute {
 public:
  Attribute(std::string variable, std::vector<AttributeQualifier> path)
      : variable_(std::move(variable)), path_(std::move(path)) {}

  absl::string_view variable() const { return variable_; }
  absl::Span<const AttributeQualifier> qualifier_path() const { return path_; }

 private:
  std::string variable_;
  std::vector<AttributeQualifier> path_;
};

// A pattern as configured by the host embedder — parsed from a
// dotted path like `request.user.*` or constructed programmatically.
class AttributePattern {
 public:
  enum class MatchType {
    NONE,     // pattern does not match this attribute nor any of its children
    PARTIAL,  // pattern refers to something nested within the attribute
    FULL,     // pattern matches the attribute itself (or one of its ancestors)
  };

  AttributePattern(std::string variable,
                   std::vector<AttributeQualifierPattern> path)
      : variable_(std::move(variable)), path_(std::move(path)) {}

  absl::string_view variable() const { return variable_; }

  // Implements the same three-way decision as cel-cpp's
  // `cel::AttributePattern::IsMatch`:
  //   - different root variable             → NONE
  //   - pattern length > attribute length   → PARTIAL (if all shared
  //                                           qualifiers match)
  //   - pattern length ≤ attribute length   → FULL (if all pattern
  //                                           qualifiers match)
  //   - any qualifier mismatch              → NONE
  MatchType IsMatch(const Attribute& attr) const;

 private:
  std::string variable_;
  std::vector<AttributeQualifierPattern> path_;
};

// Parses `variable ( '.' ( identifier | '*' ) )*` into an
// `AttributePattern`.  Identifiers may contain letters, digits, and
// underscores (the same shape cel-cpp parses).  Leading dots,
// trailing dots, consecutive dots, empty paths, and empty segments
// all return InvalidArgument.
absl::StatusOr<AttributePattern> ParseUnknownAttributePattern(
    absl::string_view text);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_HOST_ATTRIBUTE_H_
