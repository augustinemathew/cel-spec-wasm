# 2026-05-21 — M14 Slice 0 (CEL optionals, WAT-first) — independent review

Reviewer: independent pass.  The prior reviewer's report at
`doc/implementation-plan/rewrite/reviews/2026-05-21-m14-slice0.md`
was deliberately NOT read — this take is from scratch off the
diff + the cel-cpp source + the runtime headers.

Range reviewed: working-tree diff against `origin/master` on branch
`cel_optional`:

  - 4 modified files: `compiler_v2/tools/wat_runner/wat_runner.cc`,
    `compiler_v2/tools/wat_runner/wat_runner_test.cc`,
    `doc/implementation-plan/rewrite/m14-optionals.md`,
    `doc/implementation-plan/rewrite/wat-traces.md`.
  - 4 new WATs under `doc/implementation-plan/rewrite/wat/m14_*.wat`.
  - 0 production source files.  Slice 0's scope held.

## Verdict — mixed, leaning dirty

The four WATs assemble, the harness loads them, and the design
doc names every cel-cpp overload that ships in M14.  Slice
discipline held: no `expr_lower.cc` edits, no
`runtime/cel_optional.{h,c}` created, no premature
`OverloadTable` seeds.  That's the good news.

The bad news: **the "lock" Slice 0 advertises is much weaker
than the doc claims.**  The four WAT-runner tests assert only
`out->eval_return == <slot offset>` — a literal value baked into
the WAT — and never decode the post-eval CelValue out of
`memory_after`.  Because the pre-Slice-A trampolines bound by
`RegisterPendingM14Imports` (`wat_runner.cc:664-690`) are
**total no-ops** (they share the `NoopCelHostThreeArg`
trampoline at `wat_runner.cc:229-233`, which simply returns
`nullptr`), nothing the kernels are supposed to do is actually
exercised: no `present` flag is written, no CEL_OPTIONAL kind
is set on `out_slot`, no OptionalCell is arena-allocated.  The
tests would pass identically if every comment in every WAT said
the opposite.  That's not a WAT-first lock — it's a syntactic
lint that the file assembles.

The substantive locking still happened in the doc prose
(`wat-traces.md` §M14.1-M14.4 and the WAT comment headers).
That's worth something, but the WAT-first discipline in
CLAUDE.md §"WAT-first for ABI and codegen design" exists
precisely because doc prose drifts and executable WAT does not.
Slice 0 wrote prose and called it executable.

