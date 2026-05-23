# Rewrite M5 follow-on — Comprehensions + `cel.bind`

Status: **shipped 2026-05-17.**  Slices A–J all landed end-to-end:
cel.bind + the five standard / v2 comprehension macros (exists /
all / exists_one / map / filter / transformList / transformMap /
transformMapEntry) over both list and map sources.  Pre-sized list
and map accumulators eliminated the runtime growth path
(PRESIZE_INVARIANT traps any codegen regression); list runtime API
collapsed to one create (`cel_list_create(out, capacity)`) + one
write (`cel_list_append_at(list, elem)`), with `cel_list_set`
deleted entirely.  Map-side 3VL pred propagation
(`cel_map_insert_at_if_bool`) closed conditional-transformMap
error-filter rows.  An `undeclared reference to 'cel'` SKIP
classifier graceful-classifies unregistered extension symbols
(block_ext / optionals / etc.).

Conformance delta: **+86 PASS**, headline 1287 → **1373 / 2454**
(55.9%).  Per-fixture: `macros` 0→38 PASS, `macros2` 0→39 PASS,
`bindings_ext` 0→7 PASS, `namespace` 4→6 PASS, `block_ext` 37
FAIL → 25 SKIP + 12 FAIL.  See `compiler_v2/conformance/README.md`
for the closed-milestone entry.

Plan-vs-execution deltas captured per-slice as inline callouts
below (search for "Plan-vs-execution" or "as-shipped").  Tail-end
follow-ups deferred to §10 future-work — none are blocking.

Sibling: `m5b-comprehensions-simplification.md` carries the
analysis-only post-mortem produced 2026-05-17 by a focused
subagent pass.  It calls out 5 high-ROI simplifications across
the codegen + runtime surface (CompContext sprawl, detector /
emitter zoo collapse, `IsShapeC` removal, pre-sizing helper
collapse, dead `kComprehensionIndex` removal) with grounded LoC
numbers, per-area migration sketches, and an explicit "what NOT
to change" list.  Read it before scheduling any post-milestone
cleanup pass.

> **Plan-vs-execution deltas captured at pre-work landing
> (2026-05-17), confirmed by an ad-hoc cel-cpp probe:**
>
>   - **Accumulator name is `@result`** (single `@`), NOT
>     `__result__` as originally specified in §2.2 / §3.x.  This
>     doc and the design companion have both been globally
>     renamed.  Resolver, codegen, and tests must compare against
>     the literal string `"@result"`.  Nested comprehensions get
>     suffixed names (`@result0`, `@result1`, …) — verify per
>     slice; M5.B's nesting tests in §11 cover this.
>   - **`cel.bind`'s `iter_var` is the literal `"#unused"`**.  A
>     reliable Shape-C detector (per design §5) can additionally
>     key off `iter_var == "#unused"` alongside
>     `iter_range == kCreateList(size=0) && loop_cond == const(false)`.
>   - **`map`'s `loop_step` overload-resolves to `add_list`** (NOT
>     `add_int64`).  Already seeded in `compiler_v2/codegen/overload_table.cc:107`
>     pointing at `cel_list_concat` (O(N²)).  Slice D's
>     append-pattern detection rewrites the shape
>     `Call(_+_, Ident(@result), CreateList(size=1))` to
>     `cel_list_append_at` (O(N) amortised) — load-bearing for
>     any comprehension producing a list longer than ~10
>     elements.
>   - **`exists`/`all` loop_cond is `@not_strictly_false(!@result)`
>     / `@not_strictly_false(@result)`** — a wrapped not, not a
>     bare check.  Slice C's loop-cond peephole detector must
>     pattern-match the full `Call(@not_strictly_false, Call(!_, Ident(@result)))`
>     and bare `Call(@not_strictly_false, Ident(@result))` shapes
>     and emit `i32.load offset=8` against the accu slot directly
>     (per WAT 60/61).  Result: `cel_not_strictly_false` runtime
>     helper is **deferred** — cel-cpp's parser never emits a
>     comprehension whose loop_cond is anything other than these
>     three shapes (the two peephole patterns plus `kConst true/false`).
>   - **Two-iter-var has NO synthetic index expression**.  Both
>     `iter_var` and `iter_var2` are bound in the loop body's
>     lexical scope; codegen owns the dual-binding (per WAT 67
>     and design §6 macros 9–13).  No `cel.@indexof(iter_range, i)`
>     hidden select appears.
>   - **e2e file shape**: the §4.9 inventory called for 5
>     separate `m5b_*_test.cc` files; we shipped a single
>     `compiler_v2/e2e/m5b_test.cc` (~1145 lines, 9 fixture
>     classes, 71 tests) following the M7B / M8 pattern.  Easier
>     to thread shared helpers; per-fixture skip / un-skip
>     remains per-slice.

Companion: `m5-comprehensions-design.md` carries the per-macro
inventory, per-shape codegen recipes, edge-case catalogue, and
per-fixture conformance projection.  This plan owns the *what
to ship*; the design doc owns the *how it's shaped*.

