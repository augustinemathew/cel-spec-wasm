# m33 — compile-time constant folding (all types)

Status: plan — drafted 2026-06-14, not yet started.

> Retitled from the materialization-centric framing. Materialization
> (m31) and the SwissTable index (m32) are now **one of the lowering
> paths** a fold result can take, not the definition of folding. The
> unit of folding is "a variable-free, error-free-or-error-preserving
> subtree of **any** type," and the **result type** decides how the
> folded value is emitted.

## 1. Problem

A CEL expression frequently contains subtrees whose value is fixed at
compile time — every leaf is a literal, no `ident` / free variable / no
host function reaches into them — yet today our pipeline emits the full
runtime call sequence for each one and recomputes it on every `Eval`:

  - **Scalars.** `1 + 1`, `2.0 * 3.0`, `"a" + "b"`, `true && false`,
    `timestamp("2024-01-01T00:00:00Z")`, `size("héllo")` all lower to
    a kCall that, at runtime, loads two rodata operand frames, calls a
    kernel (`cel_int_add_*`, `cel_string_concat`, …) into a workspace
    slot, and returns that slot's offset. The answer never changes
    between evals, but the kernel runs every time, and the operand
    frames + the workspace slot cost rodata + workspace bytes.
  - **Aggregates.** Constant list/map literals are rebuilt in the
    dlmalloc arena on every `Eval` — the headline m31 problem
    (`size({…100-entry int map})` 126 µs vs cel-cpp's 3.4 µs).

These are the same problem at two type granularities: **a
variable-free subtree is recomputed at runtime instead of being
collapsed to a constant at compile time.** m31/m32 solved the
aggregate half by materializing the in-memory representation into
static data and lowering the aggregate node to a single `i32.const`.
m33 generalizes the *front* of that pipeline — recognizing the
variable-free subtree and computing its value at compile time — to
**every CEL type**, and routes the result to the lowering path its
type dictates.

## 2. Design in one sentence

A compile-time IR pass identifies every **maximal variable-free
subtree**, evaluates it once at compile time with semantics
**byte-identical to a runtime eval** (errors, short-circuit, overflow,
heterogeneous equality, comprehension macros over constant ranges all
preserved), and **replaces the subtree with its folded value**; the
folded value's **result type** then selects the lowering:

  - **scalar** (int, uint, double, bool, string, bytes, duration,
    timestamp, null) → a single constant: an `i32.const` /
    `i64.const` / `f64.const` for the inline-able kinds, or an
    `i32.const` pointing at a rodata CelValue frame (+ payload blob for
    string/bytes/duration/timestamp) — collapsing the whole subtree's
    call sequence to a constant load;
  - **list / map** → m31 materialization (header + entry run + payloads
    in static memory), plus the m32 SwissTable index when the result is
    a map — the aggregate node lowers to a single `i32.const` at the
    materialized header.

**Materialization is the downstream consumer of an aggregate fold
result, not the definition of folding.** The fold pass produces a typed
constant value; m31 is the emitter the pass calls when that value is an
aggregate, exactly as the scalar-const emitter is the one it calls when
the value is a scalar.

## 3. Pipeline flow

```
              checked AST (CheckedExpr) → our typed IR + annotations
                                  │
        ┌──── m33 IR pass: fold maximal variable-free subtrees ────┐
        │  bottom-up: a node is foldable iff every child is        │
        │  foldable AND the node itself is fold-safe (§5) — i.e.   │
        │  no ident/free var, no lazy/contextual/host overload,    │
        │  and its eval is error-preserving                        │
        └───────┬────────────────────────────────────────────────┘
            yes │ evaluate subtree once at compile time → CelValue
                ▼
        ┌────── lowering dispatch on RESULT TYPE ──────┐
        │                                              │
   scalar value                              list / map value
        ▼                                              ▼
  emit a constant:                          m31 MaterializeList/Map
   - inline `i32/i64/f64.const`              (header+run+payloads into
     where the kind fits a wasm               static memory; m32 index
     primitive directly, OR                   appended for maps)
   - `i32.const <rodata frame off>`                 │
     for string/bytes/dur/ts/                        ▼
     mixed-payload frames                    node → i32.const <hdr off>
        │
        ▼
  node → the const instruction
  (entire call subtree erased)

        not foldable (ident/host/error-at-runtime) ──► existing
                                                        per-Eval lowering
                                                        (kCall sequence /
                                                         arena build),
                                                        UNCHANGED
```

## 4. Can we get this "for free" from cel-cpp's folder? (the central question)

The scope expansion makes the cel-cpp-reuse question decisive: **if
cel-cpp's constant folder rewrote all constant subtrees (scalar and
aggregate) into constant `Expr`s in the `CheckedExpr` before our IR is
built, our existing literal lowering would handle the scalar results
for free, and only aggregates would need m31.** It does not. Quantified
below.

