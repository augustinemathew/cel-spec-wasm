# Repo restructure: making the compiler the repo

Status: shipped (W0–W7) 2026-05-25.  W7 (disconnect from cel-spec) landed — module is now `cel-wasm`, local proto/ consumed from upstream BCR; conformance held at 1898.

**What landed (as-built deltas from the as-written plan).** The restructure
executed and is green; `compiler_v2/` is dissolved into the top-level role dirs
below. Four decisions diverged from the original draft (resolved in
`repo-restructure-questions.md`, cited inline):

  - **`proto/` move DEFERRED (Q5).** Vendored cel-cpp pins
    `@com_google_cel_spec//proto/cel/*` → our root `//proto/cel`; moving `proto/`
    breaks the build with no in-scope fix. `tests/` → `spec/tests/` shipped;
    `proto/` STAYS at root, folded into the future module-rename workstream (§10).
  - **Shared package is `shared/`, NOT `common/` (Q9).** Our `common/type.h`
    (`celwasm::CelType`) collided with vendored cel-cpp's `common/type.h`
    (`cel::Type`) in any TU pulling both. Renamed `common/` → `shared/`
    (`//shared:type`); the cel-cpp `cel::Type` users keep its `common/`.
  - **Visibility is a 2-tier `//:internal` `package_group`, NOT strict
    `compiler ⊥ eval` (Q8).** The real dep graph has legitimate first-party
    cross-component edges (`eval` → `compiler/ir:annotations`; `abi` →
    `compiler/{codegen,frontend,ir}`), so component-scoping failed analysis.
    Public is still the curated `//visibility:public` list; everything else is
    the root `//:internal` `package_group` (first-party-wide, not external).
  - **Lint gate scoped to the working set, not a full `--branch` burndown
    (Q10).** The 244-file move makes `lint.sh --branch` re-lint the whole tree
    and surface the *pre-existing* lint-backlog; the restructure introduced no
    new warning categories. The full `--branch` clean is a backlog task, not a
    restructure gate.

