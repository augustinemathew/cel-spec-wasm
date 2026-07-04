# 05 — The custom-function subsystem (`.celfn`)

Status: current — authored 2026-06-10 from the design-rebuild notes.
The @native backend fork (§6) is an open owner decision (V5).
Supersedes: doc/implementation-plan/rewrite/m13-custom-fns.md,
m22-foreign-fn.md, m24-foreign-fn-component-backend.md,
m26-celfnc-and-component-build.md, modules-and-fnis.md.

Custom functions are the one subsystem that crosses every role
boundary: a `.celfn` declaration is parsed in `compiler/celfn/`,
stamped by the cel-cpp checker, lowered by codegen as a wasm import,
bound by the evaluator at Plan, and — for the component backend —
scaffolded by `cel generate` and built by a Bazel macro. This doc is
the end-to-end story; the evaluator-side dispatch internals are in
02-evaluator.md §3–§4 and §9 and are cited, not repeated.

## 1. The decl model & three backends

One struct describes every custom function: `CelfnDecl`
(`compiler/celfn/function_library.h:96-126`) — name, return
`CelfnType`, params (each `{is_this, type, name}`), synthesized
`overload_id`, `num_args`, and a `Backend` discriminator
(h:97-108):

- **`kHost`** — `@host.` prefix, no body. The implementation is a
  C++ callable bound on the Engine (`BindFunction` /
  `AddTypedFunction` / `AddFunction`, `eval/engine.h:137-235`). §4.
- **`kForeignComponent`** — `@component.` prefix, no body. The
  implementation is a sandboxed Component-Model component registered
  via `Engine::AddComponent(bytes, lib)` (engine.cc:1519). §5.
- **`kCelDefined`** — `@native.` prefix, the only shape with a body:
  a CEL expression after `=` (`Celfn.g4:66-68`). **Declaration-only
  today** — no body compiler exists in the tree; see §6.

Backend choice changes exactly two things: the wasm import-module
string codegen emits, and who binds that import at Plan. Everything
else — the 24-byte CelValue slot ABI, the `(out_slot, arg_slots...)`
calling convention, the checker registration, the OverloadTable row —
is identical across backends.

The load-bearing dispatch invariant: **kHost and kForeignComponent
share one import namespace.** `DispatchesViaCelFn`
(`compiler/internal/compile.cc:527`) maps both to
`ImportModule::kCelFn`, i.e. import module `"cel_fn"`
(function_library.cc:224,242; pinned by
function_library_test.cc:37,58,184). A component function *is* a host
function at the call site — checker, OverloadTable, and codegen
cannot tell them apart; only the Engine knows whether the
`cel_fn.<overload_id>` linker func it binds at Plan wraps an embedder
lambda or a component-export trampoline (02-evaluator.md §9).
kCelDefined alone routes to `ImportModule::kUserModule`, with the
library's `Module foo;` directive as the import-module string
(compile.cc, `BuildOverloadTable`; `overload_table.h:124`).

A rejected alternative worth preserving: a separate parallel dispatch
table for customs. Rejected in favour of extending the one
OverloadTable — builtins and customs flow through a single
overload-id → helper mapping, so customs inherit every codegen
invariant (import dedup, link-mode skip-if-defined) for free
(compile.cc `InstallOverloadImportsExport`).

<!-- diagram-wanted: three-backend dispatch map — one CelfnDecl at the
     top; kHost and kForeignComponent funnel into the "cel_fn"
     import namespace (Engine binds: embedder lambda vs component
     trampoline at Plan); kCelDefined routes to "<module_name>"
     (binder: Engine::AddModule — currently nothing produces the
     module; dashed/red per §6) -->

## 2. The overload-id identity chain

Overload ids are **synthesized, never user-chosen**:
`overload_id = <fn_name>_<argkind>...`
(`SynthesiseOverloadId`, function_library.cc:180-188), where
`Argkind()` (cc:30-69) renders each param type as a slug — `int`,
`list_int`, `map_string_int`, `message_acme_User` (dots →
underscores, **case preserved**), `optional_int`, `type`.

That one string is the subsystem's identity at five stations, and
the whole design hangs on the five staying equal:

