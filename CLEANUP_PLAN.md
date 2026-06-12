# CLEANUP_PLAN — Abseil-grade professionalization work queue

Status: live work queue — created 2026-06-09 from a four-agent inventory
(doc tree, compiler headers, eval/shared/abi/runtime headers, leaf dirs +
repo hygiene). This file is the single source of truth for the pass:
units burn down here, status is updated here, nothing is worked out of
order without a note.

Companion files: `PROPOSALS.md` (API/ABI-affecting changes — logged, not
made), `DEVREL_BRIEF.md` (produced at the end, Phase 5).

## Method (applies to every unit)

1. **Comprehension protocol — doc + code + tests are read together.**
   No comment or doc is rewritten from memory. Per unit: read the
   design doc(s) listed for the unit → read the code → read the tests.
   When the three disagree, tests arbitrate; when tests are silent,
   write a targeted test first (it stays in the suite). A contract that
   cannot be verified is flagged, not written.
2. **Consolidate, don't multiply.** Every doc has exactly one purpose.
   If two docs serve the same purpose, merge into the better home and
   banner-redirect the other. The doc tree must *flow*: README →
   getting-started → user guides → architecture → contributor docs →
   archive, with no dead ends and no duplicate tellings of the same
   story. Purposeless docs don't get polished — they get merged or
   archived.
3. **One unit = one commit**, independently revertable, build + tests
   green after each. Never mix mechanical formatting with substantive
   comment rewrites in one diff.
4. **Two registers.** User-facing surfaces (README, user-guide,
   getting-started): confident, example-led, every claim verifiable.
   Engineering surfaces (headers, architecture, contributor docs): dry
   Abseil register, zero salesmanship.
5. **TODO policy.** Never silently delete. Normalize to
   `TODO(owner-or-issue): actionable text`. Removal requires proof of
   done, noted in the commit message.
6. **Milestone-reference purge override.** CLAUDE.md grandfathers
   existing `M<n>`/`Slice` comment references for opportunistic
   cleanup; the owner has explicitly requested this pass purge them
   from headers (replace with design-doc path citations where
   load-bearing, plain contracts otherwise). The one carve-out stands:
   `ABSL_CHECK(false) << "... is a stub until <milestone>"` messages
   stay — they are load-bearing and deleted when the body lands.
7. **No public API signature/behavior changes.** Anything requiring one
   goes to `PROPOSALS.md`.
8. **Doc moves update every citation in the same commit.** The citation
   map (docs referenced from CLAUDE.md, scripts/, and C++ comments) is
   in §Citation-map below; consult it before any move.

## Inventory summary (2026-06-09 audit)

- **~130 markdown docs.** User-facing + checklist docs are current
  (2026-06-09 overhaul); ~50 shipped milestone plans are archive
  candidates; ~10 live plans feed the roadmap; 5 stale redirect stubs.
- **Headers are already good-to-excellent** (header guards 100%
  `CELWASM_<PATH>_H_`, IWYU spot-clean, rich field docs). Gaps:
  thread-safety statements missing on most public classes, ~120
  milestone refs in comments, a handful of stale "not yet implemented"
  claims, sparse per-method error contracts on eval public headers.
- **TODOs: 1 formal** (`cpp_codec_emitter.cc:371 TODO(m26)`) + 2
  informal ("not yet emitted" prose). Nothing else.
- **Hygiene:** `.clang-format`/`.clang-tidy` Google-based and
  documented (Phase 1 baseline already satisfied — no formatting
  commit needed). No CI workflows, no module version, no root
  CONTRIBUTING/SECURITY/CHANGELOG → PROPOSALS.md.
- **Dependency graph is acyclic and visibility-enforced** (two-tier
  regime verified; compiler ↛ eval, eval → {program, shared, runtime,
  abi}).

## Work queue

Legend: `[ ]` pending · `[~]` in progress · `[x]` done (commit hash).

### P1 — Misleading-now fixes (comment truth, smallest diffs first)

- [ ] **U01 eval/engine.h truth pass.** Stale claims: "Plan() lands in
  a later commit" (it shipped), AddComponent "Status: not yet
  implemented — m24 is at design stage" (m24 shipped 2026-06-04; e2e
  exists), Plan-side caching claim. Read: engine.{h,cc},
  engine_test.cc, m24-foreign-fn-component-backend.md,
  m21-host-call-adapter.md.
- [ ] **U02 eval/value.h stub-claims verification.** File comment says
  List/Map/Message builders are "signature-final stubs"; verify which
  bodies are real (value.cc + value_test.cc + e2e), update to truth.
- [ ] **U03 eval/error.h post-milestone semantics.** Each ErrorCode
  carrying an "until M<n>" note gets its *current* behavior stated
  (verify via tests/oracle; e.g. does kTypeUnsupported still surface?).

