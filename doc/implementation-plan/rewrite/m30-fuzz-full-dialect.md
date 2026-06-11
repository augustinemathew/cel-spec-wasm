# m30 — Differential fuzzing to the full CEL dialect

Status: in progress — drafted 2026-06-11, executing via the
autonomous loop (one slice per iteration, top-down).  **M30.A
(adversarial leaves), M30.B (error-producing arithmetic), M30.C's
nested-aggregate sub-part, and M30.D's total string-function subset
shipped 2026-06-11; M30.G's name/activation cleanup landed with the
reviewability refactor.  M30.D found and fixed the first real
miscompile — a `split` slot-aliasing bug on computed receivers.**  Supersedes the unshipped remainder of
m27 (Slice C2 proto targets, Slice D corpus/CI); m27's shipped
machinery (typed attribute grammar, L1/L2/L3 validation, oracle
harness, fuzztest properties, divergence miner) is the foundation
this builds on.  Live status + mining results: `e2e/fuzz/README.md`.

## 0. Objective

An end-to-end fuzz rig that **exposes structural weaknesses** in
the compiler/runtime, not just exercises the happy path:

1. **The grammar speaks the full CEL dialect** the compiler accepts
   (every type, every operator family, comprehensions, errors,
   unknowns) — today it speaks a small guarded-total subset.
2. **Inputs are adversarial**: boundary numerics (`INT64_MAX`,
   2^53±1, epsilon, denormals, −0.0), unicode/embedded-NUL strings,
   deep nesting, wide bodies — the places bugs actually live.
3. **The tooling is ergonomic**: one command to mine, one obvious
   recipe to add a new surface, findings that pin themselves as
   tests.  A new contributor kicks off a useful fuzz session in
   under a minute from the `e2e/fuzz/README.md` quickstart.

Non-goals: `dyn` (the compiler rejects it by design — fuzzing it
tests the gate, which conformance already covers); performance
fuzzing; coverage-guided byte-level fuzzing of the parser (m27
§"Why simple fuzzing won't work" still holds).

## 1. Ground rules (every slice)

- The discovery/pinning contract is unchanged: every `DIVERGE` /
  `ERROR-DIVERGE` becomes a `TEST(KnownBugs, Pbt…)` row in
  `e2e/known_bugs_test.cc` — exact shrunk source, spec-correct
  assertion, seed in the comment — in the same commit that records
  the find.  Fix follows (same or next slice); the skip is deleted
  with the fix.
- Grammar changes re-run L1/L2/L3 (`//e2e/fuzz:grammar_test`)
  before any mining.  A catalog change that breaks L2 never mines.
- `e2e/fuzz/README.md` notes log + gap list updated in the same
  commit as the change it describes.
- Oracle disagreements about *configuration* (budgets, options) are
  resolved in `testdata/cel_cpp_oracle.cc` with a comment, never by
  loosening the comparison.

## 2. Milestones

### M30.A — Adversarial leaf domains (boundary numerics + unicode) — SHIPPED 2026-06-11 (8236374)

The cheapest yield: the grammar's shapes are good, its *constants*
are tame (`0/1/7`, ASCII `"hello"`).  Add leaf productions:

- int: `9223372036854775807`, `-9223372036854775807`,
  `9007199254740993` / `9007199254740992` (the 2^53 lossy-double
  boundary — `KnownBugs.MapKeyLossyDoubleEquality` lives here),
  `4294967296` (2^32).
- uint: `18446744073709551615u` (UINT64_MAX),
  `9223372036854775808u` (2^63, > INT64_MAX).
- double: `-0.0`, `2.220446049250313e-16` (epsilon), `5e-324`
  (min denormal), `1e308` / `-1e308` (inf via one mul),
  `9007199254740992.0` (2^53).
- string: `"ÿ"` (2-byte), `"πέντε"` (multi-2-byte), `"💜"`
  (4-byte), `"a\u0000b"` (embedded NUL), a combining-mark
  sequence.  `size()`/`indexOf` codepoint-vs-byte bugs were all
  found manually in exactly this territory.
- bytes: `b"\x00"`, `b"\xff\xfe"` (invalid UTF-8 — legal bytes).

Arithmetic over boundary ints/uints overflows; that is the POINT —
the error-ness classification (shipped, e9ab2fc) makes overflow a
compared dimension: both-error = agreement, our-value-while-oracle-
errors = an `ERROR-DIVERGE` find (e.g. a kernel that silently
wraps).  Exit: L2/L3 green over the expanded leaves; ≥10k-seed
sweeps at depths 6–9 across all 11 targets; every find pinned.

### M30.B — Error semantics + unknowns as first-class properties

