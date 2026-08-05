# celfn — design notes (undefined)

> **2026-08-04:** the wasm-component plugin backend (`@plugin.`, the
> `celfnc_emit/` emitters, `cel generate`) and the parse-only
> `@native.`/kCelDefined stub described below were **removed**
> (m39-component-removal.md; archived on `component-functions-archive`).
> Every plugin-/native-backend finding in this note is
> resolved-by-removal; the surviving surface is host-only
> (`doc/design/05-custom-functions.md`). Historical record — do not
> cite as current.

Component: custom-function subsystem — the `.celfn` IDL (`compiler/celfn/Celfn.g4`),
`FunctionLibrary` (`compiler/celfn/function_library.{h,cc}`), the orphaned
`library_module.h`, and the `cel generate` emitters
(`compiler/celfn/celfnc_emit/`).  Branch read: `m28-configurable-linking`
(≈ master + m28), 2026-06-10.

## 1. Verified architecture

### 1.1 The decl model — one struct, three backends

`CelfnDecl` (function_library.h:96-126) is the single decl shape for all
backends.  `CelfnDecl::Backend` (h:97-108):

  - `kHost` — `@host.` prefix, no body.  C++ impl bound at Engine
    setup (`Engine::AddFunction` / `AddTypedFunction` / `BindFunction`,
    eval/engine.h:151-235).
  - `kCelDefined` — `@native.` prefix, has a CEL-expression body
    (grammar Celfn.g4:66-68).  **Declaration-side only on this branch**
    — see §2 item 1; no body compiler exists in the tree.
  - `kForeignComponent` — `@component.` prefix, no body.  Backed by a
    Component-Model component registered via
    `Engine::AddComponent(bytes, lib)` (eval/engine.cc:1489-1539).

Key dispatch invariant: kHost and kForeignComponent share ONE wasm
import namespace — `module_name` is always `"cel_fn"`
(function_library.cc:224, 242; pinned by
function_library_test.cc:37, 58, 184).  "A component fn is a host fn at
the call site": codegen/OverloadTable/checker cannot tell them apart
(compile.cc:461-484 `DispatchesViaCelFn` → `ImportModule::kCelFn`).
kCelDefined alone routed to a `kUserModule` import-module kind with the
library's `Module foo;` name as the wasm import module.
> **Removed 2026-08-04 (m39):** the `kUser`/`kUserModule` routing is
> gone.  At HEAD `ImportModuleSource` is `kCel`/`kCelHost`/`kCelFn`
> only, and `OverloadDef` carries no per-overload wasm import-module
> name (`compiler/codegen/overload_table.h:29-63`).

### 1.2 CelfnType and overload-id synthesis

`CelfnType` (function_library.h:45-87): 14 kinds.  Structural recursion
via single-element `list_element`, two-element `map_kv` `[key, value]`,
single-element `optional_element`.  `Argkind()`
(function_library.cc:30-69) synthesises the overload-id slug:
`int`→"int", `list<int>`→"list_int", `map<string,int>`→"map_string_int",
`proto(acme.User)`→"message_acme_User" (dots→underscores, **case
preserved**), `type`→"type", `optional<int>`→"optional_int".

`overload_id = <fn_name>_<argkind>...` (cc:180-188 `SynthesiseOverloadId`,
joined by `_`).  `num_args = uint8_t(params.size()) + 1` — the +1 is the
out_slot (cc:202-206 `Finalise`).  **No params-count cap exists**: 255
params silently wraps `num_args` to 0 (cc:205).  `is_receiver` is true
iff the first param carries `this`.

### 1.3 Grammar (Celfn.g4) and ParseCelfnSource

Productions: `moduleDirective` (`Module foo;`), `hostFnDecl`
(`type '@' 'host' '.' Id '(' params? ')' ';'`), `componentFnDecl`
(same with `component`), `nativeFnDecl` (adds `'=' celExprBody`),
and `bareHostDecl` — a **diagnostic-only** production matching
`host.foo(...)` without `@`, turned into a curated InvalidArgument
("reserved alias; use `@host.`") by the visitor
(function_library.cc:535-539; Celfn.g4:49-59).  There is NO bare-alias
foreign decl (`rules.allow(...)`) production — the m13-era foreign-module
decl shape never shipped (see §2 item 8).