### P2 — Public API surface, Abseil-grade rewrite (one cluster per commit)

Every cluster: file banner, class contract (ownership, lifetime,
explicit thread-safety statement), per-method contract (inputs,
Status codes, REQUIRES), usage example where non-trivial, milestone
refs purged per Method §6.

- [ ] **U04 compiler/compiler.h + compiler/program.h.** Add Compiler
  thread-safety statement; delete "replaces an earlier draft"
  narration; purge M13 Slice C.2/C.3 refs.
- [ ] **U05 eval/value.h.** AsX() mismatch contract, backing-accessor
  ownership notes, StructurallyEquals vs 3VL equality note.
- [ ] **U06 eval/engine.h + eval/host_callback.h.** Per-method Status
  contracts, reserved-module-name cross-ref to runtime_catalogue.h,
  BindFunction example with a Value-returning lambda, callback
  registration-vs-invocation thread-safety.
- [ ] **U07 eval/instance.h + eval/activation.h.** Consolidate Eval()
  error-status list into the method contract; PartialEval semantics
  (read-time unknown stamping); BindLazy/OverrideFunction stub
  contracts kept honest.
- [ ] **U08 eval/attribute.h + eval/error.h.** Parse() error examples
  (trailing dot, empty segment, bare `*`), MatchType docs,
  kFunctionUnknownSentinel usage example, value-semantic thread-safety
  note.
- [ ] **U09 eval/host_call_context.h + eval/typed_function.h.**
  ReturnProto dangling warning sharpened, nested-unknown
  responsibility note, TypedFunction callback lifetime.
- [ ] **U10 shared/type.h.** Thread-safety (value-semantic, shared_ptr
  elements), kType binding contract.
- [ ] **U11 abi/cel_abi_emit.h + abi/runtime_catalogue.h.** BuildCelAbi
  return conditions; span process-lifetime notes; FindBuiltinHelper
  kCelFn REQUIRES up front.
- [ ] **U12 runtime/ public C headers** (cel_runtime.h, cel_data.h,
  cel_arena.h, cel_memory.h). Endianness one-liner (wasm is LE),
  alignment guarantee on arena_alloc, rodata-staging rationale on
  cel_mem_base. ABI doc must be self-sufficient without tribal
  knowledge.

### P3 — Internal headers (same standard, internal-banner added)

