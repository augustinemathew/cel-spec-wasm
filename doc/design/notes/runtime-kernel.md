# runtime-kernel — design notes (undefined)

Scope: `runtime/` — the C (plus a few C++) kernel sources that become
`cel_runtime.wasm`, their native-host twin used by unit tests, and the
arena/memory substrate.  Verified against code + tests on branch
`m28-configurable-linking`, 2026-06-10.

## 1. Verified architecture

### 1.1 What the component is

A language-agnostic CEL evaluation kernel, compiled twice from the same
sources:

- **wasm32-wasi-threads** `cc_binary` `cel_runtime_wasm.bin`
  (`runtime/BUILD.bazel:570-665`) — the artifact codegen-emitted expr
  modules link against.  Flags: `-mtail-call` (required by the
  `musttail` dispatchers, `BUILD.bazel:610-615`), `-flto`
  (`BUILD.bazel:619-624`), `-Wl,--global-base=8192` (reserves
  `[0, 8192)` for expr-module data segments, `BUILD.bazel:636-638`),
  `-Wl,--allow-undefined-file=wasm_imports.txt` (tight import
  allowlist, `BUILD.bazel:639-643`), export list from a generated
  response file (`BUILD.bazel:650-655`).
- **native `cc_library` `cel_runtime`** (`BUILD.bazel:96-159`,
  `-O3 -flto`) — same sources against a static 128 KiB byte buffer so
  every unit test exercises the identical layout
  (`cel_memory.c:43-63`).

A second wasm artifact, `cel_runtime_stripped_wasm_bytes`
(`BUILD.bazel:708-772`), is the m28 static-link variant: the
Binaryen-based `strip_command_wrappers` tool retargets
`X.command_export` wrappers to bare `X` and DCEs the wrapper chain;
consumed by the compiler at static-mode `Compile()`.

### 1.2 Wire ABI (`cel_data.h`)

- `CelValue` is 24 bytes: `u32 kind, u32 _pad, 16-byte payload union`
  (`cel_data.h:141-183`); `_Static_assert(sizeof == 24)` at
  `cel_data.h:183`.  Kind values are wire-stable, append-only
  (`cel_data.h:26-30`); 20 kinds, `CEL_NULL=0 … CEL_CIDR=19`
  (`cel_data.h:31-52`).
- Aggregate headers: `ArenaMapHeader` / `ArenaListHeader`, 16 bytes
  each (`cel_data.h:71-98`); strides pinned as enums:
  `kCelMapEntryStride=48`, `kCelListEntryStride=24` =
  `sizeof(CelValue)` (`cel_data.h:188-200`).
- Error codes are wire-stable numerics mirroring `celwasm::ErrorCode`
  (`cel_data.h:216-262`).
- Little-endian host is a build-time requirement (`#error` guard,
  `cel_data.h:208-210`).
- "Pointers" are u32 byte offsets into the one shared linear memory;
  offset 0 is the universal "absent" sentinel (`cel_arena.h:52-54`,
  `cel_arena.c:125-129`, test `cel_arena_test.cc:325-328`).

### 1.3 Memory model (live source of truth: `rewrite/memory-layout-design.md`)

- The runtime **defines and exports** the shared memory (observed
  `(memory 4 1024 shared)`; `memory-layout-design.md` §1).  The expr
  module imports `cel.memory`.  `wasm32-wasi-threads` was forced by
  cctz needing `<mutex>` (`wasi/DESIGN.md:9-25`).
- Three regions (`memory-layout-design.md` §2): `[0, 8192)` reserved
  for expr rodata + workspace (`CELWASM_RESERVED_LOW_MEMORY_BYTES`,
  `cel_layout.h:37`; bounded at *compile* time by LayoutPass, not by a
  runtime check); `[8192, __heap_base)` wasi-libc statics + 64 KiB
  shadow stack; `[__heap_base, …)` dlmalloc heap.
