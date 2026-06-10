# M13 code-layering / factoring review — codegen + api + compile.cc

Date: 2026-05-24
Reviewer: codegen/api layering pass (report-only; no code changed).
Scope: `compiler/codegen/{expr_lower*,layout_pass,resolve_pass,overload_table,module}`,
`eval/{engine,instance,compiler,activation,function?,host_callback,internal/cel_host*}`,
`compiler/internal/compile.{h,cc}`, `compiler/celfn/library_module.h`, against
`modules-and-ffi.md` §4, `m13-custom-fns.md` §4/§7/§12,
`per-component-test-coverage.md`, and `CLAUDE.md`.

## Verdict: MIXED.

The host backend is clean and shipped end-to-end. The CEL-defined
single-module backend is **half-wired scaffolding that contradicts
its own design doc**, and its codegen entry point has **zero direct
test coverage**. The top three items:

1. **P1 — `LowerToCustomFn` / `EmitCustomFnParamPrelude` are
   dead-but-shipped and untested.** Defined in `expr_lower.cc`
   (lines 1137-1216), referenced by nothing outside their own TU,
   and `compile.cc::Compile` never calls them. No `TEST` exists for
   either. They are simultaneously the most novel codegen in M13 and
   the least covered. (§A, §D)

2. **P1 — separate-module scaffolding directly contradicts the
   single-module decision.** `library_module.h::CompileLibraryBodies`
   (returns `std::vector<uint8_t>` module bytes), `compile.h`'s
   `LibraryModuleBytes` struct + `CompiledArtifact::library_modules`,
   and `LayoutOptions::rodata_base_override`'s doc comment all describe
   the retired "bundle bodies into a separate `foo.wasm`" model. The
   design (`modules-and-ffi.md` §4 as briefed: single module, disjoint
   bands, no `__memory_base`) is not what the headers say. There is no
   `library_module.cc` — the declaration is a dangling stub. (§C)

3. **P2 — stale comment at `compiler.h:169`** claims `Compiler::Compile`
   does not consume `function_libraries_`; it has since `fe420f3`
   (Slice C.3). The whole "Storage only until Slice C.3" block is now
   false. (§B)

`bazel test //...` being green here is exactly the trap
`CLAUDE.md` warns about: the green suite does not exercise
`LowerToCustomFn` at all.

---

## A. codegen/ layering and factoring

### A.1 expr_lower decomposition — mostly clean, one mis-seated arm

The earlier split (`expr_lower.cc` 1218 LoC + `expr_lower_comprehension.cc`
1018 LoC + `expr_lower_internal.h`) is a good seam. `EmitCtx`,
`Emit()`, and the shared primitives (`EmitCelCopySlot`,
`LoadSlotI32Eq/Ne`, `EmitCelListAppendCall`) live in the internal
header and are correctly shared. The `Emit` dispatcher
(`expr_lower.cc:996`) is a clean closed switch with a loud
`ABSL_CHECK(false)` default — exactly per `CLAUDE.md`.

`expr_lower.cc` is **not** a god-file by line count, but it now holds
five structurally distinct lowering families in one anonymous
namespace: const/ident loads, select (`EmitKSelect`), aggregates
(`EmitKMapExpr`/`EmitKListExpr`/`EmitKStructExpr`/`EmitKIndexCall`),
general call + ternary + cross-numeric repick (`EmitGeneralCall`,
`EmitConditional`, the `CrossNumeric*` family lines 645-805), and the
custom-fn lowerer (`LowerToCustomFn` + helpers). The natural seams,
in priority order:

  - **Custom-fn lowering should be its own TU** (`expr_lower_customfn.cc`)
    — see A.2. P1.
  - The `CrossNumeric*` overload-repick family (lines 645-805, ~160
    LoC of pure string-mapping) is self-contained and could move to
    `overload_table.cc` or a `cross_numeric.cc`; it has nothing to do
    with Binaryen emission. P2.
  - The aggregate arms (map/list/struct/index, ~400 LoC) are a
    plausible `expr_lower_aggregate.cc` if the file keeps growing,
    but today they are fine in place. P2 / defer.

**Finding A.1.** Custom-fn TU split — **P1, ~1h** (mechanical move +
BUILD target). CrossNumeric extraction — **P2, ~1h**.

### A.2 `LowerToCustomFn` + `EmitCustomFnParamPrelude` placement

