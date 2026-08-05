# cel.abi wire format & boundaries

How a `Program` describes itself to the evaluator: the `cel.abi` descriptor, the runtime import namespaces, and how errors and unknowns travel the wire. The value model and memory layout these formats point into are [`03-abi-and-memory.md`](03-abi-and-memory.md).

## 1. The `cel.abi` custom section

One serialized `celwasm.abi.CelAbi` proto (`abi/cel_abi.proto`) in a wasm custom section named `"cel.abi"`. Producer: `compile.cc::AttachCelAbiSection` → `BuildCelAbi` (`abi/cel_abi_emit.cc`); consumer: `abi_decode.cc::DecodeCelAbiFromWasm`, first in `Engine::Plan` (raw-bytes walk; no wasmtime state). Emit and decode agree **by construction** — both link the same generated proto; the only hand-rolled wire code is the decode side's custom-section framing walk (the evaluator cannot link Binaryen). Tolerance: `NotFound` (no section) → empty abi; variable-free Eval works, synthetic WAT fixtures load. `InvalidArgument` (bad magic / wasm version ≠ 1 / truncated LEB128 / section overrun / parse failure) propagates.

### 1.1 Field-by-field

| # | field | emitted from | consumed by |
|---|---|---|---|
| 1 | `version` — schema version of the proto itself, constant 1 (`cel_abi_emit.cc::kCelAbiVersion`) | always | **nobody** (V43 below) |
| 2 | `variables[]` — `VariableEntry{name, local_index, slot_offset, repr}`, free variables only (comp-scope locals skipped, `EmitVariables`) | `StaticLayout::variables` | activation marshal (`instance.cc`), iterating linearly by name |
| 3 | `fields[]` — `FieldEntry{id, field_number, name, owner_fqn}`; row 0 = sentinel | codegen's `FieldRefRow` span | `cel_get_field`/`cel_set_field` trampolines via `BuildCelHostBindings` |
| 4 | `attributes[]` — `AttributeEntry{id, variable, qualifiers[]}`; row 0 = sentinel | `layout.attributes` | partial-eval unknown-pattern matcher |
| 5 | `types[]` — `TypeEntry{id, fully_qualified_name}`; row 0 = sentinel | `layout.message_types` | Plan-time FQN → `Descriptor*` resolution for `cel_make_message` |
| 6 | `runtime_abi_version` — `abi::kRuntimeAbiVersion` (currently 2) | constant | `CheckRuntimeAbiVersion` at Plan |
| 7 | `link_mode` — `LINK_MODE_DYNAMIC = 0` / `LINK_MODE_STATIC = 1` | the compile arm taken | `ValidateLinkModeLabel` — validation only, never routing |
| 8 | `required_functions[]` — `RequiredFunction{overload_id, fn_name, backend, param_types[], return_type, is_receiver}`, one row per custom function the final wasm imports from module `cel_fn` (§1.4) | `BuildRequiredFunctions` over the post-optimize import surface (`cel_abi_emit.cc`) | `Engine::Plan`'s required-function check, before any wasmtime linking |

Wire-design facts, one telling each. **Sentinel row 0 everywhere:** `id == 0` means "no id"; the emitter writes a real placeholder row 0 so host tables index 1:1 with the ids codegen burned into the wasm (`FieldEntry.field_number == 0`, not `id == 0`, is the separate "not proto-resolvable" marker). **Minimal wire:** only the numeric `repr` crosses for variables — `repr` alone picks the host encoder; a full `CelType` is a reserved additive slot; no `CheckedExpr` on the wire. **Attribute granularity is field-path only, by design:** `.field` selects extend `qualifiers`, `[k]`/`[i]` never do, and `AttributePattern::Parse` rejects bracket qualifiers rather than accept patterns it cannot honor (`cel_abi.proto:130-169` carries the full produce/propagate model).

!!! note "Open question (V7/R30)"
    `cel_abi.proto`'s comment claims `variables[]` is positional by `local_index`, but the emitter skips comp-scope locals while ResolvePass interleaves them in the same dense index space — a free variable first referenced inside a comprehension should break the claim. Consumers iterate by name — no runtime bug. Probe: `xs.map(i, i + y)`, check `variables(1).local_index()`.

### 1.2 The two versions, and the never-enforced one

