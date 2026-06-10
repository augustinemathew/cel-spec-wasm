# M16 (math_ext) — checked-AST shape probe findings

Status: research — probed 2026-05-24 against vendored cel-cpp.

Probe: `compiler/probes/math/ast_shape_probe_test.cc` (+ `BUILD.bazel`),
tagged `manual`. Run with:

```
bazel test //compiler/probes/math:ast_shape_probe_test --test_output=all
```

8 tests, all green. The probe registers the standard parser macros +
`cel::extensions::RegisterMathMacros` and the standard checker library +
`cel::extensions::MathCheckerLibrary()` (version 2 = latest, so `sqrt` is
in), then dumps `CheckedExpr.DebugString()` + the `reference_map` overload
ids for every representative shape pulled from
`tests/simple/testdata/math_ext.textproto` (199 rows; every distinct shape
covered by the battery below).

---

## Verdict (lead with this)

**Every `math.*` shape rides cel-cpp's parser macros + generic `kCall`
codegen. No shape requires custom codegen in `expr_lower.cc`.**

- `math.greatest(...)` / `math.least(...)` are **fully expanded at parse
  time** by the `math_macros()` receiver macros into **global (non-receiver)
  calls** of the internal functions **`math.@max` / `math.@min`**. The
  original receiver-style `math.greatest` / `math.least` call **never
  reaches the checked AST** — confirmed by `FindCall(ce, "math.greatest")
  == nullptr`. So codegen sees only `math.@max` / `math.@min`, never the
  surface name.
- All other functions (`ceil`, `floor`, `round`, `trunc`, `abs`, `sign`,
  `isNaN`, `isInf`, `isFinite`, `bitAnd`, `bitOr`, `bitXor`, `bitNot`,
  `bitShiftLeft`, `bitShiftRight`, `sqrt`) are plain namespaced **global**
  calls (`has_target=false`, `function="math.<name>"`) — no macro, no
  receiver form. Each resolves to exactly one overload id.

**Implication for M16:** the milestone needs only (a) runtime kernels keyed
by overload id, and (b) the existing generic `kCall` codegen arm. There is
**no new AST kind, no receiver dispatch, no list-collapse logic** to write in
codegen — the macro already collapsed everything to global calls and (for
3+/list forms) a single `kListExpr` arg. The one piece of work is that the
**list / mixed-numeric overloads are left UNRESOLVED** by the checker (see
"Surprises" below), so the runtime kernel for `math_@{min,max}_list_*` must
dispatch on the runtime element type, not on a checker-pinned overload id.

---

## Evidence table — one row per distinct AST shape

`tgt?` = `has_target`. Overload column shows the exact `reference_map`
`overload_id` set. `kCall?` = rides generic kCall codegen.