**Top three items the user should look at before approving:**

  1. **Tests are too weak — they don't verify the locked
     ABI.**  Either extend each Slice-0 test to decode the
     expected CelValue out of `memory_after`, OR make the
     no-op trampolines write the spec-claimed bytes
     ("present=1 at cell offset N", "kind=CEL_OPTIONAL at
     out_slot", "kind=CEL_BOOL/payload.b=1 at hasValue
     out_slot") so the existing `eval_return` assertion
     actually depends on the kernel doing the right thing.
     P0 — without this, Slice 0 hasn't proven what it
     claims to have proven.
  2. **The `or` overload short-circuit semantic is
     completely absent from the design.**  cel-cpp implements
     `or` / `orValue` as a `OptionalHasValueJumpStep` +
     `OptionalOrStep` pair (`eval/eval/optional_or_step.cc:80,
     111`).  The RHS is NOT evaluated when the LHS is Some.
     Slice 0's WAT4 treats `orValue` as a plain 3-arg kernel
     with both operands pre-evaluated.  For `orValue('default')`
     with a constant RHS this is observationally identical;
     for `orValue(expensive_call())` or
     `or(optional.of(1/0))` it produces wrong errors and wrong
     side-effects.  See conformance row
     `optional.of(1).orValue(1/0)` (not in the current corpus
     but trivially constructible).  P1, will hit hard in
     Slice B if not designed for now.
  3. **No `optional.none()` standalone WAT, and no
     `ofNonZeroValue`.**  Both are load-bearing per probe
     Q6/Q7 and per overload table in m14-optionals.md §1.8.
     `optional.ofNonZeroValue(null)` and
     `optional.ofNonZeroValue(0)` are EXPLICITLY exercised by
     the corpus (`tests/simple/testdata/optionals.textproto`
     lines 1-3) and the zero-value predicate is a 40-LOC
     pure-C function that the WAT-first discipline requires
     us to author by drawing the ABI in WAT first.  Slice 0
     called these "covered by symmetry"; that's the same
     argument that let M2 ship 29 silent GTEST_SKIPs.
     P1.

---

## 1. Are the four WATs the right four?

Short answer: three are right; the fourth would be better
re-aimed.  The set is also missing two WATs that are not
just covered "by symmetry."

### 1.1 What's locked well

**`m14_optional_of_int.wat` (WAT 1)** — correct choice for
WAT 1.  Locks the OptionalCell layout (`present:u32 @ 0,
_pad:u32 @ 4, inner:CelValue @ 8`), the
`cel_optional_of_at_v(out, v)` slot-out convention, and the
arena-alloc-per-construction story.  The 8-byte
`_pad` is over-explained but accurate; the inner CelValue
needs 8-byte alignment because `payload.i / payload.d` are
64-bit, and on `wasm32` natural alignment is 8 for those
fields.  The WAT header's diagram lines up with `cel_data.h`'s
`_Static_assert(sizeof(CelValue) == 24)` (line 146).

**`m14_optional_has_value.wat` (WAT 2)** — correct choice.
Locks the receiver-form kCall flatten and the present-flag
read.  The choice to NOT also write a separate `value()` WAT
is defensible — the WAT trace prose calls this out (M14.2
§"The companion `.value()` accessor") — because the kernel
shape is genuinely the same modulo "read 4 bytes (`present`)
as bool" vs "read 24 bytes (`inner`) as CelValue".  I'd
still write a `value()` WAT before Slice A starts, because
the None case for `.value()` is a `CEL_ERROR{INVALID_ARGUMENT}`
that the bool case never sees, and "error production from a
kernel" is the part of the ABI that the prose-locked symmetry
glosses over.

**`m14_optional_select_field.wat` (WAT 3)** — the most
important WAT in the set.  Correctly chosen.  Locks the
single-kernel-for-both-paths decision (Call(`_?._`) and
kSelectExpr-on-optional both converge on
`cel_optional_select_field_at_vv`).  This is the load-bearing
architectural call and the WAT does a reasonable job of
documenting why a single kernel.

### 1.2 What WAT 4 should have been

**`m14_optional_chain_or_value.wat` (WAT 4)** — picks
`{'k': 1}.?missing.orValue('default')` to exercise None
propagation + unwrap.  That's a fine end-to-end test, but
it bundles three things into one WAT:

  - The absent-key branch of `cel_optional_select_field_at_vv`
    (already exercised at the prose level by WAT 3 — pick the
    found-key branch as the canonical lock, then a separate
    WAT picks the absent-key branch).
  - The `cel_optional_or_value_at_vv` kernel.
  - The end-to-end chain.

A cleaner WAT 4 would have been **either**:

  - **`optional.none().or(optional.of(7))`** — exercises the
    `or` overload (not `orValue`) and the No-RHS-on-Some
    short-circuit case.  `or` has a different return type
    (`optional<V>`, not `V`) from `orValue`, and the design
    doc explicitly says they share an ABI "by symmetry"
    (`wat-traces.md` §M14.4 last paragraph: "The .or(other_opt)
    overload … has the same 3-arg ABI shape; the only kernel
    difference is the present branch's memcpy source and the
    present branch's output kind").  That "by symmetry" claim
    is exactly the kind of thing WAT-first exists to forbid —
    the output kind differing (CEL_OPTIONAL vs the inner kind)
    means the out_slot.kind write line is different, and "the
    output kind is different" IS an ABI difference.
  - **An arithmetic-on-unwrapped chain** like
    `optional.of(2).value() + 3` — exercises `.value()`'s
    success path AND the fact that the unwrapped CelValue
    goes into a regular slot that subsequent arith helpers
    can consume.  This locks the post-unwrap slot layout
    contract.

WAT 4 as written tests the None-propagation path through
`select_field` + a constant-RHS `orValue`.  That's fine, but
it's the easy direction.  The hard direction — `or` (not
`orValue`) propagating optional-ness vs unwrapping — is left
to "symmetry" and will burn the next slice.

### 1.3 What's missing entirely

  - **`optional.none()` standalone (no `.of()` first).**  The
    `none()` constructor is a separate overload
    (`optional_none` per cel-cpp checker/optional.cc:95).  It
    has a different kernel ABI than `of`: no inner argument,
    so a 1-arg `cel_optional_none_at(out_slot)` instead of
    2-arg `cel_optional_of_at_v(out_slot, v_slot)`.  Neither
    `RegisterPendingM14Imports` nor any WAT binds or imports
    this name.  Slice A WILL need it (m14-optionals.md §4
    Slice A bullet: "8 kernels (of, of_non_zero, none, …)").
    WAT-first means freeze it now.
  - **`optional.ofNonZeroValue(0)`** and
    **`optional.ofNonZeroValue(null)`**.  The cel-cpp source
    (`runtime/optional_types.cc:58-67`) shows
    `OptionalOfNonZeroValue` is a thin wrapper:
    `if (value.IsZeroValue()) return OptionalNone(); else
    return OptionalOf(value);`.  The "zero predicate" is
    per-kind (the design doc lists the matrix in §3.4) and
    that matrix is ALSO a closed ABI surface — for CEL_LIST
    you check the list count; for CEL_MAP you check the map
    count; for CEL_BYTES you check the span len; for
    CEL_NULL the answer is always true; **for CEL_MESSAGE
    the design says "not defined; cel-cpp errors" but that's
    wrong** (see §6.4 below).  This is exactly the kind of
    decision WAT-first locks before C code is written.
  - **`has(opt.x.y)` / `optional.has(...)` chain.**  Per probe
    Q13 (m14-optionals.md §1.7 last paragraph): outer
    `test_only` Select wraps a `Call(_?._)`.  "Combined
    present-AND-inner-has-field semantics needed at
    codegen" — the design doc names this an open question
    Slice B will solve.  But the ABI it solves through (a
    new helper `cel_optional_has_chain` per §"Test_only
    Select on optional operand" or a sequence of existing
    primitives) is one of the things Slice 0 is supposed to
    pin.  WAT-first means we don't leave the choice to
    Slice B.

Conclusion: **the right count is probably six, not four.**
The added two: `optional.none()` standalone and one
`ofNonZeroValue`-with-message-or-list case (to force the
zero-predicate ABI decision into WAT before C code is written).

---

## 2. Is the OptionalCell layout actually right?

### 2.1 Re-derived from scratch

Reading `compiler_v2/runtime/cel_data.h:46` and lines 108-144:

  - `CelValue` is 24 bytes, asserted at `cel_data.h:146`.
  - `CEL_OPTIONAL = 14` is declared at `cel_data.h:46`.
  - The payload union has a `uint32_t opt;` arm at
    `cel_data.h:140` reserved for this kind.
  - Other complex kinds use offset-pointer payloads:
    `arena_map.header_ptr:u32` (line 80, 16-byte header at
    that offset), `arena_list.header_ptr:u32` (line 91, 16-byte
    header).  Both are 16-byte headers, NOT 32-byte cells.
  - `cel_value_at(0) → NULL` (cel_arena.h:50-52) — the zero
    offset is reserved for "absent."

So `payload.opt` is already a `u32`-offset slot.  An OptionalCell
of size 32, 8-byte aligned, fits the existing pattern.  Slice 0's
layout choice is consistent and correctly typed.

### 2.2 But the cost / benefit analysis is shaky

The WAT comment header in `m14_optional_of_int.wat` argues
against two alternatives:

  - **Tag-encoded kind values** (`CEL_OPTIONAL_SOME = 14`,
    `CEL_OPTIONAL_NONE = 15`).  The argument is that every
    polymorphic switch (cel_equals, cel_log, type()) would
    need two arms.  This is genuinely true and the argument
    holds.  No quarrel.
  - **Shared-static-None sentinel.**  The argument is that
    sharing a static cell means "kernels must not write
    through pointer X" — adding a branch to every future
    kernel.  **This argument is weak.**  Three reasons:

    1. **Current kernels already don't write through opt_slot.**
       The WATs show every kernel produces a fresh out_slot,
       reading from opt_slot.  There's no existing kernel
       that writes back; there's no plausible kernel design
       that would write back (CEL values are
       value-semantics, and the codegen's slot model never
       mutates input slots in place).  The "constraint on
       future kernels" the design fears is hypothetical.
    2. **cel-cpp itself shares a static None.**
       `common/values/optional_value.cc:415-418`:
       `OptionalValue::None()` returns a value backed by a
       static `empty_optional_value_dispatcher` —
       *every* None in cel-cpp is the same object.  Diverging
       from upstream on this is a delta worth justifying;
       Slice 0 doesn't acknowledge that cel-cpp does the
       optimization at the value layer.
    3. **The existing memory map ALREADY has shared
       sentinels.**  `wat-traces.md` §"Memory map (shared
       across every expression)" line 35: `[0, 8) reserved
       null sentinel — offset 0 means absent`.  And
       `cel_arena.h:51` codifies that.  Rodata-pointed
       CelValues at fixed offsets are also shared — every
       WAT in the existing corpus puts CEL_INT/CEL_STRING
       literals in rodata that no one writes through.
       Saying "shared cells are a new constraint" ignores the
       fact that we already have them.

    A shared-static None at, say, offset 8 of linear memory
    (next to the null sentinel) — written once by the
    runtime init shim, then read-only forever — costs one
    8-byte slot in the reserved region and saves an
    arena_alloc per `optional.none()` call.  The optionals
    corpus has rows where None is constructed many times
    (`optional.none().or(optional.none()).orValue(42)` —
    two Nones per eval).  This is cheap and correct.

    Slice 0 says "defer to a post-Slice-D perf pass; the ABI
    doesn't change."  But it DOES change — the kernel
    contract becomes "this offset MAY be the shared cell;
    you must never write to it," and that contract change
    is exactly the kind of thing WAT-first is supposed to
    settle before code lands.  Either:

      - Lock the shared-static-None now (preferred), OR
      - Lock the contract that says future shared-None
        optimization can layer in, by writing the WAT for
        the kernel that reads cell.present from a
        potentially-shared cell.

    Picking neither is the worst of the three.

### 2.3 The inline-scalar opportunity that the design didn't consider

cel-cpp's `OptionalValue::Of` (`optional_value.cc:365-413`)
**doesn't arena-allocate at all for scalars** (bool, int, uint,
double, duration, timestamp, null).  The dispatcher type
encodes the inner kind and the 8-byte content is packed inline.
Only non-trivial inner types (string, bytes, list, map,
message, type, optional) hit the arena.

Slice 0 wants 32 bytes per optional, including for
`optional.of(1)` where the inner is an 8-byte int.  An
alternative ABI would inline-encode scalars into the
24-byte CelValue itself:

```c
struct CelValue {
  uint32_t kind;           // CEL_OPTIONAL_<INNER_KIND> or
                           // CEL_OPTIONAL_NONE or CEL_OPTIONAL_REF
  uint32_t _pad;
  union {
    int64_t i;             // inline-encoded CEL_OPTIONAL_INT
    double d;              // inline-encoded CEL_OPTIONAL_DOUBLE
    ...
    uint32_t opt_ref;      // CEL_OPTIONAL_REF — points at a heap cell
                           //   for non-scalar inners
  } payload;
};
```

This would re-open the tag-encoding question with a stronger
motivation (saves not just 8 bytes per cell but the whole
arena alloc for the common case of `optional<int>` /
`optional<bool>`).  The corpus has many scalar optionals
(`optional.of(42)`, `optional.of(true)`, `optional.of(null)`).

I'm not advocating that we adopt it — the design's argument
that "two CelKind values × 12 polymorphic switches = 24 extra
edges" is real — but the design didn't even consider this
alternative.  Slice 0 should have evaluated three options
and rejected two with reasons, not evaluated two and
rejected one.  P2; the existing layout is workable, but
the perf pass deferred to Slice D may surface enough to
warrant revisiting.

### 2.4 Layout summary

  - The 32-byte layout is correctly designed for the ABI it
    chose.
  - The reasoning against shared-static-None is weak; that
    optimization should land in the ABI now.
  - The inline-scalar variant was not considered and may
    become an ABI question later.

---

## 3. Are the kernel ABI shapes consistent with the rest of the runtime?

### 3.1 Slot-out convention

Existing kernels follow `(out_slot, arg0, arg1, ...) → void`
uniformly.  Spot-checks:

  - `cel_int_add_at_vv(out, a, b)` —
    `compiler_v2/runtime/cel_arith.h:49`.
  - `cel_string_size_at_v(out, v)` —
    `compiler_v2/runtime/cel_string_ops.h:44`.
  - `cel_string_char_at_at_vv(out, s, idx)` —
    `compiler_v2/runtime/cel_string_ext.h:52`.
  - `cel_string_format_at_vv(out, s, args)` —
    `compiler_v2/runtime/cel_string_format.h:39`.

The Slice 0 kernels match:

  - `cel_optional_of_at_v(out, v)` — 2-arg, single-input.
  - `cel_optional_has_value_at_v(out, opt)` — 2-arg.
  - `cel_optional_select_field_at_vv(out, src, key)` — 3-arg.
  - `cel_optional_or_value_at_vv(out, opt, default)` — 3-arg.

All consistent.  Naming follows the established suffix
convention (`_at_v` for 1-input, `_at_vv` for 2-input — note
the "v" counts INPUT slots, not all slots; out_slot is
implicit in the prefix).

### 3.2 Polymorphic-by-source pattern

`cel_optional_select_field_at_vv` dispatches on `src.kind`
internally — CEL_OPTIONAL unwraps, CEL_MAP_ARENA looks up,
CEL_MAP_HOST trampolines, CEL_MESSAGE trampolines.  Is that
consistent with how the rest of the runtime does it?

There are two precedents:

  - **`cel_map_lookup` / `cel_list_at`** (kDynamic dispatcher)
    — does a `switch (m->kind)` then `__attribute__((musttail))`
    into either `cel_map_lookup_arena` or `cel_host.cel_map_lookup`.
    See `cel_map.h:73-81` and `rewrite/map-list-dispatch.md`.
  - **`cel_equals_at_vv`** — polymorphic on the kind pair.
    Single big switch.

`cel_optional_select_field_at_vv` chooses the second pattern
(single switch with branches), not the first (musttail
dispatcher).  That's defensible because the optional unwrap is
not itself a dispatch — it's a precondition.  But the design
docs DON'T discuss why one pattern over the other.  Slice 0
should have at least named the choice; otherwise Slice B's
implementer will pick by feel.  P2.

### 3.3 The "absent key" problem

Critical undocumented gap.  `cel_map_lookup_arena` on a miss
**writes `{CEL_ERROR, CEL_ERR_NO_SUCH_KEY}` to out_slot**
(`cel_map.h:68`, confirmed at `cel_runtime.c:195`:
`poison(out, CEL_ERR_NO_SUCH_KEY);`).  For
`cel_optional_select_field_at_vv` to convert "absent key"
into `optional.none()`, it CANNOT simply call
`cel_map_lookup_arena` and pass the result through — it
needs to either:

  - Inspect the result for the specific `CEL_ERR_NO_SUCH_KEY`
    code and convert it to None, OR
  - Use a new internal helper that distinguishes "absent" from
    "error" without going through CelValue.

The WAT 3 header pseudo-code at lines 51-55 names a function
`cel_map_lookup_arena_value` and says:

  > Returns either a found value's CelValue OR a sentinel
  > "absent" marker.

**That function doesn't exist.**  Slice 0 invented a kernel
name for a kernel that hasn't been designed.  The "absent
sentinel" abstraction is at odds with the existing
`CEL_ERR_NO_SUCH_KEY` poison pattern.

This is a real ABI question Slice 0 was supposed to lock and
didn't.  Two options:

  - Reinterpret the existing `CEL_ERR_NO_SUCH_KEY` in the
    optional kernel: catch it, replace with None.  The
    cost: every kernel doing absent-key lookups must know
    about this special error code.  This is fragile.
  - Add a new `cel_map_lookup_arena_or_absent` variant that
    returns `{CEL_NULL, ...}` on miss (or an explicit
    sentinel value the optional kernel recognizes).  The
    cost: a second map lookup primitive whose semantic
    delta is exactly one CelKind value on miss.

The second is cleaner but adds an export.  The first hides
the special case inside one kernel.  Slice 0 should have
picked one; instead it hand-waved with a function name that
doesn't compile.  P1; this WILL bite Slice B's implementer.

### 3.4 ABI shape consistency summary

The slot-out shapes are correctly modeled.  The polymorphic
dispatch and the absent-key contract are not.  Naming follows
convention; semantics are underspecified.

---

## 4. Slice-discipline check

Per CLAUDE.md §"What not to do": "Don't create work for later
milestones without being explicitly asked."  Slice 0's stated
scope was: WAT traces + harness wiring.  Let's verify:

  - ✅ **No `expr_lower.cc` edits.**  `git diff --stat` shows
    zero codegen files touched.
  - ✅ **No `runtime/cel_optional.{h,c}` created.**  Confirmed
    by `ls compiler_v2/runtime/cel_optional*` — no such files.
  - ✅ **No `OverloadTable::kBuiltinSeeds` rows added.**  No
    overload_table changes in the diff.
  - ✅ **No production `kRuntimeExports` additions.**  The
    Slice 0 names are bound in `RegisterPendingM14Imports`,
    NOT in `engine.cc`'s `kRuntimeExports[]` (lines 227-328).
    Confirmed.
  - ✅ **No `parse_and_check.cc` flip.**  The 1-line
    `enable_optional_syntax = true` change is correctly
    deferred to Slice A.
  - ⚠ **The design doc has Slice B/C/D bullets.**  The
    m14-optionals.md doc was already on master at commit
    `3e584cd`; the diff against `origin/master` for that
    file (Slice 0's mods) just adds the §1.7 "Slice 0 lock
    (2026-05-21)" callout and the §3.1 "Resolved by Slice 0
    (2026-05-21)" annotation.  Those are appropriate.  The
    pre-existing Slice B/C/D sections aren't new work.

Slice discipline: **held.**

One small smell: `wat_runner.cc` `kRuntimeExports` has 102
entries (line 32) — this matches `cel_runtime.wasm`'s actual
export count.  Adding `cel_optional_*` to the no-op trampoline
list (correct for Slice 0) and not to `kRuntimeExports` (also
correct) means the array size annotation `std::array<...,
102>` won't need updating until Slice A.  No drift.

---

## 5. Naming critique

The chosen kernel names against cel-cpp's overload IDs
(`checker/optional.cc:90-117`):

| Slice 0 name | cel-cpp overload ID | CEL surface | Notes |
|---|---|---|---|
| `cel_optional_of_at_v` | `optional_of` | `optional.of(v)` | Match minus the `_at_v` suffix. |
| `cel_optional_has_value_at_v` | `optional_hasValue` | `opt.hasValue()` | OK; `has_value` (snake) vs `hasValue` (camel) follows CEL→C convention. |
| `cel_optional_select_field_at_vv` | `select_optional_field` | `obj.?field` | **Inverted word order.**  cel-cpp says `select_optional_field`; Slice 0 says `optional_select_field`.  Discoverable in either form, but breaks the convention of "match the overload ID exactly." |
| `cel_optional_or_value_at_vv` | `optional_orValue_value` | `opt.orValue(v)` | OK. |

The `select_optional_field` vs `optional_select_field` swap is
not load-bearing but it IS a tiny piece of incidental drift
that makes grep across cel-cpp + our runtime harder.  Other
existing names in the repo do match cel-cpp:
`cel_string_index_of` matches cel-cpp's `indexOf` overload
naming (snake).  P2; rename to `cel_select_optional_field_at_vv`
to follow the overload ID exactly.

The user asked specifically about alternatives:

  - **`cel_optional_some_at_v`** — clearer for English readers
    (`some` ↔ Rust/Haskell convention) but loses cel-cpp
    parity.  cel-cpp uses `of`, not `some`; we should follow.
  - **`cel_make_optional_at_v`** — matches the `cel_make_*`
    families in `cel_make.h` (which has `cel_make_int`,
    `cel_make_error`, etc.).  Possible alternative but
    diverges from the cel-cpp overload ID.  No.
  - **`cel_optional_wrap_at_v`** — describes the verb
    (wrapping a CelValue in an OptionalCell) but again loses
    parity.  No.

The current `cel_optional_of_at_v` is the right call.  Just
fix the `select` ordering.

---

## 6. Things to push back on hard

### 6.1 The WAT-runner tests are smoke tests, not lock tests

P0.  Already covered in §"Top three items" above.  The user
should require, before Slice 0 lands:

  - WAT 1 test: decode CelValue at `eval_return` (offset 40),
    assert `{kind=CEL_OPTIONAL, payload.opt=64}`.  Then
    decode the OptionalCell at offset 64, assert
    `{present=1, _pad=0, inner.kind=CEL_INT, inner.payload.i=1}`.
  - WAT 2 test: decode CelValue at offset 64, assert
    `{kind=CEL_BOOL, payload.b=1}`.
  - WAT 3 test: decode at offset 112, walk into the inner
    cell, assert the wrapped string equals `'v'`.
  - WAT 4 test: decode at offset 160, assert CEL_STRING with
    bytes `'default'`.

To make these pass, the pre-Slice-A "no-op" trampolines need
to instead write **the spec-claimed bytes**.  Per the WAT
header for `m14_optional_of_int.wat` (lines 42-51) the
contract is fully spelled out — write a wat_runner-level
trampoline that implements EXACTLY that contract.  When
Slice A's real kernel lands, deleting the wat_runner stub
and binding the real export should produce byte-identical
test output — that's the actual WAT-first lock.

Alternative: bind the four imports against a wasmtime-linked
shim that walks the existing `cel_runtime.wasm`'s arena
helpers but synthesizes the OptionalCell write directly.  More
code; bypasses needing real kernels for Slice 0 verification.

Either way, the current `NoopCelHostThreeArg` (wat_runner.cc:229)
is the wrong fixture — it doesn't write anything to memory, so
the post-eval `memory_after` snapshot contains stale zeros
where the locked layout was supposed to land.

### 6.2 Short-circuit semantics for `or` / `orValue`

P1.  Covered in §"Top three items".  The fix here isn't to
move away from the kernel ABI for `orValue` — for a
constant or pre-evaluated RHS, the kernel works fine.  The
fix is to recognize that codegen needs a separate strategy:

  - When the RHS is a pure expression (literal, ident, no
    function calls), generate the kernel call directly.
  - When the RHS has side effects or errors (CEL has no
    side effects strictly speaking, but error production
    is side-effect-equivalent here), generate a conditional
    branch: evaluate LHS, peek at its present flag, branch
    on present to skip RHS-eval and unwrap or to fall
    through to RHS-eval-and-copy.

This is a codegen decision, not a kernel decision.  Slice 0
should have surfaced it.

If Slice A goes with the eager-eval kernel as designed,
conformance row `optional.of(1).orValue(1/0)` will trip — RHS
evaluates `1/0`, produces `{CEL_ERROR, DIVIDE_BY_ZERO}`, kernel
sees present=1 on LHS, returns `1` (unwrapped) — but **the
`DIVIDE_BY_ZERO` error was still constructed in the workspace
slot**, just discarded.  Whether that conformance-passes
depends on whether the corpus checks the result value or also
checks that no error was reported.  cel-cpp doesn't error
because the jump step never evaluates the RHS at all.

Either way, the design doesn't address this and the WAT
doesn't model it.

### 6.3 No shared-static-None and no explicit forbid

P1.  Covered in §2.  Pick the optimization now, or pick the
contract that explicitly allows it later — but the WAT-locked
ABI has to choose.  The WAT comment "production can layer a
shared-sentinel optimisation on top later without changing
the ABI" is true only if the ABI explicitly says "kernels MUST
treat OptionalCells as immutable after construction."  Slice 0
doesn't say that anywhere.

### 6.4 The `ofNonZeroValue` zero-predicate matrix is wrong for CEL_MESSAGE

P1.  m14-optionals.md §3.4 says:

  > CEL_MESSAGE → not defined; cel-cpp errors

But `third_party/cel-cpp/common/values/parsed_message_value.cc:78-86`
shows `ParsedMessageValue::IsZeroValue()` is defined and
returns true when the message has no unknown fields and no set
fields.  cel-cpp does NOT error on `optional.ofNonZeroValue(<message>)`
— it produces None for the default-constructed message and
Some otherwise.

The conformance corpus has rows like
`optional.ofNonZeroValue(TestAllTypes{})` that expect a
specific behavior.  The design doc's "not defined; cel-cpp
errors" is factually wrong and will silently miscompile that
row in Slice A.

Fix: update §3.4 to say "CEL_MESSAGE → all default fields and
no unknown fields" with citation to the cel-cpp source line.
This is a doc fix, but it's also a Slice 0 ABI decision —
the kernel ABI must handle CEL_MESSAGE in the polymorphic
switch, not error out.  P1.

### 6.5 Receiver-form flatten is asserted, not WAT-locked

The WATs use `(call $cel_optional_has_value_at_v (i32.const 64)
(i32.const 40))` — two args, the optional receiver at slot 40,
the out_slot at 64.  This implies codegen has already flattened
`target → arg0` before emitting this call.  But there's no
codegen WAT showing the receiver-flatten step itself; the WAT
exercises only the post-flatten kernel signature.

That's actually fine for Slice 0 — receiver-flatten is a
codegen pattern that M5.F already uses for `s.contains(sub)`
and similar.  But the WAT comment at `m14_optional_has_value.wat:20-22`
asserts "Receiver-flattening — target → args[0] — happens at
codegen time, mirroring M5.F's `EmitGeneralCall` arm for
`s.contains(sub)`."  This is a claim about codegen, not about
the kernel ABI.  WAT-first locks the kernel side; the codegen
side still needs the unit test that walks the wasm and
confirms M5.F's pattern is reused for `hasValue`.

Not a blocker.  Noted because the WAT prose elides the
boundary between "what's locked by this WAT" (the kernel
signature) and "what's claimed but not locked" (the
receiver-flatten codegen).  P2.

### 6.6 WAT naming convention drift

Slice 0's WATs use the `m14_` prefix; every existing WAT
uses a numeric ordinal (`01_` … `66_`).  The wat-traces.md
header at line 1672 explains: "Named with the `m14_` prefix
(no numeric ordinal) because Slice 0 is authored as a single
batch, not a per-slice continuation of the 0-67 line."

The reasoning is OK but I'd push back: the convention exists
to make `ls doc/.../wat/` sort chronologically.  Either:

  - Use `70_optional_of_int.wat`, `71_optional_has_value.wat`,
    etc. — continues the ordinal scheme, sorts naturally
    after the M7B WATs.
  - Officially drop the numeric scheme going forward and
    update wat-traces.md to document the new naming rule
    (per-milestone prefix).

Picking the third option ("`m14_` because batch, `01_` …
`66_` because earlier") is the worst — readers don't know
the rule.  P2; cosmetic.

### 6.7 wat-traces.md style departure

The Slice 0 entries use `## M14.N.` section headers
(`wat-traces.md:1697, 1727, 1762, 1809`); the existing entries
use `## N.` flat numbering (line 32, 88, 122, etc.).  Same
underlying drift as 6.6.  Mixing two numbering schemes makes
the TOC harder to scan.  P2.

