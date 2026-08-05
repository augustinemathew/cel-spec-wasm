# compiler-toplevel — design notes (undefined)

Component: compiler public surface + pipeline facade —
`compiler/compiler.{h,cc}`, `compiler/program.h`, `compiler/internal/compile.{h,cc}`.
Paired docs read: `doc/implementation-plan/rewrite/two-phase-runtime-isolation.md`,
`doc/implementation-plan/rewrite/m28-configurable-linking.md`.

## 1. Verified architecture

### 1.1 Roles and data flow

- `celwasm::Compiler` is pure compile-time: declarations + function libraries,
  no wasmtime dependency (compiler.h:1-14; BUILD: `//compiler:compiler` deps
  contain no wasmtime, compiler/BUILD.bazel:29-47). Built via
  `Compiler::Builder` (rvalue-consuming `Build() &&`, compiler.h:282).
  Copyable/movable pure data (compiler.h:157-161). One Compiler → many
  Programs (compiler_test.cc:133-148).
- `celwasm::Program` is pure data: a `std::vector<uint8_t>` of wasm bytes
  behind `wasm_bytes()` (program.h:33-58). No validation at construction;
  wasmtime parse happens in `Engine::Plan` (program.h:35-39). Publicly
  visible target (compiler/BUILD.bazel:8-15).
- `Compiler::Compile(source, CompilerOptions)` (compiler.h:182-183) maps the
  public options onto the internal `celwasm::CompileOptions` and calls the
  facade `celwasm::Compile` (compiler.cc:174-196), returning
  `Program(artifact.wasm_bytes)` — all other `CompiledArtifact` fields
  (ast, layout, module, eval_fn, library_modules) are dropped at this
  boundary (compiler.cc:193-195).
- The facade `celwasm::Compile` (compile.cc:617-634) dispatches on
  `opts.link_mode`:
  - **kDynamic**: `RunFrontAndLayout` (parse → check → resolve → layout,
    compile.cc:400-416) → fresh `WasmModule` → `InstallExprModuleImports`
    (memory import + full runtime import surface, compile.cc:340-365) →
    `LowerExportAndFinalise` (compile.cc:622-633).
  - **kStatic**: `RunFrontAndLayout` → `AdoptStrippedRuntime`
    (`BinaryenModuleRead` over embedded `kCelRuntimeStrippedWasmBytes`,
    compile.cc:489-505) → `InstallExprRodataSegment` (rodata budget gate,
    compile.cc:523-538) → `InstallCelHostImports` (codegen-canonical
    `cel_host.*` names, compile.cc:56-87,609) → `LowerExportAndFinalise`
    (compile.cc:587-613).
  - `LowerExportAndFinalise` is the single shared tail of both arms:
    OverloadTable build (built-ins + customs, compile.cc:466-485,555-558),
    `InstallOverloadImportsExport` (self-skips names already defined in the
    adopted runtime, compile.cc:265-299), `LowerToEvalFunction`, export of
    `$eval` under `opts.eval_export_name`, `cel.abi` custom-section attach
    stamped with the link mode, then `FinaliseModule` (compile.cc:552-578).
- `FinaliseModule` order is load-bearing: validate FIRST, then optimize
  (mutates IR), then serialize (compile.cc:419-438).
- `AttachCelAbiSection` serialises a `celwasm.abi.CelAbi` proto including a
  `link_mode` marker; the marker is embedder-tooling metadata, NOT an engine
  routing input — `Engine::Plan` routes on wasm import introspection
  (compile.cc:373-380; m28 doc §6.2 + §5.4 delta; pinned by
  compile_test.cc:328-346).

### 1.2 Builder contract

- `DeclareVariable` appends unconditionally; all validation is deferred to
  `Build()` (compiler.cc:108-112,137-150): empty name, `kUnknown` kind,
  Message with empty FQN each → `InvalidArgument` (compiler.cc:82-100);
  duplicate names → `InvalidArgument`, not dedup (compiler.cc:144-149).
- `AddFunction(celfn_source)` parses eagerly but defers the parse error to
  `Build()`; FIRST failure wins, later calls don't overwrite
  (compiler.cc:120-135, compiler.h:287-291; pinned by
  compiler_test.cc:285-293).
- Cross-library duplicate overload-id detection happens at `Build()`
  (compiler.cc:152-166); within-library duplicates are
  `FunctionLibrary::Builder`'s job (compiler.cc:153-155, pinned by
  compiler_test.cc:433-450).
- Message-typed declarations resolve against the process-wide
  `generated_pool()` (compiler.h:271-276; pinned by
  compiler_test.cc:215-222).
