# M10 — Type conversions (`int(x)`, `string(x)`, `double(x)`, ...)

Status: **shipped 2026-05-14 (slices A–E + closeout).**

> **What landed.**  All five implementation slices A–E shipped
> end-to-end against the as-written plan, with two execution
> deltas worth flagging:
>
>   1. **`bool → int` / `bool → uint` / `bool → double` dropped.**
>      The plan §4.2 listed `bool_to_int64` / `bool_to_uint64` /
>      `bool_to_double` as in-scope, but cel-cpp's CHECKER does
>      NOT declare these overloads (only the runtime registers
>      them).  `int(true)` therefore checker-rejects, and the two
>      rows that exercised it were dropped from
>      `m10_test.cc::IntFamilyE2ETest`'s parameterized table.  A
>      v2-side checker extension for bool conversions is tracked
>      as future work in §9.
>   2. **`CEL_ERR_INVALID_ARGUMENT` deferred.**  The plan §4.5
>      proposed adding `CEL_ERR_INVALID_UTF8` (and by parallel
>      `CEL_ERR_INVALID_ARGUMENT` for parse-failure rows).  As
>      shipped, M10.C / M10.E reuse the existing
>      `CEL_ERR_OVERFLOW` code — semantically "can't represent
>      the result as the target type", which already covers the
>      M10.B numeric-conversion overflows.  The dedicated codes
>      land alongside the `api/error.h` mirror in a separate
>      slice.
>
> **As-shipped surface.**  `m10_test.cc` green: 87/87 rows
> (Identity ×6, IntFamily parameterized 7 + reject 5, UintFamily
> 8, DoubleFamily 6, StringParse ~20 admit + reject,
> StringParseBool parameterized truth-table, NumberFormat 10,
> BytesFamily 8 incl. UTF-8 reject matrix).
> `scripts/run_full_suite.sh --quick`: 8/8 PASS (M9 regression
> in `cel_runtime_wasm_test` / `wat_runner_test` from the
> `resolve_message_type_name` import was cleared by adding the
> matching no-op stub in both test harnesses).
> Conformance: `975 → 1058 PASS` (+83), `781 → 693 SKIP` (−88),
> `698 → 703 FAIL` (+5 — mostly classifier-tightening surface
> per `conformance-unlock-plan.md` Slice 3).

The plan covers every spec-named type-conversion overload whose
operand and result types are both already shippable in the v2
pipeline: bool / int / uint / double / string / bytes
inter-conversions, plus their identity arms.  Out of scope: the
`type(x)` and `dyn(x)` standard functions (M9 + M5.1.5
respectively, both shipped).

> Plan-vs-execution delta: the timestamp / duration conversion
> arms originally carved out for the "timestamps slice" (§2.2)
> **shipped at M7B (2026-05-16)** — `int(timestamp)`,
> `string(timestamp)`, `timestamp(string)`, `timestamp(int)`, the
> identity arms, and the matching duration variants.  See
> `m7b-duration-timestamp.md` for the as-shipped shape.  Affected
> rows in `conversions.textproto` graduated as part of M7B's
> +86-PASS unlock.

## 1. Why M10

Per `conformance/README.md`, after M9.A–F the
corpus sits at `pass=975 / skip=781 / fail=698 / total=2454`
(39.7%).  `conversions.textproto` accounts for **109 SKIPs**
(28 PASS today, 81 SKIP, 0 FAIL) — entirely from the
`int(x)` / `uint(x)` / `double(x)` / `string(x)` / `bytes(x)`
overload set the `OverloadTable` lists in
`kExplicitlyUnimplementedIds` (`overload_table.cc:268-352`).
This is the single largest "scope-not-yet-shipped" bucket
that's neither extension-library-shaped nor blocked on a
larger sibling slice (timestamps / wrappers / comprehensions).

Lighting up the in-scope conversion overloads (see §2.1) is a
mechanical "seed + body" exercise — the codegen path already
routes through `EmitGeneralCall`'s overload-id dispatch (every
shipped helper since M5.F follows the same pattern).  No new
ABI surface, no frontend rewrite, no wire-shape changes.

| Fixture | Today (PASS) | Post-M10 (estimate) | Driving family |
|---|---:|---:|---|
| `conversions.textproto` | 28 / 109 | +50 – +75 | int / uint / double / string / bytes families (timestamp / duration rows still SKIP — out of scope per §2.2) |
| `dynamic.textproto` (`dyn(int(x))`-shape rows) | 4 / 226 | +5 – +12 | M10 + M9.A's `dyn(scalar)` admit |
| `proto2.textproto` / `proto3.textproto` (rows that mix conversions with field reads) | 107 / 203 | +3 – +6 | int / uint family on enum-typed fields |
| `string.textproto` (`size('multibyte')` cohort + `string(b"...")` rows) | 40 / 51 | +1 – +3 | bytes→string + size-via-conversion |
| `enums.textproto` (`int(<enum>)` rows) | 52 / 85 | +2 – +5 | int family |
| `parse.textproto` (rows that test string formatting of literals) | 174 / 219 | +1 – +3 | int→string / double→string |
| **Total projected** | — | **+60 – +100 PASS** | — |

(Wide range because `conversions.textproto` mixes in-scope rows
with timestamp / duration rows that stay SKIP.  Lower bound
counts only the rows that pass cel-cpp checker resolution into a
shipped overload id; upper bound assumes the `dyn(int(x))` cohort
in `dynamic.textproto` also lights up.)

## 2. Scope

### 2.1 In-scope (per `langdef.md` §"Standard Definitions" — `bool` / `int` / `uint` / `double` / `string` / `bytes` headings)

