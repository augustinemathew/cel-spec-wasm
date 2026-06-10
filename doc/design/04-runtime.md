# 04 — The runtime kernel (`runtime/`)

Status: current — authored 2026-06-10 from the design-rebuild notes
(doc/design/notes/). Supersedes:
doc/implementation-plan/rewrite/wasi/DESIGN.md (memory-model content;
the doc remains as migration history), cel-runtime-c-split-plan.md,
doc/implementation-plan/runtime-catalogue-genrule.md (folded in).

`runtime/` is the language-agnostic CEL evaluation kernel: ~40 C (plus
a few C++) translation units that compile to `cel_runtime.wasm`, the
module every codegen-emitted expression module links against. The
kernel knows nothing about CEL source, the IR, or the host evaluator —
it speaks only the wire vocabulary of `cel_data.h` (24-byte `CelValue`
slots in one shared linear memory) and the slot-out calling convention
defined in 03-abi-and-memory.md.

## 1. Artifacts & build topology

The same sources are compiled **twice**, plus one derived artifact:

<!-- diagram-wanted: twice-compiled build topology — runtime C sources
     fanning out to (a) native cc_library `:cel_runtime` (-O3 -flto,
     g_memory backing) consumed by ~25 unit-test targets and host-side
     layout consumers, (b) wasm32-wasi-threads cc_binary
     `:cel_runtime_wasm.bin` (-mtail-call, -flto, --global-base=8192,
     import allowlist, export response file) → bytes-embed genrule →
     `:cel_runtime_wasm_bytes`, and (c) `strip_command_wrappers` →
     `:cel_runtime_stripped_wasm_bytes` consumed by static-mode
     Compile(). One shared `cel_layout.h` feeding compiler + eval. -->

1. **The wasm artifact** — `cc_binary` `//runtime:cel_runtime_wasm.bin`
   (`runtime/BUILD.bazel`), cross-compiled by the wasi-sdk toolchain
   for `wasm32-wasi-threads`. The threads target is forced by the
   linked C++ kernels (absl time / RE2 need `<mutex>` via cctz); the
   pure-C kernels use no threading primitives and wasm-ld dead-strips
   the unused wasi imports. A genrule embeds the bytes as
   `kCelRuntimeWasmBytes` (`:cel_runtime_wasm_bytes`) so the evaluator
   needs no file I/O at Plan time.
2. **The native twin** — `cc_library` `//runtime:cel_runtime`, the
   identical sources compiled for the host at `-O3 -flto`. It backs
   every kernel unit test (~25 `_test.cc` targets, ≈700 cases) against
   a static `_Alignas(8)` byte buffer (`g_memory[]`,
   `runtime/cel_memory.c`) so tests exercise the same byte layout the
   wasm build ships, without a wasm runtime in the loop. The
   `_Alignas(8)` is load-bearing: at `-O3 -flto` the linker no longer
   happens to pad the buffer, and an unaligned `CelValue*` derived from
   `cel_memory_base_() + k * 24` would trap on alignment-enforcing
   hosts.
3. **The stripped variant** — `strip_command_wrappers`
   (`runtime/strip_command_wrappers.cc`, Binaryen-based) retargets
   every `X.command_export` wrapper export of the wasi-threads command
   model to the bare body `X` and dead-code-eliminates the wrapper +
   ctor/dtor chain; a genrule runs it over the built wasm and embeds
   the result as `:cel_runtime_stripped_wasm_bytes`. This is the
   artifact static-mode `Compile()` adopts into the expression module
   (see 01-compiler.md §7); a toolchain spike showed neither reactor
   mode nor a vanilla-wasi target can produce it from clang alone.
   `:cel_runtime_stripped_wasm_bytes_test` pins the invariant: valid
   wasm, zero `.command_export` exports, smaller than the original.

### 1.1 Load-bearing flags

Each of these is a contract, not a tuning knob
(`runtime/BUILD.bazel`, `cel_runtime_wasm.bin` copts/linkopts):

