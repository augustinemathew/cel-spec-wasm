# Cross-numeric ordering / membership kernel plan (Slice 1.6 / M5.B step 2c)

Status: **shipped 2026-04-25.**

What landed (one-paragraph summary):
  - **Probe-spike findings**: cel-cpp's `reference_map.overload_id`
    contains exactly **one** candidate per call — never a list.  For
    `dyn(1) < 2u` the only candidate is `less_uint64` (the same-kind
    overload of the non-dyn operand kind); for `dyn(1) <= 2u` it's
    `less_equals_uint64`; for `dyn(1) in [1.0, 2.0]` it's `in_list`.
    cel-cpp REJECTS non-dyn cross-numeric forms (`1 < 2u` → "no
    matching overload") so only dyn-passthrough forms reach codegen.
    `dyn(1) == 1u` already routes via the polymorphic `equals` id.
    **Option A (resolve-pass overload-pick from candidate list) was
    not viable** — there is no list to choose from.  **Pivoted to
    Option B**: codegen-time re-pick from operand `Repr` pair.
  - **Codegen overload re-pick** (`expr_lower.cc`): a new helper
    `MaybeRepickCrossNumericOverload` runs inside `EmitGeneralCall`
    and inspects each operand's annotated `Repr`.  When the function
    is `_<_` / `_<=_` / `_>_` / `_>=_` and the operand Reprs span a
    cross-numeric pair (e.g. int↔uint, int↔double, uint↔double), it
    overrides the cel-cpp-picked overload id with the matching
    cross-numeric id (`less_int64_uint64`, etc.).  Same-kind pairs
    pass through untouched.
  - **Membership refactor**: extracted `cel_value_eq_polymorphic` in
    `cel_runtime.c` — equality between two CelValues using the
    `numeric_compare_kernel` for any numeric pair.  `cel_list_in_arena`
    + `cel_map_in_arena` now call this so `dyn(1) in [1.0, 2.0]` is
    `true`.  `map_keys_equal` upgraded to consult the polymorphic
    ladder for any numeric pair (so double-typed queries against
    int/uint keys work).
  - **Tests**: 18 new runtime unit tests
    (`cel_compare_test.cc` cross-numeric matrix + boundary +
    `cel_list_test.cc` / `cel_map_test.cc` polymorphic membership),
    3 codegen tests (`expr_lower_test.cc` overload re-pick), 159
    e2e tests (`m5_test.cc::CrossNumericOrderingE2ETest` —
    parameterised 72-row main matrix + 14 boundary + 20 NaN +
    36 membership + 5 NaN/negative edges + 6 sanity + 6 same-kind
    regression guards).
  - **Conformance delta**: 562 → 654 (+92 PASS).  Per-fixture:
    `comparisons` 189 → 269 (+80), `lists` 23 → 28 (+5),
    `integer_math` 45 → 49 (+4), `fp_math` 29 → 31 (+2), other
    fixtures stable.

## Why

Slice 1.5 (`dyn(scalar)` passthrough, shipped 2026-04-25) admits
`dyn(scalar) <op> other_kind` at the static-subset gate.  For
`<op> ∈ {==, !=}` the runtime kernel `cel_equals_at_vv` ships a
polymorphic dispatch ladder (M5.B step 2b) that handles every
int↔uint↔double pair plus same-kind aggregates, so those rows
graduate cleanly.  For `<op> ∈ {<, <=, >, >=, in}` the runtime
kernels assume same-kind operands and produce TYPE_MISMATCH on
mixed-numeric inputs.

Concretely, after Slice 1.5:
  - `comparisons.textproto`: 98 cross-kind ordering / membership
    rows moved SKIP→FAIL — every shape `dyn(K1) < K2` /
    `dyn(K1) <= K2` / `dyn(K1) > K2` / `dyn(K1) >= K2` /
    `dyn(K1) in [K2, ...]` reaches the kernel and surfaces
    `FAILED_PRECONDITION: want-kind=2 got-kind=error`.
  - `lists.textproto`: `double_in_ints`, `int_in_doubles`,
    `uint_in_doubles`, `int_in_uints`, `double_in_uints` (5+ rows)
    fail similarly because `cel_value_eq` (the element-equality
    matcher used by `cel_list_in_arena` / `cel_map_in_arena`) only
    handles same-kind double/bytes/null and same-kind +
    int↔uint via `numeric_keys_equal`; it does NOT consult the
    polymorphic `equality_kernel`.
  - `integer_math.textproto`, `fp_math.textproto`: a handful of
    rows that thread `dyn(...)` through ordering / membership.

> **Failure-mode root-cause hypothesis (worth pinning before code lands):**
> the FAIL is not "kernel hits an Unimplemented arm" — it's
> "wrong overload selected".  cel-cpp's reference-map lists
> multiple candidate overload ids for a `dyn` operand
> (e.g. `less_int64`, `less_int64_uint64`, `less_int64_double`);
> `OverloadIdResolver::PostVisitCall` (`resolve_pass.cc:347-353`)
> picks `overloads.front()`, which is `less_int64` (same-kind),
> routing to `cel_int_lt_at_vv` whose `require_kinds(..., CEL_INT)`
> rejects the uint/double operand and poisons.  The cross-numeric
> kernels (`cel_numeric_lt_at_vv` etc.) ARE shipped and
> `kBuiltinSeeds` (`overload_table.cc:142-189`) ALREADY maps every
> cross-pair id to them.  The slice's job is to pick the right one
> at resolve time.  **This MUST be confirmed by the probe before
> Step 2 ships** — see "Failure-mode probe" below.

This slice covers shipping the cross-numeric ladder for ordering
and membership: a runtime audit + a resolve-time overload-pick
fix + a polymorphic element-equality refactor for `in`, plus the
edge-case test matrix langdef demands.

## Scope

In:
  - `_<_`, `_<=_`, `_>_`, `_>=_` over every numeric pair
    (int↔uint, int↔double, uint↔double — both directions).
  - `_in_` of `int|uint|double` against a list whose elements
    are mixed numerics, and the symmetric map-key membership
    test.
  - The codegen / overload-resolution path that today picks
    `less_int64` for `dyn(int) < uint` — flip to
    `less_int64_uint64`.

Out (deferred, called out below):
  - Aggregate ordering (lexicographic list compare, map compare
    on ordering operators) — M7+.
  - Custom-typed comparisons — extensions pass.
  - The `cel.@ordered` / `ordered` set extension.
  - String / bytes ordering across the cross-numeric ladder
    (`"a" < "b"` is well-typed and dispatches via
    `cel_string_lt_at_vv`; not in scope of this slice).
  - Timestamp / duration ordering — listed in
    `kExplicitlyUnimplementedIds` (`overload_table.cc:283-290`).

## Spec reference (langdef.md)

  - **§"Comparisons"**: "the one exception [to strict type
    equality] is numeric comparisons at runtime ... numeric
    comparisons across type are supported at runtime as all
    numeric representations may be considered to exist along a
    shared number line independent of their representation in
    memory."  Spelled-out example: `-1 < dyn(1u)` is `true`;
    `1 >= dyn(18446744073709551615u)` is `false`.
  - **§"Equality"**: cross-numeric equality uses
    `numericEquals(x, y)` — defined as `!(x < y || x > y)`.
    Ordering and equality share the same promotion ladder.
  - **§"List Membership (in)"** and
    **§"Map Key Membership (in)"**: signature
    `A in list(A) -> bool` / `A in map(A, B) -> bool`.  langdef
    doesn't say "use polymorphic `==`" outright, but the
    conformance corpus pins it: `dyn(3.0) in [5, 4, 3, 2, 1]`
    expects `true` (`lists.textproto`).  cel-cpp's
    `runtime/standard/container_membership_functions.cc::InContainer`
    delegates to `equal_value`, which is the polymorphic
    equality kernel.
  - **§"Numbers"**: "arithmetic operators typically contain
    overloads for arguments of the same numeric type ... mixed-
    type computations require explicit conversion" — but
    comparisons are the documented exception (above).
  - **§"Homogeneous Equality"**: "Equality and inequality are
    homogeneous; comparing values of different runtime types ...
    is a [false] result."  This is the fallback at the bottom of
    the equality kernel today; ordering of different runtime
    types is a checker-time error (no overload), but cross-
    numeric is the documented exception.

