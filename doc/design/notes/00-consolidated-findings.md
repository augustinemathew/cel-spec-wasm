# 00 — Consolidated findings (design-documentation rebuild)

Input: all 13 component notes + 4 lens passes in `doc/design/notes/`,
read in full 2026-06-10. This file is the merge point: the deduplicated
discrepancy register, the ordered validation backlog, the proposed new
design-doc set, and the keep-as-is list. Where two notes disagreed, the
lens passes (90/91/92/93) settled the conflict against code; this file
carries the settled verdict only.

> **Citation caveat (91 §1):** `compile.cc`, `compile_test.cc`,
> `engine.cc`, `engine_test.cc`, and `known_bugs_test.cc` carry
> **uncommitted working-tree changes** made during the note-writing
> window. Line numbers in the notes for those five files may be off by
> the diff; every register row sourced from them is tagged
> *(tree-moved)*. V1 (commit + re-anchor) must run before any of those
> rows is transcribed into a design doc.

---

## 1. Executive summary

**Verdict: the code is in much better shape than its documentation.**
Across ~6,700 lines of verified notes, the recurring pattern is not
"the implementation diverged from the design" but "the design docs and
header comments froze at the milestone that wrote them while the code
kept moving." The architecture that actually shipped — Compiler/Program/
Engine/Instance role split, static-link default, marker-derived runtime
catalogue, three-path aggregate dispatch, the 24-byte CelValue slot ABI
— is coherent, consistently told across five independent component
reads (lens 90 found *zero* disagreement on any core wire fact), and
well-tested at most layers. But essentially every historical design doc
under `rewrite/` contains at least one load-bearing falsehood, two docs
the index labels "[live] authoritative" are provably wrong on wire
facts, and several public headers assert the opposite of their own
implementation's behavior.

**Top findings, in order of danger:**

