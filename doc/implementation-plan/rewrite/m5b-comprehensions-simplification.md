# M5.B comprehensions — simplification analysis

Status: **analysis-only report, drafted 2026-05-17.**  Produced by a
focused subagent pass against HEAD `c8a4c56` (post-Slice-J close).
Read alongside `m5-comprehensions-followon.md` (the as-shipped
plan) — this doc owns the *what could be tighter*, the followon doc
owns the *what was built*.

This report is decision-grade input for a future simplification
session.  Nothing in it has been executed; treat each
recommendation as a proposal the maintainer will accept, reject, or
defer.  When a proposal lands, mirror the change into both this doc
(strike-through the proposal, add an "as-shipped" callout) and the
followon doc's §10 future-work bullet that motivated it.

The analysis brief that produced this report lives at
`/tmp/comprehensions_simplify_prompt.md` (not committed — it's a
session artifact, kept locally for re-runs).

### What has landed since the original report was written

  - **TU split (commit `fb55b7f`, 2026-05-17).**  The
    comprehension surface moved out of `expr_lower.cc` into
    `expr_lower_comprehension.cc`; shared primitives
    (`EmitCtx`, the 6 wasm-emission helpers, the `Emit`
    dispatcher forward) live in a new internal header
    `expr_lower_internal.h`.  Mechanical, zero behavioural
    change.  Net file sizes: `expr_lower.cc` 2049 → 1132,
    `expr_lower_comprehension.cc` 0 → 980, `expr_lower_internal.h`
    0 → 103.  The report's per-area analyses still cite line
    numbers from the pre-split file; re-grep when working from
    a recommendation.  The split was a precondition for
    landing several proposals in this report (Area 2, 3, 5)
    safely — touching ~950 LoC inside a ~2050 LoC file is a
    bigger blast radius than touching the same LoC inside its
    own 980 LoC TU.
  - **Testing-instrumentation gap surfaced as a new Area 9.5
    (2026-05-17).**  Question from the maintainer: "how do
    compilers generally test this sort of code?  Are we
    missing unit tests?"  The honest answer is yes — the
    comprehension surface has ResolvePass + LayoutPass unit
    tests, runtime helper unit tests for most of cel_list /
    cel_map, and m5b_test e2e coverage, but **zero
    codegen-IR-inspection unit tests** of the kind the
    non-comp expr_lower arms already have (see
    `ScalarListLiteralEmitsCreateAndAppends` in
    `expr_lower_test.cc:792`).  Area 9.5 enumerates the gap,
    ranks 5 concrete items, and recommends that items 1–4
    land BEFORE the simplification proposals in Areas 1–5
    are executed — otherwise a regression bisects via
    conformance run instead of via a focused unit-test
    failure.

---

# Comprehensions Simplification Report — M5.B post-mortem

Reviewer: analysis-only pass against HEAD (`c8a4c56`), baseline `7e161e6`.
Working tree: `/Users/augustine/cel-spec-wasm/`.

## Executive summary — five highest-ROI simplifications

1. **Collapse the four `Try…Insert(Entries)` matchers + four `Emit…Step`
   emitters into one classifier that returns a tagged
   `LoopStepShape { kind, key, value, entries, pred }`.** The current
   six-arm cascade in `EmitCompLoopStep`
   (`compiler_v2/codegen/expr_lower.cc:1711-1759`) re-walks the
   `loop_step` AST six times before falling through to the generic path;
   each `EmitConditional…` variant is a wrapper that prepends a
   `pred` arg and routes to the `_if_bool` helper. Single-pass
   classification + a uniform 3-line emitter table removes ~120 LoC of
   near-duplicate matcher/emitter code and makes a 7th shape (if any
   later macro adds one) a one-row table extension.

2. **Drop `kComprehensionIndex` from `ResolvedVariableKind` and merge
   `IsPresizableCollectionAccu`/`PerIterEntryCount`/`EmitLoadSourceCount`/
   `EmitPresizeAccu` into a single `EmitAccuInit(comp, ann)` helper.**
   `kComprehensionIndex` is declared in `resolve_pass.h:38-43` and
   `layout_pass.h:34-36` but never assigned by any pass — the list
   two-iter index uses `kComprehensionAccu` instead. The pre-sizing
   logic is a single 30-line decision tree split across five
   forward-declared functions for no benefit other than line-count
   gates. Removing one dead enum value + inlining yields a single
   ~40-line helper (~ −60 net LoC) and eliminates the
   forward-declaration of `TryMatchAccuMapInsertEntries` /
   `TryMatchAccuConditionalMapInsertEntries` 250 lines before their
   definitions.

