#ifndef CELWASM_COMPILER_IR_ANNOTATIONS_H_
#define CELWASM_COMPILER_IR_ANNOTATIONS_H_

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
  // `optional<T>`-typed nodes.  Stamped by `ReprOf` when the type is
  // the cel-cpp `AbstractType` named `"optional_type"`.  Codegen
  // branches on this in `EmitKSelect` / `EmitKIndexCall` to route
  // through `cel_select_optional_field_at_vv`, whose runtime
  // contract handles the unwrap internally.
  kOptional,
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
// Populated by ResolvePass's MapOriginVisitor / ListOriginVisitor.
// Direct producers (kCreateMap / kCreateList) stamp `kArena`;
// `kSelect` / `kIdent` nodes whose result type is `map<K,V>` /
// `list<T>` stamp `kHost`; everything else (ternary, comprehension
// result, dyn-call) stays `kDynamic` and routes through the
// runtime dispatcher.
//
// See `rewrite/map-list-dispatch.md` for the full inference rule
// set and `rewrite/m2-ident-select-unknowns.md` §2.6 / §2.8 for
// the kSelect path.
enum class Origin : uint8_t {
  kDynamic = 0,  // default — could be arena or host, decided by runtime
  kArena = 1,    // arena-backed (kCreateMap / kCreateList / …)
  kHost = 2,     // host-backed (proto field read, Activation::Bind)
};

absl::string_view OriginName(Origin o);

// Per-node facts populated by ResolvePass + LayoutPass.  Zero
// sentinels mean "not applicable to this kind".
struct NodeAnnotation {
  Repr repr = Repr::kUnknown;
  uint32_t field_number = 0;  // SelectExpr proto field number.
  // CallExpr's resolved cel-cpp overload id, e.g. "add_int64".
  // ResolvePass populates this from `cel::Ast::reference_map()`'s
  // first overload string; codegen looks it up in `OverloadTable`.
  // Empty for non-call nodes.  String_view points into cel-cpp's
  // owned reference_map storage; lifetime tied to the TypedAst.
  absl::string_view overload_id;
  uint32_t local_index = 0;   // IdentExpr resolved wasm local.
  uint32_t scope_id = 0;      // comprehension scope id.
  uint32_t attribute_id = 0;  // interned AttributeId; 0 = none.
  // Dense index into `cel.abi.types[]` populated by ResolvePass's
  // MessageTypeIdVisitor for kStructExpr nodes; 0 = none (any
  // non-struct node).  Codegen reads this in the kStructExpr arm
  // to emit `cel_host.cel_make_message(type_id, out_slot)`; the
  // host resolves the id → Descriptor* against the descriptor pool
  // at Plan time.  See `rewrite/m7-proto-literals.md` §4.2.
  uint32_t message_type_id = 0;
  Storage storage;
  // Map/list dispatch origin — see
  // `rewrite/map-list-dispatch.md` and
  // `rewrite/m2-ident-select-unknowns.md` §2.6 / §2.8.
  Origin map_origin = Origin::kDynamic;
  Origin list_origin = Origin::kDynamic;
  // Base wasm-local index for a `kComprehensionExpr` node's
  // auxiliary locals (end_off, iter cursor, index counter).
  // `LayoutPass`'s ComprehensionLocalsVisitor stamps this with the
  // first of `StaticLayout::comprehension_extra_locals_per_comp`
  // consecutive locals.  Zero on non-comprehension nodes.
  uint32_t comp_aux_local_base = 0;
  // Per-comprehension iter_var / accu_var bindings.  Populated by
  // ResolvePass's ScopedIdentResolver at PreVisitComprehension time.
  // Required because nested comprehensions can share accu_var names
  // (`@result` at every depth in cel-cpp's standard macros), so
  // name-based lookup would conflate the inner's binding with the
  // outer's.  Zero on non-comprehension nodes.  iter_var2 reuses
  // `comp_aux_local_base + 1` as the index-counter slot for
  // list-source two-iter-var, or as the value workspace for
  // map-source.
  uint32_t comp_iter_local_index = 0;
  uint32_t comp_accu_local_index = 0;
  // iter_var2's local_index (zero for single-iter-var
  // comprehensions).  For list source: iter_var binds to the
  // synthesized index counter, iter_var2 to the value pointer.
  // For map source: iter_var binds to key, iter_var2 to value.
  uint32_t comp_iter2_local_index = 0;
  // Absolute linear-memory offset of a rodata CelValue holding the
  // kSelectExpr's field name as a CEL_STRING.  Allocated by
  // `LayoutPass::SelectKeyRodataVisitor` only when the operand has
  // `Repr::kOptional` — that's the only codegen path that needs a
  // key_slot CelValue (the `cel_select_optional_field_at_vv` kernel
  // ABI; rationale + memory map in
  // `wat/m14_optional_select_field.wat`).  Zero on every other
  // kSelect node.
  uint32_t select_key_rodata_offset = 0;
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

#endif  // CELWASM_COMPILER_IR_ANNOTATIONS_H_
