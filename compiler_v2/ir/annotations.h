#ifndef CELWASM_COMPILER_V2_IR_ANNOTATIONS_H_
#define CELWASM_COMPILER_V2_IR_ANNOTATIONS_H_

#include <cstdint>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"

namespace celwasm {

// ABI representation for a CEL value at a WASM call boundary.
//
// Every checked expression node carries exactly one `Repr` (derived from
// its static type).  Codegen and the host ABI dispatch on this enum to
// decide whether a value travels as an immediate scalar, a (ptr,len)
// pair into linear memory, or an externref slot.
enum class Repr : uint8_t {
  kUnknown = 0,
  kNull,
  kBool,
  kInt,
  kUint,
  kDouble,
  kString,
  kBytes,
  kList,
  kMap,
  kMessage,
  kEnum,
  kDuration,
  kTimestamp,
  kType,
};

absl::string_view ReprName(Repr r);

// Where a node's result CelValue lives at eval time.  Populated by
// LayoutPass.  M1 only emits `kStaticRodata` (every `kConst` node lands
// in the module's `.rodata` active data segment).
enum class StorageKind : uint8_t {
  kNone = 0,       // default / not yet populated
  kStaticRodata,   // CelValue in .rodata at a known byte offset
  kWorkspaceSlot,  // CelValue in a pre-assigned 24B workspace cell
  kLocal,          // wasm local (ident_expr only)
};

absl::string_view StorageKindName(StorageKind k);

struct Storage {
  StorageKind kind = StorageKind::kNone;
  uint32_t payload = 0;  // rodata offset | slot offset | local index
};

// Per-node facts populated by ResolvePass + LayoutPass.  The schema is
// intentionally the final shape from M1 even though M1 only writes
// `repr` (for kConst nodes) and `storage` (kStaticRodata for kConst).
// Later milestones fill the other fields; zero sentinels mean "not
// applicable to this kind".
struct NodeAnnotation {
  Repr repr = Repr::kUnknown;
  uint32_t field_number = 0;  // SelectExpr proto field number (M2)
  uint32_t overload_id = 0;   // CallExpr interned into OverloadTable (M3)
  uint32_t local_index = 0;   // IdentExpr resolved wasm local (M2)
  uint32_t scope_id = 0;      // comprehension scope (later)
  Storage storage;
};

// Side map keyed by cel::ExprId.
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

#endif  // CELWASM_COMPILER_V2_IR_ANNOTATIONS_H_
