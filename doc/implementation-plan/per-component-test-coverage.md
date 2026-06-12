# Per-component test coverage

**The keystone testing doc.**  Every component in the pipeline must
have a test that exercises every code path it ships, and every
feature must close out by running the full suite — including the
`manual`-tagged e2e targets.  This doc defines the gate.

It complements:

  - `rewrite/feature-pipeline-checklist.md` — which **files** to
    touch per feature type.
  - `testing-checklist.md` — running grid of what coverage has
    landed per milestone.

Use those two for "what to write."  Use **this** doc for "what
must be green before declaring a slice or milestone done."

---

## 0. The "M2 incident" — why this doc exists

On 2026-04-24, a routine validation of M2 found:

  - `bazel test //...` was 100% green (27 / 27 tests).
  - `m2-ident-select-unknowns.md` and `testing-checklist.md` both
    described M2 as substantially shipped; subsequent work (M3
    map literals, conformance harness) had been built on top of
    that assumption.
  - But `e2e:m2_test` is `tags = ["manual"]` and
    therefore excluded from `bazel test //...`.  Running it
    explicitly revealed **29 of 44 tests `GTEST_SKIP`ped at the
    fixture level**: every `SelectE2ETest` (12), every
    `HasE2ETest` (6), every `UnknownE2ETest` (7), plus 4 others —
    each with an explicit "pending M2.C" or "pending M2.E" reason.
  - The Layer-2 trampoline bodies (`CelGetFieldImpl` /
    `CelHasFieldImpl`) referenced from `cel_host_wasmtime.cc` were
    declared but never defined — the conformance binary couldn't
    even link.

**Lesson:** a default-test-suite-green is not the same as a
feature working.  Manual-tagged tests carry the load-bearing e2e
assertions; they must run before declaring a milestone done.

---

## 1. The rule

A feature is **not done** until **all** of the following are true:

  1. Every component file the slice touched has a corresponding
     `*_test.cc` with passing assertions exercising every new
     code path (positive + negative + boundary, per CLAUDE.md
     "Cover the edge-case matrix").
  2. The end-to-end test for the feature exists in
     `e2e/m<N>_test.cc` and is **not** `GTEST_SKIP`ped
     at the fixture or test level.
  3. **All** `manual`-tagged test targets affected by the slice
     run green explicitly — see §2 below for the catalog.
  4. The corresponding rows in `testing-checklist.md` are ticked
     and reference the test that proves them.
  5. `bazel run //conformance:run_conformance` is
     re-run; `conformance/README.md` headline + the
     per-fixture rows that moved are refreshed in the same
     commit.
  6. The milestone doc's status header reflects shipping (not
     "planned" or "in progress").  Plan-vs-execution deltas
     called out at the top per CLAUDE.md "Closing out a planning
     doc."

A slice that ships with **any** of these unmet either:

  - Documents the gap in the milestone doc's "Future work"
    section with a tracked follow-up, **and** the rows that
    depend on it are explicitly marked `[ ] (deferred to <slice>)`
    rather than ticked, **or**
  - Is sent back as not-done — even if `bazel test //...` is
    green.

---

## 2. Manual-tagged targets — must all run before milestone close

These targets are excluded from `bazel test //...`
because they require external toolchains (wasmtime, brew clang
for wasm32 cross-compile) or pull large fixtures.  They are
**not optional** — a milestone is not done until each one that
exercises the slice's surface runs green.

| Target | Module under test | When required |
|---|---|---|
| `//eval:instance_test` | `Instance::Eval`, `Instance::PartialEval`, decoder, activation marshal, full Compile→Plan→Eval | Any change to compile / engine / instance / value / activation |
| `//eval:engine_test` | `Engine::Plan`, runtime instantiation, linker wiring | Any change to engine, host imports, runtime exports |
| `//eval:cel_host_test` | Layer-1 `ProtoBacking`, Layer-2 trampolines (`CelGetFieldImpl`, `CelHasFieldImpl`, `CelMapLookupImpl`) | Any cel_host change |
| `//benchmark/compiler:stage_bench` | Pipeline-stage perf bench (per-stage + cold/warm-start) | Major pipeline change (regression catch) |
| `//e2e:m<N>_test` | The milestone's e2e suite | Every feature in milestone N |
| `//e2e:eval_test` | Cross-cutting eval shapes (legacy compiler) | Compatibility check | <!-- FLAG: no `eval_test` target exists under e2e/ (only m<N>_test + optimize_test + program_roundtrip_test + known_bugs_test); this row may be stale — verify before relying on it as a gate. -->
| `//runtime:cel_runtime_wasm_test` | wasm32-cross-compiled runtime, exercised under wasmtime | Any runtime primitive change |
| `//tools/wat_runner:wat_runner_test` | WAT trace re-assemble + re-run regression | Any codegen arm change (WAT-first rule) |
| `//conformance:run_conformance` | Upstream CEL conformance corpus | Every milestone close |
| `//tools/cel:cel_smoke_test` (smoke) | `cel` CLI binary, end-to-end eval/check/compile from source string | Any public-API change |
| `//examples:examples_smoke_test` (always-on, not manual) | Every `examples/` binary runs and produces its documented output — the rot gate for all doc/README sample code | Any public-API change; any doc-snippet change |

