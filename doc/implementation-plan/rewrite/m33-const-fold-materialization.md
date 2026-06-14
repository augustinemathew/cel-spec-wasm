# m33 — constant folding into compile-time materialization

Status: plan — drafted 2026-06-14, not yet started.

## 1. Problem

m31 (`rewrite/m31-static-aggregates.md`) materializes constant aggregate
**literals** — `[1, 2, 3]`, `{'a': 'b'}` — into static memory at compile
time. Its eligibility predicate (m31 §6) is "every element/key is a
*literal* or an already-materialized const aggregate, recursively." That
predicate is too narrow: it misses subtrees that *reference no variables*
but are not syntactically literal —

```
[1 + 1, size([1, 2, 3])]          // should bake as [2, 3]
{'a': 'x' + 'y'}                  // should bake as {'a': 'xy'}
"https://" + host_const            // (host_const an inlined enum/const) → one string
base64.encode(b'hi')               // bakes to the encoded string
```

Every such subtree is a **compile-time constant**: it has no `ident`
that resolves to an activation variable, so its value is fixed at build
time. Today these compile to a per-`Eval` construction sequence (the arena
rebuild m31 set out to eliminate) plus the arithmetic/call ops to produce
each element — paid on every evaluation, forever, for a value that never
changes.

This is the standard **constant-folding** optimization: evaluate the
variable-free subtree once at build time, replace it with its result, and
(for aggregate results) hand that result to m31's materializer. m31's
literal case becomes the trivial sub-case — a literal is a variable-free
subtree that folds to itself.

## 2. Design in one sentence

A new frontend AST-rewrite pass (`FoldConstants`, running in the existing
`ParseAndCheck` rewrite chain) detects every maximal variable-free
subtree, evaluates it **once at compile time through the vendored cel-cpp
runtime** (the same engine the differential oracle already links —
`testdata/cel_cpp_oracle.cc`), and rewrites the subtree in place to a
`kConstant` (scalars) or a fully-literal `kCreateList`/`kCreateMap`
(aggregates) — after which m31's *unchanged* materializer bakes the
literal into static memory, byte-for-byte as before.

> This is the recommended mechanism, but it is one of **three**
> candidate constant-materialization engines. §10 evaluates all three
> head-to-head — (a) the cel-cpp host-side folder above, (b) Binaryen's
> `precompute` pass, (c) Binaryen's `wasm-ctor-eval` linear-memory
> snapshot — and explains why (a) is recommended as the primary path,
> what (b)/(c) would buy, and why neither displaces (a) here.

## 3. The crux: what cel-cpp gives us, and what it does not

### 3.1 cel-cpp HAS a constant folder — but it folds the *program*, not the AST

cel-cpp ships two entry points to one implementation:

- `cel::extensions::EnableConstantFolding(RuntimeBuilder&, …)` —
  `third_party/cel-cpp/runtime/constant_folding.h:43-65`. The header
  states: "Constant folding eagerly evaluates sub-expressions with all
  constant inputs **at plan time** to simplify the resulting program"
  (`runtime/constant_folding.h:30-32`).
- `cel::runtime_internal::CreateConstantFoldingOptimizer(...)` —
  `third_party/cel-cpp/eval/compiler/constant_folding.h:34-38`, the
  underlying `ProgramOptimizer` factory.

The implementation is a `ProgramOptimizer` (`eval/compiler/constant_folding.cc:69`,
`class ConstantFoldingExtension : public ProgramOptimizer`). It runs
**during flat-program planning**, not during checking, and it operates on
the **execution path**, not the AST:

- `OnPostVisit` grabs the already-planned subprogram for the node
  (`context.GetSubplan(node)`, `constant_folding.cc:201`), builds an
  `ExecutionFrame` over it (`constant_folding.cc:212`), runs
  `frame.Evaluate()` (`constant_folding.cc:219`) — i.e. it invokes the
  **cel-cpp runtime evaluator** — and replaces the subprogram with a
  `CreateConstValueDirectStep` / `CreateConstValueStep`
  (`constant_folding.cc:239-249`).

**Consequence for us.** cel-cpp's folder produces a rewritten
`ExecutionPath` inside a built `cel::Runtime` program. It does **not**
produce a rewritten `CheckedExpr` / `cel::Ast` that our IR pipeline
(`PopulateAnnotations` → codegen → `StaticMemoryBuilder`) consumes. There
is no AST-level constant-folding pass in cel-cpp to enable so that "the IR
arrives pre-folded." Enabling `EnableConstantFolding` on a runtime we do
not build buys us nothing — we never build a cel-cpp runtime in the
compile path; we lower to wasm ourselves.

