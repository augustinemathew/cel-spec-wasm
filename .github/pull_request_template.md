<!--
PR template for cel-wasm (CEL → WebAssembly AOT compiler), lifted from
CLAUDE.md's "Closing out a planning doc" + "Per-component test coverage"
gates.  Delete sections that don't apply (a one-line fix doesn't need the
full grid).  For milestone closeouts, every box should be ticked or
explicitly N/A'd before merge.

`$PROJ` below means the project-package set — Bazel `//...` is unusable in
this repo (vendored cel-cpp's tools/testdata loads an undeclared
`@com_github_google_flatbuffers`, so `//...` fails to *load*).  Use:

  //compiler/... //eval/... //shared/... //abi/... //runtime/... \
  //tools/... //conformance/... //e2e/... //bench/... //testdata/... //spec/...
-->

## Summary

<!-- 1-3 bullets: what changed and why.  Cite the milestone or
issue that motivated this. -->

## Test plan

<!-- Bulleted checklist of what was verified.  Include test
file paths so a reviewer can run them. -->

- [ ] `bazel test $PROJ` green (the project-package set above — never `//...`).
- [ ] Manual-tagged tests run — `scripts/run_full_suite.sh` (the canonical
      runner: `bazel test $PROJ` + every `manual` target + conformance).
      `bazel test` alone SKIPS the manual targets, which carry the
      load-bearing e2e/wasmtime assertions.  List which ran: …
- [ ] `scripts/lint.sh --branch` clean (full branch diff; bare `lint.sh`
      only checks working-tree edits).  Pre-existing-only is OK — list any
      remaining warnings + which `lint-backlog.md` entry tracks them.

## Conformance

<!-- Mandatory when touching codegen, runtime, or cel_host. -->

- [ ] `scripts/check_conformance_monotonic.sh` passes (PASS count ≥
      `conformance/.baseline`, currently 1898).
- [ ] If this PR ships a new feature: PASS count delta is
      _____ → _____ (+/− _____); update `conformance/.baseline` if monotonic.

## Visibility

<!-- Mandatory when adding/moving Bazel targets or deps.  The public/internal
boundary is enforced by `visibility` (CLAUDE.md "Visibility regime"). -->

- [ ] No new dependency edge onto another package's internal target from
      outside the first-party `//:internal` group.
- [ ] No target widened to `//visibility:public` — or, if so, it's a
      reviewed addition to the curated public API surface (`//compiler:compiler`,
      `//compiler:program`, `//eval:{engine,instance,activation,value,error,
      attribute}`, `//shared:type`, `//abi:*`, `//runtime:*`) and called out below.

## Doc reconciliation

<!-- Mandatory when this PR closes out a planning doc, ships a
new feature, or touches public surface. -->

- [ ] `scripts/check_doc_drift.sh` clean (or new findings listed
      + accounted for).
- [ ] Active milestone doc updated:
  - [ ] Status header flipped if shipping (`Status: shipped <date>`).
  - [ ] As-shipped delta noted if it diverges from the as-written plan.
  - [ ] Future-work section appended for anything surfaced but
        out of scope.
- [ ] `doc/implementation-plan/testing-checklist.md` rows ticked
      for components × pipeline stages touched.
- [ ] `doc/implementation-plan/per-component-test-coverage.md`
      rows ticked for any new TU + new test scenarios.
- [ ] Sibling docs (`rewrite/design.md`, `rewrite/cel-host-surface.md`,
      milestone-N docs) reconciled if this PR invalidates them.
- [ ] `doc/implementation-plan/lint-backlog.md` reconciled —
      entries closed when fixed; new entries added for debt this
      PR introduces or surfaces.

## Risk & rollback

<!-- Describe what could go wrong and how to revert if needed. -->
