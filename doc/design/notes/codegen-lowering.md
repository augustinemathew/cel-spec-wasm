# codegen-lowering — design notes (undefined)

Component: `compiler/codegen/{expr_lower.{h,cc}, expr_lower_internal.h,
expr_lower_comprehension.cc, overload_table.{h,cc}, module.{h,cc}}` + their
`*_test.cc`.  Paired docs read: `rewrite/m5-kcall-comprehensions.md`,
`rewrite/cross-numeric-ordering-plan.md`, `rewrite/slice2-control-flow-plan.md`
(all three exist; all marked "shipped 2026-04-25").

## 1. Verified architecture

### Responsibilities and inputs/outputs
- `LowerToEvalFunction(TypedAst, StaticLayout, func_name, WasmModule&, OverloadTable&, LoweringOptions)`
  adds a nullary `() -> i32` `$eval` function to a caller-prepared Binaryen
  module; the returned i32 is the linear-memory offset of the root expression's
  CelValue (expr_lower.h:210-213, expr_lower.cc:1210-1266).  Caller must have
  installed memory + every function import first; codegen only emits the
  function body (expr_lower.h:17-21; tests redo the install in
  `PrepareHostModule`, expr_lower_test.cc:208-230).
- `LowerToCustomFn` emits a `(out_slot, arg0..argN) -> ()` wasm function for a
  CEL-defined custom-fn body: param prelude copies wasm params into the
  ResolvePass-assigned locals (offset by `wasm_local_offset = 1 + params.size()`,
  expr_lower.cc:1320-1329), result written via `cel_copy_slot(local.get 0, <root>)`
  (expr_lower.cc:1346-1347).  Free variables not in `params` CHECK
  (expr_lower.cc:1295-1303).
- `Emit(EmitCtx&, Expr&)` is the single dispatcher (expr_lower.cc:1147-1208);
  every arm returns an i32-valued Binaryen expression whose runtime value is
  the node's CelValue offset (rodata for kConst, `local.get` for kIdent,
  `(block (call …) (i32.const out_slot))` for everything else).
  `EmitCtx` (expr_lower_internal.h:36-53) threads module, ast, layout,
  field_refs, OverloadTable, and `wasm_local_offset` through the walk.
- `LoweredFunction.field_refs`: index 0 is a reserved sentinel pushed before the
  walk (expr_lower.cc:1220-1222); rows appended at each kSelect
  (expr_lower.cc:255-260, field_number from `ann.field_number`) and each
  kStructExpr field (expr_lower.cc:642-647, field_number=0 → host resolves the
  FieldDescriptor by name).  Consumed by `BuildCelAbi` (expr_lower.h:166-175).

### kCall dispatch ladder (expr_lower.cc:1162-1193)
1. `dyn(x)` 1-arg, no target → identity: emit the argument
   (expr_lower.cc:1170-1173; pinned by expr_lower_test.cc:402-415).
2. `_[_]` → `EmitKIndexCall` (expr_lower.cc:690-747): origin comes from the
   OPERAND annotation; `Repr::kOptional` routes to
   `cel_select_optional_field_at_vv`; `kMap`/`kList` pick the call target from
   `map_origin`/`list_origin` (kArena → `*_arena` fast path, kHost →
   `cel_host_*` trampoline, kDynamic → runtime dispatcher;
   expr_lower.cc:375-403).  Any other repr → `ABSL_CHECK(false)`
   (expr_lower.cc:724-731).
3. `_?_:_` → `EmitConditional` (expr_lower.cc:1049-1097): nested `BinaryenIf`.
   Outer probe `i32.eq (i32.load offset=0 cond_slot) 1` (kind == CEL_BOOL);
   non-bool cond is copied verbatim to out_slot; inner probe
   `i32.ne (i32.load offset=8 cond_slot) 0` selects the arm.  The arm's
   eval-expression is nested INSIDE the if-arm, so only the chosen arm executes
   (expr_lower.cc:1085-1091).  `BuildConditionalArm` deliberately copies from
   the arm's eval value, not `storage.payload`, because kLocal payload is a
   local index, not a slot offset (expr_lower.cc:1008-1022).