### 4.1 What cel-cpp's folder actually is

cel-cpp ships `EnableConstantFolding` (`runtime/constant_folding.cc:74`),
which registers a `ProgramOptimizer`
(`CreateConstantFoldingOptimizer`, `eval/compiler/constant_folding.h:33`)
on the **`RuntimeBuilder`**. The optimizer
(`ConstantFoldingExtension`, `eval/compiler/constant_folding.cc:69`)
runs **during program *planning*** — it walks the flat
`ExecutionPath` plan leaf-to-root (`OnPreVisit`/`OnPostVisit`,
`:177`/`:185`), evaluates the subplan for a constant subtree
(`ExecutionFrame frame(subplan, empty_, …); frame.Evaluate()`,
`:212-219`), and **replaces that segment of the execution plan** with a
constant step.

The load-bearing fact: **it mutates the *program plan*, not the
`CheckedExpr`.** The AST that flows into our `frontend/` → `ir/` is the
checker's output, which the runtime-side folder never touches (it lives
strictly downstream, inside cel-cpp's own `RuntimeBuilder`, which we do
not use — our codegen *is* our "runtime builder"). There is **no
CheckedExpr→CheckedExpr constant-folding rewrite** in cel-cpp to reuse:
`tools/` and `parser/` have no AST-folding pass (grep confirms only the
plan-time optimizer and its `runtime/` façade exist). So **nothing
falls out for free along the "folded constant Exprs in the CheckedExpr"
path — that path does not exist.** We must author the fold pass
ourselves, in our IR.

### 4.2 What we can still borrow, and what cel-cpp's folder deliberately won't do