- Variable declarations cross into the frontend as `"name:Type"` spec
  strings via `CelTypeToSpec` (compiler.cc:32-76,184-188); `kUnknown`
  reaching the encoder is `ABSL_CHECK(false)` because `Build()` filters it
  (compiler.cc:65-71).

### 1.3 CompilerOptions knobs — where each takes effect (the focus question)

- **`mem_size_bytes`** (default 128 KiB = 2 pages, compiler.h:75):
  - kDynamic: sets the initial page count of the imported `cel.memory`
    (`PagesForBytes`, compile.cc:34-39,350-353; declared shared, max 1024
    pages to match the runtime's `--max-memory=67108864`,
    compile.cc:345-353).
  - It is ALSO copied to `LoweringOptions.mem_size_bytes`
    (compiler.cc:177 → compile.cc:560-561), but that field is explicitly
    **vestigial** — "retained for source compatibility — the arena lives in
    the wasi-libc dlmalloc heap and is sized at runtime"
    (expr_lower.h:177-181); `arena_reset` is imported with ZERO parameters
    (compile.cc:357-358) and the prologue call has 0 operands (pinned by
    compile_test.cc:160-168).
  - kStatic: **no effect at all.** `CompileStatic` never calls
    `InstallExprModuleImports` (compile.cc:580-613) — the adopted runtime
    module defines its own memory (internal name `"0"`,
    compile.cc:520-522) — and the only other consumer is the vestigial
    `LoweringOptions` field. Not verified by any test; see §3.
- **`container`** (compiler.h:77-83): forwarded verbatim to
  `CheckOptions::container` (compiler.cc:178) → cel-cpp checker builder
  `set_container` when non-empty (parse_and_check.cc:1067-1068). Affects
  the checker only; no codegen/layout effect. No test exercises it through
  the public `Compiler::Compile` or the facade `Compile` (see §4).
- **`optimize_level`** (default 0, compiler.h:107): forwarded
  (compiler.cc:179) → `FinaliseModule` runs `WasmModule::Optimize(level)`
  **only when `level > 0`** (compile.cc:428-430). `Optimize` itself rejects
  outside [0,3] with `InvalidArgument` and treats 0 as a byte-identical
  no-op (module.cc:294-303); ShrinkLevel is pinned to 0 — perf over size,
  since the wasm is Cranelift input (module.cc:305-311). Consequence:
  levels > 3 are rejected, **negative levels silently behave as 0** — they
  never reach the range check. Applies identically in both link modes (the
  shared tail). Per-module-not-thread-safe caveat lives on
  module.h:156-165.
- **`link_mode`** (default `kStatic`, compiler.h:141-145): forwarded by
  `static_cast` through `uint8_t` to the internal enum (compiler.cc:180-183);
  the two enums are declared with identical values (compiler.h:141-144,
  compile.h:98-102) but nothing (no static_assert, no test) locks them
  together. Effects: selects the compile arm (compile.cc:619); kStatic
  embeds the wrapper-stripped runtime (~800 KB-1.1 MB Program vs ~10 KB
  dynamic — order-of-magnitude pinned by compiler_test.cc:119-131 and
  compile_test.cc:209-216), enforces the rodata budget (below), and stamps
  `LINK_MODE_STATIC` vs `LINK_MODE_DYNAMIC` into `cel.abi`
  (compile.cc:611-612,632-633).
- Internal-only knobs not surfaced publicly: `eval_internal_name`,
  `eval_export_name`, `validate`, `serialize` (compile.h:52-79;
  compiler.h:64-67 names them as intentionally non-public).

### 1.4 Static-mode invariants

- Rodata must end at or below `CELWASM_RESERVED_LOW_MEMORY_BYTES` (8192,
  runtime/cel_layout.h:37): the runtime is linked with `--global-base=8192`
  so the bottom 8 KiB is reserved for expr active data segments; overflow
  is `ResourceExhausted` (a status, deliberately not a CHECK — embedder
  input must not crash the process) and the check + segment install are one
  unit (compile.cc:506-538). Pinned positive/negative/dynamic-control by
  compile_test.cc:356-385.
- kStatic Program has ZERO `"cel"`-module function imports (the
  load-bearing shape invariant; compile_test.cc:218-230) but retains
  `cel_host.*` imports (compile_test.cc:232-243) and exports `eval`
  (compile_test.cc:245-258).