## Failure-mode probe (executed 2026-04-25, findings below)

> **Probe spike findings (2026-04-25):**
>
> The probe (`compiler/codegen/probe_overloads.cc`, deleted post-
> spike) compiled each shape end-to-end and printed the
> `reference_map.overload_id()` list plus the post-resolve
> annotation.  Findings:
>
>   1. cel-cpp emits **exactly one** candidate per call — never a
>      list.  For `dyn(1) < 2u`: `[less_uint64]`.  For `dyn(1) > 2u`:
>      `[greater_uint64]`.  For `dyn(1) <= 2u`: `[less_equals_uint64]`.
>      The picked id is the same-kind overload of the *non-dyn*
>      operand's kind.
>   2. cel-cpp REJECTS non-dyn cross-numeric forms at the checker:
>      `1 < 2u` → "found no matching overload for `_<_` applied to
>      `(int, uint)`".  Same for `1 < 2.0`, `1u < 2.0`, and
>      `1.0 in [1, 2]`.  Only dyn-touched forms reach codegen.
>   3. `dyn(1) in [1.0, 2.0]` → reference_map gives a single
>      candidate `[in_list]` (already polymorphic in name; the
>      cross-numeric work is the runtime element-equality refactor,
>      not a different id).
>   4. `dyn(1) == 1u` → `[equals]`.  Already polymorphic at the id
>      layer; runs through the existing equality kernel.
>
> **Verdict — Option A is non-viable.**  There is no candidate
> list to choose between; the cross-numeric id is simply absent.
> **Pivot to Option B**: at codegen time in `EmitGeneralCall`,
> inspect operand Reprs.  When the function is `_<_` / `_<=_` /
> `_>_` / `_>=_` and operand Reprs span a numeric cross-pair,
> override the cel-cpp-picked id with the cross-numeric id from
> `kBuiltinSeeds`.  Operand Reprs are accurate at expr_lower time
> thanks to Slice 1.5's `DynPassthroughVisitor` annotation
> forwarding.

## Kernel layout — already mostly shipped

The arithmetic + tri-state primitive `numeric_compare_kernel`
(`cel_runtime.c:1599`) returns `CmpResult` ∈
`{kCmpLess, kCmpEqual, kCmpGreater, kCmpNanInequal}`:

```c
typedef enum {
  kCmpLess = 0,
  kCmpEqual = 1,
  kCmpGreater = 2,
  kCmpNanInequal = 3,
} CmpResult;

static CmpResult numeric_compare_kernel(const CelValue* a,
                                        const CelValue* b);
```

The kernel dispatches on `numeric_kind_pair(a->kind, b->kind)`
through nine arms (3×3 matrix) feeding into five primitives:

```c
static CmpResult cmp_i64(int64_t, int64_t);
static CmpResult cmp_u64(uint64_t, uint64_t);
static CmpResult cmp_double(double, double);
static CmpResult cmp_int_vs_uint(int64_t, uint64_t);
static CmpResult cmp_int_vs_double(int64_t, double);
static CmpResult cmp_uint_vs_double(uint64_t, double);
static CmpResult cmp_flip(CmpResult);  // for swapped-operand case
```