**`CelAbi.version` (field 1)** is the proto message's own schema version, constant 1 since inception. **`runtime_abi_version` (field 6)** is the helper-catalogue version (§2) — the one actually **enforced**, by `CheckRuntimeAbiVersion` (`abi/runtime_catalogue.cc`): `prog_v == engine_v` → OK; `prog_v == 0` AND an empty surface (no variables / fields / attributes / types) → OK, so hand-written fixtures need no stamped version; `prog_v == 0` with a non-empty surface → `FailedPrecondition` "predates ABI versioning; recompile"; otherwise → `FailedPrecondition` naming **both** versions. Hard rejection is deliberate: the alternative is wasmtime's opaque type-mismatch trap at first call into a renamed helper.

!!! note "Open question (V43)"
    `CelAbi.version` is read by no non-test code (grep-verified); "schema version exists and is never checked" was never recorded as deliberate policy. A section with `version = 99` plus one variable will load at Plan. Decide: tolerate-forever, or reject unknown majors.

### 1.3 The link-mode label

`LinkMode` is an enum, not a bool, so future modes land additively. `LINK_MODE_DYNAMIC = 0` is load-bearing: pre-label Programs carry no field-7 tag and decode (proto3 default) to the shape they actually have — dynamic-mode sections are **byte-identical** to legacy ones (byte-pinned by `cel_abi_emit_test.cc`). The label is metadata + tripwire, never routing: the engine routes on import introspection (`is_static` ⇔ zero `cel.*` imports, `module_imports.cc`); `ValidateLinkModeLabel` (`engine.cc`) rejects a label/shape contradiction with `FailedPrecondition`; unknown future enum values skip validation (open wire set).

### 1.4 `required_functions` (field 8) — the custom-fn signature table

One `RequiredFunction` row per **surviving `cel_fn` import of the final
wasm** — i.e. one function instantiation WILL demand. Emission is from
the post-optimize import surface (`WasmModule::ListFunctionImports` →
`BuildRequiredFunctions`), not the AST or the registered libraries:
overload imports are installed unconditionally for every declared
custom fn and Binaryen `Optimize` at O1+ drops unused ones, while
wasmtime demands every *surviving* import at link time — deriving from
anything but the final module would desync per optimize level. (This is
why `cel.abi` attachment happens *after* the optimize pass.)

Signature types use the recursive `Type` message — THE wire spelling
of a CEL type, a 1:1 mirror of `shared/CelType` (the one C++ type
vocabulary; `abi/celfn_wire.{h,cc}`: `TypeFromCelType`, `TypeEquals`,
`RenderSignature`; `KIND_PROTO` carries `CelType::Kind::kMessage`'s
FQN). `Type` is the section's *general* type
vocabulary, not a function-specific one: `RequiredFunction` carries
it, and `VariableEntry.type` (field 5) carries the same message for
variable introspection (see the section at the bottom of this doc).

```proto
message Type {
  enum Kind {
    KIND_UNSPECIFIED = 0;
    KIND_BOOL = 1;   KIND_INT = 2;    KIND_UINT = 3;
    KIND_DOUBLE = 4; KIND_STRING = 5; KIND_BYTES = 6;
    KIND_DURATION = 7; KIND_TIMESTAMP = 8;
    KIND_PROTO = 9;   // fqn in proto_fqn
    KIND_LIST = 10;   // params = [elem]
    KIND_MAP = 11;    // params = [key, value]
    KIND_TYPE = 12;
    KIND_OPTIONAL = 13;  // params = [elem]
    KIND_NULL = 14;      // CEL `null` is a declarable celfn shape
  }
  Kind kind = 1;
  string proto_fqn = 2;
  repeated Type params = 3;
}

message RequiredFunction {
  enum Backend {
    reserved 2;            // was PLUGIN — the removed wasm-component
    reserved "PLUGIN";     // plugin backend; never reassigned
    BACKEND_UNSPECIFIED = 0;
    HOST = 1;
  }
  string overload_id = 1;    // == the cel_fn import base name
  string fn_name = 2;        // source-level name, for messages
  Backend backend = 3;
  repeated Type param_types = 4;  // excludes out_slot; wasm arity = size+1
  Type return_type = 5;
  bool is_receiver = 6;
}
```