1. **Checker stamp.** `RegisterCustomFunctionsOnChecker`
   (`compiler/frontend/parse_and_check.cc:798-823`) calls
   `overload.set_id(decl.overload_id)`, so the cel-cpp checker stamps
   *our* synthesized id on every resolved call node.
2. **OverloadTable key.** Codegen looks the stamped id up in the
   table to find the helper (compile.cc `BuildOverloadTable`:
   `RegisterCustom(overload_id, ..., helper_name = overload_id,
   num_args)`).
3. **Wasm import field name.** `InstallOverloadImportsExport` emits
   `(import "cel_fn"|"<module>" "<overload_id>"
   (param i32 × num_args))`, all-void returns, out_slot first
   (compile.cc:286-353).
4. **Engine callback key.** `wasmtime_->host_callbacks[overload_id]`
   is bound as the `cel_fn.<overload_id>` linker func at Plan
   (02-evaluator.md §3); `BindFunction` re-derives the id from the
   same `.celfn` string, so the binding cannot diverge from the
   import name.
5. **Component export name, kebab'd.** The Component-Model identifier
   grammar rejects snake_case, so the Engine resolves exports by
   `OverloadIdToKebab(overload_id)` (engine.cc:1067) — underscores →
   hyphens **and** uppercase → lowercase — and the WIT emitter
   produces the matching name via `SnakeToKebab`
   (`compiler/celfn/celfnc_emit/wit_emitter.cc:151`).

The snake↔kebab translation is mirrored in exactly those two places
and must stay in lockstep; both flatten proto-fqn CamelCase segments
(pinned: wit_emitter_test.cc:128-131). Note the kebab transform is
lossy (lowercasing), which is why station 5 is derived, never
reversed — the canonical id is always the snake-case original.

<!-- diagram-wanted: the identity chain — one overload-id string
     flowing through checker stamp → OverloadTable key → wasm import
     field → engine callback key → (kebab) component export, with
     the two snake↔kebab sites highlighted as the only transforms -->

Cross-library uniqueness is enforced at `Compiler::Builder::Build`
(compiler.cc:155-166); per-library uniqueness at
`FunctionLibrary::Builder::Build` (§3). Engine-side,
`AddFunction`/`AddComponent` conflict-check ids against everything
previously registered (02-evaluator.md §3).

## 3. The IDL & the Builder validation funnel

### 3.1 Grammar

`compiler/celfn/Celfn.g4` (single file). Productions:
`moduleDirective` (`Module foo;`), `hostFnDecl`
(`type '@' 'host' '.' Id '(' params? ')' ';'`), `componentFnDecl`
(same with `component`), `nativeFnDecl` (adds `'=' celExprBody`), and
`bareHostDecl` — a **diagnostic-only** production matching
`host.foo(...)` without `@`, converted to a curated InvalidArgument
("reserved alias; use `@host.`") by the visitor
(function_library.cc:535-539; Celfn.g4:49-59). There is no bare-alias
foreign decl (`rules.allow(...)`) shape — that early design never
shipped and was superseded by `@component.`.

Type syntax: `bool|int|uint|double|string|bytes`, `Duration`,
`Timestamp`, `list<T>`, `map<K,V>` (keys grammar-restricted to
`bool|int|uint|string`, Celfn.g4:107-109), `proto(fqn)`, `null`.
**No `type` or `optional<T>` syntax** — those `CelfnType` kinds are
constructible only programmatically (§7).

`@native` bodies are captured as one raw token:
`CelExprText : { _input->LA(-1) == '=' }? ~[;]+` (Celfn.g4:153-155) —
everything up to the first `;`. Documented v0 limitation: a `;`
inside a string literal in the body breaks the lexing
(Celfn.g4:141-152).

### 3.2 One funnel for file and programmatic paths

`ParseCelfnSource` (function_library.cc:498-555) is a thin driver: an
ANTLR parse with a collecting error listener (errors →
InvalidArgument with line:col), then a tree walk that calls the same
`FunctionLibrary::Builder` the programmatic API exposes. The Builder
is therefore the **single validation funnel** — grammar-bypassing
embedders and the celfnc generator hit identical gates, at
registration time, with the offending decl named.

