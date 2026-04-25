#include "compiler_v2/api/internal/cel_host_wasmtime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/api/internal/cel_host.h"
#include "compiler_v2/runtime/cel_data.h"
#include "google/protobuf/descriptor.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

// ═══════════ HostExternrefTable ═══════════

HostExternrefTable::HostExternrefTable() {
  backings_.push_back(nullptr);      // slot 0 sentinel (messages)
  map_backings_.push_back(nullptr);  // slot 0 sentinel (maps)
}

uint32_t HostExternrefTable::Intern(
    std::shared_ptr<const HostMessageBacking> backing) {
  backings_.push_back(std::move(backing));
  return static_cast<uint32_t>(backings_.size() - 1);
}

const HostMessageBacking* absl_nullable HostExternrefTable::Lookup(
    uint32_t slot) const {
  return slot < backings_.size() ? backings_[slot].get() : nullptr;
}

uint32_t HostExternrefTable::InternMap(
    std::shared_ptr<const HostMapBacking> backing) {
  map_backings_.push_back(std::move(backing));
  return static_cast<uint32_t>(map_backings_.size() - 1);
}

const HostMapBacking* absl_nullable HostExternrefTable::LookupMap(
    uint32_t slot) const {
  return slot < map_backings_.size() ? map_backings_[slot].get() : nullptr;
}

void HostExternrefTable::Reset() {
  backings_.clear();
  backings_.push_back(nullptr);
  map_backings_.clear();
  map_backings_.push_back(nullptr);
}

// ═══════════ BuildCelHostBindings ═══════════

void BuildCelHostBindings(const celwasm::abi::CelAbi& abi,
                          const google::protobuf::DescriptorPool* /*pool*/,
                          CelHostCallbackEnv& out) {
  out.field_refs_storage.clear();
  out.field_refs_storage.reserve(static_cast<size_t>(abi.fields_size()));
  for (const celwasm::abi::FieldEntry& f : abi.fields()) {
    out.field_refs_storage.push_back(
        FieldRefEntry{/*field_number=*/f.field_number(), f.name()});
  }

  out.attrs_storage.clear();
  out.attrs_storage.reserve(static_cast<size_t>(abi.attributes_size()));
  for (const celwasm::abi::AttributeEntry& a : abi.attributes()) {
    std::vector<std::string> qualifiers(a.qualifiers().begin(),
                                        a.qualifiers().end());
    out.attrs_storage.push_back(
        AttributeEntry{a.variable(), std::move(qualifiers)});
  }

  // unknown_patterns is populated per-call by PartialEval; left
  // empty here so the default Eval path stays a no-op in the
  // trampoline's MatchesAnyUnknownPattern.
  out.bindings = CelHostBindings{
      absl::MakeConstSpan(out.field_refs_storage),
      absl::MakeConstSpan(out.attrs_storage),
      /*unknown_patterns=*/{},
  };
}

namespace {

// ═══════════ Wasmtime-backed Layer 2 adapters ═══════════

class WasmtimeMemoryView final : public MemoryView {
 public:
  WasmtimeMemoryView(wasmtime_context_t* ctx, wasmtime_memory_t mem)
      : ctx_(ctx), mem_(mem) {}

  CelValue ReadCelValue(uint32_t offset) const override {
    CelValue cv{};
    std::memcpy(&cv, Data() + offset, sizeof(cv));
    return cv;
  }

  void WriteCelValue(uint32_t offset, const CelValue& v) override {
    std::memcpy(Data() + offset, &v, sizeof(v));
  }

  absl::string_view ReadSpan(uint32_t ptr, uint32_t len) const override {
    return {reinterpret_cast<const char*>(Data() + ptr), len};
  }

 private:
  uint8_t* Data() const {
    wasmtime_memory_t m = mem_;
    return wasmtime_memory_data(ctx_, &m);
  }

  wasmtime_context_t* ctx_;
  wasmtime_memory_t mem_;
};

// Calls the runtime's `cel_alloc(size) -> offset` wasm export.
// Reentrant into wasm from inside a host trampoline — wasmtime
// supports this.  A trap from cel_alloc (OOM, ill-formed state)
// surfaces as `nullptr` from Alloc; Layer 2 turns that into
// ResourceExhausted.
class WasmtimeArenaAllocator final : public ArenaAllocator {
 public:
  WasmtimeArenaAllocator(wasmtime_context_t* ctx, wasmtime_func_t fn,
                         wasmtime_memory_t mem)
      : ctx_(ctx), fn_(fn), mem_(mem) {}