1. **The workspace-overflow P0 is real and was just fixed — uncommitted.**
   `SlotAllocator::Release` is a no-op (free-list never shipped, despite
   `design.md`/`memory-layout-design.md` claiming bounded layout and
   `design-heritage` initially misreading it as shipped); ~340
   slot-acquiring nodes historically pushed workspace writes past the
   8192-byte reserve into runtime statics — the actual root cause of
   both the long-arith "unaligned atomic" trap and the 10K-literal-list
   wasmtime panic (cleanup-backlog #16 blamed the wrong layer). The
   working tree adds `ValidateExprStaticRegion` (Compile, both modes)
   and `ValidateAbiSlotExtents` (Plan) gates that close it. Committing
   and re-verifying this is V1.
2. **The `@native` (CEL-defined) custom-fn backend is a ghost.** m13's
   doc says it shipped end-to-end; the implementing commit lives only
   on `master-local`. On master, a compiled `@native` call emits an
   unresolvable wasm import; `CompileLibraryBodies` is declared,
   contracted, and cross-referenced from four headers but has no
   implementation, no BUILD target, and no caller. Two competing
   designs exist on different branches; the new custom-fn doc must
   *decide* the fork, not just record it.
3. **The error/unknown wire contract has live forks.** Errors travel as
   a bare code (messages dropped at `cel_host.cc:802-807`, backlog
   #31), but `cel_log`'s `%v` still implements the never-shipped
   descriptor shape (renders garbage on real errors); `payload.unk`
   has two conflicting contracts (runtime: descriptor offset; host:
   attribute id) and `cel_unknown_merge` dereferences host-minted ids
   as offsets — reachable via `&&` under PartialEval, a possible OOB
   read; 3VL absorption has three implementations with two different
   precedence rules, so `unknown OP error` propagates differently
   depending on operand routing. The new ABI doc must crown one telling
   for each (V2–V4).
4. **The benchmarking publication pipeline is partly fictional.**
   `report.sh` covers 4 of 232 corpus cells and stamps a hardcoded
   "parity verified (eyeballed)" line; the published m28 full-corpus
   tables are not reproducible from the recorded commands; kernel_bench
   pokes a dead arena-cursor offset so its allocating rows time OOM
   paths; the M7B/M7A benches are `#ifdef`-dead and name-drifted.
5. **Header-comment rot is systemic on the eval public surface**:
   `engine.h` says AddComponent is unimplemented (it is fully shipped,
   and ~40 e2e skips cite the stale blocker), `value.h` says aggregate
   builders are stubs (all real), `instance.h` says aggregates don't
   decode (they do), `engine.h` describes a host-allocated memory model
   that was inverted by Phase C.

**Shape of the proposed doc set:** eight new docs under `doc/design/`
— an architecture overview, then compiler (pass-pipeline spine),
evaluator (plan/instantiate/eval spine), ABI & memory (the single wire
telling), runtime kernel, custom functions (with the fork decision),
testing strategy, and benchmarking — superseding `rewrite/design.md`,
`memory-layout-design.md`, `cel-host-surface.md`, `map-list-dispatch.md`,
`two-phase-runtime-isolation.md`, the m13/m24/m26 custom-fn docs, and
`benchmark/DESIGN.md` (all gaining archive banners). Fuzzing is
deliberately out of scope here (planned on a separate branch); the
testing-strategy doc reserves a section that points at it.

---

## 2. Discrepancy register

Deduplicated across all 17 note files. Columns: severity, the
discrepancy, deciding citation(s), source note(s). "(tree-moved)" =
affected by the uncommitted working-tree diff (see caveat above).
Lens verdicts already applied: where a note was itself wrong
(design-heritage ×3), the corrected fact is registered and the note
error is listed in §2.4.

### 2.1 P0 — ships-breaking or rebuild-poisoning

| ID | Discrepancy | Citations | Source |
|----|-------------|-----------|--------|
| R1 | **Workspace bounded nowhere at HEAD; docs claim a LayoutPass ResourceExhausted gate that never existed.** `memory-layout-design.md:87-93,173-174` promises `LayoutOptions::reserved_region_limit_bytes` + A11/A12 enforcement; `layout_pass.h:50-64` has no such field. Because `SlotAllocator::Release` is a no-op (`slot_allocator.cc:24-28`), workspace grows 24 B per acquire; ~340 nodes overrun the 8192 reserve into runtime statics. Confirmed root cause of the long-arith trap and the 10K-list wasmtime panic. **Fixed in the uncommitted working tree** by `ValidateExprStaticRegion` (Compile, both modes) + `ValidateAbiSlotExtents` (Plan) (tree-moved). | layout_pass.h:50-64; slot_allocator.cc:24-28; known_bugs_test.cc:815-835 (tree) | codegen-memory #1; 91 §1-2; 90 F1 |
| R2 | **m13 doc claims the CEL-defined (`@native`) backend shipped end-to-end; it never landed on master.** Named files/symbols/tests (`custom_fn_emit.cc`, `EmitCustomFnBodies`, `RejectCelDefinedRecursion`, scale tests, 254/255 cap test) exist only on `master-local` (commit 0386851e). On master a compiled `@native` call yields an unresolvable wasm import. | m13-custom-fns.md:3-5,190-246 vs repo grep | celfn #1; 91 §0 |
| R3 | **`CompileLibraryBodies`/`CompiledArtifact.library_modules` are dead surface documented as live.** Declared with full contract (library_module.h:45-46), no `.cc` ever existed, no BUILD target, no caller; `compile.h:104-126` says `Engine::Plan` walks the vector (Plan takes bytes only); `layout_pass.h:57` + `expr_lower_internal.h:51` cite the nonexistent `library_module.cc`. (`rodata_base_override`'s status changed in-tree: the new gate's comment names it the kStatic relocation seam — verify at V1.) | library_module.h:45-46; compile.h:104-126; compiler.cc:195 | compiler-toplevel #2; celfn #2; codegen-memory #4; 91 §0 |

### 2.2 P1 — must fix before/while the new docs are written

**Compiler surface & pipeline**

| ID | Discrepancy | Citations | Source |
|----|-------------|-----------|--------|
| R4 | `compiler.h:206-216` claims `function_libraries_` is storage-only ("wiring lands in Slice C.3"); code forwards to checker+codegen and the acceptance test passes. | compiler.cc:189-192; compiler_test.cc:322-338 | compiler-toplevel #1 |
| R5 | `compile.h:8-15` header docblock describes a removed module shape: memory "exported as `memory`", `arena_reset(i32,i32)`. Memory is imported (`cel.memory`); `arena_reset` is zero-arg. | compile.cc:343-358; compile_test.cc:87-168 | compiler-toplevel #3 |
| R6 | `compile.h:44-49` says `mem_size_bytes` flows to the `arena_reset` call's second arg — `arena_reset` has no args and `LoweringOptions.mem_size_bytes` is self-described vestigial. | expr_lower.h:177-181; expr_lower.cc:136-144 | compiler-toplevel #4; codegen-memory #10 |
| R7 | `compiler.h:106,170-173` claims optimize_level outside [0,3] rejects; negative levels silently compile as level 0 (gate is `> 0` before the range check). | compile.cc:428; module.cc:294-303 | compiler-toplevel #5 |
| R8 | `compiler.h:69-74` tells embedders to raise `mem_size_bytes` for heavy concatenation; the knob is a no-op under the default kStatic and the arena is dlmalloc-sized at runtime. (Also see R-V8: in kDynamic, >256 KiB plausibly breaks Plan.) | compile.cc:580-613; expr_lower.h:177-181 | compiler-toplevel #6; 91 §9 |
| R9 | `design.md` claims the implicit-dyn RejectDyn gap is still open (unticked checklist row); code recurses through container/abstract types and e2e pins both rejections. | parse_and_check.cc:360-377; e2e/m4_test.cc:360-384 | frontend-ir #1 |
| R10 | `design.md` §4.1 NodeAnnotation schema is six fields behind (`message_type_id`, four `comp_*` fields, `select_key_rodata_offset` missing). | annotations.h:99-136 | frontend-ir #2; design-heritage §1.1 |
| R11 | `parse_and_check.h:65-66` overstates the static-subset gate — omits all five admit carve-outs (dyn-passthrough, select-through-Any, math.@min/@max, `.format([list])`, cel.bind shape). | parse_and_check.cc:400-630 | frontend-ir #3 |
| R12 | `expr_lower.h:4-15,197-209` claims M1 kConst-only with UnimplementedError for everything else; all 8 expr kinds lower. | expr_lower.cc:1147-1208 | codegen-lowering #1 |
| R13 | `overload_table.h:238-250` reasons-list for `kExplicitlyUnimplementedIds` names families that are all seeded now; the actual set is 6 ids. (P2 sibling: h:10-12 "kBuiltinSeeds empty" — now 271.) | overload_table.cc:735-759 | codegen-lowering #2 |
| R14 | `m5-kcall-comprehensions.md` §2.4 claims explicit branching is "the only correct lowering" for `&&`/`||`; shipped code routes them through the eager slot-out arm with 3VL absorption in `cel_and`/`cel_or` (spec-equivalent; both operands always evaluate — a perf fact). | expr_lower.cc:1180-1193; slice2-control-flow-plan.md:65-68 | codegen-lowering #3 |
| R15 | **Tracked known bug:** the loop-cond peephole reads accu payload bits without checking kind, so an ERROR accumulator trips `exists`'s exit early — `[0, 2].exists(x, 2/x == 1)` returns error instead of spec-true. Pinned skipped in known_bugs. | expr_lower_comprehension.cc:167-172,648-657; known_bugs_test.cc:391-405 | codegen-lowering #5 |
| R16 | `slot_allocator.h:102-104` + `memory-layout-design.md:83-85` describe a recycling allocator tracking peak liveness; `Release` is a no-op and `peak_slots` counts total acquires. (Feeds R1.) | slot_allocator.cc:18-28 | codegen-memory #2; 90 F1 |
| R17 | `layout_pass.h:72-74` lists kComprehensionExpr among slot-assigned nodes; no visitor assigns comp storage (result flows via the accu slot). | layout_pass.cc:313-339 | codegen-memory #3 |
| R18 | `map-list-dispatch.md` §2.1 presents an origin-inference table wider than shipped (kCall→kHost, comp-fold→kArena, same-origin coalesce all unimplemented; only literal→kArena and scope-free ident/select→kHost exist). | resolve_pass.cc:386-467 | codegen-memory #5 |

**Custom functions / celfn**

| ID | Discrepancy | Citations | Source |
|----|-------------|-----------|--------|
| R19 | `m26-celfnc-and-component-build.md:3` says "not yet started"; Phases A–C (emitters, CLI, macro, demo e2e) shipped — the doc's own §10 says so. | celfnc_emit/*, run_generate.cc | celfn #3 |
| R20 | `function_library.h:59-66,183-184` says kType/kOptional are admitted on foreign decls "per m24 §6"; Build() permanently rejects them (the same header says so three paragraphs later). `parse_and_check.cc:770-771` stub message still says "until m24" — m24 shipped and closed those types. | function_library.cc:287-318 | celfn #4 |
| R21 | **AddComponent staleness cluster:** `engine.h:174-176` says "not yet implemented — returns Unimplemented"; fully implemented + e2e-exercised. ~40 skips in `foreign_fn_type_matrix_test.cc` cite the dead blocker (kBlockerB0); `engine.cc:1471-1487` banner equally stale; `engine_test.cc:850` skip's blocker (fixtures) shipped. | engine.cc:1489-1539 | eval-public #1; celfn #5; testing-system #2; 91 §0 |
| R22 | **No custom-fn arity cap anywhere; ≥5-value-arg decls silently skip import install.** `num_args = uint8_t(params)+1` wraps at 255; codegen's arity switch returns false outside [1,5] so the call target is never imported; engine rejects only arity 0. m13 doc claims a 254/255 cap test that doesn't exist. | function_library.cc:205; compile.cc:276-296 | celfn #6; 91 §8 |
| R23 | m26 §4 claims map LOWER is emitted; code emits a literal `// TODO(m26)` — a map-returning `@component.` decl generates a stub calling a nonexistent `codec::lower`; the full_matrix gate that would catch it was never built (fixture dead). | cpp_codec_emitter.cc:371 | celfn #7 |
| R24 | `examples/BUILD.bazel:7-10` claims examples depend only on public targets; example 09 deps `//compiler/celfn:function_library` (visibility `//:internal`) — bug #32; an external embedder cannot build 09. | examples/BUILD.bazel:173; compiler/celfn/BUILD.bazel:4 | tools-examples #4 |

**Runtime kernel**

| ID | Discrepancy | Citations | Source |
|----|-------------|-----------|--------|
| R25 | `cel_map.h:47-51` + `cel_runtime.c:128-131` claim geometric growth on full maps; code traps (`PRESIZE_INVARIANT`) — growth was replaced by codegen pre-sizing. | cel_runtime.c:164-167 | runtime-kernel #1 |
| R26 | `cel_memory.h:6-13` + `cel_runtime.h:17-30` document fixed-offset arena state (`[8..12)/[12..16)`) and 2-arg `arena_reset`; state is a BSS struct, reset is zero-arg, bytes 8/12 dead. | cel_arena.c:20-27; cel_arena.h:46 | runtime-kernel #2 |
| R27 | wasm `cel_memory_size_` returns fixed 64 KiB with a comment claiming an imported 1-page memory and cursor-slot bounds-checks; the runtime owns `(memory 4 1024 shared)`, bounds check uses `g_arena.capacity`, no wasm caller exists — a latent footgun for future callers of `cel_mem_size()`. | cel_memory.c:36-41; cel_arena.c:89 | runtime-kernel #3 |
| R28 | `cel_runtime_wasm_test.cc:13,196-198` claims the harness memory matches a `--import-memory` build; no such flag exists; the host-defined `cel.memory` in the harness is dead weight. | runtime/BUILD.bazel:644-649 | runtime-kernel #4 |
| R29 | `cleanup-backlog.md:268-271` (#16) blames "a runtime-side allocation that doesn't check capacity" for the 10K-list panic; every in-runtime `arena_alloc` consumer checks the 0 return (exhaustive audit). Real cause = R1 (workspace overrun), confirmed + gated in the working tree (tree-moved). Backlog entry must be rewritten. | runtime-kernel §1.7 audit table | runtime-kernel #5; 91 §1 |

**ABI / shared**

| ID | Discrepancy | Citations | Source |
|----|-------------|-----------|--------|
| R30 | `cel_abi.proto:227-229` claims `variables[]` is positional by `local_index`; the emitter skips comp-scope locals while ResolvePass interleaves them in the same dense index space — a free variable first referenced inside a comprehension should break the claim. Consumers iterate by name, so no runtime bug; the wire-contract doc states a (probably) false invariant. | cel_abi_emit.cc:27; resolve_pass.cc:206-216 | abi-shared #1 |
| R31 | `abi-refactor.md` §2/§4 describe a hand-written `AbiHelper` POD + macros; the catalogue is the generated proto parsed from an embedded textproto derived from `// cel:codegen-export` markers (commit 511c3ec8 "kill AbiHelper struct"). | runtime_catalogue.h:78-92 | abi-shared #2 |
| R32 | **Deleted `runtime_catalogue_consistency_test` still cited as a live drift gate** in `runtime/BUILD.bazel:12-16`, `wasm_exports.txt:14-16`, `abi-refactor.md`. Deleted as tautological (511c3ec8); the actual guarantee is "both sides derive from the same markers". | repo grep: no target | abi-shared #3; runtime-kernel #11; 90 F4; 91 §3 |
| R33 | `abi-refactor.md` §6 case 23 says a module without `cel.abi` is rejected at Plan; code tolerates NotFound and proceeds with an empty abi. | abi_decode.h:33-35; engine.cc:440-443 | abi-shared #4 |
| R34 | `shared/type.h:2-4` says CelType mirrors a wire `cel.abi.CelType` message; no such message exists (wire carries only the `repr` u32; the message is an unimplemented sketch in cel-host-surface.md). | cel_abi.proto:52-56 | abi-shared #5 |
| R35 | `ir::Repr` uses implicit enum numbering while `cel_abi.proto:48-50` promises wire stability; nothing pins the numeric values — a mid-enum insertion would silently renumber every emitted `repr` without tripping `runtime_abi_version`. | annotations.h:17-39 | abi-shared #6 |

**Evaluator (public + internal)**

| ID | Discrepancy | Citations | Source |
|----|-------------|-----------|--------|
| R36 | `engine.h:168-170` claims AddComponent validates exported FuncType vs decl signature at registration; export resolution is Plan-time, no FuncType comparison exists, arity mismatch surfaces only as a call-time trap. | engine.cc:1052-1097,856-861 | eval-public #2 |
| R37 | `engine.h:99-105` describes Plan as host-allocating a 2-page memory and binding it as `cel.memory`; shipped model is inverted — the runtime declares + exports shared memory, host clones the export. | engine.cc:120-210 | eval-public #3 |
| R38 | `value.h:9-15,108-110` says aggregate builders + `CelEquals` are CHECK-stubs ("OwnedMessage stays stubbed until M7"); all builders are real (in cel_host.cc); no `CelEquals` exists — `StructurallyEquals` is the shipped equality. | cel_host.cc:3983-4016 | eval-public #4 |
| R39 | `value.h:60-61` claims `Value::Kind` numbering is "stable on the wire — kept in sync with CelKind"; false from kind 9 up (wire CEL_MAP_HOST=9 vs Value kMessage=9, etc.). Dangerous if a reader static_casts. | value.h:62-81 vs cel_data.h:32-49 | eval-public #5 |
| R40 | `instance.h:59-64` says Eval returns InvalidArgument for LIST/MAP/MESSAGE results; all aggregate kinds (+ DURATION/TIMESTAMP/TYPE) decode to real Values. | instance.cc:262-340 | eval-public #6 |
| R41 | **(#31)** ErrorPayload.message + expr_id dropped at the host→wasm boundary (`EncodeValue` kError arm writes bare code only); `cel-host-surface.md:820-823` specifies a message-carrying wire shape that never shipped; read-back synthesizes generic messages from the code. | cel_host.cc:802-807; cel_host_error.cc:89-94 | eval-internal D1; 91 §0 |
| R42 | `cel_log`'s `%v` error formatter implements the never-shipped descriptor-struct wire — a real `%v` on a production error renders garbage read from offsets 10..41; tests pin the formatter against fixture memory shaped to the old design, hiding the divergence. | cel_log.cc:168-184 | eval-internal D2 |
| R43 | **`payload.unk` has two live, conflicting contracts.** Runtime: u32 offset to a 2-word UnknownSet descriptor, dereferenced by `cel_unknown_merge`; host: attribute_id written directly (+ `kFunctionUnknownSentinel = 0xFFFFFFFF`). `a.x && b.y` under PartialEval (eager `cel_and` → `cel_unknown_merge`) feeds attribute ids in as descriptor offsets — garbage/possibly-OOB reads. The runtime comment's claim that the host mints `payload.unk == 0` is simply false. | cel_3vl.c:60-130; cel_host.cc:1546-1551 | eval-internal D3; 90 F3; 91 §6 |
| R44 | `instance.cc`'s CEL_ERROR decoder omits `kInvalidArgument` (sibling decoder has it) — wire code 18 (e.g. bad TZ) surfaces as `kHostAdapterError`/"runtime error code 18". Example 08 + the examples smoke test **enshrine this bug as documented output**; fixing it must update both. | instance.cc:233-246 vs host_call_context.cc:85 | eval-internal D4; 91 §7 |
| R45 | **Three 3VL absorption implementations, two precedence rules.** Runtime kernel: error-dominates-unknown across operands; cel_host trampolines: first-operand-wins (UNKNOWN(a) beats ERROR(b), test-pinned); custom-fn trampoline: error-dominates. Operand origin routing decides which rule fires. `cel_host_error.h:97-101`'s two clauses contradict each other; `cel-host-surface.md:118-119` says error dominates. | cel_internal.h:83-102; cel_host_error.cc:134-145; engine.cc:603-624 | eval-internal D5; 91 §5 |
| R46 | `cel-host-surface.md` §3.1 claims Plan precomputes `FieldRef = {FieldDescriptor*, result_type}`; FieldRefEntry carries only `(number, name)`, descriptors resolve per call, the expected-type hint is an unplumbed placeholder. | cel_host.h:368-371; cel_host.cc:533-540 | eval-internal D6 |

**Testing / tools / benches**

| ID | Discrepancy | Citations | Source |
|----|-------------|-----------|--------|
| R47 | `per-component-test-coverage.md` §2 catalog + §5 closeout block name dead/re-tagged targets (`//e2e:m<N>_test` vs the macro-emitted `_dynamic`/`_static` pairs; most e2e is no longer manual); the closeout checklist is uncopyable as written. | e2e/link_mode_e2e_test.bzl:29-43 | testing-system #1 |
| R48 | Stale M2-era skips contradicted by later suites: `m2_test.cc:206,210` + `m4_test.cc:448` cite "host arena plumbing, deferred to M2.C"; string/bytes + list<string> bindings work throughout m5/activation_boundary. Also `expr_lower_test.cc:583-601` skip "until M3 kCall lands". | e2e/m5_test.cc:842-928 | testing-system #3; codegen-lowering §4 |
| R49 | `cel-cli-design.md` describes a 3-verb CLI and plans `cel celfn gen --lang go`; code ships a 4th verb `generate` with `--language cpp` only. Never re-statused. | cel.cc:544-593; run_generate.cc | tools-examples #1 |
| R50 | **WAT corpus coverage overstated everywhere:** wat_runner_test header + BUILD comment claim "every WAT file"; 63 on disk, ~33 loaded; target is `manual` so nothing runs per build. CLAUDE.md's "re-runs every .wat on every build" doubly wrong. | tools/wat_runner/BUILD.bazel:13,35 | tools-examples #2; 90 F5; 91 §4 |
| R51 | Bound-aggregate eval three-way contradiction: cel_smoke_test.sh comment says comprehensions over bound lists/maps unsupported; tools/cel/README shows them working; activation_matrix_test (manual) asserts them green. | cel_smoke_test.sh:50-54 vs README.md:87-98 | tools-examples #3 |
| R52 | `wat_runner.h` "What this does" is pre-Phase-C: claims a harness-created 2-page memory and arena export binding; harness adopts the runtime's exported shared memory and binds all 115 exports. | wat_runner.cc:469-476,754-825 | tools-examples #5 |
| R53 | `//bench:cel_pipeline_bench` doesn't exist; `POST_MIGRATION_BENCH.md` gives it as the reproduction command and `per-component-test-coverage.md:94` lists it as a manual gate target; the source file is orphaned. | bench/BUILD.bazel (6 targets) | benchmarking #1; 93 §6.2 |
| R54 | `benchmark/README.md` + `DESIGN.md` describe a never-shipped system (parity binary, report.py + test, profile.sh, comparator wrappers, committed results/); as-shipped is two inline mains + report.sh + /tmp JSONs. | repo grep | benchmarking #2 |
| R55 | `report.sh` covers 4 operators / 20 of 232 cells and stamps a hardcoded "Parity verified for all 20 cells (eyeballed…)" line; the published m28 full-corpus tables are not producible by the recorded reproduction path. | report.sh:26-27,108-109 | benchmarking #3; 93 §4 |
| R56 | M7B/M7A kernel benches permanently dead (`#ifdef CELWASM_M7B_SHIPPED` defined nowhere) AND name-drifted (call `cel_ts_year_utc` vs shipped `cel_ts_year_utc_at_v`) although both milestones shipped 2026-05-16. | kernel_bench.cc:558-712; cel_time.h:109 | benchmarking #4; 93 §6.4 |
| R57 | `BM_Eval_LongArith_10kTerms` is 1000 terms with three mutually contradicting comments (50/10,000/1,000); cel-cpp sibling repeats it. | in_operator_bench.cc:104-119 | benchmarking #5 |
| R58 | The `_Opt2` "paired" twenty-term benches compile a different expression than their non-Opt2 counterparts (`a<b && …` vs `a+b+…==t`); the README trade-off table compares unlike workloads. | pipeline_bench.cc:163-169 vs 475-481 | benchmarking #6; 93 §6.3 |
| R59 | **Live bench bug:** kernel_bench pokes the arena cursor at `cel_mem_base()+8` — dead since the WASI migration (state moved to BSS) — so allocating benches never reclaim, hit the 64 KiB cap, and time OOM/poison paths. Numbers from those rows are not production-shape. | kernel_bench.cc:388-396,457-492; cel_arena.c:19-27 | 90 F6 |

### 2.3 P2 — cleanup-when-touched (compressed; one row per cluster)

| ID | Cluster | Items | Source |
|----|---------|-------|--------|
| R60 | Compiler-surface comment rot | `compiler.h:11-12` `Plan(program, bindings)` stale signature (also engine.h:16, instance.h:6); `compiler.h:34-37` wrong CelType-namespace claim; `compiler.h:65` "all three fields" (four); two-phase doc §4.3/§5.2/§5.3 un-marked superseded sections; m28 doc §0/§2/§4 still say dynamic default | compiler-toplevel #7-11 |
| R61 | Frontend doc rot | dyn-passthrough-plan.md missing the `has_type()` admission arm; design.md §4.1 "per-kind populated-ness audit" doesn't exist (only KConstReprAudit); `annotations.h:44-45` "M1 only emits kStaticRodata"; parse_and_check.h var-spec doc omits `dyn`/`type` | frontend-ir #4-7 |
| R62 | Codegen doc rot | ternary kind probe differs from slice2 plan (kind==CEL_BOOL vs kind>=15 — reachable difference only for dyn non-bool cond, see V10); module.h:10-16 stale module narrative; cross-numeric plan body still presents rejected Option A as "Recommendation"; resolve_pass.cc cites nonexistent map-list-dispatch §2.6; map-list-dispatch §4.4 CelKind numbering stale; memory-layout A13 `==` vs `>=`; "[0,16) null sentinel" vs split [0,8)+[8,16); stale "stub until M5/M6/M10" pointers | codegen-lowering #4,#6,#7; codegen-memory #6-#11 |
| R63 | celfn doc rot | m26 §2 shows decl syntax the grammar rejects + wrong grammar file names; m26 §3.5.1 `author_*` prefix + absl time types vs `customfn_*` + protobuf types (also macro docblock); `m23_native_quad_inline.wat` missing from wat-traces.md; h:199 "iff" enforced one direction only | celfn #8-11 |
| R64 | Runtime comment rot | `cel_list_arena_view` "returns 0" comment (never does); `cel_map.h` 8-byte iter-state (16); `iter_key_at` "no-op" (poisons); wasi/DESIGN A16 vs idempotent re-init; +65536 slack comment; BUILD comment citing deleted consistency test | runtime-kernel #6-11 |
| R65 | ABI doc rot | runtime_catalogue.proto "only CEL rows generated" (host/env rows are in); wit/README "no first-party consumer yet" (cel_component shipped); abi-refactor stale counts + wasm_exports section structure + "byte offset" error claim | abi-shared #7-9 |
| R66 | Eval comment rot | engine.h:29-30/instance.h:11-13 "lands in a later commit"; parse-failure status code claims (FailedPrecondition not InvalidArgument, unpinned); AddModule "imports cel.memory" v1 constraint; instance.h memory_size_bytes UB claim; activation.h "Instance::Reset"; CLAUDE.md public-eval-set list omits host_call_context/typed_function; engine_test header synthetic-WAT rationale; m24 kType Lower stub status omitted; "broken LiftNull" comment names nothing; cel_host.h:506 concat comment | eval-public #7-15; eval-internal D7-D10 |
| R67 | Testing doc rot | binding_marshal.h stale Unimplemented list; conformance README fail=92 autogen vs "93" prose + duplicate run instructions with wrong path; run_full_suite.sh "`//...` unusable" claim; feature-pipeline-checklist + testing-checklist pre-restructure path rot; "IsInM\<N\>Envelope" vs `IsInEnvelope`; CLAUDE.md "oracle has no activation/unknown surface" (filled by PartialEvalWithCelCpp); cleanup-backlog #2 names `engine.cc::kRuntimeExports` which was replaced by the catalogue (real residual copies: wat_runner's list + runtime linkopts) | testing-system #4-10; 90 F7 |
| R68 | Bench doc rot | 229 vs 232 cell counts; OPERATORS.md claimed CI parser doesn't exist; DESIGN §6.4.4 mandatory `purpose:` field unimplemented; bench default kDynamic ≠ production kStatic (documented deviation, restate); m28-bench-results §2† vs §6 self-contradiction; stale fn-name cross-refs in report.sh; stale future-work rows; pipeline_bench dummy-bind comment | benchmarking #7-13 |
| R69 | Tools doc rot | examples/README "Seven" (nine); `FormatMessages` vs `FormatMessage`; var_parser.h advertises `\uXXXX` escapes the impl lacks; cel-cli-design cites deleted `doc/user-guide.md`; wat_runner.cc cites `api/engine.cc`; exit-code taxonomy approximate; usage text under-advertises format aliases | tools-examples #6-12 |
| R70 | design.md / wat-traces archival fixes | §4.6 customs API never existed; §12 Q1 recommends against the now-default static mode; §10.2.3 "no new NodeAnnotation fields" broke silently; §8.1 wasm_exports.txt single-source claim; §13 Sethi–Ullman "shipped" (never); stale counts (80→271 seeds, 86→6 unimpl, "four imports"); wat-traces M14.1 contradicts its own preamble; first "§6" names never-existed `cel_list_index_at_vv` + duplicate section number; stale "runs once Slice X lands" status lines; inline WAT listings drifted from on-disk twins (all 59 migrated) | design-heritage #1-11 |
| R71 | 91 §10: lifecycle thread-safety story stops at Program — Compiler-side contract undocumented; module.h's "serialised by Compiler ownership" premise false (Compiler is copyable; two Compilers race on process-global `BinaryenSetOptimizeLevel`) | module.h:154-165 | 91 §10 |
| R72 | 91 §9: `mem_size_bytes` kDynamic >256 KiB plausibly fails Plan (import min vs runtime's exported 4-page memory); compile side and engine side never reconciled; CLI still advertises the flag | compile.cc:36-39; engine.cc:274-284 | 91 §9 |

### 2.4 Note-level corrections already applied (do not re-import the wrong telling)

- **design-heritage** misread three things as shipped/alive; corrected
  by 90/91/92/93 and folded above: free-list slot reuse (→ R1/R16),
  `runtime_catalogue_consistency_test` (→ R32), "59 WATs per build"
  (→ R50). Every design-heritage row sourced from a header comment
  rather than a `.cc` body needs the read-the-implementation check
  before transcription.
- **doc-index** grants "[live] authoritative" to `cel-host-surface.md`
  and `memory-layout-design.md`, both proven wrong on wire facts
  (R41/R46; R1/R16). Downgrade both rows; the new ABI/memory docs
  supersede them (90 F2).
- **compiler-toplevel** conflated `InstallOverloadImport` (the helper,
  compile.cc:266) with `InstallOverloadImportsExport` (:301) and
  mis-anchored `InstallCelHostImports` (:92); celfn's anchors are right
  (90 F9). All five tree-moved files need re-anchoring anyway (V1).
- **cel_host import counts**: 20 (catalogue) vs 12 (`wasm_imports.txt`)
  is a subset relation, not a conflict — the runtime kernel itself
  imports only 12; expr modules may import all 20 (90 F8). Page counts
  (4-page export / 2-page import min / 2-page assertion floor /
  3-4 actual) are consistent; the new memory doc carries one table
  (90 F10).

---

## 3. Validation backlog

Deduplicated; ordered by how much a design doc depends on the answer.
Each item names the exact command/test/probe. Items marked ⚑ block a
specific doc section from being written honestly.

| # | Question | Settles | How |
|---|----------|---------|-----|
| V1 ⚑ | Are the working-tree static-region gates the real, committed story (and is `rodata_base_override` now live as the kStatic relocation seam)? | R1, R29, R3-partial; memory-model sections of the compiler + ABI docs; re-anchors all citations into compile.cc/engine.cc/known_bugs_test.cc | Commit the tree (user-authorized), then `bazel test //compiler/internal:compile_test //e2e:known_bugs_test_dynamic //e2e:known_bugs_test_static --test_filter='*LongArith*:*Rejected*'`; confirm `LongArith_166Terms_RejectedAtCompile` + `LiteralIntListInScanRejectedAtCompileAt10K` green; `grep -rn rodata_base_override compiler/` for the gate's consumer claim |
| V2 ⚑ | Can `cel_unknown_merge` receive two host-minted unknowns (attribute ids dereferenced as descriptor offsets — OOB)? | R43; the ABI doc's unknown wire contract must crown one `payload.unk` shape | e2e PartialEval case `a.x && b.y` with both attributes FULL-matched (ids ≥ 1); inspect result for garbage/trap; if unreachable, document why codegen routes around the merge |
| V3 ⚑ | What is the spec-correct precedence for (unknown, error) operand pairs? | R45; the eval doc states ONE rule and the two losing implementations get fixed | Extend `testdata/cel_cpp_oracle_test.cc` (PartialEvalWithCelCpp) with a strict binary op over one error + one unknown arg, both orders; compare against all three layers |
| V4 ⚑ | Error wire contract end-to-end: does `%v` on a production error render garbage, and does wire code 18 decode as kHostAdapterError? | R41/R42/R44; the ABI doc's error section + the #31 fix shape (bare-code vs message-carrying wire) | (a) wat_runner fixture calling `CEL_LOG("%v", <poison slot>)` with CapturingCelLogSink; (b) e2e `timestamp(0).getHours('NotATz')` asserting `ErrorInfo()->code`; (c) oracle for the same expr's message text |
| V5 ⚑ | What does a `@native` (kCelDefined) library do end-to-end today? | R2/R3; the custom-fn doc's architectural fork decision (reject-at-Compile vs build the library-module producer vs port master-local's inlining) | cc_test: `SetModuleName("foo").AddCelDefined("is_num", bool, {string}, "s == '1'")` → Compile `is_num('1')` → Plan; assert the failure shape (expected: unresolved import at instantiate) |
| V6 ⚑ | Pin `ir::Repr` numeric values | R35; ABI doc wire-stability claim | Add explicit `= N` initializers (or static_asserts) + a test `EXPECT_EQ(static_cast<uint32_t>(Repr::kOptional), 15u)` per member |
| V7 | Is the `variables[]` positional invariant false under comprehensions? | R30; cel_abi.proto comment vs emitter | Compile `xs.map(i, i + y)` with `{"xs:list<int>","y:int"}`, decode, assert `variables(1).local_index()` (predicted 3, not 1); fix proto comment or re-densify |
| V8 ⚑ | `mem_size_bytes` triple probe: static no-op, dynamic page stamping, dynamic >256 KiB Plan break | R6/R8/R72; decides fix-or-delete for the public option + CLI flag + vestigial field | (a) compile `"42"` kStatic at default vs 64 MiB, assert `wasm_bytes()` identical; (b) kDynamic at 3×64Ki, `wasm-dis` and read import min pages == 3; (c) kDynamic at 1 MiB + `Engine::Plan`, observe instantiation result |
| V9 ⚑ | Custom-fn arity hole failure shapes: ≥5-arg decl, 255-param wrap, kType-on-kHost crash | R22, celfn #4; the custom-fn doc places the cap at ONE layer | (a) compile 5-param `@host` decl + call, assert error names the missing helper (not silent miscompile); (b) Build() with 255 params, observe num_args wrap; (c) `AddHost("f", Prim(kType), …)` → Compile, confirm ABSL_CHECK crash on embedder input (rule violation), then extend Build() gate |
| V10 | Ternary with a dyn non-bool cond: cel-cpp error vs our copied-cond value | R62 (slice2 delta); compiler doc's ternary contract | Oracle case `EvalWithCelCpp("dyn(1) ? 2 : 3", "")` + e2e `TryEval` of the same; compare |
| V11 | Would a CelValue layout change fail anywhere before e2e? | 92 §2 magic-number gap; compiler doc's ABI-constants discipline | Temporarily perturb `kCelListEntryStride`/CEL_INT in cel_data.h, run `bazel test //compiler/codegen/...` (expect green = gap confirmed); fix = dep `//runtime:cel_runtime` from `:expr_lower`, replace literals |
| V12 | Does a kStatic Program retain `cel_host.*` imports while importing nothing from `"cel"`? | eval doc link-mode routing section | `bazel run //tools/cel -- compile "1+1" --output /tmp/p.wasm` (default static) then `wasm-objdump -x /tmp/p.wasm` — cel_host imports present, zero `cel.*` |
| V13 | Do all 24 cross-numeric re-pick cells resolve? | codegen-lowering residual-debt note | Parameterized codegen test `dyn(K1) <op> K2` for 4 ops × 6 pairs asserting `cel_numeric_<op>_at_vv` in the body; or `bazel test //e2e:m5_test_dynamic --test_filter='*CrossNumeric*'` (72-row matrix) |
| V14 | Is an optional-typed free variable reachable on the wire (emitter stamps 15, DecodeRepr clamps to kUnknown)? | ABI doc asymmetry note | Attempt optional-typed declaration via variable_specs and DeclareVariable; expect frontend reject; document unreachable-by-construction |
| V15 | Is `Repr::kEnum` producible by the compiler at all? | ABI doc (wire-format-only enumerator?) | `grep -rn "Repr::kEnum" compiler/ eval/` + unit test of both `ReprOf` overloads; if only abi_decode consumes, say so in the doc |
| V16 | Comp-scope map select gets Origin::kHost on an arena value — live mis-route? | R18 edge; compiler doc origin-inference table | e2e `[{'a': {'b': 1}}].exists(m, m.a['b'] == 1)`; if green, document the kind-dispatching kernel that saves it |
| V17 | Frontend gate probes (group): "no type_map entry" arm dead? cel.bind shape false-positives? select-through-Any unit admit? `"%s".format([msg_var])` clean runtime error? wrapper-FQN variable spec repr? | R11 carve-out section of the compiler doc | (a) RejectDyn unit test on hand-built AST with untyped child; (b) probe enumerating each macro expansion vs `IsCelBindShape`; (c) frontend unit tests with an Any-field schema fixture; (d) e2e format-with-message-element; (e) unit test `{"w:google.protobuf.Int64Value"}` asserting `Variable::repr` |
| V18 | Did the optional `or`/`orValue` short-circuit requirement (wat-traces M14.4) land? | compiler doc comprehension/optional section | `grep -n "orValue\|optional_or" compiler/codegen/expr_lower.cc` + oracle/e2e `optional.of(1).orValue(1/0)` asserting no spurious error |
| V19 | What does an embedder observe when a BindFunction callback returns non-OK (status code lost — #31 trap path)? | eval doc host-fn error contract | e2e registering `return absl::InvalidArgumentError("boom")`; assert exact code+message from `Instance::Eval`; decide contract-or-bug |
| V20 | Does zero-arg `Eval()` (no externref Reset) leak/grow across calls? | eval doc Eval lifecycle | Probe test calling `instance.Eval()` N times on a program whose host fn `ReturnProto`s; observe table growth/decoded results |
| V21 | Where does a wrong-arity component export fail? | R36; eval doc AddComponent contract | Extend `e2e/foreign_component_dispatch_test.cc` with a wrong-arity WAT component; assert trap site + message |
| V22 | Stale-skip sweep: which lingering skips are removable? | R21/R48; testing doc skip-discipline inventory | Delete skips at m2_test.cc:206,210; m4_test.cc:448; expr_lower_test.cc:600; engine_test.cc:850; one foreign_fn_type_matrix B0 cell — run each target; classify pass/fail-with-new-reason |
| V23 | Does `container` work through public `Compiler::Compile`? | compiler doc options table (only unpinned knob) | Test: declare `c: celwasm.testdata.Customer`, `opts.container = "celwasm.testdata"`, compile short-form reference; + no-container control |
| V24 | `optimize_level = -1` silently level-0; `= 4` rejects? | R7; pin whichever contract the fix chooses | Facade test: `-1` expect OK + bytes == level-0 output; `4` expect InvalidArgument |
| V25 | Are the three LinkMode enums (public/internal/abi proto) locked? | compiler doc; silent-miscompile hazard | static_assert pairs or a test asserting `static_cast<int>` equality across all three |
| V26 | Is Builder double-`Build()` defined behavior? | compiler doc Builder contract | Test second `std::move(b).Build()` on a moved-from Builder; decide accept-empty vs reject |
| V27 | Does `demo_component_proto` actually build/link (stub symbol case vs wit-bindgen's expected symbol)? | custom-fn doc component naming chain | `bazel build //e2e/foreign_component_fixtures/cel_wasm_component_demo:demo_component_proto` (manual target); if it links, document why |
| V28 | Map-return through the generator fails on the missing `codec::lower`? | R23; custom-fn doc generator matrix | `cel generate` on `map<string,int> @component.f(int x);` + compile the four files; then wire full_matrix.idl into the planned compile gate |
| V29 | `cel generate` end-to-end + the missing run_generate test | tools coverage; R49 | `bazel run //tools/cel:cel -- generate --idl examples/adder.idl --out_dir /tmp/gen`; diff vs the macro's generated files; add run_generate_test.cc |
| V30 | Does `cel eval` handle activation-bound lists/maps (smoke comment vs README)? | R51 | `bazel run //tools/cel:cel -- eval "xs.exists(x, x > 5)" --var "xs:list<int>=[1, 3, 5, 7]"` and `… eval 'm["us"]' --var 'm:map<string,int>={"us": 1, "ca": 2}'`; upgrade smoke cases or fix README |
| V31 | Have the ~30 never-loaded WATs rotted? | R50; testing doc WAT-layer claims | `for f in <the never-loaded list>; do wasm-as "$f" -o /dev/null || echo "BROKEN: $f"; done` |
| V32 | Is the wasmtime C-API tail-call panic (dispatcher WATs 09/14) still live? | testing doc wat_runner limitation note | Delete the GTEST_SKIP at wat_runner_test.cc:515-518; run the test under a current wasmtime |
| V33 | Example 09 visibility: promotion or README correction? | R24 | `bazel query "visible(//examples:09_component_function, //compiler/celfn:function_library)"`; decide widen-with-review vs re-scope the example |
| V34 | Fix kernel_bench's dead cursor pokes and re-baseline the allocating rows | R59; benchmarking doc baseline validity | Replace `cel_mem_base()+8` pokes with `arena_reset()` per iteration + re-staging (or a real rewind API); re-run `bazel run -c opt //bench:kernel_bench` and diff vs published numbers |
| V35 | Register or delete `cel_pipeline_bench.cc` (+ fix the two docs citing it) | R53 | `bazel query 'kind(cc_binary, //bench:all)'` (expect 6); then decide; same commit fixes POST_MIGRATION_BENCH.md + per-component-test-coverage.md:94 |
| V36 | How were the m28 full-corpus tables produced? | R55; benchmarking doc reproduction section must be honest | Run `benchmark/eval/run.sh smoke`; diff report.sh output shape vs m28-bench-results §1-§5; recover or rewrite the analysis pipeline before the next publish |
| V37 | Does flipping `CELWASM_M7B_SHIPPED` even compile? | R56; delete-or-fix decision | `bazel build -c opt //bench:kernel_bench --copt=-DCELWASM_M7B_SHIPPED` (expect failure on `cel_ts_year_utc`) |
| V38 | Do corpus paths survive non-repo-root invocation; do the two mains' prefix/file tables agree? | benchmarking doc harness contract | Run `bazel-bin/benchmark/eval/celwasm_bench --benchmark_filter=BM_arith_intAdd2` from repo root AND another cwd; diff the two static tables (celwasm_bench.cc:189-212,355-369 vs celcpp_bench.cc:222-245,305-319); add a shared header or pin test |
| V39 | Is the conformance README in sync (fail=92 vs "93" prose)? | R67; testing doc gate description | `bazel run //conformance:run_conformance` then `scripts/regen_conformance_readme.sh --check` |
| V40 | Manual-target query completeness + which conformance FAILs lack pinning tests | R47; testing doc coverage ledger | `bazel query 'attr(tags, "manual", tests(//...))'` vs the §1.1 list; cross-ref `run_conformance -- --max_fail_examples=100000` output against greps over e2e/ (proto2-ext/parse/enums buckets ≈ 57 unpinned rows) |
| V41 | Do the dual-mode e2e targets actually run in the default suite? | testing doc layer table | `bazel query 'tests(//e2e/...) except attr(tags, "manual", tests(//e2e/...))'`; then `bazel test //e2e/...` |
| V42 | Catalogue ⇄ actual wasm exports (post-deletion of the consistency test) | R32 residual audit; ABI doc | `wasm-dis bazel-bin/runtime/cel_runtime_wasm.bin/cel_runtime.wasm | grep '(export' | sort` vs `CelRuntimeHelpers()` names — once per toolchain bump |
| V43 | Is `CelAbi.version` (field 1) deliberately never enforced? | ABI doc version-policy section | Hand-roll a section with `version=99` + one variable; `Engine::Plan` — it will load; record the policy decision |
| V44 | `arena_alloc(0)` contract vs `WasmtimeArenaAllocator`'s 0→nullptr | ABI doc arena contract | wat_runner call of `arena_alloc(0)`; assert non-zero offset (A9); else document the EncodeSpan empty-path save |
| V45 | Runtime-kernel small probes (group): opacity barrier still required? optional/math TUs only via archive pull? alloc-before-init traps cleanly on wasm? does EMITTED wasm guard `arena_alloc == 0`? harness `cel.memory` define dead? | runtime doc build/arena sections | (a) build `cel_runtime_wasm.bin` without the barrier, run `//runtime:cel_runtime_wasm_test`; (b) `wasm-dis … | grep -c 'cel_optional_\|cel_math_'` + try adding TUs to srcs; (c) wasm test calling `arena_alloc` without init, assert trap not panic; (d) compile a comprehension via tools/cel, `wasm-dis`, look for `i32.eqz` guards at arena_alloc sites; (e) delete the harness define, re-run |
| V46 | Is Pass D's two-traversal layout compatible with real slot reuse (prereq for any future free-list)? | compiler doc layout section future-work | Implement a free list behind `!debug_mode`; run layout_pass_test + e2e on `c.f[x].g + c.h` shapes; check for aliasing between live select and call results |
| V47 | Re-pick under disable_check / pre-stamped cross ids benign? | compiler doc cross-numeric section | `bazel run //tools/cel -- check '1 < 2u'` parse-only + probe printing `effective_ann.overload_id` for a hand-built annotation |
| V48 | Which seed satisfies `kTimestampToDate`/`WithTz` in the coverage tripwire? | overload-table section completeness | `bazel test //compiler/codegen:overload_table_test` + one-line print of the cel-cpp constant |
| V49 | Is the manual 32-bit-halves overflow multiply still needed post-wasi-sdk? | runtime doc arith note | `grep -n "uint64_mul_overflows\|__builtin_mul_overflow" runtime/cel_arith.c` |
| V50 | `\uXXXX` escape, `cel compile` stdout purity, manual tags on function_library_test intentional? (tools odds-and-ends group) | tools corrections | (a) `cel eval "s" --var 's:string="A"'` expect unknown-escape error → fix var_parser.h:25 + README; (b) `cel compile "1+1" > /tmp/x.wasm && head -c4 | od -An -tx1` == `00 61 73 6d`; (c) record or remove the manual tags at compiler/celfn/BUILD.bazel:41,51 |

Settled during the notes pass (do not re-probe): FieldRefEntry is
descriptor-uncached per call (eval-internal D6 verified — design.md's
claim was right, cel-host-surface's was wrong); backlog #16's layer
attribution (R29, pending only V1's commit); the 20-vs-12 import-count
and page-count "conflicts" (consistent, §2.4).

---

## 4. Proposed new design-doc set (`doc/design/`)

Reader path (per notes/README principle 4): 00 → 01/02 (subsystems) →
03/04/05 (contracts they share) → 06/07 (quality systems). All docs
share one diagram palette; every claim cites a notes section or a
resolved V-item. Fuzzing is **excluded** — planned on a separate
branch; 06 reserves a stub section pointing at it.

### 00-architecture.md — System architecture & lifecycle
**Scope.** The one-page-deep system view a newcomer reads first: the
Compiler → Program → Engine → Instance role split and why (Program as
pure-bytes serialization boundary; compiler stays wasm-targetable);
the link-mode fork as THE top-level architectural decision (static
default, dynamic opt-in, why the original recommendation flipped —
design.md §12 Q1's reasoning still governs dynamic mode); the four
enums and their alignment rules; repo layout by lifecycle role; the
end-to-end thread-safety contract **including the compiler half** (the
undocumented Binaryen process-global optimize race, R71).
**Outline.** 1. Problem & goals (restated from design.md §0-§2, the
still-true subset). 2. Role split + lifecycle diagram. 3. Link modes:
both shapes, routing-by-import-introspection, the flip rationale.
4. The data contracts at a glance (Program bytes, cel.abi, Activation,
Value). 5. Threading model end-to-end. 6. Map of the doc set.
**Supersedes (archive banners):** `rw/design.md` (whole, already
self-declared historical), `rw/two-phase-runtime-isolation.md`,
`rw/m28-configurable-linking.md` (status text retained as history).
**Fed by:** compiler-toplevel, eval-public, design-heritage, 91.

### 01-compiler.md — Compiler design (the pass pipeline)
**Scope.** The compiler as a pass-contract chain — parse/check →
rewrites → RejectDyn → annotate → resolve → layout → lower → finalize —
each pass stated as consumes/produces/invariant-established/breaks-if-
reordered (notes/README principle 2). Every mechanism at its spot in
the spine: the static-subset gate with its five carve-outs (R11), the
NodeAnnotation side-table (full 12-field schema, R10), origin
inference as-shipped (R18's three real rows, the doc table's unshipped
rows moved to future work), the linear-memory map + the NEW
compile-time static-region gates (V1), StaticMemoryBuilder, the
truthful SlotAllocator (no-op release, total-acquires, the capacity
consequence — R1/R16), the kCall dispatch ladder, comprehension
lowering (incl. the R15 known bug), cross-numeric re-pick with the
probe rationale, OverloadTable + coverage tripwire, WasmModule rules
(memoryName=nullptr, Adopt union, validate→optimize→serialize), both
link-mode compile arms sharing `LowerExportAndFinalise`, and the
public options table with each knob's real effect (R7/R8/V8/V23).
**Outline.** 1. Pipeline overview + pass-contract table. 2. Frontend
(parse/check, schema overlay, rewrites, the gate). 3. IR & annotations.
4. ResolvePass. 5. LayoutPass & the memory map (canonical region table
+ gates). 6. Lowering (per-arm, WAT-first cross-refs to wat/*.wat
files, never inline listings — design-heritage §4 lesson). 7. Module
finalization + link modes. 8. Public surface & options. 9. Rejected
alternatives (Strahler slots, MessagePattern, always-host dispatch…).
10. Future work.
**Supersedes:** compiler sections of `rw/design.md`;
`rw/memory-layout-design.md` (jointly with 03);
`rw/map-list-dispatch.md`; `rw/m5-kcall-comprehensions.md` +
follow-on; `rw/cross-numeric-ordering-plan.md`;
`rw/slice2-control-flow-plan.md`; `rw/dyn-passthrough-plan.md`;
`rw/m9-type-subsystem.md` (mechanism parts; milestone history stays
[hist]).
**Fed by:** frontend-ir, codegen-memory, codegen-lowering,
compiler-toplevel, design-heritage, 90, 91.

### 02-evaluator.md — Evaluator design (plan / instantiate / eval)
**Scope.** The eval side on its lifecycle spine: Engine construction
and what it caches (bench-justified module cache), the Plan pipeline
step-by-step (ABI decode + tolerance policy, import-shape link-mode
routing with the label as tripwire-only, dynamic vs static
instantiation, the binding order), the registration surfaces
(AddModule / AddFunction / AddTypedFunction / BindFunction /
AddComponent — with the REAL AddComponent contract incl. call-time
arity traps, R21/R36), the m21 three-layer host-call stack, the
cel_host trampoline architecture (three layers, bijection-checked
import table, one-call walkthrough), activation marshal (buffer-
outside-arena rationale, the three deliberate coercions, PartialEval
pattern lifecycle), result decode + Value model (truthful builder
status, R38-R40), and the Instance threading/lifetime contract.
**Outline.** 1. Roles & lifecycle. 2. Plan, step by step (per link
mode). 3. Registration surfaces & validation points. 4. Host-call
dispatch (L0 trampoline / L1 HostCallContext / L2 typed adapter).
5. The cel_host surface (backings, trampolines, externref namespaces,
WKT peel/pack chains). 6. Activation marshal & PartialEval. 7. Eval &
decode. 8. Error/unknown propagation (cites 03's single contract).
9. Components (m24 path, lift/lower, wasip2 stubs). 10. Future work.
**Supersedes:** `rw/cel-host-surface.md` (jointly with 03 — its wire
sections go to 03, its surface sections here);
`rw/m21-host-call-adapter.md`; eval half of
`rw/two-phase-runtime-isolation.md`; eval sections of
`rw/m24-foreign-fn-component-backend.md`.
**Fed by:** eval-public, eval-internal, abi-shared, 91.

### 03-abi-and-memory.md — The wire contracts & memory model
**Scope.** One telling for every byte-level fact, written from lens 90
§1 (the verified-consistent set): CelValue layout + strides + the
CelKind table; the four-enum alignment rules (restate abi-shared §5
verbatim); the uniform slot-out calling convention + sanctioned
exceptions; the canonical memory-region table and the page-count table
(90 F10); the arena contract (A-invariants, OOM-is-a-value, offset-0
sentinel); the `cel.abi` section field-by-field with the two-version
policy; the runtime catalogue (marker derivation, four namespaces, the
20-vs-12 import-set table — 90 F8); link-mode label semantics; and the
**error & unknown wire contract** — bare-code errors (with the #31
decision from V4), ONE `payload.unk` shape (V2's verdict), ONE 3VL
precedence rule (V3's verdict) — plus the WIT vocabulary for the
component boundary.
**Outline.** 1. CelValue & kinds. 2. The four enums. 3. Calling
convention. 4. Memory map (regions, pages, gates) — canonical table.
5. Arena. 6. cel.abi (fields, versions, link-mode label). 7. Runtime
catalogue & import namespaces. 8. Errors & unknowns on the wire (the
crowned contracts + oracle citations). 9. Component-boundary WIT.
10. Change discipline (what bumps `runtime_abi_version`, Repr pinning
per V6).
**Supersedes:** wire sections of `rw/cel-host-surface.md`;
`rw/memory-layout-design.md`; `rw/abi-refactor.md`.
**Fed by:** abi-shared, runtime-kernel, codegen-memory, eval-internal,
90, 91.

### 04-runtime.md — The runtime kernel
**Scope.** `runtime/` as a component: the twice-compiled design
(wasm32-wasi-threads artifact / native twin / m28 stripped variant)
with the load-bearing flags (`-mtail-call`, `-flto`, `--global-base`);
kernel conventions (slot ABI, 3VL-absorb-then-kind-check, poison
vs trap regimes — the complete deliberate-trap list); three-path
dispatch + musttail; map/list/iteration internals; the export/import
catalogue mechanism from the runtime's side (markers, wasm_exports
host-only bucket, the archive-pull link topology caveat per V45);
arena-sizing cliffs as documented limitations; weak host stubs.
**Outline.** 1. Artifacts & build topology. 2. Wire data (cites 03).
3. Kernel conventions & the two failure regimes. 4. Aggregates &
iteration. 5. Memory substrate (opacity barrier status per V45,
relocation discipline). 6. Export catalogue mechanics. 7. Known
limitations (64 KiB cliff, pre-size/trap pair). 8. Future work.
**Supersedes:** `rw/wasi/DESIGN.md` (already historical; banner
pointing here); `rw/cel-runtime-c-split-plan.md`;
`implementation-plan/runtime-catalogue-genrule.md` (folded in).
**Fed by:** runtime-kernel, 90.

### 05-custom-functions.md — The custom-function subsystem
**Scope.** The one cross-cutting subsystem doc (it spans compiler,
eval, tools, and build): the `.celfn` IDL + grammar, the single decl
struct and three backends, overload-id synthesis as the load-bearing
identity (checker stamp = table key = import name = callback key =
kebab'd export name), the Builder validation funnel **with the arity
cap placed at one layer (V9's outcome)**, host binding (BindFunction
declaration-first path), the component backend end-to-end (celfnc
emitters, `cel generate`, the bazel macro, wasip2-direct-component),
and — the key decision this doc must make, not record — **the
kCelDefined fork**: single-module inlining (master-local) vs bundled
library module (this branch's orphaned header) vs reject-at-Compile
until one ships (V5 feeds the choice). Until decided, `@native` is
documented as declaration-only with a Compile-time rejection proposal.
**Outline.** 1. The decl model & backends. 2. Overload-id identity
chain. 3. IDL & Builder gates (incl. the arity cap decision).
4. @host end-to-end. 5. @component end-to-end (generators, macro,
naming translations, ownership rules). 6. @native: the fork, the
decision, the migration path. 7. Type-surface policy (kType/kOptional
permanently out on foreign; null distinct). 8. Future work (Go path,
full-matrix gate, map-lower emitter).
**Supersedes:** `rw/m13-custom-fns.md`, `rw/m22-foreign-fn.md`,
`rw/m24-foreign-fn-component-backend.md`,
`rw/m26-celfnc-and-component-build.md`, `rw/modules-and-fnis.md`.
**Fed by:** celfn, eval-public, eval-internal, tools-examples, 91.

### 06-testing-strategy.md — Testing strategy
**Scope.** Per lens 92's outline, adopted nearly whole: the 10-layer
model (units / native-twin / build-time compile gates / WAT harness
with HONEST coverage numbers (R50) / dual-link-mode e2e / known-bugs
registry / conformance + monotonic gate / oracle / examples smoke /
benches-as-probes), the gates (query-driven manual list — cite the
query, never names), the disciplines (skip rules + the stale-skip
audit proposal, known-bugs lifecycle, dual-emission, matrix rules,
oracle-outranks-reading), the coverage ledger (successor of
per-component-test-coverage.md §2/§3 with dead targets fixed; the §2
gap register from lens 92 as the open-items table), and an honest
"what green does NOT mean" section (monotonic-gate blindness,
kind-only matchers, manual-tag bit-rot, the magic-number gap).
**Outline.** 1. Philosophy. 2. The layer pyramid. 3. The gates.
4. Disciplines. 5. The oracle. 6. Coverage ledger. 7. Fuzzing —
**stub section**: deferred to the dedicated fuzzing branch; records
only the four target surfaces lens 92 §4 identified so the branch has
its brief. 8. Known weaknesses of the testing system itself.
**Supersedes:** `implementation-plan/per-component-test-coverage.md`
(catalog + closeout sections; the M2-incident narrative is preserved),
`rw/feature-pipeline-checklist.md` (path-rotted; regenerated as a
section here), `implementation-plan/testing-checklist.md` header text
(the grid itself may live on as an appendix/ledger).
**Fed by:** testing-system, 92, tools-examples, design-heritage §4.

### 07-benchmarking.md — Benchmarking design
**Scope.** Per lens 93's outline §8, adopted whole: the two-tree split
(localisation vs publication) as the top-level decision; the measured-
boundary table mapped to the four host caches; the production-config
contract as FOUR axes (-c opt + CEL_LOG kill, -O3 -flto, Binaryen O2,
link mode — with the bench-default-vs-production-default deviation
stated); the comparative harness (corpus schema, linkage isolation,
same-BM-name join, skip-tag regime, OPERATORS.md as ledger); the
honest-reporting rules as structural requirements; baselines &
reproduction (canonical numbers: 62/230 ns floors, slopes, N=10
crossover, 20% noise rule; the honest admission that the full-corpus
analysis pipeline must be rebuilt — V36); perf-model facts from the
architecture (eager &&/||, constant-aggregate rebuild, arena cliff, no
slot reuse); and future work incl. a perf analogue of the conformance
monotonic gate, machine parity, and bench build-smoke in CI so manual
binaries can't bit-rot (R56/R59 as the cautionary tales).
**Supersedes:** `benchmark/DESIGN.md` (re-statused plan-with-deltas or
archived), `rw/wasi/POST_MIGRATION_BENCH.md` (tombstone after V35).
**Fed by:** benchmarking, 93, runtime-kernel, codegen-lowering.

**Plus one housekeeping deliverable (not a design doc):** graduate
`notes/doc-index.md` into a corrected `doc/README.md` router — with the
two "[live]" mislabels fixed (90 F2), archive banners added to every
superseded doc above, and the code-comment reverse index (12 doc paths
cited from C++) honored: any rename above lands with same-commit
comment updates.

---

## 5. Docs that remain authoritative as-is

| Doc | Why it stands |
|-----|---------------|
| `doc/langdef.md`, `doc/extensions/strings.md`, `spec/tests/` | Upstream contract; we vendor, never rewrite. |
| `conformance/README.md` | Auto-regenerated + drift-gated (pre-push `--check`); only the hand-prose fail-count off-by-one (V39) needs a touch. |
| `bench/README.md` | Stays [live] per lens 93 after the small fixes (Opt2 pairing note, stale future-work rows); its boundary-taxonomy and config-rule content feeds 07 but remains the operator manual. |
| `rw/m28-bench-results.md` | The published-results exemplar of the honest-narrative rule; keep as a dated results artifact (fix the §2†/§6 self-contradiction). |
| `tools/cel/README.md`, `examples/README.md` | Operationally accurate modulo P2 nits (R69); they are user manuals, not design docs — corrected in place, not superseded. |
| `implementation-plan/{lint-backlog,cleanup-backlog,dev-loop-performance,known-issues-findings}.md` | Living ledgers; cleanup-backlog needs entry rewrites (#16 per R29, #2 per R67) but the ledger mechanism is the keeper. |
| `doc/contributing.md`, `CLAUDE.md` | Process docs; two targeted corrections (WAT-corpus claim R50, oracle-gaps claim R67) rather than rewrite. |
| `rw/wat/*.wat` + `wat-traces.md` prose | The on-disk WAT files are the maintained regression corpus (never inline listings in new docs); wat-traces stays as the per-arm walkthrough reference with the R70 corrections + a banner deferring the memory map to 03. |
| All other `rw/mNN-*.md` milestone docs | Historical intent ([hist]); they get archive banners pointing at the superseding design doc, not rewrites — the git-log-free "why" trail. |
| `abi/wit/README.md`, `user-guide/**` | Live with one-line corrections (R65); user-guide is a separate audience from design docs. |

---

## 6. Disposition reminders for the doc-authoring pass

1. **Run V1 first.** Until the working-tree gates are committed and
   citations re-anchored, every memory-model sentence is provisional
   and the register double-counts fixed items as open.
2. **V2–V5 before their sections.** The ABI doc's unknown/error/3VL
   contracts and the custom-fn doc's fork decision cannot be written
   from the notes alone — they need the oracle/probe verdicts.
3. **Header-comment fixes ride the doc commits.** Each register row
   that names a header (R4-R8, R12-R13, R16-R17, R25-R28, R34, R36-R40)
   should be fixed in the same commit as the doc section that would
   otherwise contradict it — stale headers are how the next
   design-heritage-style misreading happens.
4. **The notes are disposable** (README): once 00–07 ship, this
   directory gets archived; the validation backlog items that remain
   open migrate to cleanup-backlog.md with their V-numbers.
