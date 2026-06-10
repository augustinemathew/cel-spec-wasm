# design-heritage — design notes (undefined)

Scope: the two historical master docs —
`doc/implementation-plan/rewrite/design.md` (4027 lines, status line self-declares
"historical design baseline", design.md:3) and
`doc/implementation-plan/rewrite/wat-traces.md` (2548 lines, header self-declares the
memory map SUPERSEDED, wat-traces.md:3-18).  Deliverable: a section-by-section
supersession map deciding what the new design docs restate vs archive.
Classifications: **STILL-TRUE** (restate), **SUPERSEDED-BY <x>** (archive; cite
successor), **WRONG** (never matched code / invariant broke).

## 1. Verified architecture — section-by-section supersession map

### 1.1 design.md

| § | Verdict | Evidence (one line) |
|---|---|---|
| Header status + Phase C callout (1–58) | STILL-TRUE for kDynamic only; SUPERSEDED for the default by m28 static linking | `CompileOptions::LinkMode link_mode = kStatic` is the default (compiler/internal/compile.h:102, compiler/compiler.h:145); `CompileStatic` adopts the wrapper-stripped runtime as the base module, skips `InstallExprModuleImports` entirely (compile.cc:616-647) — no `cel.memory` import in the default mode |
| Shipping snapshot table (60–80) | SUPERSEDED — frozen at 2026-04-25 | claims customs (§4.6) and proto literals (§4.7.1) "not shipped"; both shipped (engine.h:177-244; expr_lower.cc:613 `EmitKStructExpr`); kBuiltinSeeds "80" is now 271 (overload_table.cc:96) |
| §0–§2 problem/goals | STILL-TRUE as motivation; restate goals 1,2,5,6,7,8 | goal 6 (literals unconditionally in rodata) holds (`StaticMemoryBuilder` infallible `uint32_t` returns, static_memory_builder.h per design §6.2.1); goal 3 `--debug-layout` exists as `LayoutOptions::debug_layout` (layout_pass.h:51) |
| §3.1 pipeline diagram | STILL-TRUE | parse → check → resolve → layout → emit is `RunFrontAndLayout` + `LowerExportAndFinalise` (compile.cc:579-614) |
| §3.2 memory regions (Phase C callout) | STILL-TRUE for kDynamic; the topology is now mode-dependent | dynamic: runtime owns shared `cel.memory`, expr imports it (engine.cc:182 pulls "memory" off `helpers_instance`); static (default): the Program module IS the runtime — one module, own memory.  `[0,8192)` reserve + malloc-backed arena verified: `CELWASM_RESERVED_LOW_MEMORY_BYTES 8192` (runtime/cel_layout.h:37), `g_arena` BSS struct + `arena_init/alloc/reset` (runtime/cel_arena.c:37-110) |
| §3.2 "Original drafting (historical)" block (364–387) | SUPERSEDED-BY Phase C (doc says so itself) | archive |
| §4.0–§4.1 NodeAnnotation | STILL-TRUE in concept; field list STALE | as-shipped adds `message_type_id` (M7), `comp_aux_local_base` + per-comprehension iter/accu binding fields (M5-followon), `Repr::kOptional` (M14) — compiler/ir/annotations.h:36, 97-120; none appear in design.md §4.1 |
| §4.2 uniform call ABI | STILL-TRUE — restate verbatim | every kernel is `(i32 out_slot, i32 args…) -> void`; receiver-form calls flatten target→arg0 at codegen (wat-traces M14.2; expr_lower `EmitGeneralCall`); exceptions: `&&`/`||` eager helpers, `?:` inline branch (wat 30-33) |
| §4.3 OverloadTable | STILL-TRUE structurally; counts stale | builder→frozen table, string-keyed `Lookup`, deque ownership all live (overload_table.h:60-189); seeds 80→271 (overload_table.cc:96), kExplicitlyUnimplementedIds 86→6 (overload_table.cc:750) |
| §4.4 codegen dispatch | STILL-TRUE shape | kCall arm: special arms (`_[_]`, control flow) then `Lookup(a.overload_id)` (expr_lower.cc:1099-1201) |
| §4.4.1 `kPendingRuntimeExports` guard | SUPERSEDED — guard removed | `grep kPendingRuntimeExports compiler/ eval/` → zero hits; M5.D step 2 flipped it; section is purely historical |
| §4.5 coverage tripwire | STILL-TRUE | `kBuiltinSeeds`/`kExplicitlyUnimplementedIds` tripwire alive (overload_table.cc:736-765); `CompileOptions::allowed_overloads` still NOT shipped (grep empty) — the delta callout remains accurate |
| §4.6 custom functions (+§4.6.1–4.6.4) | SUPERSEDED-BY the `.celfn` / engine-method surface (m13/m21) | no `RegisterFunction(FunctionDecl)`, no `RuntimeBindings`, no `Plan(program, bindings)` anywhere.  As-shipped: `Compiler::Builder::AddFunction(celfn_source)` (compiler.h:266) + `CompileOptions::function_libraries` (compile.h:81-90); eval side `Engine::AddFunction` / `AddTypedFunction` / `BindFunction(celfn_decl, fn)` (engine.h:205-244); `Engine::Plan(const Program&)` is the only overload (engine.h:117).  The §4.6.1 *principle* — one wasm import per custom, uniform slot-out, no shared trampoline — survived and should be restated |
| §4.7.1 proto literals | SHIPPED as designed (M7) — STILL-TRUE | `EmitKStructExpr` + `MaybeEmitWktUnwrapTailCall` (expr_lower.cc:535-660), `NodeAnnotation::message_type_id` (annotations.h:97-104); M20 poison-on-error contract supersedes the "non-OK Status" wording for value-range errors (wat-traces M20.1) |
| §4.7.2/§4.7.3 map/list three-path dispatch | STILL-TRUE — restate | `Origin` enum + visitors (annotations.h:62-80); `cel_list_create/set` fixed-length API plus the later `cel_list_append_at` family for comprehensions (runtime/cel_list.h) |
| §4.7.4–§4.7.6 cel_host three-layer split | STILL-TRUE in shape | `HostMessageBacking`/`ProtoBacking`, Layer-2 `Cel*Impl`, Layer-3 `RegisterCelHostImports` all live (eval/internal/cel_host.{h,cc}, cel_host_wasmtime.cc); trampoline list has grown far beyond the doc's 4–12 (timestamp_tz_accessor, wkt_unwrap_wrapper, set_field, map_iter host arms — cel_host_wasmtime.cc) |
| §4.8 + §5 ResolvePass | STILL-TRUE interface; internals superseded | the scope-FLAT `IdentResolver` + comprehension early-reject (§10.2 delta) were replaced by `ScopedIdentResolver` with per-comprehension iter/accu bindings stamped on the annotation (annotations.h:106-120) |
| §6 LayoutPass / SlotAllocator | PARTIALLY SUPERSEDED | `debug_layout` shipped (layout_pass.h:51).  `Release` is no longer a no-op — free-list reuse landed ("M10 flips on the free list", slot_allocator.h:77, worked example :64-73).  `PushScope`/`PopScope` never landed; comprehensions use a free-cursor snapshot per `ComprehensionFrame` instead (wat-traces §66 invariant 1).  §6.3 slot-Strahler / Sethi–Ullman aliasing NEVER shipped — "S10 … not pursued; the naive slot allocator stayed" (design.md:7); reuse is release-based, not Strahler-aliased |
| §7 codegen interface | STILL-TRUE shape | `LowerToEvalFunction(ast, layout, name, mod, overload_table, opts)`; `(call $arena_reset)` zero-arg prologue confirmed (expr_lower.cc:136, 1230) |
| §7.3 "what disappears" | DONE — historical | v1 surfaces gone with the compiler_v2 swap (landed 2026-05-25, design.md:6) |
| §7.4 import installation | PARTIALLY SUPERSEDED | dynamic mode still installs imports eagerly off the catalogue; static mode (default) installs only `cel_host.*` internal-name imports and rodata into the adopted runtime module (compile.cc:631-643 `InstallCelHostImports`, `InstallExprRodataSegment`) |
| §8.1 build flags | PARTIALLY SUPERSEDED | flags block is right, but `wasm_exports.txt` is NO LONGER the single source of truth for codegen helpers: the catalogue is generated from `// cel:codegen-export` markers in `runtime/cel_*.{h,c}` by `//bazel:gen_runtime_catalogue`; wasm_exports.txt now lists host-only symbols (wasm_exports.txt:1-40; bazel/gen_runtime_catalogue.py exists).  A first-party `bazel/` dir now exists |
| §8.2 arena (Phase C callout) | STILL-TRUE — restate | matches runtime/cel_arena.c exactly (absolute-offset `arena_alloc`, trap-before-init A16, native build over `g_memory`) |
| §8.2 "As-shipped (M1)" cel_reset/cel_alloc code block | SUPERSEDED | `cel_reset`/`cel_alloc` deleted; bytes-8/12 cursor gone (cel_arena.c) |
| §8.3 reset ownership (Phase C callout) | STILL-TRUE | codegen emits the prologue, host never resets per-Eval; `arena_init` once per Instance (engine.cc:324 `SeedRuntimeArena`) |
| §8.4 aliasing-ABI test | STILL-TRUE as requirement | aliasing tests exist (e.g. runtime/cel_compare_test.cc); the invariant is load-bearing for the release-based slot reuse that DID ship |
| §8.5 error provenance (`CelErrorPayload.expr_id`) | NEVER SHIPPED | no `expr_id` in runtime/cel_data.h (grep empty); S10/S11 abandoned.  Archive as a rejected/deferred idea |
| §9 host runtime + §9.1 two-phase instantiation | STILL-TRUE for kDynamic; SUPERSEDED for the default mode by m28 | static-mode Plan: `BindStaticModeHelpers` aliases `helpers_instance = expr_instance`, runs `__wasm_call_ctors` once, then the shared post-instantiate bindings (engine.cc:398-435); `ValidateLinkModeLabel` cross-checks the `cel.abi` link_mode label against import-derived reality (engine.cc:508-523, 1369-1373) |
| §9 delta "Plan(program, bindings) at M6/M7" | WRONG (prediction never materialized) | bindings-style Plan never shipped; engine-level registration replaced it (engine.h:117, 177-244) |
| §9.2 deletions | STILL-TRUE — historical record | host_loader gone; arena_init is the only host-driven arena call |
| §10.1 (M2), §10.3 (M3), §10.4 (M4) absorption | STILL-TRUE — accurate close-out records | restate the absorbed invariants (codegen oblivious to partial-eval; origins write-once) |
| §10.2 comprehensions (M5) | invariant BROKE, doc never updated | §10.2.3 "No new NodeAnnotation fields" — M5-followon added `comp_aux_local_base` + iter/accu binding fields (annotations.h:106-120) and `LayoutPass` grew `comprehension_extra_locals_per_comp`.  §10.2.2's "accu lives in a wasm local / PushScope-PopScope slots" does not describe the shipped shape (accu is a workspace slot; aux locals via base index) |
| §11 implementation plan (slices, layout, estimates) | SUPERSEDED — pure history | S7 ("PENDING post-M5") shipped as m13/m21; S9 ("PENDING M7") shipped; S10 abandoned; S12 swap landed 2026-05-25 (design.md:6); §11.9 checkboxes and §14 deliverables frozen mid-flight.  Archive whole |
| §12 Q1 "inline runtime into expr module? Recommend not" | SUPERSEDED — became m28 kStatic, the DEFAULT | compile.cc:616-647; the rejected option won.  New docs must state both modes and why static became default (m28-configurable-linking.md) |
| §12 Q2 bytes-8/12 cursor | SUPERSEDED-BY Phase C arena | cel_arena.c |
| §12 Q3 boxed doubles, Q4 no rodata cap, Q5 externref per-instance, Q7 split tables, Q8 no expr_id on Storage | STILL-TRUE — restate Q4/Q7/Q8 as standing decisions | static_memory_builder infallible API; externref table host-side per Instance |
| §13 completion summary | MOSTLY-TRUE, one wrong clause | "workspace slots pre-assigned by Sethi–Ullman" — never shipped; reuse is release-based (slot_allocator.h:64-77) |
| §14 deliverables checklist | SUPERSEDED — stale boxes | "kBuiltinSeeds empty (M5 fills)" etc.; archive |