The six wrappers `cel_numeric_eq_at_vv` / `cel_numeric_ne_at_vv` /
`cel_numeric_lt_at_vv` / `cel_numeric_le_at_vv` /
`cel_numeric_gt_at_vv` / `cel_numeric_ge_at_vv` already exist
(`cel_runtime.c:1647-1703`).  Each calls
`numeric_prelude(out, a, b)` (3VL absorption + numeric-kind
check) then writes a CEL_BOOL based on the tri-state.

> **Key call:** the C kernel infrastructure for ordering already
> ships.  This plan is mostly a *resolution* fix, not a kernel
> fix.  The element-equality refactor for `in` is the only new C.

> **Plan-vs-execution delta:** Option A (resolve-pass overload-pick
> from a candidate list) was non-viable — see §"Failure-mode probe"
> for findings.  Pivoted to Option B at codegen time inside
> `EmitGeneralCall`.  The membership refactor extracted
> `cel_value_eq_polymorphic` and `map_keys_equal` was upgraded to
> consult `numeric_compare_kernel` for any numeric pair (including
> double-typed *queries* against int/uint keys, which the original
> plan called out specifically).  No deeper kernel surgery was
> needed.

## Promotion-ladder primitive

If we end up needing a callable from element-equality (`in`) and
we don't want to recurse through the slot-out wrapper, factor a
header-visible helper:

```c
// cel_compare.h — internal ABI; no wasm export.
// Returns the tri-state ordering result.  Both operands must be
// numeric (int / uint / double); caller has already verified.
// Mirrors `numeric_compare_kernel`'s body but with the noinline
// attribute relaxed since this becomes a callable from
// element-equality + the slot-out wrappers.
CmpResult cel_numeric_compare_pair(const CelValue* a,
                                   const CelValue* b);
```

**Decision (motivated below in Risks #3): keep
`numeric_compare_kernel` `static` for now.  Don't refactor.**
The element-equality refactor uses the polymorphic
`equality_kernel` directly via a non-slot-out internal entry
point — see "List / map membership refactor" below.  Refactoring
the ordering kernel would touch M5.B step 2b's existing test
surface for no behavioural gain.

## Resolve-time overload pick (the actual fix)

The diagnosis says: cel-cpp's reference-map for a comparison
call site whose operand list contains a `dyn` value is a vector
of candidate overload ids.  `OverloadIdResolver::PostVisitCall`
(`resolve_pass.cc:343-354`) picks `overloads.front()`.  Today
that's whichever id cel-cpp ranks first — for `dyn(int) < uint`,
empirically `less_int64`.

Fix shape (one of three):

  **Option A — Resolve-pass: prefer cross-numeric on dyn-touched
  comparison sites.**  If the call's `function()` is one of
  `_<_`/`_<=_`/`_>_`/`_>=_`/`@in` AND any operand has
  `Repr::kUnknown` / `dyn`-typed annotation OR if the candidate
  list contains BOTH a same-kind (`less_int64`) and a cross-kind
  (`less_int64_uint64`) entry, pick the cross-kind entry.

  **Option B — Codegen-time: re-pick the overload from the
  operand `Repr` pair.**  In `EmitGeneralCall`, after operand
  emission, inspect each operand's `ann.repr` (NOT the call's
  `overload_id`).  If the function is comparison/membership and
  operands span numerics, look up `cel_numeric_<op>_at_vv`
  directly.  The OverloadTable lookup becomes a fallback.

  **Option C — Runtime: make every per-kind ordering helper
  delegate cross-kind operands to the cross-numeric kernel.**
  Drop the same-kind fast path; route everything through
  `cel_numeric_<op>_at_vv`.  Simplest but pays a kind-switch on
  every same-kind compare.

**Recommendation: Option A.**  Smallest blast radius — touches
one visitor, doesn't change codegen invariants, doesn't pay a
runtime-dispatch tax on the same-kind path.  Concrete shape:

```cpp
// In OverloadIdResolver::PostVisitCall, after fetching `overloads`:
absl::string_view picked = overloads.front();
if (overloads.size() > 1 &&
    IsCrossNumericComparisonOrMembership(call.function())) {
  // Prefer a cross-numeric overload id (one that names two
  // different numeric kinds in the suffix) over a same-kind id.
  for (const std::string& cand : overloads) {
    if (IsCrossNumericOverloadId(cand)) {
      picked = cand;
      break;
    }
  }
}
annotations_[expr.id()].overload_id = picked;
```

Helper predicates (file-local, alongside existing classifiers):

```cpp
// True for `_<_` / `_<=_` / `_>_` / `_>=_` / `@in` (the canonical
// receiver-form names cel-cpp parser emits — verify in the probe).
static bool IsCrossNumericComparisonOrMembership(absl::string_view fn);

// True for `less_int64_uint64`, `less_double_int64`, etc. — any
// id that names two distinct numeric kinds.  Mechanical check
// against a small allowlist of suffix combinations.
static bool IsCrossNumericOverloadId(absl::string_view id);
```

The allowlist matches the cross-pair IDs already in
`kBuiltinSeeds` (`overload_table.cc:142-189`).

## List / map membership refactor

`cel_list_in_arena` (`cel_runtime.c:646`) and
`cel_map_in_arena` (`cel_runtime.c:762`) call element-equality
through `cel_value_eq` (`cel_runtime.c:620`) and
`map_keys_equal` (`cel_runtime.c:290`) respectively.  Neither
consults the polymorphic equality ladder.  Spec:
`A in list(A) -> bool` with the cross-numeric corpus rows
(`lists.textproto` `int_in_doubles` etc.) requires `1 in [1.0]`
to be `true`.

Two options:

  **Option A — Plumb the polymorphic kernel into the loop.**
  Extract a non-slot-out primitive from `equality_kernel`:

