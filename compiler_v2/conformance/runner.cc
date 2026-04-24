#include "compiler_v2/conformance/runner.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
// `test_all_types.pb.h` is included for the side-effect of
// registering the `cel.expr.conformance.proto{2,3}.TestAllTypes`
// descriptors into the global pool.  Several fixture textprotos
// (block_ext, dynamic, enums, parse, proto2, proto2_ext, proto3,
// type_deduction) embed `google.protobuf.Any` values of these
// types; without the descriptors registered, TextFormat::Parse
// fails at load time with "Could not find type".  The pointer
// references below defeat the linker's dead-strip — protobuf
// descriptor registration is a static constructor in the .pb.cc,
// and nothing else in this TU names a symbol from those objects.
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto2/test_all_types_extensions.pb.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/value.pb.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/text_format.h"

namespace celwasm::conformance {

namespace {

// Force-link the .pb.o files that carry the static-initialiser
// descriptor registration.  Calling the generated `descriptor()`
// class-static method from a function (rather than a namespace-scope
// variable initialiser) keeps the linker from dead-stripping the
// .pb.o while dodging clang-tidy's
// `bugprone-throwing-static-initialization` on the
// generated-code declaration.  The helper is called from `RunOne`
// via a single no-op guarded by the `static` local below.
void ForceLinkFixtureDescriptors() {
  [[maybe_unused]] const auto* p2 =
      ::cel::expr::conformance::proto2::TestAllTypes::descriptor();
  [[maybe_unused]] const auto* p3 =
      ::cel::expr::conformance::proto3::TestAllTypes::descriptor();
  // Extensions live in a separate .pb.cc.  Reference the extension
  // scoped message's descriptor to pull that TU in — the file-scope
  // extension *fields* (`int32_ext`, `nested_ext`, …) register via
  // the same static initialiser.
  [[maybe_unused]] const auto* p2x = ::cel::expr::conformance::proto2::
      Proto2ExtensionScopedMessage::descriptor();
}

using ::cel::expr::conformance::test::SimpleTest;
using ::cel::expr::conformance::test::SimpleTestFile;
using ProtoValue = ::cel::expr::Value;

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

// A mismatch reports kinds only — compare-site callers already know
// which test they ran, and the detailed value isn't load-bearing for
// debugging (the fail list shows the expression source).  Keeping the
// switch-heavy per-kind formatters out of the TU also sidesteps
// clang-tidy's `bugprone-branch-clone` on structurally-similar
// `return absl::StrCat(...)` arms.
absl::Status Mismatch(const cel::Value& got, const ProtoValue& want) {
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

}  // namespace

// clang-tidy runs per-TU and can't see the external callers in
// run_conformance.cc, so `misc-use-internal-linkage` misclassifies
// these public-API functions (declared in runner.h) as could-be-
// static.  Mirror the NOLINT pattern used in
// compiler/cli/celwasmc_eval_main.cc.
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

bool IsM1Eligible(const SimpleTest& t) {
  if (t.disable_check()) return false;
  if (t.check_only()) return false;
  if (!t.type_env().empty()) return false;
  if (!t.bindings().empty()) return false;
  if (!t.container().empty()) return false;
  if (t.result_matcher_case() != SimpleTest::kValue) return false;
  return IsScalarMatcherKind(t.value().kind_case());
}

absl::Status CompareValue(const cel::Value& got, const ProtoValue& want) {
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
          "non-scalar matcher — caller should filter via IsM1Eligible");
  }
}

namespace {

Result Unsupported(absl::string_view why) {
  return {Outcome::kUnsupported, std::string(why)};
}

Result Fail(absl::string_view stage, const absl::Status& s) {
  return {Outcome::kFail, absl::StrCat(stage, ": ", s.ToString())};
}

}  // namespace

Result RunOne(const SimpleTest& t, const cel::Compiler& compiler,
              const cel::Engine& engine) {
  if (!IsM1Eligible(t)) return Unsupported("outside M1 envelope");

  auto prog_or = compiler.Compile(t.expr());
  if (!prog_or.ok()) {
    // Two non-regression compile outcomes:
    //   - `Unimplemented` — `expr_lower` arm not yet shipped (future
    //     milestone fills in).
    //   - `InvalidArgument` with "static subset" — `RejectDyn` gate;
    //     the test uses `dyn(…)` or other dynamic typing which
    //     `celwasm` deliberately never accepts (CLAUDE.md § "What
    //     not to do").  Not a regression, ever.
    const auto& s = prog_or.status();
    if (s.code() == absl::StatusCode::kUnimplemented) {
      return Unsupported(absl::StrCat("compile unimplemented: ", s.message()));
    }
    if (s.code() == absl::StatusCode::kInvalidArgument &&
        absl::StrContains(s.message(), "static subset")) {
      return Unsupported("outside static subset (dyn)");
    }
    return Fail("compile", s);
  }

  auto inst_or = engine.Plan(*prog_or);
  if (!inst_or.ok()) return Fail("plan", inst_or.status());

  cel::Instance inst = *std::move(inst_or);
  auto val_or = inst.Eval();
  if (!val_or.ok()) return Fail("eval", val_or.status());

  absl::Status s = CompareValue(*val_or, t.value());
  if (!s.ok()) return Fail("compare", s);
  return {Outcome::kPass, ""};
}

absl::Status LoadTestFile(absl::string_view path, SimpleTestFile& out) {
  // Called from runtime code rather than at static-init time so the
  // .pb.o force-link doesn't tickle `bugprone-throwing-static-
  // initialization` on the generated `descriptor()` accessor.
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
