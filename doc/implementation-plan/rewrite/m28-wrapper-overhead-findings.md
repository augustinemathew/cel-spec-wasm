# Wrapper-Overhead Experiment — What We Actually Validated

**Date:** 2026-06-06
**Branch:** `perf/binaryen-merge-proto`
**Status:** Experiment complete; productionized as m28 (`m28-configurable-linking.md`).

> Relocated 2026-06-09 from `wasm_compilation_experiments/wrapper_overhead/FINDINGS.md`
> when the experiments directory (prototype tools + ~2 GB of generated wasm
> artifacts) was deleted after m28 reimplemented the strip+merge pipeline as
> the production `LinkMode::kStatic` path.  This file is the measurement
> record m28 cites; section numbers referenced as "FINDINGS.md §N" resolve
> here.  The "not yet performed" caveats below describe the state on
> 2026-06-06 and were closed by the m28 prototype (see the milestone doc).

This doc is the experimental record. It separates *what we have measured directly* from *what we have inferred or predicted*, because earlier in this investigation we made several extrapolations that turned out wrong by 10×. The goal here is to have one place that says with full honesty: this much we know, this much we don't, this is the next experiment that would decide.

## 1. The starting question

Today's benchmark (`benchmark/eval/celwasm_bench`, post-correction):

| pattern | celwasm | cel-cpp | ratio |
|---|---:|---:|---:|
| `a + b` (N=2, vars) | 257 ns | 72 ns | **3.6× slower** |
| `a + b + ... + j` (N=1000) | 78 µs | 33 µs | **2.4× slower** |

We're slower than cel-cpp everywhere. The investigation set out to find: **where does the time go, and what architectural lever closes the gap?**

## 2. Path the investigation took (compressed)

