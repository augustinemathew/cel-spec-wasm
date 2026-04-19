#include "compiler/host/host_loader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/runtime/cel_runtime_wasm_bytes.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// Pulls the message out of a wasmtime error (and frees it).  Returns
// an empty string if err is null; the caller checks for null first and
// only calls this on the error path.
std::string ErrorMessage(wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string out(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return out;
}

std::string TrapMessage(wasm_trap_t* trap) {
  wasm_message_t msg;
  wasm_trap_message(trap, &msg);
  std::string out(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return out;
}

// Builds a wasmtime engine with the feature set the runtime module and
// the eval module require.  Keep in sync with the feature mask applied
// in WasmModule's constructor (module.cc) — if they drift, the runtime
// or eval bytes will decode cleanly in Binaryen and then be rejected
// here at instantiation time.
wasm_engine_t* NewEngine() {
  wasm_config_t* config = wasm_config_new();
  // reference-types is required for the `$cel_refs` externref table,
  // and function-references/gc are the follow-on proposals wasmtime
  // demands once a typed `ref.null externref` initializer appears.
  wasmtime_config_wasm_reference_types_set(config, true);
  wasmtime_config_wasm_function_references_set(config, true);
  wasmtime_config_wasm_gc_set(config, true);
  // Multi-value and bulk-memory ride along in wasmtime's default set;
  // sign-ext and mutable-globals are always on.  Leave defaults for
  // those rather than touching settings that have moved between
  // wasmtime releases.
  return wasm_engine_new_with_config(config);
}

// Small helper — the flow below allocates modules, instances, and the
// linker one step at a time, and any step can fail.  Rather than
// hand-write a goto chain, use a local struct + early returns, and let
// LoadedEval::Reset() (on the out-param) clean up on failure.
absl::Status CompileModule(wasm_engine_t* engine,
                           absl::Span<const uint8_t> bytes,
                           absl::string_view label,
                           wasmtime_module_t** out) {
  wasmtime_error_t* err =
      wasmtime_module_new(engine, bytes.data(), bytes.size(), out);
  if (err != nullptr) {
    return absl::InternalError(
        absl::StrCat("wasmtime_module_new(", label, "): ", ErrorMessage(err)));
  }
  return absl::OkStatus();
}

absl::Status InstantiateBare(wasmtime_context_t* ctx,
                             wasmtime_module_t* module,
                             absl::string_view label,
                             wasmtime_instance_t* out) {
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_instance_new(ctx, module, /*imports=*/nullptr, /*nimports=*/0,
                            out, &trap);
  if (err != nullptr) {
    return absl::InternalError(absl::StrCat(
        "wasmtime_instance_new(", label, "): ", ErrorMessage(err)));
  }
  if (trap != nullptr) {
    return absl::InternalError(absl::StrCat(
        "start-function trap in ", label, ": ", TrapMessage(trap)));
  }
  return absl::OkStatus();
}

// Looks up a function export on the given instance and invokes it with
// `args` / expecting `n_results` results.  Fails (NotFound) if the
// export is missing or not a function.
absl::Status CallInstanceFn(wasmtime_context_t* ctx,
                            const wasmtime_instance_t& instance,
                            absl::string_view name,
                            absl::Span<const wasmtime_val_t> args,
                            absl::Span<wasmtime_val_t> results) {
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &instance, name.data(), name.size(),
                                    &ext)) {
    return absl::NotFoundError(
        absl::StrCat("instance has no export named `", name, "`"));
  }
  if (ext.kind != WASMTIME_EXTERN_FUNC) {
    wasmtime_extern_delete(&ext);
    return absl::FailedPreconditionError(
        absl::StrCat("export `", name, "` is not a function"));
  }
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_func_call(
      ctx, &ext.of.func, args.data(), args.size(), results.data(),
      results.size(), &trap);
  if (err != nullptr) {
    return absl::InternalError(
        absl::StrCat("wasmtime_func_call(", name, "): ", ErrorMessage(err)));
  }
  if (trap != nullptr) {
    return absl::InternalError(
        absl::StrCat(name, " trapped: ", TrapMessage(trap)));
  }
  return absl::OkStatus();
}

}  // namespace

void LoadedEval::Reset() noexcept {
  has_instances_ = false;
  if (linker_ != nullptr) {
    wasmtime_linker_delete(linker_);
    linker_ = nullptr;
  }
  if (eval_mod_ != nullptr) {
    wasmtime_module_delete(eval_mod_);
    eval_mod_ = nullptr;
  }
  if (runtime_mod_ != nullptr) {
    wasmtime_module_delete(runtime_mod_);
    runtime_mod_ = nullptr;
  }
  if (store_ != nullptr) {
    wasmtime_store_delete(store_);
    store_ = nullptr;
  }
  if (engine_ != nullptr) {
    wasm_engine_delete(engine_);
    engine_ = nullptr;
  }
  runtime_instance_ = {};
  eval_instance_ = {};
}

