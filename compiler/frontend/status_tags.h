#ifndef CELWASM_COMPILER_V2_FRONTEND_STATUS_TAGS_H_
#define CELWASM_COMPILER_V2_FRONTEND_STATUS_TAGS_H_

#include "absl/strings/string_view.h"

namespace celwasm {

// `absl::Status::SetPayload` type URLs used by `ParseAndCheck` to mark
// the failure category of an `InvalidArgument` status without forcing
// the caller to substring-match the human-readable message.
//
// Producers (`parse_and_check.cc`) attach these payloads alongside the
// usual message text.  Consumers (notably the conformance harness)
// call `status.GetPayload(url)` to classify and route the failure.
//
// Payload bodies:
//   - `kStaticSubsetViolationUrl` — set by `RejectDyn` when the
//     checked AST contains DYN / ERROR / type-param / function /
//     unset nodes.  Body is the offending expr-id list as a
//     comma-separated string (e.g. `"3,17"`).
//   - `kUndeclaredReferencesUrl` — set by `RunTypeCheck` when the
//     `ValidationResult` issues include one or more
//     `"undeclared reference to '<sym>'"` errors.  Body is the
//     deduplicated, newline-separated list of bare symbol names
//     (e.g. `"cel\nmath"`); the harness compares those names
//     against its ext-lib roots list to decide
//     SKIP-as-ext-unimpl vs FAIL.

inline constexpr absl::string_view kStaticSubsetViolationUrl =
    "celwasm/static-subset-violation";
inline constexpr absl::string_view kUndeclaredReferencesUrl =
    "celwasm/undeclared-references";

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_FRONTEND_STATUS_TAGS_H_
