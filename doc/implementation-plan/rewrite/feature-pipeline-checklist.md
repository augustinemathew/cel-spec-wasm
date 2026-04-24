# Feature pipeline checklist

**When you add a new compiler feature, you touch many files across
many layers.**  This doc is the reference for "which files, which
tests, which ABI changes, in which order".

The compiler pipeline has a lot of stages.  A new feature (a new
expression kind, a new host import, a new variable type) typically
ripples through 8–12 files plus their tests — skip one and the
feature ships broken in a way that may not surface for several
commits.  Use the checklists below as a forcing function: every
time you add a feature, open the matching section and tick off
each file as you go.

---

## 1. Pipeline map

```
┌──────────────────────────────────────────────────────────────────┐
│  Stage                      Files (the "spine")                  │
├──────────────────────────────────────────────────────────────────┤
│  Frontend (parse + check)   compiler_v2/frontend/parse_and_check │
│  Typed IR                   compiler_v2/ir/annotations            │
│                             compiler_v2/ir/typed_ast              │
│  Codegen passes             compiler_v2/codegen/resolve_pass      │
│                             compiler_v2/codegen/layout_pass       │
│                             compiler_v2/codegen/static_memory_*   │
│                             compiler_v2/codegen/slot_allocator    │
│                             compiler_v2/codegen/overload_table    │
│                             compiler_v2/codegen/expr_lower        │
│                             compiler_v2/codegen/module            │
│  Top-level facade           compiler_v2/compile                   │
│  Runtime (wasm side)        compiler_v2/runtime/cel_data.h        │
│                             compiler_v2/runtime/cel_runtime.{h,c} │
│                             compiler_v2/runtime/cel_make          │
│                             compiler_v2/runtime/cel_arena         │
│                             compiler_v2/runtime/cel_memory        │
│                             compiler_v2/runtime/cel_log           │
│  Host imports               compiler_v2/host/cel_log              │
│                             compiler_v2/api/internal/cel_host     │
│  ABI                        compiler_v2/abi/cel_abi.proto         │
│                             compiler_v2/abi/cel_abi_emit          │
│                             compiler_v2/api/internal/abi_decode   │
│  Public API                 compiler_v2/api/type                  │
│                             compiler_v2/api/value                 │
│                             compiler_v2/api/attribute             │
│                             compiler_v2/api/activation            │
│                             compiler_v2/api/compiler              │
│                             compiler_v2/api/program               │
│                             compiler_v2/api/engine                │
│                             compiler_v2/api/instance              │
│                             compiler_v2/api/internal/instance_impl│
│                             compiler_v2/api/internal/wasmtime_*   │
│  CLI + e2e + conformance    compiler_v2/cli                       │
│                             compiler_v2/e2e                       │
│                             compiler_v2/conformance               │
│  WAT prototyping            doc/.../wat/                           │
│                             compiler_v2/tools/wat_runner          │
└──────────────────────────────────────────────────────────────────┘
```

### Stage responsibilities (single-sentence each)

  - **Frontend** — produces a type-checked AST from source; no codegen decisions.
  - **Typed IR** — annotates the AST with per-node facts (`repr`,
    `storage`, `local_index`, …) downstream passes consume.
  - **Codegen passes** — walk the annotated AST, make memory
    decisions (LayoutPass), emit wasm bytes (expr_lower), wrap them
    in a module (module.cc).
  - **Top-level facade** — stitches every codegen pass into a single
    `Compile` call.
  - **Runtime** — wasm-side helpers the emitted module calls (via
    imports) to manipulate CelValues.
  - **Host imports** — C++ callbacks wasm can call for things that
    can't live in wasm (proto reflection, logging, custom fns).
  - **ABI** — the wire schema between compiler and runtime.  Custom
    section in the emitted wasm; decoded at Engine::Plan load time.
  - **Public API** — surface the embedder sees: Compiler, Program,
    Engine, Instance, Activation, Value.
  - **CLI + e2e + conformance** — integration-level exercisers.
  - **WAT prototyping** — hand-written wasm targets that lock the
    codegen output shape before a C++ arm is written.

---

## 2. Feature-type checklists

Every new feature falls into one of these categories.  Find the
matching checklist, tick every box as you go.

### 2.1 New AST expression kind (kSelect, kCall, kComprehension, …)