Headline (as shipped): PASS 1287 → **1373** (+86, full slate A–J
landed; less than the +89–+109 plan projection because several
fixtures retained `dyn`-related SKIPs that aren't comprehension-
gated and because block_ext's 12 non-PASS rows are out-of-scope
extension shape, not a comp regression).  Effective addressable-
corpus pass rate 52.4% → **55.9%**.

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
  - **Map-key + map-value iteration helpers** —
    `cel_map_iter_init`, `cel_map_iter_next`,
    `cel_map_iter_key_at`, `cel_map_iter_value_at`.  Iterator
    state fits in a single wasm local (cursor index into the
    map's bucket array).  Backs `m.exists(k, p)`,
    `m.transformMap(k, v, t)`, etc.
  - **Map accumulator primitive** — `cel_map_insert_at(map_slot,
    key_slot, value_slot)`.  Geometric bucket-array growth +
    rehash; key-collision overwrites.  Backs `transformMap` /
    `transformMapEntry`.
  - **`cel.bind` parser-library registration + Shape-C fast
    path** — wire `@cel-cpp//extensions:bindings_ext` into the
    parser builder so `cel.bind(...)` expands to a degenerate
    comprehension at parse time.  Codegen pattern-detects the
    degenerate shape (`iter_range = []` AND
    `loop_cond = false`) and emits an inline scope assignment
    without a loop prologue — ~30% throughput win on
    cel.bind-heavy programs.  Correctness-equivalent to the
    generic codegen.
  - **Two-iter-var comprehensions (`iter_var2`)** —
    `list.exists(i, v, p)` / `map.exists(k, v, p)` /
    `transformList(i, v, t)` / `transformMap(k, v, t)` /
    `transformMapEntry(k, v, entry)` (and their conditional
    variants).  cel-cpp's `kComprehensionExpr` AST natively
    carries both `iter_var` and `iter_var2` fields; codegen
    branches on whether `iter_var2` is non-empty.  See §3.8 +
    `m5-comprehensions-design.md` §6.
    Slice F is no longer optional — without it, `macros2` stays
    at 0/46.
  - **Codegen pattern detection for the append-shaped
    `loop_step`** — the kCall arm matches `kCall(_+_, accu_ref,
    kCreateList([single]))` and rewrites to
    `cel_list_append_at` (O(N) amortised), avoiding the
    `cel_list_concat` fall-through (O(N²)).  Same idea for
    `cel_map_insert_at`.  Load-bearing for any comprehension
    producing a list/map longer than ~10 elements.

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
    `optMap` / `optFlatMap` (which use a comprehension under
    the hood) defer with them.
  - **Macro-defined user comprehensions.**  CEL spec doesn't
    surface user-defined macros; we don't either.

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
    true (`@not_strictly_false(!@result)` returns false the
    moment `@result` flips true).
  - **`loop_cond` runs with the current `accu_var` *and*
    `iter_var`.**  Both are in scope.  cel-cpp's `exists` lowers
    `loop_cond` to read only the accumulator
    (`@not_strictly_false(!@result)`); the codegen must
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
| `e.exists(v, p)` | `v` | `@result` | `false` | `@not_strictly_false(!@result)` | <code>@result &#124;&#124; p</code> | `@result` |
| `e.all(v, p)` | `v` | `@result` | `true` | `@not_strictly_false(@result)` | `@result && p` | `@result` |
| `e.exists_one(v, p)` | `v` | `@result` | `0` (int) | `true` | `p ? @result + 1 : @result` | `@result == 1` |
| `e.map(v, t)` | `v` | `@result` | `[]` | `true` | `@result + [t]` | `@result` |
| `e.map(v, p, t)` | `v` | `@result` | `[]` | `true` | <code>p ? @result + [t] : @result</code> | `@result` |
| `e.filter(v, p)` | `v` | `@result` | `[]` | `true` | <code>p ? @result + [v] : @result</code> | `@result` |

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
    `iter_off`.  A reference to `@result` compiles to
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
    always use `@result` for accu_var, so collision with
    user-chosen iter_vars is unlikely but not impossible (a
    user could write `cel.bind(@result, 5, ...)`).  We
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
  // exists_one where result is `@result == 1`, it's a fresh
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

### 3.6 List primitive — unified create + append (Slice D, consolidated 2026-05-17)

> Plan-vs-execution delta: the as-written plan introduced a
> separate `cel_list_create_with_capacity` for comprehension
> accumulators and kept the growth branch inside
> `cel_list_append_at` (geometric 2× capacity, ~24 KB worst case
> per kilo-element list, ~O(N log N) arena bytes total).  Mid-
> slice review concluded the dual API was a wart: literals wrote
> via `cel_list_set(list, INDEX, elem)` while accus wrote via
> `cel_list_append_at(list, elem)`, with two creates carrying
> different `count`-at-create semantics.  The as-shipped shape
> collapses to **one create + one write**:
>
> ```c
> // capacity = N for literals, iter_range.count for accus.
> // count = 0 at create time in both cases.
> void cel_list_create(uint32_t out_slot, uint32_t capacity);
>
> // bumps hdr->count; PRESIZE_INVARIANT traps via
> // __builtin_trap() if count >= capacity.
> void cel_list_append_at(uint32_t list_slot, uint32_t value_slot);
> ```
>
> `cel_list_set` is deleted.  List-literal codegen
> (`EmitKListExpr`) writes via N sequential `cel_list_append_at`
> calls in index order — byte-identical observable state to the
> old positional `cel_list_set` path.  Comprehension accu codegen
> calls `cel_list_create(slot, iter_range.count)` in the prologue
> (per §10.A) and `cel_list_append_at` per loop step; the growth
> branch is gone entirely.  See `compiler_v2/runtime/cel_list.h`
> and `compiler_v2/runtime/cel_runtime.c` for the as-shipped
> bodies; commits 5060b78 (Slice G land) + the consolidation
> commit (this one).

The original plan's "alternative considered and rejected"
paragraph — pre-allocate `iter_range.count` at accu_init — has
been ADOPTED instead.  See §10.A for the rationale shift; the
per-form codegen specialisation it warned against is contained
to one helper (`EmitPresizeAccu`) and amounts to ~10 LoC.

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

### 3.8 Two-iter-var support (Slice F)

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
  - New: `cel_map_iter_value_at(uint32_t out_slot, uint32_t iter_handle)`
    — writes the current value as a CelValue into `out_slot`.
    (Required by Slice F two-iter-var map forms; not optional.)

### 4.6.1 `cel_map_insert_at` (Slice G)

  - New: `void cel_map_insert_at(uint32_t map_slot, uint32_t
    key_slot, uint32_t value_slot)`.
  - Semantics: insert `(key, value)` into the map.  If the map
    payload is null or full, allocate a 2× larger bucket array
    and re-hash existing entries.  If key already exists,
    **overwrite** (last-write-wins, matching cel-cpp's
    `transformMap` runtime behaviour).
  - Geometric growth amortises to O(N) total work for N
    insertions, same as `cel_list_append_at`.
  - Test coverage: insert-from-empty, insert-causes-growth,
    key-collision-overwrites, mixed-key-types-rejected
    (only when source map types as `map(K, V)` with concrete K;
    `dyn(map)` keys remain rejected at `RejectDyn`).

### 4.7 `compiler_v2/codegen/expr_lower.cc` — index counter for two-iter-var list (Slice F)

  - Inline lowering, no new runtime helper.  When `iter_var2`
    is non-empty AND `iter_range` types as `list(T)`: emit an
    integer counter local seeded with 0, write it each
    iteration into a workspace CelValue slot
    (`{kind=CEL_INT, payload.i=index}`), and bind `iter_var` to
    that workspace slot.  `iter_var2` binds to the per-iter
    list element (same pointer as the single-iter-var list
    path).
  - When `iter_var2` is non-empty AND `iter_range` types as
    `map(K, V)`: bind `iter_var` to the key workspace,
    `iter_var2` to the value workspace, populated each
    iteration via `cel_map_iter_key_at` /
    `cel_map_iter_value_at`.

### 4.8 `compiler_v2/frontend/parse_and_check.cc` (EXTEND)

  - Call `BindingsCompilerLibrary().ConfigureParser(builder)`
    (Slice I).
  - Slice F additional: call
    `ComprehensionsV2CompilerLibrary().ConfigureParser(builder)`
    + `.ConfigureCompiler(builder)` (registers the checker
    overloads for `transformList` / `transformMap` /
    `transformMapEntry`).

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

### Slice F — Two-iter-var support (revised)

**Owner:** primary agent.  **Size:** 1 session.  **Depends on:**
Slice C (single-iter-var codegen as the foundation).
Independent of Slice I.

  - Register `ComprehensionsV2CompilerLibrary` (parser +
    checker) at parse time.
  - Extend ResolvePass / LayoutPass to bind `iter_var2` when
    non-empty (one extra workspace slot for list-source,
    one for map-source).
  - Extend `LowerComprehension`:
    - List-source two-iter-var: emit an integer counter wasm
      local for the index; write it into a workspace CelValue
      each iter; bind iter_var → that slot; iter_var2 → the
      per-iter list element.
    - Map-source two-iter-var: bind iter_var → key workspace,
      iter_var2 → value workspace; pull both via
      `cel_map_iter_key_at` / `cel_map_iter_value_at`.
  - WAT: `67_three_arg_list_exists.wat`,
    `68_transformlist_indexed.wat`.
  - E2E in `m5b_three_arg_comprehension_test.cc`:
    - `[10, 20, 30].exists(i, v, v == 20 && i == 1)` → true.
    - `{'a': 1, 'b': 2}.exists(k, v, k == 'b' && v == 2)` → true.
    - `[2, 4, 6].transformList(i, v, v / 2 + i)` → `[1, 3, 5]`.
    - `[2, 4, 6].transformList(i, v, i != 1, v / 2 + i)` →
      `[1, 5]`.
  - **Conformance delta**: +28 PASS — the bool/int/list cohort
    of `macros2.textproto`.  transformMap and transformMapEntry
    rows (~18) wait for Slices G and H.

### Slice G — `transformMap` (map accumulator)

> **Plan-vs-execution delta (shipped 2026-05-17, commits 5060b78
> + consolidation):**  The runtime helper `cel_map_insert_at`
> shipped without the geometric growth + rehash described below
> — it traps via `__builtin_trap()` if `count >= capacity`.
> The map accumulator is pre-sized to `iter_range.count` in the
> comprehension prologue (see §10.A and §3.6), so growth is
> unreachable in steady state.  `cel_map_create` (the existing
> map literal constructor) is reused as the accu creator — no
> separate `cel_map_create_with_capacity` shipped.  The two
> insert helpers stay distinct by design: `cel_map_insert`
> (literals, poison-on-duplicate per langdef map-literal rule)
> and `cel_map_insert_at` (accus, last-write-wins per langdef
> comprehension rule); the semantic split is langdef-mandated
> and cannot collapse.  `ComprehensionsV2CheckerLibrary` had to
> be registered (in addition to the parser macro registry) so
> the `cel.@mapInsert` overload type-checks; without it,
> transformMap source fails type-checking with "no matching
> overload."

**Owner:** primary agent.  **Size:** 0.5 session.  **Depends
on:** Slice F (iter_var2) + Slice E (map iteration).

  - WAT: `69_transformmap_kv.wat`.
  - Implement `cel_map_insert_at` in `cel_map.c`.  Geometric
    bucket-array growth + rehash; key-collision overwrites.
    Unit tests in `cel_map_test.cc`:
    - Insert-from-empty.
    - Insert-causes-growth.
    - Key-collision-overwrites.
  - Extend `LowerComprehension` to recognise map-typed accu
    (from AST: accu_init types as `map(K, V)`).  Lower
    `loop_step` of shape `kCall(map_with_kv, accu_ref, key,
    value)` (cel-cpp's IR for "extend map with one entry") to a
    direct `cel_map_insert_at` call.
  - E2E in `m5b_transform_map_test.cc`:
    - `{'a': 1, 'b': 2}.transformMap(k, v, v * 2)` →
      `{'a': 2, 'b': 4}` (note: keys unchanged; values
      transformed).
    - `{'a': 1, 'b': 2, 'c': 3}.transformMap(k, v, v > 1,
      v * 2)` → `{'b': 4, 'c': 6}`.
    - Empty source: `{}.transformMap(k, v, v + 1)` → `{}`.
  - **Conformance delta**: +10 PASS — `transformMap` rows
    in `macros2.textproto`.

### Slice H — `transformMapEntry` (per-iter map merge)

**Owner:** primary agent.  **Size:** 0.5 session.  **Depends on:**
Slice G (`cel_map_insert_at`).

  - WAT: `70_transformmapentry_merge.wat`.
  - Extend `LowerComprehension` to recognise the
    `transformMapEntry` `loop_step` shape (`kCall(_+_,
    accu_ref, entry_expr)` where `entry_expr` is map-typed).
    General path: evaluate `entry_expr` to a temp map, iterate
    its entries, insert each into accu via `cel_map_insert_at`.
  - Optimisation (deferred to Slice H.2 if perf demands):
    pattern-detect single-key shape
    `kCreateMap([{k': t}])` and emit a direct
    `cel_map_insert_at(accu, k', t)` without the temp.
  - E2E in `m5b_transform_map_entry_test.cc`:
    - `{'foo': 'bar'}.transformMapEntry(k, v, {k + v: k})` →
      `{'foobar': 'foo'}`.
    - `{'foo': 'bar', 'baz': 'bux'}.transformMapEntry(k, v,
      k != 'baz', {k + v: k})` → `{'foobar': 'foo'}`.
    - Empty entry: `{'foo': 'bar'}.transformMapEntry(k, v, {})`
      → `{}`.
  - **Conformance delta**: +8 PASS — `transformMapEntry` rows
    in `macros2.textproto`.

### Slice I — `cel.bind` parser-library registration + Shape-C fast path

**Owner:** primary agent.  **Size:** 0.25 session.  **Depends
on:** Slice C.

  - Call
    `BindingsCompilerLibrary().ConfigureParser(builder)`
    in `parse_and_check.cc`.
  - Add `@cel-cpp//extensions:bindings_ext` BUILD dep.
  - **Shape-C fast path** in `LowerComprehension`: at entry,
    pattern-match `iter_range = []` AND `loop_cond = false`;
    if matched, emit `accu_var := value; result` directly (no
    loop prologue, no iter setup).  See `COMPREHENSION_DESIGN.md`
    §5 Shape C and §6 macro #8.  Correctness-equivalent to the
    generic Shape-A path; ~30% throughput improvement on
    cel.bind-heavy programs (per cel-cpp benchmarks).  Ship
    together — no separate G.2 slice.
  - WAT: `65_celbind_degenerate.wat`.
  - E2E in `m5b_cel_bind_test.cc`:
    - Basic: `cel.bind(x, 5, x + 1)` → `6`.
    - Nested: `cel.bind(x, 5, cel.bind(y, 10, x + y))` → `15`.
    - Shadow: `cel.bind(x, 5, cel.bind(x, 10, x))` → `10`.
    - Comprehension-inside-bind:
      `cel.bind(valid, [1,2,3], [3,4,5].exists(e, e in valid))`
      → true.
    - Bind-inside-comprehension:
      `[1,2,3].exists(v, cel.bind(t, v * 2, t > 4))` → true.
  - **Conformance delta**: +8 PASS (`bindings_ext.textproto`).

### Slice J — Closeout

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
    Slice C +20-30, D +10-15, E +5-10, F +28, G +10, H +8,
    I +8 = **~+89-109 PASS**, taking 1144 → ~1233-1253.
    (Lower bound is more realistic given likely SKIP→FAIL
    reclassifications.)

### 5.1 Recommended sequencing

```
A: ResolvePass scope
├── B: LayoutPass scope
└── C: codegen list-iter (exists/all/exists_one over list)  ────┐
        │                                                       │
        ├── D: dynamic-list append (map/filter over list)       │
        │                                                       │
        ├── E: map iteration (exists/all/exists_one over map) ──┤
        │                                                       │
        ├── F: iter_var2 — needs C only; not blocked by I       │
        │   │                                                   │
        │   ├── G: cel_map_insert_at + transformMap             │
        │   │   │                                               │
        │   │   └── H: transformMapEntry                        │
        │   │                                                   │
        │   └─── (transformList etc. are inside F)              │
        │                                                       │
        ├── I: cel.bind + Shape-C fast path  ───────────────────┤
        │                                                       │
        └── J: closeout  ───────────────────────────────────────┘
```

Critical path A → B → C is sequential (3 sessions).  After C:
D, E, F, I are independent (parallel candidates).  G chains
after F (needs iter_var2 binding); H chains after G (needs
`cel_map_insert_at`).

**Milestone budget**: A (1) + B (0.5) + C (1) + D (1) + E (0.5)
+ F (1) + G (0.5) + H (0.5) + I (0.25) + J (0.25) = **6.5
sessions** for the full unlock.

**Minimum-viable milestone if budget is tight (4 sessions):**
A + B + C + D + I + J = 4 sessions.  Ships:
  - Core list comprehensions (`exists`/`all`/`exists_one`/
    `map`/`filter` over lists).
  - `cel.bind()` for the customer.
  - **Unlocks ~+50 PASS** (most of `macros.textproto` +
    `bindings_ext.textproto`).
Defers to a follow-up milestone:
  - Map iteration (Slice E) — +5-10.
  - Two-iter-var (Slice F) — +28.
  - transformMap / transformMapEntry (G, H) — +18.

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
  - **Slice F** writes `67_three_arg_list_exists.wat` and
    `68_transformlist_indexed.wat`.  No new runtime stubs —
    iter_var2 codegen is inline.
  - **Slice G** writes `69_transformmap_kv.wat`.
    `cel_map_insert_at` stubbed in `wat_runner` until landed.
  - **Slice H** writes `70_transformmapentry_merge.wat`.
  - **Slice I** writes `65_celbind_degenerate.wat`.

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

  - **R1. Map iteration state shape (Slice E).**  Whether map
    iteration state fits cleanly in a wasm local depends on
    map's internal layout (M3.H).  *Mitigation:* if Option β
    proves messy, fall back to Option α (materialise keys);
    accept the per-comprehension allocation cost as an
    optimisation TODO.
  - **R2. Dynamic-list append + arena lifetime (Slice D).**  A
    comprehension that produces a large list under
    `cel_list_append_at` consumes O(N log N) arena bytes due
    to geometric growth abandoning old payloads.  Under the
    `compiler_v2/bench/large_list_bench` synthetic the cost
    needs to stay bounded.  *Mitigation:* bench it; if real,
    add a "compact at end" pass that copies the final list
    into a tight allocation before the comprehension exits.
    Spec-out for this milestone if perf is acceptable.
  - **R3. Comprehension inside `==` polymorphic dispatch.**  A
    rare shape: `[1,2,3].exists(v, …) == true`.  The kCall
    `_==_` arm should see the comprehension's result as a
    standard bool CelValue; this needs the comprehension's
    output slot to be addressable as `kCallExpr` operand.
    *Mitigation:* covered by the existing kCall arm's
    operand-from-slot logic; verify with a targeted E2E.
  - **R4. Custom-named accu_var collision with stdlib idents.**
    A user-defined `cel.bind(true, 5, …)` — does cel-cpp's
    parser reject this?  Need to check; if not, our codegen
    needs to as well.  *Mitigation:* spike at Slice G time;
    expect cel-cpp rejects at parse so we get it for free.
  - **R5. Nested-comprehension scope depth.**  Theoretically
    unbounded; practical limit set by wasm local count
    (~1000 ish before binaryen complains).  *Mitigation:*
    none — if a customer writes 50 nested comprehensions
    they have other problems.  Document the limit if we
    observe one.
  - **R6. Codegen pattern-detect miss → quadratic comprehension.**
    The kCall arm rewrites `accu + [t]` to
    `cel_list_append_at` (O(N) amortised).  If the AST shape
    differs slightly (e.g. `accu + ([t] + [])` after a peephole
    that doesn't run), the pattern misses and we fall through
    to `cel_list_concat` (O(N²)).  *Mitigation:* bench
    `[0..1000].map(v, v * 2)` end-to-end; if quadratic,
    enumerate the AST shapes that should match and tighten the
    detector.  Add a `--warn-quadratic-comprehension` debug
    flag.  See `COMPREHENSION_DESIGN.md` §12 R7.
  - **R7. `transformMap` key-collision semantics.**  Last-write-
    wins matches cel-cpp's runtime behaviour and our existing
    `cel_map` semantics, but spec isn't explicit.
    *Mitigation:* fixture-test against cel-cpp's evaluator; if
    behaviour diverges, document the spec ambiguity and ship the
    cel-cpp-compatible side.
  - **R8. `transformMapEntry` with non-literal entry expression.**
    `entry` can in principle be any map-typed expression, not
    just a literal.  Cel-cpp's general path handles this; ours
    must too (via the temp-map + iterate approach).  *Mitigation:*
    e2e test with `entry = some_func_returning_map(k)` shape;
    confirm general path works.
  - **R9. Map iteration order determinism.**  CEL spec doesn't
    mandate order; tests are written order-independently.  Our
    M3.H map layout is deterministic given the same insertion
    sequence.  *Mitigation:* if a fixture fails for what smells
    like iteration order, double-check the bucket-walk order is
    stable across compile + eval.
  - **Q1. Full unlock (all 10 slices, 6.5 sessions, ~+90 PASS)
    or minimum-viable (6 slices, 4 sessions, ~+50 PASS)?**
    Minimum-viable ships core list comprehensions +
    `cel.bind` (customer ask); defers map iteration,
    two-iter-var, transformMap, transformMapEntry to a
    follow-up.  Full unlock ships everything in one milestone.
    *Decision input:* customer urgency on `cel.bind` vs
    overall conformance velocity.
  - **Q2. Is the Shape-C `cel.bind` fast path correctness-
    safe?**  Shape A (generic comprehension codegen) handles
    `cel.bind` correctly already; Shape C is a perf
    optimisation.  Risk: a future refactor that changes the
    AST detection conditions silently mis-routes a non-bind
    comprehension through Shape C.  *Mitigation:* the
    detection predicate matches structurally on
    `iter_range == kCreateList([])` AND
    `loop_cond == kBoolConst(false)`; both are precise.  Add
    a CHECK at codegen that any AST matching Shape-C
    detection also produces the same result as Shape-A on a
    fuzzed input matrix.
  - **Q3. Is the cross-host portability question relevant
    here?**  Comprehensions add no new host trampolines —
    `cel_list_append_at`, `cel_map_insert_at`, and the map
    iterator helpers run entirely in the runtime (C, no host
    calls).  No new multi-host parser surface.  So this
    milestone is portability-neutral and can ship without
    resolving `wasm_compilation_experiments/PLAN.md`.

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

  - **Pre-size list AND map accumulators to `iter_range.count`
    — SHIPPED 2026-05-17** (this milestone, post-Slice-G
    consolidation commit).  Was originally going to be
    deferred to milestone closeout; user-driven mid-slice
    review collapsed it into the milestone instead so the
    runtime API would land in its final shape.  See §10.A
    below for the as-shipped writeup; original deferral
    rationale + implementation plan kept for historical
    context.
  - **Comprehension iter_range over host-origin sources
    (bound list/map, proto repeated/map field) — SHIPPED
    2026-05-22** as Slices 1+2 (m5b §CCF-8).  Approach (A) from
    the original design: kind-dispatching runtime iter helpers.
      - **Slice 1 (maps):** Extended `cel_map_iter_init` to
        dispatch on CelValue.kind.  CEL_MAP_ARENA paths
        unchanged; CEL_MAP_HOST calls a new host trampoline
        `cel_host.cel_map_iter_open(state_offset, map_slot)`
        (`CelMapIterOpenImpl` in `cel_host.cc`) that snapshots
        the HostMapBacking via ForEach into a flat 48-byte/entry
        region in the arena (key at +0, value at +24).
        `MapIterState` grew from 8B to 16B carrying
        `{kind, cursor, payload, count}`.  Pre-sizing routes
        through a new `cel_map_count(slot) → i32` runtime
        helper that dispatches on kind (arena: inline header
        read; host: calls `cel_host.cel_map_size` and unboxes).
      - **Slice 2 (lists):** New runtime helper
        `cel_list_arena_view(slot) → slot` returns the input
        for CEL_LIST_ARENA, or for CEL_LIST_HOST calls a new
        trampoline `cel_host.cel_list_iter_open` that snapshots
        the HostListBacking into an ArenaListHeader + N×24-byte
        run and returns a workspace slot holding a synthetic
        CEL_LIST_ARENA CelValue.  The codegen prologue calls
        `cel_list_arena_view` once at entry and stores the
        result in a per-comp wasm local (`aux0_local + 2`,
        bumping `comprehension_extra_locals_per_comp` from 2 to
        3); pre-sizing and the three inline header loads read
        through that local.  The existing inline arena pointer
        walk works unchanged for both arena passthrough and
        host-snapshotted sources.
    Affected SKIPs flipped: `m5b_test.cc::ExistsOverBoundList`,
    `AllOverBoundList`, `MapOverBoundList`,
    `MapLargeListGrowthPath` (all green).  Plus every
    activation-bound list/map and proto repeated/map field
    comprehension that previously failed silently is now
    correct.  See `compiler_v2/tools/cel/
    activation_matrix_test.cc::Bound*Comprehension*` /
    `BoundProtoRepeatedField` / `BoundProtoMapFieldComprehension`
    for the 7 new e2e cases locking the behavior.
  - **Inline `cel.bind` fast path (Slice G.2).**  Ship if
    bench shows >20% win on cel.bind-heavy programs.
  - **Remove `IsShapeC` / `LowerShapeC` cel.bind escape hatch
    once the generic loop_step path handles `kLocal` storage**
    (surfaced 2026-05-17, mid-consolidation review).  Slice I's
    commit message framed `IsShapeC` as a ~30% perf win on
    cel.bind-heavy programs (cel-cpp benchmark number), but the
    real reason it exists is a codegen correctness escape, not
    a perf optimisation: cel.bind's `loop_step` references the
    bound value via `kLocal` storage (the value lives in a wasm
    local, not a workspace slot).  The generic
    `EmitCompLoopStep` emits the `loop_step` instructions into
    the wasm body regardless of whether the loop body executes
    at runtime — Binaryen's wasm validator type-checks them
    statically, and the kLocal storage shape fails the
    `StorageKind::kWorkspaceSlot` ABSL_CHECK in the generic
    arm.  Removing `IsShapeC` therefore requires teaching the
    generic loop_step emit to handle kLocal-storage operands.
    Not a small change, and the current escape works.  The perf
    win is real but small in the one-shot-codegen / per-eval
    runtime model (one wasm function call + a count==0
    comparison per cel.bind eval).  Right cleanup is "fix
    generic path, then delete `IsShapeC`" — defer to a future
    codegen refactor.  Note: the new pre-sizing detector
    (`IsPresizableCollectionAccu`, §10.A) DOES handle the
    cel.bind-with-`[]`-value edge case correctly without
    needing IsShapeC, because `LowerComprehension` dispatches
    IsShapeC *before* `EmitCompPrologue` runs (the predicate
    can't fire spuriously).
  - **Arena compaction for under-filled accus** (surfaced
    2026-05-17 mid-consolidation review).  `filter` /
    `transformList`-with-predicate / 4-arg `transformMap` end
    with `count < capacity`; the unused trailing slots sit
    dead in the bump arena until `cel_reset` at next eval.
    For a `filter` that excludes 99% of N=1000 entries, ~24KB
    of arena bytes per eval are wasted.  Move-back compaction
    (decrement arena bump_ptr by `(capacity - count) * 24`
    after the loop) would reclaim them, BUT requires proving
    no other allocations happened during the comprehension
    body — true for simple cases, false for any nested call
    that allocated a string / intermediate.  Not worth the
    safety analysis for transient waste freed at the next
    `cel_reset` (microseconds later).  Trigger to revisit:
    measured arena pressure in production workloads or a
    long-lived comprehension scenario.
  - **Compact-at-end for dynamic lists.**  If R3 bites,
    ship a compaction pass.
  - **Streaming comprehension** (very long-term, post-M-ext):
    if a customer brings a streaming use case, evaluate.
  - **`max_iter` flag** if customer demand surfaces.
  - **Comprehension-aware PartialEval**: track unknown
    accumulators through iterations; unblock the deferred
    `unknowns:` ExprValue binding case in
    `binding_marshal.cc`.
  - **Public-namespace consistency — collapse `celwasm::api`
    aliases (REQUIRED — defer to milestone closeout or
    immediately after).**  Surfaced 2026-05-17 mid-Slice-I.
    Phase X (commit ecf355d) moved the four classes that
    previously lived directly under `namespace cel` (`Value`,
    `Program`, `Activation`, `Compiler`) into
    `namespace celwasm::api` to break a duplicate-symbol
    collision with cel-cpp's same-named classes (`cel::Value`,
    `cel::Compiler`, …) that became visible the moment
    `compiler_v2/frontend/parse_and_check.cc` started linking
    against `@cel-cpp//extensions:comprehensions_v2` (which
    transitively pulls in `@cel-cpp//common:value`).  The
    minimum-disruption fix was to add backward-compat
    `using ::celwasm::api::Value;` (etc.) inside
    `namespace cel`, so every existing caller — tests, the
    CLI, internal pipeline code — could keep writing
    `cel::Value`, `cel::Activation`, … unchanged.  That
    shim is now load-bearing for ~40 source files but
    has two costs: (a) the public API namespace is misleading
    — a reader looking at `compiler_v2/api/value.h` sees
    `namespace celwasm::api { class Value … }` but every
    call site writes `cel::Value`, so there's a permanent
    cognitive bridge to cross; and (b) when we eventually
    expose another type that collides with cel-cpp (very
    likely as we adopt more cel-cpp surface), the same
    workaround has to be re-applied per type rather than
    being structurally impossible.  The clean shape is to
    rename `namespace cel { … }` declarations in
    `compiler_v2/api/`, `compiler_v2/cli/`, `compiler_v2/e2e/`,
    `compiler_v2/internal/`, `compiler_v2/tools/` to either
    `namespace celwasm` (drop the `api` segment — short, no
    risk of further cel-cpp collisions since cel-cpp doesn't
    use this namespace) or commit to `namespace celwasm::api`
    everywhere.  The bulk operation is mechanical
    (`s/cel::Value/celwasm::Value/g` across non-third-party
    files, then drop the `using` shim) but touches every test
    file and the CLI — best done as a single atomic commit
    once the m5b test surface settles, so the rename diff
    isn't entangled with feature work.  **Scoping inventory
    (subagent run 2026-05-17)**: 35 files declare
    `namespace cel { … }`, all isolated to the public-API
    layer.  Breakdown:
      - 8 API headers (`compiler_v2/api/{value,program,
        activation,compiler}.h`) contain the load-bearing
        `using ::celwasm::api::X;` aliases — the rename's
        primary action is deleting these EOF blocks.
      - 4 additional API headers (`error.h`, `attribute.h`,
        `type.h`, `engine.h`) declare `ErrorPayload`,
        `Attribute*`, `CelType`, `Engine` *directly* in
        `namespace cel` — these don't collide with cel-cpp
        today and could either be left alone or moved
        wholesale; the cleaner shape is to move them under
        `namespace celwasm` too so the public surface is
        uniform.
      - 8 API impl + test files (`value_test.cc`,
        `program_test.cc`, `compiler_test.cc`,
        `activation_test.cc`, `error_test.cc`,
        `type_test.cc`, `attribute_test.cc`, `engine_test.cc`,
        plus `instance_test.cc`) open `namespace cel { … }`
        for the test fixtures — rewrite to
        `namespace celwasm`.
      - 11 e2e test files (`m{2,4,5,5b,7a,7b,8,9,10}_test.cc`,
        `optimize_test.cc`, `program_roundtrip_test.cc`) and
        2 benchmark files open `namespace cel` for harness
        scaffolding — same rewrite.
      - **Zero `using namespace cel;` lines exist** anywhere
        in `compiler_v2/`; every reference is qualified
        (`cel::Value`, `cel::Activation`, …), so the rename
        is a mechanical `s/cel::/celwasm::/g` over the four
        target classes plus the namespace-open lines.
      - **No published `.h` aggregate or CLI public contract**
        — `compiler_v2/cli/celwasmc_v2.cc` is a standalone
        demo; nothing under `compiler_v2/` is exposed as a
        shared library / SDK header.  So the rename has
        zero external blast radius.
    **Triggers**: any future class that collides with
    cel-cpp; any contributor confusion when first reading
    `compiler_v2/api/`; or simply doing it once Slice J
    ships and the diff has no feature-work to fight with.
    Estimated effort: one focused agent session, ~35 files
    touched, single atomic commit.
  - **`TransformMapKeyCollisionLastWriteWins` is malformed
    against the actual cel-cpp `transformMap` semantics
    (surfaced 2026-05-17, mid-Slice-G).**  The test as
    written invokes
    `{"a":1,"b":2,"c":3}.transformMap(k, v, "x", v).size()`
    — the speculative reading was that the 4-arg
    `transformMap(k, v, p, t)` lets `p` (or `t`) re-map the
    key, so a constant slot `"x"` would collapse all entries
    to a single bucket and force the collision-overwrite
    semantics.  cel-cpp's actual signature is `(k, v, p, t)`
    where `p` is a `bool` predicate (filter) and `t` is the
    value transform; the key is **never** remapped by
    `transformMap`.  Key remapping is the contract of
    `transformMapEntry` (Slice H, where the loop_step
    inserts the entire `{k': t}` map and the user controls
    `k'`).  Net: this test belongs in Slice H's
    `transformMapEntry` test fixture, written as e.g.
    `{"a":1,"b":2,"c":3}.transformMapEntry(k, v,
    {"x": v}).size()` — every iter inserts `{"x":v}` into
    the accu, the third write wins, and result size is `1`.
    Action: SKIP the test in Slice G with a note pointing
    at this bullet; rewrite it under Slice H once
    `cel.@mapInsert` codegen lands and re-include it in
    `ComprehensionTransformMapEntryE2ETest`.  Lock the
    collision-contract assertion either way — last-write
    wins on identical key, runtime helper
    `cel_map_insert_at` already enforces this and
    `cel_map_test.cc::MapInsertCollision` covers the unit
    side.
  - **`x.y` shorthand for map field access** (surfaced
    2026-05-17 during Slice I e2e).  Currently every kSelect
    lowers to `cel_host.cel_get_field`, the proto-message read
    path.  For a map-typed operand (e.g. `cel.bind(x, {"y": 0},
    x.y == 0)`) the host trampoline expects a message backing
    and poisons with TYPE_MISMATCH.  Workaround in
    `CelBindE2ETest.BindWithMapAccu`: use the explicit
    `x["y"]` index form, which routes through `cel_map_lookup`
    correctly.  Per langdef §"Field Selection": for map
    operands `m.f` is equivalent to `m["f"]` when `"f"` is a
    legal identifier-shaped key — codegen should dispatch on
    `kSelect.operand`'s annotated repr (`kMap` → map lookup;
    otherwise → proto field read).  Tracked separately from
    comprehensions; lands when host-side `kSelect` over map
    operands is implemented (`m5-kcall-comprehensions.md`
    follow-up or a dedicated kSelect-dispatch slice).

### 10.A Pre-sized list AND map accumulators — SHIPPED 2026-05-17

**Status: shipped in this milestone** (post-Slice-G consolidation
commit).  Original plan deferred this to closeout (or a follow-on
milestone); user-driven mid-Slice-G review redirected to ship now
so the runtime API would land in its final shape rather than
churning twice.

**As shipped:**

  - Codegen (`compiler_v2/codegen/expr_lower.cc`):
    `ResolveCompContext` returns a plain `CompContext` (no flags
    carried).  `EmitCompPrologue` inlines a
    `IsPresizableCollectionAccu(comp, init_ann)` predicate at the
    consumer site; when matched, calls `EmitPresizeAccu` which
    emits `cel_list_create` (list accu) or `cel_map_create` (map
    accu) with `capacity = iter_range.count` loaded at runtime
    from the source's arena header (offset 0 of header pointed
    to by source slot's payload offset 8 — uniform layout for
    both arena-list and arena-map).  Shape-C (cel.bind) is
    dispatched in `LowerComprehension` BEFORE `EmitCompPrologue`
    runs, so the predicate cannot fire spuriously for a bind
    whose `value` happens to be `[]`.
  - Runtime (`compiler_v2/runtime/cel_runtime.c`):
    `cel_list_create` (collapsed; see §3.6) and `cel_map_create`
    (unchanged signature, count=0 at create) are the universal
    creates.  `cel_list_append_at` and `cel_map_insert_at` use
    PRESIZE_INVARIANT: `if (count >= capacity) __builtin_trap()`
    — a codegen regression that drops pre-sizing surfaces as a
    wasm trap at the call site, not silent memory corruption.
    The geometric-growth branches that previously lived inside
    both helpers are deleted.
  - List API consolidation: see §3.6.  `cel_list_set` and
    `cel_list_create_with_capacity` (the latter introduced
    mid-Slice-G then immediately retired) are gone.

**What this means in steady state:**
  - Every list-producing comprehension allocates exactly
    `iter_range.count * 24` bytes for elements once, never
    grows.  `map`: count == capacity at end.  `filter` /
    `transformList`-with-predicate: count ≤ capacity; the
    unused trailing slots sit dead in the bump arena until
    `cel_reset` at next eval.
  - Every map-producing comprehension allocates exactly
    `iter_range.count * kCelMapEntryStride` bytes for entries.
    `transformMap` 3-arg: count == capacity unless predicate
    filters; 4-arg: count ≤ capacity by the predicate.
    `transformMapEntry` single-entry-literal: same bound as
    transformMap; multi-entry literal would exceed and trap (see
    "Out of scope" below — Slice H ships single-entry pattern
    only).
  - The runtime ABI is now stable: literals and accus share
    creators and writers, so the next codegen feature (e.g.
    streaming) drops into the same primitives.

**The observation that triggered this** (review note, 2026-05-17):

**Status**: deferred to milestone closeout (Slice J or after).
Discussed mid-Slice-D 2026-05-17; chosen to ship dynamic-growth
first so the e2e test matrix could be validated end-to-end before
touching the runtime allocator path again.

**The observation that triggered this** (review note, 2026-05-17):
the result size of any list-producing comprehension is bounded
above by `iter_range.count`:

  - `xs.map(v, t)`: result size = `xs.size()` exactly (every
    iter contributes one element).
  - `xs.filter(v, p)`: result size ≤ `xs.size()` (some iters
    skipped).
  - `xs.transformList(i, v, t)` / `xs.transformList(i, v, p, t)`:
    same bounds as `map` / `filter` respectively.

The same bound applies to **map-producing** comprehensions
(observed 2026-05-17, Slice G review):
  - `m.transformMap(k, v, t)`: result size ≤ `m.size()`.
    Exactly `m.size()` when every key is unique post-transform
    (the 3-arg form never changes keys, so equality is the
    same as for the source); strictly less only when the
    4-arg form's predicate filters iters out.
  - `m.transformMap(k, v, p, t)`: result size ≤ `m.size()`
    (filtered).
  - `m.transformMapEntry(k, v, entry)`: result size ≤
    `m.size() * max_entry_count`.  For the by-far-dominant
    single-entry-literal case (`{k': t}`), each iter inserts
    exactly one entry, so the bound collapses to ≤ `m.size()`
    — the same as `transformMap`.  General-case bound is the
    sum of per-iter entry counts; the codegen can choose
    between (a) pre-sizing to `m.size()` for the dominant
    pattern + falling back to growth if the entry literal
    contains more than one key, or (b) detecting at codegen
    time that the entry literal is a single-key map and
    pre-sizing to `m.size()` unconditionally for that shape.
  - `xs.transformMap{,Entry}` (list source): bounded by
    `xs.size()` (or `xs.size() * 1` for the single-entry
    case) — symmetric to the map-source case above.

So **we never need to grow beyond the source size in any
list- or map-producing comprehension built from the
standard / v2 macros.**  Pre-allocating capacity
= `iter_range.count` at accu_init time would:
  - Eliminate the growth + copy path entirely (zero waste on
    `map`, ≤50% over-allocation on `filter` in the worst case).
  - Save arena bytes the geometric-growth path abandons in the
    forward-only bump arena.
  - Simplify the runtime helper (no growth branch).

**What we shipped instead (Slice D, 2026-05-17)**: full dynamic
growth.  `cel_list_append_at` allocates a fresh elements run at
2× capacity (min 4) when full, copies existing entries, abandons
the old run.  Amortises to O(N) total work for N appends; about
50% arena waste during growth.

**Why we shipped dynamic growth first**:
  1. Correctness-equivalent.  Pre-sizing is a perf / memory
     optimisation, not a behavioural difference.
  2. The runtime helper is general — it handles any list-grow
     scenario, not just comprehension accumulators.  A future
     `cel.list.append` user-facing function (none currently
     planned) would reuse the same helper.
  3. The codegen for the pre-sized variant requires
     intercepting `accu_init` — instead of evaluating the
     `kCreateList(size=0)` accu_init expression, we'd need to
     emit a special "create with capacity N" call where N is the
     iter_range's count loaded at runtime.  That's a comp-form-
     specific override in `LowerComprehension`, adding ~30 LoC
     and a per-form branch.  Worth it; not blocking the e2e
     unlock.
  4. Letting the dynamic-growth runtime path exist in tree means
     future codegen experiments (e.g. a `cel.list.append`
     primitive, or a streaming comprehension prototype) have
     the runtime ready.  The pre-sized path becomes a fast-path
     codegen choice on top of an existing general helper.

**The plan doc's original Slice D justification** (lines
510-514) read: *"Alternative considered and rejected:
pre-allocate `cel_list_size(iter_range)` capacity at `accu_init`
time.  …The codegen would need to specialise per-form, which
complicates the lowering arm.  Not worth it for the 24 KB
best-case savings."*  The "not worth it" was wrong.  Per-form
codegen specialisation already exists (the append-shape pattern
detector for `map` / `filter` loop_step).  Pre-sizing is the
matching prologue specialisation.

