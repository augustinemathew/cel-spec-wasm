# 90-abi-memory-consistency — cross-component lens pass

Lens: ABI & memory-layout consistency.  Cross-checked every claim about
CelValue layout, slot sizes, rodata/workspace/arena offsets, calling
conventions, and import signatures across the codegen-memory,
codegen-lowering, runtime-kernel, abi-shared, and eval-internal notes
(plus the other ten notes where they touch the wire).  Disagreements
were settled against code on branch `m28-configurable-linking`,
2026-06-10; every verdict below carries the deciding citation.

## 1. Verified-consistent: the wire facts with exactly one telling

These were claimed independently by ≥3 notes and re-verified against
code; the new design docs can state them once and cite this section.

- **CelValue is 24 bytes: u32 kind @ +0, u32 pad @ +4, 16-byte payload
  union @ +8** (`runtime/cel_data.h:183` `_Static_assert(sizeof == 24)`;
  builder frames `static_memory_builder.cc:56-63`; conditional probe
  offsets `expr_lower.cc:1080-1091`; host MemoryView
  `eval/internal/cel_host.h:298-314`).  Strides:
  `kCelListEntryStride = 24 == sizeof(CelValue)`
  (`cel_data.h:199`), `kCelMapEntryStride = 48`; `ArenaMapHeader`/
  `ArenaListHeader` are 16 bytes (`cel_data.h:78,97`).
  codegen-lowering's hand-copied literals (CEL_BOOL=1, CEL_INT=2,
  24-byte stride) verified correct today (`cel_data.h:33-34`) — the
  missing compile-time tie remains codegen-lowering's known-issue #2,
  not a notes conflict.
- **CelKind numbering** — runtime-kernel §1.2, codegen-memory #7,
  eval-public §1.6, and design-heritage all agree, and code confirms:
  `CEL_NULL=0 … CEL_BYTES=6, CEL_LIST_ARENA=7, CEL_MAP_ARENA=8,
  CEL_MAP_HOST=9, CEL_MESSAGE=10, CEL_TYPE=11, CEL_DURATION=12,
  CEL_TIMESTAMP=13, CEL_OPTIONAL=14, CEL_UNKNOWN=15, CEL_ERROR=16,
  CEL_LIST_HOST=17, CEL_IP=18, CEL_CIDR=19` (`cel_data.h:31-52`).
  The four enums (`CelKind` wire / `ir::Repr` / `shared::CelType` /
  `Value::Kind`) are distinct numberings by design; abi-shared §5 and
  eval-public §1.6 state the alignment rules identically.
- **Calling convention** — uniform slot-out:
  `(i32 out_slot, i32 arg_slot…) -> void` for every kernel and every
  `cel_host`/`cel_env` trampoline; `returns_i32` true only for runtime
  arena/iter/count helpers (`runtime_catalogue.proto:53-63`, pinned
  `runtime_catalogue_test.cc:91-104`).  codegen-lowering §5.1,
  abi-shared §1.5, eval-internal §1.2, celfn §5, and design-heritage §5.1
  all tell it the same way.  Sanctioned exceptions: `_?_:_` inline
  BinaryenIf and the `dyn` identity arm.
- **Memory regions** — `[0,8)` null sentinel; `[8,16)` dead legacy
  arena-cursor slot kept so `rodata_base` stays 16; rodata from 16;
  workspace at `RoundUp8(rodata_base + rodata.size())` in 24-byte
  cells; `[8192, …)` runtime statics (`--global-base =
  CELWASM_RESERVED_LOW_MEMORY_BYTES = 8192`, `cel_layout.h:37`); arena
  is a 64 KiB malloc'd buffer in the dlmalloc heap
  (`CELWASM_ARENA_CAPACITY_BYTES`, `cel_layout.h:41`), state in BSS
  (`cel_arena.c:19-27`), NOT at fixed offsets.  codegen-memory §1.6
  and runtime-kernel §1.3 agree byte-for-byte.
