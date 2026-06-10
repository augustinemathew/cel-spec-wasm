# M27 — Property-based testing via a type-driven CEL generator

Status: design 2026-06-04 (revised 2026-06-05 — model + scope
decisions locked); Slice A starting next.

## Locked design decisions (as of 2026-06-05)

The chat-thread discussion settled the model.  This section is
the singular reference; the per-section narrative below is
preserved for context but where it conflicts with this list, this
list wins.

| Decision | Value | Why |
| --- | --- | --- |
| Generator structure | **typed attribute grammar** — for each `CelType` T, a closed table of `Production` rows; each Production is a string template (`"%0 + %1"`, `"size(%0)"`, …) plus a list of typed argument slots | every emitted source type-checks by construction; 100% of iterations reach our compiler + the oracle; the grammar is data, auditable, easy to extend |
| Type-checker enforcement | **by construction** (not post-hoc filter) | shape-driven generation wastes ≥50% of iterations on type-check rejects |
| Scope handling | `GenCtx { depth_budget, in_scope: name→CelType, rng }` threaded through the recursion; per-rule `extra_scope_for_arg_[i]` extends `in_scope` for that arg's recursion (comprehension `iter_var`, `cel.bind` body, `accu_var`) | comprehensions can't be modeled without scope-extension; this is the only "non-data" piece of the model |
| Termination | depth budget decrements per recursion; at depth 0 the generator filters to `is_leaf == true` rules; CHECK at grammar load time that every type has ≥1 leaf | guaranteed terminating; no need for runtime "out of budget" fallbacks |
| Weighting | per-production `weight` field; start uniform, tune after the first divergence-rich runs | data-driven; matches fuzztest's `WithFrequencies` API |
| Runtime errors | **guarded at the grammar level** — only productions whose output is total over their typed inputs are admitted; no division, no modulo, no unrestricted indexing, no string→int, no bytes→string | every divergence is a value mismatch, no false positives from "both sides errored but differently"; coverage relaxation is a follow-up slice, not blocking |
| Framework | fuzztest as a `bazel_dep` in MODULE.bazel | shrinking is the killer feature; rolling our own loses that |
| Slicing | A (fuzztest wiring + smoke test) → B (scalar generator + property over Int/Uint/Double/Bool/String/Bytes only) → C (aggregates + comprehensions) → D (corpus persistence + CI) | each slice independently useful; can stop after A or B if value runs out |
| Activation vocabulary | the existing `e2e/slot_aliasing_test.cc` bindings plus `c: celwasm.testdata.Customer` and `opt: optional<int>` | matches what our e2e battery already uses; no new fixtures needed |
| Oracle-divergence policy | every PBT divergence becomes (a) a permanent row in `e2e/slot_aliasing_test.cc` (or its proto/optional sibling), (b) a focused unit test at the right layer (`layout_pass_test`, `slot_allocator_test`, `expr_lower_test`, …), AND (c) a commit message citing the PBT seed | PBT is the discovery tool, hand-written tests are the pinning tool; this is the same workflow the spec conformance suite already uses |
| Grammar validation | three gtest layers — **L1** static structural checks inside `Grammar::Validate()` (leaf coverage, placeholder consistency, arg-type reachability), **L2** per-production roundtrip through the real cel-cpp parser + checker, **L3** sampled composition check at depths {1, 3, 6} for type agreement with cel-cpp | the grammar IS the spec for the generator; if a `format` typo lets the generator emit ill-typed source, every PBT iteration is wasted; L1/L2/L3 green = an oracle-property failure is necessarily a runtime bug, not a generator misconfig.  See §"Grammar validation" below for the full schema. |

## The data model — what a Production looks like

```cpp
struct Production {
  std::string name;                                    // "int_add", "list_int_index", …
  std::string format;                                  // "(%0 + %1)", "%0[%1]", …
  std::vector<CelType> arg_types;                      // one per %i placeholder
  std::vector<std::vector<std::pair<std::string, CelType>>>
      extra_scope_for_arg;                             // arg index → vars added to scope inside %i
  bool is_leaf;                                        // usable at depth 0
  int weight = 1;                                      // sampling weight
};

class Grammar {
 public:
  void AddProduction(CelType target, Production p);
  const std::vector<Production>& Rules(CelType target) const;

  // CHECK at construction: every type with any rule has at least
  // one leaf rule, so depth-0 recursion always terminates.
  absl::Status Validate() const;
};
```

