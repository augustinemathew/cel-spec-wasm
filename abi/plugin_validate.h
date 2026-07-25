// Shared validation for the two phases that vet a plugin binary and
// its `.celfn` declarations: `Plugin::Load` (abi/plugin.cc) and
// `cel embed-decls` (tools/cel/run_embed_decls.cc).  Both phases
// deliberately re-run these checks (see doc/implementation-plan/
// rewrite/m35-plugin-ergonomics.md §3.4 — each phase rejects bad
// input at its own boundary), and each phase owns its message
// wording: the helpers take the caller's message text / prefix so
// the pinned per-phase diagnostics stay byte-identical.

#ifndef CELWASM_ABI_PLUGIN_VALIDATE_H_
#define CELWASM_ABI_PLUGIN_VALIDATE_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"

namespace celwasm {

// Requires `bytes` to classify as a Component-Model component
// (`ClassifyWasmBinary`).  Returns InvalidArgument with
// `core_module_message` when the bytes are a core wasm module (the
// wrong-artifact case gets its own text) and `bad_preamble_message`
// when they are not a wasm binary at all; OK otherwise.  The caller
// owns both message texts.
ABSL_MUST_USE_RESULT absl::Status RequireComponentLayer(
    absl::Span<const uint8_t> bytes, absl::string_view core_module_message,
    absl::string_view bad_preamble_message);

// Requires every declaration in `lib` to be `@plugin.`-backed.  On
// the first violation returns InvalidArgument with the message
//   `<prefix>decl \`<fn_name>\` is @<backend>.-backed — every
//    declaration <clause> must be @plugin.`
// (backend spelled via `BackendPrefix`); OK otherwise, including for
// an empty library — the at-least-one-decl policy differs per phase
// and stays with the caller.
ABSL_MUST_USE_RESULT absl::Status RequireAllPluginBacked(
    const FunctionLibrary& lib, absl::string_view prefix,
    absl::string_view clause);

}  // namespace celwasm

#endif  // CELWASM_ABI_PLUGIN_VALIDATE_H_
