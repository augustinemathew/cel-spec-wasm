# Repo restructure: making the compiler the repo

Status: plan — drafted 2026-05-25, revised 2026-05-25 (final shape), not yet started.

> **Operational companion:** the step-by-step parallel rollout — the frozen
> path/label/include mapping, copy-paste agent briefs, and wave/merge
> procedure — lives in
> [`repo-restructure-execution.md`](repo-restructure-execution.md). This doc
> is the *why/what* (design); that doc is the *how* (execution). §6 here is a
> summary only; the execution doc is authoritative for the rollout.

## 1. Why

This repo began as a fork of CEL's language-spec repo
(`module(name = "cel-spec")`). Three things change so the compiler becomes the
unambiguous centre of the repo:

  1. **Shed the cel-spec heritage cleanly.** The generated Go bindings and
     their regen toolchain are dead weight (the compiler is C++; nothing
     consumes a Go target) — deleted. The parts we still depend on — the
     `.proto` type definitions and the `.textproto` conformance corpus — get
     corralled into one clearly-inherited `spec/` directory.

  2. **Dissolve `compiler_v2/`.** The `_v2` suffix is a scar from the V1
     deletion (2026-05-24). Its children promote to the top level, organised
     by **lifecycle role** — the layout cel-cpp itself uses (`compiler/`,
     `eval/`/`runtime/`, `common/`, no `api/`).

  3. **Drop the `api/` umbrella.** cel-cpp — the repo's designated style
     source — has no `api/`. In Abseil/cel-cpp convention the public surface
     is "every header not under an `internal/` subdir," enforced by Bazel
     `visibility`, not by a directory name. `api/` straddled the compile/eval
     seam and bundled a half-shared value model; splitting it along that seam
     makes the architecture structural.

Settled decisions (2026-05-25):

  - Go surface **deleted**, not relocated (reversible via git history).
  - Corpus keeps upstream nesting under `spec/`.
  - C++ is the **first-class** binding (it's the reference impl and the
    compiler is C++). Future TS/Go bindings live under `bindings/`. The
    long-term aim (§9) is that **the compiler itself compiles to wasm**, so
    those bindings get *both* compile and eval — not eval-only — by embedding
    `compiler.wasm` alongside `cel_runtime.wasm`. The layout must not preclude
    this: `compiler/` stays wasm-targetable (no host-only deps), the same
    discipline `runtime/` already follows.
  - Follow **Abseil/cel-cpp** convention, not gRPC: no `include/` tree;
    public/private gate is `internal/` + `visibility`.
  - **`third_party/` stays** — it is the external-dependency integration
    layer (cel-cpp fetch, Binaryen + wasmtime BUILD glue, wasi-sdk toolchain,
    patches), almost all tiny committed glue. Nothing builds without it.
  - **No `bazel/` directory yet.** cel-cpp's `bazel/` holds *first-party*
    Starlark rules/macros (a different job from `third_party/`'s dep
    integration). We have almost none of our own today (the wasi-sdk `.bzl`
    correctly lives with its dep; `antlr_cc_library` is borrowed from
    `@cel-cpp//bazel`), so a `bazel/` now would be near-empty. Convention
    reserved: when a reusable, dep-independent first-party macro appears
    (e.g. a `cel_wasm_embed` rule, a `wat_test` macro), it goes in a
    top-level `bazel/` mirroring cel-cpp — not scattered into package dirs.

## 2. The seam: compile-time vs eval-time vs shared

Verified by `#include` / BUILD-dep inspection (2026-05-25):

| Stage | Flow | Packages |
|---|---|---|
| **Compile** | CEL source → `Program` (wasm bytes + `cel.abi`) | `frontend` → `ir` → `codegen` + `celfn`, behind `compile.{h,cc}`; public face `Compiler`/`Program` |
| **Eval** | `Program` + `Activation` → `Value` | `Engine`/`Instance` → `runtime` + `host` + `ir/annotations` + wasmtime |
| **Shared** | the contract + vocabulary both sides speak | `abi/` (emit *and* parse — `runtime_catalogue` used by `compile.cc` **and** `engine.cc`); `CelType` |