### 1.2 wat-traces.md — entry-by-entry

The doc's own header (wat-traces.md:3-18) already declares the memory map
superseded and points at `wat/*.wat` as the maintained set.  Verified: **0 of 59**
`wat/*.wat` files still import `cel_reset`; **59** use `arena_reset` + shared-memory
import (`(import "cel" "memory" (memory 2 1024 shared))`, e.g. wat/01_literal_42.wat:13-15).
So every INLINE WAT listing in the .md (entries 1–5, 40–41, 50–55) is drifted from
its on-disk twin; the prose ABI/arm shapes are what's still load-bearing.

| Entry | Verdict | Evidence |
|---|---|---|
| Memory map header (49–80) | SUPERSEDED (self-declared) | bytes-8/12 cursor gone (cel_arena.c) |
| Ident→local mapping | STILL-TRUE doctrine | uniform `(local.get <idx>)` kIdent arm survived through M5 comprehensions (wat 60 invariant 2) |
| 1 literal, 2 ident, 3 two-idents, 4 select | shape-accurate, inline listings drifted | on-disk twins migrated; 3's `unreachable` stub superseded by 16 |
| 5 comprehension prototype | SUPERSEDED-BY 60 (doc says so at §60) | archive |
| 6 (first) `arr[0]` via `cel_list_index_at_vv` | WRONG — helper never existed | grep `cel_list_index_at_vv` runtime/ compiler/ → zero hits; shipped names are `cel_list_at{,_arena}` (entries 11–15).  Note: the doc numbers TWO sections "6" |
| 6–15 map/list literals + 3-path index | STILL-TRUE arm shapes | kernels + origins verified (annotations.h:62-80; runtime/cel_map.h, cel_list.h) |
| 9/14 dispatcher wat_runner SKIP (wasmtime tail-call→host panic) | UNVERIFIED — validation item 3 | |
| 16–18 arith/compare/concat | STILL-TRUE | uniform `_at_vv` slot-out shipped; 16's manual 32-bit-halves mul claim is pre-wasi-sdk — validation item 4 |
| 21–22 size/in kArena | STILL-TRUE | `cel_list_size_arena` etc. exported (wasm_exports.txt:73-79) |
| 30–33 logical/ternary | STILL-TRUE | `cel_copy_slot` lives in runtime/cel_3vl.{h,c} |
| 40–41 proto make_message/set_field | STILL-TRUE shape; 41's "repeated/map → non-OK Status (trap)" partially superseded by M20 poison contract for value-range errors | wat-traces M20.1; expr_lower.cc:535-660 |
| 50–55 duration/timestamp (m7b) | STILL-TRUE — kernels shipped | runtime/cel_time.{h,c}; `cel_timestamp_tz_accessor` bound host-side (cel_host_wasmtime.cc) |
| 56 wrapper tail-unwrap (m8) | STILL-TRUE | `cel_wkt_unwrap_wrapper` in cel_host_wasmtime.cc; `MaybeEmitWktUnwrapTailCall` expr_lower.cc:589 |
| 60–67 comprehensions (M5) | STILL-TRUE — the authoritative comprehension shapes | `cel_list_append_at` (runtime/cel_list.h), `cel_map_iter_init/next/key_at` (runtime/cel_map.h, cel_runtime.c, host arms in eval/internal/cel_host.cc).  62/63/64's "Depends on Slice D/E … tagged manual" status lines are STALE — kernels shipped |
| M13 P1 probe pair | DRIFTED | wats remain (wat/m13_p1_*.wat) but `compiler/probes/` is EMPTY — `bazel test //compiler/probes/m13_custom_fns:m13_p1_test` no longer exists (probes deleted at closeout); the "host-allocated cel.memory" model described is pre-Phase-C |
| M16 math, M17 encoders, M18 net | STILL-TRUE ABI; "runs once kernel lands" status lines STALE | all kernels shipped: runtime/cel_math_ext.c, cel_base64_ext.cc, cel_net_ext.c; `CEL_IP=18`/`CEL_CIDR=19` (runtime/cel_data.h:50-51) |
| M14 optionals batch | STILL-TRUE ABI; M14.1's tail is internally inconsistent | batch preamble (wat-traces:1850-1861) says `RegisterPendingM14Imports` was deleted and kernels are production exports, but M14.1 still ends "Instantiates today, doesn't compute today — RegisterPendingM14Imports … binds a no-op" (wat-traces:1942-1948).  Kernels verified shipped (runtime/cel_optional.{h,c}) |
| M14.4 short-circuit `or`/`orValue` requirement | STILL-TRUE as a stated requirement — validation item 5 (was the impure-RHS branch emitted?) | |
| M20.1 set_field poison | STILL-TRUE — the live contract | |
| "Future entries (stubs)" list | SUPERSEDED — all but one shipped | `has()`, nested select, partial-eval unknown, `x+y`, `in`/`size` all live; note `map(x, x*2)` shipped via `cel_list_append_at` (wat 62), NOT the stub's "pre-sized accumulator + cel_list_set" |

