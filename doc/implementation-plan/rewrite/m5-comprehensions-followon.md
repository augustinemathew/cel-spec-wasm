# Rewrite M5 follow-on — Comprehensions + `cel.bind`

Status: **plan — drafted 2026-05-16, not yet started.  Depends on
M5 (kCall + control flow + activation marshalling).  Anticipated by
the M5 doc's §2.5 carve-out and the conformance README's
"Comprehensions follow-on" forecast row.**

Headline projection: PASS 1144 → ~1232–1278 (+88 to +134
depending on whether Slice F three-arg forms ship in this
milestone or split into a follow-up).  Effective addressable-corpus
pass rate 60% → 67%.

This milestone closes the single biggest language-feature gap
remaining in the post-M7B corpus: every `exists` / `all` /
`exists_one` / `map` / `filter` comprehension currently classifies
as SKIP because `ResolvePass`'s `ComprehensionDetector` early-rejects
any `kComprehensionExpr` (see
`compiler_v2/codegen/resolve_pass.cc:456-462`).  In one milestone
we lift the gate, ship the scope/codegen machinery underneath it,
and as a near-free consequence unlock the customer-named
`cel.bind(name, value, body)` macro (it's a degenerate
zero-iteration comprehension — see §4.6).

## 0 Why this milestone now, in one paragraph

CEL's comprehensions are the user-facing way to write *anything*
non-trivial over lists and maps: input validation
(`fields.exists(f, f.required && f.value == "")`), authorisation
(`request.scopes.all(s, s in user.scopes)`), DSL-style
binding-then-evaluate (`cel.bind(now, request.time,
now > start && now < end)`).  Without them, CEL is reduced to
flat boolean / arithmetic / string expressions over fixed
schemas — the floor of what a customer expects from a policy
language.  Conformance reflects that: ~88 SKIPs and ~46 FAILs
across `macros`, `macros2`, `namespace`, and `bindings_ext`
collapse the moment comprehensions ship.

## 1 Scope

### 1.1 In scope (this milestone)

  - **ResolvePass scope handler** — push/pop scope on
    `kComprehensionExpr` enter/exit, lookup-priority for inner
    bindings, removal of the `ComprehensionDetector` early-reject.
  - **LayoutPass scope-aware slot allocation** —
    comprehension-scope CelValue slots allocated within the
    comprehension's body and released on exit (reusable for
    later siblings).
  - **`kComprehensionExpr` codegen arm** — emit the canonical
    init-loop-result wasm shape per langdef §"Comprehensions".
    Lowers all single-iter-var standard comprehensions:
    `exists`, `all`, `exists_one`, `map`, `filter`, plus
    `cel.bind`'s degenerate empty-iteration form.
  - **Runtime dynamic-list primitive** —
    `cel_list_append_at(list_slot, value_slot)` so `map` and
    `filter` accumulators can grow from `[]`.  Includes arena
    allocator support for list-payload growth.
  - **Map-key iteration helpers** — `cel_map_iter_init`,
    `cel_map_iter_next`, `cel_map_iter_key_at`, OR (alternative
    chosen at design-spike time) a one-shot `cel_map_keys_at`
    materialising a list of keys.  Backs `map.exists(k, p)`,
    `map.all(k, p)`, etc.
  - **`cel.bind` parser-library registration** — wire
    `@cel-cpp//extensions:bindings_ext` into the parser builder
    so `cel.bind(...)` expands to a degenerate comprehension at
    parse time, then rides the codegen arm above with zero
    extra runtime work.  Optional degenerate-form fast path in
    codegen — gated behind a measurement slice.
  - **Three-arg comprehension forms** (`list.exists(i, v, pred)`,
    `map.exists(k, v, pred)`) — second iter-var support in the
    comprehension shape; parser-side macro registration for the
    `macros2` cohort.  May split into a follow-up slice based
    on milestone budget — see §5.

### 1.2 Out of scope (explicit)

  - **Custom comprehension macros at the user level.**  No
    user-defined macros; comprehensions only come from cel-cpp's
    parser library expansions.
  - **Comprehension over streaming sources / lazy iteration.**
    All comprehensions evaluate eagerly against materialised
    list/map values.  No generator / coroutine machinery.
  - **Unbounded recursion / `max_iter` guard.**  CEL spec
    doesn't require it, cel-cpp doesn't ship one; we don't
    either.  If a customer feeds a 10M-element list, they
    get the runtime they paid for.
  - **`unknowns:` tracking through comprehension scope.**  The
    PartialEval / unknowns subsystem (M4 Slice F partial-eval)
    is in scope for *inputs* to comprehensions (an unknown list
    yields an unknown comprehension), but mid-iteration unknown
    accumulation is deferred.
  - **`optional.*` chained access inside comprehensions.**
    Optionals are deferred to the Optionals milestone; the
    comprehension codegen never inspects optional shells.
  - **Performance optimisation of the degenerate `cel.bind`
    form.**  Detecting `iter_range=[] && loop_cond=false` at
    codegen time and emitting `accu_var := value; result`
    directly is a measurable perf win but not a correctness
    requirement.  Gated behind a measurement slice — see §5
    Slice G.2.

### 1.3 Envelope boundary probes

The envelope tightens with this milestone — `IsInM7Envelope` in
`compiler_v2/conformance/runner.cc` already admits comprehension-
shaped programs but they short-circuit at compile.  After this
milestone:

  - **Admit:** any program whose comprehension subtrees pass
    ResolvePass's scope check and LayoutPass's slot allocation.
  - **Reject (SKIP `static_subset:` as before):** comprehensions
    whose `iter_range` types as `dyn(list)` (heterogeneous list
    literals).  The `RejectDyn` gate fires before the
    comprehension is admitted; this milestone doesn't loosen
    that constraint.
  - **Reject (SKIP `compile unimpl:` until Slice F):** three-arg
    comprehension forms, if Slice F is deferred.

## 2 Spec semantics — langdef + cel-cpp parity

### 2.1 The canonical comprehension form (langdef §"Comprehensions")

CEL comprehensions are surfaced as macros at parse time.  Every
macro expands to the same 7-tuple AST node `kComprehensionExpr`:

```
ComprehensionExpr {
  iter_var   : string       // bound name for each iter element
  iter_range : Expr         // typed list or map
  accu_var   : string       // bound name for the accumulator
  accu_init  : Expr         // initial accumulator value
  loop_cond  : Expr → bool  // continue iff true
  loop_step  : Expr         // new accumulator value each iter
  result     : Expr         // returned after the loop
}
```

Evaluation model (langdef + cel-cpp `EvalComprehension` reference):

```
accu := eval(accu_init)
let range := eval(iter_range)
for each elem in range:
    if not eval(loop_cond [iter_var=elem, accu_var=accu]):
        break
    accu := eval(loop_step [iter_var=elem, accu_var=accu])
return eval(result [accu_var=accu])
```

