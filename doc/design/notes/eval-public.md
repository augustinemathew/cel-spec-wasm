# eval-public — design notes (undefined)

Scope: the evaluator's public surface — `eval/{engine,instance,activation,
value,error,attribute,host_call_context,host_callback,typed_function}.{h,cc}`.
Verified against code + tests on branch `m28-configurable-linking`, 2026-06-10.
`eval/dynamic_oom_probe.cc` also lives at this level but is an explicitly
throwaway probe ("DELETE before finishing", dynamic_oom_probe.cc:1-2), not API.

Public Bazel targets (eval/BUILD.bazel, `visibility:public`): `attribute`,
`error`, `value`, `activation`, `host_call_context`, `typed_function`,
`instance`, `engine`.  `host_callback` stays at the package default
`//:internal` (eval/BUILD.bazel:4,127-128) even though `engine.h:45` includes it.

## 1. Verified architecture

### 1.1 Role split and lifecycle

- `Engine` owns the process-shared wasm execution machinery: one
  `wasm_engine_t` + the parsed `cel_runtime.wasm` module, held in a
  `shared_ptr<WasmtimeEngineState>` (engine.h:255, wasmtime_engine_state.h:89-106).
  Built only via `Engine::NewBuilder().Build()` (rvalue-qualified, single-use,
  engine.h:288).  Builder's one knob is `EnableJitPerfMap` (perfmap JIT
  profiling, engine.h:275-282 → `wasmtime_config_profiler_set`, engine.cc:100-102).
- Engine config enables wasm tail calls, threads, and shared memory — required
  because cel_runtime.wasm is built wasm32-wasi-threads and uses
  `__attribute__((musttail))` (engine.cc:80-97).
- `Program` (compiler-side) is pure data — wasm bytes + embedded `cel.abi`
  custom section — and is the serialization boundary between Compiler and
  Engine (engine.h:5-9).
- `Engine::Plan(const Program&) const → StatusOr<Instance>` (engine.h:117-118).
  Per-Plan: fresh store + WASI ctx (engine.cc:128-153), fresh linker with
  `cel_env.cel_log` + `cel_host.*` + WASI preview1 (engine.cc:159-172), expr
  module compiled BEFORE instantiation so imports can be introspected
  (engine.cc:1181-1203).
- `Instance` is the live evaluator: store, linker, runtime+expr instances,
  cached shared-memory handle, `eval` export, decoded ABI, host_env.  It holds
  the engine state `shared_ptr`, so an Instance outlives the Engine handle that
  minted it (instance.h:104-108; pinned by
  `EnginePlanTest.InstanceOutlivesEngineAndCompilerWithEvalProof`,
  engine_test.cc:195).

### 1.2 Plan pipeline (engine.cc:1328-1361)

1. Decode `cel.abi` off the raw Program bytes (no wasmtime state needed);
   NotFound tolerated for ABI-less synthetic WAT fixtures (engine.cc:457-474).
   Populates `host_env.bindings` (field_refs, attributes, descriptor-pool
   lookups via `generated_pool()`).
2. Init store/linker; compile expr module.
3. Link-mode routing is by **import shape**: `is_static = !ModuleImportsCelNamespace`
   (engine.cc:1341).  A present `cel.abi` `link_mode` label is only
   cross-checked as a corruption tripwire; unknown future label values are NOT
   validated (engine.cc:476-498).  Pinned by the four
   `EnginePlanLinkModeTripwireTest` cases (engine_test.cc:981-1095).
4. Dynamic mode: instantiate standalone cel_runtime, bind helpers, then define
   `cel.memory` + every `cel.*` helper export on the linker from
   `abi::CelRuntimeHelpers()` — the single source of truth shared with
   codegen's import pass, so import set and bind set cannot drift
   (engine.cc:246-284, 500-516).
5. Bind embedder extensions: custom modules (instantiated per-Plan,
   `_initialize` called if exported, function exports defined under the alias;
   engine.cc:649-699), `cel_fn.<overload_id>` host callbacks
   (engine.cc:707-731), then Component-Model components (engine.cc:1136-1152).
6. Instantiate expr; pull `eval` export (engine.cc:1208-1228).
7. Static mode only: alias `helpers_instance = expr_instance`, run
   `__wasm_call_ctors` if exported (skips silently if absent), then the shared
   post-instantiate bindings (engine.cc:414-433).

