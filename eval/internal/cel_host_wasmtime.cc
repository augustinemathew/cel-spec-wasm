#include "eval/internal/cel_host_wasmtime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "abi/cel_abi.pb.h"
#include "abi/runtime_catalogue.h"
#include "eval/internal/cel_host.h"
#include "runtime/cel_data.h"
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

  // Resolve `cel.abi.types[]` FQNs against the descriptor pool.
  // A null pool (kept here for legacy callers) leaves every entry's
  // descriptor as nullptr; the trampoline surfaces those as a clean
  // spec-level CEL_ERROR.  Sentinel id 0 entry is included so
  // trampoline lookups can index by id directly.
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
  WasmtimeMemoryView(wasmtime_context_t* ctx, wasmtime_sharedmemory_t* mem)
      : ctx_(ctx), mem_(mem) {}

  CelValue ReadCelValue(uint32_t offset) const override {
    CelValue cv{};
    std::memcpy(&cv, Data() + offset, sizeof(cv));
    return cv;
  }

  void WriteCelValue(uint32_t offset, const CelValue& v) override {
    std::memcpy(Data() + offset, &v, sizeof(v));
  }

  void WriteU32(uint32_t offset, uint32_t value) override {
    std::memcpy(Data() + offset, &value, sizeof(value));
  }

  absl::string_view ReadSpan(uint32_t ptr, uint32_t len) const override {
    return {reinterpret_cast<const char*>(Data() + ptr), len};
  }

 private:
  uint8_t* Data() const {
    return wasmtime_sharedmemory_data(mem_);
  }

  wasmtime_context_t* ctx_;
  wasmtime_sharedmemory_t* mem_;
};

// `WasmtimeArenaAllocator::Alloc` — calls the runtime's
// `arena_alloc(size) -> offset` wasm export.  Reentrant into wasm
// from inside a host trampoline — wasmtime supports this.  A trap
// from arena_alloc (OOM, ill-formed state) surfaces as `nullptr` from
// Alloc; Layer 2 turns that into ResourceExhausted.  The contract
// matches `EncodeSpan` in cel_host.cc: zero-byte alloc returns a
// valid (possibly null-derefable on offset==0) pointer with a
// stamped `out_offset`; the caller must skip the memcpy when len==0.
//
// Definition lives outside the anonymous namespace (in the
// `celwasm` namespace) because `instance.cc` reuses this allocator
// for Activation-side kString / kBytes encoding.

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
  WasmtimeArenaAllocator alloc(ctx, env->arena_alloc_fn, env->memory);
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
// `HostMapBacking`.  `cel_host.cel_list_at` is the sibling with the
// same shape; both share the 3-arg helper below.
template <auto Impl>
wasm_trap_t* HostThreeArgTrampoline(void* absl_nonnull env_ptr,
                                    wasmtime_caller_t* absl_nonnull caller,
                                    const wasmtime_val_t* args) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->arena_alloc_fn, env->memory);
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

// `cel_host.cel_map_iter_open(state_offset, map_slot)` — 2 i32 args,
// void return.  Same shape as the 2-arg helpers below but reuses the
// existing `HostTwoArgTrampoline` once it lands; until then, define
// inline to avoid a forward declaration of the template.
extern "C" wasm_trap_t* CelMapIterOpenTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->arena_alloc_fn, env->memory);
  const TrampolineContext tctx{env->bindings, mem, env->refs, alloc};
  return StatusToTrap(
      CelMapIterOpenImpl(static_cast<uint32_t>(args[0].of.i32),
                         static_cast<uint32_t>(args[1].of.i32), tctx));
}

extern "C" wasm_trap_t* CelListAtTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelListAtImpl>(env_ptr, caller, args);
}

// `cel_host.cel_list_iter_open(out_slot, list_slot)` — 2 i32 args,
// void return.  Mirrors CelMapIterOpenTrampoline shape; forwards
// into CelListIterOpenImpl which snapshots a host list to arena
// CEL_LIST_ARENA shape (m5b §CCF-8 Slice 2).
extern "C" wasm_trap_t* CelListIterOpenTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->arena_alloc_fn, env->memory);
  const TrampolineContext tctx{env->bindings, mem, env->refs, alloc};
  return StatusToTrap(
      CelListIterOpenImpl(static_cast<uint32_t>(args[0].of.i32),
                          static_cast<uint32_t>(args[1].of.i32), tctx));
}

