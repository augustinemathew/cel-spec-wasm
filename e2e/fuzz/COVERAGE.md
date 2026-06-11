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
| Comparison (`==` `!=` `<` `<=` `>` `>=`) | 57 | 🟡 same-type numeric + string/bytes/bool eq; ordering & cross-type & temporal open |
| Logical (`&&` `\|\|` `!`) | 3 | ✅ |
| `size` | 4 | ✅ |
| `in` | 2 | ✅ |
| Ternary `?:` | (macro) | ✅ |
| Comprehensions (`exists`/`all`/`exists_one`/`filter`/`map`) | (macros) | ✅ over lists; maps predicate-only |
| Aggregates (list/map literals, nested) | — | ✅ |
| String functions | ~27 | 🟡 see breakdown |
| Type conversions | ~30 | 🟡 only `double(int\|uint)` |
| **math_ext** | 28 | ✅ |
| **net_ext** | 20 | ⬜ |
| **timestamp accessors** | 23 | ⬜ |
| **duration accessors** | 7 | ⬜ |
| **encoders (base64)** | 2 | ⬜ |
| **optionals** | ~14 | ⬜ |
| `type()` | 1 | ⬜ |

The ⬜ rows are the work queue, roughly by bug-yield. They map onto
[`m30-fuzz-full-dialect.md`](../../doc/implementation-plan/rewrite/m30-fuzz-full-dialect.md):
string-rest + conversions → M30.D; timestamp/duration/optionals →
M30.C; math_ext/net_ext/encoders → new M30.D sub-slices.

---

## Arithmetic — 🟡 (`grammar_scalars.cc`: RegisterArithmetic, RegisterFallibleArithmetic)

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

## Comparison — 🟡 (`grammar_scalars.cc`: RegisterBoolProducers)

- [x] same-type numeric `{less,less_equals,greater,greater_equals,
      equals,not_equals}_{int64,uint64,double}`
- [x] `equals` / `not_equals` (heterogeneous top-level), bool/string/bytes eq
- [ ] string/bytes **ordering**: `less_string` `less_bytes`
      `greater_string` `greater_bytes` `less_equals_*` `greater_equals_*`
- [ ] **cross-type numeric** (24): `less_double_int64`
      `greater_int64_uint64` `less_equals_uint64_double` … (the
      heterogeneous-comparison matrix — high bug-yield, needs the
      generator to mix numeric types in one comparison)
- [ ] `{less,greater,…}_duration` `{…}_timestamp` (temporal ordering)

## Logical — ✅ (RegisterBoolProducers)

- [x] `logical_and` `logical_or` `logical_not`

## size / in / type

- [x] `size_string` `size_bytes` `size_list` `size_map`
      (`grammar_aggregates.cc`: RegisterSizeProductions)
- [x] `in_list` `in_map` (RegisterInProductions)
- [ ] `type` (the `type(x)` reflection function)

## String functions — 🟡 (`grammar_scalars.cc`: RegisterStringFunctions; `split` in aggregates)

- [x] `contains_string` `starts_with_string` `ends_with_string`
- [x] `matches` `matches_string`
- [x] `string_index_of_string` `string_last_index_of_string`
- [x] `string_replace_string_string`
- [x] `string_substring_int` `string_substring_int_int`
- [x] `string_split_string` (`grammar_aggregates.cc`)
- [ ] `string_index_of_string_int` `string_last_index_of_string_int`
      (two-arg pos forms — resurface `IndexOfPosBoundIsByteNotCodepoint`)
- [ ] `string_replace_string_string_int` (`replace` with limit)
- [ ] `string_split_string_int` (`split` with limit)
- [ ] `string_char_at` `string_lower_ascii` `string_upper_ascii`
      `string_trim` `string_reverse`
- [ ] `string_format` (the `%`-format mini-language — its own bug surface)
- [ ] `strings_quote`
- [ ] `list_join` `list_join_string`

## Type conversions — 🟡 (`grammar_scalars.cc`: RegisterMixedTotalOps)

- [x] `int64_to_double` `uint64_to_double` (`double(int)` / `double(uint)`)
- [x] identity: `double_to_double` (transitively via the above)
- [ ] `int64_to_int64` `int64_to_uint64` `int64_to_timestamp`
      `int64_to_duration`
- [ ] `double_to_int64` `double_to_uint64` `double_to_string`
- [ ] `uint64_to_int64` `uint64_to_string`
- [ ] `bool_to_string` `bytes_to_string` `int64_to_string` (the
      `string(x)` family)
- [ ] `string_to_{bool,bytes,double,int64,uint64,duration,timestamp}`
      (the `int('5')` / `timestamp('…')` family — fallible, parse errors)
- [ ] `bytes_to_bytes` `bool_to_bool` `string_to_string` (identity)

## math_ext — ✅ (`grammar_scalars.cc`: RegisterMathExt; oracle gained MathCompilerLibrary + RegisterMathExtensionFunctions)

- [x] `math_abs_{int,uint,double}` `math_sign_{int,uint,double}`
- [x] `math_ceil_double` `math_floor_double` `math_round_double`
      `math_trunc_double` `math_sqrt_{int,uint,double}`
- [x] `math_isFinite_double` `math_isInf_double` `math_isNaN_double`
- [x] `math_bitAnd_{int_int,uint_uint}` `math_bitOr_*` `math_bitXor_*`
      `math_bitNot_*` `math_bitShiftLeft_*` `math_bitShiftRight_*`

## net_ext — ⬜ (no productions; needs `net.IP` / `net.CIDR` opaque-type leaves)

- [ ] `net_isIP_string` `net_isIP_string_int` `net_string_ip` `net_string_cidr`
- [ ] `net_ip_*` (family, isCanonical, isLoopback, isGlobalUnicast, …)
- [ ] `net_cidr_*` (ip, masked, prefixLength, containsIP, containsCIDR, …)

## timestamp accessors — ⬜ (needs timestamp leaves + tz strings)

- [ ] `timestamp_to_{year,month,day_of_month,day_of_month_1_based,
      day_of_week,day_of_year,hours,minutes,seconds,milliseconds}`
      and every `_with_tz` / `_tz` variant (23 total)

## duration accessors — ⬜ (needs duration leaves)

- [ ] `duration_to_{hours,minutes,seconds,milliseconds,int64,string}`

## encoders (base64) — ⬜

- [ ] `base64_encode_bytes` `base64_decode_string`

## optionals — ⬜ (needs `optional<T>` leaves; `.?` blocked by `OptionalSelectOnMapRejected`)

- [ ] `optional_of` `optional_ofNonZeroValue` `optional_none`
      `optional_value` `optional_hasValue` `optional_or_optional`
      `optional_orValue_value`
- [ ] `list_optindex_optional_int` `optional_list_index_int`
      `optional_list_optindex_optional_int`
- [ ] `map_optindex_optional_value` `optional_map_index_value`
      `optional_map_optindex_optional_value` `select_optional_field`
