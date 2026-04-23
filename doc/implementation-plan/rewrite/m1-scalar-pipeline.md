# Rewrite M1 — scalar-literal pipeline, full abstraction skeleton

Status: **plan — drafted 2026-04-21, not yet started.**

Parent: `design.md` (this directory).
Milestone boundary in the parent: **merges Slices 1 + 2 + 3** into
one deliverable. M1 is the first of a planned `M1..Mn` sequence; each
subsequent milestone adds one language capability against a frozen
abstraction skeleton.

## 0. Why one milestone, not three

The parent plan splits bootstrap / all-literals / symbol-table into
three slices because that's how they'd land as independent PRs. For
an *abstraction-first rewrite*, splitting is the wrong axis:

  - Slice 1 without Slice 3 produces a toy evaluator whose codegen
    is shaped around `kConst` alone — the pass pipeline doesn't
    exist, so the "codegen as pure translation" invariant can't be
    demonstrated.
  - Slice 3 without Slices 1–2 lands empty passes over nothing.
  - Proving the abstractions work end-to-end requires all three
    together: the pipeline has to flow from `parse` through every
    new pass down to a wasm module that a real runtime evaluates.

M1 therefore lands the **full skeleton** (every pass, every new
type, every directory) **plus** the minimum codegen arm (`kConst`)
needed to run it end-to-end against all seven scalar literal kinds.
No call arm. No ident arm. No select arm. Those are later milestones
filling in the skeleton — not restructuring it.

## 1. Scope

### 1.1 What works end-to-end after M1

```
$ bazel run //compiler_v2/cli:celwasmc_v2 -- -e "42"
42
$ bazel run //compiler_v2/cli:celwasmc_v2 -- -e "true"
true
$ bazel run //compiler_v2/cli:celwasmc_v2 -- -e '"hello"'
"hello"
$ bazel run //compiler_v2/cli:celwasmc_v2 -- -e 'b"bytes"'
b"bytes"
$ bazel run //compiler_v2/cli:celwasmc_v2 -- -e "3.14"
3.14
$ bazel run //compiler_v2/cli:celwasmc_v2 -- -e "42u"
42u
$ bazel run //compiler_v2/cli:celwasmc_v2 -- -e "null"
null
```

Every one of these goes through the full pipeline:

```
parse → check → ResolvePass → LayoutPass → expr_lower → Binaryen module
  → cel::Engine::Plan two-phase instantiate → cel::Instance::Eval → Value
```

### 1.2 Out of scope (deferred to M2+)

| Capability | Parent-doc slice | Deferred to |
|---|---|---|
| `kIdent` / parameter binding | Slice 4 | M2 |
| `kSelect` / proto field read | Slice 4 | M2 |
| `kCall` (built-ins) | Slice 5 | M3 |
| `cel_runtime` arithmetic helpers | Slice 5 | M3 |
| `OverloadTable::kBuiltinSeeds` content | Slice 5 | M3 |
| `has()` + message equality | Slice 6 | M4 |
| Custom functions + `cel_host_call_custom` | Slice 7 | M5 |
| Map / list literals | Slice 8 | M6 |
| Proto literals | Slice 9 | M7 |
| Sethi–Ullman slot-Strahler | Slice 10 | M8 |
| `cel_refs` externref table | — | M4 (with `has()`) |
| `cel_host` fixed-surface imports | Slice 4 | M2 |

M1 leaves stubs / empty tables where these will land, so M2 is
"fill in this arm" rather than "refactor the pipeline".

## 2. Abstractions introduced in M1 (long-lasting shapes)

These interfaces are **frozen at M1** — M2+ milestones populate
their internals but do not reshape them. Any M1 decision that
later turns out to constrain a milestone is a design bug in M1,
not in the milestone that hit it.

### 2.1 `ir/annotations.h` — `NodeAnnotation` with full schema

Per parent §4.1, fields present and declared at M1 even though most
are zero sentinels until their milestone:

