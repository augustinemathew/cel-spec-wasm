// Conformance harness for celwasm.
//
// Wraps a single upstream `cel.expr.conformance.test.SimpleTest` row
// as a celwasm pipeline invocation (`celwasm::api::Compiler::Compile` →
// `celwasm::api::Engine::Plan` → `celwasm::api::Instance::Eval`) and compares the
// decoded `celwasm::api::Value` against the test's `cel.expr.Value` matcher.
//
// Every row resolves to exactly one of three outcomes:
//
//   - `kPass`         — compiled, evaluated, and matched the matcher.
//   - `kUnsupported`  — outside the harness's current envelope.  Each
//                       such row carries a `SkipCategory` tag so the
//                       caller can aggregate-by-category without
//                       parsing detail text.  Not a regression.
//   - `kFail`         — anything else (compile/plan/eval error not
//                       recognised as out-of-envelope, or a value
//                       mismatch).  Treated as a regression.
//
// The envelope today admits every matcher kind defined on
// `cel.expr.Value` plus the `eval_error` / `any_eval_errors` /
// `unknown` / `any_unknowns` / `typed_result` matchers.  The
// per-stage marshal helpers in `binding_marshal.h` SKIP gracefully
// for `bindings:` / `type_env:` entries whose shape the harness
// doesn't yet support (aggregate types, error/unknown bindings).

#ifndef CELWASM_COMPILER_V2_CONFORMANCE_RUNNER_H_
#define CELWASM_COMPILER_V2_CONFORMANCE_RUNNER_H_

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/eval.pb.h"
#include "cel/expr/value.pb.h"
#include "eval/engine.h"
#include "eval/value.h"

namespace celwasm::conformance {

enum class Outcome : std::uint8_t {
  kPass,
  kUnsupported,
  kFail,
};

// Stable category tags for `kUnsupported` outcomes.  The textual
// names (see `SkipCategoryName`) are part of the harness contract:
// the conformance README's per-fixture SKIP-by-category table groups
// by these names, and operators grep against them.  Add a category
// here AND register the name in `SkipCategoryName` in the same
// commit.
//
// The mapping from "what went wrong" to category:
//
//   kDisableCheck       — row carries `disable_check: true` (parse-
//                         only eval; out-of-conformance-scope by
//                         design — our pipeline is checker-passed
//                         only).
//   kCheckOnly          — row carries `check_only: true`.  Eval is
//                         skipped; `typed_result.deduced_type`-only
//                         comparison is harness follow-up.
//   kEnvelope           — matcher kind is not one the harness can
//                         compare today (no `value:` set, or a
//                         typed_result row with no inner `result`).
//   kStaticSubset       — `RejectDyn` rejected the expression.
//                         Identified by status-payload tag
//                         `kStaticSubsetViolationUrl`.
//   kCompileUnimpl      — Compile returned `Unimplemented` (a stage
//                         stub for a later milestone).  Detail
//                         carries the stage label.
//   kEvalUnimpl         — Eval or PartialEval returned
//                         `Unimplemented`.  Detail carries the
//                         stage label.
//   kExtensionUnimpl    — Type-check failed because a cel-cpp
//                         extension library was not registered
//                         (`undeclared reference to '<ext-root>'`).
//                         Identified by the
//                         `kUndeclaredReferencesUrl` payload tag
//                         plus an ext-lib roots match.
//   kTypeEnvUnsupported — binding_marshal refused a `type_env`
//                         decl (aggregate / function / dyn type).
//   kBindingUnsupported — binding_marshal refused a `bindings`
//                         entry (aggregate value, error / unknown
//                         ExprValue shapes).
enum class SkipCategory : std::uint8_t {
  kDisableCheck,
  kCheckOnly,
  kEnvelope,
  kStaticSubset,
  kCompileUnimpl,
  kEvalUnimpl,
  kExtensionUnimpl,
  kTypeEnvUnsupported,
  kBindingUnsupported,
};

absl::string_view OutcomeName(Outcome o);
absl::string_view SkipCategoryName(SkipCategory c);

struct Result {
  Outcome outcome = Outcome::kUnsupported;
  // Tag for `kUnsupported` outcomes.  Ignored for `kPass` / `kFail`.
  SkipCategory category = SkipCategory::kEnvelope;
  // Human-readable detail.  For `kPass`, empty.  For `kUnsupported`,
  // describes the specific SKIP cause (the matcher kind, the
  // unimplemented stage, etc).  For `kFail`, the stage + absl::Status
  // string from where the failure was caught.
  std::string detail;
};

// Returns true iff the test's matcher kind is one the harness can
// compare.  Pre-screen check used in `RunOne`; out-of-envelope rows
// SKIP as `kEnvelope` without burning a compile.  `disable_check` /
// `check_only` are NOT pre-screened by this predicate — they are
// checked separately in `RunOne` and route to their own categories.
bool IsInEnvelope(const cel::expr::conformance::test::SimpleTest& t);

// Compare a decoded `celwasm::api::Value` against a proto matcher.  Returns
// `OkStatus` on equality, `FailedPrecondition` with a kind/value
// diff on mismatch, `InvalidArgument` if `want`'s kind has no
// comparator (caller should pre-screen via `IsInEnvelope`).
absl::Status CompareValue(const celwasm::api::Value& got,
                          const cel::expr::Value& want);

// Compare a `celwasm::api::Value` against an unknown-set matcher.  OK iff
// `got.IsUnknown()` — the matcher's `exprs` field carries AST IDs
// the harness can't currently round-trip through `AttributeId`
// (future work — see README).
absl::Status CompareUnknown(const celwasm::api::Value& got);

// Compare a `celwasm::api::Value` against an `eval_error` / `any_eval_errors`
// matcher.  CEL errors are values (langdef §"Error propagation");
// the matcher passes iff `got.IsError()`.  Per cel-cpp's upstream
// `conformance/run.cc` semantics, message-level matching is NOT
// part of conformance — any error matches any non-empty matcher at
// the kind level.  Kind mismatch is the only failure path.
absl::Status CompareEvalError(const celwasm::api::Value& got,
                              const cel::expr::ErrorSet& want);

// Run one test end-to-end.  Never throws; always returns a `Result`.
// `engine` is shared across rows; the compiler is built per-row
// when the row declares variables and is otherwise served from a
// process-wide shared default.
Result RunOne(const cel::expr::conformance::test::SimpleTest& t,
              const celwasm::api::Engine& engine);

// Load a `SimpleTestFile` from a workspace-relative textproto path
// (runfiles lookup handled by the caller — we just `ifstream` it).
// Returns `NotFound` / `InvalidArgument` on I/O or parse failure.
absl::Status LoadTestFile(absl::string_view path,
                          cel::expr::conformance::test::SimpleTestFile& out);

}  // namespace celwasm::conformance

#endif  // CELWASM_COMPILER_V2_CONFORMANCE_RUNNER_H_
