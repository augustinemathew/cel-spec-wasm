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
  backings_.push_back(nullptr);       // slot 0 sentinel (messages)
  map_backings_.push_back(nullptr);   // slot 0 sentinel (maps)
  list_backings_.push_back(nullptr);  // slot 0 sentinel (lists)
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

uint32_t HostExternrefTable::InternList(
    std::shared_ptr<const HostListBacking> backing) {
  list_backings_.push_back(std::move(backing));
  return static_cast<uint32_t>(list_backings_.size() - 1);
}

const HostListBacking* absl_nullable HostExternrefTable::LookupList(
    uint32_t slot) const {
  return slot < list_backings_.size() ? list_backings_[slot].get() : nullptr;
}

void HostExternrefTable::Reset() {
  backings_.clear();
  backings_.push_back(nullptr);
  map_backings_.clear();
  map_backings_.push_back(nullptr);
  list_backings_.clear();
  list_backings_.push_back(nullptr);
}

// ═══════════ BuildCelHostBindings ═══════════

void BuildCelHostBindings(const celwasm::abi::CelAbi& abi,
                          const google::protobuf::DescriptorPool* pool,
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

  // M7.A: resolve `cel.abi.types[]` FQNs against the descriptor
  // pool.  A null pool (kept here for legacy callers) leaves every
  // entry's descriptor as nullptr; the trampoline surfaces those as
  // a clean spec-level CEL_ERROR.  Sentinel id 0 entry is included
  // so trampoline lookups can index by id directly.
  out.message_types_storage.clear();
  out.message_types_storage.reserve(static_cast<size_t>(abi.types_size()));
  for (const celwasm::abi::TypeEntry& t : abi.types()) {
    MessageTypeEntry entry;
    entry.fully_qualified_name = t.fully_qualified_name();
    entry.descriptor =
        (pool != nullptr && !entry.fully_qualified_name.empty())
            ? pool->FindMessageTypeByName(entry.fully_qualified_name)
            : nullptr;
    out.message_types_storage.push_back(std::move(entry));
  }

  // unknown_patterns is populated per-call by PartialEval; left
  // empty here so the default Eval path stays a no-op in the
  // trampoline's MatchesAnyUnknownPattern.
  out.bindings = CelHostBindings{
      absl::MakeConstSpan(out.field_refs_storage),
      absl::MakeConstSpan(out.attrs_storage),
      /*unknown_patterns=*/{},
      absl::MakeConstSpan(out.message_types_storage),
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

// `WasmtimeArenaAllocator::Alloc` — calls the runtime's
// `cel_alloc(size) -> offset` wasm export.  Reentrant into wasm
// from inside a host trampoline — wasmtime supports this.  A trap
// from cel_alloc (OOM, ill-formed state) surfaces as `nullptr` from
// Alloc; Layer 2 turns that into ResourceExhausted.  The contract
// matches `EncodeSpan` in cel_host.cc: zero-byte alloc returns a
// valid (possibly null-derefable on offset==0) pointer with a
// stamped `out_offset`; the caller must skip the memcpy when len==0.
//
// Definition lives outside the anonymous namespace (in the
// `celwasm` namespace) because `instance.cc` reuses this allocator
// for Activation-side kString / kBytes encoding (Slice 0 of the
// conformance unlock plan).

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
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostFieldTrampoline<CelGetFieldImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelHasFieldTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostFieldTrampoline<CelHasFieldImpl>(env_ptr, caller, args);
}

// Layer-3 trampoline for `cel_host.cel_map_lookup` — distinct ABI
// from the field trampolines (3 i32s in, void out vs. their 4
// i32s).  Forwards into `CelMapLookupImpl` (Layer 2) which handles
// the externref dereference + virtual `Get(key)` on the
// `HostMapBacking`.  M4.E adds a sibling for `cel_host.cel_list_at`
// with the same shape; both share the 3-arg helper below.
template <auto Impl>
wasm_trap_t* HostThreeArgTrampoline(void* absl_nonnull env_ptr,
                                    wasmtime_caller_t* absl_nonnull caller,
                                    const wasmtime_val_t* args) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->cel_alloc_fn, env->memory);
  const TrampolineContext tctx{env->bindings, mem, env->refs, alloc};
  return StatusToTrap(Impl(static_cast<uint32_t>(args[0].of.i32),
                           static_cast<uint32_t>(args[1].of.i32),
                           static_cast<uint32_t>(args[2].of.i32), tctx));
}

extern "C" wasm_trap_t* CelMapLookupTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelMapLookupImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelListAtTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelListAtImpl>(env_ptr, caller, args);
}

