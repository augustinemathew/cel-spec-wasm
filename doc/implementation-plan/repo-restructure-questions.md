# Repo restructure — running questions log

Autonomous-execution scratchpad. Each question is resolved in-place (by
inspection or by asking a sub-agent), never escalated to the user. Format:

```
### Q<n> — <one-line> [OPEN|RESOLVED <date>]
Context: …
Resolution: …
```

---

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
//compiler/... //eval/... //common/... //abi/... //runtime/... \
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