**`scripts/run_full_suite.sh`** (or equivalent ergonomic wrapper)
should bundle these into one invocation; the milestone doc cites
it as the gate.

---

## 3. Per-component required scenarios

For every component in the pipeline, this is the **minimum** set of
test scenarios.  Per CLAUDE.md "Test pipeline stages component-by-
component" — every layer your slice touched must have an
assertion at the layer's boundary, not just a downstream e2e that
happens to pass.

### 3.1 Frontend (`compiler/frontend/parse_and_check_test.cc`)

For each new feature touching parse / check:

  - **Positive** — a representative source string parses + checks
    + emits the expected `TypedAst` shape.
  - **Variable spec round-trip** — if the feature adds a type
    syntax, every variant of the spec parses (primitive, list,
    map, message FQN).
  - **Reject-dyn** — anything outside the static subset must hit
    `RejectDyn` and surface `InvalidArgument` with `"static
    subset"` in the message.
  - **Annotation seeding** — if the feature populates a new
    `NodeAnnotation` field (`field_number`, `attribute_id`,
    `overload_id`, `map_origin`), at least one test asserts the
    annotation lands on the right node ID.

### 3.2 IR (`compiler/ir/{annotations,typed_ast}_test.cc`)

  - **Repr coverage** — every CEL type the feature exposes maps
    to its `Repr`; assert via the parameterised Repr suite.
  - **Origin / attribute** — when ResolvePass writes new fields,
    `typed_ast_test` asserts `PopulateAnnotations` produces the
    expected shape per node kind.

### 3.3 Codegen — ResolvePass (`codegen/resolve_pass_test.cc`)

  - **Variable interning** — every declared variable gets a
    distinct `local_index` in declaration order; unreferenced
    declared variables are not interned.
  - **Attribute interning** — every rooted ident path produces a
    unique `attribute_id`; sentinel at index 0; same path used
    twice maps to the same id.
  - **Origin inference** — for every `kCreateMap` / `kSelect` /
    `kIdent` / `?:` shape your feature touches, assert
    `map_origin` / `list_origin` lands at the right value
    (`kArena` / `kHost` / `kDynamic`).

