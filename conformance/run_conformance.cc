// Exploration binary: walks every upstream
// `spec/tests/simple/testdata/*.textproto` fixture, runs each
// `SimpleTest` through the celwasm pipeline, and prints a
// per-file `pass / skip / fail` tally + a SKIP-by-category
// breakdown (using `SkipCategory` from `runner.h`).  Optionally
// prints individual FAIL details and SKIP-detail samples.
//
// Not a CI gate — exits 0 regardless of outcome.  Use this to
// answer "how much of the spec conformance does this branch
// cover?" and to regenerate the per-fixture inventory + the
// SKIP-by-category aggregate that `conformance/README.md`
// quotes.
//
// Invocation:
//
//   bazel run //conformance:run_conformance
//   bazel run //conformance:run_conformance -- \
//       --file spec/tests/simple/testdata/comparisons.textproto
//   bazel run //conformance:run_conformance -- \
//       --max_skip_examples=2000   # dump every SKIP detail

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "eval/engine.h"
#include "conformance/runner.h"

// Same suppressions as compiler/cli/celwasmc_eval_main.cc:
//   - misc-use-internal-linkage: ABSL_FLAG generates extern helpers.
//   - bugprone-throwing-static-initialization: default ctors for
//     std::vector<std::string> / std::uint32_t flag defaults.
// NOLINTBEGIN(misc-use-internal-linkage,bugprone-throwing-static-initialization)
ABSL_FLAG(std::vector<std::string>, file, {},
          "Explicit textproto paths to run.  Empty = the full corpus "
          "(see DefaultCorpus()).");

ABSL_FLAG(std::uint32_t, max_skip_examples, 0,
          "Per-fixture cap on SKIP detail lines printed.  0 means no "
          "SKIP details — use a non-zero value when diagnosing why a "
          "fixture's PASS count isn't budging.  The SKIP-by-category "
          "summary is always printed regardless.");
ABSL_FLAG(std::uint32_t, max_fail_examples, 5,
          "Per-file cap on FAIL detail lines printed.");
// NOLINTEND(misc-use-internal-linkage,bugprone-throwing-static-initialization)