> Investigated-and-rejected: "just enable cel-cpp's folder in the
> frontend so the IR arrives pre-folded." It cannot — the folder rewrites
> the runtime program, a structure our compiler never produces or reads.
> The reuse we *can* take is cel-cpp's runtime as an **evaluator of a
> single const subtree** (§4), exactly as the oracle already does.

### 3.2 But its *eligibility predicate* is the spec we copy verbatim

`IsConstExpr` (`eval/compiler/constant_folding.cc:107-175`) is the
authoritative "is this subtree foldable" classifier, and its
exclusions are the semantics-preservation rules we MUST mirror
(§6). It returns one of `{kConditional, kNonConst}` per node and
propagates `kNonConst` up to any parent (`constant_folding.cc:194-199`):

| Node kind | Verdict | Cite |
|---|---|---|
| `kConstant` | foldable (folds to itself) | `:109-110` |
| `kIdentExpr` | **never** (this is the variable test) | `:111` |
| `kComprehensionExpr` | **never** ("can't detect if comprehension vars are only used in a const way") | `:113-116`, slot count 0 at `:96` |
| `kStructExpr` | **never** | `:117` |
| empty `kMapExpr` / empty `kListExpr` | **never** (reserved for comprehension/macro append opt) | `:122-131` |
| non-empty list / map | foldable | `:126`, `:133` |
| `kSelectExpr` | foldable | `:134-135` |
| `&&` / `\|\|` / `?:` (`kAnd`/`kOr`/`kTernary`) | **never** ("short-circuiting not yet supported") | `:139-141` |
| `cel.@block` | **never** | `:146-147` |
| call with a **lazy** overload (activation-dependent) | **never** | `:152-156` |
| call with a **contextual** overload | **never** | `:163-167` |
| other calls | foldable | `:169` |

**The error rule** is separate and load-bearing: if the const-subtree
evaluation returns a non-OK result, cel-cpp **does not fold** and leaves
the subtree to run (and error) at runtime —
`constant_folding.cc:220-225`: *"If this would be a runtime error, then
don't adjust the program plan, but rather allow the error to occur at
runtime to preserve the evaluation contract."* Same for `UnknownValue`
(`:227-229`). We adopt this verbatim: **a const subtree that errors is
left unfolded.** (§6.1)

### 3.3 The runtime is already linkable into our first-party tree

`compiler/` may depend on `shared/` and vendored cel-cpp, but never on
our `eval/` evaluator or wasmtime (CLAUDE.md "layering rule"). cel-cpp's
runtime is vendored cel-cpp — **explicitly allowed.** Proof it links: the
differential oracle (`testdata/cel_cpp_oracle.cc:39-41,106-151,288-313`)
already builds a `cel::Runtime` (`CreateStandardRuntimeBuilder` +
`ProtobufRuntimeAdapter::CreateProgram` + `Program::Evaluate`) inside a
first-party target and runs it. The compile-time evaluator in §4 is that
same call sequence, scoped to one subtree, hosted in `compiler/`.

## 4. Layering decision — recommended approach

**Recommended: option (a′) — a `FoldConstants` AST pass in `compiler/`
that evaluates each maximal variable-free subtree through the cel-cpp
runtime and rewrites it to a literal AST node.** This reuses cel-cpp for
the byte/value-exact semantics (the only way to stay conformance-clean)
without touching our `eval/` engine, and it lands the folded result as an
ordinary literal that m31's existing materializer handles.

Rejected alternatives:

- **(a) Enable cel-cpp's runtime folder in the frontend.** Impossible as
  a source of pre-folded IR — §3.1: it rewrites a runtime program we never
  build.
- **(b) A first-party constant evaluator over our IR.** Reimplements
  arithmetic/string/list/map/overflow/rounding/heterogeneous-equality in
  `compiler/`. Large, and every divergence from cel-cpp is a silent
  conformance bug — exactly the failure mode CLAUDE.md's
  "byte-exact against cel-cpp" rule exists to prevent. Rejected.
- **(c) The oracle path.** The oracle (`testdata/cel_cpp_oracle.cc`) is a
  *test* target and returns a neutral `cel.expr.Value`. We do not depend
  `compiler/ → testdata/`; instead we lift the same runtime-build +
  evaluate sequence into a small production helper under
  `compiler/frontend/` (e.g. `const_eval.{h,cc}`). Same engine, same
  options, production-side.

