# Dev-loop performance: data-driven strategy

Status: **analysis + first fixes landed 2026-05-24.**

This doc answers: *why does the inner loop (build → test → lint →
conformance → push) feel slow, and what do we change?*  Every number
below is measured on this machine (darwin-arm64, bazel 7.3.2), not
estimated.  Priorities, per the team's direction:

1. **Correctness first.**
2. **Make a single checkout fast** — dev / lint / conformance / gate
   must all stay warm even when you switch between them.
3. **Cross-checkout sharing last** (and opt-in) — it trades against
   correctness with our non-hermetic toolchain.

---

## 1. TL;DR — where the time actually goes

| Activity | Measured | Reality |
|---|---|---|
| Full `bazel test //compiler_v2/...` | **618 s wall, 608 s critical path** | |
| └ sum of all 80 tests' *execution* | **44.8 s** | tests are NOT the cost |
| └ everything else (~573 s, 93%) | **compiling** test binaries | **cel-cpp from source dominates** |
| Pre-push gate (`-c opt` conformance), cold | **~10 min** | a *second* full cel-cpp build in a separate config tree |
| Pre-push gate in fastbuild, warm | **14.75 s** (verified, same pass=1774) | shares the dev tree |
| `lint.sh` on **2 files**, cold/contended | **609 s (~10 min)** measured | NOT clang-tidy — full build, see §4 |
| bare `lint.sh` (**default = working-tree edits**), warm | **~5-9 s** | inner-loop default |
| `lint.sh <one-file>`, warm | **4.6 s** (verified) | ~floor (header parse) |
| `lint.sh --branch` (full branch diff), warm | **73 s** (verified, ~20 files) | explicit pre-commit / PR gate |

**The bottleneck is build, not test or lint.** And the single worst
offender is the **pre-push gate running under `-c opt`**, a different
build configuration whose output tree shares nothing with your dev
(`fastbuild`) tree — so every push recompiled cel-cpp from scratch.

---

## 2. Root cause: configurations multiply your build trees

Bazel produces one output tree **per configuration**.  We were using
two configurations in the inner loop:

| Tree (`bazel-out/…`) | Config | Used by | Size |
|---|---|---|---|
| `darwin_arm64-fastbuild` | default | dev, `bazel test`, lint's compile flags | **2.6 GB** |
| `darwin_arm64-opt` (+ `opt-exec`, `opt-ST`) | `-c opt` | **only** the conformance push gate | 335 MB |

`fastbuild` and `opt` share **zero** compiled artifacts.  Switching
dev↔gate meant rebuilding cel-cpp in the other tree.  cel-cpp is
vendored and compiled from source; it's on the critical path of
nearly every binary, so that rebuild *is* the ~10 minutes.

**Worse, across directories.**  Bazel keys the *output base* by the
workspace's absolute path:

```
output_base = /private/var/tmp/_bazel_$USER/<md5(workspace_path)>
```

So every checkout gets its own 2.6 GB tree and recompiles cel-cpp
independently.  This machine currently has **three** checkouts, each
with a live daemon:

```
/Users/augustine/cel2              (this one)
/Users/augustine/cel-spec-wasm
/Users/augustine/cel-spec-wasm-opt   ← a whole 2nd checkout just to hold an opt build
```

That `-opt` checkout is the manual workaround for the config-thrash
problem above — a symptom, not a solution.

---

## 3. The fixes

### 3.1 LANDED — gate runs in the dev config (single tree)

`scripts/check_conformance_monotonic.sh` dropped `-c opt`; the gate now
runs `bazel run //conformance:run_conformance` in the
**default (fastbuild)** config — the same one dev and `bazel test`
use.

- **Correctness:** pass count is identical across configs — verified
  `1774 == 1774`.  The gate checks correctness, not eval throughput,
  so fastbuild's slower (CEL_LOG-enabled) eval is the right trade.
- **Effect:** the gate reuses the warm dev tree.  Cold `-c opt`
  ~10 min → warm fastbuild **24 s**.  No second tree, no cel-cpp
  rebuild on push.
- `-c opt` is reserved for `//bench` and CI, where eval
  wall-time matters and the machine is dedicated.

**This is the one-config principle:** dev, `bazel test`, conformance,
and the gate all live in `fastbuild`, so switching between them is
free — the tree stays warm.

### 3.2 LANDED — `user.bazelrc` override hook

`.bazelrc` now ends with `try-import %workspace%/user.bazelrc`
(gitignored).  Machine-specific knobs — a remote-cache URL, a
`--disk_cache` path, `--jobs` — go there without touching the
committed config.  Committed config stays identical for every
checkout (your "persists across checkouts" requirement); per-machine
tuning is local.

