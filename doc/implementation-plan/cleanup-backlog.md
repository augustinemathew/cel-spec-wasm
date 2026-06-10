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

- [ ] **#14** — a comprehension whose `iter_range` is UNKNOWN (or
      ERROR) returns the empty-range IDENTITY instead of propagating —
      `exists`→false, `all`→true, `exists_one`→false, `map`/`filter`→[]
      — a SILENTLY WRONG answer (soundness gap, not a crash).
      `eval/internal/cel_host.cc::CelListIterOpenImpl` maps any
      non-CEL_LIST_HOST range (which now includes CEL_UNKNOWN /
      CEL_ERROR) to a zero-count arena view via `write_empty()`, and
      the comprehension prologue in
      `compiler/codegen/expr_lower_comprehension.cc` has no
      3VL-absorption branch for the range value.  Reachable two ways:
      (a) a container-typed FIELD marked unknown then iterated
      (`c.tags.exists(e, …)` with pattern `c.tags`) — pre-dates
      whole-variable unknowns; (b) a bare list/map VARIABLE marked
      unknown then iterated (`xs.exists(e, …)` with pattern `xs`) —
      newly reachable since the bare-variable marshal lever landed
      (eval/instance.cc BareVariableUnknownId).
      cel-cpp oracle (the correct behavior, confirmed against the
      source): `third_party/cel-cpp/eval/eval/comprehension_step.cc`
      `ComprehensionDirectStep::Evaluate` lines 156-172 — the iter_range
      is evaluated first, and `switch (range.kind())` routes
      `ValueKind::kError` and `ValueKind::kUnknown` (fall-through) to
      `result = std::move(range); return OkStatus();` — i.e. the
      comprehension result IS the unknown/error, no iteration.  (A map
      range additionally gets a partial-unknown check at lines 146-150.)
      Our fix mirrors `result = std::move(range)`.
      Fix shape: WAT-first codegen arm — at the comprehension prologue,
      after the iter_range value is evaluated, branch on its
      CelValue.kind; CEL_UNKNOWN / CEL_ERROR → write the range value
      straight to the comprehension result slot and skip the loop
      (3VL absorption, mirroring how `+` / index / select already
      absorb).  Pinned by GTEST_SKIP in
      `e2e/m2_partial_eval_test.cc::ListPrimitivePartialEvalTest`
      `.ComprehensionOverUnknownListIsUnknown`, carrying the assertion
      it will make once fixed.
      Surfaced: 2026-05-25 partial-eval whole-variable-unknown work.
      Files: `compiler/codegen/expr_lower_comprehension.cc`
      (prologue range-absorption branch), `eval/internal/cel_host.cc`
      (CelListIterOpenImpl), a `doc/.../wat/NN_*.wat` trace.
      Why P1 (not P2): silent wrong answer under PartialEval — exactly
      the class of bug the codebase's no-silent-miscompile rule exists
      to prevent; should be cleared before the next milestone closes.

- [ ] **#15** — Parser hard-caps CEL source at 100 000 codepoints
      (`expression_size_codepoint_limit`,
      `third_party/cel-cpp/parser/options.h:37`).  Our
      `DefaultParserOptions()` in
      `compiler/frontend/parse_and_check.cc:1079` doesn't override
      the default and `CompilerOptions` exposes no knob to raise it.
      Concrete consequence: a literal `[0, 1, ..., 999_999]` source
      (≈ 7.9 MB) is rejected at parse with
      `INVALID_ARGUMENT: expression size exceeds codepoint limit`;
      ~16 k int elements is the practical ceiling.  E2e repro:
      `e2e/known_bugs_test.cc::KnownBugs.ParserSourceCodepointLimitNotConfigurable`.
      Fix shape: add `CompilerOptions::parser_codepoint_limit` (or
      similar) that `DefaultParserOptions()` honours; default stays
      at 100 k.
      Surfaced: 2026-06-03 `in`-operator benchmark (`bench/in_operator_bench.cc`).
      Files: `compiler/compiler.h`, `compiler/frontend/parse_and_check.cc`.
      Why P2: a real ceiling but uncommon — production CEL sources
      rarely exceed even 10 k codepoints; embedders who need 1 M
      elements bind a list variable, which has no parser involvement.