The grammar is a static table populated once at test-binary
startup.  The generator function is:

```cpp
std::string GenerateExpr(CelType target, GenCtx& ctx) {
  const auto& rules = grammar.Rules(target);
  std::vector<const Production*> eligible;
  for (const Production& p : rules) {
    if (ctx.depth_budget == 0 && !p.is_leaf) continue;
    eligible.push_back(&p);
  }
  ABSL_CHECK(!eligible.empty()) << "no leaf rule for type " << target;

  const Production& chosen = WeightedPick(eligible, ctx.rng);
  std::string result = chosen.format;
  for (size_t i = 0; i < chosen.arg_types.size(); ++i) {
    GenCtx sub = ctx;
    sub.depth_budget--;
    for (auto& [name, type] : chosen.extra_scope_for_arg[i]) {
      sub.in_scope[name] = type;
    }
    std::string arg = GenerateExpr(chosen.arg_types[i], sub);
    AbslReplaceAll(&result, {{absl::StrCat("%", i), arg}});
  }
  return result;
}
```

## Guarded productions: the closed table for the first cut

Disallowed rules (versus a naive "every operator the spec admits"
catalog):

  - `int_div`, `int_mod`, `uint_div`, `uint_mod`,
    `double_div` — disabled.  Arithmetic only via `+`, `-`, `*`.
  - `list_int_index` and siblings — only admitted when the index
    is statically bounded by a sibling generator that knows the
    list length.  Easiest first cut: only emit
    `list_int_index_modN` whose template is `(%0)[(%1) % size(%0)]`
    — but `%` is disabled, so we skip this and reach `int` via
    `size(...)`, `_in_`, conversions from `double_const`, and
    comprehension results (`size(list.filter(v, pred))`) instead.
  - `map_si_lookup` — disabled on the first cut; reach maps only
    through `size(...)` and `_in_`.
  - `int_from_string`, `uint_from_string` — disabled (the string
    might not parse).
  - `int_from_double` — only with literal-bounded sources
    (`int(3.14)` is fine; `int(huge_double_expression)` is not
    emitted in Slice B; allowed once we have safe range tracking).
  - `bytes_to_string` — disabled (might be invalid UTF-8).
  - `_<<_` / `_>>_` and other extension operators outside the CEL
    spec — never admitted regardless of slice.