The full list of overload ids this milestone seeds, grouped by
return-type family.  Every id below currently sits in
`overload_table.cc::kExplicitlyUnimplementedIds`; M10 moves
each into `kBuiltinSeeds` paired with a runtime helper.

#### bool family (2 overloads)

  - `bool_to_bool` — identity.  Seed to `cel_copy_slot`.
  - `string_to_bool` — accepts `"true"`, `"True"`, `"TRUE"`,
    `"t"`, `"1"` → `true`; `"false"`, `"False"`, `"FALSE"`,
    `"f"`, `"0"` → `false`; everything else → `CEL_ERROR
    (kInvalidArgument)`.  cel-cpp parity:
    `runtime/standard/type_conversion_functions.cc::RegisterBoolConversionFunctions`.

#### int family (5 overloads — timestamp arm deferred)

  - `int64_to_int64` — identity.  Seed to `cel_copy_slot`.
  - `bool_to_int64` — `true → 1`, `false → 0`.
  - `uint64_to_int64` — overflow-checked.  uint > INT64_MAX
    surfaces `CEL_ERR_OVERFLOW`.  cel-cpp uses
    `internal::CheckedUint64ToInt64`.
  - `double_to_int64` — truncates toward zero, overflow on
    `< INT64_MIN || > INT64_MAX || NaN`.  cel-cpp uses
    `internal::CheckedDoubleToInt64`.
  - `string_to_int64` — base-10 parse with optional leading `-`;
    rejects empty / non-digit / overflowing.  cel-cpp uses
    `absl::SimpleAtoi`.
  - **Out of scope: `timestamp_to_int64`** (epoch seconds) —
    timestamps slice.

#### uint family (4 overloads)

  - `uint64_to_uint64` — identity.  Seed to `cel_copy_slot`.
  - `int64_to_uint64` — overflow-checked.  int < 0 surfaces
    `CEL_ERR_OVERFLOW`.
  - `double_to_uint64` — truncates toward zero, overflow on
    `< 0 || > UINT64_MAX || NaN`.
  - `string_to_uint64` — base-10 parse; rejects empty / non-digit /
    leading `-` / overflowing.

#### double family (4 overloads)

  - `double_to_double` — identity.  Seed to `cel_copy_slot`.
  - `int64_to_double` — exact for `|v| < 2^53`; lossy beyond
    that but no error per langdef (matches IEEE 754 round-to-
    nearest).
  - `uint64_to_double` — same shape as `int64_to_double`.
  - `string_to_double` — parses floating-point including `NaN`,
    `Infinity`, scientific notation.  cel-cpp uses
    `absl::SimpleAtod`.

#### string family (6 overloads — timestamp / duration arms deferred)

  - `string_to_string` — identity.  Seed to `cel_copy_slot`.
  - `bool_to_string` — `true → "true"`, `false → "false"`.
  - `int64_to_string` — base-10 with optional leading `-`.
  - `uint64_to_string` — base-10.
  - `double_to_string` — hand-rolled formatter in
    `cel_runtime.c` (~120 lines).  Approach: `__builtin_isnan` /
    `__builtin_isinf` for specials; for finite numbers, route
    through a simple integer-extract pipeline that handles
    integral doubles exactly + a "%.17g"-equivalent for
    non-integrals.  See §4.4 for the format-spec contract and
    fallback path if cel-cpp byte-exactness becomes load-bearing.
  - `bytes_to_string` — copies bytes verbatim into the
    per-Eval arena, with **UTF-8 validation**.  Invalid UTF-8
    surfaces `CEL_ERR_INVALID_UTF8` (a new wire error code; see
    §4.5).
  - **Out of scope: `timestamp_to_string`, `duration_to_string`**
    — timestamps slice.

#### bytes family (2 overloads)

  - `bytes_to_bytes` — identity.  Seed to `cel_copy_slot`.
  - `string_to_bytes` — copy bytes verbatim into per-Eval
    arena (no validation; CEL strings are valid UTF-8 by
    construction).

### 2.2 Out-of-scope (deferred to the timestamps slice)

> **Status update (2026-05-16):** every row below shipped at
> M7B (see `m7b-duration-timestamp.md` slice M7B.D).  Kept here
> as the historical carve-out boundary that drove M10's
> sequencing.

The `timestamp(...)` and `duration(...)` constructors plus
every conversion that crosses a timestamp / duration boundary
are deferred:

  - `timestamp_to_timestamp`, `int64_to_timestamp`,
    `string_to_timestamp` — gate on the timestamps slice's
    construction path.
  - `duration_to_duration`, `int64_to_duration`,
    `string_to_duration` — gate on the duration parse logic
    (string suffix scanner per langdef §"duration").
  - `timestamp_to_int64`, `duration_to_int64` — accessor-shaped;
    can ship in the timestamps slice with the constructors.
  - `timestamp_to_string`, `duration_to_string` — RFC3339 +
    `<n>s` formats; lands with the timestamps slice's host
    primitives.

Reasoning: timestamp / duration parsing/formatting is a
~300-line cel-cpp surface (`internal/time.cc` +
`absl::ParseTime`/`FormatTime`).  Bundling it into M10 would
double the scope; the timestamps slice plan should own it.
M10 closes out cleanly when every non-timestamp conversion
ships.

