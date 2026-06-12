// Test-only backdoor into `Instance`'s pImpl payload.
//
// Lets eval-level regression tests reach the per-Instance wasmtime
// handles (e.g. the shared linear-memory handle, to pin the
// base-pointer stability contract documented on
// `InstanceImpl::memory`) without widening the public `Instance`
// surface.  Linked only from `testonly` targets; never use outside
// tests.

#ifndef CELWASM_EVAL_INTERNAL_INSTANCE_TEST_PEER_H_
#define CELWASM_EVAL_INTERNAL_INSTANCE_TEST_PEER_H_

#include "eval/instance.h"
#include "eval/internal/instance_impl.h"

namespace celwasm {

struct InstanceTestPeer {
  static InstanceImpl& impl(Instance& instance) {
    return *instance.impl_;
  }
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_INSTANCE_TEST_PEER_H_
