# Testing strategy

Status: current — authored 2026-06-10 from the design-rebuild notes plus the
post-merge tree. Supersedes:
doc/implementation-plan/per-component-test-coverage.md (catalog/closeout
sections), doc/implementation-plan/rewrite/feature-pipeline-checklist.md
(regenerated as §4.6), and the header text of
doc/implementation-plan/testing-checklist.md (the grid lives on as the ledger
appendix).

This doc describes the testing system as a system: the layers, the gates, the
disciplines, and what a green run does NOT prove. Subsystem internals:
`00-architecture.md`, `02-evaluator.md`, `04-runtime.md`. `07-benchmarking.md`
owns benchmarks as *measurement*; this doc covers only their *probe* role (§2,
layer 11).

## 1. Philosophy

Compilers fail silently: a miscompiled expression produces a plausible-looking
wrong value four passes downstream. Every rule below defends against that:

- **Positive + negative per type and per AST variant.** A closed input set
  (map-key kinds, arithmetic operand kinds) gets every member AND complement
  members that must reject. Negative coverage is the load-bearing half.
- **Every behavior is visible to grep** — a passing test that pins it, or a
  `GTEST_SKIP` naming the verified blocker with the un-skip recipe baked in
  (§6.1).
- **One bug = one runnable regression** (`e2e/known_bugs_test.cc`); a fix is
  provably a fix.
- **The running oracle outranks source-reading** — answers come from the real
  cel-cpp pipeline (`testdata/cel_cpp_oracle.{h,cc}`, §5).
- **Differential at scale, pinned at the point.** PBT (§2, layer 5) finds
  divergences; hand-written tests pin them.

## 2. The layer pyramid

Eleven layers, inner-loop → gate; each pins a different failure class, and no
layer substitutes for another.

| # | Layer | What it pins | Runs when |
|---|-------|--------------|-----------|
| 1 | Unit/component tests (one `*_test.cc` per source file) | Per-component contracts: pass invariants, byte-exact rodata frames, error taxonomies, death tests on CHECK stubs. The conformance classifier is itself unit-tested (`conformance/runner_test.cc`). | default suite |
| 2 | Native-twin kernel tests | The wasm runtime's exact layout and semantics compiled natively with weak host stubs (`runtime/cel_runtime.c`); the same ABI re-pinned through real wasmtime in `runtime/cel_runtime_wasm_test.cc`. | native: default; wasm: manual |
| 3 | Build-time compile gates | Generated-code emitters fail `bazel build` on regression (codec genrules); `examples/` doubles as the public-surface compile gate. | every build |
| 4 | WAT harness (`tools/wat_runner`) | Frozen ABI/memory shapes per codegen arm. Honest numbers in §2.1. | manual |
| 5 | Property-based tests (`e2e/fuzz/`) | Oracle agreement over grammar-generated CEL sources; slot-aliasing, comprehension-storage, and arena-cliff classes. §2.2. | smoke + grammar/generator tests: default; the mining property: manual |
| 6 | Dual-link-mode e2e (`e2e/*.cc`) | Full Compile → Plan → Eval per theme; every suite builds twice via `link_mode_e2e_cc_test` (`e2e/link_mode_e2e_test.bzl`) — `<name>_dynamic` + `<name>_static` from one source. | default suite (4 targets manual, §3) |
| 7 | Known-bugs registry (`e2e/known_bugs_test.cc`) | Confirmed defects as runnable regressions; fixed bugs stay as live guards. §2.3. | default suite |
| 8 | Conformance + monotonic gate (`conformance/`) | Spec behavior against the vendored corpus (2454 rows); PASS count gated monotonically per link mode. §2.4. | pre-push + closeout |
| 9 | cel-cpp oracle (`testdata/cel_cpp_oracle*`) | The reference answer for any behavioral question, compared with the SAME comparator conformance uses. §5. | default suite |
| 10 | Examples smoke (`//examples:examples_smoke_test`) | Runs the nine numbered example binaries, asserts documented output lines verbatim — the doc-snippet rot gate (`examples/examples_smoke_test.sh`). | default suite |
| 11 | Benchmarks as correctness probes | Every bench Eval is `ABSL_CHECK_OK`'d and stamps a `result=` label; the corpus has surfaced real correctness bugs. Measurement role and rot risks: `07-benchmarking.md` §4.3/§8. | manual |

