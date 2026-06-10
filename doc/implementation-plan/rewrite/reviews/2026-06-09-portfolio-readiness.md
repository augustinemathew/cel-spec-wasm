# Portfolio-readiness review — 2026-06-09

Reviewer lens: (a) hiring manager, 2-minute skim; (b) senior engineer,
10-minute skim; (c) compiler/runtime specialist, deep dive. Scope: README +
first-impression surfaces, full git history (706 commits), docs culture,
test/conformance/bench infrastructure, branch + working-tree hygiene.
Review-only: no code or doc outside this file was changed.

## Verdict

**Strong-with-caveats.** The underlying engineering is genuinely impressive
— a working CEL→wasm AOT compiler with 1,899/2,454 conformance passes, every
skip and fail classified, a WAT-first ABI process, 96 first-party test files,
and an honest, profiler-driven perf arc that ends with a measured ~30×-faster-
than-cel-cpp headline. But the repo currently *hides* almost all of that from
a skimmer: the README has zero performance numbers (the only comparative
table a reader will stumble into, `benchmark/DESIGN.md` §1.4, shows cel-cpp
*winning* 3.6×), there is no CI visible on GitHub, the first-click docs still
reference a deleted `compiler_v2/` tree, and `MAINTAINERS.md` contains a
self-inserted row in the upstream "CEL Language Council" table that a sharp
reviewer will read as a claimed governance seat at Google. **Top 3 actions:
(1) fix or delete MAINTAINERS.md; (2) add GitHub Actions CI (build + test +
conformance gate) with badges; (3) land m28 and put a headline-results table
(conformance + vs-cel-cpp benchmarks) at the top of the README.**

---

## 1. First-impression surfaces

### 1.1 README.md — sells the idea, not the results — **P0**

The README is well-written prose (the "compile, don't interpret / one
artifact, every language" pitch is crisp, the embedding example is real and
matches the actual public API in `compiler/compiler.h` + `eval/engine.h`).
What it lacks is **evidence**:

- **No numbers anywhere.** Not the conformance headline
  (`pass=1899 (77.4%)` lives only in `conformance/README.md`'s autogen
  block), not the bench results (the ~30×-on-`intAdd1000Terms` /
  parity-at-N=2 static-link numbers live only in
  `doc/implementation-plan/rewrite/m28-configurable-linking.md` §4, on an
  unmerged branch). "Compiled wasm evaluates far faster than a tree-walk
  interpreter" is exactly the unverifiable claim `benchmark/README.md` itself
  says is worthless without numbers — and worse, the one comparative table
  currently committed (`benchmark/DESIGN.md` §1.4) shows **cel-cpp 2.8–3.6×
  faster** (the pre-m28 dynamic-mode measurement). A specialist who greps for
  benchmarks finds the repo apparently losing to the baseline. Fix: headline
  table in README (see §5.2), regenerated from `benchmark/eval/results/` per
  the publisher design so it can't drift.
