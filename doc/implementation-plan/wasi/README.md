# WASI / `malloc` migration

Branch: `wasi-malloc-migration` (forked from master @ `9685d72`).

The **design** is in [`DESIGN.md`](DESIGN.md) — singular source
of truth for goals, architecture, sizing assumptions (with code
assertions), per-Plan and per-Eval lifecycles, baseline numbers,
acceptance criteria, and the work-item breakdown.

This README is the **live tracker** — it lists every milestone
slice, its current status, and links to its per-slice doc once
that slice is started.  Per-slice docs land in
[`milestones/`](milestones/).

---

## Phase A — MVP (`"foo" + "bar"` end-to-end)

Goal: a runnable demo in both wasmtime AND Chrome before any
long-tail kernel migration starts.  Total: **~5 days**.

| ID | Slice | Status | Doc | Days |
|---|---|---|---|---:|
| **M1** | wasi-sdk in `MODULE.bazel` (4 platforms) | ☒ | [M1.md](milestones/M1.md) | 0.5 |
| **M2** | `runtime/BUILD.bazel` switch; `cel_layout.h` + asserts A1-A8; temp `cel_alloc`/`cel_reset` compat shim | ☒ | [M2.md](milestones/M2.md) | 1.0 |
| **M3** | `cel_arena.c` rewrite (arena over malloc); asserts A9-A10, A16; unit tests | ☒ | [M3.md](milestones/M3.md) | 0.5 |
| **M4** | Migrate kernels off `cel_alloc` — subsumed by B1 below | ☒ | (see B1) | (0) |
| **M5** | Codegen prologue: `(call $arena_reset)`; drop `arena_base` + `mem_size_bytes` | ☒ | (commit `dfc366c`) | 1.0 |
| **M6** | Runtime exports memory; host pulls + binds; drop `--import-memory` | ☒ | (commit `208ddba`) | 0.5 |
| **M7** | Instance: malloc'd activation buffer; delete `EnsureHostStringArenaCapacity` | ☒ | (commit `5d8156a`) | 0.5 |
| **M8** | E2E test `mvp_concat_test.cc` (`'foo' + 'bar'` → `"foobar"`) | ☒ | [M6-M8.md](milestones/M6-M8.md) | 0.25 |
| **M9** | absl::ParseTime in wasm runtime; Chrome stake | ◐ | [M9.md](milestones/M9.md) | 0.5 |

**◐ delta vs the as-designed plan**:

  - **M9** — `absl::ParseTime` runs inside a wasi-sdk wasm
    runtime (proven by `exp_e_absl_parsetime.cc`), but the
    runtime is a separate experimental binary, not the main
    `cel_runtime.wasm`.  Chrome smoke-test was descoped to Phase
    D (post-Phase C library vendoring).  What's proven: 9 WASI
    imports all browser-shimmable; Chrome path **unblocked**, not
    consummated.

---

## Phase B — finish the migration

| ID | Slice | Status | Doc | Days |
|---|---|---|---|---:|
| **B1** | Migrate kernels + host off `cel_alloc` shim; drop the shim | ☒ | (commit `fcb1289`) | 1.0 |
| **B2** | Migrate test file `SetUp()` from `cel_reset` to `arena_reset` | ☒ | (commit `a104ea8`) | 0.5 |
| **B3** | Codegen test fixture rebaseline | ☒ | (folded into M5 commit `dfc366c`) | 1.0 |
| **B4** | Conformance debug (deferred — see note) | n/a | — | — |
| **B5** | Post-migration bench → [POST_MIGRATION_BENCH.md](POST_MIGRATION_BENCH.md) | ☒ | (this commit) | 0.5 |
| **B6** | Doc closeout (update sibling design docs; flip this status to shipped) | ☒ | (this commit) | 0.5 |

**B4 note**: the original B4 target ("1,144 PASS") was set when
master sat at 1144.  Master has since advanced to 1373 via the
M5.B comprehensions follow-on; the migration matches that
baseline exactly with no regressions.  The remaining 491 failures
are pre-existing (missing extension subsystems — math_ext,
string_ext, network_ext, optionals, encoders_ext, block_ext) and
not in scope of this migration.

