# `ts/conformance/` — cross-host conformance for the TS eval host

Runs the upstream CEL conformance fixtures (`tests/simple/testdata/*.textproto`)
through the **TypeScript** eval host (`@celwasm/eval`: `Engine.create` →
`Program.fromBytes` → `engine.plan` → `instance.eval`) and **zero-diffs the
TS outcome against the C++ host's outcome**, row by row.

This is the TS analog of `compiler_v2/conformance/`, with one difference: it
doesn't grade against the spec matcher in isolation — **the C++ host is the
ground truth**. For every row both hosts evaluate, `ts_outcome` MUST equal
`cpp_outcome`; any disagreement fails the gate (m19 §5.6). The C++ compiler
also *produces* the Programs the TS host runs, so the two stay in lockstep:
`scripts/check_conformance_ts.sh` regenerates the fixtures from the current
C++ compiler+runner on every run (they are a build artifact, never
committed).

<!-- BEGIN headline (refresh from the gate output; see "Running") -->
```
corpus rows that compile (Programs exported):   1942
  cpp=pass:  1774   (ts pass 1341 · ts skip 433)
  cpp=fail:   130   (ts fail    7 · ts skip 123)
  cpp=skip:    38   (no C++ ground truth — excluded from the diff)

comparable (cpp ∈ {pass,fail}, ts ≠ skip):  agree=1348  diff=0   ✅
TS evaluates 1341 / 1774 = 75.6% of the C++ pass-set; the 433-row gap is
accounted for by reason below — all coverage, zero correctness diffs.
```
<!-- END headline -->

> The 512 corpus rows the C++ compiler rejects (static-subset / extension /
> `disable_check`) never emit a Program, so they aren't in the 1942 — they're
> accounted for by `compiler_v2/conformance/`, not here.

## Running

The gate regenerates fixtures from the current C++ and runs the TS host:

```sh
# Whole corpus (builds the C++ exporter + runtime wasm, regenerates
# fixtures into a temp dir, runs the TS zero-diff gate):
scripts/check_conformance_ts.sh

# A subset (faster dev loop) — files relative to tests/simple/testdata/:
scripts/check_conformance_ts.sh comparisons.textproto basic.textproto
```

It prints the confusion matrix + the coverage-gap-by-reason breakdown, then
asserts `diff == 0`. Run it wherever the C++ monotonic gate runs (CI /
pre-push).

Under the hood it sets two env vars and runs the vitest entry directly
(useful if you already have fixtures + a runtime wasm):

```sh
cd ts
CEL_TS_CONF_FIXTURES=<exporter out_dir> \
CEL_RUNTIME_WASM=<path/to/cel_runtime.wasm> \
  npm run test:conformance
```

## Outcome taxonomy

Each row classifies as exactly one of:

| Outcome | When | Diffs vs C++? |
|---|---|---|
| `pass` | Evaluated; the decoded `Value` matched the row's matcher (or an error matcher saw a CEL error). | Compared. |
| `fail` | Evaluated but the result didn't match — or an unexpected throw. | Compared — a `cpp≠ts` here fails the gate. |
| `skip` | Outside the current TS host subset (see the gap table). No TS verdict to diff. | Excluded. |

Only `cpp ∈ {pass, fail}` **and** `ts ≠ skip` rows are diffed — both sides
computed a verdict against the same matcher, so they must agree. `cpp=skip`
rows carry no ground truth; `ts=skip` rows the TS host hasn't implemented
yet. As the TS host grows (skip count → 0) the gate tightens toward full
parity with the 1774-row C++ pass-set.

## The `cpp=pass ts=skip` coverage gap (433), by reason

Every row C++ passes but the TS host skips, accounted for:

| Count | Reason | Disposition |
|---:|---|---|
| 361 | `eval-threw: TrampolineError` | A `cel_host.*` trampoline this milestone left as a stub: comprehension iteration (`cel_list_iter_open` / `cel_map_iter_open`), aggregate `in` / `==` / `concat`, message equality, message construction (`cel_make_message` / `cel_set_field`), WKT unwrap. **The single biggest bucket** — unlocked as those trampolines land (custom functions + Slice C+ host ops). |
| 27 | `eval-threw: CelDecodeError` | Result kind the codec doesn't decode yet (`CEL_TYPE` / `CEL_DURATION` / `CEL_TIMESTAMP` / `CEL_OPTIONAL`). Unlocked by extending the decoder to those kinds. |
| 20 | `scope: check-only / unknown / typed-result` | Out of the eval gate's scope by design (`check_only` / `unknown:` / `typed_result` rows) — same scope split the C++ harness draws. |
| 19 | `binding: objectValue` | A proto **message** passed as a `bindings:` value. Needs the binding decoder to build a message backing (a `TypeRegistry` + the row's message type). |
| 6 | `matcher: enumValue` | An `enum`-valued result matcher; the comparator doesn't compare enums yet. |

None of these is a correctness gap: the gate's `diff` stays **0**. They are
capabilities a later milestone graduates — overwhelmingly (83%) the
not-yet-implemented `cel_host.*` trampolines.

## How it stays in sync with C++

`scripts/check_conformance_ts.sh` documents the full contract; the short
version:

  - **Fixtures are regenerated every run** from the current C++ compiler +
    runner (`//compiler_v2/conformance:conformance_ts_export`), so a C++
    change that flips a row's outcome ships a fixture carrying the new
    `cpp_outcome`; the unchanged TS host then diffs → the gate fails → the
    TS side must be updated to match. Fixtures are never committed.
  - **The exporter walks the same corpus** the C++ gate does, so a new
    `tests/simple/testdata/*.textproto` row is exported automatically.

## Layout

| File | Role |
|---|---|
| `src/run.ts` | The runner: loads `index.jsonl` + `<n>.wasm`, builds an `Activation`, evals via the real `@celwasm/eval` library, compares the decoded `Value` to the matcher, tallies the confusion matrix + skip-gap. |
| `test/gate.test.ts` | The vitest entry: runs the corpus (fixtures + runtime wasm from env), prints the matrix + gap, asserts `diff == 0`. |

The runner drives the **production** library (not a private re-implementation),
so the aggregate / proto / list / map / arena rows are evaluated for real —
the same code paths the `ts/eval` unit + e2e suites cover.

## Future work

  - **Trampoline build-out** is the highest-leverage item — 361 of the 433
    skips are unimplemented `cel_host.*` ops (comprehension iteration,
    aggregate `in`/`==`/`concat`, message construction/equality, WKT
    unwrap). Each landed trampoline converts its rows from skip → comparable.
  - **Decode `CEL_TYPE` / `CEL_DURATION` / `CEL_TIMESTAMP` / `CEL_OPTIONAL`**
    results (27 rows).
  - **Message `bindings:`** (19 rows) — build a message backing from a
    `bindings:` `objectValue` via a `TypeRegistry`.
  - **Enum result matcher** in the comparator (6 rows).
  - **A monotonic gate** like `check_conformance_monotonic.sh`: pin the
    comparable-pass count (1348) so a regression — or a silent skip that
    should have stayed comparable — fails CI. Today the gate only asserts
    `diff == 0`.