```c
// cel_runtime.c (file-internal forward decl).
// Equality between two CelValues following langdef §"Equality"
// — the same ladder `cel_equals_at_vv` runs, minus the slot-out
// machinery.  Returns 1 / 0 / -1 (-1 reserved for ERROR /
// UNKNOWN propagation, which the caller maps to skip-this-element).
static int cel_value_eq_polymorphic(const CelValue* a,
                                    const CelValue* b);
```

  Used by:

```c
// In cel_list_in_arena's loop:
for (uint32_t i = 0; i < hdr->count; ++i) {
  int r = cel_value_eq_polymorphic(arena_list_element(hdr, i), v);
  if (r == 1) { write_bool(out, 1); return; }
  // r == -1 (ERROR/UNKNOWN element) — langdef leaves this
  // implementation-defined.  cel-cpp short-circuits on the
  // first element error; mirror that.
  if (r == -1) { /* propagate */ return; }
}
write_bool(out, 0);
```

  **Option B — Inline a numeric ladder match into `cel_value_eq`.**
  Smaller surface but duplicates ladder logic; risks divergence
  with `equality_kernel`'s exact rules (NaN, mismatched-kind →
  false, etc.).

**Recommendation: Option A.**  One ladder, one truth.  Mirrors
the equality kernel's polymorphism without re-running its
slot-out / 3VL machinery per element.

For map membership, the equivalent helper extends
`map_keys_equal` to consult the polymorphic ladder for numeric
keys.  Note: cel-cpp rejects `double` map keys at the checker;
v2 mirrors this in `is_valid_map_key_kind`
(`cel_runtime.c:243`).  So `int` / `uint` keys can compare
cross-numeric (already partially handled via
`numeric_keys_equal`); the missing piece is treating
double-typed *queries* against int/uint keys as the polymorphic
equality result rather than letting kind drift produce
TYPE_MISMATCH.  Update `map_keys_equal` to:

```c
static int map_keys_equal(const CelValue* a, const CelValue* b) {
  if (a->kind == CEL_BOOL && b->kind == CEL_BOOL) {
    return (a->payload.b != 0) == (b->payload.b != 0);
  }
  if (a->kind == CEL_STRING && b->kind == CEL_STRING) {
    return spans_equal(a->payload.s, b->payload.s);
  }
  // Numeric path: int/uint/double pairs all use the polymorphic
  // ladder so `dyn(3.0) in {3: ...}` returns true.  double is
  // not a valid map key, but it CAN appear as a query against an
  // int/uint key.
  if (is_numeric_kind(a->kind) && is_numeric_kind(b->kind)) {
    return numeric_compare_kernel(a, b) == kCmpEqual;
  }
  return 0;
}
```

> **Plan-vs-execution delta:** Option A (resolve-pass overload-pick
> from a candidate list) was non-viable — see §"Failure-mode probe"
> for findings.  Pivoted to Option B at codegen time inside
> `EmitGeneralCall`.  The membership refactor extracted
> `cel_value_eq_polymorphic` and `map_keys_equal` was upgraded to
> consult `numeric_compare_kernel` for any numeric pair (including
> double-typed *queries* against int/uint keys, which the original
> plan called out specifically).  No deeper kernel surgery was
> needed.

## Codegen treatment

This slice adds **no new wasm exports** and **no new codegen
arms**.  Every change is:
  - One resolve-pass visitor edit (`OverloadIdResolver`).
  - Two C helper bodies (`cel_value_eq_polymorphic` +
    updated `map_keys_equal`).
  - No changes to expr_lower, no new BinaryenIf shapes, no new
    OverloadTable seeds.  The cross-pair seeds already exist
    (`overload_table.cc:142-189`).

Per CLAUDE.md "WAT-first for ABI and codegen": **no WAT trace
required.**  The wasm-visible ABI is unchanged — same imports,
same call shapes, same out-slot semantics.  An audit of one
existing trace (the `cel_numeric_lt_at_vv` call path in any
trace already covering `1 < 2`) is sufficient to confirm the
post-Slice bytecode is byte-identical to the pre-Slice bytecode
for same-kind operands.

## Edge cases the spec mandates

