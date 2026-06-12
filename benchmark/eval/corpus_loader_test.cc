// Tests for the corpus loader (benchmark/eval/corpus_loader.{h,cc}).
//
// Per CLAUDE.md's TDD discipline: cases were written before the
// implementation, each `GTEST_SKIP`'d behind the matching missing
// piece, and un-skipped as the loader filled in.  The remaining
// shape mirrors DESIGN.md §6.3 (validation rules) one-for-one.

#include "benchmark/eval/corpus_loader.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/match.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celbench {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// ---------- helpers ----------------------------------------------------------

// Writes `content` to a temp file whose basename is `basename` (so
// we can exercise the "filename surface match" rule).  The temp
// directory is fresh per call so multiple files don't trample each
// other when a test wants more than one.
std::string WriteTempYaml(absl::string_view basename,
                          absl::string_view content) {
  // mkdtemp template needs a writeable buffer ending in XXXXXX.
  char tmpl[] = "/tmp/celbench_corpus_XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  EXPECT_NE(dir, nullptr);
  std::string path = std::string(dir) + "/" + std::string(basename);
  std::ofstream out(path);
  out << content;
  out.close();
  return path;
}

// Path to a committed corpus YAML, relative to the bazel runfiles
// root.  When run via `bazel test`, the working directory IS the
// runfiles root, so the relative path works directly.
std::string CorpusPath(absl::string_view file) {
  return "benchmark/eval/corpus/" + std::string(file);
}

// ---------- happy paths ------------------------------------------------------

TEST(CorpusLoader, LoadsArithmeticYaml) {
  std::vector<std::string> paths = {CorpusPath("arithmetic.yaml")};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());

  // arithmetic.yaml ships 15 cells at time of writing; assert at
  // least the lower bound so this stays robust to corpus growth
  // while still catching "loaded 0 cells" regressions.
  EXPECT_GE(cells->size(), 15u);

  // Stable order: (surface, id) — every cell's surface is
  // "arithmetic", so ids are alphabetical within.
  for (const auto& c : *cells) {
    EXPECT_EQ(c.surface, "arithmetic");
  }
  for (size_t i = 1; i < cells->size(); ++i) {
    EXPECT_LT((*cells)[i - 1].id, (*cells)[i].id);
  }

  // Spot-check a known cell — intAdd2: `a + b` with a=1, b=1, expects 2
  // (length-sweep cells bind every var to 1 so the 1000-term intMul
  // chain doesn't overflow int64; see arithmetic.yaml header).
  const Cell* add2 = nullptr;
  for (const auto& c : *cells) {
    if (c.id == "intAdd2") {
      add2 = &c;
      break;
    }
  }
  ASSERT_NE(add2, nullptr);
  EXPECT_EQ(add2->source, "a + b");
  EXPECT_EQ(add2->expected.kind, CelValueLiteral::Kind::kInt);
  EXPECT_EQ(add2->expected.int_value, 2);
  EXPECT_EQ(add2->activation.size(), 2u);
}

TEST(CorpusLoader, LoadsBothFilesAndJoinsByStableOrder) {
  std::vector<std::string> paths = {
      CorpusPath("arithmetic.yaml"),
      CorpusPath("comparisons.yaml"),
  };
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());

  // Both surfaces present.
  bool saw_arith = false;
  bool saw_comp = false;
  for (const auto& c : *cells) {
    if (c.surface == "arithmetic") saw_arith = true;
    if (c.surface == "comparisons") saw_comp = true;
  }
  EXPECT_TRUE(saw_arith);
  EXPECT_TRUE(saw_comp);

  // (surface, id) globally sorted — surface first, then id within.
  for (size_t i = 1; i < cells->size(); ++i) {
    const auto& a = (*cells)[i - 1];
    const auto& b = (*cells)[i];
    if (a.surface == b.surface) {
      EXPECT_LT(a.id, b.id);
    } else {
      EXPECT_LT(a.surface, b.surface);
    }
  }

  // (surface, id) is unique across all files.
  for (size_t i = 1; i < cells->size(); ++i) {
    EXPECT_FALSE((*cells)[i - 1].surface == (*cells)[i].surface &&
                 (*cells)[i - 1].id == (*cells)[i].id);
  }
}

// ---------- validation ------------------------------------------------------

