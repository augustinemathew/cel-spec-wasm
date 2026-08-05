# abi-shared — design notes (undefined)

Scope: `abi/` (cel.abi wire contract, runtime-helper catalogue)
and `shared/` (CelType), plus the paired decode side `eval/internal/abi_decode.*`
and its consumers in `eval/engine.cc` / `eval/instance.cc`.

## 1. Verified architecture

### 1.1 The `cel.abi` custom section — wire contract

One serialized `celwasm.abi.CelAbi` proto, wrapped in a wasm custom section
named `"cel.abi"`. Producer: `compiler/internal/compile.cc:416-423`
(`BuildCelAbi` → `WasmModule::AddCustomSection("cel.abi", …)`). Consumer:
`eval/internal/abi_decode.cc:152-163` (`DecodeCelAbiFromWasm`), called from
`Engine`'s `DecodeAbiAndBindHostEnv` (`eval/engine.cc:457-474`).

Emit and decode agree field-for-field **by construction**: both sides link the
same generated proto (`abi/cel_abi.pb.h`; emit at `abi/cel_abi_emit.h:15`,
decode at `eval/internal/abi_decode.h:14`). There is no hand-rolled mirror
struct on the decode side — the header explicitly returns the parsed proto
directly (`abi_decode.h:3-7`). The only hand-rolled wire code is the
custom-section *framing* walk (LEB128 + magic + section ids,
`abi_decode.cc:19-106`), since the decode side cannot link Binaryen.

`CelAbi` fields (`abi/cel_abi.proto:216-275`):

| # | field | emitted from | consumed by |
|---|-------|--------------|-------------|
| 1 | `version` (schema version, constant 1) | `cel_abi_emit.cc:18,46` | **nobody** (see §3.4) |
| 2 | `variables[]` (`VariableEntry{name, local_index, slot_offset, repr}`) | `cel_abi_emit.cc:24-34` from `StaticLayout::variables`, free variables only (`v.kind != kFreeVariable` skipped, line 27) | activation marshal, `eval/instance.cc:896,1053` — linear iteration, lookup by name |
| 3 | `fields[]` (`FieldEntry{id, field_number, name, owner_fqn}`) | `cel_abi_emit.cc:63-71` from codegen's `FieldRefRow` span; row 0 = sentinel, `id` = dense array index | `BuildCelHostBindings` → `cel_get_field`/`cel_set_field` trampolines (`engine.cc:470-472`) |
| 4 | `attributes[]` (`AttributeEntry{id, variable, qualifiers[]}`) | `cel_abi_emit.cc:75-84` from `layout.attributes`; row 0 = sentinel | unknown-pattern matcher for partial eval (`cel_abi.proto:117-169` documents the full produce/propagate model) |
| 5 | `types[]` (`TypeEntry{id, fully_qualified_name}`) | `cel_abi_emit.cc:90-96` from `layout.message_types`; row 0 = sentinel | Plan-time FQN→`Descriptor*` resolution against `generated_pool()` (`engine.cc:470-472`) |
| 6 | `runtime_abi_version` | `cel_abi_emit.cc:55` = `abi::kRuntimeAbiVersion` | `CheckRuntimeAbiVersion` at Plan (`engine.cc:462`) |
| 7 | `link_mode` (enum, DYNAMIC=0/STATIC=1) | caller-supplied to `BuildCelAbi` (`cel_abi_emit.cc:50`) | `ValidateLinkModeLabel` (`engine.cc:483-498`) — validation only, never routing |

Sentinel discipline: id 0 is "no id" uniformly across `fields` / `attributes` /
`types`; the emitter writes the placeholder row 0 so host tables index 1:1 with
the ids codegen burned into the wasm (`cel_abi_emit.cc:59-62,73-74,86-89`).
`FieldEntry.field_number == 0` (not `id == 0`) is the "not proto-resolvable"
marker (`cel_abi.proto:75-79`).

