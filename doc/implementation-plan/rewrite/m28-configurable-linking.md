# m28 — Configurable Linking

Status: plan — drafted 2026-06-06, not yet started.

## 0. TL;DR

Add an opt-in **static linking** mode to `Compiler::Compile`, with the existing **dynamic linking** behavior unchanged as default. Static mode produces a self-contained `Program.wasm` (the runtime helpers are merged in and the wasi-libc command-mode wrappers are stripped). Dynamic mode produces today's shape (expression wasm + separate `cel_runtime` instance). Engine detects which shape it's planning and routes to the appropriate path.

Measured upper-bound benefit on the cells already validated: **30× faster than cel-cpp on `intAdd1000Terms` and similar long chains, 2-30× faster across the rest of arithmetic + comparisons.** Lower-bound: at N=2 const cells, static-linked celwasm is within 0-17% of cel-cpp (we're at the wasm-boundary floor). Variable-bearing patterns are not yet validated — see §5.

This milestone covers landing the mode as a production-supported feature, including the Engine memory-model work that unblocks variable-bearing static-linked cells. It does **not** include the const-list-literal codegen optimization observed in §12 of `wasm_compilation_experiments/wrapper_overhead/FINDINGS.md` (that's a separate future milestone).

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
  LinkMode link_mode = LinkMode::kDynamic;
};
```

Default is `kDynamic` — every existing call site is unchanged.

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

### 7.4 Bench

- All `benchmark/eval/corpus/*.yaml` cells run on both modes (via a flag on celwasm_bench).
- Per-cell three-way report (today / static / cel-cpp) lands in `benchmark/eval/run.sh` output.
- Closeout criteria: the cells we measured in `FINDINGS.md` §11.4 still hit ~30× speedup at long N and ~parity at N=2.

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

## 11. What's been validated already (link from this doc, don't restate)

All measurements supporting this milestone are in `wasm_compilation_experiments/wrapper_overhead/FINDINGS.md`:
- §4.1 — wrapper overhead per call (microbench)
- §11.4 — production-bench numbers under the prototype's strip+merge pipeline
- §12.3 — celwasm vs cel-cpp on string + in-list canonical patterns

The full prototype lives in `wasm_compilation_experiments/wrapper_overhead/` on branch `perf/binaryen-merge-proto`. It is a research artifact, not production code; this milestone reimplements the same architectural change as a proper feature.

## 12. Closeout checklist

To be ticked when shipping:

- [ ] `CompilerOptions::link_mode` field landed, defaulted to `kDynamic`, embedders unaffected.
- [ ] `cel_runtime_stripped_wasm_bytes.h` build target lands.
- [ ] In-Compile() merge step lands.
- [ ] `Engine::Plan` routing lands; `PlanStatic` implemented.
- [ ] Engine memory-model rework: activation + result decode work for variable-bearing cells in static mode.
- [ ] Conformance suite green under both modes.
- [ ] Bench cells produce production data on both modes; the §11.4 numbers from `FINDINGS.md` reproduce within ±10%.
- [ ] `cel.abi` proto extended with `LinkMode` field; existing Programs decode as `LINK_MODE_DYNAMIC` by default.
- [ ] `compiler/compiler.h`, `eval/engine.h`, and the high-level `README.md` document the choice + when to pick each.
- [ ] `testing-checklist.md` rows ticked.
- [ ] `FINDINGS.md` status updated to point at this milestone as the production landing.
- [ ] Closeout review per CLAUDE.md "Periodic code review" — architectural drift, doc reconciliation, cleanup-backlog entries.

## 13. Future work (forward-looking, not commitments)

Items surfaced by this work that don't belong here, captured so they aren't lost:

- Investigate the per-Eval intercept (~55 ns in both modes). If we can drive it lower (e.g. by bypassing wasmtime's typed-call path on hot loops), the N=2 cells flip from parity to a measurable win over cel-cpp.
- The merged-module size (~800 KB) suggests an interest in DCE-aware partial linking — only merge helpers the expression actually uses. Would shrink static-mode artifacts considerably for arithmetic-only expressions.
- Cross-process Program sharing (Compile on host A, Eval on host B) is a deferred capability the `cel.abi` `LinkMode` field enables. The protocol for "host B verifies it can Eval this program shape" lands when there's a real consumer.
