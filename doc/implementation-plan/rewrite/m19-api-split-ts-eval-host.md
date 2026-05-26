# M19 — `api/` compiler⁄eval split + a parallel TypeScript eval host

Status: **plan — drafted 2026-05-24; Phase 0 in progress (2026-05-25).**
Probe-gated: probes resolve before the slices they gate begin. **Gates G1
(plumbing), G2 (proto), and G-conf (conformance bridge) are GREEN** —
P-1, P-3, P-4 (keystone), P-5, P-6 (real protobuf-es bind), P-7, P-8
(conformance bridge: 285/285 zero-diff vs C++), P-9 (object/JSON + list +
map bind) all PASS (verdicts in §0.5 / §5b). **Slices B and C are
unblocked. Only P-2 (browser) remains.** Slice A (the C++ `api/` split)
has no probe gate and can start anytime. No implementation slice has
begun; the TS code so far is the throwaway probes under `ts/eval/probes/
m19/` + the working `conformance_ts` bridge (`ts/conformance/run.mjs` +
the C++ `conformance_ts_export` tool) — probe coverage graduates into the
permanent suites per §5.8.

Two asks, one doc, because they share a spine:

1. **Reorganize `compiler_v2/api/`** into a `compiler` half (CEL source →
   `Program`) and an `eval` half (`Program` → result, where host
   trampolines live).
2. **Build a parallel eval host in TypeScript** that runs the compiled
   `.wasm` in Node and the browser. (Decision 2026-05-24: *TS now, Go
   later*; *full proto parity*. The Go follow-up rides on the same
   language-neutral ABI spec — §6.)

The connective tissue: the thing the split makes explicit — the
**`Program` artifact (wasm bytes + `cel.abi` section)** — is *exactly*
the contract a non-C++ host has to consume. Pinning that boundary in C++
first (§2) lets us write the ABI down once (§4) and implement it twice
(C++ today, TS next, Go later) without it drifting.

---

## 0. TL;DR / verdict

- The split is **low-risk and mostly mechanical**: the wasmtime
  dependency already cleaves the package cleanly. `Compiler` / `Program`
  / `CelType` have no wasmtime dep; `Engine` / `Instance` and everything
  under `internal/` that touches wasmtime are tagged `manual` in
  `BUILD.bazel` today. The split formalizes a line that already exists.
- The TS host is **feasible but front-loaded by two things**, neither of
  which is the wasm plumbing: (a) the runtime `.wasm` needs *shared
  memory + tail-calls + WASI preview1*, which constrains the browser
  story (COOP/COEP + a WASI shim); (b) **full proto parity means
  reimplementing the `cel_host.*` reflection trampolines in JS** against
  a `protobuf-es` descriptor pool, byte-exact against cel-cpp's field-read
  semantics. The 24-byte `CelValue` codec and the `cel.abi` decode are
  the easy parts.
- Testing both: the refactor is verified by **byte-identical Program
  output + the existing per-component suites staying green + a new
  dependency-layering test**. The TS host is verified by **running the
  same conformance corpus through it and diffing the pass-set against the
  C++ host** (currently 1774 passing — `scripts/check_conformance_monotonic.sh`).

---

## 0.5 Milestone plan — phases, probe gates, slices

The milestone is **probe-gated**: the TS host rests on assumptions about
JS wasm engines + protobuf-es that memory gets subtly wrong, so Phase 0
buys down that risk with throwaway probes *before* a line of host code is
committed. The C++ `api/` split (Slice A) is independent and can run in
parallel from day one.

```
Slice A  (C++ split)         ──────────────► independent, no probe gate
Phase 0  (probes P-1…P-8)    ───┐
                                ├─ gate G1 (plumbing) ─► Slice B  (TS P0: scalars+arena)
                                ├─ gate G2 (proto)     ─► Slice C  (TS P1/P2: proto parity)
                                └─ gate G3 (browser)   ─► Slice D  (browser packaging)
```

### Phase 0 — probe tasks (do these first; each is throwaway)

Probes live under `ts/eval/probes/<id>/`, are deleted at milestone
closeout (they are NOT the permanent suite — §5.6 is), and **each writes
its verdict back into this doc** as a dated callout citing the `.ts` file
+ the C++ file:line it was checked against (the `m17-encoders-ext.md` §2
pattern). Detailed shapes are in §5b; the checklist + acceptance bar:

- [x] **P-1 — engine proposal support (Node).** *(verdict 2026-05-25:
  **PASS** on Node **v25.6.1**. `WebAssembly.compile()` on the 2.68 MB
  `cel_runtime_wasm.wasm` succeeds — tail-calls + shared-memory proposals
  accepted at decode/validate, 30 imports / 227 exports, exported `memory`
  present. Instantiation-with-imports is P-4. **TODO before ship:** bisect
  the minimum Node version for the package `engines` field — v25 is
  far newer than needed; tail-calls landed in V8 ~Node 22, confirm.)
  *(detail: §5b TP1)*
- [ ] **P-2 — engine proposal support (browser) + COOP/COEP failure
  mode.** Instantiates under cross-origin isolation; the *exact* failure
  without the headers is characterized. **Accept:** minimum Chrome
  version pinned + the no-isolation failure documented. *(§5b TP2)*
- [x] **P-3 — minimal WASI import set.** *(verdict 2026-05-25 — §5b TP3
  callout. The runtime's full import surface is now a concrete list; the
  "which WASI are actually called" half folds into P-4.)*
- [x] **P-4 — end-to-end scalar eval.** *(verdict 2026-05-25: **PASS**.
  `ts/eval/probes/m19/p4_scalar_eval.mjs` reproduces the
  §4.4 wiring in pure Node — zero npm deps — and `eval()` on the compiled
  `"1 + 2"` Program returns offset 64 holding `{CEL_INT, i:3}`. The whole
  TS plumbing layer is de-risked. Detail + the ctor/WASI finding: §5b TP4
  callout.)* — **keystone probe, green.**
- [x] **P-5 — activation marshal round-trip.** *(verdict 2026-05-25:
  **PASS**. `p5_activation_marshal.mjs` — `x+1` with `x=41` → `42` (inline
  int slot) and `s+"!"` with `s="hi"` → `"hi!"` (string via `malloc`'d
  activation buffer). Includes a working `cel.abi` section walker +
  protobuf decoder — the first real piece of `abi.ts`. Finding: the
  `cel.abi` `repr` field is the `ir::Repr` ordinal, NOT `CelKind` — see
  §4.2. Detail: §5b TP5 callout.)*
- [x] **P-6 — protobuf bound in activation via protobuf-es reflection.**
  *(verdict 2026-05-25: **PASS**. `p6_proto_bind.mjs` builds a real
  protobuf-es `Customer` from a `--include_imports` `FileDescriptorSet`,
  binds it to `u:proto(...)`, and `cel_get_field` reads `u.name` via
  `reflect(desc,msg).get(field)` keyed on `cel.abi.fields[id].field_number`
  → "Ann". Reuses P-9's trampoline harness unchanged — only the backing's
  field accessor differs (descriptor reflection vs `obj[name]`), proving
  the harness is backing-agnostic. `@bufbuild/protobuf` v2 + the bazel
  descriptor set. Full byte-diff vs the C++ `out_slot` is the Slice-C
  follow-up; the value + the shared C++ runtime wasm make it strong
  evidence. Detail: §5b TP6.)*
- [x] **P-9 — host-backed aggregates bound in activation (zero-dep).**
  *(verdict 2026-05-25: **PASS**. `p9_host_aggregates.mjs` binds a JS
  **object** (JSON/struct backing → `u.name`="Ann"), **array** (list →
  `xs[1]`=20, `xs.size()`=3), and **Map** (map → `m["k"]`=7) in
  activation and reads them back through eval. Validates the externref
  table (3 namespaces) + `cel_get_field`/`cel_list_at`/`cel_list_size`/
  `cel_map_lookup` trampolines + `cel.abi.fields` decode + arena string
  results — the whole Slice-C plumbing minus protobuf-es reflection,
  which is P-6. Detail: §5b TP9 callout.)*
- [x] **P-7 — bigint ⟷ i64/u64.** *(verdict 2026-05-25: **PASS**.
  `p7_bigint_codec.mjs` — `INT64_MIN/MAX`, `UINT64_MAX`, `2^63` exact via
  `DataView.getBigInt64`/`setBigUint64`; confirmed `number` WOULD corrupt
  them. `int`/`uint` codec arms must use bigint. → `celvalue.test.ts`.)*
- [x] **P-8 — conformance fixture bridge round-trip.** *(verdict
  2026-05-25: **PASS, far past one row.** Built the real bridge:
  `//compiler_v2/conformance:conformance_ts_export` (C++; new
  `runner::CompileForExport` + `conformance_ts_export.cc`) emits per-row
  `<n>.wasm` + `index.jsonl` (the `SimpleTest` as proto3 JSON). `ts/
  conformance/run.mjs` evals each through the TS host wiring + a scalar
  matcher comparator and **zero-diffs vs `cpp_outcome`**. On
  `comparisons.textproto`: **285/285 comparable rows agree**, 69 ts=skip
  (string/dyn/aggregate — v1 subset), 4 `disable_check` rows correctly
  excluded (cpp=skip → no C++ ground-truth verdict). Detail: §5b TP8.)*
  — **the conformance harness (Slice B/C DoD) is now operational.**

**Probe gates** (a slice does not start until its gate is green):

| Gate | Green when | Unblocks |
|---|---|---|
| **G1 — plumbing** | P-1, P-3, P-4, P-5, P-7 pass | Slice B (host) |
| **G-conf — bridge** | P-8 passes | the conformance harness (§5.6) — Slice B/C DoD |
| **G2 — proto** | P-6 passes (or its divergence cost is accepted) | Slice C |
| **G3 — browser** | P-2 passes (or browser target is explicitly deferred) | Slice D |

> **G1 + G2 + G-conf GREEN as of 2026-05-25.** **G1** (plumbing): P-1/P-3/
> P-4/P-5/P-7 PASS — instantiation + memory sharing + CelValue codec +
> `cel.abi` decode + activation marshal (incl. string buffer) + bigint
> extremes. **G2** (proto): P-6 PASS — real protobuf-es message bound +
> read via reflection; P-9 PASS additionally de-risked the zero-dep
> host-aggregate path (object/JSON + list + map bindings). **G-conf**
> (bridge): P-8 PASS — the C++ exporter + `ts/conformance` runner
> zero-diff vs C++ (285/285 comparable rows). **Slices B and C are both
> unblocked. Remaining Phase 0: P-2 (browser) only.**

### Slices (implementation — gated as above)

- [ ] **Slice A — C++ `api/` split** (§2). `common/` + `compiler/` +
  `eval/`; path/`BUILD` rewrite only, no symbol renames. **DoD:**
  byte-identical Program golden (§3.1) + all suites incl. `manual` green
  (§3.2) + conformance 1774==1774 + the layering test (§3.3) enforced.
  *No probe gate — start anytime.*