- The per-Eval **bump arena** is a single 64 KiB
  (`CELWASM_ARENA_CAPACITY_BYTES`, `cel_layout.h:41`) `malloc`'d buffer
  inside the dlmalloc heap, allocated once per Instance by
  `arena_init` (`cel_arena.c:37-73`); state (`base/capacity/cursor/
  initialized`) lives in BSS (`cel_arena.c:20-27`), NOT at fixed memory
  offsets (the pre-WASI 8/12 cursor slots are gone).
- `CELWASM_INITIAL_MEMORY_PAGES = 2` (`cel_layout.h:29`) is a host-side
  **floor** (A13 checks observed pages `>=` 2; comment
  `cel_layout.h:18-28`), not the actual page count — Phase C pushed the
  real minimum to 3-4 pages.
- **Native twin:** `g_memory[CELWASM_ARENA_CAPACITY_BYTES + 65536]` =
  128 KiB, `_Alignas(8)` (load-bearing at -O3+LTO; `cel_memory.c:51-57`).
  `arena_init` backs the arena with `g_memory + 16` and requires
  `16 + cap <= cel_memory_size_()` (`cel_arena.c:58-68`); the return
  contract is `16 + local_off` so `cel_mem_base() + off` resolves on
  both builds (`cel_arena.c:95-107`, pinned by
  `cel_arena_test.cc:176-186`).
- **wasm opacity barrier:** `cel_memory_base_()` synthesizes a byte
  pointer from offset 0 through inline asm; without it clang treats
  stores through `(uint8_t*)0 + off` as UB and elides them
  (`cel_memory.c:9-17, 31-35`).  Still flagged for re-audit in
  cleanup-backlog #4 (`wasi/DESIGN.md:61-64`).

### 1.4 Arena contract (`cel_arena.{h,c}`; DESIGN §5 invariants)

- `arena_alloc(n)`: 8-aligned, zero-initialized, returns absolute
  offset or **0 on OOM** (A10); `alloc(0)` returns a valid 8-byte slot
  (A9, `cel_arena.c:78`); bounds check uses the subtraction form so a
  near-`UINT32_MAX` request can't wrap (`cel_arena.c:85-89`, regression
  test `cel_arena_test.cc:188-216`); failing alloc leaves the cursor
  untouched (`cel_arena_test.cc:250-259`).
- `arena_alloc` **before init traps** (`cel_arena.c:79-84`);
  `arena_init` re-init with a *different* capacity traps
  (`cel_arena.c:43-47`, death tests `cel_arena_test.cc:399-408`);
  same-capacity re-init is idempotent (`cel_arena.c:43-48`, test
  `cel_arena_test.cc:355-359`).
- `arena_reset()` is O(1) cursor rewind, emitted by codegen as the
  first instruction of `$eval`; reset-before-init is a harmless no-op
  (`cel_arena.c:110-115`).
- Export split: `arena_alloc`/`arena_reset` are codegen imports
  (`// cel:codegen-export`, `cel_arena.h:39,45`);
  `arena_init`/`arena_capacity`/`arena_cursor` are host-reentry-only
  exports (`wasm_exports.txt:49-52`).
- 3VL helper note: after any `arena_alloc`, raw pointers must be
  re-derived because `memory.grow` may relocate the base
  (`cel_3vl.c:62-64, 86-87, 123-126`).

### 1.5 Kernel conventions (uniform across ~40 TUs)

- Slot ABI: `void cel_<op>_at_v*(uint32_t out_slot, …)` — kernels read
  CelValues at offsets, write the result CelValue at `out_slot`.
- 3VL absorption first (`absorb_3vl_binary/unary`,
  `cel_internal.h:83-110`), then kind checks
  (poison `CEL_ERR_TYPE_MISMATCH`), then the operation.  Errors are
  values (`poison`, `cel_internal.h:74-77`), never traps — except the
  deliberate trap surfaces in §1.8.
- Three-path aggregate dispatch (`map-list-dispatch.md`): kArena fast
  path (pure wasm), kHost trampoline (`cel_host.*` import), kDynamic
  dispatcher that `__attribute__((musttail))` tail-calls one of the
  two so the wasm stack never grows (`cel_runtime.c:200-248,
  479-513, 870-1026`).  On the native build the host imports are
  `__attribute__((weak))` stubs that poison TYPE_MISMATCH (or write an
  empty result), overridable by strong test symbols
  (`cel_runtime.c:215-229` and 7 siblings at 813-868).
