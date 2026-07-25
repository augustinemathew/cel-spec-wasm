# m35 — Plugin ergonomics: self-describing wasm plugins, `Use`, Plan-time verification

Status: plan — revised 2026-07-25 after an interactive API-design
session with the user; supersedes the 2026-07-22 draft in place.
Naming settled 2026-07-25 (second session): the public noun is
**`Plugin`**, replacing the draft's `Component`, as a **full-family,
repo-wide rename** (§1.1) — the file was renamed from
`m35-component-ergonomics.md` accordingly.  Two slices were added in
the same session: **R** (the rename, landing first) and **S** (the
doc-site rewrite — the earlier draft under-scoped docs to one guide
page).  Active scope: slices R (rename), A (self-describing artifact
+ `Plugin` noun), V (Plan-time required-function verification +
selective instantiation), B (`Use` on both sides + rename + examples),
S (doc site), N (benchmarks).  Deferred: slice C (`Swap` — §8 keeps
the settled contract) and slice D (C ABI — §10; its m34 groundwork
was explicitly skipped 2026-07-25 — D stays deferred and its
prerequisites get renumbered when someone picks it up).

## 1. Motivation — the declaration triplication

Registering a plugin function today requires the same declaration
three times: once in the `.celfn`/`.idl` the plugin was built
from, once as a hand-written C++ `FunctionLibrary::Builder` mirror
(~15 lines in `examples/09_component_functions.cc`, ~20 in the demo
e2e's `BuildDemoLibrary`), and once threaded into both
`Compiler::Builder::AddLibrary` and `Engine::AddComponent(bytes,
lib)`.  A fourth hand-maintained coupling hides in the mirror: the
`SetWitInterface("cel:greeter/fns@0.1.0")` magic string must match
the build macro's derived WIT package name.  The C++ mirror is the
bug farm: it can drift from the `.celfn` silently, and the resulting
error surfaces at Plan or eval, far from the drift.

(This section describes the pre-m35 world and keeps the pre-rename
names; §1.1 maps them to their post-R spellings.)

Two further gaps this milestone closes, surfaced during design:

  - **No signature agreement is checked anywhere.**
    `Engine::AddComponent` validates bytes + overload-id collisions
    only; export *existence* is checked at Plan
    (`BindOneComponentDecl`); a program compiled against
    `bool is_adult(proto(acme.User))` evaluated against a plugin
    declaring a different signature is a call-time trap or a silent
    miscompile.  (The `eval/engine.h` doc comment claiming
    registration-time export + FuncType validation is false — known
    finding R36; corrected in slice B.)
  - **Every registered plugin instantiates into every Plan**,
    used or not (`InstantiateAndBindComponents`).  Register 10
    plugins and every Plan pays 10 instantiations + WASI stub
    installs.  Fixed in slice V4 (§6.4).

### 1.1 Naming — `Plugin` replaces `Component` (settled 2026-07-25)

The draft named the public noun `Component`, after the W3C
Component-Model binary it wraps.  Rejected, for three reasons:

  1. **Collision with the repo's other "component."**  The testing
     and design docs use "component" pervasively to mean *pipeline
     component* (frontend, codegen, runtime, …) —
     `per-component-test-coverage.md` is named after that sense.
     One word, two load-bearing meanings, same doc site.
  2. **It names the hidden tech, not the concept.**  The doc site
     already demoted WIT/Component-Model to an implementation
     detail; surfacing the spec's noun as the public class runs
     against that direction.
  3. **The adjective test.**  The doc site needs an "X functions"
     phrase that contrasts with "host functions" (trusted,
     in-process).  "Plugin functions" says sandboxed/third-party at
     a glance; "component functions" says nothing.

Alternatives weighed: `WasmExternalLibrary` ("external" also
describes host functions, so it fails the disambiguation job;
long), `WasmLibrary` (collides with `FunctionLibrary`, which stays
on the API), `Extension` (collides with CEL extension libraries),
`Module` (collides with core-wasm module — the `Load` error
contract distinguishes component-vs-core-module binaries).

**Scope: full family, repo-wide** (user call: the shipped surface
is not sacred).  m35 is the last cheap rename window — slice A
freezes the `cel.fns` section, whose embedded declaration text
carries the backend prefix, and whose hash covers that text; after
that the name is in the wire format.  The rename is slice R and
lands FIRST, so slices A–N build the new surface under the new
names.  The mapping:

| Today (pre-R) | After slice R |
|---|---|
| `@component.` decl prefix (celfn grammar) | `@plugin.` |
| `CelfnBackend::kForeignComponent` (`function_library.h:100`) | `kPlugin` |
| `cel_wasm_component` macro, `bazel/cel_wasm_component.bzl` | `cel_wasm_plugin`, `bazel/cel_wasm_plugin.bzl` |
| `Engine::AddComponent` (stays as the escape hatch) | `Engine::AddPlugin` |
| `RegisteredComponent`, `component_libraries` (`wasmtime_engine_state.h`) | `RegisteredPlugin`, `plugin_registry` |
| `InstantiateAndBindComponents` | `InstantiateAndBindPlugins` |
| `InstanceImpl::component_fn_envs` | `plugin_fn_envs` |
| `eval/internal/cel_component.{h,cc}` (+ test) | `eval/internal/cel_plugin.{h,cc}` |
| `examples/09_component_functions.cc` | `examples/09_plugin_functions.cc` |
| `e2e/foreign_component_dispatch_test.cc` | `e2e/plugin_dispatch_test.cc` |
| `e2e/foreign_component_fixtures/` (+ `cel_wasm_component_demo`, `demo_component_e2e_test.cc`) | `e2e/plugin_fixtures/` (+ `cel_wasm_plugin_demo`, `demo_plugin_e2e_test.cc`) |
| `benchmark/component/`, `foreign_component_bench.cc` | `benchmark/plugin/`, `plugin_bench.cc` |
| doc site: "component function(s)" | "plugin function(s)" |
| `doc/user-guide/writing-component-functions.md` | `doc/user-guide/writing-plugins.md` |

**What keeps "component":** identifiers that denote the actual
Component-Model objects — `wasmtime_component_t` and the rest of
the wasmtime C API, the `WasmLayer::kComponent` /
`IsComponentBinary` classifiers in `//abi:wasm_binary` (§4), and
prose about the CM binary format itself ("a plugin is packaged as
a Component-Model component" stays a true sentence the docs say
once, in the internals page, not the noun users hold).
`doc/design/notes/**` are frozen historical audit records and are
NOT swept.

## 2. Quickstart — a plugin function that takes a proto

> This section doubles as the seed for
> `doc/user-guide/writing-plugins.md` (rewritten onto this surface
> in slice S3).  It is written against the TARGET API; nothing
> below exists until slices R–B land.

Write the declarations once, in an `.idl`:

```c
// scorer.idl
Module scorer;

bool             @plugin.is_adult(proto(acme.User) u);
proto(acme.User) @plugin.capitalize(proto(acme.User) u);
```

Build the plugin.  The macro compiles your implementations to a
sandboxed Component-Model wasm and embeds the declarations verbatim
in a `cel.fns` custom section — the artifact describes itself:

```python
cel_wasm_plugin(
    name = "scorer_plugin",
    idl  = "scorer.idl",
    user_fns = ["scorer_fns.cc"],          # implements user_fns.h
    deps = [":user_cc_proto"],             # acme.User C++ proto
    extra_includes = ["acme/user.pb.h"],
)
```

```cpp
// scorer_fns.cc — protos cross the sandbox boundary as serialized
// bytes; the generated codec deserializes before calling you.
#include "user_fns.h"
#include "acme/user.pb.h"

namespace scorer {
bool IsAdult(const acme::User& u) { return u.age() >= 18; }
acme::User Capitalize(const acme::User& u) { /* ... */ }
}  // namespace scorer
```

Load it — one noun carries bytes, declarations, and a content hash:

```cpp
#include "abi/plugin.h"

auto plugin = celwasm::Plugin::Load(scorer_bytes).value();
// plugin.decls()      — the parsed declarations
// plugin.hash_hex()   — SHA-256 over (bytes ‖ declarations)
```

Compile side — declarations flow to the type-checker; no mirror:

```cpp
auto builder = celwasm::Compiler::NewBuilder();
builder.DeclareVariable("user", celwasm::CelType::Message("acme.User"))
       .Use(plugin);
auto compiler = std::move(builder).Build().value();
auto program = compiler.Compile("is_adult(user) && user.age < 120").value();
// The call site type-checks against the .idl signature —
// `is_adult(user.age)` fails HERE, at compile.  The Program records
// every custom function it calls (name + full signature) in its
// cel.abi, for Plan to verify.
```

Eval side — possibly a different process that never links the
compiler:

```cpp
auto engine = celwasm::Engine::NewBuilder().Build().value();
CHECK_OK(engine.Use(plugin));
// Fail-fast registration: overload-id collisions, byte parse, and a
// static check that the plugin actually exports every declared
// function — a bad plugin upload is rejected HERE, not at traffic
// time.  No instantiation happens yet.

auto instance = engine.Plan(program).value();
// Plan verifies every function the program requires exists in the
// registry with an EXACTLY matching signature (protos compare by
// fully-qualified name), then instantiates ONLY the plugins the
// program actually calls — each into its own sandbox with its own
// linear memory.

acme::User u;  u.set_name("ada");  u.set_age(30);
celwasm::Activation act;
act.Bind("user", celwasm::Value::Message(u));
auto result = instance.Eval(act);      // -> Value::Bool(true)
```

What failure looks like — forgot to register the plugin:

```
FailedPrecondition: Engine::Plan: program requires plugin function
`is_adult_proto_acme_user` (`bool is_adult(proto(acme.User))`) but no
registered plugin declares it; register the providing plugin with
Engine::Use before Plan
```

— and a plugin update that changed a signature out from under a
compiled program:

```
FailedPrecondition: Engine::Plan: program requires plugin function
`is_adult_proto_acme_user` with signature
`bool is_adult(proto(acme.User))` but the registered plugin
(hash 3f9a2c1b04de) declares `bool is_adult(proto(acme.Person))`;
signatures must match exactly — recompile the program or rebuild the
plugin
```

## 3. The C++ API

### 3.1 `Plugin` (`//abi:plugin`, public)

`Plugin` lives at `abi/plugin.{h,cc}`.  Rationale: both
`Compiler::Builder::Use` and `Engine::Use` take `const Plugin&`,
so it must sit below `compiler/` and `eval/`; `abi/` is the wire-
contract role dir, already in the curated `//visibility:public` set,
and already on both sides' dep paths.  The new edge
`abi → //compiler/celfn:function_library` (for `ParseCelfnSource`)
is the same data-vocabulary class of edge `eval` already holds.
Support targets stay `//:internal`: `//abi/internal:sha256` (small
first-party SHA-256 — no crypto dep exists in MODULE.bazel),
`//abi:wasm_binary` (§4 — the consolidated binary-format layer),
`//abi:celfn_wire` (§5 type mapping).

```cpp
class Plugin {
 public:
  // Parse `plugin_bytes`' embedded `cel.fns` section — the ONLY
  // way to construct a Plugin; every Plugin is a
  // self-describing artifact by construction.
  // InvalidArgument on: empty bytes; not a Component-Model binary
  // (message flags the core-module case); missing `cel.fns` section
  // (message: "no cel.fns section — rebuild with cel_wasm_plugin
  // or run `cel embed-decls`"); malformed section framing;
  // non-UTF-8 text; `.celfn` parse failure (line+col preserved);
  // any decl whose backend is not `@plugin.` (names the decl);
  // zero declarations.
  static absl::StatusOr<Plugin> Load(
      absl::Span<const uint8_t> plugin_bytes);

  const FunctionLibrary& library() const;     // decls + wit_interface
  absl::Span<const CelfnDecl> decls() const;  // = library().decls()
  absl::string_view celfn_source() const;     // verbatim declaration text
  const std::string& wit_interface() const;   // e.g. "cel:scorer/fns@0.1.0"
  absl::Span<const uint8_t> bytes() const;    // owned copy of the wasm
  const std::array<uint8_t, 32>& hash() const;  // SHA-256(bytes ‖ source)
  std::string hash_hex() const;

 private:
  Plugin() = default;
  std::vector<uint8_t> bytes_;
  std::string celfn_source_;
  FunctionLibrary library_;
  std::array<uint8_t, 32> hash_{};
};
```

Immutable after Load; safe to share across threads and register on
any number of compilers and engines.  Per-decl source/doc-comment
introspection is NOT on this surface (the grammar skips comments and
`CelfnDecl` keeps no source spans); the whole-text `celfn_source()`
serves slices A–B, and a per-decl re-renderer is slice-D work (§10).

**No pre-section escape hatch** (settled 2026-07-25): the draft's
`LoadWithDecls(bytes, celfn_text)` is DROPPED.  A plugin without
an embedded section is not a `Plugin` — re-embed it (`cel
embed-decls` runs standalone on any existing artifact) or, for
hand-built/pure-WAT test fixtures with top-level exports, use the
legacy `Engine::AddPlugin(bytes, lib)` escape, which keeps
explicit-decls registration alive for exactly that audience.  Two
constructors accepting drifting declaration sources was the bug
class this milestone exists to end; one blessed path keeps it ended.
(The draft C header's `cel_component_load_with_decls` is dropped
with it when slice D is picked up — noted in §10.)

Type-mapping contract (settles cleanup-backlog #44's m35 angle):
`Load` is parse-level validation only — it admits whatever
`ParseCelfnSource` admits, including `type` / `optional<T>` decls the
compile-side `CelfnTypeToCelType` cannot yet map.  The compile side
owns that gap: `Compiler::Build()` must surface a clean
InvalidArgument naming the decl and type (test pinned in slice B2)
rather than crashing.  The engine side needs no mapping, so
`Engine::Use` of such a plugin stays legal.

### 3.2 Compile side: `Use` + the `DeclareFunctions` rename

```cpp
  // Register a Plugin's declarations with the type-checker — the
  // compile-side half of the one-noun flow.  Exactly
  // `DeclareFunctions(plugin.library())`: every declaration
  // becomes callable, and each one the emitted wasm imports is
  // recorded in the Program's cel.abi required-functions table
  // (§5), which Engine::Plan verifies.  Duplicate overload-ids
  // across libraries fail at Build().
  Builder& Use(const Plugin& plugin);
```

`AddLibrary` is renamed **`DeclareFunctions`** (rhymes with
`DeclareVariable`; the axis worth naming is declaration-vs-
implementation, per the 2026-07-22 naming session).  **Clean break,
no deprecated alias** (settled 2026-07-25): every in-repo call site
(`examples/09`, `benchmark/plugin/plugin_bench.cc`,
the demo e2e, `compiler_test.cc`) and every doc page updates in the
same pass (slice B0).  `AddFunction(celfn_source)` remains the
text-based path — a CI compile host that doesn't want plugin bytes
on the build machine uses it; `Use(plugin)` is the artifact-truth
path (the decls provably come from the deployed `.wasm`, and the
compiler sees the plugin hash — the hook future program↔plugin
version pinning hangs off).

### 3.3 Eval side: `Engine::Use`

```cpp
  // Register a Plugin as the sandboxed backend for its
  // declarations — the eval-side half of the one-noun flow.  Wraps
  // the AddPlugin internals and adds:
  //   - a STATIC export check: the interface
  //     (`plugin.wit_interface()`) and every decl's kebab-case
  //     export are resolved against the parsed component — a
  //     missing interface/export fails HERE, FailedPrecondition,
  //     naming it.  No instantiation.
  //   - the plugin's content hash is retained for Plan-time
  //     diagnostics (names which plugin mismatched).
  // Collisions (vs AddFunction/BindFunction and prior plugins)
  // -> AlreadyExists.  NOT thread-safe; startup-only, same contract
  // as the rest of the registration family.
  ABSL_MUST_USE_RESULT absl::Status Use(const Plugin& plugin);
```

The static export check depends on the pinned wasmtime C API
exposing component-level export lookup
(`wasmtime_component_get_export_index` against the parsed
`wasmtime_component_t`; today's Plan path uses the instance-level
variant).  **Probe before freezing** (slice A4,
`compiler/probes/m35/`, disposable per the probe rule): if the pin
has the API, `Use` checks statically; if not, the export check
stays Plan-time-only for slice B (NO throwaway instantiation — the
fallback is the status quo, not a slower variant).  The probe
result gets a dated callout here with the wasmtime header citation.

Legacy `AddPlugin(bytes, lib)` (pre-R: `AddComponent`) stays as the
explicit-decls escape (pure-WAT tests, pre-section artifacts),
behavior unchanged — its Plan-time-only export check is pinned by
`e2e/plugin_dispatch_test.cc
MissingExportFailsAtPlanNotAddPlugin`.  Its header doc-comment
lie (claims registration-time export + FuncType validation; finding
R36 in `doc/design/notes/00-consolidated-findings.md`) is corrected
in slice B1, and R36 marked resolved.

### 3.4 Per-phase validation contract

| Phase | Checks | Failure |
|---|---|---|
| build macro (`cel embed-decls`) | input is a CM component; idl parses; every decl `@plugin.`; no pre-existing `cel.fns` section | build error, `cel embed-decls: <reason>` (+ line/col) |
| `Plugin::Load` | CM preamble; section present, well-framed, unique, UTF-8; decls parse; all `@plugin.`; ≥1 decl | InvalidArgument (missing section → points at `cel embed-decls` / the macro) |
| `Compiler::Builder::Use` | none (Plugin pre-validated) | — |
| `Compiler::Build()` | cross-library duplicate overload-ids; unmappable decl types (backlog #44) | InvalidArgument naming the id / type |
| `Compile()` | call sites type-check against decls; emits `required_functions` from the post-optimize import surface | checker InvalidArgument |
| `Engine::Use` | overload-id collisions; bytes parse; static interface + export existence | AlreadyExists / InvalidArgument / FailedPrecondition |
| `Engine::AddPlugin` (escape) | unchanged: non-empty bytes, collisions, parse | unchanged |
| `Engine::Plan` | existing checks (abi decode, `runtime_abi_version`, link-mode tripwire, bind-time export lookup) + NEW: every `required_functions` row exists in the registry with an exactly matching signature (§5) | FailedPrecondition, messages in §2/§5 |
| `Eval` | residual trap surface only: decl↔WIT type drift (unreachable for macro-built plugins) | trap / eval error |

Deliberately NOT checked (stated honestly): program↔plugin **hash**
agreement — `Plugin::hash()` is exposed for embedder bookkeeping;
enforcement is future work (§11).  WIT-level FuncType of exports —
the wasmtime C API's type introspection is too thin; existing
limitation, unchanged, and unreachable for macro-built plugins
(WIT and decls derive from one `.idl`).

## 4. The `cel.fns` custom section

**One section.**  `cel.fns` carries the `.celfn` declaration text
verbatim (UTF-8, uncompressed), appended at component top level with
raw custom-section framing (id `0x00`, LEB128 payload size, LEB128
name length, name, payload).

> Plan-vs-draft delta (2026-07-25): the 2026-07-22 draft's
> `Plugin::hash()` framing said "(bytes ‖ section text)" — kept.
> A companion `cel.fns.iface` section was considered for the WIT
> interface name and REJECTED: instead, the interface name is made
> always-derivable by **removing the package override** — the
> `package` attr on `cel_wasm_plugin` and the `--package` /
> `--version` flags on `cel generate` are deleted (zero in-repo
> callers; the WIT world/interface are already hardcoded
> `customfn`/`fns`; the only consumer of the name is our own engine
> lookup).  The name is therefore ALWAYS `cel:<module>/fns@0.1.0`,
> derived from the `Module` directive (fallback `customfn`).  A
> hand-built plugin whose WIT disagrees fails loudly at
> `Engine::Use`'s export check ("does not export interface …").

Why the section is appended by a build-time tool and not the
compiler toolchain: the wasm32-wasip2 toolchain emits a
Component-Model component directly, so a linker-emitted custom
section (`__attribute__((section))`) would land inside the *nested
core module*, invisible to a top-level walker; and Binaryen's
`AddCustomSection` cannot produce/edit CM binaries at all.  Raw
top-level framing is the only correct mechanism.

**Reader/writer: `//abi:wasm_binary`** (`abi/wasm_binary.{h,cc}`) —
THE wasm binary-format layer, not a `cel.fns`-only helper.  A
2026-07-25 sweep found the framing knowledge (magic, version words,
LEB128, section walk) already lives in FIVE places, and m35 would
have added four more:

  - `eval/internal/abi_decode.cc:19-106` — the only production
    reader (core-module-only, anonymous namespace, eval-side).
  - `compiler/internal/compile_test.cc:270-325` — a full duplicate
    walker, duplicated *because* the compiler tree can't reach the
    eval-side one.
  - `eval/internal/abi_decode_test.cc:44-71` — a raw *writer*
    (`AppendLeb128U32` + `MakeWasmWithCustomSection`) that exists
    nowhere in production.
  - `compiler/codegen/module_test.cc:32-40` and
    `runtime/cel_runtime_stripped_wasm_bytes_test.cc:42-48` —
    byte-level magic/version assertions.

The consolidation: one module, **absl-only deps** (no Binaryen, no
wasmtime, no proto) so it sits below both `compiler/` and `eval/`
— the property neither existing copy has.  Visibility `//:internal`.

```cpp
// abi/wasm_binary.h — the ONLY first-party code allowed to know
// wasm binary framing.  A magic constant or LEB decoder anywhere
// else is a review finding (feature-pipeline-checklist §2.7).

enum class WasmLayer { kCoreModule, kComponent };

// Preamble classification: \0asm + version word 0x00000001 (core
// module) vs version/layer word 0x0001000d (CM component).
std::optional<WasmLayer> ClassifyWasmBinary(absl::Span<const uint8_t>);
bool IsCoreModule(absl::Span<const uint8_t> bytes);
bool IsComponentBinary(absl::Span<const uint8_t> bytes);

// LEB128 (unsigned 32-bit) read/append — shared by walk and build.
bool ReadLeb128U32(absl::Span<const uint8_t>, size_t* pos, uint32_t* out);
void AppendLeb128U32(std::vector<uint8_t>& out, uint32_t value);

// Find the top-level custom section named `name`.  Works on BOTH
// layers (the id/size framing is identical; custom sections keep
// id 0x00 at component level); never recurses into nested
// core-module (id 1) / component (id 4) payloads.  OK -> zero-copy
// span into the input; NotFound; InvalidArgument (bad preamble /
// framing overrun / duplicate name).
absl::StatusOr<absl::Span<const uint8_t>> FindCustomSection(
    absl::Span<const uint8_t> wasm_bytes, absl::string_view name);

// Return a copy of `wasm_bytes` with a custom section appended at
// top level.  InvalidArgument on bad preamble or existing `name`.
absl::StatusOr<std::vector<uint8_t>> AppendCustomSection(
    absl::Span<const uint8_t> wasm_bytes, absl::string_view name,
    absl::Span<const uint8_t> payload);

// Test/build helper: frame {name, payload} as custom-section bytes.
std::vector<uint8_t> BuildCustomSection(absl::string_view name,
                                        absl::Span<const uint8_t> payload);
```

Migration (slice A1, before `Plugin::Load` lands on top):
`abi_decode.cc` refactors onto the shared walker — behavior-neutral,
its existing tests pin that; only the proto-decode half (which
round-trips `ir::Repr`) stays eval-side until the `Repr` relocation.
`compile_test.cc`'s duplicate walker and `abi_decode_test.cc`'s
hand-builder are DELETED in favor of the module.  The magic asserts
in `module_test.cc` / the stripped-bytes test may switch to
`IsCoreModule` opportunistically.

What deliberately does NOT move here: Binaryen-mediated module
mutation (`WasmModule::AddCustomSection`/`Serialize`,
`strip_command_wrappers` — semantic rewrites belong to Binaryen)
and wasmtime import introspection
(`eval/internal/module_imports.cc` operates on a parsed
`wasmtime_module_t`, not bytes).  The layering line: **byte-level
framing → `//abi:wasm_binary`; module semantics → Binaryen;
runtime linking → wasmtime.**

**Writer tool: `cel embed-decls`** (`tools/cel/run_embed_decls.{h,cc}`,
dispatched from the `cel` driver like `run_generate`):

```
cel embed-decls --plugin=<in.wasm> --idl=<file.idl> --out=<out.wasm>
```

Reads the plugin binary; validates (CM preamble; idl parses; **all
decls `@plugin.`** — the build-time reject; no existing `cel.fns`
section); appends the verbatim idl bytes; writes.  Deterministic
(pure function of its inputs).  Not wasm-tools: the step needs
celfn-aware validation with proper messages, which only a
first-party tool linking `ParseCelfnSource` + `//abi:wasm_binary`
gives hermetically.

**Macro change:** `bazel/cel_wasm_plugin.bzl` step 4
(`_rename_to_dot_wasm`, today a plain `cp` genrule) becomes
`_embed_decls` invoking the tool (`tools = ["//tools/cel:cel"]`,
`srcs = [core, idl]` — the idl label is already in hand).  The
macro's `package` attr is removed in the same slice (A0).

## 5. Plan-time required-function verification (slice V)

The user's headline requirement: at Plan, verify that every remote
function the program calls exists with an agreeing signature —
existence + full signature match, hash NOT enforced.

### 5.1 Wire: `cel.abi` field 8 (additive, no version bump)

```proto
// Recursive type spelling for a custom-function signature.  Mirrors
// compiler/celfn CelfnType 1:1; open-set on wire (unknown kinds
// compare numerically, never rejected at decode).
message FnType {
  enum Kind {
    FN_KIND_UNSPECIFIED = 0;
    FN_KIND_BOOL = 1;   FN_KIND_INT = 2;    FN_KIND_UINT = 3;
    FN_KIND_DOUBLE = 4; FN_KIND_STRING = 5; FN_KIND_BYTES = 6;
    FN_KIND_DURATION = 7; FN_KIND_TIMESTAMP = 8;
    FN_KIND_PROTO = 9;   // fqn in proto_fqn
    FN_KIND_LIST = 10;   // params = [elem]
    FN_KIND_MAP = 11;    // params = [key, value]
    FN_KIND_TYPE = 12;
    FN_KIND_OPTIONAL = 13;  // params = [elem]
  }
  Kind kind = 1;
  string proto_fqn = 2;
  repeated FnType params = 3;
}

// One custom function the Program's wasm imports (module `cel_fn`)
// — i.e. one function instantiation WILL demand.  Engine::Plan
// verifies each row against its registry before any wasmtime
// linking, and instantiates only the plugins the PLUGIN rows
// name.  Programs emitted before this field carry an empty list:
// the check no-ops and Plan behaves as before the field existed
// (incl. instantiate-all, §6.4).
message RequiredFunction {
  enum Backend { BACKEND_UNSPECIFIED = 0; HOST = 1; PLUGIN = 2; }
  string overload_id = 1;    // == the cel_fn import base name
  string fn_name = 2;        // source-level name, for messages
  Backend backend = 3;
  repeated FnType param_types = 4;  // excludes out_slot; wasm arity = size+1
  FnType return_type = 5;
  bool is_receiver = 6;
}

// in CelAbi:
  repeated RequiredFunction required_functions = 8;
```

`@host.` rows ARE included (settled 2026-07-25): the wire cost is
trivial, and it converts the worst current failure — an opaque
`unknown import cel_fn.<id>` wasmtime link error when a host
registration was forgotten — into the same clean FailedPrecondition.
`@native.` (`kCelDefined`) decls are excluded naturally: their
imports are per-module aliases, not `cel_fn`.

### 5.2 Emission — from the post-optimize import surface

The table must equal the **final wasm's `cel_fn` import list**, not
the AST or the registered libraries: overload imports are installed
unconditionally for every declared custom fn, and Binaryen
`Optimize` at O1+ drops unused ones — while wasmtime demands every
*surviving* import at link time regardless of reachability.
Deriving from anything but the final module desyncs per optimize
level.

Consequence: `cel.abi` attachment moves from before to after the
optimize pass — `FinaliseModule` splits into `ValidateAndOptimize` +
`SerializeModule`, and `LowerExportAndFinalise` becomes lower →
export → validate+optimize → attach → serialize.  Behavior-neutral
for every existing field (nothing else in the section depends on
optimization); existing emit/decode round-trip tests pin that.

New pieces: `WasmModule::ListFunctionImports()` (read-only Binaryen
accessor), `abi/celfn_wire.{h,cc}` (`FnTypeFromCelfn`,
`FnTypeEquals` — recursive, fqn-sensitive — and `RenderSignature`,
shared by emit tests and Plan messages so both render identically),
and `BuildRequiredFunctions(imports, libraries)` in `cel_abi_emit`
(a `cel_fn` import with no matching decl across the libraries is an
`ABSL_CHECK` invariant — the import came from those libraries).

### 5.3 The check — `//eval/internal:required_fn_check`

Called from `Engine::Plan` after `DecodeCelAbiFromWasm` + the
`runtime_abi_version` check, BEFORE `BindRegisteredExtensions`; new
TU keeps `engine.cc` inside the function-size gate.

Per row, first failure wins (wire order, deterministic):

  - **HOST** → `host_callbacks[overload_id]`.  Missing → fail.
    Arity (`param_types.size() + 1 != num_args`) → fail.  Full
    recursive type compare when the registration came through
    `BindFunction` (which parses a decl; `RegisteredHostCallback`
    gains optional typed fields it populates).  Raw `AddFunction` /
    `AddTypedFunction` registrations stay arity-only — documented on
    both methods.
  - **PLUGIN** → scan the plugin registry for the `kPlugin` decl
    with this overload-id.  Missing → fail.
    Compare `is_receiver`, param count, each param type, return
    type via `FnTypeEquals` (protos by FQN).  Any difference → fail.

Message shapes are frozen in §2's quickstart (missing-plugin,
signature-mismatch) plus:

```
Engine::Plan: program requires host function `discount_pct_string`
(`int discount_pct(string)`) but none is registered; call
Engine::BindFunction (or AddFunction) before Plan

Engine::Plan: program requires host function `discount_pct_string`
with wasm arity 2 but it was registered with arity 3
```

Thread-safety is unchanged: the check reads registration-time-frozen
state; `Plan` stays concurrent-safe.

## 6. Sharing model — one plugin, many expressions

> Like §2, this section is user-guide material: slice S3 lifts it
> (including the counter example below) into
> `writing-plugins.md` — embedders hit the per-Instance
> state question fast.

What is shared and what is not (behavior today, kept by this
milestone, except §6.4):

  - **Shared per engine:** the parsed `wasmtime_component_t`
    (compiled once at `Use`) and the declarations.  One registration
    serves every Program the engine ever plans.  A `Plugin` value
    itself is immutable — registerable on many compilers/engines
    from many threads.
  - **Fresh per Plan:** the plugin *instance*.  Each Plan
    instantiates into its own store with its own linear memory —
    Instances never see each other's plugin state.
  - **Persistent per Instance:** the plugin instance lives as
    long as the Instance, so state a plugin fn keeps in its
    linear memory (a cache, a counter) survives across `Eval` calls
    on the SAME Instance and resets on a fresh Plan.  Write
    plugin fns as pure functions unless per-Instance memoization
    is the intent.
  - **Registration is startup-only** (not thread-safe, like the
    rest of the family); `Plan` is concurrent-safe.

### 6.1 The lifecycle, made observable

Consider a plugin function with internal state, declared and
implemented in a plugin:

```c
// counter.idl
Module counter;

int @plugin.invocation_id();
```

```cpp
// counter_fns.cc — the author's implementation
namespace counter {
int InvocationId() {
  static int i = 0;   // lives in the PLUGIN INSTANCE's linear memory
  return i++;
}
}  // namespace counter
```

The embedder compiles a CEL expression that calls it — the call site
type-checks against the plugin's declaration like any other function:

```cpp
auto counter_plugin = Plugin::Load(counter_bytes).value();

auto compiler = Compiler::NewBuilder()
                    .Use(counter_plugin)
                    .Build().value();
auto program = compiler.Compile("invocation_id()").value();
// `program` is wasm that calls the plugin fn on every Eval.
```

Now run that ONE Program several ways and watch where the counter
state actually lives:

```cpp
auto engine = Engine::NewBuilder().Build().value();
CHECK_OK(engine.Use(counter_plugin));      // registered; compiled once, run never

auto a = engine.Plan(program).value();     // plugin instance A created
auto b = engine.Plan(program).value();     // plugin instance B created

Activation act;                            // no variables in this expr
a.Eval(act);   // -> 0     A's counter: i was 0, now 1
a.Eval(act);   // -> 1     same Instance, same linear memory — persists
b.Eval(act);   // -> 0     B is the SAME program on the SAME engine,
               //          but its own sandbox — not 2
a.Eval(act);   // -> 2     A unaffected by B

auto a2 = engine.Plan(program).value();    // re-plan the same program
a2.Eval(act);  // -> 0     a fresh Plan is a fresh sandbox — reset
```

The rules this pins:

  - `static` / global state in a plugin fn is **per-Instance**,
    not per-engine and not per-process.  Two Instances of the same
    Program on the same engine each start from zero.
  - The same holds for heap allocations, lazily-built caches, and
    library init inside the plugin — each Instance pays its own
    init and keeps its own copy.
  - A re-plan is a state reset.  Anything a deployment does that
    re-plans (rollout, config reload, slice-C `Swap` adoption)
    silently zeroes plugin-internal state — never park state a
    correctness property depends on inside a plugin.
  - Corollary for CEL semantics: an expression calling such a
    function is not referentially transparent across Evals.  That is
    the embedder's choice to make, but the intended model is pure
    functions; treat in-plugin state as an optimization
    (memoization) whose loss is always safe.

### 6.4 Selective instantiation (slice V4)

Today `InstantiateAndBindPlugins` (pre-R:
`InstantiateAndBindComponents`) loops over ALL registered
plugins unconditionally.  With §5's verification done, Plan knows
exactly which plugins the Program needs: instantiate only the
registered plugins owning at least one required PLUGIN row
(overload-id intersection; the per-plugin bind loop is unchanged
for selected plugins).  Register 10 plugins → a Program calling
one instantiates one; a Program calling none instantiates zero.

Legacy fallback: a Program with no `required_functions` (pre-field-8
compiler) keeps instantiate-all — the engine cannot know what it
needs, and compat holds.

The behavioral pin (not just a perf claim): register two plugins,
one broken at instantiation; a new-format Program calling only the
healthy one must Plan+Eval green (fails today), a legacy-format
Program must keep failing, and a Program calling neither must
instantiate zero.

## 7. Multi-plugin reality

The engine supports N registered plugins — each Plan instantiates
its *required* subset (§6.4), each instance with its own linear
memory.  The flat namespace is the constraint: overload-id
collisions across plugins/host fns → AlreadyExists at
registration.  Namespacing (register-under-alias so call sites say
`fraud.score()` vs `partner.score()`) is a compile-side language
decision — out of scope, §11.

## 8. `Swap` contract (deferred — slice C)

Contract as settled 2026-07-22, unchanged; kept for when the slice
is picked up:

  - **Matched by declaration set.**  The replacement's parsed decls
    must equal some registered plugin's decls
    function-for-function.  NotFound if nothing matches;
    FailedPrecondition if a same-named set differs in any signature
    — a signature change invalidates compiled call sites, so it is a
    compile-side event, never a silent runtime substitution.
  - **Validated at swap, never at eval** — a botched upload leaves
    v1 serving.
  - **Takes effect at the next `Plan`.**  Existing Instances keep
    the old version; rollback is `Swap` with the old bytes.

## 9. Lifetime & concurrency (the shared_ptr question)

Answer to "what if something is using it": extend the pattern the
codebase already uses (Instance pins per-Plan plugin state via
`std::shared_ptr<void> InstanceImpl::plugin_fn_envs`):

  - Engine registry: `std::shared_ptr<const RegisteredPlugin>`
    per plugin — an immutable snapshot (plugin, decls, hash).
    This slice adds the `hash` field to `RegisteredPlugin`
    (all-zero = legacy `AddPlugin` path); the shared_ptr
    snapshotting itself is slice-C work, since only `Swap` mutates
    the registry concurrently with `Plan`.
  - `Plan` copies the shared_ptr into the InstanceImpl; `Swap`
    replaces the registry pointer; old snapshots free when the last
    pinning Instance re-plans or dies.
  - `Swap`-vs-`Plan` concurrency needs a mutex or
    `std::atomic<std::shared_ptr>` on the registry read — slice C.

## 10. C ABI (deferred — slice D)

The draft C surface lives at `bindings/c/cel_component.h` (pure-data
plugin path — the one custom-function mechanism that crosses
every FFI and a future wasm-compiled embedder); it is renamed
`bindings/c/cel_plugin.h` (types `cel_plugin_*`) when the slice is
picked up.  Known gaps to resolve then: the C-API groundwork it
depends on has no design doc (its former "m34" number was skipped
2026-07-25 — renumber when picked up), `bindings/` has no build
graph, `cel_common.h` / `cel_status` don't exist, the header's own
usage example at line 19 contradicts its declaration at line 129
(`cel_engine_builder_use_component` vs `cel_engine_use_component` —
the declaration is authoritative), `cel_component_load_with_decls`
must be DELETED from the header (the C++ `LoadWithDecls` it mirrored
was dropped — §3.1; the load fn is the only constructor),
and the per-decl introspection fns need source re-rendering /
doc-comment capture that `CelfnDecl` doesn't store today.

## 11. Out of scope / future work

  - Plugin namespacing / aliasing (call-site `fraud.score()`).
  - `Plan`-time program↔plugin hash verification (hash is exposed
    now; enforcement policy is an embedder conversation).
  - CPU-time limits for plugin calls (the
    max-memory setter in the draft C header is the ABI slot such
    knobs land in).
  - Move the `cel.abi` PROTO-decode half of
    `eval/internal/abi_decode.cc` to `abi/` (the raw section walker
    moves in slice A1 via `//abi:wasm_binary`; the proto decode
    round-trips `ir::Repr` and rides on the `Repr` relocation).
  - Per-decl source re-render + doc-comment capture for slice D
    introspection.
  - `Swap` (slice C) and the C ABI (slice D) as drafted above.

## 12. Slices

  0. **This doc** + `feature-pipeline-checklist.md` §2.7 ("new
     custom section on a plugin binary").
  R. **Repo-wide rename** (Component → Plugin, full family; lands
     FIRST so A–N build under the final names; §1.1 has the mapping
     and the keeps-component boundary):
     R1 celfn language — `@component.` → `@plugin.` in the grammar
     + `kForeignComponent` → `kPlugin` + every `.celfn`/`.idl`
     fixture and test expectation;
     R2 build macro — `bazel/cel_wasm_plugin.bzl` rename + all
     callers (examples, e2e fixtures, benchmark, `celfnc_emit`);
     R3 eval surface — `AddComponent` → `AddPlugin`,
     `RegisteredComponent`/`component_libraries` →
     `RegisteredPlugin`/plugin registry,
     `InstantiateAndBindComponents` → `InstantiateAndBindPlugins`,
     `component_fn_envs` → `plugin_fn_envs`,
     `cel_component.{h,cc}` → `cel_plugin.{h,cc}` (wasmtime
     `wasmtime_component_*` identifiers keep their names);
     R4 test/dir renames — examples/09, `e2e/plugin_dispatch_test`,
     `e2e/plugin_fixtures/`, `benchmark/plugin/`;
     R5 doc-site mechanical terminology sweep ("component
     function" → "plugin function", `writing-component-functions.md`
     → `writing-plugins.md` + link fixes; `doc/design/notes/**`
     frozen; deep rewrites are slice S).
     Behavior-neutral: full `$PROJ` green + conformance count
     unchanged is the gate; a residual-grep review
     (`grep -ri component` over first-party code, expecting only
     the §1.1 keeps list) closes the slice.
  A. Self-describing artifact: A0 package-override removal (macro
     attr + `cel generate` flags; zero callers, no migration);
     A1 `//abi/internal:sha256` + `//abi:wasm_binary` (the
     consolidated binary-format layer, §4) INCLUDING the migration —
     `abi_decode.cc` refactored onto the shared walker,
     `compile_test.cc`'s duplicate walker and `abi_decode_test.cc`'s
     hand-builder deleted;
     A2 `cel embed-decls`; A3 macro step-4 swap + demo-e2e
     integration pin (macro output carries the section;
     `Plugin::Load` round-trips it); A4 wasmtime component-level
     export-lookup probe + `//abi:plugin`.
  V. Plan-time verification: V1 `cel.abi` field 8 +
     `//abi:celfn_wire`; V2 emission (post-optimize restructure +
     `ListFunctionImports` + `BuildRequiredFunctions`); V3 the check
     (`required_fn_check` + `BindFunction` typed capture + e2e
     negatives + two-plugins-one-Plan positive); V4 selective
     instantiation.
  B. Surface: B0 `AddLibrary` → `DeclareFunctions` rename (clean, no
     alias); B1 `Engine::Use` (+R36 doc fix); B2
     `Compiler::Builder::Use` (+backlog #44 hardening, #43
     reconciliation — Use→Plan is the public negative-path seam #43
     asked for); B3 examples/09 rewrite onto `Plugin::Load` + `Use`;
     B4 closeout (testing-checklist rows, manual-tagged runs).
  S. **Doc site** (the slice the 07-22 draft under-scoped to one
     page; ordered by traffic per the docs-loop rule — high-read
     pages first; `doc/design/notes/**` stay frozen):
     S1 `README.md` + `doc/user-guide/getting-started.md` +
     `doc/user-guide/index.md` — the plugin story at a glance
     (load → Use → Plan verifies → sandboxed call);
     S2 `doc/user-guide/custom-functions.md` — the chooser page
     (host functions vs wasm plugins), rewritten onto `Use`;
     S3 `doc/user-guide/writing-plugins.md` — full rewrite seeded
     from §2 (quickstart) + §6 (sharing model + counter example);
     THE guide for wasm plugins;
     S4 `doc/user-guide/security-model.md` (sandbox story +
     selective instantiation) + `doc/user-guide/faq.md`;
     S5 design pages — `doc/design/05-custom-functions.md`,
     `02-evaluator.md`, `00-architecture.md`,
     `08-abi-wire-format.md` (gains `cel.fns` framing + `cel.abi`
     field 8), trust-boundary diagrams regenerated via
     `doc/design/diagrams/render.py`.
  N. Benchmarks (production config: `-c opt`,
     `optimize_level = 2`):
     compile side — `BM_PluginLoad`, compile-with-plugin-decls
     vs no-library baseline, required-functions emission overhead;
     eval side — `BM_PlanScalingByRegisteredPlugins/N` (N ∈
     {1,4,16}, program calls one; the §6.4 before/after headline),
     `BM_PlanVerificationOverhead`, `BM_EngineUse`.  Published per
     the benchmark README discipline; the V4 before/after table
     lands here at closeout.
  C. `Swap` (§8) — deferred.
  D. C ABI (§10) — deferred.

Per-slice test matrices follow `feature-pipeline-checklist.md`
(§2.6 for the ABI field, new §2.7 for the section; boundary tests:
empty/missing/duplicate/truncated-LEB/non-UTF8 section; the
signature-agreement matrix: missing fn, arity, param type, proto
FQN, nested generic, return type, is_receiver; hash
stability/divergence; two plugins one Plan;
selective-instantiation pins).

Execution DAG for this milestone (which slices parallelize, agent
assignments, live status): `m35-dag.md`.
