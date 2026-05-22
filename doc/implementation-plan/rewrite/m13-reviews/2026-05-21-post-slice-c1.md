# M13 post-Slice-C.1 review — 2026-05-21

## Verdict (one paragraph)

**Mixed, leaning clean.**  Slice C.1 ships engine-owned custom-fn
state — `Engine::AddModule` (foreign-module registration) and
`Engine::AddFunction` (host-callback registration) — with sound
threading discipline, reasonable wasmtime resource ownership, and
9 new tests covering the registration matrix.  Three concerns
stand out: (1) the host-callback trampoline relies on the *caller*
exporting "memory" — which probe 5's WAT did but the production
expr codegen does NOT (`compiler_v2/codegen/module.cc` only ever
imports memory).  The smoke test never invokes the callback, so
this is latent — but the moment a real C.2/C.3 slice actually
fires a `cel_fn.<id>` import, it traps with "caller lacks `memory`
export".  (2) `IsReservedAlias` is missing `wasi_snapshot_preview1`,
which wasmtime's `define_wasi` already owns on the linker — a
collision would surface only at Plan time, not registration.  (3)
The design doc `m13-custom-fns.md` §12 still describes Slice C as
a single slice with CLI + `Compiler::Builder::AddLibrary` + host
backend + e2e; the C.1/C.2/C.3 split is documented inline in
`engine.h:71-75` but nowhere in the plan doc.  Top three to look
at first:

  1. **Trampoline `memory` export gap** (`engine.cc:530-535`) — P1.
  2. **`wasi_snapshot_preview1` not on `IsReservedAlias`** (`engine.cc:462-465`) — P1.
  3. **Slice plan doc doesn't reflect C.1/C.2/C.3 split** (`m13-custom-fns.md:1875-1879`) — P2.

## Architectural drift

### A1. Public API drift from `m13-custom-fns.md` §6 worked example

The design doc (lines 225-235) shows:

```cpp
ASSERT_OK(engine.AddModule("rules", rules_wasm_bytes));
ASSERT_OK(engine.AddFunction("upper_string", upper_impl));
```

The shipped API requires three args, not two:

```cpp
// engine.h:148-149
absl::Status AddFunction(absl::string_view overload_id,
                         uint8_t num_args, HostCallback impl);
```

The `num_args` argument is load-bearing — it's what builds the
wasm functype for `cel_fn.<overload_id>` — but the design says
the engine resolves arity from somewhere else (the
`OverloadTable`).  As shipped, the user supplies it directly,
which is fine for Slice C.1's raw-shape API but conflicts with
the typed `cel::FunctionImpl` Slice C.2 docs in `engine.h:71-75`.
The C.2 layer will need to look up arity from the overload table
(or from the `FunctionLibrary` passed to the compiler) and pass
it through automatically — the user-facing `AddFunction` won't
take `num_args`.  Doc, not code: update §6 worked example to
match the raw shape OR clarify it's the C.2 typed shape.
**P2, ~10 min.**

### A2. C.1/C.2/C.3 sub-slice split is undocumented in `m13-custom-fns.md`

`engine.h:71-75` and `engine.cc:454, 685-686, 733` reference
"Slice C.1" as a discrete unit shipping the raw `HostCallback` +
engine-owned state, deferring the typed `cel::FunctionImpl`
overlay to "Slice C.2."  The design doc §12 (line 1875-1879)
still describes Slice C as one slice covering CLI flag +
`Compiler::Builder::AddLibrary` + `OverloadTableBuilder::RegisterCustom`
+ `cel_fn.*` trampolines + e2e tests — none of which landed in
C.1.  **P2** doc drift; should be reconciled before C.2 starts
so a future reader can see the as-shipped slicing.

### A3. Trampoline shape disagrees with `cel_host` Layer-3 shape

`HostCallbackTrampoline` (`engine.cc:518-551`) gets at memory via
`wasmtime_caller_export_get(caller, "memory", ...)`.  The existing
production `cel_host` callbacks (`internal/cel_host_wasmtime.cc`)
use the inverse pattern: `env->memory` cached on
`InstanceImpl::host_env` at Plan time, accessed via
`wasmtime_sharedmemory_data(mem_)`.  Probe 5's caller WAT
intentionally re-exports memory to make the
`wasmtime_caller_export_get` shape work
(`doc/.../wat/m13_p5_caller.wat:52` — `(export "memory" (memory 0))`).
**Production expr modules do not re-export memory**
(`codegen/module.cc` only ever calls `BinaryenAddMemoryImport`
in the expr path — `ExportMemory` is wired but unused for expr
codegen).  Consequence: when Slice C.2/C.3 wires a real
host-callback-using expr program, the first call into the
trampoline traps with `"host callback: caller lacks 'memory'
export"`.

