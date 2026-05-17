#ifndef CELWASM_COMPILER_V2_CODEGEN_EXPR_LOWER_H_
#define CELWASM_COMPILER_V2_CODEGEN_EXPR_LOWER_H_

// Lowers a fully-resolved, fully-laid-out `TypedAst` into the `$eval`
// wasm function.  M1 handles only the `kConst` arm: every literal's
// value lives at a known rodata offset, so `$eval`'s body is a two-
// instruction block — `call $cel_reset(<arena_base>, <arena_limit>)`
// followed by `i32.const <root_rodata_offset>` — and the function
// returns the root literal's CelValue offset.
//
// Non-kConst expression kinds return `absl::UnimplementedError` naming
// the kind.  This is a designed rejection path, not a stub crash: the
// checker accepts `1 + 2` (a kCall) but M1 compilation of arithmetic
// lands at M3/M4, and the CLI is expected to surface the unimplemented
// status to the user.
//
// Caller responsibilities (M1): before calling `LowerToEvalFunction`
// the caller must have installed memory + the `cel.cel_reset` function
// import on `mod`.  `LowerToEvalFunction` only adds the function
// definition; the export is left to the caller so CLI callers can
// export under a different external name if they choose.

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/module.h"
#include "compiler_v2/codegen/overload_table.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

// The internal-name codegen uses when it emits `BinaryenCall` targeting
// the runtime's `cel_reset`.  Callers that install the import under a
// different internal name will produce a module Binaryen rejects at
// validate time.
inline constexpr absl::string_view kCelResetInternalName = "cel_reset";

// cel_host.cel_get_field + cel_host.cel_has_field trampolines
// (Layer 3).  Internal names codegen references; the matching
// imports carry signature `(i32, i32, i32, i32) -> ()` under wasm
// module `cel_host` — see compile.cc::InstallHostAbi.
inline constexpr absl::string_view kCelHostGetFieldInternalName =
    "cel_get_field";
inline constexpr absl::string_view kCelHostHasFieldInternalName =
    "cel_has_field";

// M7.A: cel_host.cel_make_message trampoline (Layer 3).  Two-arg
// `(type_id, out_slot)` — the host resolves type_id against the
// per-Instance `cel.abi.types[]` lookup table, allocates a default
// proto via MessageFactory::GetPrototype()->New(), wraps it in an
// owning HostMessageBacking, interns the backing, and writes a
// CEL_MESSAGE CelValue with the interned msg_slot to out_slot.
inline constexpr absl::string_view kCelHostMakeMessageInternalName =
    "cel_make_message";

// M7.B: cel_host.cel_set_field trampoline (Layer 3).  Three-arg
// `(msg_slot, field_ref_id, value_slot)` — dispatches on the
// resolved FieldDescriptor's cpp_type to pick the matching
// Reflection setter (SetBool / SetInt32 / SetString / SetEnumValue
// / etc.).  No out_slot — the message is mutated in place at the
// OwnedProtoBacking carried by msg_slot.  Wasm trap on
// repeated/map/message field types until M7.C/E ship those.
inline constexpr absl::string_view kCelHostSetFieldInternalName =
    "cel_set_field";

// M7B polish: cel_host.cel_wkt_unwrap_time trampoline (Layer 3).
// Two-arg `(out_slot, msg_slot)` — reads the CelValue at msg_slot,
// if it's a CEL_MESSAGE of well-known time type
// (`google.protobuf.Timestamp` / `Duration`) extracts (seconds,
// nanos) via reflection and writes a `CEL_TIMESTAMP` /
// `CEL_DURATION` CelValue at out_slot.  Used by the kStructExpr
// lowering to bridge the cross-form equivalence pinned by
// cel-cpp's behavior (see m7b §3.4 + the empirical probe at
// `compiler_v2/throwaway/cel_cpp_corner_probe.cc`).  No-op for
// non-WKT-time messages (codegen only emits the call when
// `s.name()` matches one of the two WKT FQNs).
inline constexpr absl::string_view kCelHostWktUnwrapTimeInternalName =
    "cel_wkt_unwrap_time";