- Map literals: fixed capacity, duplicate key → `CEL_ERR_DUPLICATE_KEY`
  poison; comprehension accumulators use `cel_map_insert_at`
  (overwrite-on-collision, error-sticks) and `cel_list_append_at`.
  `cel_map_merge_at` / `cel_map_merge_at_if_bool` fold a whole entry
  MAP into an accumulator — the general `transformMapEntry` step for
  entry expressions codegen cannot decompose, and the only insert path
  that GROWS the accumulator (`arena_map_grow_one` re-allocates the
  dense run and drops the hash index; the header stays put), because a
  computed entry has no compile-time key count to pre-size against.
- Map iteration: 16-byte `MapIterState {kind, cursor, payload, count}`
  in the arena (`cel_runtime.c:1059-1067`); ARENA walks the live
  header, HOST walks a 48-byte/entry snapshot the trampoline wrote
  (`cel_runtime.c:1028-1224`); handle 0 = empty/poisoned/OOM.
- Polymorphic equality: one `equality_kernel`
  (`cel_runtime.c:1386-1415`) — cross-numeric ladder, same-kind scalar
  arms, aggregate dispatchers, `cel_host.cel_message_eq` for messages,
  cross-kind → `false` (not an error).  `cel_not_equals_at_vv` flips
  only if the result is CEL_BOOL (3VL passthrough,
  `cel_runtime.c:1432-1441`).
- Every public helper opens with `CEL_LOG("enter")` (dead-code audit +
  tracing; `cel_log.h:1-25`); compiled out under `-c opt` via
  `-DCEL_LOG_DISABLED` (`BUILD.bazel:83-92, 152-155, 625-628`).

### 1.6 Export/import catalogue mechanism

- `// cel:codegen-export` markers in the C source are the single
  source of truth for the codegen-helper set (membership + arity +
  returns_i32); `//bazel:gen_runtime_catalogue` derives both the ABI
  catalogue and the linker `--export=` list
  (`runtime-catalogue-genrule.md`; `BUILD.bazel:24-81`).
  `wasm_exports.txt` carries only the host-only bucket: system symbols
  (`__heap_base`, `malloc`, `free`), the arena reentry API, and the
  same-kind/`_arena` tail-call targets exported for native tests
  (`wasm_exports.txt:42-79`).
- `wasm_imports.txt` is the complete undefined-symbol allowlist:
  `cel_log` (module `cel_env`, `cel_log.h:45-47`) + 12 `cel_host_*`
  trampolines (`wasm_imports.txt:1-13`).  WASI imports
  (`wasi_snapshot_preview1.*`, 14 functions) come from wasi-libc and
  are stubbed in the wasm-level test
  (`cel_runtime_wasm_test.cc:290-333`).
- Subtle link topology: `cel_runtime_wasm.bin`'s `srcs` omit
  `cel_optional.c` and `cel_math_ext.c` (`BUILD.bazel:572-604`); those
  TUs reach the wasm link as archive members of the native-named
  `:cel_runtime` cc_library pulled transitively through the C++ kernel
  deps (`cel_base64_ext` etc. all dep on `:cel_runtime`,
  `BUILD.bazel:658-664`), resolved by the `--export=` references.
  Works, but membership-by-archive-pull is implicit.

### 1.7 Backlog #16 verification — `arena_alloc()==0` consumer audit

**Every in-runtime consumer checks the 0 return.**  Exhaustive list
(call site → handling):