`VariableEntry.repr` carries the numeric value of `ir::Repr`
(`compiler/ir/annotations.h:17-39`), cast straight to `uint32` on emit
(`cel_abi_emit.cc:32`) and decoded by `DecodeRepr` (`abi_decode.cc:110-146`)
with out-of-range → `Repr::kUnknown`, on which the marshal later fails loudly
(`abi_decode.h:22-26`). Rich type info (FQN, list<T>, map<K,V>) is deliberately
NOT on the wire; `repr` alone picks the host encoder (`cel_abi.proto:52-56`,
reserved slot 5 for a future full type).

### 1.2 Decode-side error taxonomy and tolerance policy

`DecodeCelAbiFromWasm` (`abi_decode.h:28-37`):
- `InvalidArgument` — bad magic / wasm version ≠ 1 / truncated LEB128 /
  section overruns / proto parse failure (`abi_decode.cc:46-63,76-92,157-161`).
- `NotFound` — no `cel.abi` section.

`Engine` policy (`engine.cc:440-443,457-474`): `NotFound` is **tolerated** —
the decoded abi stays empty, a variable-free Eval still works, and the
link-mode label goes unvalidated. `InvalidArgument` propagates. When the
section IS present, two gates run:
1. `CheckRuntimeAbiVersion` (`engine.cc:462`) — before the abi is even stored.
2. `ValidateLinkModeLabel` (`engine.cc:1339-1345`) — after import
   introspection computes `is_static`.

### 1.3 Version-check policy (two independent versions)

- **`CelAbi.version` (field 1)** — schema version of the proto message itself,
  constant 1 since inception (`cel_abi_emit.cc:18`). Bumped only on
  renumbering/removal; additive changes keep it (`cel_abi.proto:217-223`).
  Never read by any non-test code (verified by grep across eval/ + compiler/).
- **`runtime_abi_version` (field 6)** — the runtime helper-catalogue version,
  `kRuntimeAbiVersion = 2` (`runtime_catalogue.h:97`). Enforced by
  `CheckRuntimeAbiVersion` (`runtime_catalogue.cc:175-194`):
  - `prog_v == engine_v` → OK.
  - `prog_v == 0` AND section surface empty (no variables/fields/attributes/
    types) → OK (synthetic WAT fixtures still load).
  - `prog_v == 0`, non-empty surface → `FailedPrecondition` "predates ABI
    versioning; recompile".
  - otherwise → `FailedPrecondition` naming **both** versions.
  Bump policy: rename/remove/arity/return-shape/namespace change → bump;
  additive helper → no bump (`cel_abi.proto:256-261`,
  `runtime_catalogue.h:47-53`). Hard rejection by design — the alternative is
  wasmtime's opaque type-mismatch trap at first call (`cel_abi.proto:246-249`).

### 1.4 Link-mode label semantics

`LinkMode` is an enum, not a bool, for additive future modes
(`cel_abi.proto:194-213`). `LINK_MODE_DYNAMIC = 0` deliberately: pre-link-mode
Programs decode to the shape they actually have (proto3 default). The label is
**metadata + tripwire**, never a routing input — the engine routes on import
introspection (`is_static` ⇔ no `cel.*` imports); contradiction between label
and shape → `FailedPrecondition` (`engine.cc:476-498`). Unknown future enum
values skip validation (open-set wire, `engine.cc:480-482`). Byte-level compat
is pinned: dynamic mode serializes with no field-7 tag at all, i.e.
byte-identical to pre-link-mode sections (`cel_abi_emit_test.cc:208-219`).

### 1.5 The runtime catalogue (`abi/runtime_catalogue.*`)

Single source of truth for every wasm import an expr module declares, across
four namespaces (`runtime_catalogue.h:69-74`, `AbiModuleName` at
`runtime_catalogue.cc:49-62`): `cel` (cel_runtime.wasm pure-wasm helpers),
`cel_host` (wasmtime trampolines), `cel_env` (env helpers, currently only
`cel_log`), `cel_fn` (user customs — NOT catalogued; `FindBuiltinHelper(kCelFn,…)`
returns nullptr by design, `runtime_catalogue.cc:210-213`).

