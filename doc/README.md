# `doc/` — documentation index

This is the navigable map of the `cel2` documentation tree. It splits
into two worlds:

1. **Upstream CEL spec mirrors** — `langdef.md`, `intro.md`,
   `extensions/`. These mirror the public cel-spec and describe the
   language we must honour. Do not rewrite them here; fix upstream.
2. **Project planning docs** — everything under `implementation-plan/`.
   The source of truth for the state of the `compiler/` CEL → WASM
   AOT compiler.

Status vocabulary used below: **shipped \<date\>** · **in-flight** ·
**plan/draft** (not started) · **analysis/research** (one-off report)
· **superseded** (kept for history, replaced by a newer doc).

> **Live numbers live in code, not here.** The authoritative
> conformance pass-count is in `conformance/README.md`
> (currently `pass=1774 (72.3%)`, `total=2454`). Pass-counts quoted in
> milestone docs are point-in-time snapshots from when that milestone
> shipped — treat them as historical, not current.

---

## Upstream CEL-spec mirrors (do not edit here)

| Doc | What it is |
| --- | --- |
| `langdef.md` | CEL language definition / reference (mirrors cel-spec). |
| `intro.md` | Gentle CEL introduction (mirrors cel-spec). |
| `extensions/strings.md` | `strings` extension spec (mirrors cel-spec). |
| `contributing.md` | Code-change workflow for the compiler. |

## Compiler orientation (in `compiler/`, not `doc/`)

| Doc | What it is |
| --- | --- |
| `../compiler/README.md` | Orientation page: layout, lifecycle, quickstart, CLI, knobs, perf. **Start here for the code.** |
| `../conformance/README.md` | Conformance harness + the **live** pass/skip/fail headline. |
| `../tools/cel/README.md` | The `cel` CLI (eval / check / compile). |
| `../bench/README.md` | Microbench + pipeline perf tables. |

---

## `implementation-plan/` — transverse docs

These span every milestone (not tied to one feature).

| Doc | Status | One-line |
| --- | --- | --- |
| `implementation-plan/README.md` | active | Source-of-truth overview + working rules for the rewrite. |
| `implementation-plan/testing-checklist.md` | active | Transverse CEL-type × AST-variant coverage grid; top-of-file gap summary. |
| `implementation-plan/per-component-test-coverage.md` | active | Per-component required scenarios + the milestone closeout gate. |
| `implementation-plan/known-issues-findings.md` | active (live log) | Bug-hunt findings log — appended to continuously. Do not reorganize. |
| `implementation-plan/dev-loop-performance.md` | active (2026-05) | Dev-loop / build-cache perf measurements. |
| `implementation-plan/lint-backlog.md` | active | Known clang-tidy / function-size exceedances tracked off the main gate. |
| `implementation-plan/cleanup-backlog.md` | active | P2 cleanup items from code-review passes, date-tagged. |
| `implementation-plan/design-pressure-test-prompt.md` | template | Reusable design-review prompt. |

---

## `implementation-plan/rewrite/` — design + milestones

### Master design

| Doc | Status | One-line |
| --- | --- | --- |
| `rewrite/design.md` | in-flight (S1–S6,S8 shipped; S7/S9–S12 pending) | The master memory-layout / symbol-table / codegen design. The §"Phase C delta" callouts describe the **as-shipped** memory model and override the original drafting. |
| `rewrite/cel-host-surface.md` | shipped (2026-04-22, reconciled 04-24) | The `cel_host` import surface; three-layer split. |
| `rewrite/feature-pipeline-checklist.md` | active reference | Which files/tests/ABI to touch (and in what order) when adding a feature. |
| `rewrite/cel-runtime-c-split-plan.md` | shipped 2026-05-14 (P1–P8; P9 punted) | Split of `cel_runtime.c` into per-topic `.c` files. |
| `rewrite/abi-refactor.md` | shipped 2026-05-22 | Single source of truth for `cel_runtime` + `cel_host` + `cel_fn` imports. |
| `rewrite/two-phase-runtime-isolation.md` | shipped 2026-04-22 | Two-phase instantiation restoring the wasm sandbox. |

### Milestones (m1 … m17)

Numbering note: there is no public `m6` — the original "M6 custom
functions" plan (`m-custom-fns.md`) was renumbered to **M13** and is
superseded. `m15` was never used. `m11` is the only in-flight
milestone here.