Structural axes, not per-test choices: the native-twin build (layer 2) and
dual-link-mode emission (layer 6) multiply coverage by build configuration; a
failure is attributable from the target name alone.

### 2.1 The WAT harness — honest coverage numbers

The WAT-first discipline requires every codegen arm and ABI surface to exist as
an executable `.wat` before C++ is written. Verified corpus state:

- **64 `.wat` files** under `doc/implementation-plan/rewrite/wat/`; **~33 are
  loaded** by `tools/wat_runner/wat_runner_test.cc`. The remaining ~30 have no
  executing consumer and may have rotted.
- `//tools/wat_runner:wat_runner_test` is tagged `manual`
  (`tools/wat_runner/BUILD.bazel`) — even the loaded subset runs only under
  the full-suite gate. Any claim of "every WAT re-runs on every build" is
  wrong on both axes.
- One WAT has a load-bearing consumer *outside* wat_runner:
  `wat/68_ReadSpanOobInTrampoline.wat`, exercised by
  `eval/internal/host_trampoline_bounds_test.cc` — the pattern to prefer for
  adversarial host-boundary shapes.
- wasmtime's C API panics on the `return_call`-from-runtime →
  imported-host-trampoline path, so the two dynamic-dispatcher WATs (09, 14)
  carry reasoned skips (`wat_runner_test.cc:515`, `:668`).

> **Open question (V31):** have the ~30 never-loaded WATs rotted? Probe:
> `wasm-as <file> -o /dev/null` over the unreferenced list; delete or re-adopt.

> **Open question (V32):** is the wasmtime C-API tail-call panic still live?
> Delete the skip at `wat_runner_test.cc:515` and run the target.

### 2.2 Property-based tests — the differential discovery layer

`e2e/fuzz/` is a typed-attribute-grammar CEL generator wired to fuzztest (a
`bazel_dep`, MODULE.bazel), differentially evaluated against the cel-cpp
oracle. Design rationale:
`doc/implementation-plan/rewrite/m27-pbt-cel-generator.md`.

- **Grammar** (`e2e/fuzz/grammar.{h,cc}`) — per `CelType`, a closed table of
  `Production` rows. Every emitted source type-checks *by construction*, and
  productions are total over their input domain — every divergence is a value
  mismatch, never a "both sides errored differently" false positive. Catalogs:
  `grammar_slice_b.{h,cc}` (scalars) and `grammar_slice_c.{h,cc}` (adds
  list/map literals, `size`, `in`, comprehension macros);
  struct/select/`has()` vocabulary deferred.
- **Generator** (`generator.{h,cc}`) — `GenerateExpr(grammar, target, ctx)`
  honors a depth budget; at depth 0 only leaf productions are eligible, so
  termination is guaranteed.
- **Three-layer grammar validation** so a property failure is necessarily a
  runtime/codegen bug: L1 structural (`Grammar::Validate()`); L2
  per-production round-trips through the real cel-cpp parser + checker; L3
  sampled composition at depth (`grammar_test.cc`, `generator_test.cc`,
  default-suite).
- **Harness** (`oracle_harness.{h,cc}`) — `GenAndEvalSliceC(target, seed,
  depth, out)` runs a generated source through BOTH our Compile → Plan → Eval
  and `EvalWithCelCpp`, with a fixed bound activation mirrored in both value
  representations (`BoundActivation`); source size capped (`kMaxSourceBytes`).