### 1.3 Cross-cutting verified facts the new docs must carry

- **Link modes are the top-level architectural fork.**  kStatic (default): one
  self-contained Program.wasm = stripped runtime + expr code, own memory, only
  `cel_host.*`/`cel_env.*`/`cel_fn.*` imports; kDynamic: expr imports
  `cel.memory` + `cel.*` helpers from a separately-instantiated runtime
  (compile.cc:616-668; engine.cc:398-435, 508-523).  Every memory-topology
  statement in both docs is mode-conditional now.
- **Runtime catalogue generation**: `// cel:codegen-export` markers →
  `//bazel:gen_runtime_catalogue` → `abi::CelRuntimeHelpers()`; consumed by both
  `InstallOverloadImportsExport` and `BindAllRuntimeExports` (engine.cc:243-247);
  wasm_exports.txt = host-only symbols (wasm_exports.txt:1-40).
- **Layout constants** single-sourced in runtime/cel_layout.h (pages=2,
  reserved=8192, arena cap=64 KiB, _Static_asserts at :46-54).
- **NodeAnnotation is the single per-node fact table** — now 12 fields; the
  "no parallel tables" goal held even as the schema grew.

## 2. Doc-vs-code discrepancies

1. **P1 — design.md §4.6 describes a customs API that never existed.**
   Claim: `Compiler::Builder::RegisterFunction(FunctionDecl)` +
   `RuntimeBindings::AddFunction(overload_id, impl)` + `Plan(program, bindings)`
   (design.md:921-1122, 2558-2571).  Code: `.celfn`-string registration
   (compiler.h:266; compile.h:81-90) and engine-level
   `AddFunction`/`BindFunction` (engine.h:117, 177-244).  Dangerous for a new-doc
   author transcribing the API surface.