- **`-mtail-call`** — the kind dispatchers use
  `__attribute__((musttail))` (§4), which requires the wasm tail-call
  feature to lower as `return_call`. Without the flag clang **errors
  out** rather than silently downgrading to a normal call — that
  failure mode is the point: a dispatcher that grows the wasm stack
  per element would be a silent perf/recursion regression. The
  matching consumer-side flag is
  `wasmtime_config_wasm_tail_call_set(config, true)`
  (`eval/engine.cc:89`); both halves are part of the ABI.
- **`-flto`** (copts AND linkopts, both builds) — lets the linker
  inline kernels across the per-topic TU split (e.g.
  `cel_int_eq_at_vv` into `cel_list_in`'s scan loop). Measured perf
  ceiling for the `in`-list bench before the wasm side gained it; the
  linkopts copy is required or the linker falls back to per-TU object
  code. The `static inline` helpers in `cel_internal.h` are the other
  half of the same cross-TU-inlining strategy.
- **`-Wl,--global-base=8192`** — places the runtime's own static data
  at offset 8192 and above, reserving `[0, 8192)`
  (`CELWASM_RESERVED_LOW_MEMORY_BYTES`, `runtime/cel_layout.h:37`) for
  the expression module's active data segments (rodata + workspace).
  The compiler enforces the budget at Compile time
  (`ValidateExprStaticRegion`); a violation historically corrupted
  runtime statics in the shared memory (see §7).
- **`-Wl,--allow-undefined-file=wasm_imports.txt`** — a tight
  undefined-symbol allowlist replacing the toolchain's blanket
  `--allow-undefined`: `cel_log` (module `cel_env`,
  `runtime/cel_log.h:46`) plus 12 `cel_host_*` trampoline imports
  (`runtime/wasm_imports.txt`). Anything else undefined is a link
  error, not a latent instantiation failure.
- **`-Wl,@wasm_export_args.rsp`** — the export list is a generated
  response file, UNION of the marker-derived codegen-helper names and
  the hand-maintained host-only bucket (§6).
- **`-nostartfiles -Wl,--no-entry`** — library module, no `main`.
- **`-DCEL_LOG_DISABLED` under `-c opt`** (`:opt_mode`
  config_setting) — every public kernel opens with `CEL_LOG("enter")`
  (a `cel_env.cel_log` host trampoline used for tracing and dead-code
  audits, `runtime/cel_log.h`); fatal to per-Eval performance once
  kernels stack, so opt builds compile it out. Production-shape
  numbers are opt-build numbers (see 07-benchmarking.md).

`runtime/cel_layout.h` is the single source of truth for the layout
constants (`CELWASM_RESERVED_LOW_MEMORY_BYTES = 8192`,
`CELWASM_ARENA_CAPACITY_BYTES = 64 KiB`,
`CELWASM_INITIAL_MEMORY_PAGES = 2` — a host-side `>=` assertion floor,
not the actual page count). Compiler, evaluator, and runtime all
include it; `_Static_assert`s in the header pin the cross-cutting
invariants (reserve fits initial memory, 8-alignment, power-of-two
arena capacity).

## 2. Wire data

The byte-level contracts — `CelValue` layout, the `CelKind` table,
entry strides, the slot-out calling convention, the arena invariants,
and the import/export namespaces — have exactly one telling, in
**03-abi-and-memory.md**. This doc does not restate them. The facts
the kernel itself enforces at compile time:

- `_Static_assert(sizeof(CelValue) == 24)` (`runtime/cel_data.h:183`);
  `kCelListEntryStride = 24 == sizeof(CelValue)`,
  `kCelMapEntryStride = 48`; `ArenaMapHeader` / `ArenaListHeader` are
  16 bytes (`cel_data.h`).
- `CelKind` values are wire-stable and append-only: `CEL_NULL = 0`
  through `CEL_CIDR = 19` (`cel_data.h:32-51`).
- A little-endian host is a build requirement (`#error` guard,
  `cel_data.h:208-210`) — host↔wasm `CelValue` transfer is bitwise
  memcpy.
- "Pointers" are u32 byte offsets into the one shared linear memory;
  **offset 0 is the universal absent sentinel** (arena OOM, absent
  value, empty iterator handle, empty span).

![Linear memory](diagrams/memory-map.svg)

## 3. Kernel conventions & the two failure regimes

Every kernel follows the same shape, uniform across the ~40 TUs:

1. **Slot ABI.** `void cel_<op>_at_v*(uint32_t out_slot, …)` — read
   `CelValue`s at the argument offsets, write the result `CelValue` at
   `out_slot`. The few `returns_i32` exceptions (arena, iteration
   handles, counts) are catalogued in 03-abi-and-memory.md.
2. **3VL absorption first.** `absorb_3vl_binary` / `absorb_3vl_unary`
   (`runtime/cel_internal.h`): if any operand is `CEL_ERROR` or
   `CEL_UNKNOWN`, copy it to `out` and return before touching the
   payload. Within the kernel the precedence is error-dominates across
   operands (both ERROR checks precede both UNKNOWN checks), left-bias
   within each class. The cross-layer precedence fork — the cel_host
   trampolines apply a first-operand-wins rule — is adjudicated in
   03-abi-and-memory.md §8; this doc records only what the kernel
   does.
3. **Kind checks next.** `require_kinds` poisons `out` with
   `CEL_ERR_TYPE_MISMATCH` on a wrong-kind operand and skips the
   operation.
4. **Then the operation**, with every fallible step degrading to a
   poison value, never a trap.

**Regime 1 — errors are values.** `poison(out, CEL_ERR_*)`
(`cel_internal.h`) stamps `CEL_ERROR` + a wire-stable code. Arena
exhaustion is `CEL_ERR_OVERFLOW`; a bad map key is
`CEL_ERR_NO_SUCH_KEY`; arithmetic overflow, range errors, UTF-8
violations, duplicate map keys all poison. Pinned at the kernel layer
by `AggregateOomTest` (`runtime/cel_aggregate_arena_test.cc`) and
`MakeOomTest` (`runtime/cel_make_test.cc`). An exhaustive audit of
every in-runtime `arena_alloc` consumer (notes/runtime-kernel.md §1.7)
confirmed all of them check the 0 return — within `runtime/`, OOM
never traps.

**Regime 2 — traps are invariant violations.** `__builtin_trap()` is
reserved for states that well-formed codegen and a correct host can
never produce; trapping at the violation site beats a plausible wrong
answer four passes later. The complete list (every trap site in
`runtime/`):

- `cel_arena.c:96` (`arena_init`) — re-init with a different
  first-chunk size. The host owns the lifecycle: exactly one sizing
  per Instance.
- `cel_arena.c:160` (`arena_alloc`) — allocation before `arena_init`.
  A silent 0 return would misdiagnose downstream as
  `CEL_ERR_OVERFLOW`.
- `cel_runtime.c:167` (`cel_map_insert_at`) — PRESIZE_INVARIANT:
  insert past the codegen-sized capacity.
- `cel_runtime.c:387` (`cel_list_append_at`) — PRESIZE_INVARIANT:
  append past the codegen-sized capacity.
- `cel_optional.c:124` (zero-value predicate) — host-backed kinds
  (`CEL_LIST_HOST` / `CEL_MAP_HOST` / `CEL_MESSAGE`) need a host
  trampoline that does not exist yet.
- `cel_optional.c:319` (`dispatch_lookup`) — host-backed optional
  select-field path, same missing trampoline.

The PRESIZE pair is the flip side of a deliberate design choice:
codegen pre-sizes arena aggregates (capacity = literal count or
`iter_range.count`) and the runtime traps on violation instead of
growing, so a codegen sizing regression surfaces at the first write
rather than as silent churn. The `cel_optional.c` pair follows the
stub rule (CLAUDE.md "Unimplemented features"): unreachable for the
conformance corpus today, tripwired in case a host-backed optional
binding lights the path up.

Arithmetic note: 64-bit multiply overflow is detected by a manual
32-bit-halves decomposition (`uint64_mul_overflows`,
`runtime/cel_arith.c:33`), deliberately avoiding
`__builtin_mul_overflow` for 64-bit operands.

> **Open question (V49):** is the manual 32-bit-halves overflow
> multiply still needed under the current wasi-sdk, or does
> `__builtin_mul_overflow` now lower cleanly on wasm32? The helper is
> verified still in use; the *necessity* claim has not been re-probed
> since the toolchain was last bumped.

## 4. Aggregates & iteration

Lists and maps exist in two physical representations — **arena**
(header + inline entries in linear memory, built by codegen) and
**host** (an externref-backed object behind a `cel_host.*`
trampoline). Every polymorphic aggregate operation therefore has three
entry points (`runtime/cel_runtime.c`; design rationale in the
compiler doc's origin-inference section):

1. **kArena fast path** — `cel_<op>_arena`, pure wasm over the
   in-memory header. Codegen calls it directly when ResolvePass proved
   the operand's origin.
2. **kHost trampoline** — `cel_host_cel_<op>`, an import the host
   binds (e.g. `import_module("cel_host"), import_name("cel_map_lookup")`,
   `cel_runtime.c`).
3. **kDynamic dispatcher** — `cel_<op>` reads the operand's kind at
   runtime and `__attribute__((musttail))` tail-calls one of the
   other two, so dispatch never grows the wasm stack. This is the form
   codegen emits when origin is unknown; `-mtail-call` (§1.1) makes a
   non-lowerable tail call a build error.

On the **native build** the `cel_host_*` imports are
`__attribute__((weak))` no-op stubs that poison `CEL_ERR_TYPE_MISMATCH`
(or write an empty result); unit tests that need a host path override
them with strong symbols. Accidental host-path entry in a kernel test
is therefore visible at the assertion boundary instead of silently
linking against nothing.

Construction and iteration specifics:

- **Map literals** are fixed-capacity; a duplicate key poisons
  `CEL_ERR_DUPLICATE_KEY` (`cel_runtime.c`, literal builder).
  Comprehension accumulators instead use `cel_map_insert_at`
  (overwrite-on-collision, error-sticks) and `cel_list_append_at` —
  both trapping past capacity per §3.
- **Map iteration** uses a 16-byte `MapIterState {kind, cursor,
  payload, count}` allocated in the arena
  (`_Static_assert(sizeof == 16)`, `cel_runtime.c:1096-1099`). The
  ARENA arm walks the live header; the HOST arm walks a flat
  48-byte-per-entry snapshot the `cel_host.cel_map_iter_open`
  trampoline wrote into the arena at iter-open time, with string/
  aggregate payloads deep-copied so the snapshot stays valid for the
  rest of the current Eval. Handle 0 means empty, poisoned, or OOM —
  iteration over it is a no-op, consistent with the offset-0 sentinel.
- **Polymorphic equality** is one `equality_kernel`
  (`cel_runtime.c`): cross-numeric ladder, same-kind scalar arms
  (`cel_int_eq_at_vv` and siblings, exported for direct native-test
  access), aggregate dispatchers, `cel_host.cel_message_eq` for
  messages, and cross-kind → `false` (a value, not an error).
  `cel_not_equals_at_vv` negates only when the equality result is
  `CEL_BOOL`, passing 3VL poison/unknown through unchanged.

## 5. Memory substrate

**Ownership.** The runtime **defines and exports** the shared linear
memory (observed `(memory 4 1024 shared)`); the host clones the export
after instantiating the runtime and binds it as `cel.memory` for the
expression module (dynamic mode) or the adopted module addresses it as
its own (static mode). `[0, 8192)` belongs to the expression module
(§1.1); the runtime's statics, the wasi-libc shadow stack, and the
dlmalloc heap live above.

**The arena** (`runtime/cel_arena.{h,c}`) is the per-Eval bump
allocator behind every kernel intermediate. As shipped it is a
**chained-chunk grow-on-demand** design (the fixed-capacity arena it
replaced is the shape most older docs describe — see §7 and
cleanup-backlog #34):

- State is a BSS struct (`g_arena`: head/tail chunk pointers,
  `total_used`, `total_cap`, `initialized`) — NOT at fixed memory
  offsets; the pre-WASI cursor slots at bytes 8/12 are dead.
- `arena_init(cap_bytes)`, called once per Instance by the host, seeds
  the first chunk (default `CELWASM_ARENA_CAPACITY_BYTES` = 64 KiB)
  from dlmalloc. Same-capacity re-init is idempotent; different-size
  re-init traps (§3).
- `arena_alloc(n)` bump-allocates 8-aligned, zero-filled bytes from
  the tail chunk. On overflow (wasm build) it mallocs a new chunk
  sized `pick_grow_size`: double the previous chunk, clamped to
  `[CEL_ARENA_MIN_GROW_BYTES = 4 KiB, CEL_ARENA_MAX_GROW_BYTES =
  1 MiB]`, with a floor of the triggering request so a single huge
  allocation gets a chunk that fits it. malloc failure returns 0 (OOM
  is a value, §3). The bounds check uses the subtraction form
  (`need > capacity - cursor`) so a near-`UINT32_MAX` request cannot
  wrap — regression-pinned in `runtime/cel_arena_test.cc`.
- `arena_alloc(0)` returns a valid 8-byte slot so offset 0 stays
  unambiguous as the OOM/absent sentinel.
- `arena_reset()` — emitted by codegen as `$eval`'s first
  instruction — frees every chunk except the first and rewinds its
  cursor: O(extra-chunks), no per-Eval malloc churn when the initial
  sizing suffices. Reset before init is a harmless no-op.
- The **native twin does not chain** (no shared memory to grow into):
  the first chunk is carved from `g_memory[16 …]` and OOM is terminal
  (returns 0). The wasm build returns the malloc'd pointer as an
  absolute linear-memory offset; the native build returns
  `16 + local_off` so `cel_mem_base() + off` resolves identically on
  both builds — the dual-build contract every kernel test relies on.

> **Open question (V44):** `arena_alloc(0)` returns a valid slot
> (invariant A9), but the host-side `WasmtimeArenaAllocator` maps a
> 0-byte request to nullptr. Whether any live path depends on the
> divergence (e.g. an empty-span encode) has not been probed; the
> arena contract section of 03-abi-and-memory.md needs the verdict.

**Opacity barrier.** On wasm32, `cel_memory_base_()` synthesizes the
byte pointer for offset 0 through an inline-asm barrier
(`__asm__("" : "+r"(p))`, `runtime/cel_memory.c`). Without it, clang
treats stores through `(uint8_t*)0 + off` as null-pointer UB and
elides them — disassembly of a barrier-less build showed `arena_reset`
compiled to a no-op and `arena_alloc` to `unreachable`. The barrier
costs one register copy and is load-bearing until re-proven otherwise.

**Relocation discipline.** `memory.grow` may move the linear-memory
base, so every raw pointer derived from `cel_memory_base_()` must be
re-derived after any `arena_alloc`/`malloc`. `cel_3vl.c`'s
unknown-merge is the canonical pattern (re-derives across its two
allocations). This is a kernel-authoring rule, not an API: nothing
checks it mechanically.

Known wart (kept honest here rather than hidden): the wasm arm of
`cel_memory_size_()` returns a fixed 64 KiB under a stale comment
describing an imported 1-page memory. Nothing on the wasm build
consults it today (the only caller is the native arm of `arena_init`),
but any future wasm-side caller of `cel_mem_size()` would read 64 KiB
for a multi-page memory. Flagged in notes/runtime-kernel.md
discrepancy #3.

> **Open question (V45):** five small probes remain open from the
> notes pass: (a) is the opacity barrier still required under the
> current wasi-sdk clang (build without it, run
> `//runtime:cel_runtime_wasm_test`); (b) do `cel_optional.c` /
> `cel_math_ext.c` reach the wasm link only via archive pull (§6), and
> would a deps reshuffle silently drop them; (c) does
> alloc-before-init trap cleanly (not panic) on the wasm path — the
> native death test cannot exercise it; (d) does codegen-emitted wasm
> guard `arena_alloc == 0` at its own call sites; (e) is the wasm
> test harness's host-defined `cel.memory` dead weight now that the
> runtime exports its own memory.

## 6. Export catalogue mechanics

The codegen-helper surface — which kernels an expression module may
import, with what arity and return shape — is derived, not
hand-maintained. The single source of truth is the C source itself:

1. **Markers.** Each codegen-callable helper carries a
   `// cel:codegen-export` marker at its declaration (e.g.
   `runtime/cel_arena.h:39,45`; ~200 markers across the kernel
   headers). Membership is *not* derivable from signatures —
   identical-signature helpers differ in codegen-vs-host-only role —
   so the marker is the irreducible fact.
2. **Genrule.** `//bazel:gen_runtime_catalogue` parses the markers
   plus the `void`/`uint32_t` signature (clang lowers it one-to-one to
   the wasm function type) and emits both the ABI catalogue textproto
   (consumed by `//abi`, where `CelRuntimeHelpers()` serves it to
   compiler and evaluator) and the bare name list
   (`:codegen_export_names`).
3. **Linker keep-list.** `:wasm_export_args` UNIONs the generated
   names with `runtime/wasm_exports.txt` — which now carries **only
   the host-only bucket**: system/linker symbols (`__heap_base`,
   `malloc`, `free`), the arena reentry API the host calls directly
   (`arena_init` / `arena_capacity` / `arena_cursor` — distinct from
   the codegen-imported `arena_alloc` / `arena_reset`), and the
   same-kind eq/ne + `_arena` tail-call targets exported so native
   tests can call past the polymorphic dispatchers. The union becomes
   the `--export=` response file for wasm-ld.

The once-planned `runtime_catalogue_consistency_test` was **deleted as
tautological** (both sides now derive from the same markers); the
comments in `runtime/wasm_exports.txt` and the head of
`runtime/BUILD.bazel` that still cite it are stale.

> **Open question (V42):** with the consistency test gone, nothing
> automatically checks the built wasm's actual export section against
> the marker-derived catalogue (a wasm-ld behavior change could
> silently drop an export). The agreed cadence is a manual
> `wasm-dis … | grep '(export'` vs `CelRuntimeHelpers()` diff once per
> toolchain bump; no automated gate exists yet.

**Archive-pull link-topology caveat.** `cel_runtime_wasm.bin`'s `srcs`
list omits `cel_optional.c` and `cel_math_ext.c`; those TUs reach the
wasm link as archive members of the native-named `:cel_runtime`
cc_library, pulled transitively through the C++ kernel deps
(`:cel_base64_ext`, `:cel_matches`, `:cel_string_ext`,
`:cel_string_format`, `:cel_time_parse` all dep on `:cel_runtime`) and
resolved by the `--export=` references. It works, but membership by
archive pull is implicit — a deps reshuffle that breaks the transitive
edge would surface only as missing-export link errors (probe (b) of
the V45 callout above).

The import side is symmetric and small: `runtime/wasm_imports.txt`
allowlists `cel_env.cel_log` plus the 12 `cel_host_*` trampolines the
*kernel itself* calls (dispatcher tail-call targets, message-type
resolve, timestamp tz). The full host catalogue is larger —
expression modules may import trampolines the runtime never does; the
12-vs-20 split is tabled in 03-abi-and-memory.md.

## 7. Known limitations

**The arena-cliff family.** The kernel historically had a hard
per-Eval ceiling: a fixed 64 KiB arena, so a ~2,700-element
intermediate list (24 B/entry) or ~1,350-entry map (48 B/entry)
exhausted it even when the expression's *result* was a single int.
Two changes reshaped (but did not retire) the family:

- The **chained arena** (§5, cleanup-backlog #34) removed the
  kernel-side hard cap: intermediates now grow in malloc'd chunks
  (1 MiB per-chunk clamp), bounded ultimately by the linear memory's
  maximum and dlmalloc. Exhaustion still degrades to a
  `CEL_ERR_OVERFLOW` value, never a trap.
- The **compile-time static-region gate** (`ValidateExprStaticRegion`,
  `compiler/internal/compile.cc`) now rejects expressions whose rodata
  + workspace exceed the 8192-byte reserve with `ResourceExhausted`,
  in both link modes — closing the corruption class where a large
  literal's data segment was applied over the runtime's statics in the
  shared memory (the true root cause of both the historical
  "unaligned atomic" trap and the 10K-literal-list wasmtime panic;
  the old backlog text blamed a nonexistent unchecked runtime
  allocation — the §3 audit exonerated the kernels).

The *binding* constraint for large literal aggregates is therefore now
the compile-time gate, not the arena. The family is pinned by named
known-bug tests in `e2e/known_bugs_test.cc` —
`ExpressionIntermediatesArenaCliff`, `MapSizeArenaCliff`,
`LiteralIntListInScanRejectedAtCompileAt10K`,
`BoundStringListInScanArenaOomAt10K` — each skipped with the verified
current behavior (compile-time `ResourceExhausted`) and the un-skip
recipe (a relocatable/growable static region) baked into the skip
message. A 10K-element literal list is a legitimate CEL expression;
rejecting it at Compile is the honest, safe behavior, not the desired
end state.

**Other known edges**, stated plainly:

- The PRESIZE traps (§3) have no death-test (native) or
  trap-assertion (wasm) coverage; a regression to silent growth would
  pass the suite (notes/runtime-kernel.md §4).
- `arena_alloc`-before-init is untestable natively (process-wide init)
  and not yet covered on the wasm path (V45 probe (c)).
- Wasm `cel_mem_size()` reports a fixed 64 KiB (§5) — latent footgun
  for future wasm-side callers; no test pins it.
- Several runtime headers still describe superseded designs (map
  growth-on-full in `cel_map.h`, fixed-offset arena state in
  `cel_memory.h` / `cel_runtime.h`, the deleted consistency test in
  `wasm_exports.txt` / `BUILD.bazel`); the implementation, not the
  header prose, is authoritative until the comment fixes land.

## 8. Future work

- Resolve the V42/V44/V45/V49 callouts above; each names its probe.
- Wasm-side trap-assertion coverage for the §3 trap list (PRESIZE
  pair, alloc-before-init), so a regression from trap to silent
  growth/poison fails a test.
- The host trampolines that retire the two `cel_optional.c` trap
  stubs (host-backed zero-predicate and optional select-field).
- Make the `cel_optional.c` / `cel_math_ext.c` wasm-link membership
  explicit (srcs or a dedicated library edge) once V45 probe (b)
  settles which form is correct.
- A relocatable or growable expr static region, which would let the
  known-bug cliff pins flip from compile-rejection assertions back to
  value assertions.
- Header-comment reconciliation for the stale designs listed in §7,
  riding the same commits as the corresponding doc sections.

## History

This doc supersedes, for runtime content:

- `doc/implementation-plan/rewrite/wasi/DESIGN.md` — the WASI
  migration design (memory-ownership flip, arena-to-BSS move,
  invariant catalogue). Remains in place as migration history; its
  memory-model sections are superseded by §5 here and by
  03-abi-and-memory.md.
- `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md` — the
  per-topic TU split of the original monolithic `cel_runtime.c`; the
  split shipped, and its load-bearing residue (`cel_internal.h`
  static-inline helpers + `-flto`) is recorded in §1.1.
- `doc/implementation-plan/runtime-catalogue-genrule.md` — the
  marker-derived catalogue mechanism, folded into §6 (including the
  deletion of the consistency test it documented).

Source notes: `doc/design/notes/runtime-kernel.md`,
`doc/design/notes/90-abi-memory-consistency.md`,
`doc/design/notes/00-consolidated-findings.md` (§4 outline, runtime
rows of the discrepancy register). The chained-arena description in §5
postdates those notes (cleanup-backlog #34, verified against
`runtime/cel_arena.c` as merged); where the notes describe the
fixed-capacity arena, this doc is the corrected telling.