The smoke test `PlanStillWorksWithRegisteredModuleAndCallback`
(`engine_test.cc:329-353`) compiles "42" which never imports
`cel_fn.*`, so the trampoline never fires and the bug is
dormant.  **P1, ~30 min** — either (a) switch the trampoline to
the `env`-pointer shape `cel_host` uses (pass
`&impl->host_env.memory` as part of a wrapped env struct and
read via `wasmtime_sharedmemory_data`) or (b) have codegen
emit `BinaryenAddMemoryExport(module_, "0", "memory")` so the
expr module re-exports its imported memory.  Option (a) is
preferable — it's the shape that already works in production
for cel_host trampolines, and it avoids polluting the expr
module's export surface.

## Tech-debt inventory

### T1. `SnapshotFunctionExports` is dead code — P2

`engine.cc:495-511` defines `SnapshotFunctionExports(ctx, inst)`
walking instance exports via `wasmtime_instance_export_nth`.  No
caller — `AddModule` (engine.cc:777-786) uses the *module*-level
`wasmtime_module_exports` walk instead (no instance needed).
Grep-confirmed unused.  Safe to delete.  Effort: 1-line removal.

### T2. `IsReservedAlias` missing `wasi_snapshot_preview1` — P1

`engine.cc:462-465` lists `cel`, `cel_host`, `cel_env`, `cel_fn`,
`host` as reserved.  But `Engine::Plan` (via `InitLinker` →
`RegisterWasiStubs` at `engine.cc:59-65`) calls
`wasmtime_linker_define_wasi(linker)`, which populates the
linker with `wasi_snapshot_preview1.*` defs.  An
`Engine::AddModule("wasi_snapshot_preview1", bytes)` would be
accepted at registration time and surface a collision in
`InstantiateAndBindCustomModules`'s
`wasmtime_linker_define(linker, ctx, "wasi_snapshot_preview1",
..., helper, ...)` call only at Plan time — and only if the
custom module exports a helper whose name happens to collide
with a wasi function (e.g. `proc_exit`, `fd_close`).  The
diagnostic would be confusing.

Also consider: the runtime is built against wasm-threads + might
import `wasi_unstable` in some toolchain configurations
(`engine.cc:79-86`); worth adding too if the build flips.
**P1, ~5 min** — add `wasi_snapshot_preview1` to `IsReservedAlias`.

### T3. `RegisterHostCallbacks` iterates with `auto&` but only reads — P2

`engine.cc:617`: `for (auto& [overload_id, rcb] : state->host_callbacks)`.
Only `rcb.num_args` and `rcb.callback` are read; nothing mutates
`rcb`.  Should be `const auto&` to make the contract explicit
("Plan does not mutate engine state").  This matters for the
header's threading contract — a reader of
`engine.cc:615-630` sees a non-const reference into shared engine
state from a `const`-method (Plan) and has to follow through to
verify no mutation occurs.  **P2, 1-line fix.**

### T4. `Engine::AddFunction` signature mismatches `.h` declaration on `num_args` — P2

Header (`engine.h:148-149`) declares
`AddFunction(..., uint8_t num_args, ...)`.  Implementation
(`engine.cc:792-793`) defines
`AddFunction(..., std::uint8_t num_args, ...)`.  Same type
(`uint8_t` = `std::uint8_t`), but cosmetically inconsistent —
clang-tidy may flag this once landed.  Trivial.  **P2, 1-line fix.**

### T5. `wasm_functype_t` ownership pattern is correct but worth a comment — P2

`engine.cc:618-623`: builds a `wasm_functype_t* ftype` per
callback, passes it to `wasmtime_linker_define_func`, deletes
it.  Verified consistent with the codebase: same delete-after-
define pattern in `host/cel_log.cc:437-440`, `tools/wat_runner/
wat_runner.cc` (6 sites), `runtime/cel_runtime_wasm_test.cc:270-273`,
and the m13_p5 probe.  Wasmtime's linker takes ownership of the
type info it needs at registration time (per linker.h:131-133
"creates a store-independent function within the linker").
**No action**, but the trampoline-side comment in `engine.cc:614`
could mention this invariant for the next reader.

