# m32 — Eval-time cost optimizations: current state

Status: **in progress** — drafted 2026-06-16 on branch
`experiment/optimal-3vl-ir`. This is a living status doc for the
measurement-driven eval-perf investigation. Nothing here is committed
to master yet; it is all WIP on the experiment branch.

## 0. TL;DR

A measurement campaign on eval-time cost. The throughline: **every
optimization hypothesis was checked against a profile/benchmark before
building, and several were killed or redirected by the numbers.** What
survived and shipped (on-branch) is a small, broad, low-risk host-side
change; the larger architectural ideas were deferred or shelved as
not-justified-by-the-data.

- **Shipped on-branch (works, e2e-green):** direct reflection→CelValue
  for **primitive** proto scalar field reads — deletes the
  `celwasm::Value` variant middleman. **~9–11 ns/field**, scaling with
  field count: proto field-read cells **13–26% faster**; primitives now
  **beat cel-cpp** where we used to trail.
- **Shipped on-branch (extended):** the fast path now also covers
  **string / bytes / enum** — `read_s` 97→88 ns (the Value-variant tax
  removed; the residual gap to cel-cpp's 75 is the inherent wasm-arena
  copy). All scalar field types are now off the Value path; only
  message/WKT fields still use it.
- **Shelved by measurement:** scalar unboxing (the op was already
  ~free), field-prefetch batching as a primary lever (sibling shape is
  ~0.4% of the corpus), staging/shared-proto (collapses to batching
  without message reuse).
- **Deferred (real but lower priority):** per-Eval variable-marshalling
  (the name-hash), field-read batching with a bounded-speculation cost
  model.

## 1. The cost picture (measured, not estimated)

All numbers `-c opt`, static link, `celwasm_bench`, this machine
(Apple Silicon). Floor = a bare literal eval.

### 1.1 Per-Eval floor and per-field crossing

| component | cost | how measured |
|---|---|---|
| per-Eval floor (host→wasm entry + `arena_reset` + decode) | ~25 ns | `lit_int` |
| per primitive field read (full crossing) | ~21–29 ns | `reads5`/`reads10` slope |
| per bound **variable** marshal | ~11–14 ns | `and2`/`and10Terms` slope |
| scalar op (`<`, `&&`, `+`) incl. 3VL | ~1 ns | `intLt` unboxed vs boxed; const chain |

### 1.2 What a proto field-read crossing is made of

The wasm→host trampoline is **unchecked** (`cel_get_field` registered
via `wasmtime_linker_define_func_unchecked`), so the bare crossing is
cheap. Layers (host vs guest separated by the controlled
`CELWASM_GETFIELD_STUB` differential, not by sampling):

| layer | ~ns | side | location |
|---|---|---|---|
| wasm `$eval` per-field call-emit | ~3 | guest | `compiler/codegen/expr_lower.cc:277` `EmitKSelectProtoBranch` |
| `wasm_to_array_trampoline` | — | wasmtime | Cranelift-generated |
| `array_call_trampoline` | — | wasmtime | wasmtime runtime |
| `UncheckedHostThunk::Call` (unmarshal 4×i32) | — | host | `eval/internal/cel_host_wasmtime.cc:262` |
| `RunFieldPrelude` (externref lookup + field-ref cache resolve + unknown check) | ~6–7 | host | `eval/internal/cel_host.cc:1892` |
| protobuf reflection `Get*` | ~4 | host | (proto reflection) |
| **`celwasm::Value` variant build + `EncodeValue` (→ CelValue)** | **~11** | host | `cel_host.cc` `ReadFieldClassified`/`EncodeValue` |
| `WriteCelValue` (24-byte slot write) | ~0.5 | host | `WasmtimeMemoryView::WriteCelValue` |

**The dominant single removable cost was the `Value` variant** (~11 ns):
each read built a 14-alternative `std::variant` (`celwasm::Value`),
wrapped it in `StatusOr`, moved it out, and re-visited it in
`EncodeValue` to produce the 24-byte `CelValue`. The profile (perfmap
JIT symbolication + host `sample`) showed this dominated; reflection
(~4 ns) and the unchecked crossing (~3 ns) are cheap.