Wire-design facts: **open-set on wire** — unknown `Kind` / `Backend`
values decode and compare numerically, never rejected (additive enum
growth is free), with one carve-out: wire value 2 (the retired PLUGIN
backend) is `reserved` and Plan **rejects** rows carrying it with a
clear message — a Program compiled against the removed plugin backend
can never run, and an outright rejection beats an opaque link error
(`eval/internal/required_fn_check.cc`). **`@host` rows are included** —
the wire cost is trivial, and it converts the worst legacy failure (an
opaque `unknown import cel_fn.<id>` wasmtime link error when a host
registration was forgotten) into a clean `FailedPrecondition` at Plan.
**Empty table = legacy Program**: the Plan-time check no-ops —
pre-field-8 Programs keep working unchanged. `TypeEquals` is
recursive and proto-FQN-sensitive; `RenderSignature` is shared by emit
tests and Plan diagnostics so both render a signature identically.

## 2. The runtime catalogue & import namespaces

`abi/runtime_catalogue.{h,cc}` is the single source of truth for every wasm import an expr module may declare; the entry type is the generated proto `celwasm::abi::CelRuntimeFunction` (`{name, module, num_args, returns_i32}`) — no hand-defined POD.

### 2.1 The four namespaces

| module | contents | catalogued? |
|---|---|---|
| `cel` | pure-wasm helpers exported by `cel_runtime.wasm` (kernels, dispatchers, arena, iteration) | yes — **derived** from `// cel:codegen-export` markers on the C declarations; membership from the marker, arity/return shape from the `void`/`uint32_t` C signature (clang lowers it 1:1 to the wasm type). `//bazel:gen_runtime_catalogue` |
| `cel_host` | wasmtime host trampolines (field access, aggregate ops, proto construction, WKT) | yes — hand-maintained rows in `abi/runtime_host_env.textproto` (20 rows; no C export to derive from), guarded by the startup bijection CHECK in `cel_host_wasmtime.cc` (trampolines ⊆ catalogue AND catalogue ⊆ trampolines) |
| `cel_env` | host environment helpers — today only `cel_log` | yes — hand-maintained (1 row); **not** covered by the cel_host bijection check (drift note below) |
| `cel_fn` | user custom-fn implementations | **no, by design** — `FindBuiltinHelper(kCelFn, …)` returns nullptr; arities come from per-compile registration. Open set |

The composed textproto (hand-maintained host/env rows + appended derived `cel` rows) is embedded as a string literal — `CelRuntimeHelpers()` is a pure in-process call. Cross-namespace name collisions are intentional and test-pinned (`cel.cel_list_at`, the kDynamic dispatcher, tail-calls `cel_host.cel_list_at`); lookups are `(module, name)`-keyed. Linker-export-list mechanics are `04-runtime.md` §6 (residual audit: V42).

### 2.2 The 12-vs-20 import-set table

Two correct counts for two artifacts — a subset, not a conflict:

| set | size | contents |
|---|---|---|
| `cel_host` catalogue = what an **expr module** may import | 20 | `cel_get_field`, `cel_has_field`, `cel_map_lookup`, `cel_map_iter_open`, `cel_list_iter_open`, `cel_list_at`, `cel_list_size`, `cel_list_in`, `cel_list_eq`, `cel_list_concat`, `cel_map_size`, `cel_map_in`, `cel_map_eq`, `cel_message_eq`, `cel_make_message`, `cel_set_field`, `resolve_message_type_name`, `cel_timestamp_tz_accessor`, `cel_wkt_unwrap_time`, `cel_wkt_unwrap_wrapper` |
| `runtime/wasm_imports.txt` = what **cel_runtime.wasm itself** imports | 12 (+ `cel_log`) | the kDynamic dispatcher tail-call targets plus resolve/tz: `cel_map_lookup`, `cel_list_at`, `cel_list_size`, `cel_list_in`, `cel_list_eq`, `cel_list_concat`, `cel_map_size`, `cel_map_in`, `cel_map_eq`, `cel_message_eq`, `resolve_message_type_name`, `cel_timestamp_tz_accessor` |
The other 8 catalogue rows are imported only by expr modules, never by the kernel. A static-mode Program retains its `cel_host.*` / `cel_env.*` imports while importing nothing from `"cel"` — the host boundary does not collapse with the link mode (V12).

Observed drift, recorded rather than silently inherited: the `cel_env.cel_log` catalogue row says `num_args: 4`, but both the C import declaration (`runtime/cel_log.h:48-50`) and the wasmtime registration (`cel_log.cc::CelLogType`) are **9×i32 → void** (file/fn spans, line, fmt span, argv ptr+count; argv slots are 16 bytes: u32 tag, u32 pad, u64 payload). No consumer of the env row's arity was found, and the bijection check does not cover `cel_env` — latent, but the row is wrong as written; fix it and extend the cross-check to the env namespace.