Mode-independent post-instantiate bindings (`BindHelpersInstance`,
engine.cc:391-397): clone the runtime's exported **shared** memory onto the
InstanceImpl (the runtime owns its memory; the host does NOT create it —
engine.cc:120-127, 178-210), enforce DESIGN A13/A14 memory invariants as
`ABSL_CHECK`s (engine.cc:356-376), capture `arena_alloc` + `malloc` func
handles for the host trampolines (engine.cc:289-317), and call
`arena_init(CELWASM_ARENA_CAPACITY_BYTES)` exactly once (engine.cc:323-349).

### 1.3 Engine registration surfaces

All registration methods are **NOT thread-safe**; only `Plan` is
concurrent-safe (engine.h:133-136,150,172,226-230; pinned by
`ConcurrentPlanCallsAllSucceed`, engine_test.cc:148).  Configure once, then
Plan from many threads.

- `AddModule(alias, bytes)` — foreign wasm module under an alias.  Reserved
  aliases rejected: `cel`, `cel_host`, `cel_env`, `cel_fn`, `host`,
  `wasi_snapshot_preview1` (engine.cc:529-533; engine.h:129-130 omits the WASI
  one).  Bytes parsed at registration; helper exports snapshotted from export
  *types* (no instantiation), skipping names starting `_` (engine.cc:1394-1416).
- `AddFunction(overload_id, num_args, HostCallback)` — `num_args` = params + 1
  (out_slot); rejects empty id, arity 0, empty impl, duplicates
  (engine.cc:1422-1444).  `HostCallback =
  std::function<absl::Status(HostCallContext&)>` (host_callback.h:25).
  Callbacks live in a `std::map` precisely so `&callback` node addresses stay
  stable across later insertions — the wasmtime trampoline env captures that
  address (wasmtime_engine_state.h:68-76, engine.cc:709-717).
- `AddTypedFunction(id, lambda)` — header-only sugar: `BindTypedFunction` +
  `AddFunction` (engine.h:190-195).
- `BindFunction(celfn_decl, lambda)` — declaration-first: parses the SAME
  `.celfn` IDL string the compiler takes, requires exactly one `@host.` decl,
  validates the callable's `param_kinds` positionally against declared CEL
  types (`Value` matches anything; `string_view` serves string|bytes; both
  proto spellings serve proto(...); `null`/`type`/`optional` only via `Value`),
  then registers under the synthesized overload-id (engine.cc:1251-1309,
  1448-1469).  Exhaustively pinned by `EngineBindFunctionTest`
  (engine_test.cc:403-744: every accept pairing + every reject pairing +
  arity/backend/parse errors) and e2e
  `HostFnTest.BindFunctionDeclFirstRoundTrip` (e2e/host_fn_test.cc:674).
- `AddComponent(component_bytes, lib)` — **implemented** (despite stale header
  text; see §2): conflict-checks every kForeignComponent overload-id against
  prior AddFunction + prior components BEFORE parsing, then parses the
  component and stores it (engine.cc:1489-1538).  Export resolution + binding
  happen later at Plan: overload-id converted to kebab-case (Component-Model
  identifier rule, engine.cc:1037-1047), optional `wit_interface()` parent
  index, per-decl `cel_fn.<overload_id>` trampoline that lifts CelValues →
  component vals → calls → lowers the single result (engine.cc:1052-1134,
  774-876).  wasip2 components get a deterministic-LCG
  `wasi:random get-random-bytes` stub + trap-stubs for all other unsatisfied
  preview2 imports (engine.cc:894-1023).

### 1.4 Host-call dispatch (m21 layers)

- Layer 0 trampoline (`HostCallbackTrampoline`, engine.cc:604-642): wasm args
  are `(out_slot, arg_slots...)` as i32s, no results.  Before invoking the
  callback it runs 3VL absorption: any CEL_ERROR arg (error wins over unknown)
  or CEL_UNKNOWN arg is copied verbatim to out_slot and the callback is NOT
  run (engine.cc:568-594).  A non-OK callback status becomes a wasm trap via
  `TrapFromStatus`, which must NUL-terminate the message (the wasmtime Rust
  shim panics otherwise — engine.cc:549-566).