The key facts that shape the split:

  - **`CelType` is shared, `Value` is eval-only.** Compile uses `CelType`
    (`DeclareVariable`, `CelTypeToSpec`); eval uses it internally
    (`cel_host`). But eval's *public* headers (`engine/instance/activation/
    value/error/attribute`) reference `CelType` **zero** times. `Value`'s every
    includer is eval-side. So there is no shared "value" concern — only a
    shared *type* vocabulary.
  - This mirrors cel-cpp, which keeps shared `Value`/`Type` in `common/`. We
    diverge on one point: our `Value` crosses the wasm boundary and is decoded
    host-side, so it is genuinely eval-only and stays in `eval/`; only
    `CelType` graduates to `common/`.

## 3. Target shape

```
/
├── compiler/            COMPILE-TIME: CEL source → Program (wasm + cel.abi)
│   ├── frontend/        parse_and_check — wraps cel-cpp compiler/, RejectDyn
│   ├── ir/              typed_ast + annotations (stable mid-layer)
│   ├── codegen/         resolve/layout/lower → Binaryen IR → wasm
│   ├── celfn/           function library (compile-time)
│   ├── internal/        compile.{h,cc} — private pipeline facade
│   └── compiler.{h,cc}, program.h    PUBLIC: Compiler, Program
│
├── eval/                EVAL-TIME: Program + Activation → Value (C++ evaluator)
│   ├── engine.{h,cc}, instance.{h,cc}, activation.{h,cc}   PUBLIC
│   ├── value.{h,cc}, error.{h,cc}, attribute.{h,cc}        PUBLIC (eval-only)
│   ├── host/            cel_log trampolines
│   └── internal/        wasmtime glue, abi_decode, cel_host
│
├── common/              CelType — shared type vocabulary (cel-cpp precedent)
├── abi/                 cel.abi wire contract (shared: emit + parse)
├── runtime/             cel_runtime.c → cel_runtime.wasm (language-agnostic)
│
├── bindings/            other host bindings — embed compiler.wasm +
│   ├── typescript/      cel_runtime.wasm to get BOTH compile and eval (§9);
│   └── go/              (future; never reimplement the pipeline in-language)
│
├── tools/               cel CLI (eval/check/compile), wat_runner
├── conformance/         harness that runs the corpus against the pipeline
├── e2e/                 full-pipeline integration tests
├── bench/               Google Benchmark microbenches
├── testdata/            shared proto fixtures + cel_cpp_oracle
│
├── spec/                cel-spec HERITAGE (inherited contract)
│   ├── proto/           cel/expr/*.proto + conformance/* + policy
│   └── tests/simple/testdata/*.textproto
│
└── doc/  scripts/  third_party/   unchanged
```

The public/private boundary is **`internal/` + Bazel `visibility`**, the
Abseil/cel-cpp convention — not a separate header tree. `compiler/` and
`eval/` both depend on `common/`; neither depends on the other. `bindings/`
never reimplements the compiler in-language — it embeds the **wasm
artifacts** (`cel_runtime.wasm` today; `compiler.wasm` once §9 lands) and
drives them through the host's own wasm runtime.

## 4. Disposition of every directory

| Old | New | Action |
|---|---|---|
| `compiler_v2/frontend ir codegen celfn` | `compiler/{frontend,ir,codegen,celfn}` | move |
| `compiler_v2/compile.{h,cc,_test.cc}` | `compiler/internal/compile.*` | move |
| `compiler_v2/api/compiler.*`, `program.h` | `compiler/` (public) | move + repoint |
| `compiler_v2/api/type.*` | `common/type.*` | move (shared) |
| `compiler_v2/api/{engine,instance,activation,value,error,attribute}.*`, `host_callback.h`, `internal/` | `eval/` | move |
| `compiler_v2/host/` | `eval/host/` | fold into eval |
| `compiler_v2/api/cel_pipeline_bench.cc` | `bench/` | relocate (belongs in bench) |
| `compiler_v2/abi` | `abi/` | move (shared) |
| `compiler_v2/runtime` | `runtime/` | move |
| `compiler_v2/{tools,conformance,e2e,bench,testdata}` | top-level | move |
| `compiler_v2/probes/` | — | **delete** |
| `wasm_compilation_experiments/` | — | **delete** |
| Go surface (root `*.pb.go`, `BUILD.bazel`, `go.mod/sum`, `regen_go_proto*.sh`, `conformance/*.pb.go`) | — | **delete** |
| `proto/` | `spec/proto/` | move |
| `tests/` | `spec/tests/` | move |
| `compiler_v2/README.md` | merge into root `README.md` | rewrite |

Churn measured 2026-05-25: **219 files** reference `compiler_v2`
(1238 occurrences); ~200 `//compiler_v2/api:*` label refs across the split;
**802** `celwasm::api` namespace occurrences (see §7 — namespace is a separate
deferrable workstream).

## 5. Migration mechanics

### 5.1 Go-surface deletion
`rm` the root `*.pb.go`, root `BUILD.bazel` (`go_library` `cel.dev/expr`),
`go.mod`/`go.sum`, `regen_go_proto*.sh`, and `conformance/*.pb.go`. Strip
`go_library`/`go_proto_library` targets (+ `@io_bazel_rules_go` loads,
`importpath = "cel.dev/expr…"`) from the six proto BUILD files; keep each
`proto_library`/`cc_proto_library`. Drop now-unused `rules_go`/`gazelle`
`bazel_dep`s from `MODULE.bazel` (verify with `bazel query`). Resolve whether
`docker/` + `cloudbuild.yaml` are compiler CI or Go-regen-only before deleting.

### 5.2 Heritage → `spec/`
`mv proto spec/proto`, `mv tests spec/tests`. **Fix `strip_import_prefix`:**
`"/proto"` → `"/spec/proto"` in every `proto_library` (the `.proto`
`import "cel/expr/…"` statements do **not** change — load-bearing). Rewrite
labels: `//proto/cel/` → `//spec/proto/cel/`, `@cel-spec//proto/cel/` →
`//spec/proto/cel/`, `//tests/simple:` → `//spec/tests/simple:`.

### 5.3 `compiler_v2/` strip + role grouping
`git mv` each child to its §3 home, then repo-wide rewrite of
`compiler_v2/<pkg>` → new path across all `BUILD.bazel` + `#include`s. Do this
as **one scripted pass** (it is deterministic; partial application breaks the
build everywhere). Move `api/` intact to a temporary top-level `api/` in this
pass so `compiler_v2/` disappears and the tree stays green — the split is 5.4.

### 5.4 `api/` split (the judgment step)
Repoint each `//api:foo` dependent to the new owner: `compiler` /
(`compiler`, `program`) → `compiler/`; `type` → `common/`; `engine`,
`instance`, `activation`, `value`, `error`, `attribute`, `cel_host`,
`internal/*` → `eval/`. Fold `host/` into `eval/host/`. New inter-package
deps: `compiler` → `common`, `eval` → `common` + `runtime` + `abi`. No
`compiler` ↔ `eval` edge.

### 5.5 Visibility regime — the enforced public/internal boundary

Today **18 of 19 packages default to `//compiler_v2:__subpackages__`** — every
target is visible across the whole tree, so there is no public/internal
distinction and nothing stops one package depending on another's guts. After
dissolution `//compiler_v2:__subpackages__` is meaningless. This is the moment
to install a real boundary; it is **not** a prefix-swap (a per-package decision,
done in W3). The rule we want: **nobody can take a dependency on a non-public
target** — Bazel `visibility` enforces this at analysis time (an out-of-scope
`deps` edge fails the build), so it is the mechanism, not a convention.

The regime (default-private, curated-public):

  - **Default is the narrowest scope, never public.** Each component's packages
    default-scope to that component only:
      - `compiler/**` → `default_visibility = ["//compiler:__subpackages__"]`
      - `eval/**`     → `default_visibility = ["//eval:__subpackages__"]`
    So `compiler/codegen`, `compiler/ir`, `compiler/internal`, `eval/host`,
    `eval/internal`, … are reachable only from within their own component.
    `eval/` cannot reach into `compiler/`'s guts and vice-versa (the
    architecture has no such edge — §2 — so this costs nothing).

  - **The curated public surface is explicit, per-target `//visibility:public`**
    — and small. Exactly these:
      - `//compiler:compiler`, `//compiler:program`
      - `//eval:engine`, `//eval:instance`, `//eval:activation`,
        `//eval:value`, `//eval:error`, `//eval:attribute`
      - `//common:type`
      - the shared contract: `//abi:*` (public emit/parse + `cel.abi` proto)
        and `//runtime:*` (the `cel_runtime.wasm` artifact + native test lib)
        — these are intentionally public because every binding speaks them.
    Adding `//visibility:public` to anything else is a reviewable event.

  - **`internal/` is belt-and-suspenders.** A target under `compiler/internal`
    or `eval/internal` is both physically in `internal/` *and* scoped to its
    component — a doubly-clear "do not depend on this from outside."

  - **Tests / testdata** get repo-wide test visibility as needed
    (`//visibility:public` is acceptable for `testonly = True` fixtures), but
    never widen a non-test target to reach them.

Because `compiler ⊥ eval` and both depend only on the public contract
(`common`, `abi`, `runtime`), this regime has no awkward exceptions: the only
cross-component edges land on intentionally-public targets. `bindings/` and any
external consumer can reach the curated public list and nothing else.

**Enforcement is automatic + audited.** Visibility is checked by every `bazel
build`, so a bad `deps` edge can't merge. The gate additionally *audits the
surface* so public doesn't creep: assert the set of `//visibility:public`
non-test targets equals the curated list above (a `bazel query` diff), and that
no target outside `//compiler/...` depends on `//compiler/internal/...` (resp.
`eval`) — `rdeps(//..., //compiler/internal/...) except //compiler/...` is
empty.

## 6. Parallelizable execution (summary)

> Authoritative rollout — frozen mapping, agent briefs, merge procedure — is
> [`repo-restructure-execution.md`](repo-restructure-execution.md). This is the
> shape only.

The hard constraint: a path rename breaks every dependent label until updated,
so the build is green only at **wave boundaries**, never mid-wave. Parallelism
is extracted where agents own **disjoint file-sets** (merge conflict-free) and
where the frozen mapping makes each file's rewrite independent. Global
mechanical sweeps stay serial and scripted; parallel agents run in `worktree`
isolation and merge in the stated order.

```
W0 ─── W1 ═╗(∥ ×2) ═══ W3 ─── W4 ═╗(∥ ×N) ═══ W5 ─── W6
           ╚══════════           ╚═══════════
        (Spec ∥ Docs)        (verify per pkg)
```

| Wave | Mode | Work | Maps to |
|---|---|---|---|
| **W0** | serial | capture baseline + deletions (probes, experiments, Go surface) + author the frozen rewrite script | §5.1 |
| **W1** | ∥ ×2 | **Spec:** heritage → `spec/` ‖ **Docs:** path-rewrite `doc/**/*.md` | §5.2 |
| **W3** | serial, scripted | `git mv` per §4 + apply mapping + split `api/` into `compiler`/`common`/`eval` + set `visibility` | §5.3–5.5 |
| **W4** | ∥ ×N | per-package verifiers (`bazel test //<pkg>/...` + straggler fixups) **+ W4·Conventions** (CLAUDE.md + root README rewrite, post-W3) | — |
| **W5** | serial | **exit-criteria gate** (below) → merge to master | — |
| **W6** | serial, scripted | flatten `celwasm::api` → `celwasm`; re-run exit criteria | §7 |

