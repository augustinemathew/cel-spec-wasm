# M5 — Collections + comprehensions

Status: **planned.**  Blocked on M4 (three-valued logic) — the
comprehension aggregator has to thread UNKNOWN / ERROR through the
accumulator (see M4's `cel_status_either`), and list / map element
access has to return the spec-mandated ERROR wrappers; both need the
3VL runtime helpers and the codegen Repr-dispatch plumbing that M4
lands.

**Ordering note (2026-04-19):** M4 and M5 were swapped.  Originally
this document was M4 and three-valued logic was M5, because the
comprehension lowering was the first place error propagation had
multiple branch points in one expression.  The stronger constraint
turned out to be that the §8.2 host ABI leaks semantics the compiler
cannot rely on at compile time (every `get_field` can return UNKNOWN
/ ERROR), so 3VL was promoted to M4 so the ABI has something
well-defined to hand back before collections build on top of it.
See `m4-three-valued.md`.

## Scope

Add list, map, and struct literals, plus the five comprehension
macros (`all`, `exists`, `exists_one`, `map`, `filter`), plus nested
comprehensions with shadowing per `../langdef.md` §comprehension-scoping
and `../wasm-compiler-design.md` §5.4.

Post-M5, these expressions must work end-to-end:

  - `[1, 2, 3].all(x, x > 0)` — simple `all`.
  - `[1, 2, 3].exists(x, x == 2)` — `exists`.
  - `{"a": 1, "b": 2}["a"] == 1` — map literal + indexing.
  - `request.items.filter(it, it.qty > 0).size() > 0` — filter +
    `size()` + chained member call.
  - `[[1, 2], [3, 4]].exists(row, row.exists(x, x == 3))` — **nested**
    comprehension; the inner `x` shadows any outer binding.
  - `[1].exists(x, [2].exists(x, x == 2))` — inner-`x`-wins
    shadowing (spec test case).
  - `[1].exists(x, .x == 1)` — leading-dot bypass to the root
    declaration environment.

Out of scope:
  - Error / unknown propagation through a comprehension — M4 lands
    the scalar plumbing; M5 extends it to the aggregator (see
    `m4-three-valued.md` deliverable "Comprehension aggregation").
  - User-defined macros — never (spec forbids; macros are closed).
  - Proto map fields — M5 handles CEL-level maps; proto map fields
    read via `cel_host.get_field` already slot into the same
    pipeline as long as the field descriptor declares the element
    types.

## Deliverables

### Runtime

- [ ] `cel_list_new(i32 capacity) → CelValue*` — allocates a
      `CelList` backed by a linear-memory array.  Grows via
      `cel_list_push`; no user-visible size limit (fails with OOM
      if the arena exhausts).
- [ ] `cel_list_push(CelValue* list, CelValue* element)`.
- [ ] `cel_list_size(CelValue*) → i64` / `cel_list_at(CelValue*, i64) → CelValue*`.
- [ ] `cel_map_new(i32 capacity) → CelValue*` — hash-table over a
      linear-memory array of `(key, value)` pairs.
- [ ] `cel_map_put(CelValue* map, CelValue* key, CelValue* value) → i32`
      — returns 1 on success, 0 on DUPLICATE_KEY (per spec §1110).
- [ ] `cel_map_get(CelValue*, CelValue* key) → CelValue*` — returns
      the element, or a `CelValue*` with `kind == CEL_ERROR` wrapping
      `NO_SUCH_KEY`.
- [ ] Comprehension frame: a stack-of-structs helper that
      `expr_lower` can `push` / `pop` around a comprehension body to
      mirror the resolution rules of §5.4.  (Runtime doesn't know
      about this; it's a codegen-side data structure.  Listed here
      because M5's testing grid pivots on it.)

### Codegen

- [ ] `kListExpr` — walk the elements, emit each, call
      `cel_list_push`.  Empty list → `cel_list_new(0)`.
- [ ] `kMapExpr` — same pattern with pairs.  Duplicate-key detection
      is runtime's job; codegen just threads the error.
- [ ] `kStructExpr` — construct a proto message via
      `cel_host.new_message(type_id)` + per-field setters.  Requires
      the host to gain `cel_host.set_field`.  This is the point at
      which the compiler can evaluate a full `my.pkg.MyReq{a: 1}`
      literal.
- [ ] `kComprehensionExpr` — the lowering in design §10.3 (block +
      local slots for `iter_var` / `accu_var`, loop over the range).
      Each comprehension introduces a pair of locals; `expr_lower`
      tracks them in a push/pop stack and resolves `kIdentExpr`
      nodes in the body against the stack before falling back to
      parameters + globals.
- [ ] Leading-dot ident resolution (`.x`) — during the comprehension
      body walk, a leading-dot ident skips the frame stack entirely
      and resolves in the root declaration env.  This was stubbed in
      M1's `RejectDyn` / `static_subset`; M5 hooks it up to codegen.
- [ ] Nested comprehensions — the frame stack handles this by
      construction, but add **explicit** e2e tests with the spec's
      shadowing examples so a future refactor that mistakenly
      flattens the stack is caught.
- [ ] Short-circuit `all` / `exists` — emit a conditional-break in
      the comprehension loop when the accumulator becomes decisive
      (false for `all`, true for `exists`).  Per the spec, short
      circuit is a semantic obligation, not an optimization.

### CLI

- [ ] `celwasmc --emit_wasm` on a comprehension should not need any
      new flags.  The M5 sh_test gains cases for each macro against a
      test schema.

## Testing obligations

`testing-checklist.md` rows to flip:

| Type            | codegen | e2e eval |
| --------------- | :-----: | :------: |
| `list<T>`       | [x]     | [x]      |
| `map<K,V>`      | [x]     | [x]      |

| Variant                                       | codegen | e2e |
| --------------------------------------------- | :-----: | :-: |
| `kListExpr` (empty + non-empty)               | [x]     | [x] |
| `kStructExpr` (proto ctor)                    | [x]     | [x] |
| `kMapExpr`                                    | [x]     | [x] |
| `kComprehensionExpr` (exists)                 | [x]     | [x] |
| `kComprehensionExpr` (all)                    | [x]     | [x] |
| `kComprehensionExpr` (filter)                 | [x]     | [x] |
| `kComprehensionExpr` (map)                    | [x]     | [x] |
| nested comprehensions with shadowing          | [x]     | [x] |

New e2e cases:

- [ ] Every spec example in §5.4 as its own `EvalE2ETest`, named
      with the expression.  Each one is both the positive test
      for codegen and a guard against a shadowing regression.
- [ ] `all` / `exists` short-circuit witness: a comprehension whose
      body would crash if evaluated past the decisive index (e.g.
      divides by an element known to be zero later in the list);
      assert the codegen emits the break early.  Witness via
      either IR inspection OR a trap-free run — the latter is more
      durable.
- [ ] Duplicate-key map literal — `Evaluate("{\"a\":1, \"a\":2}")`
      surfaces a runtime error (`kind == CEL_ERROR`, message mentions
      `DUPLICATE_KEY`).
- [ ] `NO_SUCH_KEY` on map indexing — surfaces the same way.
- [ ] Leading-dot `.x` inside a comprehension returns the global
      binding, not the comprehension var.

## Open design questions

1. **Map hash function.** Linear probing is fine for the sizes M5
   cares about; the open question is whether we commit to a specific
   hash (FNV-1a?) that hosts can assume for debug dumps or leave it
   opaque.
2. **List element type uniformity.** Checker already rejects
   heterogeneous lists, so codegen can assume homogeneous, but the
   `CelList` struct stores `CelValue*` entries which are tagged.
   Worth a measurement: can we specialise the representation for
   `list<int>` / `list<double>` without exploding the runtime
   surface?  Defer unless perf becomes a blocker.
3. **Comprehension accumulator encoding.** `accu_var` is always a
   scalar or a collection — never a message — so today's thinking is
   that the runtime always stores it as a `CelValue*` and we rely on
   the bump allocator.  Tracked for confirmation during M5.