`Build()` gates, in order, first failure wins
(function_library.cc:322-358):

1. Any kCelDefined decl ⇒ `module_name_` required (cc:331-335).
   Enforced in one direction only: a module name with zero
   kCelDefined decls is accepted (the header's "iff" at h:199
   overstates).
2. `this` only on the first param (cc:190-200; also enforced at
   grammar extraction, cc:481-484). `is_receiver` is true iff the
   first param carries `this`.
3. Per-library overload-id uniqueness (cc:342-346).
4. Universal map-key gate: `FirstIllegalMapKey` recurses through
   list/map/optional carriers; an illegal key kind *anywhere* ⇒
   InvalidArgument naming decl + param (cc:123-144, 270-285).
   Applies to all backends (pinned for kHost:
   function_library_test.cc:563-576).
5. kForeignComponent only: `MentionsOptional` / `MentionsType`
   structurally reject `optional<T>` and `type` anywhere in the
   return or any param (cc:85-113, 287-318). See §7.

### 3.3 The arity-cap hole

There is **no params-count cap at any layer**, and the hole has a
different failure shape at each seam:

- Library: `num_args = uint8_t(params.size()) + 1` (the +1 is the
  out_slot, function_library.cc:205) — 255 params silently wraps
  `num_args` to 0.
- Codegen: `InstallOverloadImport`'s switch installs imports only for
  `num_args` ∈ [1, 5] (i.e. ≤ 4 value-args); anything else hits
  `default: return false` — a *silent skip*, so a 5-value-arg decl
  emits a module whose call target was never imported
  (compile.cc:286-317).
- Engine: `AddFunction` rejects only arity 0 (02-evaluator.md §3).

So an embedder can register a 6-arg `@host` fn engine-side that the
compiler mis-emits, and the documented failure (a long-claimed
254/255 cap test) does not exist.

> **Open question (V9):** the cap must live at **one** layer — the
> Builder, per the funnel principle above — with the downstream
> layers CHECKing against it rather than silently skipping. Three
> probes settle the shapes before the fix: (a) compile a 5-param
> `@host` decl + call and assert the error names the missing helper
> (not a silent miscompile); (b) `Build()` with 255 params and
> observe the `num_args` wrap; (c) the kType-on-kHost crash (§7) —
> then extend `Build()` with the cap and the all-backend type gate
> in one change.

### 3.4 Library → compile flow

`Compiler::Builder::AddLibrary` / `AddFunction(celfn_source)`
accumulate libraries (compiler.cc:114-133); `Compile()` forwards them
to *both* the checker (`CheckOptions.function_libraries` — call-site
resolution, station 1 of §2) and codegen
(`CompileOptions.function_libraries` — OverloadTable registration,
stations 2-3) (compiler.cc:191-192). `CelfnTypeToCelType`
(parse_and_check.cc) maps decl types to `cel::Type` for the checker:
scalars, list, map, proto-by-descriptor-lookup; kType/kOptional hit
an `ABSL_CHECK` stub (§7).

## 4. `@host` end-to-end

The shipped, recommended path is **declaration-first**: one `.celfn`
string is the single source of truth for the signature, used verbatim
on both sides (`examples/04_host_functions.cc:33-58`):

```cpp
constexpr absl::string_view kDecl =
    "int @host.discount_pct(string tier);";
builder.AddFunction(kDecl);                      // compile side
engine.BindFunction(kDecl, [](absl::string_view tier)
                        -> absl::StatusOr<int64_t> { ... });
```

Compile side, the decl makes `discount_pct` type-check like a builtin
(§2 station 1) and lowers to `(call $cel_fn.discount_pct_string
(out_slot, arg_slot))`. Eval side, `BindFunction` re-parses the same
string (`ParseSingleHostDecl`, engine.cc:1317; exactly one `@host.`
decl required), validates the lambda's `param_kinds` positionally
against the declared CEL types (`CppParamMatchesDeclType`,
engine.cc:1281 — `Value` matches anything, `string_view` serves
string|bytes, `null`/`type`/`optional` only via `Value`), and
registers under the synthesized id — so a signature mismatch is
rejected at registration, not at eval, and the engine binding can
never diverge from the compiler's import name. Pinned exhaustively by
`EngineBindFunctionTest` (engine_test.cc) and
`e2e/host_fn_test.cc` (`BindFunctionDeclFirstRoundTrip`).