- [ ] **#16** — Compiling + planning a literal int list of 10 000
      elements succeeds, but the resulting wasm module **panics
      wasmtime on Eval** (`crates/wasmtime/src/runtime/store.rs:2440:17:
      assertion failed: fault.is_none()` — a Rust panic, not a
      graceful `absl::Status`).  This is a more severe surfacing of
      the same family as `e2e/known_bugs_test.cc::KnownBugs.ExpressionIntermediatesArenaCliff`
      (a 4 000-element `size([0..n])` returns `CEL_ERR_OVERFLOW`);
      the `in`-list path doesn't gracefully overflow — it traps the
      whole runtime.  Root cause is likely the 64 KiB per-Eval
      arena (`runtime/cel_layout.h:16` `CELWASM_WASM_PAGE_SIZE`)
      being exceeded by the materialized literal list **after**
      a runtime-side allocation that doesn't check capacity.
      E2e repro: `e2e/known_bugs_test.cc::KnownBugs.LiteralIntListInScanTrapsAt10K`.
      Surfaced: 2026-06-03 `in`-operator benchmark.
      Files: `runtime/cel_runtime.c` (or wherever `cel_list_in`
      reaches before the bounds check); `runtime/cel_arena.c`;
      `compiler/codegen/static_memory_builder.cc` (the literal
      list is materialised here at codegen time — the trap may
      be a codegen issue, not a runtime one).
      Why P0: a CEL source that the parser AND checker accept
      crashes the host process via wasmtime panic instead of
      returning a CEL error.  An untrusted source can crash the
      embedder.  Fix MUST also turn the panic into a graceful
      `absl::Status` (the test asserts post-fix Eval ok-ness; if
      a fix leaves the panic but raises N, the test still crashes
      the process when un-skipped, which is correct — a panic-on-
      large-list is not "fixed").

- [ ] **#17** — A bound `list<string>` of ≥ 10 000 50-byte strings
      Eval'd through `perm in perms` returns
      `FAILED_PRECONDITION: arena OOM in CelMapLookupImpl` from
      inside `cel_list_in`'s trampoline.  Per-Eval arena
      (`runtime/cel_layout.h:16` 64 KiB; `runtime/cel_arena.c`)
      cannot hold the materialised string-set scan view.
      Same class as backlog #16 but graceful (returns
      a non-OK Status instead of trapping) and surfaces on the
      bound-variable path, not the literal-list path.  Distinct
      from `e2e/known_bugs_test.cc::KnownBugs.ExpressionIntermediatesArenaCliff`
      because the source list isn't an intermediate — it's a bound
      variable; the OOM happens during scan, not list construction.
      E2e repro: `e2e/known_bugs_test.cc::KnownBugs.BoundStringListInScanArenaOomAt10K`.
      Surfaced: 2026-06-03 `in`-operator benchmark.
      Files: `runtime/cel_list.c` (or wherever `cel_list_in`
      lives), `runtime/cel_arena.c`.  The fix may be to make
      `cel_list_in` use a streaming/non-materialising scan that
      doesn't grow the arena per-element, OR to grow the arena
      on demand.
      Why P1 (not P0): graceful error, not a process crash.  But a
      10 k-element permission set is a real workload (cel-policy
      / IAM authorisation) — this is on the production envelope.

