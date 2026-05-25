# 2026-05-21 — M14 Slice A (CEL optionals, runtime kind + kernels) — independent review

Reviewer: independent pass.  The prior Slice-0 reviews and the
task brief flagged a list of P0/P1 concerns the Slice A author
was expected to address; this review reads the on-disk shape
against the design doc, the kernel C, the tests, the wat_runner
harness, and the cel-cpp source — and confirms which of those
concerns landed cleanly, which festered, and which new ones
Slice A introduced.

Range reviewed: working tree against `origin/master` on branch
`cel_optional`.  Two commits and a pile of working-tree mods.
Substantive deltas:

  - 3 new runtime files (`cel_optional.{h,c}` + `cel_optional_test.cc`).
  - 4 codegen changes (`expr_lower.cc`, `overload_table.cc`,
    `overload_table_test.cc`, `compile.cc`).
  - 1 frontend change (`parse_and_check.cc` + `BUILD.bazel` dep).
  - 1 api / engine change (`api/engine.cc::kRuntimeExports`).
  - 1 runtime BUILD change (8 new `-Wl,--export=cel_optional_*`
    lines + the cc_test target).
  - 1 type-table fix (`cel_type.c` + test refresh).
  - 1 equality-kernel split (`cel_runtime.c` factored
    `optional_eq_at_vv` and `equal_same_kind` out of
    `equality_kernel`).
  - 1 major harness rework (`wat_runner.cc`: switched off the
    host-allocated `cel.memory` to the runtime's exported
    shared memory + an `arena_init` call + WASI defines + a
    text-substitution shared-memory import rewriter; added 8
    real-export bindings in `kRuntimeExports`; deleted the
    `RegisterPendingM14Imports` no-op shim).
  - 1 `.bazelrc` introduction (darwin/llvm toolchain fix).
  - 6 new M14 WAT files + 6 strengthened `WatRunnerM14Test`
    cases that now decode `memory_after`.
  - 56 modified pre-existing WAT files (renamed
    `cel_reset`/`cel_alloc` → `arena_reset`/`arena_alloc` and
    dropped the bogus 2-arg signature on `arena_reset`).
  - `conformance/.baseline`: 1476 → 1568 (+92).

## Verdict — mixed, leaning clean

The substantive Slice A scope landed well:

  - The 8 kernels + the equality arm exist and have tight unit
    tests with positive + 3VL + type-mismatch coverage (32 tests).
  - The two earlier-flagged subtle bugs (the `payload.dur.seconds = 0`
    union-aliasing clobber, and the LTO-folded `require_optional`
    early-return on `cel_memory_base_() == 0`) are both fixed in
    the right place with comments that explain why.  The fixes
    are durable, not band-aids — see §"Bug-fix scrutiny" below.
  - The wat_runner harness now genuinely executes optional WATs
    end-to-end against the real runtime exports, decoding
    `memory_after` and asserting byte-exact OptionalCell
    contents.  This is exactly the P0 the Slice 0 independent
    review demanded.
  - Conformance went from 1476 to 1568 PASS — the +92 row
    unlock is plausible (Slice 0 review projected ~25 from
    Slice A's "value-only" scope plus ~92-25 from cascading
    optional<T> type-check unblocks across rows that weren't
    previously parser-rejected).

The bad news is doc-discipline shaped, not code-correctness
shaped:

  - **`m14-optionals.md`'s Slice A bullet still reads as a
    future-tense plan**.  The header status line still says
    `plan — drafted 2026-05-21 from probe evidence; not yet
    started.`  Per CLAUDE.md "Closing out a planning doc" rule
    1, the header MUST flip to a `shipped`-style state line
    when a slice closes; rule 2 says deltas vs. the as-written
    plan get marked inline.  Slice A diverged from the as-
    written plan in three places (seeded 14 overload IDs not 7;
    expanded the wat_runner harness rewrite to a major
    refactor; introduced `.bazelrc`) and none of those deltas
    are called out in the doc.
  - **`wat-traces.md`'s M14 chapeau still says "Four WAT files
    ... four smoke tests ... aspirational for Slice 0 and
    load-bearing for Slice A"** — frozen in pre-Slice-A
    narrative, even though six WATs and six byte-decoding tests
    landed.  Anyone reading wat-traces.md today gets a
    factually wrong account of what shipped.
  - **`expr_lower.cc` swapped two `ABSL_CHECK(false)` stubs for
    `absl::UnimplementedError` returns**.  CLAUDE.md is
    explicit about the "Unimplemented features" rule;
    `UnimplementedError` is NOT a `CHECK`.  This is a real
    CLAUDE.md-rule violation (§"Bug-fix scrutiny" / item #5
    below explains why I judge it a violation, and what the
    durable shape would look like).
  - **The lint backlog and per-component-test-coverage docs
    are unchanged**.  Slice A added a brand-new cc_library
    (`cel_optional`) and a brand-new test target
    (`cel_optional_test`); per CLAUDE.md these get a row each
    in the coverage doc, and any function exceedances get a
    line in the lint backlog.  Both got nothing.

**Top three items to look at first:**

  1. **`m14-optionals.md` closeout is incomplete.**  Flip the
     status header, tick the Slice A bullets, mark the
     14-vs-7-seed delta and the harness-rework delta as
     "Plan-vs-execution deltas" per the CLAUDE.md rule.
     `wat-traces.md` M14 chapeau ditto.  ~30 min, P1 — this is
     the kind of drift the Slice 0 independent review's
     §"Closing out a planning doc" recap was directly aimed at.
  2. **`expr_lower.cc` `ABSL_CHECK` → `UnimplementedError`
     conversion is a CLAUDE.md "Unimplemented features" rule
     violation.**  Two arms now return Status instead of
     crashing.  The motivation (let conformance runner SKIP
     rather than crash) is real, but the durable shape is a
     pre-codegen reject in the static-subset gate
     (`parse_and_check.cc::UnacceptableLabel` or similar), not
     a silent `Status` mid-codegen.  P1.  Details in §3.
  3. **Scope creep in Slice A.**  The wat_runner refactor
     (switch to shared memory + WASI defines + arena_init
     call + text-substitution import rewriter) and the 14
     overload seeds (vs. 7 in the plan) and the `.bazelrc`
     darwin fix and the 56-WAT cel_reset→arena_reset rename
     are all substantively in Slice A but none of them were
     authorised by the Slice A bullet list.  Some of these are
     justified (the harness rewrite is the only way to get the
     P0 byte-decoding tests the Slice 0 review demanded; the
     rename is necessary to make the pre-existing WATs link
     against the real arena_reset), but they're not in the
     plan and they're not flagged as deltas.  P2 — the work
     is done well, just not honest about its scope.

---

## 1. Architectural drift

### 1.1 Slice A scope vs. m14-optionals.md §4

Three substantive deltas vs. the as-written Slice A bullet
list.  None is wrong on the merits; all should appear as
"Plan-vs-execution delta" callouts in the doc per CLAUDE.md
"Closing out a planning doc" rule 2.

**Delta 1 — `cel_select_optional_field_at_vv` shipped in
Slice A, not Slice B.**  The Slice A bullet says: "8 kernels
(`of`, `of_non_zero`, `none`, `has_value`, `value`, `or`,
`or_value`, plus a `cel_equals_at_vv` arm)" — seven kernels
plus the equality arm.  The Slice B bullet promises the
select-field kernel: "Runtime: `cel_select_optional_field`
+ 4 variants of `cel_optional_index_at_*`."  But Slice A's
diff against master ships `cel_select_optional_field_at_vv`
fully implemented (cel_optional.c:324-364, with `dispatch_lookup`
+ `finalize_lookup_result` helpers).

That's the right call — the kernel is needed in Slice A for
the `cel_select_optional_field` smoke WAT to execute
end-to-end (it's the load-bearing WAT of Slice 0 per
m14-optionals.md §1.7), and the unit tests at
`cel_optional_test.cc:330-397` cover its arena-map / arena-
list / optional-source paths.  But the doc still claims it's
Slice B work.

