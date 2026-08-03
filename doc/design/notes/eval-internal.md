# eval-internal — design notes (undefined)

Scope: `eval/internal/` (cel_host*, cel_host_error, cel_host_wasmtime,
abi_decode, cel_component, instance_impl, module_imports,
wasmtime_engine_state) + `eval/host/cel_log`.  Paired docs:
`doc/implementation-plan/rewrite/cel-host-surface.md`,
`doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md`.

## 1. Verified architecture

### 1.1 Three-layer trampoline split (cel_host)

The component implements the host side of every `cel_host.*` wasm
import in three layers (declared eval/internal/cel_host.h:1-9):

- **Layer 1 — backing semantics** (no wasm/wasmtime types).  Abstract
  bases: `HostMessageBacking` (ReadField/HasField/`message()`,
  cel_host.h:45-75), `HostMapBacking` (Size/Get/ContainsKey/ForEach,
  cel_host.h:146-171), `HostListBacking` (Size/At/ForEach,
  cel_host.h:231-249).  Concretes: `ProtoBacking` (non-owning
  Message*, cel_host.h:79-96), `OwnedProtoBacking` (owns
  unique_ptr<Message>, delegates reads to a composed `ProtoBacking
  inner_`, mutable via `mutable_message()` for cel_set_field,
  cel_host.h:111-139, cel_host.cc:554-569), `HostMap`/`HostList`
  (vector-backed, insertion order, linear scan, cel_host.cc:669-701,
  1292-1312), `ProtoMap`/`ProtoList` (reflection over one map /
  repeated field, non-owning, cel_host.cc:1214-1284, 1383-1421).
- **Layer 2 — runtime-agnostic trampoline bodies** in cel_host.cc,
  driven by three abstractions: `MemoryView` (24-byte CelValue +
  span + raw-u32 reads/writes, cel_host.h:298-314), `ExternrefTable`
  (three *independent* slot namespaces — message/map/list — each
  monotonic per Eval, `Reset()` between Evals, cel_host.h:318-349),
  `ArenaAllocator` (bump alloc of string/bytes payloads into wasm
  linear memory, cel_host.h:354-364).  Per-call state bundles into
  `TrampolineContext{bindings, mem, refs, alloc}` (cel_host.h:406-411).
- **Layer 3 — wasmtime glue** in cel_host_wasmtime.{h,cc}: production
  `HostExternrefTable` (vector-backed, slot 0 = nullptr sentinel in
  all three namespaces, cel_host_wasmtime.cc:30-76),
  `WasmtimeMemoryView` (memcpy over `wasmtime_sharedmemory_data`,
  cel_host_wasmtime.h:100-129), `WasmtimeArenaAllocator` (re-enters
  wasm via `wasmtime_func_call` on the runtime's `arena_alloc`
  export; trap/error/0-offset → nullptr, cel_host_wasmtime.cc:457-480),
  and one extern-"C" trampoline per import.

### 1.2 Import registration is bijection-checked

`kHostTrampolines[]` (cel_host_wasmtime.cc:495-516) lists exactly 20
`cel_host` imports: cel_get_field, cel_has_field, cel_map_lookup,
cel_map_iter_open, cel_list_iter_open, cel_list_at, cel_list_size,
cel_list_in, cel_list_eq, cel_list_concat, cel_map_size, cel_map_in,
cel_map_eq, cel_message_eq, cel_make_message, cel_set_field,
resolve_message_type_name, cel_timestamp_tz_accessor,
cel_wkt_unwrap_time, cel_wkt_unwrap_wrapper.
`BuildHostTrampolineIndex` ABSL_CHECKs trampolines ⊆ catalogue
(`abi::FindBuiltinHelper(kCelHost, name)`,
cel_host_wasmtime.cc:524-540); `RegisterCelHostImports` ABSL_CHECKs
catalogue ⊆ trampolines and registers each with arity taken from
`abi::CelHostFunctions()` — all signatures are N×i32 → void
(cel_host_wasmtime.cc:407-417, 544-564).  Drift between the table
and `abi/runtime_catalogue.cc` cannot ship.

### 1.3 One host call end-to-end (cel_get_field)

1. Wasm calls import `cel_host.cel_get_field(out_slot, msg_slot,
   field_ref_id, attribute_id)`; wasmtime invokes
   `CelGetFieldTrampoline` (cel_host_wasmtime.cc:174-179), whose env
   pointer is the per-Instance `CelHostCallbackEnv` (lives on
   `InstanceImpl::host_env`, instance_impl.h:76; populated by
   Engine::Plan via `BuildCelHostBindings`,
   cel_host_wasmtime.cc:80-125).