```cpp
enum class Repr : uint8_t { kUnknown = 0, kBool, kInt, kUint, kDouble,
                            kNull, kString, kBytes, /* kMessage, kList,
                            kMap land when they ship */ };

enum class StorageKind : uint8_t { kNone, kStaticRodata, kWorkspaceSlot,
                                   kLocal };
struct Storage { StorageKind kind = StorageKind::kNone; uint32_t payload = 0; };

struct NodeAnnotation {
  Repr     repr           = Repr::kUnknown;
  uint32_t field_number   = 0;  // M2 SelectExpr
  uint32_t overload_id    = 0;  // M3 CallExpr
  uint32_t local_index    = 0;  // M2 IdentExpr
  uint32_t scope_id       = 0;  // comprehensions, later
  Storage  storage;
};

class WasmAnnotations {  // same side-map API as v1's
 public:
  NodeAnnotation& Mutable(int64_t expr_id);
  const NodeAnnotation* Find(int64_t expr_id) const;
 private:
  absl::flat_hash_map<int64_t, NodeAnnotation> map_;
};
```

**M1 populates:** `repr` (for `kConst` nodes only) and `storage`
(every `kConst` gets `{kStaticRodata, rodata_offset}`).
**M1 asserts at end-of-pipeline:** non-`kConst` kinds produce
`Unimplemented` from codegen — not crash, not silent default.

### 2.2 `codegen/overload_table.{h,cc}` — builder + frozen table

Full class per parent §4.3, `kBuiltinSeeds` is an **empty array** in
M1. `OverloadTableBuilder::RegisterCustom` is present and its
`AlreadyExists` rule works (the test exercises it against custom-
vs-custom collision — no built-ins to collide with yet).

The table exists at M1 so that M3's "turn on the overload set" lands
as a data-only change (fill `kBuiltinSeeds`), not a
new-header-plus-new-BUILD-target change.

### 2.3 `codegen/resolve_pass.{h,cc}` — the pipeline stage

Full interface per parent §5.1. M1 implementation walks the
`TypedAst` once and:

  - Writes `repr` for `kConst` nodes (mapping const kind → `Repr`).
  - Leaves every other field at its zero sentinel.
  - DCHECKs that every `kConst` now has a non-`kUnknown` `repr`.

Scope stack (§4.8) is declared but empty — M5 fills it. Overload
interning is a stub that returns `0` (no `kBuiltinSeeds` entries).

### 2.4 `codegen/layout_pass.{h,cc}` — the pipeline stage

Full interface per parent §6.1. M1 implementation:

  - Walks the AST, for each `kConst` calls the matching
    `StaticMemoryBuilder::Allocate*` and writes
    `{kStaticRodata, offset}` into the node's `Storage`.
  - For non-`kConst` nodes, leaves `storage.kind == kNone`. Codegen
    will see this and error.
  - Invokes `SlotAllocator` with its naive-only mode — no slot is
    ever acquired in M1 because there are no workspace-slot nodes;
    the allocator exists only so the interface is frozen.

`LayoutOptions::debug_layout` field declared; has no effect yet
(slot allocator is naive-only regardless).

### 2.5 `codegen/static_memory_builder.{h,cc}`

Full class per parent §6.2.1. M1 ships:

  - `AllocateNull()` / `AllocateBool(bool)` / `AllocateInt(int64_t)` /
    `AllocateUint(uint64_t)` / `AllocateDouble(double)` — each emits a
    24-byte `CelValue` frame into the buffer, 8-byte aligned, and
    returns the frame's absolute linear-memory offset.
  - `AllocateString(absl::string_view)` / `AllocateBytes(absl::string_view)` —
    emits a 24-byte `CelValue` frame (span payload = `(offset,
    length)` pointing at bytes appended immediately after the frame)
    and pads the cursor back to 8-byte alignment for the next
    Allocate.
  - `Finalize() &&` returns the owned `std::vector<uint8_t>`.

`AllocateList` / `AllocateMap` exist as signature-final stubs
returning `uint32_t`; their M1 body `ABSL_CHECK(false)`s so an
accidental early call surfaces as a crash, not a silent
miscodegen.  Body fills in at M5/M6.

### 2.6 `codegen/slot_allocator.{h,cc}`

Per parent §6.2.2, M1 ships the naive path only:

  - `Acquire()` returns a fresh 24-byte offset, monotonically
    increasing. `Release()` is a no-op.
  - `PushScope()` / `PopScope()` are no-ops.
  - `peak_slots()` / `total_bytes()` return the accurate counts.

Sethi–Ullman logic is **not** written in M1. That's deferred to
the milestone that needs it (M8 per the parent plan).

### 2.7 `codegen/expr_lower.{h,cc}`

Interface per parent §7.1. M1 implementation has one arm:

```cpp
switch (expr.kind()) {
  case kConst:
    DCHECK(a.storage.kind == StorageKind::kStaticRodata);
    return EmitStorageLoad(ctx, a.storage);  // i32.const <offset>
  default:
    return absl::UnimplementedError(
        absl::StrCat("expr kind ", ExprKindName(expr.kind()),
                     " not supported before M", RequiredMilestone(expr.kind())));
}
```