### 2.3 Out-of-scope (separate concerns, named explicitly)

  - **`type(x)` and `dyn(x)`** — both shipped (M9 / M5
    passthrough).  The `type` and `to_dyn` overload ids stay in
    `kExplicitlyUnimplementedIds` (the latter because `dyn` is
    a static-subset gate inline at codegen, not a runtime
    helper).
  - **Cross-numeric arithmetic / comparison** — the M5.B step 2
    cross-type ladder (`add_int64_uint64` etc.) handles
    arithmetic / comparison without needing explicit
    `int(x) + 1u` rewrites.  M10 only adds the explicit
    user-facing conversion form; nothing changes for the
    cross-type math kernels.
  - **`bytes→string` for non-UTF-8 input** — surfaces a clean
    `CEL_ERR_INVALID_UTF8` error per langdef "errors for
    invalid code points".  No fallback / lossy mode.
  - **String parsing of integer literals with `0x` / `0b` / `0o`
    prefixes** — cel-cpp's `SimpleAtoi` accepts decimal only;
    M10 mirrors that.  Hex / octal / binary lit parsing is a
    cel-cpp deviation if it ever surfaces, not an M10 concern.
  - **`string(<aggregate>)` / `string(<message>)`** — not in
    cel-cpp's overload set (the spec's `string` overloads cover
    only scalar inputs).  Checker rejects.

## 3. Spec-mandated semantics

Citations from `doc/langdef.md` §"Standard Definitions" + cel-cpp
reference: `third_party/cel-cpp/runtime/standard/type_conversion_functions.cc`.
Per CLAUDE.md "Testing principles", every assertion below has
a test row in §6.

### 3.1 Numeric overflow + range rules (langdef §"int" / §"uint" / §"double")

  - **`int(double)`** — "rounds toward zero, errors if out of
    range" (langdef line 2121).  `INT64_MIN` and `INT64_MAX`
    are inclusive bounds.  NaN errors.  cel-cpp's
    `CheckedDoubleToInt64`: `if (v < kMinDoubleRepresentable ||
    v >= kMaxDoubleRepresentable + 1.0 || isnan(v)) → error`.
    The bounds use `2^63` (not `2^63 - 1`) on the upper edge
    because the largest representable double less than
    `2^63` rounds to `INT64_MAX`.
  - **`int(uint)`** — values > `INT64_MAX` (= `2^63 - 1`)
    overflow.  cel-cpp's `CheckedUint64ToInt64`.
  - **`uint(int)`** — negative values overflow.  cel-cpp's
    `CheckedInt64ToUint64`.
  - **`uint(double)`** — same shape as `int(double)`; bounds
    are `0` and `UINT64_MAX`.  Negative double → error; NaN →
    error.
  - **`double(int)` / `double(uint)`** — never errors.  Lossy
    for `|v| ≥ 2^53` per IEEE 754 round-to-nearest; langdef
    doesn't require precision preservation here, and cel-cpp
    does not check.

### 3.2 String parsing (langdef §"int" / §"uint" / §"double" + cel-cpp `absl::SimpleAtoi` / `SimpleAtod`)

  - **`int(string)`** — base-10 with optional leading `-`.
    Empty string, leading-zero non-canonical (`"007"` is OK
    per cel-cpp; `absl::SimpleAtoi` admits it), trailing
    whitespace, non-digit chars → `CEL_ERR_INVALID_ARGUMENT`
    with a "cannot convert string to int" message.
    Overflow → same error code (cel-cpp returns the SimpleAtoi
    failure verbatim).
  - **`uint(string)`** — base-10 with NO leading `-`.  cel-cpp
    rejects negative strings as a SimpleAtoi failure rather
    than a separate range check.  Otherwise same as `int`.
  - **`double(string)`** — admits standard floating-point
    representations: `123`, `1.5`, `1e10`, `1.5e-3`, `inf`,
    `Infinity`, `nan`, `NaN`, `-0.0`, hex-float (cel-cpp's
    `SimpleAtod` accepts all of these).  Empty / non-numeric
    → `CEL_ERR_INVALID_ARGUMENT`.
  - **`bool(string)`** — exact-string match against the 10-row
    truth table at langdef line 2030 + cel-cpp ref impl
    (`true`/`True`/`TRUE`/`t`/`1` → true; `false`/`False`/
    `FALSE`/`f`/`0` → false; otherwise error).

### 3.3 Number → string formatting (langdef §"string" + cel-cpp `FormatDouble`)

  - **`string(int)`** — base-10 with optional leading `-`
    (cel-cpp uses `absl::StrCat`).
  - **`string(uint)`** — base-10 (cel-cpp uses `absl::StrCat`;
    note: NO `u` suffix — `string(7u)` returns `"7"`, not
    `"7u"`, despite langdef's example line 2163 which is
    misleading).
  - **`string(double)`** — **precision-preserving** per cel-cpp's
    runtime option `enable_precision_preserving_double_format`
    (defaults true in v2).  Format: `to_chars(v, ...,
    chars_format::general)` produces the shortest round-trip
    representation.  Fallback: `absl::StrFormat("%.17g", v)`
    on non-conforming libc / wasm32 freestanding.  M10 routes
    this through a **host trampoline** (§4.4) — hand-rolling
    correct double formatting in C is non-trivial and
    out-of-scope.
  - **`string(bool)`** — `"true"` / `"false"` exactly.

### 3.4 Bytes ↔ string (langdef §"string" + §"bytes")

  - **`string(bytes)`** — UTF-8 validation: per langdef line
    2152-2153, "errors for invalid code points".  cel-cpp uses
    `internal::Utf8IsValid` which scans the byte sequence
    against the RFC3629 UTF-8 rules: every byte conforms to
    the leading-byte / continuation-byte pattern, no
    overlong encodings, no surrogates.  Invalid → error.
  - **`bytes(string)`** — copy verbatim.  CEL strings are by
    construction valid UTF-8 (cel-cpp's parser rejects invalid
    string literals); no validation needed.

