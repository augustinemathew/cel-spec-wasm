# `benchmark/` — design

**Status:** shipped — the system this doc designs is built and was
used for the production m28 three-way run.  **Current numbers live in
`doc/implementation-plan/rewrite/m28-bench-results.md`** (232-cell
corpus, geomean 0.95×, honest win/loss tables); the §1.4 prototype
table below is the 2026-06-06 first-light snapshot and is superseded —
do not quote it.

Canonical spec for the comparative benchmarking system layered on top
of the existing `bench/` tree.  Implementation proceeds against this
doc; design deltas land here in the same commit as the code.

---

## 1. Context

### 1.1  The original framing (revised after prototype)

The original thesis — *"AOT-compiled CEL is materially faster than
tree-walking interpretation"* — turned out to be **wrong for short
expressions** and **not yet measured for long ones**.  See §1.4 for
what the prototype showed.

### 1.2  Revised primary goal

Publish defensible per-operator reference numbers — for celwasm and
cel-cpp on identical expressions, identical activations, identical
hardware — showing:

- **Where celwasm is faster** (long expressions; the AOT story).
- **Where celwasm is slower** (short expressions; the per-Eval
  setup story).
- **The crossover length** per operator (when does AOT start to win).
- **Slope and intercept** per operator (per-op marginal cost +
  per-Eval fixed cost) so an embedder can predict their workload.

These reference numbers are the load-bearing artifact.  Whether
celwasm "wins" any particular workload is for the embedder's data,
not us, to decide.

### 1.3  Secondary goals

- CI regression detection so performance changes block merges the
  same way correctness changes do.
- The system extends to future surfaces (comprehensions when M11
  ships, time, proto-heavy) without rewiring.

### 1.4  What the prototype showed (2026-06-06)

Five hand-coded arithmetic cells, both backends parity-verified
(same numerical result), Apple M-series @ ~3 GHz, `-c opt`:

| cell | expression | celwasm ns | cel-cpp ns | ratio |
|---|---|---:|---:|---:|
| intAdd2 | `a + b` | 260 | 73 | **cel-cpp 3.6× faster** |
| intAdd3 | `a + b + c` | 357 | 113 | **cel-cpp 3.2× faster** |
| intMul2 | `a * b` | 260 | 79 | **cel-cpp 3.3× faster** |
| intAdd10Terms | `a + b + … + j` | 994 | 350 | **cel-cpp 2.8× faster** |
| doubleMul2 | `x * y` | 260 | 76 | **cel-cpp 3.4× faster** |

Decomposing T(N) = setup + N · per_op:

- **cel-cpp:** ~50 ns setup + ~35 ns/op
- **celwasm:** ~165 ns setup + ~92 ns/op

celwasm pays ~115 ns more in fixed per-Eval setup (wasmtime entry,
activation marshal, arena reset) and ~60 ns more per operation
(over-counted at low N because per-arg marshal aliases into "per-op"
in this measurement; real kernel-level per-op cost from `bench/
BM_IntAdd` is 2.3 ns).

The crossover length where celwasm pulls ahead is **not yet
measured**.  The corpus is missing cells > 10 terms.  Until we
extend the corpus, the published numbers can only honestly say
"celwasm is slower for 2-10 ops."

### 1.5  What changes in this design as a result

- **Length sweep is promoted to first-class methodology** (§6.4).
  Per-operator cells at `{2, 10, 50, 250, 1000}` terms minimum.
- **The reporter emits slope + intercept** per operator, not
  per-cell ratios alone (§12).  Both numbers matter; neither alone
  tells the story.
- **Native floor column dropped** from the design as a published
  artifact (§13).  It's an open methodology question (how to fairly
  measure "what hand-written C++ doing the same arithmetic would
  cost" without trivial constant-folding) and we don't need it to
  publish a defensible celwasm-vs-cel-cpp comparison.  Native
  floor stays available as an ad-hoc sanity check, but not as a
  reporter column.
- **Adversarial framing (§4) extended** with the setup-vs-per-op
  decomposition as a defended methodology rather than a fairness
  ambiguity.

## 2. Non-goals

- **Not** a replacement for `bench/`.  That tree's kernel µbenches
  + pipeline shape probes remain for celwasm-vs-itself regression
  localisation.
- **Not** a custom benchmarking framework.  **Google Benchmark**
  does iteration scaling, variance reporting, JSON output, and
  `tools/compare.py` for diffing runs.  We use it.  We do not
  re-implement it.
- **Not** a profiling tool.  We **invoke** profilers (`samply` on
  macOS, `perf` on Linux) on individual cells.
- **Not** a fairness manifesto.  This system makes implementation
  trade-offs *visible*, it does not adjudicate them.

## 3. Stakeholders

| stakeholder | what they consume | what they need |
|---|---|---|
| embedder evaluating celwasm | published comparison table | defendable numbers; methodology that survives review |
| celwasm maintainer | per-PR regression alerts + flame graphs | fast triage from "this cell regressed" → "which kernel" |
| Google CEL team reviewer | methodology + reproducibility checklist | confidence numbers reflect actual cel-cpp behaviour |
| future surface owner | corpus schema | extensible enough to add a surface without forking |

---

## 4. Adversarial framing

Every way the numbers could be wrong or misleading — both in our
favour and against — and how the system defends.

### 4.1  Ways we could falsely appear faster

