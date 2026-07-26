# Fuzz coverage checklist — every exposed function

This is the **living checklist** of every overload the compiler can
codegen, and whether the differential fuzzer
([`e2e/fuzz/`](README.md)) generates it. The goal is to drive every
function through the cel-cpp oracle and check it off.

**Source of truth:** `compiler/codegen/overload_table.cc` — the
`Seed{"<overload_id>", …}` entries are exactly the overloads our
compiler lowers. **241 overloads** as of 2026-06-11. Regenerate the
list with:

```bash
grep -oE 'Seed\{"[a-zA-Z0-9_]+"' compiler/codegen/overload_table.cc \
  | sed 's/Seed{"//;s/"//' | sort
```

When `overload_table.cc` grows, add the new IDs here and to the
grammar. A row is **✅ done** only when the grammar emits it AND a
mining pass over its target type ran clean (or its divergence is
pinned in `known_bugs_test.cc`).

Legend: ✅ all overloads in the family generated · 🟡 partial (see
note) · ⬜ none generated yet.

---

## Summary

| Category | Overloads | Status |
| --- | ---: | --- |
| Arithmetic (numeric) | 17 | 🟡 numeric core; temporal/list `+` open |
| Comparison (`==` `!=` `<` `<=` `>` `>=`) | 57 | 🟡 same-type numeric + string/bytes/bool eq + string/bytes ordering + temporal done; cross-type ⊘ (dyn-only, out of subset) |
| Logical (`&&` `\|\|` `!`) | 3 | ✅ |
| `size` | 4 | ✅ |
| `in` | 2 | ✅ |
| Ternary `?:` | (macro) | ✅ |
| Comprehensions (`exists`/`all`/`exists_one`/`filter`/`map`) | (macros) | ✅ over lists; maps predicate-only |
| Aggregates (list/map literals, nested) | — | ✅ |
| String functions | ~27 | 🟡 all but the two-arg pos/limit forms; two-arg `substring` withheld (oracle off-by-one, pinned) |
| Type conversions | ~30 | 🟡 cross-numeric + string(x) + bytes/bool done; numeric-string-leaf + duration/timestamp parse open |
| **math_ext** | 28 | ✅ |
| **net_ext** | 20 | ⬜ blocked — needs `CelType` opaque-type support |
| **timestamp accessors** | 23 | ✅ no-tz + `_with_tz` forms done |
| **duration accessors** | 7 | ✅ |
| **encoders (base64)** | 2 | ✅ |
| **optionals** | ~14 | ⬜ blocked — needs `CelType` optional-type support |
| `type()` | 1 | ⬜ |

The remaining ⬜ / open rows are the work queue, roughly by bug-yield,
tracked in
[`m30-fuzz-full-dialect.md`](../../doc/implementation-plan/rewrite/m30-fuzz-full-dialect.md):
the conversion remainder (numeric-shaped string leaves, duration /
timestamp parse) and the two-arg pos/limit string forms are the
near-term yield; `net_ext` + `optionals` (34 overloads) are blocked on
a `shared/type.h` opaque/optional type-vocabulary extension; `type()`
is unstarted.

---

## PBT-found bugs (the trophy case)

Every divergence this fuzzer surfaced is pinned as a `Pbt*` test in
`e2e/known_bugs_test.cc`. **LIVE** = the fix shipped and the test is a
live regression guard; **PINNED** = a `GTEST_SKIP`'d over-permissiveness
we knowingly carry (delete the skip to fix). Grep `Pbt` in that file
for the full reproducer + cel-cpp citation + fix recipe on each.