// Aggregate-op kHost trampolines.  Three-arg helpers
// (in/eq/concat) reuse `HostThreeArgTrampoline`; size helpers take
// two args and reach Impls of arity-2 directly.
template <auto Impl>
wasm_trap_t* HostTwoArgTrampoline(void* absl_nonnull env_ptr,
                                  wasmtime_caller_t* absl_nonnull caller,
                                  const wasmtime_val_t* args) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->arena_alloc_fn, env->memory);
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

// cel_host.cel_make_message — `(type_id, out_slot)` → ().
// Same shape as `HostTwoArgTrampoline` but the impl arity matches
// `(uint32_t type_id, uint32_t out_slot, const TrampolineContext&)`.
extern "C" wasm_trap_t* CelMakeMessageTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostTwoArgTrampoline<CelMakeMessageImpl>(env_ptr, caller, args);
}

// cel_host.cel_set_field — `(msg_slot, field_ref_id, value_slot)`
// → ().  Three i32 args; reuses HostThreeArgTrampoline since the
// impl signature `(uint32_t, uint32_t, uint32_t, const TrampolineContext&)`
// matches the template's expected arity.
extern "C" wasm_trap_t* CelSetFieldTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelSetFieldImpl>(env_ptr, caller, args);
}

// cel_host.resolve_message_type_name — `(out_slot, in_slot)` → ().
// Two i32 args.  Layer-2 body lives in `cel_host.cc`.
extern "C" wasm_trap_t* CelResolveMessageTypeNameTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostTwoArgTrampoline<CelResolveMessageTypeNameImpl>(env_ptr, caller,
                                                             args);
}

// The timestamp / duration parse + format trampolines previously
// here have been deleted along with their `*Impl` bodies; codegen
// now routes the four ids to runtime-hosted absl kernels.  See
// `runtime/cel_time_parse.cc` and
// `doc/implementation-plan/rewrite/phase-c-plan.md` §4.

extern "C" wasm_trap_t* CelWktUnwrapTimeTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostTwoArgTrampoline<CelWktUnwrapTimeImpl>(env_ptr, caller, args);
}

// 3-arg `(out_slot, msg_slot, wrapper_kind)` trampoline for the
// kStructExpr tail-unwrap of WKT wrapper proto literals.  Mirrors
// `CelWktUnwrapTimeTrampoline` but with 3 slot/kind args (the third
// arg is the matching CelKind enum) — fits the existing 3-arg
// `HostThreeArgTrampoline` template above.
extern "C" wasm_trap_t* CelWktUnwrapWrapperTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  return HostThreeArgTrampoline<CelWktUnwrapWrapperImpl>(env_ptr, caller, args);
}

// 4-arg `(out_slot, ts_slot, tz_slot, accessor_kind)` dispatch
// trampoline for all 10 with-TZ accessor overloads.  The 4-arg
// shape is unique; no other host trampoline today takes more than
// 3 slot indices + optional state.  Helper template inlined
// locally.
extern "C" wasm_trap_t* CelTimestampTzAccessorTrampoline(
    void* absl_nonnull env_ptr, wasmtime_caller_t* absl_nonnull caller,
    const wasmtime_val_t* args, size_t /*nargs*/, wasmtime_val_t* /*results*/,
    size_t /*nresults*/) {
  auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  WasmtimeMemoryView mem(ctx, env->memory);
  WasmtimeArenaAllocator alloc(ctx, env->arena_alloc_fn, env->memory);
  const TrampolineContext tctx{env->bindings, mem, env->refs, alloc};
  return StatusToTrap(
      CelTimestampTzAccessorImpl(static_cast<uint32_t>(args[0].of.i32),
                                 static_cast<uint32_t>(args[1].of.i32),
                                 static_cast<uint32_t>(args[2].of.i32),
                                 static_cast<uint32_t>(args[3].of.i32), tctx));
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

// One row of the `cel_host` trampoline table.  Maps a catalogue
// name to its C++ callback.  Arity + return shape come from
// `abi::CelHostFunctions()` — Slice D of the single-source-of-truth
// refactor (see `doc/implementation-plan/rewrite/abi-refactor.md`).
struct HostTrampoline {
  absl::string_view name;
  wasmtime_func_callback_t cb;
};

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
  return wasmtime_sharedmemory_data(mem_) + offset;
}