- **Int/uint `/` and `%` shipped 2026-06-11.** Both sides agree on
  error-ness (messages stay uncompared, backlog #31); the 3VL
  absorption matrix is now reachable.  Remaining error producers
  (unbounded list index, fallible `int(string)`) land with the
  string-ext calls in M30.D.
- Unknowns: the oracle's `PartialEvalWithCelCpp` already takes
  unknown patterns and ours has `PartialEval`; add a property that
  marks a random activation subset unknown and asserts
  unknown-ness/value agreement.  New harness entry point
  (`GenAndPartialEval`), new outcome states.

### M30.C — Full type vocabulary

- timestamp/duration: literal leaves (`timestamp("…")`,
  `duration("…")`), total comparisons, guarded arithmetic; the
  max-range construction bug class was found manually here.
- `optional<T>`: `optional.of`/`ofNonZeroValue`/`.orValue` within
  the static subset (`.?` is blocked by the known
  `OptionalSelectOnMapRejected` gate — pin rides until fixed).
- proto messages (old m27 Slice C2): `celwasm.testdata.Customer`
  struct/select/has productions + a message-typed binding.
- **Nested aggregates shipped 2026-06-11** — `list<list<T>>`,
  `list<map<K,V>>`, `map<string,list<int>>` (`RegisterNestedAggregates`);
  the recursive comparator (e9ab2fc) handled the verdict, so only
  grammar productions were needed.  0 divergences mining
  `list_list_int` / `map_string_list_int` at d4.

### M30.D — Full operator surface ("speak the whole dialect")

> **Total string subset shipped 2026-06-11.** `contains` /
> `startsWith` / `endsWith` / `indexOf(sub)` / `matches` / `split`
> added; the oracle gained the strings checker + runtime extension
> (it only had standard + optional before, so it was rejecting
> `split`/`indexOf` our compiler accepts).  This surface found the
> first real miscompile: `split` on a computed receiver
> (`('a'+'b').split(sep)`) returned garbage because the output slot
> aliased the input and `DoSplit` re-read the source pointer after
> `AllocList` clobbered it — fixed in `cel_string_ext_list.cc`,
> pinned kernel + e2e.  Still open: fallible string forms
> (`substring`, two-arg `indexOf(sub, pos)`, `replace`, `format`),
> string/bytes ordering, the generated coverage table.

Enumerate the checker's builtin + shipped-extension overload
catalog (`compiler/celfn/overload_table.cc` is the seed list)
against the grammar; admit everything the static subset accepts:
string/bytes ordering, `contains`/`startsWith`/`endsWith`/
`indexOf`/`substring`/`replace`/`split`/`format`, `matches()`,
remaining conversions, `size` overloads, `in` over every container
shape, string-ext/math-ext functions.  Deliverable includes a
**generated coverage table** (grammar productions ÷ overload
catalog, % admitted) in the README so "full dialect" is a measured
claim, not a vibe — the same discipline as the conformance badge.

### M30.E — Scale and shape (depth, width, randomization)

- Width: arity-20/50 list literals, long `+`/`&&` chains via a
  width-biased production set; raise `kMaxSourceBytes` with a
  measured eval-latency budget.
- Depth: mine the 9–12 band; capacity-rejects feed the m29
  static-window workstream with rate curves per depth.
- Randomization: per-seed production-subset swarm testing (classic
  swarm fuzzing — each seed enables a random grammar subset,
  surfacing interaction bugs uniform sampling dilutes); weight
  tuning from divergence-rich runs.

### M30.F — Ergonomics + CI (the "kick off a test in a minute" bar)

- `scripts/fuzz.sh` — the one entry point: `fuzz.sh mine [target|all]
  [--depth N] [--seeds N]`, `fuzz.sh sweep` (all targets × depth
  band, JSON+text summary), `fuzz.sh repro <seed> <target> <depth>`
  (re-run one seed, print both sides).
- Miner upgrades to serve it: `all` pseudo-target, machine-readable
  summary line, exit code = #diverges (CI-gateable).
- Nightly CI job (extends `.github/workflows/ci.yml`): sweep with a
  fixed time budget; any DIVERGE fails the job (m29.D1 lands here).
- Corpus: `testdata/pbt_corpus/` for committed repro sources
  (find-by-seed breaks whenever the grammar changes — the corpus
  pins sources, not seeds).
- README quickstart rewritten around `fuzz.sh`.

### M30.G — Grammar usability (the "usable grammar" bar)

- One registration file per family (`grammar_scalars.cc`,
  `grammar_aggregates.cc`, `grammar_strings_ext.cc`, …) replacing
  the slice-lettered files; `grammar_slice_b/c` names retire.
- An `AddFamily` recipe documented in the README: add productions +
  leaves, run `grammar_test`, mine — with L2 auto-covering every
  new production by construction (already true; document it as the
  contract).
- The activation table becomes the single source of truth consumed
  by both the grammar (ident leaves, scope) and the harness
  (bindings) — today they are two hand-synced lists
  (`SliceBActivation` vs `SliceBBoundActivation`); the drift broke
  the build once already (e9ab2fc commit message).

## 3. Sequencing and the loop

A→B→C→D are ordered by bug-yield-per-line; E/F/G interleave when a
mining session goes dry (no fresh finds for two consecutive
sweeps).  Each loop iteration ships one slice-step: implement →
L1/L2/L3 → mine → pin finds → fix or record → notes log → commit.

## 4. Reconciliation

- `m27-pbt-cel-generator.md` header gains: "remaining Slice C2/D
  scope superseded by m30" (done in this commit).
- `e2e/fuzz/README.md` gap list now maps gaps → m30 milestones
  (done in this commit).
- m29.D1 (nightly fuzz job) is delivered by M30.F; m29 retains the
  pointer.

## 5. Future work

(Collected as slices land.)