The heaviest class of change — rewrites touch every stage.

  - [ ] **Frontend** — verify cel-cpp already emits the node.  If
    our type-parser (`parse_and_check.cc`) needs a new type
    syntax, add it there.
  - [ ] **IR**
        - [ ] `annotations.h`: does the node need a new
          NodeAnnotation field?  (`field_number` for kSelect,
          `overload_id` for kCall, `scope_id` for kComprehension.)
        - [ ] `typed_ast.cc`: does `ReprOf` need a new arm?
  - [ ] **Resolve** — `resolve_pass.cc`:
        - [ ] Walk the node, populate its annotation fields.
        - [ ] Update `ResolveOutput` if the pass surfaces new
          artifacts (e.g. `ResolvedVariable`, an attribute pool).
  - [ ] **Layout** — `layout_pass.cc`:
        - [ ] Does the node need a storage slot?  Most
          computed-value kinds do: reserve a workspace slot,
          write `{kWorkspaceSlot, slot_offset}` to the node's
          annotation.  Leaf kinds (kConst, kIdent) go elsewhere.
  - [ ] **Lower** — `expr_lower.cc`:
        - [ ] Add a new case to `EmitRoot` (or the non-root
          visitor if/when we grow one).
        - [ ] Emit the target wasm per the matching WAT trace.
        - [ ] Assert storage-kind invariants via ABSL_CHECK.
  - [ ] **Module** — `module.cc` (rarely): new import for a
    host-side trampoline the kind calls at runtime.
  - [ ] **Runtime** — `cel_runtime.{h,c}` (if the kind needs a
    helper implementable in wasm-C): add the function,
    list it in `wasm_imports.txt`, test it in
    `cel_runtime_wasm_test.cc`.
  - [ ] **Host** — `api/internal/cel_host.{h,cc}` (if the kind
    needs a host-only trampoline like field reads): grow Layer 1
    (semantics), Layer 2 (marshalling), Layer 3 (wasmtime glue).
  - [ ] **ABI**
        - [ ] `cel_abi.proto`: does the node surface host-visible
          metadata?  (Select → `FieldEntry`, partial-eval →
          `AttributeEntry`.)
        - [ ] `cel_abi_emit.{h,cc}`: populate the new entries
          from `StaticLayout` / `ResolveOutput`.
        - [ ] `abi_decode.{h,cc}`: decode the new entries into an
          in-memory struct the runtime consumes.
  - [ ] **Engine** — `engine.cc`: plumb the decoded metadata to
    InstanceImpl; register any new host imports on the linker.
  - [ ] **Instance** — `instance.cc`: extend the encoder /
    decoder if the kind produces a new CelValue shape.
  - [ ] **Value** — `api/value.{h,cc}`: new builders / accessors
    if the kind has a public-surface Value counterpart.
  - [ ] **WAT-first** — `doc/.../wat/NN_<feature>.wat` with a
    walkthrough added to `wat-traces.md`.  Run through
    `tools/wat_runner` with stubs before writing codegen C++.
  - [ ] **Tests** — every layer gets a test:
        - [ ] `frontend/parse_and_check_test.cc` (if parser changed)
        - [ ] `ir/annotations_test.cc` / `typed_ast_test.cc`
          (if IR grew)
        - [ ] `codegen/resolve_pass_test.cc`
        - [ ] `codegen/layout_pass_test.cc`
        - [ ] `codegen/expr_lower_test.cc` — assert the emitted
          IR matches the WAT trace byte-for-byte.
        - [ ] `abi/cel_abi_emit_test.cc`
        - [ ] `api/internal/abi_decode_test.cc`
        - [ ] `api/internal/cel_host_test.cc` (if new trampoline)
        - [ ] `tools/wat_runner/wat_runner_test.cc` — stub-driven
          end-to-end check
        - [ ] `e2e/m<N>_test.cc` — full Compile + Plan + Eval path
  - [ ] **Docs**
        - [ ] `m<N>-*.md`: progress log + any plan-vs-execution
          delta called out.
        - [ ] `design.md`: reconcile if the feature revealed a
          schema-level change that needs to live there.
        - [ ] `testing-checklist.md`: tick the coverage row(s).

### 2.2 New declarable type (new scalar, new container shape)

