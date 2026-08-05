#ifndef CELWASM_COMPILER_CELFN_FUNCTION_LIBRARY_H_
#define CELWASM_COMPILER_CELFN_FUNCTION_LIBRARY_H_

// `FunctionLibrary` — the embedder-facing collection of custom CEL
// function declarations.  Plug into
// `Compiler::Builder::DeclareFunctions(lib)` to make the declared
// functions visible to cel-cpp's type checker and to the codegen
// layer's `OverloadTable`.
//
// **Construction**.  The only public path is `FunctionLibrary::Builder`.
// Three reasons to use it:
//
//   - Programmatic registration in C++ tests or embedder code with no
//     `.celfn` source available.
//   - Library composition — embedders that compute their declarations
//     from runtime data (e.g. a policy schema service) build the
//     Library directly without going through a file.
//   - The "single attach-point" property — exactly one type plugs
//     into `Compiler::Builder`; tools that load from files do so by
//     wrapping `ParseCelfnSource` (below) which itself uses the
//     Builder internally.
//
// **`.celfn` file loading** is an orthogonal concern handled by
// `ParseCelfnSource(string_view) → FunctionLibrary`.  CLI tools
// (celwasmc, celfnc) read the file bytes themselves and feed them
// to that function.  The Library class does not perform file I/O.
//
// See `doc/implementation-plan/rewrite/m13-custom-fns.md` for the
// design + grammar.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "shared/type.h"

namespace celwasm {

// Synthesises the argkind slug used in overload-ids per
// m13-custom-fns.md §3.6, over the one type vocabulary
// (shared/type.h).  E.g. `int` → "int", `proto(acme.User)` /
// `CelType::Message("acme.User")` → "message_acme_User",
// `list<int>` → "list_int", `type` → "type",
// `optional<int>` → "optional_int".  Total over every representable
// kind; `kUnknown` (the default-constructed sentinel) is a
// builder-invariant violation and CHECK-fails.
std::string ArgkindSlug(const CelType& type);

struct CelfnParam {
  // `this` modifier on the first param → method-style dispatch.
  bool is_receiver = false;
  CelType type;
  std::string name;
};

struct CelfnDecl {
  enum class Backend : uint8_t {
    kHost,  // `@host.` prefix — a C++ callback the embedder binds
            // at Plan time.  The only backend.
  };

  Backend backend = Backend::kHost;
  // Plain function name as written ("allow" / "is_number" / …).
  std::string fn_name;
  // Wasm import-module name.  Always "cel_fn".
  std::string module_name;
  // Synthesised: `<fn_name>_<argkind>_<argkind>…`.
  std::string overload_id;
  // num_args = params.size() + 1 (the out_slot).
  uint8_t num_args = 0;
  bool is_receiver = false;
  std::vector<CelfnParam> params;
  CelType return_type;
};

// The `@<backend>.` source spelling of a decl's backend (`@host.`),
// for diagnostics.  THE spelling helper for every surface that names
// a decl's backend in an error message — do not re-spell it per call
// site.
absl::string_view BackendPrefix(CelfnDecl::Backend backend);

// Embedder-facing collection of custom CEL function declarations.
// Constructed via Builder — no other public construction path.
class FunctionLibrary {
 public:
  FunctionLibrary() = default;
  FunctionLibrary(const FunctionLibrary&) = default;
  FunctionLibrary(FunctionLibrary&&) = default;
  FunctionLibrary& operator=(const FunctionLibrary&) = default;
  FunctionLibrary& operator=(FunctionLibrary&&) = default;

  // Read accessors.
  const std::string& module_name() const {
    return module_name_;
  }
  const std::vector<CelfnDecl>& decls() const {
    return decls_;
  }

  class Builder {
   public:
    // Optional library module name (the file's `Module foo;`
    // directive).  Purely descriptive today — host-backed decls
    // always dispatch through the "cel_fn" wasm import module.
    Builder& SetModuleName(absl::string_view module_name);

    // Add a host-backed declaration.  Embedder C++ provides the impl
    // at Plan time via RuntimeBindings::AddFunction(overload_id, …).
    Builder& AddHost(absl::string_view fn_name, CelType return_type,
                     std::vector<CelfnParam> params);

    // Validate + finalise.  Validations applied:
    //
    //   - No two decls share an overload-id.
    //   - `this` modifier only on the first param.
    //   - langdef map-key restriction on every return / param type.
    //
    // Returns InvalidArgument on validation failure with the
    // offending decl identified in the message.
    absl::StatusOr<FunctionLibrary> Build();

   private:
    std::string module_name_;
    std::vector<CelfnDecl> decls_;
  };

 private:
  std::string module_name_;
  std::vector<CelfnDecl> decls_;
};

// Parse a `.celfn` source string into a FunctionLibrary.  Used by
// `celwasmc` and `celfnc` (the file-loading CLI tools).  Embedders
// constructing libraries programmatically use `FunctionLibrary::Builder`
// directly; this function is a thin convenience over the Builder that
// drives it from a parsed ANTLR4 tree.
//
// Returns InvalidArgument with line + column on any grammar or
// semantic-validation failure.
absl::StatusOr<FunctionLibrary> ParseCelfnSource(absl::string_view source);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CELFN_FUNCTION_LIBRARY_H_