- [ ] **#18** — `Value::Message(StringValue{value: "x"})` bound
      against a `string`-declared variable fails at Eval with
      `INVALID_ARGUMENT: Activation[s]: declared string, bound message`.
      The encoders for bool / int / uint / double scalars all call
      `TryEncodeWktWrapperMessage` (`eval/instance.cc:607/620/633/647`)
      and peel a wrapper message at bind; `EncodeStringOrBytes`
      (`eval/instance.cc:444`) does NOT, even though
      `WrapperFqnToCelKind` (`eval/instance.cc:494`) returns
      `CEL_STRING` for `google.protobuf.StringValue` and
      `CEL_BYTES` for `google.protobuf.BytesValue` (so the lookup
      table acknowledges the wrappers; the encoder just doesn't
      consult it).  Asymmetric API surface — the workaround is to
      bind native `Value::String` instead.  E2e repro (negative —
      asserts the workaround works):
      `e2e/host_fn_type_matrix_test.cc::HostFnTypeMatrix.WktStringValueWrapperPeelAtBindNotWired`
      (carries the `// BUG:` comment naming this entry; flip its
      assertion to positive when this lands).
      Fix shape: add a `TryEncodeWktWrapperMessage` call at the top
      of `EncodeStringOrBytes` mirroring the numeric encoders, and
      extend `WriteNumericWrapperPayload` (or add a parallel string
      helper) to copy the wrapper's inner string/bytes into the
      arena via the same path the native-Value path uses.
      Surfaced: 2026-06-03 host-fn type-matrix audit (subagent).
      Files: `eval/instance.cc:444`,
      `eval/instance.cc:518 WriteNumericWrapperPayload`.
      Why P1: asymmetric public surface — half the WKT wrappers
      auto-peel at bind, half don't.  Embedders binding raw
      `Value::Message(StringValue{…})` get a confusing kind
      mismatch with no hint to use `Value::String` instead.

- [ ] **#19** — `compiler/celfn/function_library.cc:256-322`'s
      celfn IDL parser admits only the 12 base CEL types
      (bool / int / uint / double / string / bytes / null /
      timestamp / duration / list / map / message FQN); it has
      no spelling for `google.protobuf.{Any,Struct,Value,ListValue}`
      and no `optional<T>` keyword.  Custom fns can't take any of
      those as a declared parameter today.  E2e repros (SKIP-pinned
      to this entry):
      `e2e/host_fn_type_matrix_test.cc` —
      `HostFnTypeMatrix.WktAnyHostFnArg`,
      `HostFnTypeMatrix.WktStructHostFnArg`,
      `HostFnTypeMatrix.WktValueHostFnArg`,
      `HostFnTypeMatrix.WktListValueHostFnArg`,
      `HostFnTypeMatrix.ExplicitOptionalArgNotApplicable`
      (and `HostFnTypeMatrix.ExplicitTypeArgNotApplicable` for
      `type`, which is intentionally out of scope per
      `doc/implementation-plan/rewrite/m21-host-call-adapter.md:67`).
      Fix shape: extend the IDL grammar to admit the four WKT
      struct types (probably as reserved FQNs) and an
      `optional<T>` wrapper; thread them through to the
      generated declarations and the `Backend::kHost` /
      `Backend::kNative` codegen paths.
      Surfaced: 2026-06-03 host-fn type-matrix audit.
      Files: `compiler/celfn/function_library.cc`,
      grammar in `compiler/celfn/celfn_grammar.g4` (if any),
      `compiler/celfn/parser.cc`.
      Why P2: feature gap, not a bug — declaring an unsupported
      type today produces a clear parse-time error.  Reachable
      WKT semantics are still available through the message-FQN
      path; the gap is the convenient IDL syntax.