| Call site | OOM handling |
|---|---|
| `cel_runtime.c:67-71, 76-79` (`cel_map_create`) | poison OVERFLOW |
| `cel_runtime.c:318-319` (`cel_list_arena_view`) | falls back to source slot (empty walk) |
| `cel_runtime.c:338-341, 346-350` (`cel_list_create`) | poison OVERFLOW |
| `cel_runtime.c:649-661` (`alloc_concat_list`) | poison OVERFLOW |
| `cel_runtime.c:1130-1131` (`cel_map_count`) | return 0 count |
| `cel_runtime.c:1151-1152, 1161-1162` (`cel_map_iter_init`) | return 0 handle (empty iteration) |
| `cel_3vl.c:52-53, 76-77` (unknown-set merge) | caller poisons OVERFLOW (`cel_3vl.c:122-127`) |
| `cel_make.c:24-25` ff., `:74-75` (all `cel_make_*`) | return 0 to caller (native-test-only API; no codegen marker, not wasm-exported) |
| `cel_net_ext.c:302-306, 456-457 (+477-481, 631-635), 605-609` | poison OVERFLOW |
| `cel_type.c:100-104` | poison OVERFLOW (len>0 only) |
| `cel_string_ops.c:86-90` (`concat_into_out`) | poison OVERFLOW |
| `cel_optional.c:48-49 → 138-141, 153-156, 362-365` | poison OVERFLOW |
| `cel_convert.c:535-539` (`stamp_string`) | poison OVERFLOW (len>0 only) |
| `cel_base64_ext.cc:50-53`, `cel_string_ext_internal.h:81-85`, `cel_string_ext_codepoint.cc:80-83, 245-248`, `cel_string_ext_list.cc:62-74`, `cel_string_format.cc:331-334` | poison OVERFLOW |
| `cel_time_parse.cc:109-110 → 231-235, 250-253` | poison OVERFLOW |

Consequence: the backlog #16 hypothesis ("a runtime-side allocation
that doesn't check capacity" causes the 10K-literal-list wasmtime
panic, `cleanup-backlog.md:260-275`) is **not supported by the runtime
kernel source** — within `runtime/`, arena exhaustion degrades to a
`CEL_ERR_OVERFLOW` value (pinned by `AggregateOomTest`,
`cel_aggregate_arena_test.cc:611-680`, and `MakeOomTest`,
`cel_make_test.cc:153-233`).  The trap must originate outside the
kernels (codegen-emitted stores, host-side reentry allocator, or a
shared-memory interaction) — see Validation item V1.

### 1.8 Deliberate trap surfaces (the complete list)

- `cel_arena.c:45` — `arena_init` re-init with different capacity.
- `cel_arena.c:84` — `arena_alloc` before `arena_init`.
- `cel_runtime.c` — PRESIZE_INVARIANT: `cel_map_insert_at` /
  `cel_list_append_at` past capacity (codegen sized the capacity; a
  violation is a codegen regression, trapped not grown).  The shared
  insert kernel takes an `allow_grow` flag; only `cel_map_merge_at`
  passes 1, and it never reaches the trap.
- `cel_optional.c:124, 319` — host-backed zero-predicate /
  select-field paths stubbed until a host trampoline exists.

All are invariant violations or unimplemented-path tripwires per the
CLAUDE.md stub rule; none are reachable from well-formed codegen.

### 1.9 Arena-sizing cliffs (known, documented limitation)

