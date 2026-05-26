# Cleanup backlog

P2 items surfaced by periodic code reviews (see CLAUDE.md
"Periodic code review") and ad-hoc tech-debt observations.  P0
and P1 items live in the active milestone's "pre-close cleanup"
section, not here.

Entries are dated by the review or commit that surfaced them so
the reader can trace context.  When a commit addresses an entry,
the commit message cites `cleanup-backlog #N` and the entry is
struck through or removed.

## Format

```
- [ ] **#<N>** — <one-line description>.
      Surfaced: <YYYY-MM-DD review name or commit hash>.
      Files: <main paths>.
      Why P2: <reason this isn't P0/P1>.
```

## Open

- [ ] **#1** — `third_party/wasi_sdk/BUILD.bazel` flat-aliases
      `//third_party/wasi_sdk:clang` → `@wasi_sdk_darwin_arm64//:clang`;
      Linux + macOS-x86_64 will fail at the bazel-analyzing stage.
      Surfaced: 2026-05-18 MVP review (P2 in §"Tech-debt
      inventory").
      Files: `third_party/wasi_sdk/BUILD.bazel`, `MODULE.bazel`.
      Why P2: works for the dev box today; CI matrix expansion is
      a Phase B / M1.1 follow-up, not a merge blocker.

- [ ] **#2** — `kRuntimeExports[]` in `eval/engine.cc`
      is the third source of truth for the runtime's exported
      kernel names — the other two are
      `runtime/BUILD.bazel`'s `-Wl,--export=…` flags
      and `wasm_imports.txt`.  Nothing checks they match;
      regressing one of the three lights up at instantiate-time
      as `unknown import: cel::<name>`.
      Surfaced: 2026-05-18 MVP review (P2).
      Files: `eval/engine.cc`, `runtime/BUILD.bazel`.
      Why P2: every new kernel is caught at next instantiate; a
      consolidation pass (single header generating both lists)
      is the right fix but doesn't unblock anything in flight.

- [ ] **#3** — `doc/implementation-plan/rewrite/wasi/experiments/exp1_re2/`
      contains a `wasi-sdk/` symlink + cached absl install used by
      the absl::ParseTime experiment.  Both are post-Phase C
      garbage once `abseil-cpp` lands as a proper `http_archive`.
      Surfaced: 2026-05-18 MVP review (P2).
      Files: `wasm_compilation_experiments/exp1_re2/`.
      Why P2: experimental directory; doesn't affect production
      build paths.

- [ ] **#4** — `runtime/cel_memory.c` has an inline-asm
      opacity barrier (`asm volatile("" :: "r"(...))`) that
      prevents clang from miscompiling `*(base + off)` stores as
      no-ops when `base` is a zero pointer literal.  Verify the
      barrier still triggers under wasi-sdk clang-19 by inspecting
      the emitted `.wasm` for the expected store instructions
      (was historically clang-15-specific).
      Surfaced: 2026-05-18 MVP review (P2).
      Files: `runtime/cel_memory.c`.
      Why P2: tests + e2e currently pass, so the barrier is
      working at runtime; the concern is that a future clang
      could regress.  Add a disassembly assertion in a follow-up.

- [ ] **#5** — `runtime/cel_memory.c:1-15` comment block
      describes a "1-page memory" + the `cel_alloc` ABI that no
      longer exists.  Comment is stale post-MVP; the wasi-sdk
      runtime imports a 2-page memory and `cel_alloc` was deleted
      in B1.
      Surfaced: 2026-05-18 MVP review (P2).
      Files: `runtime/cel_memory.c`.
      Why P2: comment-only; no runtime impact.

- [ ] **#6** — `cel_memory_size_()` on wasm returns a hard-coded
      64 KiB; with the wasi-sdk build the linear memory is 2 ×
      64 KiB = 128 KiB, and the accessor should reflect that.
      Either compute from `__heap_base` + arena capacity or update
      the constant.  Today's incorrect value is harmless because
      no kernel actually consults `cel_memory_size_()` on the
      wasm side (only the native build does).
      Surfaced: 2026-05-18 MVP review.
      Files: `runtime/cel_memory.c`.
      Why P2: not yet observable; lands when the host_string_arena
      cleanup goes in (M7).

- [ ] **#9** — `{'k': 'v'}.k` (map-dot-field sugar) is broken
      pre-existing.  cel-cpp's checker accepts `.k` on a `map<string,
      V>` operand as sugar for `m["k"]` and stamps the kSelect's
      result type as `V`.  Our `EmitKSelect` does not branch on
      `op_ann->repr == Repr::kMap` and instead routes every non-
      optional Select through `cel_get_field`, which is the
      message-field trampoline — it errors on a CEL_MAP_ARENA
      operand.  Confirmed by an e2e probe `EvalSource("{'k':
      'v'}.k")` returning a CEL_ERROR value.
      Impact: blocks 3 conformance rows in
      `tests/simple/testdata/optionals.textproto` —
      `optional_chaining_1`, `optional_chaining_2`,
      `optional_chaining_3`.  All three have the shape
      `{'c': {...}}.c[?'…']…`; the leftmost `.c` is the map.field
      sugar that errors out, the rest of the chain never sees a
      valid operand.  Likely blocks rows in other corpora too — the
      probe is a universal pattern, not optionals-specific.
      Fix: in `EmitKSelect`, detect `op_ann->repr == Repr::kMap`
      (with string key type per type_map) and route to
      `cel_map_lookup_arena` / `cel_map_lookup` / `cel_host_cel_map_lookup`
      via `MapLookupCallTarget(op_ann->map_origin)`, passing the
      field name as a CelValue rodata slot (same lift pattern Slice
      B introduced via `SelectKeyRodataVisitor`).  Bytes / int key
      types do not need the sugar — the spec only permits `.field`
      on string-keyed maps.
      Surfaced: 2026-05-21 M14 Slice B implementation.
      Files: `compiler/codegen/expr_lower.cc::EmitKSelect`,
      `compiler/codegen/layout_pass.cc::SelectKeyRodataVisitor`
      (extend the operand-Repr predicate to include `kMap`).
      Why P2: out of scope for M14 (Slice B mandate was
      Select-on-optional, not Select-on-map).  Self-contained
      follow-up slice — likely 2–4 hours of work, mostly mirroring
      Slice B's structure.

- [ ] **#10** — `is_zero_value` in `runtime/cel_optional.c`
      traps with `__builtin_trap()` on `CEL_MESSAGE` (and
      `CEL_LIST_HOST` / `CEL_MAP_HOST`) because the proto-zero
      predicate needs proto reflection (cel-cpp parity:
      `ParsedMessageValue::IsZeroValue()` walks
      `Reflection::ListFields` and checks the unknown-field set).
      Impact: `optional.ofNonZeroValue(<message>)` traps at
      eval-time.  Newly reachable in Slice E because the
      proto-`?field:` gate was lifted; previously the row
      classified as SKIP-static_subset.
      Affected conformance rows in
      `tests/simple/testdata/optionals.textproto`:
        - `optional_ofNonZeroValue_struct_optional_ofNonZeroValue_map_optindex_field`
          (now FAIL where it was previously SKIP).
        - Other `optional.ofNonZeroValue(TestAllTypes{…})` shapes
          in the corpus will hit the same trap once
          `static_subset` admits them (most are still SKIP'd on
          unrelated `dyn` issues, so are not yet observable).
      Fix shape: a new host trampoline
      `cel_host.cel_message_is_zero(out_slot, msg_slot)`
      that calls `ParsedMessageValue::IsZeroValue` (or the
      equivalent reflection walk) and writes a CEL_BOOL into
      `out_slot`.  The wasm-side `is_zero_value` arm for
      CEL_MESSAGE would call this trampoline; same for
      `CEL_LIST_HOST` / `CEL_MAP_HOST` (host_list_size / host_map_size
      already exist — wasm can read them directly without a new
      trampoline; trap arm replaced with `return hdr.size == 0`).
      Surfaced: 2026-05-22 M14 Slice E closeout conformance run.
      Files: `runtime/cel_optional.c::is_zero_value`,
      `eval/internal/cel_host.{h,cc}` (new
      `cel_message_is_zero` trampoline),
      `eval/internal/cel_host_wasmtime.cc` (register).
      Why P2: only one corpus row newly FAILing; the rest of M14
      ships cleanly.  Self-contained follow-up slice, mostly
      mirroring the existing `cel_host.cel_list_size` /
      `cel_host.cel_map_size` shape — likely 2–4 hours.

- [x] **#11** — `cel_set_field` (the proto field-write trampoline) was a
      void ABI: it signalled an out-of-range scalar (e.g. an int32 field
      assigned `2147483648`) only by *trapping* the wasm instance, which
      surfaced as a non-OK `Eval` status — never as a CEL error *value*.
      The conformance `eval_error` matcher compares error KIND on a
      returned CEL value, so these rows couldn't PASS: a trap is an
      engine-level failure, not a catchable CEL error.
      RESOLVED 2026-05-25 (M20, `rewrite/m20-enum-field-range.md`).
      `CelSetFieldImpl` now classifies an `OutOfRange` field-write
      status as a value-level error: it poisons the message slot in
      place with `CEL_ERROR{CEL_ERR_OVERFLOW}` and returns OK, and
      early-outs on an already-poisoned slot so the first overflow in a
      multi-field constructor propagates.  No ABI/codegen change (the
      poison rides the existing `out_slot`).  Flipped the 4 int32/uint32
      `*_range` rows plus the 4 enum `standalone_enum` range rows;
      `wkt_field_set_test.cc` `*_range` cases un-skipped, differential
      coverage in `testdata/cel_cpp_oracle_test.cc`.

- [ ] **#12** — mixed-origin map equality: `CelMapEqImpl` only handles
      the case where BOTH operands are host-backed maps (or both arena
      maps).  A host-map-field operand compared against an arena map
      literal returns `CEL_ERR_TYPE_MISMATCH` instead of a structural
      equality.
      Impact: blocks the map `*_null_pruned` Eq-form rows in
      `proto2.textproto` / `proto3.textproto` (2 rows; documented as
      GTEST_SKIP in `e2e/wkt_field_set_test.cc`).  The
      null-prune itself is proven correct (size shrinks, surviving
      entry reads back equal) — only the cross-origin `==` is missing.
      Fix shape: a normalizing comparison in `CelMapEqImpl` that reads
      both operands through the same key/value accessor regardless of
      origin (host vs arena), mirroring how list equality already
      bridges `CEL_LIST_HOST` vs `CEL_LIST_ARENA`.
      Surfaced: 2026-05-24 WKT field-set conformance work.
      Files: `runtime/` (`CelMapEqImpl`), the host map
      accessors in `eval/internal/cel_host.cc`.
      Why P2: 2 corpus rows; cross-origin map `==` is a self-contained
      runtime-kernel follow-up.

- [ ] **#13** — `Instance::PartialEval` SEGFAULTs when the activation
      binds a list (or map) whose elements are `Value::Message`, e.g.
      `xs[0].age` with `xs : list<Customer>`, or `m['k'][0].age` with
      `m : map<string, list<Customer>>`.  The same expressions evaluate
      correctly under plain `Instance::Eval` (pinned by
      `e2e/m4_test.cc::ListBindingE2ETest.BoundListOfMessageIndexedField`),
      and `PartialEval` on a list-of-PRIMITIVE binding works
      (`e2e/partial_eval_test.cc::ListPrimitivePartialEvalTest.*`), so
      the crash is specific to marshaling a host container of messages
      while `bindings.unknown_patterns` is non-empty (it crashes even
      under a non-matching pattern, so it is a marshal/setup fault, not
      a pattern-match fault).  Likely in the activation-marshal path
      that walks a `HostList`/`HostMap` of message slots under the
      PartialEval branch of `eval/instance.cc` (vs the Eval branch).
      Impact: cannot run partial-eval over container-of-message inputs
      at all.  Documented as GTEST_SKIP in
      `e2e/partial_eval_test.cc` {`ListOfMessagePartialEvalTest.*`,
      `MapOfListOfMessagePartialEvalTest.*`}, each carrying the
      assertion it will make once the crash is fixed (the read stays
      CONCRETE — a container root has no `.field` select so the unknown
      can't reach the element; see the file header).
      Surfaced: 2026-05-25 partial-eval matrix work.
      Files: `eval/instance.cc` (PartialEval marshal path),
      `eval/internal/cel_host.cc` (HostList/HostMap message marshal).
      Why P2: PartialEval over container-of-message is not on a
      conformance row; scalar / message-field partial-eval (the shipped
      surface) is unaffected.

## Closed

- [x] **#8** — `compiler/codegen/expr_lower.cc` had two
      `ABSL_CHECK(false)` stubs that Slice A of M14 converted to
      `return absl::UnimplementedError(...)`:
        - `EmitKStructExpr` ~line 507 — `f.optional()` proto-literal
          `?field:` entries (blocked on M7 Slice 9 + M14 follow-up).
        - `EmitKIndexCall` ~line 574 — `_[_]` Call with operand
          `Repr` other than `kMap` / `kList` (i.e., optional-typed
          operands; lit up in M14 Slice B via `LowerSelect`
          Repr-detection).
      Surfaced: 2026-05-21 M14 Slice A independent review.
      Closed: 2026-05-21 by M14 Slice B.
      Files: `compiler/codegen/expr_lower.cc`,
      `compiler/frontend/parse_and_check.cc`.
      Resolution: `CheckSubsetStruct` rejects `?field:` proto-literal
      entries at the static-subset gate so the harness classifies
      affected rows as SKIP-static_subset; both codegen arms restored
      to `ABSL_CHECK(false) << "stub until ..."`.  The optional-typed
      `_[_]` operand arm in `EmitKIndexCall` is no longer reachable
      either — Slice B's `Repr::kOptional` branch handles it
      explicitly, and any other non-`{kMap, kList, kOptional}` Repr
      is rejected before codegen by `parse_and_check.cc::
      UnacceptableLabel`.  Closeout verified by 70/70 unit tests + no
      conformance regressions vs baseline.

- [x] **#7** — `CEL_LOG("enter")` in every public runtime helper does
      a wasm→host `fprintf(stderr)` trampoline on every invocation;
      under wasi-sdk's call-prologue convention the per-call cost
      roughly doubled vs the pre-WASI freestanding build, dominating
      per-Eval cost on aggregate-heavy expressions (`Eval_ListAt_Arena`
      took 3.6 µs / Eval where the WASI-free body would take 0.6 µs).
      Fix: gate `-DCEL_LOG_DISABLED` on `-c opt` via `config_setting`
      in `runtime/BUILD.bazel`; CEL_LOG stays live for
      `dbg` / `fastbuild` so the dead-code audit per `cel_log.h`
      still works.  Measured improvement: 1.4×–5.7× faster on the
      `BM_Eval_*` rows of `//bench:pipeline_bench`.  See
      `bench/README.md` updated baseline and
      `doc/implementation-plan/rewrite/wasi/POST_MIGRATION_BENCH.md`
      "Mitigation paths" item 1.
      Surfaced: 2026-05-18 post-Phase-C perf-run investigation.
      Closed: 2026-05-19.
      Files: `runtime/BUILD.bazel`,
      `bench/README.md`,
      `doc/implementation-plan/rewrite/wasi/POST_MIGRATION_BENCH.md`.