These (`expr_lower.cc:1137-1216`) are the CEL-defined-body lowerer.
They differ from `LowerToEvalFunction` in three orthogonal axes
(wasm params instead of nullary; no `arena_reset`; `cel_copy_slot`
write-back via `EmitCelCopySlotDyn` instead of return-by-value) and
carry their own helper zoo (`EmitCelCopySlotDyn`,
`EmitCustomFnParamPrelude`, the `param_for_name` map, the
`wasm_local_offset` convention threaded through `EmitCtx`).

They belong in **`expr_lower_customfn.cc`**, sharing
`expr_lower_internal.h` exactly as the comprehension TU does. Reasons:
(a) the body-lowering calling convention is a distinct ABI surface
(`modules-and-ffi.md` §4 / `m13` §4.1), worth isolating like the
comprehension surface was; (b) it keeps `expr_lower.cc` focused on
the `$eval` shape; (c) `wasm_local_offset` is the only field on
`EmitCtx` that exists *solely* for custom-fn bodies — its documentation
already forward-references `library_module.cc::LowerCelDefinedFn`
(`expr_lower_internal.h:51`), a function that does not exist. Putting
the lowerer in its own TU makes that coupling explicit instead of
leaking custom-fn concerns into the shared context's doc comments.

**Finding A.2.** Move `LowerToCustomFn` + `EmitCustomFnParamPrelude` +
`EmitCelCopySlotDyn` + the `CustomFnParam` struct to
`expr_lower_customfn.{cc}` (declarations stay in `expr_lower.h`).
**P1, ~1.5h.** Note `expr_lower_internal.h:51` references a phantom
`library_module.cc::LowerCelDefinedFn` — reconcile the comment when
the real caller lands.

### A.3 Single-module band orchestration — wrong layer as designed

The briefed design (single module: expr + N bodies, disjoint
workspace bands, cumulative rodata bases, `ResourceExhausted` guard)
requires an orchestrator that: lays out each body
(`LayoutPass` with `rodata_base_override` = cumulative high-water +
`reserved_region_limit_bytes` = band end), then emits each body via
`LowerToCustomFn` into the **same** `WasmModule` the expr is in,
skipping the import for CEL-defined overload-ids.

`library_module.h` currently puts this in `compiler/celfn/` and
has it **return separate module bytes** — the wrong shape twice over:
it is the wrong *layer* (band-partitioning + calling-convention
orchestration over a single `WasmModule` is codegen, not IDL/celfn
concern) and the wrong *output* (bytes of a second module, not
functions appended to the expr module). For the single-module model
the orchestration should live in **`codegen/`** (e.g.
`codegen/custom_fn_emit.{h,cc}` or folded into `compile.cc`'s
extracted helper, see §C), operating on the live `WasmModule&`, and
`compiler/celfn/` should retain only the parse/IDL rep
(`function_library.*`) — no wasm production.

**Finding A.3.** Band-partitioning + body emission orchestration
belongs in `codegen/`, over a shared `WasmModule&`, not in
`celfn/library_module.cc` returning bytes. The `celfn/` package
should stay IDL-only. **P1, design-correction; ~0.5h to delete the
wrong header + re-home the contract, more for the real
implementation.**

### A.4 LayoutOptions surface

`LayoutOptions` has three knobs: `debug_layout`, `rodata_base_override`,
`reserved_region_limit_bytes`. The latter two are coherent *as a pair*
for the single-module model (each body gets a band: shifted rodata
base + its own region limit), and the `ResourceExhausted` guard
(`layout_pass.cc:405-414`) is exactly the right tripwire per
`modules-and-ffi.md` §4.4. **However** the doc comment on
`rodata_base_override` (`layout_pass.h:58-64`) still says it exists
"to give each bundled CEL-defined-fn body a non-overlapping rodata
range in the shared `cel.memory`... two modules instantiated against
the same memory" and cites `wat/45b_foo_module.wat` — the retired
multi-module model. Under the single-module decision the knob is
correct but the *rationale* is stale (it is now intra-module band
offsetting, not cross-module memory disjointness). Not accreting knobs;
the surface is fine. **P2, doc-only, ~15m.**

`LayoutPass` itself (`layout_pass.cc:342-417`) is 75 lines / well-
structured into lettered passes A-E and a guard. It is over the
60-line `readability-function-size` threshold but is a sequence of
named single-statement pass invocations, not deep logic; if the linter
flags it, the clean split is "extract the guard + each pass into a
helper". Not currently in `lint-backlog.md`. **P2, ~30m if flagged.**

### A.5 Function-size scan