- **The property** (`cel_oracle_property_test.cc`) — asserts both engines
  accept and agree. **Tagged `manual` on purpose**: it mines for real bugs and
  WILL fail when it finds one. Run
  `bazel test //e2e/fuzz:cel_oracle_property_test` (~1000 iterations) or
  `bazel run --config=fuzztest //e2e/fuzz:cel_oracle_property_test --
  --fuzz=...` (coverage-guided).
- **Diagnostics** — `mine_divergences`, `dump_samples`, `//e2e:repro_pbt_bug`.
  All `manual`.
- **Divergence policy** — every PBT find becomes (a) a pinned row in
  `e2e/known_bugs_test.cc` carrying the originating fuzztest seed, (b) a
  focused unit test at the right layer, (c) a commit message citing the seed.
  PBT discovers; hand-written tests pin.

Track record: the property surfaced the comprehension-result storage-stamp bug
(fixed in `compiler/codegen/layout_pass.cc`, three pins in known_bugs), the
ternary-local-storage bug (`EmitSlotBaseAddress` treating a local index as a
byte offset, `compiler/codegen/expr_lower.cc`), and reproduced the per-Eval
arena cliff — the empirical case for the chained-grow arena
(`runtime/cel_arena.c`). Post-fix sweeps: 12,000 scalar + 5,500 list/map
programs, zero divergences.

### 2.3 The known-bugs registry — current pin states

`e2e/known_bugs_test.cc` is the structured home for confirmed defects: each
test asserts the SPEC-correct behavior; a leading `GTEST_SKIP` keeps CI green;
fixing = deleting the skip line. Entries are verify-first — candidates that did
not reproduce are recorded as deliberately absent. Current census (post-merge):
**26 live guards, 16 skipped open bugs**.

Live guards (formerly skipped, now regression pins):

- The map-dot-field sugar cluster (`{'a':1}.a`, reserved-word and
  backtick-quoted selectors, bind/comprehension variables) — closed ~29
  conformance rows via `EmitKSelect`'s map branch.
- Conversion pins: `int(-2^63.0)` range error, `int('+5')` / `uint('+5')`,
  UTF-8 codepoint `size()`.
- `string(double)` pins (`DoubleToStringShortestRoundTrip`,
  `DoubleToStringExponentForm`) — guard the `std::to_chars`-backed kernel in
  `runtime/cel_convert_double_format.cc`.
- `LiteralIntListInScanRejectedAtCompileAt10K` — the 10K-element literal list
  that formerly **panicked wasmtime mid-Eval** is now a graceful
  `ResourceExhausted` at Compile in BOTH link modes
  (`ValidateExprStaticRegion`, `compiler/internal/compile.cc`); boundary pins
  N=327 / N=328 bracket the exact window edge.
- `LongArith_2000Terms_NoUnalignedAtomicTrap` — fixed by LIFO free-list slot
  reuse (`compiler/codegen/slot_allocator.{h,cc}`), validated bottom-up by
  `slot_allocator_test` and top-down by this e2e.
- The four PBT-discovered pins (`PbtTernaryInsideIntSubtract`, `PbtExistsOne*`,
  `PbtSizeOfExistsOneTernaryBytes`), each carrying its originating seed.

Skipped (open, each with the verified reproduction and root-cause file:line in
its skip message): lossy double map-key equality; the two arena-cliff `size()`
cases (skip reason re-verified 2026-06-10 — they now trip the compile-time
static-region gate *before* the arena; un-skipping needs BOTH a relocatable
static region AND arena grow/spill); dyn double/uint list-index coercion;
`indexOf` pos byte-vs-codepoint bound; two `%f` format divergences; `exists`
error-accumulator absorption; `transformMapEntry` duplicate-key overwrite;
`transformMapEntry` computed-entry ABSL_CHECK abort (kept skipped because
running it aborts the binary); max-range timestamp nanos; `.?field`
static-subset rejection; `double('  3.14  ')` whitespace + 1-ULP parse; the
parser 100K-codepoint cap; the 10K bound-string-list arena OOM during `in`
scan.