| failure mode | defence |
|---|---|
| celwasm and cel-cpp evaluate different expressions due to parse drift | `parity_check` binary asserts results agree before timing |
| celwasm pre-evaluates a constant; cel-cpp re-evaluates per call | corpus pins activation-vs-literal explicitly; "all-literal" cells run separately |
| celwasm Compile excluded from timing, cel-cpp Compile included | matched phase semantics in the wrappers: `BuildPlan` does the offline work, Eval is the timed loop body |
| celwasm iteration loop hoists work cel-cpp can't | identical loop shape via `for (auto _ : state) { plan.Eval(); }` in both binaries — no per-comparator hoisting |
| harness pre-touches memory cel-cpp's tree doesn't | both wrappers use identical warmup convention (Google Benchmark default) |

### 4.2  Ways we could falsely appear slower

| failure mode | defence |
|---|---|
| celwasm pays wasm trampoline overhead embedders amortise | corpus tag distinguishes "per-eval Instance reconstruction" from "Instance reused across N evals"; both published |
| Cranelift compilation included in Eval timing | `BuildPlan` runs Plan ONCE outside the timed loop; `state.PauseTiming()` not needed because the loop body is pure call |
| celwasm arena reset cost paid per eval; cel-cpp uses external arena | per-eval reset is the correct semantic (`arena_reset` is internal to celwasm Eval); cel-cpp's external arena lifetime documented in its wrapper |
| cel-cpp built with `-O3` while celwasm sticks at `-O2` | both built with same `-c opt`; flags captured in run header |

### 4.3  Spurious variance

| failure mode | defence |
|---|---|
| thermal throttling | `--benchmark_min_warmup_time=1s`; runner refuses to start if CPU governor isn't `performance` (Linux) |
| sibling-process noise | local runs warn if `loadavg > 0.3`; CI uses dedicated runner |
| timer resolution at sub-µs | Google Benchmark iteration auto-scaling + `--benchmark_min_time=0.5s` floor |
| compiler version drift | both pinned to same host clang; version captured in run header |

`run.sh` enforces these checks before invoking the binaries.  Runs
that fail tag the JSON `environment_warning` rather than refusing —
warning-not-block lets local debug runs through.

---

## 5. Architecture

### 5.1  Data flow

```
corpus/*.yaml ──┐
                │
                ▼
        ┌───────────────┐         ┌───────────────┐
        │ celwasm_bench │         │ celcpp_bench  │   ← TWO binaries
        │ (reads YAML   │         │ (same YAML,   │   ← linkage-isolated
        │  at startup,  │         │  separate TU) │   ← (cel-cpp symbol clash)
        │  Register-    │         │               │
        │  Benchmark    │         │               │
        │  per cell)    │         │               │
        └───────┬───────┘         └───────┬───────┘
                │                         │
                ▼                         ▼
        Google Benchmark JSON output (--benchmark_out_format=json)
                │                         │
                └────────────┬────────────┘
                             ▼
                         report.py
                  (joins by stripped suffix,
                   pivots into per-cell rows,
                   emits Markdown + CSV)
                             │
                             ▼
                  results/<date>.md
                  results/<date>.csv
                  (committed)
```

Plus one auxiliary binary (`parity_check`) that reads the same YAML
and links both wrappers in a special test-mode build that tolerates
the symbol clash (different linker config) — runs once before timing.

### 5.2  Component layout

```
benchmark/
├── DESIGN.md
├── README.md
├── compiler/
│   └── TODO.md
└── eval/
    ├── BUILD.bazel
    ├── corpus/
    │   ├── OPERATORS.md
    │   ├── arithmetic.yaml
    │   ├── comparisons.yaml
    │   ├── booleans.yaml
    │   └── strings.yaml
    ├── corpus_loader.{h,cc}       ← yaml-cpp → in-mem Cell vector
    ├── corpus_loader_test.cc
    ├── comparators/
    │   ├── celwasm_wrapper.{h,cc}
    │   ├── celcpp_wrapper.{h,cc}  ← standalone TU; no first-party deps
    │   └── BUILD.bazel
    ├── celwasm_bench_main.cc      ← celwasm timing binary
    ├── celcpp_bench_main.cc       ← cel-cpp timing binary
    ├── parity_check_main.cc       ← one-shot result-equality assertion
    ├── run.sh                     ← end-to-end: build → parity → time → report → open MD
    ├── profile.sh                 ← wraps single cell in samply / perf
    ├── report.py                  ← Google Benchmark JSON → Markdown table
    ├── report_test.py
    └── results/                   ← committed JSON + Markdown
```

### 5.3  Why no codegen

Earlier draft proposed `gen.py` → emitted `.cc` files via a bazel
genrule.  Dropped after pushback: Google Benchmark's
`RegisterBenchmark(name, fn)` is the dynamic-registration API
exactly for this case.

| was (codegen) | now (dynamic) |
|---|---|
| `gen.py` ~150 LOC | gone |
| `gen/` genrule output | gone |
| bazel genrule | gone — straight `cc_binary` |
| Python build dep | gone |
| edit YAML → re-run gen → rebuild | edit YAML → rebuild (yaml-cpp parses at startup) |
| 8 generated `.cc` (1 per surface × per comparator) | **2 hand-written `_main.cc`** files (1 per comparator) |
| generated source is what's reviewable | **the source IS the loop body — directly in `_main.cc`, nothing hidden** |

`report.py` stays Python because it's *post-run* tabular pivoting
(no build dep, no codegen, just JSON → Markdown).  Trivially
swappable.

### 5.4  File ownership and lifecycle

| file | who edits | how often |
|---|---|---|
| `corpus/*.yaml` | anyone adding a cell | per corpus expansion |
| `corpus_loader.cc` | platform maintainer | rarely; stable contract |
| `comparators/celwasm_wrapper.cc` | follows celwasm public API | per Engine-public-API change |
| `comparators/celcpp_wrapper.cc` | follows vendored cel-cpp | per cel-cpp version bump |
| `*_main.cc` | platform maintainer | rarely; loops over corpus |
| `report.py`, `run.sh`, `profile.sh` | platform maintainer | rarely |
| `results/*` | runner writes; humans commit | every published run |

