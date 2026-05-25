# Repo restructure — parallel execution plan

Status: ready to execute. Companion to `repo-restructure.md` (the design).
This doc is the **operational** plan: the canonical path/label mapping every
agent shares, copy-paste agent briefs, and the wave/merge procedure.

## 0. Execution model

A path rename breaks every dependent label until updated, so **the build is
green only at wave boundaries**, never mid-wave. Parallelism is extracted two
ways:

  1. **Disjoint file-sets** — agents in the same wave touch non-overlapping
     files, so their branches merge conflict-free.
  2. **A frozen mapping table (§1)** — every rewrite is a pure function of the
     mapping. Once §1 is fixed, any file can be rewritten independently; no
     agent needs another agent's output.

Mechanics: each parallel agent runs in its **own git worktree**
(`isolation: worktree`) branched off the integration branch, lands its slice
**green-or-isolated**, and reports the exact paths it touched. The integrator
(you, or a serial agent) merges in the stated order. Serial waves (W0, W3, W5)
run on the integration branch directly.

Integration branch: `git switch -c restructure master`. All waves target it;
merge to `master` only after W5 is green.

**Git hooks during the waves — turned OFF for the duration.** The only hook is
`.githooks/pre-push` (no pre-commit), which runs the conformance gate +
README-drift check — both path-coupled and unable to pass mid-restructure. So
**W0 disables hooks** (`git config --unset core.hooksPath`) and pushes during
the work skip the gate cleanly. The hook's *scripts* are still path-fixed in
W3 (§1.5) so the hook works once re-enabled. **W6 (final) re-enables**
(`scripts/install-hooks.sh`, i.e. `core.hooksPath → .githooks`) and confirms a
real push passes the gate — proving the corrected hook works, not just that it
was bypassed.

## 1. Canonical mapping (FROZEN — the shared contract)

Authored once in W0; **do not deviate**. Every later agent rewrites strictly
per this table.

### 1.0 The PROJECT-PACKAGE SET (use this, never `//...`)

`bazel … //...` is **unusable in this repo**: the vendored
`third_party/cel-cpp/tools/testdata/BUILD` loads `@com_github_google_flatbuffers`,
a repo not declared in our `MODULE.bazel`, so `//...` dies on a package-loading
error before evaluating anything (a false RED that also masks real failures).
Every build/test/query gate therefore targets the repo's own top-level packages
explicitly. **Post-restructure project-package set** (`$PROJ`):

```
//compiler/... //eval/... //shared/... //abi/... //runtime/... \
//tools/... //conformance/... //e2e/... //bench/... //testdata/... //spec/...
```

Pre-restructure equivalent (W0): `//compiler_v2/... //proto/... //tests/...`.
For `rdeps(universe, …)` / `visible(…)` queries the *universe* is `$PROJ`, not
`//...`. (See questions-log Q2.)

### 1.1 Directory / file moves

| Old | New |
|---|---|
| `compiler_v2/frontend/` | `compiler/frontend/` |
| `compiler_v2/ir/` | `compiler/ir/` |
| `compiler_v2/codegen/` | `compiler/codegen/` |
| `compiler_v2/celfn/` | `compiler/celfn/` |
| `compiler_v2/compile.{h,cc,_test.cc}` | `compiler/internal/compile.{h,cc,_test.cc}` |
| `compiler_v2/api/compiler.{h,cc}`, `program.h` | `compiler/compiler.{h,cc}`, `compiler/program.h` |
| `compiler_v2/api/type.{h,cc}` (+ `_test.cc`) | `common/type.{h,cc}` |
| `compiler_v2/api/{engine,instance,activation,value,error,attribute}.{h,cc}` | `eval/…` |
| `compiler_v2/api/host_callback.h` | `eval/host_callback.h` |
| `compiler_v2/api/internal/` | `eval/internal/` |
| `compiler_v2/api/cel_pipeline_bench.cc` | `bench/cel_pipeline_bench.cc` |
| `compiler_v2/host/` | `eval/host/` |
| `compiler_v2/abi/` | `abi/` |
| `compiler_v2/runtime/` | `runtime/` |
| `compiler_v2/{tools,conformance,e2e,bench,testdata}/` | `{tools,conformance,e2e,bench,testdata}/` |
| ~~`proto/`~~ | ~~`spec/proto/`~~ — **DEFERRED to the module-rename workstream (Q5, design §10).** Vendored cel-cpp pins `@com_google_cel_spec//proto/cel/*` → our root `//proto/cel`; moving it breaks the build with no in-scope fix. `proto/` STAYS at root for now. |
| `tests/` | `spec/tests/` |