TEST(CorpusLoader, RejectsDuplicateId) {
  // Two cells with the same id within one file.
  const char* kYaml = R"YAML(
schema_version: 1
surface: dupes
cells:
  - id: cellA
    source: "a"
    activation: { a: { type: int, value: 1 } }
    expected: { type: int, value: 1 }
  - id: cellA
    source: "a"
    activation: { a: { type: int, value: 2 } }
    expected: { type: int, value: 2 }
)YAML";
  std::string path = WriteTempYaml("dupes.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("cellA")));
}

TEST(CorpusLoader, RejectsSurfaceMismatchWithFilename) {
  // File named arithmetic.yaml but declares surface: comparisons.
  const char* kYaml = R"YAML(
schema_version: 1
surface: comparisons
cells:
  - id: x
    source: "a"
    activation: { a: { type: int, value: 1 } }
    expected: { type: int, value: 1 }
)YAML";
  std::string path = WriteTempYaml("arithmetic.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(
      LoadCorpus(paths).status(),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("surface")));
}

TEST(CorpusLoader, RejectsIdWithWhitespace) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: badid
cells:
  - id: "int add 2"
    source: "a"
    activation: { a: { type: int, value: 1 } }
    expected: { type: int, value: 1 }
)YAML";
  std::string path = WriteTempYaml("badid.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(
      LoadCorpus(paths).status(),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("whitespace")));
}

TEST(CorpusLoader, RejectsIdWithSlash) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: badid
cells:
  - id: "int/add"
    source: "a"
    activation: { a: { type: int, value: 1 } }
    expected: { type: int, value: 1 }
)YAML";
  std::string path = WriteTempYaml("badid.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("/")));
}

TEST(CorpusLoader, RejectsEmptyId) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: badid
cells:
  - id: ""
    source: "a"
    activation: { a: { type: int, value: 1 } }
    expected: { type: int, value: 1 }
)YAML";
  std::string path = WriteTempYaml("badid.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("id")));
}

TEST(CorpusLoader, RejectsUnboundVarInSource) {
  // source: "a + b" but only `a` bound.
  const char* kYaml = R"YAML(
schema_version: 1
surface: unbound
cells:
  - id: missingB
    source: "a + b"
    activation: { a: { type: int, value: 1 } }
    expected: { type: int, value: 1 }
)YAML";
  std::string path = WriteTempYaml("unbound.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto status = LoadCorpus(paths).status();
  EXPECT_THAT(status, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(status.message()), HasSubstr("b"));
}

TEST(CorpusLoader, RejectsExtraVarInActivation) {
  // source: "a" but `a` and `b` both bound.
  const char* kYaml = R"YAML(
schema_version: 1
surface: extra
cells:
  - id: extraB
    source: "a"
    activation:
      a: { type: int, value: 1 }
      b: { type: int, value: 2 }
    expected: { type: int, value: 1 }
)YAML";
  std::string path = WriteTempYaml("extra.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto status = LoadCorpus(paths).status();
  EXPECT_THAT(status, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(std::string(status.message()), HasSubstr("b"));
}

TEST(CorpusLoader, SkipSourceCheckTagOptsOut) {
  // `a` in source but `b` bound only — would normally fail.  Tag
  // skips the check.
  const char* kYaml = R"YAML(
schema_version: 1
surface: optout
cells:
  - id: skipMe
    source: "size(a)"
    activation:
      a: { type: string, value: "hi" }
    expected: { type: int, value: 2 }
    tags: [skip-source-check]
)YAML";
  std::string path = WriteTempYaml("optout.yaml", kYaml);

  std::vector<std::string> paths = {path};
  // Without the tag the heuristic would flag `size` as an unbound
  // var; with it, the cell loads cleanly.
  EXPECT_THAT(LoadCorpus(paths).status(), IsOk());
}