- Layer 1 `HostCallContext` (host_call_context.h): kind- and bounds-checked
  accessors over the 24-byte CelValue slots.  Error taxonomy: index out of
  range → OutOfRange, kind mismatch → InvalidArgument, dangling externref →
  FailedPrecondition (host_call_context.h:18-21, verified in
  host_call_context.cc:114,162,248,331,408-450).  `HostListView`/`HostMapView`
  are lazy dual-representation views (host backing OR arena header) valid only
  for the callback's duration (host_call_context.h:63-119).  Return setters
  route through `EncodeValueToSlot` — the same encoder the built-in cel_host
  trampolines use (host_call_context.cc:476-548); `ReturnUnknown` stamps
  `kFunctionUnknownSentinel = 0xFFFFFFFF` to distinguish function-origin
  unknowns from propagated input unknowns (attribute.h:243-252,
  host_call_context.cc:525).
- Layer 2 `BindTypedFunction` (typed_function.h): trait-based adapter from a
  typed lambda/function pointer to `{HostCallback, num_args = arity+1,
  param_kinds}` (typed_function.h:603-628).  Canonical types ONLY — the
  primary `ArgTrait`/`ReturnTrait` templates are always-false static_asserts,
  so `int`, `float`, by-value protos, or non-StatusOr returns are compile
  errors (typed_function.h:127-135, 285-293).  `string_view` params accept
  string-then-bytes (typed_function.h:178-193).  Concrete proto params
  dynamic_cast and reject wrong message types as InvalidArgument
  (typed_function.h:248-266).  `IsCanonicalArg/Return` predicates exist purely
  for must-not-compile test coverage (typed_function.h:366-435).

### 1.5 Instance::Eval / PartialEval (instance.cc)

- `Eval()` (zero-arg): call `$eval`, expect one i32 (result offset), decode the
  CelValue there (instance.cc:1081-1098).  The decoder handles NULL, BOOL,
  INT, UINT, DOUBLE, STRING, BYTES, LIST_ARENA, MAP_ARENA, LIST_HOST,
  MAP_HOST, MESSAGE, UNKNOWN, ERROR, TYPE, DURATION, TIMESTAMP; anything else
  → InvalidArgument "kind N not yet supported" (instance.cc:262-340).  Decoded
  aggregates are deep-copied/re-wrapped into host-owned `Value`s because the
  backings are per-Eval (cleared on `ExternrefTable::Reset`)
  (instance.cc:153-224).  Wire error codes cast directly to `ErrorCode`
  (mirrored numerics); unknown wire bytes fall through to `kHostAdapterError`
  — a legitimately open switch (instance.cc:230-256).
- `Eval(activation)`: resets the externref table + clears unknown_patterns,
  marshals every ABI-declared variable into its workspace slot, then calls
  `Eval()` (instance.cc:1100-1120).  Missing binding → FailedPrecondition;
  declared-Repr vs bound-Kind mismatch → InvalidArgument (instance.cc:1002-1016,
  482-487), with three deliberate coercions: `Value::Null()` binds into any
  scalar slot as CEL_NULL (langdef wrapper semantics; checker is the strict
  gate — instance.cc:589-603); a bound WKT wrapper message peels its inner
  scalar when the FQN matches the declared scalar kind (instance.cc:551-579);
  a bound `google.protobuf.Timestamp`/`Duration` message peels (seconds,
  nanos) (instance.cc:700-733).  `Repr::kEnum`/`kUnknown` marshal is
  Unimplemented (instance.cc:870-875).
- String/bytes/type payload bytes go into a per-Instance **activation buffer**
  malloc'd inside guest linear memory via wasm reentry — NOT the bump arena,
  because `$eval`'s prelude calls `arena_reset` which would clobber them
  (instance.cc:351-359).  A pre-pass sums needed bytes so the buffer is grown
  once before any encoder caches a memory base pointer (instance.cc:889-919,
  1024-1040); 4 KB-rounded growth, ResourceExhausted on guest OOM, A15
  reserved-region CHECK (instance.cc:365-421).
- `PartialEval(activation, unknowns)`: same marshal but with
  `host_env.bindings.unknown_patterns` populated for the duration; a variable
  whose bare attribute is kFull-matched gets CEL_UNKNOWN stamped with its
  interned attribute id **regardless of whether it is bound** — the pattern
  wins over a present binding (instance.cc:946-1000).  Patterns are cleared on
  every exit path so a follow-up Eval can't observe stale state
  (instance.cc:1122-1145).  Field-level (kPartial) absorption happens in the
  cel_host select trampoline, not here.