2. **P1 — design.md §12 Q1 recommends against the architecture that is now the
   default.**  "Inline runtime into every expr module … Recommend: not in this
   rewrite" (design.md:3821-3825) vs `LinkMode::kStatic` default + `CompileStatic`
   merge (compiler.h:145; compile.cc:616-653).
3. **P1 — design.md §10.2.3 invariant "No new NodeAnnotation fields" for M5
   broke silently.**  `comp_aux_local_base` + iter/accu binding fields landed
   (annotations.h:106-120); the invariant text (design.md:2816-2824) was never
   annotated with a relaxation callout the way §10.1.3's was.
4. **P1 — design.md §8.1 names wasm_exports.txt the single source of truth for
   exports** (design.md:2332-2341); code moved truth to `// cel:codegen-export`
   markers + generated catalogue (wasm_exports.txt:3-7; bazel/gen_runtime_catalogue.py).
5. **P1 — wat-traces M14.1 contradicts its own batch preamble**: preamble says
   `RegisterPendingM14Imports` deleted / kernels production (wat-traces:1850-1861);
   M14.1 tail still says the kernel "doesn't exist … no-op trampoline"
   (wat-traces:1942-1948).  Kernels verified in runtime/cel_optional.c.
6. **P1 — wat-traces first "§6" names `cel_list_index_at_vv`**, a helper that
   never shipped (zero grep hits); real names `cel_list_at{,_arena}`.  Also two
   sections share the number 6.
