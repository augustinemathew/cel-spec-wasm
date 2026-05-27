// Layer 3 — wasmtime glue for cel_host trampolines.
//
// Converts the wasmtime callback calling convention (`void* env`,
// `wasmtime_caller_t*`, `wasmtime_val_t` args) into Layer 2's
// abstract `TrampolineContext` + `CelGetFieldImpl` / `CelHasFieldImpl`.
// Engine::Plan owns a `CelHostCallbackEnv` per Instance; the linker
// callback borrows it by pointer.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_WASMTIME_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_WASMTIME_H_

#include <cstring>
#include <memory>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "eval/internal/cel_host.h"
#include "google/protobuf/descriptor.h"
#include "wasmtime.h"

namespace celwasm {

// Production ExternrefTable — vector-backed, slot 0 sentinel.
// Matches the test fake's shape 1:1; promoted here for reuse by
// Layer 3 (test fake stays local to cel_host_test).
class HostExternrefTable final : public ExternrefTable {
 public:
  HostExternrefTable();

  uint32_t Intern(std::shared_ptr<const HostMessageBacking> backing) override;
  const HostMessageBacking* absl_nullable Lookup(uint32_t slot) const override;

  // Independent slot namespace for map backings — see
  // ExternrefTable::InternMap docs.
  uint32_t InternMap(std::shared_ptr<const HostMapBacking> backing) override;
  const HostMapBacking* absl_nullable LookupMap(uint32_t slot) const override;

  // Independent slot namespace for list backings.
  uint32_t InternList(std::shared_ptr<const HostListBacking> backing) override;
  const HostListBacking* absl_nullable LookupList(uint32_t slot) const override;

  void Reset() override;

 private:
  std::vector<std::shared_ptr<const HostMessageBacking>> backings_;
  std::vector<std::shared_ptr<const HostMapBacking>> map_backings_;
  std::vector<std::shared_ptr<const HostListBacking>> list_backings_;
};

// Per-Instance payload the cel_host.cel_get_field trampoline reads.
// Populated once by Engine::Plan (from cel.abi + descriptor pool +
// the runtime's arena_alloc export) and borrowed by the linker
// callback via raw pointer.  Lives on InstanceImpl for the
// instance's lifetime.
struct CelHostCallbackEnv {
  // Storage for bindings spans.  `bindings` references these.
  std::vector<FieldRefEntry> field_refs_storage;
  std::vector<AttributeEntry> attrs_storage;
  // type_id → resolved descriptor lookup, populated by
  // `BuildCelHostBindings` from `cel.abi.types[]` + the embedder-
  // supplied descriptor pool.  The trampoline reads this at
  // `cel_make_message` call time; entries with `descriptor=nullptr`
  // are FQNs the pool didn't recognise (kTypeMismatch CEL_ERROR
  // surface).
  std::vector<MessageTypeEntry> message_types_storage;
  CelHostBindings bindings;

  // Per-eval externref table — Reset() between Evals.
  HostExternrefTable refs;

  // Filled by Engine::Plan after the runtime + expr instances are
  // ready.  `memory` is the runtime-owned shared linear-memory
  // pointer both modules share; `arena_alloc_fn` is the runtime
  // export bound onto the linker at InstantiateRuntime time.
  // Borrowed pointer (the refcount is held by `InstanceImpl::memory`
  // for the instance's lifetime; this struct is part of
  // `InstanceImpl` so the pointer stays valid).
  wasmtime_sharedmemory_t* memory = nullptr;
  wasmtime_func_t arena_alloc_fn = {};
  // Handle for the runtime's `malloc` export.  Used by
  // Instance::Eval to allocate / grow the activation buffer (where
  // kString / kBytes payloads from Activation get marshalled before
  // each Eval).  The buffer lives outside the bump arena because
  // arena_reset (the first instruction of $eval) would wipe it.
  // See `doc/implementation-plan/rewrite/wasi/DESIGN.md` for the
  // activation-buffer ownership model.
  wasmtime_func_t malloc_fn = {};
};

// Wasmtime-backed `MemoryView` over a module's shared linear memory.
// All reads / writes are bitwise memcpy at a byte offset (the wire
// CelValue is fixed-layout, LE-pinned in cel_data.h).  Used by every
// Layer-3 trampoline — the cel_host field/aggregate trampolines and
// the user `@host` callback trampoline — to read arg slots and write
// the out slot.
class WasmtimeMemoryView final : public MemoryView {
 public:
  // `ctx` is accepted for call-site symmetry with the arena allocator
  // (and a possible future bounds-checked read), but shared-memory data
  // is reachable without it via `wasmtime_sharedmemory_data`.
  WasmtimeMemoryView(wasmtime_context_t* absl_nonnull /*ctx*/,
                     wasmtime_sharedmemory_t* absl_nonnull mem)
      : mem_(mem) {}

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
  uint8_t* absl_nonnull Data() const {
    return wasmtime_sharedmemory_data(mem_);
  }
  wasmtime_sharedmemory_t* absl_nonnull mem_;
};

// Wasmtime-backed `ArenaAllocator` — calls the runtime's
// `arena_alloc(size) -> offset` wasm export by reentering wasm from
// the host.  Used both by host trampolines and by
// `Instance::Eval(Activation)` when marshalling kString / kBytes
// activation values (canonical reentry pattern).  Slot-into-
// `out_offset` contract matches `EncodeSpan` in cel_host.cc — zero-
// byte alloc still succeeds with a valid offset; OOM / trap returns
// nullptr.
class WasmtimeArenaAllocator final : public ArenaAllocator {
 public:
  WasmtimeArenaAllocator(wasmtime_context_t* absl_nonnull ctx,
                         wasmtime_func_t fn, wasmtime_sharedmemory_t* mem)
      : ctx_(ctx), fn_(fn), mem_(mem) {}

  uint8_t* absl_nullable Alloc(size_t len,
                               uint32_t* absl_nonnull out_offset) override;

 private:
  wasmtime_context_t* absl_nonnull ctx_;
  wasmtime_func_t fn_;
  wasmtime_sharedmemory_t* mem_;
};

// Build bindings from a decoded CelAbi.  `pool` is consulted only
// to validate owner_fqn entries; `ProtoBacking` dispatches via the
// bound Message's own `GetDescriptor()`, so the lookup here is a
// defensive sanity check (can be `generated_pool()` or any pool
// the embedder configured).  Unknown FQNs are tolerated — the
// trampoline surfaces a runtime CEL_ERROR rather than failing Plan.
void BuildCelHostBindings(const celwasm::abi::CelAbi& abi,
                          const google::protobuf::DescriptorPool* pool,
                          CelHostCallbackEnv& out);

// Register `cel_host.cel_get_field` on `linker` with a callback
// that dispatches into Layer 2's `CelGetFieldImpl`.  `env` must
// outlive every eval through the resulting instance.
ABSL_MUST_USE_RESULT absl::Status RegisterCelHostImports(
    wasmtime_linker_t* absl_nonnull linker,
    CelHostCallbackEnv* absl_nonnull env);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_WASMTIME_H_