The catalogue entry type is the **generated proto**
`celwasm::abi::CelRuntimeFunction` (`runtime_catalogue.proto:64-69`:
`{name, module, num_args, returns_i32}`) — there is no hand-defined POD
(`runtime_catalogue.h:78-92`). Data flow:
- `cel` rows are DERIVED from `// cel:codegen-export` markers on the C
  declarations in `runtime/cel_*.{h,c}` by `//bazel:gen_runtime_catalogue`
  (membership from the marker; arity/return shape from the `void`/`uint32_t`
  C signature, which clang lowers 1:1 to the wasm type) —
  `runtime_catalogue.proto:8-24`, `abi/BUILD.bazel:38-49`.
- `cel_host`/`cel_env` rows are hand-maintained in the committed, commented
  `abi/runtime_host_env.textproto` (20 host + 1 env rows; arg semantics
  documented per row). The genrule prepends that file verbatim and appends
  the derived `cel` rows (`runtime_host_env.textproto:9-14`).
- The composed textproto is embedded as a string literal
  (`//abi:runtime_catalogue_textproto_cc`, `abi/BUILD.bazel:54-82`) so
  `CelRuntimeHelpers()` is a pure in-process call with no runfiles dependency
  (`runtime_catalogue_textproto.h:8-12`); parsed once at first use with a
  CHECK on malformed genrule output (`runtime_catalogue.cc:73-84`).
- Per-namespace spans + name→entry hash indexes; cross-namespace name
  collisions are BY DESIGN (`cel.cel_list_at` dispatcher vs
  `cel_host.cel_list_at` trampoline) — `runtime_catalogue.cc:122-126`,
  pinned by `runtime_catalogue_test.cc:45-57`.

Linker export set: `runtime/wasm_exports.txt` now holds **only** the
`[host-only]` section; codegen-helper export names are genrule-derived from the
same C markers and UNIONed in (`runtime/BUILD.bazel:43-81`,
`wasm_exports.txt:1-7`). The old `runtime_catalogue_consistency_test` was
deleted as tautological once both sides derived from one source
(commit 511c3ec8, 2026-05-26).

Trampoline cross-check: names + arities in the host/env rows MUST match the
trampolines registered in `eval/internal/cel_host_wasmtime.cc`; a startup
cross-check CHECK-fails on drift (`runtime_host_env.textproto:19-21`,
`runtime_catalogue.cc:104-108`).

Calling-convention invariants encoded in the data: `num_args` is the exact
i32 param count (no out-slot semantics layered on); `returns_i32` is false for
every host/env trampoline (results via out_slot in linear memory) and true only
for runtime arena/iter/count helpers (`runtime_catalogue.proto:53-63`,
enforced by `runtime_catalogue_test.cc:91-104`).

### 1.6 `abi/wit/` — removed

> **Removed 2026-08-04 (m39):** `abi/wit/` (the Component-Model WIT
> vocabulary, `cel.wit` + README) was deleted with the plugin backend;
> it is preserved on the `component-functions-archive` branch. No WIT
> contract exists in the tree.

### 1.7 `shared/CelType` — the declaration-type vocabulary

`shared/type.h` is a leaf, value-semantic type (no protobuf, no compiler dep;
`shared/BUILD.bazel:3-15`, public visibility). Both `compiler/` and `eval/`
consume it (verified: `compiler/compiler.h`, `eval/host_call_context.cc`,
`eval/internal/cel_host.cc`, …) — it is the
vocabulary the two halves share without depending on each other.

- Kinds: `kUnknown=0, kBool=1 … kMessage=9, kDuration=11, kTimestamp=12,
  kType=13` (`type.h:25-43`). **Slot 10 is deliberately skipped** to keep
  numbering aligned with `Value::Kind` (`eval/value.h:62-81`, which documents
  the same 9→11 gap and why `kType` landed at 13). Note this numbering is
  distinct from BOTH `ir::Repr` (the wire `repr` numbering — Repr `kInt`=3 vs
  CelType `kInt`=2) and the runtime's wire `CelKind`.
- Construction only by named factory; default-constructed = `kUnknown`
  sentinel, "not a valid declaration" (`type.h:90-92`).
- Containers hold children via `shared_ptr` (shallow-copy cheap pass-by-value;
  `type.h:96-98`); equality recurses structurally (`type.cc:103-121`).
- Wrong-kind accessors `ABSL_CHECK`-fail naming both the accessor and the
  actual kind (`type.cc:80-101`), pinned by death tests
  (`type_test.cc:77-90`).