### 3.5 Identity overloads

`bool_to_bool`, `int64_to_int64`, `uint64_to_uint64`,
`double_to_double`, `string_to_string`, `bytes_to_bytes` are all
true identity functions.  They route through the existing
`cel_copy_slot(out_slot, in_slot)` helper (M5.G) — no new
runtime body needed.  Seeded as `OverloadId →
{kCelRuntime, "cel_copy_slot"}` rows in `kBuiltinSeeds`.

### 3.6 Error / unknown propagation (langdef §"Error propagation" + §"Unknowns")

Every conversion helper absorbs CEL_ERROR / CEL_UNKNOWN on
the operand per the standard slot-out helper contract
(`absorb_3vl_unary` in `cel_runtime.c`).  Same shape as every
M5.F arm.

## 4. Architecture

### 4.1 OverloadTable seed updates

`compiler/codegen/overload_table.cc`:

  - **Remove** every M10-in-scope id from
    `kExplicitlyUnimplementedIds` (line 268).  Bump the
    `std::array<absl::string_view, N>` size constant — count
    the removed ids precisely (~24 ids out, see §2.1 for the
    list).  Per `overload_table_test.cc:41`, the
    `kBuiltinSeedCount` constant is similarly load-bearing —
    update both in the same commit.
  - **Add** to `kBuiltinSeeds` (line 82):
      - 6 identity rows seeded to `cel_copy_slot`.
      - ~17 conversion rows seeded to per-conversion runtime
        helpers (see §4.2 for naming).
      - 1 row (`double_to_string`) seeded to a host-imported
        helper (see §4.4).
  - Bump the `std::array<Seed, 86>` size constant in
    `kBuiltinSeeds` by the corresponding delta (86 → 86 +
    24).

The matching `overload_table_test.cc::kBuiltinSeedCount`
constant must be updated in the same commit.

### 4.2 Runtime helpers — `cel_runtime.c` + `cel_convert.h`

**Every conversion lands as a pure-runtime helper** in
`cel_runtime.c` — no host trampolines.  C-level casts and
hand-rolled byte-loop parsers cover all 17 in-scope
conversions; the wasm runtime stays freestanding.

New header `runtime/cel_convert.h` (parallels
`cel_arith.h` / `cel_string_ops.h` / `cel_type.h` from M9.B).
Declarations only; bodies in `cel_runtime.c`.  Naming follows
the existing convention `cel_<from>_to_<to>_at_v` for unary
slot-out helpers.

#### Pure-runtime helpers (17 unary helpers)

| Overload id | Helper symbol | Body summary |
|---|---|---|
| `bool_to_int64` | `cel_bool_to_int_at_v` | `(int64_t)(v ? 1 : 0)` |
| `uint64_to_int64` | `cel_uint_to_int_at_v` | `if (v > INT64_MAX) poison(kOverflow); else (int64_t)v` |
| `double_to_int64` | `cel_double_to_int_at_v` | `__builtin_isnan(v) → kOverflow`; bounds-check; `(int64_t)trunc(v)` |
| `int64_to_uint64` | `cel_int_to_uint_at_v` | `if (v < 0) poison(kOverflow); else (uint64_t)v` |
| `double_to_uint64` | `cel_double_to_uint_at_v` | NaN + bounds + trunc; same shape as `double_to_int64` |
| `int64_to_double` | `cel_int_to_double_at_v` | `(double)v` — straight cast |
| `uint64_to_double` | `cel_uint_to_double_at_v` | `(double)v` — straight cast |
| `int64_to_string` | `cel_int_to_string_at_v` | hand-rolled itoa (~20 lines), arena alloc |
| `uint64_to_string` | `cel_uint_to_string_at_v` | hand-rolled utoa (~15 lines), arena alloc |
| `bool_to_string` | `cel_bool_to_string_at_v` | rodata-resident `"true"` / `"false"` constants — 5 lines |
| `double_to_string` | `cel_double_to_string_at_v` | hand-rolled dtoa (~120 lines, see §4.4) |
| `string_to_int64` | `cel_string_to_int_at_v` | hand-rolled atoi (decimal + optional `-`); ~40 lines |
| `string_to_uint64` | `cel_string_to_uint_at_v` | hand-rolled atou (decimal, no leading `-`); ~30 lines |
| `string_to_double` | `cel_string_to_double_at_v` | hand-rolled atof (digits, `.`, `e`/`E`, sign, `inf`/`nan` literals); ~80 lines |
| `string_to_bool` | `cel_string_to_bool_at_v` | exact-string match against 10-row truth table |
| `string_to_bytes` | `cel_string_to_bytes_at_v` | arena copy + kind flip CEL_STRING → CEL_BYTES |
| `bytes_to_string` | `cel_bytes_to_string_at_v` | UTF-8 validate + arena copy + kind flip CEL_BYTES → CEL_STRING |

**No host trampolines, no new ABI table, no public-API changes.**
Every helper sits behind the existing slot-out helper ABI —
`(uint32_t out_slot, uint32_t in_slot) → void` — and routes
through `EmitGeneralCall` automatically once the seed lands.

All bodies follow the existing pattern in `cel_runtime.c`:

```c
void cel_<from>_to_<to>_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind != CEL_<FROM>) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // ... conversion body ...
}
```

#### Subroutines (shared by string parsers / formatters)

