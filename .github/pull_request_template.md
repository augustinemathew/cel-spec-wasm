<!--
PR template, lifted from CLAUDE.md's "Closing out a planning
doc" and "Per-component test coverage" gates.  Delete sections
that don't apply (a one-line fix doesn't need the full grid).
For milestone closeouts, every box should be ticked or
explicitly N/A'd before merge.
-->

## Summary

<!-- 1-3 bullets: what changed and why.  Cite the milestone or
issue that motivated this. -->

## Test plan

<!-- Bulleted checklist of what was verified.  Include test
file paths so a reviewer can run them. -->

- [ ] `bazel test //compiler_v2/...` green.
- [ ] Manual-tagged tests run (list which: …).
- [ ] `scripts/lint.sh --branch` clean (full branch diff; bare
      `lint.sh` only checks working-tree edits).  Pre-existing-only is
      OK — list any remaining warnings + which lint-backlog entry
      tracks them.

## Conformance

<!-- Mandatory when touching codegen, runtime, or cel_host. -->

- [ ] `scripts/check_conformance_monotonic.sh` passes (PASS count
      ≥ baseline).
- [ ] If this PR ships a new feature: PASS count delta is
      _____ → _____ (+/− _____); update `.baseline` if monotonic.

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
- [ ] Sibling docs (`design.md`, `cel-host-surface.md`,
      milestone-N docs) reconciled if this PR invalidates them.
- [ ] `doc/implementation-plan/lint-backlog.md` reconciled —
      entries closed when fixed; new entries added for debt this
      PR introduces or surfaces.

## Risk & rollback

<!-- Describe what could go wrong and how to revert if needed. -->