`EmitStorageLoad` for `kStaticRodata` is trivially
`BinaryenConst(ctx.mod, BinaryenLiteralInt32(storage.payload))` —
the `i32` CelValue offset, which `$eval`'s caller reads as a
pointer into linear memory.

### 2.8 Runtime (`runtime/cel_*.h` + `cel_runtime.c`)

M1 ships the **minimum** consistent with the final memory model.
The runtime surface is split into narrow topic headers, each
depending on the smallest set of siblings it needs; a thin
umbrella (`cel_runtime.h`) re-exports all of them for callers that
want the whole thing:

  - `cel_data.h` — pure types: `CelKind`, `CelSpan`/`CelArray`/
    `CelMap`/`CelDurTs`, `CelValue` struct (24 bytes, asserted
    via `_Static_assert`), `CEL_ERR_*` enum. No function decls.
    Depends on `<stdint.h>` only.
  - `cel_memory.h` — `cel_mem_base()` / `cel_mem_size()` only.
    The rest of the runtime reads from these; pulling them into
    their own header means a test exercising just "does the
    backing memory exist" doesn't transitively depend on the
    arena or any CelValue constructors.
  - `cel_arena.h` — `cel_reset(arena_base, arena_limit)`,
    `cel_alloc(nbytes)`, `cel_value_at(off)`. Arena cursor lives
    at fixed linear-memory bytes 8..15 (parent §8.2). **Codegen
    emits a call to `cel_reset(<rodata_size>, <mem_size>)` at the
    top of every `$eval` body using compile-time-constant
    offsets** — there is no separate host init phase and no
    runtime-private state to initialize before calling `$eval`.
  - `cel_make.h` — `cel_make_{bool,int,uint,double,null,string,
    bytes,string_view,bytes_view}`. Used only by tests and by
    host-boundary boxing; not called from M1 codegen (literals go
    straight into `.rodata`). Included in the runtime so arena-
    aware tests can construct CelValues without reaching into the
    layout manually.
  - `cel_log.h` — `cel_log` import declaration (with wasm-side
    `import_module`/`import_name` attributes), format-tag enum,
    `CEL_LOG_*` macros, `CEL_LOG(...)` dispatch macro.
  - `cel_runtime.h` — umbrella that `#include`s all five.

Build flags per parent §8.1: `--import-memory=cel,memory`,
explicit `--export=` per symbol, **no `--export-all`**. Exports
at M1: `cel_alloc`, `cel_reset` (so the runtime-module variant
is callable from the expr module via the shared runtime-import
binding; M1's `$eval` calls `cel_reset` itself, then never calls
`cel_alloc` because pure-literal eval hits only `.rodata`).

Helpers intentionally **not** in M1: arithmetic (`_add_at_vv`),
comparisons, string ops, 3VL, size, map/list primitives. Those
land with the milestones that add the corresponding codegen arms.

### 2.9 Host runtime (`api/engine.{h,cc}`, `api/instance.{h,cc}`)

`host/host_loader.{h,cc}` was deleted in the
two-phase-runtime-isolation work (see
`rewrite/two-phase-runtime-isolation.md`).  The runtime side is now
split across `cel::Engine` (process-level wasmtime fixture) and
`cel::Instance` (per-Plan execution handle).

Topology — both expr and `cel_runtime.wasm` import a host-allocated
`cel.memory` (the smoke test on the experiment branch confirmed
this works as designed):

  - Engine setup (one-time per process): `wasm_engine_new` +
    `wasmtime_module_new(cel_runtime.wasm)` cached on the
    `WasmtimeEngineState` shared-state struct.
  - Per-Plan: `wasmtime_store_new` + `wasmtime_memory_new` (host-
    allocates 2-page memory) + `wasmtime_linker_new` +
    `RegisterCelLog` + `linker_define(cel.memory)` +
    `wasmtime_linker_instantiate(runtime)` +
    `linker_define(cel.cel_reset / cel.cel_alloc)` from runtime's
    exports + `wasmtime_module_new(expr_bytes)` +
    `wasmtime_linker_instantiate(expr)` + lookup `eval`.
  - Per-Eval: `wasmtime_func_call($eval)` returns `i32` offset of
    a `CelValue`; `Instance::Eval` reads 24 bytes from the
    host-owned memory and decodes into `cel::Value`.