### 2.4 Conformance + the monotonic gate

The harness (`conformance/runner.{h,cc}`, `run_conformance.cc`) runs the
vendored corpus (`spec/tests/simple/testdata/*.textproto`, 30 fixtures, 2454
rows) through the real pipeline. Contracts (each pinned by
`conformance/runner_test.cc`):

- **Outcome taxonomy** — every row is exactly one of kPass /
  kUnsupported(SKIP) / kFail; every SKIP carries a `SkipCategory` tag (now
  **ten** categories; `kSpecUnimpl` mirrors cel-cpp's own `_TESTS_TO_SKIP`
  per-(section, test-name), `runner.cc::IsSpecUnimplSection`).
- **Classification reads status payloads, never message text**
  (`compiler/frontend/status_tags.h` URL constants, read at
  `ClassifyCompileFailure`).
- **Error matching is kind-only**, by upstream precedent; same for unknowns
  (`CompareUnknown` checks `IsUnknown()` only). See §8.
- **Value comparison**: maps order-agnostic, lists order-aware, NaN matches
  NaN, Any unpacked via the generated pool; `MaybeCompareWktPrimitive` bridges
  empty Duration/Timestamp Any matchers against our CEL-primitive form.
- `run_conformance` **exits 0 unconditionally** — a reporting tool. The gate is
  `scripts/check_conformance_monotonic.sh`: one run per link mode, PASS count
  enforced against a per-mode monotonic baseline. Baselines (in-tree):
  `conformance/.baseline` = **1966**, `conformance/.baseline_static` =
  **1966**; README headline `total=2454 pass=1966 (80.1%) skip=481 fail=7`.
  Deliberately fastbuild — pass counts are config-identical; an opt rebuild
  costs ~10 min per push.

> **Open question (V39):** the README's hand-prose FAIL-buckets section says
> "93 FAILs"; the autogen headline says 92. Run
> `scripts/regen_conformance_readme.sh --check` after the next conformance run.
> The in-tree headline and baselines also predate the merged classifier
> changes (spec_unimpl reclassification, WKT-primitive compare, proto2
> extension fields); the next gated run moves all three numbers and must be
> locked via `--update`.

## 3. The gates

Four gates, cheapest first. The inner loop touches only what changed; the
exhaustive pass is explicit.

1. **`bazel test //...`** — the default suite: layers 1, 2 (native), 3, 5
   (smoke/grammar), 6, 7, 9, 10. Green here is necessary, never sufficient
   (§6.1).
2. **`scripts/run_full_suite.sh`** — the milestone-close gate: the default
   suite, then every `manual`-tagged test, then the conformance gate. The
   manual list is **query-driven**:

   ```
   bazel query 'attr(tags, "manual", tests(//...))'
   ```

   Cite the query, never a name list — a hardcoded list rotted the first time
   the dual-mode macro renamed every e2e target. Manual today covers the
   wasmtime-instantiating eval tests, the wasm-cross runtime test, wat_runner,
   the four host/foreign-fn e2e matrices, the CLI smoke + activation matrix,
   `run_conformance`, the PBT mining property, and all of `//benchmark/...`.
3. **`scripts/check_conformance_monotonic.sh`** — §2.4. Wired into
   `.githooks/pre-push`, which then runs
   `scripts/regen_conformance_readme.sh --check --from-log` so the README's
   AUTOGEN tables cannot drift from the live run.
4. **`scripts/check_doc_drift.sh`** — greps backtick-quoted paths and symbols
   in `doc/implementation-plan/**` against the tracked tree. Advisory by
   default; `--strict` gates. Not yet wired into any hook.

Known weakness: the monotonic gate checks PASS count only — a SKIP→FAIL swap
paired with a graduating row **nets zero and passes**. Fix candidates
(`conformance/README.md` "Future work"): a corpus-wide `kFail == 0` cc_test,
or pinned per-fixture `(pass, fail)` tuples.

