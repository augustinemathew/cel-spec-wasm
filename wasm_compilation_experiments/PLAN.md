# wasm_compilation_experiments — Plan

Status: plan — drafted 2026-05-16, not yet started.

## 1 Why

CEL parse/format for timestamps and durations (and probably more in
later milestones — most notably `matches()` / regex) is currently
routed through **host trampolines** — the wasm module imports
`cel_timestamp_parse`, `cel_duration_parse`, …, and each host
language (Rust, Go, JS/browser, Python, …) must re-implement the
body, ideally with bit-identical semantics to cel-cpp's
`absl::ParseTime` / `absl::ParseDuration` / `RE2::PartialMatch`.

That cost compounds linearly per host.  The goal of this directory
is to figure out **how much of the per-host parser surface we can
push down into the runtime wasm binary itself** — and at what cost
in binary size, build complexity, and runtime portability (notably:
does it still run in a browser without WASI?).

The question is broader than timestamp/duration parse — every
future feature that wants string parsing, regex, number formatting,
locale-aware behaviour, etc. will face the same fork.  Time-parse
is the canary; regex is the harder follow-up.

## 2 Option space

Five plausible strategies for any "parser-shaped" runtime function:

  - **A. Host trampoline** *(status quo per M7-B plan)* — runtime
    imports `cel_<feature>_<op>`; each host implements the body.
  - **B. Vendor a full library (absl, RE2) into the wasm runtime** —
    `wasi-sdk` builds the library into `cel_runtime.wasm`; no host
    work needed.
  - **C. Vendor a narrow slice of upstream** — copy *just* the
    parsing files we need from absl (or cel-cpp) into the runtime,
    snipping `absl::Time` parsing free of the rest of absl.
  - **D. Hand-roll C in the runtime** — write a focused RFC3339
    + ISO-8601-duration parser in C99, no external deps, link it
    into `cel_runtime.wasm` alongside `cel_time.c`.
  - **E. Hybrid** — runtime owns the common-case fast path
    (e.g. canonical RFC3339 `2006-01-02T15:04:05Z` and the simple
    duration grammar `\d+(\.\d+)?[a-z]+`), host handles edge cases
    (timezone names, non-UTC offsets that need tzdata).

Pros / cons / open risks:

| | Build cost | Binary size | Host parity | Browser-friendly | cel-cpp parity |
|-|-|-|-|-|-|
| **A** host trampoline | low | 0 | poor (Nx) | yes | yes (host calls absl) |
| **B** vendor absl | high (wasi-sdk + C++ stdlib in wasm) | **~200–500 KB** est. | perfect | needs WASI shim (likely) | perfect |
| **C** narrow slice | medium (extract + maintain a vendor patch) | ~20–80 KB est. | perfect | maybe (depends on slice) | high |
| **D** hand-roll C | medium (one-shot, then cheap) | ~5–10 KB est. | perfect | yes (no WASI) | requires test parity |
| **E** hybrid | medium-high (two surfaces) | small (D) + (A) | partial (still Nx for edges) | yes for fast path | depends |

### Why each one is interesting

  - **A** is the cheapest *for the cel-wasm project*, the most
    expensive *for every host author*.  We want to escape this.
  - **B** is the most attractive on paper — `cel-cpp` already uses
    absl, so the parser is *the* reference impl.  The risk is
    everything bundled with it: C++ exceptions, iostreams,
    tzdata, `clock_gettime`, exception personality, sbrk-based
    malloc.  Even with `-fno-exceptions -fno-rtti` and dead-code
    elimination, the absl flatkbuffers headers transitively pull
    in a lot.  *Empirical sizing is the whole point of this dir.*
  - **C** trades B's universality for surgical work: just copy
    `absl/time/time.cc` and the few files it touches, strip the
    tz database to a fixed `UTC` + `±HH:MM` offset, drop the
    `absl::Status` apparatus to bare `bool`/`int`.  Less binary
    weight, more long-term maintenance — when absl changes, we
    diff the slice.
  - **D** is the smallest and the only option with **zero WASI
    dependency**.  RFC3339 is a regular grammar; the C is ~200
    lines.  The risk is semantic drift from cel-cpp's exact
    behaviour (leap seconds, fractional precision, tz parsing
    rules, error messages).  Spec parity has to be enforced by
    test, not by inheritance.
  - **E** lets D handle the 95% case while leaving A for
    pathological inputs.  But it keeps the per-host trampoline,
    so it doesn't fully solve the original problem; it only
    reduces the size of what hosts must implement.

### The three questions the experiments must answer

1. **Does B compile?**  Concretely: can `wasi-sdk` clang produce
   a wasm module that calls `absl::ParseTime` and links?  If yes,
   what's the stripped/gzipped size?  Which WASI imports does
   the result demand?
