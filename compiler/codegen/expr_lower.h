#ifndef CELWASM_COMPILER_CODEGEN_EXPR_LOWER_H_
#define CELWASM_COMPILER_CODEGEN_EXPR_LOWER_H_

// Lowers a fully-resolved, fully-laid-out `TypedAst` into the `$eval`
// wasm function.  M1 handles only the `kConst` arm: every literal's
// value lives at a known rodata offset, so `$eval`'s body is a two-
// instruction block — `call $arena_reset(<arena_base>, <arena_limit>)`
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
// the caller must have installed memory + the `cel.arena_reset` function
// import on `mod`.  `LowerToEvalFunction` only adds the function
// definition; the export is left to the caller so CLI callers can
// export under a different external name if they choose.

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "binaryen-c.h"
#include "compiler/codegen/layout_pass.h"
#include "compiler/codegen/module.h"
#include "compiler/codegen/overload_table.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

// The internal-name codegen uses when it emits `BinaryenCall` targeting
// the runtime's `arena_reset`.  Callers that install the import under
// a different internal name will produce a module Binaryen rejects at
// validate time.
inline constexpr absl::string_view kArenaResetInternalName = "arena_reset";

// cel_host.cel_get_field + cel_host.cel_has_field trampolines
// (Layer 3).  Internal names codegen references; the matching
// imports carry signature `(i32, i32, i32, i32) -> ()` under wasm
// module `cel_host` — see compile.cc::InstallHostAbi.
inline constexpr absl::string_view kCelHostGetFieldInternalName =
    "cel_get_field";
inline constexpr absl::string_view kCelHostHasFieldInternalName =
    "cel_has_field";

// cel_host.cel_make_message trampoline (Layer 3).  Two-arg
// `(type_id, out_slot)` — the host resolves type_id against the
// per-Instance `cel.abi.types[]` lookup table, allocates a default
// proto via MessageFactory::GetPrototype()->New(), wraps it in an
// owning HostMessageBacking, interns the backing, and writes a
// CEL_MESSAGE CelValue with the interned msg_slot to out_slot.
inline constexpr absl::string_view kCelHostMakeMessageInternalName =
    "cel_make_message";

// cel_host.cel_set_field trampoline (Layer 3).  Three-arg
// `(msg_slot, field_ref_id, value_slot)` — dispatches on the
// resolved FieldDescriptor's cpp_type to pick the matching
// Reflection setter (SetBool / SetInt32 / SetString / SetEnumValue
// / etc.).  No out_slot — the message is mutated in place at the
// OwnedProtoBacking carried by msg_slot.  Repeated/map/message
// field types trap at the trampoline.
inline constexpr absl::string_view kCelHostSetFieldInternalName =
    "cel_set_field";

// cel_host.cel_wkt_unwrap_time trampoline (Layer 3).  Two-arg
// `(out_slot, msg_slot)` — reads the CelValue at msg_slot; if it's
// a CEL_MESSAGE of well-known time type
// (`google.protobuf.Timestamp` / `Duration`), extracts (seconds,
// nanos) via reflection and writes a `CEL_TIMESTAMP` /
// `CEL_DURATION` CelValue at out_slot.  Used by the kStructExpr
// lowering to bridge the cross-form equivalence pinned by
// cel-cpp's behavior (see `rewrite/m7b-duration-timestamp.md` §3.4).
// No-op for non-WKT-time messages (codegen only emits the call
// when `s.name()` matches one of the two WKT FQNs).
inline constexpr absl::string_view kCelHostWktUnwrapTimeInternalName =
    "cel_wkt_unwrap_time";