Productions that ARE in the first cut (Slice B, scalar-only):

  - Constants: `null`, `true`, `false`, small int / uint /
    double literals, fixed-set string and bytes literals.
  - Idents: every name in `GenCtx::in_scope`.
  - Arithmetic on int / uint / double: `+`, `-`, `*`, unary `-`.
  - Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=` (yields Bool).
  - Logical: `&&`, `||`, `!`.
  - Ternary: `cond ? a : b` (yields any T given Bool + 2×T).
  - Type conversions: `double(int)`, `double(uint)`, `int(uint)`,
    `uint(int)` only when the cast is total over the input
    domain.  (`int(double)` is admitted only with a constant
    source whose value the generator can range-check.)
  - Receiver-form `size(string)`, `size(bytes)` — total.

Slice C adds:

  - kListExpr aggregates yielding `List<T>` for every leaf T.
  - kMapExpr aggregates yielding `Map<K,V>` for closed K, V sets.
  - `_in_` over the resulting list/map shapes.
  - Comprehensions: every macro (`exists`, `all`, `exists_one`,
    `filter`, `map`, `map(filter)`) with the predicate generator
    threading the iter_var into `in_scope`.
  - Receiver-form `size(list)`, `size(map)`.
  - kStructExpr against `celwasm.testdata.Customer` — `Foo{f: g}`
    forms where every field gets a typed sub-expression.
  - kSelectExpr against bound message fields.

## Recursion budget

Default `depth_budget = 6` for Slice B; bumped to 8 in Slice C
once aggregates and comprehensions are in.  The budget is the
ONLY runtime depth control — fuzztest's shrinker can shrink the
budget toward 0 to produce the minimal failing repro.

## Grammar validation — three levels, all gtest cases

The grammar IS the spec for our generator.  A typo in a `format`
string or a misregistered type means every PBT iteration is wasted
on the wrong question.  So before the oracle property runs a
single iteration, three layers of checks fire as ordinary unit
tests in `e2e/fuzz/grammar_test.cc`.

If all three are green, an oracle-property failure in Slice B is
**necessarily** a runtime / codegen bug, not a generator
misconfiguration.  That's the discrimination this section buys us.

### L1 — Static / structural (sub-millisecond)

Runs inside `Grammar::Validate()` at construction:

  - Every type with any production has at least one `is_leaf=true`
    rule (depth-0 recursion always terminates).
  - Every `Production.format` mentions each `%i` placeholder it
    declares, and references no `%j` for `j ≥ arg_types.size()`.
  - Every `arg_types[i]` is itself a registered target (the
    recursion can find SOME production yielding it).
  - Every type that appears as an arg also appears as a target.
    (Catches "I added a production needing `List<Int>` but forgot
    to register `List<Int>`.")

Failure mode: `Grammar::Validate()` returns
`absl::InvalidArgumentError` with the offending production name +
field; the per-binary `INSTANTIATE_TEST_SUITE_P` in the property
test asserts `Validate().ok()` at startup so the harness can't run
against a malformed grammar.

### L2 — Per-production type-check via the real cel-cpp pipeline

The load-bearing check.  For each `Production p`:

  1. Build a minimal source by substituting each `%i` with a
     uniquely-named ident of type `p.arg_types[i]`.
  2. Build the matching `CheckOptions` declaring those idents.
  3. Run our `ParseAndCheck` (which wraps cel-cpp's parser +
     checker).
  4. ASSERT it accepts AND `root_type() == p.target`.

This catches every "I claimed `(%0 + %1)` is `Bytes × Bytes →
Bytes` but the checker rejects bytes concat" / "I registered
`(%0 < %1)` for `String × String → Bool` but cel-cpp dispatches
that operator differently" class of bug.  Run once per build; ~35
productions in Slice B × ~5 ms each = ~200 ms total.  Trivial.

Leaves with no `%i` (literal constants, idents) get a
`ConcreteLeafExample(target)` substituted in — the same generator
the runtime walker uses for the leaf, so we're testing the actual
emitted form.

### L3 — End-to-end sampled (composition correctness)

A spot-check that productions COMPOSE correctly under the
recursion:

  - Generate `N` (= 1000) expressions at each of depths {1, 3, 6}.
  - For each, run our `ParseAndCheck` AND the cel-cpp oracle's
    `Parse + Check`.
  - ASSERT both accept.
  - ASSERT both type the root as `Bool` (or whatever the target
    type for the property is).

This catches the rare "each production is fine alone but the
composition disagrees with cel-cpp at depth 3" bug — typically
caused by overload resolution differences when our type checker
and cel-cpp's both run.

3 depths × 1000 seeds × ~5 ms each ≈ 15 s.  Fast enough for
fastbuild local; in CI it can be sized up to 10 k seeds without
the wall-clock budget becoming an issue.

### What L1+L2+L3 do NOT cover

  - **Value-level oracle agreement.**  L3 only checks types; the
    `cel_oracle_property_test.cc` body is the value oracle.  This
    is by design — type agreement first, value agreement second,
    so a failure of one doesn't mask the other.
  - **Coverage of every kind path inside cel-cpp's eval.**  L3 only
    confirms cel-cpp accepts our source; the cel-cpp evaluator
    may take an internally-different path for the same source than
    ours does.  That's fine — the value-oracle property will catch
    a divergence; the L-tests just rule out "garbage in" before
    that property runs.

### Ordering inside Slice B (revised)

The Slice B work below is now an explicit five-step sequence;
steps 1-3 must be green before steps 4-5 are written.

  1. `e2e/fuzz/grammar.{h,cc}` — `Production`, `GrammarBuilder`,
     `Grammar::Validate()` (L1 checks).
  2. `e2e/fuzz/grammar_slice_b.cc` — the scalar-only catalog
     (~35 productions, ~50 lines).
  3. `e2e/fuzz/grammar_test.cc` — L1 + L2 + L3 as gtest cases.
     **Ship as a standalone green commit** before continuing.
  4. `e2e/fuzz/generator.{h,cc}` — `GenerateExpr(target, ctx)`
     walking the grammar; the leaf-domain helpers
     (`Domain<int64_t>`, `Domain<std::string>`, …) for the
     constant generators inside `Leaf` productions.
  5. `e2e/fuzz/cel_oracle_property_test.cc` — `FUZZ_TEST` wiring
     the generator into the value-oracle comparison.

## Why simple fuzzing won't work

Feeding random byte strings to `Compiler::Compile` burns >99% of
cycles in the lexer.  The parser rejects almost every byte sequence
on the first character; even the lucky ones that lex hit the
checker's type-disambiguation and fail before any code path under
test runs.  Two AST-kind bugs we hit this week (the m4 nested-list
slot aliasing and the m7 map-of-message clobber) wouldn't have
surfaced under random-byte fuzzing either, because the broken
shapes are *syntactically narrow* — they require valid nested
literal grammar, which lexer fuzzing rarely produces.

What we need is a **structured generator** that emits, by
construction, CEL source the parser accepts AND the type checker
accepts.  Then every iteration reaches our codegen + runtime + the
cel-cpp oracle, and a mismatch is a real bug, not parser noise.

## Architecture

```
                    fuzztest Domain<std::string>
                              │
                              ▼
   ┌──────────────────────────────────────────────────────┐
   │ CelSourceDomain(max_depth, type_vocab, activation)   │
   │   - type-driven recursive generator                  │
   │   - emits source strings + paired expected eval kind │
   └──────────────────────────────────────────────────────┘
                              │
                              ▼
   ┌──────────────────────────────────────────────────────┐
   │ FUZZ_TEST(CelOracleProperty, EvalMatchesCelCpp)      │
   │                                                       │
   │   ours      = our Compile/Plan/Eval(source, activation)│
   │   oracle    = cel-cpp Parse/Check/Eval(source, …)    │
   │                                                       │
   │   ASSERT_EQ(ours.kind(), oracle.kind())              │
   │   ASSERT_EQ(*ours.As<T>(), *oracle.As<T>())          │
   └──────────────────────────────────────────────────────┘
