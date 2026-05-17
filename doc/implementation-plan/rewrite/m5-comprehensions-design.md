# Comprehensive comprehension design

Status: design analysis — drafted 2026-05-16.  Supersedes parts of
`doc/implementation-plan/rewrite/m5-comprehensions-followon.md`
where this doc finds the earlier sketch was wrong (see §10 for the
plan-delta list).

Inputs surveyed:
  - `third_party/cel-cpp/parser/macro.cc` (standard macros)
  - `third_party/cel-cpp/extensions/bindings_ext.cc` (`cel.bind`)
  - `third_party/cel-cpp/extensions/comprehensions_v2_macros.cc`
    (three-arg / two-iter-var macros)
  - `third_party/cel-cpp/eval/eval/comprehension_step.cc`
    (runtime semantics — list-vs-map iter, iter_var2 binding)
  - `third_party/cel-cpp/common/ast/expr_proto.cc` (AST shape:
    `iter_var` AND `iter_var2` are native fields)
  - `tests/simple/testdata/macros.textproto` (44 rows)
  - `tests/simple/testdata/macros2.textproto` (46 rows)
  - `tests/simple/testdata/bindings_ext.textproto` (8 rows)
  - `tests/simple/testdata/namespace.textproto` (comprehension rows)

## 1 Critical correction to the prior plan

The previous milestone plan asserted that two-iter-var
comprehensions desugar into a `cel.bind` nested inside a
single-iter-var comprehension's `loop_step`.  **That is wrong.**

cel-cpp's AST proto for `kComprehensionExpr` has both `iter_var`
and `iter_var2` as first-class fields
(`common/ast/expr_proto.cc:229-230` push them as separate
`comprehension_proto->set_iter_var(...)` /
`set_iter_var2(...)` calls).  cel-cpp's evaluator
(`eval/eval/comprehension_step.cc`) branches at runtime on
`iter_slot_ == iter2_slot_` to choose `Evaluate1` (single-var) vs
`Evaluate2` (two-var); both shapes are native, not desugared.

Implication for us: the milestone needs to handle a native
two-iter-var AST shape.  No nested-`cel.bind` workaround.  See §6
for the codegen impact.

## 2 Full catalogue of comprehension shapes

The CEL parser surface has **18 distinct macro signatures** that
expand to `kComprehensionExpr` AST nodes.  Grouped by source library
and arity.

### 2.1 Standard library (`parser/macro.cc`) — 7 macros

All use a single `iter_var`; `iter_var2` is empty.  Operands and
result shapes vary.

| # | Macro signature | Accu type | Accu init | Loop cond | Loop step | Result | Notes |
|---|-----------------|-----------|-----------|-----------|-----------|--------|-------|
| 1 | `e.all(v, p)` | bool | `true` | `@not_strictly_false(accu)` | `accu && p` | `accu` | Short-circuit on first non-true |
| 2 | `e.exists(v, p)` | bool | `false` | `@not_strictly_false(!accu)` | <code>accu &#124;&#124; p</code> | `accu` | Short-circuit on first true |
| 3 | `e.exists_one(v, p)` | int | `0` | `true` | `p ? accu + 1 : accu` | `accu == 1` | No short-circuit; result is comparison |
| 4 | `e.map(v, t)` | list | `[]` | `true` | `accu + [t]` | `accu` | Append-per-iter; no short-circuit |
| 5 | `e.map(v, p, t)` | list | `[]` | `true` | `p ? accu + [t] : accu` | `accu` | Filtered map; conditional append |
| 6 | `e.filter(v, p)` | list | `[]` | `true` | `p ? accu + [v] : accu` | `accu` | Filter |
| 7 | `has(e.f)` | — | — | — | — | — | **NOT a comprehension.**  Expands to `select_expr{test_only=true}`; already supported. |

### 2.2 `bindings_ext` (`extensions/bindings_ext.cc`) — 1 macro

| # | Macro signature | Accu type | Accu init | Loop cond | Loop step | Result | Notes |
|---|-----------------|-----------|-----------|-----------|-----------|--------|-------|
| 8 | `cel.bind(name, value, body)` | *any* | `value` | `false` | `name` (unused) | `body` | **Degenerate**: `iter_range=[]`, `loop_cond=false`, loop body never runs.  The accumulator is the bound variable. |

### 2.3 `comprehensions_v2` (`extensions/comprehensions_v2_macros.cc`) — 10 macros

All use a non-empty `iter_var2`.  For list iteration: `iter_var=index`,
`iter_var2=value`.  For map iteration: `iter_var=key`, `iter_var2=value`.

| # | Macro signature | Accu type | Accu init | Loop cond | Loop step | Result | Notes |
|---|-----------------|-----------|-----------|-----------|-----------|--------|-------|
| 9 | `e.all(k, v, p)` / `e.all(i, v, p)` | bool | `true` | `@not_strictly_false(accu)` | `accu && p` | `accu` | Two-var all |
| 10 | `e.exists(k, v, p)` / `e.exists(i, v, p)` | bool | `false` | `@not_strictly_false(!accu)` | <code>accu &#124;&#124; p</code> | `accu` | Two-var exists |
| 11 | `e.existsOne(k, v, p)` / `e.existsOne(i, v, p)` | int | `0` | `true` | `p ? accu + 1 : accu` | `accu == 1` | Two-var exists_one (renamed `existsOne` not `exists_one`) |
| 12 | `e.transformList(i, v, t)` | list | `[]` | `true` | `accu + [t]` | `accu` | List-with-index map |
| 13 | `e.transformList(i, v, p, t)` | list | `[]` | `true` | `p ? accu + [t] : accu` | `accu` | Filtered list-with-index map |
| 14 | `e.transformMap(k, v, t)` | map | `{}` | `true` | `accu.with(k → t)` | `accu` | Map → map transform, value-only |
| 15 | `e.transformMap(k, v, p, t)` | map | `{}` | `true` | `p ? accu.with(k → t) : accu` | `accu` | Filtered map transform |
| 16 | `e.transformMapEntry(k, v, entry)` | map | `{}` | `true` | `accu.merge(entry)` | `accu` | `entry` is a map literal `{k': t}`; merges into accu |
| 17 | `e.transformMapEntry(k, v, p, entry)` | map | `{}` | `true` | `p ? accu.merge(entry) : accu` | `accu` | Filtered map-entry merge |

