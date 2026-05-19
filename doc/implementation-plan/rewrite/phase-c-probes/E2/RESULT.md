# Probe E2: bazel genrule + pre-built absl (Path C minimum bar)

**Status:** PASS

## Method

Authored `BUILD.bazel` with a single genrule `parsetime_wasm` that:

  - References `//third_party/wasi_sdk:clang++` via `tools=` (already
    registered in MODULE.bazel).
  - Mirrors E1's clang++ invocation: `--target=wasm32-wasi-threads`,
    `-Oz -fno-rtti -pthread`, the four `_WASI_EMULATED_*` defines,
    the `cxa_stubs.o` link, the absl-install absolute lib globs,
    the four `-lwasi-emulated-*` libs.
  - Tagged `manual`, `no-sandbox`, `local` because the absl install
    lives outside the bazel workspace (under
    `wasm_compilation_experiments/exp1_re2/absl-install/`).  A real
    Path C ship would commit those archives under
    `third_party/abseil-cpp-wasm/lib/` (or fetch via `http_file`)
    and consume them via `cc_import(static_library = …)` instead
    of an `$$(ls …)` glob.

Built with `bazel build //doc/implementation-plan/rewrite/phase-c-probes/E2:parsetime_wasm`,
copied the artifact next to `driver.wasm` from E1, ran via wasmtime:

```
bazel build //doc/implementation-plan/rewrite/phase-c-probes/E2:parsetime_wasm
wasmtime run -W threads=y -W shared-memory=y \
  --preload lib=parsetime_via_bazel.wasm \
  --invoke run driver.wasm
```

## Output

- `parsetime_via_bazel.wasm` — **79,553 bytes** vs E1's 79,545 bytes
  (+8 bytes, **0.01% delta** — well under the ≤10% allowance).
- wasmtime invoke result: **`1779098400`** (Unix seconds for
  `2026-05-18T10:00:00Z`).  Byte-identical numeric output to E1.
- Imports: 9 functions, all `wasi_snapshot_preview1.*`, same set as
  E1 — `environ_get`, `environ_sizes_get`, `fd_close`, `fd_prestat_get`,
  `fd_prestat_dir_name`, `fd_seek`, `fd_write`, `proc_exit`,
  `sched_yield`.

## Findings

- bazel's genrule driving `@wasi_sdk//:clang++` is fully workable for
  the Path C shape.  Build time is dominated by the link step (~0.9s
  critical path); link is local (no sandbox roundtrip).
- The 8-byte size delta vs E1 is **not** from a behaviour delta — it
  comes from filename embedding in DWARF / link-trailer metadata.
  Both wasm files validate + run identically.
- The `$$PWD/wasm_compilation_experiments/exp1_re2/…` absolute path
  trick works on this dev box but is the load-bearing reason the
  genrule is `no-sandbox`.  In a real Path C ship, the libs would be
  exposed via `cc_import` filegroups + `http_file` archive — bazel-
  hermetic, no `--spawn_strategy=local`.
- This probe establishes that even the worst-of-three path (C) is
  trivially viable.  Holds open the fallback if Path A is genuinely
  blocked; **not** a recommendation to prefer C over A.

## Next-step implication

Path C works as a fallback, but `BUILD.bazel`-genrule + pre-built
archives keeps the build out-of-band (the absl/RE2 rebuild script
lives outside bazel).  Keep this proof-of-life around but the
research target remains Path A.