- **Quickstart verified statically — it works as written.** `//tools/cel:cel`
  exists (`tools/cel/BUILD.bazel:80`), `--var name:Type=value` is real
  (`tools/cel/var_parser.{h,cc}`), `//conformance:run_conformance` exists
  (`conformance/BUILD.bazel:162`), `third_party/fetch_cel_cpp.sh` is the
  documented first step. Good. One nit: the README does not say the *first*
  `bazel build //...` compiles cel-cpp from source (~10 min cold) — a
  reviewer who tries it will think something is broken. One sentence ("first
  build compiles vendored cel-cpp, ~10 min; subsequent builds are
  incremental") buys goodwill. **P2**
- **No provenance note.** See §2.2 — the fork heritage should be one explicit
  sentence in the README, not something the reviewer discovers via
  `git shortlog`. **P1**

### 1.2 MAINTAINERS.md — **P0, the single most damaging file in the repo**

`MAINTAINERS.md` is upstream cel-spec's "CEL Language Council" governance
table. Commit `2a686f09` (the module-rename commit, 2026-05-25) added a row:

```
| Augustine Mathew| Google       | cel-wasm, cel-spec |
```

To anyone who knows the CEL project — and the people best positioned to
appreciate this repo *do* — this reads as claiming a seat on the CEL Language
Council and a Google affiliation, inside a file whose other rows are real
council members. Whatever the intent (probably "I maintain this fork"), the
optics are terrible for a portfolio: it's the one artifact that could turn an
impressed reviewer skeptical about everything else. **Fix before sharing:**
delete the inherited council table and replace the file with an honest
two-liner ("This repository is maintained by Augustine Mathew. It began as a
fork of google/cel-spec; upstream governance is documented there."), or
delete the file entirely.

### 1.3 CI — none visible — **P0**

- `.github/` contains only `pull_request_template.md`. **No workflows, no
  badges.** For a portfolio repo, a green Actions badge is the cheapest,
  loudest credibility signal there is; its absence is the first thing both a
  hiring manager and a senior engineer notice on the GitHub landing page.
- `cloudbuild.yaml` is stale upstream heritage (last touched by Tristan
  Swadell, 2025-03-06, upstream PR #447) and would fail as written — it runs
  `bazel build ...` without `third_party/fetch_cel_cpp.sh`. Delete it or
  replace it; a visibly broken CI config is worse than none.
- Concrete fix: one GitHub Actions workflow — Linux runner, cache the bazel
  output tree, steps: `fetch_cel_cpp.sh` → `bazel build //...` →
  `bazel test //...` → `bazel run //conformance:run_conformance` +
  `scripts/check_conformance_monotonic.sh`. The conformance-monotonic gate
  in CI is itself a portfolio artifact ("pass count can only go up, enforced
  on every PR") — surface it with a badge or a one-line README mention. The
  repo already builds on Linux x86_64/arm64 per its own docs and has a
  `docker/Dockerfile`, so this is wiring, not porting.

### 1.4 Naming — four names for one project — **P1**

- GitHub repo: `cel-spec-wasm`. README title: `celwasmc`. Bazel module:
  `cel-wasm` (renamed in W7). C++ namespace: `celwasm`. `doc/README.md`
  opener: "the `cel2` documentation tree".
- `cel-spec-wasm` as the public repo name is the worst of the four — it reads
  as "the spec, with wasm bindings", underselling that this is an original
  compiler. Pick one name (`cel-wasm`/`celwasm` is the natural choice given
  the module + namespace), rename the GitHub repo (GitHub redirects old
  URLs), retitle the README, and fix the `cel2` stragglers. Half a day, large
  legibility payoff.

### 1.5 License & layout — mostly fine

- `LICENSE` is Apache-2.0 (inherited from cel-spec — correct for a fork).
  Vendored cel-cpp (fetch-on-demand, only the SHA committed) and the
  conformance corpus are likewise Apache-2.0; no attribution problem.
  README's closing "Released under the [Apache License]" is fine. **OK**
- Top-level layout is legible and the README's Layout table matches reality
  (verified against the tree). The lifecycle-role organization +
  `internal/`-plus-visibility regime is a point in favor a senior reviewer
  will notice. **OK**
- `CONTRIBUTING` story is split between `CLAUDE.md` and `doc/contributing.md`
  — fine for now; a root `CONTRIBUTING.md` pointer is a **P2**.

## 2. Commit history as narrative

### 2.1 The good news

- **365 original commits in ~7 weeks** (first own commit 2026-04-18, latest
  2026-06-07) with a genuinely strong message convention: scoped prefixes
  (`restructure(W3):`, `docs(abi):`, `fix(conformance):`), bodies that state
  the *why* and the gate status ("GATE GREEN: build green, 83 tests pass,
  conformance 1898 == baseline"). Sampled across eras (M1 backfill →
  WASI migration → phase-C → M12–M21 → restructure → m28): the discipline is
  consistent end-to-end. Merge hygiene is clean — real PRs (#5–#20) with
  descriptive merge commits. Only one `wip:` message in all refs, and it's in
  a stash. This history *is* a portfolio asset: `git log --oneline` reads
  like a serialized engineering journal.
- **No secrets found**, no force-push scars visible in the published history.

### 2.2 The fork shadow — **P1**

`git shortlog -sn` shows 341 upstream commits (Tristan Swadell, Jim Larson,
Justin King, …) under the project's history because the repo is a fork of
google/cel-spec (root: "Initial import from go/api-expr"). Keeping the
heritage is defensible — the corpus and langdef genuinely descend from it,
and the W7 "disconnect" commits document the unwinding — but a 2-minute
GitHub skim of the Contributors graph shows Google engineers, which both
dilutes ownership and invites the wrong question. Cheapest fix: an explicit
provenance paragraph in the README ("began as a fork of google/cel-spec to
inherit the conformance corpus and language definition; the compiler,
runtime, evaluator, and everything outside `spec/` and `doc/langdef.md` is
original — first original commit 2026-04-18"). Honest, preemptive, and it
turns the fork from a question mark into evidence of spec fidelity.

### 2.3 The 2.4 GB blob — contained, but clean it up — **P1**

`git rev-list --objects --all` surfaces
`wasm_compilation_experiments/wrapper_overhead/artifacts/merged_intAdd1000_O3aggressive.wat`
at **2,416,499,045 bytes**. Traced it: it lives *only* in `stash@{0}`'s
untracked-files commit (`18eebb69`, "untracked files on
perf/binaryen-merge-proto") — stash refs are local-only and never push, and
no branch commit contains it (`git log --all --find-object=…` matches only
the stash). So the published GitHub history is clean. But: drop the stash
(`git stash drop` after salvaging anything wanted) and `git gc` — anyone you
hand the working checkout to (or a future `git push --mirror`) would ship it.
Also verify origin carries no `backup/*` tags (`git ls-remote --tags origin`
returned nothing from this sandbox — re-check with network). The `probes/
foreign_go/*.wasm` blobs (1.6–6.4 MB) *are* in pushed history; tolerable, not
worth a history rewrite.

### 2.4 AI-attribution trailers — a considered view — **P1 (framing, not removal)**

341 of 365 original commits carry `Co-Authored-By: Claude Opus 4.7 (1M
context) <noreply@anthropic.com>`. Two reactions are possible and you only
control which one wins by framing:

- *Unframed* (today): a reviewer greps the log, sees the trailer on
  essentially every commit, and constructs their own narrative — "the AI
  wrote it; what did the human do?" The portfolio story collapses to a
  question you're not present to answer.
- *Framed*: the same evidence reads as the differentiator. The repo is one of
  the more convincing public artifacts of **directed, gated, AI-assisted
  systems engineering** I've seen: `CLAUDE.md` is a 500-line engineering-
  management spec (WAT-first ABI rule, oracle-as-tiebreaker, skip discipline,
  probe-before-design, review cadence), and the dated review reports under
  `reviews/` show adversarial QA actually happening. That is a 2026-relevant
  senior skill, and it's honest.

Recommendation: **do not strip the trailers** (rewriting 365 commits of
history to hide tooling would be both detectable and worse than the thing it
hides). Instead add a short README section — "How this was built" — that
owns it in two or three sentences: AI-assisted under a written process,
human-directed design and review, every milestone gated on conformance +
tests, link to `CLAUDE.md` and a sample review report. Tell the story before
the log does.

## 3. Engineering-culture evidence

### 3.1 The 3–5 strongest "hire this person" artifacts

1. **`conformance/README.md` + the monotonic gate** — auto-regenerated
   per-fixture pass/skip/fail tables, a typed skip taxonomy classified by
   status *payloads* rather than message-text matching, and
   `scripts/check_conformance_monotonic.sh` wired into a pre-push hook so the
   pass count can only ratchet up. This is conformance-driven development
   with the regression-proofing made structural. (Specialist catnip; lead
   with it.)
2. **The WAT-first ABI process** — 10+ hand-written, executable `.wat` traces
   under `doc/implementation-plan/rewrite/wat/`, a 2,548-line
   `wat-traces.md`, and `wat_runner` re-assembling and re-running every trace
   as a test. "Freeze the ABI in executable artifacts before writing codegen"
   is textbook compiler-engineering judgment, and it's *documented as policy*
   in CLAUDE.md, not just practiced once.
3. **The honest perf arc** — `benchmark/DESIGN.md` §1.1 opens with "The
   original thesis … turned out to be **wrong** for short expressions",
   `m28-wrapper-overhead-findings.md` shows the profiler work
   (sample + wasmtime perfmap, per-host-call 83 ns decomposition,
   slope/intercept model), and `m28-configurable-linking.md` §4 shows the
   fix: 2.3× slower → **31× faster** on `intAdd1000Terms`, 3.6× slower →
   1.2× faster on `intAdd2`, parity (0–17%) at the N=2 floor. Publishing the
   losing number, diagnosing it, and engineering past it is the single best
   senior-engineer story in the repo — currently buried three directories
   deep on an unmerged branch.
4. **Test density + skip discipline** — 96 first-party `*_test.cc`, the
   55-case `e2e/host_fn_test.cc` custom-function suite (verified),
   `e2e/known_bugs_test.cc` pinning known fails as tests, and
   `per-component-test-coverage.md` codifying the never-skip-a-fixture rule
   with the M2 post-mortem attached.
5. **The review culture** — dated adversarial reports under `reviews/`
   (`2026-06-08-m28-prototype.md` is genuinely senior-grade: load-bearing
   invariants the design never anticipated, each with "loud or silent?"
   analysis and the missing test named).

### 3.2 What raises eyebrows

- **Stale `compiler_v2` ghosts in first-click docs** — **P1.**
  `conformance/README.md` is titled "`compiler_v2/conformance/`" and its
  run commands (`bazel run //compiler_v2/conformance:run_conformance`) are
  broken as written; `bench/README.md` same; `doc/README.md` (the
  "documentation index" the README sends readers to first) points at
  `../compiler_v2/README.md` (doesn't exist) and quotes `pass=1774 (72.3%)`
  as the live number next to an autogen block elsewhere saying 1899. For a
  repo whose calling card is doc discipline, the *entry-point* docs being
  stale is the most self-undermining flaw. (The restructure commits show
  why — W1-docs deferred the doc tail — but a reviewer won't read that.)
  One focused pass over `conformance/README.md`, `bench/README.md`,
  `doc/README.md`, `tools/cel/README.md` fixes it; `scripts/check_doc_drift.sh`
  already exists — extend it to grep for `compiler_v2` outside `archive/`.
- **Branch litter on origin** — **P2.** `claude/general-session-NfXHN`,
  `claude/evaluate-compiler-v2-ENScu`, `claude/wasm-module-abi-e7Odf` etc.
  are autogenerated session branches visible in the GitHub branch dropdown;
  plus stale feature branches (`m16_math_ext`, `m18_network_ext`,
  `cel_optional`) already merged. Prune (`git push origin --delete …`).
- **`MODULE.bazel` lying low** — the module is `cel-wasm` version `0.25.1`
  (inherited from the cel-spec versioning); consider resetting to `0.1.0`
  with your own versioning when renaming. **P2**

## 4. Gaps for a portfolio

- **P0 — CI + badges** (§1.3). Also consider a badge row: build, conformance
  pass-count (shields.io static badge regenerated by
  `regen_conformance_readme.sh`), license.
- **P0 — headline results in README** (§1.1, §5.2). Requires landing m28 to
  master first; until then the repo's best number is on a branch and its
  committed bench table shows a loss. Note: this review's brief cited "19×
  at 1000-term"; the docs on the branch say 30–31× (`m28-configurable-
  linking.md` §1, §4). Whichever number the final merged benchmark run
  produces, publish *that* one from `benchmark/eval/results/` — never a
  remembered one. Same for "conformance identical across both link modes":
  m28 doc lists it as gate P1-4 (§680) and the 2026-06-08 review notes the
  gate had *not* yet run at prototype time — run it, commit the parameterized
  conformance result, then claim it.
- **P0 — MAINTAINERS.md** (§1.2).
- **P1 — present the 92 fails on your terms.** The skip taxonomy is
  exemplary; fails get no equivalent. Add a fail-bucket table to
  `conformance/README.md` (the data is nearly there: proto2=20, parse=19,
  enums=18, fields=6, dynamic=5, proto3=4, optionals=4, namespace=4, …) with
  one-line root causes and a pointer to `e2e/known_bugs_test.cc`. "92 fails,
  each classified and pinned by a test" is a *strength*; "92 fails" alone is
  a liability. Honest favorable framing for the README: lead with pass-rate
  over *attempted* rows (1899/1991 = 95.4% of in-scope rows, with the 463
  skips itemized by design-scope category) alongside the raw 77.4%.
- **P1 — unmerged long-lived branches.** `perf/binaryen-merge-proto` (39
  ahead), `perf/ssp-fix` (38), `master-local` (25), `ts-eval-host-and-
  celfn-e2e` (3), `m22-foreign-fn` (1), plus the active
  `m28-configurable-linking`. Land m28, then triage the rest (merge, or
  delete after harvesting docs). A portfolio repo should have master = the
  story.
- **P1 — working-tree hygiene.** 67 dirty/untracked paths
  (2,126 insertions) at review time — fine mid-flight, but the share-ready
  state is "clean tree on master, CI green".
- **P1 — provenance + how-built sections in README** (§2.2, §2.4).
- **P2 — no demo.** A hosted playground is expensive (the compiler links
  cel-cpp), but cheap substitutes exist: an asciinema/GIF of
  `cel eval 'account.emails.exists(e, e.endsWith("@corp.com"))' …` in the
  README; a "what the output looks like" section showing the actual
  disassembled WAT of a compiled expression (you have `wasm-dis` tooling
  everywhere); a GitHub Release with a prebuilt `cel` binary + a sample
  `.wasm`+`cel.abi` artifact pair. The "compile once, run anywhere" pitch
  lands 5× harder next to a 40-line WAT listing.
- **P2 — GitHub repo metadata.** Description, topics
  (`webassembly`, `cel`, `compiler`, `wasmtime`, `binaryen`), social-preview
  image. Free real estate on the 2-minute skim.

## 5. The story

### 5.1 Elevator pitch (recruiter-readable, 5 sentences)

> **cel-wasm is an ahead-of-time compiler from Google's Common Expression
> Language — the policy language behind Kubernetes admission control and
> Envoy authorization — to WebAssembly.** A CEL expression is type-checked
> once (reusing cel-cpp's frontend) and lowered through Binaryen to a
> self-contained wasm module that any language with a wasm runtime can
> execute, eliminating the per-language CEL interpreters that drift apart
> today. The compiler passes 1,899 rows of the official CEL conformance
> corpus — with every skip and failure classified by category and a
> pass-count ratchet enforced on every push — and in statically-linked mode
> evaluates long expressions up to ~30× faster than cel-cpp's reference
> evaluator, measured by an apples-to-apples benchmark harness committed to
> the repo. The system spans a Binaryen-based code generator, a C runtime
> kernel cross-compiled to wasm32-wasi, a wasmtime-based host evaluator, a
> frozen wire-format ABI designed WAT-first in executable traces, and 96
> test files enforcing a full positive/negative type matrix. It was built in
> eight weeks of AI-assisted development directed through a written
> engineering process — design docs with probe-confirmed facts, adversarial
> review reports, and conformance/bench gates — that is itself part of the
> portfolio.

### 5.2 Suggested README "Headline results" table

Place directly under the opening pitch; regenerate numbers from
`benchmark/eval/results/` + the conformance autogen block (never hand-typed):

| | Result | Reproduce |
|---|---|---|
| **CEL conformance** | 1,899 / 2,454 rows pass (77.4% of corpus; 95.4% of in-scope rows — 463 skips classified by category, 92 fails each pinned by a test) | `bazel run //conformance:run_conformance` |
| **vs cel-cpp — 1000-term arithmetic** | ~30× faster (static-link mode, steady-state eval) | `benchmark/eval/run.sh` |
| **vs cel-cpp — 2-term expression** | parity (within 0–17%, at the wasm-boundary floor) | `benchmark/eval/run.sh` |
| **Both link modes** | identical conformance results (static & dynamic) | `bazel run //conformance:run_conformance -- --link_mode=…` |
| **Test suite** | 96 test files; 55-case custom-function e2e matrix; per-type positive+negative coverage grid | `bazel test //...` |
| **Artifact** | one `.wasm` + `cel.abi` pair per expression; runs under any wasm runtime | `bazel run //tools/cel:cel -- compile …` |

(Drop any row whose number isn't reproduced by a committed result file at
publish time — the table's credibility is that every cell has a command.)

---

## Consolidated priority list

**P0 — before sharing the repo:**
1. Fix/replace `MAINTAINERS.md` (remove the self-inserted CEL Language
   Council row).
2. Add GitHub Actions CI (fetch → build → test → conformance-monotonic) +
   badges; delete or fix the stale `cloudbuild.yaml`.
3. Land m28 to master; run the both-modes conformance gate and the bench
   matrix; publish the headline-results table + conformance headline in the
   README.

**P1 — do soon:**
4. Stale-doc pass: purge `compiler_v2` paths from `conformance/README.md`,
   `bench/README.md`, `doc/README.md`, `tools/cel/README.md`; extend
   `check_doc_drift.sh` to enforce.
5. README additions: provenance note (cel-spec fork heritage), "How this was
   built" (own the AI-assisted process; keep the trailers), cold-build time
   warning.
6. Resolve the repo-name split: rename GitHub repo to match the `cel-wasm`
   module; retitle README; fix `cel2` stragglers.
7. Add the fail-bucket table to `conformance/README.md`; cross-link
   `e2e/known_bugs_test.cc`.
8. Hygiene: drop `stash@{0}` (2.4 GB blob) + `git gc`; verify no `backup/*`
   tags on origin; triage/merge/delete the 6 unmerged branches; prune
   `claude/*` and merged branches from origin; clean working tree.

**P2 — nice-to-have:**
9. Demo assets: CLI GIF/asciinema, a disassembled-WAT example in the README,
   a GitHub Release with prebuilt artifacts.
10. Root `CONTRIBUTING.md` pointer; repo description/topics/social image;
    reset module version; badge for conformance count.
