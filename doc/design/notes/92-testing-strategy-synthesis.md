# 92 — testing-strategy synthesis (cross-component lens)

Input: all 13 per-component notes + README + doc-index in
`doc/design/notes/`, read in full 2026-06-10; spot-checks against code
where two notes disagreed (§5).  Output: the raw material + proposed
outline for the new `doc/design/` testing-strategy doc.

## 1. The layer model — what each layer pins

The testing-system note's seven layers (testing-system.md §1.1) are the
spine, but the other notes surface three more load-bearing layers it
folds away (build-time compile gates, the native/wasm dual kernel
build, benches-as-correctness-probes).  Consolidated model, ordered
inner-loop → gate:

| # | Layer | What it pins | Runs when | Key citations |
|---|-------|--------------|-----------|---------------|
| 1 | Unit/component tests (one `*_test.cc` per source) | Per-component contracts: pass invariants, byte-exact rodata frames, error taxonomies, death-tests on CHECK stubs | `bazel test //...` | per-component-test-coverage.md §3.1–§3.13; e.g. static_memory_builder_test (codegen-memory.md §4), runner_test pins the conformance classifier itself (testing-system.md §4) |
| 2 | Dual kernel build (native twin) | The wasm runtime's exact layout/semantics natively, with weak host stubs; the same ABI re-pinned through real wasmtime in `cel_runtime_wasm_test` (manual) | native: default suite; wasm: manual | runtime-kernel.md §1.1, §1.5 (`cel_memory.c:43-63`, weak stubs `cel_runtime.c:215-229`) |
| 3 | Build-time compile gates | Generated-code emitters (codec_dumper genrule fails `bazel build` on emitter regressions); examples as the public-surface compile gate | every build | celfn.md §1.8 (BUILD.bazel:118-163); tools-examples.md §5.10 — note 09 violates the gate (dep on `//:internal` target, tools-examples.md §2.4) |
| 4 | WAT harness (`wat_runner`) | Frozen ABI/memory shapes per codegen arm; missing-stub ⇒ instantiation failure is itself a contract | manual only | tools-examples.md §1.4; coverage is 33 of 63 WAT files, NOT "every" (§5.1 below) |
| 5 | E2E suites, dual link-mode emission | Full Compile→Plan→Eval per milestone theme; `link_mode_e2e_cc_test` emits `_dynamic` + `_static` from one source so both modes ride the default suite with zero per-test code | default suite (only 4 e2e targets are manual) | testing-system.md §1.1.3, §5.10 (e2e/link_mode_e2e_test.bzl:25-43) |
| 6 | Known-bugs registry | One CONFIRMED defect = one runnable regression; fix = delete the `GTEST_SKIP` line; fixed bugs stay as live guards | default suite | testing-system.md §1.1.4 (e2e/known_bugs_test.cc:1-17) |
| 7 | Conformance harness + monotonic gate | Spec behavior against the vendored corpus (2454 rows, pass=1899); gate enforces monotonic PASS per link mode; README tables drift-gated by regen `--check` in pre-push | gate: pre-push + milestone close | testing-system.md §1.2 (runner.cc, check_conformance_monotonic.sh:42-43) |
| 8 | cel-cpp differential oracle | "Correct" for any behavioral question — value, error-vs-value, partial-eval — using the SAME comparator as conformance, so "agrees with cel-cpp" ≡ "passes conformance" | default suite (sample-based) | testing-system.md §1.3, §5.8 (cel_cpp_oracle_test.cc:100-118) |
| 9 | Examples smoke test | Doc-snippet rot gate: runs all 9 example binaries, asserts documented output lines verbatim | default suite | testing-system.md §1.1.7 (examples/BUILD.bazel:185-200) |
| 10 | Benchmarks as correctness probes | Every Eval `ABSL_CHECK_OK`; `result=` labels; the corpus surfaced 3 real correctness bugs (ternary-null, dynamic-mode rodata miscompare, het-eq gaps) — but manual-tagged + never built in CI, so the layer bit-rots (M7B kernel-name drift) | manual | benchmarking.md §4, §5.8 (OPERATORS.md:317-340); rot: benchmarking.md §2.4 |

