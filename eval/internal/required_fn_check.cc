#include "eval/internal/required_fn_check.h"

#include <map>
#include <string>

#include "abi/cel_abi.pb.h"
#include "abi/celfn_wire.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "eval/internal/wasmtime_engine_state.h"

namespace celwasm {

namespace {

using ::celwasm::abi::RequiredFunction;

// Full recursive signature compare between a Program's row and a
// registered decl's wire spelling: is_receiver, param count, each
// param type, return type (TypeEquals — protos by FQN).
bool SignaturesAgree(const RequiredFunction& row,
                     const RequiredFunction& registered) {
  if (row.is_receiver() != registered.is_receiver()) return false;
  if (row.param_types_size() != registered.param_types_size()) return false;
  for (int i = 0; i < row.param_types_size(); ++i) {
    if (!TypeEquals(row.param_types(i), registered.param_types(i))) {
      return false;
    }
  }
  return TypeEquals(row.return_type(), registered.return_type());
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
    return absl::FailedPreconditionError(
        absl::StrCat("Engine::Plan: program requires host function `",
                     row.overload_id(), "` (`", RenderSignature(row),
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

namespace {

// Wire value 2 of `abi.RequiredFunction.backend` is the retired
// wasm-component plugin backend.  Spelled numerically (not via the
// generated enum member) so this arm keeps compiling — and keeps
// rejecting stale Programs, whose decoded rows retain the value
// through proto3's open-enum semantics — after the PLUGIN member is
// deleted from abi/cel_abi.proto.  Once no Program in circulation
// can carry backend value 2, this constant and its switch arm are
// dead and should be deleted.
constexpr auto kRetiredPluginBackend =
    static_cast<RequiredFunction::Backend>(2);

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::Status CheckRequiredFunctions(
    const celwasm::abi::CelAbi& abi,
    const std::map<std::string, RegisteredHostCallback>& host_callbacks) {
  for (const RequiredFunction& row : abi.required_functions()) {
    switch (row.backend()) {
      case RequiredFunction::HOST: {
        if (auto s = CheckHostRow(row, host_callbacks); !s.ok()) return s;
        break;
      }
      case kRetiredPluginBackend:
        // A plugin-backed Program can never run on this engine —
        // fail loudly here, naming the row, instead of surfacing an
        // opaque `unknown import: cel_fn.<id>` at wasmtime link time.
        return absl::FailedPreconditionError(absl::StrCat(
            "Engine::Plan: program requires function `", row.overload_id(),
            "` (`", RenderSignature(row),
            "`) via the removed wasm-component plugin backend; this engine "
            "supports only host-backed custom functions — recompile the "
            "program declaring the function with the `@host.` backend"));
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