> **Open question (V40):** is the manual-target query complete against the
> BUILD files, and which remaining conformance FAILs lack pinning tests? The
> proto2-ext / parse / enums buckets have historically been tracked only as
> README prose, violating "conformance FAILs are bugs too."

> **Open question (V41):** do the dual-mode e2e targets actually run in the
> default suite (no `.bazelrc` filter excludes them)?
> `bazel query 'tests(//e2e/...) except attr(tags, "manual",
> tests(//e2e/...))'`, then `bazel test //e2e/...`.

### 3.1 Test classification — Fast vs Slow

Every test target carries an **explicit** Bazel `size`; exactly **two
buckets** — `medium` is banned:

- **Fast → `size = "small"`** (60 s timeout): runs `< 20 s` locally.
- **Slow → `size = "large"`** (900 s timeout): runs `>= 20 s` locally.

An unset `size` *is* `medium` (300 s) — the trap this scheme closes: a target
drifting to 240–299 s sits silently 0.3 s from a CI timeout (live example:
`//e2e:host_fn_test_static` at 299.1 s). Collapsing to small/large lifts every
slow target to a ceiling it cannot realistically hit (slowest large target:
~127 s, 14% of timeout) and leaves no ambiguous middle. **Never leave `size`
unset.**

The dual-emission macros (§4.3) forward one `size` kwarg to **both** the
`_dynamic` and `_static` emissions; the `_static` variant pays a per-compile
Binaryen runtime-merge and is routinely the slow one. Rule: **if either
emission is Slow, set `size = "large"` on the macro call** — over-provisioning
the fast `_dynamic` emission is harmless (a timeout is a ceiling, not a
target). 22 fast `_dynamic` emissions are over-provisioned this way, which is
why bucket counts do not equal the raw per-emission Fast/Slow split.

Mis-bucketing: a `small` target whose **actual** runtime exceeds ~40 s — most
often only under full-suite parallel load — moves to `large`
(`//compiler/frontend:parse_and_check_test` and
`//compiler/internal:compile_test` were reclassified this way).

**Current census (2026-06-10):** Fast = 93 `small`, Slow = 66 `large`,
`medium` = 0, over 159 test targets; all passing. Verify:

```
bazel query 'attr(size, "medium", tests(//...))'   # MUST be empty
bazel query 'attr(size, "small",  tests(//...))' | wc -l
bazel query 'attr(size, "large",  tests(//...))' | wc -l
```

## 4. Disciplines

### 4.1 Skip discipline

Never skip a fixture's `SetUp` (skip the individual case). Every skip names a
**verified** blocker, carries the assertion it will make (closing the blocker
is "delete one line, confirm green"), and a skip that lingers after its
blocker ships is a review finding.

The lingering-skip rule has no enforcement mechanism today, and it leaks.
Verified stale-skip inventory:

- `e2e/m2_test.cc:206,210`, `e2e/m4_test.cc:448` — cite a host arena-plumbing
  gap; the bindings work throughout `e2e/m5_test.cc` and
  `e2e/activation_boundary_test.cc`.
- `e2e/foreign_fn_type_matrix_test.cc` — ~40 skips cite `kBlockerB0`
  ("AddComponent returns Unimplemented"); AddComponent is fully implemented
  and exercised by `e2e/foreign_component_dispatch_test.cc`.
- `compiler/codegen/expr_lower_test.cc:600` — cites a long-shipped lowering
  dependency.
- `eval/engine_test.cc:933` — cites a missing component fixture; fixtures
  exist under `e2e/foreign_component_fixtures/`.

> **Open question (V22):** the stale-skip sweep — delete each skip, run its
> target, classify pass / fail-with-new-reason. The periodic review pass
> should include a mechanical skip audit.

### 4.2 Known-bugs lifecycle

