// Layer 3 — wasmtime glue for cel_host trampolines.
//
// Converts wasmtime's unchecked (raw, unboxed) host-callback calling
// convention (`void* env`, `wasmtime_caller_t*`, `wasmtime_val_raw_t`
// args) into Layer 2's abstract `TrampolineContext` +
// `CelGetFieldImpl` / `CelHasFieldImpl` / …  Engine::Plan owns a
// `CelHostCallbackEnv` per Instance; the linker callback borrows it
// by pointer.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_WASMTIME_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_WASMTIME_H_

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "eval/internal/cel_host.h"
#include "google/protobuf/descriptor.h"
#include "wasmtime.h"

namespace celwasm {

// Production ExternrefTable — vector-backed, slot 0 sentinel.
// Matches the test fake's shape 1:1; promoted here for reuse by
// Layer 3 (test fake stays local to cel_host_test).
//
// Proto-backed interns are deduplicated by the underlying proto
// identity (see `ExternrefTable::InternProtoMessage`'s contract):
// re-interning the same `Message*` — or the same (owner, field)
// map/list pair — returns the previously-issued slot instead of
// allocating a fresh backing + vector entry.  Safe because every
// recorded pointer is anchored until the next `Reset()` (Activation
// bindings outlive the Eval; eval-constructed messages are held by
// their interned owning backing in this very table), so a recorded
// pointer can never be freed-and-reused for a different message
// within one Eval.  The dedup maps reset with the slots.
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

  // Deduping proto-identity interns (class comment above).
  uint32_t InternProtoMessage(
      const google::protobuf::Message* absl_nonnull msg) override;
  uint32_t InternProtoMapField(
      const google::protobuf::Message* absl_nonnull owner,
      const google::protobuf::FieldDescriptor* absl_nonnull field) override;
  uint32_t InternProtoListField(
      const google::protobuf::Message* absl_nonnull owner,
      const google::protobuf::FieldDescriptor* absl_nonnull field) override;

  void Reset() override;

 private:
  using ProtoFieldKey = std::pair<const google::protobuf::Message*,
                                  const google::protobuf::FieldDescriptor*>;

  std::vector<std::shared_ptr<const HostMessageBacking>> backings_;
  std::vector<std::shared_ptr<const HostMapBacking>> map_backings_;
  std::vector<std::shared_ptr<const HostListBacking>> list_backings_;
  // Per-Eval dedup indexes over the slot vectors above.  `Intern`
  // records every proto-exposing message backing (`message()` non-
  // null) here; the InternProto* fast paths consult the index before
  // allocating.  Cleared by `Reset()`.
  absl::flat_hash_map<const google::protobuf::Message*, uint32_t> msg_slots_;
  absl::flat_hash_map<ProtoFieldKey, uint32_t> map_field_slots_;
  absl::flat_hash_map<ProtoFieldKey, uint32_t> list_field_slots_;
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
  // Cached linear-memory base pointer + size snapshot, consumed by
  // every `WasmtimeMemoryView` the trampolines build (hot-path ctor).
  //
  //   - `mem_base` is fetched ONCE at Plan time
  //     (`BindRuntimeFuncHandles`): wasmtime shared memories keep a
  //     stable base pointer for the life of the memory, across
  //     memory.grow — the contract documented on
  //     `InstanceImpl::memory` and pinned by
  //     `MemoryGrowStabilityTest.BasePointerStableAcrossMidEvalGrow`.
  //   - `mem_size` is refreshed at the top of every Eval
  //     (`Instance::Eval`) and bumped monotonically by
  //     `WasmtimeMemoryView`'s refresh-on-bounds-miss path when
  //     arena allocation grows memory mid-Eval.  It only ever
  //     under-approximates the true size (memory never shrinks), so
  //     staleness can cause a refresh, never an out-of-bounds
  //     accept.
  uint8_t* mem_base = nullptr;
  uint32_t mem_size = 0;
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
//
// Base / size caching.  `wasmtime_sharedmemory_data` + `_data_size`
// used to be re-fetched on EVERY access; both are now cached:
//
//   - The BASE pointer is stable for the life of the shared memory,
//     across memory.grow (shared memories reserve their declared
//     maximum up front — contract documented on
//     `InstanceImpl::memory` and pinned by
//     `GrowKeepsBasePointerStable` / `MemoryGrowStabilityTest.
//     BasePointerStableAcrossMidEvalGrow`), so caching it is
//     unconditionally safe.  The hot-path ctor takes the per-Eval
//     cached base from `CelHostCallbackEnv::mem_base`.
//   - The SIZE grows monotonically mid-Eval (`arena_alloc` →
//     dlmalloc → memory.grow), so a cached size can go stale in only
//     one direction: it UNDER-approximates, making a bounds check
//     falsely REJECT offsets in freshly grown pages (a functional
//     bug — e.g. a map-lookup key span decoding empty — never a
//     memory-safety bug).  `IsInBounds` therefore refreshes the
//     snapshot on a miss and re-tests before rejecting; a genuine
//     OOB still rejects after the refresh.  Pinned by
//     `ViewConstructedBeforeGrowAcceptsGrownPages` /
//     `ViewStillRejectsGenuineOobAfterGrow` and, e2e,
//     `TrampolineReadsSpanFromPagesGrownMidEval`.
class WasmtimeMemoryView final : public MemoryView {
 public:
  // Self-contained ctor: snapshots base + size from `mem` at
  // construction.  `ctx` is accepted for call-site symmetry with the
  // arena allocator, but shared-memory data is reachable without it.
  WasmtimeMemoryView(wasmtime_context_t* absl_nonnull /*ctx*/,
                     wasmtime_sharedmemory_t* absl_nonnull mem)
      : mem_(mem),
        base_(wasmtime_sharedmemory_data(mem)),
        own_size_(static_cast<uint32_t>(wasmtime_sharedmemory_data_size(mem))),
        size_cache_(&own_size_) {}