// cel_host.cel_wkt_unwrap_wrapper trampoline (Layer 3).  Three-arg
// `(out_slot, msg_slot, wrapper_kind)` — direct clone of the
// time-WKT unwrap shape for the 9 wrapper FQNs (BoolValue,
// Int32Value, Int64Value, UInt32Value, UInt64Value, FloatValue,
// DoubleValue, StringValue, BytesValue).  Codegen emits this at
// the kStructExpr tail when `s.name()` is a wrapper FQN.
// `wrapper_kind` is the matching `CelKind` (CEL_BOOL=1, CEL_INT=2,
// CEL_UINT=3, CEL_DOUBLE=4, CEL_STRING=5, CEL_BYTES=6) — letting
// Layer-2 dispatch on the inner-scalar kind without an additional
// descriptor walk.  See `rewrite/wat-traces.md` §56 and
// `rewrite/m8-wrapper-types.md`.
inline constexpr absl::string_view kCelHostWktUnwrapWrapperInternalName =
    "cel_wkt_unwrap_wrapper";

// Runtime entry points for map literal construction + indexing.  All three lookups carry signature
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

// `cel_map_in*` — `key in m` and the map-dot-field-sugar `has(m.field)`
// boolean key-presence check.  Signature `(out_slot, key_slot, map_slot)
// -> ()`; writes a CEL_BOOL into `out_slot`.  Three dispatch arms mirror
// `cel_map_lookup*`.
inline constexpr absl::string_view kCelMapInArenaInternalName =
    "cel_map_in_arena";
inline constexpr absl::string_view kCelMapInInternalName =
    "cel_map_in";  // kDynamic dispatcher
inline constexpr absl::string_view kCelHostMapInInternalName =
    "cel_host_cel_map_in";  // kHost arm import

// Runtime entry points for list literal construction + indexing.  `cel_list_create` is
// `(i32 out_slot, i32 capacity) -> ()`; the universal append
// `cel_list_append_at` is `(i32 list_slot, i32 elem_slot) -> ()`
// and is used for both literal fills (N appends in index order)
// and comprehension accu appends.  All three indexers carry
// signature `(i32 out_slot, i32 list_slot, i32 index_slot) -> ()`.
inline constexpr absl::string_view kCelListCreateInternalName =
    "cel_list_create";
inline constexpr absl::string_view kCelListAtArenaInternalName =
    "cel_list_at_arena";
inline constexpr absl::string_view kCelListAtInternalName =
    "cel_list_at";  // kDynamic dispatcher
inline constexpr absl::string_view kCelHostListAtInternalName =
    "cel_host_cel_list_at";  // kHost arm import

// `cel_select_optional_field_at_vv` is the polymorphic
// select-field-on-optional kernel; signature
// `(out_slot, src_slot, key_slot) -> ()` where `src_slot` is a CelValue
// of kind CEL_OPTIONAL / CEL_MAP_ARENA / CEL_LIST_ARENA / CEL_MESSAGE
// and `key_slot` is a CelValue of kind CEL_STRING / CEL_INT.  The
// same kernel handles both the kSelectExpr-on-optional path and the
// `Call(`_?._`)` path (routed via OverloadTable seed).
// `cel_optional_has_value_at_v` is the present-flag reader chained
// after the select kernel for test_only Select on optional; signature
// `(out_slot, opt_slot) -> ()`.  Both are exported in
// `runtime/BUILD.bazel` and seeded in
// `eval/engine.cc::kRuntimeExports`.
inline constexpr absl::string_view kCelSelectOptionalFieldInternalName =
    "cel_select_optional_field_at_vv";
inline constexpr absl::string_view kCelOptionalHasValueInternalName =
    "cel_optional_has_value_at_v";

// Predicate-gated insert / append for `{?key: opt_v}` / `[?opt_e]`
// literal entries.  Both ABIs follow the slot-out convention; the
// predicate is the optional's `present` flag rather than a bool
// (mirrors `cel_map_insert_at_if_bool` / `cel_list_append_at_if_bool`
// at the predicate-shape level).  Signatures:
//   `(map_slot, key_slot, opt_value_slot) -> ()`
//   `(list_slot, opt_value_slot) -> ()`
// See `wat/m14_map_insert_if_present.wat` and
// `wat/m14_list_append_if_present.wat` for the per-byte contract.
inline constexpr absl::string_view kCelMapInsertAtIfPresentInternalName =
    "cel_map_insert_at_if_present";