- **`arena_reset` is zero-arg**, emitted as `$eval`'s first
  instruction; `arena_init(cap)` once per Instance from the host.  All
  five core notes agree; the stale two-arg form survives only in
  headers each note already flagged (`expr_lower.h:7,201`,
  `compile.h:44-49`, `module.h:10-16`, `cel_memory.h:6-13`,
  `cel_runtime.h:17-30`) — one telling, several dead comments.
- **Error wire shape is the bare code**: `payload.err = CEL_ERR_*` u32
  (`cel_data.h:216-262`; writer `cel_host_error.cc:89-94`; drop site
  `cel_host.cc:802-807`).  runtime-kernel, eval-internal (D1),
  eval-public, and design-heritage (§8.5 "expr_id NEVER SHIPPED")
  agree.  The two outliers are in code/docs, not in the notes:
  `cel_log.cc:168-184` `%v` still implements the never-shipped
  descriptor struct (eval-internal D2), and `cel-host-surface.md:820-823`
  still specifies the rich shape (see finding 2).
- **Offset 0 is the universal absent sentinel** (arena OOM, iter-var
  slot_offset, attribute/message-type/field ids, empty iter handle);
  `alloc(0)` returns a valid slot so 0 stays unambiguous
  (`cel_arena.c:78,125-129`; codegen-memory §1.7; runtime-kernel §5).
- **Memory ownership/topology** — the runtime defines and exports the
  shared memory (`(memory 4 1024 shared)` observed); dynamic-mode expr
  modules import `cel.memory` (min = `PagesForBytes(mem_size_bytes)`,
  default 2, max 1024); static mode adopts the runtime module so the
  Program owns its memory (named "0").  runtime-kernel §1.3,
  compiler-toplevel §1.3, eval-public §1.2/#3, codegen-lowering
  (memoryName=nullptr rule), and design-heritage §1.3 are consistent;
  the apparent 2-vs-4-page tension is min-of-import ≤ min-of-export
  plus the `CELWASM_INITIAL_MEMORY_PAGES = 2` host-side assertion
  *floor* (`cel_layout.h:29`, A13 `>=` check `engine.cc:357-360`).
  The new doc should present all three numbers in one table — see
  finding 9.

## 2. Findings — where two tellings disagree (settled)

1. **P0 — SlotAllocator release semantics: design-heritage says
   free-list reuse shipped; it did not.**
   `design-heritage.md` §1.1 (design.md §6 row) states "`Release` is
   no longer a no-op — free-list reuse landed", and §2.8 repeats
   "reuse is release-based free-list", reading the worked example at
   `slot_allocator.h:60-77` as shipped behavior.  codegen-memory §1.5
   says `Release()` is a no-op and `peak_slots` counts total acquires.
   **Code settles it for codegen-memory**: `slot_allocator.cc:24-28`
   (`// Naive path (M1–M9): no-op … (void)offset;`) and the header's
   own closing line `slot_allocator.h:76-77` ("M1 ships the no-op
   form; M10 flips on the free list under !debug_mode") — the trace is
   explicitly labelled future.  Pinned by
   `slot_allocator_test.cc:27-47`.  This is P0 for the rebuild because
   the wrong telling erases codegen-memory's verified P0 hazard:
   workspace grows 24 B per *acquire* (not per peak-live cell), is
   bounded nowhere, and ~340 slot-acquiring nodes push workspace
   stores past offset 8192 into runtime statics.  A doc author
   trusting design-heritage would write a workspace story in which
   that hazard cannot exist.  design-heritage §3 validation item 1 and
   §13/§6 rows must be corrected before that note feeds the new docs.

2. **P1 — doc-index grants "[live] / authoritative" status to two docs
   the component notes prove wrong on wire facts.**
   `doc-index.md` labels `rw/cel-host-surface.md` "**[live]** (THE
   host/guest wire contract)" and `rw/memory-layout-design.md`
   "**[live]** (THE memory model — single authoritative)".  But:
   - eval-internal D1/D2/D6/D7 verified cel-host-surface.md wrong on
     the error wire shape (`cel-host-surface.md:820-823` vs
     `cel_host.cc:802-807`), on precomputed FieldRef descriptors
     (`:640-646` vs per-call `FindFieldByNumber`,
     `cel_host.cc:533-540`), and on `CheckCompatible` /
     `cel_ref_intern` surfaces that do not exist.
   - codegen-memory #1 verified memory-layout-design.md's A11/A12
     bounded-layout rows (`memory-layout-design.md:87-93,173-174` —
     `LayoutOptions::reserved_region_limit_bytes`, LayoutPass
     ResourceExhausted) are unimplemented (`layout_pass.h:50-64` has
     no such field; the only gate is rodata-only, static-only,
     `compile.cc:524-536`).  runtime-kernel #3 corrects another of its
     consumers.
   The index's status column is the routing layer for the rebuild; a
   "[live]" label on a doc with wrong wire claims re-injects the
   second telling the lens exists to kill.  Fix: downgrade both rows
   to "[live-with-corrections]" citing the component notes' findings,
   or fold the corrections into the docs first.

