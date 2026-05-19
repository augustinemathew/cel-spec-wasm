# Phase C research + prototyping prompt

**Branch:** `phase-c-libraries` (forked from `wasi-malloc-migration` @ `a43ee8b`,
which is the merged WASI / `malloc` migration; PR #5 against master).

**Status:** plan — drafted 2026-05-18, ready to hand off to a research agent.

## 0 What this doc is

A **research + prototyping brief**, not an implementation plan.  Phase C
needs to vendor abseil-cpp and RE2 into `cel_runtime.wasm` so the runtime
can host `matches()` + parse/format kernels for timestamp/duration/format
without host trampolines.  The wiring is non-trivial because:

  - bazel doesn't have a wasm32-wasi `cc_toolchain` registered today;
    `cel_runtime.wasm` is built via a `genrule` invoking `wasi-sdk` clang
    directly.
  - absl source compiles cleanly for wasm (preprocessor guards exist), but
    its bazel `cc_library` rules build against whatever toolchain bazel
    has registered.
  - The eventual cross-compile path is not yet validated end-to-end inside
    THIS repo's bazel setup.

So before we commit a design, we want **a research agent to run a set
of probing experiments**, write up results, and recommend the path forward.
This doc is the brief for that agent.

The output of the research phase is a separate, decision-ready
`phase-c-plan.md` that the implementing agent (a different session) can
then execute against.

## 1 Goal of Phase C

After Phase C, `cel_runtime.wasm` self-hosts:

  1. `matches(string, string) -> bool` via RE2
  2. `timestamp(string) -> timestamp` via `absl::ParseTime`
  3. `duration(string) -> duration` via `absl::ParseDuration`
  4. `timestamp.string() -> string` via `absl::FormatTime`
  5. `duration.string() -> string` via `absl::FormatDuration`
  6. `strings.format(string, list) -> string` via custom format impl
     (likely `absl::StrFormat` or a hand-rolled printf-equivalent)

The four host trampolines for parse/format
(`CelTimestampParseImpl`, `CelDurationParseImpl`, `CelTimestampFormatImpl`,
`CelDurationFormatImpl` in `compiler_v2/api/internal/cel_host.cc`)
are **deleted**.  Every host (wasmtime, Chrome, embedders) inherits
these for free from the runtime.

Conformance unlocks expected:

  - `string.textproto::matches/*`: 9 SKIPs → PASS
  - `timestamps.textproto`: today 75/76 PASS; remaining 1 FAIL is parse-related
  - `string_ext.textproto::format/*`: subset of the 122 SKIPs / 94 FAILs flips
    (only `format()` rows — not `split`/`join`/`replace` which need other
    strings-ext functions)

## 2 The unknowns we need probed

**The chosen path is Path A** — a proper bazel `cc_toolchain` for
wasm32-wasi.  This is the long-term fix; we are not shopping among
A/B/C for which is cheapest this quarter.  The research exists to
discover **how** to wire Path A, not **whether** to take it.  Paths B
and C remain documented as fallbacks of last resort, but choosing
either requires the agent to file a `BLOCKED.md` with a concrete
demonstration that Path A is *infeasible* (not just painful) and
get the user's explicit sign-off before pivoting.

"Painful" looks like: 50 features in `cc_toolchain_config.bzl`, a
day or two of flag tuning, a handful of upstream-style patches to
absl's wasi guards.  That is the work of Phase C, not a reason to
bail.  "Infeasible" looks like: a wasi-sdk capability bazel
fundamentally cannot model, a hermetic-build invariant we cannot
satisfy without forking bazel, etc.  Distinguish carefully.

The three paths below are kept for context.  Read them, then treat
the probes as "validate Path A and surface the config delta needed"
rather than "bake-off among three options."

### Path A — Proper bazel `cc_toolchain` for wasm32-wasi

Register a `cc_toolchain` in `//third_party/wasi_sdk/` that uses the
bazel-managed wasi-sdk we already have.  Define platforms
`wasm32_wasi` and `wasm32_wasi_threads`.  Then absl, re2, and any future
C/C++ deps cross-compile via standard `bazel build … --platforms=…`.

  - **Cleanest engineering** — bazel-native, hermetic, reusable.
  - **Highest setup cost** — `cc_toolchain_config.bzl` has ~50 features /
    flags / tool paths to wire correctly.  Untested for wasi-sdk in this
    repo.
  - The result: `bazel build @com_google_absl//absl/time:time
    --platforms=//third_party/wasi_sdk:wasm32_wasi` Just Works.

### Path B — `rules_foreign_cc.cmake` driving absl's CMake

Use the existing `rules_foreign_cc` (already a `bazel_dep` in MODULE.bazel)
to invoke abseil-cpp's CMakeLists.txt with a wasi-sdk toolchain file.
Output static libs consumed via `cc_import`.

  - **Reuses the proven CMake build** from `exp1_re2/`.
  - **Medium setup cost** — `rules_foreign_cc.cmake` is documented but the
    cross-compile toolchain wiring needs careful flag passing.
  - **Less hermetic** — CMake invocations inside bazel sandbox.
  - For RE2 (also CMake-based), same pattern applies.

### Path C — Genrule + pre-built static libs

Commit pre-built `libabsl_*.a` + `libre2.a` to the repo (or fetch via
`http_file`).  Wrap with `cc_import`.  Provide a script
(`scripts/build_third_party_wasm.sh`) for rebuilding when versions update.

  - **Lowest setup cost** — works today.
  - **Worst hermeticity** — checked-in binaries; the build script is
    out-of-band.
  - **Ugly but expedient** — pragmatic for getting unblocked.

The research phase decides which path we take.

### Out-of-scope decisions (for later)

  - Threading variant choice (`wasm32-wasi` vs `wasm32-wasi-threads`).
    The `exp1_re2` build used the threads variant for `std::mutex` support.
    Probe E4 below decides whether absl's `time` + `strings` slice needs it.
  - libc++ vs libc++abi link order — wasi-sdk ships both; CMake build
    handled it.
  - Browser story — Phase D problem.  But: validate during the research
    that whatever absl libs we vendor stay **WASI-shimmable** (= imports
    only `wasi_snapshot_preview1.*`, no Emscripten-specific imports).
    See `experiments/exp_e_absl_parsetime.wasm` — its 9 WASI imports are
    all good-subset.  Probe E2 below re-verifies.

## 3 Probing experiments

Each experiment is **small, has a clear pass/fail criterion, and produces
an artifact** the agent can point at.  Run them in order; stop early if a
prerequisite fails.

The experiments live under `doc/implementation-plan/rewrite/phase-c-probes/<EN>/`.
Each subdir contains: a `README.md` with method + expected output, the
source file(s), any build script, the resulting `.wasm` (if applicable),
and a `RESULT.md` with conclusions.

### E1 — Baseline: rebuild `exp_e_absl_parsetime.wasm` with current toolchain

**Goal:** prove the existing wasi-sdk + CMake-built absl path still works
from scratch.  Pin the baseline before touching anything.

**Method:** run the build command from
`doc/implementation-plan/rewrite/wasi/experiments/exp_e_absl_parsetime.cc:11-24`
(the comment header has the full clang++ invocation).  It uses the cached
`exp1_re2/absl-install/`.  Invoke the produced `parse` export with
`"2026-05-18T10:00:00Z"`.

**Pass:** wasm builds, instantiates, returns unix timestamp `1779098400`.

**Output artifact:** `probes/E1/baseline_parsetime.wasm` + `RESULT.md`.

### E2 — Probe: standalone `parseTime` wasm via bazel cc_import + pre-built absl

**Goal:** prove bazel can link the existing pre-built absl `.a` files
into a wasm artifact via `cc_import`.  This is the minimum-bar Path C
validation.

**Method:**
  1. Create `probes/E2/BUILD.bazel` with `cc_import` filegroups pointing
     at `exp1_re2/absl-install/lib/*.a` and headers.
  2. Create `probes/E2/parsetime_main.cc` mirroring `exp_e_absl_parsetime.cc`
     but built via bazel genrule (since bazel has no wasm cc_toolchain yet).
  3. Genrule invokes wasi-sdk clang++ with `$(locations …)` resolving the
     `.a` file paths.
  4. Compare the output wasm bytes / size / imports / behaviour against
     the E1 baseline.

**Pass:** bazel-built wasm matches E1's behaviour; ≤10% size delta vs E1
(some delta is OK because bazel adds toolchain-version-specific things).

**Output:** `probes/E2/parsetime_via_bazel.wasm`, `BUILD.bazel`, `RESULT.md`
with diffs vs E1.

### E3 — Probe: wasm cc_toolchain registration (the Path A gateway)

**Goal:** can we register a `cc_toolchain` that targets wasm32-wasi using
the existing `//third_party/wasi_sdk:clang` filegroups?  This is the
expensive part; if it works, Path A is unlocked.

**Method:**
  1. Reference: `@bazel_tools//tools/cpp:unix_cc_toolchain_config.bzl`.
  2. Create `probes/E3/cc_toolchain_config.bzl` with a minimal subset:
     `tool_paths` for clang/clang++/ar/ranlib, `cxx_builtin_include_dirs`
     for wasi-sysroot, `compile_flags` for `--target=wasm32-wasi`,
     `link_flags` for the `-nostartfiles -Wl,--no-entry -Wl,--export=…`
     pattern.
  3. Create `probes/E3/BUILD.bazel` with `cc_toolchain` rule wrapping
     that config, plus `platform()` for `wasm32_wasi`.
  4. Try building `probes/E3/hello_wasm.cc` (a trivial `int add(int,int)`)
     with `bazel build //doc/implementation-plan/rewrite/phase-c-probes/E3:hello_wasm --platforms=//doc/implementation-plan/rewrite/phase-c-probes/E3:wasm32_wasi`.

**Pass:** bazel produces a wasm artifact that wasmtime can instantiate
and the `add` export works.

**Fail / Partial:** record exactly what feature flag / tool path / sysroot
config broke.  This is the most likely place to hit unknown-unknowns.

**Time-box:** 2 hours.  If still stuck, mark partial and continue with E4.

**Output:** `probes/E3/cc_toolchain_config.bzl`, `BUILD.bazel`,
`hello_wasm.wasm`, `RESULT.md`.

### E4 — Probe: build a small absl module via bazel cc_toolchain (if E3 worked)

**Goal:** does absl actually cross-compile via Path A?  Pick one tiny absl
target as the canary.

**Method:**
  1. Pre-req: E3 succeeded.
  2. `bazel build @com_google_absl//absl/strings:string_view
     --platforms=//doc/implementation-plan/rewrite/phase-c-probes/E3:wasm32_wasi`.
  3. If it fails, note the first error.  Common suspects: thread-local
     storage, atomics, the `absl::base::config.h` `__wasi__` branch.
  4. If `string_view` works, escalate to `@com_google_absl//absl/time:time`.

**Pass:** `absl/time:time` builds for wasm via bazel.

**Output:** build log, list of any patches needed (filenames + diffs),
`RESULT.md`.

### E5 — Probe: link absl + a C runtime into one `.wasm`

**Goal:** the runtime is C; absl is C++.  Can we link them?

**Method:**
  1. Mirror the cel_runtime structure: a small C file (`runtime.c`)
     defining `arena_alloc` + `arena_reset` (stubs), plus a C++ file
     (`time_parse.cc`) that calls `absl::ParseTime` and writes results
     into the arena.
  2. Build both, link with `clang++` (NOT `clang`) so libc++ pulls in.
  3. Export `parse_timestamp(buf, len, out_seconds, out_nanos)` similar
     to E1.
  4. Verify wasmtime instantiates + invokes.

**Pass:** the combined `.wasm` builds and runs.  Note: imports list
should include the C runtime's WASI imports + no C++ runtime imports
beyond wasi_snapshot_preview1.

**Output:** `probes/E5/combined.wasm`, `RESULT.md` with imports list.

### E6 — Probe: re-export wasi-sdk-built absl through `rules_foreign_cc.cmake`

**Goal:** Path B validation.  If E3/E4 fail or are too painful, can we
use `rules_foreign_cc.cmake` instead?

**Method:**
  1. Add abseil-cpp as `http_archive` (separate from the existing
     `bazel_dep`-pulled one, since the bazel_dep uses default
     toolchain).
  2. Write a `rules_foreign_cc.cmake` rule wrapping it, passing the
     `wasi-toolchain.cmake` toolchain file from `exp1_re2`.
  3. Use the produced `.a` files in a small wasm link test.

**Pass:** `rules_foreign_cc.cmake` produces a usable absl install dir.

**Output:** `probes/E6/BUILD.bazel`, `RESULT.md` weighing pros/cons vs
E3+E4 (Path A).

### E7 — Probe: minimum absl library set for our four kernels

**Goal:** we don't want to link all 95 absl libs.  Which subset do
`absl::ParseTime`, `absl::ParseDuration`, `absl::FormatTime`,
`absl::FormatDuration`, `absl::StrFormat` actually need?

**Method:**
  1. Start with `absl_time` + `absl_strings` + `absl_base`.
  2. Try the link; resolve unresolved symbols by adding the matching
     `absl_*` library.
  3. List the final set.
  4. Sum their stripped wasm size — this is the binary-size cost of
     Phase C.

**Pass:** complete static link with a minimal absl subset.

**Output:** ordered list of required `libabsl_*.a` files + the produced
wasm size in `RESULT.md`.

### E8 — Probe: RE2 cross-compile (after absl works)

**Goal:** RE2 depends on absl.  Once Phase C absl is working, can RE2
also be built?  Test pattern: `RE2::PartialMatch("abc", "b.")`.

**Method:**
  1. Re-use whichever path (A/B/C) won the absl bake-off.
  2. Build RE2 against vendored absl.
  3. Standalone wasm app calling `RE2` once.

**Pass:** wasm `match` export returns the expected match result; output
size is reasonable (~400 KB stripped per `exp1_re2` baseline).

**Output:** `probes/E8/re2_match.wasm`, `RESULT.md`.

### E9 — Probe: binary size impact

**Goal:** how big does `cel_runtime.wasm` get after linking absl + re2?
Current baseline: 241 KB stripped.

**Method:**
  1. Hypothetical link: combine `compiler_v2/runtime/*.c` (existing) +
     the chosen absl subset + RE2.
  2. Measure stripped + gzipped sizes.

**Pass:** record the numbers, no specific threshold.  Inform the merge
discussion (DESIGN.md §9 budget review).

**Output:** size table in `RESULT.md`.

### E10 — Probe: kernel-level integration sketch (write but don't ship)

**Goal:** what does the actual code change in `cel_runtime` look like?
Sketch (not commit) the `cel_timestamp_parse_at_v` C++ wrapper and the
codegen routing change.

**Method:**
  1. Pseudocode `cel_time_parse.cc` (C++) with `extern "C"` C entry points
     that call absl::ParseTime + write to the arena.
  2. Show the `compiler_v2/codegen/overload_table.cc` diff that routes
     `string_to_timestamp` from `cel_host.cel_timestamp_parse` to
     `cel_runtime.cel_timestamp_parse_at_v`.
  3. Show the `compiler_v2/api/internal/cel_host.cc` deletion (the
     CelTimestampParseImpl trampoline and its registration).

**Pass:** the sketches compile mentally; no obvious blockers.

**Output:** `probes/E10/integration_sketch.md`.

## 4 Decision matrix

**Default: Path A.**  The research validates Path A and produces a
concrete config + patch list to make it work.  The plan documents
the work needed, not a path choice.

Fallback only applies if Path A is *infeasible* (per §2's
distinction).  In that case the agent writes a `BLOCKED.md`
naming the specific blocker (a bazel limitation, a wasi-sdk
capability gap, etc.), and stops — the user decides whether to
authorise Path B/C.  Do not silently pivot.

  - E3+E4 succeed (likely with config iteration) → **Path A wins**;
    write `phase-c-plan.md`.
  - E3 or E4 fail after honest effort → write `BLOCKED.md`;
    document the specific bazel/wasi-sdk obstacle; stop.

E2 and E6 (the B/C-flavoured probes) remain in the sequence but
their purpose is now *risk-mitigation context* — i.e. "if Path A is
genuinely blocked, here is what the fallback would cost" — not
options to recommend.  Run them, but don't use their success as a
reason to abandon Path A.

Document the chosen path + the migration cost in `phase-c-plan.md`
(the output of the research phase).  The plan should include:

  - Chosen path with rationale (1 paragraph)
  - List of new bazel targets (toolchain, platform, third-party rules)
  - Files to create / modify
  - Per-kernel implementation plan (signature, behaviour, error cases)
  - Test plan (unit + e2e + conformance)
  - Estimated effort (per kernel + total)
  - Risks + open questions

## 5 What the research agent should NOT do

  - Don't modify `compiler_v2/runtime/` or `compiler_v2/api/` source.
    All work goes under `doc/implementation-plan/rewrite/phase-c-probes/`.
  - Don't commit pre-built `.a` files to the repo until the plan
    chooses Path C explicitly.  For probes, use the existing
    `wasm_compilation_experiments/exp1_re2/absl-install/`.
  - Don't change `MODULE.bazel` beyond adding the abseil-cpp http_archive
    for probes E2/E6.  No changes to existing wasi-sdk or abseil-cpp
    bazel_dep entries.
  - Don't run more than 4 hours per probe.  If stuck, mark partial and
    document the blocker.
  - Don't go past E10.  The research is bounded; the implementation is
    a separate session.

## 6 Reporting format

Each probe directory's `RESULT.md` has:

```markdown
# Probe E<N>: <name>

**Status:** PASS | PARTIAL | FAIL

## Method
<what was actually done>

## Output
<what the artifact does, sizes, etc.>

## Findings
<surprises, gotchas, deltas from expected>

## Next-step implication
<what this means for the Phase C plan>
```

After all probes, the agent writes `phase-c-plan.md` at the same
directory level as this phase-c-research.md.  Cap at 600 lines.

## 7 Agent prompt (copy-paste ready)

> You are the research agent for Phase C of the cel-spec-wasm project.
> The branch is `phase-c-libraries` at `/Users/augustine/cel-spec-wasm`.
> Read `doc/implementation-plan/rewrite/phase-c-research.md` start to finish
> before doing anything.  It tells you what to investigate, why, and
> what to NOT touch.
>
> Run probes E1 through E10 in order.  Each probe lives under
> `doc/implementation-plan/rewrite/phase-c-probes/E<N>/`.  Create the directory,
> do the work, write `RESULT.md`.  Time-box each probe at 4 hours;
> mark PARTIAL and continue if blocked.
>
> Stop conditions:
>   - All 10 probes complete (success path), OR
>   - 3 consecutive probes return FAIL (likely the whole approach is
>     wrong; bail and write a `BLOCKED.md` describing what's needed).
>
> Required reading before probing:
>   - `doc/implementation-plan/rewrite/phase-c-research.md` — this doc
>   - `doc/implementation-plan/rewrite/phase-c-design.md` — the per-slice plan
>   - `doc/implementation-plan/rewrite/wasi/DESIGN.md` — the migration this
>     builds on
>   - `wasm_compilation_experiments/exp1_re2/RESULTS.md` — what the
>     CMake-built absl proved
>   - `wasm_compilation_experiments/exp1_re2/wasi-toolchain.cmake` —
>     the toolchain file that worked
>   - `compiler_v2/runtime/BUILD.bazel` — how cel_runtime.wasm builds
>     today (genrule pattern)
>   - `third_party/wasi_sdk/BUILD.bazel` + `BUILD.external.bazel` —
>     how the wasi-sdk is exposed to bazel today
>   - `CLAUDE.md` — repo rules + the periodic-review process this
>     research feeds into
>
> After all probes, write `doc/implementation-plan/rewrite/phase-c-plan.md`
> with the chosen path + concrete next-step plan per RESEARCH §4.
> Then write a short summary to stdout (under 500 words) covering
> verdict, surprises, and recommended path.
>
> Do NOT change any source under `compiler_v2/`.  Do NOT commit pre-built
> binary artifacts.  Probes are sketches; the implementation happens
> in a separate session.
>
> Permission scope: read everything; write only under
> `doc/implementation-plan/rewrite/phase-c-`.  bazel commands and shell scripts
> are allowed.  No `git push` — the user commits the doc work after
> reviewing.

## 8 Where the plan lands

After research:

  - `doc/implementation-plan/rewrite/phase-c-research.md` — this brief (committed
    now, before research starts)
  - `doc/implementation-plan/rewrite/phase-c-probes/E*/` — probe artifacts
    (committed by the research agent OR by the user after review)
  - `doc/implementation-plan/rewrite/phase-c-plan.md` — the actionable
    plan (committed after research)
  - `doc/implementation-plan/rewrite/phase-c-design.md` — the per-slice slicing
    (already committed; lightweight)

The implementation agent (separate session, separate context budget)
reads `phase-c-plan.md` + this phase-c-research.md + the probe RESULTs, then
executes against the chosen path.

## 9 Open questions for the research to answer

These are the load-bearing unknowns.  The agent should explicitly call
out the answer to each in `phase-c-plan.md`.

  1. **Path A or B or C?** — per §4 decision matrix.
  2. **Threading variant** — `wasm32-wasi` or `wasm32-wasi-threads`?
     E5 / E7 will surface whether absl::time needs threads support.
  3. **Minimum absl libs** — which subset (E7).
  4. **libc++ linkage** — does adding a C++ TU to the runtime work, or
     do we need libc++ statically linked (E5)?
  5. **Patches needed** — what's the diff against vanilla abseil-cpp
     20260107.0 for wasi-sdk compatibility (E4)?
  6. **Binary size budget** — final runtime size (E7 + E9).
  7. **Multi-platform impact** — does the chosen path break the future
     M1.1 cross-platform CI work?  (Note: cross-platform deferred to
     post-Phase-C anyway.)

## 10 What success looks like

The research session ends with:

  - 10 probe directories under `probes/`, each with a clean RESULT.md
  - A committed `phase-c-plan.md` answering all §9 questions
  - One specific recommended path
  - Concrete file list + bazel target list for the implementation phase
  - Honest cost estimate for the implementation
  - Identified blockers (if any)

The implementing agent should be able to start coding within 30 minutes
of reading the plan.