### 4.1 Pass placement in `ParseAndCheck`

`FoldConstants` slots into the existing in-place rewrite chain
(`compiler/frontend/parse_and_check.cc:1648-1660`), which already runs
`InlineConstantReferences` → `InlineTypeIdentifierReferences` →
`RejectDyn` → `PopulateAnnotations`:

```
RunTypeCheck
ValidateExpressionDepth
InlineConstantReferences          // enum/const idents → kConstant
InlineTypeIdentifierReferences    // type idents → kConstant
FoldConstants                     // NEW — variable-free subtrees → literal
RejectDyn                         // unchanged; sees fewer nodes
PopulateAnnotations               // unchanged; stamps Repr/Storage on the folded literal
```

It MUST run **after** the two inline passes — those turn const-bearing
idents (`SomeEnum.X`, `int`) into `kConstant` nodes, which is what makes
a subtree like `"x" + str(SomeEnum.X)` variable-free in the first place.
It MUST run **before** `RejectDyn` and `PopulateAnnotations` — a folded
literal is simpler to validate and annotate than the call/select tree it
replaces, and folding a `dyn`-typed-but-constant subtree to a typed
literal can only *help* the static-subset gate (though see §6.5: we fold
only static-subset-clean subtrees, so this is order-robustness, not a
relaxation of `RejectDyn`).

### 4.2 What the pass does per node

Post-order walk (reusing the `VisitInlineConstantChildren` recursion
shape at `parse_and_check.cc:1378-1413`). For each subtree, compute the
`IsConstExpr` verdict (§3.2, copied) bottom-up. At each **maximal**
foldable subtree (foldable node whose parent is non-foldable, or the
root):

1. Serialize the subtree to a standalone `cel::Expr` / `CheckedExpr`.
2. Evaluate it through the cel-cpp runtime helper (§4, option (a′)),
   declaring **no** activation variables (a foldable subtree has no
   idents, so the activation is empty — mirrors `ConstantFoldingExtension`'s
   `Activation empty_` at `constant_folding.cc:101`).
3. **If the result is an error or unknown → leave the subtree unmodified**
   (§3.2 error rule; §6.1).
4. Otherwise, convert the `cel::Value` result back into an AST node:
   - scalar (`bool/int/uint/double/string/bytes/null/type/duration/timestamp`)
     → `kConstant`;
   - list/map → a fully-literal `kCreateList`/`kCreateMap` whose elements
     are themselves `kConstant` (recursively), i.e. exactly the shape m31
     already recognizes as a constant literal.

Step 4's value→AST conversion is the one new piece of code. It is the
inverse of cel-cpp's `ConvertConstant` (`constant_folding.cc:38,208-210`)
and structurally identical to what the oracle's `ToExprValue`
(`cel_cpp_oracle.cc:269`) does for scalars — but we target `cel::Expr`
(literal AST), not `cel.expr.Value`. Out-of-subset result types
(message, opaque, optional) are NOT produced as literals — leave those
subtrees unfolded (§6.3).

## 5. The m31 / m32 seam — we generalize the input, not the materialization

```
   variable-free subtree
            │
   FoldConstants (m33)  ── evaluate via cel-cpp runtime ──┐
            │                                              │
   error/unknown? ──yes──> leave subtree (runtime path)   │
            │ no                                           │
            ▼                                              ▼
   scalar → kConstant            aggregate → literal kCreateList/kCreateMap
            │                                              │
            └──────────────────────┬───────────────────────┘
                                   ▼
          ┌──── m31 const-subtree annotation (UNCHANGED) ────┐
          │ "every element/key is a literal or marked-const  │
          │  aggregate, recursively?"  (m31 §6)              │
          └────────────────┬─────────────────────────────────┘
                  yes       │
                            ▼
            StaticMemoryBuilder::Materialize{List,Map}   (m31, byte-identical)
                            │
                  m32.B: bake SwissTable index           (m32, for maps)
                            ▼
                  node lowers to a single i32.const
```

- **m33 depends on m31.** m33 produces *more* fully-literal aggregates;
  the byte-materialization that consumes them is entirely m31's
  `MaterializeList`/`MaterializeMap` + the §6 const-subtree annotation
  pass. m33 writes **no** materialization bytes — it only widens what
  reaches the materializer.