---

## 6. The corpus

### 6.1  Operator-exhaustiveness mandate

**Every single operator in the CEL surface MUST have at least one
corpus cell.**  Not "representative samples" — exhaustive.

Full enumeration lives in `eval/corpus/OPERATORS.md` (the single
source of truth).  Total: **~220 operators** across the language
surface.  Minimum coverage: each op at `{1, 3, 10}` length × `linear`
variant = **~660 cells**.

### 6.2  YAML schema

One file per surface.  One entry per cell.  **The expression is the
primary identifier humans care about.**  The `id` field is just a
short handle for filter/join — pick whatever's readable.

```yaml
schema_version: 1
surface: arithmetic

cells:
  - id: intAdd3                    # short, free-form; only must be unique within this file
    source: "a + b + c"            # THE expression — the load-bearing display
    activation:
      a: { type: int, value: 1 }
      b: { type: int, value: 2 }
      c: { type: int, value: 3 }
    expected: { type: int, value: 6 }
    tags: [phase1, adoption-core]  # optional, for filtering

  - id: intAddManyTerms
    source: "a + b + c + d + e + f + g + h + i + j"
    activation: { …each var… }
    expected: { type: int, value: 55 }

  - id: intAddDeepTree
    source: "((a + b) + (c + d)) + ((e + f) + (g + h))"
    activation: { …each var… }
    expected: { type: int, value: 36 }
```

`type`, `length`, `variant` are dropped from the schema —
**they were duplicating information the expression already carries**.
A reviewer reading the table wants to see `a + b + c` next to the
ratio, not `arith.int.add.linear.3`.

### 6.3  Validation (corpus_loader.cc enforces)

| rule | rationale |
|---|---|
| `(surface, id)` pair unique across all corpus files | timings join on this pair |
| `surface` matches the file basename | catches mis-filed cells |
| `id` is non-empty, no whitespace, no `/` | safe for `--benchmark_filter` regex |
| every var in `source` appears in `activation` | catches under-bound activations |
| every var in `activation` appears in `source` | over-bound activations inflate marshal cost |
| `expected.type` matches the checker-inferred static type | catches expected-value typos |

Violations are hard errors at startup.  The binary refuses to run
benchmarks until the corpus is clean.

### 6.4  Methodology — every cell answers a specific question

The corpus is **not** "let's add a lot of patterns and see what
happens."  Each cell exists to answer a specific question about
performance, and the doc records which question.  Cells whose
purpose can't be articulated do not belong in the corpus.

Adding a Python script that mass-generates cells without
methodology produces low-signal noise — the kind of bench you read
once and learn nothing actionable from.  The two failure modes:
either you bench too few cells and miss real regressions, or you
bench many cells but can't tell which difference matters.  Both
are avoided by naming the axes the cells move along and
requiring each cell to declare which axis it's a sample on.

#### 6.4.1  Axes of variation

Every cell sits at a point in this space.  Explicit names so we can
talk about coverage without ambiguity:

| axis | values |
|---|---|
| **operator** | every CEL surface op: `+`, `-`, `*`, `/`, `%`, `<`, `<=`, `==`, `!=`, `&&`, `||`, `!`, `?:`, `[]`, `.field`, `in`, `.contains`, `.startsWith`, `.endsWith`, `.matches`, `.size`, comprehensions (`exists`, `all`, `filter`, `map`), `has()`, casts (`int(…)`, `string(…)`, …), arithmetic-with-overflow boundaries.  Enumerated in `corpus/OPERATORS.md`. |
| **operand type** | `int`, `uint`, `double`, `bool`, `string`, `bytes`, `list<T>`, `map<K,V>`, `null`, message types, cross-type pairs (`int == double`, `string + bytes` etc. per langdef). |
| **operand source** | `literal` (baked into AST → rodata at compile time), `variable` (bound by host via Activation), `computed` (output of a prior op in the same expression). |
| **cardinality (N)** | how many of the same operator are chained: 2, 10, 50, 250, 1000.  Used to extract per-op slope vs per-Eval intercept (§6.4.2). |
| **selectivity** | for short-circuit / search ops, *where* the match lives: first / middle / last / never.  Determines best-case vs worst-case spread. |
| **payload size** | for variable-size types (string, bytes, list, map): 8 B, 64 B, 4 KB, 64 KB.  Surfaces SIMD effects, cache effects, allocation cost. |
| **boundary value** | per type: `INT64_MIN`, `INT64_MAX`, `UINT64_MAX`, 0, -1, empty string, single-char, embedded NUL, max-UTF-8, single-byte-multi-codepoint, empty list, single-element list, max-cardinality list. |
| **activation pressure** | none / few-short / many-short / few-long / many-long.  Isolates marshaling cost from eval cost. |

#### 6.4.2  Cell families — coordinated cells that produce a derived number

Single cells answer "how fast is this one input?" — useful but small.
**Families** are sets of sibling cells that differ on exactly one
axis, designed so the bench report can compute a derived quantity.
Every operator should be exercised by at least the families it can
fit into.

**Length sweep family** (the slope/intercept decomposition):
Cells at N ∈ {2, 10, 50, 250, 1000} for the same operator + types.
T(N) = setup + N · per_op via linear regression.  Lower than 2
leaves no slope; higher than 1000 stops adding information once
per-op cost stabilises.

- **setup** (intercept) is what an embedder pays for the *privilege*
  of evaluating any CEL expression, regardless of complexity.