3. **Remove `IsShapeC` / `LowerShapeC` by teaching `EmitCompLoopStep`
   to handle `kLocal`-storage `loop_step`.** This is the followon's
   own §10 future-work bullet, framed as a real cleanup not a perf
   bet. The escape hatch exists because the generic path's final
   `ABSL_CHECK(step_ann->storage.kind == kWorkspaceSlot)`
   (`expr_lower.cc:1753-1755`) rejects cel.bind's `loop_step =
   kIdent(accu_var)`. Lifting that one CHECK and routing kLocal
   storage through a no-op copy (the value is already in the right
   local) deletes a ~40-line bypass plus its detector and
   simplifies the comprehension dispatch tree.

4. **Collapse `cel_list_append_at_if_bool` and `cel_map_insert_at_if_bool`
   into a CodeGen-side optimisation rather than runtime helpers.** Both
   helpers are 14-line 3VL ladders that delegate to their unconditional
   sibling. The codegen-side conditional shape is already isolated
   (single emitter each); a BinaryenIf-with-3VL-absorb prologue at
   the call site costs ~12 wasm ops per loop iter (vs ~5 today) but
   removes 2 exported runtime helpers, 2 wat_runner entries, 2 BUILD
   `--export=` lines, and 2 `compile.cc` imports. **However, I do
   NOT recommend this** — see "What NOT to change" §1.  The real
   reason is layering + test surface, not "cross-platform allocator
   behaviour" as the original report claimed (that framing was
   sloppy and corrected here per maintainer pushback on
   2026-05-17): keeping the 3VL ladder in C means it's
   unit-testable in isolation via `cel_runtime_wasm_test.cc` and
   the planned Item 3 of §9.5; moving to codegen-emitted wasm
   splits the test surface and makes the next bug like the Slice
   G `macros2/transformMap/error_filter` regression harder to
   diagnose.  Net LoC win is small (~−15); layering regression is
   real.  Cited here for honesty + as a reminder that not every
   "looks redundant" surface should consolidate.

5. **Replace `CompContext`'s 12 fields with a 3-field root + 3
   per-source sub-structs and derive the rest at consumer sites.**
   `accu_slot`, `init_src_slot`, `aux1_local`, `two_iter`,
   `exit_label`/`continue_label` are derivable from `accu_v`, the
   `NodeAnnotation`, `comp.iter_var2().empty()`, and `expr.id()`
   respectively. Result: a `CompContext` that's exactly what
   `LowerComprehension` cannot recover from arguments —
   `{iter_v, iter_v2, accu_v, source_slot, aux0_local, map_source}`
   plus a tiny `MapSource { handle_local }` / `ListSource
   { ptr_local, end_local, index_local }` variant. Saves ~50 LoC and
   one full re-read of the file's mental model.

Cumulative as-shipped deltas vs `7e161e6`:

| file | baseline LoC | HEAD LoC | net add |
|---|---:|---:|---:|
| `compiler_v2/codegen/expr_lower.cc` | 1101 | 2049 | **+948** |
| `compiler_v2/codegen/resolve_pass.cc` | 481 | 666 | +185 |
| `compiler_v2/codegen/layout_pass.cc` | 353 | 406 | +53 |
| `compiler_v2/runtime/cel_runtime.c` | 1025 | 1241 | +216 |
| **total** | **2960** | **4362** | **+1402** |

New runtime exports added in M5.B (from `runtime/BUILD.bazel`):
`cel_list_append_at`, `cel_list_append_at_if_bool`, `cel_map_iter_init`,
`cel_map_iter_next`, `cel_map_iter_key_at`, `cel_map_iter_value_at`,
`cel_map_insert_at`, `cel_map_insert_at_if_bool` — **8 new exports**,
plus the consolidation that removed `cel_list_set` and
`cel_list_create_with_capacity` (net +6 surface).

New comprehension-related functions in `expr_lower.cc` (counted from
the function inventory above): 22 functions — `LoadAccuBoolPayload`,
`BindCompVariables`, `IsPresizableCollectionAccu`, `ResolveCompContext`,
`ListIterPointerLocal`, `EmitMapPrologue`, `EmitListPrologue`,
`EmitLoadSourceCount`, `PerIterEntryCount`, `EmitPresizeAccu`,
`EmitCompPrologue`, `EmitWriteIntCelValueToSlot`, `BuildLoopCondExit`,
six `TryMatch…` matchers, four `Emit…Step` emitters,
`EmitCompLoopStep`, `EmitMapLoopHead`, `EmitListLoopHead`,
`EmitListLoopTail`, `BuildCompLoop`, `IsShapeC`, `LowerShapeC`,
`LowerComprehension`. The detector zoo + emitter zoo + prologue
helpers account for ~600 LoC by themselves.

---

## Per-area analysis

### Area 1 — `CompContext` sprawl

**Current shape.** `CompContext` is a 12-field struct
(`expr_lower.cc:1076-1112`) populated by `ResolveCompContext`. Several
fields are pure caches of values reachable through other fields:
`accu_slot == accu_v->slot_offset` (`expr_lower.cc:1142`),
`aux1_local == aux0_local + 1` by construction (`expr_lower.cc:1141`),
`two_iter == (iter_v2 != nullptr)` (`expr_lower.cc:1129`),
`exit_label`/`continue_label` are derived from `expr.id()` and the
literal prefix string (`expr_lower.cc:1175-1176`), and `init_src_slot`
is only ever read inside `EmitCompPrologue` from one call site —
where it's a free `init_ann->storage.payload`. `map_source` is
derived from `source_slot`'s repr.

**Simplification.** Trim `CompContext` to:
```cpp
struct CompContext {
  const LaidOutVariable* iter_v;
  const LaidOutVariable* iter_v2;  // nullptr if single-iter
  const LaidOutVariable* accu_v;
  uint32_t source_slot;
  bool map_source;
  uint32_t aux0_local;  // end_off (list) or iter handle (map)
  uint32_t aux1_local;  // index counter (list two-iter) or unused
  int64_t comp_id;      // for label derivation
};
```
Then provide free helpers `ExitLabel(c)` / `ContinueLabel(c)` that
return `absl::StrCat("comp_exit_", c.comp_id)` on demand (single
short hot string per call, no allocation churn vs the cached
strings — they're built once and live for the lowering anyway). Drop
the `init_src_slot` cache; reach the annotation at consumer time.
Drop `two_iter`; replace every `c.two_iter` check with
`c.iter_v2 != nullptr`. Drop `accu_slot`; rewrite every reference as
`c.accu_v->slot_offset`.

**Estimated LoC delta:** −50 in `expr_lower.cc` (struct shrinks; ~30
field-cache writes/reads removed; ResolveCompContext shrinks).
**Risk:** low. Pure mechanical refactor; the resulting code is
strictly closer to the data model and each removed field has a
single trivial replacement.
**Migration sketch:**
- Edit `compiler_v2/codegen/expr_lower.cc` struct `CompContext` to
  the 8-field shape above.
- Inline `c.accu_slot` → `c.accu_v->slot_offset` everywhere
  (~12 sites; `grep -n 'c\.accu_slot' compiler_v2/codegen/expr_lower.cc`).
- Inline `c.two_iter` → `c.iter_v2 != nullptr` (~8 sites).
- Inline `c.init_src_slot` at the one consumer (`EmitCompPrologue`).
- Replace `c.exit_label.c_str()` with `ExitLabel(c).c_str()` (~5 sites);
  same for `continue_label`. The Binaryen API copies the label
  internally so the temporary string is safe.
- Drop `BindCompVariables`' `c->accu_slot = …` and label initialisers.

---

### Area 2 — The detector / emitter zoo for `loop_step`

**Current shape.** `EmitCompLoopStep`
(`expr_lower.cc:1711-1759`) walks `comp.loop_step()` through six
detectors in sequence:
```
TryMatchAccuAppendOne            → EmitAppendStep
TryMatchAccuConditionalAppendOne → EmitConditionalAppendStep
TryMatchAccuMapInsert            → EmitMapInsertStep
TryMatchAccuConditionalMapInsert → EmitConditionalMapInsertStep
TryMatchAccuMapInsertEntries     → EmitMapInsertEntriesStep
TryMatchAccuConditionalMapInsertEntries → EmitConditionalMapInsertEntriesStep
(generic fallback — Emit + cel_copy_slot)
```
Each `…Conditional…` matcher is a wrapper that recognises
`kCall(_?_:_, pred, <inner-shape>, kIdent(accu_var))` and delegates to
the inner non-conditional matcher; the emitter wrappers add `pred` to
the arg list and call the `_if_bool` runtime helper. The matcher
bodies are 4–6 lines each; the emitter bodies are 8–14 lines each.
Total: ~290 LoC for six near-identical match/emit pairs.

**Hypothesis under test.** The maintainer asks: can this collapse to
one classifier + a uniform emitter table?

**Answer: yes, but partially.** There are genuinely three operation
families:

  - **List append (one element)** — emit `cel_list_append_at[_if_bool]`.
  - **Map insert (one k/v pair)** — emit `cel_map_insert_at[_if_bool]`.
  - **Map insert (multi-entry literal, transformMapEntry)** — emit a
    sequence of N `cel_map_insert_at` calls, optionally gated by a
    single 3VL pred (currently only N=1 supported under the
    conditional form per Slice H stub).

Each family has a conditional vs unconditional variant. The
*classification* step is structurally uniform:

```cpp
enum class LoopStepKind {
  kAppendOne,        // map(v,t) / map(v,p,t)·then-arm
  kMapInsertOne,     // transformMap(k,v,t) / 4-arg then-arm
  kMapInsertEntries, // transformMapEntry — N entries from a literal
  kGeneric           // exists/all/exists_one/cel.bind — Emit+copy
};

struct LoopStepShape {
  LoopStepKind kind;
  const cel::Expr* pred = nullptr;  // null for unconditional
  const cel::Expr* key  = nullptr;
  const cel::Expr* value = nullptr;
  const cel::Expr* elem  = nullptr;
  const cel::MapExpr* entries = nullptr;  // for transformMapEntry literal
};

LoopStepShape ClassifyLoopStep(const cel::Expr& loop_step,
                               absl::string_view accu_name);
```

`ClassifyLoopStep` peels the optional `_?_:_` outer wrapper first
(setting `pred`), then matches the inner against the three known
shapes. Single pass, one body, one return; ~40 LoC vs the current
~80 LoC of detector cascade.

The *emit* step doesn't trivially collapse: the three families call
different runtime helpers with different arities. But it does
collapse to one dispatch:

```cpp
absl::Status EmitLoopStepShape(EmitCtx& ctx, const CompContext& c,
                               const LoopStepShape& shape, ...);