- **m31's eligibility predicate needs no widening after m33 runs.** This
  is the clean factoring: m33 rewrites a foldable subtree into a *literal*
  before m31's predicate runs, so m31 still only ever sees literals. m31
  §6's "every element/key is a literal or already-materialized const
  aggregate" is satisfied by construction. (Contrast the user's framing
  of "widen m31's predicate from is-literal to is-variable-free": doing
  the widening *inside* m31 would force m31 to also do the evaluation —
  better to keep evaluation in m33's pass and leave m31's predicate
  literally about literals.)
- **m32 pairs for maps.** A folded constant *map* is a fully-literal
  `kCreateMap` → m31 materializes it → m32.B (`m32-swisstable-map-index.md`
  §7) bakes the O(1) SwissTable index over its compile-time-constant
  keys. m33 changes nothing in m32: m32.B already keys off "m31 gates on
  constant keys+values; reuse that gate" (m32 §7), and a folded map's
  keys/values are constants. **Dependency order: m31 → {m33, m32.B}.**
  m32.A (runtime-built index) is independent of both.

**Scalar folds need no new builder surface.** A folded scalar becomes a
`kConstant`; `StaticMemoryBuilder`'s existing `AllocateInt/AllocateString/…`
(`static_memory_builder.h:54-71`) already pack it. Only aggregate folds
ride m31's `Materialize*`. So **m33 against scalars is shippable the day
the value→`kConstant` converter lands, before m31** — `size([1,2,3])`,
`'x'+'y'`, `1+1`, `base64.encode(b'hi')` all fold to a scalar `kConstant`
with zero dependency on m31. The aggregate folds (`[1+1, 2]`) wait on
m31. Recommend slicing m33.A (scalar folds, no m31 dep) and m33.B
(aggregate folds, needs m31).

## 6. Semantics that folding MUST preserve

The governing rule: **a folded subtree must produce exactly what a
runtime eval would** — same value, or the same observable error, or (if
neither can be guaranteed) stay at runtime. cel-cpp is the reference; the
oracle (`testdata/cel_cpp_oracle_test.cc`) is the empirical tiebreaker
(CLAUDE.md). Every rule below is either a cel-cpp source fact or an
oracle case to pin before merge.

### 6.1 Error literals do NOT fold — they stay runtime

`1/0`, `0/0`, `int` overflow (`9223372036854775807 + 1`),
`dyn`-type errors, `bytes`→`string` decode failures: cel-cpp's folder
leaves an erroring const subtree unfolded so the error surfaces at
runtime with the runtime contract (`constant_folding.cc:220-225`). **We
do the same** — §4.2 step 3. Rationale: an error is a first-class CEL
value-level outcome that must propagate with runtime semantics (e.g. it
can be absorbed by a `?:` or a `\|\|` higher up — which is exactly why §6.2
keeps short-circuit ops unfolded). A const subtree that errors must
**never crash the compiler**: the cel-cpp runtime returns the error as a
non-OK `Evaluate()` result or an `ErrorValue` (oracle classifies both,
`cel_cpp_oracle.cc:264-268,307-312`), our helper checks `.ok()` + `IsError`
and bails to "leave unfolded." Oracle pins to add:
`1/0`, `9223372036854775807+1`, `0/0`, `1%0` — confirm each is an error,
then assert our pipeline leaves them as runtime expressions (a folded
`kConstant` carrying an error is **not** a representable AST node, so the
"leave unfolded" path is the only correct one).

### 6.2 Short-circuit / logical ops are NOT folded

`true \|\| x`, `false && x`, `cond ? a : b` — cel-cpp's folder explicitly
declines `kAnd`/`kOr`/`kTernary` (`constant_folding.cc:139-141`,
"Short Circuiting operators not yet supported"). We mirror this: **do not
fold a subtree rooted at `&&`/`\|\|`/`?:`**, even when fully constant. This
also sidesteps the commutative-absorptive error semantics
(`true \|\| (1/0)` is `true`, `false && (1/0)` is `false`, but
`(1/0) \|\| false` is an error) — folding the *operands* is still allowed
(a const operand folds), but the logical node itself stays. (langdef
"Logical Operators" — commutative/absorptive; do not assert the exact
table from memory, pin with oracle: `true \|\| (1/0)`, `(1/0) \|\| true`,
`false && (1/0)`, `(1/0) && false`.) Because m33 only folds *maximal*
foldable subtrees and a logical node is `kNonConst`, its const operands
are themselves maximal foldable subtrees and fold independently — correct.

