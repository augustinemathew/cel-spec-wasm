# Code ↔ doc index

Purpose: when reading code anywhere in the tree, this index says which
docs pair with it — read them together. Built 2026-06-10 from the full
doc inventory (~130 files). Status tags: **[live]** current truth,
**[hist]** shipped plan / historical intent (read for *why*, trust code
for *what*), **[ref]** upstream/reference. Paths under `doc/` unless
noted; `rw/` = `doc/implementation-plan/rewrite/`.

This is a working artifact of the design rebuild; it graduates into the
`doc/README.md` router during the consolidation pass.

## By code area

| Code area | Paired docs |
|---|---|
| `compiler/compiler.h`, `program.h`, `compiler/internal/` | rw/two-phase-runtime-isolation.md **[hist]** (the Compiler/Program/Engine/Instance split rationale); rw/m28-configurable-linking.md **[hist]** (link_mode); compiler-overview.md **[live]** |
| `compiler/frontend/` (parse_and_check, status_tags) | rw/m2-ident-select-unknowns.md **[hist]**; rw/dyn-passthrough-plan.md **[hist]** (RejectDyn gate); rw/m9-type-subsystem.md **[hist]**; rw/m10-conversions.md **[hist]** |
| `compiler/ir/` (typed_ast, annotations) | rw/design.md §IR/annotations **[hist]**; rw/m9-type-subsystem.md **[hist]** |
| `compiler/codegen/` memory side (resolve_pass, layout_pass, static_memory_builder, slot_allocator) | rw/memory-layout-design.md **[live]** (THE memory model — single authoritative); rw/map-list-dispatch.md **[hist]** (three-path dispatch) |
| `compiler/codegen/` lowering side (expr_lower*, overload_table, module) | rw/wat-traces.md **[live-ref]** (one WAT trace per codegen arm, regression-run); rw/m5-kcall-comprehensions.md + m5-comprehensions-followon.md **[hist]**; rw/cross-numeric-ordering-plan.md **[hist]**; rw/slice2-control-flow-plan.md **[hist]** |
| `compiler/celfn/` (IDL, function_library, library_module) | rw/m13-custom-fns.md **[hist, partially live]** (foreign backend still open); rw/modules-and-fnis.md **[live]**; user-guide/writing-host-functions.md **[live]** |
| `compiler/celfn/celfnc_emit/` (wit/codec/skeleton/stub emitters) | rw/m24-foreign-fn-component-backend.md §6 **[hist]**; rw/m26-celfnc-and-component-build.md **[plan, unstarted]**; user-guide/writing-component-functions.md **[live]** |
| `eval/` public (engine, instance, activation, value, error, attribute) | rw/two-phase-runtime-isolation.md **[hist]**; rw/m21-host-call-adapter.md **[hist]** (HostCallContext / typed fn layers); user-guide/index.md **[live]**; user-guide/faq.md **[live]** (thread-safety answers) |
| `eval/internal/` (cel_host*, abi_decode, cel_component, wasmtime glue) | rw/cel-host-surface.md **[live]** (THE host/guest wire contract); rw/m24-foreign-fn-component-backend.md **[hist]** (component dispatch) |
| `eval/host/cel_log*` | rw/cel-host-surface.md §logging **[live]** |
| `runtime/` (C kernel) | rw/memory-layout-design.md **[live]**; rw/wasi/DESIGN.md **[live]** (malloc/WASI migration + Phase C deltas); rw/cel-runtime-c-split-plan.md **[hist]** (why the TU split); implementation-plan/runtime-catalogue-genrule.md **[live]** (export catalogue) |
| `abi/` (cel.abi emit, runtime_catalogue, wit/) | rw/abi-refactor.md **[hist]**; rw/cel-host-surface.md **[live]**; abi/wit/README.md **[live]** |
| `shared/type.h` | rw/m9-type-subsystem.md **[hist]** |
| Extension kernels: strings | rw/m12-string-ext.md **[hist]** |
| — math | rw/m16-math-ext.md + m16-ast-probe-findings.md **[hist]** |
| — encoders | rw/m17-encoders-ext.md **[hist]** |
| — network | rw/m18-network-ext.md + m18-ast-probe-findings.md **[hist]** |
| — optionals | rw/m14-optionals.md **[hist]** |
| — Any / duration / timestamp / wrappers | rw/m7a-any.md, rw/m7b-duration-timestamp.md, rw/m8-wrapper-types.md **[hist]** |
| — enums | rw/m20-enum-field-range.md **[hist]** |
| `conformance/` | conformance/README.md **[live, auto-regen]**; rw/conformance-unlock-plan.md **[hist]** |
| `e2e/`, test discipline anywhere | implementation-plan/testing-checklist.md **[live]**; implementation-plan/per-component-test-coverage.md **[live]** (manual-target catalog + closeout gate); rw/feature-pipeline-checklist.md **[live]**; implementation-plan/known-issues-findings.md **[live]** |
| `testdata/cel_cpp_oracle*` | CLAUDE.md §oracle **[live]** |
| `benchmark/` | benchmark/README.md **[live]**; bench/README.md **[archived]** at rw/archive/bench-tree-readme.md (bench/ dissolved into benchmark/ 2026-06-11); benchmark/DESIGN.md **[live-plan]**; rw/m28-bench-results.md **[live]**; rw/wasi/POST_MIGRATION_BENCH.md **[hist]** |
| `tools/cel/` | tools/cel/README.md **[live]**; rw/cel-cli-design.md **[plan]** (`cel run`, unbuilt) |
| `tools/wat_runner/` | CLAUDE.md §WAT-first **[live]**; rw/wat-traces.md **[live-ref]** |
| `examples/` | examples/README.md **[live]**; user-guide/getting-started.md **[live]** |
| CEL semantics anywhere | langdef.md **[ref, authoritative]**; extensions/strings.md **[ref]** |
| Process: lint, build, dev loop | CLAUDE.md **[live]**; contributing.md **[live]**; implementation-plan/lint-backlog.md **[live]**; implementation-plan/dev-loop-performance.md **[live]**; implementation-plan/cleanup-backlog.md **[live]** |

## Reverse index: docs cited FROM code comments

These doc paths appear in C++ comments — moving/renaming any of them
requires a same-commit comment update: abi-refactor.md,
cel-host-surface.md, m13-custom-fns.md, m21-host-call-adapter.md,
m24-foreign-fn-component-backend.md, m28-configurable-linking.md,
m5-kcall-comprehensions.md, m7b-duration-timestamp.md,
map-list-dispatch.md, phase-c-plan.md, two-phase-runtime-isolation.md,
wasi/DESIGN.md (all under rw/).

## Known-stale warnings (do not trust without checking code)

- rw/design.md — declared **historical artifact** (2026-06-10); use the
  section map in `design-heritage.md` notes once written.
- rw/m11-cel-host-refactor.md — says "in flight", slices B–I; verify.
- rw/m22-foreign-fn.md / m26 — plans; m24 shipped a v1 that diverges.
- Redirect stubs (predecessor-*, m-custom-fns.md) — 5 files, pure
  tombstones.
