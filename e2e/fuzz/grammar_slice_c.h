#ifndef CELWASM_E2E_FUZZ_GRAMMAR_SLICE_C_H_
#define CELWASM_E2E_FUZZ_GRAMMAR_SLICE_C_H_

// Slice C catalog — Slice B (scalars) plus the aggregate AST
// kinds and the comprehension macros.  C1 (this header) covers:
//
//   - kListExpr — `list<T>` literals for every scalar T
//   - kMapExpr  — `map<K,V>` literals for a realistic K×V subset
//   - kCallExpr — `size(list/map)`, `_in_` on list/map
//   - kComprehensionExpr — `exists`, `all`, `exists_one`,
//     `filter`, `map` over lists; `exists`, `all`, `exists_one`
//     over maps (iter_var is the key)
//
// kStructExpr / kSelectExpr / `has()` / Customer activation are
// deferred to Slice C2 — they need proto-message marshalling on
// both the our-side activation and the cel-cpp `OracleVar` side,
// which is a separate plumbing task.
//
// Per the m27 §"Guarded productions" policy, every production
// admitted here is **total over its typed input domain**.  No
// indexing (`xs[i]` could be OOB), no map lookup (`m[k]` could
// be missing), no map-V2 two-iter comprehensions (the index
// arithmetic introduces partiality).  Coverage of those shapes
// stays in the hand-written e2e suites (m4 / m7 / m12) where the
// fixture pins specific bounds.
//
// L1 / L2 / L3 are extended in `grammar_test.cc` to run against
// `BuildSliceCGrammar` before the oracle property fires any
// iteration.  See m27 §"Grammar validation" for the
// discipline.

#include <utility>
#include <vector>

#include "e2e/fuzz/grammar.h"
#include "e2e/fuzz/grammar_slice_b.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// The Slice C activation — Slice B's scalar bindings, unchanged.
// C2 will add `c: celwasm.testdata.Customer`.
std::vector<ActivationBinding> SliceCActivation();

// Build the Slice C grammar.  Starts from Slice B's productions
// (so every scalar-side rule is identical and Slice B
// regressions still light up), then registers the aggregate
// constructors, the size / in operators, and the comprehension
// macros.  ABSL_CHECKs internally if `Grammar::Validate` (L1)
// rejects the catalog.
Grammar BuildSliceCGrammar();

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_GRAMMAR_SLICE_C_H_