---

## 7. Coverage gaps

  - **No CelValue decoding in the WAT-runner tests.**  See
    §6.1.  This is the biggest one.
  - **No WAT for `optional.none()` standalone.**  See §1.3.
  - **No WAT for `ofNonZeroValue` and the per-kind zero
    predicate.**  See §1.3 and §6.4.
  - **No WAT for `or` (the overload that returns optional).**
    See §1.2 and §6.2.
  - **No WAT for `.value()` on a None operand (error path).**
    The error production from a kernel is part of the ABI;
    "covered by symmetry with `.hasValue()`" is too thin.
  - **No WAT for the receiver-flatten codegen pattern
    applied to a method-form optional call.**  See §6.5.
    Slice B will exercise this in the lowering tests; not
    a Slice 0 blocker.
  - **No WAT for `has(opt.x.y)` (test_only Select on
    optional operand).**  Probe Q13 / m14-optionals.md §1.7.
    Slice B will need this; Slice 0 left it open.

The four tests at `wat_runner_test.cc:983-1031` each follow
the same pattern (load, run, check `eval_return`).  None of
them inspect `memory_after`.  None of them assert any locked
ABI byte.  This is the entire coverage gap rolled into one.

---

## 8. Tech-debt inventory

| # | Severity | Item | Effort | Location |
|---|---|---|---|---|
| D1 | P0 | WAT-runner tests don't decode CelValue from `memory_after`; only check `eval_return`.  Slice 0's "lock" is unverified. | ~2h | `wat_runner_test.cc:983-1031` |
| D2 | P0 | `RegisterPendingM14Imports` binds `NoopCelHostThreeArg` for both 2-arg and 3-arg kernels — the stub writes nothing.  Either replace with a contract-implementing stub or land alongside test changes that don't depend on the stub doing anything. | ~3h | `wat_runner.cc:664-690` |
| D3 | P1 | `or` / `orValue` short-circuit semantics absent from design.  cel-cpp uses a JumpStep; M14 uses an eager kernel. | ~half day to redesign | `m14-optionals.md §1.8` (or-overload entries); WAT 4 |
| D4 | P1 | No WAT for `optional.none()` standalone (kernel ABI not locked). | ~1h | new `m14_optional_none.wat` |
| D5 | P1 | No WAT for `optional.ofNonZeroValue`; per-kind zero predicate ABI not locked. | ~2h | new `m14_optional_of_non_zero.wat` + matrix in trace doc |
| D6 | P1 | §3.4 of m14-optionals.md says "CEL_MESSAGE → not defined; cel-cpp errors" — wrong; cel-cpp's `ParsedMessageValue::IsZeroValue` returns true for default-constructed messages. | ~10min | `m14-optionals.md:299` |
| D7 | P1 | "Absent key" sentinel function `cel_map_lookup_arena_value` named in WAT 3 doesn't exist.  ABI for converting `CEL_ERR_NO_SUCH_KEY` ↔ `optional.none()` is undefined. | ~1h to decide; affects Slice A kernel sig | `m14_optional_select_field.wat:51-55` |
| D8 | P2 | Shared-static-None ruled out without a strong argument; cel-cpp does the optimization at the value layer.  Either lock it now or lock the contract that says future kernels treat OptionalCells as immutable. | ~1h to revise + 30min to update WAT | `m14_optional_of_int.wat:31-36` and trace doc §M14.1 |
| D9 | P2 | Kernel naming `cel_optional_select_field` vs cel-cpp's `select_optional_field` — inverted word order. | ~5min rename | all 4 WATs + trace doc |
| D10 | P2 | WAT filenames use `m14_` prefix; existing convention is numeric ordinal.  Inconsistent. | ~10min rename | 4 WATs + trace doc |
| D11 | P2 | wat-traces.md Slice 0 sections use `## M14.N.` headers; existing sections use `## N.`.  Inconsistent TOC. | ~5min header rename | `wat-traces.md:1697-1827` |
| D12 | P2 | `cel_log.cc:228-230` prints CEL_OPTIONAL as `optional(inner=<offset>)` — doesn't dereference the cell.  Pre-existing stub but will need to print Some/None and recurse once Slice A lands. | ~30min | `compiler_v2/host/cel_log.cc:228` |
| D13 | P2 | The polymorphic-by-source dispatch pattern in `cel_optional_select_field_at_vv` is chosen by feel, not by reference to the kDispatch/kArena precedent.  Document the choice in the trace. | ~15min doc update | trace doc §M14.3 |
| D14 | P2 | Inline-scalar layout alternative (cel-cpp does this in `OptionalValue::Of`) not considered in §3.1's "Alternatives" list. | ~30min doc | `m14-optionals.md:241-249` |
| D15 | P2 | The "value()" success kernel ABI is asserted to share shape with `hasValue` — but its error case (`CEL_ERR_INVALID_ARGUMENT` on None) is not WAT-locked. | ~30min new WAT | new `m14_optional_value_on_none.wat` |