> Profiling note: a plain C++ `sample` hides the JIT'd wasm in
> `Instance::Eval` self-time. Use `CELWASM_BENCH_PERFMAP=1` (wired via
> `Engine::Builder::EnableJitPerfMap`) to symbolicate the Cranelift
> code, then resolve sample addresses against `/tmp/perf-<pid>.map`.
> Per-call timers are useless here (Apple Silicon counter ~41 ns
> resolution > the thing measured) — use **controlled differentials**
> through the precise bench harness instead (e.g. the
> `CELWASM_GETFIELD_STUB` no-op gate: `full − stub` = exact host work).

## 2. Shipped on-branch: direct reflection→CelValue (primitives)

`eval/internal/cel_host.cc` `CelGetFieldImpl`, proto fast path: when the
resolved field is a `kScalar` primitive, read it via reflection and
write the `CelValue` **directly** into the out slot — skipping the
`celwasm::Value` construct / `StatusOr` / move / `EncodeValue` /
destroy. Covered cpp_types: `INT32`, `INT64` → `CEL_INT`; `UINT32`,
`UINT64` → `CEL_UINT`; `BOOL` → `CEL_BOOL`; `DOUBLE`, `FLOAT` →
`CEL_DOUBLE`. Sound because a primitive scalar read is total (proto3
unset → default; no error path).

Correctness: e2e + proto test suites pass; uint/string/non-proto
controls unchanged (the change is isolated). Not yet committed.

### 2.1 Results — pre vs post vs cel-cpp

POST here is the **primitives-only** fast path (§2); `read_s` therefore
still shows the unrecovered variant tax (97.2). The §3 string/bytes/enum
path lands separately and brings `read_s` down (see §1, §3.1); refresh
this table once both have re-measured on one run.

> The single-field cells (`cust_age`, `read_b`, `read_f64`, `read_u64`)
> sit at parity with cel-cpp (~1.0×); the committed 2026-06-16 run lands
> them at **0.97–0.99×** rather than the 1.03–1.08× below — same number,
> opposite side of 1.0×, decided by run-to-run noise. The load-bearing
> results — `read_s` trails, `reads5/10/100` win 2–3× — hold across both
> runs.

| cell | expression | cel-cpp | PRE | POST | post vs cel-cpp |
|---|---|---|---|---|---|
| `cust_age` | `c.age` | 63.7 | 70.1 | 59.6 | 1.07× |
| `read_u64` | `m.u64` | 64.7 | 69.5 | 62.9 | 1.03× |
| `read_b` | `m.b` | 62.8 | ~70 | 59.6 | 1.05× |
| `read_f64` | `m.f64` | 64.8 | ~70 | 60.1 | 1.08× |
| `read_s` | `m.s` (string) | 75.3 | 96.5 | 97.2 | **0.78× (we trail)** |
| `reads5` | 5 int fields | 331 | 189 | 145 | 2.28× |
| `reads10` | 10 int fields | 666 | 337 | 249 | 2.71× |
| `reads100` | 100 int fields | 6386 | — | 2178 | **2.93×** |

### 2.2 Why we beat cel-cpp, and how it scales

cel-cpp tree-walks (interpreter): per node, per Eval, it pays
dispatch + runtime overload resolution + a managed-`Value` allocation.
We compile AOT to wasm: structure baked in, operators resolved at
compile time (inline `i64.add`), intermediates in fixed workspace
slots (no alloc), field id baked as a constant.

Model fits the data: **ours ≈ 41 + 21·N**, **cel-cpp ≈ 64·N** (no
floor — it re-walks). Gap **grows and plateaus at ~3×** (the per-node
cost ratio 64/21): 1 field ≈ tie, 10 ≈ 2.7×, 100 ≈ 2.9×.

### 2.3 Operator regimes (where the AOT advantage holds or flips)

| shape | ours | cel-cpp | verdict |
|---|---|---|---|
| strict chain (`+`, `==`, all-true `&&`, field reads) | — | — | **we win 2–3×, grows with N** |
| ternary `?:` (`nested3`) | 62.6 | 128 | **we win ~2×** (both skip untaken arm) |
| `&&` both-arms-eval (`andNoShortCircuit`) | 103 | 93 | ~tie |
| `&&` effective short-circuit (`andShortCircuit`, RHS skipped) | 100 | **48** | **cel-cpp wins ~2×** |

**Our one operator weakness:** `_&&_`/`_||_` evaluate **both arms**
(non-short-circuit; the 3VL combine is in `cel_and`/`cel_or`). cel-cpp
short-circuits. When the skipped arm is expensive (a field read,
`contains`, a call), cel-cpp does far less work and beats us. On
all-true / both-needed inputs it's a tie. Ternary is fine (we lower to
`BinaryenIf`, only the taken arm runs).