  // Hot-path ctor: `base` is the Plan-time cached base pointer and
  // `size_cache` the per-Eval size snapshot, both living on
  // `CelHostCallbackEnv` (`mem_base` / `mem_size`).  Routing the
  // refresh-on-miss through the env's slot lets one trampoline's
  // refresh benefit every later trampoline in the same Eval.
  WasmtimeMemoryView(wasmtime_sharedmemory_t* absl_nonnull mem,
                     uint8_t* absl_nonnull base,
                     uint32_t* absl_nonnull size_cache)
      : mem_(mem), base_(base), size_cache_(size_cache) {}

  // May lag the true size while memory grows mid-Eval; monotonic-safe
  // snapshot (see class comment).  `IsInBounds` below is the
  // authoritative predicate.
  uint32_t Size() const override {
    return *size_cache_;
  }

  bool IsInBounds(uint32_t ptr, uint32_t len) const override {
    if (InBoundsAgainst(*size_cache_, ptr, len)) return true;
    // Miss: the snapshot may be stale — memory only ever GROWS, so
    // re-fetch once and re-test before rejecting.  The base pointer
    // needs no refresh (stable across grow; class comment).
    *size_cache_ = static_cast<uint32_t>(wasmtime_sharedmemory_data_size(mem_));
    return InBoundsAgainst(*size_cache_, ptr, len);
  }

  CelValue ReadCelValue(uint32_t offset) const override {
    if (!IsInBounds(offset, sizeof(CelValue))) return CelValue{};
    CelValue cv{};
    std::memcpy(&cv, base_ + offset, sizeof(cv));
    return cv;
  }
  void WriteCelValue(uint32_t offset, const CelValue& v) override {
    if (!IsInBounds(offset, sizeof(CelValue))) return;
    std::memcpy(base_ + offset, &v, sizeof(v));
  }
  void WriteU32(uint32_t offset, uint32_t value) override {
    if (!IsInBounds(offset, sizeof(value))) return;
    std::memcpy(base_ + offset, &value, sizeof(value));
  }
  absl::string_view ReadSpan(uint32_t ptr, uint32_t len) const override {
    if (!IsInBounds(ptr, len)) return {};
    return {reinterpret_cast<const char*>(base_ + ptr), len};
  }

 private:
  // Bounds test against an explicit size; subtraction form avoids
  // `ptr + len` u32 overflow (mirrors MemoryView::IsInBounds).
  static bool InBoundsAgainst(uint32_t size, uint32_t ptr, uint32_t len) {
    return len == 0 || (ptr <= size && len <= size - ptr);
  }

  wasmtime_sharedmemory_t* absl_nonnull mem_;
  uint8_t* absl_nonnull base_;
  // Backing slot for the self-contained ctor; `mutable` because the
  // refresh-on-miss path writes through `size_cache_` from const
  // methods.
  mutable uint32_t own_size_ = 0;
  uint32_t* absl_nonnull size_cache_;
};

// Wasmtime-backed `ArenaAllocator` — calls the runtime's
// `arena_alloc(size) -> offset` wasm export by reentering wasm from
// the host.  Used both by host trampolines and by
// `Instance::Eval(Activation)` when marshalling kString / kBytes
// activation values (canonical reentry pattern).  Slot-into-
// `out_offset` contract matches `EncodeSpan` in cel_host_common.cc —
// zero-byte alloc still succeeds with a valid offset; OOM / trap
// returns nullptr.
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