Define in `cel_runtime.c` as static helpers:

  - `static int parse_int64(const uint8_t* p, uint32_t len,
    int64_t* out)` — returns 1 on success, 0 on failure.
    Optional leading `-`, decimal digits only, overflow check
    via `__builtin_mul_overflow` / `__builtin_add_overflow`.
  - `static int parse_uint64(const uint8_t* p, uint32_t len,
    uint64_t* out)` — same shape, no leading `-`.
  - `static int parse_double(const uint8_t* p, uint32_t len,
    double* out)` — handles `[+-]?(<digits>(.<digits>)?)|nan|
    inf(inity)?` with `[eE][+-]?<digits>` exponent.  ~80
    lines.  Spec: cel-cpp uses `absl::SimpleAtod`; we mirror
    its admit-set.
  - `static int utf8_valid(const uint8_t* p, uint32_t len)` —
    RFC3629 byte-wise validator (~30 lines).  Mirrors
    `cel-cpp/internal/utf8.cc::Utf8IsValid`.
  - `static uint32_t write_int_decimal(uint8_t* dst,
    int64_t v)` — hand-rolled itoa returning the byte count
    written.  Reverse-then-reverse pattern (~20 lines).
  - `static uint32_t write_uint_decimal(uint8_t* dst,
    uint64_t v)` — same shape, no sign.

#### Arena allocation pattern

String / bytes outputs allocate in the per-Eval arena via
`cel_alloc(n)`, identical to the M9.B `cel_type_of_at_v`
pattern.  Out-of-arena → poison kOverflow.

### 4.3 Identity-overload routing

The 6 identity overloads (`bool_to_bool`, `int64_to_int64`,
etc.) seed directly to `cel_copy_slot` — no new helper bodies.
This is purely a `kBuiltinSeeds` table change.

`cel_copy_slot(out_slot, in_slot)` is already exported from
the runtime (`engine.cc:206` lists it; `BUILD.bazel`
`-Wl,--export=cel_copy_slot`).  No engine.cc changes needed
for this arm.

### 4.4 Hand-rolled `double_to_string` — format spec + fallback path

`cel_double_to_string_at_v` lives in `cel_runtime.c`, no
host call.  cel-cpp's `FormatDouble` uses `std::to_chars`
with `chars_format::general` (shortest-round-trip) and falls
back to `absl::StrFormat("%.17g", v)`.  We don't have either
on freestanding wasm32, so we ship a hand-rolled formatter
sized to "correct for normal cases, good enough for everything
else":

```c
// Pseudocode — actual impl ~120 lines.
void cel_double_to_string_at_v(out_slot, in_slot) {
  // Specials first.
  if (__builtin_isnan(v))    write "nan";
  if (__builtin_isinf(v))    write v > 0 ? "+Inf" : "-Inf";
  if (v == 0.0) {
    // Preserve sign of zero per cel-cpp.
    write copysign(1.0, v) < 0 ? "-0" : "0";
  }
  // Integral doubles in int64 range — exact via int64 path.
  if (v == trunc(v) && fabs(v) < 1e15) {
    cel_int_to_string_at_v on (int64_t)v;
    return;
  }
  // General case: %.17g-equivalent — extract integer + fractional
  // parts via repeated *10, write digits, place decimal point.
  // Produces 17 significant digits; matches cel-cpp's fallback
  // path (NOT the to_chars shortest-round-trip path).
  ...
}
```

**Format-spec contract (R5 mitigation).**  Test rows assert:

  - **Trivial values** (`0.0`, `-0.0`, `1.5`, `-1.5`,
    `1e10`) — pin exact strings.
  - **Specials** (`nan`, `Infinity`) — pin lowercase / `+Inf`
    spelling matching our impl.
  - **Round-trip equality** (`double(string(d)) == d`) for
    representative non-trivial doubles — this is the
    load-bearing assertion.  As long as round-trip holds, the
    exact byte representation can drift from cel-cpp's
    `to_chars` output without breaking semantics.

If a conformance row demands byte-exact match against
cel-cpp's `to_chars` shortest-round-trip output and our `%.17g`-
equivalent disagrees, the **fallback path** is to swap the
helper body for a Grisu / Ryu impl (vendored from cel-cpp's
deps or a permissive third-party single-file lib like
ryu.h).  This is a body-swap, not an architectural change —
the helper symbol + ABI are unchanged.  Tracked in §9 future
work.

### 4.5 New error code — `CEL_ERR_INVALID_UTF8`

`runtime/cel_data.h` grows one wire error code:

```c
enum {
  // ... existing ...
  // Returned by `bytes_to_string` / `string(bytes)` when the
  // input bytes are not valid UTF-8 per RFC3629.  Per langdef
  // §"string": `string(bytes)` errors for invalid code points.
  CEL_ERR_INVALID_UTF8 = 18,
};
```

Mirror in `eval/error.h::ErrorCode` (the user-facing
enum) and `eval/internal/cel_host.cc::ErrorCodeName`
+ the runner's error-message map (so conformance rows whose
matcher expects `"invalid_argument"` continue to match via
`LooseMessageMatch`).

### 4.6 Conformance runner

No runner changes.  All M10 conversions are scalar-matcher
rows that route through the existing `RunValueBranch` /
`RunEvalErrorBranch` paths.  The 81 SKIPped
`conversions.textproto` rows graduate to PASS/FAIL the
moment their overload ids resolve in the OverloadTable —
`ClassifyCompileFailure` no longer flags them as
"compile unimplemented: ...".

### 4.7 No public-API changes

`cel::Value` / `cel::CelType` surfaces unchanged.  No new
factory methods, no new accessors.  M10 is entirely a
codegen + runtime + overload-seed slice.

## 5. Sequencing — slices