The lower-level surfaces (`AddFunction` with a raw `HostCallback`,
`AddTypedFunction` sugar) and the L0/L1/L2 dispatch stack the bound
callable runs in — 3VL absorption before the callback, kind-checked
slot accessors, the typed-trait adapter, `ReturnUnknown`'s
function-origin sentinel — are the evaluator's story:
**02-evaluator.md §3 (registration & validation points) and §4 (the
host-call stack)**. From this subsystem's perspective the contract is
only: the callback is keyed by overload-id, receives
`(out_slot, args...)` CelValue slots, and never sees Error/Unknown
args (absorbed at L0).

## 5. `@component` end-to-end

The component backend runs an embedder-authored function inside a
sandboxed Component-Model component instead of the host process. The
authoring pipeline is generated; the eval-side marshaling is
02-evaluator.md §9.

### 5.1 The celfnc emitters & `cel generate`

Four pure text emitters under `compiler/celfn/celfnc_emit/` (in →
string, no I/O), all keyed off kForeignComponent decls only
(non-foreign decls are skipped in every emitter):

- **`EmitWit`** (wit_emitter.cc) — the WIT interface: `package
  <pkg>[@ver]; interface fns { ... } world customfn { export fns; }`.
  Type mapping: int→s64, uint→u64, double→f64, null→`option<u8>`
  (wit-bindgen's C generator rejects `option<unit>` — empirically
  probed), bytes & proto→`list<u8>` (the proto fqn is host-side
  metadata, never in WIT), Duration/Timestamp → an in-interface
  `record { seconds: s64, nanos: s32 }`, map<K,V>→`list<tuple<K,V>>`.
  Function names via `SnakeToKebab` (§2 station 5). kType/kOptional
  reaching the emitter is a FailedPrecondition tripwire in case the
  Build() gate regresses (cc:110-116).
- **`EmitCodecH`** (cpp_codec_emitter.cc) — lift/lower between
  wit-bindgen's `customfn_*_t` C structs and `std::` containers.
  `TypeCollector` dedups and topo-orders, emitting only the types the
  library uses. Strings lift to `std::string_view` (zero-copy) and
  lower via `customfn_string_dup_n`; bytes/lists/protos lower via
  `cabi_realloc`; protos are `lift_proto<M>`/`lower_proto<M>`
  templates (ParseFromArray / SerializeToString) so the generator
  never needs descriptors. **Map lower is not emitted** — see §5.4.
- **`EmitStubCc`** (cpp_stub_emitter.cc) — one `extern "C"` export
  body per decl, symbol `exports_<pkg-normalized>_fns_<overload_id>`
  (cc:224); scalar returns pass through by value, everything else is
  an out-param `<struct>* ret` filled via
  `codec::lower(ret, user::CamelFn(codec::lift(*arg)...))`.
  `SnakeToCamel` makes the author-facing function name.
- **`EmitUserFnsH`** (cpp_skeleton_emitter.cc) — the author skeleton:
  `std::string_view`, `const std::vector<T>&`, `const std::map<K,V>&`,
  `const acme::User&` params; returns by value.

CLI: `cel generate --idl <file> --out_dir <dir> [--language=cpp]
[--include=...] [--package --package_version]`
(`tools/cel/run_generate.{h,cc}`). Default package `cel:<module>`,
version `0.1.0`, C++ namespace = the IDL module name. `--language=go`
is explicitly rejected (run_generate.cc:96-101; §8).

Emitter regressions fail the build, not the next user invocation: a
`codec_dumper` genrule emits a codec for a fixed representative
library and a `cc_library` compile-checks it against the hand-curated
`fixtures/customfn.h` on every build
(celfnc_emit/BUILD.bazel:118-163). The dumper exercises map only as
an *arg* — matching the missing map-lower (§5.4).