7. **P2 — stale counts**: kBuiltinSeeds 80 vs 271 (design.md:628 vs
   overload_table.cc:96); kExplicitlyUnimplementedIds 86 vs 6 (design.md:898 vs
   overload_table.cc:750); RegisterCelHostImports "four imports today"
   (design.md:1702-1704) vs the much larger shipped set.
8. **P2 — design.md §13 claims Sethi–Ullman slot pre-assignment shipped**
   (design.md:3888); reuse is release-based free-list, Strahler never landed
   (design.md:7 itself; slot_allocator.h:64-77).
9. **P2 — code-comment drift found while verifying**: compile.h:44-45 says
   mem_size_bytes is "the second arg of the `arena_reset` call" — `arena_reset`
   is zero-arg (cel_arena.c:110; expr_lower.cc:136).
10. **P2 — wat-traces status lines** ("runs once Slice X lands") stale for 62/63/64,
   M16/M17/M18 — all kernels shipped; and the M13 P1 "runnable today" bazel target
   is gone (compiler/probes/ empty).
11. **P2 — overload_table.h header comment** still says "`kBuiltinSeeds` empty —
   M3 fills the seeds" (overload_table.h:11) — two generations stale.

## 3. Validation items

1. **Does `bazel test //e2e/...` exercise BOTH link modes for the same expression
   set?**  Settle: inspect `bazel/link_mode_test.bzl` expansion —
   `bazel query 'attr(name, ".*link_mode.*", //e2e/...)'` and run one
   static+dynamic pair.  (m28 memory notes a "hollow-dual-mode-test gotcha".)