- [ ] **Slice B — TS host P0** (§5.5 P0, gate G1). Codec, ABI decode,
  WASI shim, engine/instance, the user-guide-shaped public API (§5.7),
  all 19 `cel_host.*` as trap-stubs. **DoD:** every proto-free corpus row
  passes the cross-host diff (§5.6).
- [ ] **Slice C — host-backed aggregates, in + out** (gate G2). The full
  `HostBacking` model behind the `cel_host.*` trampolines. **Backing
  variants (all must work as activation INPUT and eval OUTPUT):**
  - **message** — (a) a real `protobuf-es` `Message` read via descriptor
    reflection (P-6), and (b) a **plain JS object that mirrors a proto**
    (read by field name — the JSON/struct backing, P-9).
  - **list** — (a) a plain JS array, and (b) a proto **repeated** field
    (reflection-backed `ProtoList`).
  - **map** — (a) a plain JS `Map`/object, and (b) a proto **map** field
    (reflection-backed `ProtoMap`).
  - Trampolines: `cel_get_field`/`has` + `cel_list_at`/`size`/`in`/`eq` +
    `cel_map_lookup`/`size`/`in`/`eq` + `make_message`/`set_field` + WKT.
  - **Result decode**: a returned message/list/map (host slot) or arena
    list/map (literal) materializes back to a TS value (copy-out).
  - **Unknowns**: `instance.partialEval(activation, patterns)` →
    `CEL_UNKNOWN` short-circuit → `Value.unknown(attributeId)`.
  **DoD:** the e2e matrix below all green + TS pass-set == C++ pass-set on
  the corpus (the conformance_ts gate at the full set).

  **Slice C e2e matrix (real wasm; each a case):**

  | binding/result | message | list | map |
  |---|---|---|---|
  | input: real protobuf-es | field read / `has` / `==` | repeated → `[i]`/`size`/`in` | map field → `[k]`/`size`/`in` |
  | input: plain JS (mirrors proto) | field read by name | array → `[i]`/`size`/`in` | `Map` → `[k]`/`size`/`in` |
  | output: returned from eval | message → copy-out | list → copy-out | map → copy-out |
  | nested (list-of-msg, map-of-msg, msg-with-repeated) | ✓ | ✓ | ✓ |
  | unknown (partial-eval over the above) | field-read short-circuits → `Value.unknown` | | |

  > **Authoritative requirement (user, this milestone):** *every* CEL data
  > type passes **both in and out** of the evaluator — via activation
  > binding, eval return value, AND host-function argument/return:
  > scalars (null/bool/int/uint/double/string/bytes), `duration`/
  > `timestamp`, message, list, map — each as a **real protobuf-es** value
  > AND as a **plain-JS equivalent** (object-mirrors-proto / array / `Map`),
  > including **proto-backed** repeated + map fields, **nested**
  > (list-of-msg, map-of-msg, msg-with-repeated), and **unknowns**. The
  > e2e suite must cover the full matrix — one compiled Program fixture
  > per shape (field read, `has`, index, key, `size`, `in`, `==`,
  > construct/return).
- [ ] **Slice E — host callbacks (`cel_fn`) + slot sugar** (gate G2, after
  Slice C). `Engine.addFunction(overloadId, impl)` registers a JS
  callback, wired into the `cel_fn` namespace at `plan`, invoked during
  eval with arg slots + an out slot (the C++ `HostCallbackTrampoline`
  shape). **The slot-read/write sugar is a requirement, not an option:** a
  typed `FunctionImpl` adapter (mirroring user-guide §6.2) so an impl
  reads each arg slot as a decoded `Value`/backing (`asInt`/`asString`/a
  list/map/message backing) and returns a `Value` that the adapter encodes
  into the return slot (scalars inline, strings/aggregates via the arena/
  externref) — the same data-type matrix as activation, now across the
  custom-fn boundary, in **both** directions. The raw
  `(memory, outSlot, argSlots)` callback is the floor; the typed adapter
  is the surface. **DoD:** custom-fn corpus rows zero-diff vs C++ + the
  in/out matrix green for a host fn taking & returning each data type.
- [ ] **Slice D — browser packaging** (gate G3). npm package, runtime
  `.wasm` asset, COOP/COEP docs, `instantiateStreaming` path. **DoD:**
  the §2 quick-start runs in a cross-origin-isolated page.

> Out of scope for M19 (later milestones, not drafted here per CLAUDE.md):
> foreign modules in the TS host (§5.7) and the Go host (§6).

### Closeout gate (copy into the M19 PR description)

- [ ] Slice A: Program golden byte-identical; `//compiler_v2/api/...` +
  the three `manual` e2e targets green; conformance 1774==1774; layering
  test enforced in CI.
- [ ] Phase 0: P-1…P-8 each have a dated verdict callout in this doc;
  gates G1/G-conf/G2/G3 recorded green-or-deferred.
- [ ] Testing (§5.8): every TS module has its positive/negative/boundary
  vitest spec; **every probe has graduated into its permanent test** (the
  graduation table) — the throwaway probe dir is then deleted;
  `per-component-test-coverage.md` has a TS section; `testing-checklist.md`
  rows ticked.
- [ ] Conformance bridge (§5.6): C++ `conformance_ts_export` +
  `runner::CompileForExport` landed; TS conformance runner + ported
  `binding_marshal` + matcher comparator (with the drift-guard table)
  landed; **zero-diff vs C++** on the exported set — proto-free subset for
  Slice B, full set for Slice C.
- [ ] Sync forcing-function (§5.6 "Staying in sync"):
  `scripts/check_conformance_ts.sh` regenerates fixtures from current C++
  each run (fixtures gitignored, never committed) and is wired into CI +
  the pre-push hook next to `check_conformance_monotonic.sh`, so a C++
  conformance change can't land without the TS gate engaging.
- [ ] Docs: this doc's status line flipped to shipped (per CLAUDE.md
  "Closing out a planning doc"); `user-guide.md` TS section added (§8
  future work); `cel-host-surface.md` / `design.md` reconciled to the
  split.

---

## 1. Current `api/` anatomy (the line already exists)

Evidence from `compiler_v2/api/BUILD.bazel` and the headers. Two cohorts:

**Compile-time cohort — no wasmtime, no `manual` tag:**

| target | file | role |
|---|---|---|
| `type` | `type.h/.cc` | `cel::CelType` — static type for declarations (`type.h:23`) |
| `error` | `error.h/.cc` | `ErrorCode` + `ErrorPayload` — payload of `Value::Error` |
| `attribute` | `attribute.h/.cc` | `AttributeId` / `AttributePattern` (partial-eval) |
| `value` | `value.h/.cc` | `cel::Value` — user-facing 24-byte counterpart (`value.h:56`) |
| `program` | `program.h` | `cel::Program` — wasm bytes; the serialization boundary (`program.h:32`) |
| `compiler` | `compiler.h/.cc` | `cel::Compiler` — `Compile(source) → Program` (`compiler.h:110`) |
| `abi_decode` | `internal/abi_decode.*` | decode the `cel.abi` custom section from raw bytes (pure) |

**Runtime cohort — pulls `@wasmtime_darwin_arm64`, tagged `manual`:**

| target | file | role |
|---|---|---|
| `engine` | `engine.h/.cc` | `cel::Engine` — owns `wasm_engine_t` + parsed `cel_runtime.wasm`; `Plan(program) → Instance` |
| `instance` | `instance.h/.cc` | `cel::Instance` — `Eval` / `PartialEval`; CelValue decode + activation marshal |
| `instance_impl` | `internal/instance_impl.*` | pImpl: per-Plan wasmtime handles |
| `wasmtime_engine_state` | `internal/wasmtime_engine_state.*` | engine-shared wasmtime state + custom-fn registry |
| `cel_host_wasmtime` | `internal/cel_host_wasmtime.*` | Layer 3: wasmtime glue for the `cel_host.*` trampolines |
| `host_callback` | `host_callback.h` | raw `cel::HostCallback` alias |

**The awkward middle — `cel_host` (Layers 1+2), runtime-agnostic but
eval-only:**

`internal/cel_host.{h,cc}` (the .cc is ~145 KB) holds Layer 1
(`HostMessageBacking` / `ProtoBacking` / `HostMap` / `ProtoList` … — pure
CEL field-read semantics) and Layer 2 (the `*Impl` trampolines driven by
the abstract `MemoryView` / `ExternrefTable` / `ArenaAllocator`,
`cel_host.h:285`+). It has **no wasmtime dep** (only Layer 3 does), but it
is unambiguously *eval-side* — it implements what `cel_host.*` imports do
at runtime. In the split it goes to `eval/`, not `compiler/`.

**Namespacing today** (per `cel-host-surface.md` §1): public surface is
`namespace cel`, internal machinery is `namespace celwasm` /
`celwasm::api`, with `using` re-export shims at the bottom of each public
header to dodge ODR collisions with cel-cpp's own `cel::Value` /
`cel::Compiler` / `cel::Activation`. The split preserves this — it moves
files between directories, it does **not** rename symbols.

---

## 2. Part A — the `compiler/` ⁄ `eval/` split

### 2.1 Target layout

```
compiler_v2/api/
  common/                 # leaf types shared by both halves
    type.{h,cc}           # CelType  (compile input + eval expected_type)
    error.{h,cc}          # ErrorCode / ErrorPayload (eval output, wire mirror)
    attribute.{h,cc}      # AttributeId / AttributePattern (partial-eval)
    value.{h,cc}          # cel::Value (eval I/O; Program-adjacent)
    BUILD.bazel
  compiler/               # CEL source -> Program.  NO wasmtime.
    compiler.{h,cc}
    program.{h}           # the artifact + boundary contract
    BUILD.bazel
  eval/                   # Program -> result.  Host trampolines live here.
    engine.{h,cc}
    instance.{h,cc}
    activation.{h,cc}     # per-Eval bindings  (eval input)
    host_callback.h
    abi_decode.{h,cc}     # was internal/ ; pure, but eval-only consumer
    internal/
      cel_host.{h,cc}             # Layers 1+2 (runtime-agnostic)
      cel_host_error.{h,cc}
      cel_host_wasmtime.{h,cc}    # Layer 3 (wasmtime)
      instance_impl.{h,cc}
      wasmtime_engine_state.{h,cc}
      cel_host_test_fakes.h
    BUILD.bazel
```