3. **P1 — the wire itself has two tellings for `payload.unk`; the
   notes agree on the conflict but the new doc must crown one.**
   Runtime contract: u32 offset to a 2-word UnknownSet descriptor
   `{ids_off, len}` (`cel_3vl.h:24-28`; `cel_unknown_merge`
   dereferences non-zero values, `cel_3vl.c:60-115`).  Host contract:
   the attribute id written directly (`cel_host.cc:1546-1551`,
   `instance.cc:996-998`, decoder `instance.cc:304-309`).
   eval-internal D3 documents both; runtime-kernel §1.4 and
   benchmarking §1.2 (BM_UnknownMerge mints descriptor-shaped
   unknowns per `cel_3vl.h`) follow the runtime telling; eval-public
   §1.4 (`kFunctionUnknownSentinel = 0xFFFFFFFF`) extends the host
   telling.  No note contradicts another about what the code does —
   but a wire field with two live encodings is exactly the
   one-telling violation this lens polices.  The new ABI doc must
   designate one shape (and the fix for eval-internal validation
   item 2 decides which); until then any prose that says "payload.unk
   is X" without the fork is wrong.

4. **P1 — design-heritage resurrects the deleted
   `runtime_catalogue_consistency_test` as a live drift gate.**
   `design-heritage.md` §4: "`runtime_catalogue_consistency_test` pins
   exports↔catalogue↔imports coherence (wasm_exports.txt:14-16)".
   abi-shared #3 and runtime-kernel #11 say the test was deleted as
   tautological (commit 511c3ec8, 2026-05-26).  **Code settles it for
   abi-shared/runtime-kernel**: no such target exists in
   `abi/BUILD.bazel` (grep: zero hits); the only references are the
   stale comments at `runtime/wasm_exports.txt:14` and
   `runtime/BUILD.bazel:12` — the exact comments abi-shared already
   flagged as the drift to clean up.  design-heritage was citing the
   stale comment, not a target.  The coherence story for the new doc
   is "both sides derive from `// cel:codegen-export` markers"; the
   remaining un-gated edge is abi-shared validation item 5
   (catalogue ⇄ actual wasm exports, once per toolchain bump).

5. **P1 — design-heritage overstates the WAT corpus as an always-on
   regression net; tools-examples has the correct telling.**
   `design-heritage.md` §4: "59 `.wat` files … are assembled +
   executed by `wat_runner_test` per build".  tools-examples #2: the
   dir holds 63 files, the test loads roughly half, and the target is
   `manual`-tagged so it does not run per build.  **Verified for
   tools-examples**: 63 `.wat` files on disk
   (`ls doc/implementation-plan/rewrite/wat/*.wat | wc -l` = 63); 33
   distinct `.wat` names referenced by `wat_runner_test.cc`; both
   `:wat_runner` and `:wat_runner_test` carry `tags = ["manual"]`
   (`tools/wat_runner/BUILD.bazel:13,35`).  CLAUDE.md's "re-runs every
   .wat … on every build" inherits the same overstatement
   (tools-examples already flags it).  Any new-doc claim that the WAT
   layer is a regression gate must say: ~half the corpus, manual-tag
   cadence only.