The fixed 64 KiB arena bounds per-Eval intermediates: a ~2,700-element
list (24 B each) or ~1,350-entry map (48 B) exhausts it.  Pinned by
skipped known-bug tests: `e2e/known_bugs_test.cc:156-185`
(graceful OVERFLOW at 4000 elements), `:617-639` (wasmtime *panic* at
10,000 — backlog #16), `:651+` (graceful host-trampoline OOM at 10 k
bound strings — backlog #17).  `cel_list_in_arena` itself is
allocation-free (`cel_runtime.c:598-617`); #17's OOM is host-side
snapshot allocation, not this kernel.

## 2. Doc-vs-code discrepancies

1. **FIXED — `cel_map.h` claimed `cel_map_insert_at` does "geometric
   growth (2× capacity, min 4) when full"; the code traps instead.**
   The growth path had been replaced by codegen pre-sizing and the
   comment survived the change, so a reader implementing a codegen
   consumer against the header would expect growth and get a
   host-visible trap.  The header now states the pre-size invariant.
   (Geometric growth does exist again, but only on the
   `cel_map_merge_at` path and only there — see §1.7.)

2. **P1 — stale fixed-offset arena description in two headers.**
   `cel_memory.h:6-13` and `cel_runtime.h:17-30` still document
   "arena state lives at fixed offsets `[8..12) bump / [12..16)
   limit`" and a 2-arg `arena_reset(arena_base, arena_limit)` prologue.
   Actual: arena state is a BSS struct (`cel_arena.c:20-27`),
   `arena_reset` is zero-arg (`cel_arena.h:46`), bytes 8/12 are dead
   (`wasi/DESIGN.md:58-60` confirms removal).

3. **P1 — `cel_memory.c:36-41`: wasm `cel_memory_size_` returns a
   fixed 64 KiB with a comment claiming "the module imports a 1-page
   memory" and that "arena_alloc's bounds check uses `limit` from the
   cursor slot".**  Actual: the runtime *defines/exports* a shared
   `(memory 4 1024)` (`memory-layout-design.md` §1), arena_alloc
   bounds-checks against `g_arena.capacity` (`cel_arena.c:89`), and on
   the wasm build nothing consults `cel_memory_size_` (the only caller
   is the native arm of `arena_init`, `cel_arena.c:65`).  Latent
   footgun: any future wasm-side caller of public `cel_mem_size()`
   gets 64 KiB for a ≥256 KiB memory.

4. **P1 — `cel_runtime_wasm_test.cc:13, 196-198` claims the harness
   memory "matches cel_runtime.wasm's `--import-memory` min=2".**
   The build has no `--import-memory`; the runtime owns its memory
   (`BUILD.bazel:644-649`).  The harness's host-owned non-shared
   2-page `cel.memory` define (`:226-239`) is dead weight — the
   instantiated module uses its own exported memory, so the test's
   "binds it as cel.memory" framing misdescribes what's exercised.

5. **P1 — `cleanup-backlog.md:268-271` (#16) attributes the
   10K-literal-list wasmtime panic to "a runtime-side allocation that
   doesn't check capacity".**  §1.7 shows every runtime-side
   `arena_alloc` consumer checks; the root cause is necessarily
   outside `runtime/`.  The backlog entry steers the fixer at the
   wrong layer.

6. **P2 — `cel_runtime.c:307-309` says `cel_list_arena_view` "returns
   0 only on arena_alloc failure; the codegen path treats 0 as fall
   back".**  The code never returns 0 — it performs the fallback
   itself (`return list_slot`, `:319`).  Comment describes a prior
   contract; behavior is strictly safer.

7. **P2 — `cel_map.h:126-128` describes an "8-byte iterator-state
   struct `{header_ptr; cursor}`"; actual is the 16-byte
   `MapIterState {kind, cursor, payload, count}`**
   (`cel_runtime.c:1059-1067`).  The header itself declares the layout
   a private detail, so cosmetic.

8. **P2 — `cel_map.h:148-150` says `iter_key_at` before a successful
   `iter_next` is "defensively a no-op"; the code poisons `out_slot`
   with `CEL_ERR_INDEX_OUT_OF_BOUNDS`** (`cel_runtime.c:1194-1200`).
   Poison is the better behavior; comment lags.

9. **P2 — `wasi/DESIGN.md:348` (A16) specifies
   "`ABSL_CHECK(g_arena.base == NULL)` on entry" (init exactly once);
   shipped behavior allows idempotent same-capacity re-init**
   (`cel_arena.c:43-48`, pinned by `cel_arena_test.cc:355-370`).
   DESIGN.md is marked historical; `cel_arena.h:31-33` documents the
   as-shipped contract correctly.

10. **P2 — `cel_memory.c:46-48` says the native buffer's +65536 slack
    is "for the [0, 16) reserved bytes"** — 16 bytes don't need
    64 KiB; the slack also absorbs test-staged rodata above the arena.
    Comment understates its own purpose.

11. **P2 — `BUILD.bazel:6-16` says `wasm_exports.txt` is consumed by
    `//abi:runtime_catalogue_consistency_test`**, but
    `runtime-catalogue-genrule.md:85-91` records that test was
    *removed* as tautological (membership now derives from the same
    markers).  One of the two is stale (likely the BUILD comment).

## 3. Validation items

- **V1 — Where does the 10K-literal-list wasmtime panic actually
  originate?**  Settle by: un-skip
  `e2e/known_bugs_test.cc::KnownBugs.LiteralIntListInScanTrapsAt10K`
  locally (`bazel test //e2e:known_bugs_test --test_filter='*LiteralIntList*'`),
  run under a debugger / with `RUST_BACKTRACE`, and disassemble the
  generated expr module (`wasm-dis`) to see whether the faulting store
  is codegen-emitted (workspace/rodata store above `--global-base`?)
  or host-side (`WasmtimeArenaAllocator` reentry).  The runtime
  kernels are exonerated by §1.7; the backlog text should be corrected
  with the finding.
- **V2 — Is the inline-asm opacity barrier in `cel_memory.c:31-35`
  still required under the current wasi-sdk clang?**  (cleanup-backlog
  #4.)  Settle by: build `cel_runtime_wasm.bin` with the barrier
  removed and run `//runtime:cel_runtime_wasm_test` — the
  `ArenaResetReturnsCursorToZero` / `ArenaAllocAdvancesCursor` cases
  fail if clang re-discovers the null-UB elision.
- **V3 — Does the wasm artifact actually contain the
  `cel_optional.c` / `cel_math_ext.c` TUs only via archive pull
  (§1.6), and would a deps reshuffle silently drop them?**  Settle by:
  `wasm-dis bazel-bin/runtime/cel_runtime_wasm.bin | grep -c
  'cel_optional_\|cel_math_'` after a build, and/or add the two files
  to the `.bin` srcs and confirm no duplicate-symbol error (archive
  semantics) — then decide which form to standardize.
- **V4 — `arena_alloc`-before-init trap on the wasm path.**  The
  native death test can't exercise it (process-wide init,
  `cel_arena_test.cc:410-418`).  Settle by: add a
  `cel_runtime_wasm_test` case that instantiates a fresh runtime and
  calls `arena_alloc` *without* `arena_init`, asserting a wasmtime
  trap is returned (not a panic).
- **V5 — Does any codegen-emitted consumer of `arena_alloc` (the
  codegen-export, `cel_arena.h:39-40`) check the 0 return in emitted
  wasm?**  Settle by: compile a comprehension expression
  (`tools/cel`), `wasm-dis` the expr module, and inspect the
  `arena_alloc` call sites for an `i32.eqz` guard.  This is the other
  half of the backlog-#16 audit that lives outside `runtime/`.
- **V6 — Is the harness's `cel.memory` define in
  `cel_runtime_wasm_test.cc:415-423` actually consumed by the module?**
  Settle by: delete the define and re-run the test; if it still
  instantiates (expected — the runtime exports its own memory), delete
  the dead code and fix the header comment (discrepancy #4).

## 4. Test coverage observations

**Pinned well:**

- Arena semantics are exhaustively pinned natively
  (`cel_arena_test.cc`, 30 cases + 2 death tests): reset/realloc
  round-trips, 8-alignment boundary table, zero-fill after dirty
  reset, exact-capacity boundary, OOM cursor preservation, the
  additive-overflow wrap regression (`:205-216`), `cel_value_at`
  sentinel.  The same ABI is re-pinned through real wasmtime on the
  cross-compiled artifact (`cel_runtime_wasm_test.cc:538-579`),
  including OOM-returns-0.
- OOM degradation paths: `AggregateOomTest`
  (`cel_aggregate_arena_test.cc:611+`, header-fits/elements-don't
  split) and `MakeOomTest` (`cel_make_test.cc:153-233`, exact-boundary
  ±1) verify A10 end-to-end at the kernel layer.
- Kernel matrices are broad (≈700 TEST cases across 25 `_test.cc`):
  convert (INT64_MIN/MAX, NaN, ±Inf, UTF-8 reject matrix per RFC3629),
  map keys (round-trip per valid kind, cross-type, disallowed-kind,
  embedded-NUL, bool-vs-int distinctness), 3VL truth tables, time
  range boundaries (sign-correlated nanos at MIN/MAX,
  `cel_time.c:38-50`), string_ext / format / matches / net / optional
  / math fixtures.
- The known arena cliffs are tracked as skipped tests with un-skip
  recipes (`e2e/known_bugs_test.cc:156-185, 617-690`).

**Gaps:**

- `arena_alloc`-before-init trap untestable natively and not covered
  on the wasm path (V4); the source comment at
  `cel_arena_test.cc:410-418` acknowledges this.
- No test pins wasm `cel_mem_size()`'s value, which is how
  discrepancy #3 stays latent.
- The PRESIZE_INVARIANT traps (`cel_runtime.c:167, 387`) have no
  death-test (native) or trap-assertion (wasm) coverage — a regression
  to silent growth/poison would pass the suite.
- `cel_list_arena_view`'s OOM fallback arm (`cel_runtime.c:319`) and
  `cel_map_count`'s host-arm OOM (`:1131`) are not directly exercised.
- No automated check that the built wasm's export section matches the
  marker-derived catalogue (acknowledged in
  `runtime-catalogue-genrule.md:100-106`).

## 5. Design decisions worth preserving

- **OOM is a value, traps are codegen bugs.**  Arena exhaustion
  degrades to `CEL_ERR_OVERFLOW` at every kernel; `__builtin_trap` is
  reserved for invariant violations (double-init, alloc-before-init,
  capacity-presize violations, unimplemented host paths).  Keep the
  two regimes distinct; never let an OOM path trap or a codegen bug
  poison.
- **Subtraction-form bounds check** (`need > capacity - cursor`,
  `cel_arena.c:85-89`): the additive form wraps for near-`UINT32_MAX`
  requests and silently admits the alloc.  Regression-pinned.
- **Offset-0 sentinel everywhere**: `arena_alloc` OOM, absent
  CelValue, empty iterator handle, empty span all use 0; `alloc(0)`
  therefore must return a *valid* slot (A9) so 0 stays unambiguous.
- **The opacity barrier in `cel_memory_base_`** is load-bearing until
  V2 proves otherwise; removing it historically compiled `arena_reset`
  to a no-op and `arena_alloc` to `unreachable`.
- **`musttail` dispatchers + `-mtail-call`**: kind dispatch never
  grows the wasm stack and the toolchain *errors* (rather than
  silently downgrading) when tail-call lowering is unavailable; the
  matching wasmtime config flag is part of the contract.
- **Weak host stubs on the native build** (poison-TYPE_MISMATCH
  no-ops, strong-overridable) let every kernel unit-test link without
  wasmtime while making accidental host-path entry visible at the
  assertion boundary.
- **`static inline` shared helpers in `cel_internal.h`** preserve
  cross-TU inlining for the wasm build (split-plan Risk #1); both
  builds also now pass `-flto` (load-bearing for the `in`-scan hot
  loop, `BUILD.bazel:143-151, 619-624`).
- **Marker-derived catalogue**: membership of the codegen-helper set
  is *not* derivable from signatures (identical-signature helpers
  differ in codegen-vs-host-only role); the `// cel:codegen-export`
  marker is the irreducible fact and the single source of truth for
  catalogue + linker keep-list.
- **Codegen pre-sizing over runtime growth** for arena aggregates:
  capacity = literal count or `iter_range.count`; the runtime traps on
  violation instead of growing, so a codegen sizing regression
  surfaces at the first write, not as silent arena churn.  (The
  flip-side cost is the 64 KiB cliff in §1.9 — any future
  grow-on-demand design must revisit both this trap and the
  poison-OVERFLOW paths together.)
- **Memory-relocation discipline**: every raw pointer into linear
  memory must be re-derived after any `arena_alloc`/`malloc`
  (`memory.grow` can move the base on wasm32) — see
  `cel_3vl.c:62-64, 123-126` for the canonical pattern.
- **Little-endian-only host** is enforced at compile time
  (`cel_data.h:208-210`) because host↔wasm CelValue transfer is
  bitwise memcpy.
