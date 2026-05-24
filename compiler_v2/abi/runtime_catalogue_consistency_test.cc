// Tripwire: the `[codegen-helpers]` section in
// `compiler_v2/runtime/wasm_exports.txt` MUST be a set-identical
// mirror of `abi::CelRuntimeHelpers()`.
//
// Why: that text file is the single source of truth feeding the
// wasm-ld response file (`//compiler_v2/runtime:wasm_export_args`).
// If a helper lands in the catalogue without being added to the
// text file, the linker dead-strips it and `Engine::Plan`'s
// `BindAllRuntimeExports` trips on "missing export <name>" at
// instantiate time — opaque to the user.  Symmetrically, a helper
// added to the text file without a catalogue entry would still
// link but codegen would have no arity / return-shape metadata to
// emit the import, so the program would compile to a wasm module
// referencing a function nobody is calling.  Both directions are
// caught here at PR time.
//
// The `[host-only]` section of the text file (malloc/free/
// __heap_base, the public arena API, `_arena` tail-call targets,
// same-kind eq/ne fast paths) is intentionally NOT in the
// catalogue and is allowed to diverge — see file header for
// rationale.

#include <fstream>
#include <sstream>
#include <string>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "compiler_v2/abi/runtime_catalogue.h"
#include "gtest/gtest.h"

namespace celwasm::abi {
namespace {

// Path relative to the test's cwd at runtime.  The text file is
// declared as `data = ["//compiler_v2/runtime:wasm_exports.txt"]`
// in the consuming BUILD target, so bazel stages it at this path.
constexpr absl::string_view kExportsTxt =
    "compiler_v2/runtime/wasm_exports.txt";

// Section in the text file we mirror against the catalogue.
constexpr absl::string_view kCodegenSection = "codegen-helpers";

struct Sections {
  absl::flat_hash_set<std::string> codegen;
  absl::flat_hash_set<std::string> host_only;
};

// Parse the text file into its two sections.  Same grammar as the
// `wasm_export_args` genrule uses (awk in runtime/BUILD.bazel):
// strip comments + blank lines + section headers; the FIRST
// whitespace token of each remaining line is the symbol name.
Sections ParseExportsTxt(absl::string_view contents) {
  Sections out;
  absl::string_view current_section;
  for (absl::string_view raw : absl::StrSplit(contents, '\n')) {
    absl::string_view line = absl::StripAsciiWhitespace(raw);
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      current_section = line.substr(1, line.size() - 2);
      continue;
    }
    // First whitespace-delimited token = symbol.
    const auto end = line.find_first_of(" \t");
    const std::string name((end == absl::string_view::npos)
                               ? line
                               : line.substr(0, end));
    if (current_section == kCodegenSection) {
      out.codegen.emplace(name);
    } else {
      out.host_only.emplace(name);
    }
  }
  return out;
}

TEST(RuntimeCatalogueConsistency, CodegenSectionMatchesCatalogueExactly) {
  std::ifstream in{std::string(kExportsTxt)};
  ASSERT_TRUE(in.is_open())
      << "could not open " << kExportsTxt << " — confirm the test's BUILD "
      << "entry includes `data = [\"//compiler_v2/runtime:wasm_exports.txt\"]`";
  std::stringstream buf;
  buf << in.rdbuf();
  const auto sections = ParseExportsTxt(buf.str());

  absl::flat_hash_set<std::string> catalogue;
  catalogue.reserve(CelRuntimeHelpers().size());
  for (const AbiHelper& h : CelRuntimeHelpers()) {
    catalogue.emplace(h.name);
  }

  // Catalogue ⊆ text file's [codegen-helpers].  A new helper added
  // to the catalogue without the matching text-file entry would
  // be linker-dead-stripped — instantiate then traps opaquely.
  for (const std::string& name : catalogue) {
    EXPECT_TRUE(sections.codegen.contains(name))
        << "catalogue entry `cel." << name
        << "` is NOT in wasm_exports.txt's [codegen-helpers] section — "
           "add the line or remove the catalogue entry.";
  }

  // Text file's [codegen-helpers] ⊆ catalogue.  A symbol in the
  // codegen section without a catalogue row would link but codegen
  // would have no metadata to emit the import.  Forces both lists
  // to stay genuinely in sync rather than allowing a one-way
  // tolerance.
  for (const std::string& name : sections.codegen) {
    EXPECT_TRUE(catalogue.contains(name))
        << "wasm_exports.txt's [codegen-helpers] has `" << name
        << "` but it is not in abi::CelRuntimeHelpers() — either "
           "add a catalogue entry or move the symbol into the "
           "[host-only] section.";
  }

  // Cardinality match catches the case where both sides happen to
  // contain each other's elements but the per-element loops missed
  // a duplicate (defensive; std::set/flat_hash_set dedup means
  // this is mostly belt-and-braces).
  EXPECT_EQ(catalogue.size(), sections.codegen.size())
      << "catalogue size (" << catalogue.size() << ") != [codegen-helpers] "
      << "size (" << sections.codegen.size() << ")";
}

TEST(RuntimeCatalogueConsistency, HostOnlySectionIsNonEmpty) {
  // Sanity: malloc / free / __heap_base / arena_init etc. MUST live
  // somewhere.  If the [host-only] section is empty, someone has
  // accidentally merged it with [codegen-helpers] — that would
  // surface as the previous test failing with "X is not in the
  // catalogue" for malloc / free etc., but flagging it here gives
  // a more direct diagnostic.
  std::ifstream in{std::string(kExportsTxt)};
  ASSERT_TRUE(in.is_open());
  std::stringstream buf;
  buf << in.rdbuf();
  const auto sections = ParseExportsTxt(buf.str());
  EXPECT_FALSE(sections.host_only.empty())
      << "[host-only] section is empty — malloc/free/__heap_base etc. "
         "should live there.";
  EXPECT_TRUE(sections.host_only.contains("malloc"));
  EXPECT_TRUE(sections.host_only.contains("free"));
  EXPECT_TRUE(sections.host_only.contains("__heap_base"));
}

}  // namespace
}  // namespace celwasm::abi