found → verified reproducing → skipped assertion in `known_bugs_test.cc` with
root-cause file:line → fix deletes the skip → the assertion stays forever as a
live guard. PBT-discovered rows carry their fuzztest seed; claims that don't
reproduce are recorded as deliberately absent. Non-eval findings go to
`doc/implementation-plan/known-issues-findings.md`.

### 4.3 Dual emission over runtime parameterization

Build-level matrix axes get **two targets**, not a test parameter: link mode
is a per-binary compile-time define (`CELWASM_E2E_USE_STATIC_LINK_MODE`, read
by `e2e/link_mode_e2e_helpers.h`), so a static-mode failure is attributable
from the target name alone and both modes ride the default suite.
`e2e/cctz_doubles_test.cc` is the forcing function for the static-mode
constructor hazard.

### 4.4 Matrix rules

Spell the matrix out, then exhaust it: every allowed type (bool, int, uint,
double, string, bytes, list, map, message, null, timestamp, duration, type),
every boundary value (`INT64_MIN/MAX`, `UINT64_MAX`, 0, -1, empty string,
embedded NUL, multi-byte UTF-8), every disallowed shape. Draft cases longhand,
consolidate structurally-identical ones into `TEST_P` tables, keep distinct
stories as individual `TEST_F`s (`runtime/cel_map_test.cc` is the canonical
shape). Host-edge boundary suites: `e2e/activation_boundary_test.cc` sweeps
every variable-length Value kind across the activation-buffer and arena
capacity boundaries (copy-marshaled string/bytes vs handle-passed
list/map/message); `e2e/proto_arena_lazy_copy_test.cc` pins the chained-grow
arena against huge proto fields.

### 4.5 Spec and oracle citations

Spec-mandated behavior (3VL, comprehension semantics, type coercion) cites the
`doc/langdef.md` section in the test comment; empirically-settled expected
values cite the oracle case. An expected value that was *reasoned out* rather
than oracle-confirmed is a guess (§5).

### 4.6 The feature pipeline — which files a feature touches

Start a feature session by copying the matching row's stage list into the
working notes; work top-down.

| Feature type | Stages (files) that MUST be touched, in order |
|---|---|
| New AST expression kind | frontend (`compiler/frontend/parse_and_check.cc`) → IR (`compiler/ir/annotations.h`, `typed_ast.cc`) → `compiler/codegen/resolve_pass.cc` → `layout_pass.cc` (+ `compiler/memory_layout.h` if a new region/stride) → WAT trace first (`doc/implementation-plan/rewrite/wat/`, `tools/wat_runner`) → `expr_lower.cc` → `module.cc` (new imports) → runtime helper (`runtime/cel_runtime.{h,c}` + `wasm_exports.txt`) → host trampoline (`eval/internal/cel_host.{h,cc}`) → ABI (`abi/cel_abi.proto`, `cel_abi_emit`, `eval/internal/abi_decode`) → `eval/engine.cc` / `instance.cc` → tests at EVERY touched layer + `e2e/` |
| New declarable type | `shared/type.{h,cc}` → `eval/value.{h,cc}` → IR `Repr` arm → frontend type spec → codegen allocator + `layout_pass.cc` Pack arm → `runtime/cel_data.h` kind + `cel_make` + log arm → `eval/instance.cc` encode/decode arms → parameterized Repr/Pack/value test rows |
| New host function | `eval/internal/cel_host.{h,cc}` (semantics → marshal → wasmtime registration) → `compiler/codegen/overload_table.cc` row → `expr_lower.cc` call emission → `module.cc` import → `eval/engine.cc` linker hook → WAT-with-stub before the real body → layered tests |
| Partial-eval / unknown plumbing | `resolve_pass.cc` attribute interning → `cel_abi.proto::AttributeEntry` emit/decode → trampoline pattern consult → `eval/instance.cc::PartialEval` → `eval/attribute` → tests at attribute / trampoline / e2e levels |
| New ABI field (additive) | `abi/cel_abi.proto` (fresh tag, never renumber) → `abi/cel_abi_emit.cc` → `eval/internal/abi_decode.cc` → the Plan-time consumer → tests at emit + decode + consumer |
| Lowering change, no new kind | the one `expr_lower.cc` arm → the WAT trace updated and re-run → `expr_lower_test.cc` shape assertions |