  uint8_t* absl_nullable Alloc(size_t len,
                               uint32_t* absl_nonnull out_offset) override {
    wasmtime_val_t arg;
    arg.kind = WASMTIME_I32;
    arg.of.i32 = static_cast<int32_t>(len);
    wasmtime_val_t result;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_func_call(ctx_, &fn_, &arg, 1, &result, 1, &trap);
    if (err != nullptr) {
      wasmtime_error_delete(err);
      return nullptr;
    }
    if (trap != nullptr) {
      wasm_trap_delete(trap);
      return nullptr;
    }
    if (result.kind != WASMTIME_I32 || result.of.i32 == 0) {
      return nullptr;
    }
    const auto offset = static_cast<uint32_t>(result.of.i32);
    *out_offset = offset;
    wasmtime_memory_t m = mem_;
    return wasmtime_memory_data(ctx_, &m) + offset;
  }

 private:
  wasmtime_context_t* ctx_;
  wasmtime_func_t fn_;
  wasmtime_memory_t mem_;
};

// Convert an absl::Status from Layer 2 into a wasmtime trap.
// Infrastructure failures (null backing, OOM, malformed input) bubble
// up as traps; spec-level CEL errors stay in-wire (out_slot holds the
// CEL_ERROR) and return nullptr.
wasm_trap_t* absl_nullable StatusToTrap(const absl::Status& status) {
  if (status.ok()) return nullptr;
  const std::string msg(status.message());
  return wasmtime_trap_new(msg.data(), msg.size());
}

// Shared argument unpack + context construction — cel_get_field and
// cel_has_field have identical ABI, only the Layer 2 dispatch differs.
template <auto Impl>
wasm_trap_t* HostFieldTrampoline(void* env_ptr, wasmtime_caller_t* caller,
                                 const wasmtime_val_t* args) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->cel_alloc_fn, env->memory);
  const TrampolineContext tctx{env->bindings, mem, env->refs, alloc};
  return StatusToTrap(Impl(static_cast<uint32_t>(args[0].of.i32),
                           static_cast<uint32_t>(args[1].of.i32),
                           static_cast<uint32_t>(args[2].of.i32),
                           static_cast<uint32_t>(args[3].of.i32), tctx));
}

extern "C" wasm_trap_t* CelGetFieldTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/,
    wasmtime_val_t* /*results*/, size_t /*nresults*/) {
  return HostFieldTrampoline<CelGetFieldImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelHasFieldTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/,
    wasmtime_val_t* /*results*/, size_t /*nresults*/) {
  return HostFieldTrampoline<CelHasFieldImpl>(env_ptr, caller, args);
}

// Layer-3 trampoline for `cel_host.cel_map_lookup` — distinct ABI
// from the field trampolines (3 i32s in, void out vs. their 4
// i32s).  Forwards into `CelMapLookupImpl` (Layer 2) which handles
// the externref dereference + virtual `Get(key)` on the
// `HostMapBacking`.
extern "C" wasm_trap_t* CelMapLookupTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/,
    wasmtime_val_t* /*results*/, size_t /*nresults*/) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->cel_alloc_fn, env->memory);
  const TrampolineContext tctx{env->bindings, mem, env->refs, alloc};
  return StatusToTrap(CelMapLookupImpl(static_cast<uint32_t>(args[0].of.i32),
                                       static_cast<uint32_t>(args[1].of.i32),
                                       static_cast<uint32_t>(args[2].of.i32),
                                       tctx));
}

wasm_functype_t* NI32sToVoid(size_t n) {
  std::vector<wasm_valtype_t*> params(n);
  for (auto& p : params) {
    p = wasm_valtype_new(WASM_I32);
  }
  wasm_valtype_vec_t params_vec;
  wasm_valtype_vec_t results_vec;
  wasm_valtype_vec_new(&params_vec, n, params.data());
  wasm_valtype_vec_new_empty(&results_vec);
  return wasm_functype_new(&params_vec, &results_vec);
}

absl::Status DefineHostFunc(wasmtime_linker_t* linker, absl::string_view name,
                            size_t arity, wasmtime_func_callback_t cb,
                            CelHostCallbackEnv* env) {
  wasm_functype_t* ty = NI32sToVoid(arity);
  const char kModule[] = "cel_host";
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, kModule, sizeof(kModule) - 1, name.data(), name.size(), ty, cb,
      env, /*finalizer=*/nullptr);
  wasm_functype_delete(ty);
  if (err != nullptr) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    const std::string text(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return absl::InternalError(absl::StrCat(
        "wasmtime_linker_define_func(cel_host.", name, "): ", text));
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status RegisterCelHostImports(wasmtime_linker_t* linker,
                                    CelHostCallbackEnv* env) {
  if (auto s = DefineHostFunc(linker, "cel_get_field", /*arity=*/4,
                              CelGetFieldTrampoline, env);
      !s.ok()) {
    return s;
  }
  if (auto s = DefineHostFunc(linker, "cel_has_field", /*arity=*/4,
                              CelHasFieldTrampoline, env);
      !s.ok()) {
    return s;
  }
  return DefineHostFunc(linker, "cel_map_lookup", /*arity=*/3,
                        CelMapLookupTrampoline, env);
}

}  // namespace celwasm