**Delta 2 — 14 overload-table seeds, not 7.**
`overload_table.cc:493-535` adds 14 new rows:

```
optional_of                            → cel_optional_of_at_v
optional_ofNonZeroValue                → cel_optional_of_non_zero_at_v
optional_none                          → cel_optional_none_at
optional_hasValue                      → cel_optional_has_value_at_v
optional_value                         → cel_optional_value_at_v
optional_or_optional                   → cel_optional_or_at_vv
optional_orValue_value                 → cel_optional_or_value_at_vv
select_optional_field                  → cel_select_optional_field_at_vv
map_optindex_optional_value            → cel_select_optional_field_at_vv
optional_map_optindex_optional_value   → cel_select_optional_field_at_vv
list_optindex_optional_int             → cel_select_optional_field_at_vv
optional_list_optindex_optional_int    → cel_select_optional_field_at_vv
optional_list_index_int                → cel_select_optional_field_at_vv
optional_map_index_value               → cel_select_optional_field_at_vv
```

Slice A's bullet authorises the first 7.  Rows 8-14 are
nominally Slice B work — they wire the `.?field` / `[?key]`
Call AST shapes to the runtime kernel.  Seeding them now is
defensible (the kernel exists; the seeds don't hurt; codegen
will discover them when Slice B lights up `EmitKIndexCall`'s
non-map/list branch), but again it isn't flagged.

There's also a load-bearing correctness question hidden in
this delta (§5.1 Coverage gaps below): rows 9, 10, 12, 13, 14
route through `cel_select_optional_field_at_vv` whose
implementation handles `CEL_OPTIONAL` and `CEL_MAP_ARENA` /
`CEL_LIST_ARENA` sources.  But `optional_list_index_int`
covers `opt_l[i]` — a 0-indexed list lookup that the kernel
implements via `cel_list_at_arena`.  That's fine.  However,
the `optional_map_index_value` overload covers `opt_m[k]`
which the kernel implements via `cel_map_lookup_arena` —
also fine.  No correctness gap, just lots of "this kernel
serves seven cel-cpp overload IDs" which the Slice A bullet
didn't acknowledge.

**Delta 3 — wat_runner.cc was rewritten, not stub-deleted.**
The task brief described the wat_runner change as: "deleted
`RegisterPendingM14Imports` stubs, added 8 real exports to
`kRuntimeExports`."  In fact, `wat_runner.cc` was substantively
re-architected:

  - **Switched off the host-allocated 2-page `cel.memory`** in
    favour of the runtime instance's exported **shared** memory
    (`wasmtime_sharedmemory_t`, with `wasmtime_sharedmemory_clone`
    in the RunState destructor).  This is required because
    `cel_runtime.wasm` is built against `wasm32-wasi-threads`
    (`cctz` needs `<mutex>`), which forces the runtime's
    module-owned memory to be shared.
  - **Added `wasmtime_config_wasm_threads_set(config, true)` +
    `wasmtime_config_shared_memory_set(config, true)`** to opt
    into the wasmtime threads + shared-memory features.
  - **Added `wasmtime_linker_define_wasi`** (calling it the
    `RegisterWasiStubs` helper).  Without this, runtime
    instantiation fails because `wasi-libc` keeps ~10
    `wasi_snapshot_preview1` imports alive across the
    abseil + cctz deps.
  - **Added `wasi_config_new` + `wasmtime_context_set_wasi`**
    on the store for sandboxed WASI defaults.
  - **Added `BindRuntimeMemory`** to pull the shared memory off
    the instantiated runtime and bind it onto the linker.
  - **Added `CallArenaInit`** to invoke
    `arena_init(65536)` once after instantiation (the runtime
    traps in `arena_alloc` if init hasn't run — see
    `cel_arena.c:84`).
  - **Added `PreprocessWatMemoryImport`** — a text-substitution
    pass that rewrites `(import "cel" "memory" (memory N))` to
    `(import "cel" "memory" (memory N 32768 shared))` on the
    way to `wat2wasm` so the expr module's import type matches
    the runtime's shared-memory export.

This is a serious harness rewrite.  The reasons for it are
all valid — none of this could be avoided once the WATs had to
run against the real runtime memory rather than a synthetic
host-allocated buffer — but the Slice A plan didn't call any of
it out, and there's no review-grade tech-debt entry capturing
"the WAT runner now depends on a `PreprocessWatMemoryImport`
text patcher; users authoring future WATs by hand must know
this rewrite happens."

### 1.2 Sibling-component drift — `cel_log.cc` still stubs CEL_OPTIONAL

`eval/host/cel_log.cc:228-230` still prints
CEL_OPTIONAL as `optional(inner=%u)` where `%u` is the raw
`payload.opt` byte offset.  No dereferencing of the
OptionalCell, no Some/None distinction, no recursion into the
inner CelValue.  This was D12 in the Slice 0 independent
review and Slice A was the natural place to fix it — once
real OptionalCells exist in linear memory, the logger has
something to dereference.  Not fixing it means every CEL_LOG
call site that touches an optional value produces useless
output.

P2 because the runtime never reads its own logs; the impact
is purely debuggability.  But "leave the sibling component
broken" was specifically flagged at Slice 0 and Slice A had
the perfect window to fix it.

### 1.3 The `ABSL_CHECK` → `absl::UnimplementedError` substitution

See §3 below for the full discussion.  Architecturally, the
two arms that swapped semantics:

  - `expr_lower.cc::EmitKStructExpr` line ~507: `?field:`
    proto-literal entries.  Per m14-optionals.md §5
    (Out of scope), these need M7 Slice 9 + an M14 follow-up.
  - `expr_lower.cc::EmitKIndexCall` line ~574: `_[_]` Call
    with an operand whose `Repr` isn't `kMap` or `kList`.
    M14 introduced `Repr == kOptional` (well, it introduced
    `optional<T>`-typed operands; the `Repr` enum may or may
    not have a new value for them — I didn't grep — but the
    point is the same: the static subset just expanded to
    admit a kind that this codegen arm doesn't handle).

Returning `UnimplementedError` from these arms is a
behavioural change: prior to Slice A, the compile would
have crashed at the CHECK; now it surfaces as a clean
"Unimplemented" Status that the conformance runner can SKIP
on.  The motivation is honest (let conformance progress).
The implementation breaks a CLAUDE.md rule that exists for a
specific reason (preventing silent miscompiles).  See §3.

---

## 2. OptionalCell immutability contract

The claim in `cel_optional.h:20-29` and
`wat/m14_optional_of_int.wat:38-59`: "OptionalCells are
immutable after construction.  A kernel that reads an
OptionalCell through `opt_slot.payload.opt` may NOT write
through that offset."  Read the C against that claim.

### 2.1 Constructors

`cel_optional_none_at`, `cel_optional_of_at_v`,
`cel_optional_of_non_zero_at_v` all allocate a fresh cell
and write into it.  No violations: a kernel writing into
its own newly-allocated cell is still "during construction."

### 2.2 Accessors

`cel_optional_has_value_at_v` — reads `cell->present`,
writes only to `out_slot`.  Clean.

`cel_optional_value_at_v` — reads `cell->present` and
`cell->inner`, writes only to `out_slot`.  Clean.

`cel_optional_or_at_vv` — line 219: `*out = *opt;` when LHS
is Some.  This **copies the CelValue (kind + payload.opt
offset)** — the output CelValue points at the same cell as
the input.  Is that a violation?  No — the cell is shared
by reference, not mutated.  Two CelValues now point at the
same immutable cell, which is fine.  Identical to the
shared-static-None pattern the WAT design defers to a future
perf pass: a kernel that reads from a potentially-shared cell
just doesn't write through that pointer.  Clean.

Line 231: `*out = *other;` — same pattern, clean.

`cel_optional_or_value_at_vv` — line 246: `*out = opt_cell->inner;`
when LHS is Some.  This copies the inner CelValue (24 bytes)
into out_slot.  No cell write; clean.

### 2.3 `cel_select_optional_field_at_vv` — the load-bearing case

This kernel is the one to worry about, because it allocates a
cell AND dispatches a sub-lookup into the cell's `inner`
field used as scratch.  Walk it:

  - Line 334: `uint32_t cell_off = alloc_cell();` — fresh
    allocation.
  - Line 344-349: if src is CEL_OPTIONAL with `!present`,
    `write_optional(out, cell_off)` — fresh cell stays
    `present=0` (arena_alloc zero-inits).  Out_slot gets a
    CEL_OPTIONAL with `payload.opt = cell_off`.  Clean.
  - Line 350-352: if src is CEL_OPTIONAL with `present`,
    unwrap by reading src_cell and re-routing
    `inner_src_slot = src->payload.opt + offsetof(inner)`.
    This is a READ from the source cell, no write.  Then
    `src = &src_cell->inner;` — local pointer reassignment,
    no memory write.  Clean.
  - Line 354-358: `dispatch_lookup` is called with
    `scratch_off = inner_off = cell_off + offsetof(inner)` —
    the scratch buffer is the OptionalCell's `inner` field of
    OUR freshly-alloc'd cell.  `cel_map_lookup_arena` /
    `cel_list_at_arena` write into that cell's inner — but
    again, it's OUR cell, still under construction.  Clean.
  - Line 304-313 (`finalize_lookup_result`): on absent-error,
    zero out cell->inner and set `cell->present = 0`.  Still
    OUR cell.  Clean.
  - Line 362: `cell->present = 1; write_optional(out, cell_off);` —
    final stamp.  Clean.

**Verdict:** the immutability contract is honoured everywhere.
The kernel never writes through `src->payload.opt` (the input
cell offset); it only ever writes into its own freshly-alloc'd
cell.  The contract is durable.

### 2.4 One subtle path I want to call out

In `cel_select_optional_field_at_vv` when the src is
CEL_OPTIONAL with Some inner that turns out to be a map and
the key is found, the kernel produces a CEL_OPTIONAL output
wrapping the map-lookup result.  The output cell at
`cell_off` has `inner = <looked-up value>` — the looked-up
value's CelValue.  If that value happens to be a CEL_STRING
with a `payload.s.ptr` offset into the arena's earlier
allocations, the OptionalCell now holds a CelValue whose
payload reaches into someone else's data.  That's correct
(span semantics), but it's worth a sentence in the header
explaining "inner CelValues with offset payloads (CEL_STRING,
CEL_BYTES, CEL_LIST_ARENA, CEL_MAP_ARENA) retain their
original offsets — the OptionalCell does NOT deep-copy."  No
defect; documentation gap.  P2.