## 5. The oracle

`testdata/cel_cpp_oracle.{h,cc}` links the real cel-cpp parser + checker +
runtime, mirroring cel-cpp's modern conformance service. Two entry points:

- `EvalWithCelCpp(source, container)` → the neutral `cel.expr.Value` exchange
  proto plus `is_error` / `is_unknown` flags.
- `PartialEvalWithCelCpp(source, container, vars, unknown_patterns)` —
  activation bindings via `OracleVar`, dotted `AttributePattern`s,
  attribute-only unknown processing.

`testdata/cel_cpp_oracle_test.cc` runs differential assertions using **the
same comparator conformance uses** (`conformance::CompareValue`), so "agrees
with cel-cpp" and "passes conformance" are one equality. It is the
authoritative tiebreaker: when a code comment, memory, or spec-reading says A
and the oracle says B, the oracle wins.

It settles values, canonical string forms, error-vs-value, rounding/overflow
edges, heterogeneous equality, partial-eval semantics. For any behavioral
uncertainty, ADD a case and run it before writing the assertion; when a
question needs a surface the oracle lacks, **extend the oracle**, don't guess.

Remaining gaps: sample-based rather than corpus-wide (the PBT harness §2.2 now
provides the bulk-differential machinery); and the comparator inherits
kind-only error/unknown matching (§8), so the oracle cannot adjudicate
error-message or attribute-set questions.

## 6. The coverage ledger

The per-milestone coverage grid lives on as the appendix:
`doc/implementation-plan/testing-checklist.md` (its grids remain the tick-box
ledger — every merged feature flips at least one box).

### 6.1 The founding cautionary tale

On 2026-04-24, a routine validation found `bazel test //...` 100% green
(27/27) while a milestone described as shipped had its e2e suite tagged
`manual` — running it explicitly revealed **29 of 44 tests GTEST_SKIPped at
the fixture level** and two host trampoline bodies declared but never defined.
The lesson is the spine of this doc: **default-suite green is not the same as
the feature working.** Manual-tagged tests carry load-bearing assertions and
must run explicitly; fixture-level skips can hide an entire feature; the
closeout gate checks this mechanically.

### 6.2 The closeout gate

A slice is done only when: every touched source file has a paired `*_test.cc`
exercising every new path (positive + negative + boundary); the feature's e2e
suite exists and is not skipped; `scripts/run_full_suite.sh` is green; and the
checklist rows are ticked citing the proving tests. Target names in closeout
text come from the query, never from memory — the runnable names are
`<name>_dynamic` / `<name>_static`.

### 6.3 Open-items register

The consolidated per-component gap register, with post-merge status.