Peak parallelism is 10 agents in W4 (9 verifiers + Conventions); the serial
spine (W0→W3→W5→W6) is unavoidable because each step re-points the build globally.
Every agent edits only its assigned file-set, lands green-or-isolated, and
reports the exact paths it touched for merge triage.

**Exit criteria (W5).** A path-only move must regress nothing — so W5 is binding
and goes beyond a green `bazel test //...`:

  - `bazel test //...` green (all unit/component/e2e).
  - **Manual-tagged tests run explicitly** — `bazel test //...` skips them, but
    they carry the load-bearing e2e assertions (CLAUDE.md); run every
    `attr(tags, manual, //...)` target, cross-checked against the catalog in
    `per-component-test-coverage.md`.
  - **Conformance pass count == the master baseline** captured in W0 (path-only
    ⇒ identical; any drop is a label/prefix slip).
  - **Benchmarks build** under `//...` and a representative bench **runs** under
    `-c opt`.
  - `scripts/lint.sh --branch` clean; `bazel query 'kind(go_.*, //...)'` empty;
    no `compiler_v2` left in code/BUILD.

## 7. Namespace rename — `celwasm::api` → `celwasm` (W6, committed follow-on)

`celwasm::api` (802 occurrences) flattens to `celwasm`. This is **separate**
from the directory move (a directory hosts any namespace), so it is its own
scripted single-purpose commit — but it is **not** open-ended "someday": once
`api/` is deleted, a `celwasm::api` namespace with no `api/` directory is
exactly the drift this restructure removes. So it is **W6, the committed
immediate follow-on after W5**, not a maybe-later.