2. `HostFieldTrampoline<CelGetFieldImpl>` builds WasmtimeMemoryView +
   WasmtimeArenaAllocator from `env->memory` / `env->arena_alloc_fn`
   and forwards the four i32s (cel_host_wasmtime.cc:160-172).
3. `CelGetFieldImpl` → `RunFieldPrelude` (cel_host.cc:1525-1561):
   read msg CelValue **before any out_slot write** (msg_slot ==
   out_slot aliasing is legal, pinned by Layer2AliasingTest,
   cel_host_test.cc:537); absorb CEL_UNKNOWN/CEL_ERROR; non-message →
   CEL_ERR_TYPE_MISMATCH; resolve `field_ref_id` against
   `bindings.field_refs` (id 0 = sentinel, OOR → CEL_ERR_FIELD_NOT_FOUND,
   cel_host.cc:1451-1456); if `attribute_id != 0` and any
   AttributePattern kFull-matches the *effective* attribute (operand
   path ⊕ leaf field name, cel_host.cc:1482-1510) write
   `CEL_UNKNOWN{payload.unk = attribute_id}` (cel_host.cc:1546-1551);
   dereference `payload.msg_slot` via `refs.Lookup` — miss →
   CEL_ERR_HOST_ADAPTER_ERROR.
4. Layer 1: `backing->ReadField(field_number, field_name,
   CelType::Int())` — the expected-type argument is a placeholder;
   ProtoBacking dispatches on descriptor cpp_type, not the hint
   (cel_host.cc:1573-1579).  `ResolveFieldDescriptor` prefers
   `field_number != 0`, else by-name (cel_host.cc:533-540) — the
   FieldDescriptor* is resolved **per call**, not precomputed.
   Map fields → `Value::HostMap(ProtoMap)`, repeated →
   `Value::HostList(ProtoList)` (is_map checked first; every map
   field is also repeated, cel_host.cc:590-595).  Scalars via
   `ReadScalarField`, message fields via `ReadSingularMessageField`
   with presence rules (unset wrapper → Null regardless of syntax;
   proto3 unset message → Null; proto2 unset → default instance,
   cel_host.cc:469-495) and the WKT peel chain: Any (iterative
   unwrap, depth-CHECK at 1024, `type.googleapis.com/` or
   `type.googleprod.com/` prefixes only, cel_host.cc:75-181) →
   wrapper (9 FQNs → inner scalar, cel_host.cc:328-414) →
   Timestamp/Duration → (seconds,nanos) (cel_host.cc:194-214) →
   Value/Struct/ListValue → CEL map/list/scalar (cel_host.cc:226-320).
   Repeated elements get the same peel chain (cel_host.cc:1360-1374).
5. Encode back: `EncodeFieldResult` (cel_host.cc:877-886).
   Aggregates (kMessage/kMap/kList) intern their shared backing into
   the matching ExternrefTable namespace and write
   CEL_MESSAGE/CEL_MAP_HOST/CEL_LIST_HOST with the slot
   (cel_host.cc:845-870).  Scalars via `EncodeValue`
   (cel_host.cc:776-838): inline for null/bool/int/uint/double,
   arena-copied spans for string/bytes (`EncodeSpan`,
   cel_host.cc:737-751), CelDurTs decomposition for
   duration/timestamp (`DecomposeAbslDuration`, header-inline,
   cel_host.h:686-691).  **kError encodes only
   `payload.err = WireErrorCode(e->code)` — see §2 D1.**  kUnknown,
   kType, and aggregate kinds reaching EncodeValue are
   ABSL_CHECK(false) contract violations (cel_host.cc:808-834).
6. Non-OK Status from Layer 2 → `StatusToTrap` → wasm trap
   (cel_host_wasmtime.cc:152-156).  Spec-level errors never produce
   non-OK Status; they travel as CEL_ERROR in out_slot.

`EncodeValueToSlot` (cel_host.cc:890-898) re-exposes
`EncodeFieldResult` with empty bindings for the `@host` callback
return path (engine.cc side), so user-fn wire output is byte-identical
to built-in trampoline output; kUnknown is CHECK-rejected there too
(cel_host.h:662-671) — the host-call path writes CEL_UNKNOWN itself.

### 1.4 Aggregate-op kHost trampolines

