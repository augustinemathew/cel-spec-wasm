// Attribute / AttributePattern / AttributeQualifier — the cel-cpp-
// shaped attribute model, rehosted in the `cel::` public namespace
// for v2 consumers.  Mirrors `third_party/cel-cpp/base/attribute.h`
// in naming and semantics; a user familiar with cel-cpp should see
// no surprises here.
//
//   AttributeQualifier        — one string-keyed segment of a
//                               resolved attribute path.
//   AttributeQualifierPattern — one segment of a pattern; may be a
//                               wildcard ("any value matches").
//   Attribute                 — a resolved path: variable + qualifiers.
//   AttributePattern          — a pattern: variable + qualifier-
//                               patterns; matches Attribute with
//                               NONE / PARTIAL / FULL granularity.
//
// Additions over cel-cpp:
//
//   AttributeId  — a dense uint32 index into `cel.abi.attributes[]`,
//                  used as the payload of `Value::Unknown` on the
//                  wire.  The host resolves id → `Attribute` at
//                  LoadEval from the ABI; `AttributeId` itself never
//                  contains the resolved path.  cel-cpp has no
//                  equivalent because it's an interpreter (resolved
//                  Attributes travel directly).
//
// ErrorCode / ErrorPayload (the other 3VL carrier) live in
// `eval/error.h` — kept separate so code that only
// needs attribute matching (e.g. partial-eval plumbing) doesn't
// pull in the error header.

#ifndef CELWASM_EVAL_ATTRIBUTE_H_
#define CELWASM_EVAL_ATTRIBUTE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

// ————————— AttributeQualifier —————————
//
// One segment in a resolved attribute path.  Segments are string
// keys only (`foo["bar"]` or `foo.bar`): the resolver interns string
// `.field` qualifiers exclusively (index / key access breaks the
// attribute chain — see e2e/m2_partial_eval_test.cc §8), so an int /
// uint / bool qualifier can never be built or matched.  cel-cpp's
// typed-variant breadth was deliberately dropped here; restore it
// from git if index-qualified attributes ever become internable.
class AttributeQualifier final {
 public:
  static AttributeQualifier OfString(std::string value);

  // The string key this segment selects.
  absl::string_view value() const {
    return value_;
  }

  // String-key equivalence — short-circuit for the common case of
  // field-like access in a `has()` / `select` resolution.
  bool MatchesStringKey(absl::string_view key) const {
    return value_ == key;
  }

  // Canonical bracketed form (for diagnostics): `"[\"key\"]"`.
  // Returns InvalidArgument on unprintable or quote bytes — they
  // can't be rendered unambiguously without escaping.
  absl::StatusOr<std::string> AsCanonicalString() const;

  bool operator==(const AttributeQualifier& other) const {
    return value_ == other.value_;
  }
  bool operator!=(const AttributeQualifier& other) const {
    return !(*this == other);
  }
  bool operator<(const AttributeQualifier& other) const {
    return value_ < other.value_;
  }

 private:
  explicit AttributeQualifier(std::string v) : value_(std::move(v)) {}
  std::string value_;
};

// ————————— AttributeQualifierPattern —————————
//
// A segment of a pattern.  Either carries a concrete qualifier
// (matches exactly one value) or is a wildcard (matches any
// qualifier in that position).
class AttributeQualifierPattern final {
 public:
  // Wildcard — matches any qualifier value at this position.
  static AttributeQualifierPattern Wildcard();

  // Exact-match constructor.  Patterns are string-keyed, matching
  // the qualifier model (see AttributeQualifier).
  static AttributeQualifierPattern OfString(std::string value);

  // Wrap an existing qualifier as an exact-match pattern.
  explicit AttributeQualifierPattern(AttributeQualifier qualifier);

  bool IsWildcard() const {
    return !value_.has_value();
  }

  // Match against a resolved qualifier.  Wildcard matches anything;
  // concrete patterns match iff the qualifier equals the stored value.
  bool IsMatch(const AttributeQualifier& qualifier) const;

  // Short-circuit for the common "field-name step" case.
  bool IsMatch(absl::string_view key) const;

 private:
  explicit AttributeQualifierPattern(std::optional<AttributeQualifier> v)
      : value_(std::move(v)) {}
  std::optional<AttributeQualifier> value_;
};

// ————————— Attribute —————————
//
// A resolved attribute path.  `variable_name()` is the root binding
// (e.g. `"request"`); `qualifier_path()` is the ordered list of
// qualifiers (e.g. `{"auth", "claims", "iss"}` for
// `request.auth.claims.iss`).
//
// Value semantics via a shared_ptr<const Impl> so copies are cheap
// (matches cel-cpp).  An empty `variable_name()` is a sentinel for
// "malformed / unresolved".
class Attribute final {
 public:
  // Bare variable with no qualifier chain — e.g. just `x`.
  explicit Attribute(std::string variable_name);