```

When ASSERT_EQ fails, fuzztest shrinks the offending source string
by replaying the generator with smaller inputs (lower recursion
depth, fewer arguments, smaller constants, simpler kinds) until it
finds the minimal source the property still fails on.  That gets
reported as the failure repro.

The cel-cpp oracle lives in `testdata/cel_cpp_oracle.cc` already and
ships in tree.  Its current `EvalWithCelCpp(source, container)`
signature handles parser + checker + runtime in one call and
returns either an `OracleResult` value or an error.  The PBT
harness reuses it directly — no new oracle wiring.

## Design choices to nail down before coding

### 1. Type-driven vs. shape-driven generation

**Type-driven (recommended).** The generator threads a `CelType`
target through the recursion: `GenerateExpr(target_type=Int)` only
ever emits Exprs that the checker types as Int.  Type information
drives the per-kind choice — `kCallExpr` picks from typed-operator
catalogs, `kListExpr` picks an element type and threads it down,
`kSelectExpr` requires the operand type to expose `target_type` as
a field, etc.

Pro: every generated source type-checks; 100% of iterations reach
our compiler and the oracle.  Con: the generator is bigger (one
arm per type × kind).

**Shape-driven.** Generate any AST shape, let the cel-cpp checker
reject the ill-typed ones.

Pro: simpler generator.  Con: most iterations rejected at the
type-check gate; reduces effective coverage.

**Decision: type-driven.**  The bug shapes we care about all live
post-checker, so we want every iteration to land there.

### 2. Framework: fuzztest

Already settled with the Explore agent — fuzztest gives us
`Domain<T>` composition, automatic shrinking, gtest integration,
and a path into Google's CI infra later.  rapidcheck is the
runner-up but loses on the shrinking story (fuzztest's
parser-replay shrinker is the killer feature for grammar
generators).

Bazel wiring: the previous agent partially set this up before you
cancelled it; we'd resume that bit only after the design is
agreed.

### 3. Activation schema (the bindings the generator can reference)

The generator needs to know which variable names + types are
declared, so it can emit kIdentExpr nodes safely.  Proposal: a
fixed activation matching the e2e suite's existing bindings,
extended with two proto-typed bindings so kSelectExpr / kStructExpr
have real targets:

| Name | CelType | Value bound in tests |
| --- | --- | --- |
| `a`..`h` | Int | distinct primes (2, 3, 5, 7, 11, 13, 17, 19) |
| `u` | Uint | 42u |
| `d` | Double | 3.14 |
| `s` | String | `"hello"` |
| `by` | Bytes | `b"hi"` |
| `b` | Bool | true |
| `xs` | list<int> | `[1, 2, 3]` |
| `ms` | map<string, int> | `{"a": 2, "b": 3}` |
| `c` | celwasm.testdata.Customer | a fixture instance |
| `opt` | optional<int> | `optional.of(7)` |

This gives the generator a closed type vocabulary it can recurse
through.

### 4. Recursion budget

The CEL parser caps at 16,384, but we want generated expressions
to stay readable and the property check to finish in milliseconds
per iteration.  Proposed: max recursion depth = 6.  That puts an
upper bound of roughly 2⁶ = 64 internal nodes per expression,
which is plenty to exercise every nesting interaction and stays
well under the slot-exhaustion gate.

The depth is a fuzztest input — fuzztest shrinks toward depth 0,
so the minimal repro for any failure naturally drops to the
shallowest expression that still fails.

### 5. Per-kind generator arms

Each ExprKindCase has its own generator function:

```cpp
// Top-level: pick a target type from the activation's vocab,
// recurse to MaxDepth.
fuzztest::Domain<std::string> CelSourceDomain(int max_depth);

