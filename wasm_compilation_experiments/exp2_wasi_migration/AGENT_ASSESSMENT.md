# `compiler_v2/` — Critical Assessment

Reviewer: Claude (Opus 4.7, 1M context). Files actually read: `runtime/cel_arena.{c,h}`, `runtime/cel_memory.{c,h}`, `runtime/cel_internal.h`, `runtime/BUILD.bazel`, `codegen/layout_pass.{h,cc}`, `codegen/expr_lower.cc` (skim of ~1100 lines), `api/engine.cc`, `api/instance.cc` (key sections), `api/internal/cel_host.cc` (skim of ~3500 lines), `api/internal/cel_host_wasmtime.cc`, `doc/implementation-plan/rewrite/design.md` (top + §4.7.6). Line counts measured directly.

---

## 1. Headline verdict

The pipeline shape (frontend → resolve → layout → emit) is sound and the layered host stack reads like a mature design. The compiler's worst issues are not in any one decision — they're in *coupling between three plausible decisions that, together, produced a system where any one of them is hard to change*: the bump arena at fixed bytes 8/12, the shared single linear memory between runtime and expr modules, and the static-linker requirement that *every* `cel_host.*` import be defined before instantiation. Those three together force the `host_string_arena` hack, gate the dual-allocator pain you're worried about (RE2/abseil), force `cel_host.cc` into a 3,528-line megafile, and require the 130-line `--Wl,--export=` wall in BUILD.bazel. The runtime C kernels (`cel_arith`, `cel_compare`, `cel_string_ops`, the `_arena` aggregate fast paths) and the type discipline of `WasmAnnotations` are genuinely well-done and worth keeping verbatim. Fix the three coupled root decisions and ~30% of the awkwardness in the rest of the system evaporates.

Concrete numeric anchor: `cel_host.cc` is 3,528 lines (not 3,215 as in the prompt — it has grown). `cel_runtime.c` is also 1,241 lines despite the split — the runtime hub didn't get fully decomposed. The two largest functions are `cel::BuildConditionalArm` at 289 lines (instance.cc) and `MaybeEmitWktUnwrapTailCall` at 176 lines (expr_lower.cc) — comparable to or worse than anything in cel_host.cc.

---

## 2. Architecture critique

### 2.1 The 3-layer host stack

You have:

  - **Layer 1**: `HostMessageBacking` / `HostMapBacking` / `HostListBacking` virtual interfaces returning `cel::Value`. Pure, runtime-agnostic.
  - **Layer 2**: `CelGetFieldImpl(out_slot, msg_slot, …, TrampolineContext)` etc. — runtime-agnostic but works in the wire CelValue ABI. Threads `MemoryView` + `ArenaAllocator` + `ExternrefTable`.
  - **Layer 3**: `CelGetFieldTrampoline` — wasmtime callback signature; unpacks `wasmtime_val_t` args, builds the WasmtimeMemoryView/WasmtimeArenaAllocator, calls Layer 2, converts Status → trap.

**Verdict: the right number of layers, wrong factoring of layer 2.** Layer 1 and Layer 3 are well-motivated and each is small. Layer 3 (`cel_host_wasmtime.cc`) is 566 lines and reads as exactly what it should be: trampoline registration table. Layer 1 backings are well-isolated; you can imagine someone subclassing `HostMessageBacking` to bind a JSON document with minimal pain.

The problem is Layer 2, which is where 3,528 lines of `cel_host.cc` live. Layer 2 conflates four genuinely separable jobs:

  1. Reading from wire CelValue → `cel::Value` (decode).
  2. Writing `cel::Value` → wire CelValue (encode).
  3. The per-impl semantics: actually call `HostMessageBacking::ReadField`, then `EncodeFieldResult`.
  4. WKT unwrap chains (`MaybeUnpackWktMessage`, `UnpackAnyToValue`, the wrapper-peeler family).

Items 1, 2, and 4 are pure marshalling and aren't really "Layer 2" in the design intent — they're a 4th layer that got smashed in. The proof is that almost every `CelXxxImpl` function in cel_host.cc is short (50-90 lines); the line count comes from the decoders/encoders/peelers. You could call this Layer 2 = wire-codec; Layer 2.5 = WKT-codec; Layer 2.6 = trampoline-bodies, and split the file three ways without changing any semantics. The fact that all three live in one file is *bookkeeping debt*, not a design failure.