`$eval`'s first instruction is still `cel_reset(arena_base,
arena_limit)` (compile-time-baked) so per-Eval arena reset is
free — the host does not call `cel_reset` itself.

Wiring order chosen to side-step the expr↔runtime "circular
import" 040c043 cited: trampolines + `cel.memory` are bound on
the linker before either module instantiates, so neither has to
exist before the other.

Bench-justified caching cuts (per
`rewrite/two-phase-runtime-isolation.md §6`):

  - Caching parsed `cel_runtime.wasm` on the engine: ~34× per-Plan
    speedup (186 µs → 5.5 µs in the experiment).
  - Sharing the engine across the process: ~64× cold-to-hot
    (351 µs → 5.5 µs).
  - Per-Plan cost in production: ~12 µs hot path; ~162 µs when
    Plan re-parses the expr bytes per call (M1 default — an
    Engine-side parsed-expr-module cache is the named follow-up).

Host does **not** call `cel_alloc` for an sret slot — the output
slot is a fixed offset baked into `$eval`'s return value.

### 2.10 `cel.abi` custom section

M1 emits a minimal section:

```
{
  rodata_base:     u32,
  rodata_size:     u32,
  workspace_base:  u32,
  workspace_size:  u32,
  arena_base:      u32,
  result_offset:   u32,   // CelValue offset $eval returns
  eval_fn_name:    string
}
```

Extended in later milestones (`custom_functions[]` at M5, etc.).
The section is a fixed-offset header — M1 reserves field positions
for growth; extensions append tagged blocks.

## 3. Source layout (M1 deliverables)

```
compiler_v2/
├── BUILD.bazel                              # root filegroup
├── ir/
│   ├── BUILD.bazel
│   ├── annotations.h                        # NEW — full NodeAnnotation schema
│   ├── annotations.cc                       # NEW
│   ├── annotations_test.cc                  # NEW
│   ├── typed_ast.h                          # PORT v1 verbatim
│   ├── typed_ast.cc                         # PORT v1 verbatim
│   └── typed_ast_test.cc                    # PORT v1 verbatim
├── frontend/
│   ├── BUILD.bazel
│   ├── parse_and_check.h                    # PORT v1 + absorb v1's ir/static_subset
│   ├── parse_and_check.cc                   # PORT v1; RejectDyn runs inline after check
│   └── parse_and_check_test.cc              # PORT v1 + absorb static_subset_test cases
├── codegen/
│   ├── BUILD.bazel
│   ├── overload_table.h                     # NEW — builder + frozen table
│   ├── overload_table.cc                    # NEW — kBuiltinSeeds = {} for M1
│   ├── overload_table_test.cc               # NEW — collision rule only
│   ├── resolve_pass.h                       # NEW — full interface
│   ├── resolve_pass.cc                      # NEW — kConst repr only
│   ├── resolve_pass_test.cc                 # NEW
│   ├── layout_pass.h                        # NEW — full interface
│   ├── layout_pass.cc                       # NEW — kConst → kStaticRodata only
│   ├── layout_pass_test.cc                  # NEW
│   ├── static_memory_builder.h              # NEW
│   ├── static_memory_builder.cc             # NEW — every scalar kind
│   ├── static_memory_builder_test.cc        # NEW — per-kind byte layout
│   ├── slot_allocator.h                     # NEW — naive only
│   ├── slot_allocator.cc                    # NEW
│   ├── slot_allocator_test.cc               # NEW — Acquire monotonic
│   ├── expr_lower.h                         # NEW
│   ├── expr_lower.cc                        # NEW — kConst arm only
│   ├── expr_lower_test.cc                   # NEW
│   ├── module.h                             # NEW — memory + data segment wiring
│   ├── module.cc                            # NEW
│   └── module_test.cc                       # NEW
├── runtime/
│   ├── BUILD.bazel                          # --import-memory, explicit exports
│   ├── cel_data.h                           # NEW — types only (CelValue, CelKind)
│   ├── cel_memory.h                         # NEW — cel_mem_base / cel_mem_size
│   ├── cel_arena.h                          # NEW — cel_reset / cel_alloc / cel_value_at
│   ├── cel_make.h                           # NEW — cel_make_{bool,int,…,string_view}
│   ├── cel_log.h                            # NEW — cel_log import + CEL_LOG_* macros
│   ├── cel_runtime.h                        # NEW — umbrella re-export
│   ├── cel_runtime.c                        # NEW — arena @ bytes 8/12
│   ├── cel_arena_test.cc                    # NEW — reset/alloc/value_at + layout asserts
│   ├── cel_memory_test.cc                   # NEW — base/size/alignment
│   ├── cel_make_test.cc                     # NEW — per-kind make_* round-trip
│   ├── cel_runtime_wasm_bytes.h             # NEW (generated from .wasm)
│   └── wasm_imports.txt                     # NEW — cel_log only at M1
├── api/                                    # SHIPPED in two-phase-runtime-isolation
│   ├── BUILD.bazel                         #   slice — see that doc for the
│   ├── compiler.{h,cc,_test.cc}            #   reasoning + bench data
│   ├── program.{h,_test.cc}
│   ├── engine.{h,cc,_test.cc}              #   formerly host_loader role (Engine half)
│   ├── instance.{h,cc,_test.cc}            #   formerly host_loader role (Instance half)
│   ├── cel_pipeline_bench.cc
│   └── internal/
│       ├── wasmtime_engine_state.{h,cc}
│       └── instance_impl.{h,cc}
├── host/
│   ├── BUILD.bazel
│   ├── cel_log.h                            # PORT v1 verbatim
│   ├── cel_log.cc                           # PORT v1 verbatim
│   └── cel_log_test.cc                      # PORT v1 verbatim
├── cli/
│   ├── BUILD.bazel
│   └── celwasmc_v2.cc                       # NEW — entry point
└── e2e/
    ├── BUILD.bazel
    └── eval_test.cc                         # NEW — one test per scalar kind