// Each typed generator returns a source string AND a static
// expected CelType (for top-level assertion).
fuzztest::Domain<std::string> GenInt(int depth, ActivationCtx ctx);
fuzztest::Domain<std::string> GenBool(int depth, ActivationCtx ctx);
fuzztest::Domain<std::string> GenString(int depth, ActivationCtx ctx);
// ... etc for every supported target type

// Each per-type generator dispatches on a uniform-random choice
// of kind, with depth=0 forcing kConstant / kIdentExpr (leaves).
```

Per-kind catalogs (drafted from cel-cpp's operator inventory):

- **kCallExpr**: arithmetic (`+`, `-`, `*`, `/`, `%`), comparison
  (`==`, `!=`, `<`, `<=`, `>`, `>=`), logical (`&&`, `||`, `!`),
  `in`, type conversion (`int`, `uint`, `double`, `string`,
  `bytes`, `bool`), receiver-form (`size`, `startsWith`,
  `endsWith`, `contains`, `matches`).  Catalog keyed by return
  type → list of (op, arg-types).  Ternary `?:` is a separate
  arm so the cond and arm types match.
- **kListExpr**: pick element type T from the vocab, generate
  N (uniform 0..5) sub-Exprs of type T, emit `[e0, …, eN-1]`.
- **kMapExpr**: pick K (string|int|uint|bool) and V from the
  vocab, generate N entries.
- **kStructExpr**: only emits when target type is one of the
  known proto descriptors; picks a subset of fields, recurses on
  each value's type.
- **kSelectExpr**: only emits when operand type exposes the
  target type as a field (precomputed map from CelType to
  available field names + types).
- **kComprehensionExpr**: pick a macro (`exists`, `all`,
  `exists_one`, `filter`, `map`), generate `iter_range:list<T>`
  for some T, recurse on the predicate with type Bool.

### 6. The property assertion

For each generated source string `s`:

```cpp
auto ours_program = our_compiler.Compile(s);
auto oracle = EvalWithCelCpp(s, kTestContainer);

// If either side rejects (compile error / parse error / type
// error), the other must reject too.  Mismatched rejection is a
// bug worth reporting — but only when both are expected to
// accept by the generator's construction.  Since the generator
// only emits well-typed source, both sides should accept.
ASSERT_THAT(ours_program, IsOk()) << s;
ASSERT_THAT(oracle, IsOk()) << s;

auto ours_instance = engine.Plan(*ours_program);
auto ours_value = ours_instance->Eval(test_activation);