Five slices, each shippable independently with its own
test class that turns green.  Effort sized as small (one
focused change, ≤200 LoC + tests) / medium (≤500 LoC +
tests + WAT trace) / large (cross-cutting change touching
ResolvePass + LayoutPass + codegen + host + ABI + tests).

### M10.A — identity overloads (6 seeds, no new bodies)

Land the 6 identity conversions: `bool_to_bool`,
`int64_to_int64`, `uint64_to_uint64`, `double_to_double`,
`string_to_string`, `bytes_to_bytes`.  All seed to
`cel_copy_slot` — purely table-data changes.

  - **OverloadTable.**  Move 6 ids from
    `kExplicitlyUnimplementedIds` to `kBuiltinSeeds`; bump the
    array sizes in both the source file and the test
    (`overload_table_test.cc:41 kBuiltinSeedCount`).
  - **Tests.**  6 rows in `m10_test.cc::IdentityE2ETest`,
    one per kind — `int(7) == 7`, `bool(true) == true`, etc.
  - **Conformance unlock.**  ~+15 PASS in
    `conversions.textproto` (every `<scalar>(<scalar>) ==
    <scalar>` row).
  - **Effort.**  Small.

### M10.B — numeric inter-conversions (8 helpers)

Land the bool↔int / int↔uint / int↔double / uint↔double cross
arms: 8 unary runtime helpers in `cel_runtime.c`.

  - **OverloadTable.**  8 new seeds.
  - **Runtime.**  8 helpers, all pure-runtime, all
    overflow-checked where langdef requires.
  - **Wasm exports.**  8 new `-Wl,--export` entries in
    `runtime/BUILD.bazel`.
  - **Engine wiring.**  8 new symbols in `engine.cc::
    BindAllRuntimeExports::kRuntimeExports`.
  - **Tests.**  `m10_test.cc::IntFamilyE2ETest` (~12 rows),
    `UintFamilyE2ETest` (~10 rows), `DoubleFamilyE2ETest` (~6
    rows).  Boundary coverage per §6.1.
  - **Conformance unlock.**  ~+20 PASS in
    `conversions.textproto` (numeric cross-rows).
  - **Effort.**  Medium.

### M10.C — string parsing (4 helpers + parse subroutines)

Land the string → numeric / bool overloads.  The hand-rolled
parse subroutines (`parse_int64`, `parse_uint64`,
`parse_double`) are shared.

  - **Runtime.**  4 helpers + 3 parse subroutines + 1 truth-
    table for `string_to_bool`.  ~150-200 lines total.
  - **OverloadTable.**  4 new seeds.
  - **Wasm exports + engine wiring.**  4 new symbols each.
  - **Tests.**  `m10_test.cc::StringParseE2ETest` (~16 rows
    per §6.2 — admit + reject for each parser).
  - **Conformance unlock.**  ~+15 PASS — string→numeric rows
    in `conversions.textproto`.
  - **Effort.**  Medium.

### M10.D — number → string formatting (4 pure-runtime helpers)

Land `int_to_string`, `uint_to_string`, `bool_to_string`,
`double_to_string` — all pure runtime.

  - **Runtime.**  4 hand-rolled helpers.  `int` / `uint` /
    `bool` are trivial (~15 lines each).  `double` is the
    locus (~120 lines per §4.4).
  - **OverloadTable.**  4 new seeds, all
    `{kCelRuntime, "cel_<x>_to_string_at_v"}`.
  - **Wasm exports + engine wiring.**  4 new symbols each.
  - **Tests.**  `m10_test.cc::NumberFormatE2ETest` (~10 rows
    — boundaries + zero / negative / Inf / NaN for double, plus
    round-trip checks for non-trivial doubles per §4.4).
  - **Conformance unlock.**  ~+10 PASS — numeric→string rows.
  - **Effort.**  Medium (most of the size is `parse_double` /
    `format_double`; the rest of the slice is small).

### M10.E — bytes ↔ string (2 helpers, UTF-8 validator)

Land `bytes_to_string` + `string_to_bytes`.  `bytes_to_string`
is the slice's complexity locus — UTF-8 validation per
RFC3629.

  - **Runtime.**  2 helpers + `utf8_valid` subroutine.
  - **New error code.**  `CEL_ERR_INVALID_UTF8` in
    `cel_data.h` + mirror in `error.h` + runner mapping.
  - **OverloadTable.**  2 new seeds.
  - **Tests.**  `m10_test.cc::BytesFamilyE2ETest` (~8 rows —
    valid + invalid UTF-8 across the byte-pattern matrix:
    overlong, surrogate, truncated, single-byte 0x00-0x7F,
    two-byte / three-byte / four-byte boundaries).
  - **Conformance unlock.**  ~+5-10 PASS in
    `conversions.textproto` + ~+1-3 in `string.textproto`.
  - **Effort.**  Small (validator is well-bounded).

### M10.F — closeout

  - Run `bazel run //conformance:run_conformance`;
    record the post-M10 deltas in
    `conformance/README.md`.
  - Run `scripts/run_full_suite.sh` (closeout gate per
    CLAUDE.md).
  - Flip this doc's status header to `shipped YYYY-MM-DD`
    with the "what landed" paragraph.
  - Tick `testing-checklist.md` rows under "Rewrite M10":
    every new conversion arm × pipeline-stage cell.
  - Reconcile sibling docs: any future timestamps-slice plan
    references the carved-out timestamp/duration arms.
  - Append a "Future work" section: timestamp/duration
    conversions, hex-float string parsing, locale-aware double
    formatting (none in scope today; surface for follow-up).

