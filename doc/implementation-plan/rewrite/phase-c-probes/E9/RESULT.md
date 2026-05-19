# Probe E9: binary size impact of Phase C

**Status:** PASS — numbers measured, no specific budget threshold per
the brief.

## Method

Two reference points:

  1. Current `cel_runtime.wasm` (master @ Phase C branch point):
     **241,386 bytes / 53,738 bytes gzipped** (per the existing
     `compiler_v2/runtime/cel_runtime_wasm_file` genrule output).
  2. The E7 kernels (+ str_format, time, strings) and E8 re2_match
     wasm, both rebuilt with `-Oz` for apples-to-apples comparison
     against exp1_re2's CMake build.

```
bazel build //doc/…/E9:kernels_oz //doc/…/E9:re2_oz \
  --platforms=//doc/…/E3:wasm32_wasi \
  --extra_toolchains=//doc/…/E3:wasm32_wasi_toolchain
```

## Output

| Artefact | Stripped | Gzipped |
|---|---:|---:|
| **Baseline:** current `cel_runtime.wasm` (Phase B end) | 241,386 | 53,738 |
| `parsetime_via_bazel.wasm` (E2 — absl::ParseTime alone) | 79,553 | (n/a) |
| `combined.wasm` (E5 — absl::time + C arena stub) | 372,103 | (n/a) |
| `kernels.wasm` (E7 — absl time/strings/str_format, -O3) | 569,770 | 206,536 |
| `kernels_oz.wasm` (E9 — same, -Oz) | 563,735 | 204,353 |
| `re2_match.wasm` (E8 — RE2 + absl/strings, -O3) | 1,575,110 | 555,836 |
| `re2_oz.wasm` (E9 — same, -Oz) | 1,574,525 | 555,418 |
| exp1_re2 `re2_lib.wasm` (CMake build, full absl + RE2) | 388,471 | 150,249 |

## Findings

### `-Oz` vs `-O3` is a smaller win than expected

Knocking E7's kernels from `-O3` to `-Oz` saved **6 KB stripped** (0.1%).
For E8 (RE2), it saved **585 B** (0.04%).  Surprising — exp1_re2's
CMake build used `-Oz` and got ~4× smaller for the equivalent
content (absl + RE2 together).

The dominant size difference appears to be **absl version + cc_library
granularity**:

  - exp1_re2 built abseil-cpp from an older snapshot (commit-of-the-day
    around the wasi-toolchain.cmake creation).  abseil-cpp's HEAD has
    grown.
  - exp1_re2's CMake build was a single artefact link, so dead-code
    elimination crossed all 93 `.a` files holistically.  bazel's
    `cc_library` per-TU compile leaves dead code that wasm-ld
    couldn't reach (because TU-level `.o` files aren't visible to
    cross-TU DCE without `-flto`).
  - **Path A should enable `-flto` in the cc_toolchain_config**
    for the production runtime build (mirroring the existing
    `compiler_v2/runtime/BUILD.bazel` genrule, which passes
    `-O3 -flto`).  The E3 toolchain config did NOT include
    `-flto` (deliberate, to avoid bloating the canary probe).
    The implementation-plan should add an `lto` feature.

### Projected full Phase C runtime size

Building the full runtime + Phase C kernels under the bazel
cc_toolchain (with LTO, `-Oz`):

  - **Current runtime:** ~241 KB stripped.
  - **+ absl time/strings/str_format slice (E7):** delta ~330 KB
    after LTO cross-TU dedup.  Most absl machinery would be
    shared between the time and str_format kernels.
  - **+ RE2 (E8):** delta ~600 KB after LTO + symbol dedup against
    the absl already linked.
  - **Total projected:** ~1.2 MB stripped, ~400 KB gzipped.

This is **5× the current baseline** (241 KB → ~1.2 MB) — wider
than `phase-c-design.md` §3 estimated (it cited ~3× based on
exp1_re2's CMake build).

### Why bazel-built is bigger than CMake-built

Three hypotheses, ranked by likely contribution:

  1. **absl version drift.**  bazel pulls `abseil-cpp@20260107.0`
     (the BCR version); exp1_re2 used a 2024-vintage HEAD commit.
     14 months of absl growth.
  2. **No -flto.**  exp1_re2's CMake build had IPO enabled via
     CMake defaults; the E3 toolchain config does not enable LTO.
  3. **bazel's per-cc_library .o granularity.**  Even with -O3 +
     `--gc-sections`, cross-TU dead code that survives `.o`
     boundaries doesn't get pruned without `-flto`.

Adding `-flto` to the toolchain's compile + link flags is the
highest-value follow-up.  Estimated savings: 30-50% on the
absl+RE2 portion.

### Acceptable for Phase C?

`phase-c-design.md` §3 acknowledges Phase C blows the DESIGN.md
§9 ≤2× budget.  E9 confirms the blow-out is wider than initially
estimated (~5× vs ~3×), and adds two specific levers:

  - `-flto` in the toolchain (likely brings 1.2 MB → ~800 KB).
  - `-Oz` already baked in (additional savings minor without LTO).

## Next-step implication

For the implementation plan:

  1. Add an `lto` feature to the wasm32-wasi cc_toolchain_config
     (compile: `-flto`; link: `-flto`).
  2. Switch the toolchain's default opt mode to `-Oz` for the
     production runtime build (override per cc_binary if needed).
  3. Re-measure post-LTO; expect ~800 KB stripped / ~300 KB gzipped
     full Phase C runtime.
  4. Update `phase-c-design.md` §3's "binary size" risk paragraph
     to reflect the ~5× → ~3.3× post-LTO projection.