Rationale for **three** dirs, not two: `Value` / `CelType` / `ErrorPayload`
/ `AttributePattern` are genuinely shared. `CelType` is a *compile* input
(`Builder::DeclareVariable`, `compiler.h:209`) **and** an *eval* input
(`HostMessageBacking::ReadField(..., const CelType& expected_type)`,
`cel_host.h:56`). `Value` is mostly eval, but `Program` lives next to the
compiler and `Value` is the thing eval hands back. Forcing these into one
half or the other creates a fake dependency edge (compiler→eval or
eval→compiler) that the split is trying to *remove*. A `common/` leaf
layer keeps the two halves siblings, both depending down on `common`,
neither on the other.

### 2.2 The dependency invariant the split buys us

```
        common  (value, type, error, attribute)
        /     \
  compiler     eval
   (no wasmtime)  (wasmtime, manual)
        \         /
         Program (bytes + cel.abi)   ← the only thing crossing the gap
```

- `compiler` → `common`, `//compiler_v2:compile`, `celfn`. **Never** →
  `eval`.
- `eval` → `common`, `Program`, wasmtime, runtime catalogue, protobuf.
  **Never** → `compiler`.
- `common` → absl/protobuf leaves only.

This is enforceable (§3.3) and is the property that makes the TS host
tractable: a TS *eval* host has to re-implement only the `eval` box's
behavior, and it talks to the C++ *compiler* through `Program` bytes —
never through C++ types.

### 2.3 What does NOT change

- **No symbol renames.** `cel::Compiler`, `cel::Value`, `celwasm::api::*`,
  the `using`-shims at header bottoms — all preserved. Only `#include`
  paths and `BUILD.bazel` `//` labels change.
- **No behavior change.** This is a move-files refactor. The
  acceptance bar is *byte-identical `Program` output and an unchanged
  conformance pass-set* (§3).