- [ ] **#21** — `cel_list_in_arena` (`runtime/cel_runtime.c:598-617`)
      and `cel_list_eq_arena` (`runtime/cel_runtime.c:619-642`)
      call `cel_value_eq` per element, which dispatches through
      `cel_value_eq_polymorphic` to a kind-switch (numeric
      ladder, bytes, null, dur/ts, IP, CIDR, map_keys_equal).
      For the homogeneous-element common case — `[s1, s2, ..., sN]
      in [bound]` or `[a]==[b]` — the per-call cost is the
      polymorphic dispatch, NOT the byte compare.  After #20
      lands (chunked byte compare), the polymorphic dispatch
      becomes the dominant per-element cost.
      Why a win: hot path; eliminates the kind-switch in the
      inner loop when both lists are statically homogeneous.
      The IR already carries `ListBacking::element_type` (per
      `rewrite/design.md`'s typed-IR contract); codegen can pick
      a specialized scan kernel by element type — `cel_list_in_string`,
      `cel_list_in_int`, `cel_list_in_double` — bypassing the
      polymorphic dispatcher entirely.  Same trick the
      `cel_*_eq_at_vv` family in `cel_compare.c` already uses
      for scalar pairs; this just lifts it to the loop level.
      Concrete shape (per-kind specialization, list_in for int):

      ```c
      void cel_list_in_int_arena(uint32_t out_slot, uint32_t v_slot,
                                 uint32_t list_slot) {
        // 3VL + kind check identical to cel_list_in_arena.
        ArenaListHeader* hdr = arena_list_header(l);
        const CelValue* elems = arena_list_element(hdr, 0);
        int64_t target = v->payload.i;
        for (uint32_t i = 0; i < hdr->count; ++i) {
          // Per-element: a single 8-byte compare, no kind switch,
          // no function call — the LTO'd version of cel_value_eq
          // currently still has the switch.
          if (elems[i].kind == CEL_INT && elems[i].payload.i == target)
            { write_bool(out, 1); return; }
        }
        write_bool(out, 0);
      }
      ```

      Codegen picks the specialized kernel from
      `ListBacking::element_type`; falls back to the polymorphic
      `cel_list_in_arena` only when the list is `dyn` or
      contains mixed numeric kinds (the cases that need the
      cross-type ladder).
      Risk: requires codegen to thread element-type through the
      `_[_]` call lowering — non-trivial scope.  Mitigation: do
      string + int first (the two by far most common element
      kinds in real CEL fixtures); leave dyn/mixed on the
      polymorphic path.  Mismatched kinds inside a "homogeneous"
      list (e.g. `[1, 1u]`) currently match cross-type via
      `numeric_compare_kernel`; the specialized kernel would
      return 0 for those — codegen MUST gate specialization on
      a checker-verified single-kind element type.
      Files: `compiler/codegen/expr_lower.cc` (the `_[_]` and
      `@in` arms), `runtime/cel_runtime.c` (new
      `cel_list_in_<kind>_arena` exports + matching wasm export
      list in `wasm_exports.txt`), `compiler/ir/annotations.h`
      (already carries the element type — verify).
      Surfaced: 2026-06-03 runtime optimization review.
      Severity: P2 — closes the residual gap after #20, but #20
      is the load-bearing fix.

- [ ] **#22** — `arena_map_entry_matches` (`runtime/cel_runtime.c:737-747`)
      is O(N) per outer iteration, so `cel_map_eq_arena`
      (lines 749-775) is O(N²) on map size.  For an N=1024 map
      equality that's 1M map_keys_equal calls plus 1k value
      eqs.  The arena map's entries are unsorted (insertion
      order; see `cel_map_insert`), so the quadratic scan is
      the only correct option without auxiliary structure.
      Why a win: hot path on map-heavy expressions; replacing
      the O(N²) scan with a single pass + key→index lookup is
      asymptotic.  Two paths:

      (a) For small N (say ≤ 16) keep the quadratic scan — the
      constant-factor advantage of a flat array dominates.
      (b) For larger N, sort one side's keys at first comparison
      and bsearch — but the arena is append-only and the map
      might be aliased; the safer shape is to *snapshot* one
      side's keys+vals into a sorted u32 index array on first
      large-N invocation, then bsearch.

      Cheaper interim: short-circuit on first hash-trivial
      key mismatch (the entry-count check on line 761 already
      covers the empty case; add the obvious `if (ha->count == 0)
      return write_bool(out, 1)` micro-shortcut).
      Risk: introducing sort-on-equality changes the worst-case
      allocation behaviour; only worth doing if a benchmark
      actually shows map-eq dominating.  Recommend deferring
      until a workload shows it.
      Files: `runtime/cel_runtime.c:737-775`.
      Surfaced: 2026-06-03 runtime optimization review.
      Severity: P2 — algorithmic concern only triggers at
      N ≥ ~64; today's fixtures rarely hit that.