DELETE: `compiler_v2/probes/`, `wasm_compilation_experiments/`, Go surface
(root `*.pb.go`, root `BUILD.bazel`, `go.mod`, `go.sum`, `regen_go_proto*.sh`,
`conformance/*.pb.go`, `conformance/{proto2,proto3,test}/`).

### 1.2 Bazel label rewrites

| Old label prefix | New |
|---|---|
| `//compiler_v2/frontend` `…/ir` `…/codegen` `…/celfn` | `//compiler/frontend` `…/ir` `…/codegen` `…/celfn` |
| `//compiler_v2:compile` | `//compiler/internal:compile` |
| `//compiler_v2/api:compiler`, `:program` | `//compiler:compiler`, `//compiler:program` |
| `//compiler_v2/api:type` | `//common:type` |
| `//compiler_v2/api:engine` `:instance` `:activation` `:value` `:error` `:attribute` `:cel_host` `:host_callback` | `//eval:…` |
| `//compiler_v2/abi` | `//abi` |
| `//compiler_v2/runtime` | `//runtime` |
| `//compiler_v2/{tools,conformance,e2e,bench,testdata}` | `//{tools,conformance,e2e,bench,testdata}` |
| `//compiler_v2/host` | `//eval` (host folds into eval target, or `//eval:host`) |
| ~~`//proto/cel`, `@cel-spec//proto/cel`~~ | ~~`//spec/proto/cel`~~ — **DEFERRED (Q5): `proto/` stays at root, labels unchanged.** |
| `//tests/simple` | `//spec/tests/simple` |

### 1.3 `#include` rewrites

Same prefix logic, applied to `#include "compiler_v2/…"`:

| Old include prefix | New |
|---|---|
| `compiler_v2/frontend/` `…/ir/` `…/codegen/` `…/celfn/` | `compiler/…` |
| `compiler_v2/compile.h` | `compiler/internal/compile.h` |
| `compiler_v2/api/compiler.h`, `…/program.h` | `compiler/compiler.h`, `compiler/program.h` |
| `compiler_v2/api/type.h` | `common/type.h` |
| `compiler_v2/api/{engine,instance,activation,value,error,attribute}.h`, `host_callback.h`, `api/internal/` | `eval/…` |
| `compiler_v2/host/` | `eval/host/` |
| `compiler_v2/abi/`, `…/runtime/`, `…/{tools,conformance,e2e,bench,testdata}/` | strip `compiler_v2/` |

### 1.4 Other rewrites
  - ~~`strip_import_prefix = "/proto"` → `"/spec/proto"`~~ — **DEFERRED with the
    `proto/` move (Q5).** `proto/` stays at root; its `proto_library`s keep
    `strip_import_prefix = "/proto"` unchanged. (Only `tests/` moves in W1.)
  - `namespace celwasm::api` (802 sites) — **out of scope here**; W6 (design §7).
    Leave `celwasm::api` intact in W0–W5.
  - `default_visibility = ["//compiler_v2:__subpackages__"]` (18 of 19 BUILDs)
    is **NOT a prefix swap** — it's the visibility-regime decision (design §5.5),
    applied per-package in W3 step 4: `compiler/**` → `//compiler:__subpackages__`,
    `eval/**` → `//eval:__subpackages__`, and the curated public targets get
    explicit `//visibility:public`. The rewrite script must NOT blindly map
    `//compiler_v2:__subpackages__` to a single new label.

### 1.5 Build / lint / hooks infrastructure (part of the rewrite surface)

These are NOT `.cc/.h/.bazel` and were preserved precisely to keep the
dev-loop fast — the move must not regress them. They hardcode the moving
paths/labels and are rewritten in the same §1.2/§1.3 sweep, so the W0 rewrite
script's file glob **must widen** beyond `*.bazel *.cc *.h *.bzl` to include:

| File | What references the moving paths |
|---|---|
| `scripts/run_full_suite.sh` | hardcoded manual-tagged labels `//compiler_v2/api:…`, `//compiler_v2/e2e:…`, `//compiler_v2/conformance:run_conformance` → `//eval:…`, `//e2e:…`, `//conformance:…` |
| `scripts/check_conformance_monotonic.sh` | `BASELINE_FILE="compiler_v2/conformance/.baseline"`; `//compiler_v2/conformance:run_conformance` |
| `scripts/refresh_compile_db.sh` (+ `_aquery_to_compdb.py`) | `//compiler_v2/...` build/aquery + manual-tag query patterns → `$PROJ` (§1.0; `+`-joined in query strings, space-joined on the build command — Q3) |
| `scripts/lint.sh`, `scripts/build_lint_pch.sh`, `scripts/lint_pch.h` | `compiler_v2/` globs/comments; PCH header set |
| `scripts/regen_conformance_readme.sh`, `scripts/check_doc_drift.sh` | `compiler_v2/conformance/README.md` + doc paths |
| `.clang-tidy`, `.clang-format-ignore` | path-scoped includes/excludes (third_party already excluded) |
| `.githooks/pre-push` | comment refs + invokes the two conformance scripts above |

Plus a **file move**: `compiler_v2/conformance/.baseline` → `conformance/.baseline`
(the checked-in conformance pass count — this *is* the W0 baseline, §3 W0·0).

Derived caches regenerate (not hand-edited): `compile_commands.json` (via
`refresh_compile_db.sh`) and `.lint-cache/lint_pch.h.pch` (via
`build_lint_pch.sh`, which rebuilds when `compile_commands.json` changes).

## 2. Waves

```
W0 ─── W1 ═╗(∥ ×2) ═══ W3 ─── W4 ═╗(∥ ×N) ═══ W5 ─── W6
           ╚══════════           ╚═══════════
        (W2 spec ∥ docs)      (verify + CLAUDE.md)
```

| Wave | Mode | Agents | Gate |
|---|---|---|---|
| W0 | serial | 1 (Foundation) | hooks off; baseline captured; `bazel build //compiler_v2/...` green |
| W1 | ∥ ×2 | Spec, Docs | per-agent (below) |
| W3 | serial | 1 (Sweep) | `bazel build $PROJ` green (§1.0 set, not `//...`); PCH loads; no `compiler_v2` in BUILD/includes/scripts |
| W4 | ∥ ×N+1 | per test package + Conventions (CLAUDE.md) | each `bazel test //<pkg>/...` green |
| W5 | serial | 1 (Gate) | run_full_suite + conformance baseline + benches + dev-loop speed + lint |
| W6 | serial | 1 (Namespace) | flatten `celwasm::api`; re-run W5 gate; re-enable hooks |

(Numbering keeps W2 reserved; W1 contains the two parallel tracks.)

## 3. Agent briefs (copy-paste)

> Every brief: work **only** on your assigned files, follow §1 verbatim, land
> your branch, and end your report with `TOUCHED:` listing every path you
> changed (for merge triage). Do not touch `third_party/`. Do not rename
> namespaces.

### W0 — Foundation [SERIAL]
```
Branch: restructure (create from master).
0. DISABLE HOOKS for the duration: git config --unset core.hooksPath
   (re-enabled in W6). CAPTURE BASELINE (on master, before any change) — W5
   compares against this:
   - conformance pass count: the checked-in compiler_v2/conformance/.baseline
     IS this number; also run scripts/check_conformance_monotonic.sh to confirm.
   - test target count: bazel query 'tests(//compiler_v2/...)' | wc -l.
   - manual-tagged target list: bazel query 'attr(tags,"\bmanual\b",//...)'
     (≈ the labels run_full_suite.sh enumerates).
   Write all three into this doc's "Baseline" note or the W0 commit message.
1. Delete: compiler_v2/probes/, wasm_compilation_experiments/, and the Go
   surface per design §5.1 (root *.pb.go, root BUILD.bazel, go.mod, go.sum,
   regen_go_proto*.sh, conformance/*.pb.go, conformance/{proto2,proto3,test}/).
2. Strip go_library/go_proto_library targets + @io_bazel_rules_go loads +
   importpath="cel.dev/expr…" from the six proto/cel/**/BUILD.bazel files;
   KEEP every proto_library / cc_proto_library.
3. bazel query to confirm rules_go/gazelle are unused; if so drop their
   bazel_dep from MODULE.bazel.
4. Decide docker/ + cloudbuild.yaml: delete iff not referenced by compiler CI
   (grep .github/workflows, scripts/); else leave with a note.
5. Write scripts/restructure_rewrite.sh implementing §1.2 + §1.3 as
   deterministic sed over a WIDE glob (EXCLUDING third_party/):
     git ls-files '*.bazel' '*.bzl' '*.cc' '*.h' \
                  'scripts/*.sh' 'scripts/*.py' scripts/lint_pch.h \
                  .clang-tidy .clang-format-ignore .githooks/pre-push
   per §1.5 — the build/lint/hooks infra hardcodes the moving paths too.
   The api/ fan-out (§1.1) is the one non-prefix case — encode it explicitly.
   Do NOT run it yet; W3 runs it.
GATE: bazel build //compiler_v2/... green (deletions don't break the build).
Commit: "restructure: delete probes/experiments/Go surface; add rewrite script".
```