```
…with a switch on `shape.kind`. The conditional/unconditional split
collapses to a `pred != nullptr` test inside each arm — one
`_if_bool` helper call vs the unconditional one.

**Net win:** ~90 LoC removed in matchers + emitters, plus the
forward-declared `TryMatchAccuMapInsertEntries` /
`TryMatchAccuConditionalMapInsertEntries` at `expr_lower.cc:1306-1312`
(declared 250 lines before their definitions because
`PerIterEntryCount` needs them — collapsing to one classifier
removes the temporal coupling).

**Pushback.** The current shape was naturally accreted slice by
slice — Slice D added append; Slice G added map-insert; Slice H added
map-insert-entries. Each addition copied the pattern. By the time of
the consolidation commit, no slice was willing to refactor what the
previous one had landed. This is precisely the kind of accretion
that doesn't fit a single coherent design and is worth one focused
flattening pass *now* before the next macro lands and ossifies the
shape further.

**Estimated LoC delta:** −90 to −110 in `expr_lower.cc`.
**Risk:** medium. The classifier must be byte-correct against every
existing matcher; 54 m5b tests + 9 macro-fixture conformance rows
cover the matrix, but introducing a single off-by-one in shape
matching silently routes `transformMap` through the generic path and
the conformance corpus catches it only at the
`ABSL_CHECK(step_ann->storage.kind == kWorkspaceSlot)` (which
*would* fire for cel.@mapInsert because its return type is
typed-void) — caught loudly but at a confusing site. Mitigation:
parameterized table-driven test against every shape, derived from
the existing six `TryMatch…` callsites' inputs.
**Migration sketch:**
- Add `LoopStepShape` + `ClassifyLoopStep` in
  `compiler_v2/codegen/expr_lower.cc`.
- Replace `EmitCompLoopStep`'s six `TryMatch` calls with one
  `Classify` call; switch on `shape.kind`.
- Inline the `EmitAppendStep` / `EmitMapInsertStep` /
  `EmitMapInsertEntriesStep` bodies into the switch arms, merging
  the conditional variants by passing `shape.pred` through a single
  arg-vector builder.
- Delete the six `TryMatch…` functions + four `Emit…Step` functions
  + the forward declarations at line 1303-1312.
- Add a `TEST_P` in `expr_lower_test.cc` parameterized over the
  shape matrix asserting `ClassifyLoopStep` returns the expected
  tag for each canonical AST shape and a curated negative set.

---

### Area 3 — Pre-sizing logic split

**Current shape.** Pre-sizing splits across:
- `IsPresizableCollectionAccu(comp, init_ann)`
  (`expr_lower.cc:1156-1167`) — accu_init shape detector.
- `PerIterEntryCount(comp)` (`expr_lower.cc:1330-1343`) — peeks at
  loop_step to compute the per-iter multiplier; forward-declares two
  matchers that live ~250 lines below.
- `EmitLoadSourceCount(ctx, c)` (`expr_lower.cc:1293-1301`) — emits
  the i32 expression that loads `source.count` from the arena
  header.
- `EmitPresizeAccu(ctx, c, is_map, per_iter, instrs)`
  (`expr_lower.cc:1354-1366`) — composes the previous two; emits the
  `cel_list_create` or `cel_map_create` call.
- `EmitCompPrologue(...)` (`expr_lower.cc:1368-1395`) — calls
  `IsPresizableCollectionAccu` to decide, then either calls
  `EmitPresizeAccu` or emits the `cel_copy_slot` generic path.

That's five functions for one decision; the dispatch lives at
`EmitCompPrologue`'s consumer site exactly as the followon's review
note said it should (per the consolidation commit message:
"Detector inlined at the consumer site per review feedback — no
flags carried on CompContext"). But the helpers around the inlined
detector still fan out across five symbols.

**Simplification.** Collapse to a single `EmitAccuInit` that takes
the prologue context and emits *either* the pre-sized create or the
generic copy — caller is `EmitCompPrologue`, sole consumer:

```cpp
// followon §10.A: emit accu_init.  Pre-sized collection accu
// (cel_list_create / cel_map_create with iter_range.count *
// per_iter_count capacity) when the accu_init is an empty list/map
// literal; otherwise generic Emit(accu_init) + cel_copy_slot.
void EmitAccuInit(EmitCtx& ctx, const cel::ComprehensionExpr& comp,
                  const CompContext& c,
                  std::vector<BinaryenExpressionRef>* instrs);
```

This body inlines `IsPresizableCollectionAccu` (4 lines),
`PerIterEntryCount` (12 lines), `EmitLoadSourceCount` (8 lines), and
`EmitPresizeAccu` (12 lines) into a single ~40-line function. The
forward-declarations at 1306-1312 disappear because the matcher
calls collapse into Area 2's classifier (which by then returns the
shape's entry count as part of the tagged union).

**Estimated LoC delta:** −40 in `expr_lower.cc` (five functions
collapse to one ~40-LoC helper, net saving in symbol/comment
overhead and the forward declarations).
**Risk:** low. All five helpers have exactly one caller; inlining
preserves behaviour by construction.
**Migration sketch:**
- After Area 2 lands (so `LoopStepShape` is the source of truth
  for the per-iter count), add `EmitAccuInit` to
  `expr_lower.cc`.
- Inline `IsPresizableCollectionAccu` + `PerIterEntryCount` +
  `EmitLoadSourceCount` + `EmitPresizeAccu` into the new body.
- Replace the `if (IsPresizable…) EmitPresize…else EmitCelCopy…`
  block in `EmitCompPrologue` with a single
  `EmitAccuInit(ctx, comp, c, instrs)`.
- Delete the five obsolete free functions.

---

### Area 4 — Variable-lifecycle machinery in resolve_pass + layout_pass

**Current shape.** Six pieces:
1. `ResolvedVariableKind` enum (`resolve_pass.h:38-43`) — four
   values: `kFreeVariable`, `kComprehensionIter`,
   `kComprehensionAccu`, `kComprehensionIndex`.
2. `LaidOutVariable.kind` (`layout_pass.h:30-46`) — propagated
   forward.
3. `ScopedIdentResolver::IterKindsFor`
   (`resolve_pass.cc:172-193`) — picks the kind for iter_var /
   iter_var2 from `(map_source, two_iter)`.
4. `ScopedIdentResolver::PreVisitComprehension`
   (`resolve_pass.cc:195-230`) — allocates the three (or four)
   variable entries.
5. `ReserveVariableSlots` (`layout_pass.cc:317-341`) — packs slots
   densely past iter holes (iter vars get `slot_offset = 0`).
6. `ComprehensionLocalsVisitor` (`layout_pass.cc:289-315`) — assigns
   the two extra wasm locals per comp (end_off + iter cursor).
7. `NodeAnnotation` carries `comp_iter_local_index`,
   `comp_accu_local_index`, `comp_iter2_local_index`,
   `comp_aux_local_base` (`annotations.h:106-124`).
8. `EmitVariablePrelude` filters to `kFreeVariable`
   (`expr_lower.cc:130-148`).
9. `BuildCelAbi`'s `EmitVariables` filters to `kFreeVariable`
   (`abi/cel_abi_emit.cc:26`).

**Concrete cleanup wins.**
- **`kComprehensionIndex` is dead.** Defined in `resolve_pass.h` and
  documented in `layout_pass.h:34-36` ("a slot pointer for
  `kFreeVariable`, `kComprehensionAccu`, `kComprehensionIndex`")
  but NEVER assigned anywhere in resolve_pass.cc or any other file
  (grep confirms). `IterKindsFor` returns `kComprehensionAccu` for
  the list-two-iter index, not `kComprehensionIndex`. Removing it
  also removes the inaccurate doc comment in `layout_pass.h`. ~6
  LoC removed across the two headers; eliminates a misleading
  fourth enum value future readers will burn time on.

- **`ReserveVariableSlots`' iter-hole packing is overengineering.**
  At max 2 iter holes per comprehension nesting (iter_var single,
  iter_var2 single), the saved workspace bytes per comp is
  2 × 24 = 48 bytes. For a typical AST with 1–3 comprehensions
  that's ~150 bytes vs a 64 KiB linear memory. The packing logic
  (`slot_count` cursor advancing only when `kind !=
  kComprehensionIter`) is a clever 4-line trick that's easy to
  misread — for so little gain it isn't worth the cognitive cost.
  Simplification: give iter vars slots too. `slot_offset = 0` then
  has its load-bearing dual meaning (the iter local doubles as a
  moving pointer; the slot is never written/read by codegen). The
  saved bytes are immaterial; the saved cognitive load is real.
  *Counter-argument:* zero is the "no slot" sentinel checked by
  `EmitMapPrologue`'s `ABSL_CHECK(c.iter_v->slot_offset != 0)`
  (`expr_lower.cc:1200-1203`); changing this means picking a new
  sentinel or removing the CHECK. Probably worth keeping the
  packing because of the CHECK. Verdict: leave as-is.

- **The four `comp_*_local_index` fields could collapse to one.**
  `comp_aux_local_base` is the first of two reserved aux locals;
  the others are derivable from `comp_iter_local_index` (= the
  `LaidOutVariable` index, which holds `local_index` =
  `slot_offset` for iter; the aux base would suffice). But these
  are AST-side annotations and the stamping happens in two
  distinct passes, so collapsing them creates pass-order coupling.
  Verdict: leave as-is; the four fields are cheap (16 bytes per
  annotation; sparse map) and load-bearing for the per-comp
  binding-by-index lookup that fixed the nested-comp bug.

- **`ComprehensionLocalsVisitor`'s `locals_per_comp = 2` is set
  once and never varies.** It's plumbed as a struct field
  `comprehension_extra_locals_per_comp` (`layout_pass.h:119`) and
  passed to the visitor constructor (`layout_pass.cc:396-398`) for
  no apparent reason — no test varies it; no plan doc mentions a
  scenario where it would. Hard-code `2` (a named constant in
  `expr_lower.cc` already implicitly) and drop the
  `StaticLayout::comprehension_extra_locals_per_comp` field.

**Estimated LoC delta:** −20 across `resolve_pass.h` (4),
`layout_pass.h` (5), `layout_pass.cc` (8), `expr_lower.cc` (3).
**Risk:** low (kComprehensionIndex is genuinely dead;
locals_per_comp is a vestigial knob).
**Migration sketch:**
- Delete `kComprehensionIndex` from `resolve_pass.h:42`; fix the
  comment in `layout_pass.h:34-36` to drop the reference.
- Delete `StaticLayout::comprehension_extra_locals_per_comp`
  (`layout_pass.h:119`); replace with
  `constexpr uint32_t kComprehensionAuxLocalsPerComp = 2;` in
  `layout_pass.cc`'s anonymous namespace; update
  `ComprehensionLocalsVisitor`'s ctor to take a single param.

---

### Area 5 — `IsShapeC` / `LowerShapeC` escape hatch

**Current shape.** `IsShapeC` (`expr_lower.cc:1861-1869`) detects
the `cel.bind` expansion (`iter_range = kCreateList(empty)` AND
`loop_cond = kConst(false)`); `LowerShapeC`
(`expr_lower.cc:1878-1897`) emits a 4-instruction sequence that
bypasses the loop scaffold. Documented in followon §10 future-work
as a correctness escape, not a perf win — the generic
`EmitCompLoopStep` would hit
`ABSL_CHECK(step_ann->storage.kind == kWorkspaceSlot)`
(`expr_lower.cc:1753-1755`) because cel.bind's `loop_step =
kIdent(accu_var)` has kind `kLocal`.

The followon doc says: "Removing `IsShapeC` therefore requires
teaching the generic loop_step emit to handle kLocal-storage
operands. Not a small change, and the current escape works."

**Reality check.** Let me look at what "handle kLocal-storage
operand" actually requires. The generic loop_step path's final
three lines (`expr_lower.cc:1753-1758`):

```cpp
const auto* step_ann = ctx.layout.annotations.Find(comp.loop_step().id());
ABSL_CHECK(step_ann != nullptr &&
           step_ann->storage.kind == StorageKind::kWorkspaceSlot)
    << "LowerComprehension: loop_step storage kind mismatch";
