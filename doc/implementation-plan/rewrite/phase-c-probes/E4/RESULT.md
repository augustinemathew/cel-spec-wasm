# Probe E4: absl cross-compile via bazel cc_toolchain (Path A canary)

**Status:** PASS

## Method

Builds against the E3 toolchain.  Started with `absl/strings:strings`
(a real .cc target, not just header-only).  Then escalated to
`absl/time:time` per the brief.

```
bazel build @com_google_absl//absl/strings \
  --platforms=//doc/implementation-plan/rewrite/phase-c-probes/E3:wasm32_wasi \
  --extra_toolchains=//doc/implementation-plan/rewrite/phase-c-probes/E3:wasm32_wasi_toolchain

bazel build @com_google_absl//absl/time ...  (same flags)
```

## Output

  - `bazel-bin/external/abseil-cpp~/absl/strings/libstrings.a` — built
    in **2.6s** (19 actions).
  - `bazel-bin/external/abseil-cpp~/absl/time/libtime.a` — built in
    **3.2s** (45 actions).  **465,026 bytes** (~454 KB) of static
    archive; this is the per-`.a`-file size before LTO + GC strips
    unused symbols.

Both archives are real `wasm-object` archives (verified via
`llvm-ar t`).

## Findings — failure modes hit + fixes

Three failure modes between "canary works" and "absl/time works":

### Failure mode #4 — `<array>` not found (libc++ headers)

Building `absl/base:log_severity` (transitive dep of `absl/strings`)
errored on `#include <array>`: `'array' file not found`.

  - **Cause:** the wasi-sdk libc++ headers live under
    `<sysroot>/include/<target>/c++/v1/`, NOT
    `<sysroot>/include/c++/v1/` — the latter doesn't exist.  The
    cc_toolchain_config's `--sysroot=...` flag doesn't make clang
    search the per-target c++/v1 dir by default for bazel-driven
    invocations (the bazel cc rules call clang directly without
    relying on driver-mode header discovery).
  - **Fix:** add a `-isystem <path>` for each entry in
    `builtin_include_directories`, emitted from the toolchain
    config's `default_compile_flags` feature.  Order matters:
    libc++ headers FIRST so they take precedence over the C-only
    sysroot.

### Failure mode #5 — `sysinfo.cc` calls `std::thread::hardware_concurrency`

Building `absl/base:base` errored on `error: no member named 'thread'
in namespace 'std'` in `absl/base/internal/sysinfo.cc:152`.

  - **Cause:** same one exp1_re2 hit.  `GetNumCPUs()` falls through
    to a `std::thread::hardware_concurrency()` call on POSIX-ish
    targets; wasm32-wasi doesn't have `<thread>`.
  - **Fix:** vendored `wasm_compilation_experiments/exp1_re2/absl-wasm.patch`
    to `third_party/patches/abseil-cpp-wasm-sysinfo.patch`; added
    a `single_version_override` for `abseil-cpp` in `MODULE.bazel`
    referencing the patch.  Patch adds a `#elif defined(__wasm__) ||
    defined(__wasi__)` branch returning 1.

### Failure mode #6 — `<mutex>` missing on vanilla wasm32-wasi

Building `absl/time/internal/cctz:time_zone` errored on
`std::mutex& TimeZoneMutex()`: `no type named 'mutex' in namespace 'std'`.

  - **Cause:** wasi-sdk's libc++ for vanilla `wasm32-wasi` is built
    with `_LIBCPP_HAS_NO_THREADS` defined, which gates out the
    `mutex` class.  cctz's TimeZoneMutex needs it for its
    timezone-cache locking.
  - **Fix:** switched the toolchain's target from `wasm32-wasi` to
    `wasm32-wasi-threads`.  This is the same choice exp1_re2 made
    for the same reason.  Cost: requires `-pthread`,
    `_WASI_EMULATED_*` defines, and `--shared-memory
    --max-memory=…` + `-lwasi-emulated-*` link flags.

### Failure mode #7 — pthread.h static-asserts on missing `-pthread`

After switching target, `__libcpp_thread_detach` errored with a
`_Static_assert(0, "This mode of WASI does not have threads
enabled; ...")` macro expansion.

  - **Cause:** wasi-sdk's pthread.h has a guard requiring `-pthread`
    (or `_WASI_EMULATED_PTHREAD`) be set when compiling.
  - **Fix:** added `-pthread` + `_WASI_EMULATED_*` defines to the
    toolchain's `default_compile_flags`, plus the matching
    `-lwasi-emulated-*` libs and `--shared-memory` /
    `--max-memory=…` linker flags to `default_link_flags`.

## Patches needed

One: `third_party/patches/abseil-cpp-wasm-sysinfo.patch` (3-line diff
to `absl/base/internal/sysinfo.cc`).  Applied via
`single_version_override` in `MODULE.bazel`.  This matches what
exp1_re2's CMake build did.

## Feature count delta vs E3

E3's 8 features + 4 extra compile flags + 6 extra link flags.
Total Path A wiring: **~10 features** in cc_toolchain_config.bzl;
nowhere near the 50 the brief estimated.  The bulk of
unix_cc_toolchain_config.bzl's 2k LoC is layering/sanitizer/PIC
plumbing we don't need.

## Tension — vanilla `wasm32-wasi` vs `wasm32-wasi-threads`

The runtime is currently built `wasm32-wasi` (vanilla, per DESIGN.md
§3).  This Path A toolchain uses `wasm32-wasi-threads` because
cctz requires `<mutex>` and the non-threads sysroot doesn't ship
it.

**This is the one architecturally interesting choice** the plan
must own:

  - Switching the runtime to `wasm32-wasi-threads` means
    `--shared-memory` linkage, `--import-memory` going back in,
    and the runtime exposing a SharedArrayBuffer-style memory.
    Browsers + wasmtime support this, but the multi-platform CI
    story re-opens.
  - Alternative: patch cctz to no-op the TimeZoneMutex on wasm
    (single-threaded execution model).  Adds a third file to the
    patch set but keeps the runtime on vanilla wasm32-wasi.

The probe used the proven exp1_re2 path (switch target).  The
plan should weigh patching-cctz vs. accepting the threads target.

## Next-step implication

**Path A is fully validated end-to-end at the absl::time level.**
The wiring + patch + threads decision are all known.  E5 (link
absl into a C+C++ wasm with the runtime's arena shape) and E7
(minimum lib set) are now straightforward to run.