LoadedEval::~LoadedEval() { Reset(); }

LoadedEval::LoadedEval(LoadedEval&& other) noexcept {
  *this = std::move(other);
}

LoadedEval& LoadedEval::operator=(LoadedEval&& other) noexcept {
  if (this == &other) return *this;
  Reset();
  engine_ = other.engine_;
  store_ = other.store_;
  runtime_mod_ = other.runtime_mod_;
  eval_mod_ = other.eval_mod_;
  linker_ = other.linker_;
  runtime_instance_ = other.runtime_instance_;
  eval_instance_ = other.eval_instance_;
  has_instances_ = other.has_instances_;
  other.engine_ = nullptr;
  other.store_ = nullptr;
  other.runtime_mod_ = nullptr;
  other.eval_mod_ = nullptr;
  other.linker_ = nullptr;
  other.runtime_instance_ = {};
  other.eval_instance_ = {};
  other.has_instances_ = false;
  return *this;
}

wasmtime_context_t* LoadedEval::context() const {
  return store_ == nullptr ? nullptr : wasmtime_store_context(store_);
}

absl::StatusOr<wasmtime_val_t> LoadedEval::CallEval(
    absl::Span<const wasmtime_val_t> args) {
  if (!has_instances_) {
    return absl::FailedPreconditionError(
        "LoadedEval::CallEval on an uninitialised instance "
        "(did LoadEval() fail or was the object moved-from?)");
  }
  wasmtime_context_t* ctx = wasmtime_store_context(store_);

  // Per-call isolation: rewind the runtime's bump arena so repeated
  // invocations don't leak earlier allocations.  The runtime exports
  // `cel_reset` precisely for this.
  if (auto s = CallInstanceFn(ctx, runtime_instance_, "cel_reset",
                              /*args=*/{}, /*results=*/{});
      !s.ok()) {
    return s;
  }

  wasmtime_val_t result{};
  absl::Span<wasmtime_val_t> results(&result, 1);
  if (auto s = CallInstanceFn(ctx, eval_instance_, "eval", args, results);
      !s.ok()) {
    return s;
  }
  return result;
}

absl::StatusOr<wasmtime_val_t> LoadedEval::CallNullaryEval() {
  return CallEval(/*args=*/{});
}

absl::StatusOr<LoadedEval> LoadEval(
    absl::Span<const uint8_t> eval_wasm_bytes) {
  LoadedEval out;
  out.engine_ = NewEngine();
  if (out.engine_ == nullptr) {
    return absl::InternalError("wasm_engine_new_with_config returned null");
  }
  out.store_ = wasmtime_store_new(out.engine_, /*data=*/nullptr,
                                  /*finalizer=*/nullptr);
  if (out.store_ == nullptr) {
    return absl::InternalError("wasmtime_store_new returned null");
  }
  wasmtime_context_t* ctx = wasmtime_store_context(out.store_);

  // Compile both modules up front.
  const absl::Span<const uint8_t> runtime_bytes(
      kCelRuntimeWasmBytes, kCelRuntimeWasmBytesSize);
  if (auto s = CompileModule(out.engine_, runtime_bytes, "runtime",
                             &out.runtime_mod_);
      !s.ok()) {
    return s;
  }
  if (auto s = CompileModule(out.engine_, eval_wasm_bytes, "eval",
                             &out.eval_mod_);
      !s.ok()) {
    return s;
  }

  // Runtime has no imports — instantiate it bare.
  if (auto s = InstantiateBare(ctx, out.runtime_mod_, "runtime",
                               &out.runtime_instance_);
      !s.ok()) {
    return s;
  }

  // Linker: register the runtime's exports under the namespace `"cel"`
  // so the eval module's `(import "cel" "memory" …)` and every
  // `(import "cel" "cel_*" …)` resolves without per-name plumbing.
  out.linker_ = wasmtime_linker_new(out.engine_);
  if (out.linker_ == nullptr) {
    return absl::InternalError("wasmtime_linker_new returned null");
  }
  {
    const char kCelNs[] = "cel";
    wasmtime_error_t* err = wasmtime_linker_define_instance(
        out.linker_, ctx, kCelNs, sizeof(kCelNs) - 1, &out.runtime_instance_);
    if (err != nullptr) {
      return absl::InternalError(absl::StrCat(
          "wasmtime_linker_define_instance(cel): ", ErrorMessage(err)));
    }
  }

  // Instantiate the eval module; the linker resolves every "cel.<name>"
  // import against the runtime instance registered above.
  {
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        out.linker_, ctx, out.eval_mod_, &out.eval_instance_, &trap);
    if (err != nullptr) {
      return absl::InternalError(absl::StrCat(
          "wasmtime_linker_instantiate(eval): ", ErrorMessage(err)));
    }
    if (trap != nullptr) {
      return absl::InternalError(
          absl::StrCat("eval start-function trap: ", TrapMessage(trap)));
    }
  }
  out.has_instances_ = true;
  return out;
}

}  // namespace celwasm
