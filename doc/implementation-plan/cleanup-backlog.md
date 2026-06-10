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

- [ ] **#43** — true-e2e coverage gap for `cel_component.cc`'s
      malformed-`wasmtime_component_val_t` NULL guards (closes
      out gap left when #37 shipped).  Today's coverage:
      `eval/internal/cel_component_test.cc` exercises
      `LowerString` / `LowerList` / `LowerBytes` /
      `DecodeSecondsNanosRecord` directly with hand-built
      `{size > 0, data == nullptr}` vals.  Missing: an e2e that
      drives the malformed val THROUGH the public
      `Engine::AddComponent` → `Eval` path.  No clean public-API
      shape produces it — the wasmtime canonical-ABI always
      allocates `data` when `size > 0`, so a real component
      cannot return the malformed shape; the NULL guards are
      defense-in-depth against a buggy / future wasmtime build
      or a vendored-patch-introduced regression.

      2026-06-06 follow-up — burndown gap 3 e2e attempt
      descoped after architecture review.  Findings:

      * The Lower call site lives at `eval/engine.cc:706`
        (`ComponentCallbackTrampoline`).  The val being lowered
        is the `result_val` written by
        `wasmtime_component_func_call` at engine.cc:693 — there
        is NO public seam between those two statements for a
        test to inject a malformed val.
      * The host-callback path proposed as "Option A" in the
        gap brief (a test-only host callback that hands wasmtime
        a malformed val for a list-returning fn) does NOT apply
        to our architecture: host-side callbacks here are
        component IMPORTS (e.g. `RandomGetBytesStub` at
        engine.cc:731 — wasi:random the component CONSUMES),
        and the wasmtime canonical-ABI Lift on the host→component
        edge owns shape validation on its side.  The Lower path
        is exclusively driven by component-RETURNED vals from
        `wasmtime_component_func_call`.  No CEL expression we
        can author produces a Lower invocation against a val
        we control.
      * The "Option B" public AddComponent boundary does not
        admit a `wasmtime_component_val_t` directly — it takes
        `Span<const uint8_t>` component bytes plus a
        `FunctionLibrary`.  No test seam exists today.
      * `LowerComponentToCel` IS a public symbol on the
        `:cel_component` cc_library (per
        `eval/internal/cel_component.h:119`), so the existing
        unit tests at `cel_component_test.cc:1461-1517` already
        test through a public API surface — just not the
        `Engine::AddComponent → Instance::Eval` surface.

      Concrete unlock paths that remain (each requires production
      or vendored-dep changes — out of scope for this gap closure):

      (a) Add an `Engine::Builder::SetComponentReturnInterceptor`
          test seam that hooks `result_val` mutation between
          `wasmtime_component_func_call` and `LowerComponentToCel`
          in `ComponentCallbackTrampoline`.  Build-config-gated
          (`#ifndef NDEBUG` or an `--//eval:component_test_seam`
          flag) so it cannot leak into prod.

      (b) Vendor a debug knob into `third_party/wasmtime` that
          opts a designated `cel:test/*` interface into producing
          malformed canonical-ABI output, then write the e2e
          against that.  Requires upstream-style patch + matching
          test fixture component.

      (c) When `wasmtime_component_val_t` is replaced by a host-
          owned representation in a future wasmtime API revision,
          revisit — the trust boundary moves and the e2e may
          become wireable without (a) or (b).

      Decision: leave open as P2.  Existing 4 unit tests
      (`StringMalformedSizeWithNullData`, `BytesMalformedSizeWithNullData`,
      `ListMalformedSizeWithNullData`, `DurationMalformedRecordWithNullData`)
      pin the guard correctness; the timestamp guard shares
      `DecodeSecondsNanosRecord` with duration so is covered
      transitively.  `StringEmptyWithNullDataOk` pins the
      benign-shape positive path.

      Surfaced: 2026-06-06 coverage-gap closeout for commit
      598c7f2b.
      Files (probable): `eval/internal/cel_component.cc`
      (LowerString/LowerList/LowerBytes/DecodeSecondsNanosRecord),
      `eval/engine.cc` (component dispatch trampoline that owns
      the val between `wasmtime_component_func_call` and `Lower…`),
      `e2e/foreign_component_dispatch_test.cc` (probable home for
      the new e2e).
      Why P2: the unit-level coverage in `cel_component_test.cc`
      already pins the guard correctness; the e2e gap is
      hardening against an attack-surface shift (buggy wasmtime
      ↔ host trust boundary).  No known production input reaches
      the malformed shape today.

- [x] **#42** — CLOSED 2026-06-06.  Trampoline-level e2e for
      `WasmtimeMemoryView` bounds check landed at
      `eval/internal/host_trampoline_bounds_test.cc` (5 cases —
      canonical .wat assembles, 3 OOB probes, 1 in-bounds positive
      control).  Hand-authored fixture
      `doc/implementation-plan/rewrite/wat/68_ReadSpanOobInTrampoline.wat`
      + walkthrough in `wat-traces.md` §68.  Drives the full
      `Engine::Plan` → `HostCallbackTrampoline` →
      `WasmtimeMemoryView::ReadSpan` path with an adversarial
      `payload.s.ptr=0xFFFFFFFF`; captures the lifted `string_view`
      via the callback's closure and asserts it is empty.  Test has
      teeth: verified by temporarily bypassing `IsInBounds` in
      `ReadSpan` — produces SIGSEGV on the max-ptr case, restoring
      the check returns it to green.

      Original description (preserved for trail):
      Trampoline-level e2e for `WasmtimeMemoryView`
      bounds check (closes out gap left when #36 shipped).
      Today's coverage: `eval/internal/wasmtime_memory_view_e2e_test.cc`
      runs `ReadSpan` / `ReadCelValue` / `WriteCelValue` / `WriteU32`
      directly against a real `wasmtime_sharedmemory_t` (6 cases),
      plus the fake-primitive matrix in
      `eval/internal/memory_view_bounds_test.cc`.  Missing: a test
      that exercises the bounds check through the FULL host-fn
      trampoline path — a wasm module imports a `@host` function,
      that function receives a CelValue arg whose
      `payload.s.ptr = 0xFFFFFFFF`, and the trampoline returns
      cleanly (the host-side string is empty, the kernel sees
      `kError` or a clean propagation) instead of crashing or
      reading host memory.  The shape needs a hand-authored
      `.wat` module that stages a corrupted CelValue slot in
      linear memory, then calls a host-imported fn with that
      slot — naturally-compiled wasm won't emit a bad ptr, so the
      `Compiler::Compile()` → `Engine::AddTypedFunction()` →
      `Engine::Plan()` → `Instance::Eval()` test surface used by
      `e2e/host_fn_test.cc` can't reach the attack surface.
      Plausible shape: a sibling to
      `runtime/cel_runtime_wasm_test.cc`'s direct-wasmtime
      harness that loads a small hand-authored `.wat`
      registering a host fn whose trampoline body asserts
      `ReadSpan` returned empty.  Estimate >2 hours per the
      coverage-gap close instructions.
      Surfaced: 2026-06-06 coverage-gap closeout for commit
      598c7f2b.
      Files (probable): a new `e2e/` test (or extension of
      `eval/internal/wasmtime_memory_view_e2e_test.cc` with a
      hand-authored .wat fixture), and the host-fn trampoline
      glue in `eval/engine.cc` / `eval/internal/cel_host_wasmtime.cc`.
      Why P2: the unit-level + raw-shared-memory tests already
      pin the bounds-check correctness against the production
      `WasmtimeMemoryView`; the trampoline-level test is
      defense-in-depth proving the same view is used (not a
      bypass) by the trampoline code path.  No known production
      input produces the attack ptr today.

- [ ] **#41** — `optional.ofNonZeroValue(message)` overload causes
      a wasm trap.  Conformance row
      `optionals/optional_ofNonZeroValue_struct_optional_ofNonZeroValue_map_optindex_field`:
      `optional.ofNonZeroValue(TestAllTypes{?single_double_wrapper:
      optional.ofNonZeroValue(0.0)}).hasValue()` should evaluate to
      `false` (the outer optional is empty because the inner
      struct has no set fields after the `0.0` is pruned via
      `?single_double_wrapper: optional.ofNonZeroValue(0.0)`).
      Currently traps: `INTERNAL: Eval trapped: error while
      executing at wasm backtrace`.  The likely root is the
      message-typed `ofNonZeroValue` overload — scalar variants
      ship, but the message arm hits an unwired codegen / runtime
      path.
      Surfaced: 2026-06-05 conformance burndown Group K-6 triage.
      Files (probable): `compiler/codegen/expr_lower.cc`'s
      optionals dispatch + `runtime/cel_optional.c`'s
      `cel_optional_of_non_zero_value` variant matrix.
      Why P2: 1 corpus row; needs a focused expr_lower + runtime
      slice to add the message arm.

- [ ] **#40** — proto2 extension field support — PARTIAL FIX
      2026-06-05.  The operator-form (`msg.`fqn``) extension
      reads + has now work for scalar / message / enum / repeated
      extensions via `Reflection::FindKnownExtensionByName`
      fallback in `ResolveFieldDescriptor`.  Closes 16 of 18
      rows under `proto2/extensions_has/*` + `proto2/extensions_get/*`.
      Remaining 2 fails (`extensions_get/package_scoped_repeated_test_all_types`
      + `extensions_get/message_scoped_repeated_test_all_types`)
      compare an extension repeated-message list (HostList from
      ProtoList) for equality with a literal-constructed list
      `[Msg{...}, Msg{...}]` and return `false` — a list-equality
      surface separate from the descriptor look-up.  The
      `proto2_ext.textproto` file (18 rows) still SKIPs as
      `ext_unimpl` because its expressions use the
      `proto.hasExt(msg, ext)` / `proto.getExt(msg, ext)`
      function form, which we don't register.
      Surfaced: 2026-06-05 conformance burndown Group C.
      Files touched (partial fix):
      `eval/internal/cel_host.cc::ResolveFieldDescriptor` +
      `eval/internal/cel_host_test.cc` (4 new
      `ProtoBackingExtensionTest` cases).
      Remaining files: a list-equality surface between
      HostList-from-extension and arena/constructed lists of
      messages (see `CelListEqImpl` if/when it exists, parallel
      to the documented `CelMapEqImpl` materialisation strategy);
      plus the proto.hasExt / proto.getExt function-form registration.
      Why P2: spec compliance gap, but the remaining 2 rows need a
      list-eq materialisation pass shared with the mixed-origin
      map-eq gap.

- [ ] **#39** — strong-typed enum support
      (cel-spec issues/119, "Future features for CEL 1.0").
      cel-cpp's own conformance harness skips
      `enums/strong_proto2` and `enums/strong_proto3` for the
      same reason — neither implementation has shipped the
      type-tracking + int→enum-constructor pieces.  Conformance
      rows affected (now SKIP as `spec_unimpl` via
      `IsSpecUnimplSection`): 18 across the two sections.
      Surfaced: 2026-06-05 conformance burndown Group B.
      Files (probable): `compiler/frontend/parse_and_check.cc`
      (type-of operator must surface the fully-qualified enum
      type name when the operand is enum-typed; enum names
      must register as callable int→enum / string→enum
      converters in the checker), plus runtime support for
      the typed enum values.
      Why P2: spec acknowledged it's unimplemented; the
      reference impl doesn't pass these either.  Genuine
      feature work, not a regression.

- [x] **#38** — FIXED 2026-06-06.  Replaced the hand-rolled
      `frac *= 10` formatter in `cel_convert.c` with
      `std::to_chars(buf, end, v, std::chars_format::general)`
      from libc++ in a sibling C++ TU
      (`runtime/cel_convert_double_format.cc`), mirroring
      cel-cpp's `FormatDouble`
      (`third_party/cel-cpp/runtime/standard/type_conversion_functions.cc:56-75`).
      Confirmed empirically against the wasi-sdk libc++:
      `123.456` → `"123.456"`, `-987.654` → `"-987.654"`,
      `6.02214e23` → `"6.02214e+23"`, `0.1` → `"0.1"`,
      `1.0/3.0` → `"0.3333333333333333"`, `1e10` → `"1e+10"`.
      Conformance: 1965 → 1966 (`conversions/string/double` flips
      green); other `string/<num>` rows stay green.  Two e2e
      regression pins in `e2e/known_bugs_test.cc`
      (`DoubleToStringShortestRoundTrip`, `DoubleToStringExponentForm`)
      are un-skipped.  New unit test matrix in
      `runtime/cel_convert_test.cc` covers the corpus rows + the
      `0.1` / `1/3` non-representable cases + `min()` / `max()` /
      `denorm_min()` boundary doubles, asserting both literal
      shortest-form pin and `strtod(s) == input` round-trip
      safety (the durability invariant).
      Original entry preserved below for the trail:
      `cel_double_to_string_at_v` (`runtime/cel_convert.c`)
      uses a per-digit `frac *= 10` chain in `append_double_fraction`
      that accumulates rounding error past ~6 fractional digits.
      cel-cpp's `absl::StrCat(double)` uses a correctly-rounded
      shortest-representation algorithm (Grisu / Ryu shape).  Symptom:
      `string(123.456)` returns `"123.45600000000000306"` instead of
      `"123.456"`.  Conformance row:
      `conversions/string/double` (1 row — only the simplest case;
      other `string/<num>` rows pass because integers/binary-exact
      doubles round-trip cleanly).
      Surfaced: 2026-06-05 conformance burndown Group E.
      Files: `runtime/cel_convert.c::append_double_fraction`,
      `double_to_string_mixed`, `double_to_string_scientific`.
      Why P2: 1 corpus row; only embedders that read
      `string(non-integer-double)` and string-diff the result see the
      bug.  Correct fix is to vendor / write a Grisu (Steele) or Ryu
      (Adams) printer — ~300 LOC of careful float math, deferred to a
      runtime-quality pass.

- [x] **#37** — FIXED 2026-06-05.  Added NULL-data guards
      to `LowerString` (`cel_component.cc:~276`),
      `LowerList` (`:~661`), `LowerBytes` (`:~681`), and
      `DecodeSecondsNanosRecord` (`:~301`).  Each returns
      `InvalidArgument` instead of dereferencing null when
      `size > 0 && data == nullptr`.  Regression pin: 5
      cases in `cel_component_test.cc::LowerComponentToCel`
      (string/bytes/list/duration malformed +
      string-empty-with-null-data benign).  Original entry
      preserved below for the trail:
      Component-model Lift/Lower paths
      dereference `wasmtime_component_val_t.of.{record,list,
      string}.data` without verifying the pointer is
      non-null or the buffer has `size` elements.  A
      malformed component value with `record.data = nullptr,
      record.size = 2` crashes at
      `eval/internal/cel_component.cc:302` (NULL deref in
      `DecodeSecondsNanosRecord`'s for-loop).  Same shape
      in `LowerList` (`eval/internal/cel_component.cc:661-
      668`), `LowerBytes` (`eval/internal/cel_component.cc:
      681-682`), and `Value::String(in.of.string.data,
      in.of.string.size)` at line 276.
      Severity: crash / OOB read.  Reachable from a buggy
      or malicious wasm-component, NOT from a buggy wasm
      module — wasmtime's component-model layer is the
      one writing these structs.  Likely safe in practice
      against bugs (wasmtime owns this struct lifetime),
      but unaudited against a malicious/compromised
      wasmtime build.
      Files: `eval/internal/cel_component.cc:276`,
      `:302`, `:661-668`, `:681-682`.  Mitigation: at
      each `data`/`size` site, `ABSL_CHECK(in.of.X.data !=
      nullptr || in.of.X.size == 0)` before the loop; the
      size invariant is a wasmtime contract so the CHECK
      is defensive, not a real validator.
      Why P2: depends on wasmtime upholding its own
      struct invariants — the practical reachable bug
      surface is small.  Files a defensive CHECK,
      doesn't change the contract.
      Surfaced: 2026-06-05 audit triggered by the PBT
      hunt; reported in detail by the Explore agent.

- [x] **#36** — FIXED 2026-06-05.  `MemoryView` now
      exposes `uint32_t Size() const` and a default
      `IsInBounds(ptr, len)` helper that uses
      overflow-safe arithmetic (`len <= Size() - ptr`,
      not `ptr + len <= Size()`).  `WasmtimeMemoryView`
      implements `Size()` via
      `wasmtime_sharedmemory_data_size`; both production
      (`eval/internal/cel_host_wasmtime.h`) and test
      (`eval/internal/cel_host_test_fakes.h`) impls now
      bounds-check `ReadCelValue` / `WriteCelValue` /
      `WriteU32` / `ReadSpan` before touching memory.
      On OOB: reads return zero / empty, writes are
      no-ops, no memory disclosure, no process crash.
      The interface comment documents the contract in
      detail (cleanup-backlog #36 reference embedded).
      Regression pin:
      `//eval:memory_view_bounds_test` — 13 cases
      covering empty range, exact boundary, off-by-one,
      `ptr=0xFFFFFFFF`, u32 wrap (`ptr=0x80000000,
      len=0x80000000`), and "OOB write does not corrupt
      a known in-bounds sentinel."  97/97 full sweep
      green post-change — no production caller relied on
      the unchecked behaviour.
      Original entry preserved below for the trail:
      `WasmtimeMemoryView::ReadSpan` returns
      `{Data() + ptr, len}` with NO bounds check against
      the linear memory's size — see
      `eval/internal/cel_host_wasmtime.h:120-122`.  The
      class-level comment at line 103 acknowledges
      "a possible future bounds-checked read" as deferred
      work; this entry surfaces it so the deferral has a
      tracking handle.  Same gap in `ReadCelValue` (line
      109-113) and `WriteCelValue` / `WriteU32` (lines
      114-119).
      Trigger: a malicious or buggy wasm module passes a
      CelValue arg with `payload.s.ptr = 0xFFFFFFFF,
      payload.s.len = N` (or any ptr+len that exceeds
      memory).  The host trampoline lifts the string via
      `mem.ReadSpan(0xFFFFFFFF, N)` →
      `Data() + 0xFFFFFFFF` → pointer arithmetic into
      the wasmtime memory's virtual reservation or past
      it (depending on wasmtime's memory model and the
      module's max-pages config) → either SIGSEGV (the
      benign case) or returns a `string_view` over host
      memory adjacent to the wasm reservation (the
      memory-disclosure case).
      Severity: depends on wasmtime's memory layout.
      Wasmtime typically reserves 4 GiB virtual + guard
      pages, so most OOB ptrs hit the guard and SIGSEGV.
      The narrow-but-real exploit window is when ptr+len
      wraps to an in-reservation byte that contains
      another module's state or host data — possible
      under shared-memory / multi-module configs.
      Files: `eval/internal/cel_host_wasmtime.h:102-122`.
      Mitigation: `MemoryView::Size()` virtual member;
      `ReadSpan` checks `if (ptr > Size() || len > Size()
      - ptr) return ""` (or returns
      `absl::StatusOr<string_view>` and the trampoline
      poisons the call with kHostAdapterError on OOB).
      Add `ReadSpanBoundedTest` covering ptr at the
      memory boundary, ptr past memory, ptr+len wrap.
      Why P2: hot path; adding a check costs one
      comparison per host call.  Sketchy under
      adversarial wasm but no concrete in-tree caller
      ships a malicious module today.  This is the
      class of bug user flagged 2026-06-05 ("we are
      missing bounds checks ... in custom functions").
      Surfaced: 2026-06-05 audit.

- [x] **#35** — FIXED 2026-06-05 (as a side-effect of #34
      landing).  The audit's original "lazy-copy" hypothesis
      was wrong: proto field reads ARE eager-arena-copy at the
      wire transition via `EncodeSpan`
      (`cel_host.cc:737-744`).  Pre-#34, this meant any
      proto field >64 KiB poisoned the Eval with arena OOM;
      post-#34, the chained-grow arena absorbs the copy
      transparently and the Eval succeeds.  Doc updated:
      `eval/internal/cel_host.h::ProtoBacking` now documents
      the actual eager-copy behaviour + DoS implications +
      grow-on-demand mitigation.  Regression pin:
      `//e2e:proto_arena_lazy_copy_test` — 3 cases
      (`size()` over a 70 KiB field, `(field + "")`, normal-
      sized sanity).  Original entry preserved below:
      Proto field reads materialise
      string/bytes into the per-Eval bump arena via
      `alloc.Alloc(s.size())` at
      `eval/internal/cel_host.cc:737-751` (`EncodeSpan`),
      with NO upstream cap on the proto field's size.
      `GetStringReference()` at line 515 can return an
      arbitrarily large string (proto field is host-
      bound, attacker-controlled in the threat model
      where a host passes an untrusted proto to Eval).
      The arena is fixed at 64 KiB (`runtime/cel_layout.
      h`); a 100 MB string field triggers
      `arena_alloc()` returning 0 (`runtime/cel_arena.c:
      89`) → host caller in `EncodeSpan` (line 744-747)
      returns a `ResourceExhaustedError` and the
      enclosing Eval poisons to `kError`.  Reasonable
      surface — but not all callers handle the 0-offset
      return path cleanly, and the cleanup-backlog #17 /
      #21 / #34 family covers what happens when proto-
      reads OR the runtime's own list scans drain the
      arena.
      The deeper issue user flagged: the path that
      copies proto BYTES into the arena (vs the path
      that holds a host-side std::string and only
      arena-allocates for the wire form) is
      asymmetric — the host-side `ProtoBacking::
      ReadField` (`eval/internal/cel_host.cc:513-519`)
      returns a heap-backed `Value::String` whose
      backing is owned by the proto's internal buffer;
      no arena copy yet.  The arena copy happens only
      when the value crosses back into the expr module
      (via `EncodeSpan`).  So a wasm expression that
      reads a 100 MB proto field via `msg.huge_field`
      AND then lifts it into wasm memory triggers the
      arena overflow at the wire transition, not at the
      proto read.  This is the correct order of
      operations but isn't documented or load-bearing —
      a future refactor that arena-copies eagerly would
      break this assumption silently.
      Files: `eval/internal/cel_host.cc:513-519`,
      `:737-751`; `runtime/cel_arena.c:75-89`.
      Mitigation: document the "arena copy is lazy /
      only at wire transition" invariant in
      `eval/internal/cel_host.h`'s `ProtoBacking`
      contract; add a regression test that reads a
      proto field larger than the arena and verifies
      the host receives a clean `ResourceExhausted`
      error rather than a corrupted truncated value.
      Why P2: 64 KiB arena limit is overdue for a
      grow-on-demand redesign (#17); a clean fix for
      that subsumes this entry.  Documentation gap is
      the bigger smell — a future refactor could break
      the lazy-copy invariant without anything in tree
      to catch it.
      Surfaced: 2026-06-05 audit triggered by user
      flag ("bounds checks in copying stuff from
      protos into the arena").

- [x] **#34** — FIXED 2026-06-05.  Replaced the fixed-cap
      arena in `runtime/cel_arena.c` with a chained-chunk
      grow-on-demand arena.  When the current chunk
      overflows, `arena_alloc` malloc's a fresh chunk
      sized `pick_grow_size(prev_capacity, at_least_bytes)`
      (doubles each grow, clamped to
      `[CEL_ARENA_MIN_GROW_BYTES=4 KiB,
      CEL_ARENA_MAX_GROW_BYTES=1 MiB]`, with a floor so a
      single huge alloc gets a chunk sized to fit it).
      `arena_reset` frees every chunk except the first to
      avoid per-Eval malloc churn for embedders whose
      initial sizing is sufficient.  Native build does NOT
      chain — there's no shared memory to extend into, so
      OOM is still terminal there (test-only path).
      `arena_capacity()` now reports the running total
      across the chain.  Closes the `#36` true-e2e
      precondition (the proto-arena overflow was the
      cleanest way to demonstrate the cap, and now it
      doesn't fire).  Regression pin:
      `//runtime:cel_runtime_wasm_test::
      ArenaGrowsOnDemandWhenInitialCapacityExceeded` +
      `//e2e:proto_arena_lazy_copy_test` (all 3 cases).
      Full sweep 99/99 green post-change.  Original entry
      preserved below:
      Nested comprehension × `_in_` over a
      multi-element haystack exhausts the per-Eval arena and
      poisons the result with `code=10 msg="overflow"` (i.e.
      `kError`).  Surfaced 2026-06-05 by the Slice C PBT at
      depths 7-8 across the entire scalar matrix
      (string/bool/int/double + list_int/list_string/
      map_string_int divergences at seed=230 / seed=185 /
      seed=246 / seed=57).  Reduced repro:

      ```
      ["a","b","c","d","e","f","g","h","i","j"].exists_one(v,
        [1,2].exists(v,
          [b"a",...,b"j"].exists_one(v,
            [-3,-2,-1,0,1,2,3].exists(v, (1 in [10,20])))
        ) ? false : true) ? "hello" : "x"
      ```

      4-deep nested comp × `_in_ [10,20]` at the innermost
      predicate → `kError "overflow"`.  Replace the `(1 in
      [10,20])` with literal `false` → returns `"x"`
      correctly.  Replace the haystack with `[10]` (single
      element) → also returns `"x"`.  The trigger is the
      combination of comp-iteration count × per-iter
      `cel_list_in` arena allocation: with outer 10 × inner 2
      × inner 10 × inner 7 = 1400 deepest iterations, each
      allocating ~24-48 B of arena state for the `_in_`
      probe, total ~50-67 KB — over the 64 KB fixed arena
      cap (`runtime/cel_layout.h`).
      Files: `runtime/cel_runtime.c` (the `cel_list_in`
      family — same surface flagged by cleanup-backlog #17
      and #21).  This entry is the PBT-measured variant of
      that family: the PBT shows it isn't only the 10K
      "huge bound list" case #17 cites — it's reliably
      reachable from depth-7 grammar-generated expressions
      with humble haystack sizes (2-10 elements).
      Manifestations also include partial corruption ("one
      element survived a filter that should've returned
      []") when the overflow happens mid-iteration rather
      than poisoning the whole result; same root cause,
      different surface symptom.
      Why P2: only the runtime grow-on-demand arena
      (cleanup-backlog #17 fix surface) closes this
      cleanly; the depth-≤6 PBT runs at 12,000 programs
      stay clean, so the bug is bounded to "deep nested
      comprehension OR very wide haystack."  Pinning test:
      `e2e/known_bugs_test::PbtNestedCompInArenaOverflow`
      (assertion is `v.kind() == kBytes / "x"`, will be
      live once arena grow ships).
      Surfaced: 2026-06-05 Slice C PBT discovery at
      depth 7-8 (post-#32 fix unblocked deep aggregate
      composition).

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

- [x] **#9** — FIXED 2026-06-05.  `EmitKSelect`
      (`compiler/codegen/expr_lower.cc`) gained a `kMap`-operand
      branch that emits a `cel_map_lookup` (value form) or
      `cel_map_in` (test_only / `has()` form) call with the field
      name lifted to rodata as a CEL_STRING CelValue by
      `SelectKeyRodataVisitor` (`layout_pass.cc`).  The dispatch
      unconditionally uses the kDynamic dispatcher (not
      `MapLookupCallTarget(map_origin)`) — ResolvePass's
      `MapOriginVisitor` stamps `kHost` on every map-typed kSelect,
      but a nested selector (`{'c': {...}}.c.d` or
      `{'c': {...}}.c['k']`) produces a CEL_MAP_ARENA value at
      runtime that the host trampoline rejects.  The kDynamic
      path's runtime kind-branch routes correctly at any nesting
      depth.  EmitKIndexCall's `_[_]` arm got the same fall-through
      (operand is a kSelectExpr → force kDynamic).  Added
      `cel_map_in_arena` / `cel_map_in` / `cel_host.cel_map_in`
      imports in `compile.cc`; added `// cel:codegen-export`
      marker on `cel_map_in_arena` in `runtime/cel_map.h` so the
      catalogue genrule exposes it.  Regression pin:
      `KnownBugs.MapFieldSelectSugar` + `MapDotFieldNestedMapValue`
      + `MapDotFieldThenIndex` + `MapDotFieldBacktickQuotedSlash` +
      `MapDotFieldBacktickQuotedDot`; un-skipped existing
      `HasOnMapPresentKey` / `HasOnMapAbsentKey` /
      `CelBindSelectorOnBoundVar` / `ComprehensionVarSelector` /
      `ReservedWordMapSelector`.  Conformance: 29 rows flipped (17
      `parse/selectors` reserved-word rows + 4 `quoted_map_fields`
      / 2 `map_has` rows + 3 `optional_chaining` rows + 1
      `bind/shadowing_namespace_resolution_selector` + 2
      `namespace_shadowing` selector rows).
      Original entry preserved below for the trail:
      `{'k': 'v'}.k` (map-dot-field sugar) is broken
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

- [x] **#18** — FIXED 2026-06-05.  `EncodeStringOrBytes` in
      `eval/instance.cc` now calls a new `TryReadWktStringWrapperValue`
      helper at the head; on hit it peels the wrapper's inner string /
      bytes via `GetStringReference` and copies into the activation
      arena via the same path the native-Value::String / Value::Bytes
      paths already use.  Symmetric with the numeric
      `TryEncodeWktWrapperMessage` peel for BoolValue / Int*Value /
      UInt*Value / Float/DoubleValue.  `TotalHostStringBytes` pre-pass
      extended to probe the wrapper-peel size so the activation buffer
      gets sized correctly.  Regression pin: conformance rows
      `dynamic/string/var`, `dynamic/bytes/var` (both flip to PASS).
      Original entry preserved below for the trail:
      `Value::Message(StringValue{value: "x"})` bound
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

<!-- Entries below were authored on the perf/ssp-fix branch; their
     #31/#32 cross-references use that branch's own numbering (the
     comprehension-storage cluster), not the host-fn-error #31 /
     AddComponent-visibility #32 immediately above.  Renumbering is
     deferred to a backlog-tidy pass. -->
- [x] **#33** — Same root cause as #32; the apparent
      "_in_ over workspace-resident list" failure was the
      DOWNSTREAM symptom of the comprehension-result slot
      being mis-stamped (cleanup-backlog #32).  Once #32 was
      fixed, the four bool/uint cases originally classified
      as #33 all pass under the 12,000-program PBT sweep.
      Surfaced + closed 2026-06-05.  Kept as a closed entry
      (rather than deleted) because the misdiagnosis is
      itself a useful artifact — the next time the PBT
      reports a "list membership wrong" shape, check whether
      the comp upstream is the actual culprit before
      diving into `cel_list_in`.  No code change attributable
      to this entry; the regression pin lives under #32's
      `KnownBugs.PbtExistsOneInTernaryCond*` tests.

      Original (incorrect) hypothesis preserved for the trail:
      `_in_` (membership) over a list whose elements
      are not all rodata-resident returns the wrong result.
      Cleanest PBT repro (`string` target, seed=49, depth=5,
      Slice C grammar) — reduces to:

      ```
      0u in [7u, u_a, 0u*0u, 7u+1u, 7u]
      ```

      with `u_a = 5u` in the Slice B activation.  Oracle: `true`
      (the third element is `0u`).  Ours: `false`.  The haystack
      list is constructed at runtime from `kCall` arithmetic
      results (`7u+0u`, `0u*0u`, `7u+1u`) and a `kIdent` load
      (`u_a`), not literals — every list element lives in a
      workspace slot, not in rodata.  The runtime's list-scan
      path (`cel_list_in` family in `runtime/cel_runtime.c`)
      almost certainly only walks rodata-resident or
      arena-resident elements; the workspace-slot case isn't
      wired in.
      The bug also fires through `.exists(k, …)` and
      `.exists_one(k, …)` over maps whose values are non-literal
      (PBT discovered four cases: `bool` seed=1, `bool` seed=27,
      `uint` seed=5 chain through the same map / list runtime
      scan); the bug is the runtime walk, not the comprehension
      lowering.
      Files: `runtime/cel_runtime.c` (`cel_list_in` and sibling
      `cel_list_in_arena` / `cel_map_lookup_*`), plus targeted
      WAT walkthrough under `doc/implementation-plan/rewrite/wat/`
      to lock the list-element addressing for workspace-resident
      elements before patching runtime.  Pinning test:
      `e2e/known_bugs_test::PbtUintInWorkspaceListMembership`.
      Why P2: fires only when the haystack list is a runtime
      construction of non-literal elements — hand-written tests
      that built lists from literals or from arena-allocated
      messages never hit it; the PBT generator's preference for
      composing `kCall`/`kIdent` into list-literal slots is what
      exposes the gap.
      Surfaced: 2026-06-05 Slice C PBT discovery (after #31 fix).

- [x] **#32** — FIXED 2026-06-05 by correcting the #31
      stamp.  Root cause was NOT in `EmitConditional` /
      `EmitGeneralCall` (the original hypothesis): the
      kind-tag was being written correctly all along.  The
      bug was that `ComprehensionLocalsVisitor::PostVisit
      Comprehension` stamped the kComprehensionExpr's
      storage with the `accu_var.slot_offset`, but for
      `exists_one` the comp's result sub-expression is
      `kCallExpr(_==_, @result, 1)` whose result lives in
      its OWN workspace slot — not in the accu slot which
      still held the Int loop counter.  Every "wrong-kind"
      / "wrong value" symptom was the downstream consumer
      reading the count Int instead of the comparison's
      Bool from the accu slot.  Fix: stamp comp storage
      from `comp.result()`'s annotation (post-visit, child
      already laid out).  For `.exists` / `.all` / `.filter`
      / `.map` the result IS `kIdent(@result)` so its
      storage points at the accu slot — same answer as
      before, by construction.  For `.exists_one` (and
      future macros whose `result` sub-expression isn't a
      bare ident) the storage now points at the correct
      slot.  Verified with a 12,000-program PBT sweep
      (depth 6, 2000 seeds × 6 target kinds): 0 value
      divergences.  Regression pin:
      `e2e/known_bugs_test::PbtExistsOneInTernaryCondBytes`
      (+ companions `…TakesThen` and
      `PbtSizeOfExistsOneTernaryBytes`).

      Original (partially incorrect) hypothesis preserved
      for the trail:
      Composite expressions whose result kind comes
      from a non-Const ternary branch or an arithmetic/`size()`
      consumer of a comprehension produce a CelValue whose
      *tag* byte is wrong (the parent reads `<wrong-kind>`),
      even though the payload bytes are correct in isolation.
      The PBT surfaced 10 of these across `int` / `uint` /
      `double` / `string` / `bytes` targets after #31 unblocked
      the comprehension storage path; the common shape is "the
      result of a ternary or comprehension feeds a `+` / `*` /
      `size()` / outer ternary, and the outer consumer reads
      the slot's kind tag without that tag ever having been
      written."  Cleanest repro (`int` target, seed=137):

      ```
      size(
        <cond> ? <bytes-typed nested ternary>
               : <bytes-typed nested ternary>
      )
      ```

      Oracle: `2` (a `kInt`).  Ours: the result CelValue is
      not even `kInt`.  Same family across `bytes` (seed=3:
      `((cond ? y_a : (y_a + b"x")) + (true ? y_a : b"x")) +
      b"x"`), `string` (seed=50, seed=56), `double` (seed=35,
      seed=37, seed=107), `uint` (seed=19, seed=46).
      Hypothesis: `EmitConditional` / `EmitGeneralCall` writes
      the *payload* of the chosen branch into the parent's
      workspace slot but does not stamp the slot's CelValue
      header (kind tag); when the parent expression is a
      consumer that reads `.kind()` to dispatch, it sees a
      stale tag from whatever was in the slot before.  Same
      seam as #58 (the Slice B kIdent / kLocal ternary fix) but
      surfacing in more positions because Slice C aggregates
      route more results through workspace slots.
      Files: `compiler/codegen/expr_lower.cc`
      (`EmitConditional`, `EmitGeneralCall`,
      `EmitCelCopySlot`); add a regression test per repro
      shape in `compiler/codegen/expr_lower_test.cc` + a
      pinning row in `e2e/known_bugs_test.cc` per concrete
      seed.
      Why P2: hidden behind a kind-tag mismatch on the
      consumer — the expression *appears* to evaluate but the
      result is the wrong CEL kind; this would silently miscompile
      a real workload but the hand-written test catalog never
      composes ternary / comp results into outer arithmetic in
      the unfortunate way the PBT does.  Pinning tests:
      `e2e/known_bugs_test::PbtSizeOfBytesTernary` (int seed=137),
      `e2e/known_bugs_test::PbtBytesTernaryPlusChain` (bytes seed=3).
      Surfaced: 2026-06-05 Slice C PBT discovery (after #31 fix
      unblocked these shapes — the 10 cases were previously masked
      because the comprehension storage CHECK fired first).

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

- [x] **#31** — `kComprehensionExpr` annotation had no
      `storage` stamped (`storage.kind == kNone`); any consumer
      that needed the comp's result CelValue address via
      `EmitSlotBaseAddress` CHECKed.  Fixed 2026-06-05 by
      extending `ComprehensionLocalsVisitor` in
      `compiler/codegen/layout_pass.cc` with a
      `PostVisitComprehension` that finds the accu_var by name
      in `layout.variables[]` and stamps the
      `kComprehensionExpr`'s annotation with
      `Storage{kWorkspaceSlot, accu_var.slot_offset}`.  Verified
      with the original ternary-over-comp repro
      (`[1,2,3].exists(v, v == 2) ? 7 : 11` returns `Int(7)`).
      Updated `LayoutPassComprehensionChildrenTest`'s six
      `EXPECT_EQ(h.none, 1)` assertions to `0` (the comp node
      no longer has kNone storage).  Discovered #32 and #33
      downstream — see Open entries.

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

- [ ] **#37** — Deep left-associative expression chains
      (`a+b+c+...`, N terms) SIGSEGV when N is large, because codegen
      emits the chain as an **N-deep nested wasm expression tree** —
      each `+` is `(call $cel_int_add (block (result i32) …
      <previous-add>))`, the prior result nested *inside* the next
      call's operand (verified by `wasm-dis` of a depth-12 chain,
      2026-06-10).  Both the host compiler and wasmtime walk that tree
      recursively, one native stack frame per nesting level, so it
      overflows the ~8 MiB native stack at depth ≈4.6k.

      History: pre-`perf/ssp-fix` the crash was at **compile** time
      (~N=4670, our `expr_lower` recursion).  The slot-allocator merge
      (Sethi-Ullman + free-list `Release`) fixed *workspace* pressure
      (live locals are now bounded ~20 regardless of N — the #16
      corruption class is gone) but did NOT flatten the *expression
      nesting*, so the crash **relocated to eval/Plan** time
      (~N=4654: 4653 compiles+evals fine, 4654 segfaults in wasmtime's
      validation/Cranelift JIT walking the deep tree).

      Two fixes, cheap→real:
        (a) interim: a COMPILE-TIME AST-depth gate returning
            `ResourceExhausted` (graceful), limit ~2000 — covers the
            1000-term bench + any realistic policy with margin, rejects
            the absurd-depth input before deep wasm is emitted. Depth
            measurement must be ITERATIVE (a recursive measure would
            itself crash). Align `parse_and_check.cc`'s
            `max_recursion_depth` (currently 16384) down to match.
        (b) real fix: FLATTEN codegen — emit each op as a top-level
            statement writing to a slot and load the slot for the next
            operand, so expression-tree depth is O(1) for any N and the
            depth limit can be dropped entirely. Localised to the
            operand-nesting in `compiler/codegen/expr_lower.cc`.

      Far beyond realistic input (nobody hand-writes a 4.6k-deep
      expression), so it did not block the ssp-fix merge — but it is a
      crash on valid CEL and pairs naturally with the m27 PBT machinery
      (a property test over expression depth finds exactly this class).
      Surfaced: 2026-06-10 hardening session.  Add an executable
      `e2e/known_bugs_test.cc` GTEST_SKIP regression pinning the
      verified N=4653/4654 boundary when the fix lands.
      Files: `compiler/codegen/expr_lower.cc`,
      `compiler/frontend/parse_and_check.cc`.