Three details that the codegen must respect:

  - **`loop_cond` is evaluated before each `loop_step`.**  Not
    after.  This lets `exists` short-circuit on the first
    true (`@not_strictly_false(!__result__)` returns false the
    moment `__result__` flips true).
  - **`loop_cond` runs with the current `accu_var` *and*
    `iter_var`.**  Both are in scope.  cel-cpp's `exists` lowers
    `loop_cond` to read only the accumulator
    (`@not_strictly_false(!__result__)`); the codegen must
    nonetheless make `iter_var` available — `exists_one` and
    `filter` do reference it.
  - **`result` does NOT have `iter_var` in scope.**  Only
    `accu_var`.  The iter binding is popped at loop exit before
    `result` is evaluated.  This is why `map` and `filter`
    work: the accumulator carries the cross-iteration state, and
    `result` simply returns it.

### 2.2 The five standard macros and their expansions

From cel-cpp's parser macro definitions
(`third_party/cel-cpp/parser/macro.cc`, observed shapes):

| Macro | iter_var | accu_var | accu_init | loop_cond | loop_step | result |
|-------|----------|----------|-----------|-----------|-----------|--------|
| `e.exists(v, p)` | `v` | `__result__` | `false` | `@not_strictly_false(!__result__)` | <code>__result__ &#124;&#124; p</code> | `__result__` |
| `e.all(v, p)` | `v` | `__result__` | `true` | `@not_strictly_false(__result__)` | `__result__ && p` | `__result__` |
| `e.exists_one(v, p)` | `v` | `__result__` | `0` (int) | `true` | `p ? __result__ + 1 : __result__` | `__result__ == 1` |
| `e.map(v, t)` | `v` | `__result__` | `[]` | `true` | `__result__ + [t]` | `__result__` |
| `e.map(v, p, t)` | `v` | `__result__` | `[]` | `true` | <code>p ? __result__ + [t] : __result__</code> | `__result__` |
| `e.filter(v, p)` | `v` | `__result__` | `[]` | `true` | <code>p ? __result__ + [v] : __result__</code> | `__result__` |

Note `exists_one` returns int-typed accumulator then post-compares
to `1`; this means we **must support comprehension `accu_var` of
any CEL type**, not just bool/list.  Codegen treats `accu_var` as
a generic CelValue slot.

### 2.3 `cel.bind` expansion (cel-cpp extensions/bindings_ext.cc)

`cel.bind(name, value, body)` is a parser-level macro registered
by `BindingsCompilerLibrary()`.  It expands to:

```
ComprehensionExpr {
  iter_var   : "#unused"
  iter_range : []                     // empty list
  accu_var   : <name>
  accu_init  : <value>
  loop_cond  : false                  // exits immediately
  loop_step  : <name>                 // unused (cond is false)
  result     : <body>
}
```

By the time the AST reaches our `ResolvePass` / codegen, there is
no `cel.bind`-specific AST node — it's a `kComprehensionExpr`
with these specific fields.  So **`cel.bind` rides the same
codegen arm as the other comprehensions**, with two consequences:

  1. We don't need a separate macro registration / desugar in our
     codebase.  Just register cel-cpp's parser library and it
     works.
  2. The optional fast path in §1.2 (`iter_range=[] &&
     loop_cond=false` → skip the loop entirely) recognises the
     `cel.bind` shape after expansion.  Pure perf, not
     correctness.

### 2.4 Three-arg comprehension forms (the `macros2` cohort)

`macros2.textproto` exercises a less-standard family registered by
cel-cpp's extended-macro library.  Forms observed:

  - `list.exists(i, v, p)` — `i` is the index, `v` is the value,
    `p` is the predicate.  Expands to a comprehension with two
    bound iter-vars: `iter_var=i`, plus a second derived binding
    `v := list[i]`.
  - `list.all(i, v, p)` / `list.exists_one(i, v, p)` /
    `list.map(i, v, t)` — same shape, different accu.
  - `map.exists(k, v, p)` / `.all` / `.map` — `k` is key, `v` is
    value, `p` is predicate.  Iteration source is map (so
    Slice E's map-key iteration must already work).

Two design choices, decided at design-spike (Slice F.0):

  - **AST shape.**  Option A: extend `kComprehensionExpr` to carry
    a second iter-var.  Option B: emit a `cel.bind(v, iter_range[i], …)`
    nested inside the standard comprehension body.  Option B is
    cleaner — reuses comprehension scope machinery for the
    second binding instead of adding a new shape — and is what
    cel-cpp's macro factory does.  Choose Option B unless the
    spike turns up a blocker.
  - **Macro registration.**  cel-cpp registers these in an
    extended-macro library; we register the same library at
    parser-build time.  No new code in our checker.

## 3 Architecture

### 3.1 Visualising the comprehension lowering

`[1, 2, 3].exists(x, x > 0)` lowers to this wasm shape (full WAT
trace already drafted at `doc/implementation-plan/rewrite/wat/05_comprehension_exists.wat`,
predating this milestone but designed for exactly this codegen
arm):

```
;; Prologue
(local.set $iter_off (i32.const <list_payload_base>))
(local.set $end_off  (i32.const <list_payload_base + count*24>))
(local.set $accu_off (i32.const <accu_init_slot>))   ;; ↦ false
(local.set $step_out (i32.const <step_workspace>))

(block $exit
  (loop $continue
    ;; Exit if past end of range
    (br_if $exit (i32.ge_u (local.get $iter_off) (local.get $end_off)))

    ;; Exit if loop_cond evaluates false — for `exists`, this means
    ;; "accu is already true".  Reading the bool payload directly
    ;; is a peephole; the general path emits a kComprehensionExpr
    ;; lowering of loop_cond as a sub-expression and br_if's on it.
    (br_if $exit (i32.load offset=8 (local.get $accu_off)))

    ;; loop_step: accu := accu || (iter_var > 0)
    (call $cel_int_gt_at_vv (local.get $step_out)
                            (local.get $iter_off)
                            (i32.const <rhs_zero>))
    (call $cel_or_at_vv     (local.get $accu_off)   ;; in/out: accu
                            (local.get $accu_off)
                            (local.get $step_out))

    ;; Bump iter pointer by sizeof(CelValue) = 24
    (local.set $iter_off (i32.add (local.get $iter_off) (i32.const 24)))
    (br $continue)))

;; Result is the accumulator — return its address
(return (local.get $accu_off))
```

Three notes on the codegen:

  - **`iter_off` and `accu_off` are wasm locals**, not memory
    slots.  They are *pointers into* memory; the CelValues
    themselves live in the list payload (for iter) and a
    LayoutPass-assigned workspace slot (for accu).  This avoids
    a per-iter copy of the iter element into a fresh
    workspace slot — we read it in place.
  - **Inner-expr lowering reuses the existing kIdent codegen.**
    A reference to `x` inside `loop_step` (or the predicate)
    compiles to `local.get $x_off`, which we set up to be
    `iter_off`.  A reference to `__result__` compiles to
    `local.get $accu_off`.  No new codegen for "identifier
    inside a comprehension" — the existing kIdent arm handles
    it once ResolvePass maps the name to the right slot.
  - **Slot reuse via LayoutPass scope.**  Workspaces used inside
    the loop (e.g. `$step_out`) can be reused for later siblings
    in the parent expression — LayoutPass releases them on
    comprehension exit.

### 3.2 ResolvePass — scope handler

Today (`compiler_v2/codegen/resolve_pass.cc:456-462`):

```cpp
ComprehensionDetector comprehension_detector;
cel::AstTraverse(ast.ast().root_expr(), comprehension_detector);
if (comprehension_detector.found()) {
  return absl::UnimplementedError(
      "ResolvePass: comprehensions are M5 — reject until scope handling "
      "lands");
}
```

Replace with a scope-aware visitor.  Required behaviour:

  - On entering a `kComprehensionExpr`, push a new scope frame
    containing two bindings: `iter_var → <slot_id>` and
    `accu_var → <slot_id>`.  Slot ids are assigned by LayoutPass
    later; at ResolvePass time we just record the name → unique
    binding-id mapping.
  - Inside the comprehension's subtrees (`iter_range`, `accu_init`,
    `loop_cond`, `loop_step`, `result`), name lookups prefer
    inner scope bindings, fall through to outer scope, fall
    through to top-level activation.
  - **Crucial scope details that must match the spec:**
    - `iter_range` is evaluated in the **outer** scope (it's
      the input to the comprehension, not part of its body).
      The iter_var and accu_var bindings are NOT visible to
      `iter_range`.
    - `accu_init` is evaluated in the **outer** scope (it's the
      initialiser, evaluated before either iter_var or accu_var
      is "current").
    - `loop_cond` and `loop_step` see both iter_var and accu_var.
    - `result` sees **only accu_var**, not iter_var (the iter
      binding is popped after the loop exits).
  - On exit, pop the scope frame.  Subsequent siblings see only
    the outer scope.
  - **Nested comprehensions** — each push a fresh frame onto a
    stack; lookup walks the stack outer-to-inner.  Shadowing
    works naturally.
  - **`accu_var` collision with `iter_var`** — cel-cpp's macros
    always use `__result__` for accu_var, so collision with
    user-chosen iter_vars is unlikely but not impossible (a
    user could write `cel.bind(__result__, 5, ...)`).  We
    follow cel-cpp: the inner binding wins, no error reported.

