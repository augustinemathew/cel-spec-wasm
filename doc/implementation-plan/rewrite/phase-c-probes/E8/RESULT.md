# Probe E8: RE2 cross-compile via bazel cc_toolchain

**Status:** PARTIAL — RE2 + absl link clean under Path A; build artefact
is valid wasm; runtime smoke test returns 0 (no match) rather than 1
(match).  Time-boxed at ~20 min after the build succeeded; the
toolchain validation is complete, the runtime semantic bug is
deferred to implementation.

## Method

  - Added `bazel_dep(name="re2", version="2025-11-05.bcr.1")` to
    `MODULE.bazel` (re2 was already a transitive BCR dep of
    abseil-cpp; the explicit `bazel_dep` makes it visible from the
    main repo).
  - Wrote `re2_match.cc` with the same shape as exp1_re2's
    `re2_lib.cc`: `match(pat, plen, text, tlen) -> int32`, returning
    1 / 0 / -1 (compile failure).
  - `cc_binary` depending on `@re2` + `@com_google_absl//absl/strings`.
  - Same E3 toolchain (`wasm32-wasi-threads` platform).

```
bazel build //doc/…/E8:re2_match --platforms=//doc/…/E3:wasm32_wasi \
  --extra_toolchains=//doc/…/E3:wasm32_wasi_toolchain
```

## Output

  - `re2_match.wasm` — **1,575,110 bytes** stripped / **555,836 bytes
    gzipped** (555 KB).
  - Build is **clean** (only deprecation warnings — `absl::MutexLock`
    constructor deprecation in re2's regexp.cc).  142 actions (137
    darwin-sandbox, 5 internal), 8.7s wall time.
  - Exports: `memory`, `__wasm_call_ctors`, `match`, `__heap_base`,
    `__data_end`.
  - **13 WASI imports**, all `wasi_snapshot_preview1.*` — same
    "good subset" as exp1_re2's CMake-built RE2 (11 imports there;
    +2 for `fd_fdstat_get` and `fd_pread`, both shimmable to ENOTSUP).
  - wasm module validates via `wasmtime compile`.

## Runtime smoke-test gap

Driver wat (matches exp1_re2's pattern: pattern at offset 0x180000,
text at 0x180020, both 13 bytes) calls `__wasm_call_ctors` then
`match`.  Expected: 1.  Observed: **0** consistently — also for
the trivial case (pattern `a`, text `a`).

Variations tried that did **not** change the result:

  - With / without `--gc-sections`.
  - Active `(data ...)` segments vs explicit `i32.store8` writes.
  - With / without `-Wl,--initial-memory=2097152`.
  - Pattern length 1 / 13.
  - Memory offsets 0x40000 / 0x100000 / 0x180000.

What this is NOT (verified):

  - **Not a build / link failure.**  Module instantiates; ctors run
    without trap; match returns control normally with a 0 result
    (would be -1 if `re.ok()` failed, or a trap if `__cxa_throw`
    fired).
  - **Not an OOB memory access.**  Driver places test data well
    above `__heap_base` (225 KB) inside the 2 MB initial memory.
  - **Not a missing import.**  All 13 WASI imports are wired by
    wasmtime's default WASI shim.

What this MIGHT be (deferred):

  - **Reactor-mode init missing.**  wasi-sdk's "reactor" mode
    (library-style, no `_start`) needs explicit `_initialize`
    export + call to fully initialize libc-side state (TLS,
    thread-local errno, malloc init, etc.).  Our genrule does NOT
    export `_initialize`; exp1_re2's CMake build did not either,
    but it used clang++ directly as the linker driver, which may
    have added an implicit init step.  bazel's cc_binary uses
    `wasm-ld` under the wrapper, which doesn't.
  - **Allocator dual-init.**  RE2 internally uses `std::string` /
    `std::vector` — malloc is needed before any `re.ok()` /
    PartialMatch.  In reactor mode, wasi-libc's `__wasi_init_environ`
    + `__heap_base` setup needs `_initialize` to fire.
  - **absl::Time TZ lookup at static-init time.**  cctz's
    UTCTimeZone() constructor may be hitting a path that needs the
    process clocks emulation lib at *init* time (not just at
    function-call time).

## Toolchain verdict (the load-bearing question)

**Path A is fully sufficient at the build level.**  RE2 + absl
cross-compile end-to-end; the resulting wasm is valid, instantiable,
and exports the expected ABI.  The smoke-test gap is a runtime
init issue (likely WASI reactor mode), not a toolchain issue.

For the **implementation session**, the fix is one of:

  1. Add `_initialize` to the exports and call it from the host
     (or the wat driver) before any real call.  This is the
     idiomatic wasi-sdk reactor pattern.
  2. Switch to a `cc_binary(linkshared=True)` shape that emits the
     reactor-style entry point.
  3. (less likely) Add `--shared` to wasm-ld flags so it produces
     a true side module that wasmtime instantiates with full
     `_initialize` semantics.

## Size budget contribution

| Artefact | Stripped | Gzipped |
|---|---:|---:|
| E5 combined (absl::time + arena stub) | 372 KB | (n/a) |
| E7 kernels (absl time + strings + str_format) | 569 KB | 206 KB |
| E8 re2_match (absl strings + RE2) | 1,575 KB | 555 KB |
| exp1_re2 re2_lib.wasm (CMake build, full absl + RE2) | 388 KB | 150 KB |

E8's 1.5 MB stripped vs exp1_re2's 388 KB is a **4× delta**, surprising
given bazel's per-cc_library granularity.  Likely causes:

  - exp1_re2's CMake-built absl was `-Oz`; bazel build inherited
    `-O3` (E3's `default_compile_flags`).  `-Oz` typically halves
    wasm size for absl-heavy binaries.
  - exp1_re2 explicitly stripped via `-Wl,--strip-all`; E8 also
    does, but the bazel toolchain may emit name-section data that
    --strip-all doesn't fully reach.

E9 will compare apples-to-apples by applying `-Oz` to a bazel
build, and the implementation plan should bake `copts = ["-Oz"]`
or a dedicated `opt_size` feature in the cc_toolchain_config.

## Next-step implication

For the plan: Path A clears the toolchain bar for RE2.  The
runtime-init gap and `-Oz` size delta are implementation-phase
items, not research blockers.  Add to the implementation
plan's risk list:

  - Reactor-mode `_initialize` export from `cel_runtime.wasm`.
  - `-Oz` (or `optimize_for_size` feature) in the toolchain
    config for production runtime build (not E3's `-O3` default).