Each row below is a langdef-cited test the matrix MUST cover.

  - **`int(MAX_INT64) < uint(MAX_UINT64)` → true**
    (`cmp_int_vs_uint` path: any non-negative int compared to
    `2^64-1` yields kCmpLess.)  langdef §"Comparisons".
  - **`uint(MAX_UINT64) > int(MAX_INT64)` → true** (symmetric).
  - **`int(-1) < uint(0)` → true.**  Negative int short-circuit
    in `cmp_int_vs_uint` (`cel_runtime.c:1556-1559`).
    langdef §"Comparisons".
  - **`uint(0) > int(-1)` → true** (`cmp_flip` path; symmetric).
  - **`double(NaN) < <any>` → false** for `<` / `<=` / `>` /
    `>=`.  `numeric_compare_kernel` returns `kCmpNanInequal`;
    every wrapper writes `false`.  Spec
    `cel_runtime.c:1510-1512`.
  - **`double(NaN) == <any>` → false; `double(NaN) != <any>` →
    true** including self.  `cel_numeric_ne_at_vv` returns true
    on `kCmpLess || kCmpGreater` only (`cel_runtime.c:1666`),
    NOT on `kCmpNanInequal` — so `NaN != NaN` returns FALSE
    today.  **Spec drift to verify in the probe**: langdef and
    cel-cpp's `equality_functions.cc::Equal::Double` say
    `NaN != NaN` returns true (NaN is never equal to itself,
    so inequality holds).  Currently
    `cel_numeric_ne_at_vv`'s comment claims "differs from IEEE
    754" but the spec actually matches IEEE here.  This may be
    a pre-existing bug; resolve in this slice or carve out and
    track separately.  **Pin in the probe.**
  - **`double(+Inf) > <any finite>` → true; `double(-Inf) <
    <any finite>` → true.**  IEEE compares are well-defined for
    Inf via `cmp_double` (returns kCmpLess / kCmpGreater).
  - **`double(+Inf) == double(+Inf)` → true** (IEEE compares
    Inf == Inf; not NaN).
  - **`int(MAX_INT64) < double(MAX_INT64+0.0+1eps)` lossy: for
    `9223372036854775807 < 9223372036854775808.0` → false**
    (the double rounds down to `MAX_INT64+0.0`; spec is precise
    via `cmp_int_vs_double`'s boundary check).
    `comparisons.textproto` (`not_lt_dyn_int_big_lossy_double`).
  - **`double(1e100) > int(MAX_INT64)` → true** —
    `cmp_int_vs_double` checks `b > (double)INT64_MAX` first
    and returns kCmpLess (i.e. int < double, so double > int).
  - **`uint(MAX_UINT64) < double(-1.0)` → false**.
    `cmp_uint_vs_double` checks `b < 0.0` and returns
    kCmpGreater (i.e. uint > double).
    `comparisons.textproto` (`not_lt_dyn_uint_small_double`).
  - **`int(1) < double(1.5)` → true; `int(2) > double(1.5)` →
    true** — non-lossy double compares cast-then-IEEE.
  - **`double(1.0) == int(1)` → true** — the equality kernel
    routes via `cel_numeric_eq_at_vv`; spec.
  - **`int(1) in [double(1.0), double(2.0)]` → true** — element
    equality must use the cross-numeric ladder.  This is the
    `lists.textproto` (`int_in_doubles`) row that this slice
    unlocks.
  - **`double(1.5) in [int(1), int(2)]` → false** — no element
    equals 1.5 under cross-numeric equality.
  - **`uint(1) in {1: "one"}` → true** — map-key polymorphic
    membership.

Disallowed at the checker (cite for the test that ASSERTS
rejection at parse-time, not in this slice's runtime kernels):
  - `"a" < 1` — string vs int has no overload candidate.
  - `null < null` — null has no ordering overload.
  - `bool < int` — no candidate.

> **Plan-vs-execution delta:** Option A (resolve-pass overload-pick
> from a candidate list) was non-viable — see §"Failure-mode probe"
> for findings.  Pivoted to Option B at codegen time inside
> `EmitGeneralCall`.  The membership refactor extracted
> `cel_value_eq_polymorphic` and `map_keys_equal` was upgraded to
> consult `numeric_compare_kernel` for any numeric pair (including
> double-typed *queries* against int/uint keys, which the original
> plan called out specifically).  No deeper kernel surgery was
> needed.

## Testing

Per CLAUDE.md "Cover the edge-case matrix — this is a compiler"
and "Test-first or test-with-shipping (this codebase: tests
parameterised, ship in same commit)":

### Runtime unit (`cel_compare_test.cc` extensions)

The existing `CompareTest` fixture and `SameKindCmpTest`
parameterised matrix already cover the same-kind happy path.
Extensions:

  - **New `CrossNumericCmpTest` parameterised matrix.**  Per
    operator (`cel_numeric_lt_at_vv`, `_le_`, `_gt_`, `_ge_`),
    every (kind_a, kind_b) ∈ {int, uint, double}² ordered both
    ways = 9 pairs × 4 ops = 36 cases.  Each row carries
    `(helper, a_factory, b_factory, expected_bool)`.
  - **Boundary-value `TEST_F`s.**  One per langdef-cited row
    above: `IntMaxLessUintMax`, `NegativeIntLessUintZero`,
    `NaNLessAny` (per op — 4 sub-asserts), `InfGreaterFinite`,
    `MinusInfLessFinite`, `LossyDoubleVsIntMax`,
    `BigDoubleVsIntMax`, `NegativeDoubleVsUintMax`.
  - **`NaNNotEqualSelf`**: pin the langdef behaviour
    (probe-confirmed) for `cel_numeric_ne_at_vv` on
    `(NaN, NaN)`.
  - **3VL absorption (existing pattern):** ERROR + UNKNOWN
    operands propagate through every cross-numeric helper.
  - **Type-mismatch rejection:** non-numeric on either operand
    poisons with `CEL_ERR_TYPE_MISMATCH`.

### Runtime unit (membership refactor — `cel_list_test.cc` /
`cel_map_test.cc` or a new `cel_membership_test.cc`)

  - `IntInListOfDoubles` — `1 in [1.0, 2.0]` → true.
  - `DoubleInListOfInts` — `1.0 in [1, 2]` → true.
  - `UintInListOfInts` — `3u in [3]` → true.
  - `NaNInListOfDoubles` — `NaN in [NaN, 1.0]` → false (NaN
    inequal to all).
  - `NegativeIntInListOfUints` — `-1 in [0u, 1u]` → false.
  - `IntInMapOfUintKeys` — `1 in {1u: "a"}` → true.
  - `DoubleInMapOfIntKeys` — `1.0 in {1: "a"}` → true (double
    promotes via the ladder; double is not a map-key kind, but
    it's a valid query).

### Codegen (`expr_lower_test.cc` / `resolve_pass_test.cc`)

  - `OverloadIdResolverPicksCrossNumericOnDynLt` — compile
    `dyn(1) < 2u`; assert
    `annotations[<call_id>].overload_id == "less_int64_uint64"`,
    NOT `"less_int64"`.
  - `OverloadIdResolverPreservesSameKindOnNonDyn` — compile
    `1 < 2`; assert overload_id is `less_int64` (same-kind path
    not regressed).
  - `OverloadIdResolverPicksCrossNumericOnInWithMixedList` — if
    cel-cpp emits multiple `@in` candidates for a mixed-numeric
    list, assert the polymorphic one is picked.  May be a no-op
    if cel-cpp emits only `in_list`.

### E2E (`e2e/m5_test.cc` —
`CrossNumericOrderingE2ETest` fixture)