4. Everything else → `EmitGeneralCall` (expr_lower.cc:1099-1136): lookup
   `ann.overload_id` in OverloadTable (Unimplemented if unstamped or
   unregistered, expr_lower.cc:932-950), flatten receiver `target` to args[0]
   (expr_lower.cc:956-972), emit one slot-out call.  `_&&_`/`_||_`/`!_` take
   this arm — eager evaluation of both operands; 3VL non-strict semantics live
   inside `cel_and`/`cel_or`/`cel_not` (expr_lower.cc:1180-1184; seeds
   overload_table.cc:250-252).

### Cross-numeric re-pick (known-issue #1 — verified, with nuance)
`MaybeRepickCrossNumericOverload` (expr_lower.cc:917-925), called from
`EmitGeneralCall` (expr_lower.cc:1108-1118), overrides the
ResolvePass-stamped `overload_id` when the function is one of
`_<_ _<=_ _>_ _>=_` (expr_lower.cc:796-798) and the two operand `Repr`s span
a numeric cross-pair (expr_lower.cc:903-911).  The replacement ids come from
four hand-written switch tables (expr_lower.cc:820-896) that duplicate 24
overload-id strings also present in `kBuiltinSeeds`
(overload_table.cc:156-203) — including cel-cpp's `_uint`-not-`_uint64` typo
`greater_equals_uint_double` mirrored at BOTH expr_lower.cc:877-890 and
overload_table.cc:202.
- **Why codegen-side:** the probe recorded in
  `cross-numeric-ordering-plan.md:149-180` found cel-cpp's reference map lists
  exactly ONE candidate per call (the same-kind overload of the non-dyn
  operand); a resolve-pass pick-from-list (the doc's original Option A,
  lines 268-309) was non-viable — there is no list.  The doc's "Verdict /
  plan-vs-execution delta" callouts (lines 172-180, 224-232, 407-415) record
  the pivot to codegen-time Option B as the as-shipped design.  The code
  comment (expr_lower.cc:767-789) restates this.  So "re-derives semantic
  decisions codegen-side" is TRUE; "against design intent" is FALSE against
  the closed-out doc — it is the documented, probe-justified design.
- **Residual debt that is real:** the 24 id strings are a duplicated table
  with no tripwire tying them to `kBuiltinSeeds`.  A drifted string fails
  LOUDLY (`ResolveCallHelper` → Unimplemented naming the id,
  expr_lower.cc:942-948), not silently; unit tests cover only 2 of 24 cells
  (expr_lower_test.cc:425-471: lt int↔uint, ge double↔uint); the
  72-row e2e matrix in `e2e/m5_test.cc::CrossNumericOrderingE2ETest`
  (m5_test.cc:988-1177) carries the behavioral load.

