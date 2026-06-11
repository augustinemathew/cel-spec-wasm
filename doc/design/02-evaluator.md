# 02 — Evaluator design (plan / instantiate / eval)

Status: current — authored 2026-06-10 from the design-rebuild notes
(doc/design/notes/). Supersedes: surface sections of
doc/implementation-plan/rewrite/cel-host-surface.md,
m21-host-call-adapter.md, the eval half of
two-phase-runtime-isolation.md, eval sections of
m24-foreign-fn-component-backend.md.

This doc covers `eval/`: how a compiled `Program` becomes a live
`Instance` and how an `Activation` becomes a `Value`, on the
lifecycle spine — Engine construction, `Plan`, registration,
host-call dispatch, marshal, eval, decode. Byte-level wire facts
(CelValue layout, CelKind table, error/unknown wire shapes, memory
regions) are owned by the forthcoming `03-abi-and-memory.md`; this
doc cites them, never restates them.

## 1. Roles & lifecycle

The system splits into four roles (`eval/engine.h:5-9`):

- `Compiler` (compiler-side) produces a `Program` — pure data: wasm
  bytes plus an embedded `cel.abi` custom section. The Program is
  the serialization boundary; the compiler has no wasmtime
  dependency, the evaluator no compile-time dependency.
- `Engine` owns the process-shared execution machinery: one
  `wasm_engine_t` and the parsed `cel_runtime.wasm` module, held in
  a `shared_ptr<celwasm::WasmtimeEngineState>`. Built only via
  `Engine::NewBuilder().Build()` (rvalue-qualified, single-use); the
  Builder's one knob is `EnableJitPerfMap`. The engine config
  enables wasm tail calls, threads, and shared memory — required
  because `cel_runtime.wasm` is built wasm32-wasi-threads and its
  dispatchers use `__attribute__((musttail))`.
- `Engine::Plan(const Program&) const → absl::StatusOr<Instance>` is
  the link step; everything per-evaluator is created here.
- `Instance` is the live evaluator: store, linker, runtime + expr
  instances, a cloned shared-memory handle, the `$eval` export, the
  decoded ABI, the per-Instance host environment. It holds the
  engine state `shared_ptr`, so an Instance outlives the `Engine`
  that minted it (test-pinned with an Eval proof).

Caching: the Engine parses the runtime module once (bench-justified:
~34x per-Plan, ~64x with process sharing — `engine.h:23-27`); the
expr module is re-parsed per Plan, an accepted default with a named
future cache seam (`engine.h:75-78`).

Threading contract (documented and test-pinned): registration
(`Add*`/`Bind*`) is single-threaded, configure-then-share; `Plan` is
concurrent-safe (fresh store/linker/memory per call, sharing only the
thread-safe engine and parsed modules — pinned by
`ConcurrentPlanCallsAllSucceed`); each `Instance` is thread-owned.
The compiler-side half of the end-to-end threading story lives in
`00-architecture.md`.

<!-- diagram-wanted: lifecycle — Compiler → Program(bytes) → Engine →
     N×Plan → N×Instance, with the shared_ptr ownership arrows -->

## 2. Plan, step by step

`Engine::Plan` (`eval/engine.cc`, `Engine::Plan`) runs the following
sequence. Steps 4 and 7 are mode-specific; everything else is shared.

<!-- diagram-wanted: Plan sequence — the steps below as swimlanes
     (abi decode / store / linker / instances), static-vs-dynamic
     fork drawn as a branch -->

**Step 1 — decode `cel.abi` and bind the host environment**
(`DecodeAbiAndBindHostEnv`, engine.cc:484). Runs first because it
walks the Program's raw bytes only — no wasmtime state. The decode
(`eval/internal/abi_decode.cc`) is a hand-rolled custom-section walk
returning the parsed `celwasm.abi.CelAbi` proto. Tolerance policy:
`NotFound` (no `cel.abi` section) is tolerated — the decoded abi
stays empty, a variable-free `Eval()` still works, the link-mode
label goes unvalidated, and synthetic WAT fixtures stay loadable;
`InvalidArgument` (bad magic / truncated LEB128 / malformed payload)
propagates.

When the section is present, two gates run before the abi is stored:

- `abi::CheckRuntimeAbiVersion` — Program `runtime_abi_version` must
  equal the engine's catalogue version, with one carve-out: version
  0 plus an empty surface (no variables/fields/attributes/types)
  loads, so hand-written fixtures need no stamped version. Mismatch
  → `FailedPrecondition` naming both versions; hard rejection is
  deliberate (the alternative is wasmtime's opaque type-mismatch
  trap at first call).
- `ValidateAbiSlotExtents` (engine.cc:445) — **the Plan-time slot
  extents gate.** Rejects (`InvalidArgument`) any Program whose
  `cel.abi` declares a variable slot whose
  `slot_offset + sizeof(CelValue)` extends past
  `CELWASM_RESERVED_LOW_MEMORY_BYTES` (8192, `runtime/cel_layout.h`).
  The compiler never emits such a slot — `Compile` validates the
  whole rodata + workspace region against the same constant before
  serializing — so a Program claiming one is corrupt, stale, or
  hand-crafted, and honoring it would have the activation marshal
  (§6) write CelValue bytes over the runtime's static data in shared
  memory. Plan is the earliest stage that sees the decoded ABI, so a
  bad Program fails loudly once, not per Eval. This is the eval-side
  half of the static-region pair; the compile-side half
  (`ValidateExprStaticRegion`) is described in `01-compiler.md`.

Then `BuildCelHostBindings` populates `host_env.bindings`: the
`field_refs` table (id → field_number, field_name), the `attributes`
table (id → root variable + qualifiers, for partial eval), and
Plan-time resolution of `cel.abi.types[]` FQNs to `Descriptor*`
against `generated_pool()` (statically-linked descriptors only;
dynamic schemas are a named follow-up).

**Step 2 — init per-Plan wasmtime state** (`InitPlanState`): fresh
store + WASI ctx, fresh linker pre-populated with `cel_env.cel_log`,
every `cel_host.*` trampoline (§5), and WASI preview1; the expr
module is compiled *before* any instantiation so its imports can be
introspected.

**Step 3 — link-mode routing**, by the module's actual import shape:
`is_static = !ModuleImportsCelNamespace(expr_module)`
(`eval/internal/module_imports.cc` — any import from module `"cel"`
means dynamic). When an abi section was present, the `link_mode`
label is cross-checked against that shape (`ValidateLinkModeLabel`,
engine.cc:513): a STATIC label on a `cel`-importing module or a
DYNAMIC label on a `cel`-free module → `FailedPrecondition`
("mislabeled or corrupted"). The label is **tripwire only, never a
routing input**; unknown future enum values are not validated (open
wire set). Pinned by the four `EnginePlanLinkModeTripwireTest` cases.

**Step 4 — dynamic mode only: instantiate the runtime**
(`InstantiateRuntime`): instantiate the cached `cel_runtime.wasm`
into the per-Plan store, run the shared post-instantiate bindings
(below), then `DefineCelLinkerBindings` — define `cel.memory` plus
every `cel.*` helper export on the linker, driven by
`abi::CelRuntimeHelpers()`, the single source of truth shared with
codegen's import pass, so the import set and the bind set cannot
drift (no lazy import tracking, per repo rule).

**Step 5 — bind embedder extensions**
(`BindRegisteredExtensions`), in order: registered custom modules
(instantiated per-Plan against the linker; `_initialize` called if
exported; function exports defined under the alias), then
`cel_fn.<overload_id>` host callbacks (§4), then Component-Model
components (§9). Modules may import `cel.*`, so this runs after
step 4; the expr module imports `cel_fn.*`, so it runs before step 6.

**Step 6 — instantiate the expr module** and pull the `eval` export.

**Step 7 — static mode only** (`BindStaticModeHelpers`): alias
`helpers_instance = expr_instance` (the runtime kernel is linked into
the expr module), call `__wasm_call_ctors` if exported (skips
silently if absent — defense-in-depth for ctor-bearing builds), then
run the shared post-instantiate bindings.

**Shared post-instantiate bindings** (`BindHelpersInstance`, both
modes): clone the runtime's exported **shared** memory onto the
`InstanceImpl` — the runtime owns and exports its memory; the host
does NOT create it (the wasi-threads build forces shared memory) —
enforce the A13/A14 memory invariants as `ABSL_CHECK`s, capture the
`arena_alloc` and `malloc` func handles for the host trampolines,
and call `arena_init(CELWASM_ARENA_CAPACITY_BYTES)` once.

The result is `Instance(wasmtime_, std::move(impl))`.
`InstanceImpl`'s destruction order is load-bearing: expr_module →
memory → linker → store (`eval/internal/instance_impl.cc`).

