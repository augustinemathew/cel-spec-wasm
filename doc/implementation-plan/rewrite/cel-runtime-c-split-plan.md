# cel_runtime.c split plan

Status: **plan — drafted 2026-04-25, not yet started.**

`compiler_v2/runtime/cel_runtime.c` is now ~2284 lines.  Each topic
already has its own header; this doc plans the corresponding `.c`
split, ordered by extraction difficulty so the easy carves ship
first.

## 1. Function / static catalog

`cel_runtime.c` walks top-to-bottom and every symbol falls into one
of the existing topic groups.  Lines refer to the current file;
"Static deps" lists the file-static helpers each function calls.

| Symbol | Kind | Lines | Target TU | Static deps |
|---|---|---|---|---|
| `memcpy`, `memset` (wasm fallback) | static | 7-19 | runtime-internal (per-TU copy or shared `cel_internal.c`) | — |
| `cel_memory_base_` | static | 42-46 (wasm), 59-61 (host) | `cel_memory.c` (linkage promoted to internal-extern) | — |
| `cel_memory_size_` | static | 47-53 (wasm), 62-64 (host) | `cel_memory.c` (internal-extern) | — |
| `g_memory` | file-scope static | 58 | `cel_memory.c` | — |
| `kBumpOffset`, `kLimitOffset` enum | enum | 68-71 | `cel_memory.c` private or `cel_arena.c` | — |
| `load_u32` | static | 80-82 | `cel_arena.c` (or hoist to `cel_internal.h`) | `cel_memory_base_` |
| `store_u32` | static | 84-86 | `cel_arena.c` | `cel_memory_base_` |
| `align_up` | static | 88-90 | `cel_arena.c` private | — |
| `cel_mem_base` | public | 92-94 | `cel_memory.c` | `cel_memory_base_` |
| `cel_mem_size` | public | 96-98 | `cel_memory.c` | `cel_memory_size_` |
| `cel_reset` | public | 100-104 | `cel_arena.c` | `store_u32` |
| `cel_alloc` | public | 106-116 | `cel_arena.c` | `align_up`, `load_u32`, `store_u32`, `memset`, `cel_memory_base_` |
| `cv_at` | static | 118-120 | `cel_internal.h` static-inline | `cel_memory_base_` |
| `cel_value_at` | public | 122-126 | `cel_arena.c` | `cv_at` |
| `alloc_cv` | static | 128-130 | `cel_make.c` | `cel_alloc` |
| `cel_make_null/bool/int/uint/double` | public | 132-179 | `cel_make.c` | `alloc_cv`, `cv_at` |
| `make_span_copy` | static | 181-195 | `cel_make.c` | `cel_alloc`, `alloc_cv`, `cv_at`, `memcpy`, `cel_memory_base_` |
| `cel_make_string`, `cel_make_bytes` | public | 197-205 | `cel_make.c` | `make_span_copy` |
| `make_span_view` | static | 207-215 | `cel_make.c` | `alloc_cv`, `cv_at` |
| `cel_make_string_view`, `cel_make_bytes_view` | public | 217-225 | `cel_make.c` | `make_span_view` |
| `poison` | static | 238-241 | `cel_internal.h` static-inline | — |
| `is_valid_map_key_kind` | static | 243-246 | `cel_map.c` private | — |
| `numeric_keys_equal` | static | 252-272 | `cel_map.c` private | — |
| `spans_equal` | static | 274-288 | `cel_internal.h` static-inline (unify with `span_eq`) | `cel_memory_base_` |
| `map_keys_equal` | static | 290-298 | `cel_map.c` (promote to internal-extern) | `spans_equal`, `numeric_keys_equal` |
| `arena_map_*` | static | 300-313 | `cel_map.c` private | `cel_memory_base_` |
| `cel_map_create/insert/lookup_arena/lookup` + `cel_host_cel_map_lookup` weak stub | public | 315-451 | `cel_map.c` | as above |
| `arena_list_header/element` | static | 468-476 | `cel_list.c` private | `cel_memory_base_` |
| `cel_list_create/set/at_arena/at` + `cel_host_cel_list_at` weak stub | public | 478-591 | `cel_list.c` | as above |
| `absorb_3vl_*`, `write_int`, `write_bool` (forward decls) | static decls | 604-608 | resolved by `cel_internal.h` | — |
| `cel_value_eq` | static | 620-631 | `cel_compare.c` (promote to internal-extern) | `spans_equal`, `map_keys_equal` |
| `cel_list_size_arena/in_arena/eq_arena` + concat helpers | public | 633-747 | `cel_list.c` | `absorb_3vl_*`, `write_*`, `cel_value_eq`, `poison` |
| `cel_map_size_arena/in_arena/eq_arena` + entry_matches | public | 749-823 | `cel_map.c` | as above |
| 8× `cel_host_cel_*` weak stubs | weak/extern | 838-916 | split: list ones in `cel_list.c`; map ones in `cel_map.c`; `cel_message_eq` in `cel_compare.c` | `cel_value_at`, `poison` |
| `cel_list_size/in/eq/concat` | public | 918-1012 | `cel_list.c` | as above |
| `cel_map_size/in/eq` | public | 1014-1074 | `cel_map.c` | as above |
| `absorb_3vl_binary/unary` | static | 1094-1121 | `cel_internal.h` static-inline | — |
| `require_kinds` | static | 1125-1132 | `cel_internal.h` static-inline | `poison` |
| `write_int/uint/double/bool` | static | 1134-1149 | `cel_internal.h` static-inline | — |
| `uint64_mul_overflows`, `int64_mul_overflows` | static | 1168-1212 | `cel_arith.c` private | — |
| `cel_int_*`, `cel_uint_*`, `cel_double_*` arith helpers | public | 1220-1433 | `cel_arith.c` | `absorb_3vl_*`, `require_kinds`, `write_*`, `poison` |
| `DEFINE_CMP_VV` macro + 24 expansions | public | 1443-1488 | `cel_compare.c` | `absorb_3vl_*`, `require_kinds`, `write_bool` |
| `cmp_*`, `numeric_compare_kernel`, `numeric_prelude`, `numeric_kind_pair` | static | 1521-1645 | `cel_compare.c` private | — |
| `cel_numeric_eq/ne/lt/le/gt/ge_at_vv` | public | 1647-1703 | `cel_compare.c` | numeric kernel + `write_bool` |
| `cel_null_eq_at_vv` | public | 1705-1712 | `cel_compare.c` | `absorb_3vl_*`, `require_kinds`, `write_bool` |
| `is_numeric`, `both_lists`, `both_maps` | static | 1746-1761 | `cel_compare.c` private | — |
| `equality_kernel` | static | 1772-1820 | `cel_compare.c` private | numeric/bool/string/bytes/list/map/message helpers + `write_bool` |
| `cel_equals_at_vv`, `cel_not_equals_at_vv` | public | 1822-1836 | `cel_compare.c` | `equality_kernel`, `cel_value_at` |
| `span_eq/lt/match_at/contains` | static | 1850-1897 | `cel_string_ops.c` private (or unify `span_eq` with `spans_equal`) | `cel_memory_base_` |
| `span_op_prelude` | static | 1903-1908 | `cel_string_ops.c` private | `absorb_3vl_*`, `require_kinds` |
| `concat_into_out` | static | 1910-1927 | `cel_string_ops.c` private | `cel_alloc`, `poison`, `memcpy` |
| `cel_string_concat_at_vv`, `cel_bytes_concat_at_vv` | public | 1929-1945 | `cel_string_ops.c` | as above |
| `size_at` | static | 1947-1955 | `cel_string_ops.c` private | `absorb_3vl_unary`, `write_int`, `poison` |
| `cel_string_size_at_v`, `cel_bytes_size_at_v` | public | 1957-1963 | `cel_string_ops.c` | `size_at` |
| `cel_string_eq/lt`, `cel_bytes_eq/lt` | public | 1965-1995 | `cel_string_ops.c` | `span_op_prelude`, `span_eq/lt`, `write_bool` |
| `DEFINE_SPAN_CMP_VV` macro + 6 expansions | public | 2003-2025 | `cel_string_ops.c` | as above |
| `cel_string_contains/starts_with/ends_with_at_vv` | public | 2027-2061 | `cel_string_ops.c` | as above |
| `merge_sorted_id_arrays` | static | 2068-2092 | `cel_3vl.c` private | — |
| `alloc_unknown_descriptor` | static | 2096-2103 | `cel_3vl.c` private | `cel_alloc`, `cel_memory_base_` |
| `merge_unknown_descriptors` | static | 2110-2129 | `cel_3vl.c` private | `cel_memory_base_`, `cel_alloc`, prior helpers |
| `write_unknown_at` | static | 2133-2137 | `cel_3vl.c` private | `cel_value_at` |
| `cel_unknown_merge` | public | 2139-2166 | `cel_3vl.c` | as above + `poison` |
| `cel_copy_slot` | public | 2168-2172 | `cel_3vl.c` | `cel_value_at` |
| `is_3vl_kind` | static | 2174-2176 | `cel_3vl.c` private | — |
| `cel_and`, `cel_or`, `cel_not` | public | 2186-2238 | `cel_3vl.c` | `cel_value_at`, `is_3vl_kind`, `write_bool`, `poison`, `cel_unknown_merge` |
| `cel_log_emit` (wasm + host), `cel_log` weak host stub | public/weak | 2249-2283 | `cel_log.c` | — |