  // Full path constructor.
  Attribute(std::string variable_name,
            std::vector<AttributeQualifier> qualifier_path);

  Attribute(const Attribute&) = default;
  Attribute(Attribute&&) = default;
  Attribute& operator=(const Attribute&) = default;
  Attribute& operator=(Attribute&&) = default;

  absl::string_view variable_name() const;
  bool has_variable_name() const;
  absl::Span<const AttributeQualifier> qualifier_path() const;

  bool operator==(const Attribute& other) const;
  bool operator!=(const Attribute& other) const {
    return !(*this == other);
  }
  bool operator<(const Attribute& other) const;

  // Canonical dotted form for diagnostics: `"request.auth.claims"`.
  // Returns InvalidArgument if any qualifier fails to stringify
  // (unprintable / quote bytes in a key).
  absl::StatusOr<std::string> AsString() const;

 private:
  struct Impl final {
    Impl(std::string variable_name,
         std::vector<AttributeQualifier> qualifier_path)
        : variable_name(std::move(variable_name)),
          qualifier_path(std::move(qualifier_path)) {}
    std::string variable_name;
    std::vector<AttributeQualifier> qualifier_path;
  };
  std::shared_ptr<const Impl> impl_;
};

// ————————— AttributePattern —————————
//
// A variable name + a list of qualifier patterns.  Matches
// `Attribute`s with three granularities:
//
//   NONE    — no match: variable differs, or a concrete qualifier
//             pattern fails to match a qualifier at the same depth.
//   PARTIAL — pattern names an entity nested strictly below the
//             attribute (pattern path is longer than the attribute
//             path, but every prefix position matches).  Useful for
//             "should I mark the parent unknown?" decisions.
//   FULL    — pattern matches the attribute itself at every
//             position; attribute path may be longer than the
//             pattern (pattern is a prefix of the attribute).
class AttributePattern final {
 public:
  enum class MatchType : uint8_t { kNone, kPartial, kFull };

  AttributePattern(std::string variable,
                   std::vector<AttributeQualifierPattern> qualifier_path);

  // Parse a dotted-path pattern like `"c.name"` or `"c.order.*"`.  `*`
  // is the wildcard segment (matches any qualifier at that position);
  // every other segment becomes a string-key exact-match qualifier.
  //
  // InvalidArgument on: empty input, leading / trailing dot, two
  // consecutive dots (empty segment).  No other validation is done on
  // the names — the checker / resolver decides what variable + field
  // names exist.
  static absl::StatusOr<AttributePattern> Parse(absl::string_view dotted);

  absl::string_view variable() const;
  absl::Span<const AttributeQualifierPattern> qualifier_path() const;

  MatchType IsMatch(const Attribute& attribute) const;

 private:
  std::string variable_;
  std::vector<AttributeQualifierPattern> qualifier_path_;
};

absl::string_view AttributePatternMatchTypeName(AttributePattern::MatchType m);

// ————————— AttributeId —————————
//
// Wire-side handle for a declared attribute.  Dense index into
// `cel.abi.attributes[]`; the host maps id → `Attribute` at
// LoadEval from the ABI and consults that map when checking
// partial-eval patterns.  Carried as the payload of
// `Value::Unknown`.
struct AttributeId {
  uint32_t id = 0;

  constexpr bool operator==(const AttributeId& other) const {
    return id == other.id;
  }
  constexpr bool operator!=(const AttributeId& other) const {
    return id != other.id;
  }
};

// Reserved attribute-id sentinel marking an unknown a host function
// *returned* (via `HostCallContext::ReturnUnknown`), as distinct from
// an unknown auto-propagated from an unknown input (which carries the
// input's real attribute ids).  Real ids are dense indices into
// `cel.abi.attributes[]` starting at 1 — row 0 is the "no attribute"
// sentinel (`abi/cel_abi.proto`), so real ids live in `[1, N]` and
// never approach `UINT32_MAX`; the function-origin sentinel sits at
// the other edge of the range.  On the wire the sentinel travels
// inside an UnknownSet descriptor like any other id (it sorts last in
// a merged set).  A consumer distinguishes the two via
// `v.IsUnknown()` plus membership of `kFunctionUnknownSentinel` in
// `*v.UnknownAttributes()` (or `v.UnknownAttribute()->id` for a
// one-element set).
inline constexpr uint32_t kFunctionUnknownSentinel = 0xFFFF'FFFFu;

}  // namespace celwasm

#endif  // CELWASM_EVAL_ATTRIBUTE_H_
