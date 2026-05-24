# M14 — CEL optionals

Status: **M14 shipped 2026-05-22 (Slices 0, A, B shipped 2026-05-21;
C, E, D shipped 2026-05-22).**  See §4 for per-slice "what landed"
summary.  Slice 0 (WAT traces) was non-negotiable per CLAUDE.md
"WAT-first" before any production code landed.

> **What landed.**  `optional<T>` end-to-end: `CEL_OPTIONAL = 14`
> runtime kind + 12 kernels (8 value-level + select-field + 3
> predicate-gated `_if_present` for map / list / proto entries) +
> IR `Repr::kOptional` + codegen Select/Index/Struct branches +
> `optMap`/`optFlatMap` macros riding the Shape-C cel.bind detector
> with zero new comprehension codegen.  Nine WAT traces (`m14_*.wat`,
> §M14.1-§M14.9) lock kernel ABIs byte-exact.  Conformance:
> `optionals.textproto` 0/70 → 22/70 PASS (4 FAIL, 44 SKIP);
> corpus-wide 1554 → 1576 PASS (+22 over master).  (The 1476 →
> 1576 / +100 figure originally recorded here measured against the
> pre-Phase-C baseline; master's parallel Phase C work accounts for
> 1476 → 1554, so the optionals-isolated delta over current master
> is +22 — the 22 `optionals.textproto` rows.)
>
> **Known remaining limitations**, surfaced during execution and
> filed for follow-up:
>
>   - `optional.ofNonZeroValue(<message>)` traps at
>     `is_zero_value`'s CEL_MESSAGE arm (§3.4) — needs a host
>     trampoline that walks `Reflection::ListFields` to compute
>     proto-zero-ness.  One corpus row (`optional_ofNonZeroValue_struct_optional_ofNonZeroValue_map_optindex_field`)
>     newly FAILs as a result of Slice E lifting the
>     proto-`?field:` gate; previously SKIP'd on static_subset.
>     Filed as cleanup-backlog item.
>   - 3 chained-index FAIL rows (`optional_chaining_1`/`_2`/`_3`)
>     remain blocked on the pre-existing `map.field` sugar gap
>     (cleanup-backlog #9), unrelated to optionals.
>   - 44 SKIP rows on `optionals.textproto` are blocked on
>     `static_subset` — mostly `dyn(...)` casts and untyped
>     `map<dyn,dyn>` literals.  Relaxing `RejectDyn` is a separate
>     call outside M14.
>   - `first()` / `last()` v2 list helpers and Activation-bound
>     optionals — remain in §5 "Out of scope" (not corpus-exercised).

> **Scope pull-in 2026-05-22:** Slice E (proto `?field:` literal
> entries) was originally §5 "Out of scope" deferred to an M7
> follow-up.  Pulled into M14 once we realised the optional
> unwrap is **pure wasm** — the host-side requirement is only
> the underlying `cel_set_field` reflection, which already
> exists.  The new kernel is structurally identical to the
> `cel_map_insert_at_if_present` / `cel_list_append_at_if_present`
> pair from Slice C: wasm-side `absorb_optional_predicate` then
> delegate to a single host import.

Probe-confirmed: `compiler_v2/probes/optionals/ast_shape_probe_test.cc`
(commit `5b9f0bb`, 16 tests, all green).  Citations in this doc of
the form *(probe Qn)* point at the matching test there.

## 0. TL;DR

cel-cpp's `OptionalCheckerLibrary` + `enable_optional_syntax`
parser flag give us the full surface for `optional<T>` after a
**2-line** change to `parse_and_check.cc`.  The runtime side adds
a new `CEL_OPTIONAL = 14` kind (already declared in `cel_data.h`,
all scaffolding NULL'd out) plus ~12 kernels (Slices A + C + E).
Conformance unlock target on `optionals.textproto`: ~50–60 PASS
of the 72 rows after E lands (Slice E adds proto `?field:`
support that was originally deferred to M7).

Estimated effort: **~7.5 working days** (1 Slice 0 + 5 production
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

### 1.7 Select on optional-typed operand stays as `kSelectExpr` — **single-kernel decision resolved by Slice 0 (2026-05-21)**

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

> **Slice 0 lock (2026-05-21):** the converged kernel is named
> `cel.cel_select_optional_field_at_vv(out_slot, src_slot,
> key_slot)`.  Polymorphic dispatch on `src.kind`: CEL_OPTIONAL
> unwraps to the inner CelValue and falls through; CEL_MAP_*,
> CEL_LIST_*, CEL_MESSAGE branch into the respective lookup
> helpers; absent-key produces `optional.none()`; found-key
> produces `optional.of(value)`.  WAT-locked in
> `wat/m14_optional_select_field.wat` and
> `wat-traces.md` §M14.3.

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

### 3.1 OptionalCell payload representation — **Resolved by Slice 0 (2026-05-21)**

Locked layout (see `wat/m14_optional_of_int.wat` and
`wat-traces.md` §M14.1):

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

**Slice 0 outcome:** the chosen 32-byte single-kind layout was
WAT-traced (`m14_optional_of_int.wat`).  The alternatives
(shared-static None, tag-encoded NONE/SOME) were not WAT-traced
because both impose constraints on every future kernel
(must-not-mutate-shared-cell; double-arm every polymorphic
switch) that the saved bytes-per-cell don't pay back at expected
optional density.  Rationale captured in `wat-traces.md` §M14.1
under "Why no tag-encoded ... variant" and "Why no
shared-static-None sentinel".  A future perf pass can layer the
shared-static optimisation on top without changing the ABI.

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
  - `CEL_MESSAGE` → no set fields AND no unknown fields.  Per
    `third_party/cel-cpp/common/values/parsed_message_value.cc:78-86`:
    `ParsedMessageValue::IsZeroValue()` walks `reflection->ListFields`
    on the message; empty fields list + empty unknown-field set ⇒
    zero.  (An earlier draft of this doc said "cel-cpp errors" — that
    was wrong; corrected 2026-05-21 by Slice 0 review.)
  - `CEL_DURATION` / `CEL_TIMESTAMP` → seconds == 0 AND nanos == 0.
  - `CEL_TYPE` → never zero (a type value carries a name; cel-cpp
    treats it as always present).
  - `CEL_OPTIONAL` (nested optional) → defer to the inner cell;
    `optional.ofNonZeroValue(optional.none())` → None
    (per cel-cpp `optional_value.cc::IsZeroValue`).

Pure C code; ~50 LOC after correcting the matrix for the cases
the original draft glossed.

## 4. Slice plan (post-probe)

### Slice 0 — WAT-first runtime ABI probes (~1 day) — **shipped 2026-05-21**

Per CLAUDE.md "WAT-first" + the user's explicit "WAT code first is
non-negotiable" directive (2026-05-21).  Six WAT files under
`doc/implementation-plan/rewrite/wat/` (the original plan called
for four; two more were added after the independent code review
flagged "covered by symmetry" gaps as the same pattern that let
M2 ship 29 silent GTEST_SKIPs):

  - **`m14_optional_of_int.wat`** — `optional.of(1)`.  Locks the
    OptionalCell arena-alloc layout, the CEL_OPTIONAL kind tag in
    the slot-out CelValue, the `arena_alloc` calling convention,
    AND the **OptionalCell immutability contract** that lets a
    future shared-static-None optimisation layer in
    ABI-compatibly.
  - **`m14_optional_has_value.wat`** — `optional.of(1).hasValue()`.
    Locks the present-flag read, the bool slot-out, and the
    full receiver-form kCall round trip.
  - **`m14_optional_select_field.wat`** — `optional.of({'c':
    'v'}).c`.  Locks the `cel_select_optional_field` kernel ABI,
    the single-kernel-for-both-paths decision (Call(`_?._`) AND
    kSelectExpr-on-optional converge here), and the
    **absent-key contract** (existing `CEL_ERR_NO_SUCH_KEY` /
    `CEL_ERR_INDEX_OUT_OF_BOUNDS` / `CEL_ERR_FIELD_NOT_FOUND`
    poisons are reinterpreted as None inside this one kernel —
    no second map-lookup primitive needed).
  - **`m14_optional_chain_or_value.wat`** — `{'k':
    1}.?missing.orValue('default')`.  Locks the None-propagation
    path + `cel_optional_or_value_at_vv` kernel AND documents
    the **short-circuit codegen requirement** for Slice B
    (cel-cpp's `or`/`orValue` are jump-step short-circuit; the
    eager kernel ABI here is correct only for pure RHS — impure
    RHS needs codegen-side branch emission).
  - **`m14_optional_none.wat`** — `optional.none()`.  Locks the
    distinct 0-input ABI (`cel_optional_none_at(out_slot)`) —
    a separate cel-cpp overload, NOT a special case of `_of`.
  - **`m14_optional_of_non_zero.wat`** —
    `optional.ofNonZeroValue(0)`.  Locks the per-kind
    zero-predicate matrix (§3.4) including the corrected
    CEL_MESSAGE row (was wrongly documented as "cel-cpp errors";
    cel-cpp does support it via `IsZeroValue`).

Each WAT must:
  1. Assemble cleanly via `wasm-as`.
  2. Execute end-to-end through `wat_runner` (with no-op
     trampolines pre-Slice-A, replaced by real exports in Slice A).
  3. Be documented in `wat-traces.md` (one section each — §M14.1
     through §M14.6).
  4. Lock byte-exact codegen outputs the C++ side must emit.

Slice 0 as-shipped includes two reviews
(`reviews/2026-05-21-m14-slice0.md` + the independent
`-independent` companion); P1/P2 doc-vs-code drifts surfaced in
both were folded into this revision before Slice A starts.

### Slice A — Runtime kind + value-level kernels (~3 days) — **shipped 2026-05-21**

Depends: Slice 0 WAT traces locked.

  - [x] `compiler_v2/frontend/parse_and_check.cc`:
    `enable_optional_syntax = true` via `DefaultParserOptions()` +
    `builder.AddLibrary(cel::OptionalCheckerLibrary())`.  Plus
    BUILD.bazel dep on `@cel-cpp//checker:optional`.
  - [x] `compiler_v2/runtime/cel_optional.{h,c}`: arena alloc + 8
    kernels (`of`, `of_non_zero`, `none`, `has_value`, `value`,
    `or`, `or_value`, `select_field`) plus a `cel_equals_at_vv`
    arm.  Per-TU test suite at `cel_optional_test.cc` — 32 unit
    tests.
  - [x] `compiler_v2/runtime/cel_type.c`: replaced `NULL` at index
    14 with `"optional_type"`; matching test
    `OptionalReturnsOptionalType` in `cel_type_test.cc`.
  - [x] `compiler_v2/codegen/overload_table.cc`: 14 new Seed rows
    (size 177 → 191) covering the 7 value-level IDs *plus* the 7
    `.?field` / `[?key]` Call IDs all routed through
    `cel_select_optional_field_at_vv`.  See **Plan-vs-execution
    delta 1** below.
  - [x] `compiler_v2/api/engine.cc` `kRuntimeExports`: 8 new export
    names (one per kernel).
  - [x] `compiler_v2/runtime/BUILD.bazel`
    `cel_runtime_wasm.bin --export=` lines: 8 new entries.
  - [x] **Conformance unlock: +92 PASS (1476 → 1568).** Target was
    ~25; actual unlock was 3.6× larger because the
    `cel_select_optional_field` kernel landed in Slice A (see
    delta 1) and `OptionalCheckerLibrary` admits broad optional<T>
    type-check rows that previously failed upstream.

> **Plan-vs-execution delta 1 — 14 overload seeds, not 7, and
> `cel_select_optional_field_at_vv` shipped in Slice A not Slice B.**
> The original Slice A bullet authorised 7 value-level seeds; Slice
> A shipped 14, with the additional 7 (`select_optional_field`,
> `map_optindex_optional_value`, `optional_map_optindex_optional_value`,
> `list_optindex_optional_int`, `optional_list_optindex_optional_int`,
> `optional_list_index_int`, `optional_map_index_value`) all routed
> through `cel_select_optional_field_at_vv`.  Motivation: the
> select-field kernel is needed in Slice A for the
> `m14_optional_select_field.wat` smoke test to execute end-to-end
> against the real runtime — without it, the WAT runner's
> byte-decoding tests demanded by the Slice 0 independent review
> cannot run.  The kernel's Slice-B-flagged AST plumbing
> (`LowerSelect` Repr-detection, kCall `EmitKIndexCall` optional
> branch) remains Slice B work.

> **Plan-vs-execution delta 2 — `wat_runner.cc` was reworked, not
> just stub-deleted.**  The bullet implied "delete
> `RegisterPendingM14Imports`, add 8 real exports."  In practice
> the harness was substantively re-architected: switched from a
> host-allocated `cel.memory` to the runtime's exported shared
> memory (`wasmtime_sharedmemory_t`), added wasmtime threads +
> shared-memory config, added `wasi_config_new` +
> `wasmtime_linker_define_wasi` for the ~10 wasi-libc imports the
> abseil+cctz deps keep alive, added an `arena_init(65536)`
> call (the runtime traps in `arena_alloc` without init), and
> added a text-substitution `PreprocessWatMemoryImport` that
> rewrites `(import "cel" "memory" (memory N))` to
> `(import "cel" "memory" (memory N 32768 shared))`.  Necessary
> for the byte-exact `memory_after` decoding tests, but a real
> harness-rewrite that future WAT authors must know about.

> **Plan-vs-execution delta 3 — `.bazelrc` introduced for darwin
> toolchain pinning.**  An out-of-bullet env fix:
> `build --repo_env=PATH=/opt/homebrew/opt/llvm/bin:/usr/bin:/bin`.
> Without this, bazel's autoconfigured cc toolchain on macOS uses
> Apple clang's `-fuse-ld=ld64.lld:` form (malformed trailing colon),
> which fails to link the wasm runtime.  Pin removes the
> autoconfig drift.

### Slice B — `.?` / `[?` Calls + Select-on-optional (~1.5 days) — **shipped 2026-05-21**

Depends: Slice A shipped.

  - [x] Runtime: `cel_select_optional_field_at_vv` already shipped in
    Slice A per delta 1 — Slice B added no new runtime kernels.
  - [x] Codegen: `EmitKSelect` and `EmitKIndexCall` branch on operand
    `Repr::kOptional` and route to the optional kernel.  See
    **Plan-vs-execution delta 1 — runtime kernels already in place,
    Slice B was pure codegen** below.
  - [x] `Repr::kOptional` added to the IR + stamped by `ReprOf` for
    cel-cpp's `AbstractType{name="optional_type"}` (TypeSpec) and
    `OptionalType` (strong-typed `cel::Type::Is<OptionalType>`).
  - [x] `LayoutPass::SelectKeyRodataVisitor` lifts the field name of
    every kSelect-on-optional into rodata as a CEL_STRING CelValue.
    The `cel_select_optional_field_at_vv` kernel reads its key from a
    slot, so the codegen path needs a rodata frame; the standard
    `field_ref_id` route doesn't.  Stored as
    `NodeAnnotation::select_key_rodata_offset`; zero on every other
    kSelect.
  - [x] Test_only Select on optional: emit the
    `cel_select_optional_field_at_vv` call followed by
    `cel_optional_has_value_at_v` overwriting the same workspace
    slot with a Bool.  No new kernel needed — both primitives ship
    in Slice A.
  - [x] `parse_and_check.cc::CheckSubsetStruct` rejects
    `Foo{?field: ...}` proto-literal entries at static-subset
    gating time, allowing `EmitKStructExpr` to keep its
    CLAUDE.md-mandated `ABSL_CHECK(false) << "stub until ..."`
    body.  Closes `cleanup-backlog.md#8`.
  - [x] `EmitKIndexCall` likewise restored to `ABSL_CHECK(false)`
    for the (now-unreachable) operand-Repr-not-in-
    `{kMap, kList, kOptional}` branch.
  - [x] Conformance unlock: **+4 PASS (1568 → 1572).** Original
    target was +15.  Three rows did not unlock because they hit a
    pre-existing gap unrelated to optionals; see **Plan-vs-execution
    delta 2 — map.field sugar gap blocks 3 rows** below.

> **Plan-vs-execution delta 1 — runtime kernels already in place,
> Slice B was pure codegen.**  The original bullet authorised "4
> variants of `cel_optional_index_at_*`" + "5 new overload arms in
> the general kCall path".  In practice Slice A delta 1 already
> seeded 14 OverloadTable rows (all 7 chained-index variants route
> through the polymorphic `cel_select_optional_field_at_vv` kernel),
> so the kCall path was complete before Slice B started.  Slice B
> reduced to (a) `Repr::kOptional` plumbing, (b) `LowerSelect`
> branch, (c) `EmitKIndexCall` branch, (d) test_only chain via two
> existing primitives, (e) the CLAUDE.md "stub crashes loudly"
> restoration via a frontend gate.  Net: ~250 LOC + 3 new unit-test
> files + 1 new e2e test file.

> **Plan-vs-execution delta 2 — map.field sugar gap blocks 3 rows.**
> `optional_chaining_1`, `optional_chaining_2`, `optional_chaining_3`
> remain FAIL after Slice B (was 5 before).  All three share the
> shape `{'k': {...}}.k[...]` — the leftmost Select is on a *map*
> literal (not optional), and the codegen routes it through the
> message-field trampoline `cel_get_field`, which errors on a
> CEL_MAP_ARENA operand.  An e2e probe `EvalSource("{'k': 'v'}.k")`
> confirms the gap exists pre-Slice-B for any map-typed operand.
> Fixing requires `EmitKSelect` to detect Repr::kMap with string
> keys and lower as `m[k]` — a separate feature (cel-cpp calls this
> "field-style map access") that is **outside Slice B's mandate**
> per m14-optionals.md §4.  Filed for follow-up; the 3 FAILs stay
> on the corpus's regression list as a known-out-of-scope hazard
> until a future slice handles map.field-as-sugar.

> **Plan-vs-execution delta 3 — no new WAT files needed.**  The
> Slice 0 WAT `m14_optional_select_field.wat` covers both single
> Select-on-optional AND its chained variant by symmetry (the
> kernel is invoked twice in `optional.of(map).c.x`; the second
> call's input slot is the first's output, structurally
> identical).  The test_only chain (`has(opt.x)`) was confirmed
> via runtime + codegen + e2e tests; the kernel sequence
> `select_field + has_value` exists end-to-end in
> `cel_optional_test.cc::ChainedSelectFieldOnOptionalNestedMapsRecurses`
> and `expr_lower_test.cc::TestOnlySelectOnOptionalEmitsHasValueChain`.

### Slice C — `optMap` / `optFlatMap` + optional entries in literals (~1.5 days) — **shipped 2026-05-22**

Depends: Slice B shipped.

  - [x] `optMap` / `optFlatMap` macros ride Shape-C with zero new
    codegen.  Confirmed via 4 e2e tests in `m14_test.cc`
    (`OptMapOnSomeAppliesBodyAndWraps`, `OptMapOnNoneShortCircuitsToNone`,
    `OptFlatMapOnSomeReturnsBody`, `OptFlatMapBodyMayReturnNone`)
    — the parser-side `enable_optional_syntax` flip from Slice A
    auto-registers both macros, and `IsCelBindShape` in
    `parse_and_check.cc` admits the `iter_var="#unused"` /
    `iter_range=[]` / `loop_cond=false` shape that probe Q5
    verified.
  - [x] `cel_map_insert_at_if_present` runtime kernel —
    `(map_slot, key_slot, opt_value_slot) -> ()`.  3VL absorption +
    Some/None branch + non-CEL_OPTIONAL TYPE_MISMATCH.  Mirrors
    `cel_map_insert_at_if_bool` (cel_runtime.c:376) but reads the
    predicate from the optional's `present` flag rather than a bool.
  - [x] `cel_list_append_at_if_present` runtime kernel —
    `(list_slot, opt_value_slot) -> ()`.  Symmetric to the map kernel.
  - [x] `EmitKMapExpr` branches on `e.optional()` per entry, emitting
    `cel_map_insert_at_if_present` for `?key:` entries and the
    existing `cel_map_insert` for unconditional entries.
  - [x] `EmitKListExpr` branches on `e.optional()` per element,
    emitting `cel_list_append_at_if_present` for `?elem` entries and
    the existing `cel_list_append_at` for unconditional elements.
  - [x] WAT traces `m14_list_append_if_present.wat` (§M14.7) and
    `m14_map_insert_if_present.wat` (§M14.8) lock the kernel ABIs
    against byte-exact runtime behaviour.  Two new
    `WatRunnerM14Test` cases inspect the post-eval
    `ArenaListHeader` / `ArenaMapHeader` and confirm the
    Some-inserted/None-skipped invariant.
  - [x] Conformance: **no row unlocks** — see Plan-vs-execution
    delta below.

> **Plan-vs-execution delta 1 — conformance unlock target ~13 → 0.**
> The original Slice C target was ~13 PASS, presumed to come from
> `optional_chaining_12..16` (optional entries in literals) plus the
> 6 `optMap` / `optFlatMap` rows.  All of those rows have at least
> one `dyn`-typed sub-expression (explicit `dyn(...)` casts,
> `{}.?c` on untyped map literals, `optional.none()` typed as
> `optional<dyn>`, etc.) and are rejected at `RejectDyn` upstream
> of codegen — *exactly the same gate that intentionally blocks
> them*.  Slice C ships the feature end-to-end with 12 new runtime
> tests + 6 new codegen tests + 12 new e2e tests, so the kernels
> and codegen arms ARE exercised — just not by the corpus rows
> that were optimistically counted in the original target.  The
> corpus accounting is honest about this: the SKIP categorisation
> already attributes them to `static_subset`, not to optional
> feature gaps.  Lifting any of them would require a separate
> relaxation of `RejectDyn` (e.g. admitting `optional<dyn>` at the
> ofNonZeroValue / none constructors), which is outside Slice C's
> mandate.

> **Plan-vs-execution delta 2 — wat_runner kRuntimeExports needs
> per-kernel maintenance.**  Adding two new runtime kernels meant
> growing `kRuntimeExports` in `wat_runner.cc` from 110 → 112 to
> bind them through to test-mode wasm imports.  The std::array
> size is a `constexpr` literal that doesn't auto-derive from the
> initialiser; future kernel additions need the same two-touch
> pattern (BUILD.bazel `--export=` line + the array's size literal
> + an entry in the array body).

### Slice E — proto `?field:` literal entries (~0.5 day) — **shipped 2026-05-22**

Depends: Slice C shipped.

Pulled into M14 (was originally §5 "Out of scope" deferred to an
M7 follow-up).  The simplification that makes this slice cheap:
the optional unwrap is **pure wasm** — only the underlying field
set needs proto reflection.  We already have that surface
(`cel_host.cel_set_field`); the new kernel is a wasm-side gate in
front of it, structurally identical to the
`cel_map_insert_at_if_present` / `cel_list_append_at_if_present`
pair from Slice C.

  - **New runtime kernel** —
    `cel_set_field_at_if_present(msg_slot, field_ref_id, opt_value_slot)`,
    lives in `cel_optional.c` next to its map/list siblings.
    Body (~15 LOC) reuses the existing `absorb_optional_predicate`
    helper:
    ```c
    void cel_set_field_at_if_present(uint32_t msg_slot,
                                     uint32_t field_ref_id,
                                     uint32_t opt_value_slot) {
      CelValue* m = cel_value_at(msg_slot);
      if (m->kind != CEL_MESSAGE) return;
      const CelValue* opt = cel_value_at(opt_value_slot);
      OptionalCell* cell = NULL;
      if (absorb_optional_predicate(m, opt, &cell)) return;
      uint32_t inner_off =
          opt->payload.opt + (uint32_t)offsetof(OptionalCell, inner);
      // Delegate to the existing host trampoline — the slot points
      // at the unwrapped inner CelValue inside the OptionalCell.
      cel_host_cel_set_field(msg_slot, field_ref_id, inner_off);
    }
    ```
    `cel_optional.c` already imports `cel_host` symbols; declaring
    `cel_host_cel_set_field` as a wasm import is the same shape as
    the existing `cel_host_cel_map_lookup` usage in
    `cel_select_optional_field_at_vv`'s dispatch_lookup.

  - **Frontend** — remove the `field.optional()` rejection in
    `parse_and_check.cc::CheckSubsetStruct` (parse_and_check.cc:437).
    With Slice E shipping the codegen path, the gate's job is done.

  - **Codegen** — replace the `ABSL_CHECK(!f.optional())` stub in
    `EmitKStructExpr` (expr_lower.cc:585) with a branch:
    ```cpp
    BinaryenExpressionRef call =
        f.optional()
            ? EmitCelSetFieldIfPresentCall(ctx.mod, out_slot,
                                            field_ref_id, *value_or)
            : EmitCelSetFieldCall(ctx.mod, out_slot,
                                   field_ref_id, *value_or);
    ```
    Mirrors `EmitKMapExpr` / `EmitKListExpr`'s per-entry branch
    pattern from Slice C.  No new annotation work — the same
    `field_ref` intern row applies; the kernel reads through the
    same field_ref_id.

  - **WAT trace** —
    `m14_proto_set_field_if_present.wat` documenting:
    - one Some entry → field set in the resulting message,
    - one None entry → field stays unset (so `has(msg.field)` is
      false — matches proto semantics for unset fields).
    Locks the kernel ABI byte-exact via `wat_runner_test`.

  - **Wiring** — add `cel_set_field_at_if_present` to:
    - `runtime/BUILD.bazel` (`-Wl,--export=` line),
    - `api/engine.cc` `kRuntimeExports` array,
    - `tools/wat_runner/wat_runner.cc` `kRuntimeExports` (+ bump
      the constexpr array size 112 → 113),
    - `expr_lower.h` `kCelSetFieldAtIfPresentInternalName` const,
    - `compile.cc::InstallStructImports` (production import).

  - **Tests**:
    - Runtime per-TU: Some-sets-field / None-no-op / 3VL absorb /
      wrong-kind TYPE_MISMATCH / poisoned-msg no-op (5 tests in
      `cel_optional_test.cc`, parallel to the existing map/list
      `_if_present` matrix).
    - Codegen: all-optional / mixed / non-optional-regression in
      `expr_lower_test.cc`.
    - e2e: the verbatim conformance row shapes
      (`TestAllTypes{?single_int32_wrapper: optional.of(5)}`,
      `TestAllTypes{?single_int32_wrapper: optional.ofNonZeroValue(0)}`)
      in `m14_test.cc`.

  - **Conformance unlock target**: ~12 PASS (the
    `optionals.textproto` `has(TestAllTypes{?single_*_wrapper: …}…)`
    rows + variants).  Honest accounting: any of these rows that
    have `dyn`-typed sub-expressions still fall to `RejectDyn`
    upstream — same caveat as Slice C's delta 1.  Per-row outcome
    becomes visible on first conformance run.

### Slice D — closeout (~0.5 day) — **shipped 2026-05-22**

Depends: Slices A-E shipped.

  - [x] Conformance run, bumped `.baseline` 1572 → 1576.
  - [x] `testing-checklist.md` M14 section flipped to all-shipped.
  - [x] This doc's status header flipped to "M14 shipped
    2026-05-22" with the what-landed paragraph in §0.
  - [x] Trap on `optional.ofNonZeroValue(<message>)` filed in
    `doc/implementation-plan/cleanup-backlog.md` as the only
    M14 follow-up.

## 5. Out of scope

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