### 3.3 RECOMMENDED — make conformance a cacheable `bazel test`

Today conformance is a `bazel run` binary, so Bazel can't cache its
*result*; it re-evaluates every invocation.  Wrapping the corpus run
in a `cc_test` (asserting `pass >= baseline`) lets Bazel's **local
action cache** skip it entirely when nothing changed — fully
correct (local, content-addressed), single-directory.  Most pushes
would then run *nothing* for the gate.

### 3.4 RECOMMENDED — move the heavy gate to CI, keep push instant

The pre-push hook is our de-facto CI (cloudbuild only does `bazel
build`).  A correctness gate on every `git push` is what created the
10-min pushes *and* the SSH idle-timeouts (GitHub drops the
connection while the hook runs long).  Target state: run conformance
in GitHub Actions / cloudbuild on push-to-branch, and keep the local
hook to the fast monotonic delta only (or nothing).  Push becomes
instant; correctness is still gated, just not synchronously on your
machine.

### 3.5 Inner-loop discipline

The 618 s number is "test everything from a cold tree."  Day to day:
build/test only the touched package (`bazel test
//runtime:cel_base64_ext_test`), not `//compiler_v2/...`.
Bazel's local cache already makes re-runs of unchanged targets
instant within a checkout.

---

## 4. Linting strategy

### Why lint was slow (measured: 609 s for 2 files)

It was **not** clang-tidy or the PCH compile.  `lint.sh` →
`build_lint_pch.sh` ran, **unconditionally on every lint**:

```
bazel build //compiler_v2/...   # line 36, "keep external symlinks live"
```

On a warm tree that's ~2 s; on a cold/evicted/contended tree it is a
**full from-scratch cel-cpp compile** — the same ~10-minute build as
everything else.  So "linting is slow" was really "linting does a
full project build first."  `clang-tidy` itself, fanned out over
`-P jobs` with the cached PCH, is seconds.

**LANDED fix:** guard that build so it only runs when the heavy
external symlinks (`abseil`, `protobuf`, `cel-cpp`) are actually
missing.  On a warm tree (the common case) lint now skips it entirely
and is back to seconds.  A fresh checkout pays it once.

### Lint the right number of files (the real per-loop lever)

clang-tidy parsing absl/cel-cpp headers is ~4.6 s **per file** even
with the PCH — that's the floor.  So the win is linting *fewer files,
more often*:

| Command | Scope | Cost | When |
|---|---|---|---|
| `lint.sh` (**default**) | working-tree edits (staged+unstaged) | **~5-9 s** | inner loop |
| `lint.sh <file>` | named file(s) | **4.6 s** | inner loop |
| `lint.sh --dirty` | explicit synonym for the default | ~5-9 s | inner loop |
| `lint.sh --branch` | whole branch diff vs `origin/master` + worktree (~20 files) | **73 s** | once, pre-commit / PR |
| `lint.sh --all` | every `compiler_v2/` source file | — | rare |

**LANDED:** the default was **flipped** — bare `lint.sh` now lints only
your working-tree edits (the inner-loop common case), and the
exhaustive full-branch sweep is the explicit `--branch` gate.  This
follows the general principle: *the cheap, working-set-scoped
operation is the default; the exhaustive sweep is opt-in* (mirrors
package-scoped `bazel test` in the loop vs `//compiler_v2/...` at the
gate).

### The rest of lint

Lint is otherwise **config-agnostic** — it does not use `-c opt`.
`clang-tidy` runs against `compile_commands.json` (from
`scripts/refresh_compile_db.sh`) with a **native** PCH
(`arm64-apple-macosx`).  There is effectively **one lint
configuration**, and the only distinction it cares about is **native
vs wasm32** — never opt vs fastbuild.

Known sharp edges (all about correctness/consistency, not speed):

- **wasm32 entries poison the PCH.**  `compile_commands.json` contains
  both a native and a `wasm32-wasi-threads` entry for every runtime
  TU; if the wasm one wins, the PCH is built for the wrong target and
  every native TU fails to lint (the M12 PCH bug).  Fix: the compile-db
  refresh / PCH builder must filter wasm32 entries at source.
- **clang-format version drift.**  Local Homebrew `clang-format` (v22)
  reformats the repo's committed style (compact macros, aligned
  switches) → large spurious diffs.  Pin the LLVM version the repo
  formats against and document it (`brew install llvm@<N>`), or run
  format only in CI with a pinned binary.
