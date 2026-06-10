// Conformance runner — drives one `SimpleTest` row through the
// celwasm pipeline (Compile → Plan → Eval) and classifies the
// outcome as PASS / SKIP / FAIL.  SKIP rows always carry a
// `SkipCategory` tag (declared in runner.h) so the aggregator can
// group counts by category without parsing detail text.
//
// Classification rules (see `runner.h::SkipCategory` for the
// authoritative description of each tag):
//
//   - Out-of-scope-by-design flags (`disable_check`, `check_only`)
//     are checked first and skip directly with their dedicated
//     categories.
//   - Matcher-kind envelope is checked next; rows whose matcher we
//     have no comparator for skip as `kEnvelope`.
//   - Compile failures are classified by `Status::GetPayload(...)`
//     tags set by `parse_and_check.cc`:
//       · `kStaticSubsetViolationUrl`  → `kStaticSubset`.
//       · `kUndeclaredReferencesUrl`   → `kExtensionUnimpl` iff
//         every undeclared root is in the ext-lib roots list
//         (`ExtensionRoots()` below); FAIL otherwise.
//     `Unimplemented` codes from Compile route to `kCompileUnimpl`.
//   - Eval / PartialEval `Unimplemented` codes route to
//     `kEvalUnimpl`.
//   - The marshal helpers in `binding_marshal.h` return
//     `Unimplemented` for `bindings:` / `type_env:` shapes the
//     harness doesn't support; those route to `kBindingUnsupported`
//     / `kTypeEnvUnsupported` respectively.
//
// Adding a new SKIP category: extend `SkipCategory` + `SkipCategoryName`
// in runner.h, then route to it from `RunOne` (or a helper).

#include "conformance/runner.h"

#include "absl/flags/flag.h"

// m28 — dual-mode conformance: same corpus, two link modes.  The
// flag default is `"dynamic"` (no behavioural change for the
// existing pre-push gate).  Set to `"static"` to exercise the
// merged-runtime path:
//
//   bazel run //conformance:run_conformance -- --link_mode=static
//
// `scripts/check_conformance_monotonic.sh` runs the binary twice —
// once per mode — and gates each mode's pass count against its own
// baseline file (`conformance/.baseline` for dynamic,
// `conformance/.baseline_static` for static).  See
// `doc/implementation-plan/rewrite/m28-configurable-linking.md` §7.3.
//
// Same suppressions as run_conformance.cc / celwasmc_eval_main.cc:
//   - misc-use-internal-linkage: ABSL_FLAG generates extern helpers.
//   - bugprone-throwing-static-initialization: std::string flag default.
// NOLINTBEGIN(misc-use-internal-linkage,bugprone-throwing-static-initialization)
ABSL_FLAG(
    std::string, link_mode, "dynamic",
    "Compiler link mode for the conformance corpus: "
    "\"dynamic\" (today's behaviour) or \"static\" (m28 merged-runtime).");
// NOLINTEND(misc-use-internal-linkage,bugprone-throwing-static-initialization)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
// `test_all_types.pb.h` is included for the side-effect of
// registering `cel.expr.conformance.proto{2,3}.TestAllTypes` (and
// the proto2 extension-scoped descriptor) into the generated pool.
// Several fixtures embed `google.protobuf.Any` values of these
// types; without registration, `TextFormat::Parse` fails at load
// time with `"Could not find type"`.  `ForceLinkFixtureDescriptors`
// referencing the generated `::descriptor()` accessors keeps the
// linker from dead-stripping the .pb.o.
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto2/test_all_types_extensions.pb.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/value.pb.h"
#include "compiler/compiler.h"
#include "compiler/frontend/status_tags.h"
#include "compiler/program.h"
#include "conformance/binding_marshal.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/internal/cel_host.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/util/message_differencer.h"