| `Pbt*` test | Status | What it pins | Fix layer |
| --- | --- | --- | --- |
| `PbtSplitComputedReceiverSlotAlias` | LIVE | `split` on a computed receiver returned arena-header garbage (output slot aliased the source ptr) | runtime `cel_string_ext_list.cc` `DoSplit` |
| `PbtTernaryInsideIntSubtract` | LIVE | ternary inside an int subtract read the wrong slot | codegen slot allocation |
| `PbtExistsOneInTernaryCondBytes` | LIVE | `exists_one` result lived in `comp.result()`'s slot, not accu_var's; ternary cond mis-routed | codegen comprehension locals |
| `PbtExistsOneInTernaryCondTakesThen` | LIVE | companion: the then-arm direction of the same shape | codegen comprehension locals |
| `PbtSizeOfExistsOneTernaryBytes` | LIVE | `size(cond ? bytes-ternary : …)` poisoned to kError via the same slot bug | codegen comprehension locals |
| `PbtStringDoubleScientificForm` | PINNED | `string(double)` emits scientific where cel-cpp emits fixed (our `to_chars` + oracle `%.17g` gap) | runtime `to_chars` |
| `PbtIntOfDurationOverPermissive` | PINNED | `int(duration)` accepted; cel-cpp rejects (no such overload) | checker + `overload_table.cc` |
| `PbtSubstringEndEqualsSizeOverPermissive` | PINNED | two-arg `substring(start, size)` returns the tail; cel-cpp errors (its own `SubstringImpl` off-by-one) | runtime (to match cel-cpp) |
| `PbtModuloInt64MinByNegOneOverflows` | LIVE | `INT64_MIN % -1` returned `0` on a wrong "cel-cpp returns 0" assumption; cel-cpp errors (integer overflow, `CheckedMod`). Found by adding the exact `INT64_MIN` leaf | runtime `cel_arith.c` `cel_int_mod_at_vv` |

---

## Arithmetic — 🟡 (`catalog_ops.cc`: RegisterArithmetic, RegisterFallibleArithmetic)

- [x] `add_int64` `add_uint64` `add_double`
- [x] `subtract_int64` `subtract_uint64` `subtract_double`
- [x] `multiply_int64` `multiply_uint64` `multiply_double`
- [x] `divide_int64` `divide_uint64` · [ ] `divide_double` (double `/0.0`
      is a value not error — admit with the total double ops)
- [x] `modulo_int64` `modulo_uint64`
- [x] `negate_int64` `negate_double`
- [ ] `add_list` (list concat)
- [ ] `add_duration_duration` `add_duration_timestamp`
      `add_timestamp_duration` (temporal — needs duration/timestamp leaves)
- [ ] `subtract_duration_duration` `subtract_timestamp_duration`
      `subtract_timestamp_timestamp`

## Comparison — 🟡 (`catalog_ops.cc`: RegisterBoolProducers)

- [x] same-type numeric `{less,less_equals,greater,greater_equals,
      equals,not_equals}_{int64,uint64,double}`
- [x] `equals` / `not_equals` (heterogeneous top-level), bool/string/bytes eq
- [x] string/bytes **ordering**: `less_string` `less_bytes`
      `greater_string` `greater_bytes` `less_equals_*`
      `greater_equals_*` (bytewise; in the static subset)
- ⊘ **cross-type numeric** (24): `less_double_int64`
      `greater_int64_uint64` `less_equals_uint64_double` … — OUT OF
      the static subset by design.  Verified 2026-06-11: `1 < 2u`,
      `1.0 < 2` etc. fail type-check in our compiler (RejectDyn);
      these overloads are reachable only through `dyn`, which the
      fuzzer (and the compiler) excludes.  Not a fuzz target.
- [x] `{less,less_equals,greater,greater_equals,equals,not_equals}_duration`
      and `_timestamp` (temporal eq + ordering — `RegisterTemporal`)

## Logical — ✅ (RegisterBoolProducers)

- [x] `logical_and` `logical_or` `logical_not`

## size / in / type

- [x] `size_string` `size_bytes` `size_list` `size_map`
      (`catalog_aggregates.cc`: RegisterSizeProductions)
- [x] `in_list` `in_map` (RegisterInProductions)
- [ ] `type` (the `type(x)` reflection function)

## String functions — 🟡 (`catalog_strings.cc`: RegisterStringFunctions; `split` in aggregates)