| Doc | Status | One-line |
| --- | --- | --- |
| `rewrite/m1-scalar-pipeline.md` | shipped 2026-04-22 | Scalar-literal pipeline + full abstraction skeleton. |
| `rewrite/m2-ident-select-unknowns.md` | shipped 2026-04-25 | Idents, proto field reads, Activation, unknowns. |
| `rewrite/m3-map-literals.md` | shipped 2026-04-24 | Map literals + indexing (tail-call dispatch). |
| `rewrite/m4-list-literals.md` | shipped 2026-04-25 | List literals + indexing (replays M3 map shape). |
| `rewrite/m5-comprehensions-design.md` | design (2026-05-16/17) | Comprehension design ("how shaped"). |
| `rewrite/m5-kcall-comprehensions.md` | shipped 2026-04-25 | kCall built-in overload set. |
| `rewrite/m5-comprehensions-followon.md` | shipped 2026-05-17 | Comprehensions + `cel.bind` ("what to ship"). |
| `rewrite/m5b-comprehensions-simplification.md` | analysis (2026-05-17) | Simplification analysis; read with the follow-on doc. |
| `rewrite/m7-proto-literals.md` | shipped 2026-04-25 | Proto message literals. |
| `rewrite/m7a-any.md` | shipped 2026-05-16 | `google.protobuf.Any` pack/unpack. |
| `rewrite/m7b-duration-timestamp.md` | shipped 2026-05-16 | Timestamp + Duration. |
| `rewrite/m8-wrapper-types.md` | shipped 2026-05-17 | Wrapper types. |
| `rewrite/m9-type-subsystem.md` | shipped 2026-05-14 | `type(x)` + type identifiers as values. |
| `rewrite/m10-conversions.md` | shipped 2026-05-14 | Type conversions (`int(x)`, `string(x)`, …). |
| `rewrite/m11-cel-host-refactor.md` | **in-flight** (Slices A+E shipped 2026-05-19; B/C/D/F–I pending) | `cel_host` refactor + WKT peel-to-runtime + test coverage. |
| `rewrite/m12-string-ext.md` | shipped 2026-05-20 | `string_ext` extension (self-hosted in runtime). |
| `rewrite/m13-custom-fns.md` | plan (drafted 2026-05-21) | User-defined custom functions. Supersedes `m-custom-fns.md`. |
| `rewrite/m13-probes.md` | in-flight (2026-05-21) | Custom-function probes; companion to m13. |
| `rewrite/m14-optionals.md` | shipped 2026-05-22 | CEL optionals. |
| `rewrite/m16-math-ext.md` | shipped 2026-05-24 | `math_ext` extension (self-hosted). |
| `rewrite/m16-ast-probe-findings.md` | research (2026-05-24) | Checked-AST shape probe for math_ext. |
| `rewrite/m17-encoders-ext.md` | **shipped 2026-05-24** (current milestone) | `encoders` extension (`base64.encode`/`decode`). |
| `rewrite/m-custom-fns.md` | **superseded** by m13 | Old "M6" custom-fns draft (pre-rewrite numbering). |

### Feature / sub-feature plans (shipped, folded into design.md)

| Doc | Status | One-line |
| --- | --- | --- |
| `rewrite/map-list-dispatch.md` | reconciled into design.md (2026-04-25) | Arena/host/dynamic map+list dispatch. |
| `rewrite/slice2-control-flow-plan.md` | shipped 2026-04-25 | Control flow + 3VL (`&&`/`||`/`?:`/`!`). |
| `rewrite/dyn-passthrough-plan.md` | shipped 2026-04-25 | `dyn(...)` passthrough (Slice 1.5). |
| `rewrite/cross-numeric-ordering-plan.md` | shipped 2026-04-25 | Cross-numeric ordering / membership kernel. |
| `rewrite/conformance-unlock-plan.md` | shipped 2026-04-25 | Post-M5.D conformance-unlock slices. |
| `rewrite/language-feature-unlock-analysis.md` | analysis (2026-05-16) | Forward-looking feature-unlock analysis (contains stale headline counts — see top-of-index note). |

### Phase C (in-runtime parsers + RE2 / libraries)

Phase C is **shipped** (RE2 + absl::time/cctz vendored into the
runtime; see `wasi/DESIGN.md` and `design.md` §"Phase C delta"). The
plan/design docs below still carry their drafting-time "in
flight"/"drafted" headers — see the FLAGGED note at the bottom of this
index.