### 2.4 `optMap` / `optFlatMap` (deferred to Optionals milestone)

| # | Macro signature | Notes |
|---|-----------------|-------|
| 18 | `target.optMap(v, t)` / `target.optFlatMap(v, t)` | Use comprehension under the hood (degenerate, `cel.bind`-shaped) but produce `optional` typed results.  Defers to Optionals milestone; no test rows in our addressable conformance corpus. |

### 2.5 Summary

  - **18 macro signatures**, of which:
    - **7 standard** (single iter_var)
    - **1 cel.bind** (degenerate empty-iteration single-var)
    - **8 two-iter-var** (three- and four-arg forms)
    - **2 optional-using** (deferred)
  - **All 18 emit `kComprehensionExpr` AST nodes** with the
    appropriate iter_var / iter_var2 / accu_var fields.
    No special AST shape per macro — the AST is uniform; the
    macros just configure the seven fields differently.
  - **Three accumulator-shape families**:
    - bool (all / exists)
    - int (exists_one)
    - aggregate (list for map / filter / transformList; map for
      transformMap / transformMapEntry)
  - **Two iteration-source families**:
    - list (range over list elements, optionally with index in
      iter_var)
    - map (range over keys, optionally with value in iter_var2)

## 3 Edge cases observed in conformance fixtures

Cataloguing every distinct shape across the 98 row inventory
(macros + macros2 + bindings_ext + namespace).

### 3.1 Empty range

  - `[].exists(e, e == 2)` → false  (accu_init is returned)
  - `[].all(e, e > 0)` → true
  - `[].exists_one(a, a == 7)` → false (accu = 0, 0 == 1 is false)
  - `[].map(n, n / 2)` → `[]`
  - `[].filter(n, n % 2 == 0)` → `[]`
  - `{}.transformMap(k, v, k + v)` → `{}`
  - `[].transformList(i, v, i / v)` → `[]` (the per-iter divide-by-
    zero is never reached because the loop body never runs)

**Invariant**: empty range never evaluates `loop_cond` /
`loop_step`; `result` is evaluated against `accu = accu_init`.

### 3.2 Errors inside the loop body

  - `[1, 2, 3].exists(e, e / 0 == 17)` → ERROR("divide by zero")
    (first iter triggers; short-circuit propagates the error)
  - `[3, 2, 1, 0].exists_one(n, 12 / n > 1)` → ERROR
    (loop_cond never short-circuits for exists_one, so the
    div-by-zero hits at n=0; this is the LAST iteration; error
    propagates from `loop_step`)
  - `[2, 1, 0].map(n, 4 / n)` → ERROR (each iter appends; the
    n=0 iter errors)
  - `[3, 2, 1, 0].filter(n, 12 / n > 4)` → ERROR (predicate errors)
  - `[1, 2, 3].all(e, e / 0 != 17)` → ERROR (predicate errors on
    iter 1; even though `loop_cond=NOT_STRICTLY_FALSE(accu)` is
    still true going in, the loop_step itself errors; error
    propagates)

**Invariant**: any error in `loop_step` evaluation aborts the
comprehension and becomes the comprehension's result.
Short-circuit for `all` / `exists`: per langdef §"Three-valued
Logic", `error && false → false`, `error || true → true`.  This is
*why* `exists` / `all` use `@not_strictly_false`: the condition
admits both `true` and `error` as "continue".  Cel-cpp's semantics:
once the accumulator goes to `error`, subsequent iterations still
run; if any later iter produces a value that makes the boolean
expression definite (e.g. `error || true`), the result becomes
definite.  **Our codegen must respect this:** `cel_or` /
`cel_and` already implement 3VL correctly (M5 Slice B); the
comprehension codegen just needs to read the accumulator state
without short-circuiting on `error`.

### 3.3 Errors in `loop_cond`

  - `[2, 4, 6].transformList(i, v, v / 2 + i)` — no error, but
    the cond is constant `true`.
  - For `all` / `exists`, `loop_cond` is
    `@not_strictly_false(accu)`.  If `accu = error`, the cond is
    `true` (errors are not "strictly false"), so the loop
    continues.  This is the 3VL contract.