The seven runtime dispatchers tail-call host impls for
CEL_LIST_HOST/CEL_MAP_HOST operands.  All absorb 3VL first
(`AbsorbUnary`/`AbsorbBinary`, cel_host_error.cc:126-145, first
non-normal operand wins), guard operand kind (TYPE_MISMATCH poison),
and treat a missing ref_slot as FailedPrecondition (trap):

- `CelListSizeImpl`/`CelMapSizeImpl` → WriteWireInt(Size())
  (cel_host.cc:1741-1758, 2004-2020).
- `CelListInImpl`: ForEach + `BackingValueEqualsQuery` directly
  against the wire query — avoids per-element arena encode (measured
  ~50us/scan saving on 1000×50B strings, comment
  cel_host.cc:1760-1765).  Scalar-only element equality; aggregates
  never match (cel_host.cc:1839-1843).
- `CelListEqImpl`: any origin pair (arena+arena/mixed/host); length
  short-circuit then per-element `HostScalarValueEq` (same-kind
  structural incl. span/temporal; cross-numeric by mathematical value
  via double, cel_host.cc:1630-1694, 1919-1959).
- `CelListConcatImpl`: mixed/both-host concat **POISONS
  TYPE_MISMATCH**; the materialise-into-arena strategy is documented
  in-line as follow-up (cel_host.cc:1962-2002).
- `CelMapInImpl`: DecodeKey (bool/int/uint/string only) +
  ContainsKey (cel_host.cc:2022-2044).