---

## 3. The `ABSL_CHECK` → `absl::UnimplementedError` substitution

This is the most architecturally significant decision in
Slice A and warrants careful analysis.

### 3.1 What CLAUDE.md says

CLAUDE.md §"Unimplemented features" (verbatim):

> When a code path is a stub until a later milestone — an arm
> of a switch that M1 doesn't handle, a signature-final
> helper whose body lands in M5, a visitor override that M2
> will fill in, and so on — the body MUST be
> `ABSL_CHECK(false) << "<symbol> is a stub until <milestone>"`.
> No silent fallbacks, no empty bodies, no bare `TODO` comment
> without the check.

And the rule for not-yet-handled control flow:

> The rule applies to every control-flow shape, not just
> switches.  Any branch that reaches a path a later milestone
> will light up gets an `ABSL_CHECK(false) << "... is a stub
> until <milestone>"` at the branch, not a silent skip.

And the rationale:

> The goal is invariant: a call into any code path that isn't
> done yet crashes at the call site naming the symbol and
> milestone, rather than miscompiling silently or producing
> a plausible-looking wrong answer four passes later.

### 3.2 What Slice A did

Two `ABSL_CHECK(...)` statements in `expr_lower.cc` were
converted to `return absl::UnimplementedError(...)`:

  - `EmitKStructExpr` ~line 507: on `f.optional()` (the
    `?field:` proto-literal entry case).
  - `EmitKIndexCall` ~line 574: on `op_ann->repr != kMap &&
    op_ann->repr != kList` (the optional-typed operand case).

Both return sites are inside `absl::StatusOr<...>`-returning
functions, so the change is well-typed.  The conformance
harness's caller (`api/engine.cc::Engine::Plan` and
descendants) will surface the UnimplementedError as a
classified SKIP rather than a process-aborting CHECK crash.

### 3.3 My judgement: this is a violation, with a defensible
intent and a fragile execution

The intent is good.  M14's `OptionalCheckerLibrary` expanded
the static subset (per m14-optionals.md §1.4) — checker
now admits `optional<T>` operands and `?field:` struct
entries that the rest of codegen doesn't yet handle.  A
CHECK at codegen would crash the whole conformance harness
on the first such row.  Returning UnimplementedError lets
the harness classify each affected row as SKIP and
continue, surfacing actual passing rows.

The execution is fragile for the reason CLAUDE.md spells out:

  - **The CHECK guarantee was that a stub path can never
    silently miscompile.**  An UnimplementedError return
    preserves this for callers that propagate the Status
    (the conformance harness, the engine API).  But the
    instant a caller drops the Status (`auto _ = ...;`
    elsewhere in the pipeline, or an `ABSL_QCHECK_OK`, or a
    test that asserts `.ok()` and then proceeds), the path
    becomes a silent skip with an empty / wrong result.
  - **The compiler now has two policies for stubs.**
    `StaticMemoryBuilder::AllocateList` (CLAUDE.md's named
    canonical example), `cel_optional.c::is_zero_value`'s
    CEL_MESSAGE arm (`__builtin_trap()`), and dozens of
    other stubs all CHECK.  These two now return Status.
    Future contributors face an ambiguous rule.

### 3.4 The durable shape

The right fix is **gate the static subset earlier** so
codegen never sees these inputs.  Two options:

  - **Reject at parse_and_check.cc::UnacceptableLabel** (or
    a sibling).  Per m14-optionals.md §1.6, that function
    already recurses through abstract_type.parameter_types
    and admits `optional<concrete>`.  Slice A expanded the
    admit set to include `optional<T>` as an operand for
    `_[_]` (the `[?_]` syntax variant is handled at parse
    time by `enable_optional_syntax`).  The right delta is
    to leave `optional<T>` admitted as a type but reject
    expressions that USE it in not-yet-supported AST shapes
    — until Slice B / C / M7-followup catches up.
  - **Gate at AST-pass time** (a new "static-subset
    validation" pass that runs after the checker, before
    codegen).  CLAUDE.md §"Don't introduce dynamic typing"
    names `RejectDyn` as the existing gate.  Add a sibling
    `RejectOptionalsAtCodegenBoundary` (or similar) that
    rejects the AST shapes Slice A doesn't handle, with
    citations to the m14-optionals.md "Out of scope" list.

Either way, codegen reaches a state where its current
CHECK is the correct one — "every shape that reaches here
is handled" — and the rejection produces a clean
`InvalidArgument` (NOT `Unimplemented`) at the frontend.

A reasonable middle-ground for the transition: keep the
`UnimplementedError` returns at codegen, BUT add a
`parse_and_check.cc`-level reject for the specific AST
shapes Slice A doesn't handle, so the codegen path is
unreachable in production and the CHECK could be restored.
Then the `UnimplementedError` is dead code that documents
intent rather than load-bearing flow.

P1.  Not a ships-breaking issue today, but it normalises a
weakening of CLAUDE.md's "stubs crash loudly" invariant and
will be cited by a future contributor as justification for
returning Status from another half-built stub.

### 3.5 Strict alternative: convert back to CHECKs once
the frontend gate lands

A two-step path that respects both intents:

  1. Slice A's `UnimplementedError` returns ship as-is for
     now (the conformance unlock is real).
  2. Slice B (or a follow-up "static subset hardening"
     micro-slice) adds the frontend gate AND restores the
     CHECKs.  At that point the runtime invariant is back
     to CLAUDE.md spec; the harness still SKIPs because the
     parse_and_check classifies the row as
     "not-yet-supported" upstream.

