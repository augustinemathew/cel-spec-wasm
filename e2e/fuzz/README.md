# `e2e/fuzz/` — differential fuzzing of the compiler against cel-cpp

This folder catches miscompiles before users do. A typed attribute
grammar generates CEL source that **type-checks by construction**;
every sample evaluates through BOTH our Compile → Plan → Eval pipeline
and the real cel-cpp evaluator; any disagreement — value, error-ness,
or acceptance — is a finding, never parser noise. Plan of record:
[`m30-fuzz-full-dialect.md`](../../doc/implementation-plan/rewrite/m30-fuzz-full-dialect.md)
(architecture rationale in the closed
[`m27-pbt-cel-generator.md`](../../doc/implementation-plan/rewrite/m27-pbt-cel-generator.md)).

This README is the topical operational reference: how the rig works,
how to run it, how to extend it. Update it in the same commit as any
grammar or harness change. The dated session journal lives separately
in [`SESSIONS.md`](SESSIONS.md) (append there, not here).

> **In-flight refactor:** the folder is being simplified per
> [`SIMPLIFY.md`](SIMPLIFY.md) (one comparator, one verdict path,
> catalogs regrouped by family). That doc carries the checklists;
> file names below may shift as its steps land.

**Function coverage is tracked in [`COVERAGE.md`](COVERAGE.md)** — a
checklist of all 241 overloads from
`compiler/codegen/overload_table.cc` and whether the grammar
generates each. Check a row off when you add its production AND mine
its target clean. The remaining ⬜ rows (net_ext, optionals,
`type()`, and the conversion / two-arg-string remainder) are the
queue; net_ext + optionals are blocked on a `shared/type.h`
type-vocabulary extension.

## How it works

```
 ActivationSchema()            Grammar (the production catalog)
 grammar_scalars.h             grammar_scalars.cc   — constants (incl. boundary/
 name → CelType for every      grammar_aggregates.cc  unicode leaves), idents,
 bound variable; the ONE       │                      arithmetic, comparisons,
 list both sides consume       │  validated by        logic, ternary, lists/maps,
        │                      │  grammar_test.cc     size, _in_, comprehensions
        │                      ▼  (L1+L2+L3, below)
        │              GenerateExpr(target, seed, depth)     generator.cc
        │                      │
        │                      ▼
        │              one CEL source string, e.g.
        │              ([xs]).exists(v, (v + i_a) > 9007199254740992)
        │                      │
        ▼                      ▼
 oracle_harness.cc :: GenAndEvalFull(target, seed, depth)
        │                                   │
        ▼                                   ▼
 OURS: Compile → Plan → Eval     ORACLE: cel-cpp parse/check/eval
 (the production pipeline)       (testdata/cel_cpp_oracle.cc)
        │                                   │
        └───────────────┬───────────────────┘
                        ▼
        compare.cc :: Compare(ours, oracle, target)
        recursive, type-driven; NaN-agreement; map = key-set
                        │
                        ▼
   verdict: agreed · DIVERGE · ERROR-DIVERGE · both-errored ·
            our-reject · oracle-reject · too-large
```

Two drivers share that harness:

- **`mine_divergences`** — sequential-seed CLI; prints every anomaly
  with its seed + source, summary line at the end. The daily tool.
- **`cel_oracle_property_test`** — fuzztest properties (one per
  target type, depth domain 0..8) with shrinking; `manual`-tagged
  because finding a bug fails the test, which is its job.

**Why a typed grammar?** Random bytes die in the lexer; shape-driven
generation wastes half its budget on type-check rejects. Here every
emitted source reaches codegen and the runtime, so all the budget
lands where the bugs are.

**Why you can trust a failure.** `grammar_test.cc` runs three
validation layers before any mining: **L1** structural checks
(`Grammar::Validate()` — every type has a leaf, placeholders are
consistent), **L2** every single production round-tripped through the
real cel-cpp checker, **L3** sampled compositions at depths {1,3,6}.
All three green ⇒ a divergence is necessarily a compiler/runtime bug,
not a generator misconfiguration.