### CelValue ABI constants (known-issue #2 — verified)
Codegen hand-copies the runtime's CelValue wire layout with NO compile-time
tie to `runtime/cel_data.h`:
- `expr_lower.cc:1080-1091` — `EmitConditional`: kind at offset 0, bool
  payload at offset 8, `CEL_BOOL = 1` as bare literals (comment says "layout
  pinned by cel_data.h" but the header is not included).
- `expr_lower.cc:562-573` — `WrapperKindFromFqn`: CelKind literals 1..6.
- `expr_lower_comprehension.cc:613-632` — `EmitWriteIntCelValueToSlot`:
  `CEL_INT = 2` literal (line 620), offsets 0/4/8.
- `expr_lower_comprehension.cc:167-172` — `LoadAccuBoolPayload`: offset 8.
- `expr_lower_comprehension.cc:456` and `:958` — 24-byte CelValue stride in
  the list-prologue end-pointer math and the loop-tail pointer advance;
  acknowledged in-comment ("hardcoded here; if the runtime layout shifts …
  must change too", expr_lower_comprehension.cc:423-424).
The authoritative constants exist: `runtime/cel_data.h:33-34`
(CEL_BOOL=1/CEL_INT=2), `:142-183` (`CelValue` struct + `_Static_assert
sizeof == 24`), `:195-200` (`kCelListEntryStride = 24`).  The dep is feasible —
`compiler/codegen/static_memory_builder` already deps `//runtime:cel_runtime`
and includes `runtime/cel_runtime.h` (BUILD.bazel:35-45,
static_memory_builder.h:44) — but the `expr_lower` target's deps omit it
(BUILD.bazel:193-216).  A CelValue layout change compiles green through all
of `//compiler/codegen` and only fails at e2e.

### Comprehension lowering (expr_lower_comprehension.cc)
- One TU per the split rationale (expr_lower_internal.h:8-14); entry point
  `LowerComprehension` (expr_lower_comprehension.cc:1001-1022) emits
  prologue + `(block exit (loop continue …))` + result, per the file-header
  emission map (lines 10-88).
- `CompContext` binds iter/iter2/accu `LaidOutVariable`s by the per-comp local
  indices ResolvePass stamped — NOT by name, because nested comprehensions
  reuse `@result` (expr_lower_comprehension.cc:242-271).  `aux0_local` is the
  list end-pointer or map iter-handle; v2 index counter = `aux0+1`; list
  source-addr local = `aux0+2` (lines 221-240).  Exit/continue labels are
  suffixed by expr_id so nested same-name comps pass Binaryen's label
  validator (lines 215-218, 331-332).
- Loop-cond peephole admits a CLOSED set of four shapes (kConst true/false,
  `@not_strictly_false(@result)`, `@not_strictly_false(!@result)`); anything
  else → UnimplementedError.  Both accu shapes gate the `br_if` on
  `kind == CEL_BOOL` as well as the payload word, so an ERROR / UNKNOWN
  accumulator keeps iterating and a later element can still absorb it
  (`BoolAccuExit` / `AccuKindIsBool`).
- Loop-step classified once into 7 kinds by AST shape (macro names are erased
  by the expander; recovery is structural — lines 494-549, 709-740):
  kListAppend(If) / kMapInsert(If) / kMapMerge(If) / kGeneric.
- Pre-sizing invariant: collection-accu shapes never grow at runtime; codegen
  emits `cel_list_create/cel_map_create` with capacity =
  `iter_range.count × per_iter` loaded at runtime; the runtime append/insert
  traps if exceeded — the pair is the regression tripwire
  (lines 72-80, 542-567).
- Source resolution: kWorkspaceSlot vs kLocal both flow through
  `SourceAddrExpr` (lines 296-325, 388-393); list sources are normalised
  through `cel_list_arena_view` (arena passthrough / host snapshot) so the
  inline 24-byte-stride pointer walk is origin-uniform (lines 578-591); map
  sources rely on `cel_map_iter_init`/`cel_map_count` kind-dispatching
  internally (lines 465-492).
- `transformMapEntry` entry expressions split two ways: a map LITERAL is
  decomposed at compile time into N direct `cel_map_insert_at` calls (exact
  pre-size, no temp map); anything else — a computed entry, or a multi-key
  literal under the 4-arg form — is evaluated to a temp map and folded in by
  `cel_map_merge_at` / `cel_map_merge_at_if_bool`, the one insert path that
  GROWS the accumulator (a computed entry has no compile-time key count).
  All three shapes used to be `ABSL_CHECK(false)` aborts.

### OverloadTable
- `kBuiltinSeeds`: 271 rows (overload_table.cc:96-733;
  count pinned by overload_table_test.cc:91-100) mapping cel-cpp overload-id
  strings → `(ImportModule, helper_name)`.  Aggregate ops seed the kDynamic
  dispatcher names (Option B; overload_table.cc:85-95).  `_[_]`, `_?_:_`,
  `not_strictly_false` are deliberately absent (special-cased in expr_lower)
  and live in `kExplicitlyUnimplementedIds`, which is now just 6 ids
  (overload_table.cc:735-759).  The coverage tripwire
  (overload_table_test.cc:573-587) partitions every
  `cel::StandardOverloadIds::k*` between the two sets and rejects overlap.
- Builder: built-in arity comes from `abi::FindBuiltinHelper` (the ABI
  catalogue is the single source of truth across codegen / engine binding /
  linker exports); a seed missing from the catalogue CHECK-fails at Build()
  (overload_table.cc:794-821).  `RegisterCustom` copies caller strings into
  deque-stable storage, rejects builtin shadowing and duplicates with
  AlreadyExists, CHECKs kCelRuntime registrations and the module_name
  empty/non-empty rules per kind (overload_table.cc:823-870).  Interned ids
  are 1-based, 0 = unresolved (overload_table.h:187-191).
- `ImportModuleName(def)` maps `wasm_import_module_type` to the module
  string — `kCel`/`kCelHost`/`kCelFn` are the only members at HEAD (the
  `kUser`/`kUserModule` per-library routing was removed 2026-08-04, m39,
  with the `@native` stub; `OverloadDef` carries no import-module-name
  string — see `overload_table.h:29-67`).  Two overloads may share a
  helper name; collision rule is on overload-id only.

### WasmModule (Binaryen wrapper)
- RAII over `BinaryenModuleRef`; default feature set = ReferenceTypes |
  Multivalue | BulkMemory | SignExt | MutableGlobals | GC | Atomics
  (module.cc:55-60), mirrored as a tripwire constant in
  module_test.cc:305-310.
- `Adopt(existing)` (static link mode: pre-built runtime wasm as base module)
  takes the feature UNION — narrowing trips Binaryen's feature-dependency
  asserts (module.cc:68-83; pinned by module_test.cc:354-369).
- `AddMemoryImport`: must call `BinaryenSetMemory` BEFORE
  `BinaryenAddMemoryImport`; reversed order silently emits a non-imported
  memory (confirmed against binaryen 129; module.cc:196-218).  Shared
  memories require max_pages (module.cc:171-174).
- `AddActiveDataSegment(offset, bytes, memory_name="memory")` appends to an
  existing memory; the adopted runtime's memory is named "0", hence the
  parameter (module.cc:268-283; module_test.cc:420-445).  This pairs with the
  codegen-wide rule that `CodegenLoad/CodegenStore` always pass
  `memoryName=nullptr` so loads bind to whatever memory exists — hard-coding
  "memory" works in dynamic mode and silently breaks static mode
  (expr_lower_internal.h:66-81, expr_lower.cc:76-100).
- `Optimize(level 0..3)`: level 0 is a guaranteed byte-identical no-op
  (golden-test contract, module.cc:300-304, pinned module_test.cc:257-270);
  ShrinkLevel held at 0 (perf > size for a JIT input, module.cc:305-311);
  `BinaryenSetOptimizeLevel` is process-global state — concurrent Optimize
  calls would race (module.h:154-164).

### `$eval` body shape (expr_lower.cc:1227-1258)
`(block i32: <local.set per free variable> (call $arena_reset) <root>)`.
Comprehension-scope variables are skipped in the prelude (set by loop
prologues; also keeps free-variable slot offsets stable for the host marshal,
expr_lower.cc:162-171).  Locals count = `layout.total_wasm_locals`, falling
back to `variables.size()` (expr_lower.cc:1251-1254).
`LoweringOptions::mem_size_bytes` is vestigial and unread
(expr_lower.h:177-182, expr_lower.cc:1213).

## 2. Doc-vs-code discrepancies

1. **P1 — expr_lower.h header claims M1 kConst-only.**  expr_lower.h:4-15
   ("M1 handles only the kConst arm … Non-kConst expression kinds return
   UnimplementedError") and :197-209 ("Fails with UnimplementedError for any
   expression kind outside the M1 subset (kConst only)") vs expr_lower.cc:1147-1208,
   which lowers all 8 kinds.  The public header's contract comment is several
   milestones stale on the component's own API.
2. **P1 — overload_table.h:238-250 reasons-list is stale.**  Claims
   `kExplicitlyUnimplementedIds` contains "(2) deferred … cross-type numeric
   ladder, timestamp/duration arithmetic, regex matches; (3) … type
   conversions, timestamp accessors" — every one of those families is now in
   `kBuiltinSeeds` (overload_table.cc:142-203, 253-357, 391-414, 423-477); the
   actual set is 6 ids: conditional, not_strictly_false ×2, index_list/map,
   to_dyn (overload_table.cc:750-759).  Also P2: overload_table.h:10-12 "M1
   ships the shape with kBuiltinSeeds empty — M3 fills the seeds" (now 271).
3. **P1 — `&&`/`||` lowering mechanism contradicts m5-kcall doc.**
   m5-kcall-comprehensions.md:420-454 (§2.4): "These cannot be lowered as
   ordinary slot-out calls … Explicit branching is the only correct lowering";
   §1.1 line 180-181: "`false && (1/0 == 0)` → false (right operand never
   evaluated)".  Shipped code routes `_&&_`/`_||_`/`!_` through the eager
   slot-out arm (expr_lower.cc:1180-1193; seeds overload_table.cc:250-252);
   slice2-control-flow-plan.md:65-68 records the (correct) rationale — CEL is
   side-effect-free, so eager eval + 3VL absorption in `cel_and`/`cel_or` is
   spec-equivalent.  Result values match langdef; the m5 doc's mechanism claim
   and its "only correct lowering" assertion are wrong as-shipped.  (Both
   operands ARE always evaluated — also a perf fact benches should state.)
4. **P2 — ternary kind probe differs from the slice2 plan.**
   slice2-control-flow-plan.md:205-229 plans `if (kind >= 15)` (non-OK =
   UNKNOWN/ERROR) → copy cond verbatim, else branch on payload.  Shipped
   EmitConditional probes `kind == CEL_BOOL(1)` (expr_lower.cc:1089-1091):
   bool → branch; ANY non-bool (not just error/unknown) → copied verbatim to
   out_slot.  Reachable difference only for a dyn-typed non-bool cond
   (checker rejects static non-bool conds): shipped code returns the cond
   value itself where cel-cpp presumably errors.  See validation item 1.
5. **FIXED — loop-cond peephole vs comprehension 3VL.**  `BuildLoopCondExit`
   / `LoadAccuBoolPayload` used to read the accu's payload bits at offset 8
   without checking `accu.kind`, so an ERROR accu (its error code in the same
   word) tripped the `exists` br_if-exit and surfaced the error even when a
   later element would have absorbed it (`[0, 2].exists(x, 2/x == 1)` → spec
   true, we returned the error).  `BoolAccuExit` now ANDs the payload test
   with `kind == CEL_BOOL` for both `exists` and `all`; the emitted shape is
   locked by `ExprLowerComprehensionTest.{Exists,All}LoopCondGatesOnAccuKind`
   and the behaviour by e2e `ComprehensionAccuAbsorptionE2ETest`.
6. **P2 — module.h:10-16 module narrative stale.**  "imports `cel.arena_reset`
   / `cel.arena_alloc`" (codegen emits no `arena_alloc` call;
   `EmitArenaResetCall` is zero-arg per the WASI migration,
   expr_lower.cc:136-144) and "M1's codegen only emits the expr side" (static
   link mode now adopts the runtime module, module.h:42-50).
7. **P2 — cross-numeric-ordering-plan.md body sections still present the
   rejected Option A as "Recommendation"** (lines 290-309) with the correction
   carried only in repeated delta callouts; a section-level rewrite was never
   done.  Internal line references into `overload_table.cc` (e.g. "142-189",
   "283-290") have drifted with the file's growth.

## 3. Validation items

1. **Ternary with a dyn non-bool cond.**  Q: what does `dyn(1) ? 2 : 3`
   produce — cel-cpp "no matching overload" error, or our copied-cond int 1?
   Settle: add an oracle case `EvalWithCelCpp("dyn(1) ? 2 : 3", "")` in
   `testdata/cel_cpp_oracle_test.cc` AND an e2e `TryEval("dyn(1) ? 2 : 3")`;
   compare.  (Shipped probe at expr_lower.cc:1089-1091 copies the cond
   verbatim when kind != CEL_BOOL.)
2. **Cross-numeric id-table sync (24 cells).**  Q: does every string returned
   by `CrossNumeric{Lt,Le,Gt,Ge}Id` resolve in `OverloadTable`?  Settle:
   parameterized codegen test compiling `dyn(K1) <op> K2` for all 4 ops × 6
   cross-pairs and asserting `BodyContainsCallTo(body, "cel_numeric_<op>_at_vv")`
   (today only 2/24 in expr_lower_test.cc:425-471); or run
   `bazel test //e2e:m5_test --test_filter='*CrossNumeric*'` and confirm the
   72-row matrix is green.
3. **Magic-number drift detection floor.**  Q: if `CelValue` grew past 24
   bytes (or CEL_INT renumbered), would anything under `//compiler/codegen`
   fail before e2e?  Settle: temporarily change `kCelListEntryStride`/struct
   in runtime/cel_data.h and run `bazel test //compiler/codegen/...` —
   expected all-green confirms the gap; the fix is depping
   `//runtime:cel_runtime` from `:expr_lower` and replacing literals
   (`kCelListEntryStride`, `CEL_BOOL`, `CEL_INT`) or adding static_asserts.
4. **Re-pick under disable_check / pre-stamped cross ids.**  Q: can
   `EmitGeneralCall` see a cross-numeric id with same-kind Reprs (re-pick
   returns empty → cross id kept; helper is polymorphic, so likely benign) or
   a parse-only `1 < 2u`?  Settle: `bazel run //tools/cel -- '1 < 2u'` with
   the parse-only flag, and a probe printing `effective_ann.overload_id` for
   a hand-constructed annotation.
5. **Tripwire id mapping for `kTimestampToDate`.**  Q: which seed satisfies
   `S::kTimestampToDate` / `WithTz` (overload_table_test.cc:521-522) — the
   `timestamp_to_day_of_month_1_based*` seeds?  Settle:
   `bazel test //compiler/codegen:overload_table_test` plus a one-line print
   of the cel-cpp constant's value.

## 4. Test coverage observations

Pinned well:
- Per-arm IR shape: kConst across all 7 scalar literal kinds
  (expr_lower_test.cc:234-289), ident prelude/local wiring (:498-581), select +
  nested select field_ref rows and operand nesting (:616-679), map/list
  literal block shapes with exact child counts (:746-780, :851-884), origin
  dispatch positive AND negative (arena must NOT emit dispatcher/host;
  :782-815, :886-917), optional-kernel routing for select/index/optindex with
  anti-assertions on the message-field path (:1203-1351), optional-literal
  mixed-entry kernels (:1362-1444), receiver-form flattening operand count
  (:1106-1132), dyn passthrough anti-helper assertions (:402-415).
- Comprehension classifier→helper chain: one test per LoopStepShape kind +
  cel.bind generic-path lock (:938-1049).
- OverloadTable: seed count, custom interning order, dangling-string-view
  safety, move-survival, UsedImports filtering, and the standard-id
  coverage tripwire (overload_table_test.cc throughout; the kUserModule
  round-trip/death cases were deleted with the routing in m39).
- WasmModule: Adopt union semantics, Optimize level-0 byte-identity and
  level-2 must-shrink, memory-import preconditions, named-"0" segment landing
  (module_test.cc).

Gaps:
- `expr_lower_comprehension.cc` has no paired `_test.cc` (repo rule: every
  source file gets one); its coverage rides expr_lower_test + e2e.  The
  loop-cond-peephole Unimplemented path, `ResolveCompSourceAddress`
  kStaticRodata/kNone rejection, and the `ABSL_CHECK` arms in
  EmitMapMerge/EmitMapMergeIf have no unit/death tests.
- `LowerToCustomFn` is untested in this component's test file (covered, if at
  all, by library_module tests elsewhere).