| Gap | Status |
|---|---|
| Workspace/rodata budget unguarded at compile time | **Closed by the merge** — `ValidateExprStaticRegion` (`compiler/internal/compile.cc`) + `MemoryLayout::MaxWorkspaceBytes` (`compiler/memory_layout.h`); over/under-budget tests in both modes (`compiler/internal/compile_test.cc`); e2e boundary pins in known_bugs (§2.3). |
| Codegen hand-copies runtime layout constants ("magic-number gap") | **Narrowed, not closed** — `compiler/memory_layout.h` single-sources region/stride constants with static_asserts mirroring `runtime/cel_layout.h`; CelValue *kind codes* are still inline literals in `compiler/codegen/expr_lower.cc`, so a kind renumber still compiles green through `//compiler/codegen`. (Register item V11.) |
| Fixed 64 KiB arena as a correctness cliff | **Substantially closed** — chained-grow arena (`runtime/cel_arena.c`; wasm build mallocs follow-on chunks, native deliberately doesn't chain), pinned by `e2e/proto_arena_lazy_copy_test.cc`; residuals: the `cel_list_in` scan OOM and the registry's cliff-class comment still describing the fixed arena. |
| `run_conformance` cannot fail CI; SKIP→FAIL swaps invisible | Open (§3). |
| Unknown/error matchers compare kind only | Open by design at the error side; unknown attribute-set matching blocked on AST-id round-trip (`conformance/runner.h`). |
| The new `spec_unimpl` SkipCategory string is unpinned | **Opened by the merge** — `runner_test.cc::RoundTripsAllValues` pins nine of the now-ten names. |
| Three-way runtime export-list drift (engine import list / runtime linkopts / `wasm_imports.txt`) | Open; no consistency test. |
| Conformance FAILs without pinning tests (proto2-ext / parse / enums buckets) | Open; partially mooted by the merge's proto2-extension and spec_unimpl work — re-inventory after the next gated run (V40). |
| `expr_lower_comprehension.cc` has no paired `_test.cc` | Open (behavioral load carried by e2e + PBT). |
| Bench `result=` labels never machine-compared | Open — owned by `07-benchmarking.md` §8. |
| Oracle not run corpus-wide | Open; harness now exists (§5). |

## 7. Fuzzing — deferred (branch brief)

Coverage-guided fuzzing of untrusted inputs is **deferred to a dedicated
branch**. Target surfaces, in payoff order:

1. **Parser/frontend input** — `ParseAndCheck` over arbitrary CEL source:
   Status or TypedAst, never a CHECK-crash on embedder input.
2. **`abi_decode` wire bytes** — the one hand-rolled binary parser (LEB128 +
   section walk); pure function, no wasmtime dep — the ideal first target:
   OK/NotFound/InvalidArgument, never UB.
3. **Program bytes at `Engine::Plan`** — a Status, never a process abort
   (wasmtime C-API panics are the known hazard).
4. **Arena/limit edges (structure-aware)** — large literals, deep nests,
   slot-heavy shapes; the PBT grammar (§2.2) at high depth is the natural
   generator.

Already in place from the PBT layer: the fuzztest bazel dep, `--config=fuzztest`,
and the differential harness. Corpus persistence and a scheduled CI job are the
deferred remainder.

## 8. Known weaknesses of the testing system itself

What green does NOT mean:

- **The monotonic gate is blind to trades.** PASS-count-only; a SKIP→FAIL swap
  nets zero (§3). The README regen gate protects the *reporting*, not the
  *semantics*.
- **Error matching is kind-only.** A row expecting a specific eval error
  passes on *any* error; a compile failure satisfies an eval-error matcher.
  Free-text error messages do not survive the wasm round-trip (decoded errors
  carry the code, not the message) — message-level regressions are invisible
  to every layer.
- **Unknown matching is kind-only.** A partial-eval result carrying the WRONG
  attribute set still passes (`conformance/runner.h::CompareUnknown`).
- **Manual tags bit-rot.** Anything outside the default suite rots invisibly:
  ~30 WATs with no consumer (V31), `#ifdef`-disabled bench code with a live
  arena-poke bug (the R56/R59 stories — `07-benchmarking.md` §8). A manual
  tag is acceptable only when the target is reachable by the run_full_suite
  query; `#ifdef`-disabled code and unreferenced fixtures are dead, not
  "manual".
- **The magic-number floor.** A CelValue kind-code renumber still compiles
  green through all of `//compiler/codegen` and fails only at e2e (V11, §6.3).
- **Stale skips have no tripwire** (§4.1, V22).
- **The skip-category contract grew without its pin** — the tenth
  `SkipCategory` name is unpinned in `runner_test.cc` (§6.3).
- **In-tree conformance numbers lag the tree** — the merged spec_unimpl /
  WKT-compare / proto2-extension changes will move pass, skip, AND fail counts
  on the next gated run (V39, V40).