namespace {

// Default corpus: every textproto we ship under `spec/tests/simple/testdata`.
// Listed once here and re-used by the BUILD `data =` attribute via
// `SIMPLE_TESTDATA` in BUILD.bazel; both lists MUST stay in sync (a
// new fixture lands here AND there).  Future cleanup tracked in
// `conformance/README.md`'s "Future work" section.
const std::vector<std::string>& DefaultCorpus() {
  static const auto* files = new std::vector<std::string>{
      "spec/tests/simple/testdata/basic.textproto",
      "spec/tests/simple/testdata/bindings_ext.textproto",
      "spec/tests/simple/testdata/block_ext.textproto",
      "spec/tests/simple/testdata/comparisons.textproto",
      "spec/tests/simple/testdata/conversions.textproto",
      "spec/tests/simple/testdata/dynamic.textproto",
      "spec/tests/simple/testdata/encoders_ext.textproto",
      "spec/tests/simple/testdata/enums.textproto",
      "spec/tests/simple/testdata/fields.textproto",
      "spec/tests/simple/testdata/fp_math.textproto",
      "spec/tests/simple/testdata/integer_math.textproto",
      "spec/tests/simple/testdata/lists.textproto",
      "spec/tests/simple/testdata/logic.textproto",
      "spec/tests/simple/testdata/macros.textproto",
      "spec/tests/simple/testdata/macros2.textproto",
      "spec/tests/simple/testdata/math_ext.textproto",
      "spec/tests/simple/testdata/namespace.textproto",
      "spec/tests/simple/testdata/network_ext.textproto",
      "spec/tests/simple/testdata/optionals.textproto",
      "spec/tests/simple/testdata/parse.textproto",
      "spec/tests/simple/testdata/plumbing.textproto",
      "spec/tests/simple/testdata/proto2.textproto",
      "spec/tests/simple/testdata/proto2_ext.textproto",
      "spec/tests/simple/testdata/proto3.textproto",
      "spec/tests/simple/testdata/string.textproto",
      "spec/tests/simple/testdata/string_ext.textproto",
      "spec/tests/simple/testdata/timestamps.textproto",
      "spec/tests/simple/testdata/type_deduction.textproto",
      "spec/tests/simple/testdata/unknowns.textproto",
      "spec/tests/simple/testdata/wrappers.textproto",
  };
  return *files;
}

// All known `SkipCategory` values, in display order.  Keeping the
// list in one place lets the per-fixture summary table iterate it
// deterministically (rather than depending on map ordering).
constexpr std::array<celwasm::conformance::SkipCategory, 9> kCategories{
    celwasm::conformance::SkipCategory::kDisableCheck,
    celwasm::conformance::SkipCategory::kCheckOnly,
    celwasm::conformance::SkipCategory::kEnvelope,
    celwasm::conformance::SkipCategory::kStaticSubset,
    celwasm::conformance::SkipCategory::kCompileUnimpl,
    celwasm::conformance::SkipCategory::kEvalUnimpl,
    celwasm::conformance::SkipCategory::kExtensionUnimpl,
    celwasm::conformance::SkipCategory::kTypeEnvUnsupported,
    celwasm::conformance::SkipCategory::kBindingUnsupported,
};

struct FileTally {
  std::string path;
  std::size_t total = 0;
  std::size_t pass = 0;
  std::size_t skip = 0;
  std::size_t fail = 0;
  // Counts indexed by `SkipCategory`'s underlying value.  Lives
  // alongside the totals so the printer doesn't need a separate map.
  std::array<std::size_t, kCategories.size()> by_category{};
  std::vector<std::string> fail_details;
  std::vector<std::string> skip_details;
};

void RecordSkip(FileTally& out, celwasm::conformance::SkipCategory c) {
  for (std::size_t i = 0; i < kCategories.size(); ++i) {
    if (kCategories[i] == c) {
      ++out.by_category[i];
      return;
    }
  }
  ABSL_CHECK(false) << "unhandled SkipCategory in run_conformance";
}

FileTally RunFile(absl::string_view path, const celwasm::api::Engine& engine,
                  std::uint32_t max_fail_examples,
                  std::uint32_t max_skip_examples) {
  FileTally out{.path = std::string(path)};
  cel::expr::conformance::test::SimpleTestFile file;
  if (auto s = celwasm::conformance::LoadTestFile(path, file); !s.ok()) {
    out.fail_details.push_back(absl::StrCat("load: ", s.ToString()));
    return out;
  }
  for (const auto& section : file.section()) {
    for (const auto& t : section.test()) {
      ++out.total;
      auto r = celwasm::conformance::RunOne(t, engine);
      const std::string label =
          absl::StrCat(section.name(), "/", t.name(), " `", t.expr(), "`");
      switch (r.outcome) {
        case celwasm::conformance::Outcome::kPass:
          ++out.pass;
          break;
        case celwasm::conformance::Outcome::kUnsupported:
          ++out.skip;
          RecordSkip(out, r.category);
          if (out.skip_details.size() < max_skip_examples) {
            out.skip_details.push_back(
                absl::StrCat(label, " — ",
                             celwasm::conformance::SkipCategoryName(r.category),
                             ": ", r.detail));
          }
          break;
        case celwasm::conformance::Outcome::kFail:
          ++out.fail;
          if (out.fail_details.size() < max_fail_examples) {
            out.fail_details.push_back(absl::StrCat(label, " — ", r.detail));
          }
          break;
      }
    }
  }
  return out;
}

void PrintTally(const FileTally& f) {
  std::cout << absl::StrCat("  ", f.path, "\n", "    total=", f.total,
                            "  pass=", f.pass, "  skip=", f.skip,
                            "  fail=", f.fail, "\n");
  if (f.skip > 0) {
    std::string by_cat = "    skip-by-category:";
    bool any = false;
    for (std::size_t i = 0; i < kCategories.size(); ++i) {
      if (f.by_category[i] == 0) continue;
      absl::StrAppend(&by_cat, " ",
                      celwasm::conformance::SkipCategoryName(kCategories[i]),
                      "=", f.by_category[i]);
      any = true;
    }
    if (any) std::cout << by_cat << "\n";
  }
  for (const auto& d : f.skip_details) {
    std::cout << "      SKIP " << d << "\n";
  }
  for (const auto& d : f.fail_details) {
    std::cout << "      FAIL " << d << "\n";
  }
}

void PrintCorpusBreakdown(
    const std::array<std::size_t, kCategories.size()>& corpus_by_category) {
  std::cout << "\nskip-by-category (corpus-wide):\n";
  for (std::size_t i = 0; i < kCategories.size(); ++i) {
    if (corpus_by_category[i] == 0) continue;
    std::cout << absl::StrCat(
        "  ", celwasm::conformance::SkipCategoryName(kCategories[i]), " = ",
        corpus_by_category[i], "\n");
  }
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  absl::ParseCommandLine(argc, argv);

  auto e = celwasm::api::Engine::NewBuilder().Build();
  ABSL_CHECK_OK(e);
  celwasm::api::Engine engine = *std::move(e);

  const auto& paths = absl::GetFlag(FLAGS_file).empty()
                          ? DefaultCorpus()
                          : absl::GetFlag(FLAGS_file);
  const std::uint32_t max_fail = absl::GetFlag(FLAGS_max_fail_examples);
  const std::uint32_t max_skip = absl::GetFlag(FLAGS_max_skip_examples);

  std::size_t total = 0;
  std::size_t pass = 0;
  std::size_t skip = 0;
  std::size_t fail = 0;
  std::array<std::size_t, kCategories.size()> corpus_by_category{};

  std::cout << "celwasm conformance run\n";
  for (const auto& path : paths) {
    FileTally t = RunFile(path, engine, max_fail, max_skip);
    PrintTally(t);
    total += t.total;
    pass += t.pass;
    skip += t.skip;
    fail += t.fail;
    for (std::size_t i = 0; i < kCategories.size(); ++i) {
      corpus_by_category[i] += t.by_category[i];
    }
  }
  std::cout << absl::StrCat("\nsummary: total=", total, "  pass=", pass,
                            "  skip=", skip, "  fail=", fail, "\n");
  PrintCorpusBreakdown(corpus_by_category);
  return 0;
}
