# M14 — CEL optionals

Status: **plan — drafted 2026-05-21 from probe evidence; not yet
started.**  Picks up after M13 (custom functions) closes.  Slice 0
(WAT traces) is non-negotiable per CLAUDE.md "WAT-first" before any
production code lands.

Probe-confirmed: `compiler_v2/probes/optionals/ast_shape_probe_test.cc`
(commit `5b9f0bb`, 16 tests, all green).  Citations in this doc of
the form *(probe Qn)* point at the matching test there.

## 0. TL;DR

cel-cpp's `OptionalCheckerLibrary` + `enable_optional_syntax`
parser flag give us the full surface for `optional<T>` after a
**2-line** change to `parse_and_check.cc`.  The runtime side adds
a new `CEL_OPTIONAL = 14` kind (already declared in `cel_data.h`,
all scaffolding NULL'd out) plus ~11 kernels.  Conformance unlock
on `optionals.textproto`: ~50–60 PASS of the 72 rows; the
remaining 12 are blocked on M7 (proto literals with `?field:`).

Estimated effort: **~7 working days** (1 Slice 0 + 4 production
slices), assuming the WAT-first traces don't surface ABI issues
that force a layout rethink.

## 1. Wire-format facts established by probes

Every one of these was a structural assumption in the original
sketch.  All are now confirmed via parse + check against cel-cpp's
real `OptionalCheckerLibrary` and a DebugString inspection of the
resulting `CheckedExpr`.

### 1.1 Operator-form sugar reaches as `kCallExpr`

  - `.?field` → `Call("_?._", [obj, field_name_string_const])`
    with `reference_map[id].overload_id = "select_optional_field"`
    *(probe Q1)*.
  - `[?key]` → `Call("_[?_]", [obj, key])` with overload_id
    `map_optindex_optional_value` (map source) /
    `list_optindex_optional_int` (list source) / variants for
    optional-typed sources *(probe Q2)*.

These are NOT desugared to has/ternary at parse time — they reach
codegen as `kCallExpr` with the synthetic function names above.
Codegen routes through the existing general `kCall` arm; the only
new work is OverloadTable seeds + runtime kernels.

### 1.2 Aggregate-literal sugar uses existing AST fields

  - `{?key: val}` → `CreateStruct.entries[i].optional_entry: bool`
    set per entry *(probe Q3)*.
  - `[?elem]` → `CreateList.optional_indices: repeated int32`
    naming the optional-element positions *(probe Q4)*.

Both fields are pre-existing in `proto/cel/expr/syntax.proto` —
our codegen currently ignores them, so the work is "honor the
flag" not "introduce a new representation."

### 1.3 `optMap` / `optFlatMap` are macros that expand to cel.bind

The expansion shape, verified verbatim from
`third_party/cel-cpp/parser/macro.cc:296-326` and confirmed by
*(probe Q5)*:

```
optMap(v, body)  →  _?_:_(
                      hasValue(target),
                      optional.of(<Shape-C cel.bind>),
                      optional.none())

where <Shape-C cel.bind> is a Comprehension with:
  iter_var    = "#unused"   (parser sentinel)
  iter_range  = []          (empty kCreateList)
  accu_var    = <user-name> (e.g. "v")
  accu_init   = target.value()
  loop_cond   = false
  loop_step   = <user-name>
  result      = body
```

`optFlatMap` is identical but the inner expression is wrapped
differently (body already returns optional; no `optional.of`
wrapper, falls through to `optional.none()` on absent target).

**This rides the M5-comprehensions-followon Shape-C detector
directly — zero new comprehension codegen.**  Verified that
`LowerComprehension` recognises iter_var="#unused" / iter_range=[]
/ loop_cond=false as Shape-C and emits the degenerate path
(`m5b-comprehensions-simplification.md` §1.5).

### 1.4 `enable_optional_syntax` auto-registers the macros

`RegisterStandardMacros(registry, opts)` calls `OptMapMacro()` and
`OptFlatMapMacro()` automatically when
`opts.enable_optional_syntax == true`
(`third_party/cel-cpp/parser/standard_macros.cc:34-37`).  A
separate `registry.RegisterMacro(OptMapMacro())` call returns
`ALREADY_EXISTS` — surfaced by the probe in its first run.

**The total parser-side wiring change is 1 line.**

### 1.5 Receiver-form methods reach as `kCallExpr` with `target` set

`.hasValue()` / `.value()` / `.or(...)` / `.orValue(...)` are
member overloads in cel-cpp's checker.  The AST shape is:

```
kCallExpr {
  function: "hasValue"        // or "value", "or", "orValue"
  target:   <the optional>    // NOT args[0]
  args:     []                // (or [other] for or / orValue)
}
```

*(probe Q12)*.  Receiver-flattening (target → args[0]) needs to
happen at codegen time, mirroring what M5.F's `EmitGeneralCall`
already does for `s.contains(sub)`.

### 1.6 `optional<T>` is `AbstractType{name="optional_type", ...}`

In the wire format, optional types appear as:

```
abstract_type {
  name: "optional_type"
  parameter_types { primitive: INT64 }   // for optional<int>
}
```

*(probe Q8)*.  Crucially, `parse_and_check.cc::UnacceptableLabel`
already recurses through `abstract_type.parameter_types()`
(lines 365-369) — so `optional<concrete>` admits automatically
and `optional<dyn>` rejects automatically.  **No static-subset
gate change needed.**

### 1.7 Select on optional-typed operand stays as `kSelectExpr`

**This was the biggest plan-vs-probe delta.**  Original sketch
assumed all optional-related access reaches as `_?._` calls.
Actually:

```
optional.of({'c': 'v'}).c      ← user wrote this (no `.?`)
   ↓ parse + check
SelectExpr {
  field: "c"
  operand: <optional<map<string,string>>-typed expression>
  test_only: false
}
type_map[<this id>] = optional<string>   ← promoted result type
```

*(probe Q11)*.  The checker DOES NOT rewrite this to `_?._`.  It
leaves the `kSelectExpr` alone and promotes the result type to
`optional<inner_field_type>`.

**Codegen implication:** `LowerSelect` in
`compiler_v2/codegen/expr_lower.cc` needs a new branch: when the
operand annotation says optional-typed, route to the same kernel
`.?` uses.  Both paths converge on the same runtime helper —
just two codegen entry points.

Symmetric story for `has(optional.x.y)`: outer test_only Select
wraps a Call to `_?._` *(probe Q13)*.  Combined "present AND
inner-has-field" semantics needed at codegen.

### 1.8 Overload-id catalogue

Sourced from `third_party/cel-cpp/checker/optional.cc` and
confirmed *(probes Q6, Q7)*.  Every entry needs an
`OverloadTable::kBuiltinSeeds` row.

| Overload ID | CEL surface |
|---|---|
| `optional_of` | `optional.of(v)` |
| `optional_ofNonZeroValue` | `optional.ofNonZeroValue(v)` |
| `optional_none` | `optional.none()` |
| `optional_value` | `opt.value()` |
| `optional_hasValue` | `opt.hasValue()` |
| `optional_or_optional` | `opt.or(other_opt)` |
| `optional_orValue_value` | `opt.orValue(default)` |
| `select_optional_field` | `obj.?field` (and Select-on-optional) |
| `map_optindex_optional_value` | `m[?key]` (map source) |
| `optional_map_optindex_optional_value` | `opt_m[?key]` |
| `list_optindex_optional_int` | `l[?i]` |
| `optional_list_optindex_optional_int` | `opt_l[?i]` |
| `optional_list_index_int` | `opt_l[i]` (chained-index sugar) |
| `optional_map_index_value` | `opt_m[k]` (chained-index sugar) |
| `list_first` | `list.first()` (v2 only) |
| `list_last` | `list.last()` (v2 only) |

`first` / `last` are v2-only extensions — defer or include based
on whether their corpus rows are inside the 72.

## 2. Runtime infrastructure already present

  - `cel_runtime/cel_data.h:46`: `CEL_OPTIONAL = 14` declared
    with a `uint32_t opt` payload field.  No code path reads it
    today; it's reserved scaffolding.
  - `cel_runtime/cel_type.c:39`: index 14 in
    `kPrimitiveTypeName[]` is `NULL` with comment "optionals-pass
    concern".  Currently produces `kTypeMismatch` poison when
    `type(...)` is called on a CEL_OPTIONAL — clean failure mode,
    not a miscompile.
  - `parse_and_check.cc::UnacceptableLabel` (lines 344-371)
    already handles `abstract_type` recursion (§1.6).
  - M5-comprehensions-followon already lowers Shape-C cel.bind
    (§1.3), so `optMap`/`optFlatMap` need zero new comprehension
    codegen.

## 3. Open design questions (need WAT traces to answer)

### 3.1 OptionalCell payload representation

Tentative layout (UNTESTED — Slice 0 will probe):

```c
struct OptionalCell {
  uint32_t present;    // 0 = None, 1 = Some
  uint32_t _pad;       // 8-byte alignment for inner
  CelValue inner;      // 24 bytes
};  // total 32 bytes
```

`CelValue.payload.opt` = u32 byte-offset of the cell in linear
memory.  Arena-allocated, lifetime to the next `arena_reset`.

Alternatives:
  - **Sentinel None.**  Allocate one shared static `OptionalCell
    { present=0 }` at a fixed memory offset; every
    `cel_optional_none()` returns that offset.  Saves an alloc
    per None construction.  Tradeoff: shared state means callers
    can't mutate the cell.  Probably worth doing.
  - **Tag-encode present in the kind word.**  Use kind values
    `CEL_OPTIONAL_NONE = 14`, `CEL_OPTIONAL_SOME = 15`; the cell
    only stores the inner CelValue.  Saves 8 bytes per cell.
    Tradeoff: type checks now need 2 kind values per check.  Not
    obviously worth it.

**Slice 0 verdict:** WAT-trace both candidates for `optional.of(1)`
and `optional.of(1).hasValue()` and pick.

### 3.2 Equality semantics

Two optionals are equal iff:
  - both `present == 0` (both None), OR
  - both `present == 1` AND `cel_equals(a.inner, b.inner)`.

This requires a new arm in `cel_equals_at_vv` for the CEL_OPTIONAL
kind.  Recursive — the inner equality dispatches through the
polymorphic ladder.

### 3.3 `type()` for CEL_OPTIONAL

cel-cpp's `type(optional.none())` returns a `Type` whose name is
the spec string `"optional_type"`.  Concrete instantiations
(`type(optional.of(1))`) return a parameterised type, but the
existing `optional_type` ident in the checker resolves to the
parameter-stripped meta-type — so the comparison
`type(optional.of(1)) == optional_type` works regardless of inner
kind.

Implementation: `cel_type.c:39` replaces NULL with
`"optional_type"`.  Done.

### 3.4 `ofNonZeroValue` per-kind zero predicate

Per cel-cpp `runtime/optional_types.cc`, the "zero value" check
is:

  - `CEL_BOOL` → `false`
  - `CEL_INT` → `0`
  - `CEL_UINT` → `0`
  - `CEL_DOUBLE` → `0.0`
  - `CEL_STRING` → `""`
  - `CEL_BYTES` → `b""`
  - `CEL_LIST_*` → empty list
  - `CEL_MAP_*` → empty map
  - `CEL_NULL` → always zero (any null → None)
  - `CEL_MESSAGE` → not defined; cel-cpp errors

Pure C code; ~40 LOC.

## 4. Slice plan (post-probe)

### Slice 0 — WAT-first runtime ABI probes (~1 day)

Per CLAUDE.md "WAT-first" + the user's explicit "WAT code first is
non-negotiable" directive (2026-05-21).  Four WAT files under
`doc/implementation-plan/rewrite/wat/`:

  - **`m14_optional_of_int.wat`** — `optional.of(1)`.  Locks the
    OptionalCell arena-alloc layout, the CEL_OPTIONAL kind tag in
    the slot-out CelValue, the `arena_alloc` calling convention.
  - **`m14_optional_has_value.wat`** — `optional.of(1).hasValue()`.
    Locks the present-flag read, the bool slot-out, and the
    full receiver-form kCall round trip.
  - **`m14_optional_select_field.wat`** — `optional.of({'c':
    'v'}).c`.  Locks the `cel_optional_select_field` kernel ABI
    + the inner-CelValue dispatch (map lookup wrapped in
    OptionalCell).
  - **`m14_optional_chain_or_value.wat`** — `{'k':
    1}.?missing.orValue('default')`.  Locks the None-propagation
    path + `cel_optional_or_value_at_vv` kernel.

Each WAT must:
  1. Assemble cleanly via `wasm-as`.
  2. Execute end-to-end through `wat_runner` (with stubbed-
     then-real kernels).
  3. Be documented in `wat-traces.md` (one section each).
  4. Lock byte-exact codegen outputs the C++ side must emit.

If any of these surface a layout issue, **revise this doc** before
proceeding to Slice A.

### Slice A — Runtime kind + value-level kernels (~3 days)

Depends: Slice 0 WAT traces locked.

  - `compiler_v2/frontend/parse_and_check.cc`: 1-line
    `parser_opts.enable_optional_syntax = true` + 1-line
    `builder.AddLibrary(cel::OptionalCheckerLibrary())`.  Plus
    BUILD.bazel dep on `@cel-cpp//checker:optional`.
  - `compiler_v2/runtime/cel_optional.{h,c}`: arena alloc + 8
    kernels (`of`, `of_non_zero`, `none`, `has_value`, `value`,
    `or`, `or_value`, plus a `cel_equals_at_vv` arm).  Per-TU
    test suite mirroring `cel_string_ext_test.cc`.
  - `compiler_v2/runtime/cel_type.c`: replace `NULL` at index 14
    with `"optional_type"`.
  - `compiler_v2/codegen/overload_table.cc`: seed 7 IDs
    (`optional_of`, `optional_ofNonZeroValue`, `optional_none`,
    `optional_value`, `optional_hasValue`, `optional_or_optional`,
    `optional_orValue_value`).
  - `compiler_v2/api/engine.cc` `kRuntimeExports`: 8 new export
    names.
  - `compiler_v2/runtime/BUILD.bazel`
    `cel_runtime_wasm.bin --export=` lines: 8 new entries.
  - Conformance unlock target: ~25 PASS (value-only rows in
    optionals.textproto).

### Slice B — `.?` / `[?` Calls + Select-on-optional (~1.5 days)

Depends: Slice A shipped.

  - Runtime: `cel_optional_select_field` + 4 variants of
    `cel_optional_index_at_*`.
  - Codegen: 5 new overload arms in the general `kCall` path
    (overload IDs from §1.8).
  - `LowerSelect` new branch: detect operand annotation
    `Repr == optional` (introduced by Slice A), route to
    `cel_optional_select_field`.
  - Test_only Select on optional operand: combined
    "present AND inner-has-field" check via a small new helper
    `cel_optional_has_chain` OR a sequence of existing
    primitives.  WAT traces in Slice 0 will inform the choice.
  - Conformance unlock target: ~15 PASS.

### Slice C — `optMap` / `optFlatMap` + optional entries in literals (~1.5 days)

Depends: Slice B shipped.

  - `optMap` / `optFlatMap` macros come for free (parser-side,
    enabled by Slice A's `enable_optional_syntax` flip).
    Verify the Shape-C cel.bind detector in
    `expr_lower_comprehension.cc` admits them (probe-validated
    iter_var="#unused", iter_range=[], cond=false shape).  Add
    e2e tests; expect zero new codegen.
  - `cel_map_insert_at_if_present` runtime kernel — symmetric
    to M5.B's `cel_map_insert_at_if_bool`.  Same ladder, new
    predicate.
  - `cel_list_append_at_if_present` runtime kernel — symmetric
    to `cel_list_append_at_if_bool`.
  - Codegen recognises `CreateStruct.entries[i].optional_entry`
    and emits the conditional-insert call (mirror existing
    M5.B comprehension-codegen for `_if_bool`).
  - Codegen recognises `CreateList.optional_indices` and emits
    the conditional-append call per indexed element.
  - Conformance unlock target: ~13 PASS.

### Slice D — closeout (~0.5 day)

Depends: Slices A-C shipped.

  - Conformance run, bump `.baseline` to the new floor.
  - `scripts/regen_conformance_readme.sh` (now auto-runs on
    pre-push via `.githooks/pre-push`).
  - `testing-checklist.md` rows ticked for optional × every
    pipeline stage.
  - This doc's status header flipped to `shipped <date>` with a
    "what landed" paragraph.
  - Tier-4 (proto-construction with `?field:`, 12 rows) deferred
    to M7 follow-up.

## 5. Out of scope

  - **Proto construction with `?field:` (12 rows of
    `optionals.textproto`)** — `TestAllTypes{?single_double_wrapper:
    optional.ofNonZeroValue(0.0)}`.  Blocked on M7 (S9, not
    started) — proto literal `cel_set_field` codegen is the
    prerequisite.  The optional arm rides M7 when it lands.
  - **`first()` / `last()` v2 list helpers** — out of M14 unless
    they appear in the 72 corpus rows; defer otherwise.
  - **Activation-bound optional values** — `Activation::Bind("x",
    Value::Optional(...))`.  Not exercised by the conformance
    corpus.  Defer until a user asks; the runtime kind + kernels
    will already support it.

## 6. Critical files for the next session to read

In order of importance:

  1. `compiler_v2/probes/optionals/ast_shape_probe_test.cc` —
     the 16 probes establishing every AST-shape fact in §1.
     Run via `bazel test //compiler_v2/probes/optionals:ast_shape_probe_test`.
  2. `third_party/cel-cpp/checker/optional.cc` — overload + type
     declarations, source of truth for §1.8.
  3. `third_party/cel-cpp/parser/macro.cc` lines 296-326 —
     `optMap`/`optFlatMap` expansion (the Shape-C generator).
  4. `compiler_v2/runtime/cel_data.h:46` — `CEL_OPTIONAL = 14`
     declaration + payload field.
  5. `compiler_v2/runtime/cel_type.c:39` — the NULL arm waiting
     to be filled in.
  6. `compiler_v2/frontend/parse_and_check.cc:633-672` — where
     the 2-line wiring goes.
  7. `doc/implementation-plan/rewrite/m5b-comprehensions-simplification.md`
     §1.5 — the Shape-C detector that `optMap` will ride.
  8. `doc/implementation-plan/rewrite/wat-traces.md` — the
     WAT-first discipline + the existing 67 WAT traces to model
     Slice 0's 4 new ones on.

## 7. Probe and code inventory (already on master)

  - **`compiler_v2/probes/optionals/`** (commit `5b9f0bb`,
    pushed to origin/master).  16 tests, all green.  Tagged
    `manual`.
  - **No production code yet** — every assertion in this doc is
    derived from the probes + reading cel-cpp source.

## 8. Update history

  - 2026-05-21: drafted from AST-probe evidence.  Slice 0 (WAT
    traces) added per "WAT code first is non-negotiable"
    directive.  Milestone numbered M14 per user direction
    (M13 reserved for custom-fns).
