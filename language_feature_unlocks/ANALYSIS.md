# Language-feature unlock analysis

Status: analysis — drafted 2026-05-16, ahead of next-milestone planning.
Inputs: `compiler_v2/conformance/README.md` (post-M7B + polish, today),
`doc/implementation-plan/rewrite/m5-kcall-comprehensions.md`,
`third_party/cel-cpp/extensions/bindings_ext.cc`.

## 1 Headline

  - **Today**: `pass=1144 / 2454 (46.6%)`, `skip=602`, `fail=708`.
  - **Out-of-scope-by-design** (will never pass): ~317 SKIPs
    (`disable_check` parse-only-eval, `static_subset` `dyn(...)`
    rejection) + ~217 `dynamic.textproto` FAILs.  Effective
    addressable corpus is **~1920 tests**.
  - **Effective pass rate against addressable corpus**: ~60%.

## 2 What "language feature" means here

Per the user, this analysis **excludes extensions**:
math_ext, network_ext, string_ext, encoders_ext, optionals,
proto2_ext, and block_ext.  Those total ~600 of the corpus and
are explicitly deferred.

Included: core CEL features (comprehensions, wrappers, regex,
type-coercion overloads, map-type marshalling) plus the
customer-named follow-up: **`cel.bind()`** (which despite
living in `bindings_ext.textproto` is mechanically a comprehension
special-form — see §6).

## 3 The unlock-priority table

Ordered by **PASS impact** within language-feature scope.
Numbers are ceilings from the README's "Forecast by open
milestone"; actual deltas may be lower if a row needs more
than one fix.

| # | Unlock | Direct PASS | Indirect unlock | Total ceiling | Effort |
|---|---|---:|---:|---:|---|
| 1 | **Core comprehensions** (`exists` / `all` / `exists_one` / `map` / `filter`) | macros.textproto +38 SKIPs, namespace.textproto +6 SKIPs | unblocks #2 and #5 | **~+44 self + chain** | Large — ResolvePass scope handler + `kComprehensionExpr` codegen arm + LayoutPass `Push/PopScope` + dynamic-list primitive (~3.5 sessions per M5 doc) |
| 2 | **Three-arg comprehension forms** (`list.exists(i, v, pred)`, `map.exists(k, v, pred)`) | macros2.textproto +46 FAILs | — | **~+46** | Medium, on top of #1 — checker enrichment + a second comprehension lowering arm |
| 3 | **M8 wrappers** (auto-wrap on construction + wrapper-vs-scalar `==` peel + wrapper auto-unwrap on read) | wrappers.textproto +27, comparisons.textproto `eq_wrapper/*` +27, proto2/3 wrapper-field rows | unblocks `*/to_any` rows post-M7-A | **~+55–60** | Medium — plan exists (`m8-wrapper-types.md`); 9 wrapper types × {construct, eq-peel, unwrap} |
| 4 | **`cel.bind(name, value, body)`** | bindings_ext.textproto +8 FAILs | foundation for customer DSLs | **+8** | **Tiny once #1 ships** — parser-library registration; cel-cpp does the expansion to a degenerate comprehension at parse time |
| 5 | **Map-type / aggregate `type_env` marshalling** | fields.textproto +8 SKIPs, parse +1, comparisons +3 | required for advanced field tests | **~+12** | Small — `binding_marshal::TypeSpecFragment` extension for `map_type` |
| 6 | **`matches(s, regex)` (host trampoline route or in-runtime regex)** | string.textproto +9 SKIPs | — | **~+9** | Variable — see `wasm_compilation_experiments/PLAN.md` for the host-vs-runtime decision; host trampoline route is the fast path |
| 7 | **M5.D step 2 bound-list ops** | lists.textproto +2 FAILs, fields.textproto +6 FAILs (`has({...}.k)`) | — | **~+8** | Small — bound-list-operand path through existing dispatch |
| 8 | **`bool → {int, uint, double}` conversion overloads** | conversions.textproto +5 FAILs | — | **+5** | Tiny — v2 checker decl additions; cel-cpp's runtime registers these but its checker doesn't (we have to add the decl) |
| 9 | **Chained-null read** (null-propagation through unset-message chains) | proto2/3 `nested_message_subfield` | — | **~+2** | Tiny — `ReadField` null-propagation in cel_host |
| 10 | **M9 follow-up** — `check_only:true` typed-result path | type_deduction.textproto +25 SKIPs | — | **~+25** | Small (harness work) — recover deduced type from `cel::Ast::TypeMap` for `check_only` rows |

