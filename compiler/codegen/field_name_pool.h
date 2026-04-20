// Dedup table for `(proto_field_number, field_name)` pairs referenced
// by a single eval module.  Codegen interns each distinct pair the
// lowered expression reads; the returned `intern_id` becomes the i32
// argument the emitted wasm passes to `cel_host.get_field` /
// `cel_host.has_field` in place of a raw proto field number.
//
// Why name *and* number (not just one):
//   - Names are what cel-cpp's `AttributePattern` matches on, so the
//     host needs them for partial-eval qualifier-path matching
//     regardless of the backing representation.
//   - Numbers let proto-backed hosts cache a `FieldDescriptor*` lookup
//     at instantiation time instead of a per-call `FindFieldByName`.
//     When `field_number == 0` the name is authoritative — the host
//     resolves by name only, which is the forward-compat path for
//     JSON / map / comprehension backings that do not carry proto
//     field numbers.
//
// Determinism is load-bearing: the pool emitted into the `cel.abi`
// section has to match the IDs the wasm call sites use.  Both paths
// build the pool by walking the same `TypedAst` via `FromTypedAst`,
// so IDs match by construction.

#ifndef CELWASM_COMPILER_CODEGEN_FIELD_NAME_POOL_H_
#define CELWASM_COMPILER_CODEGEN_FIELD_NAME_POOL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

class FieldNamePool {
 public:
  struct Entry {
    uint32_t field_number;  // 0 ⇒ not proto-resolvable; name is authoritative.
    std::string name;
  };

  FieldNamePool() = default;

  // Returns the intern_id for `(field_number, name)`, allocating a
  // fresh entry at the tail if the pair has not been seen.  IDs are
  // assigned densely from zero in insertion order.
  uint32_t Intern(uint32_t field_number, absl::string_view name);

  // Builds a pool by walking `ast` in pre-order and interning every
  // `SelectExpr` field reference found.  The walk order is the single
  // source of truth for intern-ID assignment: codegen and
  // `BuildCelAbi` both invoke this so their IDs agree by
  // construction.  `SelectExpr` nodes whose `NodeAnnotation` resolved
  // a proto field carry the concrete `field_number`; nodes without a
  // resolved number (future non-proto backings) are stored with
  // `field_number = 0` and the raw `select.field()` name.
  static FieldNamePool FromTypedAst(const TypedAst& ast);

  absl::Span<const Entry> entries() const {
    return entries_;
  }

 private:
  std::vector<Entry> entries_;
  // Key: "<field_number>:<name>" — both are needed since names can
  // collide across message types (e.g. `id` on Customer vs Address).
  absl::flat_hash_map<std::string, uint32_t> index_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_FIELD_NAME_POOL_H_
