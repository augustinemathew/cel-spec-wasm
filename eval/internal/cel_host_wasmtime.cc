#include "eval/internal/cel_host_wasmtime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "abi/runtime_catalogue.h"
#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "eval/internal/cel_host.h"
#include "google/protobuf/descriptor.h"
#include "runtime/cel_data.h"
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
  // Dedup proto-exposing backings by the underlying Message identity
  // (see the class comment for the lifetime argument).  A hit drops
  // `backing` — for an already-recorded message the existing backing
  // is observably identical, and an owning backing can never hit
  // (its freshly-allocated message cannot alias a recorded pointer
  // that is still anchored).  Non-proto custom backings
  // (`message() == nullptr`) intern unconditionally, as before.
  const google::protobuf::Message* msg =
      backing != nullptr ? backing->message() : nullptr;
  if (msg != nullptr) {
    auto [it, inserted] = msg_slots_.try_emplace(msg, 0);
    if (!inserted) return it->second;
    backings_.push_back(std::move(backing));
    it->second = static_cast<uint32_t>(backings_.size() - 1);
    return it->second;
  }
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

uint32_t HostExternrefTable::InternProtoMessage(
    const google::protobuf::Message* absl_nonnull msg) {
  // Hot select-chain path: a repeat hop over the same sub-message
  // within an Eval returns the issued slot with no allocation at
  // all — the dedup check runs BEFORE the ProtoBacking is built.
  if (auto it = msg_slots_.find(msg); it != msg_slots_.end()) {
    return it->second;
  }
  // Miss: `Intern` allocates the backing and records msg → slot
  // (ProtoBacking::message() returns `msg`).
  return Intern(std::make_shared<ProtoBacking>(msg));
}

uint32_t HostExternrefTable::InternProtoMapField(
    const google::protobuf::Message* absl_nonnull owner,
    const google::protobuf::FieldDescriptor* absl_nonnull field) {
  auto [it, inserted] = map_field_slots_.try_emplace({owner, field}, 0);
  if (!inserted) return it->second;
  it->second = InternMap(std::make_shared<ProtoMap>(owner, field));
  return it->second;
}

uint32_t HostExternrefTable::InternProtoListField(
    const google::protobuf::Message* absl_nonnull owner,
    const google::protobuf::FieldDescriptor* absl_nonnull field) {
  auto [it, inserted] = list_field_slots_.try_emplace({owner, field}, 0);
  if (!inserted) return it->second;
  it->second = InternList(std::make_shared<ProtoList>(owner, field));
  return it->second;
}

