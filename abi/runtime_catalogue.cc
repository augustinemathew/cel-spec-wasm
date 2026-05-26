// Authoritative catalogue of every wasm import an emitted expr
// module declares.  See `runtime_catalogue.h` for the design
// rationale and the three pre-existing surfaces this consolidates.
//
// The catalogue entry type is the generated proto message
// `celwasm::abi::CelRuntimeFunction`; there is NO hand-defined POD
// struct.  The `cel`-module set is parsed once at first use from the
// embedded `CelRuntimeCatalogue` textproto, which
// `//bazel:gen_runtime_catalogue` generates from the
// `// cel:codegen-export` markers in `runtime/cel_*.{h,c}`.  Adding a
// runtime helper is therefore a single edit: mark its declaration with
// `// cel:codegen-export`.  The generator picks up name + arity +
// return shape; the linker `--export=` set and this catalogue both
// flow from the marker.
//
// The `cel_host` / `cel_env` import sets are host IMPORTS with no
// `cel_runtime.wasm` export to derive from, so they are constructed
// programmatically below as `CelRuntimeFunction` protos (the cleaner
// option than a second hand-edited textproto resource: the data is
// tiny and stays right next to the cross-checks in
// `cel_host_wasmtime.cc`).
//
// Maintenance discipline (host / env imports — still hand-maintained):
//   - Adding a host trampoline: ONE entry in `CelHostFunctionsVec()`
//     below, plus the trampoline impl in `cel_host_wasmtime.cc`.
//   - Changing a helper's arity or return shape is a breaking ABI
//     change — bump `kRuntimeAbiVersion` in the header.

#include "abi/runtime_catalogue.h"

#include <cstdint>
#include <string>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "abi/runtime_catalogue.pb.h"
#include "abi/runtime_catalogue_textproto.h"
#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "google/protobuf/text_format.h"

namespace celwasm::abi {

absl::string_view AbiModuleName(AbiModule m) {
  switch (m) {
    case AbiModule::kCelRuntime:
      return "cel";
    case AbiModule::kCelHost:
      return "cel_host";
    case AbiModule::kCelEnv:
      return "cel_env";
    case AbiModule::kCelFn:
      return "cel_fn";
  }
  ABSL_CHECK(false) << "AbiModuleName: unhandled AbiModule="
                    << static_cast<int>(m);
}

namespace {

// Builds a `CelRuntimeFunction` proto for a host import.  The two host
// namespaces (`cel_host` / `cel_env`) describe wasm IMPORTS the engine
// registers, not `cel_runtime.wasm` exports, so they have no marker to
// derive from and are constructed here.  Every host trampoline writes
// its result through an out_slot in linear memory, so `returns_i32` is
// always false.
CelRuntimeFunction MakeHostFn(absl::string_view name, CelRuntimeModule module,
                              uint32_t num_args) {
  CelRuntimeFunction fn;
  fn.set_name(std::string(name));
  fn.set_module(module);
  fn.set_num_args(num_args);
  fn.set_returns_i32(false);
  return fn;
}

// ════════════════════════════════════════════════════════════════
// `cel` namespace — pure-wasm helpers exported by cel_runtime.wasm.
// Parsed once from the embedded, generated `CelRuntimeCatalogue`
// textproto.  Function-local static → process lifetime, so the spans
// and the `string_view`s into each proto's `name()` stay valid.
// ════════════════════════════════════════════════════════════════
const std::vector<CelRuntimeFunction>& CelRuntimeHelpersVec() {
  static const absl::NoDestructor<std::vector<CelRuntimeFunction>> kHelpers([] {
    CelRuntimeCatalogue catalogue;
    ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
        std::string(RuntimeCatalogueTextproto()), &catalogue))
        << "abi: failed to parse the embedded CelRuntimeCatalogue "
           "textproto — the genrule output is malformed";
    std::vector<CelRuntimeFunction> v;
    v.reserve(catalogue.functions_size());
    for (const CelRuntimeFunction& fn : catalogue.functions()) {
      ABSL_CHECK(fn.module() == CEL) << "abi: generated catalogue row `"
                                     << fn.name() << "` has module != CEL";
      v.push_back(fn);
    }
    return v;
  }());
  return *kHelpers;
}