## Coverage gaps

### C1. No test exercises the host callback through Plan → Eval — P1

Eight of the nine new tests assert registration-time behaviour
only (`AddModule` accepts/rejects, `AddFunction` accepts/rejects).
The ninth (`PlanStillWorksWithRegisteredModuleAndCallback`,
`engine_test.cc:329-353`) checks that Plan does not regress when
a module + callback are registered but the program doesn't
import them.  Nothing tests:

  - A wasm program that actually imports `cel_fn.<id>` and
    invokes it (would surface the memory-export bug A3
    immediately).
  - The trampoline's arg-decode loop (out_slot is args[0], rest
    are arg_slots — verified only in probe 5).
  - The trap-from-Status return path (callback returns
    `absl::InvalidArgumentError(...)` → wasm trap with that
    message).
  - The `mem_size = 0` boundary (memory not actually accessible).

This is the load-bearing coverage gap for C.1.  Adding a synthetic
WAT that imports `(import "cel_fn" "test" (func ...))` + a
callback that asserts on out_slot and writes a known value would
exercise the full path.  **P1, ~45 min** — write a synthetic
caller WAT (re-exporting memory if A3 isn't fixed first) +
register + Plan + Eval + decode the result, plus a "callback
returns error" variant.

### C2. No test for `AddModule` exporting a function whose name starts with non-alphanumeric — P2

`engine.cc:783`: `if (nm.empty() || nm[0] == '_') continue;` —
filters out toolchain noise.  What about names starting with a
digit (impossible per wasm spec but worth a defensive test),
empty names (filtered), names containing dots/dashes
(`cel-runtime/foo`)?  The current filter is a single
`nm[0] == '_'` check.  **P2**, low risk because wasm validators
already constrain export names.

### C3. No test that two `AddModule` calls with the SAME bytes but DIFFERENT aliases both succeed — P2

Both alias slots get the same parsed `wasmtime_module_t*` — but
since `entry.module` is re-parsed via `wasmtime_module_new` per
call, they're independent.  Easy regression: if a future
refactor caches parsed modules by bytes-hash, this could
accidentally share `wasmtime_module_t*`s with conflicting
helper_exports lists.  Worth a single TEST_F asserting two
different aliases for the same module bytes Plan cleanly.
**P2, ~10 min.**

### C4. No concurrent `Plan` test with registered customs — P2

`EnginePlanThreadingTest::ConcurrentPlanCallsAllSucceed`
(`engine_test.cc:129-174`) covers the no-customs path.  No
concurrent-Plan test with `AddModule` + `AddFunction` registered
beforehand.  The threading contract specifically calls out that
Plan-with-customs is concurrent-safe ("Configure once at startup,
then `Plan` from many threads" — engine.h:131-133), and the
implementation reads from `state->custom_modules` and
`state->host_callbacks` concurrently from per-Plan linker
definitions.  Tests should mirror this.  **P2, ~20 min** — extend
the existing concurrency test or add a `PlanWithCustomsThreadingTest`.

### C5. No test that destruction order of Engine vs Instance handles custom modules cleanly — P2

`Engine::~Engine` drops the user's handle but the shared_ptr to
`WasmtimeEngineState` keeps state alive for outstanding Instances
(verified by `InstanceOutlivesEngineAndCompilerWithEvalProof`,
`engine_test.cc:176-214`).  The same test pattern but with
`AddModule` + `AddFunction` called first would catch a regression
where `custom_modules` / `host_callbacks` accidentally get
destroyed before pending Instances run.  Specifically: the
linker on `InstanceImpl` captured `&rcb.callback` as env; if
`state->host_callbacks` were destroyed first, the env pointer
would dangle.  Today this is safe (the shared_ptr keeps state
alive), but a test pins the invariant.  **P2, ~15 min.**

## Doc drift

### D1. `m13-custom-fns.md` §12 Slice plan doesn't reflect C.1/C.2/C.3 split

Lines 1875-1879 still describe Slice C as a monolithic unit
("CLI flag, `Compiler::Builder::AddLibrary`,
`OverloadTableBuilder::RegisterCustom`, `cel_fn.*` trampolines in
the host adapter, …").  As shipped, C.1 landed only `Engine::Add*`
+ trampoline + engine-owned state — none of CLI / AddLibrary /
RegisterCustom integration.  `engine.h:71-75` mentions C.2's
typed `cel::FunctionImpl` overlay; nothing mentions C.3.

**Fix direction**: rewrite §12 Slice C bullet as three
sub-bullets (C.1 shipped, C.2 pending: typed FunctionImpl
overlay, C.3 pending: Compiler::Builder::AddLibrary +
celwasmc CLI flag + e2e), and tag C.1 with "shipped
2026-05-21".  **P2, ~15 min.**

### D2. `m13-custom-fns.md` §6 worked example shows 2-arg `AddFunction`

Line 235: `ASSERT_OK(engine.AddFunction("upper_string", upper_impl));`.
Shipped API takes 3 args (`overload_id`, `num_args`, `impl`).  Per
A1, this is the typed C.2 shape, not the raw C.1 shape; doc should
either say which it's depicting or update to match what shipped.
**P2, ~5 min.**

### D3. No `wat-traces.md` entry for the production trampoline shape

The trampoline (`HostCallbackTrampoline`) is a new ABI surface —
caller passes out_slot + arg_slots as i32s, host reads from
caller's exported memory, writes back, returns a wasm trap on
error.  Per CLAUDE.md "WAT-first" rule
("Before implementing any new codegen arm or host-ABI surface,
write the target wasm in WAT first"), the trampoline ABI should
have a `.wat` file + `wat-traces.md` entry pinning the shape.
Probe 5's WAT exists, but it's tagged as a probe; the production
trampoline's behavioural envelope (memory access pattern,
arg-slot decoding, trap-on-error) lacks a non-probe trace.
**P2, ~30 min** — promote `m13_p5_caller.wat` into a
non-probe WAT trace, citing the production trampoline impl, OR
note in `wat-traces.md` that p5 IS the production trace
(but A3 — the memory-export shape difference — needs reconciliation
first; if A3 fixes the trampoline to use `env->memory`, the
WAT shape changes and p5 is no longer authoritative).

### D4. `cel-host-surface.md` not yet updated for `HostCallback` / `Engine::Add{Module,Function}`

`engine.h:43-78` adds two new public types and methods.
`cel-host-surface.md` (the system-of-record for the public
`cel::` surface) was not touched in this slice — quickly
verified by grep (no `HostCallback` mention).  Per CLAUDE.md
"any doc that named the old shape gets updated in the same
commit," the new surface should land in cel-host-surface.md.
**P2, ~10 min** — add a §X subsection for `cel::HostCallback` +
the new `Engine` methods.

## Summary tracking

P0 (ships-breaking, before C.2):
  - none.

P1 (must-fix-before-Slice-C.2):
  - A3 / C1 (trampoline `memory` export shape + missing
    end-to-end host-callback test — these are the same root
    issue; fix together)
  - T2 (`wasi_snapshot_preview1` in `IsReservedAlias`)

P2 (cleanup-when-touched):
  - A1 (doc §6 worked example mismatch)
  - A2 / D1 (C.1/C.2/C.3 split not documented in slice plan)
  - T1 (`SnapshotFunctionExports` dead code), T3
    (`const auto&` in RegisterHostCallbacks), T4 (header vs
    impl `uint8_t` cosmetic), T5 (functype ownership comment)
  - C2 (export-name filter edge cases), C3 (same-bytes-two-
    aliases), C4 (concurrent Plan with customs), C5 (Instance
    outlives Engine with customs)
  - D2 (doc §6 AddFunction signature), D3 (production-trampoline
    WAT trace), D4 (cel-host-surface.md missing the new surface)

None of the P1 items are ships-breaking — the smoke test passes
because the production callback path is never exercised yet.
But A3/C1 must land in C.2's first commit; otherwise C.2's
typed-FunctionImpl overlay will sit on top of a broken raw
trampoline and the bug surfaces at the *end* of C.2 instead of
the start.  Fix A3 + write C1's missing test FIRST in C.2.

— review carried out by Claude Opus 4.7 (1M ctx) per the periodic
   code-review rule in CLAUDE.md.