New file: `compiler_v2/codegen/scope_resolver.h` / `.cc`.

### 3.3 LayoutPass — scope-aware slot allocation

LayoutPass currently allocates a flat slot index per AST node.
For comprehensions we need:

  - **Scope-lifetime slots.**  `iter_var`'s slot is needed only
    inside the comprehension body; on exit, it can be returned
    to the free list and reused by a later sibling.
  - **`accu_var`'s slot survives the loop body but not `result`.**
    After `result` evaluates, the accu slot can be reused.
  - **The comprehension's "return slot" is the accu slot at
    `result`-time.**  We don't allocate a separate slot for the
    comprehension's own output; whatever slot accu lives in is
    the comprehension's value.
  - **Nested comprehensions stack their scope-lifetime slots.**
    Each push records the free-list cursor at entry; pop
    restores it.

Affects: `compiler_v2/codegen/layout_pass.cc`.  New struct:
`ComprehensionFrame { uint32_t iter_slot; uint32_t accu_slot;
uint32_t step_workspace_slot; uint32_t free_cursor_at_entry; };`.

### 3.4 `kComprehensionExpr` codegen arm in `expr_lower.cc`

The actual wasm emit.  Pseudocode:

```cpp
void LowerComprehension(const ComprehensionExpr& comp, ...) {
  // Eval iter_range (outer scope) -> $list_slot
  Lower(comp.iter_range);

  // Eval accu_init (outer scope) -> $accu_slot (= LayoutPass-assigned)
  Lower(comp.accu_init);

  // Range setup: load list base + end
  EmitListIterInit(list_slot, iter_off_local, end_off_local);

  EmitBlock("$exit");
  EmitLoop("$continue");

  // Exit if past end
  EmitBrIfPastEnd(iter_off_local, end_off_local);

  // Eval loop_cond (inner scope: iter_var and accu_var bound)
  PushScope(comp.iter_var → iter_off_local,
            comp.accu_var → accu_slot);
  Lower(comp.loop_cond);                  // → $cond_slot
  EmitBrIfFalse(cond_slot);

  // Eval loop_step (inner scope still active) -> $step_workspace
  Lower(comp.loop_step);

  // Store step result back into accu_slot
  EmitCopyCelValue(step_workspace, accu_slot);

  PopScope();

  // Bump iter
  EmitIterAdvance(iter_off_local);
  EmitBr("$continue");
  EmitEnd();  // loop
  EmitEnd();  // block

  // Eval result (inner scope: only accu_var bound)
  PushScope(comp.accu_var → accu_slot);
  Lower(comp.result);
  PopScope();

  // The result of the comprehension is whatever slot Lower(comp.result)
  // wrote into.  Usually that's accu_slot; for forms like
  // exists_one where result is `__result__ == 1`, it's a fresh
  // slot from the kCall(`_==_`) arm.
}
```

The fiddly bit: `iter_off_local` is a wasm local (i32 pointer
into the list payload), but the existing kIdent arm emits
`local.get $x_off` then reads memory through it.  We bind
`iter_var → iter_off_local` in ResolvePass / LayoutPass; the
existing kIdent lowering reads through the bound local as if it
were any other slot pointer.  This is the "iter_var is just a
moving pointer" pattern from `05_comprehension_exists.wat`'s
"Key design claim" header.

### 3.5 Map iteration codegen (Slice E)

For `map.exists(k, p)`, `map.all(k, p)`, etc., `iter_range`
types as `map(K, V)` rather than `list(T)`.  Two design
options for the iteration:

  - **Option α: materialise keys.**  Add
    `cel_map_keys_at(out_slot, map_slot)` that allocates a
    list of keys.  Comprehension iterates over that list using
    the same shape as Slice C.  Simple but allocates a full
    keys list per comprehension; bad for large maps.
  - **Option β: in-place key iteration.**  Add
    `cel_map_iter_init`, `cel_map_iter_next`, `cel_map_iter_key_at`
    runtime helpers; codegen emits an iterator-shaped loop.  No
    extra allocation; iteration state lives in wasm locals.

cel-cpp uses Option β.  Memory cost matters more than codegen
complexity for our wasm runtime (where every `cel_alloc` rounds
to slot size).  **Choose Option β.**  See WAT
`63_comprehension_exists_map.wat` for the shape.

### 3.6 Dynamic-list primitive (Slice D)

`map` and `filter` accumulators start at `[]` and grow.  Today
the runtime has only static-size lists (`cel_list_create(slot,
count)` is one-shot).  Add:

```c
void cel_list_append_at(uint32_t list_slot, uint32_t value_slot);
```

Semantics:
  - If `list_slot`'s payload pointer is null or list-size is 0,
    allocate a small initial payload from the arena.
  - On growth, allocate a new payload at 2× capacity and copy
    the existing entries.  The arena allocator is forward-only
    so the old payload is simply abandoned (its slots become
    dead arena bytes — acceptable for short-lived comprehensions).
  - Update list header in-place: capacity, length.

Arena cost: each comprehension that produces a list of length N
consumes O(N log N) total payload bytes due to the geometric
growth.  For a 1k-element list this is ~24 KB.  Acceptable for
our typical-program target.

Alternative considered and rejected: pre-allocate
`cel_list_size(iter_range)` capacity at `accu_init` time.  This
is exactly correct for `map` (every iter produces one element)
but over-allocates for `filter` (some iters are skipped).  The
codegen would need to specialise per-form, which complicates the
lowering arm.  Not worth it for the 24 KB best-case savings.

### 3.7 `cel.bind` registration (Slice G)

Three changes:

  1. `compiler_v2/frontend/parse_and_check.cc` — when building
     the `ParserBuilder`, call
     `cel::extensions::BindingsCompilerLibrary().ConfigureParser(builder)`.
  2. `MODULE.bazel` / `BUILD.bazel` — add a dep on
     `@com_google_cel_cpp//extensions:bindings_ext`.
  3. `compiler_v2/conformance/runner.cc` — confirm the
     `bindings_ext.textproto` fixture is loaded.  It already is
     (see the per-fixture inventory in
     `compiler_v2/conformance/README.md`); no change needed
     beyond verifying.

Once those three changes are in, the 8 `bindings_ext.textproto`
rows go from `compile unimpl` SKIP to PASS — they don't need
any code in our codegen because cel-cpp's parser does the
expansion to a generic `kComprehensionExpr` AST node, and the
Slice A–C codegen above handles it.

### 3.8 Two-iter-var support (Slice F) — native AST shape

> **Plan-vs-execution delta (2026-05-16):** an earlier draft of
> this section claimed two-iter-var comprehensions desugar to a
> nested `cel.bind` inside `loop_step`.  **Verified against
> cel-cpp source: that was wrong.**  See
> `language_feature_unlocks/COMPREHENSION_DESIGN.md` §1, §4 for
> the corrected analysis.  The text below is the corrected
> version.

cel-cpp's `kComprehensionExpr` AST natively carries **both**
`iter_var` and `iter_var2` fields (see
`third_party/cel-cpp/common/ast/expr_proto.cc:229-230` — proto
push and pull both fields).  cel-cpp's evaluator
(`eval/eval/comprehension_step.cc`) dispatches `Evaluate1`
(single-var) vs `Evaluate2` (two-var) at runtime based on
whether `iter_var2` is empty.

Binding semantics, per the evaluator:

  - **List source, single iter_var**: iter_slot = current value.
  - **List source, two iter_vars**: iter_var = index (int),
    iter_var2 = value.
  - **Map source, single iter_var**: iter_slot = current key.
  - **Map source, two iter_vars**: iter_var = key, iter_var2 = value.

So the codegen change is real but small:

  - ResolvePass / LayoutPass: read both `iter_var` and `iter_var2`
    from the AST; allocate both bindings when iter_var2 is
    non-empty.
  - `kComprehensionExpr` codegen arm extended:
    - Two-iter-var list: emit a wasm-local int counter for the
      index, write it into a workspace CelValue slot each iter,
      bind iter_var → that slot; iter_var2 → the per-iter list
      element (same shape as the single-iter-var list path).
    - Two-iter-var map: bind iter_var → key workspace,
      iter_var2 → value workspace; pull both via
      `cel_map_iter_key_at` / `cel_map_iter_value_at`.

**No dependency on Slice G** (cel.bind parser registration)
any more.  Slice F is standalone.  Shortens the critical path.

Per-macro-shape impact on Slice F's codegen surface:

  - `list.exists(i, v, p)` / `list.all(i, v, p)` /
    `list.existsOne(i, v, p)` — bool/int accu, generic.
  - `list.transformList(i, v, t)` / `list.transformList(i, v, p, t)`
    — list accu using `cel_list_append_at`.
  - `map.exists(k, v, p)` / `map.all(k, v, p)` /
    `map.existsOne(k, v, p)` — bool/int accu over map source.
  - `map.transformMap(k, v, t)` / `map.transformMap(k, v, p, t)` —
    **map accu**.  Needs a new runtime helper
    `cel_map_insert_at` (see §4.6.1).
  - `map.transformMapEntry(k, v, entry)` /
    `map.transformMapEntry(k, v, p, entry)` — map accu, but
    `entry` is itself a map expression that gets *merged* into
    accu per iter.  See §3.8.1 for the loop_step shape.

The 46 rows in `macros2.textproto` split into:
  - ~28 covered by the bool/int/list cases above (Slice F
    body).
  - ~10 covered by transformMap (Slice G).
  - ~8 covered by transformMapEntry (Slice H).

### 3.8.1 `transformMapEntry` loop_step

`m.transformMapEntry(k, v, entry)` lowers to a
`kComprehensionExpr` with `loop_step` of shape:

```
kCall(_+_, accu_ref, entry)   // merge entry-map into accu
```

…where `entry` is itself a map-literal-shaped expression (most
commonly `{k': t}` — a single key/value pair, but in principle
any map).  Per-iter execution:

  1. Evaluate `entry` to a temp map.
  2. For each `(k_i, v_i)` in temp: `cel_map_insert_at(accu_slot,
     k_i, v_i)`.

Initial implementation: the general path (evaluate-temp-then-
merge).  Optimisation: pattern-detect the single-key shape
`kCreateMap([{k': t}])` and emit a direct
`cel_map_insert_at(accu_slot, k', t)` without the temp.

Last-write-wins on key collisions (matches cel-cpp's runtime
behaviour and our existing `cel_map` semantics).

## 4 Surfaces introduced

### 4.1 `compiler_v2/codegen/scope_resolver.h` / `.cc` (NEW)

```cpp
namespace celwasm {

// Scope-aware name resolution for comprehension bodies.
class ScopeResolver {
 public:
  void PushScope();
  void PopScope();
  void Bind(absl::string_view name, ResolveTarget target);

  // Resolve a name through the scope stack, outer scope, then
  // top-level activation.  Returns nullopt for unbound names.
  absl::optional<ResolveTarget> Resolve(absl::string_view name) const;

 private:
  // Stack of frames, each frame is a name → ResolveTarget map.
  std::vector<absl::flat_hash_map<std::string, ResolveTarget>> frames_;
};

struct ResolveTarget {
  enum class Kind { kActivationSlot, kIterPointerLocal, kAccumulatorSlot };
  Kind kind;
  uint32_t slot_or_local;
};

}  // namespace celwasm
```