Types in the grammar: `bool|int|uint|double|string|bytes`, `Duration`,
`Timestamp`, `list<T>`, `map<K,V>` (key restricted to
`bool|int|uint|string` at the grammar level, Celfn.g4:107-109),
`proto(fqn)`, `null`.  **No `type` or `optional<T>` syntax** — those
kinds are constructible only programmatically (parse_and_check.cc:702-710
confirms).

CEL bodies are captured as one raw token: `CelExprText :
{ _input->LA(-1) == '=' }? ~[;]+` (Celfn.g4:153-155) — everything up to
the first `;`.  Documented v0 limitation: a `;` inside a string literal
in the body breaks the lexing (Celfn.g4:141-152).

`ParseCelfnSource` (function_library.cc:498-555) = ANTLR parse with a
collecting error listener (errors → InvalidArgument with line:col),
then a tree walk that drives `FunctionLibrary::Builder` — the Builder is
the single validation funnel for both file and programmatic paths.

### 1.4 Builder validation gates (Build(), function_library.cc:322-358)

Run in order, first failure wins:
  1. kCelDefined present ⇒ `module_name_` required (cc:331-335; the
     "iff" claimed at h:199 is enforced in one direction only — a
     module name with zero kCelDefined decls is accepted).
  2. `this` only on first param (cc:190-200; also enforced at the
     grammar-extraction layer cc:481-484).
  3. Per-library overload-id uniqueness (cc:342-346).  Cross-library
     uniqueness is `Compiler::Builder::Build`'s job
     (compiler.cc:152-166).
  4. Universal map-key gate: `FirstIllegalMapKey` recurses through
     list/map/optional carriers; illegal key kind anywhere ⇒
     InvalidArgument naming the decl + param (cc:123-144, 270-285).
     Applies to ALL backends — pinned for kHost too
     (function_library_test.cc:563-576).
  5. kForeignComponent only: `MentionsOptional` / `MentionsType`
     structural rejection of `optional<T>` and `type` anywhere in the
     return or any param (cc:85-113, 287-318).  These are
     **permanently out of scope** for the foreign-component surface
     (m24 §14, user direction 2026-06-03); CEL `null` is a distinct
     kind and stays supported (tests cc-test:359-380).

Note the asymmetry: gate 5 does NOT apply to kHost/kCelDefined, so a
programmatic `AddHost("f", kType, ...)` passes Build() and later crashes
the compiler via `ABSL_CHECK` in `CelfnTypeToCelType`
(parse_and_check.cc:770-771, message still says "until m24").

### 1.5 Library → checker → codegen flow

`Compiler::Builder::AddLibrary` / `AddFunction(celfn_source)` accumulate
`FunctionLibrary`s (compiler.cc:114-135); `Compile()` forwards them to
BOTH `CheckOptions.function_libraries` (checker) and
`CompileOptions.function_libraries` (codegen) (compiler.cc:191-192).

Checker side: `RegisterCustomFunctionsOnChecker`
(parse_and_check.cc:798-823) groups decls by `fn_name` (cel-cpp wants
one `FunctionDecl` per name with N overloads), maps each `CelfnType` to
`cel::Type` via `CelfnTypeToCelType` (scalars, list, map, proto-by-
descriptor-lookup; kType/kOptional CHECK-stub), sets
`overload.set_id(decl.overload_id)` so the checker stamps OUR synthesised
id on resolved call nodes — that id is the OverloadTable lookup key.