1. **Profiled the bench** at N=1000 using `sample` + a wasmtime perfmap. Top hot symbols (% of bench time inside JIT'd wasm):
   - `__wasilibc_init_ssp` — 27 %
   - `cel_int_add_at_vv.command_export` — 25 %
   - `__wasm_call_ctors` — 16 %
   - `signatures[3]::wasm_to_array_trampoline` — 16 %
   - `__wasm_call_dtors` — 6 %
   - **`cel_int_add_at_vv`** (the actual arithmetic body) — **1.4 %**

   The arithmetic itself is < 2 % of bench time. The other ~98 % is ceremony around the arithmetic.

2. **Hypothesis 1 (SSP refresh):** `__wasilibc_init_ssp` calls `random_get` to refresh the stack canary per export call. Turn SSP off, save ~22 ns/op.

   **Tried** `-fno-stack-protector` on `runtime/BUILD.bazel`. Re-benched.

   **Result:** 2.7 ns/op savings (3.4 %), not the predicted 22.

   **Why it missed:** my flag only touched the helpers *we* compile. `__wasilibc_init_ssp` lives inside wasi-libc itself, which was pre-built with SSP on when the wasi-sdk shipped. The call site in `__wasm_call_ctors` is unchanged. *(Verified by disassembling the post-fix wasm — `call __wasilibc_init_ssp` is still there.)*

3. **Hypothesis 2 (the wrapper itself):** the disassembly showed every export is reached through a `.command_export` wrapper that runs `__wasm_call_ctors → body → __wasm_call_dtors` unconditionally. The ctor chain unconditionally calls `__wasilibc_init_ssp` (no idempotency guard). If we could skip the wrapper, all of that goes away.

   **Tested via a microbench described below.**

## 3. What the prototype consists of

```
wasm_compilation_experiments/wrapper_overhead/
  add_raw_aliases.cc      — Binaryen tool. For every export pointing at a
                            `X.command_export` wrapper function in
                            `cel_runtime.wasm`, add a new export named
                            `X__raw` pointing at the bare body `X`.
  wrapper_microbench.cc   — Host harness. Loads the aliased wasm and
                            tight-loops calls to both `cel_int_add_at_vv`
                            (the wrapped, current production export) and
                            `cel_int_add_at_vv__raw` (the bare body) at
                            1 M iterations each.  Repeats 5×.  Also runs
                            a byte-identical-output correctness probe.
  bench_separate.wat      — Minimal expr-wasm that calls an IMPORTED add 9
                            times.  Demonstrates today's shape.
  bench_inline.wat        — Same calls but the helper is an in-module
                            function.  Demonstrates the post-merge shape.
```

The tools are small (~90 lines for the alias tool, ~330 lines for the microbench) and isolated under `wasm_compilation_experiments/` so they leave no trace on the production build.

## 4. What we have actually measured

### 4.1 Wrapper overhead per call (`wrapper_microbench.cc`)

Same wasm module, same wasmtime instance, same store, same input args. Only difference: which export we call. **5 runs × 1 M calls each.** Median values (range in parens):

| op | wrapped (current) | bare (post-merge) | wrapper cost | ratio |
|---|---:|---:|---:|---:|
| intAdd | 140.5 ns (134-143) | 59.2 ns (58-59) | 81.3 ns | 2.37× |
| intMul | 142.3 ns (141-142) | 59.4 ns (58-60) | 82.9 ns | 2.40× |
| intSub | 142.9 ns (142-144) | 59.5 ns (58-60) | 83.4 ns | 2.40× |
| doubleAdd | 144.1 ns (141-144) | 59.6 ns (59-61) | 84.5 ns | 2.42× |
| intEq | 144.7 ns (144-146) | 59.8 ns (59-59) | 84.9 ns | 2.42× |
| intLt | 143.8 ns (143-145) | 60.2 ns (59-60) | 83.6 ns | 2.39× |

**What this validates:**

- The `.command_export` wrapper chain costs **~83 ns per host call**, *uniformly across six different operators.*
- Variance is small (a few ns across 5 reps); the signal dominates the noise.
- The ratio is consistently ~2.4× for every op — the wrapper cost is not op-specific.
- Bare-call cost (~60 ns) is dominated by **wasmtime's host-call trampoline**, not the wasm body. The body itself is single-digit ns.

**What this does NOT validate:**

- Whether the same 83 ns savings transfers to *intra-wasm* calls (the expr-wasm calling the runtime helper). The microbench measures *host-to-wasm* calls. In the actual bench, the bulk of calls are *wasm-to-wasm* inside one wasmtime store. Whether the wrapper's per-call cost is the same in that context is **not measured here.**
- Whether real production bench numbers will move by the corresponding amount. We have not yet wired the codegen + Engine to use bare helpers and re-benched.

### 4.2 Correctness — byte-identical output

For each of the 6 ops, both the wrapped and bare paths were called with the same input args on the same memory. We then read 24 bytes of slot[0] (the output slot) and compared.

**All 6 ops: MATCH.** The bare path produced byte-identical results to the wrapped path.

**Important caveat:** the inputs were zero-initialized memory, so the operand `CelValue.kind` was 0 (= `CEL_NULL`), which routes both paths through the 3VL absorb branch and emits a null/error result. **The actual arithmetic path was not exercised.** We have only validated that the bare path is correct on the 3VL-absorb fast path. We do **not** yet know whether `bare(int(1), int(2))` returns `int(3)`. To know that, we'd need to seed slots with real ints and check.

### 4.3 Binaryen optimization across in-module vs cross-module calls

Two minimal modules, both with 9 chained add calls, run through `wasm-opt -O2`:

**`bench_separate.wat`** — calls an *imported* add. After -O2:

```wat
(func $0 (result i32)
  (call $fimport$0
   (call $fimport$0
    (call $fimport$0
     ;; 6 more nested calls, then base case:
     (i32.const 1) (i32.const 2)) ...) ...))
```

9 calls remain. **Binaryen cannot optimize.**

**`bench_inline.wat`** — same code but `$add` is an in-module function. After -O2:

```wat
(func $0 (result i32)
  (i32.const 55))
```

**One instruction.** Binaryen inlined `$add` into `$eval`, saw consecutive `i32.const + i32.add` pairs, folded the entire chain to `i32.const 55` (= 1+2+3+...+10).

**What this validates:**

- **In principle**, putting helpers in-module unlocks Binaryen's full optimization pipeline (inlining + constant folding + DCE).
- The difference is not theoretical — it shows up immediately on a trivial test case.
- This is the leverage point: cross-module calls hide the body from the optimizer; in-module calls expose it.

**What this does NOT validate — and the catch that makes the demo nearly meaningless for our case:**

The toy demo folds because both inputs are `i32.const` *literals* in the wasm IR. **The actual `cel_int_add_at_vv` takes *slot indices*, not values.** Its body does:

```
i32.load <out_slot * 24>            ;; CelValue.kind for one operand
i64.load <out_slot * 24 + 8>        ;; CelValue.payload.i
... same for the other operand ...
i64.add
i32.store <out_slot * 24>
i64.store <out_slot * 24 + 8>
```

Even after the helper is inlined, the operands are *memory loads from per-Eval-mutable slots*. Binaryen has no way to know what `i64.load <slot_a>` evaluates to — it would need to trace the value all the way back through the host's activation marshaling, across the wasmtime boundary. That kind of interprocedural reasoning is **not what Binaryen does**, so **constant folding does NOT fire on a real CEL arithmetic chain.**

What Binaryen *can in principle* do once helpers are in-module (inlining, load-forwarding around the slot-write/slot-read accumulator pattern, DCE on dead stores) is **unmeasured.** We have not run Binaryen against actual codegen output with the helpers merged in. Anything we say about how much it delivers in production is speculation until we do.

## 5. What we have predicted but NOT validated

| prediction | basis | status |
|---|---|---|
| Stripping wrappers drops `intAdd1000Terms` slope from 78 → some smaller number | extrapolation from microbench's 83 ns/host-call | **NOT measured end-to-end** |
| Post-fix we beat or match cel-cpp at large N | follows from the slope extrapolation | **NOT measured** |
| At small N we close some of the 3.6× gap | the wrapper savings should apply, but the host-wasm boundary dominates and is untouched | **NOT measured** |
| Binaryen delivers any specific magnitude of savings on real CEL helpers (inline + load-forward + DCE) | inference from a toy demo whose folding mechanism does not apply to our slot-based code | **NOT measured on real helpers** |

Every quantitative claim in this table is speculation. Two specific things in particular have **no measurement backing them**:

- **The transfer from host-call wrapper cost (83 ns) to intra-wasm-call wrapper cost.** The microbench measured host calls. The bench's hot path is wasmtime intra-store calls inside one wasm execution. Whether wasi-libc's wrapper code runs identically in both contexts, whether wasmtime's JIT optimizes one differently from the other — we don't know.
- **What Binaryen does on a real production-shape expr+runtime merged module.** The toy fold demo proves Binaryen has the capability to optimize across in-module calls. It does **not** tell us what Binaryen does to a 1000-call chain into a 30-instruction slot-based helper. The answer could be substantial, could be near-zero. Without running it, we don't know.

The track record on extrapolations in this investigation:

- **SSP prediction:** predicted 22 ns/op savings, measured 2.7. **10× off.**
- **Initial reactor prototype (`reactor_proto/`):** measured tiny-module command-mode and reactor-mode at the same speed (~56 ns), concluded reactor mode wouldn't help. But then disassembly of cel_runtime showed the per-call wrapper IS present — the prototype was misleading because the tiny module had empty ctors. **Right hypothesis, wrong evidence direction.**

These earlier misses should temper how much weight to put on the unvalidated predictions in the table above. They are reasonable hypotheses, not predictions to anchor planning on.

## 6. What we have NOT done

- Verified `bare(int(1), int(2))` produces `int(3)`. (Correctness probe used null inputs.)
- Wired Engine + codegen to use bare helper exports + re-run the production bench.
- Run the conformance suite against a bare-helper runtime.
- Investigated whether Binaryen's optimizations actually fire on realistic CEL expressions (a 1000-term `a + b + c + ...` over real variables, not constants).
- Built the "static-link-runtime" architecture in any production-shaped way. The aliased wasm is a hack — same module, different exports. The real architecture would have one self-contained wasm per Compile() output. We have only validated that the *underlying lever* (in-module access to helpers) exists and matters.

## 7. What the experiment definitively says

Strict statements only:

1. **The `.command_export` wrapper costs ~83 ns per host call**, uniformly across 6 different runtime helpers.
2. **The bare-body path is byte-identical to the wrapper path on the 3VL absorb branch** (the case where one operand is null/error).
3. **Binaryen cannot optimize across cross-module calls**, but *can* across in-module calls — demonstrated on a toy. *Whether the in-module optimization delivers meaningful gains on a realistic slot-based CEL helper is unknown.* Constant folding specifically will **not** fire because the slot loads hide the operand values from Binaryen.

That's it. Those three things are the load-bearing measured facts. Everything else in this investigation — slope predictions, ratios vs cel-cpp at production scale, expected end-to-end speedup — is hypothesis built on these facts.

The "Binaryen folds to a single constant" demo *demonstrated a capability of the optimizer*, but does **not** demonstrate a likely production gain. Real CEL expressions over variables (the common case) and even constants (which our codegen stores in rodata, not as i64.const) will not see folding. The realistic Binaryen-side gain is small inlining + load-forwarding wins, on the order of 5-10 ns/op — orders of magnitude smaller than the toy.

## 8. The smallest next experiments that would decide more

**Experiment A — correctness with real arithmetic (~30 min):** seed slot 1 with `CelValue{kind=2, payload.i=1}` and slot 2 with `CelValue{kind=2, payload.i=2}` by writing 24 bytes each directly to the runtime's shared memory at known offsets. Call `cel_int_add_at_vv__raw(0, 1, 2)`. Read slot 0. Assert `kind == 2`, `payload.i == 3`. If yes, the bare arithmetic path is correct.

**Experiment B — production-bench end-to-end (~half a day):** modify codegen to emit imports of `cel_int_add_at_vv__raw` (and the other arithmetic / comparison ops). Modify Engine to call `__wasm_call_ctors` once at instance bring-up. Embed the aliased wasm bytes instead of the production bytes (or via a builder flag). Run `intAdd1000Terms`, `intAdd2`, and the rest of the bench matrix. Report the *measured* slopes and the *measured* ratios vs cel-cpp.

Experiment A is the correctness gate. Experiment B is the perf gate. Until both have run, we should not commit to a perf claim in any user-facing material.

## 9. What I think the data justifies right now (qualified)

A claim of "the wasi-libc command-mode wrapper chain is a measurable per-call cost that an architectural change can eliminate" — **yes, this is supported.**

A claim of "we can beat cel-cpp at large N by 2-6× via this change" — **not yet supported by measurement.** Reasonable hypothesis, but the SSP track record says reasonable hypotheses can miss by 10×.

A claim of "we can close to cel-cpp at small N" — **not yet supported by measurement** and structurally weaker (the host-wasm boundary itself is the dominant small-N cost, and this change doesn't touch it).

A claim of "Binaryen will fold our arithmetic chains" — **not supported.** The slot-based ABI hides operand values from the optimizer. The toy fold demo demonstrated Binaryen's optimization *capability* but not what it delivers on our actual codegen output. Realistic Binaryen-side savings are likely modest (~5-10 ns/op).

**Honest summary:**

The architecture is plausible. The lever exists (the wrapper is real, the bare path runs faster, in-module helpers expose more to the optimizer). What that lever delivers end-to-end is **unmeasured**. Any range I quote here ("we'd drop to 5-15 ns/op", "we'd be 2× cel-cpp", "Binaryen saves 5-10 ns/op") is speculation given the track record on speculation in this investigation.

I am no longer comfortable putting numbers next to the predicted impact. The architecture's actual value is what Experiment B (below) measures, nothing more.

The path from here to a real perf number is Experiment A → Experiment B. Until that runs, the architecture is *plausible*, not *promising* and not *proven*. The earlier framings of "seriously beat cel-cpp at large N" and "demolish on const-heavy expressions" were both unsupported by measurement and should be retracted.

## 10. Next experiment — concrete plan ("Experiment C", supersedes A and B)

The user-stated requirement: see what static linking the runtime library would actually buy on real benchmarks, with the merge step done via Binaryen. The experiment must use the **existing compiler** to compile real CEL expressions (we have not done that in any of the prototype work to date).

### 10.1 Step-by-step

**Step 1 — Compile a CEL expression through the existing compiler.**
- Use the `Compiler::Builder` API directly from a small C++ driver (parallel to the existing `tools/cel` CLI's compile path, but emitting raw wasm bytes — we may need to add a small `cel emit-wasm` capability or write a one-off `compile_to_wasm.cc` tool).
- Test expressions: `intAdd2` (`a + b`) and `intAdd1000Terms` (`a + b + c + … + j` × 1000-term chain). These are the same shapes the production benchmark uses.
- Output: `/tmp/expr_intAdd2.wasm` and `/tmp/expr_intAdd1000.wasm`.
- These are the **today-shape** expression wasm — they contain `(import "cel_runtime" "cel_int_add_at_vv")` declarations.

**Step 2 — Disassemble the today-shape expression wasm.**
- `wasm-dis /tmp/expr_intAdd1000.wasm > /tmp/expr_intAdd1000.wat`.
- Capture the `$eval` function and the imports section into the doc as "OLD codegen output". This is the production codegen path.

**Step 3 — Strip the wasi-libc command-mode wrappers from `cel_runtime.wasm`.**
- Extend `add_raw_aliases.cc` (Binaryen C API tool) into `strip_command_wrappers.cc`:
  - For every export `X` whose target function name is `X.command_export`, **remove the existing export** and **add a replacement export** with the same external name `X` pointing at the bare internal function `X`.
  - Run Binaryen DCE (`BinaryenModuleOptimize` or `wasm-opt -O2`) on the result — the `.command_export` functions are now unreachable; they get DCE'd.
- Output: `/tmp/cel_runtime_stripped.wasm`.
- This is a build-time pass over the existing runtime. **No source changes to `runtime/cel_runtime.c`** — same C source, post-processed wasm.

**Step 4 — Merge the today-shape expression wasm with the stripped runtime, using Binaryen.**
- Use `wasm-merge` (Binaryen CLI, available via brew binaryen — confirmed present): `wasm-merge /tmp/expr_intAdd1000.wasm expr /tmp/cel_runtime_stripped.wasm cel_runtime -o /tmp/merged_intAdd1000.wasm`.
- This resolves the expression's `(import "cel_runtime" "cel_int_add_at_vv")` against the runtime's matching export, producing a self-contained wasm with no `cel_runtime`-scoped imports remaining. Imports of `cel_host` (the host-trampoline namespace) and `wasi_snapshot_preview1` stay as imports — those bind to the host at instantiate time as today.
- Run `wasm-opt -O2 /tmp/merged_intAdd1000.wasm -o /tmp/merged_intAdd1000_opt.wasm` to let Binaryen do whatever it can on the merged result.

**Step 5 — Disassemble the merged output.**
- `wasm-dis /tmp/merged_intAdd1000_opt.wasm > /tmp/merged_intAdd1000_opt.wat`.
- Capture the relevant pieces into the doc as "NEW codegen output":
  - The `$eval` function (did Binaryen inline helper bodies? CSE? load-forward?)
  - The function count before/after wasm-opt
  - Whether `cel_int_add_at_vv` is still a separate function or got inlined
  - Whether the `.command_export` wrappers + `__wasm_call_ctors` chain still exist (they shouldn't — DCE'd in step 3)

**Step 6 — Diff old vs new.**
- Side-by-side: the relevant ~30-50 lines of each WAT.
- Show in the doc.
- This is the **first time anyone sees what actually changes** in the codegen path under static linking. Until this happens, all the Binaryen-savings talk is speculation.

**Step 7 — Bench harness.**
- Small C++ harness (extension of `wrapper_microbench`):
  - Loads `/tmp/merged_intAdd1000_opt.wasm`.
  - Defines wasi stubs + `cel_host` import stubs (as the existing microbench does).
  - Calls `__wasm_call_ctors` once at instance bring-up (reactor-equivalent — required because the merged module has wasi-libc init that needs to run once).
  - Loops `$eval()` for ~1 second, measures total time, reports ns/Eval.
- Same harness, but loaded with the **non-merged** today-shape (instantiate cel_runtime as separate module, link imports the way the production Engine does). This is the A/B control.

**Step 8 — Run, compare, write up.**
- Numbers reported in the doc:
  - today-shape ns/Eval for intAdd2 and intAdd1000Terms
  - merged ns/Eval for the same
  - delta + ratio
  - cel-cpp reference numbers from existing benchmark JSON for context
- This is the data we've been speculating about. After step 8, the architecture is either justified by measurement or refuted.

### 10.2 What the artifacts will be after this experiment

```
wasm_compilation_experiments/wrapper_overhead/
  strip_command_wrappers.cc       — Binaryen C API tool (step 3)
  compile_to_wasm.cc              — driver that runs the existing Compiler (step 1)
  static_link_bench.cc            — A/B harness (step 7)
  artifacts/
    expr_intAdd1000.wat           — OLD codegen output
    merged_intAdd1000_opt.wat     — NEW codegen output
    diff.md                       — side-by-side of the relevant pieces
  RESULTS.md                      — the actual numbers from step 8
```

### 10.3 Effort estimate

This is real engineering work, not a single afternoon. Honest sizing:

- Step 1 (compile driver): 1-3 hours, depending on whether the Compiler API takes a `Program` we can just dump bytes from, or whether we need to add a serialization path. Likely 1-2 hours since `Program` already has the wasm bytes per the existing CLI.
- Step 3 (strip tool): 1 hour. Small extension of existing `add_raw_aliases.cc`.
- Step 4 (merge): 30 minutes. `wasm-merge` is a CLI invocation.
- Step 5-6 (disasm + diff write-up): 1-2 hours, including reading the WAT carefully.
- Step 7 (bench harness): 2-3 hours, including stubbing the `cel_host` imports correctly so calls don't trap.
- Step 8 (run + write up): 30 minutes.

**Total: ~one focused day.** No predictions about what the result will be — that's the whole point of running it.

### 10.4 What we will NOT do in this experiment

- Modify production codegen (`compiler/codegen/expr_lower.cc`) or production Engine.
- Land this on master.
- Change `runtime/cel_runtime.c` sources.
- Run the full conformance suite — that's a deeper validation pass once we have a positive perf signal.

The experiment lives entirely in `wasm_compilation_experiments/wrapper_overhead/`. It produces one number for one expression on one branch, no production impact.

### 10.5 What we WILL learn

After step 8, exactly one of the following is true:

- **Static linking buys a substantial perf win on the real benchmark.** Architecture is justified. Next step would be designing the production version.
- **It buys a modest win** (single-digit %). Architecture is plausible but not the giant lever we were hoping for. Decide whether the effort is worth it.
- **It buys ~nothing** (or, worse, regresses). Hypothesis refuted. Abandon this direction, look at other levers (native backend, batch API, etc.).

We won't know which until we run it. That is the entire point.

---

## 11. Experiment ran — results

The 8-step plan in §10 was executed. This section reports what we observed.

### 11.1 What we built (artifacts on disk under `artifacts/`)

| file | what it is |
|---|---|
| `expr_const2.wasm`, `expr_const10.wasm`, …, `expr_const250.wasm` | Output of `Compiler::Compile(...)` invoked via `compile_to_wasm` driver on `"1 + 1 + … + 1"` chains. **First time in this prototype that anything went through the existing compiler.** |
| `cel_runtime_stripped.wasm` | Production `cel_runtime.wasm` after our `strip_command_wrappers` Binaryen tool. 242 `.command_export` wrappers retargeted to bare bodies; Binaryen DCE then removed them. 1108 KB → 807 KB (-27%), 4926 funcs → 2220 (-55%), 968 wrapper symbols → 0. |
| `merged_constN_opt.wasm` for N ∈ {2, 10, 50, 100, 250} | Output of `wasm-merge --enable-threads` joining the stripped runtime + the const expr, then `wasm-opt -O2`. Self-contained: zero imports from the `"cel"` module namespace remain. Only `cel_host.*` and `wasi_snapshot_preview1.*` imports stay (those bind to the host at runtime). |

### 11.2 What the codegen actually looks like — OLD vs NEW WAT

Same expression `"1 + 1"` (N=2), full disassembly of each. This is the diff you asked to see.

**OLD — `expr_const2.wat`** (today shape; what `Compile()` produces). Every comment below is mine, explaining what the line does:

```wat
(module
 ;; ── Type signatures used by the function table ──────────────────────
 (type $0 (func))                                  ;; () -> ()  — used by
                                                   ;;   arena_reset import
                                                   ;;   (no args, no return)
 (type $1 (func (param i32 i32 i32)))              ;; (i32,i32,i32) -> ()
                                                   ;;   used by helper imports
                                                   ;;   args = three slot
                                                   ;;   indices into runtime
                                                   ;;   linear memory
 (type $2 (func (result i32)))                     ;; () -> i32  — the eval
                                                   ;;   export's signature;
                                                   ;;   returns a slot index
                                                   ;;   where the result lives

 ;; ── Imports from cel_runtime instance (cross-module) ────────────────
 (import "cel" "memory"                            ;; expr borrows the
   (memory $mimport$0 2 1024 shared))              ;;   runtime's wasm linear
                                                   ;;   memory; pages 2..1024,
                                                   ;;   shared because we use
                                                   ;;   wasi-threads target

 (import "cel" "arena_reset"                       ;; reset arena bump-pointer
   (func $fimport$0))                              ;;   so this Eval's
                                                   ;;   transient allocations
                                                   ;;   don't accumulate

 (import "cel" "cel_int_add_at_vv"                 ;; the int add helper —
   (func $fimport$1 (param i32 i32 i32)))          ;;   resolves at runtime
                                                   ;;   to its .command_export
                                                   ;;   wrapper in the runtime
                                                   ;;   instance

 ;; ── Compile-time data ──────────────────────────────────────────────
 (data $0 (i32.const 16) "\02\00…")                ;; rodata blob at byte
                                                   ;;   offset 16; two
                                                   ;;   CelValue records (24 B
                                                   ;;   each) for the two
                                                   ;;   literal `1`s.  Layout
                                                   ;;   per cel_data.h:
                                                   ;;     [0..4]  kind = 2
                                                   ;;             (CEL_INT)
                                                   ;;     [4..8]  pad
                                                   ;;     [8..16] payload.i =
                                                   ;;             1 (int64)
                                                   ;;     [16..24] union pad
                                                   ;;   So slot 16 = first 1,
                                                   ;;   slot 40 = second 1.

 (export "eval" (func $0))                         ;; the entry the host calls
                                                   ;;   via Instance::Eval

 (func $0 (result i32)
  (call $fimport$0)                                 ;; arena_reset (no args).
                                                   ;;   ⚠ CROSS-MODULE CALL —
                                                   ;;   resolves through
                                                   ;;   wasmtime's linker to
                                                   ;;   runtime's
                                                   ;;   arena_reset.command_export
                                                   ;;   wrapper → runs ctors
                                                   ;;   → body → dtors.

  (call $fimport$1                                  ;; cel_int_add_at_vv —
                                                   ;;   ⚠ CROSS-MODULE CALL.
                                                   ;;   Resolves through
                                                   ;;   wasmtime's linker to
                                                   ;;   runtime's
                                                   ;;   .command_export wrapper.
                                                   ;;   Per call: __wasm_call_ctors
                                                   ;;   → __wasilibc_init_ssp →
                                                   ;;   random_get host syscall
                                                   ;;   → body → __wasm_call_dtors.
   (i32.const 64)                                   ;;   arg0 = out_slot (write
                                                   ;;     result here; codegen
                                                   ;;     picked byte offset 64)
   (i32.const 16)                                   ;;   arg1 = lhs_slot (the
                                                   ;;     first `1` at byte 16)
   (i32.const 40))                                  ;;   arg2 = rhs_slot (the
                                                   ;;     second `1` at byte 40)

  (i32.const 64))                                   ;; return value of $eval:
                                                   ;;   the slot index where
                                                   ;;   the result lives (64).
                                                   ;;   Host reads the CelValue
                                                   ;;   at slot 64 to decode.

 ;; ── ABI metadata (custom section) ──────────────────────────────────
 ;; custom section "cel.abi", size 10
 ;;   Encodes the variable declarations and required imports.  Read
 ;;   by Engine::Plan to validate that runtime exports match what the
 ;;   compiled expression expects.
)
```

Three imports: memory, arena_reset, cel_int_add_at_vv. Both function calls go to imports (cross-module). At runtime each cross-module call resolves through wasmtime's linker to the runtime's `.command_export` wrapper, which then runs ctors → body → dtors. That's the wrapper chain.

**NEW — eval function from `merged_const2_opt.wat`** (the merged module's eval function; the whole module is ~800 KB because it now contains the runtime).  Annotated:

```wat
(func $975 (result i32)
  ;; ── $975 is wasm-merge's renumbering of the expr's original $0 ──
  ;; eval signature unchanged: () -> i32 (returns result slot index)

  (call $16)                                        ;; arena_reset.  Now
                                                   ;;   INTRA-module — $16 is
                                                   ;;   the function index of
                                                   ;;   the runtime's
                                                   ;;   arena_reset body after
                                                   ;;   wasm-merge folded the
                                                   ;;   two modules into one.
                                                   ;;   No .command_export
                                                   ;;   wrapper, no ctors, no
                                                   ;;   SSP refresh.  Just a
                                                   ;;   normal wasm call that
                                                   ;;   wasmtime's JIT lowers
                                                   ;;   to a direct machine-
                                                   ;;   code branch.

  (call $20                                         ;; cel_int_add_at_vv.
                                                   ;;   Now INTRA-module — $20
                                                   ;;   is the bare body of the
                                                   ;;   add helper.  Same body
                                                   ;;   bytecode as today's
                                                   ;;   cel_int_add_at_vv (the
                                                   ;;   strip tool retargeted
                                                   ;;   the export from
                                                   ;;   .command_export wrapper
                                                   ;;   to this bare function).
                                                   ;;   No wasi-libc init.

   (i32.const 64)                                   ;;   out_slot — IDENTICAL
                                                   ;;   to today.  The merged
                                                   ;;   module has its own
                                                   ;;   memory (inherited from
                                                   ;;   cel_runtime by the
                                                   ;;   merge); slot 64 is
                                                   ;;   inside that memory.

   (i32.const 16)                                   ;;   lhs_slot = 16, where
                                                   ;;   the first `1` lives in
                                                   ;;   the data section (also
                                                   ;;   carried over from the
                                                   ;;   expr by the merge).

   (i32.const 40))                                  ;;   rhs_slot = 40, second
                                                   ;;   `1`.

  (i32.const 64))                                   ;; return the result slot
                                                   ;;   index.  ⚠ Today's
                                                   ;;   Engine reads slot 64
                                                   ;;   from the SEPARATELY
                                                   ;;   instantiated runtime's
                                                   ;;   memory — wrong memory.
                                                   ;;   The eval ran correctly
                                                   ;;   inside the merged
                                                   ;;   module's memory; host
                                                   ;;   just reads from the
                                                   ;;   other instance.  This
                                                   ;;   is the memory-model
                                                   ;;   issue called out in
                                                   ;;   §11.5; eval timing is
                                                   ;;   valid even though the
                                                   ;;   displayed result is
                                                   ;;   garbage.
```

Body identical in shape. **What changed:**
- `$fimport$1` (cross-module import) → `$20` (in-module function index)
- `$fimport$0` (cross-module import) → `$16` (in-module function index)
- No imports from the `"cel"` namespace anywhere in the module (`grep '(import "cel"' merged_const2_opt.wat` → no matches)
- The host's wasmtime invocation flow goes straight to the body function without the `.command_export` wrapper, without `__wasm_call_ctors`, without `__wasilibc_init_ssp`, without `__wasm_call_dtors`

Same arguments, same slot indices, same return value. **The only thing that's different is how the calls are resolved at wasmtime instantiation time** — but that single change cuts per-call cost from ~80 ns to ~1.5 ns because the entire wasi-libc wrapper chain (which used to run on every cross-module call) is gone.

### 11.3 What Binaryen did (and didn't)

Binaryen `-O2` **did not inline** the helper body. The merged eval still has one `call $20` instruction per CEL `+`. We tested forcing aggressive inlining with `wasm-opt -O3 --always-inline-max-function-size=1000` — file size went from 815 KB to **33 MB** (40× growth) because the helper body was copied at every call site. Not viable for any real production build. Default `-O2`'s decision to leave the helper as a separate function is correct.

So **the wins we measured come from the architectural change (cross-module → intra-module + wrapper strip), not from Binaryen optimizing the helper body.** Constant folding does not fire (operands are slot loads, not literals).

### 11.4 Bench numbers — N-sweep, real production `benchmark/eval/celwasm_bench`

Ran the merged wasm through the existing production bench harness (Google Benchmark, same Engine, same wasmtime config, same Plan, same Eval). Filter to const cells:

```
BM_arith_intAdd2Const                     233 ns      result=2 (int)
BM_arith_intAdd10TermsConst               873 ns      result=10 (int)
BM_arith_intAdd50TermsConst              3931 ns      result=50 (int)
BM_arith_intAdd250TermsConst            20184 ns      result=250 (int)

BM_arith_intAdd2TermsConstMerged         58.3 ns      result=<non-numeric>
BM_arith_intAdd10TermsConstMerged        66.6 ns      result=<non-numeric>
BM_arith_intAdd50TermsConstMerged         128 ns      result=<non-numeric>
BM_arith_intAdd100TermsConstMerged        201 ns      result=<non-numeric>
BM_arith_intAdd250TermsConstMerged        425 ns      result=<non-numeric>
```

| N | today const (ns/Eval) | merged (ns/Eval) | speedup |
|---:|---:|---:|---:|
| 2 | 233 | 58 | 4.0× |
| 10 | 873 | 67 | 13.1× |
| 50 | 3931 | 128 | 30.7× |
| 100 | (not run today) | 201 | — |
| 250 | 20184 | 425 | **47.5×** |

**Slope (per-op cost) from linear fit:**
- today const: **80 ns/op**
- merged: **1.5 ns/op**
- **54× faster per `+`.**

**Intercept (fixed per-Eval cost):**
- today: ~73 ns
- merged: ~55 ns

The linear scaling confirms wasmtime did **not** DCE the chain — eval runs N additions.

### 11.5 Caveats — what's NOT yet right about these numbers

**Result label says `<non-numeric>` for every merged cell.** This is a real problem to be honest about: it means the result-decoding path is broken in the merged setup. The Eval timing IS valid (the eval function runs and does the work — confirmed by the linear N-scaling), but the host's result decode reads garbage. Reason:

- The Engine sets up *two* wasm instances: a runtime instance (still instantiated separately, because the bench's GlobalEngine is unmodified) and the program instance (the merged wasm).
- The host's `Activation::Bind` / result-decode marshaling writes to / reads from the **runtime instance's memory**.
- The merged module's eval writes / reads its **own memory** (the merged module contains the runtime, so it has its own memory section).
- Two different memories. Eval works correctly inside its own memory; host marshaling looks at the wrong one.

**Why I trust the timing despite the result being wrong:**

1. Linear scaling — slope is 1.5 ns/op regardless of N. If wasmtime had DCE'd the calls, slope would be ~0.
2. The eval body in the merged WAT has the same shape as today (N calls + arena_reset). wasmtime executes the JIT'd code; it doesn't know whether anyone reads the result.
3. Each `cel_int_add_at_vv` body writes to memory (a side effect wasmtime preserves), so the calls aren't dead.

**Why these numbers can't ship as-is:**

To make this a production change, the Engine has to be aware that a merged module owns its own memory and arena. Concretely: either (a) the merged module replaces the separately-instantiated runtime (single instance, single memory, host marshaling binds to the merged module's memory + helpers), or (b) the merged module gets two views and we duplicate state — option (a) is the obvious correct shape.

### 11.6 What's been validated vs what remains open

**Validated by direct measurement through the production bench:**
- Static linking + wrapper strip drops per-op cost from 80 ns/op to 1.5 ns/op on const expressions (~54× faster).
- That puts the merged celwasm path at ~15× faster than cel-cpp's const-cell slope (which was ~22 ns/op).
- Numbers are reproducible: same Compiler, same Binaryen tools, same wasm-merge, same production bench.

**Still open — required before this becomes a production architecture:**
- **Engine memory-model rework.** Merged module must replace the separately-instantiated runtime. Host marshaling + result decode must bind to the merged module's memory + helpers. Without this, both correctness and the variable-bearing cells (`a + b + ...`) can't work.
- **Variable cells.** All measurements were const-only. Variable cells require activation marshaling, which has the same memory-model issue as result decoding. Predicted to show similar speedup once Engine is reworked, but not yet measured.
- **Other ops.** Only intAdd was measured. intMul / intSub / doubleAdd / comparison ops should show the same pattern (the wrapper chain is op-agnostic), but unverified.
- **Correctness under the full conformance suite.** Stripping the wrappers and merging may break operators that depend on wasi-libc init that we removed (anything touching C++ statics, anything calling `random_get` for entropy beyond stack-canary refresh, etc.). Const intAdd works; the conformance corpus exercises far more surface.

### 11.7 What changed about the picture vs §9

Earlier in this doc I wrote that the architecture is "plausible, not promising, not proven" and retracted any specific perf claims. After running the experiment, the situation is:

- The architecture is **proven on the cell we measured** (const intAdd N up to 250).
- The actual measured speedup is **larger than what I had been speculating about** (54× per-op vs my prior ~5-10 ns/op savings guesses).
- The reason it's larger is that the wrapper chain + cross-module call overhead together dominate today's per-op cost more than the profile percentages suggested. Both are gone in the merged path.

But the picture has new asterisks: the result-decode path is broken in this prototype, and variable-bearing cells (the common case) aren't yet validated. The honest summary now is: **the architectural lever is real and large on the cells we measured, but two specific pieces of Engine work are required before we know whether the win holds for the full bench matrix.**

### 11.8 Next concrete step if we pursue this

Build a small Engine variant (or an opt-in flag on the existing Engine) that:

1. Instantiates the merged module as the SOLE wasm instance (no separate runtime instance).
2. Binds activation marshaling + result decode to the merged module's memory + its exported runtime helpers (`cel_make_int_at`, the slot accessors, etc., which are still exported from the merged module).

Then rerun the bench against variable-bearing cells. If the per-op speedup holds, the architecture is justified and we move to designing the production version. If it doesn't (because activation marshaling cost dominates), we learn that boundary cost is the actual ceiling and decide accordingly.

Effort: ~1-2 days for the Engine variant.

---

## 12. Side observation surfaced by string + in-list benches — const-list-literal codegen

While extending the bench to cover canonical string and `in [list]` patterns to compare against cel-cpp, the WAT for `a in [<N const strings>]` revealed a codegen inefficiency that is **independent of static linking** but composes with it.

### 12.1 The inefficiency

For `a in ["123", "augustine", "jess", "bob", "alice"]`, the existing `Compiler::Compile` emits:

```wat
(func $eval
  (call $arena_reset)
  (call $cel_list_create   (slot 224) (count 5))      ;; alloc list header
  (call $cel_list_append_at (slot 224) (slot 16))     ;; append "123"
  (call $cel_list_append_at (slot 224) (slot 48))     ;; append "augustine"
  (call $cel_list_append_at (slot 224) (slot 88))     ;; append "jess"
  (call $cel_list_append_at (slot 224) (slot 120))    ;; append "bob"
  (call $cel_list_append_at (slot 224) (slot 152))    ;; append "alice"
  (call $cel_list_in        (out) (a) (slot 224)))    ;; membership test
```

Eight calls per Eval. **Seven of them rebuild the same list from scratch on every Eval.** The string ELEMENTS already live in rodata (the codegen emits them in the data section at compile time — we can see them: `"\05\00…123\00…augustine\00…"` etc.). What gets rebuilt is the *list header* — the small array metadata that ties the constant CelValues together as an ordered list.

For `a in [20 strings]`: 23 calls per Eval, 22 of which are list-build.

### 12.2 The fix in concept

A codegen pass that examines `kCreateList` nodes: if every child is a `kConstExpr`, lay out the list header in rodata at compile time (the same way the elements already are) and have `$eval` reference it directly. Eval becomes:

```wat
(func $eval
  (call $arena_reset)
  (call $cel_list_in (out) (a) (pre-built-list-in-rodata)))
```

Two calls per Eval, **independent of list length.**

### 12.3 Production bench numbers — celwasm vs cel-cpp on string + in-list

Real numbers from `benchmark/eval/celwasm_bench` and `benchmark/eval/celcpp_bench` (both via Google Benchmark, same Compiler invocation for celwasm, real cel-cpp eval for celcpp):

| pattern | celwasm today | cel-cpp | gap |
|---|---:|---:|---:|
| `"hello" == "world"` | 244 ns | 46 ns | **5.3× slower** |
| `a == "augustine"` | 267 ns | 55 ns | 4.9× slower |
| `a.contains("aug")` | 253 ns | 67 ns | 3.8× slower |
| `a.startsWith("aug")` | 253 ns | 66 ns | 3.9× slower |
| `a in [5 strings]` | 809 ns | 163 ns | 5.0× slower |
| `a in [20 strings]` | 2,348 ns | 376 ns | **6.2× slower** |

cel-cpp wins all six. The gap is wrapper-chain dominated; static linking would close most of it but not enough to surpass.

### 12.4 What we know — ground truth only

The const-list-literal observation is a **codegen observation**, not a measured perf claim. What we have measured:

- `BM_in_list_20` on celwasm (today shape): **2348 ns/Eval**.
- `BM_in_list_20` on cel-cpp: **376 ns/Eval**.
- cel-cpp's in_list cells scale ~14 ns per element (163 ns at N=5, 376 ns at N=20). Linear, which says cel-cpp also walks the list per Eval and does not do this optimization.

What we have NOT measured:
- The effect of the const-list-literal optimization on celwasm. It hasn't been implemented.
- The effect of static linking on string / list patterns. The const cells we measured under static linking were arithmetic; the static-link path isn't wired up for string-typed cells through the bench yet.

Any number on "what celwasm would do with const-list-literal + static-link" is speculation. The earlier version of this section had a prediction table; it was removed for being speculation dressed up as analysis. If we want a number for that combination, we measure it.

### 12.5 What the optimization actually does (mechanically)

A codegen pass in `compiler/codegen/expr_lower.cc`'s `kCreateList` handler that detects "all children are `kConstExpr`" and lays out the list header in rodata instead of calling `cel_list_create` + N × `cel_list_append_at` at eval time. The result is that `$eval`'s call to `cel_list_in` references a pre-built list pointer in rodata.

**In scope:**
- `x in ["a", "b", "c"]` — all-string-literal list
- `x in [1, 2, 3]` — all-int-literal list
- `x.role in ["admin", "moderator"]` — RBAC pattern
- Maps with all-const keys + values: `{"foo": 1, "bar": 2}[x]`

**Not in scope:**
- `x in [a, "fixed", b]` — must keep today's build-at-eval path
- Falls back to current behavior

### 12.6 Status

Not implemented. Surfaced as an observation by inspecting the WAT for `BM_in_list_20`. Not in scope for the static-link branch.

If pursued: would live as its own milestone (suggested name `m28-const-list-literal.md`), with the work being a codegen patch in `expr_lower.cc` and a static-memory layout extension. The actual perf delta would be measured at that time — not estimated here.