// ════════════════════════════════════════════════════════════════
// `cel_host` namespace — wasmtime host trampolines registered by
// `cel_host_wasmtime.cc::RegisterCelHostImports`.  Names + arities
// MUST agree with `kHostTrampolines[]` in that file; the cross-check
// loop there CHECK-fails at startup if they drift.
// ════════════════════════════════════════════════════════════════
const std::vector<CelRuntimeFunction>& CelHostFunctionsVec() {
  static const absl::NoDestructor<std::vector<CelRuntimeFunction>> kFns([] {
    return std::vector<CelRuntimeFunction>{
        MakeHostFn("cel_get_field", CEL_HOST, 4),
        MakeHostFn("cel_has_field", CEL_HOST, 4),
        MakeHostFn("cel_map_lookup", CEL_HOST, 3),
        MakeHostFn("cel_map_iter_open", CEL_HOST, 2),
        MakeHostFn("cel_list_iter_open", CEL_HOST, 2),
        MakeHostFn("cel_list_at", CEL_HOST, 3),
        MakeHostFn("cel_list_size", CEL_HOST, 2),
        MakeHostFn("cel_list_in", CEL_HOST, 3),
        MakeHostFn("cel_list_eq", CEL_HOST, 3),
        MakeHostFn("cel_list_concat", CEL_HOST, 3),
        MakeHostFn("cel_map_size", CEL_HOST, 2),
        MakeHostFn("cel_map_in", CEL_HOST, 3),
        MakeHostFn("cel_map_eq", CEL_HOST, 3),
        MakeHostFn("cel_message_eq", CEL_HOST, 3),
        MakeHostFn("cel_make_message", CEL_HOST, 2),
        MakeHostFn("cel_set_field", CEL_HOST, 3),
        MakeHostFn("resolve_message_type_name", CEL_HOST, 2),
        MakeHostFn("cel_timestamp_tz_accessor", CEL_HOST, 4),
        MakeHostFn("cel_wkt_unwrap_time", CEL_HOST, 2),
        MakeHostFn("cel_wkt_unwrap_wrapper", CEL_HOST, 3),
    };
  }());
  return *kFns;
}

// ════════════════════════════════════════════════════════════════
// `cel_env` namespace — host environment trampolines.  Currently
// just `cel_log`; reserved for future cross-cutting concerns.
// ════════════════════════════════════════════════════════════════
const std::vector<CelRuntimeFunction>& CelEnvFunctionsVec() {
  static const absl::NoDestructor<std::vector<CelRuntimeFunction>> kFns([] {
    return std::vector<CelRuntimeFunction>{
        MakeHostFn("cel_log", CEL_ENV, 4),
    };
  }());
  return *kFns;
}

// Name→helper lookups, eagerly built per namespace on first use.
// Distinct maps because `cel.cel_list_at` (the dispatcher) and
// `cel_host.cel_list_at` (the trampoline it tail-calls) share a
// name across namespaces by design.  The `string_view` keys alias
// each proto's `name()` storage, which lives for the process.
absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*> BuildIndex(
    const std::vector<CelRuntimeFunction>& fns) {
  absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*> m;
  m.reserve(fns.size());
  for (const CelRuntimeFunction& h : fns) {
    m[h.name()] = &h;
  }
  return m;
}

const absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*>&
RuntimeIndex() {
  static const absl::NoDestructor<
      absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*>>
      kIndex(BuildIndex(CelRuntimeHelpersVec()));
  return *kIndex;
}

const absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*>&
HostIndex() {
  static const absl::NoDestructor<
      absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*>>
      kIndex(BuildIndex(CelHostFunctionsVec()));
  return *kIndex;
}

const absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*>&
EnvIndex() {
  static const absl::NoDestructor<
      absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*>>
      kIndex(BuildIndex(CelEnvFunctionsVec()));
  return *kIndex;
}

}  // namespace

absl::Span<const CelRuntimeFunction> CelRuntimeHelpers() {
  return absl::MakeConstSpan(CelRuntimeHelpersVec());
}

absl::Span<const CelRuntimeFunction> CelHostFunctions() {
  return absl::MakeConstSpan(CelHostFunctionsVec());
}

absl::Span<const CelRuntimeFunction> CelEnvFunctions() {
  return absl::MakeConstSpan(CelEnvFunctionsVec());
}

absl::Status CheckRuntimeAbiVersion(const CelAbi& abi) {
  const uint32_t prog_v = abi.runtime_abi_version();
  const uint32_t engine_v = kRuntimeAbiVersion;
  if (prog_v == engine_v) return absl::OkStatus();
  if (prog_v == 0) {
    const bool empty = abi.variables_size() == 0 && abi.fields_size() == 0 &&
                       abi.attributes_size() == 0 && abi.types_size() == 0;
    if (empty) return absl::OkStatus();
    return absl::FailedPreconditionError(absl::StrCat(
        "cel.abi: program predates ABI versioning "
        "(runtime_abi_version=0); recompile against this engine's compiler "
        "(engine ABI v",
        engine_v, ")"));
  }
  return absl::FailedPreconditionError(
      absl::StrCat("cel.abi: runtime ABI version mismatch — program compiled "
                   "against v",
                   prog_v, ", this engine ships v", engine_v,
                   "; recompile the program against this engine's compiler"));
}

const CelRuntimeFunction* FindBuiltinHelper(AbiModule module,
                                            absl::string_view name) {
  const absl::flat_hash_map<absl::string_view, const CelRuntimeFunction*>* idx =
      nullptr;
  switch (module) {
    case AbiModule::kCelRuntime:
      idx = &RuntimeIndex();
      break;
    case AbiModule::kCelHost:
      idx = &HostIndex();
      break;
    case AbiModule::kCelEnv:
      idx = &EnvIndex();
      break;
    case AbiModule::kCelFn:
      // Custom-fn helpers aren't in the catalogue; arity comes
      // from the per-compile registration in Compiler::Builder.
      return nullptr;
  }
  auto it = idx->find(name);
  return it == idx->end() ? nullptr : it->second;
}

}  // namespace celwasm::abi