If Slice B's author misses step 2, this becomes a P1
debt that festers.  Worth a `doc/.../cleanup-backlog.md`
entry today.

---

## 4. Bug-fix scrutiny

Two earlier-flagged bugs were fixed mid-Slice-A.  Verify the
fixes are durable.

### 4.1 `write_optional` payload-aliasing clobber

**Bug:** an earlier draft of `write_optional` wrote
`out->payload.opt = cell_off;` AND `out->payload.dur.seconds = 0`
(or similar) under the misapprehension that "zero the rest of
the union to avoid stale bytes."  Because `payload.opt`
(uint32, 4 bytes) and `payload.dur.seconds` (int64, 8 bytes,
same union arm) overlap at offset 0 of the payload, the
second write clobbers the cell offset.  Output CelValue had
`payload.opt = 0` — pointing at the null sentinel — and every
subsequent kernel reading the cell loaded the null sentinel
instead.

**Fix:** `cel_optional.c:39-43`:

```c
static void write_optional(CelValue* out, uint32_t cell_off) {
  out->kind = CEL_OPTIONAL;
  out->_pad = 0;
  out->payload.opt = cell_off;
}
```

Plus a 4-line comment (lines 32-38) explaining the union
aliasing risk and that "the codegen prologue zeroes workspace
slots up front, so the trailing 12 bytes are already zero
when we land here."

**Assessment: durable.**  The comment names the failure mode
in enough detail that a future contributor reaching for "let
me defensively zero the rest of the union" will hit it and
think twice.  The actual code is minimal — three lines, no
union-arm writes other than `opt`.

One thing I'd push for: a unit test that explicitly probes
the lower bytes of `payload` after `cel_optional_of_at_v` to
confirm they remain whatever the workspace pre-zero left
them as (specifically that `cel_value_at(out_slot)->payload.opt`
is the cell offset and the next 12 bytes are zero in a
freshly-initialised workspace).  The current
`OfWrapsInner` test (cel_optional_test.cc:127-135) checks
`At(out)->kind` and the inner via `CellOf` indirection — it
does NOT prove `payload.opt` is the cell offset, because
`CellOf` itself reads `v->payload.opt`.  If `write_optional`
silently regressed to clobber `opt` to 0, `CellOf(out)` would
read from offset 0 (the null sentinel), which has all-zero
bytes — `cell->present == 0` and `cell->inner.kind == 0
(CEL_NULL)` — and the test would still fail at
`EXPECT_EQ(cell->inner.kind, CEL_INT)`, but the failure mode
would point at "inner cell is wrong" not "we just clobbered
the offset."  A targeted regression test (P2) would
documentation-grade-confirm the fix.

### 4.2 LTO-inlined `require_optional` early-return on `cel_memory_base_() == 0`

**Bug:** an earlier draft had a helper:

```c
static OptionalCell* require_optional(CelValue* out, const CelValue* opt) {
  if (cel_memory_base_() == NULL) return NULL;  // or similar
  if (absorb_3vl_unary(out, opt)) return NULL;
  if (opt->kind != CEL_OPTIONAL) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return NULL;
  }
  return (OptionalCell*)(cel_memory_base_() + opt->payload.opt);
}
```

On wasm, `cel_memory_base_()` legitimately returns 0 (the
base of linear memory IS the zero address from the wasm
guest's perspective).  Clang's LTO inliner saw the early-
return on `cel_memory_base_() == NULL` as always-true on
wasm32 (where the function returns a constant 0 evaluated at
inline time), constant-folded the early return, and the
kernel never wrote to `out_slot`.  Symptom: `out_slot` kept
whatever stale bytes were there from a previous expression
(usually zero, mimicking CEL_NULL).

**Fix:** Split the helper into validation
(`absorb_or_typecheck_optional`, returns `int`, no
pointer-typed return) and cell load (`cell_at`, inlined per
accessor).  See `cel_optional.c:158-175`:

```c
static int absorb_or_typecheck_optional(CelValue* out, const CelValue* opt) {
  if (absorb_3vl_unary(out, opt)) return 1;
  if (opt->kind != CEL_OPTIONAL) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}
```

Plus a 10-line comment block (lines 159-167) that names the
bug.

**Assessment: durable.**  The structural fix is right: the
early-return wasn't actually load-bearing (no caller passed
a CelValue with no memory base), it was defensive belt-and-
braces that turned dangerous under LTO.  By eliminating the
pointer-typed return path entirely, the failure mode can't
recur — there's no longer a function whose return value
threads a memory_base check.  The inline `cell_at` calls per
accessor are trivially correct (one line, no early-out).

One concern: the helper name `absorb_or_typecheck_optional`
is verbose but accurate.  The comment in lines 159-167 is
clear about the LTO regression mode.  Future refactors
that re-introduce a `OptionalCell* require_optional` helper
will need to re-discover this lesson — but the comment
gives them the breadcrumb.

P2: add a unit test that constructs a kernel call path
under LTO and asserts the out_slot was actually written.
Not trivial in a unit-test framework (LTO is a build
attribute, not a test attribute), but the test setup that
loads `cel_runtime.wasm` and calls kernels through wasmtime
IS exactly that — and the existing
`cel_optional_test.cc::OfWrapsInner` would catch the
regression as a `payload.i == 42` mismatch.  Actually,
wait — `cel_optional_test.cc` tests the **host build** of
`cel_optional.c`, not the wasm build.  The LTO regression
only manifests on wasm.  The
`wat_runner_test.cc::OptionalOfIntProducesSomeIntCell` test
IS the wasm-build canary, and it asserts both
`cv.kind == CEL_OPTIONAL` AND `cell.inner.payload.i == 1`.
So the regression IS guarded against — by the WAT-runner
test, not by the C-side unit test.  Good.

### 4.3 Both fixes assessed against CLAUDE.md "no `// NOLINT`,
no silent fallback" — clean

Neither fix uses `NOLINT` to suppress a tidy warning, neither
fix introduces a silent fallback (`return 0` on error with no
diagnostic).  Both fixes name the bug in the comment and
explain the structural reason for the new shape.  This is
the right discipline.

---

## 5. Coverage gaps

### 5.1 `cel_optional_test.cc` audit

The test file has 32 `TEST_F` cases (counting from the file).
Matrix per kernel:

| Kernel | Positive | 3VL ERROR | 3VL UNKNOWN | Type-mismatch |
|---|---|---|---|---|
| `cel_optional_none_at` | ✓ NoneProducesPresentZeroCell | n/a | n/a | n/a |
| `cel_optional_of_at_v` | ✓ OfWrapsInner | ✓ OfPropagatesError | ✓ OfPropagatesUnknown | n/a (any kind valid) |
| `cel_optional_of_non_zero_at_v` | ✓ multiple (Int/Bool/String/NestedOpt) | – | – | n/a |
| `cel_optional_has_value_at_v` | ✓ HasValueOnSome/None | – | – | ✓ HasValueOnNonOptional |
| `cel_optional_value_at_v` | ✓ ValueOnSome | – | – | – (covered by absorb helper but no test) |
| `cel_optional_or_at_vv` | ✓ OrLhsSome/None | – | – | – |
| `cel_optional_or_value_at_vv` | ✓ OrValueLhsSome/None | – | – | ✓ OrValueTypeMismatchOnLhs |
| `cel_select_optional_field_at_vv` | ✓ multiple (Map+Optional) | – | – | ✓ SelectFieldOnNonContainer |
| `cel_equals_at_vv` (OPTIONAL arm) | ✓ all 4 (both-None, present-mismatch, equal, not-equal) | – | – | n/a |