**Total addressable PASS in this table: ~+215 to ~+219**, taking
the corpus from 1144 → ~1360 (55%–55.5% of total, ~71% of
addressable).

## 4 Critical-path observation

**Comprehensions is the single highest-leverage unlock.**  It's
blocking ~44 direct SKIPs *and* the entire customer ask
(`cel.bind`, item #4) *and* the three-arg comprehension forms in
`macros2.textproto` (item #2).  Without comprehensions:

  - `cel.bind` is impossible (its parser macro generates a
    `kComprehensionExpr` that ResolvePass currently rejects).
  - `macros2` zero-passes (every row uses a comprehension form).
  - `macros` zero-passes (38 of 44 rows use a comprehension form).
  - `namespace` is stuck at 4/14 (the comprehension rows in it
    can't progress).

If we ship **only comprehensions** we go from 1144 → ~1232 PASS
(roughly +88 across the comprehension-dependent fixtures plus
the customer's `cel.bind`).  That's a 7.7% headline jump in
one milestone.

If we ship **comprehensions + M8 wrappers**, we go from
1144 → ~1290 PASS (~+146).  That clears the two biggest
language-feature buckets.

After that we're into the long tail of small unlocks (+5 to +12
each) — still worth doing but not transformative.

## 5 Why comprehensions weren't done in M5

`doc/implementation-plan/rewrite/m5-kcall-comprehensions.md`
explicitly carved comprehensions out mid-flight (line 41):

> Comprehension lowering was scoped out of M5 mid-flight … This
> requires ResolvePass scope handler + `kComprehension` codegen arm +
> LayoutPass `Push/PopScope` machinery, a follow-on milestone
> (`m5-comprehensions-followon.md` — not yet drafted).  M4's
> `ComprehensionDetector` early-reject in ResolvePass means programs
> reaching `kComprehensionExpr` continue to classify as SKIP until
> the follow-on lands.

So the situation today: M5 shipped the kCall built-in overload set,
control flow, message equality — but **not** the comprehension
codegen.  The follow-on milestone was anticipated but never written.

What the follow-on needs (extracted from the M5 doc + cel-cpp's
expansion shapes):

  - **ResolvePass scope handler.**  Comprehensions introduce two
    new variables (`iter_var`, `accu_var`) that shadow outer bindings
    within the comprehension's subtree.  ResolvePass needs a
    push/pop scope stack so name resolution prefers the inner
    binding while inside, falls back to outer when out.
  - **`kComprehensionExpr` codegen arm.**  Lowers to a wasm loop:
    init accumulator, iterate over the range, at each step check
    `loop_cond` (short-circuit exit when false), execute `loop_step`
    to update the accumulator, finally return `result`.  Trampolines
    needed: list iteration (already there via `cel_list_size` +
    `cel_list_at`), map iteration (probably new — keys-of-map).
  - **LayoutPass `Push/PopScope`.**  Comprehension-scope CelValue
    slots need to be allocated within the comprehension's body
    and released at exit.  Affects static memory layout.
  - **Dynamic-list primitive** for the accumulator in `filter`-shaped
    comprehensions (the accumulator grows from `[]`).  The runtime
    has bounded-list ops but not append-to-list — that's a new
    runtime helper.

Engineering size estimate: ~3.5 sessions per the M5 doc.  Has all
been mapped out; what's missing is the formal milestone doc and
the slicing.

## 6 `cel.bind` desugaring — confirmed mechanical

`third_party/cel-cpp/extensions/bindings_ext.cc` lines 52–72:

```cpp
absl::StatusOr<Macro> cel_bind = Macro::Receiver(
    kBind, 3,
    [](MacroExprFactory& factory, Expr& target,
       absl::Span<Expr> args) -> absl::optional<Expr> {
      if (!IsTargetNamespace(target)) return absl::nullopt;
      auto var_name = args[0].ident_expr().name();
      return factory.NewComprehension(
          /*iter_var=*/  "#unused",
          /*iter_range=*/factory.NewList(),         // empty list
          /*accu_var=*/  std::move(var_name),
          /*accu_init=*/ std::move(args[1]),        // value
          /*loop_cond=*/ factory.NewBoolConst(false), // exits immediately
          /*loop_step=*/ std::move(args[0]),        // unused
          /*result=*/    std::move(args[2]));       // body
    });
```

This is a parser-level macro: by the time the AST reaches the
checker, `cel.bind(x, y, z)` is already a `kComprehensionExpr` node
with an empty iteration range.  No special handling in checker /
codegen — it's just a comprehension with `iter_range=[]` and
`loop_cond=false`, so the loop body never runs; the accumulator
is initialised to `value`, kept in scope, and `result` is evaluated
against that scope.

**Implication.**  Once comprehensions land:

  1. Add `@cel-cpp//extensions:bindings_ext` to the parser build.
  2. Call `cel::extensions::BindingsCompilerLibrary().ConfigureParser(builder)`
     when constructing the parser/checker.
  3. The 8 rows in `bindings_ext.textproto` start passing
     automatically.

**Caveat for the degenerate-comprehension fast path.**  A naive
comprehension codegen would still emit the iteration prologue
(zero-length loop check), which is cheap.  An optimised codegen
could detect `iter_range=[]` + `loop_cond=false` at lowering time
and emit just `accu_var := value; result`.  Worth doing if perf
analysis shows this pattern is hot in customer programs (likely
it will be — `cel.bind` is meant for repeated subexpression
elimination, so customers will use it heavily).

## 7 What needs to be done now (analysis-phase deliverables)

This doc is the analysis the user asked for.  The natural
next planning artefact would be:

  - **Draft `doc/implementation-plan/rewrite/m5-comprehensions-followon.md`**
    with slice-level breakdown (Slice A: ResolvePass scope; Slice B:
    `kComprehension` codegen; Slice C: `filter` + dynamic-list;
    Slice D: macros2 three-arg forms; Slice E: `cel.bind` parser
    registration).
  - **Per-slice WAT traces** under `doc/implementation-plan/rewrite/wat/`
    showing the loop shape, scope stack, slot allocation.
  - **Per-slice conformance projection** — for each slice, expected
    PASS/FAIL/SKIP delta.

Per the user's "don't pre-commit to follow-up plans" rule from
CLAUDE.md, this analysis stops at the *what* and *why*.  The
follow-on milestone doc itself is left to the user to authorise
before drafting.

## 8 Recommendation

**Single-milestone recommendation.**  The next milestone should be
**M5 comprehensions follow-on**, scoped to include:

  1. Core comprehensions (`exists`, `all`, `exists_one`, `map`, `filter`)
     — items #1 above.
  2. `cel.bind()` parser-library registration — item #4 above.
     This is free given #1.
  3. Three-arg comprehension forms (`macros2`) — item #2 above,
     only if Slice D fits in the milestone budget; otherwise punt
     to a second slice.

**Why this scope.**  These three items share infrastructure: the
ResolvePass scope handler, the `kComprehensionExpr` codegen arm,
and the dynamic-list runtime primitive.  Doing them together
amortises the infrastructure work.

**Why not include M8 in the same milestone.**  M8 wrappers share
no infrastructure with comprehensions — wrappers live in codegen
type-classification + cel_host read/write paths, comprehensions
live in resolve/scope + control-flow codegen.  Bundling them
would double the milestone size without reducing the work.  Ship
sequentially: comprehensions first (delivers the customer's
`cel.bind`), then M8 wrappers (+55–60 PASS, biggest remaining
single-milestone unlock).

**Expected delta after this milestone.**

  - PASS: 1144 → ~1232 (+88, 50.2% of corpus, ~64% of addressable).
  - Customer `cel.bind` shipped.
  - Three-arg forms either shipped (Slice D in scope, +46 → ~1278)
    or staged for the immediately-following slice.

## 9 Open questions for the user

  1. **Confirm scope** — is "core comprehensions + cel.bind +
     three-arg forms" the right bundle for the next milestone, or
     do you want to split (e.g. ship `cel.bind` standalone first
     for the customer)?  Splitting is technically possible —
     just register the parser macro and gate ResolvePass to admit
     a comprehension *only if* it's the degenerate form
     (`iter_range = [] && loop_cond = false`).  That's a ~1
     session change instead of 3.5.
  2. **Performance for `cel.bind`** — should the degenerate
     comprehension take a fast path in codegen (no loop prologue)?
     Worth doing eventually; deciding now affects whether codegen
     gets a special case or treats it generically.
  3. **M8 after, or M8 in parallel?**  M8 is independent and could
     run as a parallel agent — but it shares the
     wasi-sdk-or-handroll question if `matches()` is in scope.
     If we want M8 to run parallel to comprehensions, we need to
     pick a lane for that question first.