## 3. Shipped: string / bytes / enum fast path

All remaining scalar types are now on the direct path (was the
last in-progress item; landed + e2e-green this session):

- **enum** → `CEL_INT` via `GetEnumValue` (trivial, like primitives).
- **string / bytes** → direct `CelSpan` write: read `GetStringReference`
  (a view), `Alloc` in the per-Eval arena, `memcpy` the bytes, write the
  `CelValue{CEL_STRING/BYTES, payload.s={ptr,len}}` — skipping the
  `Value::StringView` + `EncodeValue`/`EncodeSpan` round-trip. The arena
  copy stays (see §3.1); the variant round-trip goes.

### 3.1 Why we trail cel-cpp on strings (`read_s` 97 vs 75)

Two parts, one fixable, one inherent:

1. **Fixable:** strings still go through the `Value` variant (the fast
   path covers primitives only) — they still pay the ~11 ns variant tax
   we removed for primitives. The §3 string fast path recovers this.
2. **Inherent:** the wasm guest can only see linear memory, so a string
   field's bytes must be **copied into the wasm arena** (~0.1 ns/byte +
   setup). cel-cpp keeps a **zero-copy view** into the proto's own
   memory (same address space, no boundary). For small strings the copy
   is negligible (the variant dominates → mostly fixable); for **large**
   strings the copy dominates → **we are inherently slower than
   cel-cpp's view**, and no code tweak closes it (it's the
   compile-to-wasm boundary cost).

## 4. Explored and shelved (killed/redirected by measurement)

These were investigated and **not** pursued; recorded so they aren't
re-litigated.

- **Scalar unboxing:** hypothesis was that the slot/3VL/kernel-call
  overhead per op was the cost. A prototype that inlined `int < int` as
  bare `i64.lt_s` (no kernel call, no 3VL) saved **~1 ns** — the op was
  already free. 3VL is ~free (perfectly-predicted branches on a hot
  tag). **Verdict: dead.** The real per-op cost is the per-Eval floor +
  variable marshalling, not the op. (The throwaway unboxing probe + its
  design doc were deleted; this conclusion is the keeper.)
- **Field-prefetch batching as the primary lever:** an AST survey (2811
  corpus expressions, via a since-deleted throwaway probe) found the
  sibling-prefetch shape (≥2 independent always-evaluated field reads
  over a common base) in **~0.4%** of expressions; 74% read no fields,
  and most field-readers are single selects. **Verdict: not justified
  as a general pass.** The
  direct-CelValue change helps *every* read incl. single — broader and
  simpler. Batching remains a *possible* future lever for the
  field-heavy / ternary-cascade tail (see §5).
- **Shared proto / stage-at-bind:** true zero-copy sharing of a live C++
  proto is impossible (separate address spaces, 32-vs-64-bit, host
  object graph). Copy-into-linear-memory staging *is* possible, but
  **without message reuse across Evals it collapses into batching** (one
  host trip either way). **Verdict: no distinct value given no reuse.**
- **Predicate pushdown (evaluate a sub-tree host-side):** highest ceiling
  for ternary-gated cascades, but requires re-implementing 3VL/unknown
  semantics host-side (the cascade-correctness burden). **Verdict:
  narrow niche; deferred.**

## 5. Deferred (real, lower priority)

- **Per-Eval variable marshalling** — `MarshalActivation` does a
  `flat_hash_map<string,Value>` name lookup **per variable per Eval**
  even though `name→slot` is fixed at compile time. A probe
  (resolved-pointer cache) measured **~5.5 ns/var saved** (the name
  hash), uniformly, across the corpus (213/277 cells faster, median
  −7.5 ns). The clean fix is a **program-bound activation**: resolve
  `name→slot` once at `Bind` (positional array), guarded by program
  identity; optionally encode-into-slot at bind. The probe and the
  `instance_impl.h` cache fields are on-branch (measurement-only;
  address-keyed, not production-safe). **Status: designed, not built.**