P0 = blocks Slice 0 closeout.  P1 = blocks Slice A start.
P2 = cleanup-while-touched.

---

## 9. Doc drift

### 9.1 Sibling docs that reference an old shape

  - **`m14-optionals.md §3.1 Resolved by Slice 0`**.  The
    table reads as if the layout is locked.  Per §6.3 the
    locking is partial (no shared-None contract pinned).
    Either rewrite §3.1 to explicitly forbid future
    shared-None layering, or annotate that the ABI is
    "extension-compatible" with shared-None.
  - **`m14-optionals.md §1.7 Slice 0 lock`** is well-written
    and correctly cites WAT 3.  No drift.
  - **`m14-optionals.md §3.4 ofNonZeroValue per-kind zero
    predicate`** has the CEL_MESSAGE bug (§6.4 above).
  - **`m14-optionals.md §3.3 type() for CEL_OPTIONAL`** is
    spec-correct but doesn't note that the existing
    `cel_log.cc:228` already has a `CEL_OPTIONAL` arm (D12).
    Minor.
  - **`wat-traces.md` Slice 0 section** is faithful to the
    WATs.  No drift between the trace doc and the WAT
    contents.  Good.

### 9.2 Docs that should be updated by Slice 0 but weren't

  - **`doc/implementation-plan/testing-checklist.md`** — per
    CLAUDE.md "Every merged feature flips at least one box."
    Slice 0 is a feature.  No checkbox flip.  Possibly
    intentional (it's a slice within a milestone, not the
    full feature), but the CLAUDE.md rule doesn't have a
    "slice 0 is exempt" carve-out.  Verify.
  - **`doc/implementation-plan/per-component-test-coverage.md`**
    — should gain a row for `wat_runner_test.cc` /
    `WatRunnerM14Test`.  Slice 0 added 4 new tests; not
    mentioned in the coverage doc.  P2.
  - **`doc/implementation-plan/rewrite/feature-pipeline-checklist.md`**
    — CLAUDE.md says "Start every feature session by copying
    the matching section's checklist into the milestone
    doc's 'In progress' section."  The m14 doc doesn't have
    an "In progress" section with the checklist copied in.
    Possibly the Slice 0 author considered the
    `m14-optionals.md §4 Slice plan` to be the in-progress
    section, which is reasonable.