## 2. Shared-static disposition

| Symbol | Currently | Used post-split by | Disposition |
|---|---|---|---|
| `g_memory`, `cel_memory_base_`, `cel_memory_size_` | static | every TU | Move into `cel_memory.c`. Promote `cel_memory_base_/_size_` to internal-extern in `cel_internal.h`. `g_memory` stays static. |
| `align_up` | static | only `cel_alloc` | Keep file-static in `cel_arena.c`. |
| `load_u32`, `store_u32` | static | `cel_arena.c` only | Keep file-static. |
| `cv_at` | static | arena + make TUs | `static inline` in `cel_internal.h`. |
| `alloc_cv` | static | `cel_make.c` only | Keep file-static. |
| `poison` | static | every helper that errors | `static inline` in `cel_internal.h`. |
| `absorb_3vl_binary/unary` | static | every arith/compare/string/list/map/3vl helper | `static inline` in `cel_internal.h` — load-bearing for inlining (Risk #1). |
| `require_kinds` | static | arith + compare + string_ops | `static inline` in `cel_internal.h`. |
| `write_int/uint/double/bool` | static | arith + compare + list + string_ops + 3vl | `static inline` in `cel_internal.h`. |
| `is_3vl_kind/is_valid_map_key_kind/is_numeric_kind/is_numeric` | static | one TU each (post-split) | Keep file-static in their respective TUs. Optionally collapse `is_numeric_kind` and `is_numeric` to one `static inline`. |
| `cel_value_eq` | static | list + map | Promote to internal-extern, define in `cel_compare.c`. |
| `map_keys_equal` | static | map + `cel_value_eq` | Promote to internal-extern, define in `cel_map.c`. |
| `spans_equal` | static | map + `cel_value_eq` (via CEL_BYTES) — and `span_eq` in string_ops is byte-equivalent | Recommend unifying both into one `static inline` in `cel_internal.h`. |
| `span_lt/match_at/contains` | static | `cel_string_ops.c` only | Keep file-static. |
| `arena_list/map_*` accessors | static | one TU each | Keep file-static. |
| `cmp_*`, `numeric_*` kernels, `equality_kernel` | static | `cel_compare.c` only | Keep file-static. |
| `merge_sorted_id_arrays` and friends | static | `cel_3vl.c` only | Keep file-static. |
| `memcpy`, `memset` (wasm fallback) | static | every TU touching memory | Either `static inline` per-TU in `cel_internal.h` (one source, many copies) or hoist to `cel_internal.c`. Recommend `static inline`. |

**Recommended internal header layout.** One `cel_internal.h` (NOT
exported by `cel_runtime.h`) holding `static inline` for `poison`,
`absorb_3vl_*`, `require_kinds`, `write_*`, `is_numeric_kind`,
`cv_at`, `spans_equal`, plus `extern` decls for `cel_memory_base_`,
`cel_memory_size_`, `cel_value_eq`, `map_keys_equal`.

## 3. Phased split (easiest → hardest)

| Phase | TU | Difficulty | Notes |
|---|---|---|---|
| **P1** | `cel_log.c` | trivial | One function + weak host stub. Zero static deps inside. |
| **P2** | `cel_memory.c` | trivial | `g_memory` + accessors. The dependency, not a depender. |
| **P3** | `cel_arena.c` | low | Depends on P2 only. |
| **P4** | `cel_make.c` | low | Depends on `cel_alloc`, `cel_value_at`, `cv_at`. |
| **P5** | `cel_3vl.c` | low | Depends on `cel_value_at`, `cel_alloc`, `poison`, `write_bool`. |
| **P6** | `cel_arith.c` | medium | Best first to exercise `cel_internal.h`'s `static inline` shapes. |
| **P7** | `cel_string_ops.c` | medium | ~290 lines; pulls the same shared inlines + `cel_alloc`/`memcpy`. |
| **P8** | `cel_compare.c` | high | Houses `equality_kernel`, `cel_value_eq`. Forward-references string/bytes/list/map/message public helpers. |
| **P9** | `cel_list.c` + `cel_map.c` | hardest | Mutually entangled. Split together (`cel_value_eq` in compare.c needs `map_keys_equal` from map.c). |

**Recommended first carve: P1 (`cel_log.c`).** Most cleanly
disconnected; exercises BUILD wiring once without touching shared
statics. Follow-up CL: introduce `cel_internal.h` and ship P2-P5
together. Then P6+ one TU per CL.

### Concrete spec for the first carve (P1)

**New file** `compiler_v2/runtime/cel_log.c`:

- `#include "compiler_v2/runtime/cel_log.h"`
- Move lines 2249-2283 of current `cel_runtime.c` verbatim (both
  the `__wasm__` arm — `cel_log_emit` — and the host arm — weak
  `cel_log` + `cel_log_emit`).

**Edits to `cel_runtime.c`.** Delete those lines.  The forward-extern
for `cel_log` already lives in `cel_log.h`; `CEL_LOG(...)` callers
keep compiling.

**Verification.** `cel_log` weak symbol moves to exactly one TU
(linker behaviour identical).  `wasm_imports.txt` byte-identical
because `cel_log` remains an import on the wasm side.  The
`__wasm__` build sees one new TU with one function (`cel_log_emit`);
inlining was never possible (it issues an import call) — no LTO
regression.

## 4. BUILD.bazel before/after for the first carve

### `cc_library` host build

Before:
```
cc_library(
    name = "cel_runtime",
    srcs = ["cel_runtime.c"],
    hdrs = [...],
    copts = [...],
)
```

After:
```
cc_library(
    name = "cel_runtime",
    srcs = [
        "cel_log.c",
        "cel_runtime.c",
    ],
    hdrs = [...],         # unchanged
    copts = [...],        # unchanged
)
```

### `genrule(name = "cel_runtime_wasm_file")`

1. Add `"cel_log.c"` to `srcs = [...]` (alphabetised).
2. Append `"$(location :cel_log.c)"` to the `cmd = " ".join([...])`
   list, right after `"$(location :cel_runtime.c)"`. clang accepts
   multiple `.c` inputs and links them when `-c` isn't passed; the
   genrule does not pass `-c`.

The `wasm_imports.txt` allow-undefined list is unchanged.  Exported-
symbols list unchanged.  The wasm-side byte-loop `memcpy`/`memset`
is still file-static in `cel_runtime.c` — `cel_log_emit` doesn't
use them.

For phase 6 (`cel_arith.c`) the genrule edit is identical: append
one more `"$(location :cel_arith.c)"`.  The `--export=cel_int_*` /
`--export=cel_uint_*` / `--export=cel_double_*` lines already point
at symbols by name so they work no matter which TU contributes them.

## 5. Risk register

1. **LTO inlining of shared inlines.** Every arith and compare
   helper currently sees `absorb_3vl_*` / `write_*` / `poison` in
   the same TU.  Splitting collapses that only if those stay
   `static inline` in `cel_internal.h`.  Risk: someone refactors to
   `extern` linkage; clang at `-O2` cannot cross-TU inline a
   freestanding wasm32 build (no LTO flag passed).  **Mitigation**:
   enforce `static inline` in `cel_internal.h`, with a comment.
   Optionally measure `cel_runtime.wasm` size before/after the
   first arith carve.
2. **Weak `cel_log` host stub must remain in exactly one TU.** The
   host build relies on `__attribute__((weak))` so tests can
   override.  After P1, only `cel_log.c` defines it on host.
   **Mitigation**: confirm `cel_runtime.c` no longer contains the
   weak definition; one `nm` check suffices.
3. **Host weak stubs for `cel_host_cel_*` (8 of them).** Each must
   appear in exactly one `.c`.  Plan: `cel_host_cel_list_*` and
   `cel_host_cel_message_eq` move with `cel_compare.c` (the equality
   kernel is the sole caller of `cel_message_eq`); `cel_host_cel_map_*`
   to `cel_map.c`; remaining list ones to `cel_list.c`.
4. **Internal-helper inlining loss.** If `cel_int_add_at_vv` no
   longer sees `absorb_3vl_binary`'s body, clang loses
   dead-store-elim on the OK path.  With `static inline` in
   `cel_internal.h`, it stays inlined.
5. **`_Static_assert` in the .c body.** None.  All
   `_Static_assert`s live in `cel_data.h`; per-TU duplication is
   fine (compile-time only).
6. **`__attribute__((musttail))` across TUs.** `cel_map_lookup`
   tail-calls `cel_map_lookup_arena` (same TU today; same TU
   post-split — both go to `cel_map.c`).  No risk.
7. **`cel_runtime.wasm` byte size regression.** Measure once per
   carve.
8. **`wasm_imports.txt` byte-identical contract.** No new imports;
   visual diff after each carve must show no change.

## 6. Open questions

1. **Internal-header naming.**  One `cel_internal.h` umbrella vs
   per-topic internals (`cel_compare_internal.h`, etc.)?
   Recommendation: one umbrella — faster to ship, lower-friction
   for the cross-cutting set.
2. **`cel_internal.h` exposure.**  Don't re-export from
   `cel_runtime.h`.  `*_test.cc` files needing `poison` for
   assertion setup `#include "cel_internal.h"` directly.
3. **Hoist wasm `memcpy`/`memset`?**  `static inline` in
   `cel_internal.h` is preferred — single source of truth, clang
   dead-strips unused copies.
4. **`cel_value_eq` location.**  Live in `cel_compare.c` (semantic
   home), internal-extern via `cel_internal.h`.
5. **`cel_message_eq` weak host stub.**  Live in `cel_compare.c`
   alongside `equality_kernel`.
6. **First-carve scope.**  P1 alone first; then P2+P3+P4 with
   `cel_internal.h` introduction; then P5+ one TU per CL.
7. **Lint backlog.**  Confirm no per-TU "must include" ordering
   pragmas before scaling to nine new `.c` files.

## Critical files

- `compiler_v2/runtime/cel_runtime.c`
- `compiler_v2/runtime/BUILD.bazel`
- `compiler_v2/runtime/cel_log.h`
- `compiler_v2/runtime/cel_runtime.h` (umbrella)
- `compiler_v2/runtime/cel_data.h`