## 3. Registration surfaces & validation points

All registration happens on the Engine before Plan; none of it is
thread-safe (§1). Each surface validates what it can at
registration, defers what needs a store to Plan, and — in one
documented case — defers arity to call time.

- **`AddModule(alias, wasm_bytes)`** — a foreign wasm module bound
  under an alias. Reserved aliases rejected: `cel`, `cel_host`,
  `cel_env`, `cel_fn`, `host`, `wasi_snapshot_preview1`
  (`IsReservedAlias`). Bytes are parsed at registration so syntactic
  errors surface here, not at Plan; the parsed module is reused
  across Plans. Export names are snapshotted from export *types* (no
  instantiation), skipping names starting with `_`. Parse failures
  route through `WasmtimeErrorToStatus` → `FailedPrecondition` (the
  `engine.h` claim of `InvalidArgument` is stale and unpinned).
- **`AddFunction(overload_id, num_args, HostCallback)`** — the raw
  host-callback surface. `num_args` = declared params + 1 (the
  out_slot). Rejects empty id, arity 0, empty impl, duplicate ids.
  Callbacks live in a `std::map` precisely because the wasmtime
  trampoline env captures `&callback` — map node addresses stay
  stable across later insertions (`wasmtime_engine_state.h:68-76`).
- **`AddTypedFunction(id, lambda)`** — header-only sugar:
  `BindTypedFunction` (§4, L2) + `AddFunction`.
- **`BindFunction(celfn_decl, lambda)`** — the declaration-first
  path and the recommended registration surface. Parses the same
  `.celfn` IDL string the compiler takes, requires exactly one
  `@host.` decl, validates the callable's `param_kinds` positionally
  against the declared CEL types (`Value` matches anything;
  `string_view` serves string|bytes; both proto spellings serve
  `proto(...)`; `null`/`type`/`optional` only via `Value`), then
  registers under the synthesized overload-id — so the engine-side
  binding and the compiler-side import name cannot diverge. Pinned
  exhaustively by `EngineBindFunctionTest` and e2e
  `HostFnTest.BindFunctionDeclFirstRoundTrip`.
- **`AddComponent(component_bytes, lib)`** — fully implemented
  (`Engine::AddComponent`, engine.cc; the `engine.h` "not yet
  implemented — returns Unimplemented" comment is stale, as is the
  Plan-time banner above the implementation). The real contract:

  1. *Registration time:* every `kForeignComponent` overload-id in
     `lib` is conflict-checked against prior `AddFunction`
     registrations and prior components **before** the bytes are
     parsed — a duplicate is a clean `AlreadyExists` here instead of
     a per-Plan "duplicate import" error. Then the component bytes
     are parsed (malformed → `FailedPrecondition` via
     `WasmtimeErrorToStatus`) and stored; the parsed
     `wasmtime_component_t*` is shared across Plans.
  2. *Plan time:* each decl's overload-id is converted to kebab-case
     (`OverloadIdToKebab` — the Component-Model identifier grammar
     rejects snake_case; codegen's wasm import stays snake_case and
     the engine translates consumer-side), resolved against the
     instantiated component's exports (through the optional
     `lib.wit_interface()` parent index), and bound as a
     `cel_fn.<overload_id>` trampoline. Plan validates only "export
     exists" and "export is a function". **There is no FuncType-vs-
     decl signature comparison** — despite `engine.h:168-170`
     claiming one at registration.
  3. *Call time:* the arity traps. The trampoline traps if the wasm
     caller's arg-slot count differs from the decl's param count
     ("arity mismatch"), and a decl-vs-export shape mismatch
     surfaces as a `wasmtime_component_func_call` error → trap.

> **Open question (V21):** where exactly does a wrong-arity
> component *export* fail, and with what message? Registration and
> Plan both pass today; the failure is call-time by construction,
> but the trap site/shape is unprobed. Settle by extending
> `e2e/foreign_component_dispatch_test.cc` with a wrong-arity WAT
> component; then pin the call-time contract here, or add a
> Plan-time FuncType check and fix `engine.h`.

## 4. The host-call stack (L0 / L1 / L2)

Embedder functions (`@host.` decls) dispatch through three layers,
all in `eval/`:

<!-- diagram-wanted: L0/L1/L2 stack — wasm `call $cel_fn.<id>` at the
     bottom, L0 absorbing 3VL, L1 slot accessors, L2 typed adapter
     into the embedder lambda; annotate trap vs CEL_ERROR sites -->