Even though we cannot consume its *output*, cel-cpp's folder is the
**reference for what is safe to fold and how**, and several of its
decisions are constraints we must copy (see §5). Concretely, its
`IsConstExpr` (`eval/compiler/constant_folding.cc:107`) classifier
shows what upstream considers foldable — and the carve-outs are the
interesting part:

  - **Short-circuit operators are NOT folded.** `kAnd`, `kOr`,
    `kTernary` → `kNonConst` (`:138-142`). cel-cpp folds them at the
    plan level only through its non-strict step machinery, not as
    constant subtrees. We *can* fold them (a fully-constant
    `true && false` has a defined value) but **only by honoring 3VL
    short-circuit semantics during the compile-time eval** (§5).
  - **Comprehensions are NOT folded.** `kComprehensionExpr` →
    `kNonConst` (`:113-116`), and `kComprehensionSlotCount = 0`
    (`:96`) — upstream "can't detect if the comprehension variables are
    only used in a const way." Our pass *may* fold a comprehension whose
    range is a constant aggregate and whose body is variable-free w.r.t.
    everything except the (bound) iter var — but this is **net-new work
    we own**, not a reuse, and it must be oracle-pinned (§5, §7).
  - **Empty list / empty map are NOT folded** (`:123-124`, `:128-131`)
    to preserve comprehension list/map-append optimizations. m31 already
    excludes empties; keep that.
  - **Runtime errors are NOT folded.** If the compile-time eval errors,
    cel-cpp **leaves the plan unchanged so the error fires at runtime**
    (`:221-225`: "If this would be a runtime error, then don't adjust
    the program plan… to preserve the evaluation contract"). This is the
    single most important semantics rule we inherit — see §5.
  - **Lazy / contextual overloads are NOT folded** (`:151-167`): an
    overload bound at activation time, or a contextual one, is not a
    compile-time constant. Our analogue: any subtree reaching a **host
    function** (`cel:host`) or a **registered custom fn** that isn't
    purely compile-time-evaluable is non-foldable.
  - **`UnknownValue` results are NOT folded** (`:227-229`). N/A for us
    at fold time (we have no activation), but the principle stands: only
    a concrete value folds.

### 4.3 Quantification: free vs. must-add

| Capability | Free from cel-cpp? | What m33 must add |
|---|---|---|
| Folded constant Exprs in the CheckedExpr we consume | **No** — folder is plan-time, not AST-rewrite | The entire fold pass (IR-level) |
| Scalar fold result → const instruction | **No** | Compile-time evaluator for scalar overloads + scalar-const emitter (reuses existing rodata-frame lowering for string/bytes/dur/ts) |
| Aggregate fold result → static memory | Partially — **m31/m32 already built the emitter** | The fold pass that *feeds* m31 a const aggregate value (m31 today gates on a syntactic "all-literal-children" check; m33 generalizes that gate to "evaluates to a constant aggregate") |
| Safety rules (no short-circuit naïvely, no errors, no comprehension naïvely, no lazy/host) | **Reference only** — copy the decisions, not the code | Re-implement each rule in our pass, oracle-pinned |

Net: the **aggregate emitter is already in flight** (m31 §2, m32 §7),
so the aggregate half of m33 is mostly "widen m31's eligibility gate
from syntactic-literal to evaluates-to-constant and call the existing
materializer." The **scalar half is net-new**: a compile-time evaluator
over the foldable overload set plus a scalar-const emitter. The
**reused literal lowering** (`EmitKConstLoad`,
`compiler/codegen/expr_lower.cc:108`) is what the scalar emitter targets
for string/bytes/duration/timestamp frames — a folded `"a"+"b"`
produces the *same* rodata CelValue frame a literal `"ab"` would, so the
emitter is the existing one; only the *value* is computed by folding
instead of parsed from source.

> Implementation choice surfaced, not pre-committed: the compile-time
> evaluator can either (a) re-use cel-cpp's runtime by calling
> `EnableConstantFolding` on a throwaway `RuntimeBuilder`, planning the
> subtree, and reading back the folded constant from the plan — using
> cel-cpp purely as an oracle-grade evaluator at compile time — or
> (b) evaluate against **our own** kernels (`runtime/cel_runtime.c`
> compiled natively) so the folded value is bit-identical to what our
> runtime would produce. (b) is the safer choice for byte-identity with
> m31's keystone test and avoids a second semantics surface, but (a) is
> the faster path to coverage. Decide in implementation against the
> oracle; whichever is chosen, the result MUST match the oracle (§7).

## 5. Semantics preservation (now across ALL types)

The fold is sound **only if the folded value is exactly what a runtime
eval would produce** — including producing *no fold* where a runtime
eval would error. Every rule below is the same one m31 carried for
aggregates, now applied across all types; each is oracle-pinned.

  1. **Errors are preserved by NOT folding.** If the compile-time eval
     of a subtree errors, leave the subtree on its runtime lowering so
     the error fires at eval time with today's exact kind/semantics —
     verbatim cel-cpp's rule (`eval/compiler/constant_folding.cc:221`).
     Folding an error into a "constant error" is wrong: a CEL error is a
     value that propagates lazily, and short-circuit / ternary can
     *discard* it (`true || (1/0 == 0)` is `true`, not an error). So the
     poison must stay a runtime event, reachable only on the path that
     actually evaluates it.
     - **Scalar oracle pin:** `1 / 0` → CEL error
       (`testdata/cel_cpp_oracle_test.cc:153`,
       `DivByZeroSurfacesAsCelError`, already present). m33 MUST NOT
       fold `1/0` to anything; the kCall stays. Add the companion e2e
       assertion that `1/0` still poisons at runtime post-m33.
     - **Overflow oracle pin (new case to add):**
       `9223372036854775807 + 1` (`INT64_MAX + 1`) → CEL error
       (range/overflow). Add to `cel_cpp_oracle_test.cc`; assert m33
       leaves it unfolded. Likewise `0 - 9223372036854775808` and
       `int(1e19)`.
  2. **Short-circuit / 3VL is preserved.** A fully-constant
     `true && false` → `false`, `false && (1/0 == 0)` → `false` (the
     error arm is never evaluated). Folding `_&&_` / `_||_` / `_?_:_`
     is allowed **only** if the compile-time evaluator implements
     non-strict short-circuit exactly (langdef §"Logical operators",
     §"Conditional expression"): the unfolded arm's potential error must
     not leak into the fold. cel-cpp sidesteps this by not folding these
     at the subtree level (`:138-142`); if we fold them we own the 3VL
     correctness and pin it with oracle cases (`true || (1/0==0)`,
     `false && (1/0==0)`, `(1/0==0) ? 1 : 2` — the last *does* error,
     must not fold).
  3. **Overflow / range is preserved** by rule 1 (overflow is an error,
     so it doesn't fold). Non-overflowing arithmetic
     (`2 + 2`, `2.0 * 3.0`) folds to the exact runtime value.
  4. **Heterogeneous equality is preserved.** `1 == 1.0` → `true`,
     `1 == 2u` → `false`, `1 == "1"` → `false` (cross-type, defined),
     and the mixed-numeric cases must fold to the same bool the runtime
     `cel_*_eq_*` kernels produce. Oracle-pin a representative matrix
     (`1 == 1.0`, `1u == 1`, `2.0 == 2`, `1 == 1u && 1 == 1.0`).
  5. **Comprehension macros over constant ranges.** `[1,2,3].map(x, x*2)`,
     `[1,2,3].filter(x, x > 1)`, `[1,2,3].exists(x, x == 2)`,
     `{1:2,3:4}.all(k, k > 0)` — a comprehension whose range folds to a
     constant aggregate and whose accumulator/step is variable-free
     (modulo the bound iter/accu vars) folds to its result aggregate
     (→ m31) or scalar (→ const). This is the case cel-cpp explicitly
     declines (`:113-116`); it is the highest-value and highest-risk m33
     increment. **Oracle-pin each macro form** and confirm the iter-var
     scoping doesn't accidentally treat the bound var as a free
     variable. Where the body would error for some element
     (`[1,0].map(x, 1/x)`), rule 1 applies — do not fold.
  6. **String/bytes/duration/timestamp canonical forms.** A folded
     `string(1)`, `"a"+"b"`, `b"x"+b"y"`, `timestamp("2024-01-01T00:00:00Z")`,
     `duration("3600s")` must produce the byte-exact payload the runtime
     would (UTF-8 bytes, ms/s encoding). Reuse the runtime kernels for
     the compile-time eval (§4 option b) to guarantee this, and oracle-
     pin the canonical string forms.

**Eligibility gate (the foldable predicate), bottom-up:** a node folds
iff every child folds AND the node is fold-safe — fold-safe excludes
any `ident` / free variable, any `cel:host` import or non-const custom
fn, any lazy/contextual overload, and (until rule 5 lands) any
comprehension. A subtree that contains a *non-foldable* descendant is
not folded, but its **foldable sub-subtrees still fold** (the pass is
bottom-up and maximal: `f(x) + (1+1)` folds the `(1+1)` to `2` even
though `f(x)+…` stays a runtime call). This mirrors m31's recursive
"const-inside-non-const (inner still materializes)" golden test
(m31 §7).

## 6. Lowering paths in detail

### 6.1 Scalar fold result → constant instruction / rodata frame

The result type chooses the encoding, matching how a *literal* of that
type lowers today (`EmitKConstLoad`, `expr_lower.cc:108`, which already
returns `i32.const <rodata frame offset>` for every literal). m33's
scalar emitter writes the folded value into the same rodata-frame shape
the LayoutPass packs for a literal, so:

  - **bool / int / uint / double / null** — a CelValue frame in rodata,
    node → `i32.const <frame offset>` (identical to a literal of that
    value today). Optionally, where a consumer can take an unboxed
    primitive directly (a future peephole), an inline
    `i32.const`/`i64.const`/`f64.const` — but the rodata-frame path is
    the baseline and requires no new ABI.
  - **string / bytes / duration / timestamp** — CelValue frame + payload
    blob in rodata; node → `i32.const <frame offset>`. Byte-identical to
    the frame a source literal of the folded value would get.

The win is **erasing the call subtree**: `"a" + "b"` stops emitting two
operand-frame loads + a `cel_string_concat` call + a workspace slot, and
becomes one `i32.const` at a rodata `"ab"` frame — the same lowering as
if the source had said `"ab"`.

### 6.2 Aggregate fold result → m31 materialization (+ m32 index)

A folded list/map value is handed to
`StaticMemoryBuilder::Materialize{List,Map}` (m31 §5) exactly as a
syntactically-constant literal is today; for maps the m32 SwissTable
index is appended (m32 §7). The node lowers to `i32.const <header off>`.
**m31's keystone byte-identity test (m31 §7) is the guard** that a
folded aggregate's bytes equal an arena-built equal aggregate's bytes —
which is *why* §4 option (b) (fold against our own kernels) is the safer
evaluator choice.

This is the relationship the retitle captures: **m31/m32 are the
aggregate-typed lowering arm of m33's dispatch.** m33 generalizes the
*eligibility* (literal-children → evaluates-to-constant) and adds the
*scalar arm*; m31/m32 remain the aggregate emitter.

## 7. WAT-first + test plan

  - **WAT.** No new ABI surface or instruction shape: a folded scalar
    emits the *same* `i32.const <rodata frame>` (or inline const) a
    literal emits, and a folded aggregate emits m31's `i32.const
    <header off>`. So m33 rides the existing WAT traces
    (literal frames; m31 `72_static_aggregate.wat`); no new `.wat`
    file is required for the lowering shape. The fold pass is an
    IR transform, validated by golden + oracle tests, not by a new WAT.
  - **Oracle (mandatory, before the pass is written).** Extend
    `testdata/cel_cpp_oracle_test.cc` with the §5 matrix — at minimum:
    - error-preserving: `1 / 0` (present, `:153`),
      `9223372036854775807 + 1`, `0 - 9223372036854775808`,
      `int(1e19)`, `[1,0].map(x, 1/x)` — each must show
      `is_error`, and the corresponding m33 test asserts **no fold**.
    - short-circuit: `true || (1/0 == 0)` (value `true`, no error),
      `false && (1/0 == 0)` (value `false`), `(1/0 == 0) ? 1 : 2`
      (error → no fold).
    - heterogeneous eq: `1 == 1.0`, `1u == 1`, `2.0 == 2`, `1 == "1"`.
    - comprehension macros: `[1,2,3].map(x, x*2)`,
      `[1,2,3].filter(x, x>1)`, `[1,2,3].exists(x, x==2)`,
      `{1:2,3:4}.all(k, k>0)`.
    - canonical forms: `"a"+"b"`, `string(1)`, `b"x"+b"y"`,
      `timestamp("2024-01-01T00:00:00Z")`, `duration("3600s")`.
    Per CLAUDE.md the oracle is the authority on every expected value
    here; reason none of them out by hand.
  - **Fold-pass unit tests** (`compiler/ir/<fold>_test.cc`, interface →
    tests → impl): the foldable predicate over the full positive
    matrix (every scalar kind, list, map, nested, comprehension-macro)
    and the full negative matrix (ident anywhere; host/custom fn
    anywhere; error-producing subtree; short-circuit with error in the
    discarded arm; const-inside-non-const folds only the inner).
  - **Codegen golden tests** (`expr_lower_test.cc`): folded scalar →
    single const instruction / one rodata-frame `i32.const` (assert the
    call sequence is *gone*); folded aggregate → m31 single `i32.const`;
    non-foldable → unchanged kCall / arena build.
  - **Byte-identity (keystone, inherited from m31 §7):** a folded
    aggregate's static bytes `memcmp`-equal the arena-built equal
    aggregate's; a folded scalar frame `memcmp`-equals a source-literal
    frame of the same value.
  - **e2e** (`e2e/*_test.cc`): `"a"+"b"` evaluates to `"ab"`; `1+1` →
    `2`; `[1,2,3].map(x,x*2)` → `[2,4,6]`; **`1/0` still poisons at
    runtime** (the negative e2e proving the no-fold rule held end to
    end); `true || (1/0==0)` → `true`.
  - **Conformance.** Semantics are unchanged — m33 is a pure
    optimization, so existing rows must stay green and no expecteds
    change. Add focused rows only for the previously-uncompilable cases
    m31 newly admits (large literals); folding itself adds no new
    expected values.
  - **Benchmarks.** `benchmark/eval`: a scalar-heavy const expr (e.g. a
    long `1+2+3+…` chain), a `"a"+"b"+…` concat chain, and a
    const-comprehension (`[1..100].map(...)`-shaped) before/after;
    expectation: runtime cost → ~0 (one const load), matching the m31
    aggregate result. Use `kBenchOptimizeLevel` per the benchmark-config
    rules.

## 8. Files touched

Per feature-pipeline-checklist §2.5 (new lowering, no new AST kind) plus
a new IR pass:

  - `compiler/ir/` — **new** const-fold pass (`.h` + `.cc` + `_test.cc`):
    the bottom-up foldable predicate, the compile-time evaluator (or the
    cel-cpp/own-kernel evaluator façade per §4), producing a typed
    `CelValue` annotation on each folded node. New `cc_library`,
    `//:internal` visibility.
  - `compiler/codegen/expr_lower.cc` — the scalar-const emitter arm
    (targets the existing `EmitKConstLoad` rodata-frame path for
    string/bytes/dur/ts; emits inline/rodata const for the rest);
    aggregate folds route to the existing m31 `Materialize*` call.
  - `compiler/codegen/layout_pass.cc` — pack a folded node's value into
    rodata (scalar frame) / the materialized window (aggregate), so the
    annotation's `storage` is set before `Emit` runs (mirrors how a
    literal is packed today).
  - `compiler/codegen/static_memory_builder.{h,cc}` — widen m31's
    eligibility gate from "all children are syntactic literals" to
    "node folded to a constant aggregate"; the materializer body is
    unchanged (it already takes a constant aggregate value).
  - `testdata/cel_cpp_oracle_test.cc` — the §7 oracle matrix.
  - `compiler/ir/<fold>_test.cc`, `expr_lower_test.cc`,
    `e2e/*_test.cc` — the §7 test matrix.
  - Docs: tick `testing-checklist.md` rows; update
    `feature-pipeline-checklist.md` running example if used; reconcile
    m31 §6 (its syntactic eligibility gate is now a special case of
    m33's fold predicate) and m32 §7 (const-key gate now reads "folds to
    a constant map") in the same commit m33 lands.

## 9. Open questions (for the user — not pre-committed)

1. **Compile-time evaluator backend** — cel-cpp runtime as a
   compile-time oracle (§4 option a) vs. our own native kernels
   (option b). Byte-identity with m31 favors (b); time-to-coverage
   favors (a). Settle against the byte-identity keystone test.
2. **Comprehension-macro folding scope** (§5 rule 5) — ship in m33, or
   split to m33.B after the scalar + aggregate-literal fold lands? It's
   the case cel-cpp declines and carries the most semantic risk
   (iter-var scoping, per-element error preservation).
3. **Inline-primitive peephole** (§6.1) — emit bare
   `i32/i64/f64.const` where a consumer takes an unboxed primitive, or
   always go through the rodata CelValue frame? The frame path is the
   zero-new-ABI baseline; the peephole is a follow-up perf slice.
4. **Dedup identical folded constants** into one rodata frame /
   materialization (cheap, shares m31 §9 #2's question).

## 10. Three compile-time execution engines compared — cel-cpp folder vs. Binaryen `precompute` vs. Binaryen `wasm-ctor-eval`

The user raised a third avenue: "Binaryen has a way to partially run a
wasm module." There are in fact two distinct Binaryen mechanisms, and
this section evaluates BOTH head-to-head against the cel-cpp host-side
folder (§3–§4, the recommended primary path). They are **not mutually
exclusive** — the recommendation at the end is a layered pipeline that
uses two of the three.

> **Label note.** The `(a)/(b)/(c)` in this section label the three
> execution *mechanisms* below (cel-cpp folder / `precompute` /
> `wasm-ctor-eval`) — distinct from §4's `(a)/(b)` *evaluator-backend*
> options (cel-cpp runtime vs. our own kernels), which live *inside*
> mechanism (a).

> **Verifiability note.** Binaryen's source is **not on disk** in this
> checkout. `MODULE.bazel:83-91` pins it as an `http_archive` of the
> GitHub release tarball `binaryen-version_129`, fetched on demand and
> built via CMake through `rules_foreign_cc`
> (`third_party/binaryen/BUILD.external.bazel:20-35`). So every claim
> below about the C API and the CLI tools is grounded in **what our
> BUILD files demonstrably expose/consume** plus the well-known shape
> of Binaryen's API as of v129; where a claim depends on Binaryen
> *internals* not read here (the `precompute` interpreter's effect
> model, ctor-eval's import handling), it is flagged as an
> **assumption to verify by fetching the tarball** (then read
> `src/passes/Precompute.cpp` and `src/tools/wasm-ctor-eval.cpp`).

### 10.1 What we link today (the API-surface fact)

`BUILD.external.bazel:6-8` is explicit: **"We consume only the
officially stable C API (`binaryen-c.h`). The C++ API is not part of
the cmake install target."** The CMake invocation sets
`BUILD_TOOLS=OFF` (`:24`) and `out_static_libs = ["libbinaryen.a"]`
(`:31`) — so we link the library and **none of the standalone CLI
tools are built**. The only optimization-driver call our codegen makes
today is the whole-module optimize pipeline (`module.cc:309-311`:
`BinaryenSetOptimizeLevel` / `BinaryenSetShrinkLevel` /
`BinaryenModuleOptimize`). The C API also exposes
`BinaryenModuleRunPasses(module, passes[], count)` for a **named** pass
list (assumption: confirm the exact symbol on fetch) — the hook through
which mechanism (b) is reachable without leaving the C API.

### 10.2 Mechanism (b): the `precompute` / `precompute-propagate` pass

**What it is.** Binaryen's `precompute` pass interprets *pure* constant
expressions in place and replaces them with their constant result —
`(i32.add (i32.const 1) (i32.const 1))` → `(i32.const 2)`;
`precompute-propagate` additionally propagates known-constant locals.
It runs entirely inside the module's own function bodies.

**Reachability.** Through the C API we already link, via
`BinaryenModuleRunPasses(module_, {"precompute"}, 1)` (assumption: pass
name string is `"precompute"` in v129 — verify on fetch). Zero new
dependency, zero CLI shell-out; slots into `WasmModule::Optimize`
(`module.cc:294`).

**The decisive limit.** `precompute` models the **wasm core** and
**stops at any operation it cannot prove pure** — including **every call
to an import**. Our aggregate-construction sequence is a run of imports
(`cel.cel_list_create`, `cel.cel_map_insert`, `cel.cel_map_index_build`,
…, per `expr_lower_test.cc:155-208`, design.md:563-635) that **write
linear memory through the imported arena allocator**. `precompute` folds
none of them — it can model neither the import effect nor the
cross-module memory write. The user's intuition is correct. What it
*can* tidy is a pure-wasm scalar-arithmetic tail — but after the cel-cpp
folder (§4) collapses the semantic scalar layer to `kConstant`, little
remains. **Verdict (b):** cheap, additive, low-value once (a) is in
place; a downstream scalar tidy, not an aggregate materializer, and it
does nothing for the m31/m32 byte-layout.

### 10.3 Mechanism (c): `wasm-ctor-eval` linear-memory snapshot

**What it is (the important one).** `wasm-ctor-eval` interprets entire
functions at compile time and **snapshots the resulting linear-memory +
global state into the module's data segments**, replacing the evaluated
work with the constant result. If our aggregate-construction sequence
could be ctor-eval'd, the arena bytes it writes — list/map headers,
entry runs, AND the m32 SwissTable index built by the **real runtime
kernel** — would be snapshotted automatically, uniformly for every
result type.

**Why it is attractive (assessed).**

- *It could subsume m31's hand-written byte layout AND m32's
  shared-hash-kernel problem.* Real and large **if feasible**. m32 §6
  requires a **shared hash kernel** (`runtime/cel_map_hash.h`) compiled
  into BOTH runtime and compiler with **byte-identical** hashing as a
  load-bearing invariant, existing *only* to avoid compiler↔runtime
  drift. Under ctor-eval the index is built by the **actual kernel** and
  snapshotted — so the whole `cel_map_hash.h` apparatus + its test
  **would not need to exist**, and m31's `StaticMemoryBuilder` byte
  emitters shrink to "call the real kernel under the interpreter." This
  is the single biggest m31+m32 simplification — it removes the
  highest-risk correctness surface (hand-mirrored layout + hand-mirrored
  hashing).
- *Uniform across all types* — ctor-eval snapshots whatever memory/return
  the wasm produced, scalar through map. Matches the "all expressions" ask.

**The make-or-break blocker: ctor-eval stops at imports, and our
construction path is nothing but imports into a separate module.** Three
compounding, independently-fatal problems:

1. **Two-module split.** The expr module **imports**
   `arena_alloc`/`cel_list_create`/`cel_map_insert`/`cel_map_index_build`/
   `cel.memory` from the **separately built** `cel_runtime.wasm`, linked
   **at instantiation time by wasmtime**, never merged at compile time
   (design.md:354-356, :563, :2274). ctor-eval interprets *one* module and
   **cannot follow a call into an import** — the first
   `call $cel.cel_list_create` stops it cold. Using ctor-eval at all would
   first require **statically linking runtime + expr into one module at
   compile time** — a reversal of the Phase C memory design that
   deliberately moved *away* from expr-owned memory (design.md:293-305).
2. **Allocator + memory state.** Even merged, ctor-eval must interpret the
   bump arena allocator and wasi-libc static data; anything touching
   `memory.grow`, WASI imports, or **threads** (the runtime is
   `wasm32-wasi-threads`, design.md:22) likely makes it bail. The threads
   build is a specific red flag for an interpreter (verify on fetch).
3. **Host imports.** The path can call `cel_log` (host trampoline) — a
   no-op stub is *probably* sound, but every import touched needs a
   faithful model or a proven-sound stub (per-import work).

**Error subtrees don't come free.** A wasm trap → ctor-eval abandons that
ctor and leaves it in the module (≈ "leave unfolded", aligns with §5). But
a kernel that returns a well-formed CEL **error value** (`1/0`) would be
snapshotted as a frozen error value — exactly wrong (§5: an error must
propagate with runtime semantics, e.g. absorbable by a higher `?:`). So
even feasible ctor-eval still needs the §5 eligibility gate in front.

**Determinism.** Snapshotting concrete bytes means baked arena offsets,
uninitialized struct padding (the SwissTable control region, map-entry
padding), and host/clock/threads influence all threaten reproducible
builds — the same byte-zeroing discipline m31's `StaticMemoryBuilder`
applies by hand, relocated into "trust the kernel + interpreter." LE is
already guarded (removes the endianness axis, none of the others).

**Reachability.** `wasm-ctor-eval` is a **standalone CLI tool**
(`src/tools/wasm-ctor-eval.cpp`); with `BUILD_TOOLS=OFF` it is **not
built and not exposed through `binaryen-c.h`**. Integration is one of:
(i) shell out to a built binary (flip `BUILD_TOOLS=ON`, marshal wasm via
temp files — a process boundary in the compile hot path); (ii) vendor the
tool's internals (couples us to churning Binaryen internals the C-API
discipline avoids); (iii) reimplement an interpreter (non-starter). All
materially more expensive than (a)/(b).

**Verdict (c):** the *idea* is the strongest of the three — the only one
that could erase m32's shared-hash-kernel risk and unify all types under
one materializer. But against the **as-built two-module, wasi-threads,
instantiate-time-linked** architecture it is blocked by the import
boundary, needs a compile-time static-link reversal, per-import
models/stubs, still needs the §5 error gate, and is reachable only via
CLI shell-out or internals-vendoring. A **large speculative bet**, not a
near-term simplification — worth a dedicated feasibility probe before it
is ever scheduled.

### 10.4 Side-by-side

| | (a) cel-cpp host folder (§3–§4) | (b) Binaryen `precompute` | (c) Binaryen `wasm-ctor-eval` |
|---|---|---|---|
| **Reachable via API we link** | yes (vendored cel-cpp runtime, like the oracle) | yes (`BinaryenModuleRunPasses`, C API) | **no** (CLI tool; `BUILD_TOOLS=OFF`) |
| **Folds scalars** | yes (value-exact via cel-cpp) | only pure-wasm arithmetic tails | yes (if feasible) |
| **Folds aggregates (list/map bytes)** | yes — literal `kCreateList/Map`, m31 bakes | **no** (stops at kernel-call imports) | yes in principle — **but blocked** |
| **Builds m32 SwissTable index** | no — relies on m32's shared hash kernel | no | **yes**, via the real kernel — would erase m32 §6 |
| **Byte-exact semantics** | exact (same cel-cpp runtime) | n/a (numeric only) | exact (real kernel) — error-value snapshot is a hazard |
| **Error semantics (§5)** | handled — erroring subtree left unfolded | n/a | **not free** — needs §5 gate; trap≈unfold, error-*value* snapshots wrongly |
| **Blocking issues** | none (recommended) | low value once (a) runs | import boundary + 2-module split + wasi-threads interp + per-import stubs + CLI integration |
| **Net effect on m31/m32 complexity** | unchanged | unchanged | **large reduction** *if* feasible |

### 10.5 Recommended pipeline

**Primary: (a) the cel-cpp host-side folder, per §3–§4.** Reachable
through deps we already link, value/byte-exact against cel-cpp by
construction (the conformance requirement), handles the §5 error
semantics, folds every in-subset scalar and aggregate. The only option
with no blocking issue.

**Adjunct, optional: (b) `precompute` as a post-codegen scalar cleanup.**
After (a) + codegen, optionally `BinaryenModuleRunPasses(module_,
{"precompute"}, 1)` in `WasmModule::Optimize` to mop up residual pure-wasm
arithmetic. Near-zero cost, strictly additive, guarded behind the existing
`level > 0` gate so level-0 golden tests stay byte-stable. Low priority.

**Not now, but track: (c) `wasm-ctor-eval`.** Do **not** schedule against
the current architecture. Its prize (erasing m32's shared hash kernel,
unifying all types) is real but gated on reversing the Phase C two-module
split into a compile-time static-link plus per-import interpreter
feasibility — a milestone-sized investigation, not an m33 deliverable.
The correct next step is a **throwaway feasibility probe** (per CLAUDE.md
probe discipline) *before* any design commits to it:

> Probe (only if/when the user asks to pursue (c)): fetch the Binaryen
> tarball, read `src/tools/wasm-ctor-eval.cpp` and
> `src/passes/Precompute.cpp` to confirm (1) ctor-eval's behavior on
> imports / traps / `memory.grow` / threads, (2) whether any C-API entry
> exposes it, (3) whether a wasm-merge of `cel_runtime.wasm` + a trivial
> expr module calling `cel_list_create` can be ctor-eval'd at all. Record
> findings with `file:line` citations in a new `mNN-ctor-eval.md` if
> promising; otherwise note the dead-end here.

The recommended shipping shape is **(a) + optional (b)**, with **(c)
deferred** behind that probe. (a) remains load-bearing; nothing in this
section changes the §3–§4 recommendation or the m31/m32 seam in §6.2.
