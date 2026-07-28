# 05 — The custom-function subsystem (`.celfn`)

Status: current — authored 2026-06-10 from the design-rebuild notes;
updated 2026-07-25 for the self-describing plugin surface (`Plugin`,
`cel.fns`, `Use`, Plan-time required-function verification — m35). The
@native backend fork (§6) is an open owner decision (V5). Supersedes
doc/implementation-plan/rewrite/{m13-custom-fns, m22-foreign-fn,
m24-foreign-fn-component-backend, m26-celfnc-and-component-build,
modules-and-fnis}.md.

A `.celfn` declaration is parsed in `compiler/celfn/`, stamped by the cel-cpp
checker, lowered by codegen as a wasm import, bound by the evaluator at Plan,
and — for the plugin backend — scaffolded by `cel generate`, built by a
Bazel macro, and shipped as a self-describing `Plugin` artifact (§5.0).
Evaluator-side dispatch internals live in 02-evaluator.md §3–§4
and §9; cited, not repeated.

## 1. The decl model & three backends

One struct describes every custom function: `CelfnDecl`
(`compiler/celfn/function_library.h`) — name, return type (a
`shared/CelType`, the one C++ type vocabulary since the 2026-07-25
unification, cleanup-backlog #53), params (each `{is_this, type,
name}`), synthesized `overload_id`, `num_args`, and a `Backend`
discriminator:

| Backend | Prefix | Body | Bound by |
|---|---|---|---|
| `kHost` | `@host.` | none — C++ callable | `BindFunction` / `AddTypedFunction` / `AddFunction` (`eval/engine.h:137-235`); §4 |
| `kPlugin` | `@plugin.` | none — sandboxed plugin | `Engine::Use(plugin)` (the one-noun flow, §5.0); `Engine::AddPlugin(bytes, lib)` stays as the explicit-decls escape hatch |
| `kCelDefined` | `@native.` | CEL expression after `=` (`Celfn.g4:66-68`) | **declaration-only today** — no body compiler exists; §6 |

Backend choice changes exactly two things: the wasm import-module string
codegen emits, and who binds that import at Plan. Everything else — the 24-byte
CelValue slot ABI, the `(out_slot, arg_slots...)` calling convention, checker
registration, the OverloadTable row — is identical.

The load-bearing dispatch invariant: **kHost and kPlugin share one
import namespace.** `DispatchesViaCelFn` (`compiler/internal/compile.cc:527`)
maps both to import module `"cel_fn"` (function_library.cc:224,242; pinned by
function_library_test.cc:37,58,184). A plugin function *is* a host function
at the call site — only the Engine knows whether the `cel_fn.<overload_id>`
linker func it binds at Plan wraps an embedder lambda or a plugin-export
trampoline (02-evaluator.md §9). kCelDefined alone routes to
`ImportModule::kUserModule`, import-module string = the library's `Module foo;`
directive (compile.cc `BuildOverloadTable`; `overload_table.h:124`).

Rejected alternative: a separate dispatch table for customs. Extending the one
OverloadTable lets customs inherit every codegen invariant (import dedup,
link-mode skip-if-defined) for free (compile.cc `InstallOverloadImportsExport`).

## 2. The overload-id identity chain

Overload ids are **synthesized, never user-chosen**:
`overload_id = <fn_name>_<argkind>...` (`SynthesiseOverloadId`,
function_library.cc:180-188). `Argkind()` (cc:30-69) renders each param type as
a slug — `int`, `list_int`, `map_string_int`, `message_acme_User` (dots →
underscores, **case preserved**), `optional_int`, `type`.

That one string is the subsystem's identity at five stations, which must stay
equal:

| # | Station | Where |
|---|---|---|
| 1 | Checker stamp — `overload.set_id(decl.overload_id)` on every resolved call node | `RegisterCustomFunctionsOnChecker`, `compiler/frontend/parse_and_check.cc:798-823` |
| 2 | OverloadTable key — `RegisterCustom(..., helper_name = overload_id, num_args)` | compile.cc `BuildOverloadTable` |
| 3 | Wasm import field name — `(import "cel_fn"\|"<module>" "<overload_id>" (param i32 × num_args))`, all-void returns, out_slot first | `InstallOverloadImportsExport`, compile.cc:286-353 |
| 4 | Engine callback key — bound as the `cel_fn.<overload_id>` linker func at Plan; `BindFunction` re-derives the id from the same `.celfn` string, so binding cannot diverge from import name | 02-evaluator.md §3 |
| 5 | Plugin export name, kebab'd — the Component-Model grammar rejects snake_case; the Engine resolves exports by `OverloadIdToKebab` (underscores → hyphens, uppercase → lowercase), the WIT emitter matches via `SnakeToKebab` | engine.cc:1131; `compiler/celfn/celfnc_emit/wit_emitter.cc:151` |

The snake↔kebab translation lives in exactly those two places and must stay in
lockstep; both flatten proto-fqn CamelCase segments (pinned:
wit_emitter_test.cc:128-131). The kebab transform is lossy (lowercasing) —
station 5 is derived, never reversed; the canonical id is the snake-case
original.

Uniqueness: cross-library at `Compiler::Builder::Build` (compiler.cc:155-166);
per-library at `FunctionLibrary::Builder::Build` (§3); engine-side,
`AddFunction`/`Use`/`AddPlugin` conflict-check ids against everything
previously registered (02-evaluator.md §3).

## 3. The IDL & the Builder validation funnel

### 3.1 Grammar

`compiler/celfn/Celfn.g4` (single file). Productions: `moduleDirective`
(`Module foo;`), `hostFnDecl` (`type '@' 'host' '.' Id '(' params? ')' ';'`),
`pluginFnDecl` (same with `plugin`), `nativeFnDecl` (adds `'='
celExprBody`), and `bareHostDecl` — a **diagnostic-only** production matching
`host.foo(...)` without `@`, converted to a curated InvalidArgument ("reserved
alias; use `@host.`") by the visitor (function_library.cc:535-539;
Celfn.g4:49-59).

Type syntax: `bool|int|uint|double|string|bytes`, `Duration`, `Timestamp`,
`list<T>`, `map<K,V>` (keys grammar-restricted to `bool|int|uint|string`,
Celfn.g4:107-109), `proto(fqn)`, `null`. **No `type` or `optional<T>` syntax** —
those kinds are constructible only programmatically (§7).

`@native` bodies are one raw token:
`CelExprText : { _input->LA(-1) == '=' }? ~[;]+` (Celfn.g4:153-155) — everything
up to the first `;`. Documented v0 limitation: a `;` inside a string literal in
the body breaks the lexing (Celfn.g4:141-152).

### 3.2 One funnel for file and programmatic paths

`ParseCelfnSource` (function_library.cc:498-555) is a thin driver: ANTLR parse
(errors → InvalidArgument with line:col), then a tree walk into the same
`FunctionLibrary::Builder` the programmatic API exposes. The Builder is the
**single validation funnel** — grammar-bypassing embedders and the celfnc
generator hit identical gates, with the offending decl named.

`Build()` gates, in order, first failure wins (function_library.cc:322-358):

1. Any kCelDefined decl ⇒ `module_name_` required (cc:331-335). One-directional:
   a module name with zero kCelDefined decls is accepted (the header's "iff" at
   h:199 overstates).
2. `this` only on the first param (cc:190-200, 481-484); `is_receiver` is true
   iff the first param carries `this`.
3. Per-library overload-id uniqueness (cc:342-346).
4. Universal map-key gate: `FirstIllegalMapKey` recurses through
   list/map/optional carriers; an illegal key kind *anywhere* ⇒ InvalidArgument
   naming decl + param (cc:123-144, 270-285). All backends (pinned for kHost:
   function_library_test.cc:563-576).
5. kPlugin only: `MentionsOptional` / `MentionsType` structurally
   reject `optional<T>` and `type` anywhere in the signature (cc:85-113,
   287-318). See §7.

### 3.3 The arity-cap hole

There is **no params-count cap at any layer**; each seam fails differently:

- Library: `num_args = uint8_t(params.size()) + 1` (+1 = out_slot,
  function_library.cc:205) — 255 params silently wraps `num_args` to 0.
- Codegen: `InstallOverloadImport` installs imports only for `num_args` ∈
  [1, 5]; anything else hits `default: return false` — a *silent skip*, so a
  5-value-arg decl emits a module whose call target was never imported
  (compile.cc:286-317).
- Engine: `AddFunction` rejects only arity 0 (02-evaluator.md §3).

The long-claimed 254/255 cap test does not exist.

> **Open question (V9):** the cap must live at **one** layer — the Builder —
> with downstream layers CHECKing rather than silently skipping. Probe the
> three failure shapes above first, then extend `Build()` with the cap and the
> all-backend type gate (§7) in one change.

### 3.4 Library → compile flow

`Compiler::Builder::DeclareFunctions` (renamed from `AddLibrary`) /
`AddFunction(celfn_source)` / `Use(plugin)` (≡
`DeclareFunctions(plugin.library())`, §5.0) accumulate
libraries (compiler.cc:114-133); `Compile()` forwards them to *both* the
checker (`CheckOptions.function_libraries`, §2 station 1) and codegen
(`CompileOptions.function_libraries`, stations 2-3) (compiler.cc:191-192).
`DeclTypeToCheckerType` (parse_and_check.cc) maps decl `CelType`s to
`cel::Type`; kType/kOptional hit an `ABSL_CHECK` stub (§7) behind the
`ValidateDeclTypesMappable` gate in compiler.cc.

## 4. `@host` end-to-end

The shipped, recommended path is **declaration-first**: one `.celfn` string is
the single source of truth, used verbatim on both sides
(`examples/04_host_functions.cc:33-58`):

```cpp
constexpr absl::string_view kDecl =
    "int @host.discount_pct(string tier);";