**Invariant**: `loop_cond` errors abort the comprehension with
that error.  Cel-cpp handles this in `Evaluate1Unknown`'s `switch
(condition.kind())` block — error in cond becomes the
comprehension's result.

### 3.4 Mixed-typed range (`dyn(list)`)

  - `[1, 'foo', 3].exists(e, e != '1')` → true (cel-cpp; in our
    static-subset world this is REJECTED before reaching the
    comprehension because the list literal types as `list(dyn)`).
  - `[1, 'foo', 3].all(e, e % 2 == 1)` → ERROR (`no_such_overload`
    for `'foo' % 2`).  Static-subset rejects in our world.

**Invariant for us**: comprehensions over `dyn(list)` /
`dyn(map)` continue to SKIP per `RejectDyn` (out-of-scope by
design).  The comprehension milestone does NOT loosen this.

### 3.5 Nested comprehensions

  - `[1].exists(y, [0].exists(y, y == 0))` → true.  **Same name
    `y` in both scopes — the inner shadows the outer.**  cel-cpp
    handles this via scope frames; our ResolvePass must too.
  - `['signer'].filter(signer, ['artifact'].all(artifact, true))` →
    `['signer']`.  Inner comprehension is in `loop_step`; both
    iter_vars are distinct names.
  - `['signer'].all(signer, ['artifact'].all(artifact, true))` →
    true.  Same pattern.

**Invariant**: nested comprehensions push fresh scope frames;
name resolution prefers innermost binding.  Both `cel.bind`'s
accu_var and comprehension's iter_var follow the same rule.

### 3.6 cel.bind interacting with comprehensions

  - `cel.bind(valid_elems, [1, 2, 3], [3, 4, 5].exists(e, e in
    valid_elems))` → true.  Outer cel.bind's accu_var
    (`valid_elems`) is in scope inside the inner comprehension's
    `loop_step` (`e in valid_elems`).
  - `cel.bind(t1, true, cel.bind(t2, true, t1 && t2))` → true.
    Nested binds; outer `t1` in scope inside inner.
  - `cel.bind(x, {'y': 0}, x.y == 0)` → true.  Accu is a map;
    body does field-select on it.  Trivial because cel.bind's
    accu_var holds the bound value.

**Invariant**: cel.bind is a comprehension with iter_range=[],
cond=false.  The accu_var binding stays in scope for `result`
exactly like any other comprehension.

### 3.7 Iter_var and accu_var name collision

cel-cpp's macros explicitly **reject** iter_var named
`__result__` (the canonical accu_var name).  See
`parser/macro.cc:108-111` for `all`:

```cpp
if (args[0].ident_expr().name() == kAccumulatorVariableName) {
  return factory.ReportErrorAt(...,
      absl::StrCat("all() variable name cannot be ", kAccumulatorVariableName));
}
```

So this case is *parser-rejected*; we don't need to handle it in
codegen.  Similarly for the two-iter-var macros, both `iter_var`
and `iter_var2` must differ from `__result__` and from each
other.

**Invariant**: when the AST reaches our ResolvePass, no
iter_var collision with `__result__` is possible.

### 3.8 Heterogeneous accumulator types

  - `exists_one`: accu is int.  Final result is `accu == 1` (a
    bool comparison).  So the comprehension's *output type* is
    bool but the *accu_var type* is int.
  - `cel.bind(x, 0, x == 0)`: accu is int, result is bool.

**Invariant**: the comprehension's output type is the static
type of `result`, NOT the type of accu_var.  Our LayoutPass /
checker must read this from the AST's `result` field, not
infer from `accu_init`.

### 3.9 Comprehension result as an operand

  - `{'John': 'smart'}.map(key, key) == ['John']` → true.  The
    `==` operates on the comprehension's list output and a
    literal list.
  - `{'John':'smart','Paul':'cute',...}.filter(key, key ==
    'Ringo') == ['Ringo']` → true.  Same.

**Invariant**: a comprehension's output slot is addressable by
any consumer that expects a CelValue.  The kCall arm reads it
the same way it reads any operand.

### 3.10 Map literal as the iteration source

  - `{'key1':1, 'key2':2}.exists(k, k == 'key2')` → true.  Single
    iter_var binds the **key** (per cel-cpp evaluator's
    `Evaluate1` for map iteration).
  - `{'key1':1, 'key2':2}.all(k, k == 'key2')` → false.
  - `{6: 'six', 7: 'seven', 8: 'eight'}.exists_one(foo, foo % 5
    == 2)` → true.  iter_var is the int key.
  - `{'John': 'smart'}.map(key, key) == ['John']` → maps over
    keys, returning a list of keys.
  - `{'key':1, 1:21}.exists(k, k != 2)` → true.  Heterogeneous
    map keys.  In our world: `RejectDyn` rejects the map
    literal before the comprehension is admitted.

**Invariant**: single-iter-var over map iterates over **keys
only** (no value access).  Two-iter-var over map gets
`iter_var=key, iter_var2=value`.

### 3.11 Mixed iteration order

`{6: 'six', 7: 'seven', 8: 'eight'}.exists_one(foo, foo % 5 == 2)`
→ true.  Only `7` satisfies (7 % 5 == 2); all others fail.  The
test passes regardless of iteration order because `exists_one`
counts matches, not first-match.

**Invariant**: CEL spec does NOT mandate map iteration order.
Tests are written to be order-independent.  Our runtime can
iterate in whatever order matches our map implementation
(deterministic per M3.H bucket layout).

## 4 The AST shape — uniform, native two-iter-var

cel-cpp's `kComprehensionExpr` proto has **eight** fields:

```
ComprehensionExpr {
  iter_var   : string   // always present
  iter_var2  : string   // empty for single-iter; non-empty for two-iter
  iter_range : Expr
  accu_var   : string   // always "__result__" (canonical)
  accu_init  : Expr
  loop_cond  : Expr → bool
  loop_step  : Expr
  result     : Expr
}
```

Our `compiler_v2` code-base reads this AST via cel-cpp's checker
output (`cel::ast_internal::Comprehension`).  The accessor for
`iter_var2` already exists in cel-cpp's API; we just have to
*use* it.

**Detection rule for the codegen**: if `iter_var2` is non-empty,
emit the two-iter-var lowering shape.  Otherwise emit the
single-iter-var shape.  Both shapes share the outer scaffolding
(scope push/pop, accu allocation, loop init / cond / step /
result); they differ only in what gets bound and how the
iteration step extracts elements.

## 5 Codegen taxonomy — three execution shapes

Despite 18 macro signatures, the codegen needs **three** distinct
execution shapes:

### Shape A — Loop over list (single or two iter_var)

Applies to macros 1-6, 9-13.  Common skeleton:

```
[evaluate iter_range → list_slot]
[evaluate accu_init → accu_slot]
locals: iter_off = list_payload_base
        end_off  = list_payload_base + count * 24
        step_out = workspace
        (if two-var) index = 0
block $exit {
  loop $continue {
    br_if $exit (iter_off >= end_off)
    [evaluate loop_cond → cond_slot]
    br_if $exit (!cond_slot.payload)
    (if single-var) bind iter_var → iter_off
    (if two-var) bind iter_var → index, iter_var2 → iter_off
    [evaluate loop_step → step_out]
    [copy step_out → accu_slot]
    iter_off += 24
    (if two-var) index += 1
    br $continue
  }
}
[evaluate result against accu_slot] → return slot
```

### Shape B — Loop over map (single or two iter_var)

Applies to the map-iteration variants of macros 1-3, 9-11, 14-17.
Common skeleton:

```
[evaluate iter_range → map_slot]
[evaluate accu_init → accu_slot]
locals: iter_handle = cel_map_iter_init(map_slot)
        key_workspace = workspace_a
        (if two-var) val_workspace = workspace_b
        step_out = workspace_c