Codegen side: `BuildOverloadTable` registered one
`RegisterCustom(overload_id, kCelFn|kUserModule, module_name,
helper_name=overload_id, num_args)` row per decl — as-shipped at HEAD
(post-m39) every custom row is `kCelFn` and an `OverloadDef` has no
module-name string; the module string is derived from
`wasm_import_module_type` alone (`overload_table.h:29-67`).
`InstallOverloadImportsExport` (compile.cc:301-357) then installs one
wasm function import per row — `(import "cel_fn"|"<module>"
"<overload_id>" (param i32 ×num_args))`, all-void returns, out_slot
first.  Arity switch handles `num_args` 1..5 only; anything else
**silently skips installation** (compile.cc:294-296, "matches pre-M13
'unknown arity ⇒ silently skip'") — so a custom decl with ≥5 value-args
emits a module whose call target is missing.

### 1.6 Eval-side binding (for context; owned by eval notes)

  - kHost: `wasmtime_->host_callbacks[overload_id]` bound as
    `cel_fn.<overload_id>` linker funcs at Plan.  `BindFunction`
    (engine.h:231-235, engine.cc:1285-1309 `ParseSingleHostDecl`)
    re-parses the SAME `.celfn` decl string, requires exactly one
    `@host.` decl, and validates the C++ callable's param kinds
    against the declared CEL types (`CppParamMatchesDeclType`,
    engine.cc:1251-1283; kNull/kType/kOptional accept only `Value`).
  - kForeignComponent: `AddComponent` conflict-checks overload-ids
    against host callbacks and prior components, parses the component
    once, stores `(component, lib)` (engine.cc:1489-1539).  Per Plan,
    each component is instantiated and each decl's export resolved by
    `OverloadIdToKebab(overload_id)` — underscores→hyphens AND
    uppercase→lowercase (engine.cc:1037-1047) — either at the
    component top level or inside `lib.wit_interface()` when set
    (engine.cc:1107-1134; `FunctionLibrary::SetWitInterface`,
    function_library.h:145-169).  The bound trampoline is again a
    `cel_fn.<overload_id>` linker func (engine.cc:1086-1090) — the
    dispatch unification made concrete.
  - kCelDefined: the expr module imports
    `("<module_name>" "<overload_id>")`; the only binding mechanism on
    this branch is `Engine::AddModule(alias, wasm_bytes)`
    (engine.cc:1365+) with alias = the module name — and nothing in
    the tree produces those wasm bytes (§2 item 1).

### 1.7 library_module.h — an orphaned interface

`compiler/celfn/library_module.h` declares
`CompileLibraryBodies(lib, parent_opts) → StatusOr<vector<uint8_t>>`
with a full contract (bundle every kCelDefined body into one wasm module
exporting each body under its overload_id; FailedPrecondition on missing
module name; InvalidArgument on body type-check failure).  Facts:

  - No `library_module.cc` exists, and **never has** (git log
    `--follow` over all branches is empty for the .cc).
  - No BUILD target compiles the header; nothing includes it.
  - `compiler/internal/compile.h:105-126` defines `LibraryModuleBytes`
    + `CompiledArtifact::library_modules` "produced by
    CompileLibraryBodies" — never populated by compile.cc, and
    `Compiler::Compile` discards everything but `wasm_bytes`
    (compiler.cc:195).
  - `compile.h:137,149`, `layout_pass.h:57`, `expr_lower_internal.h:51`
    all cite "`compiler/celfn/library_module.cc`" /
    "`library_module.cc::LowerCelDefinedFn`" as their consumer.

The one real piece of CEL-defined codegen that DID land is
`LowerToCustomFn` (expr_lower.h:215-249, expr_lower.cc:1313+): lowers a
body TypedAst into a wasm fn with the M13 custom ABI — params
`(out_slot, arg0...)`, NO `arena_reset` (caller's arena), result written
via `cel_copy_slot` into the caller's out_slot, free variables only from
declared params (CHECK otherwise).  It has **zero callers and zero
tests** on this branch (grep: only its own definition + header).

### 1.8 celfnc_emit — the `cel generate` backend (m26, shipped)

Four pure text emitters (in → string, no I/O), all keyed off
kForeignComponent decls only (non-foreign decls are skipped in every
emitter: wit_emitter.cc:170, cpp_codec_emitter.cc:459,
cpp_stub_emitter.cc:335, cpp_skeleton_emitter.cc:174):

  - **EmitWit** (wit_emitter.cc): `package <pkg>[@ver]; interface fns
    { ... } world customfn { export fns; } world host { import fns; }`.
    Type mapping (WitTypeText, cc:61-119): int→s64, uint→u64,
    double→f64, null→`option<u8>` (wit-bindgen 0.57 rejects
    option<unit>, cc:73-76), bytes & proto→`list<u8>` (proto fqn is
    host-side metadata only, never in WIT), duration/timestamp →
    in-interface `record { seconds: s64, nanos: s32 }` declared once,
    map<K,V>→`list<tuple<K,V>>`.  Fn names via `SnakeToKebab`
    (cc:151-162): `_`→`-` AND uppercase→lowercase (proto CamelCase
    segments flatten; pinned wit_emitter_test.cc:128-131).
    kType/kOptional reaching the emitter = FailedPrecondition
    regression tripwire (cc:110-116).
  - **EmitCodecH** (cpp_codec_emitter.cc): lift/lower between
    wit-bindgen `customfn_*_t` C structs and `std::` containers.
    `TypeCollector` dedups + topo-orders (inner before outer) and
    emits ONLY types the lib uses (cc:193-232).  Raw-string-template
    discipline: every shape is a named `kFooTpl` + one
    SubstituteAndAppend (cc:18-28).  Strings lift to
    `std::string_view` (zero-copy), lower via `customfn_string_dup_n`;
    bytes/lists/protos lower via `cabi_realloc` (fwd-declared,
    cc:509-516, since customfn.h doesn't declare it); protos are
    `lift_proto<M>`/`lower_proto<M>` templates
    (ParseFromArray/SerializeToString, cc:261-277);
    duration/timestamp lift/lower against
    `::google::protobuf::Duration/Timestamp` with conditional
    includes (cc:160-164, 469-506).  **Map lower is NOT emitted** —
    the emitted text contains a literal
    `// TODO(m26): lower($1*, const $0&) not yet emitted` (cc:371).
    Ownership rule (header cc-h:12-17): author never calls `_free` on
    returns; `cabi_post_*` cleans up.
  - **EmitStubCc** (cpp_stub_emitter.cc): one `extern "C"` export body
    per decl, named `exports_<pkg-normalized>_fns_<overload_id>`
    (`:`→`_`; cc:25-27, 323-324).  Scalar returns pass through by
    value; everything else is out-param `<struct>* ret` +
    `codec::lower(ret, user::CamelFn(codec::lift(*arg)...))`;
    proto returns use `lower_proto<acme::User>` (cc:200-298).
    `SnakeToCamel` (cc:304-317) makes the author-facing name.
  - **EmitUserFnsH** (cpp_skeleton_emitter.cc): author skeleton.
    Param shapes: `std::string_view`, `const std::vector<T>&`,
    `const std::map<K,V>&`, `const acme::User&`; returns by value
    (cc:33-79).  Include set computed by walking types (cc:83-133).

CLI: `cel generate --idl --out_dir [--language=cpp] [--include=...]
[--package --package_version]` (tools/cel/run_generate.{h,cc}).
Default package `cel:<module>` (fallback `cel:customfn`), version
`0.1.0`, C++ namespace = the IDL module name (run_generate.cc:49-61).
`--language=go` is explicitly rejected ("arrives with H.4",
run_generate.cc:96-101).

Build macro: `bazel/cel_wasm_component.bzl` — genrule(`cel generate`) →
genrule(`wit-bindgen c --world customfn`) → wasi-sdk `cc_binary` under
the **wasm32-wasip2** platform transition (emits a CM component
directly, preamble 0x1000d, no `wasm-tools component new` step) →
rename to `<name>.wasm` (bzl:1-44, 114-163).  Toolchains
`third_party/wit_bindgen`, `third_party/wasm_tools` exist; tinygo does
not (Go path unshipped).

Compile gate: `codec_dumper` (celfnc_emit/codec_dumper.cc) emits a
codec.h for a fixed representative library; a genrule + `cc_library
codec_compile_check` compiles it against the hand-curated
`fixtures/customfn.h` every build (BUILD.bazel:118-163).  Note the
dumper exercises map only as an ARG (scalar return), matching the
missing map-lower.

E2E: `e2e/foreign_component_fixtures/cel_wasm_component_demo/` —
macro-built component from `fns.idl` (greet/add/len), dual link-mode
test.  `AddRoundTrips` (scalar) passes; `GreetRoundTripsString` is
GTEST_SKIP'd on a real wasm trap ("cannot leave component instance"
inside libc++ post-RNG-init under wasm32-wasip2; m26 #44,
demo_component_e2e_test.cc:135-147).  A proto variant
(`demo_component_proto`, `fns_proto.idl`, manual-tagged — libprotobuf
under WASI is a multi-minute build) exercises proto-as-bytes.

### 1.9 Shipped-vs-plan map (m13 / m26)

m13 (three backends):
  - @host: **shipped** end-to-end (decl → checker → `cel_fn` import →
    host callback; complex types via the m21 typed adapter;
    e2e/host_fn_test.cc + host_fn_type_matrix_test.cc).
  - @native (kCelDefined): **declaration layer only** on this branch
    (and **deleted entirely in m39**, kUserModule emission included).
    Grammar + Builder + checker registration + kUserModule import
    emission existed; the body compiler, recursion guard, scale probes,
    and e2e suites the doc lists live on `master-local` /
    `backup/local-pre-reorg-20260526` (commit 0386851e), which never
    merged.  The doc was replayed onto master (26019b06 "docs +
    foreign-go probes: replay unique local work onto reorganized
    master") without the code.
  - foreign module (bare `<alias>.` decl + `cel_call_foreign`):
    **never shipped as designed**; superseded by m24's
    kForeignComponent (`@component.`).  `Engine::AddModule` survives
    as the generic aliased-core-module registry (engine.cc:1365+,
    reserved aliases `cel|cel_host|cel_env|cel_fn|host`).

m26 (status header says "not yet started" — code disagrees):
  - Phase A toolchain: wit-bindgen + wasm-tools shipped; tinygo not.
  - Phase B celfnc emitters + `cel generate` + compile gate:
    **shipped** (doc body §10 even says so, header not updated).
  - Phase C macro + demo + e2e: **shipped** (string path skipped on
    the wasip2 trap).
  - Phase D Go/TinyGo: not shipped.  Phase E bench:
    `bench/foreign_component_bench.cc` exists.  Phase F full-matrix
    fixture: `fixtures/full_matrix.idl` exists but the planned
    `full_matrix_dumper.cc` + four-file compile gate (m26 §7.5) was
    never built — the fixture is referenced by no BUILD target.

## 2. Doc-vs-code discrepancies

1. **P0 — m13 doc claims the CEL-defined backend shipped; it did not
   (on master).**  m13-custom-fns.md:3-5 ("CEL-defined (single-module)
   backend shipped end-to-end 2026-05-24"), §0.5 table rows 46-51, and
   the closeout tracker (190-246) name files/symbols/tests that do not
   exist anywhere on this branch: `codegen/custom_fn_emit.cc`
   (`EmitCustomFnBodies`), `codegen/expr_lower_customfn.cc`,
   `compile.cc::LowerCustomFnBodies`, `CelDefinedOverloadIds`,
   `CheckCelDefinedBody`, `RejectCelDefinedRecursion`,
   `EngineCelDefinedFnE2ETest`, `EngineCelDefinedMatrixTest`,
   `CompileCelfnScaleTest`, the 254/255 arg-cap test.  The implementing
   commit (0386851e) is only on `master-local` + backup.  A reader of
   master's docs believes `@native` evaluates; in reality a compiled
   `@native` call produces an unresolvable wasm import.
2. **P0 — `CompileLibraryBodies` is declared, contracted, and
   cross-referenced, but has no implementation and no caller.**
   library_module.h:45-46 vs (no .cc, no BUILD target, no include);
   compile.h:106-126 (`library_modules` "produced by
   CompileLibraryBodies") vs compile.cc (never populated) and
   compiler.cc:195 (dropped from Program); layout_pass.h:57 +
   expr_lower_internal.h:51 cite the nonexistent `library_module.cc`.
3. **P1 — m26 doc status header is stale.**
   m26-celfnc-and-component-build.md:3 "not yet started" vs the shipped
   emitters/CLI/macro/demo (§1.8 above); the doc's own §10 records
   Phase B "Shipped 2026-06-04".
4. **P1 — function_library.h contradicts itself (and the code) on
   kType/kOptional.**  h:59-66 ("only reachable through
   kForeignComponent decls today") and h:183-184 ("AddForeignComponent
   … Admits `type` and `optional<T>` per m24 §6") vs Build()'s
   permanent rejection (function_library.cc:287-318, tests
   function_library_test.cc:261-357) — which h:202-203 correctly
   states three paragraphs later.  Also parse_and_check.cc:767-771's
   stub message "until m24's component-backend lands" — m24 shipped
   (status line m24 doc:3) and deliberately closed those types.
5. **P1 — engine.h says AddComponent is unimplemented.**
   engine.h:174-176 "Status: not yet implemented … returns
   `Unimplemented`" vs the full implementation at engine.cc:1489-1539
   and the passing demo e2e.
6. **P1 — claimed arg-count cap doesn't exist; arity >5 silently skips
   import install.**  m13 doc:203-204 "ABI arg-count cap (254 accept /
   255 reject) at the library layer (function_library_test.cc)" — no
   such test or check on this branch; `num_args =
   uint8_t(params.size())+1` wraps (function_library.cc:205), and
   `InstallOverloadImport`'s default arm returns false for num_args ∉
   [1,5] (compile.cc:294-296) — a 5-param custom decl yields a module
   whose call target was never imported.
7. **P1 — m26 §4 matrix claims map LOWER is emitted; code emits a TODO
   instead.**  m26 doc:285 ("per-entry lower into a freshly-allocated
   author_list_tuple2_<K>_<V>") vs cpp_codec_emitter.cc:371
   (`// TODO(m26): lower($1*, const $0&) not yet emitted`).  A
   map-returning `@component.` decl generates a stub calling a
   nonexistent `codec::lower`.  full_matrix.idl:32-33 declares exactly
   such decls, and the gate that would catch it (m26 §7.5
   full_matrix_dumper pipeline) was never built — the fixture is
   unreferenced by any BUILD file.
8. **P2 — m26 §2 shows a decl syntax the grammar rejects, and names
   the wrong grammar files.**  doc:87-93 `bool rules.allow(...)` (bare
   alias prefix) and doc:112 "grammar lives at CelfnLexer.g4 +
   CelfnParser.g4" vs Celfn.g4 (single file; only
   `@host./@component./@native.` decl shapes; bare prefix is the
   diagnostic-only `bareHostDecl`).  Doc §7.5.1 self-corrects later.
   Same drift in m13's TL;DR (doc:282-316 bare `rules.allow` foreign
   decls — never shipped).
9. **P2 — m26 §3.5.1 tables use the `author_*` prefix and absl
   time types; code emits `customfn_*` (world renamed) and
   `::google::protobuf::Duration/Timestamp`**
   (cpp_codec_emitter.cc:39-47, 160-164; the macro docblock
   cel_wasm_component.bzl:36-37, 51-53 also still mentions absl/time).
10. **P2 — `m23_native_quad_inline.wat` has no wat-traces.md entry**
    (CLAUDE.md WAT-first step 4; the file exists under
    doc/implementation-plan/rewrite/wat/ and is exercised by
    wat_runner_test.cc:1261-1264).
11. **P2 — h:199 "`Module` set iff any kCelDefined decl present"** —
    only the ⇐ direction is enforced (function_library.cc:331-335);
    a module name with no kCelDefined decls builds fine (untested
    either way).

## 3. Validation items

1. **What does a `@native` library actually do end-to-end today?**
   Expected: Compile succeeds emitting `(import "<module>"
   "<overload_id>")`; `Engine::Plan` fails instantiation with an
   unresolved-import error (or works iff the embedder hand-supplies a
   module via `AddModule(<module>, bytes)`).  Probe: a cc_test that
   builds `SetModuleName("foo").AddCelDefined("is_num", bool, {string},
   "s == '1'")`, compiles `is_num('1')`, Plans, and asserts the error
   shape.  Settles whether @native should be rejected at Compile until
   the producer lands.
2. **Does `demo_component_proto` build and link?**  The stub's export
   symbol embeds the overload-id verbatim
   (`exports_cel_customfn_fns_is_adult_message_acme_User`, uppercase
   'U', cpp_stub_emitter.cc:224) while the WIT name is lowercased
   kebab (`is-adult-message-acme-user`), and wit-bindgen's C generator
   derives its expected impl symbol from the WIT name (all-lowercase
   snake).  Run: `bazel build //e2e/foreign_component_fixtures/
   cel_wasm_component_demo:demo_component_proto` (manual target).  If
   it links, document why; if not, this is a P1 bug hidden by the
   manual tag.
3. **Map-return through the whole generator.**  `cel generate` on
   `map<string,int> @component.f(int x);` then compile the four files
   — expected failure on missing `codec::lower` (item 7).  The fix
   gate is wiring full_matrix.idl into the planned full-matrix
   compile-check.
4. **≥5-value-arg custom decl failure shape.**  Compile a 5-param
   `@host` decl + call; assert the error is a Binaryen validate /
   FailedPrecondition naming the missing helper, not a silent
   miscompile (compile.cc:294-296 path).
5. **kType/kOptional on a kHost decl crashes the compiler.**
   `AddHost("f", Prim(kType), ...)` passes Build() (gate 5 is
   foreign-only) and hits `ABSL_CHECK(false)` at
   parse_and_check.cc:770 during Compile — embedder input crashing
   the process violates the "embedder input must not crash" rule
   (compile.cc:515-517 states it for rodata).  Probe + decide: extend
   the Build() gate to all backends, or map the types.
6. **255-param wrap.**  `params.size()==255` ⇒ `num_args==0`.  Probe
   Build() + decide where the cap lives (the m13 doc thought it was
   the library layer).
7. **Are the `manual` tags on `function_library_test` and
   `celfn_parser_probe_test` (compiler/celfn/BUILD.bazel:41,51) still
   intentional?**  They only run via run_full_suite.sh's manual-target
   query.  If the reason was ANTLR build cost, record it; otherwise
   untag.

## 4. Test coverage observations

Pinned well:
  - Grammar layer: celfn_parser_probe_test.cc — all three decl shapes,
    module directive, proto/aggregate types, comments, bare-decl and
    body-less-`@native` rejections.
  - Library semantics: function_library_test.cc (631 lines) — overload-
    id synthesis across the type matrix (78-96), per-backend module
    routing, duplicate-id, `this` placement, module-name requirement
    (both file + programmatic paths), the full optional/type rejection
    matrix including depth-4 nesting (261-357, 464-480), null-vs-
    optional distinctness (359-380), map-key legality for all four
    legal kinds + double/bytes negatives at top level and nested in
    list/map-value, proto-on-host/component/cel-defined acceptance.
  - Emitters: per-type matrices in all three cpp emitter tests + the
    WIT test (every m24 §6 row, nesting shapes, dedup, byte-for-byte
    determinism, kebab/camel translations incl. proto-CamelCase
    flattening), plus the build-time codec compile gate.
  - Integration: demo component e2e (scalar path, dual link-mode);
    foreign_fn_type_matrix_test + foreign_component_dispatch_test on
    the eval side.

Gaps:
  - `LowerToCustomFn` (expr_lower.cc:1313+): zero tests, zero callers.
  - No test that a kCelDefined library compiles into a Program (or is
    rejected); no eval-side @native coverage at all.
  - No `run_generate_test.cc` (planned m26 §8.1.1); `cel generate` CLI
    flag handling is untested (cel_smoke_test.sh does not cover it).
  - Map-as-return, list<proto>, >4-arg decls — no generator or
    compile coverage (full_matrix.idl exists but is dead).
  - `SetWitInterface` has no function_library-level test (only used
    via the demo e2e).
  - Param-count boundaries (0 params for non-scalar return is tested
    via `now()`; 254/255/256 untested).

## 5. Design decisions worth preserving

  - **One decl struct, one wasm calling convention.**  Every custom fn
    is `(call $helper (out_slot, *arg_slots))` over the 24-byte
    CelValue ABI regardless of backend; backend choice only changes
    the import-module string and who binds it at Plan.  Component-ness
    is invisible to checker/codegen — `kForeignComponent` ⇒
    `module_name == "cel_fn"`, same as kHost (m24 §2).
  - **Builder is the single validation funnel.**  File parsing is a
    thin driver over the programmatic Builder, so grammar-bypassing
    embedders hit the same gates (map-key legality, optional/type
    rejection) at registration time with the decl named — not at
    first Eval or deep inside codegen.  The celfnc generator inherits
    the gates the same way (m26 §12 mitigation).
  - **Overload-ids are synthesised, not user-chosen** —
    `<fn>_<argkind>...` — and the SAME id is (a) stamped by the
    cel-cpp checker via `OverloadDecl::set_id`, (b) the OverloadTable
    key, (c) the wasm import field name, (d) the host-callback map
    key, and (e) kebab+lowercased, the component export name.  The
    snake↔kebab translation is mirrored in exactly two places
    (wit_emitter `SnakeToKebab`, engine `OverloadIdToKebab`) and must
    stay in lockstep.
  - **`optional<T>` and `type` are permanently rejected on the
    foreign-component surface** (m24 §14, user direction) — rejected
    at Build(), with FailedPrecondition tripwires in every emitter in
    case the gate regresses.  CEL `null` is deliberately distinct
    (kNull; wire form `option<u8>` because wit-bindgen's C generator
    rejects `option<unit>` — empirically probed, m26 §3.5).
  - **Protos cross the CM boundary as serialized bytes** (`list<u8>`),
    fqn kept host-side; codec uses `lift_proto<M>`/`lower_proto<M>`
    templates so the generator never needs proto descriptors.
  - **Return-ownership**: author/stub populate `*ret` via
    `customfn_string_dup_n`/`cabi_realloc`; `cabi_post_*` frees; the
    author NEVER calls `_free` on returns.  (m24 §4.0/m26 §4.0 —
    probed against wit-bindgen 0.57.)
  - **Emit only what the lib uses** (TypeCollector) — avoids template
    bloat; **raw-string-template discipline** in every emitter so the
    generated text is readable in the C++ source; **byte-for-byte
    determinism** is a tested contract.
  - **Generated-code compile gate as a build step** (codec_dumper
    genrule) — emitter regressions fail `bazel build`, not the next
    end-user macro invocation.  Worth extending to the full-matrix
    fixture (currently dead).
  - **wasm32-wasip2 emits components directly** — no
    `wasm-tools component new` wrap; the wasip2 and wasi-threads
    toolchains coexist, selected by target platform constraint
    (cel_wasm_component.bzl:14-29).
  - **Rejected alternative (m13 §5.2):** a separate parallel dispatch
    table for customs — rejected in favour of extending OverloadTable
    (one path for built-ins and customs).
  - **Open architectural fork to settle in the new docs:** the
    CEL-defined backend has TWO competing shipped-on-some-branch
    designs — single-module inlining (master-local: bodies become
    internal fns in the expr module, disjoint rodata bands in
    [0,8192), recursion rejected) vs the separate bundled library
    module this branch's `library_module.h` describes (imports
    resolved via Engine::AddModule).  Master currently has the
    header of the second and the docs of the first, and the working
    code of neither.