body->push_back(BinaryenDrop(ctx.mod.raw(), *step_or));
body->push_back(EmitCelCopySlot(ctx, c.accu_slot, step_ann->storage.payload));
```

For `kLocal` storage (i.e. `loop_step` is a `kIdent`), the
expression `step_or` is `local.get $i` whose runtime i32 value is
the operand's slot offset. `EmitCelCopySlot(c.accu_slot,
step_ann->storage.payload)` would mis-use `step_ann->storage.payload`
as a slot offset, but the payload for `kLocal` is a local index, not
a slot offset. So the CHECK is correct: today's generic path can't
handle `kLocal` storage.

But the fix is trivial: when `step_ann->storage.kind == kLocal`,
the runtime value of `step_or` itself is the slot offset (because
that's what `EmitKIdentLoad` returns —
`expr_lower.cc:93-104`). So just feed `step_or` as the `src_slot`
to a runtime copy. The current `EmitCelCopySlot` takes
compile-time-known src slot; we'd need a sibling that takes an i32
expression at runtime. That's one new ~6-line helper:

```cpp
// Runtime-slot variant: src is an i32 expression, not a constant.
BinaryenExpressionRef EmitCelCopySlotDyn(EmitCtx& ctx, uint32_t dst,
                                         BinaryenExpressionRef src_expr) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, dst), src_expr};
  return BinaryenCall(mod, "cel_copy_slot_dyn", args, 2,
                      BinaryenTypeNone());
}
```

…which requires a new runtime helper `cel_copy_slot_dyn(dst,
src)` — 4 lines of C. OR: use the existing `cel_copy_slot` if its
signature already takes a runtime src (worth checking — many of
these helpers do). Even if a new helper is needed, the net
delta is:

- delete `IsShapeC` (~10 LoC), `LowerShapeC` (~20 LoC), and the
  short-circuit at `LowerComprehension:1905` (~1 LoC).
- soften the CHECK to handle `kLocal`: ~10 LoC of branching at
  `EmitCompLoopStep`'s tail.
- optionally add `cel_copy_slot_dyn` (~6 LoC + 1 BUILD export + 1
  compile.cc import + 1 engine.cc entry + 1 wat_runner entry).

Even pessimistically that's a ~12-LoC net reduction with a
strictly more uniform codegen path (no special-case for the
"degenerate loop" shape; cel.bind compiles through the same path as
every other macro and runs ~3-4 ops more per call than today).

The followon's perf framing ("~30% throughput win") was already
walked back as "small in the one-shot-codegen / per-eval runtime
model (one wasm function call + a count==0 comparison per cel.bind
eval)". For a typical cel.bind program — bind, evaluate body once —
the extra cost is one `cel_alloc(0)` for the empty iter list header
plus one `iter_off >= end_off` comparison: ~5 ns. **Not worth a
special-case codegen branch.**

**However**: a concrete simplification candidate I'd recommend
holding off on. The win is small (~30 LoC); the diff touches the
hot loop emit code that's the most-tested-but-most-fragile in the
file; and the followon explicitly punted this to "a future codegen
refactor". I'd consolidate Area 2 first (which makes the generic
path's emit cleaner) and revisit `IsShapeC` removal as a
single-commit follow-up afterwards.

**Estimated LoC delta:** −30 in `expr_lower.cc`; +6 in
`cel_runtime.c` + 4 plumbing entries = net −20 across the repo.
**Risk:** medium. Touches the cel.bind code path that's exercised
by `CelBindE2ETest`'s 7 fixtures plus the 8 `bindings_ext.textproto`
conformance rows. A regression here is loud (compile-time CHECK or
runtime trap), not silent — but the iteration cost of debugging it
is high.
**Migration sketch (deferred recommendation):**
- After Area 2 lands, add a `kLocal` branch at
  `EmitCompLoopStep`'s tail that uses an existing dynamic-slot
  copy helper (audit needed: `EmitCelCopySlot` may already cope).
- Delete `IsShapeC` / `LowerShapeC` / the dispatch at
  `LowerComprehension:1905`.
- Verify against `CelBindE2ETest` and `m5b_test.cc`'s NestedBind
  / CompInsideBind / BindInsideComp cases.

---

### Area 6 — Loop-cond peephole (six predicates)

**Current shape.** `expr_lower.cc:1026-1071` defines six tiny
predicates:
- `TryMatchBoolConst` (5 LoC)
- `IsIdentNamed` (4 LoC)
- `IsNotStrictlyFalseOfIdent` (10 LoC)
- `IsNotStrictlyFalseOfNotIdent` (13 LoC)
- `LoadAccuBoolPayload` (5 LoC of body)

`BuildLoopCondExit` (`expr_lower.cc:1429-1453`) dispatches on those
three matchers; on hit emits a single `br_if` against the bool
payload at `i32.load offset=8`. On miss returns
`UnimplementedError`.

**Why it exists.** From the followon doc's plan-vs-execution
delta: cel-cpp's parser emits `loop_cond` only in three shapes —
`kConst bool`, `@not_strictly_false(@result)`, and
`@not_strictly_false(!@result)`. The peephole "avoids needing a
`cel_not_strictly_false` runtime helper."

**Is the peephole necessary?** No, but it's a tax-free win.

  - *Correctness:* The peephole covers every shape cel-cpp emits.
    The UnimplementedError on miss is the right invariant — if a
    new macro ships a different loop_cond shape, we fail loud at
    compile, not subtle at runtime.
  - *Perf:* The alternative is a runtime helper
    `cel_not_strictly_false(out, in)` (~5 LoC of C) that the codegen
    calls every iteration. For a 1000-iter comprehension that's
    1000 wasm function calls vs 1000 `i32.load + br_if`. Wasm
    function call overhead in wasmtime is ~3–5 ns; vs ~1 ns for the
    inline load. So ~3 µs per 1000-iter comprehension. Small but
    not negligible.
  - *Cleanup cost:* Replacing the peephole means: write a new
    `cel_not_strictly_false` helper; plumb it through compile.cc /
    BUILD / wat_runner / engine.cc; rewrite `BuildLoopCondExit`
    to generically lower loop_cond and `br_if` on a negation. Net:
    ~25 LoC removed; ~30 LoC added across the plumbing surfaces.

**Verdict: leave as-is.** The peephole is one of the few pieces of
M5.B that's *not* speculative complexity — it pays for itself in
~3 µs/comprehension at literally zero LoC cost (the matcher bodies
are tight; the dispatcher is one 25-line function with clear
semantics; the UnimplementedError gives future macros a single
named place to extend). The maintainer's hypothesis here is
wrong: the peephole is principled accretion, not premature
optimisation. The six predicates aren't a "zoo" — they're a
3-pattern recogniser. Their natural grouping is already correct.

**Estimated LoC delta:** 0 (recommended leave-as-is).
**Risk:** N/A.

---

### Area 7 — Map iteration via handle-based runtime API

**Current shape.** Four primitives in `cel_runtime.c:1007-1071`:
- `cel_map_iter_init(map_slot) -> handle` (24 LoC)
- `cel_map_iter_next(handle) -> bool` (12 LoC)
- `cel_map_iter_key_at(out_slot, handle)` (3 LoC body, delegates to
  `copy_iter_entry`)
- `cel_map_iter_value_at(out_slot, handle)` (3 LoC body, delegates
  to `copy_iter_entry`)

Wat shape from `wat/64`: per iter the codegen emits
`br_if exit (eqz (call iter_next))`; `call iter_key_at`; optionally
`call iter_value_at` (two-iter case). Three calls per iter for
two-iter map; two for single-iter map.

**Could it collapse to one `cel_map_iter_step(handle, out_key,
out_val) -> bool`?** Mechanically yes. Semantically the cost is:
- Single-iter callers waste a CelValue-copy worth of work writing
  to a `out_val` they never read.
- The "value" slot has to be a real workspace slot in single-iter
  shapes — currently iter_var2's slot doesn't exist for
  single-iter comprehensions; we'd need to either allocate one
  defensively (waste 24 bytes per single-iter map comp) or
  accept a sentinel `out_val_slot = 0` meaning "skip the value
  write" (puts a branch inside the helper).

The two-call shape (`next` then `key_at` / `value_at`) is also
load-bearing for the codegen-side guard semantics: `next` returns
the "advance succeeded" bit that drives the loop's exit `br_if`;
the codegen explicitly *wants* the bool result before the
key/value reads so the loop can exit cleanly without an extra
key/value read after the end.

If we did fuse `next` + `key_at` + `value_at` into one call, the
helper would need to return the bool *and* write the outputs —
which means a struct return or an out param for the bool, which
trips wasm ABI awkwardness (wasm doesn't have multi-return values
in MVP).

**Verdict: leave as-is.** The four-primitive split is principled.
The codegen pays exactly two function calls per iter for single
iter (next + key) and three for two-iter (next + key + value).
Fusing them saves at most one call per iter (~3-5 ns), at the cost
of changing wasm-ABI shape (single-vs-two-return), needing a
sentinel for "skip value", and breaking the clean exit-on-next-eq-0
pattern.

One small refactor candidate inside this surface: `cel_map_iter_init`
and `cel_map_iter_next` could move their `arena_map_iter_state`
deref into a single helper (currently `iter_header` exists for
this; `arena_map_iter_state` lives separately). But that's
already cleanly factored; nothing to do.

**Estimated LoC delta:** 0.
**Risk:** N/A.

---

### Area 8 — append + insert helper pairs

**Current shape.**
- `cel_list_append_at(list_slot, value_slot)` — 22 LoC.
- `cel_list_append_at_if_bool(list_slot, pred_slot, value_slot)` —
  16 LoC; delegates to `cel_list_append_at`.
- `cel_map_insert_at(map_slot, key_slot, value_slot)` — 34 LoC.
- `cel_map_insert_at_if_bool(map_slot, pred_slot, key_slot,
  value_slot)` — 16 LoC; delegates to `cel_map_insert_at`.

The `_if_bool` variants are uniformly: "absorb 3VL on the pred;
poison TYPE_MISMATCH for non-bool; no-op on false; delegate on
true." Their existence is purely so the codegen emits one call
per loop_step rather than a BinaryenIf wrapper.

**Could they collapse to one helper with an optional pred slot?**
Mechanically: add a `pred_slot` parameter; sentinel `0` means
unconditional. But the wasm signatures differ (3-arg vs 4-arg);
unifying to 4-arg adds a wasted i32 push per call site that doesn't
need a predicate.

**Could they go away entirely (codegen-side conditional)?** Yes — and
worth quickly costing.
- Codegen emits `BinaryenIf(pred_is_true_or_3vl_absorb, then-call,
  else-error-propagate)`.
- 3VL absorb at codegen needs the same `pred.kind == CEL_ERROR ||
  pred.kind == CEL_UNKNOWN → propagate to list_slot` logic that
  `cel_list_append_at_if_bool` does today — but now expressed in
  wasm rather than C.
- Cost: per loop iter, ~12 extra wasm ops vs the current single
  call. Per 1000-iter comprehension, ~12 µs vs ~0 µs.

The runtime helper approach has three advantages worth keeping:
1. **Codegen stays simple.** One call per loop_step, regardless
   of predicate shape. The Area 2 collapse becomes cleaner because
   the conditional / unconditional emitter arms differ only in
   "call which helper" — no nested BinaryenIf scaffolding.
2. **3VL absorption logic stays in C, where it's tested by
   `cel_runtime_wasm_test.cc` and `cel_map_test.cc`.** Moving it
   to wasm splits the test surface.
3. **`cel_map_insert_at_if_bool` was added for a real bug**
   (Slice H commit `c8a4c56`: "macros2/transformMap/error_filter
   conformance row"). The fix landed in C; replicating it in
   codegen-emitted wasm is more code, not less.

**Verdict: leave both pairs as-is.** The mirror symmetry is
load-bearing for cross-method consistency (list vs map: both
follow the same `cel_X_op_at_if_bool` pattern). The 3VL absorption
in C is the right placement.

**The one quibble**: the names. `cel_list_append_at` /
`cel_map_insert_at` — what does the `_at` suffix mean? It's the
mate to `cel_list_at` (indexing), but here it doesn't denote
positional insertion. It denotes "comprehension-accu shape" vs
the `cel_map_insert` (literal-shape) sibling. The naming
inconsistency obscures the rule. A rename pass — `_at` → `_accu`?
— would help. Out of scope for a comprehensions simplification;
flag in the Cross-cutting findings instead.

**Estimated LoC delta:** 0.
**Risk:** N/A.

---

### Area 9 — Test surface

**Current shape.** `compiler_v2/e2e/m5b_test.cc` is 1072 LoC,
9 fixture classes, 73 `TEST_F` cases. ~15 still `GTEST_SKIP` (per
`grep -n GTEST_SKIP`), mostly for the bound-list / empty-map-literal
patterns that hit RejectDyn before codegen runs.

The file's 50 lines of helper scaffolding (`CompilePlan`,
`CompilerEmpty`, `CompilerWithVar`, `EvalOk`, `ExpectCompileFails`,
`GlobalEngine`) are well-factored: one place each, used by every
fixture.

**Are the fixtures doing different work?** Mostly yes. Each
fixture targets a distinct slice:
- `ComprehensionExistsListE2ETest` — Slice C, exists/all/exists_one
  + 3VL + bound-list (4 SKIPs in this class for the bound-list
  cases — see #11 in §10 below; they want re-uplifting once
  kHost/kLocal source lands).
- `ComprehensionMapFilterListE2ETest` — Slice D append/filter.
- `ComprehensionMapIterE2ETest` — Slice E map source.
- `ComprehensionTwoIterVarE2ETest` — Slice F.
- `ComprehensionTransformMapE2ETest` — Slice G.
- `ComprehensionTransformMapEntryE2ETest` — Slice H.
- `CelBindE2ETest` — Slice I.
- `ComprehensionNestedE2ETest` — cross-slice nesting matrix.
- `ComprehensionConsumerE2ETest` — comprehension result as operand
  for further ops (5 SKIPs in this class — see §10 below; tests
  written for Slice D's `MapResult`, never un-SKIPed even though
  Slice D shipped).

**Concrete issues.**

1. **`ComprehensionConsumerE2ETest`'s 5 SKIPs are stale.** Look at
   lines 1013-1062: every test in this class
   (MapResultEqualsLiteralList, FilterResultSizeGreaterThanZero,
   FilterResultIndexedReturnsElement, MapResultAsSourceForFurtherComp,
   FilterResultEqualsEmptyList) is gated by
   `GTEST_SKIP() << "M5.B.D ships here";`. But Slice D shipped at
   `8748659` and Slices F/G/H are all green; whatever blocked these
   at Slice-D-time may well be unblocked now. **Recommended:
   un-SKIP and see.** If they pass: 5 free regression tests; if
   they fail: a real cross-cutting gap surfaces.

2. **Bound-list SKIPs (4 cases) are tracked as M5.B follow-up but
   not in the m5-comprehensions-followon doc's §10 future-work.**
   Add a bullet so the work isn't lost.

3. **Several test bodies are 4-line near-duplicates** — e.g.
   `ExistsTrueOnSingleMatch`, `ExistsTrueOnMultipleMatches`,
   `ExistsFalseOnNoMatch` (`m5b_test.cc:190-220`) are all
   "compile + eval + assert bool". A `TEST_P` parameterised over
   `(source, expected_bool)` would consolidate ~30 cases into one
   parameterised suite + a focused
   `INSTANTIATE_TEST_SUITE_P` table. Saves ~150 LoC; loses
   per-test naming clarity (the parameter value becomes the test
   name suffix — adequate). The followon doc's plan §7.2 already
   recommends this discipline.

4. **`m5b_test.cc` has a single `namespace cel { … }` open** (line
   100), which interacts with the §10 public-namespace-consistency
   followup (move to `namespace celwasm`). Tracked there.

**Estimated LoC delta:** −150 (parameterise duplicates); +0 to
+30 (un-SKIP consumer tests if they pass; small fixes if they
don't).
**Risk:** low (test-side only).
**Migration sketch:**
- Un-SKIP the 5 `ComprehensionConsumerE2ETest` cases; run; commit
  the fixes or document why they still SKIP.
- Add a `ComprehensionExistsListParamTest` `TEST_P` covering the
  ~25 list-exists/all/exists_one bool-result tests; delete the
  `TEST_F`s it subsumes.

---

### Area 9.5 — Testing instrumentation: codegen-IR-level coverage

**This section was added 2026-05-17 after the maintainer asked
"how do compilers generally test this sort of code?"  Area 9
covered the e2e test file; this section covers the layer ABOVE
that — the codegen-IR-inspection unit tests that are present for
every other expr_lower arm but missing for the comprehension
surface.**

**Current coverage by layer.**

| Layer | What it does | Coverage for comp |
|---|---|---|
| Codegen-IR-inspect unit | Compile source → walk Binaryen IR → assert tree shape | **NONE** for comprehension shapes; present for kConst/kIdent/kSelect/kMap/kList/kCall in `expr_lower_test.cc` |
| ResolvePass unit | Build expr → run ResolvePass → assert ResolvedVariable kinds + scope | YES — `ResolvePassComprehensionScopeTest` (resolve_pass_test.cc:707+) |
| LayoutPass unit | Build expr → run LayoutPass → assert slot kinds + layout invariants | YES — `LayoutPassComprehensionTest` (layout_pass_test.cc:699+) |
| Runtime helper unit | Call C helpers directly → assert state | PARTIAL — `cel_list_append_at` + `cel_map_iter_*` covered; `cel_list_append_at_if_bool` + `cel_map_insert_at_if_bool` NOT |
| E2E (wasmtime) | Compile + execute + assert decoded `CelValue` | YES — `compiler_v2/e2e/m5b_test.cc` (~54 tests) |
| Conformance (cel-cpp corpus) | Differential test against 2454 upstream rows | YES — drives the macros/macros2/bindings_ext fixtures |

**How real compilers test this layer.**

The canonical pattern is **"compile a source snippet → inspect
the IR → assert structural shape"** without executing.  Examples:

- **LLVM** — every IR transformation pass has hundreds of `.ll`
  golden files in `llvm/test/Transforms/` with `// CHECK: ...`
  directives.  Run `opt -<pass>` on the input, regex-match the
  output against the directives.  Catches "did the lowering
  produce the right ops" instantly.  Thousands of these.