## Running a session

`scripts/fuzz.sh` is the one entry point — it builds the miner, kills
stray miner processes (long sweeps compete for CPU and skew timings),
and exits non-zero on a divergence so it gates CI directly:

```bash
scripts/fuzz.sh validate                 # L1/L2/L3 grammar checks (run first)
scripts/fuzz.sh mine bool 2000 6         # one target: seeds, depth
scripts/fuzz.sh sweep 500 6              # every target; fails if any diverges
scripts/fuzz.sh repro string 113 4       # re-run one seed, print source + both sides
scripts/fuzz.sh samples bool 4 10        # eyeball what the grammar emits
scripts/fuzz.sh kill                     # kill stray miners
```

The miner prints a `--- summary ---` block plus a machine-readable
`RESULT target=… diverged=…` line; its **exit code is the divergence
count** (0 = clean). The raw targets are also runnable directly:

```bash
bazel test //e2e/fuzz:grammar_test                         # the L1/L2/L3 suite
bazel run //e2e/fuzz:mine_divergences -- list_int 1000 8 3 # target seeds depth [stop_after]
bazel test //e2e/fuzz:cel_oracle_property_test             # fuzztest mode (shrinking)
```

Targets (13; the authoritative list is `ParseTarget` in
`mine_divergences.cc`): `bool int uint double string bytes list_int
list_bool list_double list_string map_string_int list_list_int
map_string_list_int`.

Reading the summary line:

| Field | Meaning | Act on it? |
| --- | --- | --- |
| `agreed` | both sides evaluated, values match | no — the baseline |
| `diverged` | value mismatch, or we produced a value where cel-cpp errored (`ERROR-DIVERGE`) | **yes — pin it (see below)** |
| `our_rejected` | our Compile/Plan/Eval refused | `ResourceExhausted` = the known static-window capacity ceiling (rate climbs with depth); anything else = a bug |
| `oracle_rejected` | cel-cpp refused a source we accepted | yes — systematic cases are bugs |
| `both_errored` | both sides produced a CEL error | no — agreement (error *messages* aren't compared; backlog #31) |
| `too_large` | source exceeded `kMaxSourceBytes` (4 KiB) | no — generation cap |

## The bug loop

1. **Mine** until a `DIVERGE` / `ERROR-DIVERGE` appears.
2. **Pin**: add a `TEST(KnownBugs, Pbt…)` row to
   [`e2e/known_bugs_test.cc`](../known_bugs_test.cc) — the exact
   source, the spec-correct assertion, the seed in the comment,
   `GTEST_SKIP` until fixed. The fuzz failure *emits a test*; the
   test is the deliverable even before the fix.
3. **Fix** at the right layer (`layout_pass`, `expr_lower`, the C
   kernel…); delete the skip in the same commit; add the focused
   unit test beside the fix.
4. **Extend**: a clean run means the grammar is the suspect — add
   the next surface from the m30 queue and mine again.

## Adding a surface (the extension contract)

1. Productions/leaves go in `grammar_scalars.cc` (scalar families)
   or `grammar_aggregates.cc` (container families) via the
   `GrammarBuilder` shorthands (`Leaf`/`Unary`/`Binary`/`Ternary`/
   `Repeated`/`Comprehension`).
2. A new bound variable is TWO edits, enforced loudly:
   `ActivationSchema()` (grammar side) and `MakeEntry()` in
   `oracle_harness.cc` (its value, both representations) — a schema
   entry without a value CHECK-fails at first use, so the lists
   cannot drift silently.
3. `bazel test //e2e/fuzz:grammar_test` — L2 auto-covers every new
   production by construction; you write no new validation code.
4. Mine. Record the session in [`SESSIONS.md`](SESSIONS.md).

Error-producing productions (division, fallible conversions) are
admissible: error-ness is a compared dimension, so "both error" is
agreement and "only cel-cpp errors" is a find.

## Trophy case (the method works)

- **#32/#33** — `exists_one` comprehension result stamped from the
  wrong slot (`layout_pass.cc`); found at depth 6, 3 pins
  (`PbtExistsOne*`), fixed.
- **`EmitConditional` local-vs-offset confusion** — ternary inside
  int subtract (seed 3696381601904611693, depth 4); pinned as
  `PbtTernaryInsideIntSubtract`, fixed.
- **#34** — depth-7/8 reliably hit the old fixed 64 KiB arena cliff;
  PBT proved the cliff reachable from ordinary nested expressions.
- **cel-cpp comprehension budget interaction** (2026-06-11) — a
  generated ~11k-iteration `exists_one` nest tripped cel-cpp's 10k
  DoS guard; oracle budget raised, and the production observation
  (cel-wasm has NO eval iteration cap) feeds m29.

## Gap list — what the grammar cannot find yet

> Scheduled as [`m30-fuzz-full-dialect.md`](../../doc/implementation-plan/rewrite/m30-fuzz-full-dialect.md):
> #2 → M30.B, #6/#7/#8 → M30.C, #5 → M30.D, #0b-d/#1 → M30.E.
> This list stays the evidence record; the m30 doc is the order.

Each entry names a bug class found *manually* in territory the
grammar couldn't reach — PBT would have found it first if the
productions existed.

0. **Large / wide expressions** — ~~bound aggregates~~ (`xs`/`ms`
   shipped, e9ab2fc). Still open: width (arity caps at 10), long
   chains, comprehensions over larger ranges, the 4 KiB source cap.
1. **Depth > 8** — depth 7–8 opened 2026-06-11 (0 divergences; the
   static window is the limiter — rejection ~1% at d7, ~8% at d8 for
   bool). The 9–12 band is unmined.
2. **Error-producing productions** — ~~no `/`, `%`~~ int/uint `/`
   and `%` shipped (M30.B): division/modulo by zero errors, both
   engines agree (`both_errored`), and an error reaching a value
   slot is an `ERROR-DIVERGE` find. Still open: unbounded list
   index, fallible `int(string)`-style conversions (those land
   with M30.D's string-ext calls).
3. ~~**Boundary-value leaves**~~ — shipped (M30.A, 8236374): INT64
   boundaries, 2^53±1, UINT64_MAX, −0.0/epsilon/denormal/1e308.
4. ~~**Multi-byte UTF-8 leaves**~~ — shipped (M30.A, 8236374):
   2/3/4-byte, embedded NUL, combining mark; invalid-UTF-8 bytes.
5. **String-ext + conversion calls** — total subset shipped
   (M30.D): `contains`/`startsWith`/`endsWith`/`indexOf(sub)`/
   `matches`/`split`. Still open (fallible, pairs with the known
   codepoint bugs): `substring`, two-arg `indexOf(sub, pos)`
   (`IndexOfPosBoundIsByteNotCodepoint`), `replace`, `format`,
   `int(string)` with leading-`+`/whitespace shapes.
6. ~~**Nested aggregates**~~ — shipped (M30.C): `list<list<T>>`,
   `list<map<K,V>>`, `map<string,list<int>>` leaves + constructors
   + size + container-iter_var comprehensions. The comparator
   already recursed; only grammar productions were needed.
7. **Timestamp / duration** — absent entirely; the max-range
   construction bug class was found manually.
8. **Proto struct/select** — `celwasm.testdata.Customer` productions
   + a message-typed binding (needs OracleVar proto marshalling).

## Session history

Session-by-session mining history — which surface each session added,
what mined clean, and the bugs surfaced — lives in
[`SESSIONS.md`](SESSIONS.md). Append a dated entry there per session;
keep this README topical.