| source example | post-macro fn | arg shape | overload id(s) | result type | kCall? |
|---|---|---|---|---|---|
| `math.greatest(5)` | `math.@max` | 1 const | `math_@max_int` | int | yes |
| `math.greatest(-5.0)` | `math.@max` | 1 const | `math_@max_double` | double | yes |
| `math.greatest(5u)` | `math.@max` | 1 const | `math_@max_uint` | uint | yes |
| `math.least(5)` | `math.@min` | 1 const | `math_@min_int` | int | yes |
| `math.greatest(1, 1)` | `math.@max` | 2 const | `math_@max_int_int` | int | yes |
| `math.greatest(1.0, 1.0)` | `math.@max` | 2 const | `math_@max_double_double` | double | yes |
| `math.greatest(1u, 1u)` | `math.@max` | 2 const | `math_@max_uint_uint` | uint | yes |
| `math.greatest(1, 1.0)` | `math.@max` | 2 const | `math_@max_int_double` | **dyn** | yes |
| `math.greatest(1, 1u)` | `math.@max` | 2 const | `math_@max_int_uint` | **dyn** | yes |
| `math.greatest(1.0, 1)` | `math.@max` | 2 const | `math_@max_double_int` | **dyn** | yes |
| `math.greatest(1.0, 1u)` | `math.@max` | 2 const | `math_@max_double_uint` | **dyn** | yes |
| `math.greatest(1u, 1)` | `math.@max` | 2 const | `math_@max_uint_int` | **dyn** | yes |
| `math.greatest(1u, 1.0)` | `math.@max` | 2 const | `math_@max_uint_double` | **dyn** | yes |
| `math.greatest(10, 1, 3)` | `math.@max` | **1 list(3)** | `math_@max_list_int` | int | yes |
| `math.greatest([1,2,3])` | `math.@max` | 1 list(3) | `math_@max_list_int` | int | yes |
| `math.greatest([1.0,2.0])` | `math.@max` | 1 list(2) | `math_@max_list_double` | double | yes |
| `math.greatest([1u,2u])` | `math.@max` | 1 list(2) | `math_@max_list_uint` | uint | yes |
| `math.greatest([5.4,10,3u,-5.0,3.5])` | `math.@max` | 1 list(5) | **`math_@max_list_int, math_@max_list_double, math_@max_list_uint`** | **dyn** | yes |
| `math.greatest(5.4,10,3u,-5.0,MAXINT)` (5 mixed) | `math.@max` | **1 list(5)** | **all three `math_@max_list_*`** | **dyn** | yes |
| `math.greatest([dyn(..),dyn(..)])` | `math.@max` | 1 list(5) | all three `math_@max_list_*` | dyn | yes |
| (least mirrors greatest with `math.@min` / `math_@min_*`) | | | | | |
| `math.ceil(1.2)` | `math.ceil` | 1 const | `math_ceil_double` | double | yes |
| `math.floor(-1.2)` | `math.floor` | 1 const | `math_floor_double` | double | yes |
| `math.round(1.5)` | `math.round` | 1 const | `math_round_double` | double | yes |
| `math.trunc(-1.2)` | `math.trunc` | 1 const | `math_trunc_double` | double | yes |
| `math.abs(-11)` | `math.abs` | 1 const | `math_abs_int` | int | yes |
| `math.abs(-11.5)` | `math.abs` | 1 const | `math_abs_double` | double | yes |
| `math.abs(1u)` | `math.abs` | 1 const | `math_abs_uint` | uint | yes |
| `math.sign(-11)` | `math.sign` | 1 const | `math_sign_int` | int | yes |
| `math.sign(-32.0)` | `math.sign` | 1 const | `math_sign_double` | double | yes |
| `math.sign(0u)` | `math.sign` | 1 const | `math_sign_uint` | uint | yes |
| `math.isNaN(0.0/0.0)` | `math.isNaN` | 1 call | `math_isNaN_double` | bool | yes |
| `math.isInf(1.0/0.0)` | `math.isInf` | 1 call | `math_isInf_double` | bool | yes |
| `math.isFinite(1.0/1.5)` | `math.isFinite` | 1 call | `math_isFinite_double` | bool | yes |
| `math.bitAnd(1, 2)` | `math.bitAnd` | 2 const | `math_bitAnd_int_int` | int | yes |
| `math.bitAnd(1u, 2u)` | `math.bitAnd` | 2 const | `math_bitAnd_uint_uint` | uint | yes |
| `math.bitOr(1, 2)` | `math.bitOr` | 2 const | `math_bitOr_int_int` | int | yes |
| `math.bitOr(1u, 4u)` | `math.bitOr` | 2 const | `math_bitOr_uint_uint` | uint | yes |
| `math.bitXor(1, 3)` | `math.bitXor` | 2 const | `math_bitXor_int_int` | int | yes |
| `math.bitXor(1u, 3u)` | `math.bitXor` | 2 const | `math_bitXor_uint_uint` | uint | yes |
| `math.bitNot(1)` | `math.bitNot` | 1 const | `math_bitNot_int_int` | int | yes |
| `math.bitNot(1u)` | `math.bitNot` | 1 const | `math_bitNot_uint_uint` | uint | yes |
| `math.bitShiftLeft(1, 2)` | `math.bitShiftLeft` | 2 const | `math_bitShiftLeft_int_int` | int | yes |
| `math.bitShiftLeft(1u, 2)` | `math.bitShiftLeft` | 2 const | `math_bitShiftLeft_uint_int` | uint | yes |
| `math.bitShiftRight(1024, 2)` | `math.bitShiftRight` | 2 const | `math_bitShiftRight_int_int` | int | yes |
| `math.bitShiftRight(1024u, 2)` | `math.bitShiftRight` | 2 const | `math_bitShiftRight_uint_int` | uint | yes |
| `math.sqrt(4)` | `math.sqrt` | 1 const | `math_sqrt_int` | double | yes |
| `math.sqrt(4.0)` | `math.sqrt` | 1 const | `math_sqrt_double` | double | yes |
| `math.sqrt(4u)` | `math.sqrt` | 1 const | `math_sqrt_uint` | double | yes |

Notes on shape mechanics (from `math_ext_macros.cc`):

- **0 args** → macro reports a parse error (`requires at least one argument`).
- **1 arg** → emitted as-is, the single arg becomes args[0]. A scalar stays
  scalar (`math_@max_int`); an explicit list literal stays a single list arg
  (`math_@max_list_int`).
- **2 args** → emitted as two separate args (pairwise overload).
- **3+ args** → **collapsed by the macro into a single `kListExpr` arg**
  (so 3 ints become args=1, `list(3)`, resolving `math_@max_list_int`).
