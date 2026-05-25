# Slice 2 — control flow + 3VL (`_&&_` / `_||_` / `_?_:_` / `!_`)

Status: **shipped 2026-04-25.**

What landed: runtime helpers `cel_and` / `cel_or` / `cel_not` /
`cel_unknown_merge` / `cel_copy_slot` (`runtime/cel_3vl.h`
+ `cel_runtime.c`), 4×4 truth-table unit coverage in
`cel_3vl_test.cc`, WAT traces 30–33 + walkthroughs in
`wat-traces.md`, LayoutPass slot-allocation flip + tests, expr_lower
routes `_&&_` / `_||_` / `!_` through the standard slot-out
OverloadTable arm and special-cases `_?_:_` to a BinaryenIf
shape, build wiring (`engine.cc::BindAllRuntimeExports`,
`compile.cc::OverloadHelperArity` + unconditional `cel_copy_slot`
import, `wat_runner::kRuntimeExports`).  `Instance::Eval` grew a
CEL_ERROR decoder arm so 3VL ERROR values surface as `Value::Error`
rather than `status::InvalidArgument`; existing M4 OOB tests
rewritten accordingly.

Plan-vs-execution deltas:

  - `cel_and` / `cel_or` flipped from the v1-style "type-check
    first, short-circuit second" to langdef's "OK(false) absorbs
    everything (any X), then type-check, then truth table".  Unit
    tests assert the langdef shape directly.
  - Both-UNKNOWN merge is verified at the runtime unit level only.
    An end-to-end version conflicts with v2's pre-existing
    CelGetField UNKNOWN-mint shape (raw attribute_id in
    `payload.unk`, vs the descriptor shape `cel_unknown_merge` /
    `cel_log::FormatUnknown` expect).

Conformance delta: 490 → 509 PASS (+19).  `logic.textproto`
0/30 → 16/30 PASS.  Below the +30+cross-cutting projection because
the remaining `logic` SKIPs and cross-fixture `eval_error` rows are
gated on dyn passthrough + polymorphic equals dispatching cleanly
through ternary/and/or operands — both pre-existing slices in the
conformance unlock plan.

Lowering for the four control-flow operators, plus the runtime
helpers that implement CEL's three-valued logic (3VL) truth
tables.  After this slice ships, `logic.textproto` graduates fully
and every conformance test that uses `?:` / `&&` / `||` / `!`
becomes reachable (cross-cutting unlock).

Slot in `conformance-unlock-plan.md`: **after Slice 1.5
(`dyn` passthrough)**.  Listed there as Slice 2.

## Why this slice

Today, `expr_lower.cc::Emit`'s kCallExpr arm carves out
`_&&_` / `_||_` / `_?_:_` / `!_` as deterministic Unimplemented
(`expr_lower.cc:553`).  The carve-out is intentional — these
operators don't fit the slot-out helper-call pattern the other
M5.F kCalls use:

  - **Ternary** is short-circuit at the codegen level: only the
    selected arm runs.  Eager evaluation of both arms surfaces
    errors from the unselected branch, which langdef forbids.
  - **`&&` / `||`** have non-strict semantics: `false && error`
    is `false`, not error (langdef §"Logical operators").  Truth
    table evaluation requires both operands' kind+value, then a
    runtime helper applies the 3VL ladder.
  - **`!_`** is a unary slot-out; closest to the M5.F pattern but
    needs an explicit helper rather than a same-kind suffix.

CEL is side-effect-free, so eager evaluation of operands is
*semantically* safe — errors from an unevaluated arm of `&&` /
`||` get absorbed by the 3VL helper.  Ternary is the only
operator where short-circuit is load-bearing.