### 5.2 The build macro

`bazel/cel_wasm_component.bzl`: genrule(`cel generate`) →
genrule(`wit-bindgen c --world customfn`) → wasi-sdk `cc_binary`
under the **wasm32-wasip2** platform transition → rename to
`<name>.wasm`. wasip2 emits a Component-Model component *directly*
(preamble 0x1000d) — no `wasm-tools component new` wrapping step; the
wasip2 and wasi-threads toolchains coexist, selected by target
platform constraint. (The macro's docblock still mentions absl time
types; the emitters use `::google::protobuf::Duration/Timestamp` —
the code wins.)

### 5.3 Naming translations & ownership rules

Three name systems meet here, all derived from the snake_case
overload-id (§2): the WIT/world export is kebab + lowercase
(`SnakeToKebab`), the C export symbol embeds the overload-id verbatim
behind the `exports_<pkg>_fns_` prefix (cpp_stub_emitter.cc:224), and
the author implements a CamelCase function (`SnakeToCamel`).

> **Open question (V27):** the stub's export symbol preserves
> overload-id case (`..._is_adult_message_acme_User`), while
> wit-bindgen derives its expected impl symbol from the lowercased
> kebab WIT name. Whether the proto demo fixture
> (`e2e/foreign_component_fixtures/cel_wasm_component_demo:
> demo_component_proto`, manual-tagged) actually links is unprobed —
> build it; if it links, document why; if not, this is a real bug
> hidden by the manual tag.

Return ownership (codec header contract): the author/stub populate
`*ret` via `customfn_string_dup_n` / `cabi_realloc`; the generated
`cabi_post_*` frees; **the author never calls `_free` on returns.**
Probed against wit-bindgen 0.57.

### 5.4 Known traps

- **The string-return trap.** A string-returning component fn traps
  at call time ("cannot leave component instance", inside libc++
  post-RNG-init under wasm32-wasip2) — pinned as a reasoned skip in
  `e2e/.../demo_component_e2e_test.cc:135-147`. Consequence: the
  supported envelope today is **scalar returns** (the demo's
  `AddRoundTrips` passes; `examples/09_component_functions.cc` is
  scalar-only on purpose, examples/README.md:24-26).
- **The map-lower emitter gap.** `EmitCodecH` emits a literal
  `// TODO(m26): lower($1*, const $0&) not yet emitted`
  (cpp_codec_emitter.cc:371) where map-lower should be — a
  map-*returning* `@component.` decl generates a stub calling a
  nonexistent `codec::lower`. The full-matrix fixture that declares
  exactly such decls (`fixtures/full_matrix.idl`) is referenced by no
  BUILD target, so no gate catches it.

> **Open question (V28):** confirm the failure shape — `cel generate`
> on `map<string,int> @component.f(int x);` + compile the four files —
> then either emit map-lower or reject map returns at `Build()`; the
> durable fix is wiring `full_matrix.idl` into the §5.1 compile gate
> (§8).

