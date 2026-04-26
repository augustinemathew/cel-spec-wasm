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

// Where a map- or list-typed node's backing lives at runtime.
// Populated by ResolvePass in a forward-compat slot for M6 (map /
// list dispatch).  M2 only writes `kHost` on `kSelect` / `kIdent`
// nodes whose result type is `map<K,V>` / `list<T>` — every other
// map/list-producing kind (kCreateMap, kComprehension, kCall,
// ternary, …) lands in M5/M6 and stays `kDynamic` until then.
//
// See `doc/implementation-plan/rewrite/map-list-dispatch.md` for the
// full inference rule set and `m2-ident-select-unknowns.md` §2.6 +
// §2.8 for what M2 actually populates.
enum class Origin : uint8_t {
  kDynamic = 0,  // default — could be arena or host, decided by runtime
  kArena = 1,    // arena-backed (kCreateMap / kCreateList / …)
  kHost = 2,     // host-backed (proto field read, Activation::Bind)
};

absl::string_view OriginName(Origin o);

// Per-node facts populated by ResolvePass + LayoutPass.  The schema is
// intentionally the final shape from M1 even though M1 only writes
// `repr` (for kConst nodes) and `storage` (kStaticRodata for kConst).
// Later milestones fill the other fields; zero sentinels mean "not
// applicable to this kind".
struct NodeAnnotation {
  Repr repr = Repr::kUnknown;
  uint32_t field_number = 0;  // SelectExpr proto field number (M2)
  // CallExpr's resolved cel-cpp overload id, e.g. "add_int64".  M5.F
  // ResolvePass populates this from `cel::Ast::reference_map()`'s
  // first overload string; codegen looks it up in `OverloadTable`.
  // Empty for non-call nodes.  String_view points into cel-cpp's
  // owned reference_map storage; lifetime tied to the TypedAst.
  absl::string_view overload_id = {};
  uint32_t local_index = 0;   // IdentExpr resolved wasm local (M2)
  uint32_t scope_id = 0;      // comprehension scope (later)
  uint32_t attribute_id = 0;  // interned AttributeId (M2.E); 0 = none
  Storage storage;
  // Forward-compat hooks for map/list dispatch — see
  // m2-ident-select-unknowns.md §2.6 / §2.8.
  Origin map_origin = Origin::kDynamic;
  Origin list_origin = Origin::kDynamic;
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
