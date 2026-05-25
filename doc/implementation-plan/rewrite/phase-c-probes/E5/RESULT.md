# Probe E5: link a C runtime + C++ absl into one wasm via bazel

**Status:** PASS

## Method

Two TUs, one bazel `cc_binary`:

  - `runtime.c` — C stubs for `arena_alloc(size_t)` + `arena_reset()`.
    Static bss buffer, no malloc, no WASI imports.  Mirrors the
    cel_runtime arena shape in miniature.
  - `time_parse.cc` — calls `absl::ParseTime` + writes a scratch
    region via `arena_alloc`, exports `parse_timestamp` with the
    same `(buf, len, out_seconds, out_nanos) -> int32` ABI as the
    E1 baseline.

`cc_library` for each, `cc_binary` linking both + `@com_google_absl//absl/time`
+ `@com_google_absl//absl/strings`.  Built via the E3 toolchain at
`--platforms=//doc/.../E3:wasm32_wasi`.

Driver: hand-coded `driver.wat` imports `lib.memory` + `lib.parse_timestamp`,
writes the test RFC3339 string at offset 0x80000 (above the lib's
~256 KB static-data + stack + bss region), reads back the parsed
seconds value.

```
bazel build //doc/…/E5:combined --platforms=//doc/…/E3:wasm32_wasi \
  --extra_toolchains=//doc/…/E3:wasm32_wasi_toolchain
wat2wasm --enable-threads driver.wat -o driver.wasm
wasmtime run -W threads=y -W shared-memory=y \
  --preload lib=combined.wasm --invoke run driver.wasm
# → 1779098400  ✓
```

## Output

  - `combined.wasm` — **372,103 bytes** (372 KB).
  - Successful wasmtime invoke: `1779098400` (Unix seconds for
    `2026-05-18T10:00:00Z`).
  - Imports: **10 functions**, all `wasi_snapshot_preview1.*`:
    `environ_get`, `environ_sizes_get`, `clock_time_get`, `fd_close`,
    `fd_prestat_get`, `fd_prestat_dir_name`, `fd_seek`, `fd_write`,
    `proc_exit`, `sched_yield`.  One more than E1 baseline
    (`clock_time_get`) — likely because bazel + threads target
    includes a chrono symbol path that E1's CMake build dead-code-
    elided.  All in the WASI "good subset" per exp1_re2.

## Findings — failure modes hit + fixes

### Failure mode #8 — clang++ rejects `-std=c11`

Building `runtime.c` errored: `invalid argument '-std=c11' not allowed
with 'C++'`.

  - **Cause:** the original `wasm_clang.sh` wrapper unconditionally
    invoked `clang++` (so libc++ would be linked).  But clang++
    treats `.c` files as C++ and rejects C-only flags.
  - **Fix:** dispatch on input extension.  The wrapper now scans
    args for `*.cc / *.cpp / *.cxx / *.C` → `clang++`; `*.c` →
    `clang`; default (link / no-source actions) → `clang++` (so
    libc++ pulls in for link).

### No other failure modes

bazel-driven link of C + C++ + abseil + libc++ produced a valid
wasm on first try after the wrapper fix.  No additional
cc_toolchain_config features needed beyond what E3/E4 already
wired.

## Driver memory layout note

The lib's static-data + 64KB-arena + 64KB-stack collectively land
around offset 0x25890 (stack pointer init).  The lib was linked
with `-Wl,--initial-memory=1048576` (1 MB initial memory, max=64
MB) so the driver can place test data at 0x80000 without growing
memory.  The runtime's existing genrule in
`runtime/BUILD.bazel` already does similar memory
sizing.

## Size budget contribution

`combined.wasm` at 372 KB is ~1.5× the current `cel_runtime.wasm`
(241 KB).  This is the bare minimum (only absl::time + absl::strings
+ libc++ static + runtime stub).  Full Phase C delta (timestamp +
duration + RE2 + format) will be larger; E9 quantifies.

## Next-step implication

Path A scales from "hello_wasm" through "abseil-cpp::time linked
into a C+C++ runtime."  E6 (rules_foreign_cc fallback) and E7
(minimum lib set) can now run with high confidence Path A is the
choice; E8 (RE2) is the next real-library gate.
