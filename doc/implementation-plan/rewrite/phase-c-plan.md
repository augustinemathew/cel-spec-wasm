# Phase C — implementation plan

**Status:** drafted 2026-05-18 from probe results E1-E10 (see
`phase-c-probes/E*/RESULT.md`).  Branch: `phase-c-libraries`.

## 1 Chosen path

**Path A — proper bazel `cc_toolchain` for wasm32-wasi-threads.**

Validated end-to-end:

  - **E3:** minimum `cc_toolchain_config.bzl` (~280 LoC, 8 features)
    + 3 sh tool-wrapper scripts builds `hello_wasm.cc` to a valid
    wasm artefact.  `wasmtime --invoke add` returns `42`.
  - **E4:** `@com_google_absl//absl/strings` + `@com_google_absl//absl/time`
    cross-compile clean under the same toolchain (one absl patch +
    a target switch from `wasm32-wasi` to `wasm32-wasi-threads`).
  - **E5:** combined C + C++ + absl link produces a 372 KB wasm
    that `wasmtime --invoke run` returns the right Unix timestamp
    from.
  - **E7:** all four Phase C absl kernels (time + str_format)
    transitively pull **43 absl cc_library targets** (vs 93
    libabsl_*.a from exp1_re2's CMake build — 54% fewer).
  - **E8:** RE2 + absl link clean; produces a 1.5 MB stripped
    wasm.  Runtime smoke test ran but returned no-match (reactor-
    mode `_initialize` deferred to implementation phase).
  - **E9:** projected full Phase C `cel_runtime.wasm` size ~1.2 MB
    stripped / ~400 KB gzipped (5× current 241 KB / 54 KB).  With
    `-flto` added to the toolchain config, projection narrows to
    ~800 KB stripped / ~300 KB gzipped.

Path A is **not infeasible** in any way; the wiring cost was
~10 features in `cc_toolchain_config.bzl` (vs the brief's "50
features" estimate), one 3-line absl patch, and three tool-
wrapper sh scripts.  Total ~280 LoC + 1 patch.

Paths B (rules_foreign_cc) and C (pre-built libs) remain documented
in the probes as fallback context only; they are not recommended.

## 2 New bazel targets

### `//third_party/wasi_sdk/` additions

  - `cc_toolchain_config.bzl` — the toolchain config (copied from
    `//doc/.../E3:cc_toolchain_config.bzl`, with cross-platform
    selects added).
  - `wasm_clang.sh`, `wasm_ar.sh`, `wasm_nm.sh` — tool wrappers
    that exec into the `@wasi_sdk_<platform>` binaries.  The
    `dirname` math (`../../../../external/...`) needs adjusting
    for the destination package depth.
  - `cc_toolchain` + `toolchain` + `platform(name = "wasm32_wasi")`
    rules (copied from E3 BUILD.bazel).

### `//third_party/patches/`

  - `abseil-cpp-wasm-sysinfo.patch` (3-line diff) — created during
    E4, already in place.  Applied via `single_version_override`
    in MODULE.bazel.

### `MODULE.bazel` changes

  - `bazel_dep(name = "platforms", version = "1.0.0")` — required
    for `@platforms//cpu:wasm32` + `@platforms//os:wasi`
    visibility (added during E3).
  - `bazel_dep(name = "re2", version = "2025-11-05.bcr.1")` —
    makes RE2 visible from main repo (added during E8).
  - `single_version_override` block for `abseil-cpp` to apply
    the sysinfo patch.

### `//compiler_v2/runtime/` changes

  - New `cc_library(name = "cel_time_parse")` with srcs =
    `cel_time_parse.cc`, deps = `absl/strings + absl/time + re2`.
  - New `cc_library(name = "cel_strings_format")` with srcs =
    `cel_strings_format.cc`, deps = `absl/strings:str_format`.
  - **Convert** the existing `cel_runtime_wasm_file` genrule to a
    `cc_binary(name = "cel_runtime_wasm")` depending on the
    existing C sources + the two new C++ libraries.  Linkopts
    preserve all `-Wl,--export=cel_*` flags.

## 3 Files to create / modify

| Path | Action |
|---|---|
| `MODULE.bazel` | +3 entries: platforms, re2, single_version_override |
| `third_party/patches/abseil-cpp-wasm-sysinfo.patch` | exists from E4 |
| `third_party/patches/BUILD.bazel` | exists from E4 |
| `third_party/wasi_sdk/cc_toolchain_config.bzl` | NEW (from E3) |
| `third_party/wasi_sdk/wasm_clang.sh` | NEW (from E3) |
| `third_party/wasi_sdk/wasm_ar.sh` | NEW (from E3) |
| `third_party/wasi_sdk/wasm_nm.sh` | NEW (from E3) |
| `third_party/wasi_sdk/BUILD.bazel` | EXTEND with cc_toolchain + toolchain + platform |
| `compiler_v2/runtime/cel_time_parse.cc` | NEW |
| `compiler_v2/runtime/cel_strings_format.cc` | NEW |
| `compiler_v2/runtime/cel_time_parse.h` | NEW (extern "C" declarations) |
| `compiler_v2/runtime/BUILD.bazel` | EXTEND: cc_library × 2, convert genrule → cc_binary |
| `compiler_v2/codegen/overload_table.cc` | reroute 4 ids; delete 2 from unimplemented |
| `compiler_v2/api/internal/cel_host.cc` | delete ~180 LoC (the 4 Impl trampolines + RegisterHost calls) |
| `compiler_v2/api/internal/host_loader.cc` | add `__wasm_call_ctors` invocation post-instantiate (5 LoC) |
| `scripts/lint.sh` | extend coverage to new `.bzl` + sh + .cc files |

## 4 Per-kernel implementation plan

### 4.1 `cel_timestamp_parse_at_v`

  - **Signature:** `void cel_timestamp_parse_at_v(CelValue* out, const CelValue* str)`
  - **Behaviour:** validate `str->kind == CEL_STRING`; call
    `absl::ParseTime(absl::RFC3339_full, ...)`; post-validate the
    parsed `absl::Time` falls in the cel-cpp `RejectsAsTimestampPerCEL`
    envelope (year 0001-9999, no overflow); on success populate
    `out` as `CEL_TIMESTAMP`; on failure populate as error.
  - **Errors:** parse failure → `kCelErrParseTimestamp`; out-of-range
    → `kCelErrTimestampOverflow`; type mismatch → `kCelErrInvalidArgument`.
  - **Conformance:** drives `timestamps.textproto` 75/76 → 76/76 PASS.

### 4.2 `cel_duration_parse_at_v`

  - **Signature:** `void cel_duration_parse_at_v(CelValue* out, const CelValue* str)`
  - **Behaviour:** `absl::ParseDuration` + cel-cpp bounds check
    (≤ ±315B seconds).
  - **Errors:** parse failure → `kCelErrParseDuration`; out-of-range
    → `kCelErrDurationOverflow`.

### 4.3 `cel_timestamp_format_at_v`

  - **Signature:** `void cel_timestamp_format_at_v(CelValue* out, const CelValue* ts)`
  - **Behaviour:** `absl::FormatTime(absl::RFC3339_full, ts,
    UTCTimeZone())` → arena_alloc'd string in `out`.
  - **Format:** RFC3339 UTC: `2026-05-18T10:00:00Z` or
    `2026-05-18T10:00:00.123456789Z` if nanos != 0.

### 4.4 `cel_duration_format_at_v`

  - **Signature:** `void cel_duration_format_at_v(CelValue* out, const CelValue* dur)`
  - **Behaviour:** `absl::FormatDuration(dur)` → arena_alloc'd
    string in `out`.

### 4.5 `cel_matches_at_vv` (C3 — shipped 2026-05-19)

  - **Signature:** `void cel_matches_at_vv(uint32_t out_slot,
    uint32_t text_slot, uint32_t pat_slot)`.  Slot-indexed
    `_at_vv` form to match the existing runtime ABI; equivalent
    in effect to the originally-drafted pointer signature.
  - **Behaviour:** RE2 compile + PartialMatch.  Compile-failure
    propagated as `CEL_ERROR(CEL_ERR_INVALID_ARGUMENT)` and
    cached as `nullptr` so a repeat invocation of the same bad
    pattern surfaces the same sticky error without recompiling.
  - **Cache shape — delta vs plan.**  The plan drafted an LRU
    cap-128 cache; the shipped kernel is a **single-slot
    most-recent-pattern** cache.  Rationale: the common
    `list.exists(x, x.matches(pat))` workload reuses the same
    pattern across iterations and saturates a single slot; the
    multi-pattern shape is rare in practice and recompiles at
    RE2-compile cost (~µs).  LRU adds complexity for a workload
    we don't have evidence of.  Revisitable if profiling
    surfaces a multi-pattern hot path.
  - **Conformance:** drives `string.textproto::matches/*` 9
    SKIPs → 9 PASS (overall 1373 → 1382, locked in the
    2026-05-19 conformance run).  Caught a real first-call-with-
    empty-pattern bug: the cache's `CachedPattern() == ""`
    matched the default-constructed empty pattern and skipped
    the compile path, leaving `CachedRe()` null; fixed by
    adding a `CachedInitialized` flag and a kernel-layer
    regression test that doesn't rely on test-suite ordering.
  - **Tests:** `compiler_v2/runtime/cel_matches_test.cc` —
    21 cases covering 3VL absorb, kind-mismatch, pattern-compile
    failure (sticky-error cache), the 9 spec rows as a TEST_P
    matrix, cache behaviour (warm-path 1000× + alternating
    50×), and boundary (embedded NUL, anchors, empty-empty,
    invalid-UTF-8 no-crash, 4 KiB input).

### 4.6 `cel_strings_format_at_vv`

  - **Signature:** `void cel_strings_format_at_vv(CelValue* out,
    const CelValue* fmt, const CelValue* args_list)`
  - **Behaviour:**
    1. Parse `fmt->payload.s` directive stream (`%[flags][width][.precision]<type>`).
    2. Walk `args_list` element-by-element; build an
       `absl::FormatArg` per (CelKind × directive) pair, validating
       compatibility.
    3. `absl::FormatUntyped(fmt_string_view, args_span)` into a
       string sink.
    4. arena_alloc + memcpy into `out`.
  - **Errors:** mismatch between directive count and args length;
    incompatible directive type for a given CelKind; arena OOM.
  - **Conformance:** drives subset of `string_ext.textproto::format/*`
    flips (only `format()` rows, not `split`/`join`/`replace`).
  - **Estimated effort:** 1.0 day (the C2.G slice).

## 5 Test plan

### 5.1 Unit tests (under `compiler_v2/runtime/`)

  - `cel_time_parse_test.cc` — TEST_P matrix over RFC3339 inputs
    (valid + invalid + boundary).  At least 30 rows: valid
    `2026-05-18T10:00:00Z`, leap-year boundaries, year 0/9999 limits,
    negative years, fractional seconds 1-9 digits, TZ offsets,
    invalid syntax (missing `Z`, two-digit year, etc.).
  - `cel_duration_parse_test.cc` — TEST_P matrix over duration
    grammar (each unit, mixed units, signed durations, boundary
    315B seconds).
  - `cel_matches_test.cc` — TEST_P over the existing
    `string.textproto::matches/*` cases.
  - `cel_strings_format_test.cc` — TEST_P over the
    `string_ext.textproto::format/*` rows.

### 5.2 E2E tests

  - `compiler_v2/api/instance_test.cc` — add a parametric row per
    new kernel under the existing `MatchesTest` /
    `TimestampParseTest` patterns.

### 5.3 Conformance

  - `bazel run //compiler_v2/conformance:conformance_runner --
    --skip_envelope=...` — capture the diff:
      - `timestamps.textproto`: 75 → 76 PASS.
      - `string.textproto::matches/*`: 0 → 9 PASS.
      - `string_ext.textproto::format/*`: subset flip (numbers TBD
        after C2.G lands).

### 5.4 Manual-tagged tests

The new `cel_runtime_wasm_test` (already manual-tagged) re-runs
under the cc_toolchain build; verify byte-identical output to the
genrule build for the existing kernels (regression net).

## 6 Estimated effort

| Slice | Description | Days |
|---|---|---:|
| Toolchain wiring | Move E3 config + wrappers to `//third_party/wasi_sdk/`; cross-platform selects | 1.0 |
| Runtime cc_binary switch | Convert cel_runtime genrule → cc_binary; verify byte parity | 0.5 |
| `_initialize` host call | host_loader.cc + tests | 0.25 |
| C1 Slice 1 (pure-C parsers) | C1.A through C1.F per phase-c-design.md | 3.0 |
| C2.A — absl + RE2 via bazel cc_toolchain (NOT rules_foreign_cc) | Wire the deps, build the runtime under the new toolchain | 0.5 |
| C2.B — RE2 deps | Already cross-compiles via bazel (E8) — wire only | 0.25 |
| C2.C — link wiring | Already covered in cc_binary conversion | 0.0 |
| C2.D — `cel_matches_at_vv` kernel + regex cache | Real impl + LRU cache | 1.0 |
| C2.E — codegen reroute | 6-line diff to overload_table.cc | 0.25 |
| C2.F — conformance | matches/* 9 → PASS | 0.5 |
| C2.G — `strings.format` | FormatUntyped + arg-pack validation | 1.5 |
| C2.H — format conformance | Subset of format/* flips | 0.5 |
| Pre-close: lint, doc updates, post-bench | scripts/lint.sh extension, design doc updates | 0.75 |
| **Total** | | **~10 days** |

Phase C delivers ~10 days of implementation against a sequence of
slices, plus 1d of upfront toolchain wiring.

## 7 Risks + open questions

### 7.1 Reactor-mode `_initialize`

E8 surfaced that bazel-built wasm doesn't auto-call
`__wasm_call_ctors`.  Fix: 5-LoC change in `host_loader.cc` to call
`_initialize` (or `__wasm_call_ctors`) right after instantiate.

### 7.2 wasm32-wasi vs wasm32-wasi-threads

The runtime's existing build is **vanilla** `wasm32-wasi`; Path A's
toolchain is `wasm32-wasi-threads` (cctz needs `std::mutex`).
Implications:

  - The runtime wasm imports `wasi_snapshot_preview1.sched_yield`
    and uses shared memory.  Browsers + wasmtime both support
    this, but the imports list grows from 0 (current
    runtime is pure malloc, no WASI imports) to 13.
  - Alternative: patch cctz to no-op TimeZoneMutex on wasm (single-
    threaded execution model).  Keeps the runtime on
    vanilla wasm32-wasi.  Adds a second file to the patch set.

**Recommendation:** ship Phase C on wasm32-wasi-threads; revisit
cctz-noop patch if the Phase D Chrome target finds shared-memory
restrictions.

### 7.3 Binary size

Projected ~1.2 MB stripped / ~400 KB gzipped (E9).  This is 5× the
current `cel_runtime.wasm`.  With LTO, projection narrows to ~800
KB / ~300 KB.  Update `phase-c-design.md` §3 + `DESIGN.md` §9 to
reflect.

### 7.4 Spec-strict timestamp validation

`absl::ParseTime` admits some timestamps that cel-cpp rejects (e.g.
year 0 in some compilations).  The Phase C kernel MUST post-validate
against the cel-cpp `RejectsAsTimestampPerCEL` envelope explicitly,
not just trust ParseTime's output.

### 7.5 Regex compile cache eviction

A naive per-Instance regex cache grows unbounded under adversarial
input (each new regex evicts something else).  Phase C ships with
a 128-entry LRU; revisit sizing post-conformance.

### 7.6 Cross-platform CI

E3's toolchain hardcodes `@wasi_sdk_darwin_arm64`.  Add platform
selects so it dispatches on `@platforms//os` × `@platforms//cpu`
mirroring `//third_party/wasi_sdk:BUILD.bazel`'s existing alias
pattern.

## 8 What the implementation session should NOT do

  - Don't pivot to Path B or C without explicit user authorisation.
    Path A is proven; the only blocker (E8 runtime smoke) is an
    init-call fix, not a path issue.
  - Don't commit pre-built `.a` files.  All artefacts produced by
    bazel build are ephemeral; the repo carries source + patches.
  - Don't try to keep the runtime on vanilla `wasm32-wasi` unless
    the cctz-noop patch lands in the same commit.

## 9 What this plan unblocks

After Phase C ships:

  - `string.textproto::matches/*` 9 SKIPs → PASS.
  - `timestamps.textproto` 75/76 → 76/76 PASS.
  - `string_ext.textproto::format/*` partial flip (only
    `format()` rows).
  - Every wasi-libc-compatible host (Chrome via Phase D, wasmtime,
    node, custom embedders) inherits matches + parse + format
    from the runtime.  Four host-side trampolines deleted.

The implementing agent should be able to start coding within 30
minutes of reading this plan, the E3-E10 probe RESULTs, and the
existing `phase-c-design.md`.
