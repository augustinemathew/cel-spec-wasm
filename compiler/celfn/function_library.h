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
// m13-custom-fns.md, extended by §6 of m24-foreign-fn-component-backend.md
// (the plugin author surface adds `type` and `optional<T>` to
// the declarable matrix; existing host backend does not yet
// admit either — see m24 §6 for the type-to-WIT-to-C++ mapping).
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
    kType,      // CEL `type` (the type-of-types).  m24 §6: WIT `string`,
                // C++ `std::string` (the type name).  Only reachable
                // through `kPlugin` decls today.
    kOptional,  // CEL `optional<T>`.  m24 §6: WIT `option<wit T>`, C++
                // `std::optional<C++ T>`.  Element type in
                // `optional_element[0]`.  Only reachable through
                // `kPlugin` decls; the @host adapter has no
                // canonical spelling.
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
  // For kOptional: single-element vector holding the wrapped type.
  // Empty for non-kOptional.
  std::vector<CelfnType> optional_element;

  // Synthesises the argkind slug used in overload-ids per §3.6.
  // E.g. `int` → "int", `proto(acme.User)` → "message_acme_User",
  // `list<int>` → "list_int", `type` → "type",
  // `optional<int>` → "optional_int".
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
    kHost,              // `@host.` prefix
    kCelDefined,        // `@native.` prefix, has body
    kPlugin,      // `@plugin.` prefix — m24 §3 plugin-backed fn —
                // dispatched as a host callback (module_name =
                // "cel_fn"), but marshaled through a per-fn typed
                // WIT export of the Component-Model plugin
                // registered via `Engine::AddPlugin(bytes, lib)`.
                // Admits protos (as serialized bytes, m24 §8);
                // admits `type` and `optional<T>` per m24 §6.
  };

  Backend backend = Backend::kHost;
  // Plain function name as written ("allow" / "is_number" / …).
  std::string fn_name;
  // Wasm import-module name.
  //   kHost / kPlugin: always "cel_fn"
  //   kCelDefined:               the file's `Module foo;` directive name
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
  // Optional WIT interface name (e.g. `cel:customfn/fns@0.1.0`).
  // When non-empty, `Engine::AddPlugin` looks up each decl's
  // export inside this interface instance rather than at the
  // plugin's top level.  Pure-WAT plugins from
  // `plugin_dispatch_test` leave it empty and the engine
  // does the top-level lookup (m24 §3.5 v1 path).
  const std::string& wit_interface() const {
    return wit_interface_;
  }

  class Builder {
   public:
    // Wasm module name for CEL-defined functions in this library.
    // Required iff AddCelDefined() is called at least once.  When
    // unset and no CEL-defined decls are added, the resulting
    // library has no Module directive.
    Builder& SetModuleName(absl::string_view module_name);

    // WIT interface name the kPlugin decls live under in
    // the embedded plugin.  Format: `<pkg-ns>:<pkg-name>/<iface>
    // @<version>` (the `cel_wasm_plugin` macro produces plugins
    // exporting `cel:<module>/fns@0.1.0` by default; the embedder
    // calls this with the matching string).  When empty, the engine
    // does a top-level export lookup (v1 inline-WAT path).
    Builder& SetWitInterface(absl::string_view wit_interface);

    // Add a host-backed declaration.  Embedder C++ provides the impl
    // at Plan time via RuntimeBindings::AddFunction(overload_id, …).
    Builder& AddHost(absl::string_view fn_name, CelfnType return_type,
                     std::vector<CelfnParam> params);

    // Add a plugin-backed declaration (a sandboxed Component-Model
    // wasm export).  Dispatch is via
    // the `cel_fn` host-callback path (same as kHost — see m24 §2);
    // marshaling is per-fn typed WIT + a generated codec (m24 §4–§7).
    // The embedder supplies the plugin bytes at Plan time via
    // `Engine::AddPlugin(plugin_bytes, lib)`.
    //
    // Admits `proto(...)` arguments and returns — they cross as
    // serialized bytes (m24 §8).  Admits `type` and `optional<T>` per
    // m24 §6.
    Builder& AddPlugin(absl::string_view fn_name,
                                 CelfnType return_type,
                                 std::vector<CelfnParam> params);

    // Add a CEL-defined function (body is a CEL expression).
    // celwasmc compiles the body into the wasm module named by
    // SetModuleName().  The body string is taken verbatim — no
    // surrounding whitespace stripping; cel-cpp's parser handles it.
    Builder& AddCelDefined(absl::string_view fn_name, CelfnType return_type,
                           std::vector<CelfnParam> params,
                           absl::string_view body);

    // Validate + finalise.  Validations applied:
    //
    //   - `Module` set iff any kCelDefined decl present.
    //   - No two decls share an overload-id.
    //   - `this` modifier only on the first param.
    //   - kPlugin decls reject `optional<T>` / `type`
    //     return + parameter shapes (m24 §6).
    //
    // Returns InvalidArgument on validation failure with the
    // offending decl identified in the message.
    absl::StatusOr<FunctionLibrary> Build();

   private:
    std::string module_name_;
    std::string wit_interface_;
    std::vector<CelfnDecl> decls_;
  };

 private:
  std::string module_name_;
  std::string wit_interface_;
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