Why flat `celwasm` is right (and low-risk), confirmed 2026-05-25:

  - The codebase is **already predominantly flat** — 233 `namespace celwasm`
    declarations vs 90 `namespace celwasm::api`. `::api` was carved out only to
    fence the public surface off from internal symbols; `internal/` +
    `visibility` (§5.5) now does that job, so the namespace fence is redundant.
  - cel-cpp uses a flat `cel` namespace (internal bits in `cel::*_internal`),
    and we already follow that with `celwasm::string_format_internal` /
    `string_ext_internal`.
  - **Collision-clean**: every public `celwasm::api` type name (`Value`,
    `Error`, `CelType`, `Compiler`, `Program`, `Engine`, `Instance`,
    `Activation`, `Attribute*`, `Message`, `Host*Backing`, …) was checked
    against class/struct names in the internal `celwasm` packages — **no
    clashes**. W6 must still sweep free-functions/enums for the same before
    flattening, but the load-bearing type names are clear.

W6 mechanics: scripted sed `namespace celwasm::api {` → `namespace celwasm {`
(and close-comments), `celwasm::api::` → `celwasm::`, `using ::celwasm::api::X`
→ `using ::celwasm::X`. A transitional `namespace api = ::celwasm;` alias can
stage the 802 call sites if a single atomic diff is too large, removed once
green.