- **Example 09's visibility hole.** Building the embedder-side
  `FunctionLibrary` mirror requires
  `//compiler/celfn:function_library`, whose visibility is
  `//:internal` — so the example violates the "examples build against
  the public surface" property and an external embedder cannot copy
  it (examples/BUILD.bazel:173 vs compiler/celfn/BUILD.bazel:4;
  cleanup-backlog #32).

> **Open question (V33):** promote `function_library` (and the decl
> types it exposes) to the public surface — `AddComponent` already
> takes it as a public-API parameter, so the type is *de facto*
> public — or re-scope example 09. Widening visibility is a
> reviewable event per the standing rule.

Also note: example 09's comment claims AddComponent validates exports
"at registration"; actually export resolution is Plan-time and **no
FuncType-vs-decl comparison exists anywhere** — arity/shape
mismatches surface as call-time traps (R36; the full contract and its
open probe V21 live in 02-evaluator.md §3).

## 6. `@native` (kCelDefined): the fork

**Status today: declaration-only.** The grammar (§3.1), the Builder
(module-name gate), checker registration, and codegen's
`kUserModule` import emission all exist and are tested. What does
*not* exist is anything that produces the wasm module those imports
resolve against. A compiled `@native` call emits
`(import "<module_name>" "<overload_id>")`; the only binding
mechanism is `Engine::AddModule(alias, bytes)` with alias = the
module name — and nothing in the tree produces those bytes. The
expected end-to-end behavior is therefore an unresolved-import
failure at Plan (R2/R3).

Two competing implementations exist, **neither live on this branch**:

- **Single-module inlining** (the `master-local` branch, commit
  0386851e, never merged): each body is type-checked and lowered as
  an *internal function of the expression module itself* — disjoint
  rodata bands, recursion rejected, no import at all. Master carries
  this design's *docs* (the superseded plan doc claimed it shipped —
  it did not; the closeout's named files, symbols, and tests exist
  only on that branch) but not its code. One genuine fragment did
  land: `LowerToCustomFn` (`compiler/codegen/expr_lower.h:215-249`,
  expr_lower.cc) lowers a body TypedAst into a wasm fn with the
  custom ABI — params `(out_slot, arg0...)`, no `arena_reset`
  (caller's arena), result via `cel_copy_slot`. It has **zero callers
  and zero tests** on this branch.
- **Bundled library module** (this branch's since-deleted header):
  `compiler/celfn/library_module.h` (deleted per m29 §F2) declared
  `CompileLibraryBodies(lib, parent_opts)` with a full contract —
  bundle every kCelDefined body into one wasm module exporting each
  body under its overload-id, resolved at Plan via
  `Engine::AddModule`. Facts: no `.cc` ever existed (git `--follow`
  over all branches is empty), no BUILD target, no includer; the
  `CompiledArtifact.library_modules` field it was meant to feed is
  likewise gone from `compiler/internal/compile.h`, and
  `Compiler::Compile` discards everything but `wasm_bytes` anyway
  (compiler.cc:195). Master thus has, since the header's deletion,
  neither the header nor the working code of this design — only the
  *docs* of it and its sibling.

The three options, with the evidence weighed:

1. **Port master-local's inlining.** Pro: it existed end-to-end once
   (bodies, recursion guard, scale probes, e2e suites on that
   branch); no new ABI surface — calls become direct internal calls,
   the cheapest dispatch of the three backends. Con: it predates the
   branch reorg and the configurable-linking work; the port is a
   re-land, not a merge, and the rodata-band layout must be
   re-validated against the current memory gates.
2. **Build the library-module producer** the now-deleted
   `library_module.h` described. Pro: the contract was already
   written (in that removed header), and it composes
   with `AddModule` (an existing, tested Engine surface); library
   bodies become shareable across programs. Con: it has never run
   anywhere; it adds a second module to every Plan and a cross-module
   call to every `@native` invocation; `CompiledArtifact.
   library_modules` plumbing must be built end-to-end including
   `Program` carrying multiple byte vectors.
3. **Reject at Compile until one ships.** Pro: converts today's
   silent late failure (an unresolvable import discovered at Plan, or
   worse, hand-bound to wrong bytes) into an immediate, named
   InvalidArgument at the seam the embedder touches; zero design
   risk; trivially removable when an implementation lands. Con: pure
   stopgap.

**Recommendation: option 3 now, as the interim safety measure** — a
Compile-time rejection naming the decl and stating that `@native`
bodies are not yet compilable — keeping the declaration layer (and
its tests) intact while the owner decides between 1 and 2. The
unimplemented-feature rule says exactly this: a reachable
not-done path fails loudly at the edge, never silently miscompiles.

> **Open question (V5):** which backend implementation ships —
> port the single-module inlining, build the library-module producer,
> or hold at reject-at-Compile? The owner decides. First step either
> way: the probe — `SetModuleName("foo").AddCelDefined("is_num",
> bool, {string}, "s == '1'")` → Compile `is_num('1')` → Plan —
> pinning today's failure shape before any code moves. Whichever
> branch loses, its artifacts go: option 1 ⇒ (the `library_module.h`
> header and its `library_modules` plumbing in `compile.h` are already
> deleted) remove the stale header citations (`layout_pass.h:57`,
> `expr_lower_internal.h:51`); option 2 ⇒ delete or wire
> `LowerToCustomFn`.

## 7. Type-surface policy

- **`optional<T>` and `type` are permanently rejected on the
  foreign-component surface** — owner direction, not a temporary
  gap. Enforced structurally at `Build()` (§3.2 gate 5, including
  depth-4 nesting: function_library_test.cc:261-357), with
  FailedPrecondition tripwires in every emitter (§5.1) and rejection
  in both marshaling directions (`eval/internal/cel_component.cc`)
  in case the gate regresses. The grammar has no syntax for either
  kind (§3.1); they are reachable only programmatically.
- **CEL `null` is a distinct kind, and stays supported** on all
  backends (kNull ≠ kOptional; pinned
  function_library_test.cc:359-380). Wire form across the component
  boundary: `option<u8>`, because wit-bindgen's C generator rejects
  `option<unit>`.
- **Map keys are bool|int|uint|string, on every backend, at any
  nesting depth** (§3.2 gate 4) — matching the kernel's map-key
  contract.
- **The gate asymmetry is a live crash bug**: gate 5 is foreign-only,
  so a programmatic `AddHost("f", Prim(kType), ...)` passes `Build()`
  and crashes the compiler at the `ABSL_CHECK` stub in
  `CelfnTypeToCelType` (parse_and_check.cc:770) — embedder input must
  never crash the process. The fix rides V9 (§3.3): extend the
  rejection to all backends at `Build()`. (The stub's message still
  cites a long-shipped plan doc as its unblock condition — stale; the
  types were deliberately closed, not deferred.)
- **Protos cross every boundary as serialized bytes** — `list<u8>`
  over the component boundary, `SerializePartialToString` /
  ParseFromArray at the codec layer — with the fqn kept host-side, so
  neither the WIT nor the generator ever needs descriptors.

## 8. Future work

- **The V5 decision and its cleanup** (§6) — including deleting the
  losing branch's artifacts and re-anchoring the headers that cite
  the nonexistent `library_module.cc`.
- **The arity cap at the Builder** (V9, §3.3) + the all-backend
  kType/kOptional gate (§7), as one `Build()` change with the three
  failure-shape probes first.
- **The full-matrix compile gate**: wire `fixtures/full_matrix.idl`
  through the §5.1 dumper pipeline so every type × position
  combination is generator-compiled on every build — this is what
  catches the map-lower gap (V28) and any future emitter regression
  for shapes the demo doesn't exercise.
- **The map-lower emitter** (V28, §5.4).
- **The Go bindgen path**: `--language=go` is rejected today; the
  tinygo toolchain was never registered. The emitter architecture
  (pure text emitters keyed off the same decls) was designed to admit
  a second language backend.
- **Example 09 visibility** (V33, §5.4) — promote or re-scope.
- **The string-return trap** (§5.4) — root-cause the wasip2 libc++
  interaction; until then the scalar envelope is the documented
  contract.
- **Manual-tag audit**: `function_library_test` and
  `celfn_parser_probe_test` are `manual`-tagged
  (compiler/celfn/BUILD.bazel) and run only via the full-suite query;
  if the reason was ANTLR build cost, record it — otherwise untag.

## History

This doc supersedes the custom-function content of:

- `doc/implementation-plan/rewrite/m13-custom-fns.md` — note its
  CEL-defined "shipped" claims described a branch that never merged
  (§6); its decl model, overload-id synthesis, and rejected
  parallel-table alternative are carried forward here.
- `doc/implementation-plan/rewrite/m22-foreign-fn.md`
- `doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md`
  (compiler/generator sections; eval sections were superseded by
  02-evaluator.md)
- `doc/implementation-plan/rewrite/m26-celfnc-and-component-build.md`
- `doc/implementation-plan/rewrite/modules-and-fnis.md`

Source notes: `doc/design/notes/{celfn,eval-public,eval-internal,
tools-examples,91-contract-coherence}.md` (code-verified 2026-06-10;
anchors re-verified post-merge — `compiler/celfn/**` and
`eval/engine.{h,cc}` untouched; `compile.cc` line anchors updated).