## Spec reference (langdef.md)

  - **§"Logical operators"** — non-strict `&&`/`||`, `!_`
    semantics.
  - **§"Conditional expression"** — `_?_:_` short-circuit.
  - **§"Errors and unknowns"** — 3VL precedence rules:
    `OK(false) && X = false` (regardless of X);
    `OK(true) && X = X`; `OK(true) || X = true`;
    `OK(false) || X = X`; otherwise `ERROR > UNKNOWN > OK`.
  - **§"Partial evaluation"** — UNKNOWN-set merge under
    `cel_unknown_merge` for both-UNKNOWN cases.

cel-cpp parity:
`third_party/cel-cpp/runtime/standard/logical_functions.cc`.

## What ships in this slice

### A. Runtime helpers (`runtime/cel_runtime.c`)

Slot-out helpers, each `(out_slot, operand_slots...)`:

  1. `cel_and(out_slot, a_slot, b_slot)`.  Implements the truth
     table:
       - either operand `CEL_BOOL` and `false` → `out = bool(false)`.
       - both `CEL_BOOL` → `out = bool(a.b && b.b)`.
       - `OK(true) && X` → `out = X`.
       - `ERROR && X (X != false)` → `out = ERROR(a)`.
       - `UNKNOWN && X (X != false, !ERROR)` → if X is also UNKNOWN,
         `out = cel_unknown_merge(a, b)`; else `out = a`.
       - kind-mismatch (operand isn't bool/unknown/error)
         → `out = ERROR(CEL_ERR_TYPE_MISMATCH)`.
     Order-independent w.r.t. left/right symmetry — matches
     v1's `cel_and` in `compiler/runtime/cel_runtime.c:767`.

  2. `cel_or(out_slot, a_slot, b_slot)`.  Symmetric to `cel_and`
     with `OK(true)` short-circuit:
       - either operand `OK(true)` → `out = bool(true)`.
       - both `OK(false)` → `out = bool(false)`.
       - `OK(false) || X` → `out = X`.
       - `ERROR || X (X != true)` → `out = ERROR`.
       - `UNKNOWN || X (X != true, !ERROR)` → similar merge.

  3. `cel_not(out_slot, a_slot)`.  Unary:
       - `CEL_BOOL` → `out = bool(!a.b)`.
       - `CEL_UNKNOWN` / `CEL_ERROR` → `out = a` (passthrough).
       - other kind → `out = ERROR(CEL_ERR_TYPE_MISMATCH)`.

  4. `cel_unknown_merge(out_slot, a_slot, b_slot)`.  Called by
     `cel_and` / `cel_or` for both-UNKNOWN cases.  Returns a
     fresh CEL_UNKNOWN whose `payload.unk` is the deduped
     sorted union of both operands' attribute-id sets.  Either
     operand non-UNKNOWN → `out = ERROR(TYPE_MISMATCH)`.

  5. `cel_status_either(out_slot, a_slot, b_slot)` *(internal
     helper, may inline)*.  ERROR > UNKNOWN > OK precedence;
     deterministic left-bias.  Used by arithmetic helpers
     today via `absorb_3vl_binary` — port the inline version.
     If inlining covers every call site, this dedicated helper
     can be omitted.

### B. UNKNOWN-set storage

UNKNOWN values carry a `payload.unk` that's a u32 offset to a
sorted `uint32_t[]` of attribute-ids in linear memory.  The
runtime side already mints UNKNOWNs from `cel_host_cel_get_field`
when `MatchesAnyUnknownPattern` fires (M2.E); we need
`cel_unknown_merge` to:

  1. Read both operands' `payload.unk` (zero = empty set).
  2. Allocate `len_a + len_b + 1` u32s in the arena via
     `cel_alloc` (the +1 reserves space for the length prefix
     v1 stores at `[0]`; v2's CelValue layout already pins
     length implicitly via `payload.unk` offset → the exact
     wire shape lives in `cel_data.h` / matches v1's
     `make_unknown_from_ids` — verify before coding).
  3. Sorted-union with dedup (linear merge — both inputs are
     sorted by construction).
  4. Write the resulting offset as `out_slot`'s
     `payload.unk`, kind = `CEL_UNKNOWN`.

**Probe spike findings (2026-04-25, completed before any C lands):**

  - **UnknownSet wire shape: identical to v1.**  `payload.unk` is
    a u32 offset to a 2-word descriptor `(ids_off:u32, len:u32)`;
    the ids array (sorted ascending, deduped) lives separately
    in the arena.  Confirmed by `eval/host/cel_log.cc::FormatUnknown`
    at line 138 — it parses exactly that layout when rendering
    UNKNOWN values.
  - **`cel_data.h` enum:** `CEL_UNKNOWN = 15`, `CEL_ERROR = 16`
    (NOT 13/14 as v1 used).  The ternary's "non-OK" kind probe
    must use `kind >= 15`, not `>= 14`.
  - **`make_unknown_from_ids` / `merge_sorted_ids` / `merge_unknown_sets`
    structure (v1)** translates 1:1 to v2.  Three-tier split keeps
    each helper under the function-size threshold; mirror it.
  - **`cel_alloc` reentry from a 3VL helper:** safe.  Slice 0
    found `cel_reset` rewinds at $eval prologue; UNKNOWN
    allocations happen after the reset (during eval body), so
    the arena is hot.  `cel_alloc` is a wasm export the C code
    calls directly via the linker.
  - **Decision: `cel_copy_slot`** ships as a tiny C helper
    (`__builtin_memcpy(slot_dst, slot_src, sizeof(CelValue))`).
    Reasons: keeps codegen pattern uniform with existing slot-
    out helpers, easier to debug in WAT disassembly, and
    `BinaryenMemoryCopy` would force codegen to emit raw
    `memory.copy` ops which are a separate wasm feature
    (bulk-memory) that we'd need to enable on the wasmtime
    config.  `cel_copy_slot(uint32_t dst, uint32_t src)` is
    ~5 lines of C.
  - **MintUnknown reference site:** v2 host trampoline path in
    `eval/internal/cel_host.cc` (M2.E) already mints
    UNKNOWN values via `cel_alloc` + writing the descriptor.
    `cel_unknown_merge` reuses the same allocator pattern.

### C. Codegen — `expr_lower.cc`

Replace the carve-out's `Unimplemented` branches with three
specialised emitters:

  1. **`EmitLogicalAnd` / `EmitLogicalOr`** (eager eval +
     helper call).  Pattern:
     ```
     ;; LHS into its workspace slot:
     <Emit(args[0])>             ;; → block returning slot offset
     ;; RHS into its workspace slot:
     <Emit(args[1])>             ;; ditto
     ;; Slot-out call:
     (call $cel_and (i32.const out_slot) (i32.const lhs_slot)
                    (i32.const rhs_slot))
     ;; Final block result is `i32.const out_slot`.
     ```
     LayoutPass already allocates `out_slot` for general kCalls
     (the M5.F path); the carve-out skipped that, so a parallel
     change in `LayoutPass::AggregateStorageVisitor::PostVisitCall`
     allocates `out_slot` for these four operators too.

  2. **`EmitConditional` (`_?_:_`)** (BinaryenIf with kind probe).
     Pattern:
     ```
     ;; Eval cond into cond_slot:
     <Emit(args[0])>  ;; sets cond_slot to a CelValue
     (block (result i32)
       ;; Probe cond.kind: load 4 bytes from cond_slot.
       (if (i32.ge_u (i32.load (i32.const cond_slot)) (i32.const CEL_UNKNOWN_KIND))
           ;; Non-OK: copy cond verbatim into out_slot.
           (then (call $cel_copy_slot (i32.const out_slot) (i32.const cond_slot)))
           ;; OK: read cond.payload.b, branch.
           (else
             (if (i32.load offset=8 (i32.const cond_slot))
                 (then <Emit(args[1])>; copy result_slot → out_slot)
                 (else <Emit(args[2])>; copy result_slot → out_slot))))
       (i32.const out_slot))
     ```
     `cel_copy_slot(dst, src)` ships as a tiny runtime helper
     (`__builtin_memcpy(g_memory + dst, g_memory + src, 24)`).
     Decision pinned in the spike — see "Probe spike findings"
     above.  Branches DO need explicit copies because each branch
     emits into its own slot per LayoutPass.
     **Constant for "non-OK kind"**: `15` per `cel_data.h`
     (`CEL_UNKNOWN = 15`, `CEL_ERROR = 16`).  The probe is
     `i32.ge_u kind, 15` — catches both.

  3. **`EmitLogicalNot`** (unary helper call).  Same shape as
     and/or but with one operand slot.

### D. LayoutPass + ResolvePass

  1. **LayoutPass.**  `AggregateStorageVisitor::PostVisitCall`
     today skips slot allocation for the carve-out names
     (per the M5.F note in the file).  Flip the gate: allocate
     a workspace slot for `_&&_` / `_||_` / `_?_:_` / `!_`.
     The slot stores the CelValue result of the operator.

  2. **ResolvePass.**  No change — these operators don't have
     overload IDs that flow through the table.  They're
     special-cased in expr_lower.

### E. Build wiring

  1. `cel_runtime.c` declarations — add 4 helper bodies
     (`cel_and` / `cel_or` / `cel_not` / `cel_unknown_merge`).
     Plus `cel_copy_slot` if BinaryenMemoryCopy isn't
     emit-friendly.
  2. Add header decls in `cel_runtime/cel_3vl.h` (new file) or
     append to an existing header — pick the cleaner option.
  3. `runtime/BUILD.bazel` — `-Wl,--export=cel_and`,
     `cel_or`, `cel_not`, `cel_unknown_merge`, `cel_copy_slot`.
  4. `wat_runner.cc::kRuntimeExports` — same 4 (or 5) names.
  5. `engine.cc::BindAllRuntimeExports` — same.
  6. `compile.cc::InstallOverloadImports` — these are NOT in
     `kBuiltinSeeds` (special-cased in expr_lower); they must
     be installed unconditionally as imports.  Either:
       - Add a separate `Install3VLImports` helper.
       - Or extend the dispatcher-arity table in
         `OverloadHelperArity` to recognise them by name.

### F. WAT traces

Per CLAUDE.md "WAT-first" rule — frozen ABI before codegen:

  - `wat/30_logical_and.wat` — `a && b` with three operand
    shapes: both bool, mixed bool/unknown, mixed bool/error.
  - `wat/31_logical_or.wat` — symmetric.
  - `wat/32_logical_not.wat` — unary not on bool / unknown /
    error.
  - `wat/33_conditional.wat` — `cond ? T : F` over bool cond,
    plus an unknown-cond and error-cond variant.

Update `wat-traces.md` with one section per WAT.

## Out of scope

  - **Comprehensions** (`map`, `filter`, `exists`, `all`,
    `exists_one`).  Comprehension lowering deferred to
    post-M5; these use `&&` / `||` internally but the
    surrounding scaffolding is the comprehension follow-on.
  - **Customs** (M6).  3VL helpers stay built-in; the OverloadTable
    remains scoped to spec-defined functions.
  - **Optimisation passes** (constant folding `true && X = X`,
    dead-arm elimination of ternary).  Out of scope; codegen
    always emits the full structure and the runtime applies
    the truth table.

## Tests

### Unit (`runtime/cel_3vl_test.cc` — new)

Truth-table parameterised matrix per langdef.  Cases per helper:

  - **`cel_and`**: 3×3 matrix over {OK(false), OK(true), ERROR,
    UNKNOWN} × {OK(false), OK(true), ERROR, UNKNOWN} = 16 rows.
    Plus 2 kind-mismatch (`int` operand) → ERROR.
    Plus 1 both-UNKNOWN with non-empty sets → merged set.
  - **`cel_or`**: symmetric matrix.
  - **`cel_not`**: 4 rows — bool true / false / unknown / error,
    plus kind-mismatch.
  - **`cel_unknown_merge`**: 4 rows — both empty, one empty,
    sorted-disjoint sets, sets with overlapping ids (dedup),
    plus 2 kind-mismatch (one operand non-UNKNOWN).

Use parameterised `TEST_P` for the `_and` / `_or` matrices —
17+ rows benefit from the table form; keep `_not` and
`_unknown_merge` as `TEST_F` since their cases tell distinct
stories.

### Codegen (`expr_lower_test.cc`)

Add tests:
  - `KCallLogicalAndLowersToHelper` — `true && false` lowers;
    body contains `cel_and`.
  - `KCallLogicalOrLowersToHelper` — `true || false`; body
    contains `cel_or`.
  - `KCallLogicalNotLowersToHelper` — `!true`; body contains
    `cel_not`.
  - `KCallConditionalLowersToBranchedIf` — `true ? 1 : 2`
    lowers; body contains a `BinaryenIf`, NOT a `BinaryenCall`
    to `cel_conditional` (we DON'T have a helper for this —
    it's branch-style).
  - **Flip existing `ControlFlowPendingE2ETest` tests in
    `m5_test.cc` from Unimplemented to live behaviour.**
    The existing tests `AndStillUnimplemented` / `OrStillUnimplemented`
    / `TernaryStillUnimplemented` document the carve-out;
    rewrite them to assert correct results.

### E2E (`e2e/m5_test.cc`)

#### `ControlFlowE2ETest` — happy path + ERROR propagation

  - `AndBothTrue` — `true && true` → true.
  - `AndShortCircuitFalseLeft` — `false && true` → false.
  - `AndShortCircuitFalseRight` — `true && false` → false.
  - `OrBothTrue` — `true || true` → true.
  - `OrShortCircuitTrueLeft` — `true || false` → true.
  - `OrBothFalse` — `false || false` → false.
  - `NotTrue` / `NotFalse`.
  - `TernaryThenBranch` — `true ? 10 : 20` → 10.
  - `TernaryElseBranch` — `false ? 10 : 20` → 20.
  - `TernaryNestedAsExpressionLeg` — `(1 == 1) ? "yes" : "no"`
    → "yes".  Verifies cond can be a non-literal bool.
  - **`AndOverErrorBoth`** — `(1/0 == 1) && true` evaluates to
    error (left-bias of the dominating ERROR).  Tests 3VL
    propagation through arithmetic-error-producing operands.
  - **`OrOverFalseAndError`** — `false || (1/0 == 1)`
    evaluates to error (RHS is the dominant non-OK and
    LHS is OK(false), so result is RHS).  This is the key
    non-strict semantics test.
  - **`AndOverFalseAndError`** — `false && (1/0 == 1)` →
    `false`.  Non-strict: OK(false) short-circuits past
    ERROR.  Symmetric counterpart to the test above.
  - **`OrOverTrueAndError`** — `true || (1/0 == 1)` → `true`.
    OK(true) short-circuits past ERROR.

#### `ControlFlowUnknownE2ETest` — UNKNOWN propagation + set merge

UNKNOWN values are minted by `Instance::PartialEval` when an
operand matches one of the activation's `AttributePattern`s
(M2.E shipped this — see `eval/instance.cc::PartialEval`
and `eval/attribute.h`).  The harness pattern:

  ```cpp
  auto compiler = BuildCompiler([](Compiler::Builder& b) {
    b.DeclareVariable("x", CelType::Bool());
    b.DeclareVariable("y", CelType::Bool());
  });
  Instance instance = CompilePlan(*compiler, "x && y");
  Activation a;
  a.Bind("x", Value::Bool(false));   // y left unbound
  std::vector<AttributePattern> patterns = {
      AttributePattern::Variable("y"),
  };
  auto result = instance.PartialEval(a, patterns);
  // result is OK(false) — short-circuit past unknown(y).
  ```

Tests:

  - **`AndShortCircuitFalseLeftSurvivesUnknownRight`** —
    `false && y` with `y` unknown → `false`.  Non-strict:
    OK(false) short-circuits past UNKNOWN.
  - **`AndShortCircuitFalseRightSurvivesUnknownLeft`** —
    `x && false` with `x` unknown → `false`.  Symmetric.
  - **`AndUnknownAndTrue`** — `x && true` with `x` unknown →
    UNKNOWN(set={x}).  No short-circuit available; LHS's
    unknown propagates.
  - **`AndUnknownAndUnknown`** — `x && y` with both unknown →
    UNKNOWN whose set is the deduped sorted union {x, y}.
    Tests `cel_unknown_merge` end-to-end.
  - **`OrShortCircuitTrueLeftSurvivesUnknownRight`** —
    `true || y` with `y` unknown → `true`.
  - **`OrShortCircuitTrueRightSurvivesUnknownLeft`** —
    `x || true` with `x` unknown → `true`.
  - **`OrUnknownOrFalse`** — `x || false` with `x` unknown →
    UNKNOWN(set={x}).
  - **`OrUnknownOrUnknown`** — `x || y` with both unknown →
    UNKNOWN(set={x, y}).
  - **`NotUnknown`** — `!x` with `x` unknown → UNKNOWN
    (passthrough — not flipped).
  - **`TernaryUnknownCond`** — `x ? 1 : 2` with `x` unknown →
    UNKNOWN.  Cond non-OK propagates verbatim per langdef.
  - **`UnknownDominatesOverlappingSet`** — bind two `bool`
    vars `x`, `y`, mark BOTH unknown via `AttributePattern`s,
    eval `(x && y) || (x && y)` → UNKNOWN whose set is
    `{x, y}` (deduped — both occurrences of `x` collapse to
    one).  Verifies `cel_unknown_merge`'s dedup path under
    repeated ids.

#### `ControlFlowUnknownErrorPrecedenceE2ETest` — ERROR > UNKNOWN

The 3VL ladder makes ERROR dominate UNKNOWN per langdef
§"Errors and unknowns".  Verify the precedence holds when
both kinds appear in `&&` / `||`:

  - **`AndUnknownAndError`** — `x && (1/0 == 1)` with `x`
    unknown → ERROR.  ERROR dominates UNKNOWN.
  - **`AndErrorAndUnknown`** — `(1/0 == 1) && x` with `x`
    unknown → ERROR.  Order-independent under same precedence.
  - **`OrUnknownOrError`** — `x || (1/0 == 1)` with `x`
    unknown → ERROR.
  - **`OrErrorOrUnknown`** — `(1/0 == 1) || x` with `x`
    unknown → ERROR.

Asserting an UNKNOWN result requires extracting it from the
returned `Value` — use `Value::IsUnknown()` + `AsUnknown()`
(returns the AttributeId set).  See `eval/value.h`
for the accessor signatures.  If the harness's
`Value::AsUnknown` returns the raw set, write a small helper
in the test to compare against an expected
`std::vector<AttributeId>` for the merged-set tests.

UNKNOWN-set comparison matters for the dedup / sorted-union
tests — a "result is UNKNOWN" assertion alone misses the
load-bearing invariant that `cel_unknown_merge` produces a
canonical sorted-deduped set.  Make the assertion explicit.

### Conformance

Expected after this slice:
  - `logic.textproto`: 0 → ~28 PASS (30 total minus 2 SKIPs
    for parse-shaped tests).
  - `comparisons.textproto`: ~+5 (rows that thread `&&` /
    `||` between same-kind compares).
  - `parse.textproto`: ~+2 (parse self-eval over conditional
    forms).
  - `fields.textproto`: ~+2 (`has(...) && X`).
  - **Cross-cutting**: any test in any fixture that wraps a
    currently-passing expression in a ternary or `&&` graduates.

**Total projection: 490 → ~570–620 (+80–130 PASS)**.

## Constraints (CLAUDE.md)

  - WAT-first: 30/31/32/33 before any codegen change.
  - No fixture-level GTEST_SKIPs.
  - Function-size lint: split helpers if `cel_and` / `cel_or`
    bodies grow past 60 lines.  v1's bodies were ~17 lines each
    — should fit cleanly.
  - `ABSL_CHECK(false) << "<symbol> is a stub until <milestone>"`
    on any path that's not implemented this slice (e.g.
    comprehension-internal `__not_strictly_false__` if it shares
    machinery — verify in the spike).

## Sequencing

  1. **Probe spike** — ✅ completed 2026-04-25 (findings inline
     above).  Wire shape matches v1; non-OK kind boundary is 15;
     `cel_copy_slot` ships as a runtime helper.
  2. **Runtime helpers** (~150 LoC).  `cel_and` / `cel_or` /
     `cel_not` / `cel_unknown_merge` + unit tests.
  3. **WAT traces 30–33**.  Assemble + `wat_runner` end-to-end.
  4. **LayoutPass slot allocation flip** (~10 LoC + 1 test).
  5. **expr_lower emitters** (~250 LoC).  `EmitLogicalAnd`,
     `EmitLogicalOr`, `EmitLogicalNot`, `EmitConditional`.
     Plus the carve-out swap.
  6. **Build wiring** — exports, BindAllRuntimeExports,
     wat_runner kRuntimeExports, InstallOverloadImports
     special-case (~20 LoC).
  7. **E2E + conformance**.  Flip existing pending tests; new
     ControlFlowE2ETest fixture; run full corpus, update
     `conformance/README.md` with the +80–130 delta, update
     `conformance-unlock-plan.md`.
  8. **Doc closeout**.  `m5-kcall-comprehensions.md` status
     line gets `+ M5.G shipped 2026-04-XX`.

Total: ~450 LoC + 30+ tests + 4 WAT traces.  Estimate: 1–2 days.

## Risks

  1. **UnknownSet wire shape.**  v1 stored a u32-prefixed
     length array; v2's `cel_data.h` may use a different
     layout.  Mitigation: probe spike resolves before any C
     is written.

  2. **`cel_alloc` reentry from a slot-out helper.**  Slice 0
     learned that activation-time `cel_alloc` is broken because
     `cel_reset` wipes the arena.  But `cel_unknown_merge` is
     called from within `$eval`, AFTER `cel_reset` has run.
     The arena is hot at that point.  No issue — but explicitly
     verify by running an end-to-end test.

  3. **Branch-style ternary type agreement.**  BinaryenIf
     requires both arms to have the same Binaryen type.  Both
     branches return `i32` (the slot offset), so this is
     trivially satisfied.  If a future ternary mixes scalar
     and aggregate result types, both arms still return slot
     offsets — the slot's CelValue carries kind information.

  4. **Lint backlog.**  `lint-backlog.md` already tracks 7
     `readability-function-size` exceedances.  Don't add a
     new one — split helpers proactively.

## Future work this enables

  - **Macros / comprehensions follow-on.**  `exists` /
    `all` / `exists_one` desugar to comprehensions whose
    accumulator step uses `&&` / `||`.  Once 3VL is in,
    that desugaring lights up.
  - **Conformance `eval_error` / `any_eval_errors` matchers.**
    The harness today SKIPs these; with 3VL surfacing
    ERROR through `&&` / `||`, the matchers see real
    ERROR CelValues.
  - **`not_strictly_false` (`__not_strictly_false__`)** —
    used internally by comprehensions' loop_condition.
    Trivially derivable from `cel_not` + `cel_or` once the
    helpers exist; lands at the comprehension follow-on.