| Doc | Status | One-line |
| --- | --- | --- |
| `rewrite/phase-c-research.md` | plan/research (2026-05-18) | Research + prototyping handoff prompt. |
| `rewrite/phase-c-design.md` | header says "in flight" — **work is shipped** (flagged) | In-runtime parsers + RE2 / `matches()` design. |
| `rewrite/phase-c-plan.md` | header says "drafted" — **work is shipped** (flagged) | Implementation plan from probes E1–E10. |
| `rewrite/phase-c-probes/E1…E10/` | research artifacts | Probe sources + `RESULT.md` per probe. |

### WASI / malloc migration (`rewrite/wasi/`)

| Doc | Status | One-line |
| --- | --- | --- |
| `rewrite/wasi/README.md` | pointer | Points at DESIGN.md. |
| `rewrite/wasi/DESIGN.md` | shipped 2026-05-18 (M1–M7 + B1–B6; Phase C later) | The malloc-migration design + work plan; **authoritative** for the shared-memory + malloc-arena shape. §4–§5 are the memory-model reference. |
| `rewrite/wasi/milestones/M1.md … M9.md`, `M6-M8.md` | shipped | Per-milestone WASI notes. |
| `rewrite/wasi/POST_MIGRATION_BENCH.md` | shipped | Post-migration benchmark. |
| `rewrite/wasi/reviews/2026-05-18-mvp-shipped.md` | review | MVP-shipped review. |
| `rewrite/wasi/experiments/` | experiments | `exp_a..exp_e` allocator/library probes. Do **not** delete (see the `CLAUDE_Do_NOT_DELETE…` marker in that dir). |

### WAT traces (`rewrite/wat/` + `wat-traces.md`)

| Doc | Status | One-line |
| --- | --- | --- |
| `rewrite/wat/*.wat` + `wat/BUILD.bazel` | active | The maintained, assembled+run codegen-arm trace set (one `.wat` per expression shape). |
| `rewrite/wat-traces.md` | **memory map SUPERSEDED** (banner at top) | Narrative companion to the traces. Codegen-arm shapes still illustrative; memory offsets / bytes-8/12 cursor are pre-Phase-C and stale. |

### Reviews

| Doc | What it is |
| --- | --- |
| `rewrite/reviews/2026-05-2x-m14-*.md` | M14 slice review records. |
| `rewrite/m13-reviews/2026-05-21-*.md` | M13 slice-C review records. |

### Superseded predecessor designs (kept for history)

| Doc | Superseded by |
| --- | --- |
| `rewrite/predecessor-m-mem-static-layout-pass.md` | `design.md` (folded as `LayoutPass`), 2026-04-21. |
| `rewrite/predecessor-memory-ownership-flip.md` | `design.md` / `two-phase-runtime-isolation.md`, 2026-04-21. |

---

## FLAGGED — possible drift, NOT changed (verify before acting)

- **`phase-c-design.md` / `phase-c-plan.md` status headers.** They say
  "in flight, started 2026-05-18" / "drafted 2026-05-18", but Phase C
  is shipped per `wasi/DESIGN.md` ("Phase C shipped later (RE2 +
  absl::time / cctz vendored)") and the `design.md` §"Phase C delta
  (shipped)" callouts. Left as-is — closing these out is a doc-owner
  decision, not a mechanical fix.

- **"min ~4 pages" in `design.md` (line ~17).** The narrative says the
  observed runtime memory is `(memory 4 1024 shared)` "min ~4 pages",
  while the *enforced* host floor is `CELWASM_INITIAL_MEMORY_PAGES = 2`
  (`runtime/cel_layout.h:29`). design.md already reconciles
  this inline ("the host's A13 invariant only enforces a `>= … = 2`
  floor", and `cel_layout.h:18-28` explains the auto-sized 2→3-4 page
  drift by build mode). No code or doc change needed — the "4" is an
  observed value, the "2" is the contract floor; they are consistent.

- **Stale conformance pass-counts in milestone docs.** Each shipped
  milestone records the headline count *at ship time* (e.g.
  `language-feature-unlock-analysis.md:10` `pass=1144`,
  `m10-conversions.md:60` `pass=975`, `m5-comprehensions-followon.md`
  `1373`). These are legitimate historical snapshots, not errors —
  left in place. The single live number is in
  `conformance/README.md`.
