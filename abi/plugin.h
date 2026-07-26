// `Plugin` — the self-describing wasm plugin artifact.
//
// A plugin is a Component-Model `.wasm` binary carrying its own CEL
// function declarations in an embedded `cel.fns` custom section (the
// verbatim `.celfn` declaration text the plugin was built from; see
// doc/implementation-plan/rewrite/m35-plugin-ergonomics.md §3.1/§4).
// `Plugin::Load` parses and validates that section, so every `Plugin`
// is self-describing by construction — the declarations provably come
// from the deployed artifact, never from a hand-maintained C++ mirror
// that can drift.
//
// Both `Compiler::Builder::Use` and `Engine::Use` take
// `const Plugin&`, which is why the class lives in `abi/` (below both
// `compiler/` and `eval/`).  The `abi → //compiler/celfn` dep is the
// same data-vocabulary class of edge `eval` already holds.

#ifndef CELWASM_ABI_PLUGIN_H_
#define CELWASM_ABI_PLUGIN_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"

namespace celwasm {

// Immutable after Load; safe to share across threads and register on
// any number of compilers and engines.
class Plugin {
 public:
  // Parse `plugin_bytes`' embedded `cel.fns` section — the ONLY
  // way to construct a Plugin; every Plugin is a
  // self-describing artifact by construction.
  // InvalidArgument on: empty bytes; not a Component-Model binary
  // (message flags the core-module case); missing `cel.fns` section
  // (message points at cel_wasm_plugin / `cel embed-decls`);
  // malformed section framing; non-UTF-8 text; `.celfn` parse
  // failure (line+col preserved); any decl whose backend is not
  // `@plugin.` (names the decl); zero declarations.
  static absl::StatusOr<Plugin> Load(absl::Span<const uint8_t> plugin_bytes);

  // Decls + wit_interface, ready for Compiler::Builder::Use /
  // Engine::Use registration.
  const FunctionLibrary& library() const {
    return library_;
  }
  // = library().decls().
  absl::Span<const CelfnDecl> decls() const {
    return absl::MakeConstSpan(library_.decls());
  }
  // Verbatim declaration text of the embedded `cel.fns` section.
  absl::string_view celfn_source() const {
    return celfn_source_;
  }
  // The WIT interface the plugin exports its functions under —
  // always `cel:<module>/fns@0.1.0`, derived from the declaration
  // text's `Module` directive (fallback module `customfn`); e.g.
  // "cel:scorer/fns@0.1.0".  See DeriveWitInterface.
  const std::string& wit_interface() const {
    return library_.wit_interface();
  }
  // Owned copy of the wasm bytes, `cel.fns` section included.
  absl::Span<const uint8_t> bytes() const {
    return absl::MakeConstSpan(bytes_);
  }
  // SHA-256(bytes ‖ celfn_source) — content identity for embedder
  // bookkeeping and Plan-time diagnostics.  Not enforced anywhere
  // today (m35-plugin-ergonomics.md §3.4).
  const std::array<uint8_t, 32>& hash() const {
    return hash_;
  }
  // hash() as 64 lowercase hex characters.
  std::string hash_hex() const;

 private:
  Plugin() = default;
  std::vector<uint8_t> bytes_;
  std::string celfn_source_;
  FunctionLibrary library_;
  std::array<uint8_t, 32> hash_{};
};

}  // namespace celwasm

#endif  // CELWASM_ABI_PLUGIN_H_