2. **Does B run in a browser?**  WASI imports must be either
   absent or supplied by a small JS shim (`@bjorn3/browser_wasi_shim`,
   `@wasmer/wasi`).  If absl drags in `fd_write` for `iostream`
   sentinel writes or `clock_time_get` for `absl::Now()`, those
   shims handle it; if it wants `path_open` or threading
   primitives, we have a problem.
3. **How small is D?**  Order-of-magnitude check on a hand-rolled
   RFC3339 + duration parser — and what's its semantic delta
   vs absl on a 200-case test matrix lifted from cel-cpp's
   conformance suite?

## 3 Experiment plan

All experiments live in this directory; each is self-contained
with its own `BUILD.bazel` / `Makefile` and a `RESULTS.md`
capturing what we observed.

### Exp 1 — absl::ParseTime compiles to wasm with wasi-sdk

  - Install `wasi-sdk` (via `http_archive` in a sibling
    `WORKSPACE.bazel`, or sideload tarball for the experiment;
    do **not** add a system-dependency to the main repo).
  - Write `exp1_absl_parsetime/main.cc`:
    ```cpp
    #include "absl/time/time.h"
    extern "C" int parse(const char* s, int64_t* sec, int32_t* nsec) {
      absl::Time t;
      std::string err;
      if (!absl::ParseTime(absl::RFC3339_full, s, &t, &err)) return -1;
      *sec  = absl::ToUnixSeconds(t);
      *nsec = absl::ToUnixNanos(t) % 1000000000;
      return 0;
    }
    ```
  - Build with `wasi-sdk` clang, `-Oz -fno-exceptions -fno-rtti
    -flto -Wl,--strip-all -Wl,--gc-sections`.
  - Output: `parse.wasm` size (raw + gzip), list of imported
    WASI functions (`wasm-objdump -j Import -x`).
  - Compare to identical binary with `absl::ParseDuration` added.
  - Record: did it compile, did it link, did it run under
    `wasmtime run --invoke parse "2026-05-16T12:00:00Z" 0 0`?
  - **Decision input:** if size > 200 KB stripped + gzipped, or
    if imports include anything the browser can't shim, this
    option's case weakens.

### Exp 2 — Same as Exp 1, with cel-cpp's parse slice instead of raw absl

  - Pull `cel-cpp`'s timestamp/duration parsing code path (it's
    already vendored at `third_party/cel-cpp`).  Identify the
    minimal set of `.cc` / `.h` files.
  - Repeat the size + imports test from Exp 1.
  - **Decision input:** cel-cpp's slice may already be a
    pre-pruned form of absl::Time we can vendor directly.

### Exp 3 — Hand-roll RFC3339 + duration parser in C99

  - Write `exp3_handroll/parse_rfc3339.c` (target ~200 LOC) and
    `parse_duration.c` (~100 LOC), plus a matching pair of
    formatters (~130 LOC) — see §5 for why parse+format share
    a fate.
  - Compile with `clang --target=wasm32 -nostdlib -Oz` — no WASI,
    no libc, just `memcpy` (we provide).
  - Output: `parse_handroll.wasm` size, list of imports
    (should be **empty** — pure module).
  - Run against a **conformance harness** that loads the same
    300-case input list as Exp 1 / Exp 2 and asserts byte-
    identical `(seconds, nanos, status)` outputs.
  - The 300 cases come from grepping `cel-spec/tests/simple/testdata/`
    for `timestamp(...)` / `duration(...)` literals, plus
    boundary cases: leap-second `:60`, fractional-precision
    `.999999999`, `-0001-01-01T00:00:00Z`, `9999-12-31T23:59:59.999999999Z`,
    bad inputs (`""`, `"X"`, `"2026-13-01"`, …).
  - **Decision input:** any non-zero parity delta vs Exp 1 is a
    list of TODOs against the hand-rolled parser.  If the delta
    is "leap seconds" only, D wins; if it's a long tail of
    formatting subtleties, B/C win.

### Exp 4 — Browser instantiation smoke-tests

  - For each of `parse_absl.wasm` (B), `parse_celcpp.wasm` (C),
    `parse_handroll.wasm` (D): instantiate in Node.js with
    `@bjorn3/browser_wasi_shim` and (D additionally) plain
    `WebAssembly.instantiate` with **no** WASI shim.
  - Capture: load time, first-call latency, errors.
  - **Decision input:** does B's WASI import set match what
    browser shims provide?  Does D really stand alone?

### Exp 5 — Code-size budget against cel_runtime.wasm

  - Take the current `cel_runtime.wasm` size as baseline.
  - For each option, simulate linking it in by adding the wasm
    object to a copy of cel_runtime.wasm and measuring stripped
    + gzipped output.
  - **Decision input:** what fraction of the runtime's existing
    size does each option add?  (B at 300 KB on top of a 60 KB
    runtime is a different conversation than B at 60 KB on top
    of a 300 KB runtime.)

