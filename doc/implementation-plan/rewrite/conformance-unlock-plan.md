# Conformance unlock plan (post-M5.D step 2)

Status: **shipped 2026-04-25 — all numbered slices (0 / 1 / 1.5 /
1.55 / 1.6 / 2) landed; only Slice 3 (classifier tightening)
remains open.**

> **Post-MVP note (2026-05-18).**  The wasi-sdk migration on
> `wasi-malloc-migration` does not regress conformance (pass=1373
> on both master and the migration branch as of 2026-05-18).  This
> doc's per-slice numbers reflect the pre-migration baseline; the
> migration is a no-op for the conformance harness.

Final result: `412 / 1622 / 420` baseline → **`664 / 1362 / 428`**
(+252 PASS, −260 SKIP, +8 FAIL net).  Per-slice as-shipped deltas
recorded inline below.

Working sequence to maximise conformance PASS count from the
M5.D-step-2 baseline.  Ordered by unlock-per-LoC; each slice is
self-contained and ships independently.

See `compiler_v2/conformance/README.md` for the per-fixture
inventory + the projection method these numbers come from.

## Why this order

The README's per-milestone forecast bundles unlocks in ways that
silently compound — e.g. "M5.D step 2 unlocks +50–100" assumes
polymorphic `==` is also there.  Today's M5.D step 2 in isolation
delivered only +21, because most of its projected unlocks need
`cel_equals_at_vv` (M5.B step 2b) AND/OR string activation marshalling.
The slices below decouple those compounded dependencies and run them
in the order that minimises blocking.

## Slice 0 — kString / kBytes activation encoder

**Owner**: parallel coding agent.
**Status**: shipped 2026-04-25.
**Projection (as written)**: +50–100 PASS standalone.
**Projection (as shipped)**: +78 PASS (412 → 490 total).

**Scope.**  `compiler_v2/api/instance.cc::EncodeBoundValue`
(formerly `EncodeScalarValue`) routed `Repr::kString` and
`Repr::kBytes` to a new `EncodeStringOrBytes` arm.  The original
plan was to allocate via `cel_alloc` reentry — but during
implementation that path was found to be **blocked** by the
`cel_reset` rewind: `$eval`'s prelude rewinds the bump pointer to
`arena_base`, and the first in-eval `cel_alloc` zero-fills the
range, stomping any pre-marshalled bytes.  As-shipped solution:

  1. Engine memorytype creation (`engine.cc::InitStoreAndMemory`)
     drops `max_present=true,max=2` to `max_present=false`, so the
     host can grow linear memory above the codegen's `arena_limit`.
  2. `instance.cc::EnsureHostStringArenaCapacity` lazily captures
     the post-instantiation memory size as `host_string_arena_floor`
     (= the codegen's `arena_limit` — the value the wasm-side
     `cel_alloc` bounds-checks against) and grows memory by whole
     pages on demand to fit every kString / kBytes activation
     payload.  The runtime never touches the tail because every
     `cel_alloc` call traps once `bump >= arena_limit`.
  3. `instance.cc::EncodeStringOrBytes` writes the payload bytes
     directly into linear memory at `floor + cursor`, advances the
     cursor by `aligned_len`, and stamps `{kind:CEL_STRING|CEL_BYTES,
     payload.s={offset, len}}` into the variable's workspace slot.
  4. `WasmtimeArenaAllocator` was promoted from
     `cel_host_wasmtime.cc`'s anonymous namespace to the named
     `celwasm` namespace via the header — kept as the canonical
     wasm-arena reentry helper for trampolines, but no longer
     reused for the activation encoder (the direct linear-memory
     write path replaces it).