- No `Null`, `Optional`, `Error`, or `Unknown`-as-declarable factories — the
  declarable surface is intentionally narrower than the Value model.

## 2. Doc-vs-code discrepancies

1. **P1 — `cel_abi.proto:227-229` claims `variables[]` is positional by
   `local_index`** ("position i holds the entry whose `local_index == i`"),
   but `EmitVariables` skips comprehension-scope locals
   (`cel_abi_emit.cc:27`), and `ResolvePass` interleaves iter/accu locals into
   the same dense index space as free variables
   (`resolve_pass.cc:206-216`). A free variable first referenced inside a
   comprehension body (e.g. `y` in `xs.map(i, i + y)`) should get
   `local_index` 3 while sitting at `variables[1]`. Consumers don't rely on
   the positional claim (linear name-keyed iteration,
   `instance.cc:896,1053`), so no runtime bug — but the wire-contract doc
   states an invariant that appears false. No test pins either way.
2. **P1 — `abi-refactor.md` §2 (lines 99-112) and §4 Slice A (160-168)
   describe a hand-written `struct AbiHelper` POD + `K_AT_V`-style macros**;
   code has neither — the catalogue is the generated proto
   `CelRuntimeFunction` parsed from an embedded textproto derived from C
   markers (commit 511c3ec8 "kill AbiHelper struct", 2026-05-26;
   `runtime_catalogue.h:78-81`). The doc is marked "shipped 2026-05-22" and
   was not reconciled by the later refactor.
3. **P1 — references to the deleted `runtime_catalogue_consistency_test`**:
   `runtime/BUILD.bazel:12-16`, `runtime/wasm_exports.txt:14-16`, and
   `abi-refactor.md:230-238,253-254,478-479` all describe a test that commit
   511c3ec8 removed as tautological. Maintainers are pointed at a
   nonexistent drift gate; the actual guarantee is now "both derive from the
   same markers".
4. **P1 — `abi-refactor.md` §6 case 23 (line 431)** says a module with no
   `cel.abi` section is rejected at Plan with
   `InvalidArgument("missing cel.abi section")` "already today"; code
   **tolerates** `NotFound` and proceeds with an empty abi
   (`abi_decode.h:33-35`, `engine.cc:440-443,467-468`).
5. **P1 — `shared/type.h:2-4`** says CelType "mirrors the kinds carried on
   the wire in `cel.abi.CelType`"; no `CelType` message exists in
   `abi/cel_abi.proto` — the wire deliberately carries only the `repr` u32
   (`cel_abi.proto:52-56`). The referenced message is an unimplemented design
   sketch in `cel-host-surface.md:1182-1201`.
6. **P1 — `ir::Repr` uses implicit enum numbering**
   (`annotations.h:17-39`) while `cel_abi.proto:48-50` promises "values are
   stable on wire; … never a renumbering". Nothing (no explicit `= N`, no
   static_assert, no test) pins the numeric values; an insertion mid-enum
   would silently renumber every emitted `repr` and desynchronize old
   Programs from new engines without tripping `runtime_abi_version`.
   Currently correct (kOptional was appended), but unenforced.
7. **P2 — `runtime_catalogue.proto:43-49`** says "Only `CEL` rows are
   generated today; the `CEL_HOST`/`CEL_ENV` members reserve their slots so
   the import arrays can later move into the catalogue" — the host/env rows
   ARE in the composed catalogue already (`runtime_host_env.textproto` +
   `runtime_catalogue.cc:109-120` filter them out of the same parse). The
   message-level comment "One runtime helper exported by cel_runtime.wasm"
   (`runtime_catalogue.proto:52`) is likewise too narrow.
8. **Resolved-by-removal (m39):** the `abi/wit/README.md` staleness
   finding is moot — `abi/wit/` was deleted with the plugin backend
   (archived on `component-functions-archive`).
9. **P2 — stale counts/structure in `abi-refactor.md`**: §2 "~146 entries"
   vs §4 "167 entries" for the `cel` set; §4 Slice C describes
   `wasm_exports.txt` as having `[codegen-helpers]` + `[host-only]`
   sections, but the file now holds only `[host-only]`
   (`wasm_exports.txt:3-7,22-23`). Also case 22's "InvalidArgument with byte
   offset" — the decode error carries no byte offset
   (`abi_decode.cc:158-160`).

