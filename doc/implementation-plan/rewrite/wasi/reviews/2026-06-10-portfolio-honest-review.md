# 2026-06-10 — Honest whole-repo review (portfolio lens)

Scope: full first-party tree (~111k LOC) at `88b3a9f2` on
`m28-configurable-linking`, reviewed by four parallel passes
(architecture/design drift, tech debt, test honesty, docs-vs-reality),
explicitly framed for the repo's intended use as a **portfolio project**.
Builds on the 2026-06-09 portfolio-readiness and production-readiness
reviews; per-item dispositions of those reviews' findings are in §5.

## Verdict: MIXED — strong substance, trust-surface polish lagging

The engineering substance is genuinely strong: 1966/2454 conformance
(80.1%) with a monotonic dual-mode gate, an honest two-sided perf story
(geomean 0.95× with the losses published), line-accurate design docs
rebuilt from the code, a disciplined runtime kernel, and a model m28
closeout. The weakness is concentrated in exactly the surface a
portfolio reviewer touches first: one outright-false front-page claim
("run by CI" — there is no CI), a conformance README that contradicts
itself (93 vs 7 FAILs), a public header declaring a shipped+demoed
feature "not yet implemented", ~60 stale GTEST_SKIPs citing blockers
that shipped weeks ago, and dead doc links on GitHub because the
design-doc commit never left the local machine. None of it is dishonest
by intent — it is all "doc lagged the last two merges" — but for a repo
whose explicit brand is honesty, these are the findings that cost the
most per minute of reviewer attention.

Top 3 to fix first:
1. The "run by CI" claims (`README.md:88`, `doc/README.md:28`) — delete,
   soften to "gated by `bazel test`", or land a real workflow.
2. Push `88b3a9f2` (design docs 00-07) to origin — GitHub's doc index
   has dead links until it lands.
3. `conformance/README.md` hand-written FAIL-bucket section (claims 93
   FAILs across 14 fixtures; the autogen headline says 7).

## 1. Architectural drift