### W1·Spec — Heritage → spec/ [SERIAL on restructure branch]
```
NOTE: proto/ move DEFERRED (Q5) — only tests/ moves in W1. Worktree isolation
unavailable (Q4) so this runs serially on the restructure branch.
1. git mv tests spec/tests.   (proto/ stays at root — see Q5/§1.1.)
2. §1.2: //tests/simple → //spec/tests/simple across ALL BUILD files
   (the only consumer is compiler_v2/conformance/BUILD).
GATE: bazel build //compiler_v2/conformance/... //compiler_v2/e2e/... green;
      git ls-files tests → empty; no //tests/simple left outside third_party.
DONE in this session by the orchestrator directly (small move).
```

### W1·Docs — Path references in PLAN docs [PARALLEL, worktree]
```
Branch: restructure-docs (from restructure@W0).
Rewrite every compiler_v2/ and tests/ path string to its FINAL §1.1 target
in: doc/**/*.md ONLY (the forward-looking design + milestone plans).
Also fix relative doc links broken by the moves.
DO NOT rewrite proto/ paths — the proto/ move is DEFERRED (Q5); proto/ stays at
root, so proto/cel/… references in docs remain correct as-is.
EXCLUDE: CLAUDE.md and the root README.md — those are AS-BUILT operational
docs that need substantive (not just path) rewrites and must describe a shape
that exists; they are handled post-W3 by W4·Conventions, NOT here. Rewriting
them now would describe a layout that doesn't exist yet.
GATE: none (no build impact). FILES: doc/**/*.md ONLY. Do not touch CLAUDE.md,
root README.md, BUILD, or .cc/.h.
Report TOUCHED.
```
Merge order into `restructure`: Spec, then Docs (disjoint; clean).

### W3 — The sweep [SERIAL, scripted]
```
Branch: restructure (after W1 merged).
1. git mv every directory/file per §1.1 (incl. the api/ fan-out to
   compiler/, common/, eval/, and host/→eval/host/) AND the §1.5 infra move
   git mv compiler_v2/conformance/.baseline conformance/.baseline.
   compiler_v2/ must not exist afterward.
2. Run scripts/restructure_rewrite.sh → applies §1.2 + §1.3 across the WIDE
   glob (BUILD + includes + scripts + configs + pre-push hook, §1.5).
2b. HAND-CONVERT the literal wildcard `//compiler_v2/...` (the rewrite script
   deliberately skips it — Q3) to the §1.0 PROJECT-PACKAGE SET in three scripts:
   - run_full_suite.sh: `bazel test //compiler_v2/...` → `bazel test` + the
     space-joined $PROJ.
   - build_lint_pch.sh: `bazel build //compiler_v2/... //compiler_v2/bench:…`
     → space-joined $PROJ (the `:kernel_bench` target is already covered by
     `//bench/...`; drop the now-redundant explicit ref or keep it, it resolves).
   - refresh_compile_db.sh: the aquery/query strings use `+`-union — expand to
     the `+`-JOINED $PROJ (`//compiler/... + //eval/... + …`); the `bazel build
     --config=lint //compiler_v2/...` command line uses the space-joined form.
   Prefer introducing a `PROJ="//compiler/... //eval/... …"` shell var at the top
   of each script and referencing it, over inlining the long list repeatedly.
3. Add BUILD.bazel for new packages: common/, eval/, compiler/ (public targets
   compiler/compiler, compiler/program; compiler/internal:compile), splitting
   the old compiler_v2/api/BUILD targets to their new owners.