namespace {

// `cel_host` trampoline table.  Maps each name (the catalogue key)
// to its C++ callback.  Arity + return shape come from
// `abi::CelHostFunctions()`; this table only carries the function
// pointer.  The bijection check in `RegisterCelHostImports` makes
// drift between this table and the catalogue impossible to ship.
//
// For multi-overload trampolines (e.g. cel_timestamp_tz_accessor
// dispatches all 10 with-TZ accessor overloads through one 4-arg
// slot), see `doc/implementation-plan/rewrite/m7b-duration-timestamp.md`.
// For the WKT unwrap pair (`cel_wkt_unwrap_time`,
// `cel_wkt_unwrap_wrapper`), see `m8-wrapper-types.md`.
constexpr HostTrampoline kHostTrampolines[] = {
    {"cel_get_field", &CelGetFieldTrampoline},
    {"cel_has_field", &CelHasFieldTrampoline},
    {"cel_map_lookup", &CelMapLookupTrampoline},
    {"cel_map_iter_open", &CelMapIterOpenTrampoline},
    {"cel_list_iter_open", &CelListIterOpenTrampoline},
    {"cel_list_at", &CelListAtTrampoline},
    {"cel_list_size", &CelListSizeTrampoline},
    {"cel_list_in", &CelListInTrampoline},
    {"cel_list_eq", &CelListEqTrampoline},
    {"cel_list_concat", &CelListConcatTrampoline},
    {"cel_map_size", &CelMapSizeTrampoline},
    {"cel_map_in", &CelMapInTrampoline},
    {"cel_map_eq", &CelMapEqTrampoline},
    {"cel_message_eq", &CelMessageEqTrampoline},
    {"cel_make_message", &CelMakeMessageTrampoline},
    {"cel_set_field", &CelSetFieldTrampoline},
    {"resolve_message_type_name", &CelResolveMessageTypeNameTrampoline},
    {"cel_timestamp_tz_accessor", &CelTimestampTzAccessorTrampoline},
    {"cel_wkt_unwrap_time", &CelWktUnwrapTimeTrampoline},
    {"cel_wkt_unwrap_wrapper", &CelWktUnwrapWrapperTrampoline},
};

// Build the name→callback index, asserting no duplicate trampoline
// rows AND that every trampoline is in the ABI catalogue.  The
// second check is the "trampolines ⊆ catalogue" half of the
// bijection (an extra trampoline codegen never imports is dead
// code).  The "catalogue ⊆ trampolines" half is the per-entry
// check in `RegisterCelHostImports`.
absl::flat_hash_map<absl::string_view, wasmtime_func_callback_t>
BuildHostTrampolineIndex() {
  absl::flat_hash_map<absl::string_view, wasmtime_func_callback_t> idx;
  idx.reserve(std::size(kHostTrampolines));
  for (const HostTrampoline& t : kHostTrampolines) {
    const bool inserted = idx.emplace(t.name, t.cb).second;
    ABSL_CHECK(inserted) << "duplicate cel_host trampoline `" << t.name
                         << "` in cel_host_wasmtime.cc::kHostTrampolines";
    ABSL_CHECK(abi::FindBuiltinHelper(abi::AbiModule::kCelHost, t.name) !=
               nullptr)
        << "cel_host trampoline `" << t.name
        << "` is not in abi::CelHostFunctions() — add a catalogue entry "
           "in abi/runtime_catalogue.cc or delete the "
           "trampoline.";
  }
  return idx;
}

}  // namespace

absl::Status RegisterCelHostImports(wasmtime_linker_t* linker,
                                    CelHostCallbackEnv* env) {
  const auto by_name = BuildHostTrampolineIndex();
  // Catalogue ⊆ trampolines: every catalogue entry MUST have a
  // registered callback.  Otherwise codegen emits an import the
  // linker can't resolve, and instantiate fails with an opaque
  // wasmtime error — exactly the failure mode the single-source-
  // of-truth refactor exists to prevent.
  for (const abi::AbiHelper& h : abi::CelHostFunctions()) {
    auto it = by_name.find(h.name);
    ABSL_CHECK(it != by_name.end())
        << "abi::CelHostFunctions() entry `" << h.name
        << "` has no trampoline in cel_host_wasmtime.cc::kHostTrampolines";
    if (auto s = DefineHostFunc(linker, h.name, h.num_args, it->second, env);
        !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

}  // namespace celwasm