- Dual `cel_host.*` import sets (runtime's wasm-ld-named `$cel_host_*` +
  codegen-canonical names) are intended, wasm-spec-correct, and resolve to
  the same trampoline at instantiate (compile.cc:46-56; m28 §5.3.2 — the
  "obvious cleanup" of skipping `InstallCelHostImports` in static mode is a
  documented rejected alternative).
- `InstallOverloadImportsExport` probes `BinaryenGetFunction` per entry so
  helpers already defined in the adopted runtime are skipped; only custom
  `cel_fn.*` imports land in static mode. The per-entry probe cost is
  documented as microseconds and deliberately not gated on link mode
  (compile.cc:265-299).
- `cel_copy_slot` is installed unconditionally (emitted directly by ternary
  lowering, not OverloadTable-seeded), arity drawn from the ABI catalogue
  (compile.cc:301-321).

### 1.5 Import-surface policy (dynamic mode)

Per the no-lazy-imports rule, `InstallExprModuleImports` installs the full
runtime surface regardless of AST shape: `arena_reset`/`arena_alloc`
(compile.cc:356-360), all `cel_host.*` trampolines (compile.cc:56-87), the
map family incl. iteration helpers (compile.cc:97-159), and the list family
incl. `*_at_if_present` + `cel_set_field_at_if_present` (compile.cc:164-204).
Pinned by compile_test.cc:476-516 (imports present even for literal-only
expressions).

## 2. Doc-vs-code discrepancies

1. **P1 — `Compiler::function_libraries_` comment claims the field is
   storage-only.** compiler.h:206-216 says "`Compiler::Compile` does not
   yet consume this field — the wiring … lands in Slice C.3" and that
   referencing a custom fn "fails at the checker". Code forwards the
   libraries to both checker and codegen (compiler.cc:189-192) and the
   acceptance test compiles `name.is_number()` green
   (compiler_test.cc:322-338). The comment is two milestones stale and
   asserts the opposite of behavior.
2. **P1 — `CompiledArtifact.library_modules` / `CompileLibraryBodies` are
   documented as live but are dead surface.** compile.h:104-126 says the
   vector is "populated iff … one entry per `opts.function_libraries[i]`
   that carries `kCelDefined` decls" and "`Engine::Plan` walks this vector
   before instantiating the expr module". Nothing in compile.cc ever writes
   `library_modules`; `CompileLibraryBodies` is declared
   (celfn/library_module.h:45-46) with **no definition and no BUILD
   target** (grep: only .h hits; no `library_module` in any BUILD.bazel);
   `Engine::Plan` takes a `Program` (bytes only — engine.h:117-118) and
   cannot see a `CompiledArtifact` at all. The eval tree has zero
   references to either symbol.
3. **P1 — compile.h's header docblock describes a removed module shape.**
   compile.h:8-15 says the emitted module has "memory: one wasm page,
   exported as `memory`" and "imports … `arena_reset(i32,i32)->()`". Code:
   memory is IMPORTED as `cel.memory` (2-page default, shared, max 1024
   pages — compile.cc:343-354; export absence pinned by
   compile_test.cc:87-123) and `arena_reset` takes zero args
   (compile.cc:357-358; pinned by compile_test.cc:160-168).
4. **P1 — compile.h:44-49 says `mem_size_bytes` "flows to
   `LoweringOptions.mem_size_bytes` (the second arg of the `arena_reset`
   call emitted at the top of every `$eval` body)".** Both halves are
   false: `arena_reset` has no args (above) and the `LoweringOptions` field
   is self-described as vestigial/unused (expr_lower.h:177-181).
5. **P1 — compiler.h:106 claims "Levels outside [0, 3] are rejected with
   `InvalidArgument`"** (also the status table at compiler.h:170-173).
   Negative levels are NOT rejected: `FinaliseModule` gates on `> 0`
   (compile.cc:428), so `optimize_level = -1` silently compiles as level 0
   and never reaches the range check at module.cc:294-299. Only levels > 3
   reject.
6. **P1 — `mem_size_bytes` public docblock omits that the knob is a no-op
   in the default link mode.** compiler.h:69-74 says "Raise this when an
   expression needs a larger arena (e.g. heavy string concatenation…)" —
   but (a) the arena is dlmalloc-heap-sized at runtime, not by this field
   (expr_lower.h:177-181), and (b) under the default `kStatic` the field
   has no effect whatsoever (§1.3). An embedder following this doc gets
   nothing.
7. **P2 — compiler.h:11-12 says Programs are executed "by passing them to
   `celwasm::Engine::Plan(program, bindings)`"**; the real signature is
   `Plan(const Program&)` (engine.h:117-118) — bindings moved to
   `Instance::Eval(Activation)`. (engine.h's own line-16 diagram has the
   same stale form — sibling-component drift.)
