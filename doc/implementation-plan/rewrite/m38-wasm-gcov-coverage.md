# m38 — wasm-side gcov coverage via the eval host

Status: shipped 2026-07-27.  Core implemented + smoke-verified, then
the full measurement run completed the same day: engine-level
`CollectWasmCoverage` API, per-workload collection tooling
(`scripts/coverage/collect_wasm_gcov.sh` + `wasm_gcov_report.py`,
§4.1), and headline numbers 87.3% line / 95.1% function for
`runtime/*` in the shipping wasm across 38 workloads (audit report
§2.3).  §6 records the as-shipped deltas.

## 1. Problem

All coverage numbers for `runtime/*.c` are measured on the **native** build
that the `runtime/*_test.cc` suites link (see
`reviews/2026-07-27-test-inventory-and-coverage.md` §2). The wasm copy —
the artifact that actually ships — is invisible to gcov: e2e and
conformance exercise it through wasmtime, but nothing records which lines
ran. The audit found whole families (`cel_time.c`'s `*_with_tz` accessors,
`cel_runtime.c`'s comprehension helpers) with zero native hits that are
*believed* covered via the wasm path; without wasm-side coverage that
belief is unverifiable.

## 2. Probe record (2026-07-27, all empirical)

Building `//runtime:cel_runtime_wasm` with
`--collect_code_coverage --instrumentation_filter='^//runtime[/:]'`:

  - Links successfully; the undefined profile-runtime symbols become
    **function imports on module `env`** (wasi-sdk 25 ships no
    `libclang_rt.profile` for wasm32, only `builtins` — checked
    `lib/clang/19/lib/wasi/`).
  - Exact import surface (parsed from the binary's import section):
    `llvm_gcda_start_file(i32 filename_ptr, i32 version, i32 checksum)`,
    `llvm_gcda_emit_function(i32 ident, i32 func_checksum, i32 cfg_checksum)`,
    `llvm_gcda_emit_arcs(i32 num_counters, i32 counters_ptr)`,
    `llvm_gcda_summary_info()`, `llvm_gcda_end_file()`,
    `llvm_gcov_init(i32 writeout_fn, i32 reset_fn)` — the last two args are
    **function-table indices**, registered per-TU by static ctors (which the
    engine already runs — the `cctz_doubles` forcing function).
  - `.gcno` notes files ARE emitted per TU, even with `-flto`, at
    `bazel-out/<wasm-config>/bin/runtime/_objs/cel_runtime_wasm.bin/<tu>.gcno`.
  - The function table is **not** exported (`tables=[]` in the export
    section), so host-invoked write-out needs `-Wl,--export-table`.
  - The runtime exports its memory as `memory` (shared); the host already
    holds a stable base pointer (`InstanceImpl::memory`).

Reference for the gcda wire protocol: compiler-rt `GCDAProfiling.c` at
`llvmorg-19.1.5` (matches wasi-sdk 25's LLVM). Header = magic `"gcda"` +
version word (verbatim from the call) + stamp; function record = tag
`0x01000000`, len 3 (gcov ≥ 4.7); arcs record = tag `0x01a10000`,
len `2*n`, u64 counters, **merged by summing against an existing file's
positionally-aligned record**; object summary tag `0xa1000000` (gcov ≥ 9).

## 3. Design

Host-side implementation of the six `env::llvm_*` imports — no compiler-rt
port, no WASI filesystem involvement:

  - **`//runtime:instrument_wasm`** (`bool_flag`, default False). When set,
    `cel_runtime_wasm.bin` adds `-fprofile-arcs -ftest-coverage` to copts
    and `-Wl,--export-table` + the profile-import allowance to linkopts.
    The normal artifact is byte-identical to today when the flag is off.
  - **`eval/internal/wasm_gcov.{h,cc}`** (`//:internal`):
    - `WasmGcovSink` — the gcda writer. Mirrors GCDAProfiling.c semantics
      (create-or-merge, positional arc summing, object summary) but
      in-memory: old file slurped on StartFile, new bytes buffered, one
      write on EndFile. Output path = `$CELWASM_WASM_GCOV_DIR/<basename>`
      (runtime TU basenames are unique).
    - `WasmGcovEnv` — per-Instance state: the sink, the registered
      write-out table indices, and the borrowed memory base/size.
    - `RegisterWasmGcovImports(linker, env)` — defines the six imports.
      Always registered (harmless when the module isn't instrumented);
      callbacks no-op when no collection destination is configured.
    - `DumpWasmGcov(context, helpers_instance, env)` — resolves the
      exported `__indirect_function_table`, `table.get`s each registered
      write-out index, and calls it; the guest then drives the
      `llvm_gcda_*` imports against the sink.
  - **Wiring**: `InitLinker` registers the imports; `InstanceImpl` gains a
    `std::unique_ptr<WasmGcovEnv>`; the `InstanceImpl` dtor calls
    `DumpWasmGcov` before tearing down the linker/store (counters live in
    guest memory, so the dump must precede store deletion).
  - **Engine-level API** (added at measurement time):
    `Engine::Builder::CollectWasmCoverage(output_dir)` fixes the
    collection destination at `Build()` — explicit dir wins, else the
    `CELWASM_WASM_GCOV_DIR` env var, else disabled.  Every Instance the
    engine Plans dumps into that dir on destruction.  Behavior pinned
    by `engine_test.cc` `EngineWasmCoverageTest` (config-aware: inert
    against the normal artifact, collecting against an instrumented
    one).

Scope: **dynamic link mode** is the measured configuration (the runtime is
a separate module there). Static mode merges the identical C code; the
imports are registered either way, so an instrumented static program would
also collect, but the measurement workflow below only exercises dynamic.

## 4. Measurement workflow (verified 2026-07-27)

One subtlety, learned empirically: a plain `--//runtime:instrument_wasm`
build emits `.gcno` only as an **undeclared side effect**, which the
sandbox discards. Combining the flag with bazel's coverage flags makes
the `.gcno` declared outputs that land in `bazel-out`; the duplicate
instrumentation flags are harmless.

```bash
# Build (the flag adds --export-table + import allowance; the coverage
# flags make bazel keep the .gcno):
bazel build --//runtime:instrument_wasm --collect_code_coverage \
    '--instrumentation_filter=^//runtime[/:]' \
    //e2e:<dynamic targets> //conformance:run_conformance

# Run each workload binary DIRECTLY (not `bazel test` — the sandbox
# would eat the output dir), sequentially (the sink has no file lock):
export CELWASM_WASM_GCOV_DIR=/tmp/wasm-gcov
./bazel-bin/e2e/<each dynamic e2e binary>
./bazel-bin/conformance/run_conformance   # dynamic link mode

# Pair the collected .gcda with the notes files.  The wasm objs dir is
# the ST-suffixed config, e.g.:
#   bazel-out/k8-fastbuild-ST-*/bin/runtime/_objs/cel_runtime_wasm.bin/
cp "$(dirname "$(find "$(bazel info output_path)" -path '*cel_runtime_wasm.bin*' -name 'cel_time.gcno' | head -1)")"/*.gcno /tmp/wasm-gcov/

cd /tmp/wasm-gcov && llvm-cov gcov -n *.gcda   # percentages; drop -n for .gcov annotations
```

The gcda version emitted by clang 19 here is `"408*"` (gcov 4.8 wire
format) — readable by llvm-cov 18+. Smoke-verified end to end: 2 cases
of `mvp_concat_test_dynamic` (since folded into `operators_test.cc`)
produced 15 .gcda files, decoded cleanly, with plausible per-file
percentages (`cel_string_ops.c` 17%, `cel_arena.c` 63%,
`cel_memory.c` 40%).

### 4.1 Productionized workflow (the full measurement run, 2026-07-27)

The manual recipe above is superseded by two committed tools:

  - **`scripts/coverage/collect_wasm_gcov.sh <out_root> <bin>...`** —
    runs each workload binary sequentially with its own
    `CELWASM_WASM_GCOV_DIR` (per-workload attribution), pairs the
    .gcda with the build's .gcno, and decodes twice (`llvm-cov gcov
    -n -b -f` human summary; `-i` machine-parsable intermediate
    records).
  - **`scripts/coverage/wasm_gcov_report.py --cov-root <out_root>`** —
    joins the per-workload records with `e2e/test_taxonomy.json` into
    `report.json` (function×workload hit matrix + gap/redundancy
    pivots) and `report.html` (clickable explorer: overview, function
    audit, workload contribution, annotated source).

Two .gcno-pairing traps, both learned the hard way and now encoded in
the collect script:

  1. **The runtime wasm links TUs from several targets.**  The
    extension TUs (`cel_base64_ext`, `cel_math_ext`, the
    `cel_string_ext_*` splits, `cel_time_parse`, `cel_optional`,
    `cel_matches`) compile in their own `_objs/<lib>/` dirs — copying
    only `_objs/cel_runtime_wasm.bin/` silently drops them from the
    report.  Gather .gcno from every `bin/runtime/_objs/` dir.
  2. **Basename collisions must resolve toward the linked objects,
    with `cp -f`.**  The base TUs compile twice (the `cel_runtime`
    cc_library AND the `.bin`); the .gcda stamps match the `.bin`
    objects, so its notes must be copied last — and because bazel
    outputs are mode `r-x`, a plain `cp` over an earlier copy fails
    `Permission denied` and silently leaves the WRONG notes in place
    (surfacing later as `file checksums do not match`).

Conformance is collected via
`CELWASM_WASM_GCOV_DIR=<dir> bazel run --//runtime:instrument_wasm
--collect_code_coverage '--instrumentation_filter=^//runtime[/:]'
//conformance:run_conformance -- --link_mode=dynamic`; the eval manual
suites (`engine_test_dynamic` needs its runfiles, so `bazel run` it the
same way) join the sweep as workloads.

Results of the full run (38 workloads: 34 dynamic e2e + engine /
instance / memory_grow_stability + conformance) are recorded in
`reviews/2026-07-27-test-inventory-and-coverage.md` §2.3: **87.3% line
/ 95.1% function** coverage of `runtime/*` in the shipping wasm, 27
zero-hit functions all classified (native-test-only, never-emitted
dispatch arms, defense-in-depth).

## 5. Testing

`eval/internal/wasm_gcov_test.cc` (target `//eval:wasm_gcov_test`,
manual-tagged like its eval/internal siblings) covers both halves:

  - Sink: header bytes, function/arcs/summary record layout (both the
    ≥4.7 and pre-4.7 function-record shapes), create-then-merge summing,
    run-count merge, shape-mismatch fallback, missing-dir creation,
    basename flattening, no-op paths.
  - Glue, hermetically: a synthetic WAT module (via `wasmtime_wat2wasm`)
    that imports the six `env::llvm_*` functions and exports a funcref
    table — asserting registration is inert for non-instrumented
    modules, `llvm_gcov_init` capture, table-driven dump producing a
    byte-checked .gcda, the disabled-sink no-op, and the
    missing-table error.

All 13 cases pass.

## 6. Handoff status (2026-07-27)

Done and verified in this branch:

  - `eval/internal/wasm_gcov.{h,cc}` + `wasm_gcov_test.cc` (13/13 pass).
  - `//runtime:instrument_wasm` flag; flag-built wasm verified to carry
    the six imports AND the exported `__indirect_function_table`.
  - Engine wiring: `InitLinker` registers the imports unconditionally;
    `CacheRuntimeMemory` hands the collector the stable memory view;
    `InstanceImpl` owns the env and its dtor dumps before store
    teardown.
  - End-to-end smoke: instrumented `mvp_concat_test_dynamic` run with
    `CELWASM_WASM_GCOV_DIR` set → 15 .gcda files, decoded by
    `llvm-cov gcov` against the build's .gcno (see §4 numbers).

Remaining-work status (updated 2026-07-27, measurement session):

  1. **Regression pass** — DONE for the touched surfaces
     (`wasm_gcov_test`, `engine_test_dynamic`, the six e2e suites that
     gained gap cases, `e2e_limits_dynamic`, `cel_cpp_oracle_test`, all
     green un-flagged); the full `$PROJ` sweep + conformance gate run
     at this session's commit gate.
  2. **`scripts/lint.sh --branch`** — at the commit gate.
  3. **The full measurement run** — DONE; see §4.1 and the audit
     report §2.3 (87.3% line / 95.1% function across 38 workloads,
     per-workload attribution, all 27 zero-hit functions classified).
     The zero-native-hit families the audit flagged are CONFIRMED
     wasm-covered.
  4. Closeout: testing-checklist row ticked; static-link measurement
     deliberately NOT run (imports are registered either way; the
     static merge embeds the same C code, so dynamic-mode numbers are
     representative — revisit only if a static-only codepath appears).

Known limitations (by design, documented here rather than hedged):
collection is single-threaded, unlocked, dynamic-mode-scoped; the
`.gcno` pairing requires the combined-flags build in §4; the sink bumps
the run count once per file per dump (deviation from compiler-rt's
once-per-process quirk, noted in the .cc).

## 7. Session context for pickup

Everything below is state from the session that produced this branch
(2026-07-27, base `ba28793`), recorded so a fresh agent/machine can
continue without re-deriving it.

### 7.1 Where this came from

This milestone is the direct follow-up to the audit at
`doc/implementation-plan/rewrite/reviews/2026-07-27-test-inventory-and-coverage.md`
(both commits are ancestors of this branch). The audit measured
coverage for the runtime on the **native** build only and flagged two
function families with zero native hits that are believed covered via
the wasm path (`cel_time.c` `*_with_tz` + conversions;
`cel_runtime.c` comprehension/aggregate helpers, minus the
`cel_host_cel_*` import shims which are wasm-only by construction).
The whole point of the m38 measurement run is to confirm or refute
that belief with real wasm-side line data and write the comparison
into that report's §2.

### 7.2 How the audit's NATIVE numbers were produced (reproduce before comparing)

Three non-obvious mechanics, all learned the hard way:

  1. **The toolchain's default gcov mismatches clang.** With the
     autodetected clang toolchain, bazel's collector runs GNU `gcov`,
     which cannot read clang's profile output — tests "pass" with
     empty coverage.dat. Fix: a wrapper script
     (`exec llvm-cov gcov "$@"`) and
     `coverage --repo_env=GCOV=<wrapper>` in `user.bazelrc`. Compile
     flags are unchanged, so this costs no rebuild.
  2. **A single `bazel coverage` pass cannot work.** Instrumenting
     `//runtime` leaks the profile flags into the wasm32 cross-compile
     (this is also what m38 exploits deliberately) — before this
     branch, every wasm-instantiating test then died at Plan on the
     unresolved `env::llvm_gcda_start_file` import. So native coverage
     ran as two passes, merged:
       - pass 1: `--instrumentation_filter='^//(compiler|eval|shared|abi|runtime|conformance|tools)[/:]'`
         over the 99 native-only suites;
       - pass 2: the same minus `runtime` over the 81
         wasm-instantiating suites;
       - `scripts/coverage/lcov_merge.py pass1.dat pass2.dat merged.dat`
         then `scripts/coverage/lcov_report.py merged.dat --misses`.
     NOTE: now that this branch's host hooks exist, the imports
     resolve, so a single-pass run may work again — but `--export-table`
     is keyed to `--//runtime:instrument_wasm`, not to the coverage
     config, so the dump step would error `no __indirect_function_table`
     (collection is env-var-gated anyway; it degrades to a warning).
     The two-pass recipe stays the documented native path.
  3. **The fuzz miner is excluded**: `//e2e/fuzz:cel_oracle_property_test`
     is a divergence discovery tool, not a regression suite.

Native headline at `ba28793`, all 180 targets green: compiler 90.2% /
runtime (native) 88.8% / eval 82.5% / abi 95.1% / shared 95.7% /
tools 75.9% lines. Full tables in the audit report.

### 7.3 Tooling committed with this branch

  - `scripts/coverage/lcov_merge.py` — merge lcov .dat reports.
  - `scripts/coverage/lcov_report.py` — per-file/per-dir tables +
    uncovered ranges.
  - `scripts/coverage/wasm_sections.py` — wasm import/export dumper
    (the §2 probe tool).

The wasm-side gcda→lcov step still needs a converter (llvm-cov gcov's
per-file output → lcov `DA:` records, or just report percentages
directly from `llvm-cov gcov -n`); nothing exotic — the report script
only needs `SF:`/`DA:` records.

### 7.4 Decisions already made (don't relitigate silently)

  - **Host-implemented gcda imports over porting compiler-rt to wasi**:
    wasi-sdk 25 ships no profile runtime, GCDAProfiling.c wants mmap,
    and host-side collection needs no preopened WASI dirs. Recorded in
    §2/§3; revisit only if wasi-sdk starts shipping
    `libclang_rt.profile-wasm32`.
  - **Imports registered unconditionally** on every Plan linker:
    harmless for normal modules, and it means an instrumented Program
    (static mode) also instantiates. Collection stays env-var-gated.
  - **Run-count deviation** from compiler-rt (per-file-per-dump instead
    of once-per-process) — deliberate, commented in the .cc, asserted
    by the tests.
  - **Basename flattening** of gcda paths is safe because runtime TU
    basenames are unique; revisit if a second instrumented module ever
    collects into the same directory.
  - The milestone number is **m38** (m37 is reserved/taken elsewhere).

### 7.5 Environment note

This branch was produced in a sandboxed cloud session whose egress
policy blocks bcr.bazel.build, GitHub `/archive/` tarballs,
mirror.bazel.build, and antlr.org; Bazel was fed via a registry mirror
+ repository-cache injection, all confined to the session's gitignored
`user.bazelrc`. None of that is needed on a normal network — a stock
checkout builds as usual. The only `user.bazelrc` line worth copying
locally is the coverage GCOV wrapper from §7.2 when the host compiler
is clang.