**Tests required.**  E2E coverage in `compiler_v2/e2e/m5_test.cc`
under a new `StringBytesActivationE2ETest` fixture (avoid colliding
with Slice 1's m5_test additions):

  - `BindStringPlusLiteral` — `Activation::Bind("s", Value::String("hi"))`,
    eval `s + " world"` → `"hi world"`.
  - `BindBytesAccess` — bind bytes value, eval `size(b)` → length.
  - `BindEmptyString` / `BindEmbeddedNul` / `BindMultibyteUtf8`
    edge cases.

**Why first.**  Polymorphic equals (Slice 1) is useless on tests
like `s == "foo"` when `s:string` can't be bound.  Same for
ternary/control-flow tests.  Every later slice's projected unlock
is silently gated on this.

**Out of scope.**  `Repr::kList` / `kMap` / `kMessage` activation
marshalling — those land in M7 / a separate slice (the README's
"`unknown:` ExprValue bindings" Future Work bullet).

## Slice 1 — M5.B step 2b polymorphic `equals` / `not_equals`

**Status**: SHIPPED 2026-04-25.
**Actual delta**: **+74 PASS** (412 → 486).  Below the projected
+200–300 because cel-cpp's checker rejects cross-type `==` without
`dyn(...)` — the README's projection assumed those would graduate,
but the static-subset gate (CLAUDE.md "What not to do") keeps them
out.  The runtime kernel's cross-numeric ladder is still reachable
via aggregate-element equality.

**Scope.**

  1. `cel_runtime.c`: add `cel_equals_at_vv` and
     `cel_not_equals_at_vv`.  Each switches on
     `(a->kind, b->kind)` and tail-calls (or inlines) the already-
     shipped same-kind helpers:
       - bool ↔ bool → `cel_bool_eq_at_vv` value.
       - int / uint / double cross-type → `cel_numeric_eq_at_vv`.
       - string ↔ string → `cel_string_eq_at_vv`.
       - bytes ↔ bytes → `cel_bytes_eq_at_vv`.
       - null ↔ null → `true`; null ↔ anything-else → `false`.
       - list ↔ list → `cel_list_eq` (M5.D step 2 dispatcher).
       - map ↔ map → `cel_map_eq`.
       - message ↔ message → `cel_host_cel_message_eq`.
       - mismatched kinds (per langdef "Equality"): `false`, NOT
         error.  E.g. `1 == "1"` → `false`.
       - 3VL: either operand UNKNOWN/ERROR → propagate.
  2. `codegen/overload_table.cc`: seed `equals` / `not_equals`
     overload IDs to `cel_equals_at_vv` / `cel_not_equals_at_vv`.
     Removes the `KCallSameKindEqualsStillUnimplemented` test.
  3. WAT trace `wat/29_equals_polymorphic.wat` locks the dispatch
     shape.
  4. Build wiring: BUILD.bazel exports + wat_runner kRuntimeExports
     + BindAllRuntimeExports.
  5. Tests: matrix in `cel_runtime` unit + e2e in `m5_test.cc`
     `PolymorphicEqualsE2ETest`.

**Tests required.**  Every spec equality case from langdef §"Equality":

  - same-kind: each of the 8 scalar kinds, plus list/map/message.
  - cross-numeric: int↔uint, int↔double, uint↔double.
  - cross-kind rejected: `1 == "1"` → false (not error).
  - 3VL: error operand propagates.
  - boundary: `INT64_MIN`, `UINT64_MAX`, NaN (NaN ≠ NaN per IEEE).
  - empty containers: `[] == []` true; `{} == {}` true.

**Coordination with Slice 0.**  Both add tests to `m5_test.cc`.
Slice 0 lives under `StringBytesActivationE2ETest`; Slice 1 lives
under `PolymorphicEqualsE2ETest`.  No file conflicts otherwise —
Slice 0 is in `api/instance.cc`, Slice 1 in `runtime/cel_runtime.c`
+ `codegen/overload_table.cc`.

## Slice 1.5 — `dyn(scalar)` passthrough

**Owner**: this session.
**Status**: SHIPPED 2026-04-25 — full plan at `dyn-passthrough-plan.md`.
**Actual delta**: **+53 PASS** (509 → 562).  `comparisons.textproto`
83 → 189 (+106), `fp_math` 24 → 29 (+5), `integer_math` 35 → 45 (+10),
`lists` 4 → 23 (+19), `fields` 11 → 19 (+8), `parse` 152 → 157 (+5).
Below the +135–165 projection: cross-kind `<` / `<=` / `>` / `>=` / `in`
rows admit at the gate post-Slice-1.5 but the runtime kernels for
ordering/membership don't yet dispatch cross-numeric (only `==` / `!=`
ship the polymorphic kernel).  Those rows moved SKIP → FAIL (+98 in
`comparisons` alone), unlocking at Slice 1.6.

## Slice 1.6 — cross-numeric ordering / membership ladder

**Owner**: this session.
**Status**: SHIPPED 2026-04-25 — full plan at
`cross-numeric-ordering-plan.md`.
**Actual delta**: **+102 PASS** (562 → 664).  `comparisons.textproto`
189 → 287 (+98) — graduated every cross-kind ordering / membership FAIL
that Slice 1.5 unlocked at the gate.  `lists.textproto` 23 → 27 (+4)
on cross-numeric `in`.  Pivoted from Option A (resolve-pass overload
re-pick from a candidate list) to Option B (codegen-time re-pick from
operand `Repr` pair) because cel-cpp's reference_map lists exactly
ONE candidate per call — no list to choose from.  Membership refactor
(`cel_value_eq_polymorphic` extraction + `map_keys_equal` polymorphic
upgrade) widens `_in_` to consult the `numeric_compare_kernel` for
any numeric pair.

Surgical relaxation: admit `dyn(scalar)` and `dyn(scalar_var)` as
type-check escape hatches, treating them as identity at codegen
time.  Stays rejected: `dyn`-typed variables, `dyn(message)`,
`dyn(list)`, `dyn(map)`, `dyn(...).field`, `dyn`-typed function
signatures.

Why now: the conformance corpus uses `dyn(scalar) == other_kind`
to express polymorphic equality.  Slice 1 (polymorphic equals)
gave us the runtime kernel for cross-kind comparison; this slice
unlocks the type-check path so the corpus's tests reach that
kernel.  +120–150 from `comparisons.textproto` alone.

Out of scope: late-bound field reads (M7), aggregate `dyn`
(future), `dyn`-typed bindings (future).