- Negative paths of `ResolveCallHelper` (empty overload_id, unregistered id;
  expr_lower.cc:936-948) and `Emit`'s missing-annotation error
  (expr_lower.cc:1150-1153) are not unit-tested.
- Cross-numeric re-pick: 2/24 cells unit-tested (behavioral load is on e2e).
- Stale skip: expr_lower_test.cc:583-601
  (`MultipleVariablesGetSeparateLocalsAndPrelude`) is GTEST_SKIP'd "until M3
  kCall lands" — the blocker shipped long ago; per the repo skip discipline
  this is a review finding.
- `ImportModuleName(kCelFn)` and UsedImports-with-builtin-ids unasserted.

## 5. Design decisions worth preserving

1. **Uniform slot-out ABI**: every runtime helper is
   `(i32 out_slot, i32 arg_slot…) -> void`; `Emit` always value-types as the
   i32 CelValue offset.  This is what makes the general kCall arm a single
   lookup-and-emit (expr_lower.cc:749-766).
2. **The special-case set is closed and motivated**: `_[_]` (origin-aware
   3-path dispatch), `_?_:_` (BinaryenIf — the ONLY operator where laziness is
   load-bearing), `dyn` (identity), comprehension internals.  `&&`/`||`/`!`
   are deliberately NOT special-cased: eager slot-out + runtime 3VL absorption
   is spec-equivalent because CEL is side-effect-free
   (slice2-control-flow-plan.md:65-68) — and means both operands always
   evaluate (perf fact).
