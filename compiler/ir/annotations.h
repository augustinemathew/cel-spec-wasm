#ifndef CELWASM_COMPILER_IR_ANNOTATIONS_H_
#define CELWASM_COMPILER_IR_ANNOTATIONS_H_

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"

namespace celwasm {

// ABI representation for a CEL value at a WASM call boundary.
//
// Every checked expression node carries exactly one `Repr` (derived from its
// static type).  Codegen and the host ABI dispatch on this enum to decide
// whether a value travels as an immediate scalar, a (ptr,len) pair into linear
// memory, or an externref slot.
enum class Repr : uint8_t {
  kUnknown = 0,  // type checker left the node as DYN
  kNull,         // null_type
  kBool,         // i32 {0, 1}
  kInt,          // i64 signed
  kUint,         // i64 unsigned
  kDouble,       // f64
  kString,       // linear memory (char*, i32 len)
  kBytes,        // linear memory (uint8_t*, i32 len)
  kList,         // linear memory CelList header
  kMap,          // linear memory CelMap header
  kMessage,      // externref slot (i32 index into $cel_refs table)
  kEnum,         // i64 (widened, signed)
  kDuration,     // i64 nanoseconds
  kTimestamp,    // i64 microseconds since unix epoch
  kType,         // i32 interned type id
};

absl::string_view ReprName(Repr r);

// Per-node annotation derived from the CheckedExpr AST plus the compiler's own
// analysis.  Side-mapped by `cel::ExprId`.
//
// M1 populates `repr` only.  M3 Slice G2 adds `field_number` for `SelectExpr`
// nodes (see `doc/implementation-plan/m3-proto-and-strings.md`, Option B).
// Later milestones add:
//   uint32_t attribute_id;   // §8 host ABI interning
//   uint32_t pattern_id;     // §12 custom fn pattern interning
//   uint32_t scope_depth;    // §5.4 comprehension scoping
//   std::string local_name;  // freshened iter_var for codegen
struct NodeAnnotation {
  Repr repr = Repr::kUnknown;

  // Proto field number resolved for `SelectExpr` nodes.  Zero means "not a
  // SelectExpr" or "could not be resolved" — proto field numbers start at 1,
  // so zero is an unambiguous sentinel.  Populated in `PopulateAnnotations`
  // while the descriptor pool is still live; codegen consumes it as a pure
  // lookup when lowering `kSelectExpr` to `cel_host.get_field`.
  uint32_t field_number = 0;
};

// Side map keyed by expression id (`cel::ExprId`).
class WasmAnnotations {
 public:
  NodeAnnotation& operator[](int64_t expr_id) {
    return nodes_[expr_id];
  }

  const NodeAnnotation* Find(int64_t expr_id) const {
    auto it = nodes_.find(expr_id);
    if (it == nodes_.end()) return nullptr;
    return &it->second;
  }

  const absl::flat_hash_map<int64_t, NodeAnnotation>& nodes() const {
    return nodes_;
  }

 private:
  absl::flat_hash_map<int64_t, NodeAnnotation> nodes_;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_IR_ANNOTATIONS_H_
