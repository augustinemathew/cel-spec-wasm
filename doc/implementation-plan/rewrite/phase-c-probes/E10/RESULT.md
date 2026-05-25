# Probe E10: kernel-level integration sketch

**Status:** PASS

## Method

Audited the existing call sites for the four host trampolines and
the two unimplemented overload ids that Phase C retires:

  - `eval/internal/cel_host.cc:3031-3199` — the four
    `CelTimestampParseImpl` / `CelDurationParseImpl` /
    `CelTimestampFormatImpl` / `CelDurationFormatImpl` trampolines
    + their `RegisterHost(...)` registrations.
  - `compiler/codegen/overload_table.cc:332-339` — the four
    `kCelHost` seeds that route the spec overload ids to those
    trampolines.
  - `compiler/codegen/overload_table.cc:469-480` — the
    `kExplicitlyUnimplementedIds` array containing `"matches"` and
    `"matches_string"`.

Wrote `integration_sketch.md` with:

  - The new `runtime/cel_time_parse.cc` C++ TU sketch
    (signatures + body shapes for the four parse/format kernels
    plus `cel_matches_at_vv`).
  - The `cc_library` + `cc_binary` BUILD.bazel changes to fold the
    C++ kernels into the existing runtime link.
  - The four-line diff to `overload_table.cc`'s `kBuiltinSeeds`
    rerouting + the two-line deletion from
    `kExplicitlyUnimplementedIds`.
  - The ~180 LoC of host-side `cel_host.cc` getting deleted.

## Findings

  - **Arena interaction.**  Each string-producing kernel calls
    `arena_alloc` for the output bytes — same shape as
    `cel_string_concat_at_vv`.  No memory-model changes.
  - **`strings.format` is the awkward one.**  CEL spec mandates a
    runtime format string; `absl::StrFormat` requires constexpr.
    Implementation must use `absl::FormatUntyped` + per-arg
    `FormatArg` packing.  Non-trivial ~150-200 LoC kernel.
  - **Reactor-mode `_initialize`.**  E8 surfaced that bazel-built
    wasm doesn't auto-call `__wasm_call_ctors`; the runtime's host
    loader needs to invoke it after instantiate.  5-LoC change
    flagged for the implementation phase.
  - **Cross-platform.**  E3's toolchain hardcodes
    `@wasi_sdk_darwin_arm64`; need select-based dispatch on
    `@platforms//os` × `@platforms//cpu` mirroring
    `//third_party/wasi_sdk:BUILD.bazel`.
  - **Lint coverage.**  New `.bzl` + sh files need adding to
    `scripts/lint.sh`'s coverage.

## Next-step implication

The implementation order is:

  1. Move E3's toolchain config + wrappers into
     `//third_party/wasi_sdk/` (1d).
  2. Switch `cel_runtime.wasm` from genrule to cc_binary (0.5d).
  3. Land Slice 1 (C1.A-F per phase-c-design.md) — pure-C parsers
     (3d).
  4. Land Slice 2 (C2.A-H per phase-c-design.md) — absl + RE2 + format
     (5.5d).

Total estimated implementation: ~10 days, mostly Slice 2.