## 3. Errors & unknowns on the wire

Settled contract; docs 02 §8 and 04 §3 defer here.

### 3.1 Errors: the bare-code wire, per layer

The on-wire error is `kind = CEL_ERROR`, `payload.err = CEL_ERR_*` — a bare u32 code (§2 rule 4); every kernel (`poison`) and host writer (`WriteWireError`) emits it.

| layer | behavior on the error path | citation |
|---|---|---|
| host → wasm encode | `EncodeValue`'s kError arm writes ONLY `WireErrorCode(code)`; **`ErrorPayload.message` and `expr_id` are discarded** (cleanup-backlog #31). Every Layer-1 message (`FieldNotFound(name)`, the out-of-bounds range text, …) dies here. `WireErrorCode` maps all 14 named `ErrorCode` values and passes an out-of-enum numeric through unchanged | `eval/internal/cel_host.cc`, kError arm of `EncodeValue`; `cel_host_error.cc::WireErrorCode` |
| wasm → host decode (Instance) | `DecodeCelError` synthesizes a generic message from the code (`ErrorCodeName`); its switch covers every named `ErrorCode` including `kInvalidArgument`; an unrecognized wire byte degrades to `kHostAdapterError` / "runtime error code N". The full code matrix is pinned by `instance_test.cc::ErrorCodeRoundTripTest` | `eval/instance.cc::DecodeCelError` |
| wasm → host decode (host-call ctx) | `DecodeWireError` — same arm set as its Instance sibling; the two decoders agree | `eval/host_call_context.cc` |
| in-guest formatting | `cel_log`'s `%v` error formatter reads the bare-code wire: `payload.err` IS the `CEL_ERR_*` code, rendered as `error(code=N)` | `eval/host/cel_log.cc::FormatError`; `cel_log_test.cc::ValueErrorKindBareCode` |
| host-callback trap path | a non-OK Status from an embedder callback becomes a wasm trap; the message survives, the status CODE is lost | `eval/engine.cc`, `TrapFromStatus`; V19 |