### 6.3 Macros and comprehensions — expanded by the parser, NOT folded

`has(...)`, `all/exists/exists_one/map/filter` are macros the **parser**
expands into `kComprehensionExpr` (+ `cel.@block` etc.) **before**
type-check. cel-cpp's folder never folds a `kComprehensionExpr`
(`constant_folding.cc:113-116`; comprehension slot count 0 at `:96`,
with the comment that it "can't detect if the comprehension variables are
only used in a const way"). **So a variable-free comprehension like
`[1,2,3].map(x, x*2)` is NOT folded by cel-cpp, and m33 does not fold it
either.** It compiles through our existing comprehension codegen and runs
per-Eval. (This is a deliberate scope boundary, §7 — a future milestone
could fold provably-const comprehensions, but it inherits cel-cpp's open
problem and gains us nothing m31 doesn't for the literal `[1,2,3]` range.)
Oracle pin: `[1,2,3].map(x,x*2)` evaluates to `[2,4,6]` (confirms the
*value* for the eventual fold), AND a codegen golden asserting m33 leaves
the comprehension node intact today.

`cel.bind` (bindings_ext) expands to a degenerate comprehension
(`parse_and_check.cc:477-485`, `IsCelBindShape`) → `kComprehensionExpr`
→ not folded. Consistent.

### 6.4 Heterogeneous equality, overflow, rounding — exact because we reuse cel-cpp

The entire reason to evaluate through cel-cpp rather than reimplement
(§4 option (b) rejected): `1 == 1u`, `1 == 1.0`, `int` overflow traps,
`double`→`int` rounding/truncation (`int(-2^63.0)` is a range error per
the oracle precedent in CLAUDE.md), `uint` wraparound rules — all are
the emergent product of parser + checker + runtime dispatch. We get them
right by construction because the *same* cel-cpp runtime evaluates the
fold. Oracle pins (representative, extend per case touched): `1 == 1u`,
`1u == 1.0`, `int(-9223372036854775808.0)` (range error → unfolded),
`9223372036854775807 + 1` (overflow → unfolded), `2.5 + 2.5`,
`'café'.size()` (UTF-8 codepoint count), `b'\xff\xfe'.size()`.

### 6.5 Static-subset discipline — fold only subsets we can re-emit

m33 produces literal AST nodes that flow into `RejectDyn` and codegen.
Fold only when the result type is in the static subset *and* representable
as a literal (§4.2 step 4): scalars, and lists/maps of foldable elements.
A subtree whose cel-cpp result is a message, opaque/abstract type, or
optional is **left unfolded** — there is no literal AST form for it that
our materializer/codegen handles (m31 §6 already excludes struct literals;
m33 inherits that). Note this is conservative, not a correctness risk:
leaving it unfolded just keeps today's runtime path.

## 7. Scope boundaries / risks (explicitly out of scope)

- **Custom-function calls (host-side impls).** A foldable-looking call to
  a host custom function (`compiler/celfn`) cannot be evaluated at compile
  time — the impl lives in the embedder's host, unavailable to the
  compiler. cel-cpp models exactly this as **lazy overloads**, which its
  folder declines (`constant_folding.cc:152-156`). m33 MUST treat any
  call resolving to a celfn/lazy/contextual overload as `kNonConst`. Our
  helper's cel-cpp runtime won't have these registered, so an attempted
  fold would *fail to plan* — but we must not even attempt: gate on the
  overload-id being a stdlib/extension overload the cel-cpp runtime we
  build (§4) actually registers (the oracle's `RegisterRuntimeExtensions`
  set, `cel_cpp_oracle.cc:87-104`: optional/strings/math/encoders +
  standard). A call to anything outside that set → leave unfolded.
- **Comprehensions / macros** — §6.3. Out of scope.
- **Short-circuit/ternary roots** — §6.2. Out of scope (operands still fold).
- **Struct/message/opaque/optional results** — §6.5. Out of scope.
- **Non-deterministic / context builtins.** CEL has no nondeterministic
  builtins in the static subset (no `now()`/`rand()` — `timestamp`/`now`
  is activation/context-supplied, modeled as a contextual overload, which
  the folder declines, `constant_folding.cc:163-167`). If any extension
  introduces a context-dependent overload, it is `is_contextual()` and
  must not fold — gate identically.
- **Observable-error divergence — the central risk.** The one way folding
  could change behavior is if a subtree errors at runtime but folding
  either (a) silently swallowed the error, or (b) hoisted an error past a
  short-circuit that would have absorbed it. Both are prevented:
  (a) by §6.1 (erroring subtrees stay runtime); (b) by §6.2 (logical
  nodes never fold, so a const erroring operand is reached only when the
  short-circuit would reach it — identical to runtime). The keystone test
  is a differential one: for a corpus of folded-vs-unfolded expression
  pairs, the oracle result MUST equal our pipeline result (value or error
  kind), with folding on. This is the `cel_cpp_oracle_test.cc` extension
  that gates the milestone.
- **Compiler robustness.** A const-fold evaluation that the cel-cpp
  runtime cannot plan/evaluate (unexpected) returns non-OK; the helper
  treats *any* non-OK as "leave unfolded" — folding is a pure
  optimization and never the reason a compile fails. (Same posture as
  cel-cpp's folder, which returns `OkStatus` and skips on any eval
  failure, `constant_folding.cc:223-225`.)

## 8. WAT-first / test plan

Per CLAUDE.md "interface → tests → implementation" and the
per-component coverage doc:

- **No new WAT / ABI surface.** m33 emits the *same* lowering m31/scalar
  codegen already emits (a folded scalar → existing rodata `i32.const`; a
  folded aggregate → m31's single `i32.const`). No new wasm instruction
  shape or host import, so no new `wat/NN_*.wat`. (m31's
  `wat/72_static_aggregate.wat` and m32's index WAT already freeze the
  byte layouts m33 reuses.)
- `compiler/frontend/const_eval_test.cc` (new): the cel-cpp-runtime
  helper — scalar round-trips, error→non-OK, unknown→non-OK, each
  result-kind→AST conversion.
- `compiler/frontend/fold_constants_test.cc` (new): the pass — positive
  (`1+1`→`kConstant 2`; `'x'+'y'`→`kConstant "xy"`; `[1+1, size([1,2,3])]`
  →literal `[2,3]`; `{'a':'x'+'y'}`→literal `{'a':'xy'}`); negative/leave-
  unfolded (`x+1` with `x` a var; `1/0`; `true\|\|y`; `[1,2,3].map(x,x*2)`;
  celfn call; struct literal); idempotence; **maximality** (const operand
  of a non-const parent folds independently).
- `testdata/cel_cpp_oracle_test.cc` additions: every §6 pin above —
  errors-stay-runtime, hetero-equality, overflow/rounding, UTF-8 size,
  comprehension value, short-circuit absorption — asserted folding-on
  against the oracle.
- e2e (`e2e/*_test.cc`): the differential folded-vs-unfolded keystone
  (§7), plus the benchmark deltas — `[1+1, size([1,2,3])]` rebuild cost
  → 0 (rides m31's bench harness).
- Conformance: **no rows change semantics** (folding is value-preserving),
  but folded-constant corpus rows that were previously rebuilt per-Eval
  should now compile to a single `i32.const` — add a focused golden, not
  a new `.textproto` baseline shift (unless m31's larger-literal unlocks
  apply, which are m31's to claim).
- Update `doc/implementation-plan/testing-checklist.md` and
  `per-component-test-coverage.md` rows.

## 9. Future work

- **Const comprehensions over const ranges.** `[1,2,3].map(x, x*2)` is
  variable-free in the user-visible sense but expands to a comprehension
  cel-cpp's folder won't touch (§6.3). A future pass could fold a
  comprehension whose range is const and whose iter/accu vars are used
  only in const ways — but it inherits cel-cpp's stated open problem
  ("can't detect if comprehension variables are only used in a const
  way", `constant_folding.cc:114-116`). Out of scope here; surface only.
- **Const-subtree dedup.** Two equal folded constants could share one
  materialization (m31 §9 #2 raises the same for literals; m33 widens the
  population of dedup candidates). Decide with m31.
- **Folding into short-circuit constant-true/false simplification.** A
  future milestone could rewrite `true \|\| x` → `true` (dropping `x`'s
  codegen) once the short-circuit error semantics are encoded — cel-cpp
  declines this today (§6.2); we inherit the decline. Surface only.
- **Re-emitting opaque/message constants.** If a later milestone gains a
  literal AST form for an opaque/abstract constant (e.g. a folded
  `net.IP` value), §6.5's exclusion could narrow. Not now.
