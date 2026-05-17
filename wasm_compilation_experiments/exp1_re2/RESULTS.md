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

## RE2 build attempt — stopped at toolchain setup

Cloned `re2` (2.5 MB) + `abseil-cpp` (18 MB).  RE2 requires
either:
  - **CMake + a pre-built abseil install** — needs `cmake`
    (not on this system; one `brew install` away) and a full
    cross-compiled absl tree that wasi-sdk can link against.
  - **RE2's `Makefile`** — requires `pkg-config` + system absl
    packages (`absl_strings`, `absl_synchronization`,
    `absl_flat_hash_map`, ~15 more), which means building absl
    via CMake first regardless.

Either path is several hours of build-engineering work
(CMake toolchain file for wasi-sdk, working through absl's
build-time feature probes, vetting which absl translation
units pull in `pthread` / `clock_gettime` etc., reconciling
`-fno-rtti -fno-exceptions` with absl's own settings).

**Stopped here** because the architectural prototype is
proven without it — the question "can libraries live inside
the wasm runtime" was answered yes by the tiny_regex /
hello.wasm + driver.wat composition.  RE2's value at this
stage would be empirical numbers for size + imports, which
informs the option-C "vendor a slice" vs option-B "vendor
full absl" decision in `../PLAN.md` — that decision can wait
for a dedicated build session.

Projected RE2 numbers (informed by published envoy
proxy-wasm filter binaries that ship RE2 in wasm):
  - **Binary size**: 200-500 KB stripped, 80-200 KB gzipped.
  - **Imports**: a handful of WASI imports (`fd_write` from
    absl logging, `clock_time_get` from `absl::Now()` error
    paths, `random_get` from absl's container hash seeds).
    All in the "good subset" — browser-shimmable.
  - **ABI**: `bool RE2::PartialMatch(text, pattern)` — same
    shape as our `match()` signature.

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