```

**Not in M1** (deferred, no placeholder files):
  - `host/cel_host.{h,cc}` — M2, when `kSelect` lands
  - `host/attribute.{h,cc}` — M4, partial-eval
  - `testdata/*.proto` — M2, when proto fixtures are needed
  - `bench/eval_bench.cc` — M8, with Sethi–Ullman parity
  - `ir/wasm_annotations*.{h,cc}` from v1 — its role is filled by
    `annotations.h`'s `WasmAnnotations`

## 4. What gets ported verbatim from v1

Each item is a `cp` from v1 to the v2 tree **without edits**. BUILD
target names adjust (`//compiler/ir:typed_ast` →
`//compiler_v2/ir:typed_ast`); file contents do not.

| v1 path | v2 path | Why safe to port verbatim |
|---|---|---|
| `compiler/ir/typed_ast.{h,cc,_test.cc}` | `compiler_v2/ir/` | Thin wrapper over cel-cpp's `CheckedExpr`; stable, no M1 design depends on changing it |
| `compiler/frontend/parse_and_check.{h,cc,_test.cc}` + `compiler/ir/static_subset.{h,cc,_test.cc}` | `compiler_v2/frontend/parse_and_check.{h,cc,_test.cc}` (merged) | Cel-cpp parser/checker wrapper **with `RejectDyn` folded in**. Static-subset enforcement is a frontend concern — it runs on checker output before the IR is built — so v2 collapses the two files into one translation unit: `ParseAndCheck` returns a `TypedAst` only if the static-subset gate passes. v1's `static_subset` header stops existing as a separate surface. |
| `compiler/host/cel_log.{h,cc,_test.cc}` | `compiler_v2/host/` | Already-new log surface |

Everything else under `compiler_v2/` is **written from scratch**.
Specifically:

  - `compiler/ir/annotations.{h,cc}` is **not** ported. v1's
    version has a different schema (no `storage`, no `overload_id`,
    `local_index`, `scope_id`). M1's annotations header is born in
    its final shape.
  - `compiler/codegen/expr_lower.{h,cc}` is **not** ported. The
    new one is annotation-driven and has a different
    `LoweringContext`; cross-referencing v1 during authoring is a
    trap (v1's shape encodes the assumptions we're eliminating).
  - The wasmtime boilerplate (error mapping, linker setup) from
    `compiler/host/host_loader.{h,cc}` is **reauthored** —
    transcribed into `compiler_v2/api/engine.cc` +
    `compiler_v2/api/instance.cc` — and split: the engine + parsed
    runtime half lives on `cel::Engine`, the per-Plan handles on
    `cel::Instance`.  See `two-phase-runtime-isolation.md` §4
    for the cut.

## 5. Work breakdown (order of authoring)

Author the files roughly in this order — each step compiles and
tests in isolation. Parent §11.3 invariants hold: every commit
passes `bazel test //compiler_v2/...` and does not touch `compiler/`.

1. **Directory + BUILD skeleton** — root `BUILD.bazel` + per-dir
   `BUILD.bazel`, empty filegroups. Smoke test: `bazel build
   //compiler_v2/...` green (nothing to build yet).
2. **Port v1 verbatims** — `typed_ast` (→ `ir/`),
   `cel_log` (→ `host/`), and `parse_and_check` (→ `frontend/`)
   **with `static_subset` folded in**: copy `parse_and_check.*`,
   inline v1's `ir/static_subset::RejectDyn` call as the last step
   of `ParseAndCheck` before it returns, and merge the
   `static_subset_test.cc` cases into `parse_and_check_test.cc`.
   v2 has no `static_subset.{h,cc}` surface. Run tests under v2
   BUILD targets; green.
3. **`ir/annotations.{h,cc,_test.cc}`** — write with full schema;
   test field round-trip via `WasmAnnotations::Mutable` /
   `Find`.
4. **`codegen/overload_table.{h,cc,_test.cc}`** — empty
   `kBuiltinSeeds`; test `RegisterCustom` + `AlreadyExists` on
   custom-vs-custom collision.
5. **`codegen/static_memory_builder.{h,cc,_test.cc}`** — every
   scalar kind; per-kind byte layout test.
6. **`codegen/slot_allocator.{h,cc,_test.cc}`** — naive path;
   monotonic-offset test.
7. **`codegen/resolve_pass.{h,cc,_test.cc}`** — kConst repr
   population; non-kConst leaves zero-sentinel fields; kind
   round-trip on every scalar-literal fixture.
8. **`codegen/layout_pass.{h,cc,_test.cc}`** — kConst →
   kStaticRodata; StaticMemoryBuilder invocation; final
   `rodata.size()` matches expected bytes per kind.
9. **`runtime/` header split + `cel_runtime.c` + `cel_{arena,
   memory,make}_test.cc`** + BUILD + WASM cross-compile rule —
   author `cel_data.h` / `cel_memory.h` / `cel_arena.h` /
   `cel_make.h` / `cel_log.h` + umbrella `cel_runtime.h`; each
   test `#include`s only the narrow header it exercises (proves
   the small headers are self-sufficient). CelValue layout test
   lives in `cel_arena_test` (the arena is where layout matters);
   arena at bytes 8/12 round-trip via `cel_alloc` / `cel_reset`;
   `cel_make_*` for every scalar kind.
10. **`codegen/module.{h,cc,_test.cc}`** — emits a module that
    defines memory, places rodata as an active data segment at
    `rodata_base = 16`, exports `memory` + `$eval` + `cel_reset`,
    imports `cel.cel_alloc` (declared even if unused at M1).
11. **`codegen/expr_lower.{h,cc,_test.cc}`** — kConst arm only;
    other kinds return Unimplemented.
12. **`api/engine.{h,cc,_test.cc}` + `api/instance.{h,cc,_test.cc}`** —
    two-phase instantiation (Engine owns shared wasmtime state,
    Instance is per-Plan); `Instance::Eval` invokes `$eval` and
    decodes the result.  Replaces the original
    `host/host_loader.{h,cc,_test.cc}` plan.
13. **`cli/celwasmc_v2.cc`** + BUILD — wire parse → check →
    resolve → layout → emit → write .wasm or direct-run.
14. **`e2e/eval_test.cc`** — one test per scalar kind.

Ordering is strict: each step depends on the previous compiling
and its tests passing. Do not bundle; do not reorder.

## 6. Test plan

### 6.1 Unit tests (what each file's `_test.cc` covers)

  - `annotations_test.cc` — `Mutable(id)` creates, `Find(id)`
    reads back; all fields round-trip at their widths.
  - `overload_table_test.cc` — build with empty seeds;
    `RegisterCustom` appends and returns the id;
    `RegisterCustom` with the same id twice returns
    `AlreadyExists`; `InternOverloadId("unknown")` returns `0`.
  - `static_memory_builder_test.cc` — `Allocate{Null,Bool,Int,
    Uint,Double}`: per-kind byte layout asserted against a
    golden; 8-byte alignment of the next Allocate.
    `Allocate{String,Bytes}`: header bytes, payload bytes,
    alignment pad; length + offset fields match.
  - `slot_allocator_test.cc` — `Acquire()` monotonic; 24-byte
    stride; `peak_slots()` equals acquire count; `Release()` is
    a no-op; `PushScope`/`PopScope` are no-ops.
  - `resolve_pass_test.cc` — kConst → `repr` populated per
    kind; non-kConst fixtures leave `overload_id`/`local_index`
    etc. at zero; end-of-pass DCHECK asserts the per-kind
    populated-ness pattern.
  - `layout_pass_test.cc` — kConst → `storage.kind ==
    kStaticRodata`, `storage.payload` matches the builder's
    offset; total rodata bytes sum across kinds.
  - `cel_memory_test.cc` — `cel_mem_base()` non-null,
    8-byte-aligned; `cel_mem_size()` ≥ one wasm page.
  - `cel_arena_test.cc` — `sizeof(CelValue) == 24`;
    `cel_reset(arena_base, arena_limit)` writes bytes 8/12 and
    rewinds the cursor; `cel_alloc(n)` bumps the cursor, aligns
    to 8, and returns `0` on OOM; `cel_value_at(0)` returns
    null, non-zero returns `base + off`.
  - `cel_make_test.cc` — `cel_make_int(7).payload.i == 7` etc.
    per kind; `cel_make_{string,bytes}` copies into the arena;
    `cel_make_{string,bytes}_view` reuses a caller-owned region
    without copying; `cel_make_string(nullptr, 0)` returns a
    zero-ptr CelValue.
  - `module_test.cc` — compile an empty-body `$eval` module,
    validate via Binaryen; memory defined with correct pages;
    data segment at offset 16 matches expected rodata bytes.
  - `expr_lower_test.cc` — per-kind `kConst` lowering emits
    `i32.const <offset>` where `<offset>` is the builder's
    rodata offset.
  - `api/engine_test.cc` + `api/instance_test.cc` +
    `runtime/cel_runtime_wasm_test.cc` — two-phase instantiation
    (host-allocated memory imported by both modules); per-scalar-
    kind round-trip through Compile → Plan → Eval; runtime
    cel_alloc actually mutates the shared memory cursor (the
    null-pointer-elision regression test).

### 6.2 E2E tests (`eval_test.cc`)

```cpp
TEST(EvalScalar, Bool)   { EXPECT_THAT(Eval("true"),   IsOkAndBool(true));   }
TEST(EvalScalar, Int)    { EXPECT_THAT(Eval("42"),     IsOkAndInt(42));     }
TEST(EvalScalar, NegInt) { EXPECT_THAT(Eval("-42"),    IsOkAndInt(-42));    }
TEST(EvalScalar, Uint)   { EXPECT_THAT(Eval("42u"),    IsOkAndUint(42));    }
TEST(EvalScalar, Double) { EXPECT_THAT(Eval("3.14"),   IsOkAndDouble(3.14));}
TEST(EvalScalar, Null)   { EXPECT_THAT(Eval("null"),   IsOkAndNull());      }
TEST(EvalScalar, String) { EXPECT_THAT(Eval("\"hi\""), IsOkAndString("hi"));}
TEST(EvalScalar, Bytes)  { EXPECT_THAT(Eval("b\"x\""), IsOkAndBytes("x"));  }
TEST(EvalScalar, EmptyString) { EXPECT_THAT(Eval("\"\""), IsOkAndString(""));}
TEST(EvalScalar, EmptyBytes)  { EXPECT_THAT(Eval("b\"\""), IsOkAndBytes("")); }
TEST(EvalScalar, LongString) {
  std::string s(1024, 'x');
  EXPECT_THAT(Eval(absl::StrCat("\"", s, "\"")), IsOkAndString(s));
}
TEST(EvalScalar, UnimplementedOpFailsCleanly) {
  EXPECT_THAT(Compile("1 + 2"),
              StatusIs(absl::StatusCode::kUnimplemented,
                       HasSubstr("kCall not supported before M3")));
}
```

The last test is load-bearing: it proves the "codegen errors on
unimplemented kinds" path works, so M2 onwards extends behavior by
filling arms, not by ripping out silent fallbacks.

### 6.3 Lint + testing-checklist rows

Per CLAUDE.md, every merged feature flips at least one checklist
row. M1 flips:

  - Static-literal lowering × bool
  - Static-literal lowering × int
  - Static-literal lowering × uint
  - Static-literal lowering × double
  - Static-literal lowering × null
  - Static-literal lowering × string
  - Static-literal lowering × bytes
  - Two-phase instantiation × fresh memory per eval module
  - No-`cel_alloc` × static-only eval (eval of a pure-literal
    expression makes zero `cel_alloc` calls)

These get added to `testing-checklist.md` under a new
"Rewrite M1" section and ticked in the same commit that lands M1.

## 7. Exit criteria

M1 is done when all of these hold simultaneously:

  - [ ] `bazel test //compiler_v2/...` green.
  - [ ] `bazel test //compiler/...` green (v1 untouched).
  - [ ] All seven E2E scalar tests (§6.2) pass.
  - [ ] `scripts/lint.sh` clean for every `compiler_v2/` file
        (zero clang-tidy warnings).
  - [ ] Every `compiler_v2/` non-trivial source file has a
        companion `*_test.cc`.
  - [ ] `testing-checklist.md` rows in §6.3 are ticked.
  - [ ] This doc is updated with a `Status: shipped <date>` stanza.
  - [ ] No `TODO(M2)` / `TODO(M3)` comments pointing at
        abstraction *shape* — only at *population* (filling in an
        empty table, adding an arm to a switch). If a shape needs
        to change, M1 is misscoped and has to split.

## 8. What M1 explicitly proves

After M1 lands, the following claims are testable on `master`:

  1. The pipeline is `parse → check → resolve → layout → emit`.
     Not `parse → check → emit-that-also-decides-memory`.
  2. `NodeAnnotation` is the single source of per-expr facts for
     codegen. `LoweringContext` has no `idents` map, no
     `scratch_slot`, no `prologue_setups`.
  3. Literals land in `.rodata` unconditionally — zero `cel_alloc`
     calls during evaluation of a pure-literal expression,
     verifiable by a test that binds `cel_alloc` to a trampoline
     that counts invocations.
  4. The expr module owns memory; the runtime imports it. Two
     expr instances against one runtime are memory-isolated.
  5. Adding a new expression kind later is "add an arm to
     `expr_lower`'s switch + populate the relevant
     `NodeAnnotation` fields in `ResolvePass`" — no pipeline
     reshaping, no new passes.

These five are the load-bearing claims the parent rewrite makes.
M1 is where they stop being design-doc prose and start being
runtime behavior.

## 9. Risk register (M1-specific)

  - **`Allocate{String,Bytes}` alignment.** Span payload is byte-
    aligned; the next `Allocate*` must pad to 8 before writing a
    new `CelValue` frame. A one-byte slip corrupts every
    subsequent offset. *Mitigation:* an explicit alignment test
    (§6.1 `static_memory_builder_test`) asserts the next frame's
    offset after a span payload of each of {0, 1, 7, 8, 9} bytes.
  - **Two-phase instantiation sequencing.** Wasmtime's linker API
    requires all imports to be resolvable before `instantiate`
    returns. Phase 1's `cel_alloc` import has no provider yet.
    *Mitigation:* install a trampoline that forwards to a
    placeholder; rebind to the real runtime export in phase 3
    (parent §9.1). Host loader test exercises the rebind
    explicitly.
  - **Binaryen memory-export shape.** `BinaryenSetMemory` + a
    single active data segment must produce a module the runtime
    module can import as `cel.memory`. *Mitigation:* `module_test`
    round-trips a minimal module through a wasmtime linker step
    in the test harness, catching ABI mismatches at unit-test
    time rather than at e2e time.
  - **Over-scoping.** The temptation is to slip an M2 capability
    ("just idents, they're easy") into M1. Every such slip
    compounds through the milestone sequence. *Mitigation:* the
    `UnimplementedOpFailsCleanly` test at §6.2 is the gate — if
    M1 has code that handles a non-`kConst` kind, that test fails
    and the reviewer rejects the PR.

## 10. After M1

Next milestones, in order (each against the frozen M1 skeleton):

  - **M2** — `kIdent` + `kSelect` (proto field reads); adds
    `host/cel_host.{h,cc}` for `cel_get_field` / `cel_has_field`;
    ports the `Customer` proto fixture.
  - **M3** — `kCall` + built-in overload set; fills
    `kBuiltinSeeds`; adds arithmetic / comparison / string ops in
    `cel_runtime`.
  - **M4** — `has()`, message equality, 3VL, partial-eval; adds
    `cel_refs` externref table.
  - **M5** — custom functions (single `cel_host_call_custom`
    trampoline).
  - **M6** — map + list literals (runtime primitives).
  - **M7** — proto literals (host primitives).
  - **M8** — Sethi–Ullman, debug layout, error provenance, bench
    parity, swap.

Each subsequent milestone gets its own sub-plan doc following
this template.