- `memory_size_bytes()` reads the cloned shared-memory handle
  (instance.cc:1077-1079); exists as the lifetime-test observability hook
  (instance.h:84-95).

### 1.6 Value / Error / Attribute / Activation

- `Value` is a kind-tagged `std::variant`, owns payloads by value, default
  constructs to Null (value.h:218-238, value.cc:30).  ALL builders are real —
  scalars/Unknown/Error/Type in value.cc:39-130; `List/Map/HostList/HostMap/
  Message/OwnedMessage` defined in `eval/internal/cel_host.cc:3983-4016`
  (one-way dep: cel_host → value, never reverse).  Accessors are
  `StatusOr<T>`, kind mismatch → InvalidArgument (value.cc:133-204).
  `StructurallyEquals`: scalars by value, string/bytes/type byte-equality
  (kind tag already disambiguates), aggregates by backing-pointer identity —
  explicitly NOT spec equality (value.h:210-216, value.cc:206-259).
- `Value::Kind` numbering matches the wire `CelKind` only for kinds 0-8; from
  kMessage=9 up it deliberately diverges (wire: CEL_MAP_HOST=9, CEL_MESSAGE=10,
  CEL_TYPE=11, CEL_DURATION=12, CEL_TIMESTAMP=13, CEL_UNKNOWN=15, CEL_ERROR=16,
  CEL_LIST_HOST=17 — runtime/cel_data.h:32-49 vs value.h:62-81).  All
  conversion is via explicit switches, never casts.
- `ErrorCode` numerics DO mirror `CEL_ERR_*` 1:1 and must stay
  static_cast-equivalent (error.h:18-54; relied on by instance.cc:232).
- `attribute.h` mirrors cel-cpp's Attribute model plus `AttributeId` (dense
  u32 index into `cel.abi.attributes[]`, the wire payload of Unknown —
  attribute.h:225-241).  Only string-keyed qualifiers are constructible
  (attribute.h:60-63); `AttributePattern::Parse("a.b.*")` is the embedder
  entry, wildcard `*` segments, root wildcard rejected
  (attribute.h:202-211, attribute_test.cc:279).
- `Activation` maps names to values two ways: `Bind` takes a `Value`
  directly, `BindLazy` registers a callback that produces one on demand.
  Both overwrite any prior binding of the same name (either kind) and
  return `*this` for fluency.  `Resolve` is the single lookup entry point
  — it returns nullptr when unbound, and a non-OK status only when a lazy
  binder failed (propagated verbatim).  A lazy binder runs at most once
  per evaluation and only for a variable the program declares and no
  unknown pattern blanks; `Instance` calls `ClearLazyCache` at the start
  of each marshal so the next evaluation re-invokes it.  The memo cache
  makes `Activation` move-only and not thread-safe — one per evaluation,
  matching `Instance`'s thread-owned model (activation.{h,cc},
  activation_test.cc).
  `OverrideFunction` was removed rather than implemented: per-call
  function override never shipped and had no user, so overriding stays
  an `Engine`-build-time operation (`AddFunction` / `BindFunction`).

### 1.7 Thread-safety model (as documented + tested)

Engine: setup (`Add*`/`Bind*`) single-threaded; `Plan` concurrent-safe (fresh
store/linker/memory per call, sharing only thread-safe engine + parsed module)
(engine.h:113-116,133-136; engine_test.cc:148).  Instance: thread-owned,
single-threaded, bind one per worker (instance.h:4).  Instances outlive Engine
via the shared_ptr chain (instance.h:7-9).  Two-phase doc §4.4 matches.

## 2. Doc-vs-code discrepancies

1. **P1 — engine.h:174-176 says `AddComponent` is "not yet implemented —
   returns Unimplemented"; it is fully implemented** (engine.cc:1489-1538 +
   Plan-time binding engine.cc:1052-1152; tests engine_test.cc:798-893,
   e2e foreign_component fixtures).  The engine.cc:1471-1487 banner comment
   ("forward-declared, not yet wired", "tests SKIP until this returns ok")
   is equally stale.