TEST(CorpusLoader, UintLiteralSuffixIsNotAVariable) {
  // `1u` / `20u` are uint literals; the `u` glued to the digit is a
  // literal suffix, not an identifier — the source-variable scan
  // must not flag it as unbound.
  const char* kYaml = R"YAML(
schema_version: 1
surface: uintsfx
cells:
  - id: suffix
    source: "x in [1u,2u,20u]"
    activation:
      x: { type: uint, value: 20 }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("uintsfx.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(), IsOk());
}

TEST(CorpusLoader, RejectsMissingPath) {
  std::vector<std::string> paths = {"/nonexistent/path/foo.yaml"};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(CorpusLoader, RejectsUnknownExpectedType) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: badtype
cells:
  - id: badT
    source: "a"
    activation: { a: { type: int, value: 1 } }
    expected: { type: gronk, value: 1 }
)YAML";
  std::string path = WriteTempYaml("badtype.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("gronk")));
}

// ---------- duplicate across files (cross-file rule) ------------------------

TEST(CorpusLoader, RejectsDuplicateAcrossFiles) {
  // Same (surface, id) appearing in two files.  But surface must
  // match basename, so build one file as "x.yaml" with surface x
  // and id `dup`, and a second "y.yaml" with surface y and id
  // `dup` — that's fine (different surface).  To trigger the
  // cross-file dupe we need two files BOTH named the same surface
  // basename... which the basename-match rule prevents.  So a true
  // (surface, id) cross-file dupe can only happen if a single
  // surface is split across more than one path — not the current
  // convention, but the loader still defends against it by way of
  // the duplicate-detection map.  Emulate by giving two files the
  // same basename in different dirs.
  const char* kYamlA = R"YAML(
schema_version: 1
surface: split
cells:
  - id: only
    source: "a"
    activation: { a: { type: int, value: 1 } }
    expected: { type: int, value: 1 }
)YAML";
  const char* kYamlB = R"YAML(
schema_version: 1
surface: split
cells:
  - id: only
    source: "a"
    activation: { a: { type: int, value: 1 } }
    expected: { type: int, value: 1 }
)YAML";
  // mkdtemp gives a fresh dir per call, so the two files coexist
  // even though they share a basename.
  std::string a = WriteTempYaml("split.yaml", kYamlA);
  std::string b = WriteTempYaml("split.yaml", kYamlB);

  std::vector<std::string> paths = {a, b};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("only")));
}

// ---------- list activation entries -----------------------------------------

TEST(CorpusLoader, LoadsExplicitIntList) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: explicitInt
    source: "1 in xs"
    activation:
      xs: { type: list, elem: int, values: [1, 2, 3] }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  ASSERT_EQ(cells->size(), 1u);
  ASSERT_EQ((*cells)[0].activation.size(), 1u);
  const CelValueLiteral& xs = (*cells)[0].activation[0].value;
  EXPECT_EQ(xs.kind, CelValueLiteral::Kind::kList);
  EXPECT_EQ(xs.list_elem_kind, CelValueLiteral::Kind::kInt);
  ASSERT_EQ(xs.list_value.size(), 3u);
  EXPECT_EQ(xs.list_value[0].kind, CelValueLiteral::Kind::kInt);
  EXPECT_EQ(xs.list_value[0].int_value, 1);
  EXPECT_EQ(xs.list_value[1].int_value, 2);
  EXPECT_EQ(xs.list_value[2].int_value, 3);
}

TEST(CorpusLoader, LoadsExplicitStringList) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: explicitStr
    source: "\"a\" in xs"
    activation:
      xs: { type: list, elem: string, values: ["a", "b"] }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  const CelValueLiteral& xs = (*cells)[0].activation[0].value;
  EXPECT_EQ(xs.kind, CelValueLiteral::Kind::kList);
  EXPECT_EQ(xs.list_elem_kind, CelValueLiteral::Kind::kString);
  ASSERT_EQ(xs.list_value.size(), 2u);
  EXPECT_EQ(xs.list_value[0].kind, CelValueLiteral::Kind::kString);
  EXPECT_EQ(xs.list_value[0].string_value, "a");
  EXPECT_EQ(xs.list_value[1].string_value, "b");
}

TEST(CorpusLoader, LoadsGenRangeList) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: genRange
    source: "1 in xs"
    activation:
      xs: { type: list, elem: int, gen: { range: 1000 } }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  const CelValueLiteral& xs = (*cells)[0].activation[0].value;
  EXPECT_EQ(xs.kind, CelValueLiteral::Kind::kList);
  EXPECT_EQ(xs.list_elem_kind, CelValueLiteral::Kind::kInt);
  ASSERT_EQ(xs.list_value.size(), 1000u);
  EXPECT_EQ(xs.list_value.front().kind, CelValueLiteral::Kind::kInt);
  EXPECT_EQ(xs.list_value.front().int_value, 0);
  EXPECT_EQ(xs.list_value.back().int_value, 999);
}