### 9.3 Closeout status

The m14-optionals.md header still reads "plan — drafted
2026-05-21 from probe evidence; not yet started."  Slice 0
shipping isn't a full milestone closeout, but it IS a status
transition — should read something like "Slice 0 in flight,
Slices A-D not started."  P2.

---

## 10. Sibling-component reconciliation

CLAUDE.md says the reviewer picks "one neighbouring component"
to catch drift by adjacency.  I picked **`cel_log.cc`** because
it's the closest pretty-printer that already has a
`CEL_OPTIONAL` arm.

Findings:

  - `cel_log.cc:228-230` prints CEL_OPTIONAL as a stub:
    `optional(inner=<u32 offset>)`.  Does not dereference
    the OptionalCell, does not distinguish Some/None, does
    not recurse into the inner CelValue.  Already noted as
    D12.
  - The other obvious sibling is `cel_type.c:39` — the
    NULL arm in `kPrimitiveTypeName[]` that Slice A will
    fill in with `"optional_type"`.  m14-optionals.md §2
    correctly cites this.  No drift.
  - `cel_equals_at_vv` doesn't have a CEL_OPTIONAL arm.
    m14-optionals.md §3.2 names this as Slice A work.
    Correct deferral.

Adjacent file that DIDN'T get touched but probably should
have been at least skimmed:

  - **`compiler_v2/codegen/expr_lower_call.cc`** (or wherever
    `EmitGeneralCall` lives — receiver-flatten origin).  The
    WAT 2 header asserts that M5.F's pattern will be reused
    for `hasValue`.  I'd verify the existing function is
    structurally suitable — particularly its handling of
    the case where `target` is itself a kCallExpr (chained
    `.of(1).hasValue()` is exactly this shape).  Not Slice 0
    work; flagged for Slice B's prep.