- Non-numeric simple-literal args are rejected at macro time
  (`simple literal arguments must be numeric`); empty list literal `[]` is
  rejected (`IsListLiteralWithValidArgs` requires non-empty).

---

## Resolved overload-id set for @min / @max (the seed table must cover all)

Built by the `StrCat` loops in `AddMinMaxDecls` (`math_ext_decls.cc`).
Prefix is `math_@min_` / `math_@max_`. Numeric kinds = {int, double, uint}.

**Unary (3 each):**
`math_@min_int`, `math_@min_double`, `math_@min_uint`
`math_@max_int`, `math_@max_double`, `math_@max_uint`

**Pairwise (9 each — full int/double/uint cross-product):**
`math_@min_int_int`, `math_@min_int_double`, `math_@min_int_uint`,
`math_@min_double_int`, `math_@min_double_double`, `math_@min_double_uint`,
`math_@min_uint_int`, `math_@min_uint_double`, `math_@min_uint_uint`
(and the 9 `math_@max_*_*` mirrors).
Same-kind pairs return that kind; **mismatched-kind pairs return `dyn`**
(per `out_type = DynType()` unless `type.kind() == other_type.kind()`).

**List (3 each — element type int/double/uint):**
`math_@min_list_int`, `math_@min_list_double`, `math_@min_list_uint`
`math_@max_list_int`, `math_@max_list_double`, `math_@max_list_uint`

Total: **30 overload ids** (15 min + 15 max). The runtime must back every
one of these.

---

## Open question resolved: single-scalar `math.least(5)` / `math.greatest(5)`

**The macro DOES emit a call — no identity / pass-through special-case.**
`math.least(5)` desugars to a global call `math.@min(5)` (args=1, no
target), which the checker resolves to the **unary overload
`math_@min_int`** (result type int). `math.greatest(5)` → `math.@max(5)` →
`math_@max_int`. Likewise `math.least(5.0)` → `math_@min_double`,
`math.least(5u)` → `math_@min_uint`. So the unary overloads above are
load-bearing and must have runtime kernels (they are the identity function
on a single numeric, but they still flow through the same kCall path).

---

## Surprises (shapes that don't match the "fully resolves to one id" assumption)

1. **Mixed-type / dyn list args leave the overload UNRESOLVED — the
   reference_map carries ALL THREE list overloads as candidates.**
   `math.greatest([5.4, 10, 3u, -5.0, 3.5])` and the 5-arg mixed scalar
   form (which the macro collapses to a list) both produce
   `overload_id: [math_@max_list_int, math_@max_list_double,
   math_@max_list_uint]` with **result type `dyn`**. The checker infers the
   list element type as `dyn` and cannot pin a single overload, so it
   records the full candidate set. **Codegen/runtime cannot key off a
   single checker overload id for these** — the `math_@{min,max}_list_*`
   kernel must inspect the runtime element type (or be one polymorphic
   kernel over a `list(dyn)`). This is the only place the "one overload id
   per call" assumption breaks.

2. **Cross-type pairwise resolves to a single id but result type is `dyn`,
   not a numeric.** `math.greatest(1, 1.0)` → `math_@max_int_double`, result
   `dyn`. The runtime kernel for each mixed pairwise overload must therefore
   produce a value whose runtime type depends on the comparison outcome
   (the textproto asserts e.g. `math.greatest(1, 1.0) == 1`, i.e. it can
   return either operand's type). 12 of the 18 pairwise overloads (the
   mismatched-kind ones) are `dyn`-typed this way.

3. **`sqrt` always returns `double`** regardless of input kind
   (`math_sqrt_int` / `_uint` / `_double` all → double). Don't assume
   result type mirrors arg type for sqrt.

4. **`isNaN`/`isInf`/`isFinite` only have a `_double` overload.** The
   textproto's `math.isNaN(dyn(true))` rows are runtime-error cases that
   only type-check because `dyn` erases the kind; with a concrete bool the
   checker would reject. No `_int`/`_uint` overload exists.

None of these surprises require a new codegen *arm* — they all ride generic
`kCall`. They are runtime-kernel design constraints (the list/mixed kernels
must be runtime-type-dispatched), captured here so the M16 seed table and
kernel signatures account for them.

---

## What the probe does NOT cover (out of scope, runtime-only)

The textproto has `dyn(...)`-wrapped error rows (`math.bitAnd(2u, dyn(''))`,
`math.bitNot(dyn(''))`, `math.ceil(dyn(1))`, etc.) that type-check via `dyn`
erasure and assert a *runtime* error. These reach codegen as ordinary kCall
to the same overload (the `dyn` arg just defers the kind check to runtime);
they add no new AST shape, so they're not in the probe battery. The
`isNaN(dyn(true))` / `bitAnd(2u, dyn(''))` family is a runtime-kernel
error-path concern, not a codegen-shape concern.