- [x] `contains_string` `starts_with_string` `ends_with_string`
- [x] `matches` `matches_string`
- [x] `string_index_of_string` `string_last_index_of_string`
- [x] `string_replace_string_string`
- [x] `string_substring_int` (one-arg `substring(start)`)
- [⊘] `string_substring_int_int` — two-arg `substring(start, end)`
      WITHHELD from the grammar: cel-cpp errors on every `end ==
      size()` slice (a SubstringImpl off-by-one), we return the value;
      the grammar can't avoid `end == size`.  Pinned as
      `PbtSubstringEndEqualsSizeOverPermissive`.
- [x] `string_split_string` (`catalog_aggregates.cc`)
- [ ] `string_index_of_string_int` `string_last_index_of_string_int`
      (two-arg pos forms — resurface `IndexOfPosBoundIsByteNotCodepoint`)
- [ ] `string_replace_string_string_int` (`replace` with limit)
- [ ] `string_split_string_int` (`split` with limit)
- [x] `string_char_at` `string_lower_ascii` `string_upper_ascii`
      `string_trim` `string_reverse` (`catalog_strings.cc`)
- [x] `strings_quote`
- [x] `list_join` `list_join_string` (`catalog_aggregates.cc`)
- [x] `string_format` (`catalog_strings.cc`: RegisterStringFormat) —
      directive→type-matched productions (`%d`/`%x`/`%o` int, `%s`
      string, `%b` bool, `%f`/`%e` double, two-arg combos). The
      type-MISMATCHED combos (`%f` of int/string) are deliberately
      absent — cel-cpp errors, we don't; pinned as
      `FormatFixedRejectsInt` / `FormatFixedAcceptsNanToken`.

## Type conversions — 🟡 (`catalog_ops.cc`: RegisterMixedTotalOps + RegisterConversions)

- [x] `int64_to_double` `uint64_to_double` (`double(int)` / `double(uint)`)
- [x] `int64_to_uint64` `uint64_to_int64` `double_to_int64`
      `double_to_uint64` (cross-numeric — fallible, range-compared)
- [x] `string_to_int64` `string_to_uint64` `string_to_double`
      `string_to_bool` (parse family — fallible).  Numeric-shaped
      string leaves (`"42"` `"3.14"` `"-7"`) now drive the **success**
      path (`int("42")`==42, `double("3.14")`==3.14, …), not only the
      both-error branch; mined clean across int/uint/double/bool/string
      at d5.  The over-permissive whitespace (`"  3.14  "`) and
      leading-`+` (`"+5"`) forms stay withheld (pinned
      `DoubleFromStringRejectsWhitespace` / `IntFromStringLeadingPlus` /
      `UintFromStringLeadingPlus`).
- [x] `int64_to_string` `uint64_to_string` `bool_to_string`
      `bytes_to_string` (the `string(x)` family)
- [ ] `double_to_string` — EXCLUDED from the grammar: the oracle's
      cel-cpp lacks <charconv> double-to-chars (falls back to %.17g)
      so it can't validate double→string, and our wasm `to_chars`
      has a scientific-vs-fixed bug (`PbtStringDoubleScientificForm`).
      Pinned directly by the DoubleToString* known_bugs instead.
- [x] `string_to_bytes` (`bytes(string)`)
- [x] `int64_to_timestamp`-via-`int(timestamp)` reverse +
      `string(timestamp|duration)` shipped with RegisterTemporal
- [ ] `string_to_{duration,timestamp}` (`duration('…')` /
      `timestamp('…')` — need date/duration-shaped string leaves)
- [x] numeric-shaped string leaves — the oracle-agreeing forms
      (`"42"`, `"3.14"`, `"-7"`) are admitted and exercise the parse
      success path (mined clean d5).  The whitespace (`"  3.14  "`) and
      leading-`+` (`"+5"`) forms remain withheld — they trip our
      over-permissive parse vs cel-cpp (the
      `DoubleFromStringRejectsWhitespace` / `*FromStringLeadingPlus`
      pins); un-withhold when those are fixed.
- [ ] identity: `int64_to_int64` `uint64_to_uint64` `double_to_double`
      `bytes_to_bytes` `bool_to_bool` `string_to_string`