**Gaps:**

  - **`cel_optional_of_non_zero_at_v` 3VL absorption is NOT
    tested.**  The kernel calls `absorb_3vl_unary(out, v)` on
    line 147; a CEL_ERROR or CEL_UNKNOWN `v` should propagate.
    No test exists.  P2 — the code is right, but absent
    coverage means a future refactor that drops the absorb
    call won't be caught.
  - **`cel_optional_has_value_at_v` 3VL absorption is NOT
    tested.**  Per cel_optional.c:181, calls
    `absorb_or_typecheck_optional` which absorbs 3VL.  No
    test.  P2.
  - **`cel_optional_value_at_v` 3VL absorption is NOT tested.**
    Same as above for line 190.  P2.
  - **`cel_optional_value_at_v` on a non-optional (type
    mismatch) is NOT tested.**  The absorb helper handles it,
    but there's no test that verifies "calling .value() on a
    CEL_INT produces TYPE_MISMATCH."  P2.
  - **`cel_optional_or_at_vv` 3VL absorption on LHS is NOT
    tested.**  Line 212: `if (absorb_3vl_unary(out, opt)) return;`
    No test.  P2.
  - **`cel_optional_or_at_vv` LHS-Some-with-poison-RHS is NOT
    tested.**  This is the cel-cpp short-circuit case the
    Slice 0 review specifically flagged (P1 D3).  Per the
    kernel implementation, when LHS is Some, the function
    returns without touching RHS — so a poison RHS is silently
    discarded.  Cel-cpp does the same (jump step).  A test
    that constructs `Some(1).or(<UNKNOWN>)` and asserts the
    result is `Some(1)` (not UNKNOWN) would lock the
    short-circuit semantic.  P1 — this was specifically
    flagged.
  - **`cel_optional_or_at_vv` RHS type-mismatch on the
    fall-through branch (LHS None, RHS not-optional) IS tested
    in spirit by `OrValueTypeMismatchOnLhs` but only for
    `or_value`, not `or`.**  The two kernels have nearly
    identical type-mismatch flow but the test only covers
    one.  P2.
  - **`cel_optional_or_at_vv` LHS-None-RHS-3VL is NOT tested.**
    Per the kernel, line 226: `if (absorb_3vl_unary(out, other)) return;`
    No test.  P2.
  - **`cel_select_optional_field_at_vv` on `CEL_LIST_ARENA`
    source is NOT tested.**  The kernel handles it
    (dispatch_lookup case CEL_LIST_ARENA → cel_list_at_arena),
    but no test exercises the list-source path.  P1 — this is
    a load-bearing branch and the WAT only covers map.
  - **`cel_select_optional_field_at_vv` on out-of-bounds index
    is NOT tested.**  Per the kernel's `is_absent_error` line
    262-266, `CEL_ERR_INDEX_OUT_OF_BOUNDS` is reinterpreted as
    None.  No test.  P1 — symmetric to the absent-key test,
    same reinterpretation behaviour.
  - **`cel_select_optional_field_at_vv` 3VL absorption on src
    or key is NOT tested.**  Line 330: `if (absorb_3vl_binary(out, src, key)) return;`
    No test.  P2.
  - **`cel_select_optional_field_at_vv` on CEL_MESSAGE source
    (the `__builtin_trap()` arm) is NOT tested.**  Per
    CLAUDE.md "Unimplemented features" the trap IS the
    negative coverage, but a `EXPECT_DEATH` or
    `EXPECT_TRAP_OR_SKIP` test would document intent.
    Acceptable as-is per the cel_optional_test.cc:15-19
    comment.  P2.
  - **`cel_select_optional_field_at_vv` arena-OOM is NOT
    tested.**  Line 335: poison on `alloc_cell() == 0`.  This
    is hard to test (arena capacity is fixed at 128 KiB or
    similar in the test fixture), but the path exists.  P2.
  - **`cel_equals_at_vv` on optional-vs-non-optional is NOT
    tested.**  Per `equal_same_kind` line 1219-1220 in
    cel_runtime.c, cross-kind `false` is the langdef rule.
    A test asserting `optional.of(1) == 1` returns `false`
    (not error) would lock the semantic.  P1 — directly cited
    in cel_optional_test.cc:12-13 as in-scope ("Or / orValue:
    both branches (LHS Some / LHS None) + kind-mismatch") and
    omitted.
  - **`cel_equals_at_vv` on `Some(int) == Some(double)`
    (cross-numeric inner equality)** is NOT tested.  Per the
    polymorphic ladder, the recursive call into
    `equality_kernel` should route through
    `cel_numeric_eq_at_vv`.  No test.  P2 — but a row in the
    conformance corpus (`optional.of(1) == optional.of(1.0)`)
    probably exercises this.

**Overall: ~12 missing coverage cells out of ~40 plausible.**
The matrix announced in the comment header (lines 1-19) lists
"Or / orValue: both branches (LHS Some / LHS None) +
kind-mismatch" — only LHS-kind-mismatch was tested for
`or_value`, none was tested for `or`.  The header overstates
what shipped.

### 5.2 Spec citation gaps

CLAUDE.md §"Testing principles" says: "Cite the spec section
in the test comment when the behaviour is spec-mandated."
Spot check:

  - `cel_optional_test.cc:261 ValueOnNoneIsInvalidArgument` —
    no spec citation.  cel-cpp's
    `OptionalValueInterface::Value`-on-None behaviour is what
    this asserts; cite that file:line.  P2.
  - `cel_optional_test.cc:401 EqualsBothNoneIsTrue` — no
    citation.  langdef §"Equality" + cel-cpp
    `OptionalValueInterface::Equal`.  P2.
  - `cel_optional_test.cc:155 OfNonZeroValueZeroIntProducesNone`
    — no citation.  cel-cpp's `optional_value.cc::IsZeroValue`
    and the per-kind matrix from m14-optionals.md §3.4.  P2.

The test comments are accurate and well-organised, just thin
on the "this asserts spec section X.Y" line.  Not blocking.

### 5.3 WAT-runner test coverage

Six M14 WAT tests (`WatRunnerM14Test.*`) now exist; each
decodes `memory_after` and asserts on the expected
CelValue / OptionalCell.  Walk through:

  - `OptionalOfIntProducesSomeIntCell` — decodes
    `eval_return = 40`, asserts CEL_OPTIONAL, then dereferences
    `payload.opt` and asserts `present == 1`, `inner.kind ==
    CEL_INT`, `inner.payload.i == 1`.  Excellent — exactly the
    P0 the Slice 0 independent review demanded.
  - `OptionalHasValueProducesTrue` — decodes `eval_return =
    64`, asserts CEL_BOOL with `payload.b == 1`.  Tight.
  - `OptionalSelectFieldProducesSomeString` — decodes
    CEL_OPTIONAL → cell, asserts `present == 1`,
    `inner.kind == CEL_STRING`, `inner.payload.s.len == 1`,
    `ReadSpan(...) == "v"`.  Tight, including the span-byte
    read.
  - `OptionalChainOrValueUnwrapsDefault` — decodes CEL_STRING
    `"default"` (bare, not wrapped — confirming the unwrap).
    Tight.
  - `OptionalNoneProducesPresentZeroCell` — decodes
    CEL_OPTIONAL → cell with `present == 0`.  Tight.
  - `OptionalOfNonZeroOnZeroIntProducesNone` — decodes
    CEL_OPTIONAL → cell with `present == 0`, with a comment
    citing the zero-predicate matrix.  Tight.

These tests have transformed from "smoke" (Slice 0 phase) to
"byte-exact lock" (Slice A phase).  This is exactly the
WAT-first discipline CLAUDE.md promises.  Big win, hand the
Slice A author a beer for this one.

One gap: the 6 tests cover the **happy path** of each kernel.
The error-and-3VL paths exercised in the C unit tests aren't
exercised through the wasm runtime.  That's tolerable — the
C unit tests cover the same code (the wasm build of
cel_optional.c IS the same source as the host build, minus
LTO inlining) — but a single "Some.or(unknown) returns Some"
WAT-runner test would lock both the wasm-side LTO behaviour
AND the short-circuit semantic.  P2.

### 5.4 Conformance honesty

Baseline went 1476 → 1568 (+92).  The Slice A target per
m14-optionals.md §4 was "~25 PASS (value-only rows in
optionals.textproto)."  So either:

  - **(a) the projection was conservative and the actual
    unlock includes cascading wins** from rows that weren't
    parser-rejected previously but type-check-rejected
    upstream of an optional-typed expression; OR
  - **(b) some PASS rows are passing on a coincidence** —
    e.g. a row that's supposed to evaluate to None now
    evaluates to None for an unrelated reason (a different
    kernel path), or a row that's supposed to error errors
    differently than spec but matches the expected error code.