- `CelMapEqImpl`: any origin pair (host+host/mixed/arena+arena —
  arena+arena normally short-circuits in the runtime fast path).
  Both operands are normalized into (key, value) CelValue snapshots
  (`SnapshotMapEntries`: arena entries read from linear memory via
  `ArenaMapHeader`, host entries via ForEach + `EncodeBackingScalar`)
  then size check + set-equality walk (`NormalizedMapEq`) with
  `HostMapKeyEq` on keys (the LOSSLESS numeric rule, matching the
  arena kernel's `cel_map_key_eq`) and `HostScalarValueEq` on values
  (the rounding `==` rule; scalar-only, aggregate values never
  match).
- `CelMessageEqImpl` (standalone, for the polymorphic equals ladder):
  both CEL_MESSAGE; non-proto backing (message()==nullptr) →
  TYPE_MISMATCH; peels google.protobuf.Any operands
  (`PeelAnyForEq`, clones the unpacked inner, cel_host.cc:2129-2145);
  cross-descriptor after peel → false (not error);
  `MessageDifferencer::Equals` (cel_host.cc:2147-2197).

Indexing: `CelMapLookupImpl` (key-first 3VL order,
cel_host.cc:1038-1090) and `CelListAtImpl` (index-first 3VL order,
matching runtime fast-path; non-int index → TYPE_MISMATCH; negative →
INDEX_OUT_OF_BOUNDS host-side, >= Size() via the backing's error
return, cel_host.cc:900-949).

Comprehension snapshots: `CelListIterOpenImpl` materialises a host
list into a fresh arena ArenaListHeader+elements run and writes a
synthetic CEL_LIST_ARENA (empty header on non-host input/OOM so the
loop body never runs, cel_host.cc:951-1036).  `CelMapIterOpenImpl`
writes the 16-byte `MapIterState{kind=1,cursor,payload,count}` at a
caller-supplied state_offset over a 48B/entry key+value snapshot
(cel_host.cc:1092-1177).

### 1.5 Proto construction (cel_make_message / cel_set_field)

`CelMakeMessageImpl`: type_id → `bindings.message_types` (populated
at Plan from `cel.abi.types[]`; descriptor nullable = FQN not in
pool → clean TYPE_MISMATCH, not a trap) →
`generated_factory()->GetPrototype()->New()` → `OwnedProtoBacking` →
Intern → CEL_MESSAGE (cel_host.cc:2217-2254).  Dynamic-pool
descriptors (no generated prototype) also degrade to TYPE_MISMATCH
(cel_host.cc:2233-2241).

`CelSetFieldImpl` (cel_host.cc:3535-3637): poison-read contract — a
CEL_ERROR already in msg_slot makes the call a no-op so the error
rides out of the construction; only `OwnedProtoBacking` is mutable
(dynamic_cast gate; Activation-bound ProtoBacking → InvalidArgument
trap, cel_host.cc:3558-3578).  Dispatch: map → `SetMapField`
(arena/host sources, null message-values prune the entry,
cel_host.cc:3306-3531), repeated → `SetRepeatedField` (null message
elements pruned, cel_host.cc:3119-3125, 3258-3295), scalar →
`SetScalarField` (per-cpp_type with int32/uint32/enum range checks
→ kOutOfRange, cel_host.cc:2501-2690).  Write-side WKT pack mirrors
the read-side peelers: wrapper auto-wrap from scalar
(cel_host.cc:2636-2639), Duration/Timestamp/Value/Struct/ListValue/Any
pack (`MaybePackWktMessage`, cel_host.cc:2980-3007; Any inner-FQN
choice per kind — int/uint/bytes → wrapper, list → ListValue, map →
Struct, else Value, cel_host.cc:2901-2918), `null` on a
google.protobuf.Value field packs explicit null_value, every other
message type clears (cel_host.cc:2622-2630).  Out-of-range tail:
kOutOfRange Status converts to CEL_ERR_OVERFLOW *stamped into
msg_slot* and OK is returned (cel_host.cc:3632-3636); every other
non-OK Status traps.  3VL value on a scalar field set is
UnimplementedError (deliberate trap, cel_host.cc:3608-3617).

### 1.6 Misc trampolines

- `CelResolveMessageTypeNameImpl`: CEL_MESSAGE → backing →
  `GetDescriptor()->full_name()` arena-copied; out = CEL_TYPE with
  CelSpan (cel_host.cc:3649-3701).  Non-proto backing →
  HOST_ADAPTER_ERROR.
- `CelTimestampTzAccessorImpl`: one 4-arg import absorbs all 10
  with-TZ accessor overloads; `ResolveTimeZone` accepts
  "UTC"/"Z", signed/unsigned fixed `HH:MM` offsets, IANA names via
  `absl::LoadTimeZone`; bad TZ → CEL_ERR_INVALID_ARGUMENT
  (cel_host.cc:3775-3869).  cel-cpp index conventions pinned:
  month/dayOfMonth/dayOfYear 0-based, dayOfWeek sunday=0
  (cel_host.cc:3738-3771).
- `CelWktUnwrapTimeImpl` / `CelWktUnwrapWrapperImpl`: struct-literal
  tail-unwrap of WKT time/wrapper messages to scalar CelValues; the
  wrapper variant cross-checks produced kind against the codegen-
  supplied `wrapper_kind` (regression tripwire,
  cel_host.cc:3882-3966).

### 1.7 Wire-error vocabulary (cel_host_error)

Value factories (FieldNotFound/MakeError/KeyTypeMismatch/NoSuchKey/
IndexOutOfBounds) build `Value::Error(ErrorPayload{code, message,
expr_id=0})` (cel_host_error.cc:19-58).  `WireErrorCode` maps host
ErrorCode → CEL_ERR_* u32; unknown codes degrade to
CEL_ERR_TYPE_MISMATCH (legitimately open switch,
cel_host_error.cc:62-87).  Wire writers stamp 24-byte CelValues
directly (`payload.err` = bare code u32, cel_host_error.cc:89-122,
matching `cel_data.h:179` and every runtime writer).

### 1.8 abi_decode

Hand-rolled wasm section walker: header check (magic+version=1),
LEB128 u32 (≤5 bytes), custom-section scan for name "cel.abi",
proto-parse payload (abi_decode.cc:28-163).  NotFound (no section) is
a *distinct* status callers treat as "empty CelAbi is fine"
(abi_decode.h:29-37).  `DecodeRepr` maps wire u32 → ir::Repr with
out-of-range → kUnknown so the marshaller fails loudly later
(abi_decode.h:22-26, abi_decode.cc:110-146).

### 1.9 cel_component (m24 foreign-component marshaling)

`LiftCelToComponent` / `LowerComponentToCel`
(cel_component.h:108-121) bridge `celwasm::Value` ↔
`wasmtime_component_val_t` per the m24 §6 mapping: bool/s64/u64/f64,
string (length-based, NUL-safe), bytes ↔ list<u8> per-element, null ↔
option-none, duration/timestamp ↔ record{seconds:s64,nanos:s32}
(fields matched by name, order-agnostic; Lower enforces |nanos|<1e9
for duration, nanos∈[0,1e9) for timestamp,
cel_component.cc:284-372), list<T> recursive, map<K,V> ↔
list<tuple<K,V>> with key kinds restricted to bool/int/uint/string
(cel_component.cc:435-552), proto(fqn) ↔ list<u8> of
SerializePartialToString bytes, Lower re-materialises via
`ctx.pool` + generated factory (cel_component.cc:565-646).
`optional<T>` is permanently rejected in both directions
(cel_component.cc:750-758, 809-812, per m24 §14 user direction).
kType Lift = string of type-name (cel_component.cc:761-774); kType
**Lower is an Unimplemented stub** (cel_component.cc:815-818).
List/map Lift pre-init slots to no-alloc BOOL so a mid-loop error
leaves the partial val safely deletable (cel_component.cc:396-400,
486-489).  3VL absorption is explicitly the caller's job; Lift never
sees Error/Unknown (cel_component.h:63-67).

### 1.10 Instance/engine plumbing

- `InstanceImpl` (instance_impl.h:45-113): owns store, linker,
  expr_module, the shared-memory clone (refcount balanced in dtor,
  instance_impl.cc:5-27), `helpers_instance` (aliases expr_instance
  in m28 static mode), decoded `abi`, `host_env`
  (CelHostCallbackEnv), activation buffer offsets (malloc'd outside
  the bump arena because arena_reset is $eval's first instruction,
  instance_impl.h:78-90), per-Plan `host_fn_envs`
  (heap-stable HostFnEnv: borrowed HostCallback + borrowed host_env —
  sharing host_env.refs is what lets @host args/returns interoperate
  with built-in trampolines, instance_impl.h:30-43) and type-erased
  `component_fn_envs` (instance_impl.h:99-107).
- `WasmtimeEngineState` (wasmtime_engine_state.h:88-110): engine +
  cached parsed runtime_module + registered custom_modules (map,
  stable node addresses for callback env pointers) + host_callbacks +
  component_libraries (vector; no natural key).  Component-model
  handle is forward-declared so the header doesn't require
  `-DWASMTIME_FEATURE_COMPONENT_MODEL`
  (wasmtime_engine_state.h:37-48).
- `ModuleImportsCelNamespace` (module_imports.cc:11-24): m28 routing
  predicate — any import from module `"cel"` ⇒ dynamic mode (Engine
  instantiates cel_runtime.wasm separately); none ⇒ static.
- Destruction orders are load-bearing and documented:
  expr_module → memory → linker → store (instance_impl.cc:5-27);
  modules/components → engine (wasmtime_engine_state.cc:7-24).

### 1.11 cel_log

`cel_env.cel_log` 9×i32 import.  Three layers: pluggable
`CelLogSink` (process-global, default stderr, cel_log.cc:31-47,
332-346), pure `DecodeCelLog` over a byte span (no wasmtime types,
cel_log.cc:354-363), wasmtime glue that fetches the caller's
exported "memory" and no-ops when absent (logging is diagnostic,
never traps, cel_log.cc:392-417).  Format mini-language
%s/%d/%u/%f/%b/%v/%% with verbatim fallback for unknown directives,
`<oob>`/`<oob-arg>` for bad pointers (cel_log.cc:263-328).  argv
slots are 16 bytes: tag u32 at +0, payload u64 at +8
(cel_log.cc:24-28).  Note: `%v` for CEL_ERROR/CEL_UNKNOWN interprets
the payload as a *descriptor pointer* — see §2 D2.

## 2. Doc-vs-code discrepancies

**D1 (P1, = cleanup-backlog #31). ErrorPayload.message is dropped at
the host→wasm boundary; the doc claims a message-carrying wire
shape.**  cel-host-surface.md:820-823 specifies the on-wire error as
`{code: u16, _pad: u16, expr_id: u32, msg_off: u32, msg_len: u32}`
with message bytes in the arena.  Shipped wire: `payload.err` is the
bare code u32 (runtime/cel_data.h:179; cel_host_error.cc:89-94).
**Exact drop site:** `EncodeValue` kError arm,
eval/internal/cel_host.cc:802-807 — `out->payload.err =
WireErrorCode(e->code)`; `e->message` and `e->expr_id` are discarded.
Every Layer-1 message (e.g. `FieldNotFound(name)` carrying the field
name, `IndexOutOfBounds` carrying the range,
cel_host_error.cc:19-58) dies here.  Read-back then *synthesizes* a
generic message from the code: eval/instance.cc:230-256
(`DecodeCelError`, `p.message = ErrorCodeName(code)`) and
eval/host_call_context.cc:70-100 (`DecodeWireError`).

**D2 (P1). cel_log's %v formatter implements the doc's
descriptor-pointer error shape, not the shipped bare-code wire.**
`FormatError` (eval/host/cel_log.cc:168-184) reads `payload.err` as
an offset to a 16-byte `(code, msg_ptr, msg_len, _pad)` struct —
the cel-host-surface.md:820-823 planned shape.  Production writers
store the code itself (CEL_ERR_* = 10..41), so a real `%v` on an
error renders garbage read from linear-memory offsets 10..41.
cel_log_test.cc:415-430 pins the formatter against fixture memory
shaped per the old design, so tests don't catch the divergence.

**D3 (P1). payload.unk has two live, conflicting contracts.**
Runtime contract: u32 offset to a 2-word UnknownSet descriptor
`{ids_off, len}`, 0 = legal empty (runtime/cel_3vl.h:24-28;
`cel_unknown_merge` dereferences non-zero values as descriptors,
cel_3vl.c:60-115).  Host trampolines write the *attribute_id*
directly (cel_host.cc:1546-1551; instance.cc:996-998;
host_call_context.cc:525,544) and the decoder reads it back as an
AttributeId (instance.cc:304-309).  cel_3vl.c:105-108 acknowledges
only the `payload.unk == 0` case ("before per-id provenance is
wired"); a runtime merge of two host-minted unknowns with non-zero
attribute_ids would dereference offsets 1..N as descriptors.
`FormatUnknown` in cel_log follows the runtime contract
(cel_log.cc:141-166), so `%v` on a host-minted unknown also misreads.

**D4 (P1). instance.cc's CEL_ERROR decoder is missing
kInvalidArgument; the sibling decoder has it.**  Both decoders claim
"runtime CEL_ERR_* mirror host ErrorCode 1:1" (instance.cc:226-229).
host_call_context.cc:85 lists `ErrorCode::kInvalidArgument`;
instance.cc:233-246 omits it, so a wire CEL_ERR_INVALID_ARGUMENT (18,
e.g. a bad TZ name from CelTimestampTzAccessorImpl,
cel_host.cc:3851-3853) surfaces from Instance::Eval as
`kHostAdapterError` / "runtime error code 18" instead of
kInvalidArgument.

**D5 (P1). Header comment vs code vs surface-doc on 3VL operand
precedence.**  cel_host_error.h:97-101 says "error beats unknown
beats normal; the FIRST non-normal operand is propagated" — the two
clauses conflict.  cel-host-surface.md:118-119 says "`ERROR`
dominates `UNKNOWN`".  Code is first-operand-wins
(cel_host_error.cc:134-145) and the test
`AbsorbBinaryTest.FirstOperandUnknownBeatsSecondOperandError`
(cel_host_error_test.cc:251) pins UNKNOWN(a) beating ERROR(b).
Tests arbitrate: code is intentional; the header's first clause and
the doc's §1.4 are wrong as statements of the implementation.

**D6 (P1). cel-host-surface.md §3.1 claims LoadEval precomputes
`FieldRef[field_ref_id] = {FieldDescriptor*, result_type}` ("one
array lookup; no per-call descriptor search",
cel-host-surface.md:640-646, 731-735).  Code: `FieldRefEntry` carries
only `(field_number, field_name)` (cel_host.h:368-371);
`BuildCelHostBindings` resolves no descriptors for fields
(cel_host_wasmtime.cc:83-97); `ProtoBacking::ReadField` runs
`FindFieldByNumber`/`FindFieldByName` per call
(cel_host.cc:533-540, 579-580).  The `result_type`/`expected_type`
hint is likewise unplumbed — trampolines pass `CelType::Int()`
placeholders (cel_host.cc:1573-1579, 1084, 946).**

**D7 (P2). cel-host-surface.md describes unshipped surface as
present:** `Engine::CheckCompatible` / `Program::CheckCompatible`
(cel-host-surface.md:1057-1059, 1363) — no occurrence in compiler/
or eval/ (grep verified); submessage interning "via the expr
module's `cel_ref_intern` export" (cel-host-surface.md:657-659) —
no such export exists; interning is host-side `ExternrefTable`
(cel_host.cc:845-870).  File-layout §8 still shows the pre-rename
`compiler/api/` tree; the proto lives at `abi/cel_abi.proto`, not
`eval/host/cel_abi.proto` (cel-host-surface.md:1070-1071).  The doc
self-declares it defers to wasi/DESIGN.md post-MVP (lines 12-25) but
these sections carry no delta callouts.

**D8 (P2). m24 doc says "the kType Lift/Lower arm stays because
other kCelFn / kHost paths can still use it"
(m24-foreign-fn-component-backend.md:30-35, 451-454); the Lower arm
is an Unimplemented stub** ("stub until m24 B.4",
cel_component.cc:815-818) and the doc's shipped-slice list omits
B.8/B.4 status.  Unreachable for foreign-component decls (kType is
rejected at Build), so impact is latent.

**D9 (P2). Stale comment: "Bypass the broken LiftNull dispatch
above" (cel_component.cc:722) — no `LiftNull` function exists
anywhere in the TU; the null arm was always inline.**

**D10 (P2). cel_host.h:506 says mixed-origin list concat "POISON
with TYPE_MISMATCH" but `CelListConcatImpl` poisons *all* paths that
survive 3VL absorption — including arena+arena, which the header
implies works** (in practice the runtime dispatcher only routes
host-involved pairs here, so the dead arm is unreachable; the
comment block at cel_host.cc:1967-1999 describes the unimplemented
materialisation accurately).

## 3. Validation items

1. **Does a real `%v` log of a production CEL_ERROR render garbage?**
   Run a wat_runner / e2e fixture whose runtime calls
   `CEL_LOG("%v", <slot holding poison>)` with a CapturingCelLogSink
   installed; assert the line.  Expected per D2: `FormatError` reads
   offsets 10..41 of linear memory.  Settles whether cel_log must be
   rewritten to the bare-code wire or the wire upgraded to the doc
   shape (the #31 fix decides which).
2. **Can `cel_unknown_merge` actually receive two host-minted
   unknowns (non-zero attribute_ids)?**  Write an e2e PartialEval
   case `unknown_a && unknown_b` (both operands FULL-matched
   attribute patterns with attribute_id ≥ 1) and inspect the result.
   If reachable, D3 is a live miscompute (merge dereferences
   attribute_id as a descriptor offset); if codegen always routes
   host unknowns around the runtime merge, D3 downgrades to latent.
3. **kInvalidArgument decode gap (D4):** e2e
   `timestamp(0).getHours('NotATz')` through `Instance::Eval`; assert
   ErrorInfo()->code.  Today reading predicts kHostAdapterError +
   "runtime error code 18".  Cross-check what cel-cpp's oracle
   returns for the same expression (error message text) before
   choosing the fix shape.
4. **3VL precedence vs langdef (D5):** add an oracle case
   (`testdata/cel_cpp_oracle_test.cc`) evaluating a strict binary op
   with (unknown, error) operand pair under partial eval — does
   cel-cpp propagate the unknown or the error?  The oracle outranks
   both the header comment and cel-host-surface §1.4.
5. **arena_alloc(0) contract:** cel_host.h:362-363 promises
   "zero-byte alloc returns a valid pointer"; WasmtimeArenaAllocator
   returns nullptr when the wasm export returns offset 0
   (cel_host_wasmtime.cc:474-476).  Probe: call the runtime's
   `arena_alloc(0)` via wat_runner and check the returned offset is
   non-zero; if it can be 0, `EncodeSpan`'s empty-string path is the
   only thing saving the contract (cel_host.cc:743).
6. **m28 static-mode + cel_host imports:** does a static-linked
   Program still import `cel_host.*` (host trampolines) while
   importing nothing from `"cel"`?  `ModuleImportsCelNamespace`
   checks only `"cel"`; verify with
   `wasm-objdump -x` on a kStatic Program that `cel_host` imports
   remain and Plan still registers them.

## 4. Test coverage observations

Well pinned:
- Layer-1 backings: full positive/negative/boundary matrices —
  proto3/proto2 presence, field_number=0 name fallback, JSON-like
  custom backing (cel_host_test.cc:70-300); HostMap/HostList +
  ProtoMap/ProtoList incl. cross-type numeric keys, negative-int vs
  uint key, bool≠int(1) distinctness, embedded NUL
  (host_map_test.cc, host_list_test.cc, proto_map_test.cc,
  proto_list_test.cc).
- Layer-2 prelude: every sentinel arm (unknown/error absorb,
  non-message, OOR field_ref, bad externref slot), slot aliasing,
  unknown-pattern match/non-match/wildcard
  (cel_host_test.cc:363-642).
- cel_set_field: Any pack round-trips, WKT packs (Value from
  string/int/null/bool/list, Struct/ListValue from arena
  aggregates), null-pruning for repeated/map message values, poison
  contract (overflow/underflow/enum, set-on-poisoned no-op)
  (cel_host_test.cc:863-1247).
- Any-of-Any unwrap depths 1-4, both URL prefixes, malformed
  URL/payload/FQN negatives, wrapper-kind unwraps
  (cel_host_test.cc:1302-1533).
- cel_host_error: every factory message pinned, every ErrorCode→wire
  mapping enumerated, absorber precedence pinned in all four operand
  orders (cel_host_error_test.cc).
- abi_decode: full Repr matrix, compiler round-trips, malformed-wasm
  negatives incl. truncated/oversize LEB128 (abi_decode_test.cc).
- cel_component: 79-case boundary matrix (INT64_MIN/MAX, NaN/±Inf/-0,
  embedded NUL, ragged nesting, all four map-key kinds, nanos range
  rejects, tuple-arity/record-field-name negatives, large payloads).
- cel_log: every directive, every %v kind, oob fallbacks, sink
  swap/restore (cel_log_test.cc).

Gaps:
- **No test feeds a production-encoded (bare-code) CEL_ERROR or
  attribute-id CEL_UNKNOWN through cel_log's %v** — the fixtures are
  hand-built to the descriptor shape (D2 invisible to the suite).
- **No test covers instance.cc's DecodeCelError kInvalidArgument
  miss** (D4) — the impl-level TZ tests stop at the wire CelValue.
- `CelListConcatImpl` has no dedicated impl test (only the poison
  behavior is observable e2e); `CelMapIterOpenImpl` /
  `CelListIterOpenImpl` OOM fallback paths untested (would need a
  failing fake allocator — the fakes exist in cel_host_test_fakes.h).
- `EncodeValueToSlot`'s kUnknown CHECK-death is untested.
- `WalkMapEq`'s infrastructure-error propagation (backing Get
  failing mid-walk) untested.
- module_imports_test covers the predicate matrix but not the
  cel_host-only-imports shape (validation item 6).

## 5. Design decisions worth preserving

- **Spec errors travel in-wire; Status means trap.**  Every Layer-2
  impl returns non-OK only for infrastructure failure (bad ref_slot,
  arena OOM, missing reflection); all langdef-level errors are
  CEL_ERROR CelValues written to out_slot (cel_host.h:413-417).
  StatusToTrap is the single conversion point.
- **Read operands before writing out_slot** — out_slot may alias an
  operand slot (cel_host.cc:1427-1430).
- **Three independent externref namespaces** (message/map/list) with
  slot 0 as nullptr sentinel; per-Eval Reset.  An implementation may
  share storage but callers must treat namespaces as disjoint
  (cel_host.h:330-346).
- **`is_map()` before `is_repeated()`** everywhere — every proto map
  field is also repeated (cel_host.cc:585-595, 3598-3607).
- **OwnedProtoBacking composes ProtoBacking** rather than duplicating
  reflection; member order `msg_` before `inner_` is load-bearing
  (cel_host.h:132-138).
- **Mutability gate by dynamic_cast to OwnedProtoBacking** — host-
  bound messages can never be mutated through cel_set_field; the
  const_cast is confined to that one site and justified in-line
  (cel_host.cc:3558-3578).
- **Poison contract for construction:** out-of-range writes poison
  the *message slot* (CEL_ERR_OVERFLOW) and return OK; subsequent
  set_field calls no-op on a poisoned slot (cel_host.cc:3540-3547,
  3632-3636).  Matches cel-cpp's error-value (not trap) semantics.
- **Single-source-of-truth import table** with two-sided
  ABSL_CHECKed bijection against `abi::CelHostFunctions()` — a
  trampoline/catalogue drift cannot link (cel_host_wasmtime.cc:
  524-560).
- **One dispatch trampoline over N overloads where the ABI surface
  would otherwise balloon** (cel_timestamp_tz_accessor's 10
  accessors via a kind arg; cel_wkt_unwrap_wrapper's 9 wrappers via
  wrapper_kind with a produced-kind cross-check).
- **Host aggregates lift to arena snapshots for comprehensions**
  (iter_open) so the inline arena loop shape stays single-form;
  empty/OOM degrade to empty iteration, never a trap.
- **`in`/`eq` element equality is scalar-only on the host arms**
  (mirrors the arena fast path); cross-origin materialisation is the
  documented strategy for lifting that limit (cel_host.cc:1601-1617,
  1967-1999).
- **Lift pre-initialises component-val slots to no-alloc BOOL** so a
  mid-loop failure leaves a safely deletable partial value
  (cel_component.cc:392-400).
- **proto crosses the component boundary as serialized bytes**
  (SerializePartialToString — tolerates unset required fields, the
  full form would not), never as a handle (cel_component.cc:580-587).
- **Logging never traps** — every cel_log failure mode degrades to a
  printable sentinel; a caller without a memory export no-ops
  (cel_log.cc:392-417).
- **Rejected alternative recorded in-tree:** hand-rolled import-list
  walker for m28 routing rejected in favor of asking wasmtime
  (module_imports.h:22-27).
