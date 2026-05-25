# M20 — enum/scalar field-assignment range errors via a poison-on-error `cel_set_field` ABI

Status: plan — drafted 2026-05-25, not yet started.

## 1. Scope (as decided)

Make out-of-range scalar assignments to proto fields produce a **CEL
error value** (matching cel-cpp), not a silent wrong message and not a
wasm trap.  Concretely flips ~8 conformance rows:

  - `enums.textproto` `legacy_proto2/assign_standalone_int_too_big`,
    `…_too_neg`, and the `legacy_proto3` pair (4 rows) —
    `TestAllTypes{standalone_enum: 5000000000}` must be a range error.
  - `proto2.textproto` / `proto3.textproto` int32/uint32 wrapper
    `*_range` rows (4 rows) — currently `GTEST_SKIP`'d in
    `compiler_v2/e2e/wkt_field_set_test.cc`, blocked on the same ABI
    (cleanup-backlog #11).

**Explicitly descoped: strong enum types** (the 18 `strong_proto2` /
`strong_proto3` rows).  See §6 "Future work" — cel-cpp itself does not
implement them, so they are a separate, larger milestone and a
deliberate divergence from our source of truth.

## 2. Probe findings (dated, cited)

> 2026-05-25 — cel-cpp behaviour, read directly from the pinned source.

  - **cel-cpp range-checks enum-field writes and returns a CEL error
    value.**  `common/values/struct_value_builder.cc:1081-1099`
    (`CPPTYPE_ENUM` arm): if the int is in `[INT32_MIN, INT32_MAX]` it
    `SetEnumValue`s; otherwise it returns `TypeConversionError(...)` —
    an `ErrorValue`, i.e. the expression result is a CEL error.  So
    the 4 legacy rows expect a value-level error, which cel-cpp
    produces.
  - **cel-cpp decays enums to int in the checker** (no strong-enum
    mode).  `checker/internal/type_checker_impl.cc:951-952` and
    `:1074-1075` both `set_type(IntType())` with the comment
    "preserves existing behavior in the other type checkers."  This is
    why §1 descopes strong enums.
  - **Our `cel_set_field` is a void/trap ABI.**  `CelSetFieldImpl`
    returns `absl::Status` (`cel_host.cc:2396`, `:2488-2498`), and the
    trampoline runs it through `StatusToTrap`
    (`cel_host_wasmtime.cc:231`, `:180`).  A non-OK status becomes a
    `wasmtime_trap_new` → eval-level failure, never a CEL error value.
    So merely adding the range check would turn "wrong message" into
    "eval trap" — still a conformance FAIL.  This is the root of
    cleanup-backlog #11 and the reason this work is ABI-shaped, not a
    one-line host tweak.
  - **Our enum write has no bound check.**  `cel_host.cc:2497`,
    `:3002`, `:3114`, `:3353` all do `static_cast<int>(i)` /
    `static_cast<int>(*i)` with no `[INT32_MIN, INT32_MAX]` guard.

## 3. WAT-first ABI design — poison-on-error `cel_set_field`

The codegen shape today (`expr_lower.cc:509-517`):

```
(call $cel_host.cel_make_message (i32.const type_id) (i32.const out_slot))
;; for each entry:
(<eval value> -> value_slot)
(call $cel_host.cel_set_field (i32.const out_slot) (i32.const fref) <value_slot>)  ;; void
(i32.const out_slot)  ;; result = the message slot
```

**Design: keep `cel_set_field` void / 3-arg — no signature change.**
Change only its *semantic contract*:

  1. **Early-out on an already-poisoned slot.**  At entry, read the
     CelValue at `msg_slot`; if `kind == CEL_ERROR`, return OK
     immediately (no-op).  This lets a poison set by an earlier entry
     propagate untouched through the remaining sets.
  2. **Value-error → poison in place, no trap.**  On an out-of-range
     scalar (enum / int32 / uint32 wrapper / bare int32/uint32), write
     a `CEL_ERROR{CEL_ERR_RANGE}` CelValue into `msg_slot` (overwriting
     the partially-built message) and return OK.  The final
     `(i32.const out_slot)` then naturally carries the error.
  3. **Internal invariant violations still trap.**  Wrong value kind
     for the field, unresolvable descriptor, non-mutable backing — these
     are codegen/checker bugs, not CEL semantics; they stay non-OK →
     `StatusToTrap` (per CLAUDE.md: a release build that miscompiles
     silently is worse than one that crashes).

Why this shape: **zero codegen change.** The poison rides the existing
`out_slot` linear-memory cell; no per-field branch, no new return value,
no new ABI arg.  The only edits are inside `CelSetFieldImpl` (early-out
+ poison-vs-trap classification) and a new `CEL_ERR_RANGE` code if one
doesn't exist.

> Note: `CEL_ERR_RANGE` — check `cel_data.h`; `CEL_ERR_OVERFLOW=10`
> already exists and may be the right code (cel-cpp's message is a
> conversion/range error; the conformance matcher compares error KIND
> only, so any error kind passes — pick the closest existing code,
> `CEL_ERR_OVERFLOW` or add `CEL_ERR_RANGE`).

WAT deliverable: `doc/implementation-plan/rewrite/wat/m20_set_field_poison.wat`
— `make_message` + a `cel_set_field` that poisons on overflow + a
following `cel_set_field` that no-ops on the poisoned slot + result =
the error slot.  Assembled with `wasm-as`, run through `wat_runner`
with a `cel_host_cel_set_field_stub` that models the poison (the stub
hook already exists, `wat_runner.h:134`).  Documented in
`wat-traces.md`.

## 4. Differential test harness — cel-cpp oracle vs our pipeline

Goal (per user): for each M20 expression, **parse + compile + run on
BOTH cel-cpp and our pipeline and assert identical results**, rather
than only comparing against pre-baked corpus values.

**Architecture — the namespace + dependency constraints force the
shape:**

  - Our public API `Value` is `celwasm::api::Value`, aliased into
    `namespace cel` (`api/value.h:261`).  cel-cpp's `cel::Value`
    (`common/value.h`) collides.  **The oracle must live in its own TU
    that never includes our `api/value.h`** and must return results in
    a neutral type.
  - We do not currently link cel-cpp's evaluator (only its
    parser/checker/common).  The oracle adds deps on
    `@cel-cpp//runtime:*` (`CreateStandardRuntimeBuilder` →
    `Runtime::CreateProgram(ast)` → `Program::Evaluate(activation)` →
    `cel::Value`, per `runtime/runtime.h:84`).

  - **Neutral exchange type: the `cel.expr.Value` proto** — the same
    type the conformance corpus uses.  The oracle evaluates through
    cel-cpp and encodes its `cel::Value` to `cel.expr.Value`.  Our
    pipeline's result is encoded to `cel.expr.Value` too (the
    conformance runner already has this encode + compare path —
    `runner.cc` / `instance.cc`).  The comparison reuses that existing
    comparator, so "identical" means the same equality the conformance
    gate uses.