### 4.2 `compiler_v2/codegen/resolve_pass.cc` (DELETE+EXTEND)

  - Delete `ComprehensionDetector` and the early-reject.
  - Add `kComprehensionExpr` visitor that:
    - Recurses into `iter_range` in outer scope.
    - Recurses into `accu_init` in outer scope.
    - Pushes a frame with `iter_var`, `accu_var` bindings.
    - Recurses into `loop_cond`, `loop_step` in inner scope.
    - Pops the frame.
    - Pushes a frame with only `accu_var`.
    - Recurses into `result`.
    - Pops the frame.
  - Extends `kIdent` visitor to consult `ScopeResolver` before
    falling through to the activation lookup.

### 4.3 `compiler_v2/codegen/layout_pass.cc` (EXTEND)

  - New `ComprehensionFrame` struct (see §3.3).
  - On `kComprehensionExpr` enter: capture free-list cursor;
    allocate `iter_slot` (a wasm local for the iter pointer),
    `accu_slot` (a CelValue workspace slot), `step_workspace_slot`.
  - On exit: release `iter_slot` + `step_workspace_slot`; `accu_slot`
    survives until after `result` is lowered.

### 4.4 `compiler_v2/codegen/expr_lower.cc` (EXTEND)

  - New `LowerComprehension` arm (~150 LoC) per §3.4 pseudocode.
  - `kIdent` arm extended to consult ResolvePass output for
    iter-local / accu-slot bindings.

### 4.5 `compiler_v2/runtime/cel_list.{h,c}` (EXTEND)

  - New: `void cel_list_append_at(uint32_t list_slot, uint32_t
    value_slot)`.  Grows the list payload geometrically (2×
    capacity) when full; copies into the new payload.
  - Test coverage: append-from-empty, append-to-full,
    cross-type-disallowed (matches existing `cel_list_concat`
    coverage), large-list (10k elements).

### 4.6 `compiler_v2/runtime/cel_map.{h,c}` (EXTEND)

  - New: `cel_map_iter_init(uint32_t map_slot) → uint32_t`
    (returns an iterator handle / cursor).
  - New: `cel_map_iter_next(uint32_t iter_handle) → uint32_t`
    (returns 0 if done, 1 if a key is available).
  - New: `cel_map_iter_key_at(uint32_t out_slot, uint32_t iter_handle)`
    — writes the current key as a CelValue into `out_slot`.
  - Optional (Slice E.2): `cel_map_iter_value_at` for
    two-iter-var map forms.

### 4.7 `compiler_v2/codegen/expr_lower.cc` — `range(N)` codegen (Slice F)

  - Inline lowering, no new runtime helper.  Emits an integer
    counter local seeded with 0 and a comparison against an
    `N` local seeded from `cel_list_size`.  iter_var binds to
    the counter local directly (as an int CelValue, written each
    iteration into a workspace slot for kIdent reads).

### 4.8 `compiler_v2/frontend/parse_and_check.cc` (EXTEND)

  - Call `BindingsCompilerLibrary().ConfigureParser(builder)`.
  - Slice F additional: call
    `ComprehensionsV2CompilerLibrary().ConfigureParser(builder)`
    (or whatever cel-cpp names the extended-macros lib).

### 4.9 New e2e test files

  - `compiler_v2/e2e/m5b_comprehension_basic_test.cc`
    (Slices A–C closeout).  Covers `exists` / `all` /
    `exists_one` over list literals + bound lists.
  - `compiler_v2/e2e/m5b_comprehension_filter_map_test.cc`
    (Slice D closeout).  Covers `map(v, t)`, `map(v, p, t)`,
    `filter(v, p)` end-to-end.
  - `compiler_v2/e2e/m5b_comprehension_map_iter_test.cc`
    (Slice E closeout).  Covers `m.exists(k, p)`, `m.all(k, p)`,
    `m.map(k, t)`.
  - `compiler_v2/e2e/m5b_cel_bind_test.cc` (Slice G).  Covers
    `cel.bind(name, value, body)` shapes, including nested
    binds, bind-inside-comprehension, bind-shadows-outer.
  - `compiler_v2/e2e/m5b_three_arg_comprehension_test.cc` (Slice F).

### 4.10 New WAT traces

  - `60_comprehension_exists_list.wat` — extends the existing
    `05_comprehension_exists.wat` with the actual milestone-
    final memory layout and trampoline names.  The earlier
    file becomes a historical reference.
  - `61_comprehension_all_list.wat` — the `all` flavour.
    Differs from `60` only in `accu_init` (true) and
    `loop_step` (`&&`).
  - `62_comprehension_map_list.wat` — `map(v, t)` with
    `cel_list_append_at`.
  - `63_comprehension_filter_list.wat` — `filter(v, p)`.
  - `64_comprehension_exists_map.wat` — map iteration via
    `cel_map_iter_*`.
  - `65_celbind_degenerate.wat` — degenerate empty-iteration
    form for `cel.bind`.  Doubles as the spec for the
    optional fast-path codegen.
  - `66_nested_comprehension.wat` — `users.exists(u,
    u.scopes.exists(s, s == "admin"))`.  Validates scope-stack
    behaviour, slot-allocation under nesting, identifier
    resolution across two levels.
  - `67_three_arg_list_exists.wat` (Slice F) — `list.exists(i,
    v, p)`, showing the index counter + nested-bind expansion.

## 5 Slicing (work breakdown)

Each slice ships independently, lands on master with green
`scripts/run_full_suite.sh`, and updates the
`testing-checklist.md` / `per-component-test-coverage.md` /
conformance README in the same commit.

### Slice A — ResolvePass scope handler

**Owner:** primary agent.  **Size:** 1 session.

  - Delete `ComprehensionDetector` and the early-reject.
  - Add `ScopeResolver` (new file).
  - Extend `ResolvePass` to visit `kComprehensionExpr` with
    scope push/pop per §3.2.
  - Extend the `kIdent` resolver to consult the scope stack.
  - Unit tests in `resolve_pass_test.cc`:
    - Single comprehension binds `iter_var` and `accu_var`.
    - Nested comprehensions stack scopes correctly.
    - `accu_init` is resolved in outer scope (negative test:
      iter_var not bound when resolving `accu_init`).
    - `result` is resolved with only `accu_var` (negative test:
      iter_var not bound when resolving `result`).
    - Outer-scope fallthrough (a free variable referenced
      inside the comprehension binds correctly).
    - Shadowing (inner `cel.bind` shadows outer name).
  - **Conformance delta**: ~0 (codegen still rejects in Slice
    B–C; the gate just moves from ResolvePass to LayoutPass /
    expr_lower with a clearer error message).

### Slice B — LayoutPass scope-aware slot allocation

