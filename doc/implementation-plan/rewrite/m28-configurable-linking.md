# m28 — Configurable Linking

Status: **shipped 2026-06-09.**  Prototype 2026-06-08; default flipped to `kStatic` same day (§5.1 delta); dual-mode e2e infrastructure + full dual-mode conformance (1899/463/92, byte-identical — §7.3) + all §13 P1/P2 follow-ups closed 2026-06-09.  Production three-way bench over the full 232-cell corpus in `m28-bench-results.md` — headline: 17–22× faster than cel-cpp on 1000-term chains (supersedes the prototype's ~31× claim, which did not reproduce), corpus-wide geomean parity (0.95×) with honest two-sided win/loss analysis.  Architecture differs from the as-drafted plan in §5.2.1 (strip-tool pass list), §5.3 (merge strategy + `InstallStructImports` fold), §5.4 (`ModuleImportsCelNamespace` replaces the byte-walker; `__wasm_call_ctors` defense-in-depth ultimately shipped after a P1→P2→shipped arc); see the in-line "Plan-vs-execution delta" callouts.  Remaining before master merge: the branch is ~30 commits behind origin/master (sync required; parser `max_recursion_depth` fix already mirrored).

**As-shipped summary (2026-06-08):** the prototype lands `LinkMode::kStatic`
end-to-end (Compile → Plan → Eval), with 51/51 `e2e/` tests green in both
modes.  The shipped shape diverges from the as-drafted design in three
material ways — strip-tool pass list (§5.2.1 delta), merge strategy
(§5.3 delta), and `CallInit` not yet implemented (§5.4 delta) — each
captured below.  The post-prototype review at
`doc/implementation-plan/rewrite/reviews/2026-06-08-m28-prototype.md`
catalogues nine load-bearing invariants the as-drafted plan did not
anticipate (see §10.1) and an 8-item P1 / 7-item P2 follow-up queue
(see §13).

## 0. TL;DR

Add an opt-in **static linking** mode to `Compiler::Compile`, with the existing **dynamic linking** behavior unchanged as default. Static mode produces a self-contained `Program.wasm` (the runtime helpers are merged in and the wasi-libc command-mode wrappers are stripped). Dynamic mode produces today's shape (expression wasm + separate `cel_runtime` instance). Engine detects which shape it's planning and routes to the appropriate path.

Measured upper-bound benefit on the cells already validated: **30× faster than cel-cpp on `intAdd1000Terms` and similar long chains, 2-30× faster across the rest of arithmetic + comparisons.** Lower-bound: at N=2 const cells, static-linked celwasm is within 0-17% of cel-cpp (we're at the wasm-boundary floor). Variable-bearing patterns are not yet validated — see §5.

This milestone covers landing the mode as a production-supported feature, including the Engine memory-model work that unblocks variable-bearing static-linked cells. It does **not** include the const-list-literal codegen optimization observed in §12 of `m28-wrapper-overhead-findings.md` (that's a separate future milestone).

## 1. Why now

The static-link investigation under `perf/binaryen-merge-proto` measured the architectural lever:

- The wasi-libc `.command_export` wrapper costs ~83 ns per host call, uniformly across runtime helpers (microbench).
- Stripping wrappers + merging cel_runtime into the expression wasm drops per-`+` cost from ~80 ns/op to ~1.5 ns/op (production bench, intAdd const cells).
- At 1000-term arithmetic chains, this puts celwasm at ~30× faster than cel-cpp — measured, not extrapolated.

These numbers were measured through the production `benchmark/eval/celwasm_bench` harness against `benchmark/eval/celcpp_bench`, both via Google Benchmark, identical wasmtime config, identical Engine instantiation. Full data in §11 of `FINDINGS.md`. The architectural change is real; this milestone productionizes it.

Two reasons it isn't already on master:

1. The prototype is wired entirely via external tools (`compile_to_wasm` driver + `wasm-merge` CLI + `wasm-opt`). Production needs the merge step to happen inside `Compile()` via the Binaryen library API.
2. The merged-module memory model breaks activation marshaling + result decode in today's `Engine`. The prototype validated arithmetic const cells (no activation needed); variable-bearing cells (the canonical embedder shape) trap or produce garbage results pending an Engine fix.

This milestone delivers both pieces and the configuration surface that exposes them.

## 2. Goals

1. `CompilerOptions::link_mode = kStatic | kDynamic` — embedders opt into static linking at Compile() time.
2. Engine routes both shapes transparently — same `Plan/Eval` API on both paths.
3. Existing embedders see no behavior change. Default stays dynamic; today's tests stay green.
4. Static mode's variable-bearing cells produce correct results (the memory-model fix lands).
5. Conformance suite (`spec/tests/`) green under both modes.
6. Bench cells (corpus-driven, see `benchmark/eval/DESIGN.md`) run on both modes; the comparison is part of the headline report.

## 3. Non-goals

- Removing the dynamic-link path. Both modes ship and stay supported. (Justified by the size/perf tradeoff table in §4.)
- Adding new CEL operators or extending the language surface.
- Const-list-literal codegen (a separate observation surfaced during this work; tracked as a candidate future milestone — see §10).
- Native (non-wasm) codegen backend. Out of scope here.
- Changes to the cel-cpp comparator side of the bench (`celcpp_bench`).

## 4. The two modes — measured tradeoffs

| | dynamic (default, today) | static (opt-in) |
|---|---|---|
| **per-Program wasm size** | ~10 KB (expression only) | ~800 KB (expression + linked runtime) |
| **per-op cost slope** | ~80 ns/op | ~1.5 ns/op |
| **per-Eval intercept** | ~60 ns | ~55 ns |
| **shared runtime across cached Programs** | yes (single instance) | no (each Program owns its runtime) |
| **best for** | many distinct expressions, memory-constrained hosts, config eval | hot eval loop, latency-critical paths, single expression evaluated millions of times |
| **measured vs cel-cpp on intAdd1000Terms** | 2.3× slower | **31× faster** |
| **measured vs cel-cpp on intAdd2 (var)** | 3.6× slower | **1.2× faster** |
| **measured vs cel-cpp on intAdd2Const** | 4.3× slower | 1.08× slower (parity-ish) |

Source for measured rows: `benchmark/eval/celwasm_bench` + `benchmark/eval/celcpp_bench` run at `--benchmark_min_time=0.5s` on 2026-06-06, recorded in `FINDINGS.md` §11.4 and §12.3.

> **Superseded 2026-06-09 by the full-corpus production run** —
> see `m28-bench-results.md` for the definitive three-way numbers
> over 232 cells.  Key corrections to the table above: the measured
> 1000-term advantage is **17× (int) / 22× (double)** vs cel-cpp, not
> 31× (the prototype number did not reproduce within ±10%; candidate
> causes recorded in the results doc §4).  Corpus-wide geomean vs
> cel-cpp is **0.95× (parity)**, two-sided: comprehensions/arithmetic
> win up to 48×, while maps / `in` / single-op floor cells lose
> 1.2–4× (constant-aggregate construction, SIMD-less byte scans, and
> the 62 ns boundary floor — causes in the results doc §3).

The two modes attack different embedder needs. They are NOT a transitional state — both ship indefinitely.

## 5. What needs to change

### 5.1 Compiler API surface

Add a configuration field on `CompilerOptions` (declared in `compiler/compiler.h`):

```cpp
struct CompilerOptions {
  int optimize_level = 0;  // existing

  enum class LinkMode : uint8_t {
    kDynamic,  // default: expression wasm imports "cel.*" helpers from a
               //   separately-instantiated cel_runtime
    kStatic,   // self-contained: helpers merged into the program, wrappers
               //   stripped, wasi-libc init runs once at instantiate
  };
  LinkMode link_mode = LinkMode::kStatic;
};
```

> **Plan-vs-execution delta (2026-06-08, simplification pass):** the
> as-drafted default of `kDynamic` ("every existing call site is
> unchanged") was **flipped to `kStatic`** the same day the prototype
> shipped (`compiler/compiler.h:144`, `compiler/internal/compile.h:102`).
> Rationale: the static-link path is the perf-dominant shape we
> productionised m28 around (see §4 table — 30× faster on long
> arithmetic chains, parity-or-better at N=2), and shipping with the
> slower path as the default would mean every embedder needs to opt
> in to the win.  This is a **behavioural change for embedders**:
> calling `Compile()` with no options now produces a self-contained
> ~800 KB Program rather than the ~10 KB dynamic-mode Program.  All
> 51/51 e2e tests stay green under either default (verified by
> temporarily flipping back to `kDynamic`).  Embedders that want the
> dynamic shape opt in via `opts.link_mode = LinkMode::kDynamic`.

### 5.2 Build-time artifact: `cel_runtime_stripped.wasm`

#### 5.2.0 Why post-processing (toolchain spike, 2026-06-07)

Before settling on a post-processing approach, we asked whether bazel + clang alone could produce a wrapper-free runtime — bypassing the need for any Binaryen tooling at our build. Two routes were tried and both are blocked by the wasi-sdk we ship:

**Route 1 — Reactor exec-model on the current target.** `-mexec-model=reactor` causes wasi-libc to emit a single `_initialize` export instead of wrapping every export. Trying it via `runtime/BUILD.bazel`'s copts:

```
clang: error: unsupported option '-mexec-model=' for target 'wasm32-wasi-threads'
```

The same rejection fires on `wasm32-wasip1-threads`. The clang driver explicitly forbids `-mexec-model=` on any `*-threads` target. We cannot use reactor mode without leaving the threads target.

**Route 2 — Switch to vanilla `wasm32-wasi` (no threads) + stub the mutex symbols.** This was the natural fallback: cel_runtime never spawns threads; we only need mutex symbols to satisfy cctz/absl. But the headers themselves are absent:

```
with_mutex.cc:3:13: error: no type named 'mutex' in namespace 'std'
```

wasm32-wasi's libc++ does not ship `<mutex>` headers (the type doesn't exist in `std::`). It's a *compile-time* failure, not a link-time one — stubs cannot help.

Both routes fail because of wasi-sdk policy choices we don't control. The only realistic ways to make routes 1 or 2 work would be to (a) drop cctz/absl entirely from cel_runtime, or (b) vendor a custom wasi-sdk variant. Both are out of scope for m28.

**Decision: post-process with Binaryen** — accept the cost of an extra build-time tool, keep the toolchain choice unchanged, ship the prototype's approach as production code.

#### 5.2.1 Build pipeline

Two build steps run at our build time (not at embedder Compile() time):

- New `cc_binary` `runtime/strip_command_wrappers` (port of `wasm_compilation_experiments/wrapper_overhead/strip_command_wrappers.cc`, ~90 lines, Binaryen C API only — no new dep). For every export `X` whose target is `X.command_export`, retargets the export at the bare body `X`; the wrapper becomes orphaned and Binaryen's `BinaryenModuleOptimize` DCE removes it.
- New genrule `runtime/cel_runtime_stripped_wasm.bin` runs the tool over `cel_runtime_wasm.bin`.
- New header `runtime/cel_runtime_stripped_wasm_bytes.h` embeds the stripped bytes (parallel to existing `cel_runtime_wasm_bytes.h`).

Both byte arrays ship in the compiler library. The Compiler picks which to embed at Compile() time based on `link_mode`.

> **Plan-vs-execution delta (2026-06-08):** the as-drafted plan said
> "Binaryen's `BinaryenModuleOptimize` DCE removes it"; the as-shipped
> tool runs `BinaryenModuleRunPasses(m, {"remove-unused-module-elements"}, 1)`
> instead, plus a process-global `BinaryenSetDebugInfo(1)` set before
> the write.  Why: `BinaryenModuleOptimize` is a catch-all that ALSO
> runs `merge-similar-functions` and inliners, which collapse
> semantically-identical exports (e.g. `cel_dur_to_int_at_v` and
> `cel_dur_seconds_at_v`, both `(local.get 0) (i64.load) → i32`) into
> a single body and retarget the dead twin's export to the survivor.
> Static-mode codegen then can't resolve `BinaryenCall("cel_dur_to_int_at_v")`
> because the function with that internal name is gone — failure
> surfaces at Compile time as a Binaryen validation error or, worse,
> a silent miscompile to the wrong helper.  The `BinaryenSetDebugInfo(1)`
> flag preserves the wasm `name` custom section across the write
> cycle so function names survive (without it Binaryen emits anonymous
> `$<index>` names and the compiler's intra-module `BinaryenCall("arena_reset", …)`
> calls fail to resolve in the adopted module).  Both pin-points are
> load-bearing invariants (#1 and #4 in §10.1).  Source:
> `runtime/strip_command_wrappers.cc:88` and `:132`.

### 5.3 In-Compile() merge step

When `link_mode == kStatic`, after Binaryen produces the expression wasm:

1. Load `cel_runtime_stripped` bytes into a Binaryen `Module`.
2. Add the expression's `$eval` function (and any siblings) into that Module by walking the expression's IR and emitting Binaryen IR against the same Module — referencing the runtime's helpers by Binaryen `Name` (intra-module) rather than by import.
3. Run `BinaryenModuleOptimize(module)`.
4. `BinaryenModuleAllocateAndWrite` → bytes → `Program(std::move(bytes))`.

This is the only step inside `Compile()` that diverges. Effort estimate: ~1 week.

Notes:
- We already link Binaryen for the dynamic-mode codegen. Static mode uses additional Binaryen C API entry points (`BinaryenModuleRead`, `BinaryenAddFunction`, etc.). No new external dep.
- We do **not** ship `wasm-merge` (the binary) as a runtime dep. The merge happens via Binaryen library calls only.

> **Plan-vs-execution delta (2026-06-08):** the as-drafted shape
> ("walk the expression's IR and emit Binaryen IR against the same
> Module — referencing the runtime's helpers by Binaryen `Name`") was
> replaced by a smaller and cleaner adopted-module shape.  As shipped
> (`compiler/internal/compile.cc::CompileStatic`):
> `WasmModule::Adopt(BinaryenModuleRead(kCelRuntimeStrippedWasmBytes, ...))`
> takes ownership of the stripped runtime's Binaryen `Module`; then
> the normal `LowerToEvalFunction` runs against the adopted module —
> the same lowering used for the dynamic-mode codegen, just targeted
> at the runtime's module instead of a fresh one.  Memory references
> in `BinaryenLoad` / `BinaryenStore` pass `nullptr` for the memory
> name (rather than the hardcoded `"memory"`) so the adopted module's
> only memory is targeted regardless of its internal name
> (`"0"` in wasi-libc / wasm-ld output vs `"memory"` in our fresh
> builds).  No new IR-walk emitter is needed; the existing codegen
> path is reused with two new entry-point surfaces on `WasmModule`:
> `Adopt(BinaryenModuleRef)` and `AddActiveDataSegment(offset, bytes, ...)`.
> See `compiler/codegen/module.h:42,137` and
> `compiler/internal/compile.cc:489-523`.

#### 5.3.1 Load-bearing invariants in the adopted-module path

The simpler adopted-module shape ships with four newly-load-bearing
invariants the as-drafted plan didn't anticipate.  Each is now a P1
follow-up for explicit assertion / focused regression test before
m28 closes (see review §2 for full rationale and §13 for the
follow-up items):

- Strip tool's pass list MUST NOT run `merge-similar-functions` /
  inliners (see review §2 invariant 1).
- All 11 codegen `BinaryenLoad` / `BinaryenStore` sites MUST pass
  `nullptr` for the memory name (see review §2 invariant 2).
- Strip tool MUST call `BinaryenSetDebugInfo(1)` before write so
  function names survive (see review §2 invariant 4).
- The adopted module's feature set is the UNION of declared +
  `DefaultFeatures()` — narrowing trips Binaryen's internal feature-
  dependency consistency check (see review §2 invariant 5).
- Expression rodata MUST fit in `[0, CELWASM_RESERVED_LOW_MEMORY_BYTES)`;
  the static-mode runtime's `-Wl,--global-base=8192` reserves the
  bottom 8 KiB but **nothing currently asserts** rodata stays in
  that window — silent corruption candidate (see review §2
  invariant 6).

#### 5.3.2 Dual `cel_host.*` import declarations are intended

The merged static-mode artifact carries **two** import-declaration
sets for `cel_host.*`:

- The runtime declares the `cel_host.*` trampolines it imports for
  its own use (16 of the 20 trampolines in our catalogue), under
  wasm-ld-assigned internal names like `$cel_host_cel_get_field`.
- The expression module declares the `cel_host.*` trampolines its
  codegen calls (8 of the 20, of which 3 overlap with the runtime's
  set), under the codegen-canonical internal names that
  `InstallCelHostImports` installs.

In dynamic mode the two modules are separate, and each declares its
own imports.  In static mode they merge into one artifact that has
both sets.  **This is wasm-spec-correct and is the natural
consequence of merging two modules that each import the same
external.**  The 3 overlapping triples appear twice with different
internal names; they resolve at instantiate time to the same host
C++ trampoline.  Per-Program cost is roughly 150 bytes of import
declarations and 3 extra linker lookups at Plan time.

`InstallCelHostImports` is called from BOTH `Compile()` and
`CompileStatic` (`compiler/internal/compile.cc:357,523`) — this is
the intended design.  The "obvious cleanup" of skipping
`InstallCelHostImports` in static mode (on the reasoning "the
runtime already imports those names") regresses static-mode codegen
because the runtime's import names are wasm-ld-assigned and don't
match the codegen-canonical names that the expression's
`BinaryenCall(kCelHostGetFieldInternalName)` targets.

The post-prototype review originally listed this as P2-1 / CHANGE I
(strip-time rename to eliminate the duplication); both have been
**retired** in §13 — the dual-import shape is not a bug, and the
strip-time rename would introduce coupling between the strip tool
and codegen's internal-name policy for negligible benefit.

#### 5.3.3 `InstallStructImports` folded into `InstallListImports` (simplification, 2026-06-08)

> **Plan-vs-execution delta (2026-06-08, simplification pass):**
> the standalone `InstallStructImports` helper (which had shrunk to
> a single import — `cel.cel_set_field_at_if_present`, the sole
> field-mutation host trampoline still imported by codegen) was
> **folded into `InstallListImports`** alongside the sibling
> `*_at_if_present` predicate imports.  Its name no longer reflected
> its content and the per-call indirection bought nothing.  The
> `InstallExprModuleImports` call list in `compiler/internal/compile.cc`
> dropped the `InstallStructImports` entry; the import installation
> now happens inside `InstallListImports` (`compile.cc:159`).
> Retires the review's P2-7 / P2-5 item.

### 5.4 Engine: unified Plan path (no `PlanStatic` / `PlanDynamic` bifurcation)

The first draft of this doc had `Engine::Plan` dispatch into two separate function bodies based on detected link mode. That was overscoped. The actual difference between the two modes is two conditional steps; the rest of `Engine::Plan` runs unchanged regardless of mode.

The simpler shape: **one `Plan` body, two conditional branches inside it.** Rename `InstanceImpl::runtime_instance` → `InstanceImpl::helpers_instance` to reflect that it's "wherever the runtime helpers live" — which is either a separately-instantiated cel_runtime (dynamic) or the program instance itself (static).

```cpp
absl::StatusOr<Instance> Engine::Plan(const Program& program) {
  auto impl = std::make_unique<InstanceImpl>();

  // ... existing setup (store, linker, host imports) — unchanged ...

  // BRANCH 1: only dynamic mode instantiates cel_runtime separately.
  if (HasCelImports(program.wasm_bytes())) {
    if (auto s = InstantiateRuntime(...); !s.ok()) return s;
    impl->helpers_instance = impl->runtime_instance;
  }

  // Instantiate the program. Linker resolves cel.* imports if present.
  if (auto s = InstantiateExpr(program); !s.ok()) return s;

  // BRANCH 2: static-mode helpers ARE the program instance; trigger
  // wasi-libc init once.
  if (!HasCelImports(program.wasm_bytes())) {
    impl->helpers_instance = impl->expr_instance;
    if (auto s = CallInit(impl->helpers_instance); !s.ok()) return s;
  }

  // ... existing bindings (memory, helper handles, arena seed) —
  // unchanged; they read from `impl->helpers_instance`.
  if (auto s = BindHelpersInstance(impl.get()); !s.ok()) return s;
  // ...

  return Instance(std::move(impl));
}
```

Everything downstream of the branch — `BindRuntimeMemory`, the helper-function-handle bindings, activation marshaling at `Eval` time, result decode at `Eval` time — reads from `impl->helpers_instance`. They don't care whether that instance is a separate runtime or the merged program. **One code path, parameterized by which instance to bind helpers from.**

Concrete diff to today's `eval/engine.cc`:

1. **Rename**: `InstanceImpl::runtime_instance` → `helpers_instance` (mechanical; touch sites listed below).
2. **Two `if` branches** in `Engine::Plan` around runtime instantiation and `_initialize` invocation, as shown above.
3. **`HasCelImports(bytes)`** detection helper — small, reads the wasm import section, returns true if any import is from module `"cel"`.
4. **`CallInit(instance)`** helper — looks up `_initialize` (or `__wasm_call_ctors`) export, calls it once with no args. Skips silently if export absent (dynamic-mode runtime doesn't have it).

Touch sites for the rename (audit before commit):
- `eval/internal/instance_impl.h` — the field
- `eval/engine.cc::InstantiateRuntime` (only the assignment changes)
- `eval/engine.cc::BindRuntimeMemory` (reads the field — rename only)
- `eval/engine.cc::BindAllRuntimeExports` (reads the field — rename only)
- `eval/engine.cc::SeedRuntimeArena` (reads the field — rename only)
- `eval/instance.cc` activation / result-decode code (reads the field — rename only)

Effort estimate: **~2-3 days for the rename + two if-branches + tests.** Down from "3-5 days for memory-model rework" in the v1 plan because we are NOT writing a parallel `PlanStatic`.

The dynamic path remains bit-identical to today — when the program imports from `"cel"`, the code that runs is the same code that runs today, just with a renamed field. Risk to existing behavior is contained.

> **Plan-vs-execution delta (2026-06-08):** as shipped, the rename
> (`InstanceImpl::runtime_instance` → `helpers_instance`) and the
> two-`if` branch structure in `Engine::Plan` landed exactly as
> drafted (`eval/internal/instance_impl.h`, `eval/engine.cc:751-779`),
> and `HasCelImports` is implemented as an ~85-line ULEB128 + section
> walker on the Program bytes (`eval/engine.cc:380`).  However:
> **`CallInit` is NOT implemented.**  The static-mode branch sets
> `impl->helpers_instance = impl->expr_instance` and proceeds directly
> to `BindHelpersInstance` without invoking `_initialize` or
> `__wasm_call_ctors`.  This is a known gap, NOT an intentional drop:
> any static-mode code path that depends on C++ global constructors
> (cctz timestamp parsing, RE2, `absl::str_format` registry, any
> const-initialised global state) is a silent-corruption candidate.
> The current `m28_static_link_test.cc` body even names the gap
> ("the missing `__wasm_call_ctors` ... doesn't surface here") but
> there's no failing-skipped test pinning the open work.  Closing
> this is P1-1 in §13.
>
> *(Reframed 2026-06-09: the dual-mode e2e sweep + targeted
> `cctz_doubles_test.cc` showed every tested cctz / absl path
> produces bit-identical results in both modes, and `wasm-dis`
> inspection confirmed `__wasm_call_ctors` is DCE'd entirely
> from the stripped runtime.  Item moved to P2 as
> defense-in-depth for future surfaces.  See §10.1 invariant 9
> and §13 P2.)*
>
> *(SHIPPED 2026-06-09, evening pass: the strip tool now re-exports
> `__wasm_call_ctors` so it survives DCE, and the static-mode arm of
> `Engine::Plan` invokes it once at instantiate
> (`BindStaticModeHelpers`, `eval/engine.cc`), skipping silently when
> the export is absent.  The full dual-mode conformance run
> (2454 rows, byte-identical between modes — see §7.3 update) ran
> WITH this in place.  The gap paragraph above is retained as
> history; the code is now as the original plan drafted it.)*

> **Plan-vs-execution delta (2026-06-08, simplification pass):** the
> initial prototype's hand-rolled ~85-line `HasCelImports(bytes)`
> ULEB128 + section walker was **replaced with
> `ModuleImportsCelNamespace(wasmtime_module_t*)`** — ~15 lines using
> wasmtime's own `wasmtime_module_imports` introspection API
> (`eval/engine.cc:378`).  This required splitting `InstantiateExpr`
> into two phases: `CompileExprModule(state, impl, bytes)` does the
> `wasmtime_module_new` step; `InstantiateExpr(impl)` does the
> post-compile instantiate + eval-export pull.  `Engine::Plan` now
> compiles the Program's wasm to a `wasmtime_module_t*` first
> (`engine.cc:690` — `const bool is_static =
> !ModuleImportsCelNamespace(impl->expr_module);`), asks wasmtime
> for the import list, routes on the presence of any `"cel"` import,
> then instantiates conditionally.  Retires the review's CHANGE 5 /
> P2-1 ("`use wasmtime_module_imports`") item.  Invariant 8 in
> §10.1 was rewritten to point at the new function and noted that
> routing is now a single wasmtime API call rather than a
> hand-rolled parser.

### 5.5 Dual-mode e2e test infrastructure (added 2026-06-09)

Every e2e test source under `e2e/` now compiles twice via the
`link_mode_e2e_cc_test` bazel macro
(`e2e/link_mode_e2e_test.bzl`), once per link mode.  The two
cc_test targets per source — `<name>_dynamic` and `<name>_static`
— share the source file and differ only in whether
`CELWASM_E2E_USE_STATIC_LINK_MODE` is defined at compile time.
The shared `e2e/link_mode_e2e_helpers.h` reads that macro to
select `kE2ELinkMode` and routes every `CompilePlan` call through
the chosen mode.  Replaces 14 per-file duplicate `Instance
CompilePlan` helpers with one shared definition.

Currently in dual-mode (22 source files):

  - `mvp_concat_test`, `known_bugs_test`, `host_fn_test`,
    `optimize_test`, `program_roundtrip_test`,
    `wkt_field_set_test`
  - `m2_test`, `m2_partial_eval_test`, `m4_test`, `m5_test`,
    `m5b_test`, `m7_test`, `m7a_test`, `m7b_test`, `m8_test`,
    `m9_test`, `m10_test`, `m12_test`, `m14_test`, `m16_test`,
    `m17_test`, `m18_test`

The bespoke `m28_static_link_test` remains explicitly
mode-scoped (it asserts on link-mode-specific shape rather
than mode-invariant behaviour).

The targeted regression test `e2e/cctz_doubles_test.cc`
(14 cells) exercises the specific paths the review report and
the as-drafted plan flagged as silent-corruption candidates:
`timestamp(RFC3339)` parse round-trip, timezone-aware accessor
via `cel_host.cel_timestamp_tz_accessor`, `duration(string)`
parse, `timestamp + duration` and `timestamp - timestamp`
arithmetic, `string(<double>)` formatting (full precision
IEEE 754), `double(<string>)` parse round-trip, and
double-arithmetic edge cases.  All 14 cells pass in both
modes — see §10.1 invariant 9 for the framing correction
this enabled.

Test signal as of 2026-06-09: **45/45 dual-mode e2e targets
pass; 55/55 of `//compiler/...` `//eval/...` `//runtime/...`
pass.**

## 6. ABI considerations — does the wire format change?

Sub-question: **does `Program.wasm_bytes()` (and the embedded `cel.abi` custom section) need a version bump or a mode marker for cross-process portability?**

### 6.1 What changes structurally in the wasm

Dynamic-mode `Program.wasm`:
- Imports `cel.memory`, `cel.arena_reset`, `cel.cel_int_add_at_vv`, etc.
- Bytes: ~10 KB.
- `cel.abi` custom section: existing schema (declared vars, required imports).

Static-mode `Program.wasm`:
- No `cel.*` imports.
- Still imports `cel_host.*` (host trampolines) and `wasi_snapshot_preview1.*` (WASI primitives the runtime needs).
- Bytes: ~800 KB.
- `cel.abi` custom section: same schema; the declared-vars list is identical, the required-imports list is empty for the `cel.*` namespace but still lists `cel_host.*` requirements.

### 6.2 Does Engine need an explicit marker?

Engine can detect link mode from the wasm imports section alone (presence/absence of `cel.*` imports). **No explicit marker is required for runtime detection.**

But for **embedder tooling** (cache validators, signature systems, debug dumps), an explicit field is useful. Proposed addition to `cel.abi` custom section:

```proto
message Abi {
  // ...existing fields (declared_variables, required_imports, ...)

  enum LinkMode {
    LINK_MODE_DYNAMIC = 0;  // default, matches existing Programs
    LINK_MODE_STATIC = 1;   // self-contained, runtime statically linked
  }
  LinkMode link_mode = N;  // next free field number in Abi proto
}
```

Adding a new optional proto field is backward-compatible — old Programs decode with default value 0 = `LINK_MODE_DYNAMIC`, which matches their actual shape.

**Schema version bump?** No. The proto's `schema_version` (if any) stays. The new field is optional with a default that matches existing Programs.

### 6.3 What about future modes?

Reserving `enum LinkMode` rather than a `bool` makes the field extensible — if a future milestone adds a third mode (e.g. component-model-static, or a hybrid), it slots in without re-encoding existing Programs.

> **Plan-vs-execution delta (2026-06-08):** the explicit `cel.abi`
> `LinkMode` field was **not** added in the prototype.  `Engine::Plan`
> currently routes entirely on `ModuleImportsCelNamespace(impl->expr_module)`
> (see §5.4 simplification delta — replaces the original
> `HasCelImports(program.wasm_bytes())` byte-walker), which is the
> implicit-detection arm §6.2 described as
> "no explicit marker is required for runtime detection."  This is
> acceptable as an interim state but means: (a) embedder tooling
> (cache validators, signature systems) has no in-band signal of
> link mode; (b) a future link mode that genuinely has no `cel.*`
> imports but isn't fully static would be misclassified.  Adding the
> proto field is P1-7 / CHANGE J in the review (`reviews/2026-06-08-m28-prototype.md`).
> See §13.

## 7. Test plan

### 7.1 Compiler tests

- `compiler/compiler_test.cc` extended with cells matrix `LinkMode × {empty expr, 1-term arith, 1000-term arith, string eq, in-list, has(msg.field)}`. Each cell compiles in both modes; both produce a valid Program that the Engine can Plan.
- Byte-shape assertions: dynamic-mode Program has `cel.*` imports; static-mode Program does not. Both have `cel_host.*` and `wasi_snapshot_preview1.*` imports.

### 7.2 Engine tests

- `eval/engine_test.cc` extended with the same matrix. Each cell evaluates to the same Value in both modes.
- The memory-model rework gets focused tests in `eval/instance_test.cc`: activation marshaling round-trips for every CelType under static mode.

### 7.3 Conformance

- `conformance/` runs the full corpus under both modes via parameterized test. **Gate: same pass/fail count under both modes**, byte-identical results.
- `scripts/check_conformance_monotonic.sh` extended to enforce no regression in either mode.

> **Executed 2026-06-09:** full corpus (2454 rows) run in both modes —
> **1899 pass / 463 skip / 92 fail in each, byte-identical** down to
> the per-row FAIL detail text (the 92 fails are the pre-existing
> backlog, unchanged by link mode).  Gate wired: the script now runs
> the runner once per `--link_mode`, each gated against its own
> baseline (`conformance/.baseline` = 1899 dynamic, new
> `conformance/.baseline_static` = 1899).  **Operational note:** the
> static leg takes ~25 min fastbuild (per-row Binaryen merge) vs ~3
> min dynamic — the pre-push hook is correspondingly slower; consider
> making the static leg CI-only if push latency becomes a problem.

### 7.4 Bench

- All `benchmark/eval/corpus/*.yaml` cells run on both modes (via a flag on celwasm_bench).  *(Landed 2026-06-09: `--link_mode=dynamic|static` on `celwasm_bench`, default dynamic to keep historical baselines comparable; the flag is consumed before Google Benchmark's own arg parsing.)*
- Per-cell three-way report (today / static / cel-cpp) lands in `benchmark/eval/run.sh` output.  *(Landed 2026-06-09: `run.sh` runs `celwasm_bench` once per mode and emits two `report.sh` comparison tables against the single `celcpp_bench` run.)*
- Closeout criteria: the cells we measured in `FINDINGS.md` §11.4 still hit ~30× speedup at long N and ~parity at N=2.  *(Still open — needs a `-c opt` run on master-shape code.)*

## 8. Phasing

**Phase 1 — Compiler-side support (parallel-safe, no Engine touch).** ~1 week.
- `cel_runtime_stripped` build step + embedded bytes header.
- `CompilerOptions::link_mode` field + plumbing through `Compile()`.
- Static-mode merge implemented via Binaryen library calls inside Compile().
- Tests: compile-only, byte-shape assertions, no Engine path exercised yet.
- Visible effect: embedders can produce static-mode Programs but can't run them yet.

**Phase 2 — Engine unified-path change.** ~2-3 days. (Revised down from v1 plan after the simpler design surfaced — see §5.4.)
- Rename `InstanceImpl::runtime_instance` → `helpers_instance`.
- Two `if (HasCelImports(...))` branches inside `Engine::Plan` (skip the separate runtime instantiate in static mode; trigger `_initialize` once instead).
- `HasCelImports` and `CallInit` helpers.
- Tests: full Engine + Instance + Eval matrix in both modes; existing tests stay green.
- Visible effect: static mode is functional end-to-end; variable-bearing cells produce correct results.

**Phase 3 — Conformance + bench gate.** ~3-5 days.
- Conformance suite parameterized over LinkMode; both modes pass identically.
- Bench cells run both modes; report joined by BM-name with mode as a suffix.
- `cel.abi` proto field added (the optional `LinkMode link_mode = N`).
- Closeout: §11.4 numbers reproduced from master.

Total: ~3 weeks of focused engineering.

## 9. Risks & open questions

- **wasi-libc init that depends on global C++ statics may break.** The strip + merge removes the per-export ctor calls, replacing them with a single `__wasm_call_ctors` at instantiate. If any operator's body assumes specific C++ static state that gets reset between calls today (because of the per-call dtor chain), it could regress. Mitigation: conformance gate. Cost: TBD if a regression surfaces.

- **Merged module size (~800 KB) is large.** Embedders that cache many compiled programs see N× the memory footprint of the dynamic path. Mitigation: keep dynamic mode as the default; document the size tradeoff prominently.

- **`Compile()` latency increases in static mode.** Binaryen merge + `-O2` over the merged module is observably slower than emitting today's expression-only wasm. Likely ~50-200 ms per Compile() in static mode vs ~5-20 ms today. Acceptable for the "compile once, eval many" model; surfaced in docs.

- **The `cel_runtime_stripped` artifact must remain semantically identical to `cel_runtime`.** Any future runtime change must keep both build outputs in sync, since the stripped variant is used in production. Build-time test asserts both produce the same set of helper exports (bare-body for stripped, command_export-wrapped for default).

- **Variable-bearing static-mode performance is not yet measured.** Phase 2 closes this gap; the bench numbers will land at Phase 3 closeout. If the measured numbers diverge significantly from the const-mode pattern (53× per-op slope improvement), the milestone may need adjustment in scope.

## 10. Out of scope (explicit)

- **Const-list-literal codegen optimization.** Surfaced in `FINDINGS.md` §12 as an orthogonal lever — would make `x in [const list]` a 2-call eval regardless of list size. Independent of static linking; deserves its own milestone. Candidate: `m29-const-list-literal.md`.
- **Heterogeneous equality (`int == double`)** — the checker gap surfaced by `BM_cmp_intEqDouble` in the bench run. Real conformance bug. Independent milestone.
- **N=10000 string literal rodata budget** — codegen-side limit observed during long-string corpus bench. Independent codegen milestone.
- **Native (non-wasm) backend.** Would moot the wasm-boundary cost entirely on short expressions but is a much bigger architectural conversation. Not this milestone.

### 10.1 Invariants the prototype enforces

The prototype enforces nine load-bearing invariants surfaced during
execution that the as-drafted plan did not anticipate.  Each is a
candidate for explicit assertion or focused regression test as part
of P1 closeout.  Source: review report 2026-06-08
(`doc/implementation-plan/rewrite/reviews/2026-06-08-m28-prototype.md`).

1. **Strip tool MUST NOT run `merge-similar-functions` / inliners** —
   `BinaryenModuleRunPasses({"remove-unused-module-elements"})` only;
   silent today (see review §2 invariant 1).
2. **Codegen `BinaryenLoad` / `BinaryenStore` MUST pass `nullptr` for
   the memory name** — 11 sites in `expr_lower.cc` +
   `expr_lower_comprehension.cc`; loud-on-regression in static mode
   but silent in dynamic-only test runs (see review §2 invariant 2).
3. **Codegen-canonical `cel_host.*` import names MUST be installed in
   both link modes** — `InstallCelHostImports` runs from both
   `Compile()` and `CompileStatic`; intended design per §5.3.2
   above (see review §2 invariant 3).
4. **Strip tool MUST call `BinaryenSetDebugInfo(1)` before write** —
   process-global; preserves the wasm `name` section so codegen's
   intra-module calls resolve (see review §2 invariant 4).
5. **Adopted module's feature set is the UNION of declared +
   `DefaultFeatures()`** — narrowing trips Binaryen's feature-
   dependency check (see review §2 invariant 5).
6. **Expression rodata MUST fit in `[0, CELWASM_RESERVED_LOW_MEMORY_BYTES)`** —
   silent corruption candidate; nothing currently asserts the bound
   (see review §2 invariant 6).
7. **`helpers_instance` MUST be the runtime helpers source at every
   reader** — 6 reader sites in `eval/engine.cc`; rename complete
   (see review §2 invariant 7).
8. **`is_static` detection routes on the program module's import list** —
   `ModuleImportsCelNamespace(wasmtime_module_t*)` in
   `eval/engine.cc:378`, ~15 lines wrapping `wasmtime_module_imports`.
   Loud-on-misclassification.  *(Updated 2026-06-08, simplification
   pass: replaces the original ~85-line hand-rolled ULEB128 + section
   walker `HasCelImports(bytes)` — single wasmtime API call now, not
   a hand-rolled parser; see §5.4 delta.  The "no unit test for the
   walker" gap from invariant 8 in the review is now a unit-test gap
   for the wasmtime-driven routing — see P1-6 in §13.)*
9. **Static-mode wasi-libc init footprint (REVISED 2026-06-09).**
   The strip tool's `remove-unused-module-elements` pass DCEs
   `__wasm_call_ctors` entirely from the stripped runtime
   (verified by `wasm-dis` inspection of
   `cel_runtime_stripped_wasm.bin`; the symbol does not exist
   as a function).  The prior agent review (2026-06-08) flagged
   this as a P1 silent-corruption candidate for cctz / RE2 /
   absl init paths.  **Empirical evidence accumulated 2026-06-09
   contradicts that framing:** the dual-mode e2e sweep (22 source
   files × 2 modes = 44 cells, plus the focused
   `e2e/cctz_doubles_test.cc` exercising timestamp parse /
   accessor / duration / `string(<double>)` / `double(<string>)`)
   produces bit-identical results between kDynamic and kStatic.
   The C++ statics the runtime actually depends on are either
   constexpr-initialized or zero-init-safe within the tested
   surface.  The invariant is **defense-in-depth, NOT load-bearing
   for tested paths.**  A future expression surface (e.g.
   RE2-driven regex, a new absl format-spec) could in principle
   hit a static that doesn't fall in either category — adding an
   explicit `CallInit` helper at `Engine::Plan` time remains a
   low-cost prophylactic.  See §13 P2 for the carry-forward.

## 11. What's been validated already (link from this doc, don't restate)

All measurements supporting this milestone are in
`doc/implementation-plan/rewrite/m28-wrapper-overhead-findings.md`
("FINDINGS.md" wherever this doc says so):
- §4.1 — wrapper overhead per call (microbench)
- §11.4 — production-bench numbers under the prototype's strip+merge pipeline
- §12.3 — celwasm vs cel-cpp on string + in-list canonical patterns

> **Plan-vs-execution delta (2026-06-09):** the prototype directory
> `wasm_compilation_experiments/` (strip/merge driver tools + ~2 GB of
> generated wasm artifacts, never committed to any branch) was
> **deleted** once this milestone reimplemented the strip+merge
> pipeline as the production `LinkMode::kStatic` path.  `FINDINGS.md`
> — the measurement record — was relocated to
> `m28-wrapper-overhead-findings.md` in the same pass.  The bench's
> hardcoded "merged-wasm" cells (`BM_arith_intAdd<N>TermsConstMerged`
> et al.), which loaded pre-built artifacts from the deleted
> directory, were removed from `celwasm_bench.cc`; the same
> measurement is now expressed as `--link_mode=static` over the
> regular corpus cells.

## 12. Closeout checklist

To be ticked when shipping:

- [x] `CompilerOptions::link_mode` field landed.  *(Default flipped to `kStatic` in the 2026-06-08 simplification pass — see §5.1 delta; this is a behavioural change for embedders who relied on the implicit default, who now opt into `kDynamic` explicitly.)*
- [x] `cel_runtime_stripped_wasm_bytes.h` build target lands.
- [x] In-Compile() merge step lands.
- [x] `Engine::Plan` routing lands; single unified Plan body with two `if (is_static)` branches (no separate `PlanStatic` — see §5.4).
- [x] Engine memory-model rework: activation + result decode work for variable-bearing cells in static mode.  *(Verified 2026-06-09 by the dual-mode e2e sweep — 22 e2e source files run in both kDynamic and kStatic, covering variable bindings, comprehensions, `has(msg.field)`, list/map literals, and cctz / `string(<double>)` paths; 45/45 targets pass with bit-identical results.  See §5.5.)*
- [x] Conformance suite green under both modes.  *(2026-06-09 — 1899/463/92 each, byte-identical; per-mode baselines wired into the gate script.  See §7.3.)*
- [x] Bench cells produce production data on both modes.  *(2026-06-09 — full 232-cell three-way run, `m28-bench-results.md`.  The ±10% reproduction criterion **failed**: 1000-term chains measure 17–22× vs the claimed ~31×; the results doc records the corrected numbers and candidate causes.  The honest headline supersedes the prototype claim.)*
- [x] `cel.abi` proto extended with `LinkMode` field; existing Programs decode as `LINK_MODE_DYNAMIC` by default.  *(2026-06-09 — `abi/cel_abi.proto` field 7, emit from both compile arms, legacy-bytes + unknown-future-value tests.  Metadata only; Plan routing stays import-introspection-based per §6.2.)*
- [x] `compiler/compiler.h`, `eval/engine.h`, and the high-level `README.md` document the choice + when to pick each.  *(2026-06-09 — compiler.h docblock predates; engine.h Plan docs + README results section added in the closeout docs pass.)*
- [x] `testing-checklist.md` rows ticked (M28 — Configurable linking section added 2026-06-08).
- [x] `FINDINGS.md` status updated to point at this milestone as the production landing.  *(2026-06-09 — relocated to `m28-wrapper-overhead-findings.md` with a status line naming m28 as the production landing; see §11 delta.)*
- [x] Closeout review per CLAUDE.md "Periodic code review" — architectural drift, doc reconciliation, cleanup-backlog entries.  See `reviews/2026-06-08-m28-prototype.md`; follow-ups listed in §13.

## 13. Future work (P1 + P2 carry-forward)

The agent-driven review (2026-06-08,
`reviews/2026-06-08-m28-prototype.md`) catalogued 8 P1 items totaling
~5–7 engineer-days to milestone-shippable and an additional 7 P2 items
for ~10–12 total.  The dual `cel_host.*` import case originally listed
as P2-1 (and the related CHANGE I to eliminate it via strip-time
rename) has been **retired** — it is not a bug but the natural
consequence of merging two modules that each import the same host
trampoline (per §5.3.2 above).

**Two further review items retired by the same-day simplification
pass** (see §5.4 and §5.3.3 deltas above): the review's CHANGE 5 / P2-1
("`use wasmtime_module_imports` instead of the hand-rolled byte
walker") shipped — `ModuleImportsCelNamespace` is the live routing
helper.  The review's P2-7 ("`InstallStructImports` fold") shipped —
the helper's single remaining import now lives in `InstallListImports`.

### P1 — must-fix before next milestone — **ALL CLOSED 2026-06-09**

- ~~**P1-1 — Add `CallInit` in static mode**~~ Reframed to P2, then
  **shipped** the same day (see the P2 entry below and §5.4's final
  delta).
- [x] **P1-2 — Strip tool's no-merge invariant pinned.**
  `runtime/cel_runtime_stripped_wasm_bytes_test.cc::CatalogueExportsTargetDistinctFunctions`
  asserts every `abi::CelRuntimeHelpers()` name exists as an export
  AND that no two catalogue exports share an internal target.
- [x] **P1-3 — Rodata budget enforced.**
  `InstallExprRodataSegment` in `compiler/internal/compile.cc` returns
  `ResourceExhaustedError` when `rodata_base + rodata.size()` exceeds
  `CELWASM_RESERVED_LOW_MEMORY_BYTES`; check and segment-install are
  one unit so an unchecked segment can never be added.  Negative test
  confirmed the prior silent-corruption behavior (a 9000-char literal
  compiled cleanly before the fix); boundary-positive + dynamic-mode
  control tests included.
- [x] **P1-4 — Conformance under static mode** — executed and wired;
  see §7.3 update (byte-identical, per-mode baselines).
- [x] **P1-5 — Test matrix expansion** — satisfied by the dual-mode
  e2e sweep (22 source files × 2 modes) + full dual-mode conformance;
  the deferred 1000-term-chain cell is covered by the production
  bench run (`m28-bench-results.md`).
- [x] **P1-6 — Routing helper unit-tested.**  Extracted to
  `eval/internal/module_imports.{h,cc}` with its own
  `module_imports_test.cc` — synthetic-WAT matrix: no imports /
  wasi-only / cel_host-only / cel.* present / mixed / multiple /
  prefix-confusion ("celx", "ce") boundary.
- [x] **P1-7 — `WasmModule::Adopt` + `AddActiveDataSegment` tested.**
  `compiler/codegen/module_test.cc`: adopt round-trip, feature-set
  union (MVP module gains DefaultFeatures), ownership/destruction,
  segments on `"memory"` and `"0"` memories, empty-span boundary.
- [x] **P1-8 — Codegen load/store guard is structural.**
  `CodegenLoad`/`CodegenStore` wrappers (no memory-name parameter,
  always nullptr) replaced all 11 direct sites in `expr_lower.cc` +
  `expr_lower_comprehension.cc`; grep-verified zero direct calls
  remain; emitted wasm byte-identical (golden + dual-mode e2e green).

### P2 — cleanup-when-touched

- ~~**P2 — Defense-in-depth `CallInit` helper**~~ **SHIPPED
  2026-06-09 (evening pass).**  The strip tool now explicitly
  re-exports `__wasm_call_ctors` so it survives DCE
  (`runtime/strip_command_wrappers.cc`), and static-mode
  `Engine::Plan` invokes it once at instantiate via
  `BindStaticModeHelpers` (`eval/engine.cc`) — skipping silently
  if the export is absent for back-compat with older stripped
  bytes.  The previously-feared silent corruption never
  materialized empirically (dual-mode e2e sweep + full dual-mode
  conformance, byte-identical), so this is pure defense-in-depth
  for future static-init-dependent surfaces; it is now real code,
  not a list item.  See §10.1 invariant 9.
- [x] **P2-1 — Debug-info global scoped** (2026-06-09).  Binaryen's C
  API confirmed to have NO per-module/per-write debug-info option
  (`BinaryenModuleAllocateAndWrite` reads process-global state); a
  getter exists, so the strip tool now saves via
  `BinaryenGetDebugInfo()` and restores after the write.
- [x] **P2-2 — Shared back-half extracted** (2026-06-09).
  `LowerExportAndFinalise(artifact, opts, link_mode)` in
  `compiler/internal/compile.cc` is the single tail both `Compile()`
  and `CompileStatic` call after their mode-specific bootstrap; the
  only parameter difference is the `cel.abi` link-mode stamp.
- [x] **P2-3 — Self-skip cost documented** (2026-06-09).  Verified
  against vendored Binaryen source: `BinaryenGetFunction` is one
  hash-map lookup; ~280 fixed seeds walked once per Compile —
  microseconds total.  Comment replaced with the real cost story.
- [x] **P2-4 — Stale comment fixed** (2026-06-09) —
  `eval/engine_test.cc` brace list now names `helpers_instance`.
- ~~**P2-5**~~ Retired 2026-06-08 (see §5.3.3 delta).
- [x] **P2-6 — Const-cast contained** (2026-06-09).  Lives in the
  dedicated `AdoptStrippedRuntime()` helper with a named-check NOLINT
  and a comment explaining Binaryen's deserialize path takes `char*`
  but does not mutate.

### Forward-looking (not commitments; pre-existing items)

- Investigate the per-Eval intercept (~55 ns in both modes).  If we
  can drive it lower (e.g. by bypassing wasmtime's typed-call path
  on hot loops), the N=2 cells flip from parity to a measurable win
  over cel-cpp.
- The merged-module size (~800 KB) suggests an interest in DCE-aware
  partial linking — only merge helpers the expression actually uses.
  Would shrink static-mode artifacts considerably for arithmetic-
  only expressions.
- Cross-process Program sharing (Compile on host A, Eval on host B)
  is a deferred capability the `cel.abi` `LinkMode` field (P1-7 /
  CHANGE J in the review) enables.  The protocol for "host B
  verifies it can Eval this program shape" lands when there's a
  real consumer.