6. **P1 — kernel_bench writes the arena cursor at a dead memory
   offset; the benchmarking note repeats the bench's stale mechanism
   as fact.**  benchmarking §1.2 (kernel_bench row): "allocating
   kernels rewind the arena cursor per iteration by poking the cursor
   word at `cel_mem_base()+8`".  runtime-kernel §1.3/#2: arena state
   moved to the BSS struct `g_arena` (`cel_arena.c:19-27`); bytes
   8..12 are dead.  **Code settles it for runtime-kernel** — and
   surfaces a live bench bug neither note flagged:
   `benchmark/kernel/kernel_bench.cc:388-396, 457-459, 470-472, 490-492` read
   and write `cel_mem_base() + 8` with the comment "arena_alloc reads
   the cursor from bytes 8..12" — false since the WASI migration.
   The rewind stores are no-ops on `g_arena.cursor`, so
   `BM_UnknownMerge` and the three sibling allocating benches never
   reclaim; the cursor bumps monotonically toward the 64 KiB cap,
   after which `arena_alloc` returns 0 and the kernels time their
   OOM/poison paths (`cel_3vl.c:122-127`) instead of the operation.
   Numbers from those rows are not production-shape.  Fix: replace
   the pokes with `arena_cursor()` snapshot + a real rewind API, or
   per-iteration `arena_reset()` + re-staging.  (Benches are
   manual-tagged, so this bit-rot was invisible — benchmarking §4
   gap (d) predicted the failure class.)