- **Rust** — `src/test/mir-opt/` files contain Rust source with
  embedded MIR-shape assertions.  Same idea, different IR.
- **V8 TurboFan** — `test-pipeline.cc` builds an optimization
  input, runs the pass, asserts the output graph has specific
  node count + kinds.
- **Go SSA** — `test/codegen/<arch>.go` files contain Go source
  with `// amd64: instr1, instr2` directives matching the
  emitted assembly.

**Our equivalent (already used elsewhere in this codebase):**
`ScalarListLiteralEmitsCreateAndAppends`
(`expr_lower_test.cc:792`) — compiles `[10, 20, 30]`, walks the
Binaryen IR, asserts:

```cpp
EXPECT_EQ(BinaryenBlockGetNumChildren(root), 5u);
EXPECT_STREQ(BinaryenCallGetTarget(create), "cel_list_create");
for (i = 1; i <= 3; ++i) {
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_list_append_at");
}
```

No wasm execution.  Walks the tree directly.  This pattern
exists for kMap / kList / kSelect literals but **zero** of these
exist for any comprehension shape.

**The gap, ranked by ROI.**

1. **Codegen-IR golden tests for the 9 comprehension shapes**
   (~12 tests, ~400 LoC).  One TEST per shape — exists, all,
   exists_one, map, filter (list source); transformMap,
   transformMapEntry (single-key entry), transformList (3-arg
   v2), transformList (4-arg v2 conditional); cel.bind via
   Shape-C; nested same-name accu.  Each test compiles the
   source through `LowerToEvalFunction`, then asserts the
   emitted block contains:
   - The expected create call (`cel_list_create` or
     `cel_map_create`) with the right capacity multiplier;
   - The expected per-iter helper call (`cel_list_append_at`,
     `cel_list_append_at_if_bool`, `cel_map_insert_at`,
     `cel_map_insert_at_if_bool`);
   - The expected loop scaffold (Block/Loop/Break shape for
     list source; `cel_map_iter_*` call sequence for map
     source).
   Would have caught the Slice G 3VL-pred bug at codegen-test
   time (1 second), instead of macros2 conformance time
   (whole-suite run).  Living target file:
   `compiler_v2/codegen/expr_lower_comprehension_test.cc` (new).