TEST(CorpusLoader, LoadsGenTemplateListWithZeroPad) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: genTemplate
    source: "\"x\" in perms"
    activation:
      perms:
        type: list
        elem: string
        gen: { template: "aaaa.bbbb.cccc.perm%07d", count: 1000 }
    expected: { type: bool, value: false }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  const CelValueLiteral& perms = (*cells)[0].activation[0].value;
  EXPECT_EQ(perms.kind, CelValueLiteral::Kind::kList);
  EXPECT_EQ(perms.list_elem_kind, CelValueLiteral::Kind::kString);
  ASSERT_EQ(perms.list_value.size(), 1000u);
  EXPECT_EQ(perms.list_value.front().kind, CelValueLiteral::Kind::kString);
  EXPECT_EQ(perms.list_value.front().string_value,
            "aaaa.bbbb.cccc.perm0000000");
  EXPECT_EQ(perms.list_value.back().string_value, "aaaa.bbbb.cccc.perm0000999");
}

// Boundary: a 1-element range expands to exactly [0].
TEST(CorpusLoader, GenRangeOneExpandsToSingleZero) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: rangeOne
    source: "0 in xs"
    activation:
      xs: { type: list, elem: int, gen: { range: 1 } }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  const CelValueLiteral& xs = (*cells)[0].activation[0].value;
  ASSERT_EQ(xs.list_value.size(), 1u);
  EXPECT_EQ(xs.list_value[0].int_value, 0);
}

// Boundary: a plain `%d` directive (no zero-pad width) substitutes the
// bare index.
TEST(CorpusLoader, GenTemplatePlainPercentD) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: plainD
    source: "\"x\" in xs"
    activation:
      xs: { type: list, elem: string, gen: { template: "p%d", count: 3 } }
    expected: { type: bool, value: false }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  const CelValueLiteral& xs = (*cells)[0].activation[0].value;
  ASSERT_EQ(xs.list_value.size(), 3u);
  EXPECT_EQ(xs.list_value[0].string_value, "p0");
  EXPECT_EQ(xs.list_value[1].string_value, "p1");
  EXPECT_EQ(xs.list_value[2].string_value, "p2");
}

// Boundary: template count of 1 expands to a single entry.
TEST(CorpusLoader, GenTemplateCountOne) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: countOne
    source: "\"p0\" in xs"
    activation:
      xs: { type: list, elem: string, gen: { template: "p%d", count: 1 } }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  const CelValueLiteral& xs = (*cells)[0].activation[0].value;
  ASSERT_EQ(xs.list_value.size(), 1u);
  EXPECT_EQ(xs.list_value[0].string_value, "p0");
}

// Boundary: an explicit empty `values` list is legal — the declared
// list<T> type is still derivable from the stamped `list_elem_kind`.
TEST(CorpusLoader, EmptyExplicitValuesListIsAllowed) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: emptyList
    source: "1 in xs"
    activation:
      xs: { type: list, elem: int, values: [] }
    expected: { type: bool, value: false }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  const CelValueLiteral& xs = (*cells)[0].activation[0].value;
  EXPECT_EQ(xs.kind, CelValueLiteral::Kind::kList);
  EXPECT_EQ(xs.list_elem_kind, CelValueLiteral::Kind::kInt);
  EXPECT_TRUE(xs.list_value.empty());
}

TEST(CorpusLoader, RejectsListWithoutElem) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: noElem
    source: "1 in xs"
    activation:
      xs: { type: list, values: [1] }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("elem")));
}

TEST(CorpusLoader, RejectsListWithBothValuesAndGen) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: bothForms
    source: "1 in xs"
    activation:
      xs: { type: list, elem: int, values: [1], gen: { range: 3 } }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("exactly one of `values` / `gen`")));
}

TEST(CorpusLoader, RejectsGenWithBothRangeAndTemplate) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: bothGens
    source: "1 in xs"
    activation:
      xs:
        type: list
        elem: int
        gen: { range: 3, template: "p%d", count: 3 }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("exactly one of `range` / `template`")));
}

TEST(CorpusLoader, RejectsGenRangeZero) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: rangeZero
    source: "1 in xs"
    activation:
      xs: { type: list, elem: int, gen: { range: 0 } }
    expected: { type: bool, value: false }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr(">= 1")));
}

