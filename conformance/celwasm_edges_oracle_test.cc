// Differential confirmation for the first-party
// `conformance/testdata/celwasm_edges.textproto` fixture: every row's
// expected value / expected-error kind is checked against the REAL
// cel-cpp pipeline via `testdata/cel_cpp_oracle`.
//
// This is the "oracle is the empirical tiebreaker" discipline from
// CLAUDE.md applied to a whole fixture at once: a row whose expected
// value was *reasoned out* rather than oracle-confirmed is a guess,
// so this test re-derives every expectation from cel-cpp on every
// run.  Rows WITHOUT bindings go through `EvalWithCelCpp`; rows WITH
// bindings go through `PartialEvalWithCelCpp` (no unknown patterns —
// the vars bind as `dyn`, which is semantically transparent for the
// concrete message / string values these rows bind).
//
// If this test disagrees with the fixture, the FIXTURE is wrong —
// fix the textproto, never the comparison.

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/value.pb.h"
#include "google/protobuf/generated_message_reflection.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/util/message_differencer.h"
#include "gtest/gtest.h"
#include "testdata/cel_cpp_oracle.h"

namespace celwasm::conformance {
namespace {

using ::cel::expr::conformance::test::SimpleTest;
using ::cel::expr::conformance::test::SimpleTestFile;
using ::celwasm::testdata::EvalWithCelCpp;
using ::celwasm::testdata::OracleResult;
using ::celwasm::testdata::OracleVar;
using ::celwasm::testdata::PartialEvalWithCelCpp;

constexpr absl::string_view kFixturePath =
    "conformance/testdata/celwasm_edges.textproto";

// Force the proto3 conformance descriptors into the generated pool so
// TextFormat can resolve the fixture's `google.protobuf.Any` payloads.
[[maybe_unused]] const bool
    kLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<
          ::cel::expr::conformance::proto3::TestAllTypes>();
      return true;
    }();

SimpleTestFile LoadFixture() {
  std::ifstream f{std::string(kFixturePath)};
  EXPECT_TRUE(f.good()) << "cannot open " << kFixturePath;
  std::stringstream buf;
  buf << f.rdbuf();
  SimpleTestFile file;
  EXPECT_TRUE(google::protobuf::TextFormat::ParseFromString(buf.str(), &file))
      << "textproto parse failed for " << kFixturePath;
  return file;
}

// Route one row through the oracle, picking the entry point by
// whether the row carries bindings.
absl::StatusOr<OracleResult> EvalRow(const SimpleTest& t) {
  if (t.bindings().empty()) {
    return EvalWithCelCpp(t.expr(), t.container());
  }
  std::vector<OracleVar> vars;
  vars.reserve(t.bindings().size());
  for (const auto& [name, expr_value] : t.bindings()) {
    OracleVar v;
    v.name = name;
    EXPECT_TRUE(expr_value.has_value())
        << "binding '" << name << "' is not a plain value";
    v.value = expr_value.value();
    vars.push_back(std::move(v));
  }
  return PartialEvalWithCelCpp(t.expr(), t.container(), vars,
                               /*unknown_patterns=*/{});
}

// Compare an oracle outcome against the row's result matcher.  Only
// the matcher kinds this fixture uses (`value`, `eval_error`) are
// admitted — anything else is an authoring error in the fixture.
testing::AssertionResult MatchesRow(const SimpleTest& t,
                                    const OracleResult& got) {
  switch (t.result_matcher_case()) {
    case SimpleTest::kValue: {
      if (got.is_error) {
        return testing::AssertionFailure()
               << "cel-cpp errored ('" << got.error_message
               << "') but the fixture expects a value";
      }
      if (google::protobuf::util::MessageDifferencer::Equals(got.value,
                                                             t.value())) {
        return testing::AssertionSuccess();
      }
      return testing::AssertionFailure()
             << "value mismatch: cel-cpp=" << got.value.ShortDebugString()
             << " fixture=" << t.value().ShortDebugString();
    }
    case SimpleTest::kEvalError:
      if (!got.is_error) {
        return testing::AssertionFailure()
               << "fixture expects an eval error but cel-cpp returned "
               << got.value.ShortDebugString();
      }
      return testing::AssertionSuccess();
    default:
      return testing::AssertionFailure()
             << "fixture row uses a matcher kind this differential test "
                "does not admit (case "
             << t.result_matcher_case() << ")";
  }
}

TEST(CelwasmEdgesOracleTest, EveryRowMatchesCelCpp) {
  const SimpleTestFile file = LoadFixture();
  ASSERT_GT(file.section_size(), 0);
  for (const auto& section : file.section()) {
    for (const SimpleTest& t : section.test()) {
      SCOPED_TRACE(absl::StrCat(section.name(), "/", t.name(), ": ",
                                t.expr().substr(0, 120)));
      auto got = EvalRow(t);
      // EXPECT (not ASSERT): a harness failure on one row must not
      // mask the differential result of every row after it.
      EXPECT_TRUE(got.ok()) << "oracle harness failure: " << got.status();
      if (got.ok()) EXPECT_TRUE(MatchesRow(t, *got));
    }
  }
}

}  // namespace
}  // namespace celwasm::conformance
