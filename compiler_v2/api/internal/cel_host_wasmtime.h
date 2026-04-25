// Layer 3 — wasmtime glue for cel_host trampolines.
//
// Converts the wasmtime callback calling convention (`void* env`,
// `wasmtime_caller_t*`, `wasmtime_val_t` args) into Layer 2's
// abstract `TrampolineContext` + `CelGetFieldImpl` / `CelHasFieldImpl`.
// Engine::Plan owns a `CelHostCallbackEnv` per Instance; the linker
// callback borrows it by pointer.

#ifndef CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_WASMTIME_H_
#define CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_WASMTIME_H_

#include <memory>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/api/internal/cel_host.h"
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

  // M3.E: independent slot namespace for map backings — see
  // ExternrefTable::InternMap docs.
  uint32_t InternMap(std::shared_ptr<const HostMapBacking> backing) override;
  const HostMapBacking* absl_nullable LookupMap(uint32_t slot) const override;

  void Reset() override;

 private:
  std::vector<std::shared_ptr<const HostMessageBacking>> backings_;
  std::vector<std::shared_ptr<const HostMapBacking>> map_backings_;
};

// Per-Instance payload the cel_host.cel_get_field trampoline reads.
// Populated once by Engine::Plan (from cel.abi + descriptor pool +
// the runtime's cel_alloc export) and borrowed by the linker
// callback via raw pointer.  Lives on InstanceImpl for the
// instance's lifetime.
struct CelHostCallbackEnv {
  // Storage for bindings spans.  `bindings` references these.
  std::vector<FieldRefEntry> field_refs_storage;
  std::vector<AttributeEntry> attrs_storage;
  CelHostBindings bindings;

  // Per-eval externref table — Reset() between Evals.
  HostExternrefTable refs;

  // Filled by Engine::Plan after the runtime + expr instances are
  // ready.  `memory` is the host-owned linear-memory handle both
  // modules share; `cel_alloc_fn` is the runtime export bound onto
  // the linker at InstantiateRuntime time.
  wasmtime_memory_t memory = {};
  wasmtime_func_t cel_alloc_fn = {};
};

// Build bindings from a decoded CelAbi.  `pool` is consulted only
// to validate owner_fqn entries; M2 ProtoBacking dispatches via the
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

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_WASMTIME_H_
