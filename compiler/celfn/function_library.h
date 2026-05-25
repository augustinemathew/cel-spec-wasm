#ifndef CELWASM_COMPILER_CELFN_FUNCTION_LIBRARY_H_
#define CELWASM_COMPILER_CELFN_FUNCTION_LIBRARY_H_

// `FunctionLibrary` — the embedder-facing collection of custom CEL
// function declarations.  Plug into `Compiler::Builder::AddLibrary(lib)`
// to make the declared functions visible to cel-cpp's type checker
// and to the codegen layer's `OverloadTable`.
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

namespace celwasm {

// CEL type as it appears in a custom-fn signature.  Mirrors §3.6 of
// m13-custom-fns.md.
struct CelfnType {
  enum class Kind : uint8_t {
    kBool,
    kInt,
    kUint,
    kDouble,
    kString,
    kBytes,
    kNull,
    kDuration,
    kTimestamp,
    kList,
    kMap,
    kProto,
  };

  Kind kind = Kind::kBool;
  // For kProto: fully-qualified message name ("acme.User").  Empty
  // otherwise.
  std::string proto_fqn;
  // For kList: single-element vector holding the element type.
  // Empty for non-kList.
  std::vector<CelfnType> list_element;
  // For kMap: two-element vector [key, value].  Empty for non-kMap.
  std::vector<CelfnType> map_kv;

  // Synthesises the argkind slug used in overload-ids per §3.6.
  // E.g. `int` → "int", `proto(acme.User)` → "message_acme_User",
  // `list<int>` → "list_int".
  std::string Argkind() const;
};

struct CelfnParam {
  // `this` modifier on the first param → method-style dispatch.
  bool is_receiver = false;
  CelfnType type;
  std::string name;
};

struct CelfnDecl {
  enum class Backend : uint8_t {
    kHost,        // `@host.` prefix
    kForeign,     // `<alias>.` prefix
    kCelDefined,  // body, no prefix
  };

  Backend backend = Backend::kHost;
  // Plain function name as written ("allow" / "is_number" / …).
  std::string fn_name;
  // Wasm import-module name.
  //   kHost:       always "cel_fn"
  //   kForeign:    the alias from `<alias>.<fnname>`
  //   kCelDefined: the file's `Module foo;` directive name
  std::string module_name;
  // Synthesised: `<fn_name>_<argkind>_<argkind>…`.
  std::string overload_id;
  // num_args = params.size() + 1 (the out_slot).
  uint8_t num_args = 0;
  bool is_receiver = false;
  std::vector<CelfnParam> params;
  CelfnType return_type;
  // For kCelDefined only: the raw CEL expression source.
  std::string body;
};

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
  const std::vector<std::string>& foreign_aliases() const {
    return foreign_aliases_;
  }

  class Builder {
   public:
    // Wasm module name for CEL-defined functions in this library.
    // Required iff AddCelDefined() is called at least once.  When
    // unset and no CEL-defined decls are added, the resulting
    // library has no Module directive.
    Builder& SetModuleName(absl::string_view module_name);

    // Add a host-backed declaration.  Embedder C++ provides the impl
    // at Plan time via RuntimeBindings::AddFunction(overload_id, …).
    Builder& AddHost(absl::string_view fn_name, CelfnType return_type,
                     std::vector<CelfnParam> params);

    // Add a foreign-wasm-backed declaration.  `alias` is the wasm
    // import-module name; the embedder supplies the instance at
    // Plan time via RuntimeBindings::AddModule(alias, …).
    Builder& AddForeign(absl::string_view alias, absl::string_view fn_name,
                        CelfnType return_type, std::vector<CelfnParam> params);

    // Add a CEL-defined function (body is a CEL expression).
    // celwasmc compiles the body into the wasm module named by
    // SetModuleName().  The body string is taken verbatim — no
    // surrounding whitespace stripping; cel-cpp's parser handles it.
    Builder& AddCelDefined(absl::string_view fn_name, CelfnType return_type,
                           std::vector<CelfnParam> params,
                           absl::string_view body);

    // Validate + finalise.  Validations applied (see §3.3 + §4.5.1
    // of m13-custom-fns.md):
    //
    //   - `Module` set iff any kCelDefined decl present.
    //   - No two decls share an overload-id.
    //   - Foreign alias does not collide with `Module` directive.
    //   - `host` not used as a foreign alias.
    //   - `this` modifier only on the first param.
    //   - No foreign decl mentions `proto(...)` (v1 cross-foreign-
    //     boundary constraint).
    //
    // Returns InvalidArgument on validation failure with the
    // offending decl identified in the message.
    absl::StatusOr<FunctionLibrary> Build();

   private:
    std::string module_name_;
    std::vector<CelfnDecl> decls_;
    std::vector<std::string> foreign_aliases_;
  };

 private:
  std::string module_name_;
  std::vector<CelfnDecl> decls_;
  std::vector<std::string> foreign_aliases_;
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
