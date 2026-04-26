// Conformance-suite harness for compiler_v2.
//
// Wraps one `cel.expr.conformance.test.SimpleTest` as a pipeline
// invocation (`cel::Compiler::Compile` → `cel::Engine::Plan` →
// `cel::Instance::Eval`) and compares the decoded `cel::Value`
// against the test's `cel.expr.Value` matcher.
//
// Every run yields exactly one of three outcomes:
//
//   - `kPass`         — compiled, evaluated, and matched the proto.
//   - `kUnsupported`  — the test sits outside the current milestone's
//                       envelope (bindings / container / type_env /
//                       aggregate matcher / error matcher) *or*
//                       compilation returned `Unimplemented`.  Not a
//                       regression; later milestones graduate these.
//   - `kFail`         — anything else.  Treated as a regression by the
//                       test wrapper; the binary form tallies them.
//
// The envelope widens each milestone.  M1 accepts scalar `value:`
// matchers only; M2 additionally accepts `unknown` / `any_unknowns`
// matchers and routes them to `Instance::PartialEval`.  M3 admits
// `map_value:` matchers — the `CompareValue` map arm decodes the
// proto map entries and matches them order-agnostically against
// the cel::Value's `HostMapBacking`.  M4 admits `list_value:`
// matchers — `CompareList` walks the decoded `cel::Value`'s
// `HostListBacking` ORDER-aware (lists are ordered per langdef
// § "List equality", unlike maps).  M4 additionally admits
// `eval_error:` / `any_eval_errors:` matchers — `CompareEvalError`
// asserts the runtime returned a `Value::Error`; the matcher's
// per-error `message` strings are checked loosely (substring,
// either direction) against the runtime payload, with empty
// `errors[]` matching any error (mirrors cel-cpp's
// `conformance/run.cc` which checks `has_error()` only).
// Subsequent milestones loosen further dimensions (typed_result,
// comprehensions) and update the filter here in the same commit.

#ifndef CELWASM_COMPILER_V2_CONFORMANCE_RUNNER_H_
#define CELWASM_COMPILER_V2_CONFORMANCE_RUNNER_H_

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/eval.pb.h"
#include "cel/expr/value.pb.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/value.h"

namespace celwasm::conformance {

enum class Outcome : std::uint8_t {
  kPass,
  kUnsupported,
  kFail,
};

struct Result {
  Outcome outcome = Outcome::kUnsupported;
  // Human-readable reason.  For kPass, empty.  For kUnsupported, the
  // envelope dimension or `Unimplemented` message.  For kFail, the
  // underlying status + a short diff of got-vs-want.
  std::string detail;
};

absl::string_view OutcomeName(Outcome o);

// Returns true iff the test shape is one the M3 pipeline can even
// attempt: `check_only` and `disable_check` both unset, and the
// matcher is one of
//
//   - `value:` with a scalar kind
//     (null/bool/int64/uint64/double/string/bytes), or
//   - `value:` with `map_value` (M3 — compared order-agnostically
//     against the decoded `cel::Value`'s `HostMapBacking`), or
//   - `unknown:` / `any_unknowns:` — routed to `PartialEval`.
//
// `bindings:` / `type_env:` / `container:` are NOT pre-filtered:
// the harness-side marshaller (`binding_marshal.h`) attempts to
// decode each entry into the public `cel::` surface and gracefully
// returns `Unimplemented` (caller SKIPs) on aggregate / non-scalar
// shapes.  This lets a single fixture file mix M3-eligible scalar
// bindings with M6/M7 aggregate bindings and have only the
// in-envelope tests graduate.
//
// A false here short-circuits to `kUnsupported` without compiling.
bool IsInM4Envelope(const cel::expr::conformance::test::SimpleTest& t);

// Compare a decoded `cel::Value` against the proto `cel.expr.Value`.
// OK on equality, `FailedPrecondition` with a diff-ish payload on
// mismatch, `InvalidArgument` if `want` is a kind the runner has no
// comparison for (list_value / object_value / enum_value /
// type_value — caller should have short-circuited via
// `IsInM4Envelope` first).
absl::Status CompareValue(const cel::Value& got, const cel::expr::Value& want);

// Compare a `cel::Value` against an `UnknownSet` matcher.  OK iff
// `got.IsUnknown()` — the matcher's `exprs` carry AST expression IDs
// that our runtime-interned `AttributeId` can't be diffed against
// without a per-run expr-id → attribute-id map, which the harness
// doesn't plumb.  Upgrading to id-level equality is a future-work
// item in `conformance/README.md`; for now the kind-level match is
// enough to lock the PartialEval route end-to-end.
absl::Status CompareUnknown(const cel::Value& got);

// Compare a `cel::Value` against an `eval_error` / `any_eval_errors`
// matcher.  OK iff `got.IsError()` AND the matcher loosely matches
// the runtime's `ErrorPayload::message`.  Matching rule (mirrors
// cel-cpp's `conformance/run.cc`, which only checks `has_error()`):
//
//   - If `want.errors_size() == 0` → any error matches.
//   - Otherwise → at least one of `want.errors[i].message` matches
//     the runtime message via substring-either-direction (the
//     fixture-author-supplied messages and our `ErrorCodeName(...)`
//     payloads use different phrasings — e.g. `"divide by zero"` vs
//     `"divide_by_zero"`, `"return error for overflow"` vs
//     `"overflow"` — so strict equality would reject every row).
//
// Returns `FailedPrecondition` with a kind/message diff on mismatch.
absl::Status CompareEvalError(const cel::Value& got,
                              const cel::expr::ErrorSet& want);

// Run one test end-to-end using the shared compiler + engine
// fixtures.  Never throws; always returns a `Result`.
Result RunOne(const cel::expr::conformance::test::SimpleTest& t,
              const cel::Compiler& compiler, const cel::Engine& engine);

// Load a `SimpleTestFile` from a workspace-relative textproto path
// (runfiles lookup handled by the caller — we just `ifstream` it).
// Returns `NotFound` / `InvalidArgument` on I/O or parse failure.
absl::Status LoadTestFile(absl::string_view path,
                          cel::expr::conformance::test::SimpleTestFile& out);

}  // namespace celwasm::conformance

#endif  // CELWASM_COMPILER_V2_CONFORMANCE_RUNNER_H_