- **per_op** (slope) is what each additional operation costs.
- The ratio of slopes is the asymptotic AOT advantage; the ratio of
  intercepts is the per-Eval overhead penalty.

Naming: `<op><n>Terms` for n>2, bare `<op>` for n=2.  Example:
`intAdd2`, `intAdd10Terms`, `intAdd50Terms`, `intAdd250Terms`,
`intAdd1000Terms`.

**Selectivity family**:
Same shape, different match position.  `a in [list]` cells at
position 1 / middle / last / never; `&&`-chains at fail-first /
fail-middle / never-fail.  Tells us the best-case vs worst-case
cost — separated, not averaged.  Naming: `<op>_<pos>` e.g.
`inList20_first`, `inList20_middle`, `inList20_last`,
`inList20_never`.

**Payload-size family** (for variable-size types):
Same operator, same shape, sweeping size of the variable-size
operand.  String eq at 8 B / 64 B / 4 KB / 64 KB; list `.size` at
10 / 100 / 1000 elements.  Tells us per-byte / per-element cost
once per-call cost is amortised.  Surfaces effects (or lack
thereof) of SIMD, memcmp implementation, allocation policy.
Naming: `<op>_L<bytes>` e.g. `strEq_L64`, `strEq_L4K`,
`strEq_L64K`.

**Boundary-value family**:
Explicit cells for every boundary listed in §6.4.1's boundary-value
row.  Per operator that touches that type.  These cells are NOT
about performance per se — they're about catching the case where
the boundary value triggers a slow path (e.g., overflow check
fires, NULL handling diverges).  Documented as such; reviewers
treat unexpected slowdowns at boundaries as findings.  Naming:
`<op>_<boundary>` e.g. `intAdd_INT64MAX`, `strEq_emptyEmpty`,
`strEq_UTF8MaxCodepoint`, `listSize_empty`, `listSize_one`.

**Embedder-canonical family**:
Real-world CEL shapes that real adopters write.  Not synthetic.
Examples and the use case each represents:

| cell name | source pattern | embedder use case |
|---|---|---|
| `iamAllowlist_member` | `request.principal in [const list]` | IAM allow-list |
| `iamAllowlist_nonmember` | same, principal not in list | IAM deny path |
| `featureFlag_country` | `request.country in ["US","CA",…]` | feature gating |
| `featureFlag_userAgent` | `request.ua.startsWith("Mozilla")` | UA-based routing |
| `logFilter_combined` | `r.level == "ERROR" && r.service.startsWith("api")` | log filtering |
| `schemaCheck_required` | `has(msg.required_field) && msg.required_field.size() > 0` | schema validation |
| `pricingTier_lookup` | `{"free": 0, "pro": 10}[user.tier]` | pricing table |

The synthetic 1000-term arithmetic chain is a measurement
instrument; the embedder-canonical cells are the actual product
benchmark — what an adopter looks at to decide if this is fast
enough for their workload.

#### 6.4.3  Sibling rule

Every cell must share its axis values with at least one other cell,
differing on exactly one axis.  This makes the bench report
diff-able: any cell can be compared against a sibling to attribute
a perf delta to a specific axis change.

A cell that doesn't have a sibling on any axis is either
duplicative (already covered by an existing cell + family) or
under-specified (we don't know what it's a sample of).  Either way
it doesn't belong.

#### 6.4.4  Cell rationale required

Every cell carries a `purpose:` field in the YAML, one or two
sentences naming the axis and the question it answers.  Examples:

```yaml
- id: intAdd1000Terms
  source: "a + b + c + ... + j (1000 terms cyclic over 10 vars)"
  purpose: "Length-sweep family member at N=1000.  Establishes the
           asymptotic per-op cost for int addition once per-Eval
           setup is amortised."

- id: inList20_never
  source: "a in [\"alice\", ..., \"tom\"]  ;; 20 elements"
  purpose: "Selectivity family member at position=never.  Forces
           full-list scan; worst case for `in`."

- id: strEq_L64K
  source: "a == \"<64KB string>\""
  purpose: "Payload-size family member at 64 KB.  Tells us per-byte
           equality cost (memcmp throughput) with per-call cost
           amortised."

- id: iamAllowlist_nonmember
  source: "request.principal in [\"admin\",\"auditor\",\"sre\"]"
  purpose: "Embedder-canonical family.  Models IAM deny path,
           where principal is not in the allow-list."
```

If a cell can't articulate its purpose without restating its
source, it's noise.  Drop it.

#### 6.4.5  What this rules out

- **Python YAML generators that produce many cells without naming
  the axis they're varying.**  Generating "intAdd at N=3,4,5,…,200"
  is mass-production without purpose — the slope is already
  characterised by the {2,10,50,250,1000} family; the
  intermediate points add no information.
- **Cells that share BOTH the source and the activation with another
  cell**, accidentally produced by a generator iterating over a
  matrix whose axes collapse to the same expression.
- **One-off cells with no sibling** — usually a sign they were added
  to chase a single number rather than answer a question.
- **Cells whose stated purpose is "see if this is fast"** — that's
  not a question, that's a vibe.  Replace with the actual axis +
  expected comparison.

A reviewer asked "why is this cell here?" must get a one-sentence
answer from the YAML alone.  If they have to read git history,
the cell isn't paying its rent.

### 6.4.6  As-shipped corpus delta (2026-06-09)