### Exp 6 — wasi-sdk **as a clean dep** check

  - Mirror Exp 1, but try to wire `wasi-sdk` into the
    main repo's bazel toolchain via `http_archive` rather than
    a sideloaded tarball — this is the integration cost B/C
    actually pay if we adopt them.
  - **Decision input:** does adding wasi-sdk to the main repo
    require host-platform-specific config?  (Per repo memory,
    runtime build must be cross-platform — `brew install
    wasi-runtimes` is out.)  `http_archive` of an upstream
    tarball is the bar.

## 4 Regex (matches) — same shape, different library

The same option grid applies to CEL's `matches(target, regex)`.
cel-cpp uses **RE2** (`runtime/standard/regex_functions.cc:25`,
`#include "re2/re2.h"`, `RE2::PartialMatch`).  Our runtime does
not currently link RE2 — `matches()` is unimplemented.

### Why regex is the harder version of the same question

  - **No first-class C port of RE2 exists.**  RE2 is C++.
  - Realistic C alternatives:
    - **PCRE2** — pure C, mature, 200–400 KB.  Backtracking
      engine; admits catastrophic-backtracking patterns RE2
      would reject.  Syntax is a different dialect from RE2
      (Perl-ish vs Go-ish character classes, group flags,
      Unicode property semantics).  Adopting PCRE2 = a CEL
      conformance dialect divergence.
    - **regex9 / re1** (Russ Cox's plan9 lineage) — same
      algorithmic family as RE2 (Thompson NFA, linear time),
      tiny (~10 KB), but missing RE2's Unicode property
      classes and named-capture extensions.  Good fit for
      a *subset* of CEL regexes; insufficient for full
      conformance.
    - **rure** (Rust's `regex` crate) — RE2-class engine,
      ~600 KB in wasm, adds the Rust toolchain to our build
      surface (which we currently don't have).
    - **re2c** — compile-time regex→C codegen; **not a
      runtime engine.**  Useless for arbitrary user regex.
  - **RE2 itself compiles to wasm.**  Envoy's proxy-wasm
    filters and several other production stacks ship RE2 in
    wasm via `wasi-sdk`.  Order-of-magnitude size:
    500–800 KB stripped; gzipped probably 200–300 KB.
  - **Perf wrinkle.**  RE2 in wasm runs ~2–3× slower than
    native.  Hand-rolled regex9 has the same algorithmic
    perf as RE2 (Thompson NFA); PCRE2 is faster on simple
    patterns but pathologically slower on adversarial ones.
    None of these win materially against the others for
    *typical CEL usage* (URL / email / validation patterns).

### How regex maps onto the A–E options

  - **A** host trampoline: `cel_host.cel_matches(target,
    regex) → bool`.  Host implements with RE2 (C++ hosts)
    / Go's `regexp` (Go host) / `RegExp` (JS host) / Python
    `re` (Python host).  Cross-host parity is **worse than
    timestamp parse** because every host's regex library
    has subtly different dialects.
  - **B** vendor RE2 into the wasm runtime via wasi-sdk.
    Same shape as B for absl; bigger binary; better parity.
  - **C** narrow slice — no clean cut here.  RE2 is one
    library; you can drop the C++ stdlib wrapper but you
    can't usefully cut RE2 in half.
  - **D** hand-roll — only viable if we restrict CEL's
    supported regex dialect to what regex9 implements
    (no Unicode property classes, no `(?P<name>…)` named
    captures, no `\p{…}` script properties).  That's a
    spec divergence we'd need explicit user approval for.
  - **E** hybrid — runtime ships regex9 for simple patterns,
    falls back to host trampoline for advanced ones.
    Detection is at parse time: scan the regex string for
    forbidden constructs.  Real codepath, but doubles the
    maintenance surface.

### Decision sketch

If RE2-via-wasi-sdk binary size is acceptable (<500 KB
gzipped on top of `cel_runtime.wasm`), B wins for regex
even if D wins for time-parse — they're independent
decisions.  If size is unacceptable, A (host trampoline)
is more defensible for regex than for time because RE2
is the de facto standard and most hosts have a real RE2
binding available (cel-cpp host gets it free; Go host
uses `regexp/RE2` directly; Rust host uses `regex` crate;
JS host has `RegExp` with **non-RE2 dialect** — this is
the corner where parity actually hurts).

### Experiments to add for regex (Exp 7–9)

  - **Exp 7** — Compile RE2 to wasm via wasi-sdk; measure
    size + imports.  Mirror of Exp 1 but for regex.
  - **Exp 8** — Build regex9 / re1 to wasm with bare-target
    clang; measure size + imports + parity against RE2's
    behaviour on the CEL conformance regex corpus.
  - **Exp 9** — Catalog what % of CEL conformance regex
    cases hit the "advanced" features (Unicode props, named
    groups, …).  If <20% need them, hybrid E is realistic;
    if >50%, hybrid isn't worth the complexity.

## 5 Format (string conversion) — symmetric with parse

`string(timestamp)` and `string(duration)` are first-class
CEL operations.  cel-cpp implements them via:

  - `absl::FormatTime("%Y-%m-%d%ET%H:%M:%E*SZ", t, UTC)`
    — found at `common/constant.cc:94` and
    `eval/public/cel_value.cc:72`.  Strips trailing
    fractional zeros, always UTC, always `Z` suffix.
  - `absl::FormatDuration(d)` — picks smart units
    (`absl::FormatDuration(1500ms)` → `"1.5s"`, not
    `"1500ms"`).  See `common/constant.cc:91`.

Implication: **format and parse share an option choice.**
If we hand-roll parse (option D), we hand-roll format —
roughly +130 LOC C on top of the parser (50 for
timestamp formatter, 80 for duration with unit picking).
If we vendor absl (B/C), both parse and format are
covered.  If we host-trampoline (A), we need 4
trampolines not 2.

There's no scenario where it makes sense to mix —
parse in-runtime + format on host (or vice-versa) is
weird since they share string-parsing helpers and the
test matrices overlap.  Treat them as one decision.

## 6 What we are *not* trying to figure out here

  - The CEL grammar for timestamp/duration/regex.  M7-B's
    plan doc covers time; CEL's regex dialect is RE2's
    dialect by definition.
  - The full M7-B feature surface (arithmetic, accessors).
    Those are settled in-runtime in `cel_time.c` already;
    this experiment is *only* about the parse/format/regex
    edge.
  - Long-term host ABI design.  Whatever we pick gets
    reflected in the M7-B and (future) M-regex plans after
    the experiment lands.

## 7 Recommended order

  1. Exp 3 first (hand-roll).  Cheapest and tells us the
     parity-deltas baseline.
  2. Exp 1 (absl).  Tests the heaviest option's viability.
  3. Exp 2 (cel-cpp slice) only if Exp 1 looks promising.
  4. Exp 4 (browser) on whatever survived 1–3.
  5. Exp 5 (size budget) writes the final decision matrix.
  6. Exp 6 only if we end up wanting B/C and need to confirm
     toolchain integration is clean.
  7. Exp 7–9 (regex) — run after the time decision lands so
     we can reuse the wasi-sdk integration (if adopted) and
     the conformance-harness scaffolding.

If Exp 3's size is <10 KB and parity-delta is <5% of test
cases, we likely stop there — adopt **D**, defer everything
else.  If Exp 3 parity is bad and Exp 1's binary is <100 KB
with browser-shimmable WASI, **B** becomes the choice.
Hybrid **E** is the fallback if neither pure path wins.

## 8 Open questions for the user before starting

  - **Is "exactly matches cel-cpp / absl behaviour" a hard
    requirement, or is "matches the conformance suite" enough?**
    The two answers point at different options (B/C vs D).
    For *time*, "passes conformance" probably allows D.
    For *regex*, "passes conformance" basically *requires*
    RE2-dialect parity, which means B (or A).
  - **Multi-host roster:** which hosts are real and which are
    speculative?  If "Rust + browser" is real but "Python +
    Go" is hypothetical, the multi-host pain of A is smaller
    than it looks.  Note: JS host's `RegExp` is a non-RE2
    dialect, so A for regex hurts the JS host specifically.
  - **Browser as first-class target?**  Yes/no changes the
    weight on Exp 4 considerably.
  - **Is wasi-sdk in the build acceptable** if it's added via
    `http_archive` and only used for the runtime, not the
    host-side build?
  - **Are time and regex one decision or two?**  They share
    the wasi-sdk integration cost (Exp 6 is paid once for
    either).  But time-parse fits D cleanly while regex
    likely doesn't.  Picking B for regex and D for time is
    coherent if we're willing to pay the wasi-sdk
    integration cost for *one* thing.
  - **Acceptable runtime binary size ceiling?**  If the
    target is "fits in <200 KB gzipped", that's a hard
    constraint that rules out B for regex.  If "anything
    under 1 MB", B-for-everything is back on the table.

## 9 Deliverables

  - One subdir per experiment under
    `wasm_compilation_experiments/exp<N>_<slug>/`.
  - Per-exp `RESULTS.md` with: binary size table, imports list,
    parity-delta table, conclusions.
  - This `PLAN.md` updated with status flags as each exp ships.
  - A final `DECISION.md` at the root recommending A/B/C/D/E
    with the evidence inline.