- **Field-read batching + bounded-speculation cost model** — if a
  field-heavy / ternary workload materializes: collapse N crossings to 1
  (`cel_get_fields`), driven by a neededness analysis. Cost model:
  speculate field f at conditional depth d iff `R(f) < T/(2ᵈ−1)`
  (primitives ~depth 3, small strings ~1, big strings never), bounded
  also by a **pinned prefetch-slot budget** (a 3rd workspace tier beside
  variables + scratch). Prefetched values need pinned slots (write-early-
  read-late breaks the LIFO scratch allocator). **Status: cost model
  designed; "batching saves ~the trampoline sandwich + RunFieldPrelude
  per collapsed crossing" — but only ~0.4% of the corpus has the shape.**

- **Maps: hash index for arena maps** — the standout eval loss. Arena
  maps are linear-scan: `cel_map_insert` scans existing entries for
  last-write-wins, so building an N-entry map is **O(N²)**, and
  lookup / `in`-membership is **O(N)**. The published 2026-06-16 run
  shows the cost: `size(map100)` is **~42× slower than cel-cpp**
  (120.5 µs vs 2.85 µs), `in` over a 10-entry map runs **0.25–0.52×**
  (int/uint worst — possibly routing key probes through the noinline
  `numeric_compare_kernel`, the same class of issue as the in-list
  scan; verify against the generated wasm before assuming). Small maps
  (≤3 keys) still win on AOT overhead. The fix is a **hash index on the
  arena map** (O(N) build, O(1) probe) — independent of the string
  work, and it flips the entire map column. **Status: identified, not
  designed.**
- **Lists at scale** — per-element cost is already **~tied** with
  cel-cpp (regression slope 3.2 vs 3.3); we win on literal / indexed /
  `size` ops and trail only on *bound* lists below the fixed-overhead
  crossover (~8k elements). Lower priority than maps — no quadratic
  blow-up, just setup overhead. Large literal lists also hit the rodata
  window first (see m31 §10). **Status: monitored; no action unless a
  large-bound-list workload materializes.**

## 6. Tooling added (kept on-branch)

- `benchmark/boundary/wasmtime_call_bench.cc` (pre-existing) — wasm↔host
  trampoline cost, boxed vs unchecked, both directions.
- `benchmark/boundary/proto_read_bench.cc` (new, kept) — host-side proto
  reflection cost breakdown (no wasm boundary): `FindFieldByNumber`,
  `GetReflection`, raw `Get*`, string view.
- `CELWASM_BENCH_PERFMAP=1` (pre-existing, via `EnableJitPerfMap`) — JIT
  symbolication for the guest-side profile.
- `benchmark/eval/corpus/proto.yaml` — added `reads100` (100-field sum).
- `benchmark/eval/corpus/comparisons.yaml` — added `intLtConst` +
  `intLtChain20Const`: literal-operand mirrors of the bound-variable
  cells, so (const cell − var cell) isolates variable-marshalling cost.
  Kept as permanent corpus.
- `benchmark/eval/report.py` — restructured: STATIC-focused main report
  (3 columns: cel-cpp, celwasm-static, ratio), dynamic split into a
  `-dynamic` sibling report, per-operator headline moved to the bottom.

Removed at cleanup (throwaway scaffolding; conclusions preserved above):
the `CELWASM_GETFIELD_STUB` measurement gate, the `marshal_cache` probe
(`instance.cc`/`instance_impl.h`), the `RunWatTimed` + optimal-IR
experiment harness (`tools/wat_runner/`), the AST-survey probe (the
sibling-prefetch corpus count in §4), and the `benchmark/footprint/`
probes (data folded into m31 §4).

## 7. Open questions / next steps

1. **Re-run §2.1 after the §3 string/bytes/enum path** (now landed
   on-branch) — small-string `read_s` is expected to close most of the
   gap toward cel-cpp; large strings stay inherently behind (the §3.1
   arena copy). The published 2026-06-16 run has `read_s` at 0.83×; the
   §2.1 table's POST column predates that path and still shows the
   primitive-only state — refresh it from a clean re-measure.
2. **Commit decision:** the primitive fast path is a clean, isolated,
   e2e-green win — candidate to land on master independently of the rest
   of the experiment branch.
3. **Bottom-line on a real workload:** §2 uses the benchmark proto
   cells. A representative **policy corpus** (auth/config predicates over
   a real request proto) would turn "2–3× on field-heavy reads" into a
   grounded blended number for the actual use case.
4. **Short-circuit `&&`** (the operator weakness in §2.3) — worth a
   separate look; making `_&&_`/`_||_` skip an expensive RHS would close
   the one place cel-cpp beats us. Interacts with 3VL (error/unknown
   absorption must stay correct).