> Plan-vs-execution delta: the operator×type grid landed as
> **13 surface files, 229 cells** — `arithmetic`, `comparisons`,
> `comprehensions`, `conversions`, `index`, `lists`, `logic`,
> `long_strings`, `maps`, `size`, `strings`, `ternary`, `time`
> (the planned `booleans.yaml` shipped as `logic.yaml` +
> `ternary.yaml`).  Coverage state, per-cell skip tags
> (`celwasm-skip-*`, `celcpp-skip-*`), and the correctness findings
> the corpus surfaced (ternary-ident-cond null bug, dynamic-mode
> silent rodata miscompare, heterogeneous-equality checker gaps)
> are tracked in `corpus/OPERATORS.md` — that file, not this
> section, is the authoritative coverage ledger.  Activation values
> remain scalar-only (corpus_loader.h); aggregate-/time-typed
> operands are constructed in-source and scalar-reduced
> (`size(…)`, `int(…)`, `.getSeconds()`), which keeps both
> comparators' timed work identical.

### 6.5  Phase 1 corpus

- **arithmetic.yaml**: 25 ops × `{2, 10, 50, 250, 1000}` lengths ≈
  125 cells.  Per-op slope + setup intercept extractable for every
  operator.
- **comparisons.yaml**: 40 ops × same lengths ≈ 200 cells.  Chained
  comparison (`a < b && b < c …`) is the natural long-form variant.
- **booleans.yaml**: 4 ops (`&&`, `||`, `!`, ternary) × `{2, 10, 50}`
  × short-circuit-early / no-early-exit variants ≈ 25 cells.
- **strings.yaml**: 10 ops × payload size `{8B, 64B, 4K, 64K}` ≈ 40
  cells.  String surface uses payload size as the scaling axis, not
  operation count (the operation IS the I/O).

Phase 1 total: ~390 cells × 2 comparators = ~780 BMs.  At
`--benchmark_min_time=0.5s`, ~25-35 min on a dedicated runner.

The 1000-term cells matter even though no real workload uses them:
they're how we measure the **asymptotic per-op cost** without
setup pollution.  Embedders use the slope to predict their own
workload, not the 1000-term cell directly.

---

## 7. Corpus loader (`corpus_loader.{h,cc}`)

A small cc_library wrapping yaml-cpp.

```cpp
namespace celbench {

struct Activation {
  std::string name;
  Value value;
};

struct Cell {
  std::string surface;  // from the YAML file
  std::string id;       // free-form short handle
  std::string source;   // THE expression — load-bearing
  std::vector<Activation> activation;
  Value expected;
  std::vector<std::string> tags;
};

// Loads + validates + cross-checks IDs across all listed paths.
// Hard-fails on any validation error.  Returns Cells in stable
// order (alphabetical by id) so the registration order is
// deterministic for run-to-run diffability.
absl::StatusOr<std::vector<Cell>> LoadCorpus(
    absl::Span<const std::string> yaml_paths);

}  // namespace celbench
```

yaml-cpp via bazel `http_archive`.  ~200 KB build addition; standard
dep, no transitive surprises.

---

## 8. Comparator wrappers

### 8.1  Surface (per comparator)

```cpp
namespace celwasm_wrapper {  // or celcpp_wrapper

class Plan {
 public:
  Value Eval();
  // Counters exposed via state.counters[…].  cel-cpp wrapper
  // returns 0 / -1 for celwasm-specific counters.
  int64_t LastArenaBytes() const;
  int64_t LastHostCalls() const;
};

Plan BuildPlan(absl::string_view source,
               absl::Span<const Activation> activation);

// Canonical-form serialiser — used by parity_check.
std::string CanonicalForm(const Value&);

}  // namespace
```

celwasm wrapper: Compiler::Builder → Compile → Engine::Plan →
Instance::Eval (the standard public-API path).

cel-cpp wrapper: Parse → Check → Runtime::CreateProgram →
program->Evaluate.  In its own cc_library with `linkstatic = True`,
**zero first-party deps** — same trick as
`bench/in_operator_cel_cpp_bench.cc` already uses.

### 8.2  No comparator registry at runtime

Each `_main.cc` knows its comparator at compile time.  No virtual
dispatch, no abstract base class.  The wrappers happen to expose
the same surface; that's a convention, not a contract.  Keeps
the inner loop branch-free.

---

## 9. Bench main (`celwasm_bench_main.cc`)

The whole binary in ~80 lines.  This is THE source of truth for
what gets run — no codegen, no hidden expansion.

```cpp
#include "benchmark/benchmark.h"
#include "benchmark/eval/comparators/celwasm_wrapper.h"
#include "benchmark/eval/corpus_loader.h"
#include "absl/log/absl_check.h"
#include <string>
#include <vector>

namespace celbench {

void RegisterCell(const Cell& cell) {
  // Capture by value into the lambda; cell vector outlives the
  // benchmarks (kept in a function-local static, see main).
  // BM name = "BM_" + surface + "_" + id so --benchmark_filter
  // works as a surface selector AND a per-cell selector.
  std::string bm_name = "BM_" + cell.surface + "_" + cell.id;
  benchmark::RegisterBenchmark(
      bm_name.c_str(),
      [cell](benchmark::State& state) {
        celwasm_wrapper::Plan plan = celwasm_wrapper::BuildPlan(
            cell.source, cell.activation);
        for (auto _ : state) {
          auto v = plan.Eval();
          benchmark::DoNotOptimize(v);
        }
        state.counters["arena_bytes"] =
            static_cast<double>(plan.LastArenaBytes());
        state.counters["host_calls"] =
            static_cast<double>(plan.LastHostCalls());
      })->Unit(benchmark::kNanosecond);
}

}  // namespace celbench

int main(int argc, char** argv) {
  // Static so lambdas can capture cells by const-ref or copy safely.
  static const std::vector<celbench::Cell> kCells = [] {
    auto cells = celbench::LoadCorpus({
        "benchmark/eval/corpus/arithmetic.yaml",
        "benchmark/eval/corpus/comparisons.yaml",
        "benchmark/eval/corpus/booleans.yaml",
        "benchmark/eval/corpus/strings.yaml",
    });
    ABSL_CHECK_OK(cells);
    return *std::move(cells);
  }();

  for (const auto& cell : kCells) {
    celbench::RegisterCell(cell);
  }

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
```