## 3. Validation items

1. **Variables positional invariant under comprehensions.** Compile
   `xs.map(i, i + y)` with `{"xs:list<int>", "y:int"}` via
   `Compile()` (pattern of `abi_decode_test.cc:155-168`), decode, and check
   `decoded->variables(1).local_index()`. If it's 3 (not 1), discrepancy #1
   is confirmed → fix the `cel_abi.proto` comment (or change the emitter to
   re-densify and update the `$eval` prelude contract).
2. **Repr numeric pinning.** Add a test (or explicit `= N` initializers +
   comment) asserting `static_cast<uint32_t>(Repr::kX)` for every member,
   e.g. `EXPECT_EQ(static_cast<uint32_t>(Repr::kOptional), 15u)`. Settles
   whether discrepancy #6's hazard is real before anyone touches the enum.
3. **Is an optional-typed free variable reachable on the wire?** `DecodeRepr`
   has no `kOptional` arm (decodes to `kUnknown`, `abi_decode.cc:110-146`)
   while the emitter would happily stamp 15. Probe: attempt to declare a
   variable with an optional type (variable_specs string form, and the
   `FunctionLibrary`/`DeclareVariable` path) and compile `x`; if any path
   accepts it, the round trip silently degrades to a loud-but-confusing
   "kUnknown repr" marshal error. Expected: frontend rejects; confirm and
   document the asymmetry as unreachable-by-construction.
4. **`CelAbi.version` (field 1) enforcement policy.** No code validates it
   (grep: zero non-test readers). Probe: hand-roll a section with
   `version=99` + one variable and run `Engine::Plan` — it will load. Decide:
   is "schema version exists but is never checked" the intended policy
   (proto3 unknown-field tolerance makes it mostly moot), or should Plan
   reject unknown major versions? Either way, record it in the new design
   doc; today only `runtime_abi_version` gates.
5. **Catalogue ⇄ actual wasm exports.** Post-deletion of the consistency
   test, confirm the by-construction claim end-to-end once per toolchain
   bump: `wasm-dis bazel-bin/runtime/cel_runtime_wasm.bin/cel_runtime.wasm |
   grep '(export' | sort` vs the names in `CelRuntimeHelpers()` — a marker on
   a declaration whose definition was dropped would only surface at wasm-ld.

## 4. Test coverage observations

**Well pinned:**
- Emit: per-field variable round trip incl. all four scalar reprs and slot
  contiguity (+24/slot, `cel_abi_emit_test.cc:89-101`); field sentinel row;
  proto serialize/parse round trip; link-mode **byte-level** compat — legacy
  hand-rolled bytes (`{0x08,0x01}`) decode as DYNAMIC, STATIC bytes ==
  DYNAMIC bytes + `\x38\x01`, and unknown future enum value 2 parses and
  survives re-serialization (`cel_abi_emit_test.cc:192-239`).
- Decode: happy path on hand-built section framing, 9 scalar reprs through
  the full byte path, real-compiler round trips (incl. nested select
  `c.billing_address.city` field table with owner FQNs), and the complete
  error taxonomy — short stream / bad magic / wasm version 2 / missing
  section / other-name section / malformed payload / truncated section /
  oversize LEB128 (`abi_decode_test.cc:221-276`).
- Catalogue: per-namespace name uniqueness, the expected cross-namespace
  collision set, arity bounds (0-5; host ≥ 2 for out_slot), module
  assignment, host/env void-return invariant, lookup totality + negative
  lookups, arity canaries, and **every branch** of `CheckRuntimeAbiVersion`
  including both-versions-in-message (`runtime_catalogue_test.cc`).
- CelType: factories, equality recursion (incl. nested containers), death
  tests on wrong-kind accessors.

**Gaps:**
- No test exercises `variables[]` emission for a program containing a
  comprehension (the `kind != kFreeVariable` skip at `cel_abi_emit.cc:27`
  is untested; see validation item 1).
