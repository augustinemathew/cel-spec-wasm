# Probe E3: wasm cc_toolchain registration (Path A gateway)

**Status:** PASS

## Method

Wrote a minimal `cc_toolchain_config.bzl` modelled on
`@rules_cc//cc/private/toolchain:unix_cc_toolchain_config.bzl` (~2k LoC
reference) — kept to **8 features** total:

  - `default_compile_flags`: `--target=wasm32-wasi -no-canonical-prefixes
    -Wall`, plus `-std=c11` for C / `-std=c++17 -fno-rtti -fno-exceptions`
    for C++.
  - `default_link_flags`: `--target=wasm32-wasi -nostartfiles
    -Wl,--no-entry -Wl,--allow-undefined`.
  - `opt` / `dbg`: `-O3` / `-O0 -g`.
  - `user_compile_flags` / `user_link_flags`: propagate `copts`/`linkopts`.
  - `sysroot`: `--sysroot=<wasi-sysroot>`.
  - `supports_pic` (disabled — wasm doesn't have ELF-style PIC).

Then wrote `BUILD.bazel` with:

  - A `filegroup(name="wasi_sdk_all")` aliasing `@wasi_sdk_darwin_arm64//:all`.
  - Three sh wrapper scripts (`wasm_clang.sh`, `wasm_ar.sh`, `wasm_nm.sh`)
    that exec the real wasi-sdk binaries.  This is the resolution of
    failure mode #2 (see below): `tool_paths` strings are package-
    relative, not exec-root-relative.
  - `cc_toolchain_config` rule call, `cc_toolchain`, `toolchain`, and
    `platform(name="wasm32_wasi")` with `@platforms//cpu:wasm32` +
    `@platforms//os:wasi` constraints.
  - Canary `cc_binary(name="hello_wasm")` over `hello_wasm.cc`'s
    `int add(int a, int b)` with `export_name("add")`.

Build invocation:

```
bazel build //doc/implementation-plan/rewrite/phase-c-probes/E3:hello_wasm \
  --platforms=//doc/implementation-plan/rewrite/phase-c-probes/E3:wasm32_wasi \
  --extra_toolchains=//doc/implementation-plan/rewrite/phase-c-probes/E3:wasm32_wasi_toolchain
```

Smoke test:

```
cp bazel-bin/.../hello_wasm hello_wasm.wasm
wasmtime run --invoke add hello_wasm.wasm 7 35
# → 42
```

## Output

  - `hello_wasm.wasm` — **161 bytes**, wasm version 0x1 (MVP).
  - Exports: `memory`, `add`.
  - `add(7, 35) = 42` under wasmtime.
  - Zero imports (no wasi_snapshot_preview1).

## Findings — failure modes hit + fixes

**Failure mode #1 (analysis):** `ERROR: no such package '@platforms//cpu'`.
  - **Cause:** `@platforms` was a transitive dep of every existing
    `bazel_dep` but not visible from the root module — bazel's bzlmod
    requires explicit `bazel_dep` for any repo the root module names.
  - **Fix:** added `bazel_dep(name="platforms", version="1.0.0")` to
    `MODULE.bazel`.  This is the **first** required MODULE.bazel
    change for Path A.  No version conflict (already at 1.0.0
    transitively).

**Failure mode #2 (execution):** `execvp(.../E3/external/_main~_repo_rules~wasi_sdk_darwin_arm64/bin/clang++)`
  — `No such file or directory`.
  - **Cause:** `tool_paths` strings in `cc_toolchain_config` are
    treated as **package-relative**, not exec-root-relative.  Bazel
    prepended `doc/implementation-plan/rewrite/phase-c-probes/E3/`
    to the `external/...` path I supplied.
  - **Fix:** three small sh wrapper scripts in the E3 package
    (`wasm_clang.sh`, `wasm_ar.sh`, `wasm_nm.sh`).  Each does
    `exec "$(dirname "$0")/../../../../../external/<wasi_sdk_repo>/bin/<tool>" "$@"`.
    The relative-path `../../../../../external/` resolves up from
    the package (5 levels of `doc/implementation-plan/rewrite/phase-c-probes/E3/`)
    to the exec root, then into `external/`.  The wrappers are
    listed in a `tool_wrappers` filegroup that's unioned with
    `wasi_sdk_all` for `cc_toolchain.all_files`.

**Failure mode #3 — not hit, but expected next:** `cxx_builtin_include_directories`
  is similarly exec-root-relative; passing `external/_main~_repo_rules~…`
  paths Just Worked (bazel did not prepend the package), suggesting
  the rule treats them via `%sysroot%` substitution.  No
  intervention needed.

## Wiring cost vs. brief's estimate

The brief said "~50 features in `cc_toolchain_config.bzl`".  Reality
for **this minimum-canary** case: **8 features**, ~250 LoC
config.bzl.  The full reference (`unix_cc_toolchain_config.bzl`)
is ~2k LoC, but most of those features (PIC variants, profiling,
LTO objects, sanitizers, layering checks, header parsing) are
optional and don't need wiring for our use case.

Likely additions for E4 (absl cross-compile):

  - `archive_param_file` — for the bazel-style ar invocation that
    cc_library produces.
  - `dependency_file` / preprocessor flags for `.d` file emission.
  - May need `archiver_flags` feature for `llvm-ar`'s syntax.

## Update after E4

After running E4, the toolchain target was switched from
`wasm32-wasi` to `wasm32-wasi-threads` (cctz needs `std::mutex`,
which is only available in the threads variant of wasi-sdk's
libc++).  hello_wasm.cc continues to build under the threads
toolchain (verified post-switch).  See E4/RESULT.md for the
details.

## Next-step implication

**Path A is unlocked at the canary level.**  E4 (a real absl
target) is the next gate; the wiring delta from "canary works" to
"absl works" is the load-bearing question.  Estimate: 3-6 more
features need wiring, plus possibly absl-side patches (the
exp1_re2 `absl-wasm.patch` may or may not still be needed under
bazel's selects).