The closing gate binding layers 2/4/5(manual)/7/10:
`scripts/run_full_suite.sh`, whose manual-target list is **query-driven**
(`bazel query 'attr(tags, "manual", tests(//...))'`, script :39-47) —
the durable form after a hardcoded list rotted (testing-system.md §5.2).

## 2. Per-component coverage gaps (the readers' consolidated register)

Highest-leverage first; each entry is a candidate row for the new doc's
coverage ledger.

**Systemic:**
- **No fuzzing anywhere** — verified again this pass: grep for
  `cc_fuzz|fuzztest|FUZZ_TEST|LLVMFuzzer` over compiler/ eval/ runtime/
  abi/ conformance/ tools/ returns zero hits (also testing-system.md §4).
  See §4.
- **92 conformance FAILs largely lack pinning tests** — known_bugs
  encodes ~27 expressions; proto2-ext/parse/enums buckets (57 rows)
  tracked only as README prose, violating "Conformance FAILs are bugs
  too" (testing-system.md §4, validation item 7).
- **The gate cannot catch a SKIP→FAIL swap** — monotonic PASS only;
  `run_conformance` exits 0 unconditionally (run_conformance.cc:249;
  testing-system.md §4).
- **Unknown matchers compare kind only** (runner.h:127-131) — a
  partial-eval result with the WRONG attribute set passes conformance.
