# M8 — Conformance gate

Status: **planned.**  The last milestone; a gate rather than a
feature set.

## Scope

Run the compiler against every conformance fixture under
`tests/simple/testdata/` (the repo's inherited cel-spec tests) that
is valid in the **static subset**, and make them all pass.  Fixtures
that require `dyn`, heterogeneous literals, `Any` unpacking, or
other rejected features are skipped with an explicit allowlist.

No new features; only the glue to drive fixtures + a publishable
results report.

## Deliverables

### Harness

- [ ] `compiler/conformance/runner.{h,cc}` — reads a
      `simple.SimpleTest` textproto, uses `celwasmc` to compile the
      expression against the declared types + bindings, executes under
      wasmtime, and compares the returned `CelValue` against the
      fixture's `result` field.
- [ ] Value comparator that understands every CelKind (spec equality
      for timestamps is wall-clock, not byte-identical, etc.).
- [ ] Skip-list file at `compiler/conformance/skip.textproto`
      enumerating every fixture the static subset can't run, with a
      one-line reason.  A fixture that ever lands on the skip-list
      without a reason is a CI failure.
- [ ] Bazel test target running the full suite; split by shard so
      one flake doesn't red the bar.

### CI

- [ ] A reported run-summary: pass / skip / fail counts per fixture
      file; committed alongside the commit that makes the suite
      green.
- [ ] Regression gate: a new commit that drops a previously-passing
      fixture into the skip-list fails CI.  Mechanism: compare the
      new skip-list against the prior commit's.

### Docs

- [ ] `../wasm-compiler-design.md` §conformance — update to cite the
      runner + skip-list + current pass rate.  The README gets the
      headline number too.

## Testing obligations

M8 is itself the test.  The only new tests it writes are:

- [ ] `conformance/runner_test.cc` — smoke-tests that the runner
      correctly PASS / FAIL / SKIP a trivial fixture for each
      outcome.  A real fixture run is the production test.
- [ ] Skip-list hygiene test — every reason in the skip-list is
      non-empty and cites either a spec paragraph or a milestone
      tag (e.g. `SKIP: requires dyn (see design §3 non-goals)`).

## Open design questions

1. **Fixture path in the vendored tree.** `tests/simple/testdata/`
   is the Go-era layout.  The compiler's `conformance/` harness
   links against the textprotos by relative path; if cel-spec
   reshuffles the directory we need a single update point.
2. **What counts as "conformance" post-M8.** After M8, is every new
   feature expected to land with a corresponding fixture
   contribution upstream?  Likely yes — flag in the README.