`celcpp_bench_main.cc` is identical in shape; only the included
wrapper differs.

**That is the whole compile-time surface for both binaries.**  No
templates, no macros, no codegen.  Read the file, know what runs.

### 9.1  How `--benchmark_filter` partitions runs

The user partitions runs at runtime via Google Benchmark's filter
flag — same mechanism that picks one BM out of thousands in any
Google Benchmark binary:

```bash
# Just arithmetic cells:
bazel-bin/benchmark/eval/celwasm_bench \
  --benchmark_filter='^BM_arithmetic_'

# Just one cell:
bazel-bin/benchmark/eval/celwasm_bench \
  --benchmark_filter='^BM_arithmetic_intAdd3$'

# Cells whose id matches a regex (e.g. all the "intAdd*" variants):
bazel-bin/benchmark/eval/celwasm_bench \
  --benchmark_filter='BM_arithmetic_intAdd'
```

For tag-based runtime filtering, the binary appends `_<tag>` to
the registered BM name for each tag the cell carries — so
`--benchmark_filter='_adoption-core$'` selects the adoption-core
subset.  Cells with no tags get no suffix.

---

## 10. Iteration and timing

Google Benchmark handles all of this.  We use its defaults plus:

| flag | publish | smoke (PR) |
|---|---|---|
| `--benchmark_min_time` | `0.5s` | `0.1s` |
| `--benchmark_repetitions` | `5` | `3` |
| `--benchmark_report_aggregates_only` | `true` | `true` |
| `--benchmark_out_format` | `json` | `json` |
| `--benchmark_out` | `results/raw/<date>.json` | `/tmp/smoke.json` |
| `--benchmark_enable_random_interleaving` | `true` | `true` |

Per-BM structure:

```
BuildPlan(source, activation)         ← outside timing (lambda capture)
for (auto _ : state) {
  auto v = plan.Eval();               ← only this
  DoNotOptimize(v);
}
state.counters["arena_bytes"] = …      ← post-hot
```

No custom variance handling — Google Benchmark's `cv` (coefficient
of variation) column is the truth.  report.py tags `cv > 0.10` as
`(noisy)`.

---

## 11. Parity check (`parity_check_main.cc`)

Separate binary that links **both** wrappers under a build config
that uses the explicit-symbol-isolation linker flags (the
`linkstatic` shape from `bench/in_operator_cel_cpp_bench.cc`'s
notes).  Run once before any timing.

```cpp
int main(int argc, char** argv) {
  auto cells = celbench::LoadCorpus({…});
  ABSL_CHECK_OK(cells);
  int failures = 0;
  for (const auto& cell : *cells) {
    auto cw = celwasm_wrapper::BuildPlan(cell.source, cell.activation);
    auto cc = celcpp_wrapper::BuildPlan(cell.source, cell.activation);
    auto cw_form = celwasm_wrapper::CanonicalForm(cw.Eval());
    auto cc_form = celcpp_wrapper::CanonicalForm(cc.Eval());
    auto expected = celbench::CanonicalForm(cell.expected);
    if (cw_form != expected || cc_form != expected) {
      std::cerr << "PARITY FAIL " << cell.id
                << "\n  expected: " << expected
                << "\n  celwasm:  " << cw_form
                << "\n  cel-cpp:  " << cc_form << "\n";
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
```

`run.sh` runs `parity_check` first.  Any failing cells get
appended to `--benchmark_filter='-<failing-id>'` for the timing
binaries.  Timing proceeds for the rest.

Decoupled: a parity mismatch on one cell does not block others
from publishing numbers.

---

## 12. Reporter (`report.py`)

~200 LOC Python script.  Post-run only — no build coupling.

### 12.1  Input

Two Google Benchmark JSON files (one per comparator).  Optional:
parity_check JSON line-stream for the "(parity mismatch)" tagging.

### 12.2  Pivot

```
benchmarks[i] = {"name": "BM_arithmetic_intAdd3",
                 "real_time": 329.4, "cv": 0.036,
                 "iterations": 12500000,
                 "arena_bytes": 0, "host_calls": 2}

Joined (same BM name across both binaries):
surface     | id              | expression       | celwasm | cel-cpp | ratio  | counters
arithmetic  | intAdd3         | a + b + c        | 329     | 620     | 1.88×  | arena=0, calls=2
arithmetic  | intAddManyTerms | a+b+c+d+e+f+g+h+i+j | 1142 | 2400    | 2.10×  | arena=0, calls=9
strings     | concatHello64B  | s + " " + n      | 18      | 47      | 2.61×  | arena=72, calls=2
```

**The `expression` column is the load-bearing display.**  A
reviewer scanning the table sees `a + b + c` next to `1.88×` and
can verify "yes that's the operation I care about."  The `id`
column is just the join key.

`report.py` reads the corpus YAML to fetch the `source` field for
each cell — joins it into the table from the corpus, not from the
JSON (the JSON only knows the BM name).

### 12.3  Expression display — long-expression policy

The `expression` column is load-bearing but Markdown tables break
on cells longer than ~80 chars.  `report.py` applies a deterministic
truncation policy so the table stays scannable.

**Default: max 60 chars in the Markdown column.**  Beyond that:

1. **Pattern-aware truncation** — if the expression matches a
   repeating-operand pattern (`<operand> <op> <operand> <op> …`),
   render as `head + … + tail (N terms)`:

   ```
   a + b + c + d + e + f + g + h + i + j
   ──truncates to──▶  a + b + … + i + j (10 terms)
   ```

   Same for `<` chains, `&&` chains, `||` chains, `.contains()`
   sequences.  Pattern detection is regex-based in `report.py`;
   ~50 LOC.

2. **Naive truncation** — for expressions that don't match a
   recognised pattern, show first 55 chars + `…`:

   ```
   (((a + b) * (c - d)) > ((e + f) * (g - h))) && (foo(bar))
   ──truncates to──▶  (((a + b) * (c - d)) > ((e + f) * (g - h))) && (f…
   ```

   The full expression is always available in the CSV and via the
   `--expand` flag.

3. **Tag opt-out** — cells with `tags: [show-full]` are never
   truncated.  Use for cells whose shape is the whole point.

The CSV column is always the full expression, never truncated —
machine consumers see the truth.

A `--max-expr-width=N` flag overrides the default for one-off
deep-dive runs (e.g. wider terminal review): `report.py
--max-expr-width=120`.

### 12.4  Per-operator reference table — the headline artifact

This is what gets published.  One row per operator, computed by
linear regression over the length-sweep cells (`{2, 10, 50, 250,
1000}` per the methodology in §6.4):

```
Eval steady-state, median ns/call.

operator   | T(2)  | T(10) | T(50)  | T(1000)  | celwasm slope (ns/op) | cel-cpp slope (ns/op) | celwasm setup | cel-cpp setup | crossover @ N terms
─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
+ int      | 260   | 994   | ~3200  | ~62000   | 62                    | 35                    | 165           | 50            | n/a (we never win)
* int      | 260   | ~1100 | ~3500  | ~70000   | 70                    | 38                    | 165           | 48            | n/a
< int      | …     | …     | …      | …        | …                     | …                     | …             | …             | …
&& bool    | …     | …     | …      | …        | …                     | …                     | …             | …             | …
== string  | …     | …     | …      | …        | …                     | …                     | …             | …             | …
```

The two slope columns answer "what does each additional operation
cost in steady state?"  The two setup columns answer "what's the
fixed per-Eval tax?"  The crossover column answers "at what
expression length does celwasm pull ahead?" — if the cell is
`n/a` the embedder knows AOT isn't beating tree-walking on this
operator at any length their workload reaches.

This table is the **adoption pitch when celwasm slopes win** and
the **honest accounting when they don't**.  Either way, the
embedder gets reference numbers they can plug into their own cost
model.

### 12.5  Per-cell detail table — secondary artifact

The full N-cell table from earlier, kept for drilling down when the
slope summary shows something interesting:

```
| surface     | id              | expression          | celwasm | cel-cpp | ratio
| arithmetic  | intAdd2         | a + b               | 260     | 73      | 0.28×
| arithmetic  | intAdd10Terms   | a + b + … + j       | 994     | 350     | 0.35×
| arithmetic  | intAdd50Terms   | a + … + AX (50 tms) | …       | …       | …
| arithmetic  | intAdd250Terms  | a + … + IP (250)    | …       | …       | …
| arithmetic  | intAdd1000Terms | a + … + ALL (1000)  | …       | …       | …
```

`(parity mismatch)` and `(noisy)` tags replace timing when
applicable.

### 12.6  Output files

```
results/
├── <date>-<host>.md        ← committed (human-readable)
├── <date>-<host>.csv       ← committed (long-format time series)
└── raw/<date>-<host>/      ← gitignored
    ├── celwasm.json
    └── celcpp.json
```

The Markdown file has both tables — slope summary first, per-cell
detail second.  The CSV is per-cell only (the slope summary is
recomputable from it).

### 12.7  README publication

`report.py --update-readme` overwrites the `## Results` section of
`benchmark/README.md`.  README stops drifting.

---

## 13. Profiler integration

`profile.sh` wraps a single cell in a platform-appropriate
profiler.

### 13.1  macOS (samply)

```bash
./profile.sh celwasm BM_arith.int.add.linear.50
```

Runs:
```bash
bazel build -c opt //benchmark/eval:celwasm_bench
samply record -o /tmp/profile.json -- \
    bazel-bin/benchmark/eval/celwasm_bench \
    --benchmark_filter='^BM_arith\.int\.add\.linear\.50$' \
    --benchmark_min_time=2s
samply load /tmp/profile.json
```

Opens browser flame graph.  Install: `cargo install samply`
(one-time).

### 13.2  Linux (perf)

```bash
perf record -g -F 999 -o /tmp/perf.data -- \
    bazel-bin/benchmark/eval/celwasm_bench \
    --benchmark_filter='^BM_arith\.int\.add\.linear\.50$' \
    --benchmark_min_time=2s
perf script -i /tmp/perf.data | stackcollapse-perf.pl | flamegraph.pl > /tmp/flame.svg
xdg-open /tmp/flame.svg
```

`flamegraph.pl` from https://github.com/brendangregg/FlameGraph
(vendored at `tools/flamegraph/` for CI).

### 13.3  What you see

Flame graph of just the target cell's hot path.  For
`BM_arith.int.add.linear.50` you see Cranelift-emitted native code
calling `cel_int_add` 49 times.  If `cel_int_add` dominates →
kernel-level work.  If `arena_alloc` dominates → memory-pattern
work.  Etc.  Actionable.

---

## 14. Counters for attribution

`state.counters["name"] = value` emits per-iteration counters into
Google Benchmark's JSON output.  report.py renders them as columns.