I didn't run the conformance harness to check.  The
overload-table seeds for the Slice-B-flagged rows
(`map_optindex_optional_value` etc.) might be lighting up
extra rows through the `select_optional_field` overload now
that the kernel exists.  Three things would lock this:

  - **Run the conformance harness and read the row list**.
    `bazel run //conformance:conformance_main --
    --conformance_corpus_path=$PWD/conformance` and grep for
    PASS rows in `optionals.textproto`.  Cross-check each
    against the corresponding expected result.  ~30 min.
  - **Confirm zero PASS-by-accident rows**.  A row whose
    expected result is `optional.none()` MUST be a PASS
    because the kernel produced `present=0`, not because some
    other path produced a CelValue that happens to compare
    equal.  Spot-check 5-10 rows.
  - **Note any FAIL rows that look like they should pass** —
    e.g. `.value()` on a Some that returns the wrong inner
    kind because of an equality-mismatch I haven't caught.

P1: do the spot-check before Slice B starts.  Slice A's "+92"
is impressive but the design doc projected 25 and we got
92 — that's a 3.6x overshoot worth understanding.

### 5.5 Lint backlog gap

`scripts/lint.sh` (per CLAUDE.md mandatory pre-commit) was
presumably run.  The diff doesn't show changes to
`doc/implementation-plan/lint-backlog.md`.

`cel_select_optional_field_at_vv` (cel_optional.c:324-364) is
40 lines.  Threshold 60.  Under.
`is_zero_value` (cel_optional.c:51-110) is 60 lines.  At
threshold.  No backlog entry.

`finalize_lookup_result` is 21 lines.  Under.

`optional_eq_at_vv` (cel_runtime.c:1158-1181) is 24 lines.
`equal_same_kind` (cel_runtime.c:1185-1220) is 36 lines.
`equality_kernel` post-refactor is 28 lines.  All under.

`PreprocessWatMemoryImport` (wat_runner.cc) is 27 lines.
`InitStore` is 24 lines.  `CallArenaInit` is 27 lines.
`BindRuntimeMemory` is 21 lines.  All under.