namespace celwasm::conformance {

namespace {

void ForceLinkFixtureDescriptors() {
  [[maybe_unused]] const auto* p2 =
      ::cel::expr::conformance::proto2::TestAllTypes::descriptor();
  [[maybe_unused]] const auto* p3 =
      ::cel::expr::conformance::proto3::TestAllTypes::descriptor();
  [[maybe_unused]] const auto* p2x = ::cel::expr::conformance::proto2::
      Proto2ExtensionScopedMessage::descriptor();
}

using ::cel::expr::conformance::test::SimpleTest;
using ::cel::expr::conformance::test::SimpleTestFile;
using ProtoValue = ::cel::expr::Value;

// Matchers the harness can compare today — every `cel.expr.Value`
// kind plus the unknown / eval_error / typed_result aggregates.
// Update both this predicate and `EnvelopeRejectReason` in the same
// commit when widening.
bool IsScalarMatcherKind(ProtoValue::KindCase k) {
  switch (k) {
    case ProtoValue::kNullValue:
    case ProtoValue::kBoolValue:
    case ProtoValue::kInt64Value:
    case ProtoValue::kUint64Value:
    case ProtoValue::kDoubleValue:
    case ProtoValue::kStringValue:
    case ProtoValue::kBytesValue:
      return true;
    default:
      return false;
  }
}

bool IsAggregateOrObjectMatcherKind(ProtoValue::KindCase k) {
  return k == ProtoValue::kMapValue || k == ProtoValue::kListValue ||
         k == ProtoValue::kObjectValue || k == ProtoValue::kEnumValue ||
         k == ProtoValue::kTypeValue;
}

bool IsUnknownMatcher(const SimpleTest& t) {
  return t.result_matcher_case() == SimpleTest::kUnknown ||
         t.result_matcher_case() == SimpleTest::kAnyUnknowns;
}

bool IsEvalErrorMatcher(const SimpleTest& t) {
  return t.result_matcher_case() == SimpleTest::kEvalError ||
         t.result_matcher_case() == SimpleTest::kAnyEvalErrors;
}

// Kind-only diff payload — compare-site callers already know which
// test they ran, and the detailed value isn't load-bearing for
// debugging (the fail list shows the expression source).  Keeping
// the switch-heavy per-kind formatters out of the TU also sidesteps
// clang-tidy's `bugprone-branch-clone` on structurally-similar
// `return absl::StrCat(...)` arms.
absl::Status Mismatch(const celwasm::Value& got, const ProtoValue& want) {
  return absl::FailedPreconditionError(
      absl::StrCat("want-kind=", static_cast<int>(want.kind_case()),
                   " got-kind=", ValueKindName(got.kind())));
}

absl::Status CompareDouble(double got, double want) {
  // CEL per langdef: NaN matches any NaN.
  if (std::isnan(got) && std::isnan(want)) return absl::OkStatus();
  if (got == want) return absl::OkStatus();
  return absl::FailedPreconditionError(
      absl::StrCat("double want=", want, " got=", got));
}

// Forward decls — these recurse with CompareValue.
absl::Status CompareMap(const celwasm::Value& got,
                        const cel::expr::MapValue& want);
absl::Status CompareList(const celwasm::Value& got,
                         const cel::expr::ListValue& want);

}  // namespace

// External-linkage API funcs (declared in runner.h) — clang-tidy's
// `misc-use-internal-linkage` can't see callers across TUs and
// misclassifies these as could-be-static.  Mirror the NOLINT pattern
// used elsewhere in the tree.
// NOLINTBEGIN(misc-use-internal-linkage)
absl::string_view OutcomeName(Outcome o) {
  switch (o) {
    case Outcome::kPass:
      return "PASS";
    case Outcome::kUnsupported:
      return "SKIP";
    case Outcome::kFail:
      return "FAIL";
  }
  ABSL_CHECK(false) << "unhandled Outcome";
  return "?";
}

absl::string_view SkipCategoryName(SkipCategory c) {
  switch (c) {
    case SkipCategory::kDisableCheck:
      return "disable_check";
    case SkipCategory::kCheckOnly:
      return "check_only";
    case SkipCategory::kEnvelope:
      return "envelope";
    case SkipCategory::kStaticSubset:
      return "static_subset";
    case SkipCategory::kCompileUnimpl:
      return "compile_unimpl";
    case SkipCategory::kEvalUnimpl:
      return "eval_unimpl";
    case SkipCategory::kExtensionUnimpl:
      return "ext_unimpl";
    case SkipCategory::kTypeEnvUnsupported:
      return "type_env";
    case SkipCategory::kBindingUnsupported:
      return "bindings";
  }
  ABSL_CHECK(false) << "unhandled SkipCategory";
  return "?";
}

bool IsInEnvelope(const SimpleTest& t) {
  // Snapshot the oneof case once — repeated calls to
  // result_matcher_case() trigger clang-tidy's path-sensitive
  // EnumCastOutOfRange analyzer (each compare narrows the
  // believed-possible enum set, and after two narrowings the
  // analyzer concludes no documented value remains).
  const auto matcher_case = t.result_matcher_case();
  if (matcher_case == SimpleTest::kUnknown ||
      matcher_case == SimpleTest::kAnyUnknowns) {
    return true;
  }
  if (matcher_case == SimpleTest::kEvalError ||
      matcher_case == SimpleTest::kAnyEvalErrors) {
    return true;
  }
  if (matcher_case == SimpleTest::kTypedResult) return true;
  // Implicit bool-true shortcut: a test with no result_matcher set
  // is a bool-asserting expression (e.g. `'tacocat'.charAt(3) == 'o'`)
  // expected to evaluate to true.  Matches cel-cpp's upstream
  // convention in `conformance/run.cc`.
  if (matcher_case == SimpleTest::RESULT_MATCHER_NOT_SET) return true;
  if (matcher_case != SimpleTest::kValue) return false;
  const auto k = t.value().kind_case();
  return IsScalarMatcherKind(k) || IsAggregateOrObjectMatcherKind(k);
}
// NOLINTEND(misc-use-internal-linkage)

namespace {

std::string ValueMatcherKindName(ProtoValue::KindCase k) {
  switch (k) {
    case ProtoValue::kNullValue:
      return "null_value";
    case ProtoValue::kBoolValue:
      return "bool_value";
    case ProtoValue::kInt64Value:
      return "int64_value";
    case ProtoValue::kUint64Value:
      return "uint64_value";
    case ProtoValue::kDoubleValue:
      return "double_value";
    case ProtoValue::kStringValue:
      return "string_value";
    case ProtoValue::kBytesValue:
      return "bytes_value";
    case ProtoValue::kEnumValue:
      return "enum_value";
    case ProtoValue::kObjectValue:
      return "object_value";
    case ProtoValue::kMapValue:
      return "map_value";
    case ProtoValue::kListValue:
      return "list_value";
    case ProtoValue::kTypeValue:
      return "type_value";
    case ProtoValue::KIND_NOT_SET:
      return "<unset>";
  }
  return "<unknown>";
}

// The only reachable callers pass `kind` precomputed before the
// IsInEnvelope check, so this function never re-queries the
// protobuf oneof case.  See callsite for the path-sensitive-analyzer
// rationale.
std::string EnvelopeRejectReason(const cel::expr::Value::KindCase kind) {
  return absl::StrCat("value matcher kind `", ValueMatcherKindName(kind),
                      "` not in scope");
}

}  // namespace

// Scalar / aggregate comparators ---------------------------------

namespace {

absl::Status CompareScalar(const celwasm::Value& got, const ProtoValue& want) {
  switch (want.kind_case()) {
    case ProtoValue::kNullValue:
      return got.IsNull() ? absl::OkStatus() : Mismatch(got, want);
    case ProtoValue::kBoolValue: {
      auto b = got.AsBool();
      if (!b.ok() || *b != want.bool_value()) return Mismatch(got, want);
      return absl::OkStatus();
    }
    case ProtoValue::kInt64Value: {
      auto i = got.AsInt();
      if (!i.ok() || *i != want.int64_value()) return Mismatch(got, want);
      return absl::OkStatus();
    }
    case ProtoValue::kUint64Value: {
      auto u = got.AsUint();
      if (!u.ok() || *u != want.uint64_value()) return Mismatch(got, want);
      return absl::OkStatus();
    }
    case ProtoValue::kDoubleValue: {
      auto d = got.AsDouble();
      if (!d.ok()) return Mismatch(got, want);
      return CompareDouble(*d, want.double_value());
    }
    case ProtoValue::kStringValue: {
      auto s = got.AsString();
      if (!s.ok() || *s != want.string_value()) return Mismatch(got, want);
      return absl::OkStatus();
    }
    case ProtoValue::kBytesValue: {
      auto b = got.AsBytes();
      if (!b.ok() || *b != want.bytes_value()) return Mismatch(got, want);
      return absl::OkStatus();
    }
    default:
      return absl::InvalidArgumentError(
          "CompareScalar called with non-scalar matcher kind");
  }
}

}  // namespace

// `object_value` matcher: an Any wrapping the expected proto.
// Unpacks via `binding_marshal::UnpackAny` (generated pool +
// `ForceLinkFixtureDescriptors`) and runs `MessageDifferencer::Equals`.
// NOLINTBEGIN(misc-use-internal-linkage)
absl::Status CompareMessage(const celwasm::Value& got,
                            const google::protobuf::Any& want) {
  auto got_backing_or = got.MessageBacking();
  if (!got_backing_or.ok()) {
    return absl::FailedPreconditionError(
        absl::StrCat("want-kind=message got-kind=", ValueKindName(got.kind())));
  }
  const google::protobuf::Message* got_msg = (*got_backing_or)->message();
  if (got_msg == nullptr) {
    return absl::FailedPreconditionError(
        "want-kind=message got non-proto backing");
  }
  auto want_or = celwasm::conformance::UnpackAny(want);
  if (!want_or.ok()) return want_or.status();
  // MessageDifferencer::Equals CHECK-fails on cross-descriptor
  // compare; pre-screen so a mismatch surfaces as a clean
  // FailedPrecondition instead of aborting the whole run.
  if (got_msg->GetDescriptor() != (*want_or)->GetDescriptor()) {
    return absl::FailedPreconditionError(
        absl::StrCat("message type mismatch: want=",
                     (*want_or)->GetDescriptor()->full_name(),
                     " got=", got_msg->GetDescriptor()->full_name()));
  }
  if (!google::protobuf::util::MessageDifferencer::Equals(*got_msg,
                                                          **want_or)) {
    return absl::FailedPreconditionError(
        absl::StrCat("message mismatch: want=", (*want_or)->ShortDebugString(),
                     " got=", got_msg->ShortDebugString()));
  }
  return absl::OkStatus();
}

// Enum matcher: enums are spec-typed as int (langdef §"Enumerated
// Types").  Compare against the numeric value; ignore the type field.
absl::Status CompareEnum(const celwasm::Value& got,
                         const cel::expr::EnumValue& want) {
  auto i = got.AsInt();
  if (!i.ok() || *i != want.value()) {
    return absl::FailedPreconditionError(
        absl::StrCat("enum mismatch: want=", want.value(),
                     " got-kind=", ValueKindName(got.kind())));
  }
  return absl::OkStatus();
}

// Type matcher: byte-equal on the spec type-name string per langdef
// §"Type Values".
absl::Status CompareType(const celwasm::Value& got, absl::string_view want) {
  auto name_or = got.AsType();
  if (!name_or.ok()) {
    return absl::FailedPreconditionError(
        absl::StrCat("want-kind=type got-kind=", ValueKindName(got.kind())));
  }
  if (*name_or != want) {
    return absl::FailedPreconditionError(
        absl::StrCat("type mismatch: want=`", want, "` got=`", *name_or, "`"));
  }
  return absl::OkStatus();
}

absl::Status CompareValue(const celwasm::Value& got, const ProtoValue& want) {
  switch (want.kind_case()) {
    case ProtoValue::kMapValue:
      return CompareMap(got, want.map_value());
    case ProtoValue::kListValue:
      return CompareList(got, want.list_value());
    case ProtoValue::kObjectValue:
      return CompareMessage(got, want.object_value());
    case ProtoValue::kEnumValue:
      return CompareEnum(got, want.enum_value());
    case ProtoValue::kTypeValue:
      return CompareType(got, want.type_value());
    default:
      return CompareScalar(got, want);
  }
}

absl::Status CompareUnknown(const celwasm::Value& got) {
  if (got.IsUnknown()) return absl::OkStatus();
  return absl::FailedPreconditionError(
      absl::StrCat("want-kind=unknown got-kind=", ValueKindName(got.kind())));
}

// CEL errors are values (langdef §"Error propagation"): the runtime
// returns ok with `Value::Error` rather than a host-side
// absl::Status failure.  Per cel-cpp's upstream `conformance/run.cc`
// — which only checks `has_error()` — message-level matching is
// NOT part of conformance.  Any error matches any matcher at the
// kind level; the per-error `message` strings in the corpus disagree
// with our runtime payloads along too many axes (phrasing,
// punctuation, even non-semantic placeholders like "foo") for any
// stricter rule to be useful.  Kind mismatch is the only failure
// path.
absl::Status CompareEvalError(const celwasm::Value& got,
                              const cel::expr::ErrorSet& /*want*/) {
  if (!got.IsError()) {
    return absl::FailedPreconditionError(
        absl::StrCat("want-kind=error got-kind=", ValueKindName(got.kind())));
  }
  return absl::OkStatus();
}
// NOLINTEND(misc-use-internal-linkage)

namespace {

// Order-agnostic map equality per langdef §"Map equality": same
// size, and every want-entry has a got-entry whose key is
// structurally equal and whose value matches recursively.
absl::Status CompareMap(const celwasm::Value& got,
                        const cel::expr::MapValue& want) {
  auto bk_or = got.MapBacking();
  if (!bk_or.ok()) {
    return absl::FailedPreconditionError(
        absl::StrCat("want-kind=map got-kind=", ValueKindName(got.kind())));
  }
  const auto* backing = *bk_or;
  const auto want_size = static_cast<std::size_t>(want.entries_size());
  if (backing->Size() != want_size) {
    return absl::FailedPreconditionError(
        absl::StrCat("map size want=", want_size, " got=", backing->Size()));
  }

  std::vector<std::pair<celwasm::Value, celwasm::Value>> got_entries;
  got_entries.reserve(backing->Size());
  backing->ForEach([&](const celwasm::Value& k, const celwasm::Value& v) {
    got_entries.emplace_back(k, v);
  });

  for (const auto& want_entry : want.entries()) {
    auto want_key_or = ValueFromProto(want_entry.key());
    if (!want_key_or.ok()) {
      return absl::FailedPreconditionError(absl::StrCat(
          "undecodable map key: ", want_key_or.status().message()));
    }
    bool found = false;
    for (const auto& [gk, gv] : got_entries) {
      if (!gk.StructurallyEquals(*want_key_or)) continue;
      if (auto s = CompareValue(gv, want_entry.value()); !s.ok()) return s;
      found = true;
      break;
    }
    if (!found) {
      return absl::FailedPreconditionError("map key missing in got");
    }
  }
  return absl::OkStatus();
}

// Order-aware list equality per langdef §"List equality".
absl::Status CompareList(const celwasm::Value& got,
                         const cel::expr::ListValue& want) {
  auto bk_or = got.ListBacking();
  if (!bk_or.ok()) {
    return absl::FailedPreconditionError(
        absl::StrCat("want-kind=list got-kind=", ValueKindName(got.kind())));
  }
  const auto* backing = *bk_or;
  const auto want_size = static_cast<std::size_t>(want.values_size());
  if (backing->Size() != want_size) {
    return absl::FailedPreconditionError(
        absl::StrCat("list size want=", want_size, " got=", backing->Size()));
  }

  std::vector<celwasm::Value> got_elems;
  got_elems.reserve(backing->Size());
  backing->ForEach([&](const celwasm::Value& v) {
    got_elems.push_back(v);
  });

  for (std::size_t i = 0; i < want_size; ++i) {
    if (auto s = CompareValue(got_elems[i], want.values(static_cast<int>(i)));
        !s.ok()) {
      return absl::FailedPreconditionError(
          absl::StrCat("list[", i, "]: ", s.message()));
    }
  }
  return absl::OkStatus();
}

// Result helpers --------------------------------------------------

Result Skip(SkipCategory c, absl::string_view detail) {
  return {Outcome::kUnsupported, c, std::string(detail)};
}

Result Fail(absl::string_view stage, const absl::Status& s) {
  Result r{Outcome::kFail, SkipCategory::kEnvelope, ""};
  r.detail = absl::StrCat(stage, ": ", s.ToString());
  return r;
}

// Ext-lib roots ---------------------------------------------------

// CEL extension namespaces shipped by cel-cpp libraries we don't
// register.  A type-check failure whose `kUndeclaredReferencesUrl`
// payload lists ANY symbol from this set is an ext-lib gap
// (SKIP as `kExtensionUnimpl`), not a regression.  Sources:
//
//   bindings_ext / block_ext  — `cel`, `block`
//   optionals                 — `optional`, `optional_type`
//   math_ext                  — `math`
//   string_ext                — `strings`
//   network_ext               — `ip`, `cidr`, `net`
//   encoders_ext              — `base64`
//
// Trailing method/function names within an ext namespace
// (`math.greatest` → roots `math`, `greatest`) needn't be listed —
// the namespace root alone is enough to identify the call site as
// living inside an ext library we don't register.
const absl::flat_hash_set<absl::string_view>& ExtensionNamespaceRoots() {
  static const absl::NoDestructor<absl::flat_hash_set<absl::string_view>> kSet{
      {"cel", "block", "optional", "optional_type", "math", "strings", "base64",
       "ip", "cidr", "net",
       // proto_ext: `proto.hasExt(msg, ext)` / `proto.getExt(msg, ext)`
       // (proto2 extension accessor functions).  The operator-form
       // `has(msg.`fqn.ext_name`)` is checker-accepted today and
       // reaches the runtime — that surface is a separate slice.
       "proto"}};
  return *kSet;
}

// Bare receiver-style function names shipped by extension libraries
// that have NO namespace prefix.  string_ext's receiver methods
// (`'foo'.charAt(0)` → root `charAt`) are the canonical case;
// network_ext also has a few receiver-only forms.  Used as a
// fallback when no namespace root is present in the payload — if
// EVERY root in the payload is one of these names, the row is an
// ext-lib gap.
//
// A row whose payload mixes receivers with non-extension symbols
// stays FAIL — we don't hide regressions behind an ext-lib SKIP.
const absl::flat_hash_set<absl::string_view>& ExtensionReceiverRoots() {
  static const absl::NoDestructor<absl::flat_hash_set<absl::string_view>> kSet{
      {// string_ext receivers
       "charAt", "indexOf", "lastIndexOf", "substring", "replace", "split",
       "join", "lowerAscii", "upperAscii", "format", "reverse", "quote", "trim",
       // network_ext receivers / predicates
       "isIP", "isCIDR", "family", "isCanonical", "isUnspecified", "isLoopback",
       "isGlobalUnicast", "isLinkLocalMulticast", "isLinkLocalUnicast",
       "prefixLength", "containsIP", "containsCIDR", "masked"}};
  return *kSet;
}

// `payload` is the `kUndeclaredReferencesUrl` body — a newline-
// separated list of undeclared root symbols.  Returns true iff the
// row is an ext-lib gap by either rule:
//
//   1. ANY namespace root from `ExtensionNamespaceRoots` appears,
//      OR
//   2. EVERY listed root is in `ExtensionReceiverRoots` (the
//      bare-receiver fallback for string_ext / network_ext
//      receiver-style calls that have no namespace prefix).
//
// The first rule fires for the bulk of ext-lib FAILs;
// `math.greatest` lists `math` (matches) and `greatest` (which is
// outside both sets, but rule 1 already passed).  The second rule
// catches receiver-only calls like `'foo'.charAt(0)` where there's
// no namespace in the call shape.
bool IsExtensionFailure(absl::string_view payload) {
  if (payload.empty()) return false;
  const std::vector<absl::string_view> roots = absl::StrSplit(payload, '\n');
  const auto& namespaces = ExtensionNamespaceRoots();
  for (absl::string_view r : roots) {
    if (namespaces.contains(r)) return true;
  }
  const auto& receivers = ExtensionReceiverRoots();
  return std::all_of(roots.begin(), roots.end(), [&](absl::string_view r) {
    return receivers.contains(r);
  });
}

// Source-level fallback for ext-lib rows whose Compile failure
// surfaces at the parser (before type-check, so the
// `kUndeclaredReferencesUrl` payload tag never gets set).  The
// markers below are syntax shapes that exist ONLY in cel-cpp
// extensions we don't register:
//
//   `.?` / `[?` / `{?`    — optionals extension's optional-chaining
//                           (select / index / struct-or-map init)
//   `cel.iterVar(`        — block_ext's iterator-variable form
//   `cel.index(`          — block_ext's bound-index reference
//   `cel.block(`          — block_ext's bound-block expression
//
// A real CEL parser-rejected row in the conformance corpus would
// live under `disable_check: true` (a parse-failure test); those
// are caught upstream by `ScopeReject` before we reach the
// classifier.  Any non-disable_check row that fails to parse and
// matches one of these markers is by construction an ext-lib gap.
bool LooksLikeExtensionSyntax(absl::string_view expr) {
  return absl::StrContains(expr, ".?") || absl::StrContains(expr, "[?") ||
         absl::StrContains(expr, "{?") ||
         absl::StrContains(expr, "cel.iterVar(") ||
         absl::StrContains(expr, "cel.index(") ||
         absl::StrContains(expr, "cel.block(");
}

}  // namespace

namespace {

// Per-test compiler vs shared default ----------------------------

// The default compiler is reused for every row whose `type_env` is
// empty (the bulk of the corpus).  Rows with declared variables
// build a per-test compiler via `BuildPerTestCompiler` below.
const celwasm::Compiler& SharedDefaultCompiler() {
  static const absl::NoDestructor<celwasm::Compiler> kShared([] {
    auto c = celwasm::Compiler::NewBuilder().Build();
    ABSL_CHECK_OK(c) << "default Compiler::Build failed";
    return *std::move(c);
  }());
  return *kShared;
}

absl::StatusOr<celwasm::Compiler> BuildPerTestCompiler(const SimpleTest& t) {
  auto b = celwasm::Compiler::NewBuilder();
  if (auto s = DeclareVariablesOnBuilder(t, b); !s.ok()) return s;
  return std::move(b).Build();
}

absl::StatusOr<celwasm::Program> CompileForTest(
    const celwasm::Compiler& compiler, const SimpleTest& t) {
  celwasm::CompilerOptions opts;
  opts.container = t.container();
  // m28 — dual-mode conformance.  Mode is a runtime flag rather than
  // a compile-time macro because `run_conformance` is shipped as a
  // single binary and the gate script invokes it twice (once per
  // mode), gating each mode's pass count against its own baseline.
  const std::string mode = absl::GetFlag(FLAGS_link_mode);
  if (mode == "static") {
    opts.link_mode = celwasm::CompilerOptions::LinkMode::kStatic;
  } else {
    opts.link_mode = celwasm::CompilerOptions::LinkMode::kDynamic;
  }
  return compiler.Compile(t.expr(), opts);
}

// Map a compile-stage status to either an `kUnsupported` outcome
// (graceful) or `kFail` (regression).  Classification order:
//
//   1. `kUnimplemented` status → `kCompileUnimpl`.
//   2. `kStaticSubsetViolationUrl` payload (set by `RejectDyn`) →
//      `kStaticSubset`.
//   3. `kUndeclaredReferencesUrl` payload (set by `RunTypeCheck`)
//      with `IsExtensionFailure` true → `kExtensionUnimpl`.
//   4. Source-level fallback: parser failed before type-check could
//      attach the payload; if `t.expr()` contains ext-lib-only
//      syntax markers, classify as `kExtensionUnimpl`.
//   5. Otherwise → `kFail` (genuine regression).
Result ClassifyCompileFailure(const SimpleTest& t, const absl::Status& s) {
  if (s.code() == absl::StatusCode::kUnimplemented) {
    return Skip(SkipCategory::kCompileUnimpl, std::string(s.message()));
  }
  if (auto p = s.GetPayload(kStaticSubsetViolationUrl); p.has_value()) {
    return Skip(SkipCategory::kStaticSubset, std::string(s.message()));
  }
  if (auto p = s.GetPayload(kUndeclaredReferencesUrl); p.has_value()) {
    const std::string roots(p->Flatten());
    if (IsExtensionFailure(roots)) {
      return Skip(SkipCategory::kExtensionUnimpl,
                  absl::StrCat("undeclared roots: ", roots));
    }
  }
  if (LooksLikeExtensionSyntax(t.expr())) {
    return Skip(SkipCategory::kExtensionUnimpl,
                absl::StrCat("ext-lib syntax: ", s.message()));
  }
  // A row whose matcher expects an error is satisfied by a
  // compile-stage error too: cel-cpp's upstream conformance/run.cc
  // treats any error (compile OR eval) as matching an error matcher.
  // Reaching here means the program failed to compile for a real
  // reason (not a SKIP carve-out above), which IS the error the row
  // expects — e.g. `isIP(cidr(...))` is a deliberate "no matching
  // overload" type-check error the corpus asserts via eval_error.
  // Rows expecting a concrete value have no error matcher and still
  // Fail here.
  if (IsEvalErrorMatcher(t)) {
    return {Outcome::kPass, SkipCategory::kEnvelope, ""};
  }
  return Fail("compile", s);
}

Result ClassifyEvalFailure(absl::string_view stage, const absl::Status& s) {
  if (s.code() == absl::StatusCode::kUnimplemented) {
    return Skip(SkipCategory::kEvalUnimpl,
                absl::StrCat(stage, ": ", s.message()));
  }
  return Fail(stage, s);
}

// Per-matcher-kind eval branches ---------------------------------

Result RunUnknownBranch(celwasm::Instance& inst,
                        const celwasm::Activation& act) {
  auto val_or = inst.PartialEval(act, {});
  if (!val_or.ok()) return ClassifyEvalFailure("partial_eval", val_or.status());
  if (auto s = CompareUnknown(*val_or); !s.ok()) return Fail("compare", s);
  return {Outcome::kPass, SkipCategory::kEnvelope, ""};
}

Result RunValueBranch(celwasm::Instance& inst, const celwasm::Activation& act,
                      const SimpleTest& t) {
  auto val_or = inst.Eval(act);
  if (!val_or.ok()) return ClassifyEvalFailure("eval", val_or.status());
  if (auto s = CompareValue(*val_or, t.value()); !s.ok()) {
    return Fail("compare", s);
  }
  return {Outcome::kPass, SkipCategory::kEnvelope, ""};
}

// Implicit-bool-true: a SimpleTest with no result_matcher set is a
// bool-asserting expression; success means it evaluates to true.
// Per the upstream cel-cpp convention in `conformance/run.cc`.
Result RunImplicitBoolTrueBranch(celwasm::Instance& inst,
                                 const celwasm::Activation& act) {
  auto val_or = inst.Eval(act);
  if (!val_or.ok()) return ClassifyEvalFailure("eval", val_or.status());
  cel::expr::Value want;
  want.set_bool_value(true);
  if (auto s = CompareValue(*val_or, want); !s.ok()) {
    return Fail("compare", s);
  }
  return {Outcome::kPass, SkipCategory::kEnvelope, ""};
}

// `kEvalError` and `kAnyEvalErrors` share the kind-only compare;
// `any_eval_errors` succeeds if any of its contained `errors[]`
// matches (kind-only semantics make all entries equivalent, so we
// can just compare against the first or an empty set).
Result RunEvalErrorBranch(celwasm::Instance& inst,
                          const celwasm::Activation& act, const SimpleTest& t) {
  auto val_or = inst.Eval(act);
  if (!val_or.ok()) return ClassifyEvalFailure("eval", val_or.status());
  cel::expr::ErrorSet empty;
  const cel::expr::ErrorSet& want =
      t.result_matcher_case() == SimpleTest::kEvalError ? t.eval_error()
                                                        : empty;
  if (auto s = CompareEvalError(*val_or, want); !s.ok()) {
    return Fail("compare", s);
  }
  return {Outcome::kPass, SkipCategory::kEnvelope, ""};
}

Result RunTypedResultBranch(celwasm::Instance& inst,
                            const celwasm::Activation& act,
                            const SimpleTest& t) {
  // typed_result rows compare on the embedded `result` via the
  // standard value comparator.  Pure-`deduced_type` rows (no
  // `result` set) SKIP — the no-eval check path is harness
  // follow-up.
  if (!t.typed_result().has_result()) {
    return Skip(SkipCategory::kEnvelope,
                "typed_result matcher with no `result` value");
  }
  auto val_or = inst.Eval(act);
  if (!val_or.ok()) return ClassifyEvalFailure("eval", val_or.status());
  if (auto s = CompareValue(*val_or, t.typed_result().result()); !s.ok()) {
    return Fail("compare", s);
  }
  return {Outcome::kPass, SkipCategory::kEnvelope, ""};
}

// Pre-compile scope check.  Returns a `kUnsupported` result for
// rows the harness handles without ever burning a compile —
// `disable_check`, `check_only`, and matcher-kind-out-of-envelope.
std::optional<Result> ScopeReject(const SimpleTest& t) {
  if (t.disable_check()) {
    return Skip(SkipCategory::kDisableCheck,
                "parse-only eval out of conformance scope "
                "(checker-passed expressions only)");
  }
  if (t.check_only()) {
    return Skip(SkipCategory::kCheckOnly,
                "typed_result matcher requires no-eval check path "
                "(harness follow-up)");
  }
  // Snapshot the inner value kind BEFORE IsInEnvelope's narrowing —
  // the path-sensitive analyzer concludes every result_matcher case
  // is ruled out post-check and flags subsequent protobuf accesses
  // as EnumCastOutOfRange.  Reading once up front sidesteps that.
  const auto value_kind = t.value().kind_case();
  if (!IsInEnvelope(t)) {
    return Skip(SkipCategory::kEnvelope, EnvelopeRejectReason(value_kind));
  }
  return std::nullopt;
}

// Returns a Compiler appropriate for the row — the shared default
// when `type_env` is empty, a per-test build otherwise.  The
// per-test path can SKIP gracefully (aggregate type_env decls) or
// FAIL (real Builder errors).  Variant ordering: 0 = shared, 1 =
// owned per-test build, 2 = SKIP/FAIL result already produced.
std::variant<const celwasm::Compiler*, celwasm::Compiler, Result>
ResolveCompiler(const SimpleTest& t) {
  if (t.type_env_size() == 0) return &SharedDefaultCompiler();
  auto built = BuildPerTestCompiler(t);
  if (built.ok()) return *std::move(built);
  if (built.status().code() == absl::StatusCode::kUnimplemented) {
    return Skip(SkipCategory::kTypeEnvUnsupported,
                std::string(built.status().message()));
  }
  return Fail("type_env", built.status());
}

// Dispatches the matcher-kind eval branch.  `RunOne`'s tail is just
// "plan + dispatch"; lifting it keeps `RunOne` under the lint gate.
Result DispatchEvalBranch(celwasm::Instance& inst,
                          const celwasm::Activation& act, const SimpleTest& t) {
  if (IsUnknownMatcher(t)) return RunUnknownBranch(inst, act);
  if (IsEvalErrorMatcher(t)) return RunEvalErrorBranch(inst, act, t);
  if (t.result_matcher_case() == SimpleTest::RESULT_MATCHER_NOT_SET) {
    return RunImplicitBoolTrueBranch(inst, act);
  }
  if (t.result_matcher_case() == SimpleTest::kTypedResult) {
    return RunTypedResultBranch(inst, act, t);
  }
  return RunValueBranch(inst, act, t);
}

}  // namespace

// NOLINTBEGIN(misc-use-internal-linkage)
Result RunOne(const SimpleTest& t, const celwasm::Engine& engine) {
  if (auto skip = ScopeReject(t)) return *skip;

  auto compiler_var = ResolveCompiler(t);
  if (std::holds_alternative<Result>(compiler_var)) {
    return std::get<Result>(std::move(compiler_var));
  }
  const celwasm::Compiler* compiler =
      std::holds_alternative<const celwasm::Compiler*>(compiler_var)
          ? std::get<const celwasm::Compiler*>(compiler_var)
          : &std::get<celwasm::Compiler>(compiler_var);

  celwasm::Activation act;
  if (auto s = PopulateActivation(t, act); !s.ok()) {
    if (s.code() == absl::StatusCode::kUnimplemented) {
      return Skip(SkipCategory::kBindingUnsupported, std::string(s.message()));
    }
    return Fail("bindings", s);
  }

  auto prog_or = CompileForTest(*compiler, t);
  if (!prog_or.ok()) return ClassifyCompileFailure(t, prog_or.status());

  auto inst_or = engine.Plan(*prog_or);
  if (!inst_or.ok()) return Fail("plan", inst_or.status());

  celwasm::Instance inst = *std::move(inst_or);
  return DispatchEvalBranch(inst, act, t);
}

absl::Status LoadTestFile(absl::string_view path, SimpleTestFile& out) {
  // Called from runtime code rather than at static-init time so the
  // .pb.o force-link doesn't tickle
  // `bugprone-throwing-static-initialization` on the generated
  // `descriptor()` accessor.
  ForceLinkFixtureDescriptors();
  std::ifstream f{std::string(path)};
  if (!f) {
    return absl::NotFoundError(absl::StrCat("conformance textproto: ", path));
  }
  std::stringstream buf;
  buf << f.rdbuf();
  if (!google::protobuf::TextFormat::ParseFromString(buf.str(), &out)) {
    return absl::InvalidArgumentError(absl::StrCat("parse failure: ", path));
  }
  return absl::OkStatus();
}
// NOLINTEND(misc-use-internal-linkage)

}  // namespace celwasm::conformance