2. **Pre-sizing predicate tables.**  Two `TEST_P`-style table
   tests:
   - `IsPresizableCollectionAccu` × 8 cases:
     `(accu_init kind, init_ann.repr, expected_result)`.
     Covers `kListExpr empty` + `kMapExpr empty` × list/map
     repr × non-list/non-map repr fall-through.
   - `PerIterEntryCount` × 8 cases: 1 for transformMap 3-arg /
     4-arg, `entry.size()` for transformMapEntry literal of
     size 0/1/2/3, 1 conservative default for unmatched
     shapes.
   ~80 LoC total.  Catches matcher regressions in milliseconds.

3. **`_if_bool` runtime helper unit tests.**  Mirror the
   existing `cel_list_test.cc` / `cel_map_test.cc` patterns:
   - `cel_list_append_at_if_bool` × 5 cases:
     `(pred = ERROR / UNKNOWN / non-bool / false / true) →
     (expected list state)`.
   - `cel_map_insert_at_if_bool` × 5 cases: same matrix.
   ~60 LoC total.  Trivial to write.  Closes the
   regression-coverage gap for the Slice H fix.

4. **Shape-C / `LowerShapeC` IR assertion.**  One TEST.
   Compile `cel.bind(x, 5, x + 1)`, verify the emitted block
   contains NO `cel_map_iter_init` / no list-iter prologue /
   no loop scaffold — just the bind-value copy + the result
   expression.  Locks the fast-path invariant that today only
   m5b_test's `CelBindE2ETest::BindScalarAndUse` exercises
   end-to-end.  ~30 LoC.

5. **Loop-cond peephole matchers** (`TryMatchBoolConst`,
   `IsIdentNamed`, `IsNotStrictlyFalseOfIdent`,
   `IsNotStrictlyFalseOfNotIdent`).  Table tests with positive
   shapes (each predicate's match) + negative shapes (close-
   but-wrong: `@strictly_false` instead of `@not_strictly_false`,
   wrong arg count, wrong inner shape).  ~40 LoC.  Catches
   subtle cel-cpp version drift if a future vendoring of
   cel-cpp changes the loop_cond shape.

6. **Fuzzing harness (longer-term).**  A `libfuzzer`-driven
   target that takes random CEL source, runs `Compile`,
   asserts "either Status is OK and Eval succeeds, or Status
   is a typed error; never crashes; never produces undefined
   behavior."  Out of scope for this milestone but worth
   tracking as a recurring future-work bullet.  Real compilers
   (Rust, V8, LLVM) all run continuous fuzzing; cel-cpp
   does not (verified via repo grep), so we'd be the first.