- **P1 — the headline layering rule is false as written.** CLAUDE.md and
  `doc/design/00-architecture.md:344-346` say compiler/ and eval/
  "neither depends on the other". Bazel says otherwise: production
  `//eval:{engine,instance,abi_decode,instance_impl}` depend on
  `//compiler:program`, `//compiler/celfn:function_library`, and
  `//compiler/ir:annotations` (`eval/engine.h:43-44`,
  `eval/internal/abi_decode.h:18`). The load-bearing half DOES hold:
  `deps(//compiler/...)` ∩ eval/wasmtime is empty — compiler stays
  wasm-targetable. The generated diagram is honest; the prose isn't.
  Correction: DOC (state the real one-directional rule). Optional CODE:
  `ir::Repr` is wire vocabulary and belongs in `shared/`/`abi/`, which
  would also let `abi_decode` move under `abi/` (whose "emit *and*
  parse" doc claim is currently half-true).
- **P1 — error/unknown wire forks are documented but live.** Verified in
  code: two `DecodeCelError`s disagree (`eval/instance.cc:249-269` omits
  `kInvalidArgument`; `host_call_context.cc:85` has it); 3VL precedence
  differs by routing (kernel `absorb_3vl_binary` error-dominant,
  `runtime/cel_internal.h:82-101`; host `AbsorbBinary` first-operand-wins,
  `eval/internal/cel_host_error.cc:134-144`) — a latent conformance
  diff; `cel_log`'s `%v` error formatter reads a struct shape the
  production wire never writes (`eval/host/cel_log.cc:169-182`), with
  tests pinning the dead shape. These are §8 of
  `doc/design/03-abi-and-memory.md` — tracked, but tracked-and-live is
  still live. The V2/V3/V4 probes belong at the top of the next queue.
- **P2 — visibility regime drift.** `tools/cel`, `tools/wat_runner`, and
  `bazel/` are package-default public (incl. tests and a test helper
  that deps `//compiler:compiler`); `//eval:host_call_context` and
  `//eval:typed_function` are public but absent from CLAUDE.md's
  "Exactly:" curated list. Narrow package defaults; refresh the list.
- **P2 — `compiler/codegen/module.h:156-160`** carries the thread-safety
  claim `00-architecture.md:285-297` already debunks (process-global
  Binaryen optimize-level race). Fix the comment embedders read.
- **P2 nits:** NodeAnnotation "12 fields" doc vs 15 actual; blind
  `static_cast` between the three LinkMode enums with no `static_assert`
  (`compiler/compiler.cc:182`); `Repr` implicitly numbered while treated
  wire-stable; a fresh banned milestone-reference comment at
  `compiler/compiler.cc:180` ("m28 plumbing").

## 2. Tech debt

- **P1 — `eval/internal/cel_host.cc` is 4,106 lines** (2.6× the next
  largest file) with **12 functions over the 60-line gate** (largest:
  `SetScalarField`, 187 lines, :2592) and zero NOLINTs — invisible
  because the lint backlog only ever inventoried `compiler/`. Its
  decomposition milestone `m11-cel-host-refactor.md` has read "in
  flight — Slices B/C/D/F/G/H/I pending" since 2026-05-19 while the
  file absorbed 10 more commits. If M11 stays parked through two more
  milestones, the debt verdict flips from "mixed" to "accumulating".
- **P1 — all 5 production stubs name shipped milestones**
  (`eval/activation.cc:29,37` "until M2"/"until M5";
  `compiler/codegen/static_memory_builder.cc:142,151` "until M5/M6" —
  bypassed by the arena path, with tests pinning the stale messages;
  `compiler/frontend/parse_and_check.cc:769` and
  `eval/internal/cel_component.cc:847` "until m24" — m24 shipped).
  Still CHECK-loud (nothing miscompiles), but the milestone pointer is
  the load-bearing part and every one now misleads.
- **P1 — lint-backlog.md is a graveyard:** ~330 of its 545 lines cite
  directories deleted in the 2026-05-25 restructure (`compiler/host/`,
  `compiler/e2e/`); counts are M4-era; the regen command filters
  `compiler/` so eval/ was never scanned. Regenerate or delete.
- **P1 — open crash class filed under "Closed":** deep `a+b+c+…` chains
  (~4.6k terms) SIGSEGV via N-deep nested wasm expr trees —
  `cleanup-backlog.md:1547`, an unchecked `[ ]` **#37** sitting in the
  "## Closed" section, with reused IDs (#31/#32/#37 each appear twice).
- **P1 — phantom feature surface:** `compiler/celfn/library_module.h`
  declares `CompileLibraryBodies` with no `.cc` ever in history, no
  BUILD target, no includers; `compile.h:127`
  (`CompileResult::library_modules`) written and read nowhere. Delete.
- **P2:** wrapper-FQN→kind table duplicated compiler↔eval
  (`expr_lower.cc:639` / `eval/instance.cc:552`); the four
  `CrossNumeric*Id` switches are one table written four times
  (`expr_lower.cc:912-988`); silent `return "unknown"` after a
  closed-enum switch in `engine.cc` (~:1273) and the kernel's OOM→
  empty-walk fallback (`cel_runtime.c:307-330`) are banned-pattern
  stragglers; 32-bit `size_t` multiply in arena size math wants a
  one-line overflow guard.
- **Backlog health:** cleanup-backlog is a working queue, not a
  graveyard — m28 closed all 8 of its P1s pre-merge; close cadence on
  items that matter is days. But open count tripled (6→27) in three
  weeks, items #1–#6 (2026-05-18) are untouched, and **#1 claims Linux
  builds fail at analysis** — directly contradicting CLAUDE.md's
  cross-platform claim; one of the two is wrong and a portfolio
  reviewer on Linux will find out which.

## 3. Test honesty: mostly-honest, skip hygiene rotting

- **P1 — ~40 skips cite kBlockerB0** ("`Engine::AddComponent` returns
  `Unimplemented`", `e2e/foreign_fn_type_matrix_test.cc:121`) — false
  since m24 shipped 2026-06-04 (`eval/engine.cc:1519` is a full
  implementation, exercised by `e2e/foreign_component_dispatch_test.cc`).
  Sweep: unskip what passes, re-cite what doesn't.
- **P1 — ~22 more stale milestone-pending skips** whose blockers shipped
  3+ weeks ago: `e2e/m5b_test.cc:996-1042` ("M5.B.D ships here" —
  shipped 05-17), `e2e/m8_test.cc:1109-1303` (M8.A shipped),
  `e2e/m9_test.cc:268,474` + `e2e/m10_test.cc:835-850` (timestamps
  shipped in m7b), `e2e/m2_test.cc:206,210` (string-arena plumbing is
  `eval/instance.cc:486-529`; :210 is also a bare skip with no assertion
  body), `compiler/codegen/expr_lower_test.cc:600` (M3),
  `eval/engine_test.cc:933` (component fixtures now exist).
- **P1 — m27 (PBT) shipped with no closeout:** machinery landed
  (b70676f7, e2e/fuzz/ exists, +50 conformance rows) but
  `m27-pbt-cel-generator.md` still says "design … Slice A starting
  next" and testing-checklist.md has zero m27/PBT rows. Both closeout
  rules skipped. (Same family, lighter: m26 says "plan — not yet
  started" while `bazel/cel_wasm_component.bzl` shipped in 0030c89d.)
- **P2 — the conformance gate enforces monotonic pass count only;**
  a SKIP→FAIL conversion at flat pass count passes silently. With only
  7 FAILs left, a `fail<=N` clamp is cheap. `spec_unimpl` skip-table
  row still says "_New category — fill in disposition prose._".
- **Clean:** no SetUp-level skips; no "feature isn't done" phrasing;
  known_bugs_test.cc (16 delete-this-line-to-fix pins) is exemplary;
  manual-target suite is genuinely query-driven — zero silent holes;
  0 hard-uncovered source files (4 letter-violations of the
  one-test-per-cc rule, all materially covered:
  `eval/internal/{instance_impl,wasmtime_engine_state,cel_host_wasmtime}.cc`,
  `compiler/codegen/expr_lower_comprehension.cc`).

## 4. Docs vs reality (the portfolio surface)

- **P1 — F1: README claims CI that does not exist** (`README.md:88`
  "run by CI"; `doc/README.md:28` "CI-gated"). `.github/` has only the
  PR template; `cloudbuild.yaml` is the same stale no-test config both
  06-09 reviews flagged. The smoke test is real and runs under
  `bazel test` — say that, or land CI.
- **P1 — F2: conformance/README self-contradiction** — autogen headline
  `fail=7 (0.3%)`; hand-written bucket section 40 lines down: "93 FAILs
  across 14 fixtures" with the full pre-ssp-fix table, citing a backlog
  item marked FIXED 2026-06-05.
- **P1 — F3: `eval/engine.h:174-176`** says AddComponent "**Status: not
  yet implemented** … returns `Unimplemented`" — shipped 06-04, demoed
  in `examples/09_component_functions.cc`. Same family:
  `compiler/compiler.h:207` "Storage only until Slice C.3".
- **P1 — F8 (verified this review): `88b3a9f2` is local-only.**
  origin/master (`3d9bab24`) carries the rewritten `doc/README.md`
  index whose links to `design/03-abi-and-memory.md` and
  `design/06-testing-strategy.md` resolve only in the unpushed commit —
  dead links on GitHub right now.
- **P2:** README/examples-README say "seven runnable programs" — there
  are nine; backlog #16 unchecked despite fix commit 8041dc97;
  `benchmark/DESIGN.md` still leads with the prototype losing-table and
  no pointer to `m28-bench-results.md`; `doc/contributing.md` still
  titled "celwasmc", never mentions `fetch_cel_cpp.sh` (fresh clone
  following it alone fails), no cold-build warning; `module()` in
  `MODULE.bazel` carries no version (correction: the originally-cited
  `0.25.1` is the cel-spec *dependency* pin, which is legitimate), no
  tags, no CHANGELOG; merged/`claude/*` branches still on origin.
- **Fact-checks that PASSED:** quickstart targets all exist; conformance
  arithmetic internally consistent (1966+481+7=2454); the two-sided
  perf rule is fully implemented (losses table with causes, 48× regex
  win caveated, "_pending re-measure_" honesty note);
  `examples_smoke_test` is real; every doc/README link resolves locally.

## 5. Disposition of the 2026-06-09 reviews' P0/P1s

FIXED: MAINTAINERS.md provenance (model shape); m28 on master +
honest headline results in README; #16 crash class (code — backlog
checkbox stale); stale compiler.h/program.h header comments.
~70% of the combined P0/P1 surface closed within 24 hours — itself a
strength worth showing.

STILL OPEN: CI + badges (now with two new false "CI" claims);
versioning/CHANGELOG; #14 comprehension-over-UNKNOWN (3VL soundness,
skips at `e2e/m2_partial_eval_test.cc:276,348`); codegen re-deriving
semantic decisions (`MaybeRepickCrossNumericOverload`,
`expr_lower.cc:1008`); #31 error-message loss; #32 visibility;
HostMapView enumeration; `Value::AsProto<T>()`; `cel run`; string-
returning `@component` fns trap (honestly documented); fuzzing;
branch hygiene.

## 6. Strengths (the other half of honest)

- Conformance: 186 → 1,966 monotonic, dual-mode baselines verified
  byte-identical, pre-push gated.
- The perf narrative is the honest-claims culture actually operating
  under pressure: losses published with architectural causes, a lucky
  passing test retracted with a "pending re-measure" footnote.
- `doc/design/00-07` is line-accurate (every spot-checked citation
  exact, including a literal 271-seed count) and openly carries
  V-numbered open questions instead of papering over them; the diagram
  generator encodes verified BUILD edges and ended up more truthful
  than the prose around it.
- m28's link-mode split avoided the copy-paste fork: one compile-side
  branch, one eval-side router, wire-label cross-check, genuinely
  instantiated dual-mode test macro. m28's closeout doc is the model.
- The runtime kernel is disciplined C: checked allocs, sticky poison
  errors, every function under the size gate.
- Probe discipline held (`compiler/probes/` empty at closeout);
  query-driven manual suite replaced a rotted hardcoded list — a
  learned lesson, encoded.

## 7. Tracking

Per the review-process rules: P1s above go to the active milestone's
"Pre-close cleanup" (no milestone is active — they form the natural
next session's worklist, roughly: trust-surface fixes F1/F2/F3/F8 +
skip sweeps + stub-message sweep + m26/m27 closeouts ≈ one day);
P2s belong in `cleanup-backlog.md` tagged `2026-06-10 review` —
deferred to the follow-up commit the user authorises, since this
review deliberately changed no files outside this report.
