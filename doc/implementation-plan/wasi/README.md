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
| **M5** | Codegen prologue: `(call $arena_reset)`; drop `arena_base` + `mem_size_bytes`; asserts A11-A12, A17 | ☐* | — | 1.0 |
| **M6** | Engine: pull runtime-owned memory; bind `arena_*`/`malloc`/`free` | ◐ | [M6-M8.md](milestones/M6-M8.md) | 0.5 |
| **M7** | Instance: malloc'd binding buffer; delete `EnsureHostStringArenaCapacity` | ☐* | — | 0.5 |
| **M8** | E2E test `mvp_concat_test.cc` (`'foo' + 'bar'` → `"foobar"`) | ☒ | [M6-M8.md](milestones/M6-M8.md) | 0.25 |
| **M9** | absl::ParseTime in wasm runtime; Chrome stake | ◐ | [M9.md](milestones/M9.md) | 0.5 |

*M5 + M7 deferred to Phase B; today's pipeline works because the
codegen-prologue `cel_reset` compat shim and the legacy
`host_string_arena` bookkeeping route the old shapes through the
new arena.  See [M6-M8.md](milestones/M6-M8.md) "Plan-vs-execution
deltas" + [reviews/2026-05-18-mvp-shipped.md](reviews/2026-05-18-mvp-shipped.md)
for the rationale and exact deferred work.

**◐ deltas vs the as-designed plan**:

  - **M6** — WASI `random_get` stub + `arena_alloc` export wired
    end-to-end, but `--import-memory=cel,memory` was re-added
    after M2's removal (the runtime still imports memory from
    the host).  Full ownership flip is bundled with the codegen
    prologue swap in Phase B.
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
| **B2** | Migrate 20 test file `SetUp()` from `cel_reset` to `arena_reset` | ☐ | — | 0.5 |
| **B3** | Codegen test fixture rebaseline (~50 sites in `codegen/*_test.cc`) | ☐ | — | 1.0 |
| **B4** | Conformance debug → **1,144 PASS** | ☐ | — | 1-2 |
| **B5** | Post-migration bench against §11 workload → `POST_MIGRATION_BENCH.md` | ☐ | — | 0.5 |
| **B6** | Doc closeout (update sibling design docs; flip this status to shipped) | ☐ | — | 0.5 |

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
| **B1 shipped** | Kernels + host migrated off `cel_alloc` compat shim; shim deleted | `fcb1289` |

---

## Sentinel

[`CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR`](CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR)
flags this directory as owned by the migration.  Other agents
working on the repo should leave these files alone unless their
commit is explicitly part of this work.