W6 (the `celwasm::api` → `celwasm` namespace flatten, §7) is the committed
immediate follow-on, not yet run.

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
    `CelType` graduates to our shared package (`shared/`, renamed from
    `common/` per Q9 to avoid the cel-cpp `common/` collision).

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
├── shared/              CelType — shared type vocabulary (cel-cpp precedent;
│                        named shared/ not common/ — Q9 collision avoidance)
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
│   └── tests/simple/testdata/*.textproto
│   # proto/ STAYS at root for now — DEFERRED (Q5): vendored cel-cpp pins
│   # @com_google_cel_spec//proto/cel/*, which resolves to our root //proto/cel;
│   # the move is folded into the module-rename workstream (§10).
│
└── doc/  scripts/  third_party/   unchanged
```

The public/private boundary is **`internal/` + Bazel `visibility`**, the
Abseil/cel-cpp convention — not a separate header tree. `compiler/` and
`eval/` both depend on `shared/`; neither depends on the other. `bindings/`
never reimplements the compiler in-language — it embeds the **wasm
artifacts** (`cel_runtime.wasm` today; `compiler.wasm` once §9 lands) and
drives them through the host's own wasm runtime.

## 4. Disposition of every directory

| Old | New | Action |
|---|---|---|
| `compiler_v2/frontend ir codegen celfn` | `compiler/{frontend,ir,codegen,celfn}` | move |
| `compiler_v2/compile.{h,cc,_test.cc}` | `compiler/internal/compile.*` | move |
| `compiler_v2/api/compiler.*`, `program.h` | `compiler/` (public) | move + repoint |
| `compiler_v2/api/type.*` | `shared/type.*` | move (shared; renamed common→shared, Q9) |
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
`mv tests spec/tests`; rewrite `//tests/simple:` → `//spec/tests/simple:`.

> **Plan-vs-execution delta (Q5, 2026-05-25): the `proto/` move is DEFERRED.**
> Vendored `third_party/cel-cpp` hardcodes `@com_google_cel_spec//proto/cel/*`
> in ~195 BUILD sites; our `module(name="cel-spec")` + `local_path_override` of
> cel-cpp makes that resolve to our root `//proto/cel`, which we build via
> cel-cpp's parser/checker. Moving `proto/` breaks the build and every fix is
> out of scope (editing cel-cpp is forbidden; `local_path_override` takes no
> patch; an alias-shim doesn't actually shed `proto/` and adds unrequested
> debt). The move is the natural tail of the module rename (§10): once we stop
> being the `cel-spec` module, cel-cpp consumes upstream cel-spec protos and our
> local `proto/` relocates to `spec/proto/` cleanly. So `tests/` moves now;
> `proto/` stays at root with `strip_import_prefix = "/proto"` unchanged until
> the rename. See questions-log Q5.

### 5.3 `compiler_v2/` strip + role grouping
`git mv` each child to its §3 home, then repo-wide rewrite of
`compiler_v2/<pkg>` → new path across all `BUILD.bazel` + `#include`s. Do this
as **one scripted pass** (it is deterministic; partial application breaks the
build everywhere). Move `api/` intact to a temporary top-level `api/` in this
pass so `compiler_v2/` disappears and the tree stays green — the split is 5.4.

### 5.4 `api/` split (the judgment step)
Repoint each `//api:foo` dependent to the new owner: `compiler` /
(`compiler`, `program`) → `compiler/`; `type` → `shared/`; `engine`,
`instance`, `activation`, `value`, `error`, `attribute`, `cel_host`,
`internal/*` → `eval/`. Fold `host/` into `eval/host/`. New inter-package
deps: `compiler` → `shared`, `eval` → `shared` + `runtime` + `abi`. No
`compiler` ↔ `eval` *public-API* edge — but as-built (Q8) there ARE
first-party internal edges (`eval` → `compiler/ir:annotations`; `abi` →
`compiler/{codegen,frontend,ir}`), which is why the visibility regime below
is the `//:internal` package_group, not strict component-scoping.

### 5.5 Visibility regime — the enforced public/internal boundary

Today **18 of 19 packages default to `//compiler_v2:__subpackages__`** — every
target is visible across the whole tree, so there is no public/internal
distinction and nothing stops one package depending on another's guts. After
dissolution `//compiler_v2:__subpackages__` is meaningless. This is the moment
to install a real boundary; it is **not** a prefix-swap (a per-package decision,
done in W3). The rule we want: **nobody can take a dependency on a non-public
target** — Bazel `visibility` enforces this at analysis time (an out-of-scope
`deps` edge fails the build), so it is the mechanism, not a convention.

> **Plan-vs-execution delta (Q8, 2026-05-25).** The as-written regime below was
> strict component-scoping (`compiler/**` → `//compiler:__subpackages__`,
> `eval/**` → `//eval:__subpackages__`), justified by "`compiler ⊥ eval`, all
> cross-component edges land on public targets." That **failed analysis**: the
> real dep graph has legitimate first-party cross-component edges into the
> compiler internals — `eval` → `compiler/ir:annotations` (abi_decode/instance
> decode the IR annotation contract) and `compiler/internal:compile`
> (abi_decode_test); `abi` (the `cel.abi` emit side) → `compiler/{codegen,
> frontend,ir}`. The compiler internals are consumed first-party-wide, not
> within `//compiler`. So the **as-built** regime is the 2-tier model below; the
> original component-scoped paragraphs are retained after it, marked, for the
> reasoning trail.

**As-built regime (2-tier: public API vs first-party `//:internal`):**

  - **Public API** — a curated, small set carrying explicit
    `//visibility:public`. Exactly these (unchanged from the plan except
    `common→shared`):
      - `//compiler:compiler`, `//compiler:program`
      - `//eval:engine`, `//eval:instance`, `//eval:activation`,
        `//eval:value`, `//eval:error`, `//eval:attribute`
      - `//shared:type`
      - the shared contract: `//abi:*` and `//runtime:*` — public because
        every binding speaks them.
    Adding `//visibility:public` to anything else is a reviewable event. This
    is what `bindings/` and any external consumer may depend on.

  - **First-party internal** — a `package_group(name = "internal", …)` in the
    root `BUILD.bazel` listing every first-party package (`//compiler/...`,
    `//eval/...`, `//shared/...`, `//abi/...`, `//runtime/...`, `//tools/...`,
    `//conformance/...`, `//e2e/...`, `//bench/...`, `//testdata/...`,
    `//spec/...` — NOT a future `//bindings/...`). The compiler pipeline
    components (`frontend`, `ir`, `codegen`, `celfn`, `compiler/internal`) get
    `default_visibility = ["//:internal"]`: reachable by any first-party
    package, NOT by `bindings/` or an external consumer.

  - **eval internals stay component-scoped.** `cel_host*`, `abi_decode`,
    `instance_impl`, `wasmtime_engine_state`, `cel_host_wasmtime`,
    `host_callback` are consumed only within `eval/` (verified), so they keep
    `//eval:__subpackages__` — narrower than `//:internal`.

  - **`internal/` is the readability signal** that pairs with the visibility
    scope; tests/testdata get test visibility as needed (`//visibility:public`
    is acceptable for `testonly` fixtures).

This still satisfies "nobody external can depend on a non-public target": a
hypothetical `//bindings` package is outside `//:internal`, so it can reach the
curated public list and nothing else. The W5 audit asserts that nothing under
`//compiler/{frontend,ir,codegen,celfn,internal}` or the eval internals is
visible to such a package, and that the public set equals the curated list.

> **(Plan, superseded by Q8 — retained for the reasoning trail.)**
> The regime as originally drafted (default-private, curated-public):
> - *Default is the narrowest scope, never public.* `compiler/**` →
>   `["//compiler:__subpackages__"]`; `eval/**` → `["//eval:__subpackages__"]`,
>   on the premise that `eval/` never reaches into `compiler/`'s guts and
>   vice-versa.
> - Curated public surface as above.
> - `internal/` as belt-and-suspenders (physically `internal/` AND
>   component-scoped).
> This was discarded because the premise — no cross-component edge into compiler
> internals — was false (Q8); strict component-scoping fails the real build.

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
| **W3** | serial, scripted | `git mv` per §4 + apply mapping + split `api/` into `compiler`/`shared`/`eval` (Q9) + set `visibility` | §5.3–5.5 |
| **W4** | ∥ ×N | per-package verifiers (`bazel test //<pkg>/...` + straggler fixups) **+ W4·Conventions** (CLAUDE.md + root README rewrite, post-W3) | — |
| **W5** | serial | **exit-criteria gate** (below) → merge to master | — |
| **W6** | serial, scripted | flatten `celwasm::api` → `celwasm`; re-run exit criteria | §7 |

Peak parallelism is 10 agents in W4 (9 verifiers + Conventions); the serial
spine (W0→W3→W5→W6) is unavoidable because each step re-points the build globally.
Every agent edits only its assigned file-set, lands green-or-isolated, and
reports the exact paths it touched for merge triage.

**Exit criteria (W5).** A path-only move must regress nothing — so W5 is binding
and goes beyond a green `bazel test $PROJ`. (`$PROJ` = the project-package set,
not `//...` — the vendored cel-cpp `tools/testdata/BUILD` references an undeclared
`@com_github_google_flatbuffers`, so `//...` fails to *load*; see execution-doc
§1.0.):

  - `bazel test $PROJ` green (all unit/component/e2e).
  - **Manual-tagged tests run explicitly** — `bazel test $PROJ` skips them, but
    they carry the load-bearing e2e assertions (CLAUDE.md); run every
    `attr(tags, manual, $PROJ)` target, cross-checked against the catalog in
    `per-component-test-coverage.md`.
  - **Conformance pass count == the master baseline** (1898) captured in W0
    (path-only ⇒ identical; any drop is a label/prefix slip).
  - **Benchmarks build** under `$PROJ` and a representative bench **runs** under
    `-c opt`.
  - `scripts/lint.sh --branch` clean; `bazel query 'kind(go_.*, $PROJ)'` empty;
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
    Go `wazero`, …), so wasmtime never leaks into `compiler/` or `shared/`.
  - **`shared/` and `abi/` are the cross-binding contract** — the type
    vocabulary and the `cel.abi` wire format are what every binding speaks;
    keeping them dep-light keeps them wasm-portable.

Open questions deferred to the dedicated `compiler.wasm` design doc (future):
Binaryen's footprint under `wasm32-wasi`, the compiler's C entry ABI, and
whether the parser/checker portion of cel-cpp builds clean for wasm32.

## 10. Out of scope / future

  - **Rename the module** `cel-spec` → e.g. `cel-wasm` — the real "disconnect
    from parent"; own pass once the move settles. **This unblocks the `proto/`
    → `spec/proto/` move (Q5):** today vendored cel-cpp resolves
    `@com_google_cel_spec//proto/cel/*` to our root `//proto/cel`, so `proto/`
    cannot move while we remain the `cel-spec` module. Post-rename, cel-cpp
    consumes the upstream cel-spec protos (BCR) and our local `proto/` relocates
    to `spec/proto/` cleanly. Until then `proto/` stays at root (the `tests/`
    half of the heritage move shipped; the `proto/` half rides with this rename).
  - **First TS/Go binding** under `bindings/` — slot reserved, no code now.
  - **Collapse the doubled `conformance` in `proto/cel/expr/conformance/`**
    — inherited package layout; flattening changes proto package names. Skip.
    (Path stays under root `proto/` until the module rename moves it to
    `spec/proto/` — Q5.)
  - **Docs refactor (separate, sizeable, AFTER this restructure).** This move
    only path-rewrites docs and rewrites CLAUDE.md + root README (W1·Docs,
    W4·Conventions) — it does **not** reorganize the doc tree itself. A real
    `doc/` refactor is needed and is its own effort: the
    `doc/implementation-plan/rewrite/` milestone sprawl, archived/superseded
    designs, the `compiler_v2`-shaped narrative throughout, and stale
    cross-references all want restructuring to match the new repo shape. Out
    of scope here; tracked as the next docs workstream. **This includes the
    residual `compiler_v2/` tail (Q6)** the W1·Docs mechanical sweep left in the
    historical milestone/review/probe records (~40 files): rewriting them now
    would falsify the record (e.g. a review whose finding IS "compiler_v2/
    functions is drift"), so they're reconciled here, holistically, not chased
    mid-restructure.

## 11. Future work (surfaced during execution)

Open follow-ups the restructure surfaced or deferred, so a reader sees what's
done AND what's still open without reading the git log:

  - **`proto/` move + disconnect from cel-spec (Q5)** — PROMOTED to an active
    workstream **W7 (§12)** at the user's request (2026-05-25): rename our module
    off `cel-spec` so cel-cpp consumes upstream cel-spec from BCR, unpinning our
    local `proto/`.
  - **W6 namespace flatten** `celwasm::api` → `celwasm` (§7) — SHIPPED 2026-05-25
    (commit `c0ec349`): 78 files, build green, 83 tests, conformance 1898.
  - **Full lint-backlog burndown (Q10)** — the 244-file move surfaced the
    pre-existing `lint.sh --branch` backlog (braces-around-statements in
    `var_parser.cc`/`cel_runtime.c`; the wasmtime-edge clang-tidy config
    limitation). The restructure introduced no new categories; clearing the
    backlog is a separate task (`doc/implementation-plan/lint-backlog.md`).
  - **Docs refactor (Q6, §10)** — the doc-tree reorganisation and the residual
    historical `compiler_v2/` tail (~40 files); its own workstream.
  - **First `bindings/` (TS/Go)** — slot reserved, no code; will embed
    `cel_runtime.wasm` (and `compiler.wasm` once §9 lands).

## 12. W7 — Disconnect from cel-spec (module rename + proto)

Requested 2026-05-25. The real "disconnect from parent": stop **being** the
`cel-spec` module and instead **consume** cel-spec as a normal dependency. This
is what unpins our local `proto/` (Q5).

**Why `proto/` is pinned today.** cel-cpp (vendored, `local_path_override`)
declares `bazel_dep(name = "cel-spec", version = "0.25.1", repo_name =
"com_google_cel_spec")`. Our root module is *also* `module(name = "cel-spec")`,
and bzlmod's root-wins rule makes cel-cpp's dep resolve to **us**, so
`@com_google_cel_spec//proto/cel/*` (≈195 refs in cel-cpp BUILDs) points at our
`//proto/cel`. We can't edit cel-cpp and can't move `proto/` while we answer to
that name.

**The disconnect.**
  1. Rename the module: `module(name = "cel-spec")` → `module(name = "cel-wasm")`.
  2. Add our own `bazel_dep(name = "cel-spec", version = "0.25.1", repo_name =
     "com_google_cel_spec")`. Now `@com_google_cel_spec` resolves to the **real
     upstream cel-spec 0.25.1 from BCR** for both cel-cpp and us.
  3. Repoint our own `//proto/cel...` consumers (conformance, testdata,
     binding_marshal — the non-third_party referrers) to
     `@com_google_cel_spec//proto/cel...`.
  4. Delete the local `proto/` (now redundant — upstream provides it) **or** keep
     a curated local copy under `spec/proto/`; consuming upstream is the cleaner
     disconnect.
  5. Update `MODULE.bazel` overrides / `WORKSPACE*` that assume the `cel-spec`
     module identity.
  6. **Remove cel-spec heritage remnants (DONE, commit `0504718`).** The
     parent-repo governance + BCR-publish machinery we no longer need:
     `.bcr/` (metadata/presubmit/source templates + README), the
     `.github/workflows/publish_to_bcr.yml` publish workflow, and the root
     `CODE_OF_CONDUCT.md` / `CONTRIBUTING.md` / `GOVERNANCE.md` (cel-spec
     governance; our compiler dev-workflow stays in `doc/contributing.md`).
     `MAINTAINERS.md` + `LICENSE` left in place. No build/script referenced
     any of these. Landed on `master` directly — independent of, and not gated
     by, the riskier module-rename (steps 1–5).

**The risk (and the gate).** Upstream BCR cel-spec 0.25.1 must expose the same
proto/target layout our code + cel-cpp expect, and the corpus/protos must not
drift from our fork — so **conformance must stay 1898 and the build green**.
That is a *semantic* change (dependency topology), unlike the path-only moves of
W0–W6: if 0.25.1 differs from our fork in a way that moves conformance, the
change wants user review. Execution therefore runs on a branch with the 1898
gate as the hard stop — land only if green; otherwise hold on the branch and
report findings. See the W7 brief in `repo-restructure-execution.md`.