2. **In kStatic mode, is the Program's memory exported as `shared` and does the
   A13/A14 invariant check still run?**  Settle:
   `bazel run //tools/cel -- -e "1+1"` (default static), then
   `wasm-objdump -x` the emitted wasm and read `EnforceRuntimeMemoryInvariants`
   call path in engine.cc for the static branch.
3. **Are wat 09/14 (kDynamic dispatcher → host import tail-call) still SKIPped in
   wat_runner_test for the wasmtime c-api panic?**  Settle:
   `bazel test //tools/wat_runner:wat_runner_test --test_output=all` and grep the
   log for `09_map_index_dynamic` / SKIP.
4. **Does the manual 32-bit-halves overflow multiply (wat-traces §16,
   `uint64_mul_overflows`) still exist post-wasi-sdk, or did
   `__builtin_mul_overflow` become linkable with compiler-rt?**  Settle:
   `grep -n "uint64_mul_overflows\|__builtin_mul_overflow" runtime/cel_arith.c`.
5. **Did Slice B's impure-RHS short-circuit for `or`/`orValue` (wat-traces M14.4
   requirement) actually land in codegen?**  Settle:
   `grep -n "orValue\|optional_or" compiler/codegen/expr_lower.cc` + an oracle/e2e
   case `optional.of(1).orValue(1/0)` asserting no spurious error AND a
   partial-eval case with unknown RHS.