3. **Cross-numeric re-pick lives in codegen because cel-cpp's reference map
   has exactly one candidate** — there is nothing for ResolvePass to choose
   from; the id must be synthesized from operand Reprs, which only exist
   post-annotation (cross-numeric-ordering-plan.md:149-180).  Any future
   "move it to resolve" proposal must re-confirm that probe finding first.
4. **CodegenLoad/CodegenStore with memoryName=nullptr is mandatory** — static
   mode's adopted memory is named "0", dynamic mode's "memory"; a hard-coded
   name silently breaks one mode (expr_lower_internal.h:66-81).
5. **Pre-size + runtime trap pair**: collection accus are sized
   `count × per_iter` at the prologue and the runtime traps on overflow —
   the trap is the tripwire for a codegen sizing regression
   (expr_lower_comprehension.cc:72-80).
6. **Loop-cond peephole is a closed 4-shape set**; unrecognised shapes fail
   compile loudly rather than emitting a wrong loop (expr_lower_comprehension.cc:634-663).
7. **Comp variable binding by stamped index, never by name** (`@result` is
   reused at every nesting depth); labels are expr_id-suffixed
   (expr_lower_comprehension.cc:242-246, 215-218).
8. **`BuildConditionalArm` consumes the arm's eval expression, not
   `storage.payload`** — kLocal payload is a wasm local index, not a slot
   offset; hard-coding it was a real trap (expr_lower.cc:1008-1015).
9. **Adopt = feature UNION**; Optimize level-0 = byte-identical no-op
   (golden-test contract); ShrinkLevel pinned 0; Binaryen opt knobs are
   process-global (module.cc:68-83, 294-313; module.h:154-164).
10. **`BinaryenSetMemory` before `BinaryenAddMemoryImport`** — reversed order
    silently produces a non-imported memory (module.cc:196-199).
11. **Arity from `abi/runtime_catalogue` only**; a seed absent from the
    catalogue is a Build()-time CHECK, not a silent import skip
    (overload_table.cc:800-815).
12. **Mirror cel-cpp overload-id strings verbatim, typos included**
    (`greater_equals_uint_double`, `timestamp_to_seconds_tz`) — the tripwire
    does byte-equal lookups; "fixing" the string regresses silently
    (overload_table.cc:151-155, 384-388).
13. **Seeds point at kDynamic dispatchers for aggregate ops** (one runtime
    branch per call) to keep the table a flat id→name map
    (overload_table.cc:85-95).
14. **field_refs[0] sentinel + field_number=0 = resolve-by-name** host
    fallback (expr_lower.cc:637-647, 1217-1222).