- [ ] **#23** — `cv_at(off)` is `cel_memory_base_() + off`
      (`runtime/cel_internal.h:93-95`), and `cel_memory_base_`
      on wasm32 is an out-of-line function with an inline-asm
      opacity barrier (`runtime/cel_memory.c:31-35`).  Hot
      loops re-call `cv_at`/`cel_value_at` per iteration — every
      call is a function call + opacity barrier (forcing the
      base into a fresh register).  See e.g.
      `runtime/cel_runtime.c:670-677` (`copy_elements`):

      ```c
      static void copy_elements(uint32_t elements_off, uint32_t dst_index,
                                ArenaListHeader* src) {
        for (uint32_t i = 0; i < src->count; ++i) {
          *(CelValue*)(cel_memory_base_() + elements_off +
                       ((size_t)kCelListEntryStride * (dst_index + i))) =
              *arena_list_element(src, i);
        }
      }
      ```

      and `arena_list_element` itself (line 330-333) re-fetches
      the base on every call.  With `-flto + -O3` clang ought to
      hoist the asm-opacified return value out of the loop —
      the asm clause is `__asm__("" : "+r"(p));` which only
      constrains the *result*, not the function — but the call
      to `cel_memory_base_()` is opaque to LTO across TUs
      (definition lives in `cel_memory.c`).
      Why a win: small fixed cost per element on every
      aggregate construction / scan; multiplicative over loop
      bodies that touch 3+ slots per iter (eq, lt, etc.).
      Concrete fix: hoist the base pointer load to a local
      `const uint8_t* base = cel_memory_base_();` at function
      entry and index `(CelValue*)(base + …)` in the loop.
      Already done in `spans_equal` itself (line 170) and
      `type_eq_at_vv` (line 1298); apply consistently to
      `arena_list_element`, `arena_map_entry_key`,
      `arena_map_entry_val`, and their callers'
      hot loops at `cel_runtime.c:610, 635, 672, 725, 741, 768`.
      Risk: under LTO this *may* already be hoisted by clang
      after `-flto` lets it see across TUs.  Verify with
      `wasm-objdump -d cel_runtime.wasm | grep -A 4
      cel_list_in_arena` before/after — if every iter shows a
      `call $cel_memory_base_`, the hoist isn't happening and
      this is a real win.
      Files: `runtime/cel_runtime.c` (the 6 loops cited).
      Surfaced: 2026-06-03 runtime optimization review.
      Severity: P2 — speculative until disassembly confirms;
      cheap to apply and never a pessimization.

