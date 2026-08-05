# m39 — Component-model removal (prime-time cleanup, phase 1)

Status: in progress — plan drafted and owner-approved 2026-08-04.
Execution ordering + live status: `m39-dag.md`.

Decisions log (owner, 2026-08-04):
  - Branch basis approved: `rip-out-components` on top of #38+#39.
  - `known_bugs_test.cc` plugin pins are DELETED with the feature,
    not kept as tombstones.
  - Work is executed by multiple agents per the DAG; ≤2 build-heavy
    concurrent, lint once at the final gate.
  - S1-inventory triage (owner, 2026-08-04): DELETE the
    `optional.ofNonZeroValue(message)` arm at every layer — new node
    F1, after D3/D4.  **RESCINDED BY EVIDENCE 2026-08-05:** the F1
    agent found backlog #41 (the basis for the decision) was a stale
    duplicate of #10, fixed 2026-06-10; the arm is fully working and
    conformance-passing at HEAD (oracle-confirmed, all layers
    green), and deleting it would fail the monotonic conformance
    gate.  F1 closed no-change; #41 closes as fixed-by-#10 (DOC3).
    Reopening requires an explicit owner decision to diverge from
    cel-cpp plus a baseline change.  DELETE the
    `@native`/`kCelDefined` parse-only stub — folded into D4 (same
    files as the kPlugin arm; verified no codegen/eval consumer, so
    frontend-only depth).  DELETE the two stale skips
    (`type_value_test.cc:256`, `list_test.cc:448`) + the
    contradicting comment at `eval/instance.cc:936` — new node F2,
    after D3; each un-skip observed green, not assumed.  Host-fn
    error carriage (PROPOSALS #2) = tracked work item AFTER m39, not
    in scope.

## 1. Decision

Remove the wasm Component-Model custom-function backend (`@plugin.`)
in its entirety: the wit-bindgen pipeline, the wasm32-wasip2
toolchain, the canonical-ABI marshalling layer, the `Plugin` API, and
every consumer.  The **host-callback backend (`@host.`) stays** — it
is the surviving custom-function mechanism.

Why (evidence gathered 2026-08-03/04, all empirical):

  - **Type coverage is a strict subset of ours.**  WIT cannot express
    CEL maps as returns (map-lower never emitted), protos degrade to
    `list<u8>`, `option<unit>` is rejected by wit-bindgen's C
    generator, Duration/Timestamp need ad-hoc records.
  - **The component boundary imposes import restrictions we must
    stub around.**  libc++'s lazy hash-seed reaches
    `wasi:random/random@0.2.0`; the wasmtime C API cannot provide it
    for components (works fine for core modules via `wasi_config_new`
    — proven by `cel_runtime_wasm.wasm`, which imports preview1
    `random_get` and never traps), and mid-canonical-ABI the call is
    forbidden outright (`cannot leave component instance`) regardless
    of provider.  Removing the guest RNG stub fails 4 of 5 plugin
    e2e tests — measured, not assumed.
  - **A second toolchain for one feature.**  wasip2 is a parallel
    toolchain variant in `cc_toolchain_config.bzl`, absl does not
    compile under its sysroot (permanent CELSKIP in the demo
    fixture), and wit-bindgen 0.57 quirks are load-bearing in our
    emitters.

Phase 2 (separate, later): the component work is preserved at its
best state — master + the plugin codegen fixes + passing kind-matrix
— on branch **`component-functions-archive`** (pushed 2026-08-04).
Any future plugin backend (see alternatives in §10) starts from the
design notes, not from reverting this removal.

## 2. What stays (explicitly)

  - `Engine::AddFunction` / `AddTypedFunction` / `BindFunction` —
    the `@host.` native-callback path, including `.celfn` decl
    parsing (`abi/celfn_wire`, `FunctionLibrary`) minus the
    `kPlugin` arm.
  - `abi/wasm_binary` — wasm framing (preamble/LEB128/custom
    sections).  Verify remaining consumers at execution; if
    `cel.fns` embedding was its only client beyond tests, it is
    DELETE not KEEP (flagged for the dead-API audit).
  - The wasip1 core-module toolchain, `cel_runtime.wasm`, and the
    entire cel_host ABI — untouched.

## 3. Code inventory

### 3.1 DELETE — whole files / dirs / targets

| Path | What it is |
|---|---|
| `compiler/celfn/celfnc_emit/` (entire dir, 4,032 lines incl. tests + fixtures + codec_dumper) | WIT/codec/stub/skeleton emitters |
| `abi/plugin.{h,cc}`, `abi/plugin_test.cc` | `Plugin::Load`, cel.fns read side |
| `abi/plugin_validate.{h,cc}`, `abi/plugin_validate_test.cc` | plugin artifact validation |
| `eval/internal/cel_plugin.{h,cc}`, `cel_plugin_test.cc` | component instantiation + call path |
| `e2e/plugin_fixtures/` (entire dir) | demo plugin fixture |
| `e2e/plugin_dispatch_test.cc` | plugin dispatch e2e |
| `examples/09_plugin_functions.cc`, `examples/adder_fns.cc` + the `cel_wasm_plugin` targets in `examples/BUILD.bazel` | plugin example pair |
| `benchmark/plugin/` (entire dir) | plugin call-overhead bench |
| `benchmark/compiler/plugin_compile_bench.cc` | plugin compile bench |
| `tools/cel/run_generate.{h,cc}` | `cel generate` subcommand |
| `tools/cel/run_embed_decls.{h,cc}`, `run_embed_decls_test.cc` | `cel embed-decls` subcommand |
| `bindings/c/` (entire dir) | draft C-bindings header for components — **also a stale feature**: README says "not yet functional"; the only header is `cel_component.h` |
| `bazel/cel_wasm_plugin.bzl`, `bazel/plugin_rng_stub.c`, `bazel/BUILD.bazel` (dir if then empty) | build macro + RNG stub |
| `third_party/wit_bindgen/` | wit-bindgen toolchain dep |
| `third_party/wasm_tools/` | componentize step dep (only consumer: the macro) |
| wasip2 toolchain variant: `cc_toolchain_config.bzl` wasip2 blocks, platform defs, `wasm_cc_binary.bzl` transition, `wasm_clang.sh` -pthread strip if wasip2-only | second toolchain |
| `doc/user-guide/writing-plugins.md` | plugin author guide |

### 3.2 TRIM — plugin arm removed, host arm kept

| File | What to remove |
|---|---|
| `eval/engine.h` / `engine.cc` | `Use(const Plugin&)`, `AddPlugin(bytes, lib)`, all `WASMTIME_FEATURE_COMPONENT_MODEL` code (52 refs), plugin half of Plan-time required-fn verification + instantiation |
| `eval/internal/wasmtime_engine_state.{h,cc}` | `RegisteredPlugin`, `plugin_registry`, `wasmtime_component_t` fwd-decl |
| `eval/internal/required_fn_check.{h,cc}` + test | PLUGIN row handling |
| `eval/engine_test.cc` | plugin registration/conflict cases |
| `compiler/compiler.h` / `compiler.cc` | `Builder::Use(const Plugin&)`, `#include "abi/plugin.h"` |
| `compiler/internal/compile.{cc}` + test | kPlugin overload-table seeding / import synthesis |
| `compiler/celfn/function_library.{h,cc}` + test | `Backend::kPlugin` arm, WIT-name derivation (`DeriveWitInterface`), kPlugin validation rules |
| `compiler/celfn/celfn_parser_probe_test.cc` | kPlugin probe cases |
| `abi/celfn_wire.{h,cc}` + test | `@plugin.` prefix parse arm |
| `abi/cel_abi.proto` + `cel_abi_emit.{h,cc}` + test | `required_functions` PLUGIN row kind (pre-1.0: change the wire format, no compat shim) |
| `abi/program_facts.h/.cc`, `tools/cel/program_report.cc` | plugin fields in `cel inspect` output |
| `tools/cel/cel.cc` | `generate` / `embed-decls` subcommand wiring, flags, help text |
| `tools/cel/cel_smoke_test.sh`, `examples/examples_smoke_test.sh` | plugin invocations |
| `e2e/foreign_fn_type_matrix_test.cc` | plugin-backend rows (155 refs) — keep host-backend rows |
| `e2e/known_bugs_test.cc` | plugin-related pins (6 refs) — delete pins whose feature is gone |
| `eval/internal/{abi_decode_test,cel_host_test}.cc`, `instance_impl.h`, `compiler/codegen/overload_table.h/_test`, `compiler/frontend/parse_and_check.cc`, `shared/type.h`, `examples/04_host_functions.cc` | comment/ref cleanups; overload-table plugin origin if present |
| `scripts/refresh_compile_db.sh` | plugin-fixture path refs |
| `MODULE.bazel` | wit_bindgen / wasm_tools repo refs |

### 3.3 KEEP — false positives

`abi/internal/sha256.h` (used by Plugin::hash — check for other
users; if none, moves to DELETE), `examples/04_host_functions.cc`
(host path, comment tweak only), `abi/wasm_binary*` (pending §2
verification).

## 4. Commit plan (leaf-first; tree green after every commit)

1. **e2e + examples + benches**: delete plugin fixtures, dispatch
   test, type-matrix plugin rows, example 09 + adder, benchmark
   plugin targets, smoke-test refs.
2. **CLI**: delete `generate` + `embed-decls`, trim `cel.cc`,
   `program_report`, smoke tests.  Docs for `tools/cel` in the same
   commit (docs-ship-with-change rule).
3. **eval layer**: engine Use/AddPlugin + component code,
   cel_plugin, engine-state, required_fn_check PLUGIN rows, tests.
4. **compiler + abi layer**: Builder::Use, compile.cc seeding,
   celfnc_emit dir, function_library kPlugin arm, celfn_wire arm,
   cel_abi.proto PLUGIN kind, plugin.{h,cc} + validate, bindings/c.
5. **toolchain**: build macro, RNG stub, wit_bindgen, wasm_tools,
   wasip2 toolchain variant, MODULE.bazel.
6. **docs sweep** (§5) + diagram regeneration.
7. **dead-API audit** (§6) — separate commits per surface.
8. **stale-feature inventory** (§7) — doc only.

Each commit: `scripts/lint.sh` on touched files; `bazel test` on
touched packages.  Final gate (task #19): `bazel build //...`,
`bazel test $PROJ` + the manual-tagged catalog, `lint.sh --branch`,
conformance monotonic both modes, `bug_pins.py validate`, and a
zero-hit grep for `wit|wasip2|component|plugin` outside
`third_party/cel-cpp`, archive-branch docs, and history notes.

## 5. Docs inventory — per-doc deltas

DELETE: `doc/user-guide/writing-plugins.md`.

REWRITE — what each doc says after m39:

| Doc | Delta |
|---|---|
| `doc/design/05-custom-functions.md` | Becomes the host-callback design doc.  §§ on the celfnc pipeline, WIT type-mapping tables, the four emitters, the build macro, `Plugin::Load`, plugin dispatch, and selective instantiation are removed.  The overload-id identity chain, `.celfn` IDL, `FunctionLibrary`, `BindFunction` validation, and the host-call adapter layers stay and become the whole story.  Backend enum documented as host-only. |
| `doc/design/00-architecture.md` | Component backend removed from the architecture overview, module table, and trust story; custom functions described as host callbacks; pointer to the archive branch for the removed backend. |
| `doc/design/02-evaluator.md` | `Engine` surface loses `Use(Plugin)` / `AddPlugin`; plugin registry, per-Plan component instantiation, and the plugin half of required-fn verification removed from the Plan walkthrough. |
| `doc/design/06-testing-strategy.md` | Plugin fixture/e2e strategy rows removed; the foreign-fn type-matrix described as host-backend-only. |
| `doc/design/07-benchmarking.md` | Plugin bench section removed. |
| `doc/design/08-abi-wire-format.md` | `required_functions` documented without the PLUGIN row kind (wire-format change, pre-1.0). |
| `doc/design/notes/{00-consolidated-findings,celfn,eval-public}.md` | Component findings marked resolved-by-removal; eval-public loses the plugin surface. |
| `doc/implementation-plan/rewrite/design.md` | The 3 component refs updated to host-only custom functions. |
| `doc/user-guide/custom-functions.md` | Rewritten host-only; opens with the `AddTypedFunction` runnable example; states plainly that sandboxed wasm plugins are not offered (limitation-in-place rule). |
| `doc/user-guide/writing-host-functions.md` | Absorbs any still-true content from `writing-plugins.md`; becomes the single custom-fn how-to. |
| `doc/user-guide/security-model.md` | Sandboxing claims scoped to the expression/runtime wasm only; the "sandboxed plugin code" claim removed. |
| `doc/user-guide/{faq,getting-started,index}.md`, `doc/index.md`, `doc/README.md` | Plugin mentions removed; feature lists updated. |
| Root `README.md`, `tools/cel/README.md` | Feature lists + CLI subcommand tables lose `generate`/`embed-decls`/plugins (both tellings — site and GitHub). |
| `PROPOSALS.md` | Decision record added (what/why/alternatives/archive pointer). |

TRIM refs: `tools/celwasm-tool/DESIGN.md` (~10 plugin/component
mentions — found by D2, missed by the original inventory),
`current-capabilities.md`, `feature-pipeline-checklist.md`
(the "new plugin fn" feature-type section removed),
`per-component-test-coverage.md` (plugin targets out of the manual
catalog), `testing-checklist.md` (plugin rows marked removed, not
ticked), `cleanup-backlog.md` (plugin items closed as
resolved-by-removal), `lint-backlog.md`, `modules-and-ffi.md`.

REGENERATE: `doc/design/diagrams/` via `render.py` (dependency graph
+ trust boundary lose the component nodes); `doc/img/pipeline-*.svg`
if they show the plugin path.

ANNOTATE (history docs — do not rewrite): `m13-custom-fns.md`,
`m22-foreign-fn.md`, `m23/m24-…component….md`,
`m26-celfnc-and-component-build.md`, `m35-*.md`,
`m36-cli-runtime-and-lazy-binding.md` (plugin-run sections),
`celfn-go-bindgen-design.md`, `foreign-go-bindgen-findings.md`,
`reviews/2026-07-25-m35-closeout.md`: add a status-line note
"backend removed 2026-08-04 (m39); archived on
`component-functions-archive`".

`PROPOSALS.md`: add the decision record (what was removed, why, the
alternatives menu for phase 2, pointer to the archive branch).

## 6. Dead-API audit (curated public surface)

Method: for each public target
(`//compiler:{compiler,program}`, `//eval:{engine,instance,activation,
value,error,attribute,host_call_context,typed_function}`,
`//shared:type`, `//abi:*`, `//runtime:*`), list exported symbols;
delete any with zero non-test first-party callers, per the pre-1.0
"delete rather than deprecate" rule.  Known candidates to verify:

  - `abi/wasm_binary` + `abi/internal/sha256.h` (post-removal
    orphans?)
  - `Engine::AddPlugin`'s escape-hatch rationale dies with plugins —
    gone in §3.2 regardless.
  - Anything `ABSL_CHECK(false)`-stubbed with no owning milestone
    (the `Activation::OverrideFunction` precedent).
  - `abi/program_facts` fields only `cel inspect` printed for
    plugins.

Each deletion: its own commit, callers fixed in the same commit,
docs in the same commit.

## 7. Stale/partial-feature inventory (deliverable)

A new doc `doc/implementation-plan/stale-feature-inventory.md`
listing every partially-working surface with a ship/finish/remove
recommendation, built from:

  - `scripts/bug_pins.py list` (CELBUG queue, severity-ordered)
  - `scripts/bug_pins.py skips` — `deferred-feature` CELSKIPs
  - grep `is a stub until` (ABSL_CHECK(false) stubs + owning
    milestone)
  - `PROPOSALS.md` open entries (re-verified against HEAD)
  - `cleanup-backlog.md` open items
  - `bindings/c/` (already identified: drafted, non-functional —
    deleted with this milestone)

Owner triages the inventory; nothing partially-working ships in the
Google PR without an explicit line in this doc.

## 7b. Work items (tick as they land; mirrors m39-dag.md status)

In-milestone:

  - [x] P0 plan + DAG docs
  - [x] S1 stale-feature inventory (`stale-feature-inventory.md`, 478c3ec)
  - [x] D1 e2e/examples/benches deletion (a3f5c62)
  - [x] D2 CLI deletion (fda6124)
  - [x] D3 eval layer (+204/−3835)
  - [x] D4 compiler/abi/bindings + @native retirement (+242/−5589)
  - [x] D5 toolchain: wit-bindgen, wasm-tools, wasip2, macro (+16/−840)
  - [x] DOC1 historical milestone annotations (13 docs)
  - [x] DOC2 design/user-guide/README/PROPOSALS rewrite (30 files)
  - [x] F2 stale-skip un-skips, observed green both link modes
  - [x] F1 — closed NO-CHANGE (premise stale; see decisions log)
  - [x] Master reconcile after #38/#39 merged (rebase onto 9a85cd8)
  - [x] Header-comment stub-claim fixes (value.h, cel_string_format.h)
  - [x] A1 dead-API audit (5 commits, net −213; kUser +
        wasm_import_module_name, WasmLayer::kComponent, 3 toolchain
        knobs, RequiredFn::is_host, abi/wit/ all deleted by census;
        ~10 doc-promised surfaces kept with reasons; 157/157 sweep)
  - [ ] DOC3 diagrams regen (render.py: drop plugin edges,
        trust-boundary SVGs) + final doc pass + close backlog #41 as
        fixed-by-#10 + correct stale-feature-inventory §5/summary
  - [ ] G gate: `lint.sh --branch`, `bazel test $PROJ` + manual
        catalog, conformance monotonic BOTH modes, `bug_pins.py
        validate`, zero-hit grep, push

Post-gate (owner-scheduled, in order):

  - [ ] CI adjustments: clean dead plugin greps in
        `refresh_compile_db.sh` remnants if any survive A1; publish
        the warm CI image via `workflow_dispatch` AFTER m39 merges
        (stamp digests MODULE.bazel — publishing earlier warms a
        stale dep set); one green run of all lanes on the post-m39
        tree.  `ci.yml` itself needs no edits (query-driven targets).
  - [ ] Toolchain analysis agent: WASI import-surface trim
        feasibility; threads-triple cost quantification (absl mutex
        via cctz AND self-hosted RE2 — keep is the default).
  - [ ] Host-fn error carriage decision (PROPOSALS #2 /
        backlog #31): ABI carriage vs documented code-only contract;
        example 08 asserts the current drop.
  - [ ] Owner triage of `stale-feature-inventory.md` FINISH bucket —
        chiefly execution cost limits (fuel/epoch), or soften the
        security-model claims before any upstream submission.

## 8. Branch / PR mechanics

Working branch: `rip-out-components`, based on `pr3-e2e-tests`
(#38 + #39 content included), so it stacks cleanly: owner merges
#38 → #39 → this becomes a normal master-based PR.  If #38/#39 are
instead abandoned, this branch already contains their content.

## 9. Risks / open verifications

  - `foreign_fn_type_matrix_test.cc` split (host vs plugin rows) is
    the largest trim — verify host rows still compile alone.
  - `cel_abi.proto` PLUGIN-kind removal changes the wire format —
    loud break, fine pre-1.0, but conformance + e2e must re-run.
  - `wasm_binary` keep-vs-delete decided by real caller census, not
    assumption.
  - The wasip2 toolchain removal must not disturb the wasip1
    toolchain registration (same file).

## 10. Phase 2 pointer (not in scope)

Alternatives menu if a sandboxed-plugin backend returns:
(A) isolated wasip1 core module + frozen CelValue-layout call ABI —
recommended; (B) link-time composition via kStatic-style fusion;
(C) shared-memory dynamic linking — not recommended (immature
tooling); (D) components with hand-rolled bindings — not recommended
(keeps the boundary restrictions).  Archive branch:
`component-functions-archive`.