- **Three-way runtime export-list drift has no consistency check**
  (engine.cc::kRuntimeExports / runtime linkopts / wasm_imports.txt;
  testing-system.md §4 + runtime-kernel.md §4 "no automated check that
  the built wasm's export section matches the marker-derived catalogue").
- **Magic-number drift floor**: codegen hand-copies CelValue layout
  constants (24-byte stride, CEL_BOOL/CEL_INT, offsets) with no dep on
  `runtime/cel_data.h` — a layout change compiles green through all of
  `//compiler/codegen` and fails only at e2e (codegen-lowering.md
  "CelValue ABI constants"; expr_lower.cc:1080-1091,
  expr_lower_comprehension.cc:456,958).

**Per component:**
- *frontend-ir*: zero unit tests for the four non-dyn RejectDyn
  carve-outs (Any-select, math.@min/@max, `.format([...])`, cel.bind);
  status payloads (`kStaticSubsetViolationUrl` bodies) untested at the
  producer; the two constant-inlining rewrites pinned only via e2e
  (frontend-ir.md §4).
- *codegen-lowering*: `expr_lower_comprehension.cc` has no paired
  `_test.cc` (violates the repo rule); cross-numeric re-pick 2/24 cells
  unit-tested (behavioral load on e2e/m5); `ResolveCallHelper` negative
  paths untested (codegen-lowering.md §4).
- *codegen-memory*: **no workspace-budget guard exists at all** (the
  doc-promised LayoutPass ResourceExhausted is unimplemented — P0,
  codegen-memory.md §2.1), hence no negative test can exist;
  `rodata_base_override` zero tests/zero callers; `AllocateType` no
  direct unit test; equal-literals-distinct-frames untested
  (codegen-memory.md §4).
- *compiler-toplevel*: `container` untested anywhere;
  `optimize_level` facade contract unpinned (negative levels silently
  no-op vs documented reject);
  `MemSizeBytesLargerThanOnePageGrowsPageCount` asserts nothing about
  page count (compiler-toplevel.md §4).
- *abi-shared*: `ir::Repr` numeric values unpinned (implicit enum, wire
  contract promises stability — discrepancy #6); `DecodeRepr`
  aggregate/out-of-range arms untested; `variables[]` emission under
  comprehensions untested (positional-invariant doubt); `cel.wit` has no
  validation target (abi-shared.md §4).
- *runtime-kernel*: PRESIZE_INVARIANT traps have no death/trap tests;
  `arena_alloc`-before-init trap uncovered on the wasm path; wasm
  `cel_mem_size()` value unpinned (keeps the stale-64KiB footgun
  latent) (runtime-kernel.md §4).
- *eval-internal*: cel_log `%v` never tested against the
  production-encoded bare-code error wire (fixtures hand-built to the
  old descriptor shape — divergence D2 invisible to the suite);
  `DecodeCelError` kInvalidArgument miss untested; iter-open OOM
  fallbacks untested (eval-internal.md §4).
- *eval-public*: activation-bind coercion seam (WKT wrapper/time peel,
  null-to-scalar) untested at the unit level; zero-arg `Eval()`
  performs no externref Reset — growth behavior untested;
  AddModule/AddComponent malformed-bytes tests assert only `!ok()`,
  leaving wrong header-claimed status codes unpinned (eval-public.md §4).
- *celfn*: `LowerToCustomFn` zero tests AND zero callers; no test that a
  `@native` (kCelDefined) library compiles or is rejected — the
  shipped behavior is an unresolvable wasm import; `run_generate.cc`
  has no test (the component's only untested source file);
  `full_matrix.idl` exists but is dead (no BUILD target), hiding the
  missing map-lower emitter; 254/255/256-param boundaries untested
  (celfn.md §2.1, §4).
- *tools-examples*: `ExtractRepeated` argv edge shapes unpinned;
  ~30 WAT trace files have no executing consumer; `cel generate` never
  invoked by any test (tools-examples.md §4).
- *benchmarking*: nothing machine-compares `result=` labels across
  binaries or against `expected` — a wrong-value regression times
  successfully; bench binaries never build in CI (benchmarking.md §4).

## 3. Skip / manual-tag discipline — as practiced, and where it leaks

Rules (per-component-test-coverage.md §4; CLAUDE.md): never
fixture-level `SetUp` skips; every skip names a verified blocker with
the un-skip recipe baked in; a skip lingering after its blocker is gone
is a review finding.  Founding rationale: the M2 incident — `bazel test`
green while 29/44 e2e tests silently skipped behind a manual tag
(testing-system.md §1.5).

Verified state: 97 `GTEST_SKIP`s in e2e/ outside known_bugs, no
fixture-level skips found (testing-system.md §1.4).  But the
cross-component read shows the "lingering skip" rule has **no
enforcement mechanism**, and it is leaking — four independent notes
found stale skips whose blockers shipped:

1. `e2e/foreign_fn_type_matrix_test.cc:121-126` — ~40 skips cite
   "Engine::AddComponent returns Unimplemented"; AddComponent is fully
   implemented (engine.cc:1489-1539) (testing-system.md §2.2,
   eval-public.md §2.1).
2. `e2e/m2_test.cc:206,210` + `e2e/m4_test.cc:448` — "needs host arena
   plumbing; deferred to M2.C"; string/bytes binding works throughout
   m5/activation_boundary suites (testing-system.md §2.3).
3. `compiler/codegen/expr_lower_test.cc:583-601` — "until M3 kCall
   lands"; M3 shipped long ago (codegen-lowering.md §4).
4. `eval/engine_test.cc:850` — "blocked on a real Component-Model
   fixture"; fixtures exist under e2e/foreign_component_fixtures/
   (eval-public.md §3.5).

Manual-tag story: the load-bearing mitigation is structural — most e2e
suites are NOT manual anymore (only 4 e2e targets carry the tag), and
the gate's manual list is query-driven.  But the keystone doc
(per-component-test-coverage.md §2 catalog + §5 closeout block) names
dead/re-tagged targets (`//e2e:m<N>_test` vs the macro-emitted
`_dynamic`/`_static` pair) — the closeout checklist is uncopyable as
written (testing-system.md §2.1).  The new doc should: (a) cite the
query, never target names; (b) propose a cheap skip-audit (grep skips,
extract blocker tags, cross-check against shipped symbols) as part of
the periodic review pass.

A second leak class: manual-tagged code that nothing exercises rots
invisibly — the M7B `#ifdef` kernel benches reference helper names the
runtime renamed (`cel_ts_year_utc` vs `_at_v`), and ~30 WATs have no
consumer (benchmarking.md §2.4, tools-examples.md §2.2).  Rule for the
new doc: a manual tag is acceptable only when the target is in the
run_full_suite query's reach; `#ifdef`-disabled code and unreferenced
fixtures are not "manual", they are dead.

## 4. Where fuzzing slots in

Confirmed absent (grep, this pass).  Four surfaces, in payoff order:

1. **`abi_decode` (wire bytes)** — the one hand-rolled binary parser in
   the tree: LEB128 + section walk over untrusted Program bytes
   (abi_decode.cc:19-106).  Pure function, no wasmtime dep, complete
   error taxonomy already enumerated (abi-shared.md §1.2) — ideal first
   fuzz target.  Property: returns OK/NotFound/InvalidArgument, never
   crashes/UB.  Seeds: the abi_decode_test fixtures + real compiled
   Programs.
2. **Parser/frontend (`ParseAndCheck` over arbitrary CEL source)** —
   cel-cpp's parser is vendored, but our gate + rewrites
   (InlineConstantReferences, InlineTypeIdentifierReferences, RejectDyn
   carve-outs) and the `.celfn` ANTLR grammar are first-party.  Known
   payoff signals: the parser codepoint-cap known-bug
   (known_bugs_test.cc:579-598), the `CelExprText` `;`-in-string lexing
   break (Celfn.g4:141-152), the 255-param `num_args` wrap
   (function_library.cc:205).  Property: Status or TypedAst, never
   CHECK-crash on embedder input (the kType-on-kHost crash,
   celfn.md §3.5, is exactly the class this finds).
3. **Program bytes → `Engine::Plan`** — arbitrary/mutated bytes must
   produce a Status, never a process abort.  The notes record two
   wasmtime-C-API panic surfaces (non-NUL-terminated trap messages,
   engine.cc:553-566; tail-call-to-host panics, tools-examples.md §1.4)
   plus the version/link-mode gates (CheckRuntimeAbiVersion,
   ValidateLinkModeLabel) — all reachable from bytes.  Seeds: compiled
   Programs both modes, WAT fixtures, truncations.
4. **Arena/workspace limits (structure-aware)** — a generator producing
   large literals, deep nests, comprehensions, and >340 slot-acquiring
   nodes, asserting "CEL_ERR_OVERFLOW value or clean Status, never
   trap/panic/memory corruption".  This targets the two open P0-class
   holes: the unbounded workspace overrunning runtime statics past 8192
   (codegen-memory.md §2.1) and the 10K-literal-list wasmtime panic
   (backlog #16; runtime-kernel.md §1.7 exonerates the kernels — the
   fault is codegen- or host-side, which is precisely what an e2e-shaped
   fuzzer exercises).

Highest-leverage long-term: **differential fuzzing against the oracle**
— an expression generator feeding both `Instance::Eval` and
`EvalWithCelCpp`, compared with the existing conformance comparator
(the `ExpectAgree` plumbing already exists, cel_cpp_oracle_test.cc:100-118).
This also closes the "oracle is sample-based, not corpus-wide" gap
(testing-system.md §4) — the corpus-wide bulk differential run is the
degenerate (non-mutating) case of the same harness.

Mechanics: FuzzTest or libFuzzer `cc_fuzz` targets, manual-tagged like
benches, run as a scheduled CI job (not the inner loop); seed corpora
from `spec/tests/simple/testdata/`, known_bugs expressions, and the
benchmark YAML corpus.  Smaller targets if cheap: `var_parser`
(tools/cel), `corpus_loader` (benchmark).

## 5. Note-vs-note disagreements — settled

1. **wat_runner WAT coverage.**  design-heritage.md §4 claims "59 .wat
   files … are assembled + executed by wat_runner_test per build";
   tools-examples.md §2.2 says ~half, manual-tagged.  Verified: 63
   `.wat` files on disk under doc/implementation-plan/rewrite/wat/; 33
   unique `.wat` paths referenced in
   tools/wat_runner/wat_runner_test.cc; the target is `manual`
   (tools/wat_runner/BUILD.bazel:32-35).  **tools-examples is right**;
   design-heritage §4 and CLAUDE.md's "every .wat on every build" are
   both wrong (subset, and never on a default build).
2. **SlotAllocator release.**  design-heritage.md §1.1 (§6 row) says
   "`Release` is no longer a no-op — free-list reuse landed";
   codegen-memory.md §1.5 says it is a no-op.  Verified:
   `slot_allocator.cc:24-28` — `Release` body is `(void)offset;` with
   the "Naive path (M1–M9): no-op … M10 flips on the free list"
   comment.  **codegen-memory is right**; slot reuse has NOT landed and
   `peak_slots` is total-acquires.  (Feeds the workspace-unbounded P0.)
3. **`runtime_catalogue_consistency_test`.**  design-heritage.md §4
   cites it as the live exports↔catalogue coherence pin; abi-shared.md
   §1.5/§2.3 says it was deleted as tautological (commit 511c3ec8).
   Verified: no such target in abi/BUILD.bazel (grep: zero hits); the
   only mentions are stale comments at runtime/BUILD.bazel:12 and
   runtime/wasm_exports.txt:14.  **abi-shared is right** — and the
   stale comments are themselves discrepancy items (abi-shared.md §2.3,
   runtime-kernel.md §2.11).  Consequence for the strategy doc: the
   exports↔catalogue guarantee is "derived from one source", with the
   per-toolchain-bump `wasm-dis` audit (abi-shared.md §3.5) as the only
   residual check.
4. (Intra-note, flagged not settled here: conformance README fail=92
   autogen vs "93 FAILs" hand prose — testing-system.md §2.5,
   validation item 4; requires a bazel run, out of scope this pass.)

## 6. Proposed outline — `doc/design/testing-strategy.md`

1. **Philosophy** — compilers fail silently; every behavior is visible
   to grep as a passing test or a reasoned skip; one bug = one runnable
   regression; the running oracle outranks source-reading; negative
   coverage is the load-bearing half.
2. **The layer pyramid** (diagram, shared palette) — the 10 layers of
   §1 with, per layer: what it pins, the failure class it catches
   first, and when it runs (default suite / manual / pre-push gate /
   milestone gate).  Includes the native-twin and dual-link-mode
   emission as structural axes, not test-by-test choices.
3. **The gates** — `bazel test //...` vs `run_full_suite.sh`
   (query-driven manual list — cite the query, never names) vs
   `check_conformance_monotonic.sh` (fastbuild, per link mode, dual
   baselines) vs the pre-push README regen check.  State the known
   gate weakness: monotonic-PASS nets zero on a SKIP→FAIL swap; name
   the fix candidates (kFail==0 test / pinned per-fixture tuples).
4. **Disciplines** — skip discipline (M2 incident as the canonical
   counter-example; the §3 stale-skip inventory as the cleanup list +
   the periodic skip-audit proposal); known-bugs lifecycle
   (verify-first, fixed-stay-as-guards); dual-emission over runtime
   parameterization for build axes; matrix rules (full type list,
   boundary values, complement-set negatives); spec/oracle citations in
   test comments.
5. **The oracle** — API surface (Eval + PartialEval), the comparator
   identity with conformance, extension rule ("extend the oracle, don't
   guess"); planned: corpus-wide bulk differential run.
6. **Coverage ledger** — the successor of
   per-component-test-coverage.md §2/§3 with the dead-target catalog
   fixed; the §2 gap register above as the open-items table, each with
   the component note that sourced it.
7. **Fuzzing** (new) — the four targets of §4 + differential fuzzing,
   infrastructure choice, seed corpora, CI placement, and the
   properties each fuzzer asserts.
8. **Known weaknesses of the testing system itself** — monotonic-gate
   blindness, kind-only unknown/error matching, bench/WAT bit-rot under
   manual tags, three-way export drift, the codegen↔runtime
   magic-number gap — so the doc is honest about what green does NOT
   mean.