2. **P1 — engine.h:168-170 claims AddComponent validates "a declared fn is not
   exported... or its exported FuncType does not match the decl's signature →
   FailedPrecondition" at registration.**  Code defers export resolution to
   Plan time (engine.cc:1052-1097), and validates only "export exists" and
   "export is a function" — there is NO FuncType-vs-decl signature comparison;
   an arity mismatch surfaces only at call time as a trap
   (engine.cc:856-861).
3. **P1 — engine.h:99-105 describes Plan as "host-allocates a 2-page
   wasmtime_memory_t... binds it as cel.memory".**  The shipped model is the
   reverse: the runtime module declares + exports its own *shared* memory and
   the host clones the export (engine.cc:120-127, 178-210); `cel.memory` is
   defined on the linker FROM that export, dynamic mode only
   (engine.cc:274-284).
4. **P1 — value.h:9-15 + value.h:108-110: "Aggregate builders
   (List/Map/Message/OwnedMessage) and equality (CelEquals) are... stubs whose
   bodies ABSL_CHECK", "OwnedMessage stays stubbed until M7. Lists land in M4".**
   All are real (cel_host.cc:3983-4016).  No `CelEquals` exists on this
   surface at all; `StructurallyEquals` is the shipped equality (value.h:216).
5. **P1 — value.h:60-61: "Numeric values are stable on the wire — kept in sync
   with runtime/cel_data.h::CelKind".**  False from kind 9 upward (see §1.6;
   value.h's own kType comment at 75-77 admits the numbering is independent).
   Dangerous if a reader trusts it and `static_cast`s between the enums.
6. **P1 — instance.h:59-64 says Eval returns "InvalidArgument if $eval
   returned... LIST/MAP/MESSAGE, etc."** — those (plus DURATION, TIMESTAMP,
   TYPE) all decode to real Values today (instance.cc:262-340); pinned by
   InstanceEvalTest map/list cases (instance_test.cc:414-502).
7. **P2 — engine.h:29-30 "Plan() lands in a later commit; this one only stands
   up the engine"** and **instance.h:11-13 "Eval(activation) → Value lands in
   the next commit"** — both shipped long ago.
8. **P2 — engine.h:16 + instance.h:6 lifecycle text says
   `Plan(program, bindings)`**; actual signature is `Plan(const Program&)`
   only (engine.h:117-118).
9. **P2 — engine.h:131 ("module's wasm bytes fail to parse → InvalidArgument")
   and engine.h:166-167 (same for components)**: parse failures route through
   `WasmtimeErrorToStatus`, which returns **FailedPrecondition**
   (engine.cc:43-51 → 1387-1392, 1527-1532).  Tests only assert `!ok()`
   (engine_test.cc:324-331, 806-815), so the header's claimed code is unpinned
   and wrong.
10. **P2 — engine.h:122-125 says AddModule's "v1 constraint is that the foreign
    module imports cel.memory from the engine"**; code instantiates registered
    modules against the full linker after the runtime is bound — `cel.memory`
    is an available import, not a constraint, and self-contained modules work
    (engine.cc:649-699).
11. **P2 — instance.h:85-86 says memory_size_bytes "reads through the wasmtime
    store, so will crash / UB if the store has been freed"** — it now reads the
    Instance's own refcounted cloned shared-memory handle
    (instance.cc:1077-1079, engine.cc:201-209); the UB scenario is no longer
    constructible while the Instance lives.
12. **P2 — activation.h:44 "cleared on Instance::Reset"** — no `Instance::Reset`
    exists on the public surface (instance.h:37-95).
13. **P2 — two-phase-runtime-isolation.md §4.1/§4.3 (lines ~200-240)**: the
    table row "host-owned wasmtime_memory_t" and §4.3's "Program holds
    shared_ptr<Compiler::WasmtimeState>... Instance holds a back-reference to
    its Program" describe pre-Phase-C shapes; as-shipped Program is pure data
    and Instance holds `shared_ptr<WasmtimeEngineState>` + `InstanceImpl`
    (instance.h:104-108).  Doc is marked shipped-with-deltas but these
    sections weren't reconciled.
14. **P2 — CLAUDE.md "Visibility regime" lists the public eval set as
    `engine,instance,activation,value,error,attribute`**; eval/BUILD.bazel
    also marks `host_call_context` and `typed_function` public (they are
    structurally required: engine.h includes both).