void HostExternrefTable::Reset() {
  // Per-Eval entry path: an Eval that interned nothing (every
  // scalar-only expression) leaves all three slot vectors at their
  // sentinel-only size — nothing to clear.  The dedup indexes can
  // only be non-empty when a slot vector grew past the sentinel
  // (every index insert pairs with a slot push), so the three size
  // checks imply the indexes are empty too.
  if (backings_.size() == 1 && map_backings_.size() == 1 &&
      list_backings_.size() == 1) {
    return;
  }
  backings_.clear();
  backings_.push_back(nullptr);
  map_backings_.clear();
  map_backings_.push_back(nullptr);
  list_backings_.clear();
  list_backings_.push_back(nullptr);
  msg_slots_.clear();
  map_field_slots_.clear();
  list_field_slots_.clear();
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
//
// `WasmtimeMemoryView` lives in the header (shared with the user
// `@host` callback trampoline in engine.cc); `WasmtimeArenaAllocator`
// is declared there too with its `Alloc` body below.

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

// ─── Unchecked (raw, unboxed) trampoline adapter ─────────────────────
//
// wasmtime's default host-callback ABI boxes every param/result
// through `wasmtime_val_t` — measured at ~75-120 ns of pure
// calling-convention cost per call (benchmark/ANALYSIS.md P0;
// //benchmark/boundary:wasmtime_call_bench).  The unchecked ABI hands
// the host a raw `wasmtime_val_raw_t` array instead, which is only
// safe because nothing but i32 ever crosses the cel_host boundary
// (values travel through linear-memory CelValue slots; see
// abi/runtime_catalogue.h).  `UncheckedHostThunk` derives the wasm
// arity from the Layer-2 impl's real C++ signature and PROVES the
// i32-only invariant at compile time: a future impl taking any other
// parameter type fails to build, right here, naming the rule.

template <typename F>
struct HostImplTraits;

template <typename... Args>
struct HostImplTraits<absl::Status (*)(Args...)> {
  // Last parameter is `const TrampolineContext&`; the rest are the
  // wasm-visible args.
  static constexpr size_t kNumWasmArgs = sizeof...(Args) - 1;

  template <size_t... I>
  static constexpr bool AllI32(std::index_sequence<I...>) {
    return (std::is_same_v<std::tuple_element_t<I, std::tuple<Args...>>,
                           uint32_t> &&
            ...);
  }
  static constexpr bool kAllI32 =
      AllI32(std::make_index_sequence<kNumWasmArgs>{});
};

template <auto Impl>
struct UncheckedHostThunk {
  using Traits = HostImplTraits<decltype(Impl)>;
  static constexpr size_t kNumArgs = Traits::kNumWasmArgs;
  static_assert(Traits::kAllI32,
                "cel_host trampolines may only take uint32_t (wasm i32) "
                "params — the unchecked wasmtime ABI has no safe story for "
                "anything else.  See abi/runtime_catalogue.h ('LOAD-"
                "BEARING') before changing this.");

  static wasm_trap_t* Call(void* env_ptr, wasmtime_caller_t* caller,
                           wasmtime_val_raw_t* args_and_results,
                           size_t /*num_args_and_results*/) {
    return CallImpl(env_ptr, caller, args_and_results,
                    std::make_index_sequence<kNumArgs>{});
  }

 private:
  template <size_t... I>
  static wasm_trap_t* CallImpl(void* env_ptr, wasmtime_caller_t* caller,
                               wasmtime_val_raw_t* raw,
                               std::index_sequence<I...>) {
    auto* env = static_cast<CelHostCallbackEnv*>(env_ptr);
    wasmtime_context_t* ctx = wasmtime_caller_context(caller);
    // Hot-path view: base cached at Plan time, size snapshot shared
    // per-Eval through the env (see CelHostCallbackEnv::mem_base).
    WasmtimeMemoryView mem(env->memory, env->mem_base, &env->mem_size);
    WasmtimeArenaAllocator alloc(ctx, env->arena_alloc_fn, env->memory);
    const TrampolineContext tctx{env->bindings, mem, env->refs, alloc};
    return StatusToTrap(Impl(static_cast<uint32_t>(raw[I].i32)..., tctx));
  }
};

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
                            size_t arity, wasmtime_func_unchecked_callback_t cb,
                            CelHostCallbackEnv* env) {
  wasm_functype_t* ty = NI32sToVoid(arity);
  const char kModule[] = "cel_host";
  wasmtime_error_t* err = wasmtime_linker_define_func_unchecked(
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
        "wasmtime_linker_define_func_unchecked(cel_host.", name, "): ", text));
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
  // Raw-ABI (unchecked) callback: `UncheckedHostThunk<...>::Call`.
  wasmtime_func_unchecked_callback_t cb;
  // The thunk's compile-time arity (derived from the Layer-2 impl's
  // C++ signature), cross-checked against the catalogue's
  // `num_args()` at registration.
  size_t arity;
};

}  // namespace

// ═══════════ WasmtimeArenaAllocator (named-namespace export) ═══════════