8. **P2 — compiler.h:34-37 claims "`CelType` still lives in `cel`"** and
   justifies `using ::celwasm::CelType;`. `CelType` is declared directly in
   `namespace celwasm` (shared/type.h:21-23); the comment is wrong and the
   using-declaration is a no-op.
9. **P2 — compiler.h:65 "All three fields below are forwarded"** — the
   struct has four fields since `link_mode` landed (all four ARE forwarded,
   compiler.cc:176-183); the count rotted.
10. **P2 — two-phase doc §4.3 contradicts its own §4.1 revision and the
    code.** two-phase-runtime-isolation.md §4.3 ("`Program` holds
    `std::shared_ptr<Compiler::WasmtimeState>`", "`Instance` holds … a
    back-reference to its Program") and §5.2/§5.3 (Commit B/C shapes)
    describe the rejected pre-Engine design; the shipped shape is
    Program-as-pure-bytes (program.h:33-58) with all wasmtime state on
    `Engine`. The §4 delta callout corrects §4.1 but §4.3/§5.2/§5.3 carry
    no superseded marker. Doc is status:shipped, so it reads as current.
11. **P2 — m28 doc §0/§2(goal 3)/§4-table still say dynamic is the
    default** ("Default stays dynamic; today's tests stay green",
    "dynamic (default, today)"); the default is `kStatic`
    (compiler.h:145, compile.h:102). The §5.1 delta callout documents the
    flip, but the uncorrected sentences remain load-bearing-looking for a
    skimmer.

## 3. Validation items

1. **Is `mem_size_bytes` byte-for-byte a no-op under kStatic?** Compile
   `"42"` with `link_mode=kStatic` at `mem_size_bytes` = default and
   = 64 MiB; assert `wasm_bytes()` identical. Then under kDynamic with
   `mem_size_bytes = 3*64Ki`, read the memory import's initial pages
   (`BinaryenMemoryGetInitial` on `artifact.module.raw()`, or `wasm-dis`)
   and assert == 3 — the existing test
   (compile_test.cc:387-398 `MemSizeBytesLargerThanOnePageGrowsPageCount`)
   claims this in its comment but only asserts `Validate()` passes.
2. **Does `optimize_level = -1` really compile silently as level 0?**
   Facade test: `CompileOptions o; o.optimize_level = -1;` expect OK +
   bytes identical to level-0 output; `o.optimize_level = 4` expect
   `InvalidArgument`. Settles discrepancy #5 and pins whichever contract
   the fix chooses.
3. **Resolved-by-removal (m39, 2026-08-04):** the `kCelDefined`/
   `@native.` parse-only stub and its `kUserModule` import routing were
   deleted outright (the D4 agent probed the end-to-end behaviour first
   and pinned the parse error); `ImportModuleSource` at HEAD is
   `kCel`/`kCelHost`/`kCelFn` only.
4. **Does `container` work through the public `Compiler::Compile`?** Test:
   declare `c: celwasm.testdata.Customer`, set
   `opts.container = "celwasm.testdata"`, compile an expr referencing a
   short-form type/enum name; positive + control (no container → checker
   `InvalidArgument`). Currently the knob is verified only by reading
   parse_and_check.cc:1067-1068.
5. **Are the three LinkMode enums (public, internal, abi proto) locked?**
   The forwarding is a blind `static_cast` (compiler.cc:180-183). Probe:
   `static_assert` pairs, or a test asserting
   `static_cast<int>(CompilerOptions::LinkMode::kStatic) == 1 ==
   abi::LINK_MODE_STATIC`. Today a third mode added to one enum only would
   miscompile silently.
6. **Is `Builder` double-`Build()` defined?** `Build() &&` consumes via
   member moves (compiler.cc:168-171); a second `std::move(b).Build()` on
   the moved-from Builder would succeed with empty state (vectors are
   valid-but-empty after move). No test pins whether that's intended or a
   misuse to reject.

## 4. Test coverage observations

Pinned well:
- Builder validation matrix: duplicate / empty name, default-constructed
  `CelType` (compiler_test.cc:164-184); cross-library overload-id collision
  (295-312); `AddFunction` deferred-error + first-error-wins (276-293).
- Link-mode shape: zero `cel.*` imports in static (compile_test.cc:218-230),
  `cel_host.*` retained (232-243), `eval` exported (245-258), abi
  `link_mode` stamp both modes via a local wasm-section walker
  (compile_test.cc:269-346), size ratio >10× (compiler_test.cc:119-131),
  rodata budget positive/negative/dynamic-control (356-385).
- Dual-mode harness: `link_mode_cc_test` compiles compiler_test once per
  mode (`kTestLinkMode`, compiler_test.cc:24-30; compiler/BUILD.bazel:49-64).
- Option flow: `serialize=false` (compile_test.cc:147-153),
  `eval_export_name` (170-186), zero-arg `arena_reset` prologue (160-168).
- Routing of custom-fn backends (`cel_fn` import byte-grep,
  compiler_test.cc:345-405).

Gaps:
- **No test anywhere for `container`** through `Compiler::Compile` or the
  facade.
- **No test for `optimize_level`** at this layer — not the happy path
  (level 2), not the range error, not negative-silent-no-op.
  (`e2e/optimize_test` covers behavior downstream, but the facade contract
  in compiler.h:106 is unpinned.)
- `MemSizeBytesLargerThanOnePageGrowsPageCount` asserts nothing about page
  count despite name + comment (compile_test.cc:387-398); no
  static-mode-no-op companion.
- `ValidateDecl`'s Message-with-empty-FQN arm (compiler.cc:93-98) has no
  test (`RejectsUnknownType` and `RejectsEmptyName` exist; `Message("")`
  does not).
- `validate=false` and `eval_internal_name` (compile.h:54,62) untested.
- `CompiledArtifact.library_modules` untested — consistent with it being
  dead (discrepancy #2).
- Program serialization round-trip across the Compile→bytes→`Program(bytes)`
  →Plan path lives in `e2e/program_roundtrip_test.cc`, not here;
  program_test.cc covers only value semantics.

## 5. Design decisions worth preserving

- **Role split: Compiler is wasmtime-free; Program is pure bytes; Engine
  owns execution.** The original cut (wasmtime state on Compiler,
  two-phase doc §5.2-5.3) was rejected mid-execution as role conflation
  (two-phase §4 delta). Keeps `compiler.wasm` reachable as a build target
  and lets Compile run on a build server that executes nothing.
- **`kStatic` default is deliberate** (m28 §5.1 delta): the perf-dominant
  shape shouldn't require opt-in; embedders wanting small Programs opt
  into `kDynamic`. Tests treat the default as a release-engineering
  choice, not a contract (compiler_test.cc:83-85).
- **Adopted-module merge, not IR-walk re-emit** (m28 §5.3 delta):
  `BinaryenModuleRead` over embedded stripped-runtime bytes +
  the SAME `LowerToEvalFunction` targeting the adopted module. One codegen
  path, two bootstraps; `LowerExportAndFinalise` is the single shared tail
  "so the two arms can't silently diverge" (compile.cc:540-551).
- **Dual `cel_host.*` import declarations are intended** (m28 §5.3.2):
  skipping `InstallCelHostImports` in static mode is a documented rejected
  alternative — the runtime's wasm-ld import names don't match
  codegen-canonical names (compile.cc:46-56).
- **8 KiB low-memory rodata window** (`--global-base=8192`,
  cel_layout.h:37): overflow is a status error, never a CHECK, because
  rodata size is embedder input (compile.cc:514-518); gate and segment
  install are one function so an unchecked segment can't be added.
- **abi `link_mode` is metadata only**; Plan routing stays
  import-introspection-based so old Programs and third shapes can't be
  misrouted by a stale stamp (compile.cc:373-380; m28 §6.2).
- **No lazy imports**: the full runtime surface is installed regardless of
  AST (compile.cc:91-96,206-209,301-308) — unused imports are harmless;
  AST-gated imports are a silent-breakage vector.
- **Validate before optimize before serialize** (compile.cc:419-438);
  level 0 is a guaranteed byte-identical no-op so codegen golden tests
  survive (module.cc:299-303).
- **ShrinkLevel = 0 always**: the expr module is Cranelift input; wasm size
  is irrelevant in a JIT flow, smaller-but-slower is the wrong trade
  (module.cc:305-311).
- **`VariableDeclaration` (not `VariableDecl`)** avoids an ODR collision
  with cel-cpp's `cel::VariableDecl` that crashed the checker builder
  (compiler.h:43-47).
- **Builder error model**: setters never fail; all failures (including
  deferred `AddFunction` parse errors, first-wins) surface at `Build()`
  (compiler.h:287-291) — keeps chaining ergonomic without losing errors.
- **Public-knob firewall**: pipeline-only knobs (`validate`, `serialize`,
  eval names) stay on the internal `CompileOptions`; `CompilerOptions`
  carries only what tunes a specific expression's lowering
  (compiler.h:64-67).