- `DecodeRepr` is only exercised through scalar reprs; `kList/kMap/kMessage/
  kEnum/kType` wire values and the out-of-range→`kUnknown` clamp have no
  direct test, and `Repr::kOptional` (15) silently clamps.
- No test pins `Repr`'s numeric values (validation item 2).
- `ValidateLinkModeLabel` and the engine-side NotFound-tolerance policy are
  tested in eval-side suites, not here; the emit test file's claim that
  "decoder library tests land in a following commit"
  (`cel_abi_emit_test.cc:103-106`) did land (`abi_decode_test.cc`).
- CelType: no test for `Type()` factory kind, `map_value()` death, or
  `CelTypeKindName` totality (test samples 5 of 13 kinds).

## 5. Design decisions worth preserving

- **Two versions, two jobs.** `CelAbi.version` = proto schema shape;
  `runtime_abi_version` = helper-catalogue compatibility. Only the latter is
  enforced, and hard-rejection (vs tolerate-and-trap) is deliberate: the
  whole catalogue exists to replace opaque wasmtime traps with named
  FailedPreconditions (`cel_abi.proto:244-249`, `abi-refactor.md` §5 policy
  notes). Additive helper = no bump; rename/remove/arity/return-shape = bump.
- **`prog_v == 0` + empty surface loads** — keeps synthetic WAT fixtures and
  the wat_runner harness working without stamping versions into hand-written
  fixtures (`runtime_catalogue.cc:179-188`).
- **`LINK_MODE_DYNAMIC = 0` is load-bearing**: proto3 default makes every
  pre-field-7 Program decode to its true shape, and dynamic-mode sections
  stay byte-identical to legacy sections. Label is tripwire-only; routing
  derives from import introspection. Unknown enum values pass through
  unvalidated (open wire set).
- **Sentinel row 0 everywhere** (fields/attributes/types): id 0 = "no id",
  emitted as a real placeholder row so host arrays index 1:1 with the ids
  burned into wasm — uniform bounds checks, no off-by-one mapping layer.
- **Attribute granularity is field-path only, by design**: `[k]`/`[i]` never
  extend qualifiers; `AttributePattern::Parse` rejects bracket qualifiers
  rather than accept patterns that would silently match nothing
  (`cel_abi.proto:130-169`). FieldEntry (HOW to read) vs AttributeEntry
  (WHAT path, for unknown matching) are distinct tables populated together.
- **Minimal wire, derived facts stay derived**: only `repr` crosses for
  variables; full `CelType` is a reserved additive slot. No CheckedExpr on
  the wire (v1 embedded it; v2's host runs no second analysis pass)
  (`cel_abi.proto:11-23,52-56`).
- **Catalogue from C markers, not from the wasm binary**: the C signature
  lowers 1:1 to the wasm type, so the generator needs no disassembler or
  cross-compile (`runtime_catalogue.proto:10-17`). The rejected alternative —
  hand-synced POD array + consistency tripwire test — was deleted because a
  tripwire detects drift but doesn't eliminate it (commit 511c3ec8;
  `abi-refactor.md` §4 Slice C records the same reasoning for the linker
  list). Host/env imports stay a committed, commented textproto because they
  have no C export to derive from, guarded by the cel_host_wasmtime startup
  bijection CHECK.
- **Cross-namespace name collisions are intentional** (`cel.cel_list_at`
  dispatcher tail-calls `cel_host.cel_list_at`); lookups are
  `(module, name)`-keyed, and a test pins the expected collision set so a
  cleanup that "fixes" the duplication surfaces loudly.
- **Embedded textproto, not runfiles** — `CelRuntimeHelpers()` is callable
  from non-test library code (`overload_table.cc`, `cel_abi_emit.cc`) with no
  cwd assumptions (`runtime_catalogue_textproto.h:8-12`).
- **`cel_fn` is open-set by design**: user custom-fn arities come from
  per-compile registration, and `FindBuiltinHelper(kCelFn, …) == nullptr`
  keeps the catalogue from ever pretending to know them.
- **CelType numbering tracks `Value::Kind`** (gap at 10, `kType=13`) and is
  intentionally NOT the wire `repr` numbering — three enums, three jobs
  (declaration vocabulary / IR-and-wire repr / runtime value kind); any new
  design doc must state all three and their alignment rules.