A message-carrying `{code, expr_id, msg_off, msg_len}` wire upgrade was considered and not taken; message loss is the documented contract (cleanup-backlog #31 stays open for that decision).

### 3.2 Unknowns: the descriptor-offset contract

**The one contract, every layer:** `CEL_UNKNOWN`'s `payload.unk` is a u32 byte offset to a 2-word `{ids_off, len}` **UnknownSet descriptor**; `ids_off` points at a contiguous u32 array of attribute ids in sorted, deduplicated order. `payload.unk == 0` is the legal empty set (no recorded provenance) — production writers never mint it. Real attribute ids are intern ids in `[1, N]` (row 0 of `cel.abi.attributes[]` is the no-attribute sentinel, `abi/cel_abi.proto:99-101`); the function-origin sentinel `kFunctionUnknownSentinel = 0xFFFFFFFF` travels inside a descriptor like any other id.

| layer | behavior | citation |
|---|---|---|
| kernel merge | `cel_unknown_merge` dereferences both descriptors and mints a fresh sorted-deduped union in the bump arena; OOM → `CEL_ERR_OVERFLOW` | `runtime/cel_3vl.c::merge_unknown_descriptors` |
| strict-op absorption | `absorb_3vl_binary` routes both-UNKNOWN operands through `cel_unknown_merge` (after the error scan) — neither side's provenance drops | `runtime/cel_internal.h`; `cel_arith_test.cc::BothUnknownMergesAttributeIdSets` |
| host writers | all mint descriptors via `EncodeUnknownSet` (`eval/internal/cel_host.{h,cc}`): the `cel_get_field` trampoline on a FULL pattern match (`RunFieldPrelude`) and `HostCallContext::{ReturnUnknown,ReturnValue}` allocate in the **guest bump arena** (in-eval; survives until the next $eval's `arena_reset`); the activation marshal (`EncodeUnknownVariable`, `eval/instance.cc`) allocates in the **activation buffer** — the marshal runs BEFORE $eval, whose prelude resets the arena, so an arena-minted descriptor would be zero-filled (same lifetime argument as string payload bytes) | `eval/internal/cel_host.cc`; `eval/host_call_context.cc`; `eval/instance.cc` |
| host readers | both decoders (`Instance::Eval`/`PartialEval` result decode and the host-call arg decode) dereference `{ids_off, len}` and surface EVERY id | `eval/instance.cc::DecodeUnknownSetAt`; `eval/host_call_context.cc::DecodeUnknownSet` |
| user surface | `Value::Unknown` holds the attribute-id SET (sorted, deduped): `Unknown(vector<AttributeId>)` + `UnknownAttributes()` span accessor; the single-id `UnknownAttribute()` works for one-element sets and returns FailedPrecondition on merged sets (silently picking a winner would hide provenance) | `eval/value.{h,cc}` |
| in-guest formatting | `cel_log`'s `%v` unknown formatter dereferences the descriptor | `eval/host/cel_log.cc::FormatUnknown` |

The merged-set requirement is cel-cpp's: both-unknown operands route through `AttributeUtility::MergeUnknowns` (cel-cpp `eval/eval/logic_step.cc`, `attribute_utility.cc:107-130`; strict fns merge after the error scan, `function_step.cc:219`; `AttributeSet::Merge` is a sorted-set union), oracle-pinned by `testdata/cel_cpp_oracle_unknown_payload_test.cc`. E2E pins: `e2e/m2_partial_eval_test.cc::MergedUnknownProvenanceTest` (a&&b / a||b / a+b both-unknown decode BOTH identities; dedup; dotted `a.age && b.age`), both link modes.

Known residuals: a custom host fn called with several unknown args propagates the FIRST arg's set un-merged (`eval/engine.cc::AbsorbUnknownOrErrorArg`) where cel-cpp's function_step would union them; each arg's own set survives intact. Provenance granularity at a `.field` select is the OPERAND's interned attribute (bare root), so `c.age && c.name` both-unknown dedupes to one id where cel-cpp reports two dotted paths.

### 3.3 3VL precedence: two rules, per op class

The precedence between UNKNOWN and ERROR operands is **per-op-class**, not global:

- **STRICT ops** (arithmetic, comparisons, every dispatched function): **ERROR dominates UNKNOWN** across operands, left-bias within each class; both-UNKNOWN merges. The rule is cel-cpp's: `NoOverloadResult` (cel-cpp `eval/eval/function_step.cc:202-223`) scans the args for an `ErrorValue` before merging unknowns; langdef §"Evaluation" leaves multi-error propagation order unspecified, so the reference implementation's behavior is the contract. Oracle-pinned in both operand orders (`PartialEvalOracle.{UnknownPlusErrorIsError,ErrorPlusUnknownIsError}`).
- **LOGIC ops** (`_&&_`, `_||_`): the absorbing bool dominates everything (`false && X = false`, `true || X = true`), then **UNKNOWN dominates ERROR** — cel-cpp's `LogicalOpStep::Calculate` (cel-cpp `eval/eval/logic_step.cc`) merges unknowns BEFORE scanning for errors, because the resolved unknown may later short-circuit the error away. Oracle-pinned both orders for both ops (`UnknownPayloadOracle.{And,Or}{UnknownLeftErrorRight,ErrorLeftUnknownRight}IsUnknown`, `testdata/cel_cpp_oracle_unknown_payload_test.cc`), plus the absorbing-bool controls.

| layer | rule | citation |
|---|---|---|
| runtime kernel `absorb_3vl_binary` (strict ops) | error dominates (both ERROR checks precede both UNKNOWN checks), left-bias within each class; both-UNKNOWN merges via `cel_unknown_merge` (§3.2) | `runtime/cel_internal.h`; pinned both orders in `cel_arith_test.cc::{UnknownLeftErrorRightPropagatesError, ErrorLeftUnknownRightPropagatesError}` |
| cel_host trampolines `AbsorbBinary` (strict ops) | error dominates — aligned to the kernel | `eval/internal/cel_host_error.cc`; pinned both orders in `cel_host_error_test.cc::AbsorbBinaryTest` |
| host-call trampoline `AbsorbUnknownOrErrorArg` (strict: custom fns) | error dominates (scans all args with an explicit `!have_error` guard on the unknown arm) | `eval/engine.cc` |
| runtime kernel `cel_and` / `cel_or` (logic ops) | absorbing bool > UNKNOWN (merged) > ERROR, left-bias within each class | `runtime/cel_3vl.c`; `cel_3vl_test.cc` 4×4 matrices; e2e in `e2e/m5_test.cc::ControlFlowUnknownErrorPrecedenceE2ETest` |

## 4. Wasm binary framing (`abi/wasm_binary`)

Custom-section framing knowledge lives in exactly one module:
`//abi:wasm_binary` (`abi/wasm_binary.{h,cc}`) — core-module preamble
check (`\0asm` magic + version word `0x00000001`; anything else,
including a Component-Model component's `0x0001000d`, is not a core
module), LEB128 read/append, `FindCustomSection` (a duplicate of the
*requested* name is an error), `AppendCustomSection`. The module is
core-module-only: `FindCustomSection` / `AppendCustomSection` reject
component bytes with `InvalidArgument`. It is absl-only — no Binaryen, no
wasmtime — so it sits below both `compiler/` and `eval/`; a magic
constant or LEB decoder anywhere else in first-party code is a review
finding. `eval/internal/abi_decode.cc`'s `cel.abi` walk rides this
module (the evaluator cannot link Binaryen, so the decode side walks
raw bytes).

## 5. Change discipline

**Bumps `runtime_abi_version`** (currently 2): renaming/removing a helper, changing an arity, a return shape, or a namespace. **Does not bump:** adding a helper, adding a proto field (field 8, `required_functions`, landed additively this way — empty table = legacy behavior), appending a `LinkMode` / `CelKind` / `CEL_ERR_*` / `Type.Kind` / `RequiredFunction.Backend` / `Repr` value (`Type.Kind` and `Backend` are open-set at decode by design, §1.4). Append-only enums, and what enforces each:

| surface | rule | enforcement today |
|---|---|---|
| `CelKind` | append at tail, never renumber | comment + downstream fixtures; no static pin |
| `CEL_ERR_*` ↔ `ErrorCode` | append in lockstep, stay cast-equivalent | comment ("MUST mirror"); `cel_host_error_test.cc` enumerates the mapping |
| `ir::Repr` (wire via `VariableEntry.repr`) | values stable on wire | **nothing** — the V6 gap. Implicit numbering, no `= N`, no pin test; a mid-enum insertion silently desynchronizes old Programs from new engines *without* tripping `runtime_abi_version`. Fix: explicit initializers + per-member `EXPECT_EQ(static_cast<uint32_t>(...))` |
| `LinkMode` (×3: public option, internal option, proto) | additive; forwarded by blind `static_cast` | **nothing locks them together** (V25) — a value added to one enum only would miscompile silently; a static_assert pin is pending |
| layout constants | compiler/runtime parity | `static_assert`s in `compiler/memory_layout.h` tie `MemoryLayout` to the `CELWASM_*` macros; `slot_allocator.h` ties `kSlotStride` |

Known un-tied seams (gaps a wire change can fall through): codegen hand-copies a few wire literals (`CEL_BOOL = 1`, `CEL_INT = 2`, the 24-byte stride) into `expr_lower.cc` instead of including `cel_data.h` — a CelValue layout perturbation would pass `//compiler/codegen/...` green (the V11 magic-number probe); the built wasm's export section vs the catalogue has no automated check (V42); the `cel_env` rows sit outside the cel_host bijection CHECK (§2.2's `cel_log` arity drift is the live instance).

Byte-compat pins that must keep passing: dynamic-mode `cel.abi` sections serialize with no field-7 tag; legacy bytes decode as DYNAMIC; unknown `LinkMode` values parse and survive re-serialization (`cel_abi_emit_test.cc`); the empty-surface carve-out keeps versionless synthetic fixtures loading.

The §3.2 unknown wire is settled contract: any new unknown producer/consumer MUST speak the descriptor shape; a raw id in `payload.unk` re-opens the fork. The §3.1 error wire and both §3.3 precedence rules are likewise settled contract.

### `VariableEntry.type` (field 5)

Each declared free variable carries its **full declared type**
alongside `repr`.

`repr` is the wire *kind* the marshal encodes against — `kList`,
`kMap`, `kMessage` — and deliberately says nothing about a list's
element type, a map's key/value types, or a message's fully-qualified
name. `type` carries the rest, using the same `Type` message as
`RequiredFunction.param_types`, so a consumer can describe or bind a
variable without the source declaration in hand.

That is what lets `cel inspect` print `xs:list<int>` rather than
`xs:list`, and `cel run --var xs=[1,2,3]` parse the literal against the
real element type instead of demanding a re-declaration.

**Additive, no `runtime_abi_version` bump.** Programs emitted before
this field carry no `type`; consumers fall back to the bare `repr`
rather than inventing one, since a guessed element type would parse a
literal wrongly. The marshal path never reads it — `repr` still drives
encoding — so the runtime contract is unchanged.
