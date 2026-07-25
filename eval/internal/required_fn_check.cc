#include "eval/internal/required_fn_check.h"

#include <map>
#include <string>

#include "abi/cel_abi.pb.h"
#include "abi/celfn_wire.h"
#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "eval/internal/wasmtime_engine_state.h"

namespace celwasm {

namespace {

using ::celwasm::abi::RequiredFunction;

// Renders a registered plugin's content identity for the §2
// signature-mismatch message: the first 12 lowercase hex chars of
// its SHA-256 (`Plugin::hash()`), e.g. `hash 3f9a2c1b04de`.  A
// legacy `AddPlugin(bytes, lib)` registration has no Plugin object
// and an all-zero hash — rendering 12 zeros would look like a real
// digest, so it renders as prose instead.
std::string RenderPluginHash(const RegisteredPlugin& reg) {
  bool all_zero = true;
  for (uint8_t b : reg.hash) {
    if (b != 0) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) {
    return "hash unavailable; registered via AddPlugin";
  }
  const absl::string_view prefix(reinterpret_cast<const char*>(reg.hash.data()),
                                 6);
  return absl::StrCat("hash ", absl::BytesToHexString(prefix));
}

// The registered `kPlugin` decl with this overload-id, plus its
// owning registry entry (for the hash in the mismatch message).
// Overload-ids are unique across the registry (enforced at Use /
// AddPlugin registration), so at most one decl can match.
struct FoundPluginDecl {
  const RegisteredPlugin* plugin = nullptr;
  const CelfnDecl* decl = nullptr;
};

FoundPluginDecl FindPluginDecl(absl::Span<const RegisteredPlugin> registry,
                               absl::string_view overload_id) {
  for (const RegisteredPlugin& reg : registry) {
    for (const CelfnDecl& decl : reg.library.decls()) {
      if (decl.backend != CelfnDecl::Backend::kPlugin) continue;
      if (decl.overload_id == overload_id) return {&reg, &decl};
    }
  }
  return {};
}

// Full recursive signature compare between a Program's row and a
// registered decl's wire spelling: is_receiver, param count, each
// param type, return type (FnTypeEquals — protos by FQN).
bool SignaturesAgree(const RequiredFunction& row,
                     const RequiredFunction& registered) {
  if (row.is_receiver() != registered.is_receiver()) return false;
  if (row.param_types_size() != registered.param_types_size()) return false;
  for (int i = 0; i < row.param_types_size(); ++i) {
    if (!FnTypeEquals(row.param_types(i), registered.param_types(i))) {
      return false;
    }
  }
  return FnTypeEquals(row.return_type(), registered.return_type());
}

// One PLUGIN row: registry lookup + full signature compare.  Message
// shapes frozen in m35-plugin-ergonomics.md §2.
absl::Status CheckPluginRow(const RequiredFunction& row,
                            absl::Span<const RegisteredPlugin> registry) {
  const FoundPluginDecl found = FindPluginDecl(registry, row.overload_id());
  if (found.decl == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Engine::Plan: program requires plugin function `", row.overload_id(),
        "` (`", RenderSignature(row),
        "`) but no registered plugin declares it; register the providing "
        "plugin with Engine::Use before Plan"));
  }
  const RequiredFunction registered = RequiredFunctionFromDecl(*found.decl);
  if (!SignaturesAgree(row, registered)) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Engine::Plan: program requires plugin function `", row.overload_id(),
        "` with signature `", RenderSignature(row),
        "` but the registered plugin (", RenderPluginHash(*found.plugin),
        ") declares `", RenderSignature(registered),
        "`; signatures must match exactly — recompile the program or rebuild "
        "the plugin"));
  }
  return absl::OkStatus();
}

// One HOST row: callback lookup, wasm-arity check, and — only for
// `Engine::BindFunction` registrations, which capture the parsed
// decl — the full recursive type compare.  Missing/arity message
// shapes frozen in m35-plugin-ergonomics.md §5.3.
absl::Status CheckHostRow(
    const RequiredFunction& row,
    const std::map<std::string, RegisteredHostCallback>& host_callbacks) {
  const auto it = host_callbacks.find(row.overload_id());
  if (it == host_callbacks.end()) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Engine::Plan: program requires host function `", row.overload_id(),
        "` (`", RenderSignature(row),
        "`) but none is registered; call Engine::BindFunction (or "
        "AddFunction) before Plan"));
  }
  const RegisteredHostCallback& registered = it->second;
  const int wasm_arity = row.param_types_size() + 1;
  if (wasm_arity != static_cast<int>(registered.num_args)) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Engine::Plan: program requires host function `", row.overload_id(),
        "` with wasm arity ", wasm_arity, " but it was registered with arity ",
        static_cast<int>(registered.num_args)));
  }
  if (registered.decl_signature.has_value() &&
      !SignaturesAgree(row, *registered.decl_signature)) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Engine::Plan: program requires host function `", row.overload_id(),
        "` with signature `", RenderSignature(row),
        "` but Engine::BindFunction registered `",
        RenderSignature(*registered.decl_signature),
        "`; signatures must match exactly — recompile the program or fix the "
        "registration"));
  }
  return absl::OkStatus();
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::Status CheckRequiredFunctions(
    const celwasm::abi::CelAbi& abi,
    const std::map<std::string, RegisteredHostCallback>& host_callbacks,
    absl::Span<const RegisteredPlugin> plugin_registry) {
  for (const RequiredFunction& row : abi.required_functions()) {
    switch (row.backend()) {
      case RequiredFunction::HOST: {
        if (auto s = CheckHostRow(row, host_callbacks); !s.ok()) return s;
        break;
      }
      case RequiredFunction::PLUGIN: {
        if (auto s = CheckPluginRow(row, plugin_registry); !s.ok()) return s;
        break;
      }
      default:
        // Open-set wire data: rows stamped by a future compiler with
        // a backend this engine doesn't know are skipped, never
        // rejected (the FormatDirective precedent) — if the import
        // ends up unbound, wasmtime's link error still fires loudly.
        break;
    }
  }
  return absl::OkStatus();
}

}  // namespace celwasm