6. **Is `FieldRefEntry` still descriptor-uncached (design.md:1608-1615 claims
   re-resolution per call so one Program serves multiple descriptor pools)?**
   Settle: read `eval/internal/cel_host.h::FieldRefEntry` + the multi-pool test
   named in design.md §4.7.6.7.
7. **Does `LoweringOptions::mem_size_bytes` have any effect in kStatic mode**
   (the adopted runtime module already fixes memory)?  Settle: compile the same
   expr static with two mem_size values, diff the wasm.

## 4. Test coverage observations

- The WAT corpus is a real regression net: 59 `.wat` files under
  `rewrite/wat/` are assembled + executed by `wat_runner_test` per build
  (wat/BUILD.bazel), and the M14 set is asserted **byte-exact**
  (wat-traces:1839-1848).  This is the only mechanism that caught the inline-md
  vs on-disk drift — the .md listings have no test, the .wat files do.
  Consequence for new docs: never inline WAT bodies in prose; cite the file.
- `runtime_catalogue_consistency_test` pins exports↔catalogue↔imports coherence
  (wasm_exports.txt:14-16) — replaced the hand-audit design.md §11.7 rows asked for.
- The overload coverage tripwire (overload_table.cc:736-765) is alive and is
  what forced kExplicitlyUnimplementedIds down from 86 to 6 — good example of a
  test that *drives* milestone scope.
- Gaps: nothing tests the historical docs' claims themselves (expected — they're
  archives); the §8.5 error-provenance and Sethi–Ullman test rows in design.md
  §11.6/§11.7 were never created because the features were dropped — those
  checklist rows should be deleted, not left unticked.
- Link-mode duality is the newest under-pinned surface (validation item 1).

## 5. Design decisions worth preserving (restate in the new docs)

1. **Uniform slot-out ABI** `(i32 out_slot, i32 args…) -> void` for every
   overload, built-in or custom; control flow (`?:`) and 3VL `&&`/`||` are the
   only sanctioned exceptions (design.md §4.2; wat 30-33).  Aliasing rule: every
   helper reads all inputs before writing out — load-bearing for slot reuse.
2. **One per-node fact table** (`NodeAnnotation`), zero-sentinel for
   not-applicable fields, ResolvePass/LayoutPass as the single writers — survived
   28 milestones of schema growth without a parallel table appearing.
3. **Three-path origin dispatch** (kArena/kHost/kDynamic) with musttail
   dispatchers — the trade (one runtime branch vs table bloat vs codegen
   special-casing) is re-derivable only from design.md §4.3.2's Option-B callout;
   keep the rationale.
4. **Per-function wasm imports for customs, no shared trampoline** — the
   symmetry argument (design.md §4.6.1) survived even though the registration
   API around it was rewritten twice.
5. **Empty-then-populate aggregate construction** (create + per-entry
   insert/set/set_field), poison-on-error riding the out_slot (M20) — keeps every
   aggregate call site shaped like every scalar call site.
6. **Literals unconditionally in rodata; no cap; infallible builder API**
   (design.md §12 Q4) — deliberate non-feature.
7. **Reset is codegen-emitted, not host-driven**; `arena_init` once per Instance
   is the only host arena call — the isolation contract per Plan.
8. **Rejected alternatives worth keeping on record**: tag-encoded optional
   SOME/NONE kinds (wat-traces M14.1 — polymorphic-switch sprawl argument);
   `MessagePattern` table (design.md §4.7.1); wasm-globals arena cursor (§12 Q2,
   twice superseded but the "no wasm-ld moving part" reasoning recurs);
   args-staging shared customs trampoline (§4.6.1); per-macro pre-sized map()
   accumulator (wat-traces §62).
9. **The link-mode fork resolution**: §12 Q1's "import-based shape, keep
   toolchain deps stable" reasoning lost to m28's static default — the new doc
   must state *both* the original reasoning and why it flipped
   (m28-configurable-linking.md), since the dynamic mode still exists and the
   old argument still governs it.
