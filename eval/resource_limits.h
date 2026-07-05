// `celwasm::ResourceLimits` — sandbox resource bounds enforced on
// every `Instance::Eval`.
//
// A single set of limits covers BOTH the expression module and any
// `@component` functions the expression calls: they share one per-Eval
// wasm store, so one deadline and one memory cap bound the whole
// evaluation.  The point is the security-model promise (see
// `doc/user-guide/security-model.md`): a `@component` is arbitrary
// guest wasm — unlike a total CEL expression, it can loop forever or
// allocate without bound — so an *untrusted* component must be held to
// a wall-clock deadline and a memory ceiling, or it can hang / OOM the
// embedder.
//
// The defaults are chosen so that loading an untrusted component is
// safe out of the box.  A fully-trusted deployment that wants maximum
// throughput and no background timer can opt out with `Unlimited()`.
//
// Configure with `Engine::Builder::WithResourceLimits(...)`.

#ifndef CELWASM_EVAL_RESOURCE_LIMITS_H_
#define CELWASM_EVAL_RESOURCE_LIMITS_H_

#include <cstdint>

#include "absl/time/time.h"

namespace celwasm {

struct ResourceLimits {
  // Wall-clock ceiling on a single evaluation.  When execution exceeds
  // it, the Eval is trapped and returns a `ResourceExhausted` status —
  // the calling thread is never left spinning.  The same bound applies
  // to per-Plan component instantiation, so a component that hangs in
  // its own constructor is caught at `Plan` too.
  //
  // Enforced via wasmtime epoch interruption: near-zero steady-state
  // cost in JIT'd code (a check at loop back-edges and call entries),
  // plus one background timer thread per Engine.  That timer is
  // idle-parked — it blocks at zero cost whenever no evaluation is in
  // flight and only ticks (at a coarse ~deadline/16 cadence) while one
  // is running — so an idle Engine costs nothing.  `absl::ZeroDuration()`
  // (or any non-positive value) disables the deadline entirely: no
  // timer thread, no checks, no per-Eval bookkeeping.
  absl::Duration max_eval_time = absl::Seconds(1);

  // Per-memory ceiling, in bytes, on wasm linear-memory growth.  It
  // applies independently to the expression runtime's memory and to
  // each component's own linear memory, so no single memory can grow
  // past it via `memory.grow`.  A growth request beyond the cap fails
  // (the guest sees allocation failure), which surfaces as a clean
  // eval error rather than host OOM.
  //
  // The default (64 MiB) sits far above the expression runtime's
  // ~128 KiB working set, leaving ample room for a component's libc++
  // heap.  `0` disables the cap.
  uint64_t max_memory_bytes = uint64_t{64} << 20;

  // The safe-by-default preset — identical to a default-constructed
  // `ResourceLimits` (1s deadline, 64 MiB per-memory cap).
  static ResourceLimits Default() {
    return ResourceLimits{};
  }

  // No enforcement: no deadline (hence no background timer thread) and
  // no memory cap.  For deployments where every loaded component is
  // trusted and raw throughput matters.  Choosing this means an
  // `@component` with an infinite loop CAN hang the calling thread —
  // only select it when that trade is acceptable.
  static ResourceLimits Unlimited() {
    return ResourceLimits{/*max_eval_time=*/absl::ZeroDuration(),
                          /*max_memory_bytes=*/0};
  }
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_RESOURCE_LIMITS_H_