`lint-backlog.md` records `expr_lower.cc` at **0 warnings** (the old
`LowerCall` offender was split). The comprehension TU has many small
helpers (good). No new function-size violations spotted by eye in the
M13-touched code, but `LowerToCustomFn` (55 LoC) is near the limit and
will trip the threshold the moment band-orchestration or
pointer-relocation (`modules-and-ffi.md` §4.3) is added inline — another
reason to split it out now (A.2) rather than grow it in place.

---

## B. api/ layering and factoring

### B.1 HostCallback vs typed FunctionImpl boundary — INCOMPLETE, not unclean

`Engine::AddFunction(overload_id, num_args, HostCallback)`
(`engine.h:148`, `engine.cc:695`) takes the **raw**
`HostCallback = std::function<absl::Status(uint8_t* mem, size_t,
uint32_t out_slot, Span<const uint32_t> arg_slots)>`
(`engine.h:76`). The trampoline (`engine.cc:412 HostCallbackTrampoline`)
adapts wasmtime's `func_callback_t` to that raw callback cleanly.

The typed `FunctionImpl = AnyInvocable<Value(Span<const Value>)>`
lives in `activation.h:29` and is **not** wired into `Engine` at all —
the e2e test (`engine_test.cc:369 ExprCallsHostStringLengthAndEvaluates`)
hand-rolls the CelValue `memcpy` decode/encode inside a raw
`HostCallback` lambda (lines 381-390). So the "typed `FunctionImpl`"
the `engine.h:72` comment promises ("Slice C.2 wires the typed
`cel::FunctionImpl`...") does **not exist** in the engine path. The
boundary isn't *unclean* — there is simply no typed layer yet; every
embedder must write the byte-shuffling by hand.

**Finding B.1.** The raw→typed adapter (decode arg slots into `Value`
per declared `arg_types[]`, call `FunctionImpl`, encode result, write
`kError` on kind-mismatch — exactly `m13` §4.2 steps 1-4) is unbuilt.
Until it lands the `engine.h:72` comment is aspirational. **P1 if M13
closeout requires the typed surface (the §2.1 API sketch shows
`engine.AddFunction("upper_string", upper_impl)` with a typed impl);
P2 if raw-only is acceptable for M13.** Either way the comment must
stop claiming it is wired. ~3-4h to build the adapter + tests.

### B.2 Compiler / Engine / Instance separation — sound

The three-way split holds for the custom-fn flow: `Compiler` declares
(accumulates `function_libraries_`, wires the checker + OverloadTable
via `BuildOverloadTable`), `Engine` binds impls
(`AddModule`/`AddFunction` populate `WasmtimeEngineState` maps,
`RegisterHostCallbacks` defines `cel_fn.<id>` on the linker per-Plan),
`Instance` evals. Reserved-alias validation (`engine.cc:363-374`) and
duplicate detection are at registration time, per design. No layering
complaint here.

One adjacency note (the "drift by adjacency" the review process asks
for): `InstantiateAndBindCustomModules` (`engine.cc:451`) already
implements the `_initialize` reactor-coexistence dance + per-helper
export binding for **foreign** modules. When the CEL-defined backend
lands as a single module (no separate instance), this function should
*not* grow a CEL-defined branch — CEL-defined bodies are in the expr
module and need no instantiation/binding. Keep that invariant explicit
so a future slice doesn't route CEL-defined libs through the foreign
path by reflex.

### B.3 Stale comments

  - `compiler.h:169-178` — "**Storage only until Slice C.3.**
    `Compiler::Compile` does not yet consume this field". False since
    `fe420f3`. **P2, ~10m.**
  - `engine.h:72` — see B.1; "Slice C.2 wires the typed
    `cel::FunctionImpl`" — not wired. **P1/P2, ~10m for the comment.**
  - `expr_lower_internal.h:51` — references
    `library_module.cc::LowerCelDefinedFn`, which does not exist.
    **P2, ~10m.**

---

## C. compile.cc orchestration

`Compile()` (`compile.cc:416-453`) is currently **clean** — it is a
38-line driver delegating to `RunFrontAndLayout`, `InstallExprModuleImports`,
`BuildOverloadTable`, `InstallOverloadImportsExport`,
`LowerToEvalFunction`, `AttachCelAbiSection`, `FinaliseModule`. The
prior factoring (front/back-half extraction to keep under the lint
threshold) is good.

**But it does not yet do CEL-defined body lowering at all.** There is
no plan-body-layouts → install-segments → skip-CEL-defined-imports →
lower-bodies sequence. When that lands, `Compile()` *will* balloon past
the threshold. The clean extraction to do **now, before** adding it:

  - `EmitCustomFnBodies(WasmModule&, const std::vector<FunctionLibrary>&,
    const OverloadTable&, ...)` in `codegen/` (per A.3) — owns band
    partitioning (cumulative rodata base + per-body
    `reserved_region_limit_bytes`), per-body `LayoutPass` +
    `LowerToCustomFn`, and returns the merged `field_refs` / updated
    high-water. `Compile()` calls it once, right after
    `LowerToEvalFunction`.
  - The import-installation must **skip CEL-defined overload-ids**
    (their bodies are sibling functions, not imports). Today
    `InstallOverloadImportsExport` (`compile.cc:230`) installs an
    import for **every** OverloadTable row regardless of backend.
    `BuildOverloadTable` (`compile.cc:392-412`) maps `kHost`→`kCelFn`
    and everything-else→`kUserModule` — there is **no `kCelDefined`
    ImportModule kind**, so a CEL-defined decl currently can't even be
    distinguished from foreign at import-install time. This is a
    concrete gap that must close before the single-module backend
    works: codegen would emit an `(import "<module>" "<id>")` for a
    function that lives in the same module. **P1.**

**Finding C — dead/contradictory separate-module scaffolding to delete:**

  - `compile.h:97-100` `struct LibraryModuleBytes` — separate-module
    output; contradicts single-module. **Delete. P1.**
  - `compile.h:109-113` `CompiledArtifact::library_modules` — ditto.
    **Delete. P1.**
  - `compile.h:92-96` doc comment block describing "bundled CEL-defined-fn
    library wasm module". **Delete. P1.**
  - `library_module.h` whole file — declares `CompileLibraryBodies`
    returning module bytes; no `.cc` implements it; the contract is the
    retired model. **Delete or rewrite as the codegen-side
    `EmitCustomFnBodies` per A.3. P1.**

These are not merely stale comments — `LibraryModuleBytes` is a public
struct on the `CompiledArtifact` returned by `Compile()`, so anything
that reads `artifact.library_modules` (the briefed `Engine::Plan` walk)
would be coding to the wrong model. Flag before any Plan-side wiring is
written against it.

**Estimated effort, §C:** scaffolding deletion ~1h; the real
`EmitCustomFnBodies` + `kCelDefined` ImportModule kind + import-skip is
the M13-D implementation itself (out of review scope to size).

---

## D. Missing unit tests (HIGH PRIORITY) — prioritized list

### D.1 expr_lower per-arm coverage — current state

`expr_lower_test.cc` (50 TESTs) covers, with direct Binaryen-IR
assertions: kConst (all 7 scalar/bytes kinds), kIdent (root + multi-var
+ prelude), kSelect (single + nested), kCallExpr general (int add,
double sub, int lt, string concat, receiver-form contains, size,
equals, nested arithmetic, dyn-passthrough, cross-numeric repick + same-
kind), ternary (`KCallConditionalLowersToBranchedIf`), `_[_]`
(map/list literal arena fast-path + host trampoline), map/list literals,
and comprehension shapes (cel.bind, map, filter, transformMap×2,
transformMapEntry, exists). This is good per-arm coverage for the
`$eval` lowerer.

**The gap is total for the custom-fn lowerer:** `LowerToCustomFn`,
`EmitCustomFnParamPrelude`, and `EmitCelCopySlotDyn` have **zero**
tests (grep confirms no `LowerToCustomFn` / `CustomFnParam` reference
in any `_test.cc`). Given it is shipped code on a public header, this
is the single largest coverage hole in M13 codegen.

### D.2 layout_pass per-scenario — current state

`layout_pass_test.cc` (47 TESTs) covers rodata packing (all kinds,
alignment, distinct offsets, multi-node), workspace slot recycling +
contiguity, the variables table (Repr, slot_offset, local_index,
unreferenced-skip, message var), select/map/list/control-flow slot
assignment, comprehension extra locals (iter no-slot, accu slot,
two-iter index, free-vars coexist), and — landed 2026-05-24 — the
`reserved_region_limit` guard (`FitsWithinDefaultReservedRegion`,
`OverflowReservedRegionFailsLoudly`, `BoundaryExactlyAtLimitFits`,
`BoundaryOneByteOverFails`).

**Gaps:** `rodata_base_override` has **no direct test** (no TEST sets
`opts.rodata_base_override` and asserts every rodata/workspace/arena
offset shifts by the override). `peak_slots` is referenced but has no
dedicated assertion that it equals the expected recycled-slot count for
a slot-reuse scenario. The **multi-function band offsetting** (the new
single-module orchestration) has no test because the orchestrator does
not exist yet — it must land with its implementation.

### D.3 Cross-reference per-component-test-coverage.md

§3.4 (LayoutPass) lists slot-allocation, rodata-packing, boundary-
invariants, variables-table — all ticked by existing tests **except**
`rodata_base_override` (not in the doc's required list, but required by
the single-module model) and explicit `peak_slots` assertion. §3.5
(ExprLower) requires WAT-match + Validate + Negative **for every new
arm** — the custom-fn arm satisfies none of these. These should be
added to the coverage doc as M13 rows when the tests land.

### D.4 Exact new test cases to add, grouped by file

**`compiler/codegen/expr_lower_customfn_test.cc`** (new file, paired
with the A.2 TU split — or `expr_lower_test.cc` if the split is
deferred):

  - `TEST(LowerCustomFnTest, NullaryBodyEmitsCopySlotFromOutParam)` —
    body `= 1`; assert function signature is `(i32) -> none` (out_slot
    only), body is `cel_copy_slot(local.get 0, <rodata>)`, no
    `arena_reset`.
  - `TEST(LowerCustomFnTest, SingleParamBodyWiresParamToLocal)` — body
    `= x` with one param; assert prelude is `local.set <var_local>
    (local.get 1)` and `wasm_local_offset` shifts the ident load.
  - `TEST(LowerCustomFnTest, MultiParamBodyWiresEachParamPositionally)`
    — two params; assert `param_for_name` maps each to its 1-based
    wasm param index and prelude order matches.
  - `TEST(LowerCustomFnTest, BodyDeclaresOutPlusNParams)` — assert wasm
    param count == `1 + params.size()` and result type is `none`.
  - `TEST(LowerCustomFnTest, ArithmeticBodyEmitsHelperCall)` — body
    `= x + y`; assert `cel_add_*` call present and final write-back is
    `cel_copy_slot(local.get 0, <call slot>)`.
  - `TEST(LowerCustomFnTest, EmittedCustomFnValidates)` — install
    imports + the custom fn, `WasmModule::Validate()` OK (the §3.5
    Validate requirement).
  - `TEST(LowerCustomFnTest, ReceiverBodyTreatsThisAsArg0)` — `this`
    param lands at wasm param 1.
  - `TEST(LowerCustomFnDeathTest, BodyReferencingUndeclaredNameChecks)`
    — body references a name not in `params`; assert the
    `ABSL_CHECK` fires (the §3.5 invariant at
    `EmitCustomFnParamPrelude:1150`).
  - `TEST(LowerCustomFnDeathTest, NonFreeVariableKindChecks)` — a
    comprehension-scope variable sneaking in; assert the
    `kFreeVariable` CHECK fires (`expr_lower.cc:1144`).
  - `TEST(LowerCustomFnDeathTest, ParamIndexZeroChecks)` — a
    `CustomFnParam{wasm_param_index=0}`; assert the `>= 1` CHECK fires
    (`expr_lower.cc:1183`).
  - `TEST(LowerCustomFnTest, StringLiteralBodyDoesNotBakeAbsolutePtr)`
    — once the §4.3 pointer-relocation construct lands; asserts the
    body constructs `ptr = base + K` rather than a baked absolute
    offset. (Add with that feature, not before.)

**`compiler/codegen/layout_pass_test.cc`** (add to existing):

  - `TEST(LayoutPassTest, RodataBaseOverrideShiftsAllOffsets)` — set
    `opts.rodata_base_override = N`; assert `rodata_base == N`,
    `workspace_base`/`arena_base` all shift by the same delta vs the
    default-base layout.
  - `TEST(LayoutPassTest, RodataBaseOverrideRespectsRegionLimit)` —
    override base + small `reserved_region_limit_bytes` such that
    rodata+workspace overruns the band → `ResourceExhausted` (the
    band-guard interaction the single-module model relies on).
  - `TEST(LayoutPassTest, PeakSlotsEqualsRecycledCount)` — an
    expression with N nested intermediates that recycle to M<N peak
    slots; assert `peak_slots == M` (not just that it's nonzero).
  - **(with the band orchestrator)** `TEST(CustomFnEmitTest,
    TwoBodiesGetDisjointBands)` — lay two CEL-defined bodies; assert
    body 2's rodata base == body 1's static-top (no overlap) and both
    fit under their region limits. (New test file alongside the
    `EmitCustomFnBodies` implementation.)

**`compiler/internal/compile_test.cc`** (add when single-module backend lands):

  - `TEST(CompileTest, CelDefinedDeclEmitsSiblingFunctionNotImport)` —
    a `FunctionLibrary` with a `kCelDefined` decl; assert the emitted
    module exports/defines `<overload_id>` as an internal function and
    does **not** emit an `(import ...)` for it (the §C import-skip
    invariant).
  - `TEST(CompileTest, CelDefinedBodyOverflowingBandFailsLoudly)` —
    e2e `ResourceExhausted` for a body too large for its band.

**`eval/engine_test.cc`** (when the typed adapter lands, B.1):

  - `TEST(EngineTypedFnTest, TypedFunctionImplDecodesAndEncodesValue)`
    — register a typed `FunctionImpl`, assert the adapter decodes arg
    slots to `Value` and encodes the result without the embedder
    touching raw memory.
  - `TEST(EngineTypedFnTest, KindMismatchResultWritesError)` — impl
    returns a `Value` whose kind disagrees with declared return type →
    `kError` in out_slot (the `m13` §4.2 step 4 contract).

### D.5 Priority

  - **P0:** none (nothing ships-breaking today because the CEL-defined
    path is not yet reachable from `Compile`).
  - **P1:** all `LowerToCustomFn` direct + death tests (D.4 group 1) —
    shipped untested code on a public header. ~3-4h.
  - **P1:** `RodataBaseOverrideShiftsAllOffsets` +
    `RodataBaseOverrideRespectsRegionLimit` — the band model's
    load-bearing layout invariant. ~1h.
  - **P2:** `PeakSlotsEqualsRecycledCount`, typed-adapter tests (gated
    on B.1 landing), compile_test single-module tests (gated on the
    backend landing).

---

## Tech-debt inventory (severity / effort)

| # | Item | Sev | Effort |
|---|---|---|---|
| 1 | `LowerToCustomFn`/`EmitCustomFnParamPrelude` untested | P1 | 3-4h |
| 2 | `library_module.h` declares unimplemented `CompileLibraryBodies` (retired model) | P1 | 1h delete / rewrite |
| 3 | `compile.h` `LibraryModuleBytes` + `CompiledArtifact::library_modules` contradict single-module | P1 | 1h |
| 4 | No `kCelDefined` ImportModule kind → can't skip self-imports | P1 | with M13-D impl |
| 5 | Move custom-fn lowering to own TU | P1 | 1.5h |
| 6 | Band-orchestration belongs in codegen/, not celfn/, over shared WasmModule& | P1 | design fix |
| 7 | Typed `FunctionImpl` adapter unbuilt; `engine.h:72` comment false | P1/P2 | 3-4h |
| 8 | `rodata_base_override` untested | P1 | 1h |
| 9 | `compiler.h:169` stale "Storage only until Slice C.3" | P2 | 10m |
| 10 | `layout_pass.h:58` `rodata_base_override` rationale describes retired multi-module model | P2 | 15m |
| 11 | `expr_lower_internal.h:51` references phantom `library_module.cc::LowerCelDefinedFn` | P2 | 10m |
| 12 | CrossNumeric repick family extractable from expr_lower.cc | P2 | 1h |
| 13 | `peak_slots` lacks a dedicated equality assertion | P2 | 30m |

P1 items should become bullets in the M13 doc's "Pre-close cleanup"
section; P2 items go to `cleanup-backlog.md` tagged
`surfaced-2026-05-24-codegen-api-layering`.

## Doc drift

  - `modules-and-ffi.md` §4 (single-module, no `__memory_base` per the
    briefing) vs `m13-custom-fns.md` §4.4 (still describes the separate
    `foo.wasm` + `Engine::InstantiateLibraryModule` + per-module
    instance). The §4.4 text and §2.1's `InstantiateLibraryModule` /
    `AddLibraryModule` API sketch describe the retired model; reconcile
    or add a `> Plan-vs-execution delta` callout pointing at
    `modules-and-ffi.md` §4 (the same way §4.5 was already superseded).
  - `library_module.h`'s file-header comment ("bundle into a single
    `foo.wasm` ... `Engine::Plan` registers the produced bytes under
    the library's `module_name()`") is the multi-module model verbatim —
    delete with item #2.