## 8. Risks

  - **`api/` split (W3)** — riskiest single step; fans one package into three
    with new inter-deps. Its own commit, after the simpler moves.
  - **`strip_import_prefix` slip (W1·Spec)** — surfaces as proto
    import-resolution errors; its own commit, verified by W5 conformance.
  - **Mid-phase red build** — inherent to path renames; mitigated by keeping
    sweeps scripted/atomic and parallelising only disjoint file-sets.
  - **`docker`/`cloudbuild` ambiguity** — resolve before deleting (§5.1).

## 9. Direction: compiler-to-wasm → full multi-language bindings

The end-state the layout is built toward (not part of this restructure, but it
explains the shape): **compile the compiler itself to wasm.** Two wasm
artifacts then exist —

  - `cel_runtime.wasm` — the evaluator kernel (already shipping).
  - `compiler.wasm` — `compiler/` (frontend + cel-cpp parser/checker + codegen
    + Binaryen) cross-compiled to `wasm32-wasi`, exposing a small C/wasi entry
    (`compile(source, env) → {wasm_bytes, cel.abi}`).

With both, **any host that has a wasm runtime gets the full pipeline** — a
TS or Go app loads `compiler.wasm` to turn CEL source into a program, then
loads `cel_runtime.wasm` (+ the program) to evaluate, all in-process, with no
native C++ toolchain. That is why `bindings/` is framed as embedding wasm
artifacts rather than as eval-only language ports.

What this restructure does to keep the door open:

  - **`compiler/` is a clean, self-contained unit** with no `eval/`-side or
    host-only (wasmtime) dependency — so a `wasm32-wasi` build target can be
    added later without untangling it. `eval/` (wasmtime) is the C++ host's
    evaluator; other bindings bring their own wasm host (JS `WebAssembly`,
    Go `wazero`, …), so wasmtime never leaks into `compiler/` or `common/`.
  - **`common/` and `abi/` are the cross-binding contract** — the type
    vocabulary and the `cel.abi` wire format are what every binding speaks;
    keeping them dep-light keeps them wasm-portable.

Open questions deferred to the dedicated `compiler.wasm` design doc (future):
Binaryen's footprint under `wasm32-wasi`, the compiler's C entry ABI, and
whether the parser/checker portion of cel-cpp builds clean for wasm32.

## 10. Out of scope / future

  - **Rename the module** `cel-spec` → e.g. `celwasmc` — the real "disconnect
    from parent"; own pass once the move settles.
  - **First TS/Go binding** under `bindings/` — slot reserved, no code now.
  - **Collapse the doubled `conformance` in `spec/proto/cel/expr/conformance/`**
    — inherited package layout; flattening changes proto package names. Skip.
  - **Docs refactor (separate, sizeable, AFTER this restructure).** This move
    only path-rewrites docs and rewrites CLAUDE.md + root README (W1·Docs,
    W4·Conventions) — it does **not** reorganize the doc tree itself. A real
    `doc/` refactor is needed and is its own effort: the
    `doc/implementation-plan/rewrite/` milestone sprawl, archived/superseded
    designs, the `compiler_v2`-shaped narrative throughout, and stale
    cross-references all want restructuring to match the new repo shape. Out
    of scope here; tracked as the next docs workstream.
