// Exploration binary: walks every upstream `tests/simple/testdata/
// *.textproto`, runs each `SimpleTest` through the compiler_v2
// pipeline, and prints a per-file `pass / skip / fail` tally plus
// the first few failure details per file.  Not a gate — exits 0
// regardless of outcome.  Use this to answer "how much of the spec
// conformance does M<n> cover?" and to regenerate the inventory
// table in README.md.
//
// Invocation:
//
//   bazel run //compiler_v2/conformance:run_conformance
//   bazel run //compiler_v2/conformance:run_conformance -- \
//       --file tests/simple/testdata/comparisons.textproto
//
// Exit 0 always — no failure is fatal.  The caller reads the table.

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
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/conformance/runner.h"

// Same suppressions as compiler/cli/celwasmc_eval_main.cc:
//   - misc-use-internal-linkage: ABSL_FLAG generates extern helpers.
//   - bugprone-throwing-static-initialization: default ctors for
//     std::vector<std::string> / std::uint32_t flag defaults.
// NOLINTBEGIN(misc-use-internal-linkage,bugprone-throwing-static-initialization)
ABSL_FLAG(std::vector<std::string>, file, {},
          "Explicit textproto paths to run.  Empty = run the built-in "
          "list covering the whole `tests/simple/testdata/` corpus.");

ABSL_FLAG(std::uint32_t, max_fail_examples, 5,
          "Per-file cap on fail-detail lines printed.");
// NOLINTEND(misc-use-internal-linkage,bugprone-throwing-static-initialization)

namespace {

// Default corpus: every textproto we ship under `tests/simple/testdata`.
// Listed explicitly (rather than glob'd at runtime) so the `data =`
// attribute and this list stay in sync — adding a new fixture forces
// both a BUILD edit and a source edit.
const std::vector<std::string>& DefaultCorpus() {
  // All 30 `.textproto` fixtures shipped under `tests/simple/testdata/`.
  // Kept in sync with `SIMPLE_TESTDATA` in the BUILD file (the `data =`
  // attribute must also list them).
  static const auto* files = new std::vector<std::string>{
      "tests/simple/testdata/basic.textproto",
      "tests/simple/testdata/bindings_ext.textproto",
      "tests/simple/testdata/block_ext.textproto",
      "tests/simple/testdata/comparisons.textproto",
      "tests/simple/testdata/conversions.textproto",
      "tests/simple/testdata/dynamic.textproto",
      "tests/simple/testdata/encoders_ext.textproto",
      "tests/simple/testdata/enums.textproto",
      "tests/simple/testdata/fields.textproto",
      "tests/simple/testdata/fp_math.textproto",
      "tests/simple/testdata/integer_math.textproto",
      "tests/simple/testdata/lists.textproto",
      "tests/simple/testdata/logic.textproto",
      "tests/simple/testdata/macros.textproto",
      "tests/simple/testdata/macros2.textproto",
      "tests/simple/testdata/math_ext.textproto",
      "tests/simple/testdata/namespace.textproto",
      "tests/simple/testdata/network_ext.textproto",
      "tests/simple/testdata/optionals.textproto",
      "tests/simple/testdata/parse.textproto",
      "tests/simple/testdata/plumbing.textproto",
      "tests/simple/testdata/proto2.textproto",
      "tests/simple/testdata/proto2_ext.textproto",
      "tests/simple/testdata/proto3.textproto",
      "tests/simple/testdata/string.textproto",
      "tests/simple/testdata/string_ext.textproto",
      "tests/simple/testdata/timestamps.textproto",
      "tests/simple/testdata/type_deduction.textproto",
      "tests/simple/testdata/unknowns.textproto",
      "tests/simple/testdata/wrappers.textproto",
  };
  return *files;
}

struct FileTally {
  std::string path;
  std::size_t total = 0;
  std::size_t pass = 0;
  std::size_t skip = 0;
  std::size_t fail = 0;
  std::vector<std::string> fail_details;
};

FileTally RunFile(absl::string_view path, const cel::Compiler& compiler,
                  const cel::Engine& engine, std::uint32_t max_examples) {
  FileTally out{.path = std::string(path)};
  cel::expr::conformance::test::SimpleTestFile file;
  if (auto s = celwasm::conformance::LoadTestFile(path, file); !s.ok()) {
    out.fail_details.push_back(absl::StrCat("load: ", s.ToString()));
    return out;
  }
  for (const auto& section : file.section()) {
    for (const auto& t : section.test()) {
      ++out.total;
      auto r = celwasm::conformance::RunOne(t, compiler, engine);
      switch (r.outcome) {
        case celwasm::conformance::Outcome::kPass:
          ++out.pass;
          break;
        case celwasm::conformance::Outcome::kUnsupported:
          ++out.skip;
          break;
        case celwasm::conformance::Outcome::kFail:
          ++out.fail;
          if (out.fail_details.size() < max_examples) {
            out.fail_details.push_back(absl::StrCat(section.name(), "/",
                                                    t.name(), " `", t.expr(),
                                                    "` — ", r.detail));
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
  for (const auto& d : f.fail_details) {
    std::cout << "      FAIL " << d << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  absl::ParseCommandLine(argc, argv);

  auto c = cel::Compiler::NewBuilder().Build();
  ABSL_CHECK_OK(c);
  cel::Compiler compiler = *std::move(c);
  auto e = cel::Engine::NewBuilder().Build();
  ABSL_CHECK_OK(e);
  cel::Engine engine = *std::move(e);

  const auto& paths = absl::GetFlag(FLAGS_file).empty()
                          ? DefaultCorpus()
                          : absl::GetFlag(FLAGS_file);
  const std::uint32_t max_examples = absl::GetFlag(FLAGS_max_fail_examples);

  std::size_t total = 0;
  std::size_t pass = 0;
  std::size_t skip = 0;
  std::size_t fail = 0;

  std::cout << "compiler_v2 conformance run (M3-scope)\n";
  for (const auto& path : paths) {
    FileTally t = RunFile(path, compiler, engine, max_examples);
    PrintTally(t);
    total += t.total;
    pass += t.pass;
    skip += t.skip;
    fail += t.fail;
  }
  std::cout << absl::StrCat("\nsummary: total=", total, "  pass=", pass,
                            "  skip=", skip, "  fail=", fail, "\n");
  return 0;
}