**Where it bites in practice:** every time someone adds a new `cel_host.cel_xxx` trampoline they touch the encoder/decoder helpers (which are file-private statics in cel_host.cc), the impl body, the Layer 3 trampoline registration, AND `engine.cc::kRuntimeExports[]`. Four-file ripple per new host op.

### 2.2 The bump arena + fixed cursor at bytes 8/12 + `cel_reset` prologue

What's good:

  - Lock-step bump allocation is the simplest possible model. `cel_alloc` is 11 lines. The whole arena is 66 lines.
  - The cursor *lives in linear memory*, which means tests can stage memory bytes before eval and see the same bytes after — no host-side cursor state to sync.
  - Resetting in the `$eval` prologue rather than from the host means there's exactly one synchronization point (the host calls `$eval`, the wasm side rewinds before doing anything).

What's awkward:

  - **`arena_base` and `arena_limit` are baked at codegen time as `i32.const` literals.** This means if you ever want to change arena size at instantiation time (e.g. give a particular Activation a bigger workspace because it's bound to a megabyte string), you can't — you have to recompile. The mem_size_bytes plumbing in `LoweringOptions` is one knob that goes from `Engine` down to `EmitCelResetCall`, threaded through three layers; an Instance can't override it.
  - **The cursor is at bytes 8/12 of *shared* linear memory.** This is the foundation of the host-side string arena hack (§2.4). It's also why the two modules MUST share memory.
  - **`cel_reset` is the first thing every `$eval` runs.** That's an invariant the host CAN'T violate; the prelude sets free-variable workspace slot offsets *and then* runs cel_reset *and then* runs the root expression. Per the comment at expr_lower.cc:138-152, the order works because cel_reset only touches bytes 8/12. But — this is fragile — anything below `arena_base` is sacred; anything below `arena_limit` will be rewound (workspace slots are above rodata, below arena, so they aren't rewound but ARE potentially clobbered by `cel_alloc` returning low offsets if the math ever drifts).

Assumption embedded: **once-shot evaluation**. The runtime cannot have a long-lived value across two `$eval` calls — every `$eval` rewinds. This is fine for CEL's stateless semantics. But it makes the host_string_arena necessary, because *activation bindings* MUST outlive the rewind.

### 2.3 Two-instance shared-memory model

The expr module owns `(memory $mem 1)` and exports it; the runtime imports it as `cel.memory`. They share *one linear address space*. Pro: zero serialization across the boundary, `cel_alloc` writes bytes the expr module reads directly. Con: the runtime is statefully coupled to whichever expr module was instantiated last — except you instantiate them in lock-step per Plan, so this isn't actually a problem in practice.

The deeper trade-off: **you've committed to one wasm runtime binary across all programs.** `cel_runtime.wasm` is shipped as a bytes blob and is the same for every Plan. This is great for cold-start (parse runtime once, cache the parsed module — `WasmtimeEngineState` does exactly this) but it means every Plan pays for every `cel_*` export, every kHost trampoline, every WKT unwrap helper — even if the program is just `1 + 2`. The `kRuntimeExports[]` array in engine.cc is 76 entries long (compare to the 130+ exports in BUILD.bazel — there's already a small drift). Each one is a function the wasm-ld kept because someone might call it.

This is fine in the AOT-compiler world (Plan is rare, Eval is hot), but it makes the runtime binary the load-bearing artifact of cold-start performance. Per the bench comment, `Engine::Build ≈ 167µs` — most of that is parsing the runtime module. The runtime is around 64KB (a single page); if you ever push toward 128KB+ of wasm by adding more kernels, that 167µs grows.

### 2.4 `host_string_arena` (~110 LoC in instance.cc)

This is the canonical example of arena lifecycle dictating the rest of the system.

The activation marshal has to write bound string/bytes payload bytes *somewhere* in linear memory before calling `$eval`. The wasm-side arena (above `arena_base`, below `arena_limit`) doesn't work because the `$eval` prologue calls `cel_reset(arena_base, arena_limit)`, which rewinds the bump pointer to `arena_base`, and the first in-eval `cel_alloc` will zero-fill the bytes you just wrote. So you grow memory beyond `arena_limit` (which was set as `mem_size_bytes` at codegen, = the initial-memory size at Plan time) and put bindings there. The wasm-side `cel_alloc` bounds-checks against `arena_limit` and so never touches the host-arena tail.

**Verdict: this is a workaround, not a design choice.** It works, but: (a) `wasmtime_memory_grow` only ever extends, so successive Evals with growing strings will permanently inflate the instance; (b) the bookkeeping (`host_string_arena_floor`, `host_string_arena_capacity`, per-call `arena_cursor`, the `TotalHostStringBytes` pre-pass to avoid invalidating the data pointer) is non-trivial and lives in `instance.cc` rather than in the arena module; (c) it imposes a phase-ordering invariant: "grow the arena before any encoder runs" — get the order wrong and `wasmtime_memory_data`'s cached base pointer is stale and you scribble into freed memory.

The cleaner factoring would have been: "the host provides bindings as wire-CelValue payloads in host memory, separately from the wasm-owned linear memory." That would have made the wasm-side memory *just* a workspace, not a shared host-and-guest address space. But that conflicts with §2.3 (one shared address space). So host_string_arena exists.

### 2.5 `cel_host.cc` (3,528 lines)

Past the point where it should be split. There's a natural seam:

  - `cel_host_proto.cc` — `ProtoBacking`, `ProtoMap`, `ProtoList`, `ReadField`, `SetScalarField`, `UnpackAnyToValue`, the WKT peelers (~1500 lines).
  - `cel_host_host.cc` — `HostMessage`, `HostMap`, `HostList` non-proto backings (~400 lines).
  - `cel_host_field_dispatch.cc` — `CelGetFieldImpl`, `CelHasFieldImpl`, `RunFieldPrelude`, `EffectiveSelectAttribute`, `ResolveAttribute` (~300 lines).
  - `cel_host_aggregate_ops.cc` — every `CelListXxxImpl` / `CelMapXxxImpl` (~600 lines).
  - `cel_host_message_ops.cc` — `CelMakeMessageImpl`, `CelSetFieldImpl`, message eq (~300 lines).
  - `cel_host_time.cc` — every `CelTimestampXxxImpl` / `CelDurationXxxImpl` (~300 lines).
  - `cel_host_codec.cc` — `EncodeValue`, `EncodeSpan`, `EncodeFieldResult`, `DecodeKey`, `WriteWireBool`/`Int`, `EncodeBackingScalar` (~400 lines).

The reason the split didn't happen is that the file-private anonymous-namespace helpers (the `static` MakeError/FieldNotFound/UnpackWrapperMessage/... family) are referenced from all over. Pulling them into a header turns them into either `inline` definitions or a separate TU's exports — both of which require some refactoring. But it's mechanical refactoring. The fact that `cel_host.cc` doubled from M3 (when it was already big) to M7B+ without anyone reaching for the split is the canonical "growing tech debt" story. **Do the split before M10 lands more codecs.**

### 2.6 3-tier value representation (rodata / workspace / arena)

The boundary IS clean *at the LayoutPass level*. `Storage{kStaticRodata, off}` for kConst, `Storage{kWorkspaceSlot, off}` for kSelect/kCall/aggregates, `Storage{kLocal, idx}` for kIdent. Each node has exactly one storage tag, set in one of three visitor passes. This part is good.

Where it gets muddled is *runtime allocations through `cel_alloc`*: aggregate values like maps/lists allocate their entry storage from the arena (not from a workspace slot), and string concat results land in the arena. So there's a 4th tier — arena allocations from inside kernels — that LayoutPass doesn't know about. This is correct (you can't know aggregate sizes at compile time) but it means the cute "3-tier value rep" mental model is actually 4-tier with a phase boundary: LayoutPass owns 3 tiers statically; the arena owns the 4th dynamically.

The `Repr` enum (`kBool`, `kInt`, `kString`, `kList`, `kMessage`, …) is propagated to every annotation so codegen can dispatch on it. This is also clean — fewer of the wire-CelValue's `payload` union accesses are needed if you statically know the repr.

**Where it breaks down:** kComprehension introduces scope, and scope means a workspace slot can be re-acquired across iterations. The SlotAllocator handles this (`Acquire` / `Release`), but the boundary between "this slot is dead after the kCall" and "this slot is alive for the rest of the comprehension body" lives in `AggregateStorageVisitor::PostVisitCall` (`ReleaseIfWorkspaceSlot` for each arg before `slots_.Acquire()` for the result). That's correct *only because* the AST traversal is post-order — change the visitor scheme and the whole slot allocator's correctness assumption breaks silently. There's no test that pins "every kCall's arg slot is released before the kCall's result slot is acquired" — that's an emergent property of the visitor order.

### 2.7 `Allocator&` (`WasmtimeArenaAllocator`)

It's an abstraction over the `cel_alloc(size) → offset` reentry into wasm. The Layer 2 impl calls `alloc.Alloc(len, &out_offset)` rather than directly calling `wasmtime_func_call`. Pros: testable without wasmtime (cel_host_test_fakes.h presumably has a fake `ArenaAllocator`); switching runtimes would only touch Layer 3. Cons: it carries weight only because Layer 2 is being tested independently of wasmtime — if you commit to wasmtime, the abstraction is dead weight.

I'd keep it. The abstraction cost is ~20 lines and the test-independence payoff is real (the cel_host_test_fakes pattern is one of the things this codebase got right).

---

## 3. Implementation critique

### 3.1 The inline-asm opacity barrier in `cel_memory.c`

```c
uint8_t* cel_memory_base_(void) {
  uintptr_t p = 0;
  __asm__("" : "+r"(p));
  return (uint8_t*)p;
}
```

**This is a clang bug-workaround masquerading as a memory accessor.** The comment is accurate: without the asm barrier, clang's wasm32 backend treats `(uint8_t*)0 + off` writes as UB and elides them. This isn't a "deeper issue" in the design — it's a real backend miscompile. The cleaner fix would be using `__builtin_wasm_memory_size(0)` semantics or declaring an extern symbol the linker resolves to offset 0 (wasm-ld supports this). Even better: just take a `void*` parameter into every host function and let wasm32 thread it explicitly.

That said, the workaround is fine for now. The risk is that a future clang version's optimizer gets smarter about volatile-vs-asm and the barrier stops working. There IS a regression test (`cel_runtime_wasm_test.cc`, tagged `manual`) that asserts the bytes-8/12 mutation actually happens; that's the right guardrail. **Verdict: ugly but acceptable; the test pins it.**

The deeper issue this *points* at: the entire design relies on "address 0 is a valid pointer because wasm memory starts there." That's wasm-specific. If you ever want to run the runtime kernels natively (which the BUILD.bazel native target supports for testing — see the `g_memory[]` buffer fallback), you're already using a different code path. There's no clean abstraction that works for both; the C is conditionally compiled.

### 3.2 `EmitCelResetCall` and compile-time constant threading

```cpp
BinaryenExpressionRef EmitCelResetCall(WasmModule& mod, uint32_t arena_base,
                                       uint32_t arena_limit) {
  // (i32.const arena_base) (i32.const arena_limit) call $cel_reset
}
```

Called from `LowerToEvalFunction` as `EmitCelResetCall(mod, layout.arena_base, opts.mem_size_bytes)`. So `arena_base` comes from LayoutPass (computed as workspace_base + workspace_bytes, rounded up to 8), and `arena_limit` comes from LoweringOptions.mem_size_bytes.

**The coupling is real but not "hidden":** the same `mem_size_bytes` flows from `compile.cc` → `LoweringOptions` → here → `cel_reset` second arg → into the host's `wasmtime_memorytype_new(min=2, …)` (engine.cc) via constants. There are three places that need to agree on `2 * 64KB = 128KB`: (a) the constant baked into the i32.const at `cel_reset` call site; (b) the engine's `min=2` page-size; (c) the host_string_arena's "this is the floor" capture (instance.cc:388). If any one drifts, the system silently breaks — `cel_alloc` either bounds-checks against the wrong limit (corruption) or rejects valid allocations (FailedPrecondition).

I'd say this is over-coupled, not because the constants are individually wrong but because there's no single source of truth for "what's the initial memory size?" — three places have to be updated together. A `compiler_v2/runtime/memory_layout.h` with `constexpr uint32_t kInitialMemoryPages = 2; constexpr uint32_t kInitialMemoryBytes = kInitialMemoryPages * kWasmPageSize;` consumed from all three sites would be a five-minute fix that closes a real foot-gun.

### 3.3 `LayoutPass` and `arena_base`

```cpp
layout.workspace_base = RoundUp8(rodata_base + rodata.size());
// ... slot allocations happen ...
layout.workspace_bytes += slots.total_bytes();
layout.arena_base = RoundUp8(layout.workspace_base + layout.workspace_bytes);
```

The math is right but the brittleness is that `workspace_bytes` is updated *twice*: once after `ReserveVariableSlots` (variable slots), once after `SelectStorageVisitor`+`AggregateStorageVisitor` (workspace slots). If a future visitor allocates more workspace AFTER `arena_base` is computed, `arena_base` will overlap with workspace. There's no assertion that catches this. A `final_workspace_bytes_` setter that's called exactly once (and asserts on second call) would harden this; the current code relies on convention.

The bigger structural issue: LayoutPass runs N visitors over the AST (`ConstLayoutVisitor`, `IdentStorageVisitor`, `SelectStorageVisitor`, `AggregateStorageVisitor`, `ComprehensionLocalsVisitor`). Each is a separate `cel::AstTraverse` walk. Five walks. For most expressions this is fine; for deeply nested expressions it's measurably wasteful (the comment in `cel_pipeline_bench.cc` says Plan is shape-agnostic at ~12µs, but I'd bet a deep comprehension breaks that). The natural factoring is one visitor with five callbacks per node-kind. But that's a refactor with no functional payoff; flag it as polish, not bug.

### 3.4 The `WasmAnnotations` / storage-kind type system

I read `annotations.h` lightly. The design — keyed by `expr_id`, one `NodeAnnotation` per AST node, fields populated by successive passes — is standard and correct. The discipline is enforced by `ABSL_CHECK` at every consumer ("this node's storage kind must be X here") which means a pass that forgets to set a field will crash at the first read site rather than silently miscompile. This is excellent.

Where it breaks: `Repr::kUnknown` is the default. If a pass forgets to set Repr on a kCall result, downstream codegen will dispatch as if it's "untyped" — and the `OverloadTable` lookup will fall through. Mitigated because OverloadTable returns Status, but the failure mode is "your expression is unsupported" rather than "ResolvePass didn't tag node N's Repr." A pre-codegen audit pass that asserts `Repr != kUnknown` on every reachable node would catch ResolvePass regressions immediately.

### 3.5 `runtime/BUILD.bazel`

130+ explicit `--Wl,--export=` lines, ordered manually. Hardcoded `/opt/homebrew/opt/llvm/bin/clang`. `darwin-arm64` only. `tags = ["manual", "no-sandbox"]`.

**This is not a healthy build target.** It works because the project is one-developer, one-machine. It will break in CI the first time you try.

The export wall in particular is a code-smell: every new kernel touches BOTH this list AND `engine.cc::kRuntimeExports[]` AND the actual `.c` definition. Three places, no compile-time check that they agree. A possible fix: parse one of the lists (e.g., a wasm_imports.txt-style file listing exports) and generate the other two with codegen. The fact that the bench/runtime/host all duplicate the export list verbatim is duplication a CI grep-diff would catch — but better to remove the duplication entirely.

Apple-clang issue: real, but more limited than it sounds. `wasi-sdk` via `http_archive` would let CI build on Linux; the absolute path is a hack until the dep is properly declared.

### 3.6 "No lazy tracking of runtime imports"

The rule (memory: `feedback_no_lazy_imports.md`) is right for the stated reason — the alternative requires AST inspection at link time, and the dispatch table you'd need to lazy-link grows in lock-step with what you'd avoid. But it has costs:

  - Every `cel_host.*` trampoline is registered every Plan, even when the program doesn't use it. Cheap, but it's ~21 `wasmtime_linker_define_func` calls per Plan. In a microbenchmark this is measurable (the Plan_Hot ~12µs includes this).
  - Every kernel is in `cel_runtime.wasm`, so the binary grows monotonically.

For a CEL runtime where the entire ABI is ~150 exports, this is the right call. For a generic wasm AOT compiler with thousands of host operations, it wouldn't be. **Verdict: correct for this domain, but write it down as a domain-specific call.**

### 3.7 Comment density

20%. I sampled — most comments are load-bearing. The "split-plan §2 Risk #1 / #4" callouts, the "M5.B Slice C" milestone tags, the "Plan-vs-execution delta" callouts in design.md. These are pinning *why* a particular shape was chosen, which is the hardest information to recover from git history.

But a fraction is noise:

  - Multi-line comments at the top of obvious helpers (`I32Const` doesn't need 4 lines).
  - "Why we don't do X" comments where X is so obviously wrong that calling it out invites the question.
  - Milestone tags on lines that are now load-bearing across many milestones — "M5.B Slice C" loses meaning once you're at M10.

I'd call ~70% load-bearing, ~30% noise. The noise is harmless; the load-bearing fraction is unusually high and worth preserving.

### 3.8 Wartiest functions

Top 3 by line count + complexity:

  1. **`BuildConditionalArm` (instance.cc:???, ~289 lines)** — by far the longest function. I didn't read it in detail but the name suggests it's building the marshal path for one arm of a conditional Activation pipeline. Anything 289 lines deserves to be a class with private methods, not a function.
  2. **`MaybeEmitWktUnwrapTailCall` (expr_lower.cc, ~176 lines)** — codegen for the WKT-unwrap tail call. The "Maybe" prefix suggests it's a guard chain that does *something* in a non-default arm; 176 lines of guard chain is a smell.
  3. **`UnpackWrapperStringOrBytes` (cel_host.cc:210, 189 lines)** — handles the wrapper-message peel for the String/Bytes wrappers. Probably has 10+ cases for null/missing/empty/length-bounded variants per kind.

`SetScalarField` (165 lines) is also notable but it's a switch-on-cpp_type with each arm doing essentially the same shape (kind check, refl call). That's the kind of length that's hard to compress without obscuring intent.

The lint config has a 60-line / 40-statement / 15-branch gate; these are clearly NOLINT'd or in lint-backlog.md. The fact that `BuildConditionalArm` lives at 289 lines is consistent with "we've shipped while waving at the lint backlog."

---

## 4. Tech debt — confirm/refute

### 4.1 Bump arena makes vendoring C++ libs painful — CONFIRMED

The bump arena is `cel_alloc(n) → offset` returning a u32. Any C++ library you'd want to vendor (RE2 for `matches()`, parts of abseil for time formatting) uses `malloc`/`new`/`operator new` and a real heap. You can't link `re2.a` against a freestanding wasm32 build with `-nostdlib`; you'd need to either (a) hand-roll a `malloc` shim that bumps the arena, (b) bring in a `dlmalloc` and a separate heap region, or (c) wasi-sdk-style libc.

Option (a) breaks because RE2 holds allocations across calls (compiled regex objects), and the arena rewinds every `$eval`. You'd be allocating into a region that gets zapped under the C++ runtime's feet.

Option (b) is what `wasi-sdk` does. It's not a tiny refactor — you'd lose the "bytes 8/12 cursor" model and need to negotiate a real heap layout. The host_string_arena hack would also need to be rethought (it lives above arena_limit precisely because the arena pretended to own everything below).

The third option (`wasi-sdk`-style) is closest to the right answer if you ever want to vendor regex.

### 4.2 `cel_reset` prologue + `arena_base` compile-time threading is over-coupled — CONFIRMED

Per §3.2 — three sources of truth for "what's the memory layout?" with no single point of correctness. The fix is mechanical (one header with constants).

### 4.3 `host_string_arena` is an artifact of the arena lifecycle — CONFIRMED

Per §2.4. If activation bindings were marshalled into host-owned memory (not the wasm linear memory) and the host trampolines read them from host pointers, you wouldn't need it. But that conflicts with the shared-memory choice.

### 4.4 `cel_host.cc` is past the split point — CONFIRMED

3,528 lines and growing. The split is mechanical (§2.5). Doing it costs maybe a day of refactoring; not doing it costs every contributor ~20% extra navigation overhead and creates merge-conflict surface.

### 4.5 Inline-asm in `cel_memory.c` is brittle — PARTIALLY CONFIRMED

Brittle to clang versions (could break on a future optimizer pass), but well-pinned by the cross-built wasm test. Not high-priority.

### 4.6 `ExternrefTable` — used and load-bearing

Looking at the references: it's used for `kHostMessage`, `kHostMap`, `kHostList` backings — anything where the actual data lives outside the bump arena and gets a slot id baked into the wire CelValue's `payload.ref_slot`. `cel_host.cc` references it in ~13 places. Reset between Evals.

This is doing real work — without it, `Activation::Bind(Value::HostMessage(...))` has no way to flow a `Message*` through to the trampoline. The name is slightly misleading (it's not WASM externref, it's a slot table on the host side acting like one), but the abstraction is sound.

### 4.7 Benchmark numbers — plausible, but with caveats

  - `Engine::Build ~167µs`: dominated by `wasmtime_module_new` parsing 60+KB of wasm. Plausible. Mostly amortized across Plans.
  - `Plan_Hot ~12µs`: this is `wasmtime_store_new + memory_new + linker_new + RegisterCelLog + RegisterCelHostImports + linker_instantiate(runtime) + BindAllRuntimeExports + module_new(expr) + linker_instantiate(expr) + DecodeCelAbiFromWasm + BuildCelHostBindings`. 12µs is *very* fast for that much work. I'd want to verify on real hardware, but with the runtime module already-parsed (state.runtime_module is cached), the per-Plan instantiation in wasmtime is genuinely sub-10µs for small modules.
  - `Eval ~tens of ns`: this is *suspicious*. `wasmtime_func_call` itself has overhead on the order of 100ns minimum (entry, argument boxing, etc.). For a literal `42` the body is a single i32.const, so the wasm execution is free, but the call dispatch ought to be ≥100ns. The bench comment says "≈ tens of ns" — I'd verify this. If it's e.g. 30ns the wasmtime fast-path is faster than I think; if it's 300ns the bench comment is wrong.

I don't have the bench output to check absolute numbers; flag this as "verify before quoting in any external doc."

---

## 5. What I'd do differently starting over

1. **Make the host-side memory and the wasm-side memory separate.** The shared-memory choice was made for zero-copy ABI; the cost is host_string_arena, the i32.const arena_limit threading, and the inability to grow workspace dynamically per Activation. The alternative — host marshals bindings into a host buffer; trampolines copy into wasm memory at eval start — adds one memcpy per binding per Eval. For typical activations (< 10 bindings, < 1KB total) that's maybe 50ns. Worth it for the architectural cleanliness.

2. **Don't bake `arena_base`/`arena_limit` as i32.const in the module.** Pass them as globals (`global.get $arena_base`) the host sets at instantiation. Two-line codegen change, removes the constant-threading problem, and lets a single compiled Program serve Plans with different memory budgets.

3. **Treat `cel_host.cc` as four files from day one.** The split is obvious in retrospect (per §2.5). Establish the seam early so it doesn't grow welded.

4. **`wasi-sdk` from M1, not freestanding `-nostdlib`.** Trades ~50KB of libc for the ability to vendor C++ libraries. RE2 / abseil-time / wasi-clock-realtime all become straightforward. The `cel_memcpy_internal_` / `cel_memset_internal_` shims in `cel_internal.h` vanish.

5. **Single source of truth for runtime exports.** A `.txt` file (or a `.proto` enum) consumed by both BUILD.bazel and `engine.cc::kRuntimeExports[]` via codegen. Removes the duplication and the silent-drift footgun.

6. **The 5-visitor LayoutPass should be one visitor.** Mechanical refactor, removes 5 AST walks per compile. Probably 2x faster on big expressions, harmless on small ones.

I would NOT change:

  - The Layer 1 backings being virtual (the as-shipped delta from the original free-function design was the right call — see design.md plan-vs-execution delta at §4.7.6.1).
  - The 3-tier rodata/workspace/arena storage discipline. It's good.
  - The wire CelValue ABI (24 bytes, 8-aligned, kind+payload union). It's compact and the alignment story holds together.
  - The two-instance runtime+expr split. It's the right cold-start tradeoff.
  - The `OverloadTable` + `ResolvePass`/`LayoutPass` split. Clean.

---

## 6. What's good and worth preserving

1. **The pipeline shape.** frontend → resolve → layout → emit is the right factoring. `expr_lower.cc` reads as "switch on kind, look up annotation, emit Binaryen." That's exactly what compilers should look like.

2. **`WasmAnnotations` as the single per-node fact table.** The decision to not have a separate `SymbolTable` — to make `NodeAnnotation` carry everything keyed by `expr_id` — is unusually clean for a compiler IR. Every pass extends the same struct rather than maintaining sidecar maps.

3. **The C runtime kernels.** `cel_arith.c` (299), `cel_compare.c` (273), `cel_string_ops.c` (239), `cel_3vl.c` (238), `cel_time.c` (599). These are short, focused, and exhaustively unit-tested (the BUILD.bazel test layout is one-test-per-header). The `static inline` helpers in `cel_internal.h` (`absorb_3vl_binary`, `require_kinds`, `write_int`, etc.) are exactly the right abstraction level — they read like spec-aligned predicates.

4. **The kArena / kHost / kDynamic three-path dispatch for aggregates.** This is the architectural insight that made M3+M4 ship cleanly. The arena fast-path (`cel_list_at_arena`) inlines; the host path goes through a trampoline; the dynamic path tail-calls into one of the two. `__attribute__((musttail))` gives you zero-cost dispatch. Clever and correct.

5. **The closeout discipline.** CLAUDE.md's rules about per-component-test-coverage, the "manual-tagged tests carry load-bearing assertions" rule, the closeout gate. These are the rules that made M2 actually ship after its first half-shipped attempt. They're worth keeping.

6. **The Layer 1 / Layer 2 / Layer 3 host stack** — when factored correctly (§2.1). The current Layer 2 is bloated but the layering decision is right.

7. **`StaticMemoryBuilder`-style packed rodata.** The kConst-into-rodata pass means every literal is a `(i32.const offset)` and the wire CelValue lives in the data segment. Zero-cost at runtime, deduplicable, ABI-stable.

8. **The bench harness as a planning tool, not a CI gate.** The comment in `cel_pipeline_bench.cc` explicitly says "No CI gate on absolute numbers; bench is a planning + regression investigation tool." This is mature. Most projects either ignore perf or perf-test in CI with brittle absolute thresholds; this is the right middle ground.

9. **`wat_runner` + WAT-first design discipline.** Per CLAUDE.md: WAT lands in `doc/.../wat/`, assembled with `wasm-as`, run through wasmtime via a harness, BEFORE the codegen C++ is written. This forces ABI questions to be answered at WAT-authoring time, not at debug-time-during-codegen. It's the single most underrated decision in this project.

10. **The "stub until Mn" CHECK discipline.** `ABSL_CHECK(false) << "X is a stub until Mn"` everywhere a future-milestone path is reachable. Every "TODO" in this codebase crashes at the call site naming the symbol and milestone, rather than silently miscompiling. Wonderful.

---

## Bottom-line action list (if you wanted to spend a week)

In order of payoff:

1. Split `cel_host.cc` along the §2.5 seam. (1-2 days, removes ~30% of "where do I find this?" friction.)
2. Single source of truth for runtime exports. (0.5 day, removes a real footgun.)
3. `memory_layout.h` with the three constants that today live in three places. (1 hour, removes a different real footgun.)
4. Remove the 5-walk LayoutPass — merge into 1. (1 day, perf polish.)
5. Audit `Repr::kUnknown` reachability — add an assertion pass. (0.5 day, catches future ResolvePass regressions.)

Everything else is M11+ territory: wasi-sdk migration, per-Activation memory sizing, host-and-wasm memory separation. Those are project-shape decisions, not feature work.