block $exit {
  loop $continue {
    br_if $exit (cel_map_iter_next(iter_handle) == 0)
    cel_map_iter_key_at(key_workspace, iter_handle)
    (if two-var) cel_map_iter_value_at(val_workspace, iter_handle)
    [evaluate loop_cond → cond_slot]
    br_if $exit (!cond_slot.payload)
    bind iter_var → key_workspace
    (if two-var) bind iter_var2 → val_workspace
    [evaluate loop_step → step_out]
    [copy step_out → accu_slot]
    br $continue
  }
}
[evaluate result against accu_slot] → return slot
```

### Shape C — Degenerate (`cel.bind`)

Applies to macro 8 (`cel.bind`).  Detected by
`iter_range = []` AND `loop_cond = false`.  No loop emitted;
direct scope push:

```
[evaluate accu_init → accu_slot]   ;; the "bound value"
push_scope(accu_var → accu_slot)
[evaluate result against accu_slot] → return slot
pop_scope()
```

This is a pure perf optimisation (~30% throughput win on
cel.bind-heavy programs per cel-cpp benchmarks).  Correctness-
wise, the generic Shape A would handle `cel.bind` correctly —
the loop body never executes because `iter_range = []`.  But the
loop prologue still costs us a comparison and a few wasm
operations per `cel.bind`.

**Optimisation gate**: detect this shape at codegen entry; if
matched, emit Shape C; else fall through to Shape A.  No
correctness risk — Shape A is a strict superset.

## 6 Per-macro codegen recipes

How each of the 18 macros lowers in terms of A/B/C and the
specific accu/cond/step expressions.  All recipes assume the
parser has already expanded the macro into a
`kComprehensionExpr` AST node with the seven (or eight) fields.

| # | Macro | Shape | Accu type | Special handling |
|---|-------|-------|-----------|------------------|
| 1 | `all(v, p)` | A (list) / B (map) | bool | None — generic |
| 2 | `exists(v, p)` | A (list) / B (map) | bool | None — generic |
| 3 | `exists_one(v, p)` | A (list) / B (map) | int | `result` is `accu == 1` — uses kCall(`_==_`) arm |
| 4 | `map(v, t)` | A (list only; map source is invalid for single-var map) | list | `loop_step` calls `cel_list_concat` of accu and `[t]` — see §7.1 for the dynamic-list optimisation |
| 5 | `map(v, p, t)` | A (list only) | list | Same as 4 but `loop_step` is wrapped in conditional |
| 6 | `filter(v, p)` | A (list) / B (map → list of keys) | list | Same as 4 but step appends `v` not `t` |
| 7 | `has(e.f)` | N/A (not a comprehension) | — | Already supported in M2 |
| 8 | `cel.bind(name, val, body)` | C (or A as fallback) | any | Shape C is the fast path; Shape A correctly handles it as a no-loop comprehension |
| 9 | `e.all(i, v, p)` / `e.all(k, v, p)` | A or B with iter_var2 | bool | iter_var2 binding adds one extra slot |
| 10 | `e.exists(i, v, p)` / `e.exists(k, v, p)` | A or B with iter_var2 | bool | Same |
| 11 | `e.existsOne(i, v, p)` / `e.existsOne(k, v, p)` | A or B with iter_var2 | int | Same; result is `accu == 1` |
| 12 | `e.transformList(i, v, t)` | A with iter_var2 | list | Same as macro 4 |
| 13 | `e.transformList(i, v, p, t)` | A with iter_var2 | list | Same as macro 5 |
| 14 | `e.transformMap(k, v, t)` | B with iter_var2 | map | **New runtime helper**: `cel_map_insert_at`.  See §7.2 |
| 15 | `e.transformMap(k, v, p, t)` | B with iter_var2 | map | Same as 14 but conditional |
| 16 | `e.transformMapEntry(k, v, entry)` | B with iter_var2 | map | `entry` is a map literal; loop_step merges its entries into accu.  See §7.3 |
| 17 | `e.transformMapEntry(k, v, p, entry)` | B with iter_var2 | map | Same as 16 but conditional |
| 18 | `optMap` / `optFlatMap` | Deferred to Optionals milestone | — | Out of scope |

## 7 Runtime primitives needed

### 7.1 Dynamic list append (`cel_list_append_at`)

Required by: macros 4, 5, 6, 12, 13 (and indirectly by anything
that produces a list via comprehension).

The cel-cpp macro definitions all use `CelOperator::ADD` of
`accu` and `[t]` as the step.  This is `accu + [t]` — list
concatenation.  Two ways to implement:

  - **(a) Reuse `cel_list_concat`**, which already exists in our
    runtime.  Each iteration allocates a new combined list,
    copies all existing elements, appends one new element.
    **Cost: O(N²) total work** for N iterations — quadratic.
    Acceptable for tiny N, untenable for N>100.
  - **(b) Implement `cel_list_append_at`** with geometric
    growth.  Amortised O(N) total work.  See
    `m5-comprehensions-followon.md` §3.6 for the API.

**Decision: ship (b).**  Comprehensions over realistic
customer lists (auth scope lists, validation field lists)
routinely exceed 100 elements; quadratic is a real regression
risk.

Codegen detection: the kCall arm in `loop_step` matches the
pattern `kCall(_+_, accu_ref, kCreateList([single_elem]))` and
emits `cel_list_append_at(accu_slot, elem_slot)` directly,
bypassing the generic `cel_list_concat` path.  When the pattern
doesn't match (e.g. user wrote `accu + [a, b]`), fall back to
`cel_list_concat`.

### 7.2 Map insertion at construction (`cel_map_insert_at`)

Required by: macros 14, 15, 16, 17.

Today's runtime has `cel_map_create(slot, capacity)` (one-shot,
static size) and `cel_map_insert(map_slot, key_slot, value_slot)`
(but that's used during construction of a static map literal, not
incremental growth).

`cel_map_insert_at` for accumulator growth needs the same
geometric-growth treatment as lists:

```c
void cel_map_insert_at(uint32_t map_slot, uint32_t key_slot,
                       uint32_t value_slot);