- [ ] **#24** — `arena_alloc` (`runtime/cel_arena.c:75-104`)
      calls `memset(p, 0, need)` on every allocation, zeroing
      every byte the caller is about to overwrite.  Every
      CelValue alloc (most common — `cel_make_*`, the per-eval
      tmp slot for `cel_map_count`, the iter-state allocs)
      zeroes 24 bytes the constructor immediately stamps; every
      ArenaMapHeader/ArenaListHeader alloc zeroes 16 bytes
      the header constructor stamps; every entries-run alloc
      zeroes N×24 or N×48 bytes that `cel_list_append_at`
      stamps element-by-element.  The zero is only load-bearing
      for the `_pad` field of headers (and even that is
      stamped on line 666 / 356).
      Why a win: small fixed cost on every alloc, multiplied
      by the per-eval alloc count.  A literal-list with N=1000
      pays 1× 16 B header zero, 1× 24000 B elements-run zero
      (24 KB of `i32.store8` × 24000 ≈ 6000 wasm instructions),
      then immediately stamps every byte.  The zero is dead
      work.  Remove with a `bool zero_init` parameter or split
      into `arena_alloc_raw` (no zero) vs `arena_alloc_zeroed`,
      and migrate the constructors that already stamp every
      byte to the raw variant.
      Risk: any caller that *implicitly* relies on the zero
      (e.g. `cel_map_create` sets `entries_offset = 0` for
      cap=0; but that's an explicit stamp at line 84) breaks.
      Audit each caller before flipping; introduce a
      `#define CELWASM_ARENA_ZERO_INIT 1` guard for an A/B
      bench.  CEL_LOG of arena bytes might also rely on it
      cosmetically.
      Files: `runtime/cel_arena.c:89`,
      `runtime/cel_make.c:18-115` (every constructor immediately
      stamps), `runtime/cel_runtime.c:64-89, 335-359, 648-668`
      (list/map header constructors).
      Surfaced: 2026-06-03 runtime optimization review.
      Severity: P2 — measurable on alloc-heavy expressions
      (large list literals, deep comprehensions); negligible
      elsewhere.

- [ ] **#25** — `arena_alloc` runs `align_up_8(n)`
      (`runtime/cel_arena.c:33-35, 77`) on every call even
      when the request is already a multiple of 8.
      `sizeof(CelValue) == 24`, `sizeof(ArenaMapHeader) ==
      sizeof(ArenaListHeader) == 16`, `kCelMapEntryStride ==
      48`, `kCelListEntryStride == 24` — every fixed-size alloc
      is already 8-aligned, so the round-up is unconditional
      dead work for the static-size paths.  The general path
      is used only for string/bytes payload allocs
      (`cel_make_string_view`-style and `concat_into_out`),
      where `n` is data-dependent.
      Why a win: small fixed cost on every alloc.  Make a
      `static inline uint32_t arena_alloc_aligned8(uint32_t n)`
      that skips the round-up; callers with statically-known
      multiples-of-8 sizes use it directly.
      Risk: caller mis-uses with a non-multiple-of-8 size,
      leading to a misaligned cursor.  Mitigate with a
      `_Static_assert((sizeof(T) & 7) == 0, ...)` at each call
      site, or a runtime DCHECK `ABSL_CHECK((n & 7) == 0)` in
      the inline.
      Files: `runtime/cel_arena.c`, every constructor in
      `runtime/cel_make.c`, `runtime/cel_runtime.c`'s header
      allocs.
      Surfaced: 2026-06-03 runtime optimization review.
      Severity: P2 — sub-1% on a typical eval; only matters in
      bench-grade tuning.

- [ ] **#28** — `cel_internal.h`'s `static inline` helpers
      (`spans_equal`, `absorb_3vl_*`, `require_kinds`,
      `write_*`, `poison`, `cv_at`) generate a copy per TU
      that includes the header — fine when each copy inlines,
      but `spans_equal` does NOT always inline (clang's
      inliner gives up on the 175-byte body when the caller is
      large, e.g. `cel_list_in_arena`).  Under `-flto`
      cross-TU LTO recovers this by collapsing the copies,
      but the worst-case codegen path still emits a real call.
      Why a win: small fixed cost; relevant only if
      disassembly shows `call $spans_equal` in
      `cel_list_in_arena`.
      Fix: tag the helper `static inline __attribute__((always_inline))`
      to force inlining where size is below the threshold.
      Same applies to `numeric_compare_kernel` — it's
      explicitly `__attribute__((noinline))` at
      `cel_compare.c:160` for a good reason (avoiding
      `__multi3` re-fold), but the wrappers
      `cel_numeric_eq_at_vv` et al. each pay one indirect call;
      consider an inline tri-state to-bool collapser if those
      show up hot.
      Risk: forcing inline grows the .wasm; usually 2–5 KB
      total even on a 50 KB runtime.  Bench-validate.
      Files: `runtime/cel_internal.h`, `runtime/cel_compare.c`.
      Surfaced: 2026-06-03 runtime optimization review.
      Severity: P2 — only worth doing after disassembly
      confirms the inliner is bailing.

- [ ] **#29** — `cel_map_count` on the CEL_MAP_HOST branch
      (`runtime/cel_runtime.c:1129-1138`) calls `arena_alloc`
      for a scratch CelValue, invokes
      `cel_host_cel_map_size`, then reads the int back —
      pays an arena alloc per map-source comprehension to
      get the size.  The alloc isn't reclaimed (arena is
      bump-only) and the arena bytes accumulate across
      iterations.
      Why a win: small fixed cost per comprehension; matters
      only for tight nested comprehensions over host maps.
      Fix: the host trampoline `cel_host_cel_map_size` could
      gain a `cel_host_cel_map_count` variant that returns
      `uint32_t` directly without staging through a CelValue.
      Symmetric with `cel_map_count`'s own `uint32_t` return.
      Risk: new ABI surface, new host import.  Worth doing
      only if comprehension-over-host-map shows up hot.
      Files: `runtime/cel_runtime.c:1123-1140`, host
      trampoline registration in `eval/host/...`.
      Surfaced: 2026-06-03 runtime optimization review.
      Severity: P2 — cleanup-when-touched; not a current
      regression source.

