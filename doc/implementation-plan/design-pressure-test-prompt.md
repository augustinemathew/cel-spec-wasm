# Design pressure-test review prompt

A reusable prompt for an agent that audits the codebase + design
docs for **load-bearing assertions that aren't actually load-bearing.**

The class of bug this catches: the author wrote "X needs to be Y"
based on a true fact about *part* of the problem, then proposed a
plan that treats the *whole* problem as Y when only the smallest
inner step actually demanded it.  The simpler composition —
generic gate + targeted specialised call — was right there, often
already implemented for a sibling shape, but the author didn't
factor it out.

This is **DRY at the design level**, not the line level.  A
linter catches duplicated source lines.  This review catches
duplicated *design shapes* — two slices that should share a
generic wrapper, a kernel that should compose with an existing
one instead of duplicating the inner work, a "we need a new
host trampoline" that's actually "we need a new wasm wrapper
around an existing host trampoline."

## When to run

  - **Before starting any new slice that adds a kernel, codegen
    arm, host trampoline, or ABI surface.**  The prompt's job is
    to catch over-scoped designs before the implementation
    multiplies the surface area.
  - **During independent code review** at milestone closeout
    (per CLAUDE.md "periodic code review" cadence).  The drift
    accretes — sibling kernels that should have shared a helper
    diverge over time.
  - **When a planning doc says "this needs to be host-side"**
    (or any equivalent boundary claim — "this needs a new
    runtime export", "this needs a new annotation field", "this
    needs a separate codegen pass").  The claim may be true for
    the smallest inner step and false for the outer wrapper.

## How to invoke

Spawn via `Agent` with `subagent_type: Explore` (read-only) or
`general-purpose` (if you want a follow-up findings file
written).  Brief with:

  1. The planning doc / commit range / slice scope under review.
  2. A reminder that the agent **writes a findings report only —
     it does not change code**.  Code changes happen in
     follow-up commits the user authorises after reading the
     report.
  3. The output path:
     `doc/implementation-plan/rewrite/reviews/YYYY-MM-DD-<slug>-pressure-test.md`

## The prompt body (paste into Agent description)

```
You are auditing the codebase + design docs for design assertions
that don't actually constrain the design.  The class of finding:
"the author wrote 'X needs to be Y' (host-side, a new export, a
new annotation, a separate pass, ...) but the same outcome is
achievable by composing existing primitives differently.  The
simpler composition is often already shipped for a sibling
shape."

## What you read

  1. The scope I gave you (a planning doc, a slice section, a
     commit range, or "the whole repo for the next N pending
     slices").
  2. Every file that scope names.
  3. Sibling shapes in the codebase — kernels, host trampolines,
     codegen arms — that solve a structurally similar problem.
     Look for `_if_bool` vs `_if_present`, `_at_v` vs `_at_vv`,
     polymorphic dispatch ladders, and any pair of helpers
     whose names differ only by predicate kind.
  4. `CLAUDE.md`, `doc/implementation-plan/design.md`, and the
     active milestone doc — they encode the architectural
     boundaries you should respect (and the ones you should
     pressure-test).

## What you look for

For each finding, identify:

  **A. The assertion.**  Quote the exact sentence from the doc or
  code comment.  Examples of the shape:
    - "this needs a new host trampoline because <reflection /
      I/O / host-only data>"
    - "this needs its own kernel — the existing one doesn't fit"
    - "this needs a new annotation field"
    - "this needs a separate codegen pass / visitor"
    - "this needs an out-of-band shim because <FFI / ABI /
      layout> reasons"

  **B. The true inner constraint.**  What part of the problem
  *actually* needs the asserted resource?  Pin it down to one
  step.

  **C. The wrapper question.**  Does anything else in the
  proposed work need that resource?  Or can the unconstrained
  outer work be done by composing pure / existing primitives
  with the constrained inner step?

  **D. The sibling.**  Search the codebase for a structurally
  identical pair: same predicate shape, same dispatch ladder,
  same boundary.  If one exists, does it have a generic helper
  that the new work could reuse?  If two exist that don't share
  a helper, the design hint is *also* a finding (the missing
  helper is debt).

  **E. The recommendation.**  Concrete restructuring: which
  helper to reuse, which boundary to redraw, which slice to
  collapse.  Cite file paths + symbol names.

## Severity tagging

  - **P0 — slice-blocking.**  The assertion is wrong AND the
    over-scoped design would ship duplicated code that the
    follow-up must un-duplicate.  Restructure before
    implementation.
  - **P1 — should-fix-before-merge.**  The simpler design is
    obvious and the agent is confident; the bigger design works
    but adds tech debt.
  - **P2 — flag-for-discussion.**  The simpler design might
    work; needs human judgement on a tradeoff (e.g. inlining
    vs. one extra import, performance vs. clarity).

## The output

Write to `doc/implementation-plan/rewrite/reviews/YYYY-MM-DD-<slug>-pressure-test.md`:

  - One-paragraph **executive summary** with verdict
    (clean / dirty / mixed) and the top 3 findings.
  - **Findings**, one per assertion, in the A/B/C/D/E shape
    above.  Severity-tag each.
  - A **"sibling map"** section: any pair of helpers in the
    repo that solve the same predicate shape with different
    code paths.  Each pair is a candidate for a shared helper
    even if neither slice the author asked you to review
    touches them.  Severity P2 by default.
  - **"What I did not check"**: any limits — e.g. "I didn't
    pressure-test the host-side proto reflection design; I
    only checked the wasm-side wrappers around it."  Honesty
    about scope is load-bearing.
```

## Worked example 1 — the proto `?field:` simplification (2026-05-22)

The case that motivated this prompt.  Spot the pattern in your
own findings.

**A. The assertion (from an earlier turn of my own working notes):**
> "Unlike map/list `_if_present` which are pure wasm, proto
> field-set needs proto reflection (host-side)."

**B. The true inner constraint:** only the `cel_set_field`
*reflection* step needs the host.  Writing a proto field requires
walking the `FieldDescriptor` and dispatching to
`Reflection::Set...`, which is C++ proto-API only.

**C. The wrapper question:** does the *optional unwrap* need the
host?  No — `OptionalCell.present` and the inner `CelValue` are
plain memory reads.  The wrapper is:

```c
void cel_set_field_at_if_present(uint32_t msg_slot,
                                 uint32_t field_ref_id,
                                 uint32_t opt_value_slot) {
  // 100% pure wasm reads — no host involvement.
  if (cel_value_at(msg_slot)->kind != CEL_MESSAGE) return;
  OptionalCell* cell = NULL;
  if (absorb_optional_predicate(...)) return;
  // ONLY this last call goes to the host.
  cel_host_cel_set_field(msg_slot, field_ref_id, inner_off);
}
```

**D. The sibling:** `cel_map_insert_at_if_present` and
`cel_list_append_at_if_present` in `compiler_v2/runtime/cel_optional.c`
already do exactly this pattern for arena maps/lists.  They share
a helper `absorb_optional_predicate` for the 3VL-absorb +
None-no-op + kind-mismatch portion.  The proto case fits the
same template; the only diff is the inner call is a host import
instead of a pure-wasm kernel.

**E. The recommendation:** add `cel_set_field_at_if_present` to
`cel_optional.c` (~15 LOC reusing the existing helper), do NOT
add a new host trampoline.  Lift the frontend gate; branch the
codegen; ship as Slice E (~0.5d) instead of deferring to an
M7 follow-up.

**What went wrong in the original assertion:** the author
(me) anchored on "proto needs host" — which is true for the
write — and let that infect the whole slice budget.  The
existence of an obvious sibling (`_if_present` for map/list)
should have been the immediate prompt to ask "is the new work
also a wrapper around an existing host primitive?"  Yes — every
proto-write CelHost call already exists.

## Worked example 2 — the no-helper-yet case (synthetic)

Sometimes the simpler composition requires *creating* the helper
that the sibling didn't bother to extract.  This is still a
finding — just one with a refactor included in the recommendation.

**Hypothetical assertion:** "The new
`cel_list_append_at_if_unknown` kernel needs to duplicate the
3VL + kind check from `cel_list_append_at_if_bool` because the
predicate types are different."

**True inner constraint:** the *predicate evaluation* differs
(bool true/false vs. CEL_UNKNOWN check).  Everything else —
list-poisoned no-op, value-3VL absorb, append delegation — is
identical.

**Wrapper question:** can the predicate be parameterised?
Yes:

```c
// Generic shape — predicate is a function pointer.
static void list_append_if(uint32_t list_slot, uint32_t pred_slot,
                           uint32_t value_slot,
                           int (*pred_fn)(const CelValue* p, CelValue* dst)) {
  CelValue* l = cel_value_at(list_slot);
  if (l->kind != CEL_LIST_ARENA) return;
  CelValue* p = cel_value_at(pred_slot);
  if (pred_fn(p, l)) return;   // pred wrote dst (3VL / mismatch / false)
  cel_list_append_at(list_slot, value_slot);
}

static int pred_is_true_bool(const CelValue* p, CelValue* dst) { ... }
static int pred_is_unknown(const CelValue* p, CelValue* dst) { ... }
```

**Sibling:** the existing `cel_list_append_at_if_bool` becomes
one caller of `list_append_if(_, _, _, pred_is_true_bool)`.
The new kernel is the other caller.  The 30-line duplicated
3VL ladder collapses to one place.

**Recommendation:** extract `list_append_if` while landing the
new kernel.  Add a one-line note in the WAT trace that the
generic + predicate-fn shape was chosen specifically to
prevent the duplicated ladder.

**What this example surfaces:** when the helper doesn't exist,
the right call is often *create it as part of the slice*, not
defer "we'll extract later."  Two diverging copies of the same
shape almost never get re-merged once shipped — they accrete
unique features and the simplification window closes.  The
review's job is to flag this in the *design* phase when extraction
is still free.

## The macro-DRY lens — what makes this different from line-level DRY

Line-level DRY (the linter's job): two functions share 20
identical lines, extract a helper.

Macro-DRY (this prompt's job): two **design decisions** share a
shape:

  - "the inner step needs a host trampoline; the outer wrapper
    is pure logic" — Slice E and the map/list `_if_present` pair
  - "test_only Select on optional chains two kernels on the same
    slot" — pattern reused later for any "modifier + base" combo
  - "rodata lift only when the operand has Repr X" —
    `SelectKeyRodataVisitor` shape; future Repr branches should
    reuse the visitor pattern not invent their own

The pattern isn't in any one file — it's in the *negative space*
between sibling components.  The agent has to read multiple
sites and ask "are these the same shape underneath?"  When yes,
the missing helper / the over-broad boundary / the duplicated
plumbing is the finding.

## Correctness pressure-test — the partner pass

Every design simplification creates **new code paths that no
existing test covers.**  Reusing primitive `B` in a new composition
with primitive `A` doesn't transfer `B`'s test coverage to the
`A→B` seam.  The simplification is only honest after targeted
tests prove the composition holds.

The design pressure-test (above) is "is this simpler than
necessary?"  The correctness pressure-test is its partner: "if
we simplify, what new tests does the simpler code need?"  A P0/P1
design finding without a paired correctness-test plan is half a
review.

### When to run

  - **In the same review pass that filed the design finding.**  The
    correctness-test list ships in the same report under the
    finding it pairs with — not as a separate session.
  - **Before authorising the slice that implements the
    simplification.**  The tests are part of the slice budget,
    not "will write after."

### What you look for

For each design finding's recommended composition `A → B`:

  **A. The seam.**  Where does the new wrapper hand off to the
  existing primitive?  Name the exact contract: what does `B`
  assume about its inputs, and is the new caller guaranteed to
  satisfy it?

  **B. The contract delta.**  `B` was tested under its previous
  callers' assumptions.  What's *different* about the new
  caller?  Examples:
    - Slot offset points inside another struct (e.g.
      `OptionalCell::inner`) vs. a standalone workspace slot.
    - Lifetime is shared with another value vs. independent.
    - The kind/type-tag is post-unwrap vs. pre-unwrap.
    - The slot is read-only-by-contract for the duration of
      the call vs. the previous caller wrote freely.

  **C. The short-circuit.**  If the wrapper has a no-op path
  (None → skip the inner call, ERROR → propagate without
  invoking B), the test must **prove `B` did not execute** in
  that path — not just that the observable output is the
  expected sentinel.  A test that passes when B silently
  executes-anyway-and-writes-nothing is a false-pass.

  **D. The 3VL surface.**  If either primitive does 3VL
  absorption, the composition's 3VL contract is the *intersection*
  of the two — and that intersection may have surprises:
    - Does the wrapper's CEL_ERROR propagation override B's
      CEL_ERROR handling?  Which wins on a double-error?
    - Are CEL_UNKNOWN paths symmetric between the wrapper's
      input and B's input?

  **E. The integration test.**  Even after unit-test coverage at
  the seam, write one end-to-end test that drives the composition
  from a real CEL source expression through the full pipeline.
  This catches plumbing errors that unit tests miss (import
  bindings, kernel-name typos, codegen branch ordering).

### The test budget

For each simplification finding, the correctness-test plan
should list, with rough effort estimates:

  - **Seam unit tests** — one per distinct A→B input shape
    (Some / None / ERROR / UNKNOWN / wrong-kind, plus any
    contract-delta-specific case from B).  Typical: 5-7 tests.
  - **Short-circuit assertions** — one per no-op branch,
    proving the inner call did not run.  Typical: 1-3 tests.
    Implementation pattern: instrument with a counter, or
    assert on a side-effect that only `B` would produce (e.g.
    `ArenaHeader.count` increments only when `cel_list_append_at`
    actually runs).
  - **e2e tests** — one per user-observable surface the
    simplification unlocks.  Typical: 2-4 tests, one per CEL
    source-expression shape (mixed entries, all-optional,
    all-unconditional regression).

If the test plan totals < 5 tests, the simplification is either
trivial (good — ship it) or under-tested (suspect — pressure-test
the plan).  If it totals > 20 tests for a half-day slice, the
simplification probably isn't.

### Worked example 1 — correctness tests for the proto `?field:` simplification

Paired with the design finding in *Worked example 1* above:
`cel_set_field_at_if_present` reuses `cel_host.cel_set_field`
behind an optional gate.

**Seam:** the wrapper passes `opt->payload.opt +
offsetof(OptionalCell, inner)` as the value-slot argument to
`cel_host.cel_set_field`.  Previously `cel_set_field` was always
called with a standalone workspace slot.

**Contract delta:** the inner CelValue offset is **inside** an
OptionalCell struct.  Two concerns:

  1. *Memory layout.*  `OptionalCell.inner` is 8 bytes into the
     cell (after `present: u32` + `_pad: u32`).  The offset
     arithmetic must match — a test proves the
     `cel_host.cel_set_field` reflection reads the right 24 bytes.
  2. *Lifetime.*  The OptionalCell stays valid until the next
     `arena_reset`, same as any workspace slot — so the lifetime
     is identical, but the test pins it explicitly with a
     post-call read of the cell's contents.

**Short-circuit:** None → no `cel_host.cel_set_field` call.
Implementation: instrument the host trampoline (test-mode only)
with a call counter; assert counter unchanged after a None-path
invocation.  Equivalent verifier: post-call
`!host_msg_has_field(field_id)` — proves the field stayed unset.

**3VL surface:** wrapper absorbs ERROR/UNKNOWN on `opt_value_slot`
into `msg_slot` (poisons the message).  Test that a downstream
`cel_get_field` on the poisoned message propagates the original
error, not a TYPE_MISMATCH.

**Targeted test plan (5 seam + 1 short-circuit + 3 e2e):**

```
// cel_optional_test.cc (per-TU)
SetFieldIfPresentSomeWritesField           — Some(int 5) → field=5
SetFieldIfPresentNoneLeavesFieldUnset      — None → !has(field)
SetFieldIfPresentErrorPoisonsMessage       — ERROR opt → msg=ERROR
SetFieldIfPresentUnknownPoisonsMessage     — UNKNOWN opt → msg=UNKNOWN
SetFieldIfPresentWrongKindIsTypeMismatch   — bare int → msg=TYPE_MISMATCH
SetFieldIfPresentNoneDoesNotInvokeHost     — short-circuit (counter)

// m14_test.cc (e2e)
ProtoLiteralOptionalFieldSomeMaterialises  — TestAllTypes{?f: opt.of(5)}.f == 5
ProtoLiteralOptionalFieldNoneLeavesUnset   — !has(TestAllTypes{?f: opt.none()}.f)
ProtoLiteralMixedOptionalAndUnconditional  — both kinds in one literal
```

**What this catches that sibling tests don't:** the offsetof
arithmetic, the host-trampoline non-invocation on None, the 3VL
poisoning semantics when the proto-write would have succeeded.
The map/list `_if_present` tests don't prove any of these for
the proto path because they hit different inner primitives.

### Worked example 2 — correctness tests for the no-helper-yet case

Paired with *Worked example 2* (synthetic
`list_append_if` + predicate-fn extraction):

**Seam:** every existing caller (`cel_list_append_at_if_bool`) now
routes through the new generic + its predicate function.

**Contract delta:** none for existing callers — the refactor is
behaviour-preserving by intent.  But intent is not proof.

**The critical test pattern for a behaviour-preserving refactor:**
**run every existing test for the old caller against the new
implementation, unchanged.**  If the existing test suite for
`cel_list_append_at_if_bool` was thorough, the refactor is safe
iff every test still passes.  If the suite was thin, **fill the
gaps before refactoring**, not after.

Targeted test plan:

  - **Coverage audit (before refactor):** list every branch of
    the existing `cel_list_append_at_if_bool` body; identify
    which has a paired test.  Branches without tests are
    *blockers* — write them against the *old* implementation
    first, watch them pass, then refactor.
  - **New predicate-fn tests:** one positive + one negative per
    new predicate (`pred_is_unknown` true / false).
  - **Generic-helper boundary test:** invoke `list_append_if`
    with a deliberately misbehaving `pred_fn` (always writes
    `*dst` to an unexpected kind) to prove the helper doesn't
    short-circuit on the predicate's return value alone — it
    must trust the predicate's `dst` write.  This is the
    contract that lets the two predicate-fns coexist.

**What this catches:** refactors that are "obviously equivalent"
often aren't.  The generic helper's branch boundaries can shift
relative to the inlined original (e.g. when the predicate's
no-op return path and ERROR-write path used to be one if-else
in the inlined code but become two function-call boundaries in
the generic).  Run the existing test suite, see green, ship.

### Self-check for the correctness-test plan

Run this before filing the test plan as part of the design
finding:

  1. **Does the plan have at least one test per A→B input shape
     B's previous callers exercised?**  No → undercoverage.
  2. **Does the plan have a short-circuit assertion that proves
     B did NOT execute when the wrapper short-circuited?**  No
     → the simplification's no-op path is untested.
  3. **Does the plan have at least one e2e test that drives the
     composition from CEL source?**  No → plumbing errors will
     leak.
  4. **For refactors of existing primitives: does the existing
     test suite cover every branch?**  Gaps must be filled
     *before* refactoring, not after.

If any check fails, expand the test plan before authorising
the slice.  Half-tested simplifications are worse than the
duplicated original — they have the same surface area plus a
new failure mode.

## How findings convert to work

  - P0 finding → restructure the slice plan *before* writing
    code.  Update the planning doc with a "scope pull-in /
    pull-out" callout (model: m14-optionals.md's 2026-05-22
    Slice E callout).  Note the simplification's rationale so
    a future reader doesn't undo it.
  - P1 finding → land the simplification in the same slice.
    Don't defer to "a future cleanup pass."
  - P2 finding → add to `doc/implementation-plan/cleanup-backlog.md`
    with the originating review date.  When a future commit
    addresses it, the commit message cites the review (same
    convention as CLAUDE.md "periodic code review").

## What this prompt does NOT do

  - **It does not pressure-test broad correctness of the
    underlying primitives.**  The correctness pressure-test
    (above) only covers the *new seams* the simplification
    creates — the A→B handoff, the short-circuit no-op, the 3VL
    intersection.  If primitive `B` itself has a latent bug, that
    bug is caught by `B`'s own test suite, not by this review.
  - **It does not propose new features.**  Slice scope expands
    only via pull-ins of work already named in §5 "Out of scope"
    that turn out to be cheap.  Inventing new requirements is
    not in scope; that path leads to scope creep dressed as
    simplification.
  - **It does not refactor for its own sake.**  Two
    structurally-similar functions that won't be touched in the
    next milestone are a P2 backlog entry, not a P0
    block-the-slice finding.  Reserve P0/P1 for shapes that the
    *current* work would compound.

## Self-check before filing a finding

Run this checklist on every finding before writing it down:

  1. **Have I cited the sibling?**  No sibling = the finding is
     speculation, not pressure-test.  Either find one or downgrade
     to P2.
  2. **Have I cited the inner constraint that *does* need the
     asserted resource?**  If the whole proposed work needs
     host/runtime/whatever, the assertion is correct and the
     finding is invalid.
  3. **Does my recommendation fit in the slice's stated budget?**
     If "just compose these primitives" actually requires a new
     ABI/annotation/kernel, the simplification isn't.
  4. **Would the reader of my report be able to act on the
     finding without re-doing my research?**  Cite paths +
     symbols + line numbers.  "There's a helper somewhere" is not
     a finding.

If any check fails, either fix the finding or drop it.

---

**Filed:** 2026-05-22, motivated by the proto `?field:`
simplification in m14-optionals.md Slice E.  Update this doc
when you encounter a worked example that surfaces a *different*
class of macro-DRY miss — the catalogue is the
forcing function against repeating the same review-blind-spot
twice.