**Owner:** primary agent.  **Size:** 0.5 session.

  - Add `ComprehensionFrame`.
  - `LayoutPass::EnterComprehension` / `ExitComprehension` —
    snapshot free-list cursor on entry; restore on exit (after
    `result`'s lowering completes).
  - `accu_slot` survives past loop exit until after `result`.
  - `iter_slot` and `step_workspace` released on loop exit.
  - Unit tests in `layout_pass_test.cc`:
    - Comprehension assigns iter/accu slots in the expected
      range.
    - Sibling comprehensions after a first one reuse the
      released slots.
    - Nested comprehensions allocate stacked frames; pop
      restores correctly.
  - **Conformance delta**: ~0 (still no codegen).

### Slice C — `kComprehensionExpr` codegen arm

**Owner:** primary agent.  **Size:** 1 session.

  - WAT first: write `60_comprehension_exists_list.wat`,
    `61_comprehension_all_list.wat`.  Assemble + run through
    `wat_runner`.  Lock memory layout, trampoline names.
  - Implement `LowerComprehension` in `expr_lower.cc` (§3.4).
  - Extend `kIdent` arm to consult ResolvePass output for
    iter / accu bindings.
  - Add `66_nested_comprehension.wat` and the matching codegen
    test (compile + byte-compare against WAT).
  - Unit tests in `expr_lower_test.cc`:
    - `exists` / `all` / `exists_one` emit the expected shape.
    - Empty `iter_range` returns `accu_init` (positive: empty
      list → false for exists, true for all, 0/false for
      exists_one).
    - Nested comprehensions emit correctly.
  - E2E tests in `m5b_comprehension_basic_test.cc`:
    - `[1, 2, 3].exists(v, v > 1)` → true.
    - `[1, 2, 3].all(v, v > 0)` → true / `[1, -1, 2].all(v, v > 0)` → false.
    - `[1, 2, 1].exists_one(v, v == 1)` → false (two matches).
    - Bound list (activation-bound) operand.
  - **Conformance delta**: +20 to +30 PASS (the half of
    `macros.textproto` that uses `exists` / `all` / `exists_one`
    over list literals; `namespace.textproto`'s 6 comprehension
    rows).

### Slice D — `filter` / `map` + dynamic-list primitive

**Owner:** primary agent.  **Size:** 1 session.

  - WAT first: `62_comprehension_map_list.wat`,
    `63_comprehension_filter_list.wat`.
  - Implement `cel_list_append_at` in `cel_list.c`.  Geometric
    growth.  Unit tests in `cel_list_test.cc`:
    - Append-from-empty.
    - Append-causes-growth.
    - Append-cross-type rejected (matches `cel_list_concat`).
    - Large-list (10k elements) succeeds.
  - Extend `LowerComprehension` to handle list-valued accu
    (no AST change; just verify slot allocation handles
    list CelValues that grow at runtime).
  - E2E in `m5b_comprehension_filter_map_test.cc`:
    - `[1, 2, 3].map(v, v * 2)` → `[2, 4, 6]`.
    - `[1, 2, 3].map(v, v > 1, v * 2)` → `[4, 6]`.
    - `[1, 2, 3].filter(v, v != 2)` → `[1, 3]`.
    - Empty source: `[].map(v, v * 2)` → `[]`.
  - **Conformance delta**: +10 to +15 PASS (the `map` and
    `filter` rows in `macros.textproto`).

### Slice E — Map-key iteration

**Owner:** primary agent.  **Size:** 0.5 session.

  - WAT first: `64_comprehension_exists_map.wat`.
  - Implement `cel_map_iter_init` / `cel_map_iter_next` /
    `cel_map_iter_key_at` in `cel_map.c`.  Iterator state
    fits in a single wasm local (cursor index into the map's
    bucket array; map layout is already deterministic per
    M3.H).
  - Extend `LowerComprehension` to branch on
    `iter_range`-type: list → existing path, map → iterator
    path.
  - E2E in `m5b_comprehension_map_iter_test.cc`:
    - `{1:"a", 2:"b"}.exists(k, k > 1)` → true.
    - `{1:"a", 2:"b"}.all(k, k > 0)` → true.
    - Empty map iteration: `{}.exists(k, true)` → false.
  - **Conformance delta**: +5 to +10 PASS.  Map-iteration rows
    in `macros.textproto` and scattered map-comprehension rows
    in other fixtures.

### Slice F — Three-arg comprehension forms (`macros2`)

**Owner:** primary agent.  **Size:** 1 session (or punt to a
follow-up milestone if budget tight).  **Depends on:** Slice G.

  - **Design spike (F.0)**: read cel-cpp's extended-macros
    library source to confirm the exact expansion shape (one of
    the two options in §3.8).  Document in a one-page spike
    doc.
  - Register the extended-macros parser library at parse time.
  - Implement `range(N)` codegen (Slice F-specific inline
    integer counter).
  - WAT: `67_three_arg_list_exists.wat`.
  - E2E in `m5b_three_arg_comprehension_test.cc`:
    - `[10, 20, 30].exists(i, v, v == 20 && i == 1)` → true.
    - `{a: 1, b: 2}.exists(k, v, k == "b" && v == 2)` → true.
  - **Conformance delta**: +46 PASS (`macros2.textproto`).

### Slice G — `cel.bind` parser-library registration

**Owner:** primary agent.  **Size:** 0.25 session.  **Depends
on:** Slice C (so the degenerate comprehension lowers cleanly).

  - Three changes per §3.7.
  - WAT: `65_celbind_degenerate.wat` (validates the canonical
    shape; doubles as the spec for the optional fast path).
  - E2E in `m5b_cel_bind_test.cc`:
    - Basic: `cel.bind(x, 5, x + 1)` → `6`.
    - Nested: `cel.bind(x, 5, cel.bind(y, 10, x + y))` → `15`.
    - Shadow: `cel.bind(x, 5, cel.bind(x, 10, x))` → `10`.
    - Inside comprehension:
      `[1,2,3].exists(v, cel.bind(t, v * 2, t > 4))` → true.
  - **Conformance delta**: +8 PASS (`bindings_ext.textproto`).
  - **Slice G.2 (optional, separate commit) — fast-path
    codegen.**  Detect `iter_range = []` + `loop_cond = false`
    at LowerComprehension entry; emit `accu_var := value;
    result` directly (no loop prologue).  Adds a bench in
    `compiler_v2/bench/kernel_bench.cc` measuring `cel.bind`
    throughput before/after; commit only if measurable win
    (>20% on `cel.bind`-heavy programs).

### Slice H — Closeout

**Owner:** primary agent.  **Size:** 0.25 session.

  - Re-run `scripts/run_full_suite.sh`.  All green.
  - Re-run `bazel run //compiler_v2/conformance:run_conformance`.
    Capture PASS / SKIP / FAIL counts.
  - Update `compiler_v2/conformance/README.md`:
    - Headline.
    - "Top remaining unlock buckets" — remove "Comprehensions
      follow-on".
    - "Forecast by open milestone" — move M5-followon to
      closed-milestones list.
  - Update `doc/implementation-plan/testing-checklist.md` —
    new "Rewrite M5 follow-on" section.
  - Update `doc/implementation-plan/rewrite/m5-kcall-comprehensions.md`'s
    §2.5 — flip "deferred to follow-on" → "shipped at
    m5-comprehensions-followon".
  - Update this doc's header status: plan → shipped, with
    as-shipped numbers.
  - **Total expected conformance delta** if all slices ship:
    +88 PASS (Slices C+D+E) + +46 PASS (Slice F) + +8 PASS
    (Slice G) = **+142 PASS**, taking 1144 → ~1286.

### 5.1 Recommended sequencing

```
        ┌─ Slice A: ResolvePass scope ────┐
        ├─ Slice B: LayoutPass scope ─────┤
        ├─ Slice C: codegen list iter ────┤── Slice G: cel.bind ──┐
        │  (unlocks exists/all/exists_one)│   (+8 PASS)           │
        ├─ Slice D: filter/map + dynlist ─┤                       │
        │  (unlocks map/filter)           │                       │
        ├─ Slice E: map-key iteration ────┤                       │
        │  (unlocks m.exists/m.all/m.map) │                       │
        └─ Slice F: three-arg (macros2) ──┘                       │
            (depends on Slice G)            ←──────────────────────┘
            (+46 PASS, may split if budget tight)
```

Slices A → B → C → D → E in a tight chain (each builds on the
previous; total ~3 sessions).  Slice G in parallel after Slice C
(0.25 session, +8 customer PASS).  Slice F at the tail, depends
on G.  Slice H closes out.

**Milestone budget check.** If we allocate 5 sessions to the
milestone: A (1) + B (0.5) + C (1) + D (1) + E (0.5) + G (0.25)
+ H (0.25) = 4.5 sessions; Slice F (1 session) fits.  Total:
~5.5 sessions for the full +142 PASS unlock.  Tight but achievable.

## 6 WAT-first design (per CLAUDE.md)

Each codegen slice writes its WAT *first*, assembles with
`wasm-as`, runs end-to-end through `wat_runner`, and locks the
shape *before* implementing the codegen arm in C++.  Per
CLAUDE.md §"WAT-first for ABI and codegen design":

  - **Slice C** writes `60_comprehension_exists_list.wat` and
    `66_nested_comprehension.wat`.  Stub trampolines added to
    `wat_runner` if needed.  Walkthrough in
    `wat-traces.md`.
  - **Slice D** writes `62_…map_list.wat` and `63_…filter_list.wat`.
    `cel_list_append_at` stubbed in `wat_runner` until the real
    body lands.
  - **Slice E** writes `64_…exists_map.wat`.  Map iterator
    helpers stubbed.
  - **Slice F** writes `67_three_arg_list_exists.wat`.  `range`
    codegen inline, no runtime stub needed.
  - **Slice G** writes `65_celbind_degenerate.wat`.

The pre-existing `05_comprehension_exists.wat` becomes a
historical reference and is **not** the milestone's reference
WAT (its memory layout predates the post-M4 codegen
conventions).  Slice C explicitly supersedes it.

## 7 Test plan

### 7.1 Unit tests (per file)

  - `compiler_v2/codegen/scope_resolver_test.cc` (new): scope
    push/pop, nested scopes, shadowing, name resolution
    priority.
  - `compiler_v2/codegen/resolve_pass_test.cc` (extend): see
    Slice A coverage list.
  - `compiler_v2/codegen/layout_pass_test.cc` (extend): see
    Slice B coverage list.
  - `compiler_v2/codegen/expr_lower_test.cc` (extend): see
    Slice C coverage list.
  - `compiler_v2/runtime/cel_list_test.cc` (extend): see Slice
    D coverage list.
  - `compiler_v2/runtime/cel_map_test.cc` (extend): iterator
    init/next/key/value semantics, empty-map iteration,
    iteration-after-mutation (rejected — maps are immutable
    in CEL).
  - `compiler_v2/codegen/wat_runner_test.cc` (extend): every
    new WAT file (60–67) gets a run + byte-for-byte
    equivalence check against its codegen emission.

### 7.2 E2E tests

Five new `m5b_*` files (§4.9).  Each `TEST_F` cites the
specific comprehension form being exercised.  Parameterise
where the matrix is structurally identical (e.g. all four
standard forms over a 3-element int list), keep
`TEST_F`s for one-off bug surfaces (e.g. shadowing inside
nested binds).

### 7.3 Conformance unlock (closeout-gate)

Per CLAUDE.md, manual-tagged tests carry load-bearing assertions
that `bazel test //compiler_v2/...` does not.  Closeout requires
running:

  - `bazel run //compiler_v2/conformance:run_conformance` —
    full corpus.  Confirm headline matches projection (1144 →
    ~1286 if all slices ship; ~1232 if Slice F deferred).
  - `bazel run //compiler_v2/conformance:run_conformance --
    --file=tests/simple/testdata/macros.textproto` — must
    show 38 newly-passing rows.
  - `bazel run //compiler_v2/conformance:run_conformance --
    --file=tests/simple/testdata/bindings_ext.textproto` — must
    show 8/8 PASS.
  - `bazel run //compiler_v2/conformance:run_conformance --
    --file=tests/simple/testdata/macros2.textproto` (Slice F)
    — must show 46/46 PASS.
  - `bazel run //compiler_v2/conformance:run_conformance --
    --file=tests/simple/testdata/namespace.textproto` — must
    show 6 newly-passing rows.

### 7.4 Closeout gate (copy into milestone PR description)

Per
`doc/implementation-plan/per-component-test-coverage.md`'s
keystone gate:

```
[ ] scripts/run_full_suite.sh — green (no GTEST_SKIP at fixture level)
[ ] bazel test //compiler_v2/... — green
[ ] bazel run //compiler_v2/conformance:run_conformance — headline
    matches §5 closeout projection
[ ] All new WAT files re-run + assemble cleanly through wat_runner
[ ] testing-checklist.md "Rewrite M5 follow-on" section all ticked
[ ] m5-kcall-comprehensions.md §2.5 flipped to "shipped"
[ ] This doc's status flipped to "shipped YYYY-MM-DD"
[ ] Conformance README.md headline + per-fixture rows + forecast
    table updated
[ ] per-component-test-coverage.md updated with the new files +
    coverage rows
```

## 8 Risks and open questions

  - **R1. Three-arg expansion shape.**  The Slice F.0 design
    spike has to confirm cel-cpp's exact macro expansion.  If
    it turns out cel-cpp uses a non-trivial AST shape (e.g.
    a new AST node), Slice F balloons.  *Mitigation:* run the
    spike before committing to Slice F in scope.  If it
    balloons, defer Slice F to a follow-up milestone (-46 PASS
    from this milestone's headline; the rest of the unlock
    still ships).
  - **R2. Map iteration state shape (Slice E).**  Whether map
    iteration state fits cleanly in a wasm local depends on
    map's internal layout (M3.H).  *Mitigation:* if Option β
    proves messy, fall back to Option α (materialise keys);
    accept the per-comprehension allocation cost as an
    optimisation TODO.
  - **R3. Dynamic-list append + arena lifetime (Slice D).**  A
    comprehension that produces a large list under
    `cel_list_append_at` consumes O(N log N) arena bytes due
    to geometric growth abandoning old payloads.  Under the
    `compiler_v2/bench/large_list_bench` synthetic the cost
    needs to stay bounded.  *Mitigation:* bench it; if real,
    add a "compact at end" pass that copies the final list
    into a tight allocation before the comprehension exits.
    Spec-out for this milestone if perf is acceptable.
  - **R4. Comprehension inside `==` polymorphic dispatch.**  A
    rare shape: `[1,2,3].exists(v, …) == true`.  The kCall
    `_==_` arm should see the comprehension's result as a
    standard bool CelValue; this needs the comprehension's
    output slot to be addressable as `kCallExpr` operand.
    *Mitigation:* covered by the existing kCall arm's
    operand-from-slot logic; verify with a targeted E2E.
  - **R5. Custom-named accu_var collision with stdlib idents.**
    A user-defined `cel.bind(true, 5, …)` — does cel-cpp's
    parser reject this?  Need to check; if not, our codegen
    needs to as well.  *Mitigation:* spike at Slice G time;
    expect cel-cpp rejects at parse so we get it for free.
  - **R6. Nested-comprehension scope depth.**  Theoretically
    unbounded; practical limit set by wasm local count
    (~1000 ish before binaryen complains).  *Mitigation:*
    none — if a customer writes 50 nested comprehensions
    they have other problems.  Document the limit if we
    observe one.
  - **Q1. Should we ship Slice F in this milestone or split it?**
    If milestone-budget is 5 sessions, F (1 session) fits but
    is the riskiest slice (depends on Slice F.0 design spike).
    *Decision input:* if the user prefers "definitely ship the
    customer feature this milestone", defer F to a small
    follow-up.  If "maximise PASS in one milestone", keep F.
  - **Q2. Should Slice G.2 (cel.bind fast path) ship in this
    milestone?**  Pure perf, no correctness impact.  *Decision
    input:* bench `cel.bind`-heavy programs.  If `cel.bind`
    is on the hot path for a customer DSL, ship it; otherwise
    park.
  - **Q3. Is the cross-host portability question relevant
    here?**  Comprehensions add no new host trampolines —
    `cel_list_append_at` and the map iterator helpers run
    entirely in the runtime (C, no host calls).  No new
    multi-host parser surface.  So this milestone is
    portability-neutral and can ship without resolving
    `wasm_compilation_experiments/PLAN.md`.

## 9 Out-of-scope (call out explicitly)

  - **Custom comprehension macros at the user level.**  CEL spec
    doesn't surface user-defined macros; we don't either.
  - **`max_iter` / iteration-limit guards.**  Spec doesn't
    require; cel-cpp doesn't ship; we don't ship.
  - **Mid-iteration unknown accumulation.**  PartialEval
    interaction with comprehensions stays at "unknown input
    → unknown output" granularity; mid-iter partial-eval is
    deferred.
  - **Optional-typed comprehension elements.**  Defers to the
    Optionals milestone.
  - **Comprehension over streaming sources.**  Defers
    indefinitely; not on any roadmap.
  - **Sliced into the deferred-extensions pass:** comprehensions
    don't unlock string-ext / math-ext / network-ext.  Those
    remain SKIP after this milestone.

## 10 Future work (post-shipping)

  - **Inline `cel.bind` fast path (Slice G.2).**  Ship if
    bench shows >20% win on cel.bind-heavy programs.
  - **Compact-at-end for dynamic lists.**  If R3 bites,
    ship a compaction pass.
  - **Streaming comprehension** (very long-term, post-M-ext):
    if a customer brings a streaming use case, evaluate.
  - **`max_iter` flag** if customer demand surfaces.
  - **Comprehension-aware PartialEval**: track unknown
    accumulators through iterations; unblock the deferred
    `unknowns:` ExprValue binding case in
    `binding_marshal.cc`.

## 11 Dependencies and sequencing

  - **Depends on:** M5 (kCall + control flow + activation
    marshalling), M4 (list literals), M3.H (map layout).  All
    shipped.
  - **Blocks:** No M-numbered milestone formally.  Customer
    `cel.bind` ask is unblocked by Slice G.  M8 wrappers is
    independent and can run in parallel.
  - **Parallel candidates:** M8 wrappers (independent
    codegen-classification work).  Extensions pass
    (independent — different fixtures).
  - **Sequencing-with-other-work-in-flight:** the other agent's
    M7-B parse work touches `compiler_v2/compile.cc` and
    `compiler_v2/runtime/cel_time.{c,h}`.  This milestone
    touches none of those; merge-clean expected.

## 12 Conformance inventory — fixture-by-fixture unlock

From `compiler_v2/conformance/README.md`'s per-fixture table,
post-M7B baseline.  Numbers are the post-milestone projection
assuming all slices ship.

| Fixture | Pre | Δ Slice C+D+E | Δ Slice F | Δ Slice G | Post |
|---|---:|---:|---:|---:|---:|
| `macros.textproto` | 0/44 | +35 | — | — | 35/44 |
| `macros2.textproto` | 0/46 | — | +46 | — | 46/46 |
| `bindings_ext.textproto` | 0/8 | — | — | +8 | 8/8 |
| `namespace.textproto` | 4/14 | +6 | — | — | 10/14 |
| `fields.textproto` | 26/60 | +3 | — | — | 29/60 |
| **Corpus total** | **1144/2454** | **+44** | **+46** | **+8** | **~1242/2454** |

(The 9 `compile unimpl` SKIPs `macros.textproto` retains
post-milestone are `dyn(aggregate)` rejections — out-of-scope
per the static-subset gate.)

If Slice F is deferred to a follow-up, the milestone's headline
is 1144 → ~1196 (+52 PASS).  Slice F shipped: 1144 → ~1242
(+98 PASS).  (The README's earlier "+88" projection in §3 was
conservative — it excluded the indirect-unlock rows in
`namespace` and `fields`; the per-fixture inventory here is
the tighter number.)

## 13 Closing thoughts

Comprehensions is the last big language-feature gap in the
post-M7B corpus that isn't an extension.  Once this milestone
ships, CEL programs written by customers will mostly compile —
the failure modes left are: extensions (math, string, network,
optionals), wrapper-typed proto fields (M8), and the long tail
of small unlocks (matches regex, map-type marshalling, bool→int
overloads).  Of those, M8 is the next-biggest single
unlock (~+55–60 PASS) and the natural sequel.

The customer-named `cel.bind` ships as a 0.25-session
slice (G) on top of this work, demonstrating that the
"foundational unlock → small customer feature" pattern is the
right shape for this milestone family.