- [ ] **#30** — `cel_value_eq_polymorphic`
      (`runtime/cel_runtime.c:539-572`) is an open-coded ladder
      of kind-pair `if`s.  Each comparison evaluates `a->kind`
      and `b->kind` separately — two loads (already in
      registers) but the kind-pair is re-derived per rung
      rather than once via the `(ka<<8)|kb` shape that
      `numeric_compare_kernel` uses
      (`cel_compare.c:155-188`).  Converting to a single
      `switch` on `numeric_kind_pair(ka, kb)` would let
      clang lower to a single jump table.
      Why a win: hot path on every cross-kind equality —
      every `cel_value_eq` call from `cel_list_in_arena` /
      `cel_list_eq_arena` / `cel_map_eq_arena`.  Replacing
      4-5 sequential `if`s with one table dispatch shortens
      the per-element cost from ~12 wasm instructions to ~4.
      Concrete shape: same pattern as
      `numeric_compare_kernel`'s switch.
      Risk: the ladder's structure encodes some "either kind
      is numeric → numeric_compare_kernel" semantics
      (`is_numeric_kind(a->kind) && is_numeric_kind(b->kind)`)
      — that ladder rung doesn't slot cleanly into a
      kind-pair switch.  Two-stage: kind-pair switch for
      same-kind same-payload arms, fall through to numeric/
      map_keys_equal.
      Files: `runtime/cel_runtime.c:539-572`.
      Surfaced: 2026-06-03 runtime optimization review.
      Severity: P2 — meaningful only when #21 (per-kind
      specialization) doesn't apply (i.e. dyn lists).

- [ ] **#31** — Host-fn `ErrorPayload.message` text does not
      survive the wasm round-trip: a callback returning
      `Value::Error({code, message})` (or a context callback
      using `ReturnError`) reaches the final decoded Value as
      a synthesized `"runtime error code N"` string — only the
      code crosses the boundary; the free-text message is
      dropped at encode time.  Pinned by the documented output
      of `examples/08_function_errors_and_unknowns.cc` (the
      smoke test asserts the current lossy behavior so the fix
      will flip an assertion).  The existing e2e cases
      (`HostFnTest.TypedLambdaReturnsErrorValue`,
      `ContextReturnErrorPropagates`) assert only the kind,
      which is why this went unnoticed.
      Why a win: error UX is policy-engine table stakes; the
      author's message is the actionable part.
      Fix shape: carry the message bytes through the error
      payload encode (arena string, like unknown attrs) and
      surface them in `abi_decode` / Value decode.
      Files: `eval/host_call_context.cc` (encode side),
      `eval/instance.cc` / decode path, `runtime/cel_data.h`
      wire shape.
      Surfaced: 2026-06-09 production-readiness review.
      Severity: P1 — fix before promoting the repo.

- [ ] **#32** — `Engine::AddComponent` (public API, eval/engine.h)
      takes `const FunctionLibrary&`, but
      `//compiler/celfn:function_library` is `//:internal` —
      a public method whose parameter type an external consumer
      cannot legally depend on.  Surfaced when
      `//examples:09_component_functions` (public-API-only by
      design) needed `//examples/...` added to the `//:internal`
      package_group to compile.  Either promote a curated
      function-library surface to public or give Engine an
      overload taking the `.celfn` decl source directly
      (mirroring the new `BindFunction` shape).
      Surfaced: 2026-06-09 production-readiness review.
      Severity: P1 — public-surface coherence; blocks a real
      external embedder from using components.


## Runtime optimization review — 2026-06-03 summary

Top three of the review (#27, #20, #26) shipped 2026-06-03 — see
"## Closed" entries.  Remaining queue:

Secondary cluster: #21 (per-element-kind list_in specialization
amortises after #20 lands), #24 (drop the unconditional memset
in arena_alloc), #25 (skip align_up_8 for static-size allocs).
#22 (O(N²) map_eq) and #30 (kind-pair switch in
cel_value_eq_polymorphic) are workload-dependent — defer until
a benchmark surfaces them.  #23 and #28 are
disassembly-confirmation-gated speculative wins.  #29 is
follow-up cleanup, not a current regression.

## Closed

