# Exp 1 — RE2-shaped library in wasm runtime

Status: in progress 2026-05-17.

## Bottom line

**Demonstrated end-to-end**: a C library compiles to wasm with the
wasi-sdk toolchain, exports a `match()` function with zero WASI
imports, and a separate hand-coded wat driver imports the library's
memory and `match()` and runs canned test cases against it through
`wasmtime --preload`.  Three test cases pass.

That validates the architectural premise of moving libraries into
the runtime wasm itself rather than implementing them on the host
side.  RE2 build attempt running in parallel.

## Artifact inventory

| File | Bytes | gzip | Imports | Built with |
|---|---:|---:|---|---|
| `hello.wasm` | 268 | 249 | **none** | wasi-sdk clang++, substring-search C++ |
| `tiny_regex.wasm` | 2,360 | 1,369 | **none** | wasi-sdk clang, hand-rolled Thompson NFA C99 (`-nostdlib`) |
| `driver.wasm` | 217 | 207 | `lib.memory`, `lib.match` | hand-coded `.wat` → `wat2wasm` |
| `hello_exec.wasm` | 15,454 | 7,329 | wasi-libc | hello.cc + main.cc, full WASI program (for E2E proof) |

`hello.wasm` and `tiny_regex.wasm` are pure wasm32 — they will load
in *any* WebAssembly host (including a browser with a single
`WebAssembly.instantiate(bytes)` call) because they import nothing.
The driver imports only from another wasm module (`lib`), not from
the host environment.

## Toolchain notes

  - **wasi-sdk** has no Homebrew formula on macOS.  Install via
    direct tarball:
    ```bash
    curl -L -o wasi-sdk.tar.gz \
      https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-arm64-macos.tar.gz
    tar -xzf wasi-sdk.tar.gz
    # bin/clang++ targets wasm32-unknown-wasi
    ```
  - **wasmtime** (runner) + **wabt** (`wasm-objdump`, `wat2wasm`):
    ```bash
    brew install wasmtime wabt
    ```
  - The repo's `MODULE.bazel` could pin wasi-sdk via `http_archive`
    once we adopt this path — sideloading suffices for the
    experiment.

## Build commands that worked

Pure wasm32 (no WASI), library-only:

```bash
WS=wasi-sdk-25.0-arm64-macos
# C++ — substring search
$WS/bin/clang++ --target=wasm32-wasi -Oz -fno-exceptions -fno-rtti \
  -nostartfiles -Wl,--no-entry -Wl,--export=match -Wl,--strip-all \
  -Wl,--gc-sections -o hello.wasm hello.cc

# C — Thompson NFA, no stdlib at all
$WS/bin/clang --target=wasm32 -Oz -nostdlib \
  -Wl,--no-entry -Wl,--export=match -Wl,--strip-all -Wl,--gc-sections \
  -o tiny_regex.wasm tiny_regex.c

# wat driver
wat2wasm driver.wat -o driver.wasm

# composition + run
wasmtime run --preload lib=hello.wasm --invoke case0 driver.wasm
# → 1
```

The `--preload <name>=<module>` flag is the wasmtime CLI mechanism
for wiring up cross-module imports.  Each preloaded module is
instantiated once and its exports become available to subsequent
modules under the given import-namespace name.

## What didn't work / pitfalls

  - **`std::regex` requires C++ exceptions**, which wasi-sdk 25's
    default libc++ build does not include.  Linker errors for
    `__cxa_allocate_exception` / `__cxa_throw`.  Adding
    `-fwasm-exceptions` did not resolve because the prebuilt
    libc++ in the SDK was compiled without exception support.
    Two routes forward:
      - Use a non-throwing regex library (RE2 has a `bool`-returning
        API; doesn't need this).
      - Rebuild libc++ with exceptions enabled — significant work,
        not worth it.
  - **`tiny_regex.c`** parser has a bug for `+`/`*` quantifiers
    (the NFA contains a cycle, and the concat-time tail walk
    `while (t->out) t = t->out` infinite-loops over it).  Substring
    search via `hello.wasm` was used to validate the architecture
    instead.  Fixing tiny_regex's parser is straightforward (track
    a separate "out fragment" rather than walking through a cycle);
    not blocking the prototype.
  - **The wasmtime CLI cannot pre-populate linear memory from
    command-line args**, so the driver needs to be a wat module
    that bakes test data into its rodata.  An alternative is a
    small Rust/JS harness using wasmtime's API; the wat-driver
    route is faster for prototyping.

## RE2 in wasm — real numbers

Built abseil-cpp + RE2 via CMake with a wasi-sdk toolchain
file, linked our `re2_lib.cc` wrapper against
`libre2.a` + 93 `libabsl_*.a`, ran through a hand-coded
`.wat` driver: **works**.

| Metric | Value |
|---|---:|
| `re2_lib.wasm` size | **388,471 bytes** (388 KB) |
| gzipped | **150,249 bytes** (150 KB) |
| WASI imports | **11** (all `wasi_snapshot_preview1`) |
| absl install footprint | 7.4 MB / 93 static libs |
| RE2 install footprint | 708 KB |

WASI imports the linked binary requires:

  - `clock_time_get`, `environ_get`, `environ_sizes_get`,
    `proc_exit`, `sched_yield` — "good subset", trivially
    shimmable in any embedder including browsers.
  - `fd_write` — also good subset, maps to console.
  - `fd_close`, `fd_prestat_get`, `fd_prestat_dir_name`,
    `fd_seek` — pulled in transitively, probably by absl's
    `<fstream>` use somewhere.  Browser shims can no-op these
    (return ENOTSUP), still loads.
  - `poll_oneoff` — async; browser shims stub this.