// Both sides accepted.  Now eval.  Match on kind THEN on value.
ASSERT_EQ(ours_value->kind(), oracle->result.kind()) << s;
switch (ours_value->kind()) {
  case Value::Kind::kBool:
    EXPECT_EQ(*ours_value->AsBool(), *oracle->result.AsBool()) << s;
    break;
  case Value::Kind::kInt: …
  // … one arm per kind
}
```

### 7. What gets reported on failure

fuzztest emits a reproducer seed; the test harness logs the
shrunk source string.  We persist the corpus to
`testdata/pbt_corpus/` (gitignored except for committed-failure
regressions, like `testdata/pbt_corpus/2026-06-04-nested-list-alias`).

When a shrunk-failure source pins a real bug, we **copy it to
`e2e/slot_aliasing_test.cc`** as a hand-written row (so the
property suite stays a discovery tool and the e2e suite stays the
documentation of known-shape bugs).

### 8. What the property does NOT cover

- Performance — fuzztest is a correctness tool.
- Behaviors that depend on activation state we don't bind
  (network ext, custom functions) — those need their own
  generators.
- Errors that depend on runtime state cel-cpp evaluates lazily
  and we evaluate eagerly (or vice versa).  Those become known
  oracle-divergence rows in the conformance docs, not PBT bugs.

## Implementation plan

Smallest useful slice — three sub-slices, each shippable on its
own:

### Slice A: fuzztest wiring + smoke test  (DONE — 2026-06-05)
   - `MODULE.bazel:329` — `bazel_dep(name = "fuzztest", version =
     "20260219.0")` is wired.
   - `e2e/fuzz/fuzz_smoke_test.cc` — trivial property
     `FUZZ_TEST(FuzzSmokeTest, SizeIsNonNegative)` confirms the
     framework runs under `bazel test`.
   - `e2e/fuzz/BUILD.bazel` — `cc_test` depending on
     `@fuzztest//fuzztest` + `@fuzztest//fuzztest:fuzztest_gtest_main`.
   - `bazel test //e2e/fuzz:fuzz_smoke_test` green.

### Slice B: type-driven scalar generator + oracle property

Five-step internal sequence — steps 1-3 ship as a green commit
before steps 4-5 are written, so a Slice B reviewer can see the
grammar is validated end-to-end before the property body exists.

   1. `e2e/fuzz/grammar.{h,cc}` — `Production`, `GrammarBuilder`,
      `Grammar::Validate()` doing the L1 structural checks
      (leaf coverage, placeholder consistency, arg-type
      reachability).
   2. `e2e/fuzz/grammar_slice_b.cc` — the scalar-only catalog
      (~35 productions covering arithmetic / comparison /
      logical / safe type conversions / safe size).
   3. `e2e/fuzz/grammar_test.cc` — L1 + L2 + L3 as gtest cases.
      L2 per-production roundtrips through the real cel-cpp
      checker; L3 spot-checks 3 × 1000 generated expressions at
      depths {1, 3, 6} for type agreement with cel-cpp.
      **Ship as a standalone green commit before continuing.**
   4. `e2e/fuzz/generator.{h,cc}` — `GenerateExpr(target, ctx)`
      walking the grammar; `Domain<…>` helpers for the leaf
      constant generators (`int64_t`, `std::string`, …).
   5. `e2e/fuzz/cel_oracle_property_test.cc` — `FUZZ_TEST`
      wiring the generator into the value-oracle comparison
      against `testdata/cel_cpp_oracle.cc`'s `EvalWithCelCpp`.

   Goal of step 5: 1000 iterations in < 1 s on fastbuild, zero
   value divergences against the current Design D pipeline.  Any
   divergence is a real codegen / runtime bug because steps 1-3
   already proved the grammar is sound.

### Slice C: aggregates + comprehensions in the generator
   - kListExpr, kMapExpr, kStructExpr arms.
   - kComprehensionExpr arm with every supported macro.
   - Bump max_depth to 8.
   - Re-run the L1/L2/L3 validation suite against the expanded
     grammar before any oracle iteration runs.
   - Goal: 100K iterations against the full grammar in a CI
     job, with any divergence translating to a hand-written e2e
     row.

### Slice D (later): corpus persistence + CI integration
   - `testdata/pbt_corpus/` committed seeds for past-bug repros.
   - `scripts/run_pbt.sh` with configurable duration.
   - Optional: nightly CI job that runs PBT for 30 min, posts
     diff against the previous run.

## Open questions

(None remaining — see the locked-decisions table at the top of
this doc.  Original chat-thread questions resolved 2026-06-05.
Slice A shipped same day; Slice B steps 1-3 are next, with
explicit grammar-validation green commit before steps 4-5.)