- [ ] **U13 compiler/frontend/** (parse_and_check.h, status_tags.h).
- [ ] **U14 compiler/ir/** (typed_ast.h, annotations.h — already
  excellent; purge M-refs, add "no shared state" note).
- [ ] **U15 compiler/codegen core** (resolve_pass.h, layout_pass.h).
- [ ] **U16 compiler/codegen lowering** (expr_lower.h,
  expr_lower_internal.h) — replace "M1 handles only kConst" with the
  actual current coverage statement (verify against code).
- [ ] **U17 compiler/codegen support** (module.h, overload_table.h,
  slot_allocator.h, static_memory_builder.h).
- [ ] **U18 compiler/celfn/** (function_library.h, library_module.h) —
  note the public-API-uses-internal-type wart → PROPOSALS.md.
- [ ] **U19 compiler/celfn/celfnc_emit/** (4 emitters + fixture) —
  m24/m26 §-refs become doc-path citations.
- [ ] **U20 compiler/internal/compile.h.**
- [ ] **U21 eval/internal/** (cel_host.h, abi_decode.h,
  cel_host_error.h, cel_component.h, wasmtime_engine_state.h,
  instance_impl.h) + eval/host/cel_log.h. Add per-Instance
  synchronization model note to HostMessageBacking.

### P4 — .cc internals + TODO normalization

- [ ] **U22 compiler .cc milestone-ref sweep** (PM chatter out,
  doc-path citations stay; stub-CHECK messages stay).
- [ ] **U23 eval/runtime/tools .cc sweep** (incl. wat_runner M-refs).
- [ ] **U24 TODO normalization repo-wide.** `TODO(m26)` →
  `TODO(<issue/owner>)`; promote the two informal "not yet emitted"
  notes to formal TODOs.

### P5 — Docs: architecture, archive, roadmap, consolidation

- [ ] **U25 doc/architecture/ tree.** New `ARCHITECTURE.md` (top-level:
  the four-noun pipeline, package dependency graph, trust boundaries)
  + `compiler.md`, `evaluator.md`, `runtime-and-abi.md` — written from
  code+tests per Method §1, *consolidating* (not duplicating) the
  current-truth content of design.md, memory-layout-design.md,
  cel-host-surface.md, two-phase-runtime-isolation.md,
  modules-and-fnis.md, m28-configurable-linking.md. Dry register.
- [ ] **U26 Diagram generator.** `doc/architecture/diagrams/render.py`
  (Python, graphviz/matplotlib — colorful per owner request), checked
  in with its SVG outputs: (1) compile→eval pipeline, (2) package
  dependency graph (from the verified edge list), (3) linear-memory
  layout, (4) sandbox/trust-boundary diagram. Replaces the ASCII art
  in README/user-guide where it improves flow.
- [ ] **U27 Archive batch 1.** Shipped milestone plans m1–m12 +
  map-list-dispatch, conformance-unlock, cross-numeric, dyn-passthrough,
  slice2 → `doc/implementation-plan/archive/` with status banners
  (Shipped date + pointer to the architecture page that supersedes
  them). Citation map consulted; cited docs get same-commit updates.
- [ ] **U28 Archive batch 2.** m13-probes, m14, m16–m18 (+probe
  findings), m20, m21, m23, m24, m28-wrapper-findings, abi-refactor,
  cel-runtime-c-split, phase-c-{design,plan}, wasi/milestones+reviews,
  phase-c-probes, language-feature-unlock, foreign-go-bindgen,
  repo-restructure trio, review docs, redirect stubs (5 stale ones
  deleted into archive). design.md → navigation-hub conversion with
  prominent banner.
- [ ] **U29 doc/roadmap.md.** Harvest every future-work item:
  LIVE-PLAN docs (m13 foreign backend, m22, m26, cel-cli-design,
  celfn-go-bindgen, phase-c-research), m28 §13 follow-up queue,
  cleanup-backlog P0/P1s, the 2026-06-09 assessment §5, code TODOs.
  Deduplicate; each surviving item actionable by a non-author. The
  ONLY forward-looking doc; harvested docs get banners or archive.
- [ ] **U30 Consolidation + flow pass.** doc/README.md becomes the one
  router (adopter path / contributor path). Merge duplicate tellings:
  compiler-overview.md vs user-guide/index.md overlap (one owns
  "orientation", the other owns "embedder how-to"); README quickstart
  vs getting-started.md (README teases, getting-started owns);
  bench/README vs benchmark/README — RESOLVED: bench/ dissolved into
  benchmark/ 2026-06-11, old README archived at
  doc/implementation-plan/rewrite/archive/bench-tree-readme.md. Add
  missing per-dir READMEs: tools/, e2e/, testdata/, spec/, scripts/
  (one-screen, purpose + how to run).
- [ ] **U31 Checklist-doc re-home.** testing-checklist,
  per-component-test-coverage, feature-pipeline-checklist,
  cleanup-backlog, lint-backlog get a contributor-docs entry point and
  a purpose line each; CLAUDE.md citations updated same-commit.

### P6 — Handoff

- [ ] **U32 DEVREL_BRIEF.md.** Verified facts only: what/who,
  differentiators with code/test/bench pointers, 2–4 hero examples
  (from `examples/`, already CI-gated), reproducible numbers
  (archived bench-tree-readme.md baselines + m28-bench-results, with
  exact commands),
  honest status, anti-claims list (e.g. "string-returning @component
  fns trap", "no `cel run` yet", "error messages don't cross the wasm
  boundary", "expression source is semi-trusted until backlog #16").
- [ ] **U33 Final summary.** Public API inventory (documented), docs
  map, TODO triage list, remaining debt, top-5 next steps.

## Citation map (docs that cannot move without same-commit updates)

Cited from **CLAUDE.md**: rewrite/design.md, langdef.md,
contributing.md, compiler-overview.md, memory-layout-design.md,
cel-host-surface.md, testing-checklist.md,
per-component-test-coverage.md, feature-pipeline-checklist.md,
wat-traces.md, wasi/DESIGN.md, lint-backlog.md, cleanup-backlog.md.

Cited from **C++ comments**: abi-refactor.md, cel-host-surface.md,
m13-custom-fns.md, m21-host-call-adapter.md,
m24-foreign-fn-component-backend.md, m28-configurable-linking.md,
m5-kcall-comprehensions.md, m7b-duration-timestamp.md,
map-list-dispatch.md, phase-c-plan.md,
two-phase-runtime-isolation.md, wasi/DESIGN.md.

Cited from **scripts/**: check_doc_drift.sh and
regen_conformance_readme.sh touch doc paths — re-check before each
move.

## Decisions of record

- Archive location is `doc/implementation-plan/archive/` (extends the
  existing `rewrite/archive/` convention) rather than the generic
  `doc/design/archive/` — minimizes citation churn.
- Phase 1 (style baseline) is pre-satisfied: `.clang-format`,
  `.clang-tidy`, lint.sh gate all exist. No mechanical formatting
  commit. CI wiring is an infrastructure addition → PROPOSALS.md.
- `wat-traces.md` stays in place (CLAUDE.md-mandated workflow doc +
  regression-tested by wat_runner_test).
- Conformance/bench READMEs are current and auto-regenerated — touch
  only for flow/linking, not content.