**How to implement** (work for the follow-up):

  1. Add `cel_list_create_with_capacity(uint32_t slot,
     uint32_t capacity)` to `cel_list.{h,c}`.  Body: allocate
     an `ArenaListHeader` with `count=0`, `capacity=capacity`,
     `elements_offset = cel_alloc(capacity * 24)`.  OOM →
     poison with `CEL_ERR_OVERFLOW`.
  2. Export it from `cel_runtime.wasm` (BUILD.bazel
     `--export=` flag) and add to wat_runner's
     `kRuntimeExports`.
  3. In `LowerComprehension`, after `ResolveCompContext`,
     detect the list-accu case (accu_init is
     `kCreateList(size=0)` AND `accu_var`'s Repr is
     `Repr::kList`).  If matched: **skip** the normal
     `Emit(comp.accu_init())` + `cel_copy_slot(accu_slot,
     init_src_slot)` prologue.  Instead emit:
     - Load `iter_range.count` (already loaded into a temp
       during prologue's list_hdr setup — reuse it).
     - Call `cel_list_create_with_capacity(accu_slot, count)`.
  4. Optionally drop the growth branch in
     `cel_list_append_at` — when ResolvePass + LayoutPass can
     prove the accu was pre-sized, codegen could call a thinner
     `cel_list_push_at` helper.  Not strictly needed since
     pre-sized lists never trip the growth branch at runtime.
  5. Bench: `[0..1000].map(v, v*2)` end-to-end; expect arena
     bytes used to drop ~50% and per-iter wasm op count to drop
     by the growth-check overhead.
  6. Re-run all `ComprehensionMapFilterListE2ETest` cases;
     `ComprehensionTwoIterVarE2ETest`'s `transformList` cases
     (Slice F); confirm they pass unchanged.

**Triggers for prioritising this work**:
  - Conformance arena-overflow regressions on
    `[1000 items].map(...)`-shaped rows (the existing
    `MapLargeListGrowthPath` e2e test is the canary).
  - Customer reports of high arena memory under
    comprehension-heavy programs.
  - **Both** `transformMap` (Slice G, shipped 2026-05-17) and
    `transformMapEntry` (Slice H, shipping next in this
    milestone) use map accumulators with the same `count ≤
    source.size()` bound described above.  The map-side
    pre-sizing primitive and the list-side primitive can be
    designed together; doing both in one follow-up commit is
    structurally cleaner than splitting the patch.

**Map-accu version of the implementation sketch** (parallel
to the list-side §10.A.1 above):

  1. Add `cel_map_create_with_capacity(uint32_t slot,
     uint32_t capacity)` to `cel_map.{h,c}`.  Body: allocate
     an `ArenaMapHeader` with `count=0`, `capacity=capacity`,
     `entries_offset = cel_alloc(capacity * kCelMapEntryStride)`.
     OOM → poison with `CEL_ERR_OVERFLOW`.
  2. Export it from `cel_runtime.wasm` and wire it through
     wat_runner / api / compile.cc identically to
     `cel_map_insert_at` (the existing landed helper).
  3. In `LowerComprehension`, after `ResolveCompContext`,
     detect the map-accu case (accu_init is
     `kCreateMap(entries=[])` AND `accu_var`'s Repr is
     `Repr::kMap`).  Replace the normal
     `Emit(comp.accu_init())` + `cel_copy_slot(accu_slot,
     init_src_slot)` prologue with:
     - Load `iter_range.count` (for map source: load the
       map header's `count` field; already loaded into a
       temp during EmitMapPrologue — reuse it.  For list
       source feeding a transformMap-on-list-via-v2-macro:
       load `iter_range`'s list-header count, also already
       loaded by EmitListPrologue).
     - Call `cel_map_create_with_capacity(accu_slot,
       count)`.
  4. Optionally drop the growth branch in
     `cel_map_insert_at` (the new `arena_map_grow` static
     helper) — when ResolvePass + LayoutPass can prove the
     map was pre-sized AND the codegen knows the comp
     inserts exactly one entry per iter (transformMap, NOT
     transformMapEntry-with-variable-entry-size), call a
     thinner `cel_map_push_at`.  Not strictly needed since
     pre-sized maps never trip the growth branch at
     runtime in that case.
  5. Bench symmetric to the list side:
     `{"a":1,"b":2,...1000 entries}.transformMap(k, v, v*2)`
     end-to-end; expect arena bytes used to drop ~50% and
     per-iter wasm op count to drop by the growth-check
     overhead.
  6. Re-run all `ComprehensionTransformMapE2ETest` and
     `ComprehensionTransformMapEntryE2ETest` cases;
     confirm they pass unchanged.

**Out of scope even for the follow-up**:
  - Streaming / lazy comprehensions (separate milestone).
  - Pre-sizing `transformMapEntry` when the per-iter entry
    literal can contain more than one key (general case).
    Defer until perf-critical; the single-key shape is by
    far the dominant pattern and covered above.

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
post-M7B baseline.  Per-slice contributions reflect the
corrected slice plan in §5; full derivation in
`m5-comprehensions-design.md` §10.

| Fixture | Pre | Δ C (list) | Δ D (filter/map) | Δ E (map iter) | Δ F (iter_var2) | Δ G (transformMap) | Δ H (mapEntry) | Δ I (cel.bind) | Post |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `macros.textproto` | 0/44 | +18 | +10 | +5 | — | — | — | — | 33/44 |
| `macros2.textproto` | 0/46 | — | — | — | +28 | +10 | +8 | — | 46/46 |
| `bindings_ext.textproto` | 0/8 | — | — | — | — | — | — | +8 | 8/8 |
| `namespace.textproto` | 4/14 | +6 | — | — | — | — | — | — | 10/14 |
| `fields.textproto` | 26/60 | +1 | +2 | — | — | — | — | — | 29/60 |
| **Corpus total Δ** | **1144** | **+25** | **+12** | **+5** | **+28** | **+10** | **+8** | **+8** | **~1240** |

Full-unlock total: **+96 PASS**, headline ~1240 / 2454.

**Minimum-viable variant** (A + B + C + D + I + J = 4 sessions):
+25 (C) + +12 (D) + +8 (I) ≈ **+45 PASS**, headline ~1189 / 2454.
Defers map iteration, two-iter-var, transformMap, transformMapEntry
to a follow-up.  Ships `cel.bind` and the bulk of single-iter-var
list comprehensions.

(The 9 `compile unimpl` SKIPs `macros.textproto` retains
post-milestone are `dyn(aggregate)` rejections — out-of-scope
per the static-subset gate.  Same for the residual SKIPs in
`namespace` / `fields`.)

## 13 Position in the milestone roadmap

Comprehensions is the last big language-feature gap in the
post-M7B corpus that isn't an extension.  Post-milestone, the
remaining failure modes are: extensions (math, string, network,
optionals), wrapper-typed proto fields (M8), and the long tail
of small unlocks (matches regex, map-type marshalling, bool→int
overloads).  M8 is the next-biggest single unlock (~+55–60
PASS) and the natural sequel.

`cel.bind` (Slice I) ships on top of the core comprehension
infrastructure at ~0.25 session cost for +8 PASS.