TEST(CorpusLoader, RejectsGenTemplateWithoutCount) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: noCount
    source: "\"x\" in xs"
    activation:
      xs: { type: list, elem: string, gen: { template: "p%d" } }
    expected: { type: bool, value: false }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("count")));
}

TEST(CorpusLoader, RejectsGenTemplateWithAbsurdWidth) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: hugeWidth
    source: "\"x\" in xs"
    activation:
      xs:
        type: list
        elem: string
        gen: { template: "p%999999999999999999999d", count: 1 }
    expected: { type: bool, value: false }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("width")));
}

TEST(CorpusLoader, RejectsListElemList) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: nestedList
    source: "1 in xs"
    activation:
      xs: { type: list, elem: list, values: [] }
    expected: { type: bool, value: false }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(
      LoadCorpus(paths).status(),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("scalar")));
}

TEST(CorpusLoader, RejectsValuesItemThatFailsElemKindParse) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: boundlist
cells:
  - id: badItem
    source: "1 in xs"
    activation:
      xs: { type: list, elem: int, values: [1, notanint] }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("boundlist.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("int")));
}

// ---------- message activation entries --------------------------------------

TEST(CorpusLoader, MessageEntryRoundTripsTypeAndTextproto) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: policy
cells:
  - id: msgVar
    source: "c.name == \"Ada\""
    activation:
      c:
        type: message
        message_type: "celwasm.testdata.Customer"
        textproto: 'name: "Ada"'
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("policy.yaml", kYaml);

  std::vector<std::string> paths = {path};
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  ASSERT_EQ((*cells)[0].activation.size(), 1u);
  const CelValueLiteral& c = (*cells)[0].activation[0].value;
  EXPECT_EQ(c.kind, CelValueLiteral::Kind::kMessage);
  EXPECT_EQ(c.message_type, "celwasm.testdata.Customer");
  EXPECT_EQ(c.string_value, "name: \"Ada\"");
}

TEST(CorpusLoader, RejectsMessageMissingMessageType) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: policy
cells:
  - id: noType
    source: "c.name == \"Ada\""
    activation:
      c: { type: message, textproto: 'name: "Ada"' }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("policy.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(
      LoadCorpus(paths).status(),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("message_type")));
}

TEST(CorpusLoader, RejectsMessageMissingTextproto) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: policy
cells:
  - id: noText
    source: "c.name == \"Ada\""
    activation:
      c: { type: message, message_type: "celwasm.testdata.Customer" }
    expected: { type: bool, value: true }
)YAML";
  std::string path = WriteTempYaml("policy.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(
      LoadCorpus(paths).status(),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("textproto")));
}

// ---------- field-selection identifiers in the source scan ------------------

// `c.name` is a field selection — the scan must not demand a binding
// for `name`.  Bound `c` only; the cell validates cleanly.  (Scalar
// binding on purpose: isolates the scanner change from message
// parsing.)
TEST(CorpusLoader, FieldSelectionIdentifierDoesNotRequireBinding) {
  const char* kYaml = R"YAML(
schema_version: 1
surface: fieldsel
cells:
  - id: dotField
    source: "c.name == \"x\""
    activation:
      c: { type: string, value: "ignored" }
    expected: { type: bool, value: false }
)YAML";
  std::string path = WriteTempYaml("fieldsel.yaml", kYaml);

  std::vector<std::string> paths = {path};
  EXPECT_THAT(LoadCorpus(paths).status(), IsOk());
}

// ---------- CanonicalForm ---------------------------------------------------

TEST(CanonicalForm, IntIsDecimal) {
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kInt;
  v.int_value = -42;
  EXPECT_EQ(CanonicalForm(v), "-42");
}

TEST(CanonicalForm, UintHasSuffix) {
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kUint;
  v.uint_value = 42;
  EXPECT_EQ(CanonicalForm(v), "42u");
}

TEST(CanonicalForm, BoolIsLowercase) {
  CelValueLiteral t;
  t.kind = CelValueLiteral::Kind::kBool;
  t.bool_value = true;
  EXPECT_EQ(CanonicalForm(t), "true");

  CelValueLiteral f;
  f.kind = CelValueLiteral::Kind::kBool;
  f.bool_value = false;
  EXPECT_EQ(CanonicalForm(f), "false");
}

