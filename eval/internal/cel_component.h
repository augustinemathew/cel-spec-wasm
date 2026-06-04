// Component-Model marshaling bridge — typed canonical-ABI Lift / Lower
// between host `celwasm::Value` and wasmtime's `wasmtime_component_val_t`.
//
// This is the eval-side counterpart to the per-fn typed WIT codec from
// `doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md`
// §6-§7.  A `kForeignComponent` decl declares its CEL-typed signature
// via `FunctionLibrary::Builder::AddForeignComponent`; at Plan time
// `Engine::AddComponent` binds a host callback whose body calls these
// two free functions to bridge each argument and the result across the
// component boundary.
//
// Per-CEL-type WIT mapping (cel.wit lives at abi/wit/cel.wit;
// the C++ side is symmetric):
//
//   CEL `bool`        → WIT `bool`           → `WASMTIME_COMPONENT_BOOL`
//   CEL `int`         → WIT `s64`            → `WASMTIME_COMPONENT_S64`
//   CEL `uint`        → WIT `u64`            → `WASMTIME_COMPONENT_U64`
//   CEL `double`      → WIT `f64`            → `WASMTIME_COMPONENT_F64`
//   CEL `string`      → WIT `string`         → `WASMTIME_COMPONENT_STRING`
//   CEL `bytes`       → WIT `list<u8>`       → `WASMTIME_COMPONENT_LIST` of u8
//   CEL `null`        → WIT `option<bool>`   → `WASMTIME_COMPONENT_OPTION` none
//                       (or any inner; we never pass a Some)
//   CEL `duration`    → WIT `record {seconds:s64, nanos:s32}`
//   CEL `timestamp`   → WIT `record {seconds:s64, nanos:s32}`
//   CEL `list<T>`     → WIT `list<wit T>`    → `WASMTIME_COMPONENT_LIST`
//   CEL `map<K,V>`    → WIT `list<tuple<wit K, wit V>>`
//                       (no native WIT map; key kind ∈ {bool,int,uint,string})
//   CEL `optional<T>` → WIT `option<wit T>`  → `WASMTIME_COMPONENT_OPTION`
//   CEL `proto(fqn)`  → WIT `list<u8>` (serialized wire bytes; the codec
//                       deserializes inside the component)
//   CEL `type`        → WIT `string` (the type-name)
//
// Ownership contract:
//
//   - `LiftCelToComponent(type, value, *out)` initialises `*out` to a
//     fully-owned component val.  The caller MUST release with
//     `wasmtime_component_val_delete(out)`.  For scalar kinds the
//     "owned" memory is just the union slot; for string/list/record
//     the lift allocates via wasmtime's vec ctors (`*_new_uninit` etc.)
//     and the delete cascades.
//
//   - `LowerComponentToCel(type, val, *out)` reads `val` (caller-owned;
//     this function does NOT take ownership or delete) and writes a
//     `Value` into `*out`.  The Value carries its own owned storage —
//     after Lower returns, the caller may freely delete `val`.
//
// Error semantics:
//
//   - Returns `InvalidArgumentError` on a CelfnType ↔ Value kind
//     mismatch (e.g. `type.kind == kInt` but `value.kind() != kInt`),
//     or on a `wasmtime_component_val_t::kind` that disagrees with the
//     declared `CelfnType` (an upstream wasmtime invariant violation
//     — should never happen if `Engine::AddComponent` validated the
//     FuncType at AddComponent time, but checked here as defence in
//     depth).
//   - Returns `OutOfRangeError` on a value that doesn't fit the WIT
//     scalar (e.g. a `kUint` Value above `UINT64_MAX`, which is
//     unreachable, but the timestamp `nanos` range check is similar).
//   - Returns the propagated `absl::Status` on a nested failure
//     (list-element Lift fails → the outer Lift fails with the index
//     prepended in the message).
//
// These functions do NOT participate in 3VL absorption — that is the
// caller's responsibility (and it already happens in
// `HostCallbackTrampoline::AbsorbUnknownOrErrorArg` upstream).  By the
// time Lift sees a Value, the value is guaranteed to be a concrete CEL
// value (not Error / Unknown).

#ifndef CELWASM_EVAL_INTERNAL_CEL_COMPONENT_H_
#define CELWASM_EVAL_INTERNAL_CEL_COMPONENT_H_

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "compiler/celfn/function_library.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"

// The wasmtime component headers are gated by
// WASMTIME_FEATURE_COMPONENT_MODEL.  This header itself does not
// include them; the implementation does (with the define forced in
// the BUILD copts).  We forward-declare the types here so callers
// (engine.cc) need only include this header, not pull in
// wasmtime/component/val.h transitively.
extern "C" {
struct wasmtime_component_val;
typedef struct wasmtime_component_val wasmtime_component_val_t;
}

namespace celwasm {

// Optional context plumbed through marshaling: the descriptor pool a
// `kProto` argument should look up its message type in.  Pass null when
// no `kProto` types are involved (scalar / aggregate-of-scalar fns).
struct CelComponentContext {
  const google::protobuf::DescriptorPool* absl_nullable pool = nullptr;
};

// Lift: host-side `Value` → wasmtime component val.
//
// `type` is the type witness from the CelfnDecl's param / return.
// `value` must match `type.kind` per the §6 mapping.  `out` must be a
// caller-owned `wasmtime_component_val_t` (e.g. stack-allocated or
// already-`_delete`'d) — Lift will overwrite its contents.  After
// Lift returns OK, the caller owns the produced component val and
// MUST call `wasmtime_component_val_delete(out)` once it has been
// consumed (typically by `wasmtime_component_func_call`).
ABSL_MUST_USE_RESULT absl::Status LiftCelToComponent(
    const CelfnType& type, const Value& value,
    const CelComponentContext& ctx,
    wasmtime_component_val_t* absl_nonnull out);

// Lower: wasmtime component val → host-side `Value`.
//
// `type` is the type witness — the same one used to declare the
// component fn's return.  `in` is the component val produced by
// `wasmtime_component_func_call` (caller owns; Lower does not delete).
// `out` receives a fully-owned `Value` that survives any subsequent
// `wasmtime_component_val_delete(in)`.
ABSL_MUST_USE_RESULT absl::Status LowerComponentToCel(
    const CelfnType& type, const wasmtime_component_val_t& in,
    const CelComponentContext& ctx, Value* absl_nonnull out);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_COMPONENT_H_