## Slice C1 shipped (2026-06-05) — what got measured

  - **Grammar:** Slice C1 catalog landed (lists + maps of all
    scalar types, comprehension macros `.exists` / `.all` /
    `.exists_one` / `.filter` / `.map` over lists, plus
    `.exists` / `.all` / `.exists_one` over maps).  Slice C2
    (kStructExpr + kSelectExpr + has() + Customer activation)
    deferred.
  - **Property targets at Slice C1:** 6 scalar (Bool / Int /
    Uint / Double / String / Bytes) + 4 list (`list<int>`,
    `list<bool>`, `list<double>`, `list<string>`) + 1 map
    (`map<string, int>`) — 11 properties total.  Each
    compares element-wise vs the cel-cpp oracle's
    `cel::expr::Value`.
  - **Mining tooling:** `e2e/fuzz/mine_divergences` — a
    loop-driven miner that runs the same `oracle_harness`
    plumbing as the fuzztest property but prints every
    divergence's source + ours-vs-oracle inline.  Used to
    triage what the property catches when fuzztest's
    unit-test mode buffers the EXPECT_EQ message past
    abort.
  - **Bugs found + fixed:** cleanup-backlog #32 / #33 — the
    `kComprehensionExpr` storage stamp used `accu_var.
    slot_offset`, but for `exists_one` the result lives in
    `comp.result()`'s own slot (a `kCallExpr(_==_, @result,
    1)` writing a Bool, not the accu's Int counter).  Fixed
    in `compiler/codegen/layout_pass.cc` —
    `ComprehensionLocalsVisitor::PostVisitComprehension`
    now stamps from `comp.result()`'s annotation.  3
    regression pins in `e2e/known_bugs_test.cc`.  Verified
    with a 12,000-program PBT sweep (depth 6, 2000 seeds ×
    6 scalar targets), then 5,500-program list/map sweep
    (depth 6, 500–1000 seeds × 5 container targets): 0
    value divergences.
  - **Bugs found + filed but not fixed:** cleanup-backlog
    #34 — depth-7/8 PBT reliably triggers the per-Eval
    arena overflow (`code=10 msg="overflow"`) through
    nested-comprehension × `_in_` × multi-element haystack.
    Same root surface as #17 / #21; PBT is the measuring
    instrument that confirms the cliff is reachable from
    grammar-generated depth-7 expressions, not just the
    pathological 10K-bound-list case.  Fix is the runtime
    grow-on-demand arena.

## Future work — type-polymorphic grammar for recursive nesting

The current Slice C1 grammar registers productions for each
concrete element type in `ScalarVocab()` (6 scalars) and
each `MapKV` in `MapVocab()` (5 K×V combos).  The
production machinery (`GrammarBuilder::Leaf` / `Unary` /
`Binary` / `Ternary` / `Repeated` / `Comprehension`) is
already `CelType`-parameterised — what's _not_ recursive
yet is the **set of types the registration loops iterate
over**.  Today the loops iterate scalars only, so the
grammar admits `list<bool>`, `list<int>`, etc. but never
`list<list<int>>` or `map<int, list<double>>`.

The natural extension — when there's a reason to chase
nested-type bugs — is:

  1. Compute a `TypeVocab` closed under one level of
     nesting: `scalars ∪ {list<T> | T ∈ scalars} ∪
     {map<K, V> | (K, V) ∈ MapVocab}`.  Cap at one level
     so the production count stays bounded (12 list
     productions + 5 map productions = 17 outer
     containers, vs 6 in Slice C1).
  2. Re-author the registration loops in `grammar_slice_c.
     cc` to iterate `TypeVocab` instead of `ScalarVocab`.
  3. Hand-author leaf sources for the new nested types
     (e.g. `[[1, 2, 3]]` for `list<list<int>>`,
     `[{"k": 0}]` for `list<map<string, int>>`).
  4. Extend the miner's comparison
     (`mine_divergences::CompareList` /
     `CompareMap`) to dispatch recursively on `CelType` —
     a list-typed element gets compared via `CompareList`
     recursively, a map-typed element via `CompareMap`,
     a scalar via `ScalarsEqual`.
  5. Add list/map list/map oracle properties to
     `cel_oracle_property_test.cc` keyed off the same
     `oracle_harness::GenAndEvalSliceC`.

This was scoped out of Slice C1 because the existing flat
grammar already exposed and fixed the #32 / #33 family at
depth 6, and surfaced #34 at depth 7-8 — i.e., it earned
its keep without needing nested types yet.  Pick it up
when there's a hypothesis about a nested-aggregate-specific
codegen path (`HostMapBacking::Get` returning a list,
proto field reads producing nested containers, etc.) worth
PBT-targeting.