See `dyn-passthrough-plan.md` for the full plan including the
ResolvePass annotation-forward, codegen identity rule, test
matrix, and risks (cel-cpp checker behaviour around `dyn`'s
type_map entry).

## Slice 2 — M5.G control flow + 3VL

**Owner**: this session (Slice 1.5 deferred — shipped Slice 2 first).
**Status**: SHIPPED 2026-04-25 — full plan at `slice2-control-flow-plan.md`.
**Actual delta**: **+19 PASS** (490 → 509 total).  `logic.textproto`
graduated 0/30 → 16/30.  Below the +80–130 projection because the
remaining `logic` SKIPs and the cross-fixture `eval_error` matchers
are still gated on Slice 1.5 (dyn passthrough) + polymorphic equals
flowing through ternary/and/or operands — both pre-existing slices
in this plan.

**Scope.**  Three lowering shapes:
  - `_&&_` / `_||_`: eager-eval both operand slots, then call
    `cel_and` / `cel_or` slot-out helpers (3VL truth table in
    runtime).
  - `_?_:_`: BinaryenIf with cond-kind probe.  Non-OK cond
    propagates verbatim; OK-bool dispatches to selected arm.
    Branches each emit into their own slot; result is copied
    into out_slot.
  - `!_`: unary slot-out helper `cel_not`.

Plus `cel_unknown_merge` for both-UNKNOWN cases inside `&&`/`||`
(sorted-dedup'd union of attribute-id sets — port v1's shape).

See `slice2-control-flow-plan.md` for runtime helper bodies,
WAT traces 30–33, LayoutPass slot-allocation flip,
codegen emitters, build wiring, and a 4-step probe spike that
must run first to settle the UnknownSet wire shape.

**Tests required.**  3VL truth table coverage; ternary with each
arm UNKNOWN / ERROR; short-circuit on bool with side-effect
operand kind-check.

## Slice 3 — classifier tightening

**Owner**: TBD.
**Projection**: 0 new PASS but reclassifies ~310 ext-lib FAILs to
SKIP, making `kFail == 0` a viable CI gate.

**Scope.**  In `runner.cc`, when compile returns `InvalidArgument`
"type check failed: undeclared reference to 'X'", look up X in the
active overload set; if missing, classify `kUnsupported`.  Tracked
in `compiler_v2/conformance/README.md` Future Work "Classifier
tightening".

## Projected trajectory

(revised after Slice 1 actuals + dyn passthrough scope; full
notes in §"Why this order").

| State | pass | skip | fail | %total | %testable* |
|---|---:|---:|---:|---:|---:|
| Post-M5.D step 2 | 412 | 1622 | 420 | 17% | 22% |
| Post-Slice 1 (polymorphic equals — actual) | 486 | 1542 | 426 | 20% | 26% |
| Post-Slice 0 (string/bytes activation — actual, on top of Slice 1) | 490 | 1538 | 426 | 20% | 26% |
| Post-Slice 2 (control flow + 3VL — actual) | 509 | 1519 | 426 | 21% | 27% |
| Post-Slice 1.5 (`dyn(scalar)` passthrough — actual) | 562 | 1362 | 530 | 23% | 30% |
| Post-Slice 1.6 (cross-numeric ordering / membership — actual) | 664 | 1362 | 428 | 27% | 36% |

*Slice 0 actual delta: +4 PASS over Slice 1 alone (486 → 490).
The "+50–100 standalone" projection overcounted because every
fixture with string-bound activations (namespace.textproto's 11
SKIPs, fields.textproto's 43, etc.) is also gated on at least one
of: `disable_check` envelope rejection, `dyn` passthrough
(Slice 1.5), polymorphic equals (Slice 1, now landed), comprehensions
(post-M5), or message-binding activation marshal (M7).
namespace.textproto's specific 3 → 4 bump confirms the encoder
works; the remaining 10 SKIPs in that fixture are
`disable_check`-gated or comprehension-shaped.  Subsequent slices
that flip those gates will multiply this slice's unlock count.*
| After Slice 1.5 (`dyn` passthrough) | ~660 | ~1370 | ~426 | 27% | 36% |
| After Slice 2 (control flow + 3VL) | ~720 | ~1310 | ~426 | 29% | 39% |
| After Slice 3 (classifier tightening) | ~720 | ~1620 | ~110 | 29% | 39% |

*%testable excludes the 226 `dynamic.textproto` rows that we
permanently reject (CLAUDE.md "What not to do") — denominator is
2228, not 2454.  After Slice 1.5, the remaining `dyn`-using rows
that *aren't* in `dynamic.textproto` become reachable, so the gap
between %total and %testable shrinks too.

~31% pass, FAIL reduced to genuine regressions.  After that, the
next milestone is M7 (proto literals + wrappers + message bindings)
for an estimated +350.

## Constraints

  - WAT-first per CLAUDE.md: every new helper gets a WAT trace
    under `doc/implementation-plan/rewrite/wat/` before codegen.
  - `bazel test //compiler_v2/...` and `scripts/run_full_suite.sh`
    must remain green at every slice boundary.
  - Conformance README + per-fixture table updated in the same
    commit as each slice.