// M3.F: runtime entry points for map literal construction +
// indexing.  All three lookups carry signature
// `(i32 out_slot, i32 map_slot, i32 key_slot) -> ()`.  cel_map_create
// is `(i32 out_slot, i32 capacity) -> ()`; cel_map_insert is
// `(i32 map_slot, i32 key_slot, i32 value_slot) -> ()`.
inline constexpr absl::string_view kCelMapCreateInternalName = "cel_map_create";
inline constexpr absl::string_view kCelMapInsertInternalName = "cel_map_insert";
inline constexpr absl::string_view kCelMapLookupArenaInternalName =
    "cel_map_lookup_arena";
inline constexpr absl::string_view kCelMapLookupInternalName =
    "cel_map_lookup";  // kDynamic dispatcher
inline constexpr absl::string_view kCelHostMapLookupInternalName =
    "cel_host_cel_map_lookup";  // kHost arm import

// M4.F: runtime entry points for list literal construction +
// indexing.  cel_list_create is `(i32 out_slot, i32 count) -> ()`;
// cel_list_set is `(i32 list_slot, i32 index, i32 elem_slot) -> ()`.
// All three indexers carry signature
// `(i32 out_slot, i32 list_slot, i32 index_slot) -> ()`.
inline constexpr absl::string_view kCelListCreateInternalName =
    "cel_list_create";
inline constexpr absl::string_view kCelListSetInternalName = "cel_list_set";
inline constexpr absl::string_view kCelListAtArenaInternalName =
    "cel_list_at_arena";
inline constexpr absl::string_view kCelListAtInternalName =
    "cel_list_at";  // kDynamic dispatcher
inline constexpr absl::string_view kCelHostListAtInternalName =
    "cel_host_cel_list_at";  // kHost arm import

// One row of the field intern table, one per kSelect emitted by
// `LowerToEvalFunction`.  Index 0 is a reserved "not proto-resolvable"
// sentinel; rows [1..N] are the ids the emitted `cel_get_field` calls
// reference.  M2.C.4 serialises this vector into `cel.abi.fields[]`.
struct FieldRefRow {
  uint32_t field_number = 0;  // proto wire number, or 0 for non-proto
  std::string name;           // always populated
  std::string owner_fqn;      // FQN of the owning message type, or ""
};

struct LoweringOptions {
  // Total linear-memory size in bytes.  `cel_reset` is called with
  // `(arena_base, arena_limit = mem_size_bytes)` at the top of every
  // `$eval` body so the runtime arena spans `[arena_base, mem_size_bytes)`.
  // Default is one wasm page (64 KiB) — enough for M1 (pure literal
  // eval touches only rodata) and the M2/M3 surface covered by the
  // e2e suite.
  uint32_t mem_size_bytes = 64u * 1024u;
};

struct LoweredFunction {
  // Binaryen-owned handle; callers can look the function up later via
  // `BinaryenGetFunction(mod.raw(), name)`.  The function's signature
  // is `() -> i32` — the CelValue offset of the root expression.
  BinaryenFunctionRef absl_nonnull func;

  // Field intern table populated during kSelect lowering.  Size == 0
  // means "no selects emitted"; otherwise `field_refs[0]` is the
  // sentinel and `field_refs[1..N]` are the referenced rows.  Consumed
  // by `BuildCelAbi` at M2.C.4.
  std::vector<FieldRefRow> field_refs;
};

// Adds a nullary `$eval` function named `func_name` to `mod`.  The
// function body is a block of type `i32`:
//
//   (block $eval (result i32)
//     (call $cel_reset (i32.const <arena_base>) (i32.const <arena_limit>))
//     (i32.const <root_rodata_offset>))
//
// Fails with `UnimplementedError` for any expression kind outside the
// M1 subset (kConst only).  Fails with `InvalidArgumentError` if the
// root expression has no storage annotation (LayoutPass was skipped)
// or its storage is not `kStaticRodata` (impossible at M1 but checked
// defensively — a later milestone accidentally flowing a workspace
// offset through here would otherwise emit a subtly wrong i32.const).
ABSL_MUST_USE_RESULT absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod,
    const OverloadTable& overload_table, const LoweringOptions& opts = {});

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_EXPR_LOWER_H_