- **L0 — `HostCallbackTrampoline`** (engine.cc:634): adapts
  wasmtime's raw callback shape. Wasm args are `(out_slot,
  arg_slots...)` as i32s, no wasm results. Before invoking the
  callback it runs 3VL absorption (`AbsorbUnknownOrErrorArg`,
  engine.cc:603): any `CEL_ERROR` arg (error wins over unknown *in
  this scan* — see §8 for the cross-layer fork) or `CEL_UNKNOWN` arg
  is copied verbatim to out_slot and the callback is NOT run,
  matching CEL dispatch semantics for strict functions. A non-OK
  callback status becomes a wasm trap via `TrapFromStatus`, which
  must NUL-terminate the message (the wasmtime Rust shim panics on a
  non-terminated buffer).
- **L1 — `HostCallContext`** (`eval/host_call_context.h`): kind- and
  bounds-checked accessors over the 24-byte CelValue slots. Error
  taxonomy: index out of range → `OutOfRange`; kind mismatch →
  `InvalidArgument`; dangling externref slot → `FailedPrecondition`.
  `HostListView`/`HostMapView` are lazy dual-representation views
  (host backing OR arena header), valid only for the callback's
  duration. Return setters route through `EncodeValueToSlot` — the
  same encoder the built-in `cel_host` trampolines use, so user-fn
  wire output is byte-identical to built-in output. `ReturnUnknown`
  mints a 1-element UnknownSet descriptor carrying
  `kFunctionUnknownSentinel = 0xFFFFFFFF` (03 §8.2) to distinguish
  function-origin unknowns from propagated ones (`eval/attribute.h`).
- **L2 — `BindTypedFunction`** (`eval/typed_function.h`): a
  trait-based adapter from a typed lambda or function pointer to
  `{HostCallback, num_args = arity + 1, param_kinds}`. Canonical
  types ONLY: the primary `ArgTrait`/`ReturnTrait` templates are
  always-false static_asserts, so `int`, `float`, by-value protos,
  or non-`StatusOr` returns are compile errors naming the type.
  `string_view` params accept string-then-bytes; concrete proto
  params `dynamic_cast` and reject wrong message types as
  `InvalidArgument`. The `param_kinds` metadata lets `BindFunction`
  validate a lambda against a parsed `.celfn` decl without
  re-deriving C++ types.

> **Open question (V19):** what does an embedder observe when a
> callback returns non-OK? The path is `TrapFromStatus(s.message())`
> → wasm trap → `Internal` "Eval trapped: <msg>" — the original
> status CODE is lost (cleanup-backlog #31). Settle with an e2e test
> registering a callback returning `InvalidArgumentError("boom")`
> and asserting the exact code + message `Instance::Eval` surfaces;
> then decide whether code loss is contract or bug.

## 5. The cel_host surface

The built-in `cel_host.*` imports — field access, aggregate ops,
proto construction, WKT handling — are implemented by
`eval/internal/cel_host*` in three internal layers
(`cel_host.h:1-9`):

- **Layer 1 — backing semantics** (no wasm/wasmtime types): abstract
  bases `HostMessageBacking` / `HostMapBacking` / `HostListBacking`;
  concretes `ProtoBacking` (non-owning `Message*`),
  `OwnedProtoBacking` (owns the message, composes a `ProtoBacking`
  for reads, mutable — the only backing `cel_set_field` accepts,
  gated by `dynamic_cast`), vector-backed `HostMap`/`HostList`, and
  `ProtoMap`/`ProtoList` (reflection views over one proto field).
- **Layer 2 — runtime-agnostic trampoline bodies** over three
  abstractions: `MemoryView` (CelValue/span reads + writes),
  `ExternrefTable` (three *independent* slot namespaces —
  message/map/list — each monotonic per Eval, slot 0 = nullptr
  sentinel, `Reset()` between Evals), `ArenaAllocator` (bump-alloc
  of string/bytes payloads into wasm linear memory).
- **Layer 3 — wasmtime glue** (`cel_host_wasmtime.cc`): production
  impls of the three abstractions (`WasmtimeArenaAllocator`
  re-enters wasm via the runtime's `arena_alloc` export) plus one
  extern-"C" trampoline per import.

Import registration is **bijection-checked**: `kHostTrampolines[]`
lists exactly 20 `cel_host` imports; startup `ABSL_CHECK`s assert
trampolines ⊆ catalogue and catalogue ⊆ trampolines against
`abi::CelHostFunctions()`, with arities taken from the catalogue.
Drift between the trampoline table and `abi/runtime_catalogue.cc`
cannot ship.

**One call end-to-end (`cel_get_field`):**

1. Wasm calls `cel_host.cel_get_field(out_slot, msg_slot,
   field_ref_id, attribute_id)`; wasmtime invokes the registered
   trampoline whose env is the per-Instance `CelHostCallbackEnv`
   (populated at Plan by `BuildCelHostBindings`).
2. The glue layer builds a `WasmtimeMemoryView` +
   `WasmtimeArenaAllocator` and forwards the four i32s to the
   Layer-2 impl.
3. `RunFieldPrelude`: read the operand CelValue **before any
   out_slot write** (msg_slot == out_slot aliasing is legal,
   test-pinned); absorb `CEL_UNKNOWN`/`CEL_ERROR`; non-message →
   `CEL_ERR_TYPE_MISMATCH`; resolve `field_ref_id` against
   `bindings.field_refs` (id 0 = sentinel); if `attribute_id != 0`
   and any registered `AttributePattern` kFull-matches the effective
   attribute (operand path ⊕ leaf field name), mint a 1-element
   UnknownSet descriptor carrying `attribute_id` in the guest arena
   (`EncodeUnknownSet`) and write
   `CEL_UNKNOWN{payload.unk = descriptor offset}` (03 §8.2), then
   return; dereference `payload.msg_slot` via the externref table.
4. Layer 1 reads the field: the `FieldDescriptor*` is resolved **per
   call** by number-then-name (`ResolveFieldDescriptor`) — there is
   no Plan-time descriptor cache, and the expected-type hint
   parameter is an unplumbed placeholder. Map fields produce
   `Value::HostMap(ProtoMap)` (`is_map()` checked before
   `is_repeated()` — every proto map field is also repeated);
   repeated fields produce `Value::HostList(ProtoList)`; message
   fields apply presence rules (unset wrapper → Null; proto3 unset
   message → Null; proto2 unset → default instance) and the WKT peel
   chain: `Any` (iterative unwrap, depth-CHECK at 1024) → wrapper →
   inner scalar → `Timestamp`/`Duration` → (seconds, nanos) →
   `Value`/`Struct`/`ListValue` → CEL scalar/map/list.
5. `EncodeFieldResult` writes back: aggregates intern their backing
   into the matching externref namespace and write
   `CEL_MESSAGE`/`CEL_MAP_HOST`/`CEL_LIST_HOST` with the slot;
   scalars encode inline; string/bytes are arena-copied spans.
6. A non-OK Status from Layer 2 → `StatusToTrap` → wasm trap.
   Spec-level errors never produce non-OK Status; they travel as
   `CEL_ERROR` values in out_slot (§8).

The other trampolines follow the same shape. Highlights: the seven
aggregate-op trampolines (size/in/eq/concat/lookup/at) absorb 3VL
first via `AbsorbUnary`/`AbsorbBinary` (`cel_host_error.cc` —
error dominates unknown, matching the kernel; see §8), guard
operand kinds with
`CEL_ERR_TYPE_MISMATCH` poisons, and treat a missing externref slot
as a trap; `in`/`eq` element equality is scalar-only on host arms;
`CelListConcatImpl` poisons every host-involved pair `TYPE_MISMATCH`
(materialise-into-arena is the documented follow-up); comprehension
snapshots (`cel_list_iter_open` / `cel_map_iter_open`) materialise
host aggregates into fresh arena runs, degrading to *empty
iteration* (never a trap) on non-host input or arena OOM — except a
CEL_UNKNOWN / CEL_ERROR input, which fails the eval loudly
(FailedPrecondition): the comprehension prologue's range-absorption
guard (`expr_lower_comprehension.cc::EmitRangeAbsorptionGuard`)
propagates poisoned ranges before the iterate path runs, so a
poisoned value reaching the snapshot is a codegen regression, and
an empty iteration there would be the silent empty-range-identity
wrong answer (cleanup-backlog #14). Proto
construction (`cel_make_message`/`cel_set_field`) carries a poison
contract: out-of-range writes stamp `CEL_ERR_OVERFLOW` into the
*message slot* and return OK; set-field on a poisoned slot is a
no-op, so the error rides out of the construction — cel-cpp's
error-value (not trap) semantics. The write side mirrors the
read-side WKT peelers with pack chains (wrapper auto-wrap, `Any`
inner-FQN choice per kind, null pruning for message-typed map and
repeated entries).

Single dispatch trampolines absorb whole overload families where
the ABI surface would otherwise balloon: `cel_timestamp_tz_accessor`
serves all 10 with-TZ accessors via a kind arg;
`cel_wkt_unwrap_wrapper` serves 9 wrapper types via a `wrapper_kind`
arg, with a produced-kind cross-check as a regression tripwire.

## 6. Activation marshal & PartialEval

`Instance::Eval(const Activation&)` marshals every ABI-declared
variable into its pre-assigned workspace slot before calling `$eval`
(`MarshalActivation`, `eval/instance.cc`). Slot offsets come from
`cel.abi.variables[]`, already bounded by the Plan gate (§2 step 1).
Missing binding → `FailedPrecondition`; declared-Repr vs bound-Kind
mismatch → `InvalidArgument` — with **three deliberate coercions**:

1. `Value::Null()` binds into any scalar slot as `CEL_NULL` —
   langdef wrapper semantics; cel-cpp's checker is the strict gate,
   the marshaller is permissive (`TryEncodeNullToScalarSlot`).
2. A bound WKT wrapper message peels to its inner scalar when the
   FQN matches the declared kind (`TryEncodeWktWrapperMessage`).
3. A bound `google.protobuf.Timestamp`/`Duration` message peels to
   (seconds, nanos) (`TryEncodeWktTimeMessage`).

`Repr::kEnum`/`kUnknown` marshal is `Unimplemented`.

**The activation buffer lives outside the bump arena.** Bound
string/bytes/type payload bytes go into a per-Instance buffer
`malloc`'d inside guest linear memory via wasm reentry — NOT
`arena_alloc` — because `$eval`'s prelude calls `arena_reset`, which
would clobber arena-resident payloads before the body reads them
(`instance.cc:369-374`). A pre-pass sums the needed bytes so the
buffer is grown *once* before any encoder caches a memory base
pointer; growth is 4 KB-rounded, guest OOM → `ResourceExhausted`,
and an A15 reserved-region `ABSL_CHECK` guards the placement.

**`PartialEval(activation, unknowns)`** is the same marshal with
`host_env.bindings.unknown_patterns` populated for the duration of
the call. A variable whose bare attribute is kFull-matched gets
`CEL_UNKNOWN` carrying a 1-element UnknownSet descriptor with its
interned attribute id (minted in the activation buffer — the bump
arena would be wiped by $eval's `arena_reset` prelude; 03 §8.2)
**regardless of whether it is bound** — the pattern wins over a
present binding.
Patterns are cleared on every exit path (and `Eval(activation)`
clears them again defensively) so a follow-up Eval can never observe
stale partial-eval state. Field-level (kPartial) absorption is not
the marshaller's job — it happens in the `cel_get_field`
trampoline's pattern match (§5 step 3).

## 7. Eval & decode

Zero-arg `Instance::Eval()` calls `$eval`, expects exactly one i32
(the result CelValue's offset), and decodes the CelValue there
(`DecodeCelValueAt`). The decoder handles NULL, BOOL, INT, UINT,
DOUBLE, STRING, BYTES, LIST_ARENA, MAP_ARENA, LIST_HOST, MAP_HOST,
MESSAGE, UNKNOWN, ERROR, TYPE, DURATION, TIMESTAMP; anything else →
`InvalidArgument` "kind N not yet supported" (the `instance.h` claim
that aggregate kinds reject is stale — all decode to real Values).
Decoded aggregates are deep-copied / re-wrapped into host-owned
`Value`s because the backings are per-Eval: `ExternrefTable::Reset`
and `arena_reset` invalidate them, so a decoded `Value` must own its
state.

`Eval(activation)` = `refs.Reset()` + clear `unknown_patterns` +
marshal (§6) + `Eval()`. A trap from `$eval` surfaces as
`absl::Internal` "Eval trapped: <msg>".

> **Open question (V20):** zero-arg `Eval()` performs no
> `refs.Reset()` — only `Eval(activation)` resets. Does repeated
> zero-arg Eval on a program whose host fn `ReturnProto`s grow the
> externref table unboundedly? Settle with a probe calling
> `instance.Eval()` N times, observing table growth and decoded
> results; then pin the contract (reset in `Eval()` vs documented
> caller obligation).

On the `Value` model itself (truthful status, against stale header
comments): ALL builders are real — scalars/Unknown/Error/Type in
`value.cc`; the aggregate builders
(`List/Map/HostList/HostMap/Message/OwnedMessage`) live in
`eval/internal/cel_host.cc` (one-way dep: cel_host → value, never
the reverse — the builders need complete backing types). There is no
`CelEquals`; `StructurallyEquals` is the shipped equality — scalars
by value, string/bytes/type by bytes, aggregates by backing-pointer
identity, explicitly NOT spec equality. `Value::Kind` numbering
matches the wire `CelKind` only for kinds 0-8; from `kMessage = 9`
upward it deliberately diverges — conversion is via explicit
switches, never casts. `ErrorCode` numerics DO mirror `CEL_ERR_*`
1:1 and must stay static_cast-equivalent.

## 8. Error & unknown propagation

The single byte-level contract for errors and unknowns on the wire is
owned by `doc/design/03-abi-and-memory.md`; this section states only
what the evaluator verifiably implements today, plus the open forks.

Verified behavior:

- **Spec errors travel in-wire; Status means trap.** Every Layer-2
  trampoline impl returns non-OK Status only for infrastructure
  failure (bad ref slot, arena OOM, missing reflection); all
  langdef-level errors are `CEL_ERROR` CelValues written to
  out_slot. `StatusToTrap` / `TrapFromStatus` are the only
  conversion points.
- **Error messages are dropped at the host→wasm boundary.** The
  `EncodeValue` kError arm writes only
  `payload.err = WireErrorCode(code)`; `ErrorPayload.message` and
  `expr_id` are discarded (cleanup-backlog #31). Read-back
  synthesizes a generic message from the code (`DecodeCelError` →
  `ErrorCodeName`).
- **The two decoders agree** (`instance.cc::DecodeCelError` and
  `host_call_context.cc::DecodeWireError` carry the same arm set,
  incl. `kInvalidArgument` — wire 18, e.g. a bad TZ name from the
  timestamp accessor, decodes as `kInvalidArgument` /
  "invalid_argument"). The Instance decoder historically omitted
  that arm; the full code matrix is pinned by
  `instance_test.cc::ErrorCodeRoundTripTest`.

Fork status (details in `03-abi-and-memory.md` §8): V3, V4, and V2
are resolved — V2's verdict is crowned with the evaluator fix still
pending:

> **RESOLVED (V3, 2026-06-10):** 3VL absorption precedence for an
> (unknown, error) operand pair is **error dominates unknown,
> regardless of operand order** — oracle-confirmed
> (`PartialEvalOracle.{UnknownPlusErrorIsError,ErrorPlusUnknownIsError}`,
> testdata/cel_cpp_oracle_test.cc) against cel-cpp's
> `NoOverloadResult` (eval/eval/function_step.cc), which scans args
> for an ErrorValue before merging unknowns. The cel_host
> trampolines' `AbsorbBinary` (previously first-operand-wins) was
> aligned; the kernel and `AbsorbUnknownOrErrorArg` already
> implemented the rule. See 03-abi-and-memory.md §8.3.

> **RESOLVED + FIXED (V2, 2026-06-10):** the **descriptor-offset**
> contract wins for `payload.unk`, and the evaluator-side fix
> SHIPPED as one unit: host writers mint descriptors
> (`EncodeUnknownSet` in cel_host.cc / host_call_context.cc — guest
> arena; `EncodeUnknownVariable` in instance.cc — activation buffer,
> because $eval's prelude `arena_reset` would wipe an arena-minted
> descriptor written before the call); both decoders dereference
> `{ids_off, len}` and surface every id; `Value::Unknown` carries
> the merged attribute-id set (`UnknownAttributes()`). cel-cpp's
> reference result carries the MERGED set (oracle-pinned,
> `testdata/cel_cpp_oracle_unknown_payload_test.cc`), which the old
> raw-id wire could not represent. As-shipped telling:
> `03-abi-and-memory.md` §8.2; e2e pins:
> `e2e/m2_partial_eval_test.cc::MergedUnknownProvenanceTest`.

> **RESOLVED (V4, 2026-06-10):** the bare-code wire was crowned and
> the readers fixed: `cel_log`'s `%v` formatter reads `payload.err`
> as the bare code (`error(code=N)`; the never-shipped
> descriptor-struct reader and its fixture-pinning test are gone),
> and the code-18 decode gap above was closed. The message-carrying
> wire upgrade was NOT taken — message loss remains the documented
> contract (backlog #31). See 03-abi-and-memory.md §8.1.

## 9. Components

The component path makes a Component-Model component's exports
callable as CEL functions. Registration and the arity contract are
in §3; the moving parts at Plan and call time:

- Per Plan, each registered component is instantiated into the
  per-Plan store and each `kForeignComponent` decl is bound on the
  linker as a `cel_fn.<overload_id>` trampoline — the wasm import
  shape is identical to an `@host` decl; only the callback body
  differs.
- `ComponentFnEnv` carries per-Plan state: the resolved component
  func, the decl's param/return `CelfnType` witnesses (copied at
  Plan), the descriptor pool, and a borrowed `CelHostCallbackEnv` —
  sharing `host_env.refs` is what lets component args and returns
  interoperate with the built-in trampolines.
- The trampoline 3VL-absorbs (identical contract to the host-call
  stack), lifts each arg CelValue → `wasmtime_component_val_t` per
  the type witness (`LiftCelToComponent`), calls, and lowers the
  single result back (`LowerComponentToCel`) through
  `HostCallContext::ReturnValue`. Lift pre-initialises every
  component-val slot to a no-alloc BOOL so a mid-loop failure leaves
  a safely deletable partial value.
- Type mapping highlights (`eval/internal/cel_component.cc`): string
  is length-based (NUL-safe); bytes ↔ `list<u8>`; duration/timestamp
  ↔ `record{seconds, nanos}` matched by field name with range
  enforcement; map ↔ `list<tuple<K,V>>`, key kinds restricted to
  bool/int/uint/string; `proto(fqn)` crosses as
  `SerializePartialToString` bytes (never a handle), re-materialised
  via the pool + generated factory on the way back; `optional<T>` is
  permanently rejected both directions; kType lifts as a type-name
  string but its **Lower arm is an Unimplemented stub** (latent —
  kType is rejected at library Build for component decls).
- wasip2 import stubs: unsatisfied preview2 imports are trap-stubbed
  (`define_unknown_imports_as_traps` — a runaway libc++ call traps
  naming the missing interface), with one shadowed exception:
  `wasi:random/random@0.2.0 get-random-bytes` returns deterministic
  LCG bytes (`RandomGetBytesStub`) — the wasi-sdk preview2 libc++
  reads it during std::string hash-seed static init, the wasmtime C
  API exposes no per-store WasiCtx, and all-zero seeds tripped
  libc++'s zero-seed special case. (The stub's leading docstring
  still says "zero bytes" — stale against its own body.)

## 10. Future work (surfaced during the notes pass)

- Per-Plan expr-module re-parse: the named cache seam
  (`engine.h:75-78`) remains unexploited.
- The error-wire decision (V4) and the status-code loss on the
  host-callback trap path (V19) should be fixed as one contract;
  `DecodeCelError`'s `kInvalidArgument` arm + example 08 + its smoke
  assertion update together.
- Cross-origin list concat: replace the `TYPE_MISMATCH` poison with
  the documented materialise-into-arena strategy.
- `FieldRefEntry` descriptor caching and the unplumbed expected-type
  hint, if field-read profiling justifies it.
- kType component Lower; dynamic-schema (`SchemaProtoSource`)
  descriptor pools for `BuildCelHostBindings`.
- Stale public-header comments contradicted by this doc (`engine.h`
  AddComponent/memory-model/Plan text, `value.h` builder and
  numbering claims, `instance.h` decode claims) should be fixed in
  the same change that lands this doc's content.

## History

This doc supersedes the evaluator-surface content of:

- `doc/implementation-plan/rewrite/cel-host-surface.md` (surface
  sections; its wire sections are superseded by
  `doc/design/03-abi-and-memory.md`)
- `doc/implementation-plan/rewrite/m21-host-call-adapter.md`
- `doc/implementation-plan/rewrite/two-phase-runtime-isolation.md`
  (the eval half)
- `doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md`
  (eval sections)

Source notes: `doc/design/notes/{eval-public,eval-internal,
abi-shared,91-contract-coherence}.md` (code-verified 2026-06-10; the
Plan-time slot-extents gate read from commit 8041dc97).