- **Pre-existing backlog** (`doc/implementation-plan/lint-backlog.md`):
  `misc-use-internal-linkage`, `pro-type-member-init`, function-size —
  these make `lint.sh` exit non-zero on touched files even when your
  change is clean.  Either NOLINT-with-rationale them or establish a
  baseline so lint gates only *new* findings.

Lint is fast enough (PCH cached, clang-tidy fanned out `-P jobs`); the
work here is making it *consistent* so it's a trustworthy gate.

---

## 5. Cross-checkout caching — LAST, opt-in, correctness-gated

This is the only way to make a *fresh* checkout (or the 2nd/3rd one)
fast, but it carries a real correctness hazard with our toolchain, so
it is **off by default**.

**It works (measured).**  With a shared `--disk_cache`, a completely
fresh output base (= a different directory checkout) rebuilds with
**0 compiles, 34/34 disk-cache hits**, critical path 2.34 s → 0.09 s.
`~` expands to `$HOME` in `--disk_cache`, so a committed path would
apply to every checkout.

**Why it's not on by default.**  We build with the **non-hermetic
system Apple/brew toolchain**.  Bazel's action key may not capture a
silent compiler/SDK change, so a cache that outlives an Xcode or
`brew upgrade llvm` could serve a **stale object** — a correctness
bug that's miserable to diagnose.  The disk cache is content-addressed
and safe *within* a stable toolchain; the risk is across toolchain
changes and across machines.

**How to opt in safely** (per developer, in `user.bazelrc`):

```
build --disk_cache=~/.cache/celwasm-bazel-disk
```

and `bazel clean` / clear that directory whenever you bump Xcode or
brew llvm.

**The correctness-safe path to fast-fresh-checkouts** (future work):

1. **Hermeticize the toolchain** — pin a specific LLVM/clang via a
   Bazel toolchain (e.g. `toolchains_llvm`) so the compiler identity
   is part of every action key.  Then a shared cache is safe by
   construction.
2. **Then** add a **remote cache** (bazel-remote / BuildBuddy) shared
   by all checkouts and CI: cel-cpp compiled once, globally.  This is
   the real end-state — it makes every checkout and CI fast without
   the staleness hazard, *because* the toolchain is now hermetic.
3. Until (1) lands, prefer **one checkout** + the one-config principle
   (§3.1) over multiple checkouts; retire the `cel-spec-wasm-opt`
   directory (the gate no longer needs opt).

---

## 6. Action checklist

- [x] Gate runs in fastbuild, not `-c opt` (§3.1) — `check_conformance_monotonic.sh`.
- [x] `try-import user.bazelrc` + gitignore (§3.2).
- [x] disk_cache documented as opt-in with caveats (§5), not enabled by default.
- [x] **Lint no longer runs a full build on a warm tree** (§4) — `build_lint_pch.sh` guards the `bazel build //compiler_v2/...` behind a "symlinks missing?" check.  Cold/contended lint was 609 s for 2 files; warm is now seconds.
- [ ] Conformance as a cacheable `bazel test` (§3.3).
- [ ] Move conformance gate to CI; slim the pre-push hook (§3.4).
- [ ] Compile-db / PCH wasm32 filter; pin clang-format version; lint backlog baseline (rest of §4).
- [ ] Hermetic LLVM toolchain → remote cache (§5 future work).

---

## Appendix — experiments (2026-05-24)

**A. Build-vs-test split.**  `bazel test //compiler_v2/...`: 618 s
wall, 608 s critical path; sum of 80 tests' execution = 44.8 s ⇒ ~93%
is compilation.  Test-time histogram: 3 tests ≥5 s (cel_smoke 11 s,
engine 5.1 s, instance 5.1 s), 9 in 1–5 s, the remaining 68 ≤1 s
(most ≈0).

**B. Config trees.**  `fastbuild` 2.6 GB vs `opt` 335 MB; 5 physical
trees across 2 configs; they share no artifacts.

**C. Output-base keying.**  `output_base` =
`/private/var/tmp/_bazel_$USER/<md5(workspace_path)>` (per-checkout);
`install_base` is shared.  Three live checkouts on this machine
(`cel2`, `cel-spec-wasm`, `cel-spec-wasm-opt`).

**D. disk_cache across a fresh output base.**  RUN 1 (cold, fresh
output base): 11.6 s, 34 compiles.  RUN 2 (different fresh output
base, shared disk_cache): 6.7 s, **34/34 disk-cache hits, 0
compiles**, critical path 0.09 s.  `~` expands in `--disk_cache`.

**E. Gate config.**  `-c opt` conformance cold ≈ 10 min (separate
tree, cel-cpp rebuild); fastbuild conformance warm = 24 s; pass count
identical (1774).