inline constexpr absl::string_view kCelListAppendAtIfPresentInternalName =
    "cel_list_append_at_if_present";
// Predicate-gated proto-field set for `Foo{?field: opt_v}`.  Same
// shape as the map/list `_if_present` kernels — wasm-side unwrap
// followed by delegation, but the inner step is a host trampoline
// (`cel_host.cel_set_field`).  See
// `wat/m14_proto_set_field_if_present.wat`.
inline constexpr absl::string_view kCelSetFieldAtIfPresentInternalName =
    "cel_set_field_at_if_present";

// One row of the field intern table, one per kSelect emitted by
// `LowerToEvalFunction`.  Index 0 is a reserved "not proto-resolvable"
// sentinel; rows [1..N] are the ids the emitted `cel_get_field` calls
// reference.  `BuildCelAbi` serialises this vector into
// `cel.abi.fields[]`.
struct FieldRefRow {
  uint32_t field_number = 0;  // proto wire number, or 0 for non-proto
  std::string name;           // always populated
  std::string owner_fqn;      // FQN of the owning message type, or ""
};

struct LoweringOptions {
  // Vestigial knob retained for source compatibility — the arena
  // lives in the wasi-libc dlmalloc heap and is sized at runtime,
  // not from this field.  See `rewrite/wasi/DESIGN.md` §4.
  uint32_t mem_size_bytes = MemoryLayout::kWasmPageSize;
};

struct LoweredFunction {
  // Binaryen-owned handle; callers can look the function up later via
  // `BinaryenGetFunction(mod.raw(), name)`.  The function's signature
  // is `() -> i32` — the CelValue offset of the root expression.
  BinaryenFunctionRef absl_nonnull func;

  // Field intern table populated during kSelect lowering.  Size == 0
  // means "no selects emitted"; otherwise `field_refs[0]` is the
  // sentinel and `field_refs[1..N]` are the referenced rows.
  // Consumed by `BuildCelAbi`.
  std::vector<FieldRefRow> field_refs;
};

// Adds a nullary `$eval` function named `func_name` to `mod`.  The
// function body is a block of type `i32`:
//
//   (block $eval (result i32)
//     (call $arena_reset (i32.const <arena_base>) (i32.const <arena_limit>))
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

// One declared parameter on a CEL-defined custom function — name
// + wasm-param position (1..N; wasm param 0 is the out_slot
// dictated by the M13 ABI).  Used by `LowerToCustomFn` to wire
// each referenced kFreeVariable `LaidOutVariable` to the wasm
// param the caller passes its slot offset through.
struct CustomFnParam {
  std::string name;
  // Wasm param index (1-based).  Param 0 is the out_slot.
  uint32_t wasm_param_index = 0;
};

// Lowers a CEL-defined custom function body into a wasm function
// with the M13 ABI:
//
//   (func (export <export_name>)
//     (param i32 i32 ...)        ;; out_slot, arg0, arg1, ...
//     (local i32 ...)            ;; one per referenced variable
//     <prelude>                  ;; local.set var_local
//                                ;;     (local.get param_for_name)
//     (call $cel_copy_slot (local.get 0)  ;; out_slot
//                          <root>))       ;; src slot from body
//
// Differs from `LowerToEvalFunction` in three ways: (a) wasm params
// instead of zero params, (b) no `arena_reset` (the arena belongs
// to the outer eval — see m13-custom-fns §4.4), and (c) the result
// is written via `cel_copy_slot` into the caller-supplied out_slot
// rather than returned by value.
//
// Bodies that reference a free variable not listed in `params` are
// an invariant violation (per m13 §3.5 the only legal references
// are declared params); CHECKs.
ABSL_MUST_USE_RESULT absl::StatusOr<LoweredFunction> LowerToCustomFn(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view export_name, absl::Span<const CustomFnParam> params,
    WasmModule& mod, const OverloadTable& overload_table,
    const LoweringOptions& opts = {});

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_EXPR_LOWER_H_