4. VISIBILITY REGIME (design §5.5) — default-private, curated-public:
   - compiler/** default_visibility = ["//compiler:__subpackages__"];
     eval/** default_visibility = ["//eval:__subpackages__"]
     (replaces the old //compiler_v2:__subpackages__ — NOT a prefix swap).
   - Curated public (explicit //visibility:public, and ONLY these):
     //compiler:compiler, //compiler:program; //eval:{engine,instance,
     activation,value,error,attribute}; //common:type; //abi:* (public) ;
     //runtime:* (wasm artifact + native test lib).
   - internal/ targets stay component-scoped (belt-and-suspenders).
   The boundary is then enforced by `bazel build` itself (a bad deps edge
   fails analysis); the W5 gate audits that public hasn't crept.
5. REBUILD the dev-loop caches (paths moved → both are stale):
   scripts/refresh_compile_db.sh  (regenerates compile_commands.json);
   scripts/build_lint_pch.sh      (rebuilds .lint-cache/lint_pch.h.pch).
GATE: bazel build $PROJ green (§1.0 set, NOT `//...` — that fails to load on
      third_party/flatbuffers); `grep -rn compiler_v2 --include=*.bazel
      --include=*.cc --include=*.h --include=*.sh .` (excl. third_party,
      bazel-*) empty; scripts/lint.sh (a file) runs with PCH LOADED — no
      "'-pch=…' file not found" canary in output (CLAUDE.md: a silent PCH
      miss changes the warning set).
Commit: "restructure: dissolve compiler_v2, split api into compiler/eval/common".
```

### W4 — Verification fan-out [PARALLEL ×N, worktree]
```
One agent per package: runtime · codegen · abi · eval · compiler · conformance
· e2e · tools · bench.
Branch: restructure-verify-<pkg> (from restructure@W3).
1. bazel test //<pkg>/... — must be green.
2. Fix only stragglers the scripted sweep missed: a dropped include, a stale
   `compiler_v2/...` path in a //-comment (update to new path, opportunistic),
   a visibility gap. NO behavior changes.
GATE: //<pkg>/... green. FILES: under <pkg>/ ONLY. Report TOUCHED + test summary.
```
Merge: any order (disjoint packages).

### W4·Conventions — Rewrite CLAUDE.md + root README [PARALLEL, worktree]
```
Branch: restructure-conventions (from restructure@W3 — needs the AS-BUILT shape).
Disjoint from the per-package verifiers (touches only CLAUDE.md + README.md).

MECHANICAL (path/label swaps per §1):
  - //compiler_v2/... → new labels; bazel-bin/compiler_v2/tools/cel →
    bazel-bin/tools/cel; //compiler_v2/testdata → //testdata; the
    `bazel test //compiler_v2/...` / `bazel build //compiler_v2/...` invocations.
  - File-path examples to new homes: compiler_v2/host/cel_log.cc → eval/host/…,
    compiler_v2/codegen/static_memory_builder.cc → compiler/codegen/…,
    compiler_v2/runtime/cel_map_test.cc → runtime/…, compiler_v2/e2e/* → e2e/*,
    compiler_v2/tools/wat_runner → tools/wat_runner.

SUBSTANTIVE (the rules/framing change — not just paths):
  - Intro: describe the role-based layout (compiler/ eval/ runtime/ abi/
    common/) instead of "the new compiler lives under compiler/".
  - Delete the now self-contradictory note "legacy V1 compiler/ tree was
    deleted… fixtures at //compiler_v2/testdata" (compiler_v2 IS compiler/ now);
    state fixtures live at //testdata.
  - Probe workflow (the "Probe vendored cel-cpp" section): probes/ is deleted —
    restate the throwaway-probe home (e.g. compiler/probes/ or a /probes/ at
    root) and drop the m13_custom_fns/optionals examples that no longer exist.
  - ADD: the bazel/-vs-third_party/ rule (design §1 settled decisions).
  - ADD: the visibility regime as a STANDING RULE for contributors (design
    §5.5) — no api/ dir; default_visibility is component-scoped
    (//compiler:__subpackages__, //eval:__subpackages__), public is an
    explicit, curated, reviewed per-target //visibility:public list; you may
    NOT add a deps edge onto another component's internal target, and may NOT
    widen a target to //visibility:public without review. internal/ +
    visibility together are the boundary (Abseil/cel-cpp convention).
  - ADD: compiler/ stays wasm-targetable (no eval/-side or wasmtime dep) so
    compiler.wasm stays reachable (design §9).
  - Header-guard convention CELWASM_<PATH>_H_ is unchanged as a RULE, but the
    guards in moved files regenerate from new paths — that happens in W3 in the
    .h files, not here; just confirm the rule text still reads correctly.

GATE: none (no build impact); self-review that every path resolves in the new
tree. FILES: CLAUDE.md + root README.md ONLY.
Report TOUCHED.
```

### W5 — Exit-criteria gate [SERIAL]
```
Branch: restructure (after W4 merged).
This restructure is PATH-ONLY: nothing that was green on master may regress.
The gate below is the binding exit criteria — ALL must pass before merge.

0. scripts/refresh_compile_db.sh                         (paths moved)
1. scripts/run_full_suite.sh — the canonical milestone-close runner: default
   `bazel test $PROJ` (§1.0 set) PLUS every MANUAL-tagged target (instance/engine/
   cel_host tests, e2e m*/optimize/roundtrip, runtime wasm, wat_runner) PLUS the
   conformance run. `bazel test $PROJ` alone SKIPS the manual targets, which
   carry the load-bearing e2e assertions (CLAUDE.md). Cross-check its label
   list against per-component-test-coverage.md §5.