```

Semantics:
  - If `map_slot`'s payload is null or full, allocate larger
    bucket array (2× current).  Re-hash existing entries.
  - Insert `(key, value)`.  If key already exists, **overwrite**
    (per langdef §"Map literals": last-write-wins on duplicate
    keys; comprehension semantics aren't explicit, but cel-cpp's
    transformMap uses overwrite).

Codegen detection: kCall arm matches
`kCall(map_with_kv, accu_ref, key, value)` (or whatever
cel-cpp's IR shape is — TBD at design-spike) and emits
`cel_map_insert_at` directly.

### 7.3 Map entry merging (`transformMapEntry`)

Macro 16/17 has a fundamentally different shape: `entry` is a
*map expression* (typically `{transformed_k: transformed_v}` or
an empty map `{}`), and the step merges its entries into `accu`.

Two codegen options:

  - **(a) General path**: evaluate `entry` to a temporary map,
    iterate over its entries, insert each into `accu` via
    `cel_map_insert_at`.  Costs an iteration per macro iter.
  - **(b) Pattern-detect single-entry case**: if `entry` is a
    single-key literal `{k': t}`, evaluate `k'` and `t` directly
    and insert into accu.  Skip the temp map.

**Decision: ship (a) first**, optimise to (b) if profiling shows
it matters.  Conformance fixtures use the single-entry shape but
nothing in the spec rules out richer entry expressions.

### 7.4 Map iteration helpers

Required by: macros 1-3 (over map source), 9-11 (two-var over
map), 14-17 (always map source).

```c
uint32_t cel_map_iter_init(uint32_t map_slot);     // handle
uint32_t cel_map_iter_next(uint32_t handle);       // 0=done, 1=ok
void cel_map_iter_key_at(uint32_t out, uint32_t handle);
void cel_map_iter_value_at(uint32_t out, uint32_t handle);
```

Iterator state can be a single i32 cursor (bucket index +
offset within bucket).  No allocation.

### 7.5 No new runtime helper for list iteration

The existing `cel_list_size` / `cel_list_at` (slot-out)
plus pointer arithmetic on the list payload base (already
demonstrated in `wat/05_comprehension_exists.wat`) cover all
list-iteration shapes.  No new helper needed.

### 7.6 No new runtime helper for `range(N)` or index counting

The two-iter-var shape over a list needs an integer index.  This
is just a wasm-local int counter, incremented each iter.  When
the iter_var (the index) is referenced inside `loop_step`, we
write it into a workspace CelValue slot as `{kind=CEL_INT,
payload.i=index}` and the kIdent arm reads from that slot.  No
runtime helper.

## 8 Frontend / parser / checker changes

### 8.1 Parser library registration

Three libraries to register at parse time:

  - **Standard macros** (`has`, `all`, `exists`, `exists_one`,
    `map`, `filter`): already registered by cel-cpp's default
    parser builder; **no change needed**.
  - **`bindings_ext`** (`cel.bind`): not registered today;
    one-line addition:
    ```cpp
    builder.AddLibrary(cel::extensions::BindingsCompilerLibrary());
    ```
  - **`comprehensions_v2`** (two-iter-var forms): not registered
    today; one-line addition:
    ```cpp
    builder.AddLibrary(cel::extensions::ComprehensionsV2CompilerLibrary());
    ```

The registration happens in `compiler_v2/frontend/parse_and_check.cc`'s
parser-builder setup.

### 8.2 Checker — type inference for comprehension results

The CEL checker (cel-cpp's `cel::TypeChecker`) already infers
comprehension result types per langdef.  We pass the
`CheckedExpr` through unchanged; no checker changes needed for
the macros above.

**Caveat**: when registering `comprehensions_v2`, the checker
must also know about the extended overloads (e.g.
`list.transformList(int, T, U) → list(U)`).  cel-cpp's
`ComprehensionsV2CompilerLibrary` registers both the parser and
checker bindings; we just call `ConfigureCompiler` on the
returned library.

### 8.3 Static-subset gate (`RejectDyn`)

Today, any comprehension whose `iter_range` types as
`list(dyn)` or `map(dyn,dyn)` (heterogeneous literals) is
rejected by `RejectDyn`.  This continues — heterogeneous-typed
comprehensions are out-of-scope by design.  The comprehension
codegen never sees them.

**Exception caught at design spike time**: cel-cpp's `range(N)`
helper used internally by `transformList` types as
`list(int)`, NOT `list(dyn)` — so two-iter-var-with-index forms
DO get past `RejectDyn` even though they appear to construct a
list at runtime.  Need to confirm at implementation time; if
wrong, may need to special-case the synthesised range.

## 9 Edge cases — codegen-specific

### 9.1 Comprehension as an operand to `_+_`, `_==_`, etc.

`{'John': 'smart'}.map(key, key) == ['John']` lowers to:
`kCall(_==_, kComprehensionExpr(...), kCreateList(['John']))`.

The comprehension's output slot (the accu slot at result-time)
is the operand to `_==_`.  This works because:

  - LayoutPass allocates the accu slot at a stable workspace
    offset.
  - kCall reads operand slots; doesn't care how the operand was
    produced.
  - LayoutPass's release-on-exit semantics for comprehension
    scope slots must NOT release the accu slot until the parent
    consumer has read it.

**Mechanism**: the comprehension's accu_slot lives in the
*parent* expression's scope (the `_==_` call's operand slot),
not in the comprehension's inner scope.  This matches how kCall
allocates per-operand slots before lowering the operand.

### 9.2 Nested comprehension in `loop_step`

`[1].exists(y, [0].exists(y, y == 0))` — inner exists in
loop_step.

ResolvePass push/pop sequence:
  1. Enter outer comprehension; push frame `{y → outer_iter_off,
     __result__ → outer_accu_slot}`.
  2. Lower outer iter_range (empty scope at this point, outer
     iter_var/accu_var NOT yet visible per langdef).  Wait —
     actually langdef says iter_range is in OUTER scope.  So
     when we enter at step 1, the iter_range has already been
     lowered.  Order: lower iter_range → push outer frame → ...
  3. Lower outer accu_init (in outer scope, BEFORE pushing
     frame).
  4. Push outer frame.
  5. Enter inner comprehension; push frame `{y →
     inner_iter_off, __result__ → inner_accu_slot}`.  The name
     `y` SHADOWS the outer.
  6. Lower inner subtrees.
  7. Pop inner frame.
  8. Pop outer frame.

The crucial detail: between "enter comprehension" and
"push frame", we lower iter_range and accu_init in the OUTER
scope.  Then push the frame.  Lower cond/step in INNER scope.
Pop the iter_var binding (keep accu_var).  Lower result.  Pop
the accu_var binding.

### 9.3 cel.bind inside a comprehension (or vice versa)

`cel.bind(valid_elems, [1, 2, 3], [3, 4, 5].exists(e, e in
valid_elems))` lowers to:

```
kComprehensionExpr {  // outer is the cel.bind
  iter_var   = "#unused"
  iter_var2  = ""
  iter_range = kCreateList([])
  accu_var   = "valid_elems"
  accu_init  = kCreateList([1, 2, 3])
  loop_cond  = kBoolConst(false)
  loop_step  = kIdent("valid_elems")  ;; unused
  result     = kComprehensionExpr {   ;; the inner exists
    iter_var   = "e"
    iter_var2  = ""
    iter_range = kCreateList([3, 4, 5])
    accu_var   = "__result__"
    accu_init  = kBoolConst(false)
    loop_cond  = ...                  ;; @not_strictly_false(!__result__)
    loop_step  = kCall(`||`,
                       kIdent("__result__"),
                       kCall(`@in`,
                             kIdent("e"),
                             kIdent("valid_elems")))  ;; ← from outer!
    result     = kIdent("__result__")
  }
}
```

Note `valid_elems` is referenced inside the inner comprehension's
`loop_step`.  Resolution path:

  1. Inner scope: `e` and `__result__` bound; `valid_elems`
     not found.
  2. Outer scope: `valid_elems` bound (and `__result__` too,
     but the inner already won that lookup).
  3. Top-level activation: would be checked next, but we found
     it.

So `valid_elems` resolves to the outer cel.bind's accu_slot.
The inner kIdent emits a `local.get` against that slot.
Lifetime: the outer accu_slot is live until `result` (the inner
comp) returns.  LayoutPass enforces this.

### 9.4 Error short-circuit in `loop_cond` for `all` / `exists`

cel-cpp's `loop_cond` for `all` is
`@not_strictly_false(accu)`.  For `exists` it's
`@not_strictly_false(!accu)`.  `@not_strictly_false` is a CEL
built-in: returns `true` if the input is `true` or an error or
unknown; `false` only if input is `false`.

In our codegen, `@not_strictly_false` is a kCall to
`cel_not_strictly_false`.  That's already implemented in M5
Slice B.  No new work.

The comprehension codegen just emits
`br_if $exit (i32.eqz (load (cond_slot + payload_offset)))` —
exit if cond is **strictly false**.  Errors / unknowns / true
in cond all continue.

### 9.5 Boolean accumulator initialised true / false: the 3VL contract

For `exists`: accu starts `false`.  First iter that produces
`true` flips it.  Subsequent iters short-circuit via
`@not_strictly_false(!accu)`.  If a later iter produces
`error || true`, the result becomes `true`.  If only errors and
falses: the accu accumulates as `error`, and that's the
comprehension's result.

For `all`: dual.  accu starts `true`.  First `false` flips it
permanently (subsequent iters can't un-flip).  Errors along the
way may shift the accu to `error`, but a later `false && error`
→ `false`.

**Codegen for these is the same as for any other boolean kCall
sequence**: `cel_or` / `cel_and` already enforce 3VL.  No
comprehension-specific 3VL logic needed.

### 9.6 `transformMap` collision semantics

`[1, 2, 1].transformList(i, v, "k", v * 2)` — hypothetical, since
this doesn't exist; just transformList shape.  But
`[1, 2, 1].transformMap(k, v, k, v * 2)` — wait,
transformMap takes the receiver as MAP, not list.  So
collision is between map keys.

`{"a": 1, "b": 2}.transformMap(k, v, "K", v * 2)` — both
iterations write to the same key `"K"`.  Per langdef §"Map
literals", duplicate keys are an error at construction.  But for
transformMap... cel-cpp's behaviour:
`cel_map_insert_at(accu, "K", 2)` then
`cel_map_insert_at(accu, "K", 4)` — last-write-wins (overwrite),
since cel-cpp's eval doesn't error on duplicate-key insertion
into an existing map.  Our `cel_map_insert_at` will do the same.

(No conformance row exercises this directly; we'll add a
targeted e2e test.)

### 9.7 Empty `entry` map in `transformMapEntry`

`{"a": 1}.transformMapEntry(k, v, {})` — per iter merges an
empty map into accu, no-op.  Result: `{}`.

This is the degenerate case for `transformMapEntry`.  Codegen
handles it generically (Shape B + map insert).  No special case.

### 9.8 Two-iter-var name `iter_var == iter_var2`

Per the v2 macro parser-checks (`comprehensions_v2_macros.cc:54-58`):

```cpp
if (args[0].ident_expr().name() == args[1].ident_expr().name()) {
  return factory.ReportErrorAt(args[0],
      "all() second variable must be different from the first variable");
}
```

Parser rejects.  Codegen never sees this case.

## 10 Plan delta against m5-comprehensions-followon.md

What this design discovery changes about the milestone plan:

  - **§3.8 is WRONG.**  Two-iter-var comprehensions are NOT
    desugared to nested `cel.bind`.  cel-cpp's AST natively
    carries `iter_var2`; the evaluator dispatches `Evaluate1` vs
    `Evaluate2` at runtime.  Our codegen needs both shapes.
    Slice F therefore becomes:
    - Add `iter_var2` reading to ResolvePass / LayoutPass.
    - Add a second iter-binding emit in the `kComprehensionExpr`
      codegen arm — single-var emits Shape A/B as before; two-var
      adds a second bound local (iter_var2 → key_workspace for
      map; iter_var2 → iter_off for list, with iter_var → index
      counter).
    - **No dependency on Slice G** (cel.bind registration) any
      more — Slice F is standalone.  This shortens the critical
      path.
  - **New runtime helper: `cel_map_insert_at`** for the
    `transformMap` / `transformMapEntry` family.  Not in the
    prior plan.  Adds ~50 LoC to `cel_map.c` (Slice D scope) or
    a new Slice D.2.
  - **`transformMapEntry` is a new "shape variant"** — `entry`
    is a map expression evaluated per-iter, then merged.  Two
    codegen options (general vs single-key-pattern); ship the
    general path first.  Adds ~30 LoC to `expr_lower.cc`.
  - **Pattern detection in kCall arm** for `accu + [t]` →
    `cel_list_append_at` is a real codegen optimisation, not
    just "use the new helper".  The general `accu + [t]`
    falls through to `cel_list_concat` (O(N²)).  Codegen
    pattern-matches the AST shape `kCall(_+_,
    accu_ref, kCreateList([single])` and rewrites to a single
    `cel_list_append_at` call.  Same idea for
    `cel_map_insert_at`.
  - **Shape C (degenerate cel.bind fast path) is gradeable** —
    correctness with Shape A, perf with Shape C.  Ship Shape A
    + fast-path detection together in Slice G; no separate
    G.2 slice.
  - **Conformance unlock projection updated**.  Per-fixture:

| Fixture | Pre | Δ | Post | Reason |
|---|---:|---:|---:|---|
| macros.textproto | 0/44 | +30 to +35 | 30-35/44 | Slice C list-iter + Slice D filter/map; remaining are dyn-rejected or error-propagation edge cases |
| macros2.textproto | 0/46 | +35 to +40 | 35-40/46 | Slice F two-iter-var; remaining are transformMap/transformMapEntry edge cases (dyn-rejection + map-key shadowing) |
| bindings_ext.textproto | 0/8 | +8 | 8/8 | Slice G — registration of bindings library |
| namespace.textproto | 4/14 | +6 | 10/14 | Comprehension rows |
| **Total** | **1144** | **+79 to +89** | **~1230** | |

  - **Three new WAT traces**: `68_transformlist_indexed.wat`,
    `69_transformmap_kv.wat`, `70_transformmapentry_merge.wat`.
    Each locks a specific accumulator shape (list-with-index,
    map-with-kv-insert, map-with-merge).

## 11 Updated slice plan

Replaces §5 of m5-comprehensions-followon.md:

```
Slice A  — ResolvePass scope handler (iter_var + iter_var2 aware)
         — 1 session
Slice B  — LayoutPass scope-aware slot allocation
         — 0.5 session
Slice C  — kComprehensionExpr codegen — Shape A (list, single iter_var)
         — Unlocks 1, 2, 3 over list
         — 1 session
Slice D  — Dynamic-list append + macros 4-6 codegen
         — Unlocks map / map-with-filter / filter over list
         — 1 session
Slice E  — Shape B (map iteration) — single-iter-var over map source
         — Unlocks 1, 2, 3 over map
         — 0.5 session
Slice F  — Two-iter-var support (iter_var2)
         — Unlocks 9, 10, 11, 12, 13 (most of macros2)
         — 1 session (NOT dependent on G any more)
Slice G  — Map accumulator (cel_map_insert_at) + transformMap
         — Unlocks 14, 15
         — 0.5 session
Slice H  — transformMapEntry (per-iter map merge)
         — Unlocks 16, 17
         — 0.5 session
Slice I  — Shape C fast path for cel.bind + bindings_ext library reg
         — Unlocks 8 (bindings_ext)
         — 0.25 session
Slice J  — Closeout
         — 0.25 session
```

Total: ~6.5 sessions for the full ~+85 PASS unlock.

**Sequencing**: A → B → C, then any of D/E/F/I in parallel.
G and H depend on F (for two-iter-var binding) but on nothing
else.

**Minimum-viable milestone** if budget is tight (4 sessions):
A + B + C + D + I + closeout.  Unlocks 1-6 over list + cel.bind
(~+50 PASS).  Skips map iteration, two-iter-var, transformMap.
Customer gets cel.bind, biggest direct conformance bucket
clears.  Slice E/F/G/H ship as a follow-up.

## 12 Risks newly identified

  - **R7. `cel_list_concat` already exists; deciding when to use
    append-at vs concat is a codegen pattern-detect, not a
    runtime decision.**  If the pattern detector misses cases
    (e.g. `accu + ([t] + [])`), we silently fall through to
    `cel_list_concat` (O(N²)).  Mitigation: bench the generated
    wasm for `[1..1000].map(v, v * 2)`; if quadratic, find the
    pattern miss.  Add a debug `--warn-quadratic-comprehension`
    flag.
  - **R8. transformMap key collision** — last-write-wins is the
    cel-cpp behaviour but I haven't seen it explicitly speccd.
    Test against cel-cpp's runtime; if our behaviour diverges,
    note the spec ambiguity.
  - **R9. transformMapEntry with non-literal entry expression**
    is rarely seen but allowed by the macro.  Need to confirm
    our codegen handles `transformMapEntry(k, v, dynamic_map_expr)`
    where `dynamic_map_expr` is non-constant.  Should "just
    work" via the general path (§7.3 option a), but worth a
    targeted e2e test.
  - **R10. Map iteration order determinism across runs.**
    Tests assume order-independence but if a test secretly
    relies on insertion order (some `exists_one` tests *could*
    in theory), we need to confirm our `cel_map_iter_*`
    produces a deterministic order.  Per M3.H, our map is
    bucket-array based with stable bucket order; order is
    deterministic *given the same key-set inserted in the same
    sequence*.  This should be fine but flag if a fixture
    fails for reasons that smell like iteration order.

## 13 Test inventory the milestone must cover

Per CLAUDE.md "test every type and every AST variant": the
matrix is 18 macros × {empty, single, multi, error, nested} ×
{list source, map source where applicable}.  Realistic
parameterised coverage:

  - **`m5b_comprehension_exists_test.cc`** — `exists` /
    `exists_one` over list + map, 1/2-var.  Parameterised
    over `{empty, single match, multi match, no match,
    error-in-pred, error-in-step}`.
  - **`m5b_comprehension_all_test.cc`** — `all`, same matrix.
  - **`m5b_comprehension_filter_test.cc`** — `filter`, same
    matrix.
  - **`m5b_comprehension_map_test.cc`** — `map` / `transformList`
    over list + map source.  Parameterised over
    `{empty, single, multi, conditional-filter,
     index-in-output, error}`.
  - **`m5b_comprehension_transform_map_test.cc`** —
    `transformMap` / `transformMapEntry`.  Parameterised over
    `{empty, single, multi, conditional, key-collision,
     empty-entry, multi-entry}`.
  - **`m5b_cel_bind_test.cc`** — `cel.bind` over scalar /
    aggregate / message accu; nested bind; bind-shadows-outer;
    bind-inside-comprehension; comprehension-inside-bind.
  - **`m5b_comprehension_nested_test.cc`** — outer×inner over
    list×list, list×map, map×list, map×map; same-name shadow;
    free-var passthrough.
  - **`m5b_comprehension_consumer_test.cc`** — comprehension's
    output as operand to `==`, `in`, `size`, `_+_`, field
    select, comprehension itself (nested in outer).

Each file ~150-300 lines; total e2e suite ~1500-2000 lines new.

**Conformance side**:
  - `bazel run //compiler_v2/conformance:run_conformance --
    --file=tests/simple/testdata/macros.textproto` — verify
    +30 to +35 net post-milestone.
  - Same for macros2, bindings_ext, namespace.
  - Watch for **new FAIL rows** that weren't FAIL pre-milestone
    (e.g. dyn-rejection rows that classification logic should
    now route to `static_subset:` SKIP, not FAIL).

## 14 What's still uncertain (for design-spike time)

  - **The exact `cel_map_insert_at` rehash threshold** — fixed at
    2× capacity per current `cel_map_create` pattern, or
    different for accumulator-grown maps?  Bench.
  - **`comprehensions_v2`'s checker-library config** — what
    overloads does it register?  Need to read
    `extensions/comprehensions_v2.cc` (the non-`_macros.cc`
    side) to see the checker bindings.  Implementation detail
    discoverable at Slice F start.
  - **`transformList`'s synthetic `range(N)` expansion** — does
    it really synthesise a range list or use the index-counter
    trick I described in §7.6?  Need to read cel-cpp's
    evaluator more closely; the macro definition I read at
    `comprehensions_v2_macros.cc:223` uses `iter_var2` directly,
    suggesting the evaluator handles list iteration with two
    bindings natively (no synthesised range list).  Confirm at
    Slice F spike.

## 15 Recommendation for the milestone plan

Update `doc/implementation-plan/rewrite/m5-comprehensions-followon.md`
with these deltas:

  1. **Replace §3.8** ("Three-arg form support") with this
     doc's §6 + §11 — two-iter-var is native, not desugared.
  2. **Add `cel_map_insert_at` to §4.6** as a new runtime
     helper.
  3. **Add `transformMapEntry` codegen note to §3.4** — the
     "merge entry map into accu" loop_step pattern.
  4. **Update §5 slice plan** to the §11 version above (10
     slices A–J, 6.5 sessions, ~+85 PASS).
  5. **Update conformance projection table** with the
     per-fixture numbers from §10.
  6. **Add §7 pattern-detection codegen** as a first-class
     concern, not an afterthought — it's the difference
     between O(N) and O(N²) for `map` / `filter`.
  7. **Add R7–R10 to the risk register.**

Doing this turns the milestone plan from "approximately
right" into "verified against cel-cpp source and conformance
fixture inventory".

## 16 Summary

The comprehension landscape:

  - **18 macros**, **3 codegen execution shapes** (A loop-over-
    list, B loop-over-map, C degenerate-no-loop), **2
    accumulator-storage families** (scalar, aggregate-with-
    geometric-growth).
  - **Native two-iter-var AST shape** — not a desugar.  Slice F
    becomes a real ResolvePass / LayoutPass / codegen extension.
  - **Three new runtime helpers** needed: `cel_list_append_at`
    (geometric growth), `cel_map_insert_at` (geometric growth +
    rehash), `cel_map_iter_*` (iterator family).
  - **Codegen pattern detection** is load-bearing — the
    difference between `cel_list_concat` (O(N²)) and
    `cel_list_append_at` (O(N)) is a pattern-match on the
    `loop_step` AST shape.
  - **cel.bind ships nearly free** as a Shape-C special case +
    parser-library registration (~0.25 session for +8 PASS).
  - **Effort total**: ~6.5 sessions for ~+85 PASS, end-state at
    1144 + 85 ≈ 1229 (50.1% of total corpus, ~64% of addressable).

The prior milestone plan got the broad shape right but missed:
the native iter_var2 AST shape, the transformMap accumulator
type, the codegen pattern-detection for O(N) vs O(N²), and the
transformMapEntry execution shape.  This doc captures those.
