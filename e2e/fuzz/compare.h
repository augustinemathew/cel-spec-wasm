#ifndef CELWASM_E2E_FUZZ_COMPARE_H_
#define CELWASM_E2E_FUZZ_COMPARE_H_

// Recursive, type-driven comparison of our `Value` against the
// cel-cpp oracle's proto `Value` — the verdict half of the
// differential harness.  Dispatches on the declared `CelType`:
// scalars compare by payload (doubles with NaN-agreement, matching
// the conformance discipline), `list<T>` element-wise, `map<K,V>`
// as a key-set (CEL map iteration order is not semantics), and
// nested aggregates recurse.  Used by `mine_divergences` for its
// divergence reports; built as its own library so grammar slices
// that add nested aggregate targets get comparison for free.

#include <string>

#include "cel/expr/value.pb.h"
#include "eval/value.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// One comparison verdict plus printable renders of both sides for
// the divergence log.  Renders are CEL-literal-flavoured
// (`[1, 2]`, `{"k": 0}`, `b"x"`) and bounded by the source shapes
// the grammar emits.
struct CompareResult {
  bool equal = false;
  std::string ours;
  std::string oracle;
};

// Compare `ours` against `oracle` at declared type `t`.  A kind
// mismatch on our side renders as `<wrong-kind=...>` and compares
// unequal; an unsupported `t` (no grammar emits it yet) renders as
// `<unsupported-type>` and compares unequal so a silent gap can't
// masquerade as agreement.
CompareResult Compare(const Value& ours, const cel::expr::Value& oracle,
                      const CelType& t);

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_COMPARE_H_