- Note: `int(duration)` (`duration_to_int64`) deliberately omitted —
      cel-cpp rejects it; over-permissiveness pinned as
      `PbtIntOfDurationOverPermissive`.

## math_ext — ✅ (`catalog_ops.cc`: RegisterMathExt; oracle gained MathCompilerLibrary + RegisterMathExtensionFunctions)

- [x] `math_abs_{int,uint,double}` `math_sign_{int,uint,double}`
- [x] `math_ceil_double` `math_floor_double` `math_round_double`
      `math_trunc_double` `math_sqrt_{int,uint,double}`
- [x] `math_isFinite_double` `math_isInf_double` `math_isNaN_double`
- [x] `math_bitAnd_{int_int,uint_uint}` `math_bitOr_*` `math_bitXor_*`
      `math_bitNot_*` `math_bitShiftLeft_*` `math_bitShiftRight_*`

## net_ext — ⬜ (BLOCKED: needs `CelType` opaque-type support)

> The bare-name functions (`isIP`/`ip`/`cidr` + ip/cidr receiver
> methods) work in our static subset, but `ip(...)`/`cidr(...)`
> produce the `net.IP`/`net.CIDR` **opaque types**, which the
> fuzzer's `CelType` (`shared/type.h`) cannot represent — no opaque
> kind.  Wiring net_ext requires extending the shared type
> vocabulary first (a compiler change, not a grammar-only slice).
> Same blocker as `optionals` (`optional<T>`).

- [ ] `net_isIP_string` `net_isIP_string_int` `net_string_ip` `net_string_cidr`
- [ ] `net_ip_*` (family, isCanonical, isLoopback, isGlobalUnicast, …)
- [ ] `net_cidr_*` (ip, masked, prefixLength, containsIP, containsCIDR, …)

## timestamp accessors — 🟡 (`catalog_temporal.cc`: RegisterTemporal; standard lib, no oracle ext)

- [x] no-tz forms: `timestamp_to_{year,month,day_of_month,
      day_of_month_1_based,day_of_week,day_of_year,hours,minutes,
      seconds,milliseconds}` (via getFullYear/getMonth/getDate/…)
- [x] `timestamp_to_int64` `timestamp_to_string` (int()/string())
- [x] every `_with_tz` variant: each accessor gets a production with a
      fixed valid tz baked into the template (IANA names —
      `America/New_York`, `Australia/Sydney`, `Europe/London`,
      `Asia/Tokyo`, … — and offset forms `-05:00`, `+09:30`), so the
      generated call exercises real cctz tz logic instead of an
      invalid-tz error.  `RegisterTimestampAccessors` in
      `catalog_*.cc`.
- Note: max-range timestamp leaf withheld until
  `MaxRangeTimestampConstruction` (known_bugs) is fixed.

## duration accessors — ✅ (RegisterTemporal)

- [x] `duration_to_{hours,minutes,seconds,milliseconds}`
- [x] `duration_to_int64` `duration_to_string` (int()/string())

## encoders (base64) — ✅ (`catalog_strings.cc`: RegisterStringFunctions; oracle gained EncodersCompilerLibrary + RegisterEncodersFunctions)

- [x] `base64_encode_bytes` (`base64.encode(bytes)` → String, total)
- [x] `base64_decode_string` (`base64.decode(string)` → Bytes,
      fallible — non-base64 leaves both-error)

## optionals — ⬜ (BLOCKED: needs `CelType` optional-type support)

> `CelType` (`shared/type.h`) has no `optional<T>` kind, so the
> fuzzer can't type optional-producing productions.  Like net_ext,
> this needs a shared type-vocabulary extension first.  (`.?` syntax
> is additionally blocked by `OptionalSelectOnMapRejected`.)

- [ ] `optional_of` `optional_ofNonZeroValue` `optional_none`
      `optional_value` `optional_hasValue` `optional_or_optional`
      `optional_orValue_value`
- [ ] `list_optindex_optional_int` `optional_list_index_int`
      `optional_list_optindex_optional_int`
- [ ] `map_optindex_optional_value` `optional_map_index_value`
      `optional_map_optindex_optional_value` `select_optional_field`