**Exhaustive across the full numeric type matrix.**  This is a
compiler — partial coverage masks miscompiles.  The fixture must
parameterise (or write longhand) every cell of the
operand-kind × operator × dyn-position grid.

  - **`CrossNumericCmpExhaustive` parameterised matrix.**  Per
    operator (`<`, `<=`, `>`, `>=`):
      - 9 ordered (kind_a, kind_b) pairs ∈ {int, uint, double}².
      - Both dyn-positions: `dyn(K1) <op> K2` and
        `K1 <op> dyn(K2)` (the latter pins that resolve-pass
        picks the cross-pair regardless of which operand
        carries the dyn wrapper).  9 × 2 = 18 rows per op,
        × 4 ops = **72 rows** before boundary cases.
      - Each row carries `(expr, expected_bool)` derived from
        the spec's number-line ordering.
  - **Boundary `TEST_F`s** (per langdef edge in the matrix above
    — written longhand, not parameterised, since each names a
    distinct invariant):
      - `LtIntMaxUintMax` (`int(MAX_INT64) < uint(MAX_UINT64)`
        → true).
      - `GtUintMaxIntMax` (symmetric).
      - `LtNegIntUintZero` (`int(-1) < uint(0)` → true).
      - `GtUintZeroNegInt` (symmetric).
      - `LtIntMaxBigLossyDouble` — match
        `comparisons.textproto`'s
        `not_lt_dyn_int_big_lossy_double` row.
      - `GtBigDoubleIntMax` (`double(1e100) > int(MAX_INT64)`
        → true).
      - `LtUintMaxNegativeDouble` (`uint(MAX_UINT64) <
        double(-1.0)` → false).
      - `LtIntDoubleNonLossy` (`int(1) < double(1.5)` → true).
      - `LtIntInt64MinDouble` (`int(INT64_MIN) < double(0.0)`
        → true).
      - `LtUintZeroDoubleNeg` (`uint(0) < double(-1.0)` →
        false).
      - `LtDoubleMinusInfFinite` (`-Inf < int(0)` → true).
      - `LtFiniteDoubleMinusInf` (symmetric → false).
      - `LtFiniteDoublePlusInf` (`int(0) < +Inf` → true).
      - `GtPlusInfFinite` (`+Inf > int(0)` → true).
  - **NaN matrix** — every operator must return false on
    NaN-touching compares.  Parameterise across
    `(<, <=, >, >=)` × `(NaN-vs-int, NaN-vs-uint,
    NaN-vs-double, NaN-vs-NaN, finite-vs-NaN)` = 4 × 5 = 20
    rows asserting `expected_bool == false`.  This matrix is
    INDEPENDENT of the equality NaN behaviour (covered by the
    Slice 1.55 prerequisite, below).
  - **Membership matrix.**  Per container kind (list / map),
    per query kind ∈ {int, uint, double}, per element / key
    kind ∈ {int, uint, double}:
      - `<query> in [<element>, ...]` → true (one element
        equals query under polymorphic equality).
      - `<query> in [<other_element>, ...]` → false.
      - `<query> in {<key>: ...}` → true / false counterparts.
      - 3 × 3 × 2 (true/false) × 2 (list/map) = **36 rows**
        before NaN-membership edges.
  - **Membership NaN edges.**
      - `dyn(NaN) in [NaN, 1.0]` → false (NaN inequal to all,
        including itself).
      - `dyn(1.0) in [NaN]` → false.
      - `dyn(1) in [NaN, 1.0]` → true (the 1.0 element matches).
  - **Membership negative-int edges.**
      - `dyn(-1) in [0u, 1u, 2u]` → false (no element matches).
      - `dyn(0u) in [-1, 0, 1]` → true.
  - **Sanity: dyn-on-both-sides and double-dyn.**
      - `dyn(1) < dyn(2u)` → true.
      - `dyn(1.0) <= dyn(1)` → true.
      - `dyn(NaN) > dyn(1)` → false.
      - `dyn(3) in dyn([1.0, 2.0, 3.0])` — but note: the
        `dyn(list)` form REJECTS at Slice 1.5's gate
        (aggregate operand).  Test that the rejection still
        fires; this is the "what stays rejected" sanity row.
  - **Same-kind regressions.**  Pin that pre-existing
    same-kind rows still take the fast path:
    `1 < 2` → true (overload_id stays `less_int64`).  At least
    one TEST per kind for `<`, `<=`, `>`, `>=`, `in`.

Total: ~150 e2e tests (parameterised + longhand combined).
This is the floor — add more if a corpus row uncovers a corner
the matrix missed.

### Conformance projection

