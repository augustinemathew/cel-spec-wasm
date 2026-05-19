# Probe E1: Baseline rebuild of exp_e_absl_parsetime.wasm

**Status:** PASS

## Method

Reran the build command from
`doc/implementation-plan/rewrite/wasi/experiments/exp_e_absl_parsetime.cc:11-24`,
which calls `wasi-sdk-25` clang++ against the cached
`wasm_compilation_experiments/exp1_re2/absl-install/` static libs.

Then ran the resulting `.wasm` via wasmtime against the hand-coded
`driver.wat` (assembled with `wat2wasm --enable-threads`, since the
build is targeted at `wasm32-wasi-threads` and the driver imports
shared memory):

```sh
wat2wasm --enable-threads driver.wat -o driver.wasm
wasmtime run -W threads=y -W shared-memory=y \
  --preload lib=baseline_parsetime.wasm \
  --invoke run driver.wasm
```

## Output

- `baseline_parsetime.wasm` — **79,545 bytes**, byte-identical to the
  archived `doc/implementation-plan/rewrite/wasi/experiments/exp_e_absl_parsetime.wasm`
  (`diff -q` returns 0).
- wasmtime invoke result: **`1779098400`** — matches the expected Unix
  timestamp for `2026-05-18T10:00:00Z`.
- Imports: **9 functions**, all under `wasi_snapshot_preview1`:
  `environ_get`, `environ_sizes_get`, `fd_close`, `fd_prestat_get`,
  `fd_prestat_dir_name`, `fd_seek`, `fd_write`, `proc_exit`,
  `sched_yield`.  All "good subset" per `exp1_re2/RESULTS.md` — no
  Emscripten-specific imports.

## Findings

- The exp1_re2 CMake-built absl + wasi-sdk toolchain still works
  end-to-end with zero modifications.  The pre-built `.a` archives
  under `exp1_re2/absl-install/lib/` are still consumable.
- wat2wasm needs `--enable-threads` to assemble a driver that imports
  `(memory ... shared)`; the documented build command did not surface
  this.  Worth pinning in the plan as a tooling note.
- Build was clean: no warnings, no patches needed.  All flags
  (`-D_WASI_EMULATED_SIGNAL`, etc.) and the `cxa_stubs.o` are still
  required because absl is built `wasm32-wasi-threads`, which pulls
  `std::mutex` and an exception-throw site that `__cxa_throw` stubs
  out via `__builtin_trap`.

## Next-step implication

Path C (pre-built libs) is trivially viable as a fallback — the
artifacts already exist on disk.  Path A / B viability depends on
later probes.  The baseline is locked: any later experiment that
diverges by more than ±10% on size or output should explain why.