No drift discovered in the adjacency sweep.

---

## 11. Recommendations

In priority order:

  1. **Land the missing tests** before Slice 0 closes.
     The "WAT-first lock" claim is hollow without
     post-eval byte verification.  Do this by either:
     (a) replacing the no-op stubs with
     contract-implementing stubs that produce the spec
     bytes, or (b) writing a `cel_runtime.wasm` companion
     with just the optional kernels and linking the WATs
     against it.  Option (a) is faster and matches the
     M7B precedent.
  2. **Add a WAT for `optional.none()` and one for
     `optional.ofNonZeroValue(0)`.**  Both lock kernel ABI
     surfaces that Slice A will need.  Don't let "covered
     by symmetry" be the spec.
  3. **Decide on `or`'s short-circuit semantics now.**
     Either: (a) declare that AOT eager-evals both arms and
     accept the conformance delta (and document which rows
     break), or (b) plan codegen to emit a conditional
     branch instead of a kernel call.  This is the kind
     of decision that compounds across Slice B's design.
  4. **Fix §3.4's CEL_MESSAGE entry.**  Five-line doc fix;
     prevents a silent miscompile in Slice A.
  5. **Lock the absent-key contract.**  WAT 3 names a
     function that doesn't exist.  Decide between
     reinterpreting `CEL_ERR_NO_SUCH_KEY` in the optional
     kernel vs adding a new map-lookup variant.  Then
     write down which.
  6. **Reconsider shared-static-None.**  cel-cpp does it
     at the value layer.  We can do it at the offset
     layer cheaply.  The argument against is weak.
  7. **Cosmetic cleanup.**  Rename `cel_optional_select_field`
     ↔ `cel_select_optional_field` (cel-cpp parity); pick a
     WAT naming convention and stick to it; renumber the
     trace doc sections to match.

If items 1–5 land, Slice 0 is a clean handoff to Slice A.
Without them, Slice A's kernel author is making half the ABI
decisions Slice 0 was supposed to make — and the WAT-first
discipline that justified Slice 0 existing as a separate slice
becomes vestigial.

---

## 12. One-line summary

Slice discipline held; the four WATs name the right shapes in
prose; the ABI lock is verifiable in the trace doc but NOT in
the tests; two more WATs (`optional.none`, `ofNonZeroValue`)
are needed before Slice A can start without re-deciding
Slice 0 calls; the `or` short-circuit semantics and the
absent-key contract are real Slice-A blockers the design
papered over.