builder.AddFunction(kDecl);                      // compile side
engine.BindFunction(kDecl, [](absl::string_view tier)
                        -> absl::StatusOr<int64_t> { ... });
```

Compile side: the decl makes `discount_pct` type-check like a builtin (§2
station 1) and lowers to `(call $cel_fn.discount_pct_string (out_slot,
arg_slot))`. Eval side: `BindFunction` re-parses the same string
(`ParseSingleHostDecl`, engine.cc:1317; exactly one `@host.` decl required),
validates the lambda's `param_kinds` positionally against the declared CEL
types (`CppParamMatchesDeclType`, engine.cc:1281 — `Value` matches anything,
`string_view` serves string|bytes, `null`/`type`/`optional` only via `Value`),
and registers under the synthesized id. A signature mismatch is rejected at
registration, not at eval. Pinned by `EngineBindFunctionTest` (engine_test.cc)
and `e2e/host_fn_test.cc` (`BindFunctionDeclFirstRoundTrip`).

The lower-level surfaces (`AddFunction`, `AddTypedFunction`) and the L0/L1/L2
dispatch stack are the evaluator's story: **02-evaluator.md §3 and §4**. This
subsystem's contract: the callback is keyed by overload-id, receives
`(out_slot, args...)` CelValue slots, and never sees Error/Unknown args
(absorbed at L0).

## 5. `@plugin` end-to-end

The plugin backend runs an embedder-authored function inside a sandboxed
plugin — packaged as a Component-Model component; eval-side marshaling is
02-evaluator.md §9.

### 5.0 The self-describing artifact & the one-noun flow

The public noun is **`Plugin`** (`abi/plugin.{h,cc}`, `//abi:plugin` in
the curated public set). A plugin artifact carries its own `.celfn`
declaration text verbatim in a top-level **`cel.fns` custom section**
(appended by the macro's `cel embed-decls` step, §5.2), and
`Plugin::Load(bytes)` is the *only* constructor — it parses and
validates the section (CM preamble, framing, UTF-8, decl parse, all
`@plugin.`, ≥1 decl), so every `Plugin` is self-describing by
construction. The declaration triplication the old flow required (the
`.idl`, a hand-written `FunctionLibrary` mirror, and the
`SetWitInterface` magic string) is gone; the WIT interface name is
always derivable as `cel:<module>/fns@0.1.0` from the text's `Module`
directive (`DeriveWitInterface`, function_library.h), so macro and
`Load` cannot drift.

`Plugin` sits in `abi/` because both sides take `const Plugin&`:

- **Compile side** — `Compiler::Builder::Use(plugin)` ≡
  `DeclareFunctions(plugin.library())` (compiler.cc). Call sites
  type-check against the artifact's decls, and `Compile` records every
  `cel_fn` import the final wasm carries — name, backend, full
  recursive signature — as `required_functions` rows in the Program's
  `cel.abi` (08-abi-wire-format.md §1.1 field 8).
- **Eval side** — `Engine::Use(plugin)` wraps the AddPlugin internals
  and adds a **static export check** against the *parsed* component
  (`CheckPluginExportsStatically`, engine.cc: interface + every decl's
  kebab export via `wasmtime_component_get_export_index`, no store, no
  instantiation) plus retention of the plugin's content hash
  (SHA-256(bytes ‖ decl text), `abi/internal/sha256`) for Plan-time
  diagnostics.
- **Plan** — verifies every `required_functions` row against the
  registry (existence + exact signature; protos by FQN; `@host` rows
  included, so a forgotten `BindFunction` fails cleanly too), then
  instantiates only the plugins owning at least one required PLUGIN
  row. Legacy Programs with no `required_functions` table keep the
  pre-verification behavior (no check, instantiate-all).

Byte-framing primitives live in `//abi:wasm_binary`
(`abi/wasm_binary.{h,cc}`) — the ONLY first-party code allowed to know
wasm binary framing (preamble classification, LEB128, top-level
custom-section find/append); `cel embed-decls`
(`tools/cel/run_embed_decls.cc`) is the standalone writer for
artifacts built outside the macro. `Engine::AddPlugin(bytes, lib)`
remains the explicit-decls escape hatch (pure-WAT fixtures,
pre-`cel.fns` artifacts): no static export check, export resolution
Plan-time only (pinned by `e2e/plugin_dispatch_test.cc`
`MissingExportFailsAtPlanNotAddPlugin`).

### 5.1 The celfnc emitters & `cel generate`

Four pure text emitters under `compiler/celfn/celfnc_emit/` (string in/out, no
I/O), keyed off kPlugin decls only:

| Emitter | Emits |
|---|---|
| `EmitWit` (wit_emitter.cc) | The WIT interface: `package <pkg>[@ver]; interface fns { ... } world customfn { export fns; }`. Type mapping: int→s64, uint→u64, double→f64, null→`option<u8>` (wit-bindgen's C generator rejects `option<unit>` — empirically probed), bytes & proto→`list<u8>` (proto fqn stays host-side, never in WIT), Duration/Timestamp → in-interface `record { seconds: s64, nanos: s32 }`, map<K,V>→`list<tuple<K,V>>`. Names via `SnakeToKebab` (§2 station 5). kType/kOptional reaching the emitter is a FailedPrecondition tripwire (cc:110-116). |
| `EmitCodecH` (cpp_codec_emitter.cc) | Lift/lower between wit-bindgen's `customfn_*_t` C structs and `std::` containers. `TypeCollector` dedups + topo-orders, emitting only used types. Strings lift to `std::string_view` (zero-copy), lower via `customfn_string_dup_n`; bytes/lists/protos lower via `cabi_realloc`; protos are `lift_proto<M>`/`lower_proto<M>` templates (ParseFromArray / SerializeToString) — no descriptors needed. **Map lower is not emitted** — §5.4. |
| `EmitStubCc` (cpp_stub_emitter.cc) | One `extern "C"` export body per decl, symbol `exports_<pkg-normalized>_fns_<overload_id>` (cc:224); scalar returns by value, everything else fills `<struct>* ret` via `codec::lower(ret, user::CamelFn(codec::lift(*arg)...))`. `SnakeToCamel` makes the author-facing name. |
| `EmitUserFnsH` (cpp_skeleton_emitter.cc) | The author skeleton: `std::string_view`, `const std::vector<T>&`, `const std::map<K,V>&`, `const acme::User&` params; returns by value. |

CLI: `cel generate --idl <file> --out_dir <dir> [--language=cpp]
[--include=...]` (`tools/cel/run_generate.{h,cc}`). The WIT package
name is **not configurable**: always `cel:<module>` (fallback
`cel:customfn`), version `0.1.0`, C++ namespace = the IDL module name —
the former `--package`/`--package_version` override flags were deleted
(zero callers; the derivation is shared with `Plugin::Load` via
`DeriveWitInterface`, so the two can never disagree).
`--language=go` is explicitly rejected (run_generate.cc; §8).

Emitter regressions fail the build: a `codec_dumper` genrule emits a codec for
a fixed library and a `cc_library` compile-checks it against the hand-curated
`fixtures/customfn.h` (celfnc_emit/BUILD.bazel:118-163). The dumper exercises
map only as an *arg* — matching the missing map-lower (§5.4).

### 5.2 The build macro

`bazel/cel_wasm_plugin.bzl`: genrule(`cel generate`) →
genrule(`wit-bindgen c --world customfn`) → wasi-sdk `cc_binary` under the
**wasm32-wasip2** platform transition → genrule(`cel embed-decls`), which
stamps the verbatim `.idl` text into the binary as the top-level `cel.fns`
custom section and emits the final `<name>.wasm`. wasip2 emits a
Component-Model binary *directly* (preamble 0x1000d) — no `wasm-tools
component new` step; the section is appended by the tool because a
linker-emitted custom section would land inside the *nested core
module*, invisible to a top-level walker. The wasip2 and wasi-threads
toolchains coexist, selected by target platform constraint.

### 5.3 Naming translations & ownership rules

Three name systems, all derived from the snake_case overload-id (§2): the WIT
export is kebab + lowercase (`SnakeToKebab`), the C export symbol embeds the
overload-id verbatim behind the `exports_<pkg>_fns_` prefix
(cpp_stub_emitter.cc:224), and the author implements a CamelCase function
(`SnakeToCamel`).

> **Open question (V27):** the stub's export symbol preserves overload-id case
> (`..._is_adult_message_acme_User`); wit-bindgen derives its expected impl
> symbol from the lowercased kebab WIT name. Whether the manual-tagged
> `demo_plugin_proto` fixture actually links is unprobed — build it; if it
> links, document why; if not, this is a real bug hidden by the manual tag.

Return ownership: the author/stub populate `*ret` via `customfn_string_dup_n` /
`cabi_realloc`; the generated `cabi_post_*` frees; **the author never calls
`_free` on returns.** Probed against wit-bindgen 0.57.

### 5.4 Known traps

- **String-return trap.** A string-returning plugin fn traps at call time
  ("cannot leave component instance", inside libc++ post-RNG-init under
  wasm32-wasip2) — reasoned skip in `e2e/.../demo_plugin_e2e_test.cc:135-147`.
  The supported envelope today is **scalar returns**
  (`examples/09_plugin_functions.cc` is scalar-only on purpose,
  examples/README.md:24-26).
- **Map-lower emitter gap.** `EmitCodecH` emits a literal
  `// TODO(m26): lower($1*, const $0&) not yet emitted`
  (cpp_codec_emitter.cc:371) — a map-*returning* `@plugin.` decl generates a
  stub calling a nonexistent `codec::lower`. The fixture declaring exactly such
  decls (`fixtures/full_matrix.idl`) is referenced by no BUILD target, so no
  gate catches it.
- **Example 09's visibility hole — closed by the `Plugin` surface.**
  The example's former `FunctionLibrary` mirror needed the
  `//:internal` `//compiler/celfn:function_library` target; the
  rewritten `examples/09_plugin_functions.cc` depends only on the
  public `//abi:plugin` (V33 resolved by construction).
- **Export existence is checked statically at `Engine::Use`;
  FuncType is not.** `Use` resolves the interface + every decl's
  kebab export against the parsed component at registration (§5.0).
  The legacy `AddPlugin` path stays Plan-time-only. On **no** path is
  the export's WIT-level FuncType compared to the decl — a
  wrong-shaped export on a hand-built plugin still traps at call time
  (unreachable for macro-built plugins, where WIT and decls derive
  from one `.idl`; R36 resolved, probe V21 in 02-evaluator.md §3).

> **Open question (V28):** confirm the map-return failure shape, then either
> emit map-lower or reject map returns at `Build()`; the durable fix is wiring
> `full_matrix.idl` into the §5.1 compile gate (§8).

> V33 (example-09 visibility) is resolved: the example now consumes the
> public `//abi:plugin` and builds no `FunctionLibrary` mirror. The
> `AddPlugin` escape hatch still takes `FunctionLibrary` as a
> public-API parameter while the target stays `//:internal` — live for
> external `AddPlugin` callers only, tracked in cleanup-backlog #32.

## 6. `@native` (kCelDefined): the fork

**Status today: declaration-only.** Grammar (§3.1), Builder gates, checker
registration, and codegen's `kUserModule` import emission all exist and are
tested. Nothing produces the wasm module those imports resolve against: a
compiled `@native` call emits `(import "<module_name>" "<overload_id>")`, and
nothing binds it: `Engine::AddModule(alias, bytes)` — the registration surface
reserved for this backend — was deleted 2026-07-27 (never exercised end-to-end;
the m38 dead-code audit removed it with its Plan-side instantiation). Expected
behavior: an unresolved-import failure at Plan (R2/R3). Whichever
implementation below is chosen would reintroduce a binding surface designed
with it.

Two competing implementations exist, **neither live on this branch**:

- **Single-module inlining** (`master-local` branch, commit 0386851e, never
  merged): each body lowered as an internal function of the expression module —
  disjoint rodata bands, recursion rejected, no import. One fragment landed:
  `LowerToCustomFn` (`compiler/codegen/expr_lower.h:215-249`, expr_lower.cc) —
  params `(out_slot, arg0...)`, no `arena_reset`, result via `cel_copy_slot` —
  with **zero callers and zero tests** on this branch.
- **Bundled library module**: `compiler/celfn/library_module.h:45` declares
  `CompileLibraryBodies(lib, parent_opts)` — bundle every kCelDefined body into
  one wasm module, resolved at Plan via `Engine::AddModule`. No `.cc` ever
  existed, no BUILD target, no includer; `compiler/internal/compile.h:107-127`
  defines `CompiledArtifact.library_modules` that nothing populates, and
  `Compiler::Compile` discards everything but `wasm_bytes` (compiler.cc:195).

Master has the *header* of one design, the *docs* of the other, and the working
code of neither. The three options:

| Option | Pro | Con |
|---|---|---|
| 1. Port master-local's inlining | existed end-to-end once; no new ABI surface — direct internal calls, cheapest dispatch | predates branch reorg + configurable linking; a re-land; rodata-band layout must be re-validated against current memory gates |
| 2. Build the library-module producer | contract already written; composes with `AddModule` (tested Engine surface); bodies shareable across programs | never ran anywhere; second module per Plan + cross-module call per invocation; `library_modules` plumbing built end-to-end incl. `Program` carrying multiple byte vectors |
| 3. Reject at Compile until one ships | immediate, named InvalidArgument instead of today's silent late failure; zero design risk; trivially removable | pure stopgap |

**Recommendation: option 3 now** — a Compile-time rejection naming the decl —
keeping the declaration layer and its tests intact while the owner decides
between 1 and 2. A reachable not-done path fails loudly at the edge.

> **Open question (V5):** which implementation ships? First step either way:
> the probe — `SetModuleName("foo").AddCelDefined("is_num", bool, {string},
> "s == '1'")` → Compile `is_num('1')` → Plan — pinning today's failure shape.
> Whichever branch loses, its artifacts go: option 1 ⇒ delete
> `library_module.h` + the `library_modules` plumbing in `compile.h` and the
> stale citations (`layout_pass.h:57`, `expr_lower_internal.h:51`); option 2 ⇒
> delete or wire `LowerToCustomFn`.

## 7. Type-surface policy

- **`optional<T>` and `type` are permanently rejected on the plugin
  surface** — owner direction, not a temporary gap. Enforced at `Build()` (§3.2
  gate 5, incl. depth-4 nesting: function_library_test.cc:261-357), with
  FailedPrecondition tripwires in every emitter (§5.1) and rejection in both
  marshaling directions (`eval/internal/cel_plugin.cc`).
- **CEL `null` is a distinct kind, supported on all backends** (kNull ≠
  kOptional; pinned function_library_test.cc:359-380). Plugin wire form:
  `option<u8>`.
- **Map keys are bool|int|uint|string, every backend, any nesting depth** (§3.2
  gate 4) — matching the kernel's map-key contract.
- **The gate asymmetry is a live crash bug**: gate 5 is foreign-only, so a
  programmatic `AddHost("f", CelType::Type(), ...)` passes `Build()` and
  crashes at the `ABSL_CHECK` stub in `DeclTypeToCheckerType`
  (parse_and_check.cc) — embedder input must never crash the process. The fix
  rides V9 (§3.3).
- **Protos cross every boundary as serialized bytes** — `list<u8>` over the
  plugin boundary, `SerializePartialToString` / ParseFromArray at the codec
  layer — with the fqn kept host-side; neither the WIT nor the generator ever
  needs descriptors.

## 8. Future work

- The V5 decision + cleanup of the losing branch's artifacts (§6).
- The arity cap at the Builder (V9, §3.3) + the all-backend kType/kOptional
  gate (§7), one `Build()` change with the three failure-shape probes first.
- The full-matrix compile gate: wire `fixtures/full_matrix.idl` through the
  §5.1 dumper pipeline — catches the map-lower gap (V28).
- The map-lower emitter (V28, §5.4).
- The Go bindgen path: `--language=go` is rejected today; the tinygo toolchain
  was never registered.
- `FunctionLibrary` visibility for external `AddPlugin` callers
  (cleanup-backlog #32; example 09 itself now rides `//abi:plugin`, §5.4).
- The string-return trap (§5.4) — root-cause the wasip2 libc++ interaction.
- Manual-tag audit: `function_library_test` and `celfn_parser_probe_test` are
  `manual`-tagged (compiler/celfn/BUILD.bazel); record why or untag.