uint8_t* absl_nullable WasmtimeArenaAllocator::Alloc(
    size_t len, uint32_t* absl_nonnull out_offset) {
  // Unchecked invocation: the runtime's `arena_alloc` export has the
  // fixed signature `[i32 size] -> [i32 offset]`, proven once at Plan
  // by `CheckAllI32FuncSignature` in engine.cc::BindRuntimeFuncHandles
  // — this is a hot reentry (every span payload encode crosses here),
  // so skip wasmtime's per-call type/arity checking.  raw[0] carries
  // the size in and the offset out (results overwrite arguments per
  // the unchecked-call contract).  Error/trap reporting is unchanged
  // (`wasm_trap_t` out-param); both still map to nullptr, which the
  // callers surface as ResourceExhausted ("arena OOM ...").
  wasmtime_val_raw_t raw[1];
  raw[0].i32 = static_cast<int32_t>(len);
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_func_call_unchecked(
      ctx_, &fn_, raw, /*args_and_results_len=*/1, &trap);
  if (err != nullptr) {
    wasmtime_error_delete(err);
    return nullptr;
  }
  if (trap != nullptr) {
    wasm_trap_delete(trap);
    return nullptr;
  }
  if (raw[0].i32 == 0) {
    return nullptr;
  }
  const auto offset = static_cast<uint32_t>(raw[0].i32);
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
    {"cel_get_field", &UncheckedHostThunk<CelGetFieldImpl>::Call,
     UncheckedHostThunk<CelGetFieldImpl>::kNumArgs},
    {"cel_has_field", &UncheckedHostThunk<CelHasFieldImpl>::Call,
     UncheckedHostThunk<CelHasFieldImpl>::kNumArgs},
    {"cel_map_lookup", &UncheckedHostThunk<CelMapLookupImpl>::Call,
     UncheckedHostThunk<CelMapLookupImpl>::kNumArgs},
    {"cel_map_iter_open", &UncheckedHostThunk<CelMapIterOpenImpl>::Call,
     UncheckedHostThunk<CelMapIterOpenImpl>::kNumArgs},
    {"cel_list_iter_open", &UncheckedHostThunk<CelListIterOpenImpl>::Call,
     UncheckedHostThunk<CelListIterOpenImpl>::kNumArgs},
    {"cel_list_at", &UncheckedHostThunk<CelListAtImpl>::Call,
     UncheckedHostThunk<CelListAtImpl>::kNumArgs},
    {"cel_list_size", &UncheckedHostThunk<CelListSizeImpl>::Call,
     UncheckedHostThunk<CelListSizeImpl>::kNumArgs},
    {"cel_list_in", &UncheckedHostThunk<CelListInImpl>::Call,
     UncheckedHostThunk<CelListInImpl>::kNumArgs},
    {"cel_list_eq", &UncheckedHostThunk<CelListEqImpl>::Call,
     UncheckedHostThunk<CelListEqImpl>::kNumArgs},
    {"cel_list_concat", &UncheckedHostThunk<CelListConcatImpl>::Call,
     UncheckedHostThunk<CelListConcatImpl>::kNumArgs},
    {"cel_map_size", &UncheckedHostThunk<CelMapSizeImpl>::Call,
     UncheckedHostThunk<CelMapSizeImpl>::kNumArgs},
    {"cel_map_in", &UncheckedHostThunk<CelMapInImpl>::Call,
     UncheckedHostThunk<CelMapInImpl>::kNumArgs},
    {"cel_map_eq", &UncheckedHostThunk<CelMapEqImpl>::Call,
     UncheckedHostThunk<CelMapEqImpl>::kNumArgs},
    {"cel_message_eq", &UncheckedHostThunk<CelMessageEqImpl>::Call,
     UncheckedHostThunk<CelMessageEqImpl>::kNumArgs},
    {"cel_message_is_zero", &UncheckedHostThunk<CelMessageIsZeroImpl>::Call,
     UncheckedHostThunk<CelMessageIsZeroImpl>::kNumArgs},
    {"cel_make_message", &UncheckedHostThunk<CelMakeMessageImpl>::Call,
     UncheckedHostThunk<CelMakeMessageImpl>::kNumArgs},
    {"cel_set_field", &UncheckedHostThunk<CelSetFieldImpl>::Call,
     UncheckedHostThunk<CelSetFieldImpl>::kNumArgs},
    {"resolve_message_type_name",
     &UncheckedHostThunk<CelResolveMessageTypeNameImpl>::Call,
     UncheckedHostThunk<CelResolveMessageTypeNameImpl>::kNumArgs},
    {"cel_timestamp_tz_accessor",
     &UncheckedHostThunk<CelTimestampTzAccessorImpl>::Call,
     UncheckedHostThunk<CelTimestampTzAccessorImpl>::kNumArgs},
    {"cel_wkt_unwrap_time", &UncheckedHostThunk<CelWktUnwrapTimeImpl>::Call,
     UncheckedHostThunk<CelWktUnwrapTimeImpl>::kNumArgs},
    {"cel_wkt_unwrap_wrapper",
     &UncheckedHostThunk<CelWktUnwrapWrapperImpl>::Call,
     UncheckedHostThunk<CelWktUnwrapWrapperImpl>::kNumArgs},
};

// Build the name→callback index, asserting no duplicate trampoline
// rows AND that every trampoline is in the ABI catalogue.  The
// second check is the "trampolines ⊆ catalogue" half of the
// bijection (an extra trampoline codegen never imports is dead
// code).  The "catalogue ⊆ trampolines" half is the per-entry
// check in `RegisterCelHostImports`.
absl::flat_hash_map<absl::string_view, const HostTrampoline*>
BuildHostTrampolineIndex() {
  absl::flat_hash_map<absl::string_view, const HostTrampoline*> idx;
  idx.reserve(std::size(kHostTrampolines));
  for (const HostTrampoline& t : kHostTrampolines) {
    const bool inserted = idx.emplace(t.name, &t).second;
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
  for (const abi::CelRuntimeFunction& h : abi::CelHostFunctions()) {
    auto it = by_name.find(h.name());
    ABSL_CHECK(it != by_name.end())
        << "abi::CelHostFunctions() entry `" << h.name()
        << "` has no trampoline in cel_host_wasmtime.cc::kHostTrampolines";
    const HostTrampoline& t = *it->second;
    // The thunk's arity is derived from the Layer-2 impl's C++
    // signature at compile time; the catalogue's num_args() is the
    // wasm-side contract.  Drift between them would read off the
    // end of the raw arg array — crash here instead.
    ABSL_CHECK(t.arity == h.num_args())
        << "cel_host." << h.name() << ": unchecked thunk arity " << t.arity
        << " != catalogue num_args " << h.num_args();
    if (auto s = DefineHostFunc(linker, h.name(), h.num_args(), t.cb, env);
        !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

}  // namespace celwasm
