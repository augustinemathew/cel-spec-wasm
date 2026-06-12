# m30 — Differential fuzzing to the full CEL dialect

Status: in progress — drafted 2026-06-11, executing via the
autonomous loop (one slice per iteration, top-down).  **Shipped
2026-06-11: M30.A (adversarial leaves), M30.B (error-producing
arithmetic), M30.C nested-aggregate sub-part, M30.D total
string-function subset + math_ext + temporal accessors (incl
`_with_tz`) + conversions + encoders, M30.F (ergonomics + nightly CI),
M30.G name/activation cleanup.  Found + pinned five miscompiles and
three over-permissiveness divergences (inventory in
`e2e/fuzz/COVERAGE.md`).  Open: M30.C proto targets + temporal
arithmetic, M30.D conversion/two-arg-string remainder, M30.E
scale/width, and net_ext/optionals (blocked on a type-vocabulary
extension) — see §5.**  A 2026-06-11 subsystem review
(`reviews/2026-06-11-pbt-subsystem.md`) drove a docs reorg + the §5
work queue.  Supersedes the unshipped remainder of m27 (Slice C2 proto
targets, Slice D corpus/CI); m27's shipped machinery (typed attribute
grammar, L1/L2/L3 validation, oracle harness, fuzztest properties,
divergence miner) is the foundation this builds on.  Live status +
mining results: `e2e/fuzz/README.md` + `SESSIONS.md`.

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
- `e2e/fuzz/SESSIONS.md` (session journal) + `README.md` gap list
  updated in the same commit as the change it describes.
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
- proto messages (old m27 Slice C2): struct/select/has productions +
  a message-typed binding.  **Fixture requirement (user, 2026-06-11):**
  do NOT reuse the small `Customer`; author a deliberately adversarial
  test proto — **large, deeply nested, self-recursive** (a field of
  its own message type, so the grammar can recurse to arbitrary depth),
  with **every scalar field type** (int32/64, uint32/64, sint, fixed,
  sfixed, float, double, bool, string, bytes, enum), **repeated**
  variants of each, and **map** fields (varied key + value types incl.
  message values).  This is what stresses the field-access /
  struct-construction codegen: nesting depth, every wire type, repeated
  + map field marshalling, and recursion.  Needs `OracleVar` proto
  marshalling (the message-typed binding the oracle can't build today).
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

> **Inventory shipped 2026-06-11** as `e2e/fuzz/COVERAGE.md` — all
> 241 overloads from `compiler/codegen/overload_table.cc`, grouped
> by family, each ✅/🟡/⬜.  It is the master queue for the rest of
> M30: math_ext (28), net_ext (20), timestamp accessors (23),
> duration (7), encoders (2), optionals (~14), most conversions,
> string-rest, cross-type + ordering comparisons.  Check rows off
> as productions land + mine clean.

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

### M30.F — Ergonomics + CI (the "kick off a test in a minute" bar) — SHIPPED 2026-06-11

> `scripts/fuzz.sh` (validate / mine / sweep / repro / samples /
> kill) is the one entry point; `mine_divergences` prints a
> machine-readable `RESULT` line and exits non-zero (= divergence
> count) so it gates CI.  `.github/workflows/fuzz.yml` runs a
> nightly sweep over all 13 targets and fails on any divergence.
> Corpus persistence (committed repro seeds) remains future work.

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
L1/L2/L3 → mine → pin finds → fix or record → SESSIONS.md → commit.

## 4. Reconciliation

- `m27-pbt-cel-generator.md` header gains: "remaining Slice C2/D
  scope superseded by m30" (done in this commit).
- `e2e/fuzz/README.md` gap list now maps gaps → m30 milestones
  (done in this commit).
- m29.D1 (nightly fuzz job) is delivered by M30.F; m29 retains the
  pointer.
- `testing-checklist.md` gains a "Rewrite M30" section ticking the
  shipped slices (done 2026-06-11 in the review-driven docs reorg).
- The session journal moved from the README notes-log to
  `e2e/fuzz/SESSIONS.md` (2026-06-11); README is now topical reference.

## 5. Future work

Surfaced by the 2026-06-11 subsystem review
(`reviews/2026-06-11-pbt-subsystem.md`); ordered by bug-yield:

- **Conversion remainder (M30.D)** — numeric-shaped string leaves
  (`"42"`, `"3.14"`, `"  3.14  "`, `"+5"`) so `int/double/uint/bool(
  string)` exercise the *success* parse path, not only both-error;
  `duration(...)` / `timestamp(...)` parse leaves.
- **Exact `INT64_MIN` leaf** — unlocks `negate_int64` / `int(double)` /
  `math.abs(int)` overflow at the two's-complement boundary the catalog
  currently dodges by one.
- **`kBothErrored` error-kind comparison** — today "both errored" is
  unconditional agreement; comparing the cel-cpp error category would
  close the largest comparator blind spot (wrong-error-kind parity).
- **list/map/nested `FUZZ_TEST` registrations** — the in-process
  `bazel test` gate covers only the 6 scalar targets; aggregate codegen
  is gated only by the nightly sweep.
- **Two-arg pos/limit string forms** (`indexOf(sub,pos)`, `split(sep,n)`,
  `replace(old,new,n)`) — `indexOf(sub,pos)` resurfaces a known live
  codepoint-vs-byte bug.
- **`divide_double`, `add_list`, temporal `+`/`-` arithmetic** — three
  untested codegen arms; temporal arith exercises the overflow-error
  path the oracle is already configured for.
- **Regex-metacharacter + one large (>10 KiB) string leaf** — validate
  the `matches()` totality assumption and length-prefix paths.
- **Multi-entry map literals** — constructors are 1-entry only; width
  >1 reaches map iteration-order / duplicate-key codegen.
- **Code structure** — shared `targets.{h,cc}` registry (the mineable
  target list is hand-synced across `mine_divergences.cc`,
  `dump_samples.cc`, `fuzz.sh` and has drifted); `grammar_scalars.cc`
  family split; comparison/math table helpers; L3 walker → real
  generator; `%i` single-pass substitution.
- **Proto field access (M30.C)** — the largest *generative* gap not
  blocked on a type-vocabulary change (`CelType::kMessage` already
  exists).  Needs: an adversarial test proto (large / deeply nested /
  self-recursive / every scalar type + repeated + maps — see M30.C),
  message-leaf + `{field: …}` constructor + `.field` select + `has()`
  productions, and `OracleVar` proto marshalling for the message-typed
  binding.  User-requested 2026-06-11.
- **net_ext (20) + optionals (14)** — blocked on a `shared/type.h`
  opaque/optional type-vocabulary extension (a compiler change). The
  largest single coverage hole; schedule the type-vocab work.
