// `cel embed-decls` subcommand — embed a plugin's `.idl` declaration
// text into its Component-Model `.wasm` binary as the `cel.fns`
// custom section, making the artifact self-describing
// (doc/implementation-plan/rewrite/m35-plugin-ergonomics.md §4).
// `Plugin::Load` parses the section back out; the `cel_wasm_plugin`
// Bazel macro runs this tool as its final build step, and it also
// runs standalone to retrofit an existing component.
//
// Sibling of RunGenerate in `run_generate.h`.

#ifndef CELWASM_TOOLS_CEL_RUN_EMBED_DECLS_H_
#define CELWASM_TOOLS_CEL_RUN_EMBED_DECLS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm::tools::cel {

struct EmbedDeclsOptions {
  std::string plugin_path;  // --plugin: input Component-Model .wasm
  std::string idl_path;     // --idl: the .idl declaration text
  std::string out_path;     // --out: output .wasm path
};

// The pure core: validates, then returns `plugin_bytes` with a
// `cel.fns` custom section carrying `idl_text` verbatim appended at
// component top level.  Deterministic — byte-identical output for
// identical inputs.
//
// Validation order (m35-plugin-ergonomics.md §3.4 row 1); every
// failure is InvalidArgument with message prefix `cel embed-decls: `:
//   1. `plugin_bytes` is a Component-Model component (a core wasm
//      module gets a distinct message naming the core-module case).
//   2. `idl_text` parses (ParseCelfnSource; line+col preserved).
//   3. Every decl is `@plugin.`-backed (the offender is named).
//   4. No pre-existing `cel.fns` section.
//
// A zero-decl idl is admitted here — the ≥1-decl requirement is
// `Plugin::Load`'s row of the §3.4 contract, not the embed tool's.
ABSL_MUST_USE_RESULT absl::StatusOr<std::vector<uint8_t>> EmbedDecls(
    absl::Span<const uint8_t> plugin_bytes, absl::string_view idl_text);

// CLI wrapper: reads --plugin and --idl, runs EmbedDecls, writes
// --out.  0 on success, 2 on usage error, 1 on any other failure.
// Diagnostics go to stderr.
int RunEmbedDecls(const EmbedDeclsOptions& opts);

}  // namespace celwasm::tools::cel

#endif  // CELWASM_TOOLS_CEL_RUN_EMBED_DECLS_H_