## 6. Test matrix (load-bearing)

Per CLAUDE.md "Cover the edge-case matrix — this is a
compiler", every combination below MUST have at least one
explicit test.  Negative coverage (rejection cases) is ≥ 30%
of the total per the same rule.

### 6.1 Numeric inter-conversion matrix (M10.B)

| Conversion | Boundary rows |
|---|---|
| `int(true)` / `int(false)` | both polarities |
| `int(0u)` / `int(INT64_MAX as uint)` / `int((INT64_MAX+1) as uint)` | identity / boundary / overflow |
| `int(0.0)` / `int(1.7)` (truncates to 1) / `int(-1.7)` (truncates to -1) / `int(INT64_MIN as double)` / `int(2^63 as double)` overflow / `int(NaN)` overflow / `int(Infinity)` overflow | trunc / boundary / NaN / Inf |
| `uint(0)` / `uint(-1)` overflow / `uint(INT64_MAX)` | sign / boundary |
| `uint(0.0)` / `uint(-1.0)` overflow / `uint(2^64-1 as double)` lossy / `uint(NaN)` / `uint(Infinity)` | trunc / boundary |
| `double(0)` / `double(INT64_MAX)` / `double(INT64_MIN)` | exact / boundary |
| `double(0u)` / `double(UINT64_MAX as uint)` | exact / boundary |

~30 rows total; consolidated into `m10_test.cc::IntFamilyE2ETest`
TEST_P + similar for uint / double.

### 6.2 String parsing matrix (M10.C)

| Conversion | Admit rows | Reject rows |
|---|---|---|
| `int("0")` / `int("-1")` / `int("9223372036854775807")` (INT64_MAX) / `int("-9223372036854775808")` (INT64_MIN) | identity / negative / boundaries | `int("")` / `int("abc")` / `int("9223372036854775808")` overflow / `int("1.5")` / `int(" 5")` whitespace |
| `uint("0")` / `uint("18446744073709551615")` (UINT64_MAX) | identity / boundary | `uint("-1")` (sign rejected) / `uint("")` / `uint("abc")` / `uint("18446744073709551616")` overflow |
| `double("0")` / `double("1.5")` / `double("-1.5e10")` / `double("inf")` / `double("Infinity")` / `double("nan")` / `double("NaN")` / `double("-0.0")` | identity / scientific / specials / signed zero | `double("")` / `double("abc")` / `double("1.2.3")` |
| `bool("true")` / `bool("True")` / `bool("TRUE")` / `bool("t")` / `bool("1")` / `bool("false")` / `bool("False")` / `bool("FALSE")` / `bool("f")` / `bool("0")` | full truth-table | `bool("yes")` / `bool("True ")` (trailing space) / `bool("")` / `bool("TruE")` (mixed case) |

~30 rows total — TEST_P matrix per family.

### 6.3 Number → string formatting matrix (M10.D)

| Conversion | Rows |
|---|---|
| `string(0)` → `"0"` / `string(-1)` → `"-1"` / `string(INT64_MAX)` / `string(INT64_MIN)` | base / boundary |
| `string(0u)` → `"0"` / `string(UINT64_MAX)` → `"18446744073709551615"` | base / boundary |
| `string(0.0)` → `"0"` / `string(-0.0)` → `"-0"` / `string(1.5)` / `string(1e-10)` / `string(NaN)` → `"nan"` / `string(Infinity)` → `"+Inf"` | base / specials |
| `string(true)` → `"true"` / `string(false)` → `"false"` | trivial |

Note on double formatting: cel-cpp's `to_chars` impl produces
shortest round-trip output, but the EXACT spelling for NaN /
Inf depends on libc.  The test asserts
`string(NaN) == string(NaN)` (both produce `"nan"` lowercase
on glibc/libc++) and pins specific values for normal numbers.

### 6.4 Bytes ↔ string + UTF-8 matrix (M10.E)

| Conversion | Rows |
|---|---|
| `bytes("hello")` / `bytes("")` / `bytes("☃")` (UTF-8 input) | trivial |
| `string(b"hello")` / `string(b"")` / `string(b"\xe2\x98\x83")` (valid UTF-8 snowman) | trivial / multi-byte |
| `string(b"\xff")` invalid leading byte | reject |
| `string(b"\xc0\x80")` overlong NUL | reject |
| `string(b"\xed\xa0\x80")` UTF-16 surrogate | reject |
| `string(b"\xe0\x80\x80")` overlong | reject |
| `string(b"\x80")` orphan continuation byte | reject |
| `string(b"\xc2")` truncated 2-byte | reject |

8 rows in `BytesFamilyE2ETest`.

### 6.5 Negative / rejection matrix

  - **`int("foo")`** — not a number → error.
  - **`int(1.5e100)`** — out of range → overflow error.
  - **`uint(-5)`** — negative → overflow error.
  - **`int(NaN)`** — NaN → overflow error.
  - **`string(b"\xff")`** — invalid UTF-8 → invalid_utf8 error.
  - **`int(timestamp(0))`** — out of M10 scope (timestamps
    slice).  Pin as `GTEST_SKIP` so the test row is visible
    when the slice lands.
  - **`int([1, 2, 3])`** — checker-rejects (no overload for
    list operand).
  - **`int({"k": 1})`** — checker-rejects.

8 rows in `RejectE2ETest`.

