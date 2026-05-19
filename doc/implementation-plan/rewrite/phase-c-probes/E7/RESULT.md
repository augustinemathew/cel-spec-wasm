# Probe E7: minimum absl library set for the Phase C kernels

**Status:** PASS

## Method

Wrote `kernels.cc` exporting one wasm function per Phase C kernel:
`parse_timestamp`, `parse_duration`, `format_timestamp`,
`format_duration`, `strings_format_int` (a stub stand-in for the
real `strings.format` kernel).  Each uses absl directly (no thin
shim layer).

The `cc_binary` lists three top-level absl deps:
`@com_google_absl//absl/time`, `@com_google_absl//absl/strings`,
`@com_google_absl//absl/strings:str_format`.  bazel resolves
transitive deps automatically — no manual library-by-library
discovery required.

```
bazel build //doc/…/E7:kernels --platforms=//doc/…/E3:wasm32_wasi \
  --extra_toolchains=//doc/…/E3:wasm32_wasi_toolchain
bazel cquery 'kind("cc_library", deps(//doc/…/E7:kernels))' \
  --platforms=…
```

## Output

  - `kernels.wasm` — **569,770 bytes stripped** / **206,536 bytes
    gzipped** (206 KB).
  - **43 absl cc_library targets** transitively pulled (full list in
    `absl_deps.txt`).  Compare exp1_re2's CMake build: **93 libabsl_*.a**
    files.  Bazel's per-cc_library granularity drops the count
    by 54%.
  - Exports: `arena_reset`, `arena_alloc`, `parse_timestamp`,
    `parse_duration`, `format_timestamp`, `format_duration`,
    `strings_format_int`.
  - Wasm valid + instantiable (verified via `wasmtime compile`).

## Findings

### Compile-time vs runtime format strings

`absl::StrFormat("%s", x)` requires a `constexpr` format string.  CEL's
`strings.format` takes a **runtime** format string (the first
arg).  The probe used a constexpr `"%d"` to demonstrate the link
works; the production implementation will need
`absl::FormatUntyped` (which accepts a `string_view` format).
This is a code-level concern, not a toolchain concern — flagged
for the implementation plan.

### CMake → bazel granularity dividend

CMake's `add_subdirectory(abseil-cpp)` rebuilds every absl
library because there's no way to opt out.  bazel's `cc_library`
graph means we only pull what `absl/time` / `absl/strings` /
`absl/strings:str_format` transitively need.  The 43-vs-93
ratio translates directly to size + build time.

### Why E7 wasm is bigger than E5 (569 KB vs 372 KB)

E5 only pulled `absl/time` + `absl/strings`; E7 adds
`absl/strings:str_format` (and its transitive deps:
`absl/types:span`, `absl/numeric:int128`, the `str_format_internal`
machinery).  StrFormat is the biggest single library in the set
— ~100 KB of stripped wasm.

## Minimum absl subset

The 43 transitive cc_libraries (top-level + Phase C-relevant):

  - **`absl/time`**, `absl/time/internal/cctz:time_zone`,
    `absl/time/internal/cctz:civil_time` — the time machinery.
  - **`absl/strings`**, `absl/strings:str_format`,
    `absl/strings:str_format_internal`, `absl/strings:internal`,
    `absl/strings:string_view`, etc.
  - **`absl/base:*`** (config, base, log_severity, raw_logging_internal,
    spinlock_wait, throw_delegate, atomic_hook, cycleclock_internal,
    errno_saver, dynamic_annotations, …).
  - **`absl/numeric:int128`** + `absl/numeric:bits` +
    `absl/numeric:representation` — needed by time + str_format.
  - **`absl/container:inlined_vector` / `:fixed_array`** — used by
    StrFormat for its arg packs.
  - **`absl/types:span` / `:optional` / `:compare`** — interface types.
  - **`absl/memory:memory`**, **`absl/meta:type_traits`**,
    **`absl/algorithm:algorithm`**, **`absl/functional:any_invocable` /
    `:function_ref`**, **`absl/hash:weakly_mixed_integer`**,
    **`absl/utility:utility`**.

Full list in `absl_deps.txt`.

## Next-step implication

Path A scales to all four Phase C absl kernels.  Final cel_runtime
size estimate (E9): start from 569 KB and add ~120-200 KB for RE2
(per exp1_re2's 388 KB total absl+RE2; bazel-granular reduction
should bring RE2 alone to ~200 KB).  No additional patches or
toolchain features needed beyond what E3/E4 wired.