// M5.D step 2 — aggregate-op kHost trampolines.  Three-arg helpers
// (in/eq/concat) reuse `HostThreeArgTrampoline`; size helpers take
// two args and reach Impls of arity-2 directly.
template <auto Impl>
wasm_trap_t* HostTwoArgTrampoline(void* absl_nonnull env_ptr,
                                  wasmtime_caller_t* absl_nonnull caller,
                                  const wasmtime_val_t* args) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->cel_alloc_fn, env->memory);
  const TrampolineContext tctx{env->bindings, mem, env->refs, alloc};
  return StatusToTrap(Impl(static_cast<uint32_t>(args[0].of.i32),
                           static_cast<uint32_t>(args[1].of.i32), tctx));
}

extern "C" wasm_trap_t* CelListSizeTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostTwoArgTrampoline<CelListSizeImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelListInTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelListInImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelListEqTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelListEqImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelListConcatTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelListConcatImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelMapSizeTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostTwoArgTrampoline<CelMapSizeImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelMapInTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelMapInImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelMapEqTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelMapEqImpl>(env_ptr, caller, args);
}

extern "C" wasm_trap_t* CelMessageEqTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelMessageEqImpl>(env_ptr, caller, args);
}

// M7.A: cel_host.cel_make_message — `(type_id, out_slot)` → ().
// Same shape as `HostTwoArgTrampoline` but the impl arity matches
// `(uint32_t type_id, uint32_t out_slot, const TrampolineContext&)`.
extern "C" wasm_trap_t* CelMakeMessageTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostTwoArgTrampoline<CelMakeMessageImpl>(env_ptr, caller, args);
}

// M7.B: cel_host.cel_set_field — `(msg_slot, field_ref_id, value_slot)`
// → ().  Three i32 args; reuses HostThreeArgTrampoline since the
// impl signature `(uint32_t, uint32_t, uint32_t, const TrampolineContext&)`
// matches the template's expected arity.
extern "C" wasm_trap_t* CelSetFieldTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelSetFieldImpl>(env_ptr, caller, args);
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

namespace {

struct HostImportEntry {
  absl::string_view name;
  size_t arity;
  wasmtime_func_callback_t cb;
};

absl::Status DefineAll(wasmtime_linker_t* linker, CelHostCallbackEnv* env,
                       absl::Span<const HostImportEntry> entries) {
  for (const HostImportEntry& e : entries) {
    if (auto s = DefineHostFunc(linker, e.name, e.arity, e.cb, env); !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

}  // namespace

// ═══════════ WasmtimeArenaAllocator (named-namespace export) ═══════════

uint8_t* absl_nullable WasmtimeArenaAllocator::Alloc(
    size_t len, uint32_t* absl_nonnull out_offset) {
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

absl::Status RegisterCelHostImports(wasmtime_linker_t* linker,
                                    CelHostCallbackEnv* env) {
  // Register every cel_host.* import the runtime module declares.
  // Arities: get/has_field are 4-arg (msg + field_ref + attr + out);
  // every other cel_host.* helper is slot-out (out + N operands).
  // M5.D step 2 added the seven aggregate-op kHost arms plus the
  // standalone `cel_message_eq` helper.
  static constexpr HostImportEntry kEntries[] = {
      {"cel_get_field", 4, &CelGetFieldTrampoline},
      {"cel_has_field", 4, &CelHasFieldTrampoline},
      {"cel_map_lookup", 3, &CelMapLookupTrampoline},
      {"cel_list_at", 3, &CelListAtTrampoline},
      {"cel_list_size", 2, &CelListSizeTrampoline},
      {"cel_list_in", 3, &CelListInTrampoline},
      {"cel_list_eq", 3, &CelListEqTrampoline},
      {"cel_list_concat", 3, &CelListConcatTrampoline},
      {"cel_map_size", 2, &CelMapSizeTrampoline},
      {"cel_map_in", 3, &CelMapInTrampoline},
      {"cel_map_eq", 3, &CelMapEqTrampoline},
      {"cel_message_eq", 3, &CelMessageEqTrampoline},
      // M7.A — proto literal construction.  Two i32 args
      // `(type_id, out_slot)`; void result.
      {"cel_make_message", 2, &CelMakeMessageTrampoline},
      // M7.B — proto literal field set.  Three i32 args
      // `(msg_slot, field_ref_id, value_slot)`; void result.
      {"cel_set_field", 3, &CelSetFieldTrampoline},
  };
  return DefineAll(linker, env, absl::MakeConstSpan(kEntries));
}

}  // namespace celwasm
