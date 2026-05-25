# Repo restructure — running questions log

Autonomous-execution scratchpad. Each question is resolved in-place (by
inspection or by asking a sub-agent), never escalated to the user. Format:

```
### Q<n> — <one-line> [OPEN|RESOLVED <date>]
Context: …
Resolution: …
```

---

### Q8 — visibility regime: first-party `//:internal` group, not compiler⊥eval [RESOLVED 2026-05-25 — DESIGN REFINEMENT]
Context (W3 gate): the design §5.4/§5.5 "compiler ⊥ eval, only public-contract
edges" model FAILED the build. The real dep graph has legitimate cross-component
edges into the compiler internals:
  - `eval` → `compiler/ir:annotations` (abi_decode/instance decode the IR
    annotation contract) + `compiler/internal:compile` (abi_decode_test).
  - `abi` (the cel.abi emit side) → `compiler/{codegen,frontend,ir}`.
  - `compiler/frontend` consumed by abi, bench, conformance, tools/cel.
  - `compiler/ir` consumed by abi, eval, + intra-compiler.
So the compiler internals are consumed FIRST-PARTY-WIDE, not within //compiler.
Strict component-scoping is simply wrong for this codebase.
Resolution: implement the 2-tier model the user actually asked for ("internal vs
public"):
  - **Public API** (//visibility:public): compiler:{compiler,program};
    eval:{engine,instance,activation,value,error,attribute}; common:type;
    abi:* ; runtime:* . This is what bindings/external consume.
  - **First-party internal** (`//:internal` package_group in the new root
    BUILD.bazel, listing every first-party package: compiler, eval, common, abi,
    runtime, tools, conformance, e2e, bench, testdata, spec — NOT bindings/):
    the compiler components (frontend, ir, codegen, celfn, internal) get
    `default_visibility=["//:internal"]`. Reachable by any first-party package,
    NOT by a future bindings/ or external consumer.
  - eval internal targets (cel_host*, instance_impl, …) STAY //eval:__subpackages__
    (only eval consumes them — verified).
This still satisfies "nobody external can depend on non-public things" (the
user's stated goal) while permitting the real intra-project wiring. Design
§5.4/§5.5 + curated-list updated to describe the package_group, not compiler⊥eval.
W5 audit: assert nothing under `//compiler/{frontend,ir,codegen,celfn,internal}`
or eval-internal is visible to a hypothetical //bindings package.

### Q7 — internal/ as package vs subdir; api/BUILD split shape [RESOLVED 2026-05-25]
Context (W3): design §5.5 leans on `//compiler/internal` + `//eval/internal` as
real packages (belt-and-suspenders + the W5 `rdeps(//eval/internal/...)` audit).
But api/internal/ today is a SUBDIR of the single api package (targets define
`srcs=["internal/abi_decode.cc"]`), not its own package. Making eval/internal a
package means moving ~8 target defs + repointing intra-eval `:foo`→`//eval/internal:foo`
= more edit surface/risk.
Resolution:
  - **compiler/internal IS a package** — `compile.{h,cc,_test.cc}` (today
    `//compiler_v2:compile` at the compiler_v2 root) moves to `compiler/internal/`
    with its own BUILD (`//compiler/internal:compile`). Clean single target; the
    W3 brief already mandates this label.
  - **eval/internal stays a SUBDIR of the eval package** — the ~8 internal targets
    (abi_decode, cel_host*, instance_impl, wasmtime_engine_state, cel_host_wasmtime)
    are defined in `eval/BUILD` with `srcs=["internal/…"]`, exactly as api/BUILD
    does today. They get the eval package's component-scoped default_visibility
    (NOT public), so external packages cannot depend on them — the boundary is
    enforced by VISIBILITY, which is directory-independent. The physical
    `internal/` subdir is preserved as the readability signal.
  - W5 AUDIT adjustment: the primary guarantee is "public-target set == curated
    list" (step 5b) — that already proves nothing internal is reachable. The
    `rdeps(//eval/internal/...)` query (step 5a) is replaced by an explicit
    enumeration of eval's non-public targets: `rdeps($PROJ, <eval internal
    targets>) except //eval:*` → empty. compiler/internal keeps the package-form
    query.

### Q6 — Doc residual `compiler_v2/` tail is historical record → defer to docs-refactor [RESOLVED 2026-05-25]
Context (W1·Docs): after the mechanical clean-mapping bulk rewrite of doc/**/*.md
(committed), ~180 `compiler_v2/` tokens remain across ~40 files. Inspection shows
the tail is dominated by HISTORICAL records, not live path errors:
  - `compiler_v2/probes/…` (33) — the dir is DELETED; these are past-tense "we
    built probe X" milestone notes.
  - `compiler_v2/cli` (25), `compiler_v2/functions` (9) — neither dir EXISTS even
    pre-restructure (CLI is tools/cel; fns are celfn). They're stale refs inside
    historical docs; `m13-reviews/2026-05-21-pre-slice-c.md` literally documents
    "compiler_v2/functions is drift" AS A REVIEW FINDING — rewriting destroys it.
  - bare `compiler_v2/` (81) — generic "the compiler_v2 tree" prose in milestone
    M1–M20 notes / reviews, written when the code lived there.
Resolution: The mechanical path-rewrite IS the W1·Docs deliverable (done). The
remaining tail belongs to the **docs-refactor workstream the user already scoped
as a separate future effort** (design §10) — and CLAUDE.md's own "Closing out a
planning doc" rule keeps old text + marks deltas rather than rewriting history.
So: do NOT chase the historical tail now.
GATE RECONCILIATION: W5 step 8 is scoped to **code/BUILD/scripts clean of
compiler_v2** (the hard correctness gate; docs don't affect the build). Docs may
retain `compiler_v2` — the full doc-tree reconciliation is the deferred
docs-refactor. The grep gate explicitly excludes doc/**.

### Q5 — `proto/` move breaks vendored cel-cpp (blocked on module rename) [RESOLVED 2026-05-25 — DESIGN CHANGE]
Context (W1·Spec): vendored `third_party/cel-cpp` hardcodes
`@com_google_cel_spec//proto/cel/expr:*` in ~195 BUILD sites (e.g.
`parser/BUILD:71`). Our root module is `module(name = "cel-spec")` and cel-cpp is
wired via `local_path_override(module_name="cel-cpp")` with
`bazel_dep(name="cel-spec", repo_name="com_google_cel_spec")`, so
`@com_google_cel_spec//proto/cel/expr` resolves to **our root `//proto/cel/expr`**.
We build cel-cpp's `parser`/`checker`/`common` libs, so moving `proto/` →
`spec/proto/` makes those deps dangle → build RED. The W1·Spec agent correctly
refused to commit and reverted to W0.

Why every in-scope fix is bad:
  - Edit cel-cpp's 195 refs → forbidden (CLAUDE.md: don't edit third_party/cel-cpp).
  - Patch via override → `local_path_override` takes no `patches`; would require
    switching cel-cpp to an archive/git override = out-of-scope infra change.
  - Alias-shim packages at `//proto/cel/**` → does NOT shed `proto/` from root
    (the packages still exist), adds a ~20-target compat layer nobody asked for,
    and is exactly the "temporary shim that becomes load-bearing" debt CLAUDE.md
    warns against.

The real enabler is the **module rename / "disconnect from parent"**, which the
design ALREADY scopes to future work (design §10): once our module stops being
`cel-spec`, cel-cpp would consume the real upstream cel-spec protos (from BCR)
and our local protos move to `spec/proto/` cleanly with no cel-cpp coupling.

RESOLUTION (design change, applied to the plan):
  - **`tests/` → `spec/tests/` proceeds now** — SAFE: only consumer is
    `compiler_v2/conformance/BUILD` (→ `//spec/tests/simple`); cel-cpp's own
    conformance/BUILD references `cel_spec//tests` but we never build it.
  - **`proto/` stays at root `//proto/cel/**` for now** — its move is folded into
    the module-rename workstream (design §10). Documented in design §3/§5.2/§8/§10
    and execution §1.1/§1.4/W1·Spec. Reversible, zero third_party edits, green.
  - FLAG FOR OWNER on return: if you want `proto/` under `spec/` before the module
    rename, the only green path is the alias-shim (call it explicitly and I'll do
    it); otherwise it lands free with the rename.

### Q4 — git worktree isolation unavailable in this environment [RESOLVED 2026-05-25]
Context: plan assumes parallel agents run in `isolation: worktree`. The Agent
tool errors: "Cannot create agent worktree: not in a git repository and no
WorktreeCreate hooks are configured" (cwd is .../cel2/compiler_v2; harness
worktree creation isn't wired here).
Resolution: Drop worktree-based parallelism. All agents run NON-isolated on the
`restructure` branch in the main checkout, committing directly. Disjoint-file
waves (W1 Spec/Docs, W4 per-package verifiers) therefore run SEQUENTIALLY, not
concurrently (concurrent same-tree agents would race the git index). No
correctness impact and negligible time cost — the serial spine (W0→W3→W5→W6)
was the critical path regardless; W1/W4 work is light vs. the W3/W4 bazel
builds. The wave ORDER and gates are unchanged.

### Q3 — Literal wildcard `//compiler_v2/...` in scripts not handled by rewrite [RESOLVED 2026-05-25]
Context: `run_full_suite.sh` (`bazel test //compiler_v2/...`), `build_lint_pch.sh`
(`bazel build //compiler_v2/... //compiler_v2/bench:kernel_bench`),
`refresh_compile_db.sh` (aquery `//compiler_v2/... + //compiler_v2/...`, and a
`bazel build` of the same) use the 3-dot wildcard. The W0 rewrite script does
specific-prefix replacement (`//compiler_v2/frontend`→…), so the bare wildcard
matches NO rule and would be left dangling after compiler_v2/ is gone.
Resolution: NOT fixable by one sed — the wildcard appears space-separated (build/
test command lines) AND `+`-joined (aquery/query strings); those need different
expansions of $PROJ (Q2). W3 hand-converts each: command lines → space-joined
$PROJ, query strings → `+`-joined $PROJ. Documented in the rewrite-script header
+ W3 brief + §1.5. Safety nets: W3 grep gate ("no compiler_v2 in scripts") and
W5 actually running run_full_suite/refresh_compile_db/lint.

### Q2 — `bazel ... //...` is unusable (third_party load error) [RESOLVED 2026-05-25]
Context: W0 agent found `bazel query '//...'` fails: vendored
`third_party/cel-cpp/tools/testdata/BUILD` loads `@com_github_google_flatbuffers`,
a repo not declared in our `MODULE.bazel` (cel-cpp BUILD we don't use). So every
plan gate written as `bazel build //...` / `bazel query ... //...` dies on a
package-loading error before evaluating anything — a false RED that also hides
real failures.
Resolution: Replace `//...` in all build/test/query GATES with an explicit
PROJECT-PACKAGE SET (the repo's own top-level packages, never `third_party`):
```
//compiler/... //eval/... //shared/... //abi/... //runtime/... \
//tools/... //conformance/... //e2e/... //bench/... //testdata/... //spec/...
```
(Pre-restructure equivalent is `//compiler_v2/... //proto/... //tests/...`.)
For `rdeps(universe, …)` / `visible(…)` queries the universe is this set, not
`//...`. Encoded into the execution doc §1.0 + W3/W5/W6 gates. The W0 baseline
queries already scoped to `//compiler_v2/...` for the same reason — correct.

### Q1 — Does `docker/` + `cloudbuild.yaml` belong to compiler CI or Go-regen only? [RESOLVED 2026-05-25]
Context: design §5.1 / W0 step 4 — delete iff not referenced by compiler CI.
Resolution: KEEP BOTH. `docker/Dockerfile` self-documents as "Linux build
environment for celwasmc — the CEL → WebAssembly AOT compiler" (host C++
toolchain for the compiler build, not Go). `cloudbuild.yaml` runs `bazel build
'...'` (compiler build CI). Neither is Go-regen. The only `.github/workflow`
is `publish_to_bcr.yml` (BCR module publish — cel-spec heritage, unrelated to
docker/cloudbuild). cloudbuild uses `'...'` (no hardcoded path) so it needs no
rewrite after the move. Note: baseline conformance pass count = **1898** (from
`compiler_v2/conformance/.baseline`).