- [x] **#13** — the "`Instance::PartialEval` SEGFAULTs on a bound
      container-of-message" report was NOT a runtime bug — it was a
      use-after-free in the test fixtures.  `Value::Message(const
      Message&)` holds a NON-owning pointer to the proto (the host owns
      the message for the Eval's lifetime; see `Value::Message` in
      `eval/internal/cel_host.cc`).  The
      `ListOfMessagePartialEvalTest` / `MapOfListOfMessagePartialEvalTest`
      fixtures built the `Customer` messages as stack locals inside a
      `BoundList()`/`BoundMap()` helper and returned the `Activation`
      by value — so the messages were destroyed when the helper
      returned, leaving the bound `ProtoBacking` pointers dangling.
      `ReadField` then dereferenced freed memory and jumped to garbage.
      It looked PartialEval-specific only because the matrix's message
      cases used that helper while `m4_test`'s message case keeps
      `c0`/`c1` in test-body scope; plain `Eval` through the same
      helper crashes identically.  Fix: hoist the bound messages to
      fixture members so they outlive every Eval (commit message cites
      this entry).  The 4 GTEST_SKIPs are removed and the cases pass —
      the container-root reads stay CONCRETE as the file header
      documents.  Surfaced + fixed: 2026-05-25 partial-eval matrix work.
      Files: `e2e/m2_partial_eval_test.cc`.

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

- [x] **#27** — `cel_memcpy_internal_` / `cel_memset_internal_`
      byte-loop fallbacks in `runtime/cel_internal.h` deleted; the
      `#ifdef __wasm__ / #else / #endif` block at lines 70-86 is
      replaced with an unconditional `#include <string.h>` at the
      top of the header.  Every TU that referenced `memcpy` /
      `memset` (`cel_arena.c`, `cel_make.c`, `cel_string_ops.c`)
      already includes `cel_internal.h` and now picks up real
      libc symbols on both host and wasi-sdk wasm builds — call
      sites needed no edits because the prior names were
      `#define` macros aliasing the now-removed internals.
      `grep cel_memcpy_internal_` confirms no surviving references.
      Resolved: 2026-06-03 in working tree pending commit.
      Files: `runtime/cel_internal.h`.

- [x] **#26** — `arena_alloc` bounds check at
      `runtime/cel_arena.c:85` rewritten from
      `g_arena.cursor + need > g_arena.capacity` (additive form
      that wraps when `need` approaches `UINT32_MAX`) to
      `need > g_arena.capacity - g_arena.cursor` (subtraction
      form, non-wrapping because the `cursor <= capacity`
      invariant guarantees the subtraction stays in-range).
      Regression test `ArenaTest.OverflowingAllocRequestRejected`
      in `runtime/cel_arena_test.cc` pins the fix by exercising
      `n = UINT32_MAX - 7` against a small cursor: the old
      additive form silently admitted the alloc; the subtraction
      form rejects with the absent-sentinel and leaves the
      cursor untouched.
      Resolved: 2026-06-03 in working tree pending commit.
      Files: `runtime/cel_arena.c`, `runtime/cel_arena_test.cc`.

- [x] **#20** — `spans_equal` and the sibling pointer-form
      `span_eq` / `span_match_at` (in `runtime/cel_string_ops.c`)
      plus the open-coded byte loop in `type_eq_at_vv`
      (`runtime/cel_runtime.c`) all funnelled through a per-byte
      `i32.load8_u; i32.ne; br_if` loop.  Option A landed: a new
      shared `cel_byteptr_equal_(const uint8_t*, const uint8_t*,
      uint32_t)` helper in `runtime/cel_internal.h` processes 8
      bytes at a time via `__builtin_memcmp(p, q, 8)` (which on
      wasi-sdk libc + LTO lowers to two `i64.load` + `i64.eq`)
      plus a 0–7 byte tail loop, and the three call sites
      (`spans_equal`, `span_eq`, `span_match_at`, `type_eq_at_vv`)
      now delegate to it.  `span_lt` is left as a byte loop on
      purpose — it's a lex comparator, and the first-mismatch
      ordering is what matters there, not a wide equality test.
      Public API of `spans_equal(CelSpan, CelSpan)` unchanged.
      No `-msimd128` flip; portable to every wasm engine.
      Expected impact on the cited regression (102 µs literal-
      list bound-in path vs 6.49 µs bound-list at 1k 50-byte
      strings) is ~16× — bench not run per the user's
      instruction.
      Resolved: 2026-06-03 in working tree pending commit.
      Files: `runtime/cel_internal.h`, `runtime/cel_string_ops.c`,
      `runtime/cel_runtime.c`.
