// Differential oracle: evaluate a CEL source expression through
// cel-cpp's OWN compiler + runtime (the same libraries the production
// checker reuses) and return the result in a neutral form that our
// pipeline's result can be compared against.
//
// Why a separate TU.  Our public API type `cel::Value` is
// `celwasm::api::Value` aliased into `namespace cel` (`api/value.h`);
// cel-cpp's `cel::Value` (`common/value.h`) collides.  Anything that
// links cel-cpp's runtime therefore cannot also include our
// `api/value.h`.  This header exposes ONLY neutral types
// (`cel.expr.Value` proto, absl), so a differential test can include
// both this oracle and our pipeline headers without the collision.
//
// Why this exists.  M20 (and differential-conformance work generally)
// needs to assert that our pipeline produces the SAME result cel-cpp
// would for a given expression — not merely the same as a pre-baked
// corpus literal.  The oracle is the cel-cpp side of that comparison.

#ifndef CELWASM_COMPILER_V2_TESTDATA_CEL_CPP_ORACLE_H_
#define CELWASM_COMPILER_V2_TESTDATA_CEL_CPP_ORACLE_H_

#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
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

}  // namespace celwasm::testdata

#endif  // CELWASM_COMPILER_V2_TESTDATA_CEL_CPP_ORACLE_H_