2. CONFORMANCE baseline — scripts/check_conformance_monotonic.sh: pass count
   EQUAL to conformance/.baseline (the moved baseline, §1.5) and to the W0
   capture (path-only move ⇒ identical; a drop = a label/prefix slip).
3. BENCHMARKS — bench targets build under $PROJ (step 1); then a representative
   run under -c opt to confirm they still execute:
     bazel run -c opt //bench:<a_pipeline_bench> -- --benchmark_min_time=0
   (opt is a separate build tree — see CLAUDE.md dev-loop notes; CI may own
   the full opt bench. The exit bar is: benches BUILD and a sample RUNS.)
4. DEV-LOOP SPEED preserved — confirm the optimizations the move could silently
   break still work: scripts/lint.sh (working-set) and `--branch` run at the
   expected speed with the PCH LOADED (no "'-pch=…' file not found" canary);
   compile_commands.json resolves the new paths.
5. VISIBILITY AUDIT (design §5.5) — nobody deps non-public targets:
   a. No internal target reachable from outside its component:
        bazel query 'rdeps($PROJ, //compiler/internal/...) except //compiler/...' → empty
        bazel query 'rdeps($PROJ, //eval/internal/...) except //eval/...'         → empty
      (universe is $PROJ — §1.0 — never `//...`, which fails to load.)
   b. Public surface hasn't crept — the set of //visibility:public non-test
      targets equals the curated list (design §5.5):
        bazel query 'attr(visibility, "//visibility:public", $PROJ) except attr(testonly,1,$PROJ)'
      compared against the curated public targets; any extra is a review event.
   (Visibility is already enforced by `bazel build` at analysis time; this step
    AUDITS that the regime is the intended shape, not accidentally widened.)
6. bazel query 'kind(go_.*, $PROJ)'  → empty (Go surface gone)
7. LINT (Q10): the restructure moved ~244 files, so `lint.sh --branch` re-lints
   the whole tree and surfaces the pre-existing lint-backlog (braces-around-
   statements in var_parser/cel_runtime.c; wasmtime-edge clang-tidy config limits)
   — NOT restructure regressions. Gate = (a) `clang-format -i` normalization
   applied + committed; (b) header guards regenerated to new paths; (c) targeted
   `lint.sh <hand-edited files>` CLEAN; (d) no NEW warning category beyond
   lint-backlog.md. Full --branch burndown is a separate backlog task.