Cross-checked against the `comparisons.textproto` failure list
post-Slice-1.5 (98 FAIL rows in `comparisons`):

  - `comparisons.textproto`: 189 → ~280–290 PASS (+91-101).
    Every cross-kind `<`/`<=`/`>`/`>=` shape graduates plus the
    in/* rows that thread cross-numeric membership.  Some FAIL
    residue likely remains for rows that combine cross-numeric
    with other gated paths (message equality, ternary).
  - `lists.textproto`: 23 → ~30 PASS (+5-7).  `double_in_ints`,
    `int_in_doubles`, `uint_in_doubles`, `int_in_uints`,
    `double_in_uints` graduate.
  - `integer_math.textproto`: 45 → ~55 PASS (+8-12).  Rows
    that thread `dyn(...)` through ordering with `<` / `>`
    against mixed-numeric operands.
  - `fp_math.textproto`: 29 → ~30 PASS (+1) plus the NaN-not-
    equal-self row if the spec-drift fix above lands.
  - **Total projection: 562 → ~660–680 PASS (+98–118)**.
    Below the naïve 98+ FAIL→PASS sum because some rows stack
    additional unimplemented gates.

> **Plan-vs-execution delta:** Option A (resolve-pass overload-pick
> from a candidate list) was non-viable — see §"Failure-mode probe"
> for findings.  Pivoted to Option B at codegen time inside
> `EmitGeneralCall`.  The membership refactor extracted
> `cel_value_eq_polymorphic` and `map_keys_equal` was upgraded to
> consult `numeric_compare_kernel` for any numeric pair (including
> double-typed *queries* against int/uint keys, which the original
> plan called out specifically).  No deeper kernel surgery was
> needed.

## Out of scope

Be explicit so future readers don't expect coverage:

  - **Aggregate ordering.**  Lexicographic list compare
    (`[1,2] < [1,3]`), map ordering, message ordering — M7+.
    cel-cpp ships `LessThan::List` / `LessThan::Map`; v2 stays
    on TYPE_MISMATCH for these until the aggregate-compare
    slice.
  - **Custom-typed comparisons.**  Extension functions whose
    return type or operand types are user-defined — extensions
    pass.
  - **Timestamp / duration ordering.**  `less_timestamp`,
    `less_duration`, etc. live in
    `kExplicitlyUnimplementedIds` (`overload_table.cc:283-290`)
    and ship with the timestamps slice.
  - **The `cel.@ordered` / `ordered` set extension.**  Not on
    the v2 critical path.
  - **String / bytes cross-type ordering.**  `"a" < b"a"` is a
    checker error (no overload); not in scope.
  - **The conformance harness's `eval_error` matcher.**  The
    rejection tests (`lt_no_overload`, `lt_mixed_types_error`)
    run `disable_check: true` and expect a runtime error
    surface; that's a separate harness story (cross-cutting,
    blocked on M5.G's error decoder which Slice 2 partially
    addressed).

## Risks

  1. **Signed-unsigned C arithmetic gotchas.**  Mostly absorbed
     by `cmp_int_vs_uint` / `cmp_uint_vs_double`'s explicit
     boundary checks.  But the kernel uses `(double)INT64_MAX`
     as a literal — clang on wasm32 may fold this to a
     `f64.const` whose nearest representable value differs from
     the conceptual boundary by 512 ULPs.  cel-cpp's
     `internal/number.h:127-143` documents this with explicit
     boundary constants; v2 inlines the casts.  **Mitigation:**
     audit the comparison test matrix to include the lossy-
     boundary rows; if any fail post-implementation, switch to
     cel-cpp's pre-computed constants.
  2. **NaN ordering invariants.**  Verified against cel-cpp:
     `Inequal<double>(double, double)`
     (`equality_functions.cc:78`) is the IEEE `lhs != rhs`
     default, returning TRUE for `NaN != NaN`.  v2's
     `cel_numeric_ne_at_vv` (`cel_runtime.c:1666`) returns
     FALSE on `kCmpNanInequal` — an unintentional spec drift.
     **Decision: extract into Slice 1.55** (a one-line change
     + 4 unit tests), shipped BEFORE Slice 1.6 so the
     ordering kernel's NaN matrix tests against a correct
     equality baseline.  See §"Prerequisite — Slice 1.55"
     above.  Bundling into Slice 1.6 would mix a spec
     correction with a feature delta and obscure both in the
     conformance numbers.
  3. **Refactoring the equality kernel.**  Could the
     `cel_numeric_compare_pair` primitive be extracted from
     `numeric_compare_kernel` for reuse from
     `cel_value_eq_polymorphic`?  The kernel is already
     `noinline`; making it `static` instead of file-scope would
     allow a header-internal entry.  **Decision: do NOT
     refactor in this slice.**  M5.B step 2b's existing tests
     exercise the kernel via the slot-out wrappers; extracting
     a primitive risks a kernel-test surface change.
     `cel_value_eq_polymorphic` calls
     `numeric_compare_kernel` directly via the
     `is_numeric_kind` short-circuit, no extraction needed.
  4. **Dispatch cost.**  9 numeric pairs × 5 operators
     (`<`/`<=`/`>`/`>=` + element equality for `in`) is
     potentially ~45 dispatch arms.  Today the equality and
     ordering kernels share `numeric_compare_kernel`'s 9-arm
     switch and use one of three primitive comparators
     (`cmp_i64`, `cmp_int_vs_uint`, etc.) — already factored.
     **No additional refactoring needed for this slice.**
  5. **Resolve-pass overload pick.**  The
     `IsCrossNumericOverloadId` allowlist must stay in sync
     with `kBuiltinSeeds`' cross-pair entries.  **Mitigation:**
     existing tripwire test in `overload_table_test.cc`
     partitions cel-cpp's `StandardOverloadIds::k*` between
     `kBuiltinSeeds` and `kExplicitlyUnimplementedIds`; add a
     parallel partition check between `kBuiltinSeeds`'
     cross-pair subset and `IsCrossNumericOverloadId`'s
     allowlist.

> **Plan-vs-execution delta:** Option A (resolve-pass overload-pick
> from a candidate list) was non-viable — see §"Failure-mode probe"
> for findings.  Pivoted to Option B at codegen time inside
> `EmitGeneralCall`.  The membership refactor extracted
> `cel_value_eq_polymorphic` and `map_keys_equal` was upgraded to
> consult `numeric_compare_kernel` for any numeric pair (including
> double-typed *queries* against int/uint keys, which the original
> plan called out specifically).  No deeper kernel surgery was
> needed.

## Prerequisite — Slice 1.55: NaN-not-equal-self spec fix

Before Slice 1.6 ships, fix the pre-existing NaN inequality
behaviour.  cel-cpp's `Inequal<double>(double, double)`
(`runtime/standard/equality_functions.cc:78`) defaults to
`lhs != rhs` — IEEE semantics — which returns TRUE for
`NaN != NaN`.  v2's `cel_numeric_ne_at_vv`
(`cel_runtime.c:1666`) returns FALSE on `kCmpNanInequal`,
treating NaN inequality as "no" rather than "yes".

This is its own slice (1.55) so the fix lands in isolation
with its own test surface — bundling it into 1.6 would mix a
spec-drift correction with a feature delta and obscure both
in the conformance numbers.

  - **One-line change** (`cel_runtime.c:1666`):
    `write_bool(out, r != kCmpEqual);` (every non-equal result
    INCLUDING NaN-inequal yields true).
  - **Unit tests** (`cel_compare_test.cc`):
      - `NaNNeNaNIsTrue` — `cel_numeric_ne_at_vv` on
        `(NaN, NaN)` writes `bool(true)`.
      - `NaNNeFiniteIsTrue` — `(NaN, 1.0)` writes
        `bool(true)`.
      - `NaNEqNaNStaysFalse` — `cel_numeric_eq_at_vv` on
        `(NaN, NaN)` still writes `bool(false)` (sanity that
        the kernel-side NaN handling is unchanged for
        equality).
      - `IntNeUintCrossNumericStillWorks` — `(int(1), uint(2))`
        writes `bool(true)` (regression guard for non-NaN
        inequalities).
  - **Comment revision in `cel_numeric_ne_at_vv`** — drop the
    "differs from IEEE 754" claim; cite cel-cpp's
    `equality_functions.cc::Inequal<double>` as the spec's
    behaviour.
  - **No conformance row movement expected** — the only
    fixture that pins NaN equality semantics is
    `fp_math.textproto`'s `nan_not_equal_to_itself` row, which
    today is one of `fp_math`'s 1 SKIP.  After this slice it
    moves to PASS (+1).
  - **Sequencing:** ship Slice 1.55 to master FIRST, then
    Slice 1.6 begins from a NaN-correct baseline.  This makes
    Slice 1.6's NaN-touching test matrix (the 20-row NaN
    ordering grid in §"Testing") actually exercise the right
    invariants — without 1.55 first, those tests would either
    fail or paper over the equality bug.

## Sequencing (Slice 1.6 proper, post-Slice-1.55)

  1. **Probe spike** (~30 min, no code).  Compile `dyn(1) < 2u`
     with debug logging on `OverloadIdResolver`; print the
     full reference_map's `overload_id()` list.  Confirm:
       - Multiple candidates listed.
       - `overloads.front()` is `less_int64` (or whichever
         non-cross-pair id).
       - The cross-pair candidate IS in the list (so picking
         it is a matter of iteration order, not synthesis).
     Also probe `1 < 2u` (no dyn) to determine whether the
     non-dyn cross-numeric form admits at the checker, and the
     `@in` function name shape cel-cpp emits for `1 in [...]`.
     Update this doc's findings.
  2. **Resolve-pass overload pick** (~50 LoC + 3 tests).
     `OverloadIdResolver::PostVisitCall` body extension +
     `IsCrossNumericComparisonOrMembership` /
     `IsCrossNumericOverloadId` predicates + tripwire test +
     unit tests.  If probe shows Option A is non-viable
     (single candidate), pivot to Option B (codegen-time
     re-pick from operand `Repr` pair) — Slice 1.5's
     annotation forwarding makes operand Reprs accurate at
     `EmitGeneralCall` time.
  3. **Membership refactor** (~80 LoC + 8 unit tests).
     `cel_value_eq_polymorphic` extraction; `map_keys_equal`
     update; new tests in `cel_list_test.cc` /
     `cel_map_test.cc` (or a new file).
  4. **Exhaustive E2E** (~400 LoC + ~150 e2e — see §"Testing"
     above).  `CrossNumericOrderingE2ETest` fixture covering
     the full operand-kind × operator × dyn-position matrix +
     boundary rows + NaN matrix + membership matrix +
     same-kind regression guards.
  5. **Conformance + closeout** (~30 min + corpus run).  Run
     full corpus; update `conformance/README.md`'s 98-FAIL
     row + per-fixture table; update
     `conformance-unlock-plan.md`'s Slice 1.5 plan-vs-execution
     delta to point to this slice.  Append a "what landed"
     summary at the top of THIS doc.

Total: ~530 LoC + ~165 tests + 0 WAT traces.  Estimate:
**1.5 days** (up from the original 1-day estimate to absorb
the exhaustive e2e matrix).  No new wasm exports, no new
BinaryenIf shapes — the slice is mostly a resolve-pass tweak
plus a membership refactor plus a load-bearing test matrix.

## Future work this enables

  - **Classifier tightening** (Slice 3 in
    `conformance-unlock-plan.md`): once cross-numeric ordering
    + membership work, the classifier can stop treating cross-
    numeric runtime errors as `kFail` (none should remain).
  - **Aggregate ordering.**  When M7's lexicographic list
    compare lands, the per-element comparator is exactly
    `numeric_compare_kernel` for numeric element types.
  - **`not_strictly_false` / comprehension internals.**
    Comprehensions' loop_condition uses 3VL `&&` over per-step
    booleans; once cross-numeric ordering lights up, predicates
    like `[1, 2, 3].exists(x, x > 1.5)` graduate.
  - **Spec-drift audit on the equality kernel.**  The
    NaN-not-equal-self check exposes a class of "comment
    contradicts behaviour" risks; the probe spike should cover
    one row, but a follow-up pass could parameterise a broader
    NaN matrix across `cel_equals_at_vv`.