7. **P2 — testing-system repeats the stale "three-way export-list
   drift" framing with an `engine.cc::kRuntimeExports` that no longer
   exists.**  testing-system §4 gaps: "runtime exports exist in
   `engine.cc::kRuntimeExports`, runtime linkopts, and
   `wasm_imports.txt` with no consistency test" (cleanup-backlog #2).
   abi-shared §1.5 and eval-public §5 say the engine binds from
   `abi::CelRuntimeHelpers()`.  **Code settles it for them**:
   `eval/engine.cc:241` is a comment recording that the hand-maintained
   array was *replaced* by the catalogue; the only surviving
   hand-maintained mirror is `tools/wat_runner/wat_runner.cc:34`
   (115-entry `kRuntimeExports`, deliberate per tools-examples §5.6).
   The backlog entry (and testing-system's copy) should be rewritten:
   the un-gated copies are wat_runner's list and the runtime linkopts
   keep-list, not the engine.

8. **P2 — `cel_host` import-count split needs one telling: 20 vs 12
   are both correct, for different artifacts.**  eval-internal §1.2
   says exactly 20 `cel_host` trampolines (bijection-checked);
   abi-shared §1.5 says 20 host + 1 env catalogue rows;
   runtime-kernel §1.6 says `wasm_imports.txt` lists 12 `cel_host_*`
   trampolines.  **Verified — no contradiction, subset relation**:
   `runtime/wasm_imports.txt` holds `cel_log` + 12 `cel_host_*`
   names (the runtime kernel's *own* undefined-symbol allowlist — the
   dispatcher tail-call targets plus resolve/tz); the other 8
   catalogue rows (`cel_get_field`, `cel_has_field`,
   `cel_map_iter_open`, `cel_list_iter_open`, `cel_make_message`,
   `cel_set_field`, `cel_wkt_unwrap_time`, `cel_wkt_unwrap_wrapper`)
   are imported only by expr modules, never by cel_runtime.wasm.  The
   new ABI doc should state: catalogue = 20 (+1 env); runtime-side
   import allowlist = the 12-name subset; expr modules may import all
   20.

9. **P2 — compile.cc citation drift between compiler-toplevel and
   celfn.**  compiler-toplevel cites `InstallOverloadImportsExport` at
   `compile.cc:265-299`, `InstallCelHostImports` at `:56-87`, and
   `cel_copy_slot` at `:301-321`; celfn cites
   `InstallOverloadImportsExport` at `:301-357`.  **Code**: the
   per-entry helper `InstallOverloadImport` is at `compile.cc:266`
   (arity-switch `default` skip at `:294-296`),
   `InstallOverloadImportsExport` at `:301`, `InstallCelHostImports`
   at `:92`, `cel_copy_slot` install inside the outer function at
   `:337-354`.  celfn's lines are right; compiler-toplevel conflated
   the helper with the outer function and mis-anchored
   InstallCelHostImports.  Matters only because the silently-skipped
   `num_args ∉ [1,5]` arm (celfn #6, a real hazard) lives at the line
   compiler-toplevel attributes to the wrong function.

10. **P2 — page-count numbers appear as four different values across
    notes; consistent but needs one table.**  Runtime exports
    `(memory 4 1024 shared)` (runtime-kernel §1.3, link-time
    auto-sized 3-4 pages); wat fixtures import
    `(memory 2 1024 shared)` (design-heritage §1.2); dynamic-mode
    codegen imports min `PagesForBytes(mem_size_bytes)` = 2 default
    (compiler-toplevel §1.3, `compile.cc:345-353`);
    `CELWASM_INITIAL_MEMORY_PAGES = 2` is an engine-side `>=`
    assertion floor, not a size (`cel_layout.h:18-29`,
    `engine.cc:357-360`).  All verified compatible (import min ≤
    export min; floor ≤ both).  The new memory doc should carry the
    one table; today a reader must reconcile four notes to learn the
    actual instantiated size is 3-4 pages and growing.

## 3. What this lens did NOT find

- No two notes disagree on CelValue layout, payload offsets, entry
  strides, the CelKind table, the slot-out calling convention, the
  rodata/workspace/arena region boundaries, the 8192 reserve, the
  64 KiB arena cap, or the zero-arg `arena_reset` contract — the five
  core notes converge on one telling for each, and code confirms it
  (§1).  The disagreements found were (a) design-heritage trusting
  header prose over code in three places (findings 1, 4, 5), (b) the
  doc-index status layer lagging the component notes (finding 2),
  (c) one in-code wire fork the notes correctly co-report (finding 3),
  and (d) one stale-mechanism echo that turned out to be a live bench
  bug (finding 6).
- The `payload.err` bare-code shape, the `MapIterState` 16-byte
  layout, the 48 B/entry host map snapshot, the externref three-
  namespace model, and the activation-buffer-outside-arena rule are
  each told identically by runtime-kernel and eval-internal — the
  producer/consumer pair agrees at the byte level.

## 4. Actions for the consolidation pass

1. Correct design-heritage §1.1 (design.md §6 and §13 rows) and §2.8:
   Release is a no-op; Strahler AND free-list both unshipped.  Its §3
   validation item 1 stands but must not presuppose free-list reuse.
2. Correct design-heritage §4 (WAT corpus, consistency test) per
   findings 4-5; correct testing-system's cleanup-backlog #2 echo per
   finding 7.
3. Re-label cel-host-surface.md and memory-layout-design.md rows in
   doc-index per finding 2; the new ABI/memory docs supersede both and
   should be written from the component notes, citing code.
4. File the kernel_bench dead-cursor-poke bug (finding 6) into the
   known-issues register; it is a code bug, not only a doc bug.
5. The new ABI doc's wire section must: state the `payload.unk` fork
   explicitly (finding 3), carry the 20-vs-12 import-set table
   (finding 8), and the page-count table (finding 10).