**Why this matters specifically for the planned simplification
pass.**  Areas 1–5 of this report propose collapsing ~250 LoC of
detector/emitter scaffolding into a tagged-classifier pattern
(Area 2), inlining the pre-sizing helpers (Area 3), and
removing `IsShapeC` (Area 5).  Each of those touches the
codegen-emit path for every comprehension shape.  Today, a
subtle regression — e.g. the unified classifier returns the
wrong shape tag for a corner-case AST, or the pre-sizing
calculation off-by-ones for `transformMapEntry(k, v, {})` —
would only surface via the conformance run (minutes to bisect)
or via an m5b_test e2e failure (seconds to bisect to the test,
but the failure is "value mismatch", not "this matcher
misfired").  With the codegen-IR tests above, a regression
bisects in seconds AND the failure points directly at the
matcher / emitter that's wrong.

The simplification pass should NOT start until at least Items
1–4 land.  Item 5 + 6 are nice-to-haves.

**Estimated LoC delta:** +610 (tests only); 0 in production code.
**Risk:** low.  These are read-only assertions over existing
behavior — they can't break what they observe, only add
coverage.
**Migration sketch:**
- Create `compiler_v2/codegen/expr_lower_comprehension_test.cc`;
  copy the `Pipeline` / `LowerWithDefaultOverloads` /
  `PrepareHostModule` scaffolding from `expr_lower_test.cc` (or
  refactor into a shared `expr_lower_test_lib.h` if duplication
  becomes painful — but keep the test files separate).
- Write Items 1, 4, 5 in that file.
- Write Item 2 in `expr_lower_comprehension_test.cc` too — it's
  pure-predicate testing, no IR walk needed.
- Write Item 3 in `compiler_v2/runtime/cel_list_test.cc` /
  `cel_map_test.cc`, next to the existing helper-unit tests.
- All five items can land in one commit; ~600 LoC of pure
  test code, no production-code changes, can ship
  independently of the simplification pass.

---

### Area 10 — Runtime ABI churn opportunity

The §3.6 list-API collapse already removed two helpers
(`cel_list_set`, `cel_list_create_with_capacity`). The remaining
M5.B runtime additions are all load-bearing:

- `cel_list_append_at` — universal append (used by literals AND
  accus per §3.6).
- `cel_list_append_at_if_bool` — 3VL pred wrapper; see Area 8.
- `cel_map_insert_at` — accu-shape insert (last-write-wins per
  langdef §9.6).
- `cel_map_insert_at_if_bool` — 3VL pred wrapper; see Area 8.
- `cel_map_iter_init` / `next` / `key_at` / `value_at` — see Area 7.

**Candidates for removal:** none I can confidently propose. The
naming inconsistency (`_at` vs no suffix; see Area 8) is the only
real ABI smell, and renaming is purely cosmetic — doesn't reduce
the export count.

**Candidate for *addition*** (not removal): a `cel_copy_slot_dyn`
runtime helper would unblock Area 5's `IsShapeC` removal. But
that's an addition gated on a separate cleanup, not a churn
opportunity in itself.

**Verdict:** the runtime ABI is in its final shape post-§3.6
consolidation. No further M5.B-scoped removals available.

---

## Cross-cutting findings

### CCF-1 — Naming inconsistency: `_at` suffix is overloaded

`cel_list_at` / `cel_list_at_arena` / `cel_list_at_if_bool` —
the `_at` here means "indexed lookup" (the wasm sibling of `[]`).

`cel_list_append_at` / `cel_map_insert_at` / `cel_map_insert_at_if_bool`
— the `_at` here means "comprehension-accu append/insert shape"
(distinguished from `cel_list_set` / `cel_map_insert` — the now-
deleted-or-still-extant literal-shape primitive).

Two different semantic meanings of `_at` in the same namespace.
Future readers will conflate them. Recommend a rename pass —
something like `cel_list_append` (drop `_at`; literals path is
gone) / `cel_map_insert_assign` (the last-write-wins variant). The
runtime ABI is small enough that a single rename commit is
mechanical.

**Effort:** mechanical, ~12 sites across `cel_list.h` / `cel_map.h` /
`cel_runtime.c` / `compile.cc` / `engine.cc` / `wat_runner.cc` /
`expr_lower.cc` + tests. **Risk:** low.

### CCF-2 — Annotation lookup boilerplate

Every codegen helper that touches a sub-expr's annotation writes
the same 3-line idiom:

```cpp
const auto* X_ann = ctx.layout.annotations.Find(comp.X.id());
ABSL_CHECK(X_ann != nullptr);
// use X_ann->{repr, storage, …}
```

…repeated 8+ times in the comprehension code alone
(`expr_lower.cc:1179-1181`, `1208-1210`, `1380-1381`, `1752-1755`,
`1883-1884`, etc.). A typed accessor on `EmitCtx`:

```cpp
const NodeAnnotation& EmitCtx::AnnotationOf(const cel::Expr& e) const;
```

…that CHECKs once and returns by ref consolidates the idiom to one
line and the CHECK to one place. Saves ~16 LoC across the file
and removes the "did I remember to CHECK?" mental burden.

### CCF-3 — Forward declarations 250 lines from their definitions

`TryMatchAccuMapInsertEntries` / `TryMatchAccuConditionalMapInsertEntries`
are forward-declared at `expr_lower.cc:1306-1312` and defined at
`1549-1576`. The forward declaration exists solely because
`PerIterEntryCount` (called from `EmitCompPrologue`) needs to peek
at the loop_step shape from the prologue. Areas 2 + 3 together
eliminate this coupling — once `LoopStepShape` is computed once
upfront, both the prologue (for capacity) and the body
(for the emit table) read from the same struct, no forward
declarations needed.

### CCF-4 — `BinaryenConst(mod, BinaryenLiteralInt32(N))` boilerplate

Used ~30 times in the comprehension code. There's already a helper
`I32Const(WasmModule& mod, uint32_t v)` (`expr_lower.cc:71-73`) but
it's not used everywhere. Mechanical cleanup: replace every
`BinaryenConst(mod, BinaryenLiteralInt32(N))` in expr_lower.cc with
`I32Const(ctx.mod, N)`. Saves ~30 LoC. Trivially low risk.

### CCF-5 — Documentation drift in `layout_pass.h:34-36`

The comment on `LaidOutVariable::slot_offset` lists `kFreeVariable`,
`kComprehensionAccu`, `kComprehensionIndex` as the kinds that get a
slot. `kComprehensionIndex` is dead (Area 4). Fix in the same commit
as that dead-enum removal.

### CCF-6 — `EmitCompPrologue`'s "drop two values then maybe replace
   with a different sequence" is unnecessary

```cpp
instrs->push_back(BinaryenDrop(mod, range_value));
instrs->push_back(BinaryenDrop(mod, init_value));
if (IsPresizableCollectionAccu(...)) EmitPresizeAccu(...);
else EmitCelCopySlot(...);
```

The caller (`LowerComprehension:1906-1911`) emits
`Emit(iter_range)` and `Emit(accu_init)` just to thread them into
the prologue, which immediately drops the values. Their side
effects matter (allocation, host trampolines), but the i32 values
are immediately discarded. This is the standard "evaluate for side
effects" wasm pattern — fine. The reason to call it out: the
generic accu_init path then *evaluates accu_init a second time*
inside `EmitCelCopySlot` via `init_ann->storage.payload`. Wait, no:
`EmitCelCopySlot` is a constant-offset memcpy, not a re-evaluation
— the first `Emit(accu_init)` populates the slot at
`init_ann->storage.payload`, then the second call reads it. So the
"two evaluations" comment in `EmitCompPrologue` ("their side
effects already ran") is accurate but the structure is confusing.
Worth a comment cleanup or, better, restructure to: evaluate
inside `EmitCompPrologue` directly rather than the caller doing
the Emit-then-pass-to-prologue dance. That centralises the "did
accu_init pre-populate the slot?" invariant.

**Estimated cleanup:** ~10 LoC removed; pass `comp` to the
prologue and have it call `Emit` itself.

### CCF-7 — `BindCompVariables` runs side-effecting `ABSL_CHECK`s that the caller relies on

`BindCompVariables` (`expr_lower.cc:1118-1145`) is structured as a
"populate `c`" helper that incidentally CHECKs annotation
consistency. Pulling apart "populate" from "validate" — a separate
`ValidateCompAnnotations` predicate — would let
`ResolveCompContext` order the calls more cleanly (validate first,
then populate). Cosmetic; ~0 LoC change. Low priority.

### CCF-8 — `kHost`/`kLocal` source for `iter_range` is rejected with `UnimplementedError`, but only one of the four bound-list SKIP messages explains why

`ResolveCompContext` (`expr_lower.cc:1188-1194`) rejects non-
workspace-slot iter_range storage with `UnimplementedError`. The
m5b_test.cc bound-list SKIPs cite "kHost/kLocal source is a
follow-up." This is a real gap that the followon doc doesn't track
explicitly — recommend adding a §10 bullet so it isn't lost. Same
issue for `MapOverBoundList` / `MapLargeListGrowthPath` in
`ComprehensionMapFilterListE2ETest:436-470`.

---

## Risk + sequencing

### Recommended order

0. **Area 9.5 Items 1–4 (codegen-IR + predicate unit tests)** —
   PRECONDITION for everything below.  Without these, a
   simplification regression bisects via the conformance run
   (minutes) instead of via a focused matcher / emitter test
   (seconds), and the failure signature is "wrong CelValue at row
   X" instead of "TryMatchAccuMapInsertEntries returned false on
   shape Y".  ~600 LoC of pure test code; can be one commit; zero
   production-code risk.  Land before touching Areas 1, 2, 3, or
   5.  Item 5 + 6 of §9.5 are nice-to-haves.
1. **Area 4 (dead-enum + locals-per-comp knob)** — lowest risk,
   smallest diff, cleanest signal.  Do first among the production
   changes.
2. **Area 1 (CompContext slimming)** — mechanical refactor; no
   behaviour change; sets up Area 2's emitter table to be smaller.
3. **Area 2 + Area 3 (collapse detector zoo + pre-sizing helpers)** —
   land together.  Area 3 is a natural follow-on once Area 2 has
   put the `LoopStepShape` struct in place; the per-iter count
   moves into the classifier's return.
4. **Area 9 (test consolidation + un-SKIP consumer cases)** —
   independent of all the above; can land in parallel.
5. **Area 5 (`IsShapeC` removal)** — deferred.  Land after Areas
   1-3 stabilise; the diff is then a one-commit follow-up.

Areas 6, 7, 8, 10 are all leave-as-is.

### Dependency graph

```
Area 9.5 (test instrumentation, blocks all production changes)
        │
        ▼
Area 4 (dead-enum cleanup, independent) ───────────────┐
                                                       │
Area 1 (CompContext slim) ─→ Area 2 (zoo collapse) ─→ Area 3 (presize merge)
                                                       │
                                                       └─→ Area 5 (IsShapeC removal)

Area 9 (test consolidation, independent — can run in parallel with Area 9.5)
```

### Runtime ABI blast radius

Only Areas 5 + 8 + CCF-1 would touch the runtime ABI. Area 5 adds
one helper (`cel_copy_slot_dyn`) if needed; Area 8 (rejected) would
remove two; CCF-1 (rename pass) doesn't change the export count but
touches every consumer. All other recommended changes are pure
codegen-internal refactors with zero impact on
`compiler_v2/runtime/`, `compile.cc`'s import set, `engine.cc`'s
`kRuntimeExports`, `wat_runner.cc`'s `kRuntimeExports`, or the
ABI emitter.

---

## Specific runtime change recommendations

Per Area 10, no helpers are recommended for deletion or
consolidation. The full as-shipped M5.B runtime surface is in its
final shape post-§3.6 consolidation. The only runtime addition
that's worth holding in reserve:

### `cel_copy_slot_dyn(uint32_t dst_slot, uint32_t src_slot)`

**Purpose:** runtime-slot variant of the existing
compile-time-slot `EmitCelCopySlot`. Used by Area 5's hypothetical
`IsShapeC` removal to handle `kLocal`-storage `loop_step` in the
generic emit path.

**Signature:** `void cel_copy_slot_dyn(uint32_t dst_slot, uint32_t
src_slot)` — both args are runtime i32 expressions.

**Implementation:** trivially `*cel_value_at(dst_slot) =
*cel_value_at(src_slot);`.

**Call sites:** one, in a refactored `EmitCompLoopStep`'s generic
arm.

**Impact:** add the helper to `cel_runtime.c` (~4 LoC); export from
`runtime/BUILD.bazel` (`--export=cel_copy_slot_dyn`); add to
`wat_runner.cc:kRuntimeExports` (array size 102 → 103);
add to `api/engine.cc:kRuntimeExports`; add an import to
`compile.cc::InstallRuntimeImports` (or whichever Install function
covers `cel.cel_copy_slot`). 5 plumbing changes for one helper.
**Hold until Area 5 is scheduled.**

**Audit needed first:** check whether `cel_copy_slot` (whose
internal name is referenced at `expr_lower.cc:854 EmitCelCopySlot`)
already takes a runtime src slot. If yes — no new helper needed;
just call `cel_copy_slot` directly with the loop_step's i32
expression as the second arg. (Likely answer: yes, because
`EmitCelCopySlot` constructs both args via `I32Const`. Quick
verify in `runtime/cel_runtime.c` and `compile.cc`'s installation
site.)

---

## What NOT to change

### 1. `_if_bool` runtime helpers — leave both pairs intact

The 3VL absorption logic for predicate-gated append/insert is in
the right place (C runtime, tested by `cel_runtime_wasm_test.cc`
and the targeted regression `ConditionalPredicateError` added in
commit `c8a4c56`). Inlining it into codegen-emitted wasm splits
the test surface and adds ~12 ops per iter for negligible LoC
savings. The mirror symmetry across list and map is structurally
correct and load-bearing for future consistency. **Do not collapse
into one helper with sentinel pred_slot=0** — the wasm ABI cost
(extra arg push per call site) and the cognitive cost (sentinel
semantics in a hot helper) outweigh the saved LoC.

### 2. Loop-cond peephole — leave as-is

The six predicates + dispatcher (`expr_lower.cc:1026-1071, 1429-
1453`) recognise exactly the three loop_cond shapes cel-cpp emits.
Replacing with a generic "Emit loop_cond; br_if on truthiness"
path requires a new `cel_not_strictly_false` runtime helper and
pays a 3-5 ns/iter wasm-call overhead for zero LoC win. The
peephole pays its way; the `UnimplementedError` on miss is the
right invariant for future-macro detection. **Maintainer's hunch
that this is over-engineered is wrong** — it's the cleanest piece
of the M5.B codegen surface.

### 3. Map iteration four-primitive ABI — leave as-is

`init` / `next` / `key_at` / `value_at` is the natural shape for a
loop-driven generator. Fusing `next + key + value` into one call
breaks wasm-ABI cleanness (single vs multi return), forces a
sentinel for "skip value", and saves at most one call per iter (~3-5
ns). The two-call shape lets the codegen put the exit-`br_if`
between `next` and the reads cleanly.

### 4. `ResolvedVariable.repr`-deferred-resolution-on-first-use

`ScopedIdentResolver::PostVisitIdent` resolves `v.repr` lazily from
the first `kIdent` reference inside the loop body
(`resolve_pass.cc:115-126`). Looks weird — why not infer at
`PreVisitComprehension` from the comp's type info? Because the
checker's type_map only assigns Reprs to `kIdent` nodes, not to
the comp's `iter_var` string. The comp's annotations don't carry a
type for the binding itself; only the kIdent references do. So
lazy resolution is the natural shape. **Don't try to "fix" by
threading type-map lookups into PreVisitComprehension** — the
checker doesn't expose the type for that.

### 5. The per-comp `comp_iter_local_index` /
`comp_accu_local_index` / `comp_iter2_local_index` annotations

These exist because nested comprehensions reuse the literal name
`@result` at every depth, and name-based `LaidOutVariable` lookup
conflates inner with outer. The fix landed in `31e7e4f` ("nested
closeout") after a real bug surfaced in
`[1].exists(y, [0].exists(y, y == 0))`. **Don't try to revert to
name-based lookup** even if it looks tidier in isolation — you
re-introduce the bug. The 16-byte annotation cost is cheap.

### 6. `LayoutPass::ComprehensionLocalsVisitor`'s iter-hole packing
(modulo Area 4's dead-enum cleanup)

`ReserveVariableSlots` packs slots densely past iter holes; the
zero `slot_offset` for iter vars is also the load-bearing
"no-slot" sentinel checked by `EmitMapPrologue`. Removing the
packing trades 48 bytes per comp for a slightly clearer pass; the
trade-off isn't favourable when the sentinel CHECK depends on it.

---

## Closing summary

The accretion is real but not catastrophic. The five recommended
high-ROI changes (Areas 1, 2, 3, 4, 9) collectively remove
~250–300 LoC across `expr_lower.cc`, eliminate one dead enum
value, and consolidate six matcher/emitter pairs into one
classifier + one dispatch. All of them are pure codegen-internal
refactors — zero runtime ABI churn, zero wat_runner / engine.cc /
compile.cc surface changes.

Areas 5 (IsShapeC removal) and CCF-1 (rename pass) are good
follow-ups but should not block the main cleanup; both have wider
diffs and lower per-LoC payoff.

Areas 6, 7, 8, 10 are explicitly NOT changes — the maintainer's
hunches there are wrong, and the leave-as-is rationale is
documented above so future-me doesn't re-burn a session
re-investigating.

Recommended sequencing: Area 4 + Area 1 in one PR (mechanical
cleanup, no behaviour change); Areas 2 + 3 + Area 9 in a second
PR (the substantive flattening of the detector zoo + tests);
Area 5 as an optional follow-up afterwards.