8. grep -rn compiler_v2 in CODE/BUILD/SCRIPTS (NOT docs) must be clean:
     grep -rn compiler_v2 --include=*.bazel --include=*.bzl --include=*.cc \
       --include=*.h --include=*.sh --include=*.py . | grep -vE 'third_party|bazel-'
   → empty. NOTE (Q6): doc/** is EXCLUDED — the doc-tree still carries historical
   `compiler_v2` references (milestone/review/probe records, deleted-dir mentions);
   reconciling those is the separately-scoped docs-refactor workstream (design §10),
   not this restructure. Only code/BUILD/scripts are gated clean.

EXIT CRITERIA (all true, steps 1–8): full suite + manual targets green;
conformance count == conformance/.baseline == W0 capture; benches build +
sample-run; dev-loop speed preserved (PCH loads); visibility audit clean
(no external dep on internal targets, public surface == curated list);
Go surface gone; lint
--branch clean; no compiler_v2 left in code/BUILD/scripts.
Then: merge restructure → master.
```

### W6 — Namespace flatten `celwasm::api` → `celwasm` [SERIAL, scripted]
```
Branch: restructure-ns (from master, after W5 merged). Its own single-purpose
commit — NOT mixed into the path move (design §7).
0. Collision sweep BEFORE flattening: confirm no free-function / enum /
   constant name exists in BOTH `celwasm` and `celwasm::api` (the public TYPE
   names were verified clash-free 2026-05-25; functions/enums still to sweep).
   Resolve any clash by moving the internal symbol to celwasm::<x>_internal.
1. Scripted sed over git ls-files '*.h' '*.cc' (EXCLUDING third_party):
     `namespace celwasm::api {` → `namespace celwasm {`  (+ close comments)
     `celwasm::api::`           → `celwasm::`
     `using ::celwasm::api::X`  → `using ::celwasm::X`
   Optional staging: add `namespace api = ::celwasm;` alias, migrate call
   sites, then delete the alias — if one atomic 802-site diff is unwieldy.
GATE: re-run the full W5 exit criteria (tests + manual + conformance baseline +
benches + lint). bazel query / grep: no `celwasm::api` remains.
Commit: "restructure: flatten celwasm::api namespace into celwasm".

FINALLY (restructure complete): RE-ENABLE HOOKS — scripts/install-hooks.sh
(sets core.hooksPath → .githooks). Do one real `git push` and confirm the
corrected pre-push gate (conformance + README drift, now on the new paths)
PASSES — proving the hook works, not merely that it was bypassed (§0).
```

## 4. What can run when (scheduling summary)

  - **Now → W0**: one agent, serial. Captures the baseline; blocks everything.
  - **After W0 → W1**: launch **Spec + Docs together** (2 agents, worktrees).
  - **After W1 merged → W3**: one agent, serial, scripted. The critical path.
  - **After W3 → W4**: launch **up to 10 agents together** — 9 per-package
    verifiers + W4·Conventions (CLAUDE.md + root README). All disjoint.
  - **After W4 merged → W5**: one agent, serial — the exit-criteria gate
    (all tests + manual suite + conformance baseline + benches + lint).
    Then merge to master.
  - **After W5 → W6**: one agent, serial, scripted — flatten the
    `celwasm::api` namespace; re-run the full exit criteria; merge to master.
    Separate commit, but the committed immediate follow-on (design §7).

Peak parallelism: 10 agents in W4. The serial spine (W0→W3→W5→W6) is the
unavoidable minimum because each globally re-points the build. W5 is the
binding exit criteria; W6 re-runs it after the namespace flatten.

## 5. Rollback

Each wave is one commit on `restructure`; `git reset --hard` to the prior
wave's commit reverts cleanly. `master` is untouched until W5 passes.

## 6. W3 APPENDIX — exact api/BUILD split (frozen target→package map)

The single `compiler_v2/api/BUILD.bazel` (666 lines, all targets) + the
`compile` target in `compiler_v2/BUILD.bazel` split into FOUR new packages.
api/internal/ files stay a SUBDIR of the eval package (Q7); compiler/internal IS
its own package (one target).

### 6.1 git mv (file moves)
```
# common
compiler_v2/api/type.{h,cc}  compiler_v2/api/type_test.cc        -> common/
# compiler (public)
compiler_v2/api/compiler.{h,cc} compiler_v2/api/compiler_test.cc -> compiler/
compiler_v2/api/program.h    compiler_v2/api/program_test.cc     -> compiler/
# compiler/internal (the pipeline facade)
compiler_v2/compile.{h,cc}   compiler_v2/compile_test.cc         -> compiler/internal/
# eval (public eval leaves + host_callback)
compiler_v2/api/{engine,instance,activation,value,error,attribute}.{h,cc} \
compiler_v2/api/{engine,instance,activation,value,error,attribute}_test.cc \
compiler_v2/api/host_callback.h                                  -> eval/
# eval/internal (SUBDIR of eval pkg — keep the internal/ path)
compiler_v2/api/internal/*                                       -> eval/internal/
# bench
compiler_v2/api/cel_pipeline_bench.cc                            -> bench/
# host folds into eval
compiler_v2/host/*                                               -> eval/host/
# straightforward dir moves (children keep their BUILDs; script rewrites refs)
compiler_v2/{frontend,ir,codegen,celfn}                          -> compiler/{…}
compiler_v2/{abi,runtime,tools,conformance,e2e,bench,testdata}   -> {…} (strip prefix)
compiler_v2/conformance/.baseline                                -> conformance/.baseline
```
After: `git ls-files compiler_v2/ | head` → EMPTY (compiler_v2/ gone).

### 6.2 Target → new package (authored BUILDs)
| New package / BUILD | Targets (from api/BUILD unless noted) |
|---|---|
| `common/BUILD` | `type`, `type_test` |
| `compiler/BUILD` | `compiler`(+`compiler_test`), `program`(+`program_test`) |
| `compiler/internal/BUILD` | `compile`(+`compile_test`) — from `compiler_v2/BUILD` |
| `eval/BUILD` | `attribute`(+test), `error`(+test), `value`(+test), `activation`(+test), `host_callback`; and (srcs under `internal/`) `abi_decode`(+test), `cel_host_hdrs`, `cel_host_error`(+test), `cel_host`, `cel_host_test_fakes`, `cel_host_test`, `host_map_test`, `proto_map_test`, `host_list_test`, `proto_list_test`, `cel_map_lookup_impl_test`, `cel_list_at_impl_test`, `cel_host_wasmtime`, `wasmtime_engine_state`, `instance_impl`, `instance`(+test), `engine`(+test) |
| `bench/BUILD` | ADD `cel_pipeline_bench` (cc_binary, tags=["manual"]) |
| `eval/host/BUILD` | the moved `compiler_v2/host` BUILD (script rewrites its refs) |

The frontend/ir/codegen/celfn/abi/runtime/tools/conformance/e2e/bench/testdata
BUILDs move WITH their dirs; `restructure_rewrite.sh` rewrites every
`//compiler_v2/…` label + `compiler_v2/…` include inside them. Do NOT re-author
those — only the api split + compile move are hand-authored.

### 6.3 Dep repointing the script does NOT do (relative `:foo` labels crossing
packages — hand-fix in the authored common/compiler/eval BUILDs):
  - `:type`     → `//common:type`   (in compiler/ + eval/ targets that dep it)
  - `:compiler` → `//compiler:compiler` (eval `instance_test`,`engine_test`; bench `cel_pipeline_bench`)
  - `:program`  → `//compiler:program`  (same callers)
  - intra-eval `:value`/`:error`/`:attribute`/`:activation`/`:cel_host`/`:engine`/
    `:instance`/`:abi_decode`/… STAY `:foo` (same eval package).
  - `//compiler_v2:compile` → `//compiler/internal:compile` — the SCRIPT does this
    (it has the rule), incl. api `compiler` target's dep.
  - All `//compiler_v2/{abi,ir,runtime,host,celfn,testdata,…}:…` → SCRIPT does it.

### 6.4 Visibility (design §5.5) — set in the authored BUILDs
  - `common/BUILD`:           `package(default_visibility=["//visibility:public"])`
    is WRONG — instead leave default private and mark `type` `//visibility:public`.
    Simplest: `common/BUILD` has `cc_library(name="type", …, visibility=["//visibility:public"])`.
  - `compiler/BUILD`:  `package(default_visibility=["//compiler:__subpackages__"])`;
    `compiler` + `program` each `visibility=["//visibility:public"]`.
  - `compiler/internal/BUILD`: `default_visibility=["//compiler:__subpackages__"]`
    (compile is internal — reachable only within //compiler; the public `compiler`
    lib deps it from within //compiler). `compile_test` needs no extra vis.
  - `eval/BUILD`: `package(default_visibility=["//eval:__subpackages__"])`; then
    `engine,instance,activation,value,error,attribute` each
    `visibility=["//visibility:public"]`. All internal targets (cel_host*,
    abi_decode, instance_impl, wasmtime_engine_state, cel_host_wasmtime,
    host_callback) inherit the package default → NOT public.
  - `eval/host/BUILD`: `default_visibility=["//eval:__subpackages__"]`.
  - testonly fixtures (cel_host_test_fakes, testdata cc_protos) may stay
    public/test-visible as needed.
  - The moved component BUILDs (frontend/ir/codegen/celfn/abi/runtime/tools/…):
    their `package(default_visibility=["//compiler_v2:__subpackages__"])` must
    change. compiler-side (frontend,ir,codegen,celfn) → `//compiler:__subpackages__`.
    abi,runtime → `//visibility:public` (shared contract, every binding speaks them).
    tools,conformance,e2e,bench,testdata → `//visibility:public` is fine (leaf
    consumers/tests; nobody deps them inward) OR component-scoped; testdata is
    testonly-public. KEEP IT SIMPLE: abi,runtime,testdata public; frontend,ir,
    codegen,celfn → //compiler:__subpackages__; tools,conformance,e2e,bench →
    //visibility:public (they're top-level binaries/tests).