e.g. adding `any` support, or a new primitive like `half_float`.

  - [ ] **Public API**
        - [ ] `api/type.{h,cc}`: add `CelType::NewKind()`.
        - [ ] `api/value.{h,cc}`: `Value::NewKind(v)` builder +
          `AsNewKind()` accessor.
  - [ ] **IR**
        - [ ] `annotations.h`: `Repr::kNewKind` enum entry.
        - [ ] `typed_ast.cc`: `ReprOf` arm.
  - [ ] **Frontend**
        - [ ] `parse_and_check.cc`: parser accepts the new type
          spec string.
  - [ ] **Compiler**
        - [ ] `api/compiler.cc`: `CelTypeToSpec` arm.
  - [ ] **Codegen**
        - [ ] `static_memory_builder.{h,cc}`:
          `AllocateNewKind` for kConst literals.
        - [ ] `layout_pass.cc`: `Pack` arm dispatching to the
          allocator.
  - [ ] **Runtime**
        - [ ] `cel_data.h`: `CEL_NEW_KIND` enum value + payload
          shape on CelValue's union.
        - [ ] `cel_make.{h,c}`: `cel_make_new_kind` constructor.
        - [ ] `cel_log.{h,c}`: pretty-print arm.
  - [ ] **Instance**
        - [ ] `api/instance.cc`: `DecodeCelValueAt` arm.
        - [ ] host-side `Value → CelValue` encoder arm (lives
          with Instance::Eval's marshal).
  - [ ] **ABI**
        - [ ] `abi_decode`: if Repr is on the wire, ensure new
          enum value round-trips (proto3 additive — no schema
          change usually required).
  - [ ] **Tests** at every layer — new row in the Repr
    parameterised tests (`resolve_pass_test` Repr suite,
    `value_test`, `layout_pass_test` Pack suite).
  - [ ] **Docs** as per §2.1.

### 2.3 New host-provided function (M5 customs, future trampolines)

  - [ ] **Host** — `api/internal/cel_host.{h,cc}`:
        - [ ] Layer 1 — semantics (how the function computes its
          output given input CelValues).
        - [ ] Layer 2 — marshalling (read inputs from linear
          memory, absorb UNKNOWN / ERROR, write output to out_slot).
        - [ ] Layer 3 — `RegisterCelHostImports` registers the
          trampoline on the wasmtime linker.
  - [ ] **Overload table** — `codegen/overload_table.{h,cc}`: add
    a row naming the import module + symbol.
  - [ ] **Codegen** — `expr_lower.cc`: emit `call $cel_host.<name>`
    in the kCall arm based on the overload_id resolution.
  - [ ] **Module** — `module.cc`: wasm import declaration.
  - [ ] **Compile** — `compile.cc`: register the import shape.
  - [ ] **Engine** — `engine.cc`: `RegisterCelHostImports` hook
    must install the new trampoline.
  - [ ] **ABI** — if the function needs compile-time config (field
    ref ids for `cel_get_field`, overload ids for arithmetic), the
    relevant section of `cel_abi.proto` grows.
  - [ ] **WAT-first** — write a WAT that imports the new function,
    run it through `wat_runner` with a stub impl BEFORE writing
    the real Layer-2 semantics.
  - [ ] **Tests** — same layered pattern as §2.1.

### 2.4 Partial-eval / attribute / UNKNOWN plumbing

  - [ ] **Resolve** — walk `kIdent` / `kSelect` chains, intern
    attribute paths, populate `NodeAnnotation::attribute_id`.
  - [ ] **ABI** — `cel_abi.proto::AttributeEntry` populated.
  - [ ] **Emit / decode** — `cel_abi_emit` + `abi_decode`.
  - [ ] **Runtime** — cel_host trampolines consult the unknown
    pattern set at runtime; write `{CEL_UNKNOWN, attribute_id}`
    to `out_slot` on match.
  - [ ] **Instance** — `PartialEval(activation, patterns)` entry
    point on the public API.
  - [ ] **Attribute** — `AttributePattern::Parse` / `IsMatch` on
    the public API.
  - [ ] **Tests** — pattern-match tests at `attribute_test`,
    trampoline absorption at `cel_host_test`, end-to-end at
    `m<N>_test`.

### 2.5 New expression lowering that doesn't add a new AST kind

e.g. adding a new runtime helper for an existing kCall overload, or
changing how `$eval`'s prelude is built.

  - [ ] Lower — the one arm in `expr_lower.cc` changes.
  - [ ] WAT trace — update or add a WAT file, verify with
    `wat_runner`.
  - [ ] Tests — update `expr_lower_test.cc` shape assertions.

### 2.6 New ABI field (additive — most common)

  - [ ] `cel_abi.proto`: add field with a fresh tag number; never
    renumber.  Document in the field's comment what milestone
    populates it.
  - [ ] `cel_abi_emit.cc`: populate from the corresponding
    `StaticLayout` / `ResolveOutput` datum.
  - [ ] `abi_decode.cc`: decode into the in-memory struct.
  - [ ] Engine::Plan: consume the decoded field (store on
    `InstanceImpl`, wire to host imports).
  - [ ] Tests at emit + decode + consumer levels.

---

## 3. Running example — M2.B (idents, Activation, variable ABI)

Worked example of §2.1 + §2.6 + a touch of §2.2 (we grew the ABI
for declared-variable metadata).  Updated as each slice lands.

### Landed slices

**M2.B.0 (resolve + layout)** — commit `afd767b`:

  - [x] `ir/annotations.h` — new `Origin` enum, new NodeAnnotation
        fields (`attribute_id`, `map_origin`, `list_origin`).
  - [x] `codegen/resolve_pass.{h,cc}` — new `ResolvedVariable`
        struct; `IdentResolver` walks kIdent, interns names,
        populates `local_index` + `ResolveOutput::variables`.
  - [x] `codegen/layout_pass.{h,cc}` — new `LaidOutVariable`;
        reserves 24-byte workspace slots; writes
        `{kLocal, local_index}` onto kIdent annotations.
  - [x] `api/compiler.{h,cc}` — `DeclareVariable` +
        `RegisterMessageType` builders; `CelTypeToSpec` converter.
  - [x] `api/attribute.{h,cc}` — `AttributePattern::Parse` with
        dotted + bracketed qualifier forms.
  - [x] Tests at every layer.

**M2.B.1 (expr_lower codegen)** — commit `431e3ff`:

  - [x] `codegen/expr_lower.cc` — kIdent arm emits
        `(local.get local_index)`; `$eval` prelude emits one
        `local.set` per referenced variable.
  - [x] Wasm locals = `vector<BinaryenType>(N, i32)`.
  - [x] `codegen/expr_lower_test.cc` — 4 tests locking the
        emitted IR shape (targeting the WAT at
        `doc/.../wat/02_ident_x.wat`).
  - [x] `api/compiler_test.cc` — `DeclaredIdentCompilesToValidModule`
        flipped green.

### In progress: M2.B.2 (ABI plumbing)

  - [x] `abi/cel_abi.proto` (new v2 schema) — `CelAbi`,
        `VariableEntry`, forward-compat `FieldEntry`,
        `AttributeEntry`.
  - [x] `abi/cel_abi_emit.{h,cc}` + `abi/cel_abi_emit_test.cc` —
        build a `CelAbi` proto from `StaticLayout`; 5 tests green.
  - [x] `compile.cc` — emit the `cel.abi` custom section after
        `LowerToEvalFunction`.
  - [x] `api/internal/abi_decode.h` — decoder interface declared.
  - [ ] **Next:** `api/internal/abi_decode.cc` — parse wasm
        bytes, find `cel.abi` custom section, `CelAbi::ParseFrom`,
        build `DecodedCelAbi`.
  - [ ] `api/internal/abi_decode_test.cc` — round-trip tests
        (emit → module → decode → compare to original layout).
  - [ ] `engine.cc::Plan` — decode the ABI before expr-module
        instantiation, store `DecodedCelAbi` on `InstanceImpl`.
  - [ ] `api/internal/instance_impl.{h,cc}` — carry the decoded
        abi on the Instance.

### Planned: M2.B.3 (host marshal + Instance::Eval(Activation))

  - [ ] `api/value.{h,cc}` — new helper: `Value` → 24-byte
        CelValue encoder keyed on Repr.
  - [ ] `api/instance.{h,cc}` — `Eval(const Activation&)` overload:
        for each decoded variable, look up in activation, encode,
        write to `slot_offset`, call `$eval`.
  - [ ] `api/activation.cc` — no changes (Find() already does
        what we need).
  - [ ] `e2e/m2_test.cc` — flip every `IdentE2ETest.*` green.
  - [ ] Close M2.B section in the plan doc.

### Planned: M2.C (selects)

See `m2-ident-select-unknowns.md §5 Slice M2.C`.  Scope per §2.1
(new AST expression kind) + §2.3 (new host import —
`cel_host.cel_get_field`) + §2.6 (new ABI field — `FieldEntry`).

### Planned: M2.D (has())

A thin extension of M2.C — same kSelect arm with `test_only`
dispatch to `cel_host.cel_has_field`.

### Planned: M2.E (partial eval)

Scope per §2.4 — attribute-pool plumbing +
`Instance::PartialEval`.

### Planned: M2.F (conformance envelope)

  - [ ] `compiler_v2/conformance/runner.cc` — envelope filter
        accepts `unknown:` / `any_unknowns:` matchers; `RunOne`
        routes to `PartialEval`.
  - [ ] `compiler_v2/conformance/README.md` — inventory refresh.

---

## 4. How to use this doc

**Start of a feature session:**

  1. Identify the feature type (§2.1 AST kind, §2.2 new type, …).
  2. Copy the matching checklist into the milestone doc's
     "In progress" section.
  3. Write the WAT trace first (every §2.1 / §2.3 feature).
  4. Work through the checklist top-down, committing per stage.

**During review:**

  - Cross-check the slice against the checklist.  Any unticked
    row is either a future-slice deferral (document it) or a
    missed file (send back for a fix).

**Closing a milestone:**

  - Every checklist row for every in-progress slice should be
    ticked (or explicitly deferred with a reason).
  - Move the "In progress" section to "Landed", add the
    follow-up milestone's planned section.

**When the checklist itself is wrong:**

  - A new feature type not covered by §2 → add a new subsection
    to this doc in the same commit as the feature.
  - A pipeline stage that changed layout / responsibilities →
    update §1 in the same commit.

This doc IS the forcing function.  It doesn't matter that some
rows don't apply to a given slice; what matters is that the
reader touches every row and either ticks it or explicitly
decides it doesn't apply — before the slice lands.