- **`Program` stays a dumb byte bag** (`program.h:32`). The split is the
  moment to *resist* enriching it; the boundary is bytes + the `cel.abi`
  section embedded in those bytes, nothing else. (The "future parsed
  `Abi`" note in `program.h:16` stays future.)

### 2.4 Migration mechanics (one PR per step, each green on its own)

1. **Create `common/`**, move `type` / `error` / `attribute` / `value`
   targets there. Update `#include` paths repo-wide
   (`compiler_v2/api/value.h` → `compiler_v2/api/common/value.h`). Keep
   `BUILD` target names identical so dependents change only the package
   path. Mechanical; `grep -rl 'api/value.h'` is the worklist.
2. **Create `compiler/`**, move `compiler` + `program`.
3. **Create `eval/`**, move `engine` / `instance` / `activation` /
   `host_callback` / `abi_decode` + the whole `internal/` subtree.
4. **Add the layering test** (§3.3) and flip CI to enforce it.
5. **Update docs**: `cel-host-surface.md`, `design.md`, `abi-refactor.md`,
   and this doc's status line (per CLAUDE.md "Closing out a planning doc").

Each step is a pure path rewrite; `bazel test //compiler_v2/...` is the
per-step gate. No step changes a `.cc` body except `#include` lines.

> Open question O1: do we keep the back-compat `namespace cel` shims in
> `common/` headers, or take the split as the moment to drop them? Recommend
> **keep** — dropping them is an orthogonal churn that touches every call
> site and muddies the "no behavior change" acceptance bar. Drop in a
> separate follow-up if ever.

---

## 3. Part B — testing the refactor

The refactor's whole risk is *silent behavior change from a botched move*
(a dep that got dropped, a file that got half-moved). Three gates:

### 3.1 Byte-identical Program golden

Before step 1, capture golden `Program::wasm_bytes()` for a spread of
sources (scalar, string-concat, proto field-read, comprehension, custom-fn,
proto-literal). After each step, recompile and assert byte-equality. A
move refactor that changes a single output byte is a bug. Add this as
`compiler_v2/api/compiler/program_golden_test.cc` (new; lives with the
compiler since it exercises `Compile`).

### 3.2 Existing suites stay green — including the `manual` ones

`bazel test //compiler_v2/...` covers the default set, but per CLAUDE.md
the **load-bearing e2e assertions live in `manual`-tagged targets**
(`instance_test`, `engine_test`, `cel_host_test`, the `*_impl_test`s, the
`engine_host_types_tdd_test` RED spec). The closeout gate must run them
explicitly:

```
bazel test //compiler_v2/api/...                              # default
bazel test //compiler_v2/api/eval:instance_test \
           //compiler_v2/api/eval:engine_test \
           //compiler_v2/api/eval:cel_host_test               # manual e2e
scripts/check_conformance_monotonic.sh                        # 1774 == 1774
```

The conformance count is the strongest single signal: it exercises the
entire compile→plan→eval path end to end. If it drops, a dep got lost in
the move.

### 3.3 A dependency-layering enforcement test (new, the real prize)

The split is only worth it if it *stays* split. Add a test that asserts
the dep graph, so a future `eval`→`compiler` edge fails CI rather than
rotting silently:

```python
# compiler_v2/api/layering_test  (sh_test over `bazel query`)
# FAIL if compiler depends on eval, or eval depends on compiler.
bazel query 'somepath(//compiler_v2/api/compiler/..., //compiler_v2/api/eval/...)'  # must be empty
bazel query 'somepath(//compiler_v2/api/eval/..., //compiler_v2/api/compiler/...)'  # must be empty
bazel query 'somepath(//compiler_v2/api/common/..., //compiler_v2/api/eval/...)'    # must be empty
```

This is the test that has teeth six months from now. Without it the
directories are cosmetic.

### 3.4 What the refactor explicitly does NOT need new tests for

Per CLAUDE.md "don't create work for later milestones" — the per-component
trampoline tests (`cel_map_lookup_impl_test`, `proto_list_test`, …) already
cover behavior; the move doesn't change behavior, so it doesn't earn new
behavioral tests. Only the *structural* tests (§3.1, §3.3) are new.

---

## 4. Part C — the eval ABI as a language-neutral contract

This section is the **spec a non-C++ host implements**. It is deliberately
written without reference to TS or Go so the Go follow-up (§6) reuses it
verbatim. Everything here is extracted from the C++ source, cited.

### 4.1 The `CelValue` wire format (`runtime/cel_data.h`)

24 bytes, little-endian (the header `#error`s on a BE host, `cel_data.h:171`):

```
offset  size  field
  0      4    kind   (u32, CelKind enum)
  4      4    _pad
  8      8|.. payload (union, 16 bytes)
```

`CelKind` (stable, append-only, `cel_data.h:31`):
`NULL=0 BOOL=1 INT=2 UINT=3 DOUBLE=4 STRING=5 BYTES=6 LIST_ARENA=7
MAP_ARENA=8 MAP_HOST=9 MESSAGE=10 TYPE=11 DURATION=12 TIMESTAMP=13
OPTIONAL=14 UNKNOWN=15 ERROR=16 LIST_HOST=17`.

Payload arms a host must encode/decode:
- scalar `b`(i32) / `i`(i64) / `u`(u64) / `d`(f64) — inline at +8.
- `CelSpan {ptr:u32, len:u32}` for STRING / BYTES / TYPE — bytes live in
  linear memory at `ptr`.
- `ArenaListRef {header_ptr:u32}` / `ArenaMapRef {header_ptr:u32}` —
  header is a 16-byte `{count, capacity, entries_offset, _pad}`
  (`cel_data.h:69`,`:88`); entries run is `count` × 24 B (list) or
  `count` × 48 B (map: key CelValue then value CelValue).
- `ref_slot:u32` for MAP_HOST / LIST_HOST, `msg_slot:u32` for MESSAGE —
  **externref-table indices**, not memory offsets (§4.6).
- `CelDurTs {seconds:i64, nanos:i32, _pad}` for DURATION / TIMESTAMP,
  sign-correlated (`cel_data.h:102`, decode in `instance.cc:321`).
- `err:u32` for ERROR — a `CEL_ERR_*` code (`cel_data.h:181`), mirrors
  host `ErrorCode` 1:1 (`instance.cc:228` `DecodeCelError`).
- `unk:u32` for UNKNOWN — the partial-eval attribute id (`instance.cc:307`).

A host's CelValue codec is ~one struct read/write per arm. This is the
*easy* part of the port — `instance.cc:260` `DecodeCelValueAt` is the
reference (~80 lines).

### 4.2 The `cel.abi` custom section (`abi/cel_abi.proto`)

A `CelAbi` proto embedded as a wasm custom section named `cel.abi`. Fields
the host consumes:
- `version` + `runtime_abi_version` — the host must reject a mismatch
  (`engine.cc:610` `CheckRuntimeAbiVersion`; current `kRuntimeAbiVersion = 2`,
  `runtime_catalogue.h`).
- `variables[]` — `{name, local_index, slot_offset, repr}`. The host
  marshals each bound activation value into the 24-byte cell at
  `slot_offset` *before* calling `$eval` (§4.5). `repr` picks the encoder.
  **`repr` is the `ir::Repr` ordinal (`annotations.h:17`), which is NOT
  the same numbering as `CelKind`** (P-5 finding): `Repr` leads with
  `kUnknown = 0`, so `Null=1, Bool=2, Int=3, Uint=4, Double=5, String=6,
  Bytes=7, List=8, Map=9, Message=10, Enum=11, Duration=12, Timestamp=13,
  Type=14, Optional=15`. An `int` var carries `repr=3` (a `CEL_INT` wire
  value is `2`); a `string` var `repr=6` (`CEL_STRING` is `5`). `abi.ts`'s
  repr→encoder dispatch table must key on these ordinals, not `CelKind`.
- `fields[]` — `{id, field_number, name, owner_fqn}`. The host resolves
  `owner_fqn` against its descriptor pool to get a `FieldDescriptor` for
  the `cel_get_field` trampoline.
- `attributes[]` — `{id, variable, qualifiers[]}` for partial-eval pattern
  matching.
- `types[]` — `{id, fully_qualified_name}` for `cel_make_message`.

Decode reference: `internal/abi_decode.cc` parses the wasm section framing
by hand (no Binaryen dep) and hands back the proto. A non-C++ host does the
same: walk wasm sections, find the custom section `cel.abi`, parse the
`CelAbi` protobuf. (`NotFound` → empty ABI → a variable-free `Eval()` still
works, `engine.cc:614`.)

### 4.3 The import surface the expr module declares

Five namespaces (`runtime_catalogue.h` is the single source of truth):

- **`cel`** — runtime helpers. The host does **not** implement these; it
  binds them from the *runtime module's exports* (§4.4). Plus `cel.memory`
  (shared), `cel.arena_reset`, `cel.arena_alloc` (`compile.cc:327`,`:334`).
- **`cel_host`** — 19 host trampolines the host **must** implement
  (`cel_host_wasmtime.cc:524`): `cel_get_field`, `cel_has_field`,
  `cel_map_lookup`, `cel_map_iter_open`, `cel_list_iter_open`, `cel_list_at`,
  `cel_list_size`, `cel_list_in`, `cel_list_eq`, `cel_list_concat`,
  `cel_map_size`, `cel_map_in`, `cel_map_eq`, `cel_message_eq`,
  `cel_make_message`, `cel_set_field`, `cel_timestamp_tz_accessor`,
  `cel_wkt_unwrap_time`, `cel_wkt_unwrap_wrapper`. **All take i32 slots,
  return void**, write their result CelValue into an `out_slot` in linear
  memory. These are the proto-reflection trampolines — the hard part.
- **`cel_env`** — `cel_log` (9-arg diagnostic, `cel_log.h`). A no-op-safe
  stub suffices for a v1 host; logging "must never trap" (`cel_log.h:24`).
- **`cel_fn`** — user custom-fn impls; only present if the program uses
  custom functions. Out of scope for v1 host parity.
- **`wasi_snapshot_preview1`** — ~10 imports wasi-libc keeps alive (env,
  fd_*, proc_exit, sched_yield, random_get). The C++ host satisfies these
  via wasmtime's built-in WASI (`engine.cc:60` `RegisterWasiStubs`). A
  non-C++ host supplies a shim.

### 4.4 The instantiation sequence (`engine.cc::Plan`, the heart)

A host reproduces this order (`engine.cc:576`):

1. New store. Configure WASI (sandbox env/stdio). Engine config **must**
   enable: **tail-calls** (`wasmtime_config_wasm_tail_call_set`,
   `engine.cc:78` — the aggregate dispatchers use `return_call`),
   **threads + shared memory** (`engine.cc:86`).
2. New linker. Register `cel_env.cel_log`, the 19 `cel_host.*`
   trampolines, and the WASI imports (`engine.cc:143` `InitLinker`).
   **`cel.memory` is NOT bound yet.**
3. Instantiate **`cel_runtime.wasm`** against the linker. It *owns and
   exports* the shared memory.
4. Pull the runtime's exported `memory`, bind it on the linker as
   `cel.memory` (`engine.cc:164` `BindRuntimeMemory`). Both modules now
   share one backing store.
5. Bind every `cel`-namespace runtime export onto the linker
   (`engine.cc:226` `BindAllRuntimeExports`, driven by the catalogue) +
   capture `arena_alloc` / `malloc` handles + `arena_init` the bump arena
   (`engine.cc:286` `SeedRuntimeArena`, capacity `CELWASM_ARENA_CAPACITY_BYTES`).
6. (Custom modules + `cel_fn` callbacks — skip for v1.)
7. Instantiate the **expr module** (the `Program` bytes) against the now-
   complete linker; grab its `eval` export (`engine.cc:532`).
8. Decode `cel.abi`, version-check, build the host bindings
   (field-ref / attribute / type tables).

The two-module, runtime-owns-memory shape is load-bearing — a host can't
just hand the expr module a fresh memory; it must instantiate the runtime
first and share *its* memory (`wasi/DESIGN.md` §3-4).

### 4.5 The eval calling convention

`$eval` takes **zero args, returns one i32** = the byte offset of the
result CelValue in shared memory (`instance.cc:1006`). Its first
instruction is a baked-in `arena_reset`, so back-to-back evals on one
instance are safe. With an activation:

1. `ExternrefTable.Reset()` (§4.6).
2. **Marshal** each declared variable: look it up in the activation, encode
   to a 24-byte CelValue, write at its `slot_offset` (`instance.cc:928`
   `MarshalActivation`). String/bytes/type payloads can't live in the bump
   arena (the `arena_reset` prologue would wipe them) — they go in a
   separately `malloc`'d **activation buffer** above the reserved low
   region (`instance.cc:377` `EnsureActivationBuffer`, `wasi/DESIGN.md` §6).
3. Call `$eval`, read the result CelValue at the returned offset, decode.

A host reimplements `MarshalActivation` + `DecodeCelValueAt`. The encoder
has per-`Repr` arms (`instance.cc:837` `EncodeBoundValue`) including the
WKT-coercion quirks (a `Value::Null` binds to a scalar slot as `CEL_NULL`,
`instance.cc:597`; a bound `Int32Value` proto peels to a scalar,
`instance.cc:561`) — these are spec-mandated and must be ported faithfully.

### 4.6 The externref table (`cel_host.h:310`)

MESSAGE / MAP_HOST / LIST_HOST CelValues don't carry data inline — their
payload is a `slot` index into a host-side table that maps slot →
host-language object (a proto message, a map backing, a list backing).
`Intern` is monotonic within an eval; `Reset()` clears between evals.
Three independent namespaces (message / map / list — a slot from one won't
resolve in another, `cel_host_wasmtime.cc` `HostExternrefTable`). A non-C++
host needs the same table: when a trampoline produces a host aggregate it
interns the host object and writes the slot; when `$eval` returns a host
aggregate the host decodes by looking the slot up (`instance.cc:159`
`DecodeHostListAt`, `:186` `DecodeHostMessageAt`).

---

## 5. Part D — the TypeScript eval host

### 5.1 Why TS serves the stated goal (and the runtime constraints)

Only JS/TS can host wasm **in a browser** — Go cannot. The stated goal is
"nodejs or chrome", so TS is the only choice that hits it.

**Does TS "come with wasm"? Yes — there is nothing to install for wasm
itself.** `WebAssembly` is a built-in global in every JS engine: V8
(Node + Chrome), SpiderMonkey, JavaScriptCore. `WebAssembly.compile` /
`.instantiate` / `.Memory` / `.Module` are always present — no package,
no polyfill. TypeScript ships the type definitions for the `WebAssembly`
namespace in its standard libs (`lib.dom.d.ts` / `@types/node`). So the
*wasm plumbing* has zero runtime dependencies. The only npm deps the host
needs are **`@bufbuild/protobuf`** (for the descriptor pool / reflection,
§5.4) and a small **WASI shim** — neither is for wasm execution per se.
What is *not* guaranteed to "come with" the engine are the wasm
*proposals* the runtime `.wasm` relies on (tail-calls, shared memory,
threads) — those depend on engine version + page isolation, which is what
the next bullets and the probes (§5b) pin down.

The constraints, all surmountable:

- **Tail-calls**: shipped in V8 (Chrome/Node ≥ recent) and Firefox. ✅ in
  Node and modern browsers; verify the target Node version.
- **Shared memory** (`SharedArrayBuffer`): in the **browser** requires the
  page to be **cross-origin isolated** (COOP `same-origin` + COEP
  `require-corp` headers). In **Node**, `SharedArrayBuffer` is unconditional.
  This is the single biggest browser deployment caveat — document it loudly.
- **WASI preview1**: Node has `node:wasi` (experimental, and weak on
  threads); cleaner to supply a **minimal hand-rolled shim** for the ~10
  imports the runtime actually keeps alive (most are never called — they're
  wasi-libc surface, `phase-c-plan.md` §7.2). In the browser, the same shim
  (e.g. modeled on `@bjorn3/browser_wasi_shim`) covers it. The shim returns
  deterministic empty-env responses, matching `wasi_config_new()` with no
  inherited env/stdio (`engine.cc:125`).

### 5.2 Package layout (greenfield — no JS in the repo today)

A **top-level `ts/`** tree, sibling packages mirroring the C++ layout —
`ts/eval` ↔ `compiler_v2/api/eval` (the host), `ts/conformance` ↔
`compiler_v2/conformance` (the corpus runner). NOT nested under
`compiler_v2/api/` — TS is a peer deliverable with its own toolchain.

```
ts/
  eval/                     # <library> — the TS host (eval half, reimplemented)
    package.json            # type:module, deps: @bufbuild/protobuf (protobuf-es)
    src/
      celvalue.ts           # 24-byte CelValue codec  (≈ instance.cc DecodeCelValueAt)
      abi.ts                # wasm custom-section walk + CelAbi decode (≈ abi_decode.cc)
      wasi-shim.ts          # minimal preview1 stub
      externref.ts          # 3-namespace intern/lookup table (≈ HostExternrefTable)
      host/                 # the cel_host.* trampolines (the hard part, §5.4)
        get-field.ts  has-field.ts  make-message.ts  set-field.ts
        map-ops.ts    list-ops.ts   wkt.ts  ...
      engine.ts             # instantiate runtime+expr, share memory (≈ engine.cc Plan)
      instance.ts           # marshal activation + call $eval + decode (≈ instance.cc)
      value.ts  type.ts  activation.ts   # public surface mirroring cel::Value etc.
      index.ts
    test/                   # vitest — per-component (§5.8)
    probes/m19/             # throwaway Phase-0 probes (deleted at closeout)
  conformance/              # the conformance_ts runner — consumes the C++
    src/                    #   exporter's fixtures, evals via ts/eval, zero-
    test/                   #   diffs vs cpp_outcome (§5.6).  ↔ compiler_v2/conformance
```

The C++ fixture **exporter** stays in `compiler_v2/conformance/`
(`run_conformance --export_ts_fixtures` — it's C++, part of the C++
conformance package); `ts/conformance` is its TS consumer.

The runtime `.wasm` ships as a build artifact the package embeds (Node:
`fs.readFile`; browser: `fetch` + `instantiateStreaming`). Source it from
`//compiler_v2/runtime:cel_runtime_wasm_bytes`'s underlying `.wasm`.

### 5.3 Component-by-component mapping to C++

| TS module | C++ reference | difficulty |
|---|---|---|
| `celvalue.ts` | `instance.cc:260` `DecodeCelValueAt` + the `Encode*` family | low — `DataView` over the memory buffer |
| `abi.ts` | `internal/abi_decode.cc` | low — protobuf-es parses `CelAbi`; section walk is ~30 lines |
| `wasi-shim.ts` | `engine.cc:60` `RegisterWasiStubs` + wasmtime built-in | low-med — stub the ~10 imports |
| `engine.ts` | `engine.cc:576` `Plan` (steps §4.4) | medium — instantiation order is exacting |
| `instance.ts` | `instance.cc` `Eval` / `MarshalActivation` | medium — encoder quirks (§4.5) |
| `externref.ts` | `HostExternrefTable` | low |
| `host/*` | `cel_host.cc` Layers 1+2 (~145 KB) | **high — see §5.4** |

### 5.4 The hard part: proto-parity `cel_host.*` in JS

Full parity (Decision: *full proto parity*) means the 19 `cel_host.*`
trampolines reimplemented against a **descriptor-driven** proto model:

- **Descriptor pool**: the host needs the message/field descriptors named
  by `cel.abi.fields[].owner_fqn` and `cel.abi.types[]`. In TS that's
  **`protobuf-es` (`@bufbuild/protobuf`)** with a registry built from a
  `FileDescriptorSet` the embedder supplies (the analog of C++'s
  `generated_pool()` / the CLI's `--schema_descriptorset`,
  per `project_m3_schema_cli` memory). protobuf-es exposes runtime
  reflection (`MessageType`, `FieldInfo`) that maps to
  `ProtoBacking::ReadField`'s `FindFieldByNumber` + cpp_type dispatch.
- **`cel_get_field` / `cel_has_field`**: resolve `field_ref_id` →
  `(field_number, name)` via the ABI, find the field on the interned
  message's descriptor, read it, encode the result CelValue. Presence
  semantics (proto2 vs proto3, `cel_host.h:61`) must match langdef.
- **`cel_make_message` / `cel_set_field`**: construct a default message from
  the descriptor, intern it (owning), set fields by `cpp_type` dispatch
  (`cel_host.h:563`,`:604` enumerate the exact mapping incl. ENUM→SetInt).
- **`cel_wkt_unwrap_time` / `_wrapper`**: peel `(seconds,nanos)` / inner
  `value` from WKT protos (`cel_host.h:635`,`:649`).
- **map/list ops**: `ProtoMap` / `ProtoList` over reflection map/repeated
  fields; cross-type numeric key equality (int/uint) per langdef
  (`cel_host.h:163`).

**Risk R1 (the big one):** byte-exact conformance requires the JS field-read
+ equality + error-code semantics to match cel-cpp *exactly* — the same
discipline CLAUDE.md mandates for C++ ("conformance is byte-exact against
cel-cpp"). protobuf-es's reflection is capable but its enum/wrapper/Any
handling differs in shape from C++ protobuf; each trampoline needs a probe
(mirror the repo's `compiler_v2/probes/` discipline) confirming the JS
output matches the C++ `out_slot` bytes for representative inputs **before**
it's trusted. Budget the bulk of TS effort here, not on plumbing.

### 5.5 Phasing (so something runs early even though v1 target is full parity)

The *target* is full parity, but the *build order* still climbs the
dependency stack — proto trampolines can't be first because they need the
instantiation + codec layers under them:

1. **P0 — plumbing + scalars**: `celvalue.ts`, `abi.ts`, `wasi-shim.ts`,
   `engine.ts`, `instance.ts` with scalar/string/bytes/arena-list/arena-map
   marshal+decode and **all 19 `cel_host.*` registered as
   trap-on-call stubs**. Runs every proto-free corpus row. Proves the
   instantiation sequence and codec against the C++ host.
2. **P1 — proto reads**: `cel_get_field` / `cel_has_field` / `ProtoMap` /
   `ProtoList` + descriptor pool. Unlocks the field-read corpus.
3. **P2 — proto construction**: `cel_make_message` / `cel_set_field` /
   WKT unwrap. Full parity.
4. **P3 — partial-eval + custom fns** if needed (attributes table,
   `cel_fn`).

Each phase's gate is the corpus diff (§5.6) — the proto-free subset must be
100% before P1 starts, so a P1 regression is unambiguously a proto-trampoline
bug, not a plumbing bug.

### 5.6 Testing the TS host: cross-host conformance diff

**Running the conformance corpus through the TS host is a first-class M19
deliverable, not a nice-to-have** — it is the only gate that proves
spec-parity, and it is the DoD spine for Slices B and C. The C++ host
already runs the corpus (`compiler_v2/conformance/run_conformance.cc` +
`runner.cc` + `binding_marshal.cc`; monotonic gate
`scripts/check_conformance_monotonic.sh` against `.baseline`, currently
**1774 passing**). The corpus is `tests/simple/testdata/*.textproto`
parsed as `cel.expr.conformance.test.SimpleTestFile` → sections → rows,
each row an `expr` + `bindings` + an expected matcher (`value:` /
`eval_error:` / `unknown:` / `typed_result:`).

#### The problem: the TS host can't compile

`RunOne` (`runner.cc`) does **compile → plan → eval → compare** in one
C++ process. The TS host has no `Compiler` (§5.7). So TS conformance
**splits `RunOne` at the `Program` boundary** — exactly the property the
§2 split formalizes:

```
  C++ (authoritative)                         TS (under test)
  ┌───────────────────────────┐               ┌──────────────────────────┐
  │ for each SimpleTest row:   │   fixtures    │ for each fixture:         │
  │   Compiler.Compile(expr)   │ ──────────►   │   Engine.plan(program)    │
  │   → Program (+ which rows  │  (per-row     │   marshal bindings→Activ. │
  │     compiled vs skipped)   │   .binpb)     │   Instance.eval()         │
  │   record cpp_outcome       │               │   compare vs matcher      │
  └───────────────────────────┘               │   assert == cpp_outcome   │
                                               └──────────────────────────┘
```

#### Component 1 — the C++ fixture exporter

Extend `run_conformance` with an `--export_fixtures=DIR` mode (it already
iterates every row and calls the compile half). For each row it would
otherwise *attempt* (i.e. not a compile-time SKIP like `disable_check` /
static-subset / compile-unimpl — those have no `Program` to ship), emit a
per-row record reusing the **existing `cel.expr` protos** (so protobuf-es
consumes them natively — no bespoke schema):

```
ConformanceFixture {            // new thin proto under conformance/
  string row_id = 1;            // file::section::name
  bytes  program_wasm = 2;      // Compiler.Compile output (bytes + cel.abi)
  repeated cel.expr.conformance.test.Binding bindings = 3;  // verbatim
  cel.expr.conformance.test.SimpleTest expected = 4;        // the matcher fields
  Outcome cpp_outcome = 5;      // pass / fail / skip+category (ground truth)
  cel.expr.Value cpp_decoded = 6;  // the C++ host's decoded result (host-vs-host diff)
}
```

Compilation stays **100% C++ and authoritative** — fixtures are
pre-compiled, so a TS-side mismatch is unambiguously an *eval* bug.

#### Component 2 — the TS conformance runner

`ts/conformance/` — a `vitest` (or plain Node)
runner that, per fixture: reconstructs the `Activation` from `bindings`,
`engine.plan` + `instance.eval`s the `program_wasm`, and compares. It
reuses the same fixtures + the same `cel.expr` matcher types via
protobuf-es.

**Two C++ surfaces must be ported to TS and kept in lockstep** — this is
the real maintenance cost, flag it:
- `binding_marshal.cc` — `cel.expr.Value` binding → `Activation` value.
  (Aggregate/proto bindings need the descriptor pool → P-6 / Slice C.)
- `runner.cc`'s **matcher comparison** — decoded `Value` vs the
  `cel.expr.Value` / `eval_error` / `unknown` matcher, incl. 3VL and the
  cross-type-numeric equality rules. A drift between the TS and C++
  comparators silently inflates or deflates the TS pass-set.

> A `runner_compare_test.cc`-equivalent in TS (vitest) pins the
> comparator against a table of `(decoded, matcher) → expect` cases
> copied from the C++ `runner_test.cc`, so the two can't drift unnoticed.

#### Component 3 — the gate: zero-diff against C++, not a second count

The TS gate is **stronger** than a standalone pass-count baseline: for
every exported (compiled) row, **`ts_outcome` must equal `cpp_outcome`**.
The C++ pass-set is the ceiling and the ground truth simultaneously; any
TS row that passes where C++ failed, or fails where C++ passed, is a
diff and fails CI. This needs no separate `.baseline` to drift — the
existing `.baseline` already governs the C++ count, and TS just has to
match it row-for-row.

Phasing maps onto the slices:
- **Slice B (gate G1):** zero-diff on the **proto-free subset** of
  exported rows (no `cel_host` proto trampoline reached). The proto rows
  are present in the fixtures but expected-to-stub-trap; the runner
  classifies them as `ts_skip` and they're excluded from the B-gate.
- **Slice C (gate G2):** zero-diff on the **full** exported set.

#### Staying in sync with C++ conformance — the forcing function

A standing requirement: **a change to C++ conformance must be reflected in
the TS harness, automatically.** The design guarantees this by construction
rather than by discipline, via two properties:

1. **Fixtures are regenerated from the CURRENT C++ on every gate run — never
   committed.** `scripts/check_conformance_ts.sh` runs
   `conformance_ts_export` (which compiles each row with *this* checkout's
   `cel::Compiler` and records `cpp_outcome` from *this* checkout's
   `RunOne`) into a temp dir, then runs `ts/conformance/run.mjs` against it.
   A committed/stale fixture would diff against an old `cpp_outcome` and
   hide the change — so fixtures are a build artifact, gitignored.
2. **The gate diffs `ts_outcome` against the freshly-recorded
   `cpp_outcome`.** So the two ways C++ conformance can change both
   propagate:
   - **C++ evaluation/comparison changes** (runner.cc comparator,
     `binding_marshal.cc`, a runtime kernel, codegen) → the regenerated
     fixture carries the new `cpp_outcome` → the (unchanged) TS host diffs
     → the gate goes red → the TS side *must* be updated to match. This is
     what keeps the two ported surfaces (the TS matcher comparator + the TS
     `binding_marshal`) honest: they can't silently drift from
     `runner.cc` / `binding_marshal.cc` because the gate compares to C++'s
     live verdict, not to a frozen expectation.
   - **Corpus growth** (a new `tests/simple/testdata/*.textproto`, added to
     `SIMPLE_TESTDATA`) → the exporter iterates the same corpus → the new
     rows are exported → the TS host sees them next run.

Wire `check_conformance_ts.sh` wherever `check_conformance_monotonic.sh`
runs (CI step + pre-push hook), so a commit that moves C++ conformance can't
land without the TS gate having a say.

**The one residual gap, made explicit (not silent):** rows the TS host
`skip`s — a matcher kind or binding shape the current TS slice hasn't
implemented — are excluded from the diff (there's nothing to compare yet).
So a C++ change touching *only* rows TS currently skips won't trip the gate.
That window shrinks monotonically: as the TS host's coverage grows the skip
count falls toward zero and the gate tightens to full parity. The skip
count is printed every run (the confusion matrix), so the gap is visible,
not hidden. (Rows where *C++* skips — out-of-scope, e.g. `disable_check` —
carry no C++ ground-truth verdict and are likewise excluded; see the P-8
verdict.)

#### Per-component unit tests (orthogonal to the corpus)

Mirroring the C++ `*_test.cc`, in vitest: CelValue round-trip per kind
incl. boundaries (`INT64_MIN/MAX`, `UINT64_MAX`, embedded NUL, multi-byte
UTF-8 — P-7); ABI decode; the matcher-comparator table above; per-
trampoline byte-parity probes (§5.4 R1 / P-6).

#### De-risking probe

**P-8 (Phase 0)** proves the bridge end-to-end on *one* row before the
full runner is built: export a single `"1 + 2"`-shaped fixture from the
C++ exporter, load it in TS, eval, compare to the matcher, assert
`ts_outcome == cpp_outcome`. Validates the fixture format + the
matcher-comparator shape + the protobuf-es consumption of `cel.expr`
protos in one shot. (Builds directly on P-4's harness.)

---

### 5.7 Public API surface — the user-guide shape, eval half only

The host mirrors `doc/user-guide.md`'s `Engine` / `Program` / `Instance`
/ `Activation` / `Value` so a TS embedder writes the *same shape* of code
as the C++ embedder (§2 of the user guide). **The one asymmetry: there is
no TS `Compiler`.** Compilation stays in C++ (it needs cel-cpp's parser +
checker + the codegen pipeline); the TS side is the **run-time half** and
receives `Program` *bytes* across the boundary — exactly the
compile-once/run-many, ship-the-`Program` model the user guide §1 and the
§2 split both describe. `Program` in TS is therefore a *loader*, not an
output of a TS compile.

```ts
import { Engine, Program, Activation, Value, CelType } from "@celwasm/eval";

// ── obtain a Program (compiled by the C++ Compiler / `cel compile`, shipped here) ──
const program = await Program.fromBytes(wasmBytes);   // bytes + cel.abi
// browser: Program.fromUrl("/expr.wasm");  node: Program.fromFile("expr.wasm")

// ── run time — mirrors user-guide §2 / §4 ──
const engine   = await Engine.create({ runtimeWasm });   // parse cel_runtime.wasm ONCE
const instance = await engine.plan(program);             // Program → Instance

const act = new Activation()
  .bind("age",  Value.int(20n))                          // i64 → JS bigint
  .bind("name", Value.string("Ann"));
const result = await instance.eval(act);                 // → Value
result.asInt();        // bigint   (throws/Err on kind mismatch, mirroring StatusOr)
result.isUnknown();    // partial-eval
```

Surface map (C++ → TS), keeping names + lifetimes recognizable
(user-guide §4.6 lifetime table carries over):

| C++ (`cel::`) | TS (`@celwasm/eval`) | notes |
|---|---|---|
| `Engine::NewBuilder().Build()` | `await Engine.create({runtimeWasm})` | parses `cel_runtime.wasm` once; reuse per process/tab |
| `Engine::Plan(program)` | `engine.plan(program)` | Program → Instance; instantiate runtime+expr, share memory |
| `Program(bytes)` / `wasm_bytes()` | `Program.fromBytes/Url/File` / `.bytes` | **loader only — no TS compile** |
| `Instance::Eval()` / `Eval(act)` | `instance.eval()` / `eval(act)` | resets arena each call |
| `Instance::PartialEval(act, pats)` | `instance.partialEval(act, pats)` | P3 (§5.5) |
| `Activation::Bind(name, v)` | `new Activation().bind(name, v)` | fluent, overwrites |
| `Value::Int(42)` … `AsInt()` | `Value.int(42n)` … `asInt()` | i64/u64 → **bigint**; `Value.message(...)` takes a protobuf-es message |
| `cel::CelType::Message(fqn)` | `CelType.message(fqn)` | needed for `At(i, elemType)` / `Get(key, valType)` decode hints |
| `StatusOr<T>` | a `Result<T>` / typed throw | pick one and apply uniformly; mirror the user guide's status-code semantics |

`Value.int` uses JS **`bigint`** for `int`/`uint` because CEL `int` is
64-bit and JS `number` loses precision past 2^53 — a correctness
requirement, not a style choice.

### 5.7.1 Canonical TS embedder API (sketch)

`plan` is present (it mirrors C++), but three abstractions the C++ side
gets implicitly are **missing and must be explicit in TS**:

1. **A type registry.** TS has no process-wide
   `DescriptorPool::generated_pool()`. To bind/return/construct proto
   **messages** the host needs the protobuf-es descriptors — so the
   embedder supplies a `TypeRegistry` (a thin wrap of protobuf-es
   `createFileRegistry`). It powers `Value.message(...)`, materializing a
   returned `CEL_MESSAGE` into a typed message, and `cel_make_message`.
   This is the single biggest gap today.
2. **Host-fn registration** (`Engine.addFunction`) + the typed slot sugar.
3. **Typed result accessors** beyond scalars (`asMessage(Schema)`,
   `asList`, `asMap`) so a returned aggregate comes back typed, not raw.

The canonical surface (`@celwasm/eval` — names track `doc/user-guide.md`;
✅ built, 🟡 partial, ⛔ to build):

```ts
// ── Engine: process-shared config + the parsed runtime ──
const engine = await Engine.create(runtimeWasm, {  // ✅ (runtimeWasm, options)
  registry,                       // ✅ TypeRegistry — protobuf-es types in/out
});
engine.addFunction(overloadId, impl: FunctionImpl);   // ⛔ host fns (Slice E)
// engine.addModule(alias, bytes)                      // ⛔ foreign (later milestone)

// ── Program: the compiled artifact (NO TS compiler — bytes cross over) ──
const program = Program.fromBytes(wasmBytes);         // ✅  + .fromUrl / .fromFile ⛔

// ── plan → instance: compile-once, eval-many ──
const instance: Instance = await engine.plan(program);   // ✅

// ── Activation: bind every CEL data type, in both backings ──
const act = new Activation()                          // ✅ scalars + aggregates
  .bind('x',  Value.int(41n))                         // ✅ bigint for int/uint
  .bind('s',  Value.string('hi'))                     // ✅
  .bind('u',  Value.message(registry.message(msg)))   // ✅ real protobuf-es msg
  .bind('o',  Value.object({ name: 'Ann' }))          // ✅ JS-object-mirrors-proto
  .bind('xs', Value.list([Value.int(1n)]))            // ✅ list (JS / proto repeated)
  .bind('m',  Value.map([[Value.string('k'), Value.int(7n)]])); // ✅ map

// ── eval ──
const r: Value  = instance.eval(act);                 // ✅ scalars + aggregates + host
const ru: Value = instance.partialEval(act, patterns);// ⛔ unknowns (Slice D)
const one       = await engine.run(program, act);     // ⛔ one-shot sugar (plan+eval)

// ── inspect the result — free functions over the union, not methods ──
isNull(r); isError(r); isUnknown(r);                  // ✅
asInt(r); asString(r); asBytes(r);                    // ✅
asMessage(r);                                         // ✅ MessageBacking (read interface)
registry.toMessage(asMessage(r));                     // ✅ typed protobuf-es message (out)
asList(r); asMap(r);                                  // ✅ aggregate out (materialized)

// ── host function: typed slot sugar (read args / write return) ──
const impl: FunctionImpl = (args: readonly Value[]): Value =>   // ⛔ (Slice E)
  Value.int(BigInt(asString(args[0]).length));
```

Notes on the shape (and the as-built deltas from the original sketch):
- **Async at the wasm boundary, sync within.** `Engine.create` / `plan`
  are `async` (`WebAssembly.compile`/`instantiate`); `instance.eval` is
  synchronous once planned. `Program.fromBytes` is sync (the ABI decode
  needs no I/O); `.fromUrl`/`.fromFile` (async) are still ⛔.
- **`Engine.create(runtimeWasm, { registry })`** — positional wasm bytes +
  an options object (not a single options bag). `engine.registry` exposes
  it for `toMessage` on a returned message.
- **`TypeRegistry`** = `TypeRegistry.fromDescriptorSet(bytes)` or
  `.fromFileRegistry(reg)`. It bridges both directions: `registry.message(msg)`
  → a `MessageBacking` for binding; `registry.toMessage(backing)` →
  the typed protobuf-es message (pass-through for proto-backed results).
  Without a registry, scalar / list / map and JS-object messages still
  work; only real-protobuf-es message I/O needs it.
- **`Value.message(backing)` takes a `MessageBacking`, not a raw message** —
  decoupling the value model from protobuf-es. `registry.message(msg)`
  builds the proto backing; `Value.object(obj)` builds the JS-object one;
  both produce a `CEL_MESSAGE`. `Value.list`/`Value.map` take `Value[]` /
  `[Value, Value][]`; `Value.listOf`/`mapOf` wrap an arbitrary backing.
- **Accessors are free functions** (`asInt(v)`, `asMessage(v)`), not
  methods — idiomatic discriminated-union narrowing, `Value`s stay plain
  data. `asMessage` returns the read interface (`MessageBacking`); typed
  materialization is `registry.toMessage` (the registry owns descriptors).
  `asList`/`asMap` materialize into `Value[]` / `[Value, Value][]`.
- **Error model:** host-side failures (unbound variable, kind mismatch,
  malformed program, dangling host-ref) **throw** (`EvalError` /
  `ValueError` / `EngineError` / `RegistryError`); CEL-level errors /
  unknowns come back as a `Value` of kind error / unknown (you inspect,
  you don't catch) — matching the C++ `StatusOr` vs error-value split.
- **`Value` is one discriminated union** that is a *superset* of the wire
  `CelValue` (scalars + error) plus the host-backed aggregate arms
  (message / list / map carry a backing) and `Unknown` — so the same type
  binds in and comes back out.
- **No `Compiler`** — compilation stays in C++; a `Program` is bytes.

**As-built status (this session).** ✅ shipped: `TypeRegistry`
(`type-registry.ts`); the public `Value` superset + aggregate factories +
materializing accessors (`value.ts` + `host/value-backing.ts`);
`Engine.create(runtimeWasm, { registry })` wired to the **real**
`cel_host.*` trampolines (deferred-binding), replacing the Slice-B
trap-stubs; `Activation.bind`/`Instance.eval` marshalling + decoding
host-backed aggregates (intern → ref slot → resolve). Validated by 140
hermetic unit tests @ 100% coverage **and** 21 real-`cel_runtime.wasm`
e2e tests (`backings.e2e.test.ts`) driving the public API across the full
Customer data-type matrix in both proto and JS-object backings, plus
aggregates as return values. Fixtures are reproducible via
`testdata/gen_fixtures.sh` (the C++ `cel compile`). Still ⛔: host fns
(`engine.addFunction` + `FunctionImpl` slot sugar, Slice E),
`partialEval` (Slice D), `engine.run` one-shot sugar, `Program.fromUrl`/
`.fromFile`.

### 5.8 Testing matrix — mandatory, per component

Per CLAUDE.md "Testing is mandatory": **every component ships positive +
negative + boundary tests in the same commit**, the matrix is enumerated
explicitly (not "one e2e that happens to pass"), and **probes are
throwaway — every behavior a probe validated must graduate into a
permanent test before closeout**, because the probe dir is deleted. The
corpus zero-diff (§5.6) is the integration gate; it does **not** absolve
the per-component unit tests below. Each landed component ticks rows in
`testing-checklist.md` and adds a row to a new TS section of
`per-component-test-coverage.md` (the keystone testing doc).

**Slice A (C++ split)** — behavior-preserving, so the obligation is
*structural*, not new behavioral tests (the moved `*_test.cc` carry those
and must stay green, §3.2):
- `program_golden_test.cc` — byte-identical `Program` across the move (§3.1).
- `layering_test` — `bazel query` dep-graph assertions (§3.3).

**TS host (vitest)** — `ts/eval/test/`, one spec per module:

| Module | Positive | Negative | Boundary |
|---|---|---|---|
| `celvalue` | encode+decode every `CelKind` round-trips | unknown kind byte; span ptr/len past memory | `INT64_MIN/MAX`, `UINT64_MAX`, `0`, `-1`, empty str, embedded NUL, multi-byte UTF-8, `NaN`/`±Inf`, sign-correlated dur/ts (graduates **P-7**) |
| `abi` | decode variables/fields/attributes/types | malformed/truncated section; `runtime_abi_version` mismatch rejected | no `cel.abi` section → empty ABI (variable-free eval still works) |
| `wasi-shim` | `random_get` fills buffer, returns 0 (graduates **P-3**/**P-4** finding) | — | all 14 imports *defined*; the 13 uncalled present as stubs |
| `externref` | intern→lookup per namespace; `Reset` clears | lookup miss → null; cross-namespace miss (list slot ≠ msg slot) | monotonic slot ids; slot 0 sentinel |
| `engine`/`instance` | scalar eval (graduates **P-4**); activation marshal incl. string-buffer (graduates **P-5**); back-to-back evals deterministic (arena reset) | missing binding → error; declared/bound kind mismatch → error; malformed program bytes | the §4.4 instantiation order; `arena_init` triggers lazy ctors |
| `value`/`activation`/`type` | factories + `asX()` accessors; fluent `bind` overwrite | wrong-kind `asX()` throws/Err | `bigint` for int/uint at the extremes |
| `host/*` (Slice C) | each trampoline's happy read/construct; **byte-parity vs C++ `out_slot`** (graduates **P-6**) | missing field → `kFieldNotFound`; wrong type → `kTypeMismatch`; OOB index → `kIndexOutOfBounds`; missing key → `kNoSuchKey`; **3VL absorb** unknown/error operand | proto `cpp_type` matrix (every numeric width, ENUM→int, string/bytes); every valid map-key kind + a rejecting complement; proto2/proto3 presence for `has()` |
| matcher comparator (§5.6) | the `(decoded, matcher)→expect` drift-guard table copied from `runner_test.cc` | mismatched kinds; `eval_error`/`unknown` matcher shapes | cross-type-numeric equality (int/uint/double); 3VL |
| `binding_marshal` port (§5.6) | each `cel.expr.Value` binding → `Activation` | aggregate/proto binding before the descriptor pool exists → graceful skip | — |

**Conformance bridge**:
- C++ `conformance_fixture_export_test.cc` — export a fixture + reload it;
  assert round-trip (proto parses, wasm bytes intact, `cpp_outcome` +
  `cpp_decoded` recorded).
- TS — the **zero-diff vs C++** run IS the integration test; the matcher
  comparator + `binding_marshal` unit tables above are its scaffolding.

**Probe → permanent-test graduation** (probes are deleted at closeout —
the right-hand column MUST exist before the milestone closes):

| Probe (throwaway) | Graduates into (permanent) |
|---|---|
| P-1 compile runtime | CI matrix: min Node version in `engines` + a smoke `WebAssembly.compile` test |
| P-3 import surface | `wasi-shim` + `engine` import-table tests |
| P-4 scalar eval | `instance.test.ts` scalar cases |
| P-5 activation marshal | `instance.test.ts` marshal + string-buffer cases |
| P-6 protobuf-es parity | `host/*.test.ts` byte-parity tables |
| P-7 bigint codec | `celvalue.test.ts` boundary cases |
| P-8 fixture bridge | the conformance runner + `conformance_fixture_export_test.cc` |
| P-2 browser | a Playwright/headless-Chrome smoke under cross-origin isolation |

---

### 5b. Probe tasks — run these BEFORE writing host code

Per the repo's probe discipline (CLAUDE.md "Probe vendored cel-cpp…" and
the `compiler_v2/probes/` precedent): a design doc that *asserts* a
runtime fact without a probe behind it is a guess. The TS host has several
load-bearing assumptions about the JS wasm engines and protobuf-es that
**memory gets subtly wrong** — pin each with a throwaway probe before
committing to the design. Probes live under `ts/eval/probes/<id>/`,
are disposable (delete at host-v1 closeout — they are NOT the permanent
test suite, §5.6 is), and each records what it confirmed in this doc with
a dated callout.

| ID | Assumption to confirm | Probe shape | Why it can't be assumed |
|---|---|---|---|
| **TP1** | Target **Node version executes the runtime `.wasm`'s tail-calls + shared memory + threads** proposals | `WebAssembly.compile(cel_runtime.wasm)` + instantiate in the target Node; assert no `CompileError` | tail-calls shipped recently; pin the **minimum Node version** the package declares (`engines` field) |
| **TP2** | **Browser** runs it under cross-origin isolation; and what fails *without* COOP/COEP | tiny page with/without the headers; instantiate; observe `SharedArrayBuffer`/`Memory{shared:true}` behavior | this is the headline browser-deployment caveat (R2); confirm the exact failure mode + minimum Chrome version |
| **TP3** | The **minimal WASI preview1 surface** `cel_runtime.wasm` actually *imports* (vs the full wasi-libc surface) | parse the runtime module's import section; list `wasi_snapshot_preview1.*`; instantiate with a stub that traps-on-call and run a scalar eval to see which are *called* | C++ leans on wasmtime's full WASI; the TS shim only needs the imported set, and only the *called* subset needs real behavior (`phase-c-plan.md` §7.2 says most are never called) |
| **TP4** | **End-to-end scalar eval works**: instantiate runtime+expr sharing the runtime's exported memory, call `$eval`, decode the result CelValue | take a `Program` for `"1 + 2"` from `cel compile`; reproduce the §4.4 sequence in TS; assert the decoded offset holds `CEL_INT 3` | the runtime-owns-memory two-module wiring (`engine.cc:164`) is the riskiest plumbing assumption; prove it on the simplest possible program |
| **TP5** | **Activation marshal** round-trips: write a bound CelValue into a `slot_offset`, eval, read it back | `Program` for `"x + 1"`, bind `x=41`, expect `42`; then a string var to exercise the activation buffer (`instance.cc:377`) | the malloc'd activation-buffer dance (string/bytes can't live in the bump arena) is non-obvious and easy to get wrong |
| **TP6** | **protobuf-es reflection produces byte-identical `out_slot`** to C++ for a `cel_get_field` read | build a `FileDescriptorSet` for a test proto, read one scalar + one nested field via protobuf-es reflection, encode the CelValue, diff bytes against the C++ `cel_host.cel_get_field` output for the same input | **R1, the dominant risk.** protobuf-es's reflection model (enum/wrapper/Any handling) differs from C++ protobuf; this probe decides whether full proto parity is as cheap as hoped or needs per-trampoline shimming |
| **TP7** | **bigint ⟷ wire i64/u64** round-trips at boundaries | encode `INT64_MIN`, `INT64_MAX`, `UINT64_MAX` through the CelValue codec via `DataView.getBigInt64`/`setBigUint64`; assert exact | JS `number` silently corrupts > 2^53; confirm the `DataView` bigint path is correct at the extremes |

Sequencing: **TP1/TP3/TP4 first** (they gate the whole plumbing layer and
the package's declared engine support), **TP6 before committing to full
proto parity** (it sizes the P1/P2 effort), TP5/TP7 alongside the codec
work. Record each probe's verdict here with a dated callout citing the
`.ts` file + the C++ file:line it was checked against (the
`m17-encoders-ext.md` §2 pattern).

> **TP3 verdict — 2026-05-25 (static inspection, no TS yet).** Method:
> `bazel build //compiler_v2/runtime:cel_runtime_wasm` →
> `wasm-objdump -x` on the 2.68 MB artifact. The runtime declares
> **exactly 30 function imports, no imported memory/table/global** —
> confirming it owns + exports its own `memory` (plus `arena_alloc` /
> `arena_init` / `arena_reset` / `malloc`), validating the §4.4
> runtime-owns-memory wiring against the real binary, not just
> `engine.cc:164`. The 30 imports a TS linker MUST define:
>
> - **`cel_env.cel_log`** ×1.
> - **`cel_host.*`** ×15 — `cel_map_lookup`, `cel_list_iter_open`,
>   `cel_list_at`, `cel_list_size`, `cel_list_in`, `cel_list_eq`,
>   `cel_list_concat`, `cel_map_size`, `cel_map_in`, `cel_map_eq`,
>   `cel_map_iter_open`, `cel_message_eq`, `cel_timestamp_tz_accessor`,
>   `resolve_message_type_name`, `cel_set_field`.
> - **`wasi_snapshot_preview1.*`** ×14 — `environ_get`,
>   `environ_sizes_get`, `clock_time_get`, `fd_close`, `fd_fdstat_get`,
>   `fd_prestat_get`, `fd_prestat_dir_name`, `fd_read`, `fd_seek`,
>   `fd_write`, `poll_oneoff`, `proc_exit`, `sched_yield`, `random_get`.
>
> **Three findings that correct/refine the design:**
> 1. **The `cel_host` surface is split across the two modules.** The
>    *runtime* imports only the 15 above; the remaining trampolines
>    (`cel_get_field`, `cel_has_field`, `cel_make_message`,
>    `cel_wkt_unwrap_time`, `cel_wkt_unwrap_wrapper`) are imported by the
>    *expr* module. A TS host registers **all 19** on the shared linker
>    regardless, since both modules resolve against the same `cel_host`
>    namespace — but P-4's scalar program will only pull the subset its
>    expr module references. Register the full set as trap-stubs from day
>    one (matches `cel_host_wasmtime.cc:524`).
> 2. **WASI is 14 imports, not "~10".** All 14 must be *defined* or
>    instantiation fails with `unknown import`; only a subset is *called*
>    during eval (P-4 measures which). `clock_time_get` + `random_get` are
>    the likely live ones; the `fd_*` / `environ_*` / `proc_exit` set is
>    almost certainly dead wasi-libc surface (`phase-c-plan.md` §7.2). The
>    TS shim defines all 14; real behavior needed only for the called few.
> 3. **No imported memory** means the TS `engine.ts` must mirror
>    `engine.cc:164` exactly: instantiate the runtime *first*, pull its
>    exported `memory`, and supply *that* as the `cel.memory` import when
>    instantiating the expr module — not a host-allocated `WebAssembly.Memory`.
>
> Carries straight into Slice B's import-registration table. TP3 box
> ticked in §0.5.

> **TP4 verdict — 2026-05-25 (PASS, the keystone).** Probe:
> `ts/eval/probes/m19/p4_scalar_eval.mjs` (pure Node, no
> npm deps). Reproduces `engine.cc` InstantiateRuntime → SeedRuntimeArena
> → InstantiateExpr + `instance.cc:260` DecodeCelValueAt for the
> `cel compile "1 + 2"` Program. Result: `eval()` returns offset **64**
> holding `{kind: CEL_INT, i: 3}`. The two-module runtime-owns-memory
> wiring, `cel.*` export-binding, `cel_host.*` trap-stubs, and the
> CelValue codec all work in JS. **Findings that correct the design:**
> 1. **Ctors run lazily, not at instantiation.** There is no
>    `_initialize`; wasi-libc's `.command_export` wrappers run
>    `__wasm_call_ctors` on the **first exported call** (`arena_init`).
>    So the TS `engine.ts` ordering matches `engine.cc` exactly: call
>    `arena_init` first, and ctors fire transparently inside it.
> 2. **`random_get` is the ONE WASI import actually called** — and it's
>    called from `__wasilibc_init_ssp` (stack-canary seed) *during ctor
>    init*, not during eval. It must be a **real** impl (fill the buffer,
>    return errno 0), not a trap. The other 13 WASI imports are
>    defined-but-never-called for a scalar eval (confirms `phase-c-plan.md`
>    §7.2). This refines P-3's earlier guess (which had named
>    `clock_time_get` too — *not* called).
> 3. **`expr.eval()` is a plain JS call** returning an i32; the result
>    lives in the runtime's shared-memory buffer, read via
>    `new DataView(memory.buffer)`. `int`/`uint` decode via
>    `getBigInt64`/`getBigUint64` (→ P-7).
>
> The probe is the direct seed for Slice B's `engine.ts` + `instance.ts`
> + the import tables.

> **TP5 verdict — 2026-05-25 (PASS).** Probe:
> `ts/eval/probes/m19/p5_activation_marshal.mjs`. Mirrors
> `instance.cc:928` MarshalActivation. Two cases:
> - **int** `x + 1`, `x=41` → `42` — write `{CEL_INT, i:41}` directly into
>   the workspace cell at the `cel.abi` `slot_offset`; `arena_reset` (the
>   `$eval` prologue) does not touch the workspace, so the slot survives.
> - **string** `s + "!"`, `s="hi"` → `"hi!"` — bytes go in a `malloc`'d
>   region (mirrors `EnsureActivationBuffer`, `instance.cc:377`), the slot
>   gets `{CEL_STRING, s:{ptr,len}}`. Round-trips out as an arena string.
>
> The probe carries a **working `cel.abi` decoder** (wasm custom-section
> walk + a minimal protobuf reader) — this is the first real piece of
> `abi.ts` and graduates there + into `instance.test.ts`. **Finding (now
> in §4.2):** `cel.abi.variables[].repr` is the `ir::Repr` ordinal, not
> `CelKind` — int=`3`, string=`6` (Repr leads with `kUnknown=0`). The
> probe initially mis-asserted 2/5; the marshal still worked because it
> keys on the known declared type, but `abi.ts`'s repr→encoder table must
> use the corrected ordinals.

> **TP7 verdict — 2026-05-25 (PASS).**
> `ts/eval/probes/m19/p7_bigint_codec.mjs` round-trips
> `INT64_MIN`, `INT64_MAX`, `UINT64_MAX`, `2^63`, `0`, `-1` exactly via
> `DataView.getBigInt64`/`setBigUint64`; the same script confirms JS
> `number` *would* corrupt them. Locks: the `int`/`uint` CelValue arms in
> `celvalue.ts` use **`bigint`**, and `Value.int`/`Value.uint` take/return
> `bigint`. → `celvalue.test.ts` boundary table.

> **TP9 verdict — 2026-05-25 (PASS).** `p9_host_aggregates.mjs` (zero
> npm deps). Binds three plain-JS backings in activation and reads them
> back through eval, implementing the real `cel_host` trampolines:
> - **object** `{name:"Ann",age:20n}` → `CEL_MESSAGE` externref → `u.name`
>   via `cel_get_field` (field NAME from `cel.abi.fields[]`) → `"Ann"`.
>   This is the JSON/struct backing the C++ host supports via a custom
>   `HostMessageBacking` subclass.
> - **array** `[10n,20n,30n]` → `CEL_LIST_HOST` → `xs[1]`=20 via
>   `cel_list_at`, `xs.size()`=3 via `cel_list_size`.
> - **Map** `{k:7n,j:9n}` → `CEL_MAP_HOST` → `m["k"]`=7 via `cel_map_lookup`.
>
> Validates the externref table (3 independent namespaces, slot 0
> sentinel) + 4 trampolines + `cel.abi.fields` decode + arena-allocated
> string results — the whole Slice-C host-aggregate plumbing minus
> protobuf-es reflection. Key structural finding: the **runtime's kHost
> dispatchers tail-call `cel_host.*`**, so the real trampolines must be
> wired into the *runtime* instance's imports (not just the expr
> module's). → `externref.test.ts` + `host/*.test.ts` + `abi.test.ts`.

> **TP6 verdict — 2026-05-25 (PASS).** `p6_proto_bind.mjs`
> (`@bufbuild/protobuf` v2). Builds a real protobuf-es `Customer` from a
> `protoc --include_imports` `FileDescriptorSet` (the bazel
> `*-descriptor-set.proto.bin` is NOT transitive — it omits the WKT
> imports `e2e_fixture.proto` pulls in; regenerate with `--include_imports`
> + `-I /opt/homebrew/include`). Binds it to `u:proto(...)`; the SAME
> `cel_get_field` harness as P-9 reads `u.name` via
> `reflect(desc,msg).get(field)` keyed on `cel.abi.fields[id].field_number`
> → `"Ann"`. **Confirms the trampoline harness is backing-agnostic** —
> only the field accessor swaps (descriptor reflection vs `obj[name]`).
> protobuf-es v2 API used: `createFileRegistry(fromBinary(
> FileDescriptorSetSchema, fds))` → `registry.getMessage(fqn)` →
> `create(desc,{...})` → `reflect(desc,msg).get(field)`. **Open for
> Slice C:** byte-exact diff of the encoded `out_slot` vs the C++
> `cel_host.cel_get_field` across the proto `cpp_type` matrix (enum,
> wrapper, repeated, map, nested) — this probe only did a scalar string
> field. → `host/*.test.ts` byte-parity tables.

> **TP8 verdict — 2026-05-25 (PASS — the bridge is real, not a one-row
> probe).** Built both halves of §5.6:
> - **C++ exporter** `//compiler_v2/conformance:conformance_ts_export`
>   (new `runner::CompileForExport` exposed in `runner.{h,cc}` +
>   `conformance_ts_export.cc`). Reuses the corpus iteration; per row
>   emits `<n>.wasm` + an `index.jsonl` line carrying `cpp_outcome` + the
>   `SimpleTest` as **proto3 JSON** (`MessageToJsonString`) — so the TS
>   side needs no extra descriptors for scalar matchers. Rows that don't
>   compile (e.g. static-subset) are skipped (no Program to ship).
> - **TS runner** `ts/conformance/run.mjs` — compiles `cel_runtime.wasm`
>   once, instantiates each fixture's expr against it (the proven P-4/P-9
>   wiring), marshals scalar bindings, evals, decodes, compares against
>   the row's matcher with a scalar comparator (the `value:` arms +
>   implicit-bool-true + `eval_error`), and zero-diffs vs `cpp_outcome`.
>
> On `comparisons.textproto` (406 rows; 358 compiled+exported): **285/285
> comparable rows agree, 0 diffs**; 69 ts=skip (string/`dyn`/aggregate —
> the v1 proto-free-scalar subset; `cel_host.*` are trap-stubs that
> classify a reached row as skip); 4 `cpp=skip / ts=pass` correctly
> EXCLUDED — they're `disable_check:true` rows C++ rejects at scope
> (`ScopeReject`) but which compile + evaluate to the expected
> `eval_error` in TS. **Gate semantics locked:** diff only where
> `cpp ∈ {pass,fail}` (both evaluated the same matcher → must agree);
> `cpp=skip` = no C++ ground-truth verdict, excluded; `ts=skip` = not
> implemented in v1, excluded. This is the Slice B/C DoD harness in
> embryo. → graduates into `ts/conformance` + `conformance_fixture_export_test.cc`.

## 6. Part E — the Go follow-up (later)

Decision was *TS now, Go later*. When Go happens, it consumes **§4 verbatim**
— the language-neutral ABI is the point. Notes so the door stays open:

- **Host runtime**: `wasmtime-go` (or `wazero`, pure-Go, no cgo — attractive;
  confirm it supports tail-calls + shared memory + the threads proposal,
  which is the gating question for wazero specifically).
- **Proto reflection is *easier* in Go**: `google.golang.org/protobuf/reflect/protoreflect`
  maps almost 1:1 to C++ protobuf reflection, so the §5.4 trampolines —
  the hard part in TS — are close to mechanical in Go. This is the one place
  Go strictly beats TS, and it's why Go is the natural *server-side* second
  host if that need ever materializes.
- **Same corpus-diff gate** (§5.6), pointed at the Go host.
- Go does **nothing** for the browser, which is why it's the follow-up, not
  the lead.

---

## 7. Risks & open questions

- **R1 — proto-parity in JS** (§5.4): the dominant risk. Mitigation: probe
  every trampoline against C++ `out_slot` bytes before trusting it.
- **R2 — browser shared-memory** requires COOP/COEP cross-origin isolation.
  Mitigation: document; offer a non-shared-memory fallback only if a
  single-threaded runtime build exists (it doesn't today — runtime is
  `wasm32-wasi-threads`, `engine.cc:80`). Revisit if browser deployment
  without COOP/COEP is a hard requirement.
- **R3 — runtime `.wasm` distribution + versioning**: the TS package must
  embed a runtime build whose `kRuntimeAbiVersion` matches the Programs it
  evals (`runtime_catalogue.h`; check at instantiate, `engine.cc:610`). Ship
  them together; surface a clear version-mismatch error like the C++ host.
- **O1** (§2.4): keep vs drop `namespace cel` back-compat shims — recommend
  keep.
- **O2**: does the TS host need `PartialEval` (unknowns) for v1? It's a
  distinct corpus slice; defer to P3 unless required.
- **O3**: fixture-bridge tool — extend `tools/cel` or new tool? Lean toward
  extending the existing CLI.

## 8. Future work (surfaced here, not committed)

- A non-shared-memory single-threaded runtime build to drop the browser
  COOP/COEP requirement (only if browser-without-isolation becomes a hard
  ask).
- `cel_fn` custom-function support in the TS host (host callbacks → JS
  functions).
- Publishing the TS host as an npm package + the runtime `.wasm` as a
  versioned asset.
- A **TypeScript section in `doc/user-guide.md`** once the host ships —
  the same `Engine`/`Program`/`Instance`/`Activation`/`Value` walkthrough
  (§5.7), with the eval-half-only caveat and the browser COOP/COEP note
  called out via the guide's ✅/🟡/⛔ status legend.
