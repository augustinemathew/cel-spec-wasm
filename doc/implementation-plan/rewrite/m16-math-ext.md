# M16 — `math_ext` extension (self-hosted in runtime)

Status: **shipped 2026-05-24** (Slices 0 + A + B + C + D in one
branch).  Mirrors the M12 `string_ext` template (self-hosted kernels
in `cel_runtime.wasm`, overload-table seeds, `MathCheckerLibrary()`
registration, no new codegen).

> **What landed.**  All 17 `math` functions end-to-end.  20 kernels
> in a single `cel_math_ext.c` (10 scalar + 6 bitwise + 4 min/max;
> unary min/max reuse `cel_copy_slot`); 58 overload-table seeds; the
> `math` checker library + parser macros registered in
> `parse_and_check.cc`; a targeted static-subset admission for the
> `dyn`-typed cross-type / mixed-list `math.@min`/`@max` results.
> Two Slice-0 WAT traces (`m16_math_min_list`, `m16_math_bit_shift`).
> Native kernel unit tests (`cel_math_ext_test.cc`) + a 67-case host
> e2e (`e2e/m16_test.cc`).  Conformance: `math_ext.textproto`
> 0 → 194/199 PASS (5 SKIP = `dyn`-error rows out of scope per
> `RejectDyn`, 0 FAIL); corpus-wide **+194** (1554 → 1748 standalone; 1576 → 1770 after merging M14).

> **Codegen strategy decided by AST probe (2026-05-24).**  Before any
> design choices were frozen, all 199 corpus shapes were run through
> cel-cpp's parser-macros + type-checker and the checked ASTs dumped.
> See `m16-ast-probe-findings.md` (probe:
> `compiler_v2/probes/math/ast_shape_probe_test.cc`, `manual`-tagged).
> **Verdict: every `math.*` shape rides cel-cpp's parser macros +
> the existing generic `kCall` codegen arm — no new codegen, no
> receiver dispatch, no list-collapse logic to write.**  We register
> `MathCheckerLibrary()` + the math macros and the macros do all the
> rewriting at parse time.  The scope below is reconciled against
> that evidence.

> **Target.**  CEL `math` extension functions, end-to-end through the
> AOT pipeline.  Corpus: `tests/simple/testdata/math_ext.textproto`
> (199 rows, 17 functions).  Today every row SKIPs under
> `ext_unimpl`; M16 graduates them to PASS.  Conformance is the
> definition of done.

## 1. Why M16

`math_ext` is the single largest remaining `ext_unimpl` bucket that is
a clean self-host target: pure operate-on-`CelValue` kernels, no
descriptor-pool reads, no host round-trip, and the cross-type numeric
comparison the variadic `greatest`/`least` need is **already
implemented** in the runtime (`cel_numeric_lt_at_vv` … ladder, M-cross-
type-numeric).  199 corpus rows unlock for ~18 kernels because cel-cpp
collapses the function surface through macros + shared overloads.

## 2. Scope

In scope — 17 surface functions.  Two of them (`greatest` / `least`)
are **parser macros** supplied free by cel-cpp's
`MathCheckerLibrary()`; the rest are plain overloaded functions.

### 2.1 The `greatest` / `least` macros

`math.greatest(...)` / `math.least(...)` are `ReceiverVarArg` macros
(see `third_party/cel-cpp/extensions/math_ext_macros.cc`).  At parse
time they rewrite the receiver-style call into a call of the internal
functions `math.@max` / `math.@min`, in one of three shapes:

| Arg shape | Rewrites to | Underlying overload family |
|---|---|---|
| `math.least(x)` (1 scalar) | `math.@min(x)` | unary scalar `math_@min_<t>` |
| `math.least([a,b,…])` or `math.least(x)` where `x` is a list literal | `math.@min([a,b,…])` | unary list `math_@min_list_<t>` |
| `math.least(a, b)` (2 args) | `math.@min(a, b)` | binary `math_@min_<t>` / `math_@min_<t1>_<t2>` |
| `math.least(a, b, c, …)` (3+) | `math.@min([a, b, c, …])` | unary list (args collapsed into a list literal) |

The macro validates that every leaf is numeric (int / uint / double) or
a non-empty numeric list literal; non-numeric leaves are a parse-time
error.  **We get all of that for free** by registering the checker
library — the only thing M16 owns is the runtime behaviour of
`math.@min` / `math.@max`.