### 3.4 Codegen — LayoutPass (`codegen/layout_pass_test.cc`)

  - **Slot allocation** — every node kind that needs a workspace
    slot gets one; `peak_slots` matches the expected count.
  - **Rodata packing** — every `kConst` literal lands in rodata
    at an 8-aligned offset.
  - **Boundary invariants** — `arena_base = workspace_base +
    workspace_bytes`, both 8-aligned.
  - **Variables table** — every referenced variable lands with
    the correct `Repr`, distinct `slot_offset`, contiguous
    `local_index`.
  - **Conditional rodata visitors** — when a visitor allocates
    rodata only under a predicate (e.g. `SelectKeyRodataVisitor`
    allocates only when the operand annotation is
    `Repr::kOptional`), at least one positive test (predicate
    holds → offset stamped, frame header bytes valid) AND one
    negative test (predicate doesn't hold → offset stays zero)
    must exist.

### 3.5 Codegen — ExprLower (`codegen/expr_lower_test.cc`)

  - **WAT match** — every new arm: assert the emitted Binaryen
    body contains the expected calls / locals / constants the
    matching `doc/.../wat/NN_<feature>.wat` predicts.
  - **Validate** — `WasmModule::Validate()` returns OK on the
    emitted module.
  - **Negative** — every kind that a milestone has *not* yet
    enabled must surface `Unimplemented` with a message that
    names the kind (so accidentally lighting one up is
    immediately visible).
  - **Per-Repr dispatch coverage** — when a new arm branches on
    operand `Repr` (e.g. `EmitKSelect`'s optional branch,
    `EmitKIndexCall`'s optional branch), every reachable Repr
    must be exercised by at least one codegen test that
    asserts the emitted call target.  Skipping `optional<list>`
    coverage on the grounds that `optional<map>` is "the same
    code path" is exactly the M2-incident-shaped mistake the
    rest of this doc exists to prevent.

### 3.6 Module / Compile (`codegen/module_test.cc`, `compile_test.cc`)

  - **Import surface** — every wasm import the codegen emits is
    declared on the module under the right `(module, name)` pair
    (`cel.cel_alloc`, `cel_host.cel_get_field`, …).  Use
    `BinaryenFunctionImportGetModule` / `…Base`.
  - **Export surface** — `eval` (or the configured name) is
    exported.  Memory is imported from `cel.memory`, **not**
    exported.
  - **ABI custom section** — `cel.abi` section is present;
    decoded with `abi_decode` it round-trips to the same proto
    the emit pass produced.
  - **Compile() facade** — every public surface option flows
    through (`mem_size_bytes` → `cel_reset` second arg,
    `eval_export_name` → export, schema → variable resolution).

### 3.7 ABI (`abi/cel_abi_emit_test.cc`, `api/internal/abi_decode_test.cc`)

  - **Round-trip** — emit → bytes → decode produces the same
    proto the emit pass generated.
  - **Sentinel discipline** — index-0 sentinel for every dense
    table (`fields[0]`, `attributes[0]`).
  - **Missing-section fallback** — `abi_decode` returns
    `NotFound` on M1-era modules without the section; callers
    fall back to an empty `CelAbi`.

### 3.8 Runtime (`runtime/{cel_data,cel_make,cel_arena,cel_memory,cel_map,cel_list,cel_arith,cel_compare,cel_string_ops,cel_aggregate_arena}_test.cc`, `cel_runtime_wasm_test.cc`)

  - **Wire stability** — every `CelKind` enum value pinned by
    `_Static_assert`s in `cel_data.h` and asserted in
    `cel_data_test`.
  - **Per-kind round-trip** — every payload kind: write via
    `cel_make_*`, read back; assert byte layout matches the
    static asserts.
  - **Boundary values** — `INT64_MIN` / `INT64_MAX` /
    `UINT64_MAX` / `0` / `-1`, embedded NUL strings, multi-byte
    UTF-8.  No type with a defined boundary can ship without
    those rows.
  - **Wasmtime exercise** — `cel_runtime_wasm_test` instantiates
    the cross-compiled module and round-trips through every new
    export.
  - **Arena public API** (`cel_arena.{h,c}` — DESIGN §4-§5
    wasi/malloc migration).  `cel_arena_test.cc` covers:
      - `arena_init` — capacity reflected by `arena_capacity()`;
        idempotent with same arg; **traps on different cap_bytes**
        (A16, death test).  `InitWithDifferentCapacityTraps`,
        `InitWithLargerDifferentCapacityTraps`,
        `InitWithSameCapacityIsIdempotent`,
        `CapacityMatchesDesignDefault`.
      - `arena_alloc` — 8-byte alignment for every n ∈
        {1,7,8,9,15,16,23,24} (A9); zero-fill on success; bumps
        cursor monotonically; alloc(0) returns valid 8-byte slot
        (A9); alloc-after-OOM leaves cursor unchanged (A10);
        return value resolves via `cel_mem_base() + ret`;
        alloc-before-init traps (A16 corollary in `arena_alloc`).
        `AllocOffsetIsAlwaysEightAligned`,
        `AllocZeroReturnsValidEightByteSlot`,
        `AllocBumpsCursorMonotonically`, `AllocReturnsZeroedBytes`,
        `AllocAfterResetReturnsZeroedBytesEvenIfPreviouslyDirty`,
        `AllocReturnContractResolvesViaCelMemBase`,
        `AllocReturnsZeroWhenOutOfSpace`,
        `AllocExactlyRemainingCapacitySucceeds`,
        `AllocOneBytePastCapacityReturnsZero`,
        `FailedAllocLeavesCursorUnchanged`,
        `OverflowFollowingSuccessLeavesEarlierAllocsIntact`.
      - `arena_reset` — O(1) rewind; same offset on repeat alloc
        across many cycles; backing pointer unchanged.
        `ResetRewindsCursor`, `ArenaResetRoundTripGivesSameOffset`,
        `ResetAllocCycleIsIdempotentAcrossManyIterations`,
        `ResetDoesNotChangeBackingPointer`,
        `ResetBeforeInitIsHarmless`,
        `PerEvalLifecycleResetGivesSameStartingOffset`.
      - `arena_cursor` / `arena_capacity` — cursor reflects each
        alloc's aligned size; capacity is stable across allocs
        and resets.  `CursorReflectsAllocations`,
        `CursorAdvancesByAlignedSize`,
        `CapacityIsStableAcrossAllocsAndResets`,
        `CursorIsZeroAfterFreshReset`.
      - `cel_value_at` — offset 0 → nullptr (absent sentinel);
        non-zero → CelValue* resolving via `cel_mem_base() +
        off`.  `ValueAtZeroReturnsNull`,
        `ValueAtNonZeroResolvesViaCelMemBase`,
        `ValueAtForSizeofCelValueIsWriteable`.
      - `cel_reset` **compat shim** (M5 will delete) — ignores
        both args; rewinds cursor; auto-inits on cold path
        (cold-path covered by SetUp's first call across the
        process).  `CelResetIgnoresArgs`, `CelResetRewindsCursor`.
  - **Kernel-side arena OOM paths** — every kernel that calls
    `arena_alloc` has a graceful-failure test that drains the
    arena and asserts the kernel poisons rather than UB.
      - `cel_make_test::MakeOomTest*` — every constructor returns
        0 when arena full; payload-fits-but-header-doesn't and
        header-fits-but-payload-doesn't boundary rows for
        `cel_make_string` / `cel_make_bytes`.
      - `cel_string_ops_test::*Oom*` — `cel_string_concat_at_vv`
        / `cel_bytes_concat_at_vv` poison with
        `CEL_ERR_OVERFLOW` on payload OOM; exact-capacity-fit
        succeeds at boundary; empty-operand concat never needs
        arena.
      - `cel_3vl_test::UnknownMerge*` — non-empty/non-empty
        merge poisons on descriptor alloc OOM; empty-side merge
        and both-empty merge succeed even when arena is full.
      - `cel_aggregate_arena_test::AggregateOomTest` —
        `cel_list_create` / `cel_map_create` poison with
        `CEL_ERR_OVERFLOW` for both header-OOM and
        elements/entries-OOM; zero-capacity create needs only
        the header (succeeds when entries arena would not fit).
  - **Compile-time invariants** (`cel_layout.h`).  Five
    `_Static_assert`s pin `CELWASM_RESERVED_LOW_MEMORY_BYTES <
    initial memory size`, the 8-byte alignment of the reserved
    region, and `CELWASM_ARENA_CAPACITY_BYTES` being a
    power-of-2.  No runtime tests — if any of these constants
    ever becomes a tunable, add boundary tests for the dynamic
    range.

### 3.9 Host imports (`api/internal/cel_host_test.cc`)

  - **Layer 1** — `ReadField` × every proto cpp_type;
    `HasField` × proto2 explicit and proto3 implicit presence;
    repeated / map field handling.
  - **Layer 2** — every trampoline absorbs `UNKNOWN` / `ERROR`
    on every input slot without dereferencing the backing;
    invalid slot returns infrastructure failure (`absl::Status`
    non-OK), not a poisoned `out_slot`; cross-backing dispatch
    (a JSON-like fake confirms the contract is concrete-agnostic).
  - **Layer 3** — wasmtime registration succeeds; trampoline
    fires from a wasm program that imports it; arity / signature
    matches the import declaration.

### 3.10 API (`api/{value,activation,attribute,compiler,engine,instance}_test.cc`)

  - **Value** — every `Kind` round-trips through its builder +
    accessor; `StructurallyEquals` honors langdef equality;
    aggregates carry their backing pointer correctly.
  - **Activation** — `Bind` + `Find` for every Repr; rebinding;
    interleaved bindings across multiple instances.
  - **AttributePattern::Parse** — every qualifier shape
    (single / dotted / wildcard / array index / map key);
    rejects every malformed shape (leading dot, trailing dot,
    consecutive dots, empty, etc.).
  - **Compiler** — per-Repr `DeclareVariable`; schema reload;
    container forwarding; build idempotence.
  - **Engine** — Plan against an empty / minimal / map-bearing /
    field-bearing module; runtime + expr instances both
    instantiate.
  - **Instance** — every literal kind decodes; every `Repr`
    bound activation marshals; PartialEval routes through
    pattern matching; `DecodeCelValueAt` arm for every CelKind.

### 3.11 E2E (`e2e/m<N>_test.cc`)

  - **One fixture per feature theme** (idents, selects, has,
    unknowns, maps, …).  `SetUp` does **not** unconditionally
    `GTEST_SKIP` an entire fixture — that hides the gap.  If a
    feature is deferred, mark the *one* TEST that needs it with
    `GTEST_SKIP` and a referenced follow-up issue.
  - **Every shape end-to-end** — `Compile` → `Plan` → `Eval` /
    `PartialEval`; assert the decoded `cel::Value` matches.
  - **`mvp_concat_test`** — DESIGN §2 MVP: `"foo" + "bar"` →
    `"foobar"` via the malloc-backed arena.  `FooBar` (single
    eval, codegen + runtime concat round-trip).
    `FooBarRepeatedAcrossManyEvals` (1024 evals; locks
    `arena_reset` across-Eval semantics from DESIGN §7 — without
    correct reset the arena overflows by ~iteration 2000).

### 3.12 Conformance harness (`conformance/runner_test.cc`, `run_conformance`)

  - Envelope filter (`IsInM<N>Envelope`) updated for the new
    matcher kind / binding shape.
  - Re-run the binary; the per-fixture `total / pass / skip /
    fail` table in `README.md` refreshes.  Pass-count movement
    is documented in the milestone doc's "What this milestone
    unlocked in the conformance suite" subsection.

### 3.13 WAT prototyping (`tools/wat_runner_test.cc`)

Per CLAUDE.md "WAT-first for ABI and codegen design": every new
codegen arm or host-ABI surface gets a `.wat` trace, assembled +
re-run on every build via `wat_runner_test.cc`.  A codegen arm
that ships without a WAT companion is incomplete.

---

## 4. SKIP discipline

`GTEST_SKIP` is a code path, not a comment.  Rules:

  - **Never** `GTEST_SKIP` an entire fixture's `SetUp` for "this
    whole feature isn't done yet."  If the feature isn't done,
    the slice isn't done — don't pretend tests exist.  Either
    remove the fixture (and add it back when the work is real) or
    write the tests with concrete expectations and let them fail
    until the work is done.  A wall of skips is the failure mode
    the M2 incident caught.
  - `GTEST_SKIP` is OK for **per-test** skips that document a
    real environment gap (e.g. "skipped on macOS — wasi-sdk
    fixture only ships on linux"), with a comment naming the
    condition and an issue tracking the unconditional path.
  - `GTEST_SKIP` is OK for **single-test** deferrals during a
    multi-slice rollout, with a `// TODO(M<N>.<slice>):` comment
    naming the slice that lifts the skip.  When that slice
    lands, the comment + skip are removed in the same commit.

If you find yourself wanting to skip more than two tests in a
fixture, the feature is not ready to merge.  Stop and finish
the work.

---

## 5. Closing-out gate (copy into the milestone PR description)

```
M<N>.<slice> closeout

[ ] All component _test.cc files written (per §3 above)
[ ] No fixture-level GTEST_SKIPs added to e2e/m<N>_test.cc
[ ] bazel test //... passes
[ ] bazel test //eval:instance_test passes
[ ] bazel test //eval:engine_test passes
[ ] bazel test //eval:cel_host_test passes
[ ] bazel test //e2e:m<N>_test passes (no fixture skips)
[ ] bazel test //runtime:cel_runtime_wasm_test passes
[ ] bazel test //tools/wat_runner:wat_runner_test passes
[ ] bazel run //conformance:run_conformance — README inventory refreshed
[ ] testing-checklist.md rows ticked with test refs
[ ] m<N>-*.md status header reflects shipping
[ ] feature-pipeline-checklist.md rows ticked
[ ] WAT trace exists for every new codegen arm
```

This block is the contract.  A milestone doesn't close until
every box is ticked or explicitly deferred with a tracked
follow-up.