---

## Phase C — RE2 / `absl::ParseTime` vendoring (post-migration)

The architectural payoff.  Phase C ships **regex** (`matches()`)
and **timestamp parse/format** as proof that library vendoring
works under the new architecture.

| ID | Slice | Status | Doc | Days |
|---|---|---|---|---:|
| **C1** | Vendor `abseil-cpp` (`http_archive` + cross-compile via wasi-sdk + the `absl-wasm.patch`) | ☐ | — | 1.0 |
| **C2** | Vendor `re2` (build against vendored absl) | ☐ | — | 0.5 |
| **C3** | `cel_matches_at_vv` + per-Instance regex cache | ☐ | — | 1.0 |
| **C4** | `cel_timestamp_parse_at_v` calling `absl::ParseTime` | ☐ | — | 0.5 |
| **C5** | Conformance: `string.textproto::matches/*` (9 SKIPs) flip to PASS; timestamp parse rows too | ☐ | — | 0.5 |
| **C6** | Bench delta + Chrome retest | ☐ | — | 0.5 |

---

## Status legend

| Symbol | Meaning |
|---|---|
| ☐ | Not started.  No doc yet. |
| ◐ | In progress.  Per-slice doc in `milestones/<ID>.md` with WIP notes. |
| ☒ | Shipped.  Per-slice doc records what landed; "Doc" column links to it. |

---

## Done-to-date

| Step | Outcome | Commit |
|---|---|---|
| Baseline benchmark captured | Pre-migration numbers in `DESIGN.md` §10 | `a086393` |
| Memory experiments validated | All 5 architectural decisions resolved (`DESIGN.md` §3) | `df49328` |
| Authoritative plan consolidated | `DESIGN.md` is now singular | `8392651` |
| **M1 shipped** | wasi-sdk available via `//third_party/wasi_sdk:clang` | `eca5583` |
| **M2 + M3 shipped** | Runtime builds with wasi-sdk + bump arena over malloc | `f4b7157` |
| **M6 + M8 shipped (partial)** | WASI random_get stub + mvp_concat_test green; all e2e + conformance regression-clean | `582def9` |
| **M9 (partial)** | absl::ParseTime running inside wasi-sdk wasm (experiment binary only — Chrome path **unblocked**, not consummated; main runtime untouched until Phase C) | `f4b09da` |
| **First periodic review** | Mixed verdict; 8 DESIGN §1 simplifications still hidden behind shims | `66a17cb` |
| **Review P1+P2 follow-ups** | arena_alloc trap-on-uninit; A13/A14/A15 assertions; cleanup-backlog seeded | `1f2bdbd` |
| **Edge-case memory tests** | 47 new tests across 6 files (arena boundaries, alignment, OOM, alloc-before-init death tests, per-Eval lifecycle) | `9bb1403` |
| **B1 shipped** | Kernels + host migrated off `cel_alloc` compat shim; shim deleted | `fcb1289` |
| **B2 shipped** | All test SetUp() migrated off `cel_reset` shim | `a104ea8` |
| **M5 + B3 shipped** | Codegen emits `(call $arena_reset)` zero-arg; `cel_reset` shim deleted; ~50 codegen fixtures rebaselined | `dfc366c` |
| **M6 shipped** | Runtime owns + exports memory; host pulls from `runtime_instance`; `--import-memory` dropped | `208ddba` |
| **M7 shipped** | `host_string_arena` deleted; replaced with malloc'd activation buffer via wasm reentry | `5d8156a` |
| **B5 + B6 shipped** | POST_MIGRATION_BENCH.md numbers; DESIGN status flipped to shipped; sibling docs reconciled | (this commit) |

---

## Sentinel

[`CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR`](CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR)
flags this directory as owned by the migration.  Other agents
working on the repo should leave these files alone unless their
commit is explicitly part of this work.