### 6.6 Test placement

  - `runtime/cel_arith_test.cc` (or new
    `cel_convert_test.cc`) — unit tests for each pure-runtime
    helper, exercising the conversion + 3VL absorption +
    type-mismatch poison.
  - `eval/internal/cel_host_test.cc` — Layer-2
    `CelHostDoubleToStringImpl` unit (precision + arena
    write).
  - `compiler/codegen/expr_lower_test.cc` — codegen shape
    for `int(x)` calls (asserts the emitted wasm calls the
    right helper symbol).
  - `conformance/binding_marshal_test.cc` — no
    new tests; existing surface unchanged.
  - `e2e/m10_test.cc` (new) — load-bearing e2e
    spec; classes per §6.1–§6.5 above.
  - `doc/implementation-plan/rewrite/wat/16_double_to_string.wat`
    (M10.D).

## 7. Risks + open questions

Ranked highest → lowest.

  - **R1 — Hand-rolled `parse_double` correctness.**  Floating-
    point string parsing has many edge cases: scientific
    notation, leading/trailing zeros, special tokens (NaN /
    Inf / Infinity / inf / nan in mixed case), explicit
    positive sign.  Mitigation: pin the admit-set to cel-cpp's
    `absl::SimpleAtod` behavior verbatim — that's our spec.
    Test matrix in §6.2 explicitly covers each case.
    Considered: punt to a host trampoline for `double_to_string`
    is precedented (§4.4); we could do the same for
    `string_to_double` if the parser turns out gnarly.  M10.C
    decides at implementation time based on parse-impl LoC
    creep.

  - **R2 — Wasm linker dropping the 8 new exports.**  The
    `runtime/BUILD.bazel` genrule is manual-tag; touching
    `cel_runtime.c` requires explicit
    `bazel build //runtime:cel_runtime_wasm_bytes`
    invocation to refresh the cached wasm bytes.  Mitigation:
    document this in the M10.B + M10.C + M10.D commit messages
    so a future builder remembers; the closeout runs the
    full `bazel test //compiler_v2/...` which catches a stale
    wasm via the m2/m4/m5/m7/m9 e2e tests' link checks.

  - **R3 — Overload-id partition test drift.**  The
    `OverloadTableTest::CoverageTripwireClassifiesEveryStandardId`
    test (overload_table_test.cc) asserts every cel-cpp
    `StandardOverloadIds::k*` value is in either
    `kBuiltinSeeds` or `kExplicitlyUnimplementedIds`, never
    both / never neither.  Moving 24 ids between buckets must
    keep this partition sound.  Mitigation: the tripwire
    fails loud on any mis-partitioning; closeout test run
    catches it.

  - **R4 — `string_to_double` behavior on hex / octal / binary
    string inputs.**  `absl::SimpleAtod` admits hex floats
    (`"0x1p10"` = 1024.0).  Mitigation: pin the admit-set to
    decimal-only at M10.C; if a corpus row surfaces requiring
    hex floats, M10.G future work.

  - **R5 — Hand-rolled `double_to_string` byte-exactness vs
    cel-cpp.**  cel-cpp uses `std::to_chars` (shortest
    round-trip) with `%.17g` fallback.  Our hand-rolled
    formatter (§4.4) targets `%.17g`-equivalent semantics —
    correct for round-trip, but not byte-identical to
    `to_chars` for values where shortest-round-trip differs
    (e.g. `0.1` formats as `"0.1"` under to_chars vs
    `"0.10000000000000001"` under `%.17g`).  Mitigation:
    M10.D tests use round-trip assertions
    (`double(string(d)) == d`) for non-trivial values; pin
    exact strings only for unambiguous cases (`0`, `1.5`,
    `nan`, `+Inf`).  If a conformance row demands byte-exact
    `to_chars` output, swap the helper body for a Grisu/Ryu
    impl — body-swap, not architectural change.

  - **R6 — `parse_int64` overflow on `INT64_MIN`.**  `INT64_MIN`
    is `-9223372036854775808` — the absolute value
    (`9223372036854775808`) overflows `int64_t`.  Mitigation:
    parse as `uint64_t` accumulator; check `acc <= INT64_MAX
    + (sign ? 1 : 0)`; cast at the end.  Standard pattern.

## 8. Out-of-scope (re-stated)

  - Timestamp / duration conversions — sibling timestamps
    slice (§2.2).  *Shipped at M7B (2026-05-16); see
    `m7b-duration-timestamp.md` for the full surface.*
  - `type(x)` / `dyn(x)` — already shipped (M9 / M5).
  - Hex / octal / binary integer string parsing — cel-cpp
    deviation territory.
  - Locale-aware number formatting — out of CEL spec.
  - `string(<aggregate>)` / `string(<message>)` — not in
    cel-cpp's overload set; checker rejects.

## 9. Future work

  - **Timestamp / duration conversions** (~30 conversion arms)
    — **shipped at M7B (2026-05-16)**.  M7B.D landed the 12
    conversion ids (`int64_to_timestamp` /
    `string_to_timestamp` / `timestamp_to_string` / identity /
    matching duration variants); M7B.B-E landed the arithmetic,
    ordering, and accessor surfaces.
  - **Hex float string parsing** — `0x1p10` and friends.
    Surface if a fixture row demands it.
  - **Shortest-round-trip `double_to_string` (Grisu/Ryu)** —
    if a fixture row demands byte-exact match against
    cel-cpp's `to_chars` output, swap the §4.4 hand-rolled
    `%.17g`-equivalent for a vendored Ryu/Grisu impl.  The
    helper symbol + ABI are unchanged.
  - **`int(x)` / `uint(x)` / `double(x)` cross-numeric inlining
    in the cel-cpp checker's constant-folding** — checker
    sometimes folds `int(1u)` to `1`; verify no test
    regression.  No explicit M10 work; surface if a row fails.