Layout:

  - `compiler_v2/testdata/cel_cpp_oracle.{h,cc}` (new) — links
    `@cel-cpp//runtime:*`; exposes
    `absl::StatusOr<cel::expr::Value> EvalWithCelCpp(absl::string_view
    source, const ActivationProto& bindings)`.  No celwasm headers.
  - `compiler_v2/e2e/m20_field_range_diff_test.cc` (new) — for each
    case: run our pipeline → `cel.expr.Value`; run the oracle →
    `cel.expr.Value`; assert match via the shared comparator.  This is
    the M20 test suite of record.

This oracle is reusable beyond M20 (a general differential-conformance
tool); M20 is its first consumer.

## 5. Slice plan

  - **Slice 0** — this doc + WAT (`m20_set_field_poison.wat`) +
    `wat-traces.md` entry; assemble + run through `wat_runner`.
  - **Slice A** — cel-cpp oracle library + 3-4 smoke tests proving the
    oracle agrees with our pipeline on already-passing expressions
    (e.g. `1 + 1`, `TestAllTypes{single_int32: 7}.single_int32`).
  - **Slice B** — poison-on-error `cel_set_field` ABI: early-out +
    value-error poison + trap-classification in `CelSetFieldImpl`.
    Unit-test the trampoline behaviour.
  - **Slice C** — int32 range check at every enum / int32 / uint32
    write site (`cel_host.cc:2497`, `:3002`, `:3114`, `:3353`, and the
    int32/uint32 wrapper arms).
  - **Slice D** — M20 differential test suite (the 8 rows + a boundary
    matrix: INT32_MIN, INT32_MAX, ±1 past each, 0); un-skip the
    `wkt_field_set_test.cc` `*_range` cases; conformance run; tick
    `testing-checklist.md`; close cleanup-backlog #11 (+ note #12
    untouched).

## 6. Future work

  - **Strong enum types** (the 18 `strong_*` rows).  Requires a real
    enum-carrying runtime value (new `CelKind`), checker enum-type
    modeling, per-enum-type constructor overloads (`Foo(int)` /
    `Foo('NAME')`), host read/write as enum (not int), and new
    conformance `enum_value` / enum `type_value` comparators.  Diverges
    from cel-cpp (which decays enums to int) — needs an explicit
    decision to exceed the reference.  `Repr::kEnum` already exists
    (`ir/annotations.h`, `ir/typed_ast.cc:99`) but is unwired
    downstream; `instance.cc:868` `UnimplementedError`s enum-typed
    activation binding.
  - **cleanup-backlog #12** (mixed-origin map equality) is independent
    of this ABI and stays open.