| counter | source | unit |
|---|---|---|
| `arena_bytes` | `Plan::LastArenaBytes()` | per-iter bytes |
| `host_calls` | `Plan::LastHostCalls()` | per-iter calls |
| `wasm_traps` | `Plan::LastWasmTraps()` | per-iter; should be 0 |
| `arena_chunks` | `Plan::LastArenaChunks()` (post-#34) | per-iter peak |

cel-cpp wrapper reports these as 0 (or omits them entirely; report.py
shows `—`).  Honest signal: counters are celwasm-specific
attribution, not a fairness asymmetry.

The counters turn "we're 2× faster" into "we're 2× faster
because…".

---

## 15. CI integration

Once Phase 1 is in green:

- **Per-PR**: `run.sh smoke` on CI.  `min_time=0.1s`, `repetitions=3`.
  `tools/compare.py` diff against last baseline.  Regression (median
  > 15% slower on any non-noisy cell) blocks merge.
- **Nightly**: full run on dedicated runner.  Publishes new
  baseline.  PR check rebaselines.
- **Per-release**: full run on macOS-arm64 + Linux-x86_64.  README
  regenerated.

`scripts/check_perf.py` is the regression detector (mirrors the
existing `scripts/check_conformance_monotonic.sh`).

---

## 16. Failure modes

| failure | behaviour |
|---|---|
| YAML parse error | `LoadCorpus` returns non-OK; binary exits 1 with clear message; bazel build also runs a `corpus_loader_test` that flags this at build time |
| comparator wrapper throws at BuildPlan | Google Benchmark reports the failure for that BM; report shows `(build failed)` |
| parity mismatch | parity_check exits nonzero; run.sh excludes those cells from timing |
| variance > 10% | report.py tags `(noisy)`; no merge block |
| samply / perf unavailable | profile.sh exits with install hint |

No silent-skip.

---

## 17. Extensibility

### 17.1  Adding a new comparator

1. Write `comparators/foo_wrapper.{h,cc}` matching the surface.
2. Add `foo_bench_main.cc` (copy `celwasm_bench_main.cc`, swap the
   include).
3. Add `cc_binary` entry to BUILD.
4. Update `report.py`'s known-comparator list.
5. Update `run.sh` to invoke the new binary.

### 17.2  Adding a new surface

1. Add `corpus/<surface>.yaml`.
2. Add the path to the `LoadCorpus` list in both `*_main.cc`.
3. Tick the surface in `OPERATORS.md`.

No wrapper changes.  No `report.py` changes (it pivots by cell id,
not by surface).

### 17.3  Adding a new axis

Bump `schema_version` in YAML.  Add the field to `Cell` in the
loader.  Old corpora without the field get a default.

---

## 18. Phasing

| phase | duration | output |
|---|---|---|
| Day 0 (this design) | — | DESIGN.md, OPERATORS.md, corpus YAML stubs, compiler TODO.md |
| Day 1 | 1 day | yaml-cpp dep + corpus_loader + loader tests |
| Day 2 | 1 day | celwasm_wrapper + celwasm_bench_main + first smoke run |
| Day 3 | 1 day | celcpp_wrapper + parity_check + first 4-cell diff |
| Day 4 | 1 day | report.py + first published Markdown comparison |
| Day 5 | 1 day | profile.sh + flame graph on one cell |
| Days 6-10 | 5 days | Phase 1 corpus populated all four surfaces; iterate |
| Phase 2 | 1 week | composite-workload corpus (auth, K8s, Envoy) |
| Phase 3 | when ships | comprehensions, time, ext |

CI integration after Phase 1 is stable.

---

## 19. Open questions

1. **Cell-loading speed at startup.**  ~260 cells × yaml-cpp parse
   = ~10 ms.  Acceptable; not in any timed path.
2. **Counters for cel-cpp**.  Could expose `arena->SpaceUsed()` via
   the wrapper; small effort, decide before publishing.
3. **`--benchmark_perf_counters` (Linux only)** — cycles,
   instructions, cache-misses.  Worth enabling on Linux runs.
4. **Threaded benches.**  Google Benchmark's `->ThreadRange(1, 4)`
   for cells real embedders run in parallel.  Phase 2.
5. **Result canonicalisation for doubles.**  Until #38 landed
   (`std::to_chars`), the two comparators printed different
   strings for the same value.  Post-#38 it's fine; document the
   contract in `CanonicalForm`.
6. **Tag-based runtime filtering.**  The `tags` field isn't yet
   exposed to `--benchmark_filter`.  Append `_<tag>` suffix to the
   BM name?  Or run a tag-pruning pre-pass in `_main.cc` before
   registering?  Decide in Day 2.
7. **Comparator-cardinality validation.**  Should report.py refuse
   to publish with only one comparator?  Probably yes — gate on
   `≥ 2`.

---

## 20. References

- `bench/README.md` — as-shipped celwasm-vs-self numbers + accounting
  for CEL_LOG_DISABLED / opt-level / LTO.
- `bench/in_operator_cel_cpp_bench.cc` — proven cel-cpp standalone-TU
  pattern.
- cel-cpp: `third_party/cel-cpp/runtime/` (vendored via
  `third_party/fetch_cel_cpp.sh`).
- Google Benchmark: https://github.com/google/benchmark — the
  foundation; we use `RegisterBenchmark(name, fn)` for dynamic
  registration.
- yaml-cpp: https://github.com/jbeder/yaml-cpp — bazel `http_archive`.
- samply: https://github.com/mstange/samply — macOS profiler.
- FlameGraph: https://github.com/brendangregg/FlameGraph — Linux
  post-processing.