TEST(CanonicalForm, StringIsQuoted) {
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kString;
  v.string_value = "hello";
  EXPECT_EQ(CanonicalForm(v), "\"hello\"");
}

TEST(CanonicalForm, NullIsLiteral) {
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kNull;
  EXPECT_EQ(CanonicalForm(v), "null");
}

TEST(CanonicalForm, DoubleUsesShortestRoundTrip) {
  // std::to_chars(double, general) produces the shortest decimal
  // round-trip — same convention `cel_convert_double_format.cc`
  // adopted (cited in the header doc).  For 3.14 the shortest
  // form is "3.14"; for 1.0 it's "1".
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kDouble;
  v.double_value = 3.14;
  EXPECT_EQ(CanonicalForm(v), "3.14");

  CelValueLiteral one;
  one.kind = CelValueLiteral::Kind::kDouble;
  one.double_value = 1.0;
  EXPECT_EQ(CanonicalForm(one), "1");
}

TEST(CanonicalForm, BytesPrefixed) {
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kBytes;
  v.string_value = "abc";
  // Distinct shape from string so a comparator that mis-tags bytes
  // as string can't accidentally pass parity.
  EXPECT_EQ(CanonicalForm(v), "b\"abc\"");
}

TEST(CanonicalForm, ListRendersBracketedElements) {
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kList;
  v.list_elem_kind = CelValueLiteral::Kind::kInt;
  for (int i = 1; i <= 3; ++i) {
    CelValueLiteral e;
    e.kind = CelValueLiteral::Kind::kInt;
    e.int_value = i;
    v.list_value.push_back(e);
  }
  EXPECT_EQ(CanonicalForm(v), "[1, 2, 3]");
}

TEST(CanonicalForm, MessageRendersTypeBracedTextproto) {
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kMessage;
  v.message_type = "celwasm.testdata.Customer";
  v.string_value = "name: \"Ada\"";
  EXPECT_EQ(CanonicalForm(v), "celwasm.testdata.Customer{name: \"Ada\"}");
}

TEST(CanonicalForm, StableAcrossInvocations) {
  CelValueLiteral v;
  v.kind = CelValueLiteral::Kind::kDouble;
  v.double_value = 1.0 / 3.0;
  EXPECT_EQ(CanonicalForm(v), CanonicalForm(v));
}

// ---------- AbbreviateForLabel ----------------------------------------------

TEST(AbbreviateForLabel, ShortPassesThrough) {
  EXPECT_EQ(AbbreviateForLabel("hello"), "hello");
  EXPECT_EQ(AbbreviateForLabel(""), "");
}

TEST(AbbreviateForLabel, ExactBoundaryPassesThrough) {
  std::string s(64, 'x');
  EXPECT_EQ(AbbreviateForLabel(s), s);
}

TEST(AbbreviateForLabel, LongTruncatesWithLength) {
  std::string s(100, 'x');
  EXPECT_EQ(AbbreviateForLabel(s), std::string(64, 'x') + "...(len=100)");
}

TEST(AbbreviateForLabel, StableAcrossInvocations) {
  std::string s(2000, 'y');
  EXPECT_EQ(AbbreviateForLabel(s), AbbreviateForLabel(s));
}

// ---------- new-surface corpus files load cleanly ----------------------------

TEST(CorpusLoader, LoadsAllCommittedSurfaces) {
  std::vector<std::string> paths = {
      CorpusPath("arithmetic.yaml"),     CorpusPath("comparisons.yaml"),
      CorpusPath("comprehensions.yaml"), CorpusPath("conversions.yaml"),
      CorpusPath("index.yaml"),          CorpusPath("lists.yaml"),
      CorpusPath("literals.yaml"),       CorpusPath("logic.yaml"),
      CorpusPath("long_strings.yaml"),   CorpusPath("maps.yaml"),
      CorpusPath("policies.yaml"),       CorpusPath("proto.yaml"),
      CorpusPath("size.yaml"),           CorpusPath("strings.yaml"),
      CorpusPath("ternary.yaml"),        CorpusPath("time.yaml"),
  };
  auto cells = LoadCorpus(paths);
  ASSERT_THAT(cells.status(), IsOk());
  // 16 surfaces; well over 150 cells once the operator×type grid
  // landed.  Lower bound only, so corpus growth doesn't churn this.
  EXPECT_GE(cells->size(), 150u);
}

}  // namespace
}  // namespace celbench
