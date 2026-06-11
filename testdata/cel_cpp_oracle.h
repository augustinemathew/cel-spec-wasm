// Differential oracle: evaluate a CEL source expression through
// cel-cpp's OWN compiler + runtime (the same libraries the production
// checker reuses) and return the result in a neutral form that our
// pipeline's result can be compared against.
//
// The result/argument types are the neutral `cel.expr.Value` exchange
// proto (the same type the conformance corpus uses), so a differential
// test can hold both this oracle's result and our pipeline's result
// side by side and compare them.  (Historically this header also had to
// stay free of cel-cpp's own headers because OUR namespace used to be
// `cel` and collided with cel-cpp's `cel::Value`; that limitation is
// gone now that we live in `namespace celwasm`, so the `.cc` links
// cel-cpp freely.  The neutral exchange type stays for comparison
// ergonomics, not to dodge a collision.)
//
// Why this exists.  M20 (and differential-conformance work generally)
// needs to assert that our pipeline produces the SAME result cel-cpp
// would for a given expression — not merely the same as a pre-baked
// corpus literal.  The oracle is the cel-cpp side of that comparison.

#ifndef CELWASM_TESTDATA_CEL_CPP_ORACLE_H_
#define CELWASM_TESTDATA_CEL_CPP_ORACLE_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "cel/expr/value.pb.h"

namespace celwasm::testdata {

// The outcome of evaluating an expression through cel-cpp.  A CEL
// evaluation error is a first-class value-level outcome (langdef
// §"Error propagation"), NOT a harness failure — `cel.expr.Value` has
// no error representation, so it is surfaced via `is_error` rather than
// folded into `value`.
//
//   - `is_error == false`: `value` holds the evaluated result encoded
//     as a `cel.expr.Value`.
//   - `is_error == true`:  the expression evaluated to a CEL error;
//     `error_message` is cel-cpp's error string (informational —
//     conformance compares error KIND, not message).
//
// A non-OK `absl::Status` from `EvalWithCelCpp` is reserved for
// harness/setup failures (parse failure, type-check failure,
// descriptor/runtime build failure) — i.e. the oracle itself could not
// run, which is distinct from the expression legitimately erroring.
// Note that a CEL *eval* error is NOT a harness failure even when
// cel-cpp surfaces it as a non-OK `Evaluate()` status (some range
// overflows do): those are folded into `is_error`, matching cel-cpp's
// own conformance harness.
struct OracleResult {
  bool is_error = false;
  // cel-cpp returned an UnknownValue (partial-eval: the result depends
  // on an attribute marked unknown).  Mutually exclusive with is_error;
  // when set, `value` is unset.
  bool is_unknown = false;
  // When `is_unknown`, the dotted string form of EVERY attribute in the
  // UnknownValue's attribute set (cel-cpp `Attribute::AsString`, e.g.
  // "a.x"), in the set's own sorted order.  cel-cpp merges unknown
  // operands into one set (`AttributeUtility::MergeUnknowns`), so a
  // result that depends on several unknown attributes carries ALL of
  // their identities here — the empirical reference for what an
  // unknown result's provenance must preserve.
  std::vector<std::string> unknown_attributes;
  cel::expr::Value value;
  std::string error_message;
};

// Parse + type-check + evaluate `source` through cel-cpp under the
// given proto `container` (e.g. "cel.expr.conformance.proto2").  The
// runtime is configured to mirror cel-cpp's own conformance harness
// (`conformance/service.cc` modern impl): qualified type identifiers,
// heterogeneous equality, the reference resolver, and the registered
// proto2/proto3 conformance enums — the combination that lets
// `GlobalEnum.GAZ` resolve and proto constructors range-check.
absl::StatusOr<OracleResult> EvalWithCelCpp(absl::string_view source,
                                            absl::string_view container);

// A free variable for a partial-eval oracle query.  Declared on the
// checker as `dyn` (so `source` type-checks for any usage — the static
// type is irrelevant to the unknown-propagation semantics under test)
// and, when `value` is set, bound on the activation.  An unset `value`
// leaves the variable UNBOUND — legal under partial eval, where an
// unknown variable need not have a value.  `value` is the bound value
// as the neutral `cel.expr.Value` exchange proto (the same type
// `OracleResult::value` uses).
struct OracleVar {
  std::string name;
  std::optional<cel::expr::Value> value;
};

// Partial-eval counterpart of `EvalWithCelCpp`: declares every `vars`
// entry on the checker, binds those carrying a `value`, marks each
// dotted `unknown_patterns` entry unknown (e.g. `"xs"`, `"c.field"` —
// a bare name is a whole-variable pattern; dotted segments are string
// field qualifiers), and evaluates with cel-cpp's attribute-unknown
// processing enabled (`UnknownProcessingOptions::kAttributeOnly`).
// `OracleResult::is_unknown` is set when cel-cpp returns an
// UnknownValue.  This is the empirical reference for partial-eval
// behavior (whole-variable unknowns, comprehension-over-unknown,
// loop-variable immunity) that plain source-reading cannot settle.
absl::StatusOr<OracleResult> PartialEvalWithCelCpp(
    absl::string_view source, absl::string_view container,
    absl::Span<const OracleVar> vars,
    absl::Span<const std::string> unknown_patterns);

}  // namespace celwasm::testdata

#endif  // CELWASM_TESTDATA_CEL_CPP_ORACLE_H_