So min/max needs exactly **four** new kernels, regardless of the
**30** resolved overload IDs the checker emits (15 `@min` + 15 `@max`,
enumerated in the findings doc):

  - `cel_math_min_at_vv` — binary, all int/uint/double combinations
    (cross-type via the existing numeric ladder).
  - `cel_math_max_at_vv` — binary.
  - `cel_math_min_list_at_v` — unary list (iterate + fold).
  - `cel_math_max_list_at_v` — unary list.

The **unary-scalar** case (`math.least(5)` → `math.@min(5)`) is
identity on a single numeric — probe-confirmed it resolves to a real
unary overload (`math_@min_int` / `_uint` / `_double`), so the seed
table maps those 6 ids to the **existing `cel_copy_slot`** (no new
kernel).

**Runtime-dispatch constraint (probe surprise #1 + #2):** the checker
leaves **mixed-element list** args UNRESOLVED — it records all three
`math_@{min,max}_list_*` candidates with result type `dyn`.  And
**cross-type pairwise** overloads (`math_@max_int_double`, …) resolve
to a single id but with `dyn` result.  Therefore the binary and list
kernels must **dispatch on the runtime value kind**, not on a
checker-pinned type — exactly what the cross-type numeric ladder
already does.  Both the per-element-type list ids and the mixed
candidate set map to the *same* one list kernel per direction (the
kernel reads each element's runtime kind).

### 2.2 The plain functions

| Function | Overloads (cel-cpp ids, prefix) | Result | Kernel |
|---|---|---|---|
| `math.abs(x)` | `math_abs_{int,uint,double}` | same as arg | `cel_math_abs_at_v` (kind-dispatch) |
| `math.sign(x)` | `math_sign_{int,uint,double}` | same as arg | `cel_math_sign_at_v` |
| `math.ceil(x)` | `math_ceil_double` | double | `cel_math_ceil_at_v` |
| `math.floor(x)` | `math_floor_double` | double | `cel_math_floor_at_v` |
| `math.round(x)` | `math_round_double` | double | `cel_math_round_at_v` |
| `math.trunc(x)` | `math_trunc_double` | double | `cel_math_trunc_at_v` |
| `math.isInf(x)` | `math_isInf_double` | bool | `cel_math_is_inf_at_v` |
| `math.isNaN(x)` | `math_isNaN_double` | bool | `cel_math_is_nan_at_v` |
| `math.isFinite(x)` | `math_isFinite_double` | bool | `cel_math_is_finite_at_v` |
| `math.bitAnd(a,b)` | `math_bitAnd_{int,uint}` | same as args | `cel_math_bit_and_at_vv` |
| `math.bitOr(a,b)` | `math_bitOr_{int,uint}` | same | `cel_math_bit_or_at_vv` |
| `math.bitXor(a,b)` | `math_bitXor_{int,uint}` | same | `cel_math_bit_xor_at_vv` |
| `math.bitNot(x)` | `math_bitNot_{int,uint}` | same | `cel_math_bit_not_at_v` |
| `math.bitShiftLeft(x,n)` | `math_bitShiftLeft_{int,uint}_int` | same as `x` | `cel_math_bit_shift_left_at_vv` |
| `math.bitShiftRight(x,n)` | `math_bitShiftRight_{int,uint}_int` | same as `x` | `cel_math_bit_shift_right_at_vv` |
| `math.sqrt(x)` | `math_sqrt_{int,uint,double}` | double | `cel_math_sqrt_at_v` |

**`sqrt` is in scope** (decision 2026-05-24) even though
`math_ext.textproto` has zero `sqrt` rows: the checker library
declares it, so omitting the kernel would leave a declared-but-no-
kernel gap where any real `math.sqrt(x)` expression fails codegen.
Implementing it now is one libm-style kernel.

Total: **~20 kernels**, ~40 resolved overload-id seeds.

### 2.3 Out of scope (deliberately)

  - **`disable_check: true` rows** in the corpus (parse-only eval) —
    out of scope by design (`RejectDyn` + checker-passed-only
    contract).  Stay SKIP.
  - **`math.@min` / `math.@max` over `list(dyn)` with non-literal
    construction** (a variable holding a mixed list).  The macro
    only admits literal numeric lists; a `dyn`-typed list variable
    stays `static_subset`-rejected.  Mirrors M12's
    `format`-list-literal-only admission.
  - **Big-integer / overflow-trapping bit ops beyond CEL's wrap
    semantics.**  Match cel-cpp's `int64`/`uint64` wraparound; do
    not add saturating variants.

## 3. Why self-hosted in runtime (not host trampolines)

Same trade as M12 `string_ext` and Phase C's `matches`:

  - **One `(call $math_X)` against linear memory** instead of a
    wasm→host round trip per call.  Math ops fire inside hot
    comparison / arithmetic policy paths.
  - **Pure CelValue kernels** — no descriptor-pool reads, no
    externref, no per-Instance host state.  Cleaner self-host target
    than `proto2_ext` / `network_ext`.
  - **Cross-type numeric comparison already self-hosted** — the
    variadic min/max fold reuses `cel_numeric_lt_at_vv` and friends;
    no new comparison logic.
  - **libm is available in the wasi-sdk runtime** — `ceil` / `floor`
    / `round` / `trunc` / `sqrt` / `isnan` / `isinf` are
    `<math.h>` one-liners against the vendored libc.

## 4. File structure (mirrors M12 §4)

### 4.1 New runtime files

**All math kernels in a single `.c` file** (per 2026-05-24 guidance —
"all that goes into one .c file, not each").  One dedicated TU for the
whole math extension, separate from `cel_runtime.c` but not split per
function or per family.  Each individual kernel function stays short
(the `readability-function-size` gate still applies per function); the
families are organised by section comments within the file.  All plain
C against the `CelValue` ABI; the libm-backed kernels
(`ceil`/`floor`/`round`/`trunc`/`sqrt`/`is*`) include `<math.h>`.

  - `runtime/cel_math_ext.c` — every math kernel: the 4
    min/max (`cel_math_min`/`max`/`min_list`/`max_list`), 10 scalar
    (`abs`, `sign`, `ceil`, `floor`, `round`, `trunc`, `sqrt`,
    `is_inf`, `is_nan`, `is_finite`), and 6 bitwise (`bit_and`/`or`/
    `xor`/`not`/`shift_left`/`shift_right`), grouped by section
    comment.  Internal static helpers (`Poison`, 3VL absorption,
    numeric kind dispatch, the cross-type compare fold) live at file
    scope here.
  - `runtime/cel_math_ext.h` — public ABI header; all 20
    kernel declarations + the arity / `out_slot` convention comment.

Tests:

  - `runtime/math_ext_test_helpers.h` — `MakeIntArg`,
    `MakeUintArg`, `MakeDoubleArg`, `MakeListArg` (shared fixture).
  - `runtime/cel_math_ext_test.cc` — the kernel unit
    tests (positive + negative + boundary matrix, §5.1).  May be split
    per family for readability if it grows unwieldy; the single-file
    rule is about the kernel source, not the test.

> BUILD note: a single `:cel_math_ext` `cc_library` (`cel_math_ext.c`
> behind `cel_math_ext.h`) that `cel_runtime_wasm.bin` adds as one
> dep — mirrors M12's `:cel_string_ext`.

### 4.2 Registration (data, not code)

  - `runtime/wasm_exports.txt` — +20 export lines, new
    `# math_ext extension kernels` section.
  - `runtime/BUILD.bazel` — `:cel_math_ext` cc_library
    (3 TUs + 2 headers, dep `:cel_runtime` + `absl/strings`); add to
    `cel_runtime_wasm.bin` deps; 3 cc_test targets.
  - `abi/runtime_catalogue.cc` — +20 `K_AT_V`/`K_AT_VV`
    entries in a `// math_ext extension kernels` block.
  - `compiler/codegen/overload_table.cc` — ~40 `Seed{}` entries
    mapping resolved overload IDs → the ~20 kernels (many-to-one,
    like string_ext).  Bump `kBuiltinSeedCount` in
    `overload_table_test.cc`.
  - `compiler/frontend/parse_and_check.cc` — register
    `cel::extensions::MathCheckerLibrary()` (one `AddLibrary` call,
    mirrors the `StringsCheckerLibrary()` block).  Add
    `@cel-cpp//extensions:math` to `frontend/BUILD.bazel`.

### 4.3 No codegen, no engine, no compile.cc changes

All `math.*` calls route through the existing `kCall` arm; arity comes
from the catalogue via `OverloadImpl::num_args`; the engine binds
runtime exports dynamically from `CelRuntimeHelpers()`.  This is a
§2.5-shaped feature (new runtime helpers for existing kCall
overloads), so the only codegen-side touch is verifying
`expr_lower_test.cc` shape assertions still hold — no new arm.

## 5. Test coverage strategy

### 5.1 Per-TU unit tests (runtime, native)

Each kernel gets positive + negative + boundary coverage against the
edge-case matrix that matters for a compiler:

  - **min/max**: same-type (int/uint/double), every cross-type pair,
    list of each kind, single-element list, NaN handling per spec,
    int/uint/double boundary values (`INT64_MIN/MAX`, `UINT64_MAX`,
    `±0.0`, `±Inf`), poison/UNKNOWN absorption, non-numeric kind →
    type-mismatch poison.
  - **scalar**: abs of `INT64_MIN` (overflow per spec), sign of
    `±0.0` / NaN, ceil/floor/round/trunc on negatives + huge
    magnitudes, isNaN/isInf/isFinite truth table, sqrt of negative
    (→ NaN) and of int/uint.
  - **bitwise**: int vs uint, full-width masks, shift by 0 / ≥64
    (spec-defined), bitNot two's-complement.

### 5.2 E2E test

`e2e/m16_test.cc` — one fixture per family
(`MinMaxE2ETest`, `ScalarE2ETest`, `BitwiseE2ETest`), plus a
`MacroExpansion` fixture asserting `math.least(1,2,3)` and
`math.greatest([a,b])` compile + evaluate correctly through the macro
rewrite.

### 5.3 Conformance lock

Add `tests/simple/testdata/math_ext.textproto` to
`conformance/BUILD.bazel` + `run_conformance.cc`.  Target:
the addressable (non-`disable_check`) `math_ext` rows flip 0 → PASS.
Re-measure `.baseline`, regenerate `conformance/README.md` (the
pre-push drift gate), tick `testing-checklist.md` rows.

## 6. Slicing

### Slice 0 — WAT traces + this doc (WAT-first) — **DONE 2026-05-24**

AST probe (`probes/math/`) + design doc landed first.  Then the two
WAT traces whose memory/ABI shape is *not* already covered by an
existing trace, authored + assembled (`wasm-as`) + written up in
`wat-traces.md` §M16.1/§M16.2:

  - `wat/m16_math_min_list.wat` — unary-list iteration (arena-list
    layout + per-element cross-type fold; the one genuinely new
    shape).  Assembles, 298 B.
  - `wat/m16_math_bit_shift.wat` — shift slot/kind shape + spec wrap.
    Assembles, 201 B.

Both import not-yet-existing kernels, so they assemble now and are
registered in `wat_runner_test` (and run end-to-end) when the kernels
land (Slices B / C).

Deliberately NOT traced (no new ABI to freeze):

  - **Binary min/max** (`_at_vv`) — shape is identical to the existing
    `cel_numeric_lt_at_vv` / `cel_int_add_at_vv` traces
    (`wat/17_compare_int_eq.wat`, `wat/16_arith_int_add.wat`); the
    cross-type fold is internal C, not a new ABI.
  - **Scalar kernels** (ceil/floor/abs/sign/sqrt/is*) — single
    `(call $libm-style)` `_at_v` shape, same as every other unary
    kernel; nothing to freeze.

### Slice A — scalar family (~1 day)

The scalar section of `cel_math_ext.c` + tests: abs, sign, ceil,
floor, round, trunc, sqrt, isInf, isNaN, isFinite.  Simplest; no
list/iteration.  Creates `cel_math_ext.{c,h}` + the `:cel_math_ext`
BUILD target.

### Slice B — bitwise family (~0.5 day)

Adds the bitwise section to `cel_math_ext.c` + tests.  Pure integer
ops.

### Slice C — min/max binary + list (~1.5 days)

Adds the min/max section to `cel_math_ext.c` + tests.  The variadic
fold; reuses the numeric ladder; consumes the Slice-0 WAT shapes.

### Slice D — wiring + conformance lock (~0.5 day)

`wasm_exports.txt`, `runtime_catalogue.cc`, `overload_table.cc` seeds,
`MathCheckerLibrary()` registration, `m16_test.cc`, conformance add +
baseline + README regen.  Close the milestone.

## 7. Risks

  - **Resolved overload-id enumeration.**  *De-risked by the probe* —
    the full set is **30 min/max ids** (3 unary + 9 pairwise + 3 list,
    ×2), enumerated verbatim in `m16-ast-probe-findings.md` §"Resolved
    overload-id set".  The seed table must cover every one, or a
    cross-type `math.least(1, 2u)` fails at import-install.  Slice D
    cross-checks against a parametrized overload-coverage test that
    compiles one expr per resolved id.
  - **`dyn`-typed results from mixed list / cross-type pairwise.**
    The checker can't pin a numeric result type for these (probe
    surprises #1/#2), so the kernels are runtime-kind-dispatched.
    This is a kernel-design constraint, not a codegen one.
  - **NaN ordering in min/max.**  CEL spec mandates specific NaN
    behaviour for greatest/least; assert against `math_ext.textproto`
    rows, not against libm intuition.
  - **`abs(INT64_MIN)` / shift-by-≥64.**  Spec-defined edge cases;
    cite the spec section in the test, don't guess.

## 8. Open questions

  - **RESOLVED (2026-05-24, AST probe).**  Single-scalar
    `math.least(5)` *does* emit a call — `math.@min(5)` resolving to
    the unary overload `math_@min_int` (no identity special-case in
    the macro).  Since min/max of one value is identity, the seed
    maps the 6 unary ids to `cel_copy_slot`; no `cel_math_min_at_v`
    unary kernel is needed.  See `m16-ast-probe-findings.md` §"Open
    question resolved".
  - No open questions remain that block Slice 0.

## 9. Closeout gate

  - [x] `bazel test //compiler_v2/...` green.
  - [x] Per-TU runtime tests: positive + negative + boundary for every
        kernel (`cel_math_ext_test.cc`).
  - [x] `m16_test.cc` e2e: every family + macro-expansion (67 cases).
  - [x] Slice-0 WATs assemble (`wasm-as`).  *Follow-up:* register them
        in `wat_runner_test` to run end-to-end now that the kernels
        exist — deferred; the kernels are validated by the unit + e2e
        + conformance gates.
  - [x] `math_ext.textproto` → 194/199 PASS (5 SKIP `dyn`-error rows,
        0 FAIL); `.baseline` → 1770 (post-M14 merge; +194 from M16); `conformance/README.md`
        regenerated (pre-push drift gate clean).
  - [x] `overload_table.cc` seeds cover every resolved id (the e2e
        cross-type cases exercise the full pairwise/list set); seed
        count test (`kBuiltinSeedCount = 235`) passes.
  - [x] `testing-checklist.md` rows ticked; this doc's status flipped
        to shipped with a "what landed" summary.

## In progress

Pipeline checklist for this feature type — §2.5 (new runtime helpers
for existing kCall overloads) + the M12-style extension-library
registration.  Files to touch, top-down:

  - [ ] Runtime kernels: `cel_math_ext.h`, `cel_math_ext_internal.h`,
        `cel_math_{minmax,scalar,bitwise}.cc` (+ tests).
  - [ ] Exports: `wasm_exports.txt`, `runtime/BUILD.bazel`.
  - [ ] ABI catalogue: `runtime_catalogue.cc`.
  - [ ] Overload seeds: `overload_table.cc` (+ `kBuiltinSeedCount`).
  - [ ] Checker: `parse_and_check.cc` + `frontend/BUILD.bazel`.
  - [x] WAT (Slice 0): `wat/m16_math_min_list.wat` +
        `wat/m16_math_bit_shift.wat` authored, assembled, documented in
        `wat-traces.md` §M16.1/§M16.2.  [ ] register in
        `wat_runner_test.cc` when kernels land.
  - [ ] E2E: `e2e/m16_test.cc` + `e2e/BUILD.bazel`.
  - [ ] Conformance: `conformance/BUILD.bazel` + `run_conformance.cc`
        + `.baseline` + `README.md`.
  - [ ] Docs: tick `testing-checklist.md`; close this doc out.