15. **P2 — engine_test.cc:1-9 header says Plan tests use synthetic WAT "because
    the production codegen still emits expr-defines-memory"** — codegen
    imports `cel.memory` now; later tests in the same file use
    `Compiler::Compile` directly (engine_test.cc:870-893).

## 3. Validation items

1. **What does an embedder observe when a BindFunction callback returns a
   non-OK status?**  Code path: `TrapFromStatus(s.message())`
   (engine.cc:637-640) → wasm trap → `WasmTrapToStatus` → `Internal` with
   "Eval trapped: <msg>" (instance.cc:52-60, 1090) — the original status CODE
   is lost (docs-overhaul memory calls this bug #31).  Settle: add an e2e test
   registering `[](...) { return absl::InvalidArgumentError("boom"); }` and
   assert the exact code+message Instance::Eval surfaces; decide whether
   code-loss is contract or bug.
2. **Is error-over-unknown precedence in `AbsorbUnknownOrErrorArg`
   (engine.cc:568-594) spec-correct?**  cel-cpp dispatch arguably gives
   unknowns precedence over errors for strict functions.  Settle: add an
   oracle case to `testdata/cel_cpp_oracle_test.cc` evaluating a custom fn
   with one error arg and one unknown arg under partial eval (extend the
   oracle's activation surface if needed) and compare against the engine's
   propagation choice.
3. **Zero-arg `Eval()` performs no `refs.Reset()` (instance.cc:1081-1098;
   only `Eval(activation)` resets, instance.cc:1107).**  Does repeated
   zero-arg Eval on a program whose host fn `ReturnProto`s grow the externref
   table unboundedly / leak backings across evals?  Settle: probe test calling
   `instance.Eval()` (no activation) N times on such a program and observing
   table growth / decoded results.
4. **AddComponent arity/FuncType mismatch behavior** (per discrepancy #2): a
   component exporting the right name with the wrong arity passes registration
   AND Plan-time binding; the trap fires only at call time
   (engine.cc:856-861).  Settle: extend
   `e2e/foreign_component_dispatch_test.cc` with a wrong-arity WAT component
   and assert where/how the failure surfaces.
5. **engine_test.cc:850 GTEST_SKIP** ("blocked on a real Component-Model
   component fixture") — the named blocker (C.5/D.1 fixtures) appears to have
   landed (e2e/foreign_component_fixtures/ exists).  Settle: un-skip
   `ConflictWithEarlierAddComponentReportedAtRegistration` using an existing
   component fixture and confirm green.

## 4. Test coverage observations

Pinned well:
- Engine lifecycle, move semantics, concurrent Plan, Instance-outlives-Engine
  with an Eval proof (engine_test.cc:88-235).
- Link-mode tripwire matrix incl. mislabeled-both-ways, unknown future label,
  ABI-less module (engine_test.cc:981-1095); all engine tests run under the
  dual-mode `link_mode_cc_test` macro (engine_test.cc:42-47).
- BindFunction registration matrix: every accepting param pairing AND every
  rejecting cross-pairing, arity both directions, backend rejections,
  parse/multi-decl errors, diagnostic message contents
  (engine_test.cc:403-744).
- HostCallContext full arg/return matrix over fakes: boundaries (INT64_MIN/MAX,
  NaN, embedded NUL, empty), wrong-kind negatives per accessor, dangling
  externref slots, arena+host list/map duals, cross-type int/uint map keys,
  nested `map<string, list<proto>>` (host_call_context_test.cc:194-767).
- TypedFunction: arity = params+1, param_kinds metadata order, proto
  concrete-vs-polymorphic, wrong-message-type, status propagation, function
  pointers, canonical-type static_assert predicates (typed_function_test.cc).
- Instance: per-scalar literal evals, determinism across calls, two-instance
  independence, select/has, PartialEval pattern matrix, list/map literal
  round-trips incl. every allowed key kind, string/bytes activation encoders
  (NUL, UTF-8, empty, arena rewind across evals) (instance_test.cc).
- Value scalar matrix + structural equality + kind-name exhaustiveness;
  Activation stub death tests; Attribute pattern/parse matrix incl. reject
  table (value_test.cc, activation_test.cc, attribute_test.cc).
- e2e/host_fn_test.cc drives the full customer flow per type for BOTH
  registration surfaces and proves trampoline wiring + 3VL absorption through
  real wasmtime.

Gaps:
- No test (anywhere in eval/) for the activation-bind coercion paths:
  `TryEncodeWktWrapperMessage`, `TryEncodeWktTimeMessage`,
  `TryEncodeNullToScalarSlot`, `EncodeType`, `EncodeDuration/Timestamp`,
  `EncodeMap/EncodeList/EncodeMessage` at the instance_test level — coverage,
  if any, lives in e2e/m14/known_bugs suites; the unit seam is untested.
- Zero-arg Eval's no-reset behavior (validation item 3) untested.
- AddModule/AddComponent malformed-bytes tests assert only `!ok()`, leaving
  the (wrong) header-claimed status codes unpinned.
- One live GTEST_SKIP in engine_test.cc:850 whose blocker may be gone.
- `dynamic_oom_probe.cc` is a self-declared throwaway still in tree.

## 5. Design decisions worth preserving

- **Compiler/Engine role split with Program as the pure-data boundary** —
  Compiler has no wasmtime dep; Engine has no compile-time dep; Programs are
  serializable across processes (engine.h:5-9, two-phase doc §4.2).
- **Engine-cached parsed runtime module** is bench-justified (~34x per-Plan,
  ~64x with process sharing — engine.h:23-27); per-Plan expr re-parse is the
  accepted M1 default with a named future cache seam (engine.h:75-78).
- **Setup-then-share threading contract**: registration single-threaded, Plan
  concurrent, Instance thread-owned.  The `std::map` node-stability guarantee
  for callback env pointers is load-bearing (wasmtime_engine_state.h:68-76).
- **Link-mode transparency**: routing by actual import shape, ABI label only a
  cross-checked tripwire, unknown labels tolerated (open wire set) — callers
  never branch on link mode (engine.h:80-97, engine.cc:476-498).
- **Runtime-owned exported shared memory** (not host-created): the wasi-threads
  build forces shared memory; the host clones the export handle per Instance
  (engine.cc:186-210).
- **Single source of truth for `cel.*` bindings**: `abi::CelRuntimeHelpers()`
  drives both codegen imports and engine binds; no lazy import tracking
  (engine.cc:234-253, per repo rule).
- **3VL absorption in the trampoline, not the callback**: callbacks only see
  all-known args; emission of unknowns is explicit via `ReturnUnknown` +
  `kFunctionUnknownSentinel` at the far end of the id range (engine.cc:568-594,
  attribute.h:243-252).
- **Activation buffer outside the bump arena**: bound string/bytes/type bytes
  must survive `$eval`'s prelude `arena_reset`; hence guest-side `malloc` via
  wasm reentry, pre-pass sizing to keep the cached memory base pointer stable
  (instance.cc:351-421, 1024-1040).
- **Canonical-types-only typed adapter**: rejected implicit conversions; a
  non-canonical spelling is a compile error naming the type.  The `Bytes`
  newtype was deliberately dropped — string_view serves both, ReturnBytes is
  the bytes-specific escape (m21 doc "what landed" deltas; typed_function.h:15-32).
- **`param_kinds` metadata on TypedFunction** exists so declaration-first
  validation (BindFunction) can compare a lambda signature against parsed
  `.celfn` without re-deriving C++ types (typed_function.h:63-68,603-611).
- **Decoded Values own their state**: every aggregate decode deep-copies out of
  per-Eval backings/arena precisely because `ExternrefTable::Reset` and
  `arena_reset` invalidate them (instance.cc:153-224, 312-321).
- **Permissive null/wrapper bind, strict checker**: the marshaller accepts
  `Value::Null()` into scalar slots and peels WKT wrappers; cel-cpp's checker
  is the strictness gate (instance.cc:551-603, langdef wrapper semantics).
- **TrapFromStatus NUL-termination**: wasmtime's C API trap constructor reads a
  stringz and the Rust shim aborts the process on a non-terminated buffer
  (engine.cc:553-566).
- **Kebab-case translation for component exports**: Component-Model identifiers
  reject snake_case; the engine translates overload-ids consumer-side so
  codegen's wasm import shape stays snake_case (engine.cc:1025-1047).
- **One-way dep value ← cel_host**: aggregate Value builders live in
  cel_host.cc because they need complete backing types; value.h only
  forward-declares the abstract bases (value.h:39-46, cel_host.cc:3975-3980).