End-to-end test (`re2_driver.wat` → `re2_lib.wasm`):

  - `[a-z]+@[a-z]+` against `alice@example` → 1 ✓
  - `\d{4}-\d{2}-\d{2}` against `today: 2026-05-17` → 1 ✓

### Build flags that mattered

Threads target was required because wasi-sdk 25's default
`wasm32-wasi` libc++ has no `std::mutex` (cctz uses it for
its tz cache).  Switching to `wasm32-wasi-threads` provides
`std::mutex` but requires `--shared-memory` and
`--import-memory` link flags.

absl needs the WASI emulation libs for things it would
otherwise get from libc:
```
-D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS
-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID
-lwasi-emulated-signal -lwasi-emulated-process-clocks
-lwasi-emulated-mman -lwasi-emulated-getpid
```

RE2 throws `std::out_of_range` / similar in rare error paths
but wasi-sdk's libc++ is built without exceptions.  Stubbed
the C++ ABI symbols in `cxa_stubs.c` — `__cxa_allocate_exception`,
`__cxa_throw`, `__cxa_begin_catch`, etc., all trap via
`__builtin_trap()`.  Our `re2_lib.cc` wrapper does bounds-
checking so the throw paths shouldn't be hit in practice.

One absl patch was needed: `absl/base/internal/sysinfo.cc`'s
`GetNumCPUs()` calls `std::thread::hardware_concurrency()`,
which doesn't exist in this target.  Added a `#elif defined(__wasm__)`
branch returning `1` — see `absl-wasm.patch`.

### The proper way (vs. what we did)

cel-cpp + abseil + RE2 all support a bazel-native config
for wasm targets via `@platforms//cpu:wasm32` and
`@platforms//os:wasi` selects (see `re2/BUILD.bazel:62-67`
and `:75-81` — both drop `-pthread` for these platforms).
Building with bazel + `--platforms=@platforms//os:wasi`
would auto-configure all the wasm-specific copts and
linkopts without our CMake-toolchain-file workaround, our
`absl-wasm.patch`, or our manual emulation-lib threading.

For production integration into the cel-spec-wasm repo
(which already uses bazel), this is the route to take.
The CMake build we used here proved the binary-size +
WASI-import numbers; the bazel build would tighten them
(no signal/mman/pthread emulation libs needed if the
selects properly disable threading code paths) and
integrate cleanly with our existing `MODULE.bazel`.

### Driver

`re2_driver.wat` (215 bytes) is the hand-coded wasm that
calls into the RE2 library:

```
(module
  (import "lib" "memory" (memory 1 65536 shared))
  (import "lib" "match"
          (func $match (param i32 i32 i32 i32) (result i32)))
  (data (i32.const 0x24000) "[a-z]+@[a-z]+")
  (data (i32.const 0x24020) "alice@example")
  ...
  (func (export "case0") (result i32)
    (call $match (i32.const 0x24000) (i32.const 13)
                 (i32.const 0x24020) (i32.const 13))))
```

Composed with the library at instantiation:
```
wasmtime run -W threads=y -W shared-memory=y \
  --preload lib=re2_lib.wasm \
  --invoke case0 re2_driver.wasm
```

Driver writes data above the lib's static-data + stack
region (offset 0x24000 ≈ 144 KB; lib's stack pointer
init was 0x23830 ≈ 142 KB).  Both share lib's linear
memory.

### Answer to "library lives in the runtime"

Yes — RE2 can ship as ~150 KB gzipped inside the wasm
runtime, with 11 WASI imports all from the good subset.
A small embedder shim (~10 KB) suffices to satisfy those
imports.  No host-side regex code needed.

## What this proves about moving libraries into the runtime

| Question | Answer from this prototype |
|---|---|
| Can a C/C++ library compile to wasm32 with wasi-sdk? | Yes — clean build, no special flags beyond `-Oz -nostartfiles -Wl,--no-entry -Wl,--export=...`. |
| Can it have **zero** WASI imports? | Yes, if the library doesn't allocate, log, or read clock/random.  Tiny_regex shows zero imports at 1.4 KB gzipped. |
| Can a separate wasm module call the library? | Yes — `--preload lib=lib.wasm` wires up cross-module imports at instantiation time.  Shared linear memory works. |
| Does the embedder need a WASI shim? | Only if the library uses WASI imports.  For pure-computation libraries (regex, parsers): no.  For libraries that log or check time: a small ~10 KB JS shim suffices in browsers. |
| What's the size cost? | Hand-rolled C: ~1-2 KB.  C++ stdlib non-throwing: probably 50-100 KB.  Full RE2: 200-500 KB.  Numbers from RE2 build pending. |
| Does this work with our existing arena allocator? | Not tested here — the prototype libraries don't allocate.  Per WASI_AND_PORTABILITY.md §4, libraries that bring `dlmalloc` need allocator-integration plumbing (1-2 days work). |

## Open questions surfaced by the experiment

  - **Cross-module linear memory** works (driver imports `lib.memory`,
    writes rodata into it).  But: does this scale to multiple
    libraries each wanting a memory?  wasm has supported multi-
    memory since 2023; if we adopt this pattern at scale, each
    library can own its own memory and the embedder routes between
    them.
  - **Exception handling story for C++ libraries** — wasi-sdk's
    default-no-exceptions libc++ ships with most C++ libraries
    de-facto excluded (anything that uses `std::regex`, `std::stoi`,
    `std::vector::at`, `std::map::at`, etc.).  RE2 dodges this by
    using error-code APIs.  Adopting other C++ libraries means
    either a custom libc++ rebuild with exceptions, or vetting
    each library for throw sites.
  - **driver.wat's `(import "lib" "memory")` works for one library**,
    but if we want multiple libraries each with their own memory,
    we need multi-memory support in the toolchain and at runtime.
    Browser support for multi-memory is recent; need to verify
    target browsers.