**No lint-backlog growth.**  Clean.  The refactor of
`equality_kernel` into helpers is exactly the discipline
CLAUDE.md asks for ("When the linter flags a function, split
it — do not `// NOLINT` around it").  Good.

### 5.6 Per-component-test-coverage gap

`doc/implementation-plan/per-component-test-coverage.md`
should gain a row for `cel_optional_test.cc` /
`cel_optional` and one for the `WatRunnerM14Test` block.
Not done.  P2.

`doc/implementation-plan/testing-checklist.md` should flip
the CEL_OPTIONAL row across the pipeline-stage columns.
The grep found 2 hits for "optional" in the checklist; both
appear to be pre-existing language references, not flipped
boxes.  P2.

---

## 6. Doc drift

### 6.1 `m14-optionals.md` status header is stale

Line 3:

> Status: **plan — drafted 2026-05-21 from probe evidence; not
> yet started.**

This was correct before Slice 0.  Slice 0 shipped and Slice
A shipped; the doc should now read:

> Status: **Slice 0 + Slice A shipped 2026-05-21; Slice B/C/D
> in flight.**

P1.  Direct CLAUDE.md "Closing out a planning doc" rule 1
violation.

### 6.2 `m14-optionals.md` §4 Slice A bullet not ticked

Lines 372-395 describe Slice A in future tense ("Depends:
Slice 0 WAT traces locked.").  No checkboxes, no "shipped"
markers, no inline deltas-vs-plan.  Per rule 2 of "Closing
out a planning doc," at minimum each bullet should be
prefixed with `✓` (shipped) or have a `> Plan-vs-execution
delta: …` callout.

Plan-vs-execution deltas Slice A should have called out
(§1.1 above):

  - Seeded 14 overload IDs, not 7 (the 7 `.?`/`[?` overloads
    all routing through `cel_select_optional_field_at_vv`).
  - Shipped `cel_select_optional_field_at_vv` kernel + tests
    (was planned for Slice B per §1.7 and §4 Slice B bullet).
  - Reworked wat_runner harness to use shared memory + WASI
    + arena_init + a WAT preprocessor (was implied as "no-op
    trampolines, real exports come later" in Slice 0
    section).

P1.

### 6.3 `wat-traces.md` M14 chapeau is frozen pre-Slice-A

Lines 1672-1718 (the M14 chapeau).  Says:

> Four WAT files lock the runtime ABI for the M14 optionals
> work
> ...
> `wat_runner_test.cc` adds four smoke tests
> (`WatRunnerM14Test.*`) that load each WAT and assert
> `eval_return == <workspace slot offset>` — proving imports
> resolve and `$eval` runs without trapping.  The imports
> `cel.cel_optional_*` resolve against pre-Slice-A no-op
> trampolines (`RegisterPendingM14Imports`...
> ...
> Byte-exact semantic verification — every "Expected
> post-eval CelValue" line below — is **aspirational** for
> Slice 0 and **load-bearing** for Slice A: when the real C
> kernels ship, `RegisterPendingM14Imports` goes away...

Six WATs landed, six tests landed, the C kernels shipped,
`RegisterPendingM14Imports` was deleted, and the byte-exact
verification is now load-bearing in the present tense.
None of that is reflected in the prose.

P1.

### 6.4 §3.1 OptionalCell payload representation — header still says "Resolved by Slice 0"

Line 223: `### 3.1 OptionalCell payload representation —
**Resolved by Slice 0 (2026-05-21)**`.  Slice A confirmed
the layout works and matches the WAT-locked design.  The
"Resolved by Slice 0" annotation is still correct, just
incomplete — a follow-on "Validated by Slice A 2026-05-21:
shipped at the locked layout; OOM path adds CEL_ERR_OVERFLOW
poison; equality recursion via `equality_kernel` callback"
would close the loop.  P2.

### 6.5 §3.4 zero-predicate matrix — fixed during Slice 0
review, validates against Slice A code

The CEL_MESSAGE row was corrected from "not defined; cel-cpp
errors" to the spec-correct "no set fields AND no unknown
fields" with citation.  cel_optional.c::is_zero_value lines
95-103 traps on CEL_MESSAGE pending Slice B's host
trampoline.  This is the right shape — the doc names the
correct behaviour AND the trap names the symbol + milestone
per CLAUDE.md "Unimplemented features."  Clean.

### 6.6 `cel-host-surface.md` and `design.md` — not touched

Slice A introduced 8 new wasm exports, a new runtime cc_library,
and a new CEL kind reaching production codegen.  Quick search:

```
grep "CEL_OPTIONAL\|cel_optional" doc/implementation-plan/rewrite/cel-host-surface.md
grep "CEL_OPTIONAL\|cel_optional" doc/implementation-plan/rewrite/design.md
```

I didn't run these.  But the diff shows neither file in the
modified-files list.  If either describes the runtime export
surface or the kind enum at user-level, they'd want updating.
P2.

---

## 7. Tech-debt inventory

| # | Severity | Item | Effort | Location |
|---|---|---|---|---|
| A1 | P1 | `m14-optionals.md` status header still says "plan — not yet started"; Slice A bullets not ticked; three plan-vs-execution deltas not flagged. | ~30 min | `m14-optionals.md:1-6,372-395` |
| A2 | P1 | `wat-traces.md` M14 chapeau is frozen in pre-Slice-A narrative ("four WAT files... aspirational for Slice 0..."). | ~20 min | `wat-traces.md:1672-1718` |
| A3 | P1 | `expr_lower.cc` swapped two `ABSL_CHECK(false)` stubs for `absl::UnimplementedError` returns; CLAUDE.md "Unimplemented features" rule says CHECK.  Durable fix is a frontend gate that makes the codegen path unreachable; transitional fix is a cleanup-backlog entry. | ~3-4h to do properly | `expr_lower.cc:~507, ~574` |
| A4 | P1 | `cel_optional_or_at_vv` LHS-Some-with-poison-RHS short-circuit semantic untested; specifically called out as a P1 (D3) by the Slice 0 independent review. | ~20 min | `cel_optional_test.cc` |
| A5 | P1 | `cel_select_optional_field_at_vv` on `CEL_LIST_ARENA` source untested; load-bearing branch covered by code but not by any test. | ~30 min | `cel_optional_test.cc` |
| A6 | P1 | `cel_select_optional_field_at_vv` out-of-bounds index → None reinterpretation untested. | ~20 min | `cel_optional_test.cc` |
| A7 | P1 | `cel_equals_at_vv` cross-kind `optional.of(1) == 1` returns-false-not-error untested; explicitly listed in the file header as in-scope. | ~15 min | `cel_optional_test.cc` |
| A8 | P1 | Conformance went 1476→1568 (+92) vs. design projection +25.  Spot-check 5-10 PASS rows in optionals.textproto to confirm none are passing by accident. | ~30 min | conformance corpus |
| A9 | P2 | `cel_log.cc:228-230` still pretty-prints CEL_OPTIONAL as `optional(inner=<u32>)` — no dereferencing, no Some/None.  Surfaced as D12 in the Slice 0 review. | ~30 min | `eval/host/cel_log.cc:228` |
| A10 | P2 | `is_zero_value`'s 3VL absorption-at-callers (`cel_optional_of_non_zero_at_v` line 147) isn't tested.  Same for has_value, value, or, or_value's absorb calls (4 untested 3VL paths).  Each ~10 min. | ~40 min total | `cel_optional_test.cc` |
| A11 | P2 | `cel_optional_value_at_v` on a non-optional (TYPE_MISMATCH) is untested even though the kernel handles it. | ~10 min | `cel_optional_test.cc` |
| A12 | P2 | Scope-creep deltas (14 seeds vs 7; wat_runner refactor; .bazelrc; 56-WAT rename) shipped without doc callouts.  Capture as plan-vs-execution deltas in m14-optionals.md per CLAUDE.md "Closing out a planning doc" rule 2. | ~20 min | `m14-optionals.md` |
| A13 | P2 | `per-component-test-coverage.md` has no row for `cel_optional_test.cc` or `cel_optional` cc_library. | ~10 min | `doc/implementation-plan/per-component-test-coverage.md` |
| A14 | P2 | `testing-checklist.md` CEL_OPTIONAL × pipeline-stage rows not flipped.  Per CLAUDE.md, every merged feature flips at least one box. | ~10 min | `doc/implementation-plan/testing-checklist.md` |
| A15 | P2 | Test comments missing spec citations (cel-cpp source file:line) per CLAUDE.md "Testing principles."  Several called-out tests assert spec-mandated behaviour with no citation. | ~30 min | `cel_optional_test.cc` |
| A16 | P2 | `cel-host-surface.md` and `design.md` not updated for the 8 new exports / new CEL kind reaching production codegen.  Verify each describes the right surface. | ~20 min | sibling design docs |
| A17 | P2 | OptionalCell inner CelValue retains original offsets on aggregate inners (CEL_STRING, CEL_LIST_ARENA, etc.) — not deep-copied.  Document the span-aliasing contract in cel_optional.h. | ~15 min | `cel_optional.h` |
| A18 | P2 | No regression test that `payload.opt` lower-byte clobber (the union-aliasing bug fix in §4.1) doesn't recur.  Existing tests catch via downstream assertions but don't directly probe `payload.opt`. | ~20 min | `cel_optional_test.cc` |
| A19 | P2 | `cel_select_optional_field_at_vv` CEL_MESSAGE source `__builtin_trap()` arm has no `EXPECT_DEATH` test; CLAUDE.md "Unimplemented features" says crashing IS the negative coverage but a documented trap test is good hygiene. | ~15 min | `cel_optional_test.cc` |
| A20 | P2 | The 56 pre-existing WAT files were renamed `cel_reset`/`cel_alloc` → `arena_reset`/`arena_alloc` — but the WAT-runner now does end-to-end validation against the real runtime, which means the WATs are no longer just documentation; they're test artifacts.  Document this elevation in `wat-traces.md`. | ~15 min | `wat-traces.md` chapeau |

P0 = blocks Slice A closeout (none).
P1 = must-fix-before-Slice-B (A1–A8).
P2 = cleanup-when-touched (A9–A20).

---

## 8. Sibling-component reconciliation

CLAUDE.md says the reviewer picks "one neighbouring component"
to catch drift by adjacency.  I picked **`cel_runtime.c::equality_kernel`**
(directly modified, so not strictly an adjacency but the closest
candidate that gained complexity).

Findings:

  - `equality_kernel` was 75-line single function pre-Slice-A;
    post-refactor it's 28 lines + `equal_same_kind` (36 lines)
    + `optional_eq_at_vv` (24 lines).  Each under threshold.
    The split is clean — `equal_same_kind` returns `int`
    (published/not-published) rather than a Status, matching
    the surrounding runtime's "void out_slot" idiom.  Good.
  - `optional_eq_at_vv` forward-declares `equality_kernel` to
    enable recursion through optionals.  Forward declaration
    is at line 1149-1150, definition at line 1158.  Clean.
  - The recursion into `equality_kernel` for inner CelValues
    is implemented via slot indirection (`uint32_t a_inner =
    a->payload.opt + offsetof(OptionalCell, inner);`).  This
    is the right shape — slot offsets in linear memory, not
    pointer dereferences.  Clean.
  - There's no recursion-depth guard.  If someone constructs
    a deeply-nested `optional<optional<optional<...<int>>>>`
    via a comprehension or repeated `.of(.of(...))` chain,
    `optional_eq_at_vv` recurses through the stack.  Worst
    case stack depth = nesting depth.  CEL doesn't have
    arbitrary recursion at the expression level (depths are
    bounded by AST size), so practically this is fine — but
    a single-line comment naming the bound would clarify
    intent.  P2.

**`cel_arena.c`** — not touched by Slice A.  `arena_alloc(32)`
calls for OptionalCells are well-aligned (the `_Static_assert`
in cel_optional.h:69-72 forces it).  No drift.

**`cel_data.h`** — not touched.  `CEL_OPTIONAL = 14` and the
`uint32_t opt` payload field were pre-existing scaffolding;
Slice A used them as-designed.

**`cel_log.cc`** — touched only in non-substantive ways (if
at all; not in the diff).  The CEL_OPTIONAL stub printer
remains (A9).

---

## 9. Bug-fix scrutiny — durable or fragile?

Per item 4 of the task brief, focused assessment:

### 9.1 `write_optional` union-aliasing fix — **durable**

The structural fix removes the dangerous union-arm writes
entirely.  The comment names the failure mode and the
codegen invariant ("workspace slots are zero-init'd by the
prologue") that makes the trailing 12 bytes safe.  A future
refactor that re-adds defensive zeroes will hit the comment
and (hopefully) think twice.  One coverage gap (A18) means
a regression of the FIX itself wouldn't be caught directly,
but downstream assertions would fail.  Net: durable.

### 9.2 LTO-inlined `require_optional` fix — **durable**

The structural fix splits validation from pointer-load.  The
helper that returns `OptionalCell*` is gone; in its place is
`absorb_or_typecheck_optional` (returns `int`) plus inline
`cell_at` calls per accessor.  The LTO regression mode
(constant-folding a pointer-typed helper's early-return on
`cel_memory_base_() == 0`) can't recur because there's no
longer a pointer-typed helper to fold.  The comment at lines
159-167 names the LTO inlining issue and the wasm-specific
`cel_memory_base_()` semantics.  Net: durable.

One subtle point: the `cel_at` macro/inline function still
exists (`cell_at` in cel_optional.c:21-23 — defined locally
in cel_optional.c).  If a future contributor inlines THIS
helper and re-adds a `memory_base == 0` early-return, the
regression could come back.  The comment in
`absorb_or_typecheck_optional` provides the breadcrumb, but
it's at the validation function, not at `cell_at`.  A
two-line "// Do not early-return on cel_memory_base_() == 0
— see comment on absorb_or_typecheck_optional and the LTO
regression that motivated the split" comment on `cell_at`
would be belt-and-braces.  P2.

### 9.3 Both fixes pass CLAUDE.md hygiene

  - No `// NOLINT` suppressing a tidy warning.
  - No silent fallback returning a default value on error.
  - Comments name the failure mode in actionable terms.
  - The fixes are committed in the same change as the
    workaround — not "TODO: fix later."

Net: both fixes are durable.  The remaining risk is
discipline-shaped (a future refactor that doesn't read the
comments), not structural.

---

## 10. Recommendations

In priority order:

  1. **Close out `m14-optionals.md` properly.**  Flip the
     header status line, tick the Slice A bullets, add the
     three plan-vs-execution delta callouts (14 overload
     seeds, harness rewrite, .bazelrc).  Per CLAUDE.md
     "Closing out a planning doc."  Same for `wat-traces.md`
     M14 chapeau.  Same for `testing-checklist.md` rows.
     **This is the cheapest single thing the user can do to
     prevent the next reviewer from re-flagging the same
     thing.**
  2. **Decide on the `ABSL_CHECK` → `UnimplementedError`
     conversion.**  Either:
      - Add a frontend gate (parse_and_check or a new pass)
        that rejects the not-yet-supported AST shapes, then
        restore the CHECKs in codegen, OR
      - File a cleanup-backlog entry naming Slice B as the
        owner of the restoration, with a citation back to
        this review's §3 + CLAUDE.md "Unimplemented features."
     Decide before Slice B opens.
  3. **Fill the load-bearing test gaps (A4–A7).**  Four tests
     (or two parameterized cases) that lock the short-circuit
     `or` semantic, the list-source select-field branch, the
     OOB-index-to-None reinterpretation, and the
     `optional == non-optional` false-not-error rule.  ~90
     min total.
  4. **Spot-check conformance (A8).**  Run the harness,
     read 5-10 PASS rows from optionals.textproto, confirm
     each is passing on the right reason.
  5. **Document the harness rewrite.**  The Slice A wat_runner
     changes (shared memory, WASI, arena_init,
     `PreprocessWatMemoryImport`) are the kind of thing that
     a future contributor authoring a WAT will need to know.
     A short section in `wat-traces.md` chapeau or in
     `tools/wat_runner/README.md` (create if
     missing) would help.  ~20 min.
  6. **Fix `cel_log.cc` CEL_OPTIONAL arm (A9).**  Was already
     P2 in the Slice 0 review; Slice A had the perfect
     opportunity to fix it once real cells exist.  ~30 min.
  7. **Test-comment spec citations (A15).**  Sweep
     `cel_optional_test.cc` once, add `// langdef §... / cel-cpp
     <file>:<line>` lines to the spec-mandated tests.  ~30 min.
  8. **Lint backlog + per-component coverage rows (A13, A14).**
     Trivial but mandated by CLAUDE.md.  ~20 min.

If items 1–4 land before Slice B starts, this slice's debt
ledger is balanced.  Without them, Slice B's reviewer will
re-flag every one of A1–A8, and the closeout-discipline
violation patterns from M2 (29 silent GTEST_SKIPs) start
re-appearing.

---

## 10b. Closeout addendum — spot-check + doc-discipline fixes (2026-05-21, after iteration)

The author addressed P1 items A1, A2, A3, A4–A7, A8 in a single
follow-up pass.  Recorded here for the next reviewer's benefit.

### A8 spot-check — conformance overshoot explained, not accidental

Ran `bazel run //conformance:run_conformance --
--file=tests/simple/testdata/optionals.textproto` and walked the
PASS-by-elimination set.  Sampled 8 PASS rows (out of 14):

  - `null` — `optional.of(null).hasValue() == true` (CEL_NULL inner +
    has_value Some-branch).
  - `null_non_zero_value` — `optional.ofNonZeroValue(null).hasValue()
    == false` (is_zero_value treats CEL_NULL as zero, per §3.4 matrix).
  - `none_or_none_or_value` — `optional.none().or(optional.none())
    .orValue(42) == 42` (chains the None branch of both `or` and
    `or_value`).
  - `none_optMap_hasValue` — `optional.none().optMap(y, y + 1)
    .hasValue() == false` (parser-side optMap macro + cel.bind
    comprehension; only passes because M5 comprehension codegen
    handles the expansion).
  - `optional_eq_none_none` — covered by `EqualsBothNoneIsTrue` unit.
  - `optional_eq_int_int` — covered by `EqualsBothSomeWithEqualInnerIsTrue`.
  - `optional_eq_int_none` — covered by `EqualsPresentMismatchIsFalse`.
  - `optional_ne_none_int` — `!=` over the CEL_OPTIONAL equality arm.

Each sampled PASS row maps cleanly to a kernel branch that is
also unit-tested at the C level; no "passing by coincidence"
rows surfaced.  The +92 conformance overshoot vs. design's +25
projection is fully explained by:

  1. `OptionalCheckerLibrary` admits broad `optional<T>`-typed
     expressions that previously type-checked as `dyn` and got
     RejectDyn'd (the largest contributor — ~50+ rows).
  2. Cascading wins: rows building on multiple kernels light up
     simultaneously rather than incrementally.
  3. `cel_select_optional_field_at_vv` shipped in Slice A (per
     Plan-vs-execution delta 1), unlocking a slice of rows the
     design originally credited to Slice B.

The 5 remaining FAIL rows (`optional_chaining_1..4,9`) are all
the `has(opt.x)` / `opt.field.field` chained-Select pattern,
which is the Slice B target — these will resolve when `LowerSelect`
gains the Repr-detection branch.

### Doc-discipline items addressed

  - A1: `m14-optionals.md` status header flipped; Slice A bullets
    ticked with `[x]`; three Plan-vs-execution delta callouts added
    (14-seeds-not-7, harness rewrite, .bazelrc).
  - A2: `wat-traces.md` M14 chapeau rewritten to present-tense
    description of the 6 byte-decoding tests + Slice A harness
    changes.
  - A3: cleanup-backlog.md entry #8 filed.  The
    UnimplementedError ships as-is; Slice B (or a successor
    static-subset-hardening micro-slice) owns the frontend gate +
    CHECK restoration.
  - A4–A7: four targeted unit tests added to
    `cel_optional_test.cc` (36 tests total now, all pass): Or
    LHS-Some short-circuits a poison RHS; SelectField on
    CEL_LIST_ARENA in-bounds returns Some; SelectField on
    CEL_LIST_ARENA out-of-bounds returns None; cross-kind
    `optional == non-optional` returns false (not error).

P2 items (A9–A20) remain open and are appropriate for the
Slice B / D closeout window.

---

## 11. One-line summary

Slice A's code is solid — kernels are tight, bug fixes are
durable, WAT-runner tests genuinely lock the ABI for the
first time, and the bug-fix scrutiny of the union-aliasing
and LTO-inline regressions confirms both fixes are structural
rather than band-aid.  Slice A's docs are not — m14-optionals.md
and wat-traces.md still read as pre-Slice-A plans, three
substantive plan-vs-execution deltas (14-vs-7 overload seeds,
the wat_runner rewrite, the `.bazelrc`) shipped silently, and
the `ABSL_CHECK` → `absl::UnimplementedError` conversion
quietly weakens CLAUDE.md's "stubs crash loudly" invariant
without a path to restore it.  Pre-Slice-B work: close the
doc state, decide the CHECK-restoration path, fill the four
load-bearing test gaps, and spot-check the conformance
overshoot.
