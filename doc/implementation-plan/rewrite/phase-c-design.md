# Phase C — In-runtime parsers + RE2 / matches()

Branch: `phase-c-libraries` (forked from `wasi-malloc-migration` @ `a43ee8b`).

**Status: in flight, started 2026-05-18.**

> **Memory consequence consolidated into
> [`memory-layout-design.md`](memory-layout-design.md).** Phase C's
> structural memory change — the flip to `wasm32-wasi-threads` + a
> **shared** `(memory 4 1024 shared)` (cctz needs `<mutex>`) — is
> captured in that singular memory doc (§1, §7). **This doc remains the
> historical build-plan** for the in-runtime parsers + RE2/`matches()`
> work (Slice 1/2, the vendoring approach, risks). Read
> `memory-layout-design.md` for the live memory model.

The wasi-malloc-migration cleared the path: runtime now uses
wasi-sdk + dlmalloc, so cross-compiled C/C++ libraries (RE2, absl)
can link in without dual-allocator pain.  Phase C delivers on that
architectural payoff.

---

## 1 Goal

Move the runtime's last "host trampoline for library work"
dependencies into the runtime itself:

  - `matches(string, string) -> bool` — currently unimplemented
    (in `kExplicitlyUnimplementedIds`); must use RE2.
  - `timestamp(string) -> timestamp` — currently
    `cel_host.cel_timestamp_parse` (uses absl::ParseTime).
  - `duration(string) -> duration` — currently
    `cel_host.cel_duration_parse` (uses absl::ParseDuration).
  - `timestamp.string() -> string` — currently
    `cel_host.cel_timestamp_format` (uses absl::FormatTime).
  - `duration.string() -> string` — currently
    `cel_host.cel_duration_format` (uses absl::FormatDuration).
  - `strings.format(string, list) -> string` — currently
    unimplemented (string_ext.textproto: 0/216 PASS).

After Phase C: every wasi-libc-compatible host (Chrome, wasmtime,
node, embedders in any language) inherits matches / parse / format
from the runtime — no host work required.

## 2 Slicing

Phase C splits into two independent slices.  Slice 1 needs no
vendoring and can ship first.  Slice 2 needs the absl+RE2 bazel
integration, which is the architecturally interesting bit.

### Slice 1 — Pure-C parsers + formatters (no vendoring)

| ID | Slice | Status | Days |
|---|---|---|---:|
| C1.A | `cel_duration_parse_at_v` in `cel_time.c` | ☐ | 0.5 |
| C1.B | `cel_timestamp_parse_at_v` in `cel_time.c` | ☐ | 1.0 |
| C1.C | `cel_duration_format_at_v` + `cel_timestamp_format_at_v` | ☐ | 0.5 |
| C1.D | Codegen reroutes `string_to_*` / `*_to_string` from `cel_host.*` to `cel_runtime.cel_*_at_v` | ☐ | 0.25 |
| C1.E | Delete `CelTimestampParseImpl` + `CelDurationParseImpl` + `CelTimestampFormatImpl` + `CelDurationFormatImpl` host trampolines | ☐ | 0.25 |
| C1.F | Conformance verification (timestamps.textproto + parse rows in basic.textproto + conversion rows) | ☐ | 0.5 |

**Total Slice 1: ~3 days.**

Why pure C is reasonable for these:

  - **Duration parsing** — grammar is tiny: `sign? (digits (.digits)? unit)+`
    where `unit ∈ {ns, us, µs, ms, s, m, h}`.  Validation: ≤ ±315B
    seconds.  ~80 LoC of straight-line parsing.
  - **RFC3339 timestamp parsing** — cel-cpp restricts to 4-digit
    year, year 0001-9999, RFC3339_full subset.  ~150 LoC including
    leap-year math + fractional-second handling.  Already partially
    present in `cel_time.c` for the FormatTime side.
  - **Format** — symmetric inverse of parse; ~50 LoC each.

### Slice 2 — RE2 + absl vendoring (real library work)

| ID | Slice | Status | Days |
|---|---|---|---:|
| C2.A | abseil-cpp via `http_archive` + `rules_foreign_cc` CMake wrapper | ☐ | 1.5 |
| C2.B | re2 via `http_archive`, built against vendored absl | ☐ | 0.5 |
| C2.C | Wire `libabsl_strings.a` + `libre2.a` into `cel_runtime.wasm` link | ☐ | 0.5 |
| C2.D | `cel_matches_at_vv` kernel + per-Instance regex cache | ☐ | 1.0 |
| C2.E | Codegen routes `matches` + `matches_string` out of `kExplicitlyUnimplementedIds` | ☐ | 0.25 |
| C2.F | Conformance: `string.textproto::matches/*` 9 SKIPs → PASS | ☐ | 0.5 |
| C2.G | `strings.format` runtime kernel — `printf`-like, validates
       indices, type-coerces.  Uses pure-C `snprintf` (no absl) | ☐ | 1.0 |
| C2.H | Conformance: subset of `string_ext.textproto::format/*` flips | ☐ | 0.5 |

**Total Slice 2: ~5.5-6 days.**

The C2.A scaffolding work is the long pole.  Two viable approaches:

  - **rules_foreign_cc** — bazel's official rules for CMake-based
    deps.  Pro: properly integrated; the wasi-sdk toolchain is
    passed through.  Con: rules_foreign_cc has its own learning
    curve; abseil's CMake build is parameter-heavy (40+ modules,
    optional features).
  - **Genrule + script** — a bazel genrule that invokes a shell
    script that runs CMake against the http_archived source.  Pro:
    simpler bazel surface.  Con: less hermetic; the script needs to
    drive CMake parameter setting.

Recommendation: **rules_foreign_cc**.  The hermeticity is worth
the learning curve, and the wasi-sdk toolchain integration is
cleaner.

The `wasm_compilation_experiments/exp1_re2/` work proved the
CMake build produces 388 KB stripped of absl + re2.  This Slice 2
work is "make that bazel-native + integrate with the runtime
link step".

## 3 Risks

  - **Slice 1 spec compliance** — pure-C parsers must match
    cel-cpp's `absl::ParseTime` + `RejectsAsTimestampPerCEL`
    behaviour byte-for-byte to avoid silent conformance
    regressions.  Mitigation: drive the implementation from the
    timestamps.textproto + parse-row conformance cases, not from
    the langdef alone.
  - **Slice 2 binary size** — adding ~388 KB of vendored absl+RE2
    (compressed) into `cel_runtime.wasm` blows the current 241 KB
    by ~3×.  Budget is already over the DESIGN §9 ≤2× target;
    this widens the gap.  Acknowledge in POST_MIGRATION_BENCH.md
    + accept the trade — Phase C is the architectural payoff
    that justifies the size.
  - **Slice 2 wasi-sdk patching** — abseil-cpp needs the
    `absl-wasm.patch` (1 line) to compile under wasm32-wasi.
    Vendor the patch under `third_party/patches/`.

## 4 Out of scope

  - Phase D — Chrome / browser host port.
  - `string_ext.textproto`'s non-format() rows (96 fails) — they
    need other strings extensions (split, join, replace, …).
    Possible follow-up after Phase C ships.
  - The 491 pre-existing conformance failures from missing
    extension subsystems (math_ext, network_ext, optionals,
    encoders_ext, block_ext).
