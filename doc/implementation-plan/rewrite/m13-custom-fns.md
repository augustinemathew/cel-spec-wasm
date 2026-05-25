# M13 — User-defined custom functions

Status: **plan — drafted 2026-05-21, not yet started.**  Supersedes
the stale `m-custom-fns.md` (drafted under the old M6 numbering,
pre-rewrite; never closed out).

## 0. TL;DR

Let users declare custom functions in a small `.celfn` IDL.  The
file produces a single wasm module (named by `Module foo;`) plus a
set of declarations the compiler consumes.  Each declaration is
C-style (`<return-type> <name>(<params>)`); the three backend
flavours are distinguished by **the shape of the declaration**,
not by an enclosing block keyword:

  - **Host-backed** carries an `@host.` prefix on the name and has
    no body.
  - **Foreign-wasm-backed** carries an `<alias>.` prefix (no `@`)
    on the name and has no body.  The alias is **implicit by
    use** — there is no separate declaration directive; the set
    of foreign aliases in a file is the set of distinct
    first-identifiers across all foreign decls.  The embedder
    supplies a pre-instantiated wasm module under each alias at
    Plan time via `RuntimeBindings::AddModule(<alias>, <instance>)`.
  - **CEL-defined** has a body (`= <CEL expression>;`) and no
    prefix; lives in the wasm module named by `Module foo;`.

```celfn
// fns.celfn

Module foo;                                  // CEL-defined fns compile
                                             // into wasm module `foo`.

// CEL-defined: body is a pure CEL expression.  celwasmc compiles
// the body into the wasm module named by `Module foo;`.
bool is_number(this string s) = s.matches("^[0-9]+$");

string greet(this proto(acme.User) u) =
  cel.bind(name, u.first + " " + u.last, "hi, " + name);

// Host-backed: embedder C++ provides the impl, bound at Plan time.
// Each declaration carries its own @host. prefix.
string @host.upper(this string s);
bool   @host.is_admin(proto(acme.User) user);

// Foreign module: any wasm-producing toolchain (TinyGo, Rust,
// AssemblyScript, …) compiles the impl; bound at Plan time.
// `rules` is the alias — implicit by use.  The embedder must
// RuntimeBindings::AddModule("rules", <instance>) before Plan.
bool   rules.allow(this proto(acme.User) u, string resource);
bool   rules.deny(this proto(acme.User) u, string resource);
```

Three backends, **one** calling convention at the wasm boundary —
every custom fn is a `(call $helper (out_slot, *arg_slots))` import
over the 24-byte CelValue ABI, regardless of whether its body lives
in C++, foreign wasm, or compiled-CEL wasm.  Dispatch routes through
the existing `OverloadTable` (extended); cel-cpp's checker resolves
the call site to the right overload-id; codegen emits the import in
the right wasm namespace.

The forcing function for the foreign-module backend: **a Go
function compiled to wasm via TinyGo, called from a CEL expression
through the C++ engine, end-to-end.**  Everything in §4.3 and §8
below is shaped by that goal.

This doc covers the IDL, the three-backend ABI, the C++ API, the
dispatch design (with pros / cons of two routing options), the
cel-cpp checker hookup, the codegen plan, the WAT-first prototypes,
the cross-language toolchain (`celfnc`), and the testing matrix.

## 1. Scope

In scope:

  - `.celfn` IDL parser (grammar §3) — including `Module foo;`
    directive, the three declaration shapes (CEL-defined body /
    `@host.<name>` / `<alias>.<name>`), the `proto(<fqn>)` type
    form, and the receiver-style `this T arg0` parameter syntax.
  - The closed type catalog (§3.6) custom fns may take or return.
  - CEL-defined function bodies (pure CEL expression form — §3.5).
    Bodies parse + type-check + lower through the existing pipeline;
    the lowered wasm function is exported from the file's
    `Module foo;` wasm module.
  - `--functions=<path>.celfn` CLI flag → `Compiler::Builder::AddLibrary`
    calls (§6).
  - Codegen for all three backends — host, foreign-module,
    CEL-defined.
  - Receiver-style (`this T arg0`) and free-function declarations.
  - 3VL plumbing: a custom fn's args are absorbed (UNKNOWN / ERROR
    short-circuit before the call) and the fn can return UNKNOWN /
    ERROR via `Value::Unknown` / `Value::Error`.
  - `cel.abi.functions.host_custom_imports` extended with `backend`
    enum (host / foreign / cel_defined) + module name + module path.
  - **`celfnc` stub generator** for cross-language modules — targets
    Go (TinyGo), Rust, AssemblyScript.  Generates the
    `(out_slot, *arg_slots)` exports + CelValue decode/encode
    boilerplate so the user writes typed code in their language.
    Go is the forcing-function target; Rust/AS land alongside.
  - C public ABI header (`include/cel/cel_value.h`) — stable layout
    of the 24-byte CelValue, the `arena_alloc` import contract, and
    the shared-memory binding.  This is the cross-language
    interop surface.
  - `bazel test //compiler_v2/...` green + per-component coverage doc
    updated (positive + negative + boundary for each declared kind).

Out of scope (deferred to follow-up slices):

  - Macros — the spec reserves macros and forbids user-defined ones.
  - Procedural body syntax (`{ let x = ...; return x; throw … }`).
    The IDL ships with pure-CEL-expression bodies only (§3.5);
    procedural bodies are a real language design exercise we defer
    until pure-expression bodies prove insufficient.
  - Generics on custom-fn signatures (e.g. `T pick(this list<T>, int i)`).
    cel-cpp's `FunctionDecl` supports them, but the IDL grammar in §3
    declines them in v1 to keep the parser tiny.
  - `dyn` as an argument or return type.  Consistent with
    `RejectDyn`; custom fns cannot reintroduce dynamic typing.
  - `proto(google.protobuf.Any)` as an argument or return type.
    Technically representable but defeats static typing; behind a
    feature flag for v2.
  - `type<T>` / first-class types in custom-fn signatures.
  - Custom fns calling other custom fns within the same compile unit
    — falls out of the design but no dedicated test target in v1.
  - Component Model wasm modules — shared-memory model only in v1.
    Component Model revisit deferred until the cross-language
    toolchain story stabilizes (TinyGo / AssemblyScript component
    support is partial today).

## 2. Architecture overview

```
.celfn file
   │  (parser §3)
   ▼
FunctionLibrary (in-memory rep)
   ├─ declarations: FunctionDecl[]
   ├─ backends:     {host | foreign(alias) | cel_defined(module_name)}[]
   └─ cel_bodies:   optional compiled wasm module (the `Module foo;`
                    wasm produced from CEL-expression bodies)
   │
   ├──► Compiler::Builder::AddLibrary(lib)
   │      │
   │      ├──► For every decl: cel-cpp TypeCheckerBuilder::AddFunction
   │      │      → checker resolves kCallExpr → overload_id
   │      │      → ResolvePass stamps NodeAnnotation.overload_id
   │      │
   │      └──► OverloadTableBuilder::RegisterCustom(
   │             overload_id, ImportModule{kind, module_name}, helper_name)
   │             → codegen emits `(call $helper)` at use sites
   │             → InstallOverloadImports emits one wasm import per
   │               unique (module_name, helper_name) — namespace
   │               selected per-backend
   │
   └──► At Engine::Plan time, RuntimeBindings:
          - host backend:        AddFunction(overload_id, C++ impl)
          - foreign backend:     AddModule(name, pre-instantiated wasm)
          - cel_defined backend: AddModule(name, instance of cel_bodies)

The three backends differ ONLY in WHERE the wasm export lives:
  - host:        wasmtime callback into C++ adapter
  - foreign:     user-authored .wasm exporting the helper
  - cel_defined: celwasmc-authored .wasm exporting the helper
At the call site in the expression's wasm, all three look identical:
  (call $helper (i32.const out_slot) (i32.const arg0_slot) …)
```

Four components are new; everything else is reuse:

  1. **`compiler/celfn/`** — `.celfn` parser + `FunctionLibrary`
     in-memory rep + `FunctionDecl` builder.
  2. **`OverloadTable` extension** — `ImportModule` becomes a tagged
     `(kind, module_name)` value (§5.3) so the table can name
     arbitrary user-declared module imports, not just `cel` / `cel_host`.
  3. **CEL-body sub-compiler** — for every CEL-defined fn in the
     `.celfn` file, run the existing frontend → IR → codegen pipeline
     against the body expression with the receiver + params bound as
     variables; collect every fn's wasm into a single `Module foo;`
     output wasm.
  4. **`celfnc` cross-language generator** — emits Go/Rust/AS
     boilerplate from the IDL so users in those languages write
     typed code without seeing byte offsets.

## 2.1 C++ API surface

The embedder touches one new builder method (`AddLibrary`) and one
new bindings method (`AddModule`).  Everything else is the existing
[cel-host-surface.md §2](cel-host-surface.md) shape.

```cpp
// 1) Build a FunctionLibrary via its Builder (programmatic only —
//    no FromCelfnFile static factory; embedders that have a .celfn
//    file on disk read it themselves and call ParseCelfnSource).
//    The Library is a pure-data artifact; reusable across many
//    Compile() calls.
auto lib_or = FunctionLibrary::Builder()
    .SetModuleName("foo")                         // for CEL-defined fns
    .AddHost("upper", StringType, {{true, StringType, "s"}})
    .AddForeign("rules", "allow", BoolType,
                {{true, StringType, "u"}, {false, StringType, "r"}})
    .AddCelDefined("isAdult", BoolType,
                   {{true, ProtoType("acme.User"), "u"}},
                   /*body=*/"u.age >= 18")
    .Build();
ASSIGN_OR_RETURN(auto lib, std::move(lib_or));

// 1b) OR: load from a .celfn source string (used by celwasmc CLI).
ASSIGN_OR_RETURN(auto lib_from_src,
                 ParseCelfnSource(ReadFileOrDie("fns.celfn")));

// 2) Plug the library into the Compiler.  All declarations become
//    visible to cel-cpp's checker; the call-site resolution
//    (`x.is_number()`, `upper(s)`, etc.) just works.
ASSIGN_OR_RETURN(auto compiler, Compiler::Builder()
    .AddStandardDeclarations()
    .AddLibrary(lib)
    .Build());

ASSIGN_OR_RETURN(auto program, compiler.Compile("name.is_number()"));

// 3) Engine owns the runtime configuration.  AddModule + AddFunction
//    register state on the engine; conflicts (duplicate alias,
//    duplicate overload-id) error at registration time.  No separate
//    `RuntimeBindings` object — see Probe 4.
ASSIGN_OR_RETURN(auto engine, Engine::Create());

// 3a) Register foreign modules under their aliases.  Engine
//     instantiates, calls `_initialize` if exported (§4.5.2 point 4).
ASSERT_OK(engine.AddModule("rules", rules_wasm_bytes));

// 3b) Register CEL-defined-fns module (compiled into the library).
ASSERT_OK(engine.AddLibraryModule(lib));  // alias = lib.module_name()

// 3c) Register host-fn impls by overload-id.
ASSERT_OK(engine.AddFunction("upper_string", upper_impl));

// 4) Plan resolves the program's wasm imports against the engine's
//    registered modules + host fns; instantiates; returns Instance.
ASSIGN_OR_RETURN(auto instance, engine.Plan(program));

// 5) Evaluate.
ASSIGN_OR_RETURN(auto value, instance.Eval(activation));
```

Backends are uniform at the binding site — `engine.AddModule(name, bytes)`
is the **single attach-point for any wasm-backed custom fn**, whether
the user compiled it from Rust, TinyGo, AssemblyScript, or whether
celwasmc produced it from CEL bodies.  The engine can't tell them
apart; that's intentional.

Surface additions to `cel-host-surface.md`:

```cpp
// compiler/celfn/function_library.h  (new — landed in Slice B)
class FunctionLibrary {
 public:
  FunctionLibrary() = default;
  // Copyable + movable.

  // Read accessors — what celwasmc + Compiler::Builder need.
  const std::string& module_name() const;       // empty if no Module directive
  const std::vector<CelfnDecl>& decls() const;  // backend, fn_name, overload_id, …
  const std::vector<std::string>& foreign_aliases() const;  // first-use order

  class Builder {
   public:
    Builder& SetModuleName(absl::string_view module_name);
    Builder& AddHost(absl::string_view fn_name, CelfnType return_type,
                     std::vector<CelfnParam> params);
    Builder& AddForeign(absl::string_view alias, absl::string_view fn_name,
                        CelfnType return_type,
                        std::vector<CelfnParam> params);
    Builder& AddCelDefined(absl::string_view fn_name,
                           CelfnType return_type,
                           std::vector<CelfnParam> params,
                           absl::string_view body);
    absl::StatusOr<FunctionLibrary> Build();
  };
};

// Free function — used by the CLI to load a .celfn file's bytes
// into a Library.  Embedders that don't have a file should use the
// Builder directly.
absl::StatusOr<FunctionLibrary> ParseCelfnSource(absl::string_view source);

// eval/runtime_bindings.h  (extension)
class RuntimeBindings {
 public:
  // … existing surface from cel-host-surface.md §2.4 …

  // Bind a pre-instantiated wasm module to a name.  Lookup key is
  // the module_name declared in the .celfn file:
  //   foreign     → alias from `<alias>.<fnname>` decls
  //   cel_defined → name from `Module <name>;`
  // The embedder owns where the wasm bytes came from; this API
  // only cares about the name.
  RuntimeBindings& AddModule(
      absl::string_view module_name,
      wasmtime::Instance instance);
};

// eval/engine.h  (extension)
class Engine {
 public:
  // … existing surface …

  // Load + instantiate an arbitrary wasm file from disk.  The
  // module must import `cel.memory` and (optionally) `cel.arena_alloc`
  // from the engine; both are resolved automatically.  Fails if the
  // module's imports don't match the published ABI version
  // (cel.abi.toolchain_abi_version).
  absl::StatusOr<wasmtime::Module> LoadModule(absl::string_view path);
  absl::StatusOr<wasmtime::Instance> Instantiate(
      const wasmtime::Module& mod);

  // Instantiate the wasm module embedded in a FunctionLibrary.
  // Returns Unimplemented if `lib.module_bytes()` is empty (i.e. the
  // library has no CEL-defined functions).
  absl::StatusOr<wasmtime::Instance> InstantiateLibraryModule(
      const FunctionLibrary& lib);
};
```

The `wasmtime::Module` / `wasmtime::Instance` types are the
existing wasmtime-cpp wrappers (already used inside the engine);
exposing them at the public surface is the minimum-additional-API
move.  A future slice may wrap them in an opaque `cel::Module` /
`cel::ForeignInstance` to hide the wasmtime dependency from public
headers; that's polish, not architecture.

## 3. `.celfn` IDL

### 3.1 Lexical structure

  - Line comments: `//` to end-of-line.  Block comments: `/* … */`.
  - Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
  - String literals: double-quoted, no escapes in v1 — used only
    for module paths.
  - Whitespace insignificant outside literals.
  - Statements end with `;`.
  - Keywords (reserved, can't be used as identifiers): `Module`,
    `host`, `this`, `proto`, `bool`, `int`, `uint`, `double`,
    `string`, `bytes`, `null`, `Duration`, `Timestamp`, `list`,
    `map`.

### 3.2 Grammar (EBNF)

```ebnf
file        = [ module-directive ] { file-item } ;

module-directive
            = "Module" identifier ";" ;

file-item   = host-fn-decl
            | foreign-fn-decl
            | cel-fn-def ;

(* Host-backed: C-style declaration with `@host.` prefix on the
   function name; no body.  `host` is a reserved alias. *)
host-fn-decl
            = type "@" "host" "." identifier
              "(" [ params ] ")" ";" ;

(* Foreign-wasm-module-backed: C-style declaration with
   `<alias>.` prefix on the function name; no body, no `@`.
   Aliases are implicit by use — the parser collects the set of
   distinct first-identifiers across foreign decls and surfaces
   it as the file's foreign-alias set. *)
foreign-fn-decl
            = type identifier "." identifier
              "(" [ params ] ")" ";" ;

(* CEL-defined: C-style declaration with an `= <cel-expr>` body.
   No prefix, no qualifier; lives in the file's `Module foo;`
   output wasm. *)
cel-fn-def  = type identifier "(" [ params ] ")" "=" cel-expr ";" ;

params      = param { "," param } ;
param       = [ "this" ] type identifier ;

type        = primitive-type
            | wkt-keyword
            | aggregate-type
            | proto-type
            | "null" ;
primitive-type
            = "bool" | "int" | "uint" | "double" | "string" | "bytes" ;
wkt-keyword = "Duration" | "Timestamp" ;
aggregate-type
            = "list" "<" type ">"
            | "map" "<" map-key-type "," type ">" ;
map-key-type
            = "bool" | "int" | "uint" | "string" ;  (* langdef restriction *)
proto-type  = "proto" "(" fully-qualified-name ")" ;

fully-qualified-name
            = identifier { "." identifier } ;

(* cel-expr is whatever cel-cpp's CEL parser accepts.  Parsed as an
   opaque token-string by the .celfn parser and fed verbatim to
   cel-cpp's parser at typecheck time. *)
cel-expr    = ? CEL expression terminated by `;` ? ;
```

### 3.3 Restrictions and validation

  - `Module` directive is required iff the file contains at least one
    CEL-defined fn.  Pure-declarations files (only `@host.` decls
    and/or `<alias>.` foreign decls) omit it.
  - Three declaration shapes; the parser dispatches on the token
    sequence immediately after the return type:
       - `@ host .`  → host-backed.
       - `<ident> .` → foreign-backed (`<ident>` is the alias).
       - `<ident> (` → CEL-defined.  Must have a `= <body>;` form.
  - A host-backed decl carrying a body → parse error.
  - A foreign-backed decl carrying a body → parse error.
  - A bare-identifier decl (no `@host.` and no `<alias>.` prefix)
    without a body → parse error ("a declaration with no body
    must be either host-backed (`@host.<name>`) or foreign-backed
    (`<alias>.<name>`)").
  - `host` is a reserved alias; a decl shaped `bool host.foo();`
    (i.e. without the `@`) → parse error suggesting `@host.foo()`.
  - An alias matching the file's `Module <name>;` directive is a
    parse error.  A file can't both produce a wasm module under
    that name and import a foreign one under the same name.
  - The same `<alias>.<fnname>` declared twice with different
    signatures → parse error.  Two foreign overloads of the same
    qualified name are allowed; identical signatures are not.
  - `this` may appear on at most one param, and only as the **first**
    param.  If present, the call site lowers as `arg0.fn(arg1, …)`;
    if absent, as `fn(arg0, …)`.  Both forms parse the same kCallExpr
    on the cel-cpp side — the `is_receiver` bit on the FunctionDecl
    decides the dispatch shape.
  - Function names must not collide with cel-cpp standard overloads
    (`add`, `size`, `startsWith`, …) — the same rule the OverloadTable
    enforces.  Caught at `OverloadTableBuilder::RegisterCustom` time
    (returns `AlreadyExists`); the IDL parser surfaces this as a
    parse error with the offending name + the colliding stdlib
    overload-id.
  - Overload-id is synthesised from the declaration:
    `<fnname>_<argkind>_<argkind>…` where each `argkind` is the
    lowercase CelKind name (§3.6).  Deterministic, stable across
    recompiles, matches cel-cpp's convention.  The IDL author never
    writes the overload-id directly; the parser derives it and
    surfaces it in error messages.
  - Identical signatures within one file → parse error.  Different
    signatures with the same `fnname` → distinct overload-ids,
    same `fnname` — cel-cpp's resolver picks the right one.
  - For CEL-defined fns, the body is type-checked against the
    declared `return_type` after binding the receiver + params as
    free variables.  Return-type mismatch is a parse-time error,
    not a runtime trap.
  - `proto(<fqn>)` resolves against the descriptor pool passed to
    `ParseCelfnSource` at parse time.  Unknown FQN → parse error with
    suggestions (descriptor pool already knows the catalog).

### 3.4 Worked example: all three backends in one file

```celfn
// fns.celfn

Module foo;

// ─── CEL-defined functions ───────────────────────────────────────────
// Body is a pure CEL expression.  Compiled into the `foo` wasm module.

bool is_number(this string s)
   = s.matches("^[0-9]+$");

bool is_adult(this proto(acme.User) u)
   = u.age >= 18;

// `cel.bind` covers the `let`-binding case.
string greet(this proto(acme.User) u)
   = cel.bind(name, u.first + " " + u.last, "hi, " + name);

// CEL-defined fn whose body references another CEL-defined fn in
// the same file.
bool is_eligible(this proto(acme.User) u)
   = is_adult(u) && u.first.size() > 0;

// ─── Host-backed declarations ────────────────────────────────────────
// Per-declaration `@host.` prefix.  Embedder C++ provides the impl,
// bound at Plan time via RuntimeBindings::AddFunction(overload_id, impl).

// Method-style.  Overload-id: `upper_string`.
string @host.upper(this string s);

// Free function.  Overload-id: `is_admin_message_acme_User`.
bool   @host.is_admin(proto(acme.User) user);

// Returns a fresh proto.  Overload-id: `lookup_user_string`.
proto(acme.User) @host.lookup_user(string user_id);

// ─── Foreign wasm module declarations ────────────────────────────────
// User-authored wasm (TinyGo, Rust, AssemblyScript, …) provides the
// impl.  Alias `rules` is implicit by use — no separate directive.
// At Plan time the embedder calls:
//   engine.Instantiate(engine.LoadModule("rules.wasm"))
//        → RuntimeBindings::AddModule("rules", instance)

// Overload-id: `allow_message_acme_User_string`.
bool rules.allow(this proto(acme.User) u, string resource);

// Multiple decls under the same alias just share the prefix.
bool rules.deny(this proto(acme.User) u, string resource);

// Second alias — `policy` — just appears.  The embedder must
// AddModule("policy", inst) before Plan.
int  policy.score(this proto(acme.User) u);
```

The compiled `fns.celfn` produces:

  - One **`foo.wasm`** (the `Module foo;` output) exporting:
       - `is_number_string                (i32, i32) → ()`
       - `is_adult_message_acme_User      (i32, i32) → ()`
       - `greet_message_acme_User         (i32, i32) → ()`
       - `is_eligible_message_acme_User   (i32, i32) → ()`
  - A `FunctionLibrary` carrying the above wasm + the declarations
    for the host + foreign + CEL-defined fns.

A CEL expression `name.is_number()` compiled with this library
imports:

  - `foo.is_number_string                 (i32, i32) → ()`
  - (if the expression also calls host fns)
    `cel_fn.upper_string                  (i32, i32) → ()`
  - (if the expression also calls foreign fns)
    `rules.allow_message_acme_User_string (i32, i32, i32) → ()`

Every import takes `(out_slot, arg0_slot, arg1_slot, …)` — the same
ABI built-in overloads already use.

### 3.5 CEL-defined function bodies (pure-expression form)

Body grammar:

```
cel-fn-def = type identifier "(" [ params ] ")" "=" cel-expr ";"
```

Body semantics:

  - The body is a single CEL expression.  Anything the cel-cpp
    parser accepts — operators, calls (including macros like
    `cel.bind`, `has`, comprehensions), proto field access,
    map/list literals, ternary, etc.
  - At parse time, the body is type-checked under a scope where
    each declared parameter (including the `this`-receiver if any)
    is a free variable of its declared type.
  - The body's inferred type must match the declared `return_type`,
    or the parse fails with the full type-mismatch diagnostic.
  - **`cel.bind` is the `let` of CEL.**  `cel.bind(x, value, body)`
    binds `x = value` in `body`.  Nested `cel.bind`s give a
    multi-let block.  No new `let` keyword is needed.
  - **Errors are values, not exceptions.**  A body that wants to
    "throw" returns an error-typed value (e.g.
    `0 / 0` produces `ERROR("divide by zero")`).  The surrounding
    3VL absorption rules apply automatically.  This is the same
    semantics as the rest of CEL — there is no `throw` keyword
    because CEL is referentially transparent and we keep it that
    way.  If a future slice introduces a procedural body form,
    that's where an explicit `throw CelError(...)` would land —
    deferred.
  - **Bodies may call each other** — the IDL author may freely
    reference any other CEL-defined fn from the same `.celfn`
    file in a body.  All declarations are visible to all bodies
    at typecheck time; codegen orders the wasm exports
    topologically (cycles are a parse-time error).

The body's wasm export is generated through the existing pipeline:
`cel-cpp parser → checker → ResolvePass → LayoutPass → ExprLower`,
with the params bound as wasm locals at the function entry instead
of as cel.abi variable entries.

### 3.6 Type catalog

What types a custom-fn declaration may reference, and how each maps
to CEL / cel.abi:

| IDL syntax | CEL kind | `argkind` token (for overload-id) | Notes |
|---|---|---|---|
| `bool` | `bool` | `bool` | |
| `int` | `int` (int64) | `int` | signed |
| `uint` | `uint` (uint64) | `uint` | |
| `double` | `double` | `double` | |
| `string` | `string` | `string` | |
| `bytes` | `bytes` | `bytes` | |
| `null` | `null_type` | `null` | useful for fns that take or produce JSON-coerced data |
| `Duration` | `google.protobuf.Duration` | `duration` | keyword shorthand for `proto(google.protobuf.Duration)` |
| `Timestamp` | `google.protobuf.Timestamp` | `timestamp` | keyword shorthand for `proto(google.protobuf.Timestamp)` |
| `list<T>` | `list(T)` | `list_<argkind-of-T>` | T must be a non-aggregate type in v1 (no `list<list<…>>`); revisit if a use case appears |
| `map<K, V>` | `map(K, V)` | `map_<argkind-of-K>_<argkind-of-V>` | K ∈ `{bool, int, uint, string}` per langdef |
| `proto(<fqn>)` | `message(<fqn>)` | `message_<fqn-with-dots-as-underscores>` | resolved against the descriptor pool at parse time; unknown FQN → parse error |

Explicitly **not allowed** (parser surfaces a citation):

  - `dyn` — `RejectDyn` policy.  Custom fns cannot reintroduce
    dynamic typing.
  - `type<T>` / first-class types — not useful at function
    boundaries; v1 omits the grammar production entirely.
  - `proto(google.protobuf.Any)` — technically representable but
    defeats static typing; v1 rejects with a citation pointing at
    the design doc.  Behind a feature flag for v2.
  - Generic type variables (`T pick(list<T>, int)`) — cel-cpp
    supports them; the v1 grammar declines them to keep the parser
    tiny.

`proto(google.protobuf.{Int32,Int64,UInt32,UInt64,Float,Double,Bool,String,Bytes}Value)`
— wrapper types — ARE allowed.  They appear in real proto schemas
and a custom fn returning a `proto(google.protobuf.Int64Value)` is
a legitimate signature.  CEL's existing wrapper-coercion semantics
apply at call sites.

**`argkind` token derivation for proto FQNs.**  `acme.User` becomes
`message_acme_User`; dots become underscores.  Collision risk
between e.g. `acme.User` and `acme_User` (a hypothetical message
literally named with an underscore) is theoretical but real —
parser fails at FQN registration time when two different FQNs
produce the same argkind token.

### 3.7 Parser implementation

**ANTLR4 generated** parser + lexer driven by the grammar at
`compiler/celfn/Celfn.g4`.  The generated C++ is wrapped by the
visitor at `compiler/celfn/function_library.{h,cc}` which calls
into `FunctionLibrary::Builder` to populate a typed Library.
Surface:

```cpp
namespace celwasm {

// Per-decl typed record.  Self-contained — does NOT carry a
// reference to cel-cpp's FunctionDecl (Slice C constructs those
// from CelfnDecl when registering with the cel-cpp checker).
struct CelfnDecl {
  enum class Backend : uint8_t { kHost, kForeign, kCelDefined };
  Backend backend;
  std::string fn_name;       // "allow"
  std::string module_name;   // kHost → "cel_fn"; kForeign → alias; kCelDefined → Module name
  std::string overload_id;   // synthesised: "<fn>_<argkind>_…"
  uint8_t num_args;          // params.size() + 1 (out_slot)
  bool is_receiver;          // method-style?
  std::vector<CelfnParam> params;
  CelfnType return_type;
  std::string body;          // empty iff signature-only
};

class FunctionLibrary {
 public:
  const std::string& module_name() const;                // file's Module directive
  const std::vector<CelfnDecl>& decls() const;
  const std::vector<std::string>& foreign_aliases() const;
  class Builder { … };                                   // see §2.1
};

// Parse the .celfn source string + populate a Library via Builder.
// CLI use only; embedders constructing libraries programmatically
// call FunctionLibrary::Builder directly.  No descriptor pool
// dependency — `proto(<fqn>)` strings are stored verbatim; Slice C
// resolves them against the compiler's pool at registration time.
absl::StatusOr<FunctionLibrary> ParseCelfnSource(absl::string_view source);

}  // namespace celwasm
```

CEL bodies are stored as raw text on `CelfnDecl::body` and parsed
by cel-cpp during the CEL-body sub-compile (§4.4), not by the
ANTLR parser.  Lexer rule `CelExprText` matches `~[;]+` after the
`=` token; CEL itself has no `;` outside statement terminators, so
this is correct for every legal body.

Why ANTLR4 (not hand-rolled)?  The grammar has 15+ productions
and we want crisp errors with line + column.  ANTLR's `BaseErrorListener`
gives us those for free; cel-cpp's existing `bazel/antlr.bzl`
machinery (vendored to the root MODULE.bazel) handles the build.
Hand-rolling would duplicate that infrastructure without buying
anything.

Earlier drafts of this section sketched a hand-rolled
recursive-descent parser; replaced 2026-05-21 after Slice B's
ANTLR integration shipped (see `m13-reviews/2026-05-21-pre-slice-c.md`
finding A1 for the doc-vs-code drift this fixes).

## 4. ABI: three backends, one CelValue shape

### 4.1 Helper signature (all three backends)

Identical to the existing `_at_v…` ABI:

```
(func $<helper_name>
  (param $out_slot i32)     ;; pointer into wasm linear memory
  (param $arg0_slot i32)    ;; pointer into wasm linear memory
  (param $arg1_slot i32)
  ...
  (result)                  ;; void; result written into *out_slot
)
```

`*out_slot` and `*argN_slot` are 24-byte `CelValue` structs (kind
+ tag + payload).  The trampoline (host) or the wasm function
(module backend) reads each arg via `CelValue::kind` to dispatch on
type, writes the result back into `*out_slot`, and signals errors
by writing `CelValue::Kind::kError` / `kUnknown` into `*out_slot`.

The 3VL absorption rule — built-in operands of `UNKNOWN` / `ERROR`
short-circuit before the trampoline runs — is enforced by the
codegen emitting `cel_and` / `cel_or` / poison-propagation around
the call, identical to how built-in overloads are wrapped today.
So a custom function's body **only ever sees concrete arg
CelValues**; it cannot accidentally evaluate an unknown.  (See
[cel-host-surface.md §4](cel-host-surface.md#4-valueunknown--valueerror-in-the-user-surface).)

### 4.2 Host-backend specifics (`@host.` prefix)

Wasm import name: `cel_fn.<helper_name>`.  The `cel_fn` module
(distinct from `cel_host`) is reserved exclusively for
user-declared overloads.  This separation lets `RuntimeBindings`
walk `cel.abi.functions.host_custom_imports[]` and bind only
user-owned trampolines without touching the built-in trampoline
plumbing.

At `Engine::Plan` time, wasmtime binds each `cel_fn.<helper>` to
a generated trampoline that:

  1. Reads each `arg_slot` into a typed C++ `Value` using the
     declared `arg_types[]` from `cel.abi.functions.host_custom_imports[helper].arg_types`.
  2. Invokes the user's `FunctionImpl` (signature: `absl::StatusOr<Value>
     fn(absl::Span<const Value>)`).
  3. Writes the result `Value` back into `*out_slot` via the inverse
     coercion.
  4. On `FunctionImpl` returning a `Value` whose kind disagrees with
     the declared `return_type`, writes `kError` (kind-mismatch is a
     user bug, not a wasm trap — see [cel-host-surface.md §3.4](cel-host-surface.md#34-typed-abi-entries-vs-cel_host-call-shape)).

### 4.3 Foreign-module backend (`<alias>.<fnname>(...)`)

Wasm import name: `<alias>.<helper_name>` where `<alias>` is the
identifier appearing before the dot in the function-name
position of the declaration.  The IDL declares the **contract**
(which functions exist, with what signatures, under what alias);
the **location** of the wasm implementation is a runtime concern
owned entirely by the embedder.  This decouples the IDL from
filesystem paths and makes the IDL portable across processes /
machines / build systems.

Aliases are **implicit by use** — there's no `Foreign <alias>;`
directive.  The set of aliases the file uses is the set of
distinct first-identifiers across all foreign-fn-decls.  The
parser surfaces this set on `CelfnFile::foreign_aliases` so the
embedder API can iterate it for binding validation.

Alias rules:

  - `host` is reserved.  `bool host.foo();` is a parse error
    (the user almost certainly meant `@host.foo()`); the error
    message suggests the fix.
  - An alias matching the file's `Module <name>;` directive →
    parse error.  A file can't simultaneously produce and import
    a module of the same name.
  - An alias unbound at `Plan` time → `Plan`-time error citing
    the missing `RuntimeBindings::AddModule(<alias>, …)` call.
  - An `AddModule` for an alias the program never references is
    a warning (not an error) — a single `RuntimeBindings` can
    serve multiple compiled expressions.

At `Engine::Plan` time, the embedder is responsible for:

  1. Loading the user's wasm bytes from wherever they live (disk,
     network, embedded resource, in-memory build product, …).
  2. Instantiating against the engine (via `Engine::LoadModule` /
     `Engine::Instantiate`, which auto-wire `cel.memory` +
     `cel.arena_alloc`).
  3. Passing the resulting `Instance*` to
     `RuntimeBindings::AddModule("<alias>", instance)`, where
     `<alias>` matches the first-identifier in `<alias>.<fnname>`
     decls.
  4. `RuntimeBindings::AddModule` walks the user module's exports,
     asserts each declared overload's `helper_name` exists as an
     exported function with the right wasmtime `FuncType`, and
     stages it in the wasmtime `Linker`.  A `helper_name` the
     user's module fails to export is a `Plan`-time error citing
     the missing symbol.
  5. A foreign alias used in the file but with no `AddModule`
     call before `Plan` is a `Plan`-time error ("foreign alias
     `<name>` not bound").  Symmetric mismatch (an `AddModule`
     for an alias the file never used) is a warning, not an
     error, so a single `RuntimeBindings` can serve multiple
     programs.

The user's module **must** be linked against the published
cross-language ABI (§4.5).  The contract is small and explicit;
any wasm-producing toolchain (TinyGo, Rust, Zig, AssemblyScript,
C/C++ via wasi-sdk) that can produce that contract works.  We
generate language-specific boilerplate via `celfnc` (§8.2) so the
user writes typed code in their language and never sees raw byte
offsets.  The Go-via-TinyGo path is the forcing function (§8.3).

### 4.4 CEL-defined backend (top-level `fn` definitions)

Wasm import name: `<module-name>.<helper_name>` where `<module-name>`
comes from the file's `Module foo;` directive.  Architecturally
identical to the foreign-module backend at the wasm boundary — the
expression's `.wasm` imports `foo.is_number_string`, and at
`Engine::Plan` time a wasm instance of `foo` is bound to the
import.

The only difference from the foreign backend: **we produce the
wasm**.  `FunctionLibrary::Builder` runs a sub-compilation:

  1. For each `cel-fn-def`, type-check the body against cel-cpp's
     checker with the params bound as free variables.
  2. Lower the typed body through the existing `ResolvePass →
     LayoutPass → ExprLower` pipeline, producing one wasm function
     per CEL-defined fn.  The wasm function's signature is the
     canonical `(out_slot, *arg_slots) → void`; the body reads each
     arg slot, evaluates, and writes the result.
  3. Bundle all generated functions into a single wasm module,
     exporting each by its `helper_name`.  The module imports
     `cel.memory` and `cel.arena_alloc` from the engine — same as
     a foreign module.
  4. Embed the module bytes in the `FunctionLibrary`.

At Plan time, `Engine::InstantiateLibraryModule(lib)` instantiates
this wasm module exactly as it would a foreign one.  The
`RuntimeBindings::AddModule("foo", instance)` call is identical.

The CEL-defined fns can freely call:

  - **Standard CEL operators / built-ins** (`+`, `size`, etc.) —
    lowered as normal `OverloadTable` calls against the shared
    `cel_runtime.wasm` they import.
  - **Macros** (`cel.bind`, `has`, comprehensions) — expanded by
    cel-cpp's parser before reaching codegen.
  - **Other CEL-defined fns in the same file** — emit a wasm `call`
    to the sibling export, no import needed (both live in the same
    `foo.wasm`).
  - **Host + foreign fns declared in the same file** — emit a
    wasm import to `cel_fn.<helper>` or `<modulestem>.<helper>`.
    The CEL-defined module's wasm gains these imports too; at
    Plan time, the engine wires them up the same way it wires the
    expression's imports.  This is what makes "CEL-defined fn calls
    a host fn calls a foreign fn" compose cleanly.

### 4.5 Cross-language ABI surface

> **Plan-vs-execution delta (2026-05-21, from Probe 2).**  The first
> draft of this section tried to set strict producer-side rules
> (every module imports memory, etc.).  Probe 2 with TinyGo and
> subsequent analysis of the toolchain landscape (Go/TinyGo,
> Rust, C with wasi-sdk, C with bare clang, AssemblyScript) showed
> that this is the wrong stance — every toolchain has different
> defaults, and "strict producer requirements" makes the IDL
> hostile to non-trivial fractions of the wasm ecosystem.  The
> revised stance: **loose producer requirements, strict engine-side
> negotiation.**  The engine adapts; the producer just has to land
> on something representable.

### 4.5.1 Foreign-module type-flow constraint (v1)

> **Plan-vs-execution delta (2026-05-21).**  Added after recognizing
> the externref-vs-arena impedance mismatch at the foreign-module
> boundary.

**Foreign-wasm-backed custom fns MAY NOT take or return proto
messages in v1.**  Allowed arg + return types:

  - `bool`, `int`, `uint`, `double`, `string`, `bytes`, `null`
  - `Duration`, `Timestamp`
  - `list<T>` and `map<K, V>` whose element/key/value types are
    drawn from the allowed set (recursively)

Disallowed:

  - `proto(<fqn>)` — any proto-message type
  - `list<proto(...)>`, `map<…, proto(...)>` — any aggregate carrying
    a proto

This is a v1 simplification with a real-cost motivation.  Proto
messages enter the expression's wasm as externrefs from the host
adapter; they live in a host-resident table and are addressed
through `payload.msg_slot`.  When such a CelValue would cross
into a foreign wasm module (which has its own memory and no
externref table populated by us), the slot id is meaningless —
the foreign module can't dereference it.

Two ways to make protos cross cleanly:

  - **Copy-to-arena**: serialize the proto's relevant fields into
    a wire-format struct in shared memory, hand the foreign module
    a CelValue whose payload points at that struct.  This is real
    engineering: cel_runtime gains a message-serialize helper,
    foreign-module ABI gains a deserialize helper (per language),
    the cel.abi grows field-layout metadata.
  - **Externref bridge**: foreign module imports a "proto reader"
    set of host trampolines (`cel_proto_get_field`, etc.) and the
    externref is opaquely passed through.  This works but
    couples every foreign module to our adapter shape.

Both are deferred.  v1 ships with the simple rule.  Allowed
boundary crossings:

| From       | To              | Proto OK? |
|------------|-----------------|-----------|
| host       | expression wasm | ✅ (externref into msg_slot table) |
| host       | cel_runtime     | ✅ (cel_host trampolines) |
| host       | foreign wasm    | ❌ via celwasmc — host can pass directly but the IDL refuses to declare such a sig |
| expression | host fn         | ✅ (RuntimeBindings::AddFunction sees `Value::Message`) |
| expression | cel-defined fn  | ✅ (same wasm memory, msg_slot stays valid) |
| expression | foreign wasm    | ❌ (the v1 constraint) |
| cel-defined| host / expr     | ✅ |
| cel-defined| foreign wasm    | ❌ (transitively, same reason) |

The IDL parser enforces this at parse time: a `<alias>.<fnname>(...)`
declaration (foreign-backed) whose signature mentions `proto(...)`
anywhere → parse error citing this section.  Host-backed
(`@host.`) declarations and CEL-defined bodies have no such
restriction.

This is documented as a stability property, not just a v1 cap:
future relaxation must add a new declaration form or a flag so
that a `.celfn` written today doesn't silently change semantics
when the constraint loosens.

### 4.5.2 What the producer is required to provide

Loose by design.  A user's wasm module (TinyGo / Rust / C / AS /
hand-WAT / …) interops if it provides four things:

**MUST** (producer is required to provide):

1. **Function exports** for every declared helper, with the
   canonical `(out_slot, *arg_slots) → void` signature — all i32
   params, no result:

   ```wat
   (func (export "<helper_name>")
     (param i32 i32 ...) (result))
   ```

   The export NAME must match the overload-id the .celfn parser
   synthesises from the declaration (e.g. `allow_string_string`
   for `bool rules.allow(this string u, string r);`).  No other
   exports are inspected.

2. **CelValue ABI compliance.**  When the function body reads from
   an arg slot or writes to the out slot, it must use the 24-byte
   layout published in `include/cel/cel_value.h` (excerpt below).
   Reads decode the kind tag and extract the appropriate payload;
   writes set both kind and payload consistently.  Layout is
   frozen as of M13.A; see point 4 for version checking.

   ```c
   // include/cel/cel_value.h (excerpt — full header is normative)
   typedef enum {
     CEL_KIND_NULL = 0, CEL_KIND_BOOL = 1, CEL_KIND_INT = 2,
     CEL_KIND_UINT = 3, CEL_KIND_DOUBLE = 4, CEL_KIND_STRING = 5,
     CEL_KIND_BYTES = 6, CEL_KIND_DURATION = 7,
     CEL_KIND_TIMESTAMP = 8, CEL_KIND_LIST = 9, CEL_KIND_MAP = 10,
     CEL_KIND_UNKNOWN = 12, CEL_KIND_ERROR = 13,
     // CEL_KIND_MESSAGE (11) is reserved — never appears in a foreign
     // module's args/result in v1 (see §4.5.1).
   } cel_kind_t;
   typedef struct {
     uint32_t kind;        // cel_kind_t
     uint32_t tag;          // kind-specific: string len, …
     uint8_t  payload[16];  // kind-specific union
   } cel_value_t;
   ```

**MAY** (producer chooses; engine accommodates):

3. **Memory ownership.**  The producer chooses whether to import
   `cel.memory` from the engine or to define its own memory and
   export it.  Both are valid:

   ```wat
   ;; (a) Producer imports memory.  Rust with --import-memory,
   ;; C with -Wl,--import-memory, hand-rolled WAT.
   (import "cel" "memory" (memory 0))

   ;; (b) Producer defines + exports memory.  TinyGo wasm-unknown,
   ;; stock Go wasip1, AssemblyScript default, C with default link.
   (memory (export "memory") 2)
   ```

   The engine resolves at link time: if a foreign module exports
   memory, that becomes the shared memory bound as `cel.memory`
   for every other module; if no module exports it, the engine
   allocates one.  **At most one module in the link may define
   memory** — multi-foreign deployments where two foreign modules
   each define their own memory fail at `Plan` with a clear
   diagnostic ("conflicting memory definitions; one foreign
   module must import memory instead").

4. **Initialization hook (`_initialize`).**  Producers MAY export
   a single zero-arg `_initialize` function.  TinyGo, wasi-libc-
   linked C, and stock Go's wasip1 do; Rust's `wasm32-unknown-unknown`
   (no_std) and hand-rolled WAT typically do not.  The engine
   calls `_initialize` once after instantiation if it's exported;
   otherwise no-op.  Forced by Probe 2: TinyGo's `wasm-unknown`
   target traps on the first export call without it.

5. **Arena allocations (`cel.arena_alloc` import).**  Producers
   that need to return outbound strings / bytes / lists / maps
   MAY import:

   ```wat
   (import "cel" "arena_alloc"
     (func $arena_alloc (param i32) (result i32)))
   ```

   Sized in bytes; returns an offset into the shared memory.  The
   arena is reset by the engine between evaluations; user code
   need not free.  Producers that only return primitives (bool,
   int, uint, double) need not import arena_alloc — bool/numeric
   results fit inline in the CelValue payload union (no
   allocation).  See `wasi/DESIGN.md` for the arena lifecycle.

6. **Version-section advisory (`cel.toolchain` custom section).**
   Producers MAY embed a `cel.toolchain` custom section declaring
   the published ABI version they built against + the source
   language.  `celfnc`-generated code emits this automatically;
   hand-written modules MAY omit it.  When present, the engine
   verifies the version matches; when absent, the engine accepts
   the module with a debug-log warning (the running invariant
   tests catch ABI drift cheaply elsewhere).

This is the **whole** producer-side contract.  The engine adapts
to everything past this.

### 4.5.3 What the engine guarantees / adapts to

  - **Memory negotiation** — resolves importer vs exporter shape
    per §4.5.2 point 3.
  - **`_initialize` calling** — calls if exported.
  - **Multiple foreign modules** — supported as long as at most
    one defines memory; the rest import.
  - **Toolchain-specific custom sections** — ignored unless
    `cel.toolchain` (the version section).  TinyGo's `producers`
    section, Rust's `__heap_base`, AssemblyScript's `runtime`, etc.
    are silently passed through.

### 4.5.4 Toolchain compatibility (verified vs untried)

| Toolchain | Verified? | Memory default | Has `_initialize`? | Binary | Notes |
|---|---|---|---|---|---|
| TinyGo (`wasm-unknown`) | ✅ Probe 2 | defines (2 pages) | yes — must call | 816 B | Runtime statics at offset 65536; checks init flag on every export entry. |
| Rust (`wasm32-unknown-unknown`) no_std | ✅ Probe 3 | defines (16 pages) | no | 180 B | Smallest binary; exports `__data_end` + `__heap_base`. |
| C (`--target=wasm32 -nostdlib`) | ✅ Probe 3 | defines (configurable) | no | 331 B | Bare-metal; closest to hand-WAT.  Default 1 page — set `-Wl,--initial-memory=131072` for the caller's min-2 constraint. |
| C (`--target=wasm32-wasip1 -mexec-model=reactor`) wasi-libc | ✅ Probe 3 | defines (2 pages) | yes — must call | 448 B | Real production-shape C; full libc available; reactor mode swaps `_start` for `_initialize`. |
| Hand-rolled WAT | ✅ Probe 1 | either (caller's choice) | either | varies | Reference shape; used in caller WAT (imports memory). |
| Stock Go (`GOOS=wasip1`) | ⏳ untried | defines + WASI imports | yes | larger | Full WASI preview1 stdlib; heavier than TinyGo. |
| Rust (`wasm32-wasi`) | ⏳ untried | defines + WASI imports | yes | larger | Same WASI shape as stock Go. |
| AssemblyScript | ⏳ untried | defines | depends | varies | Variable runtime size. |
| Zig | ⏳ untried | defines | no (default) | small | Similar shape to bare-C. |

Probe 3 (M13-A scope) shipped four toolchains; the rest are
straightforward extensions when needed.  All verified shapes have
the same `(out_slot, *arg_slots) → void` ABI and 24-byte CelValue
wire format — no per-toolchain accommodation in the host harness.

### 4.5.5 Coexistence: WASI-reactor and standalone wasm modules together

The toolchain matrix in §4.5.4 spans **two structural shapes** of
foreign wasm — and a v1 invariant of this design is that both
shapes plug into the same engine simultaneously.

**WASI-reactor shape**: the module exports `_initialize` (and
typically imports `wasi_snapshot_preview1.*` symbols if it uses
WASI features).  The engine MUST call `_initialize` once after
`Engine::AddModule` returns, before any of the module's other
exports are reachable.  Producers in this shape: TinyGo
`wasm-unknown` (verified), wasi-libc-linked C in reactor mode
(verified), stock Go `GOOS=wasip1`, Rust `wasm32-wasi`.

**Standalone wasm shape**: the module does NOT export `_initialize`
and has no WASI dependencies.  The engine instantiates it and
calls exports directly.  Producers in this shape: bare C with
`--target=wasm32 -nostdlib` (verified), Rust `wasm32-unknown-unknown`
no_std (verified), AssemblyScript with stub runtime, hand-rolled
WAT.

**Engine behavior is symmetric across both shapes:**

  - `_initialize` is called iff the module exports it.  The engine
    does NOT require it; it does NOT forbid it.  Modules without
    `_initialize` just skip that step.
  - Memory is the module's choice (define vs import) regardless of
    init shape.  §4.5.2 point 3 handles both.
  - WASI-syscall imports are NOT bound by the engine.  Foreign
    modules using actual WASI features (file I/O, env, time) must
    not be added through this path; that's outside v1 scope.  A
    reactor-mode module that imports `wasi_snapshot_preview1.*` but
    never *calls* those imports works because wasmtime's
    instantiation doesn't require importers to call their imports.

**Mixing shapes in one engine** is the supported configuration:

```cpp
Engine engine;
engine.AddModule("rules",  tinygo_built_bytes);     // WASI-reactor
engine.AddModule("policy", rust_no_std_bytes);      // standalone
engine.AddModule("audit",  wasi_c_reactor_bytes);   // WASI-reactor
engine.AddModule("hash",   bare_c_no_stdlib_bytes); // standalone
engine.AddFunction("upper_string", cpp_upper_impl); // host
// ... engine.Plan(program) ...
```

The engine calls `_initialize` on `rules` + `audit` (they export
it); skips for `policy` + `hash`.  All four foreign modules share
the same `cel.memory` (whichever was first to define it; conflict
checks in §4.5.2 point 3 enforce uniqueness).

**Implication for `celfnc`** (the cross-language stub generator):
when picking the producer toolchain for a `.celfn` file, the
user has free choice — wasi-libc-linked C for production
robustness, Rust no_std for binary size, TinyGo for ergonomics,
hand-rolled WAT for full control.  Output from any of them
interoperates with the others in one engine, by design.

> Verified by Probe 3 (shipped 2026-05-21, all four toolchains)
> and Probe 4 (shipped 2026-05-21, engine-owned model resolving
> imports across module shapes).

### 4.6 ABI entry for custom fns

`cel.abi.functions.host_custom_imports[]` becomes:

```proto
message CustomFunctionEntry {
  string function_name        = 1;  // "is_number"
  string overload_id          = 2;  // "is_number_string"
  string helper_name          = 3;  // matches overload_id by default
  bool   is_receiver          = 4;  // method-style?
  repeated CelType arg_types  = 5;  // includes receiver if any
  CelType return_type         = 6;
  enum Backend {
    BACKEND_UNSPECIFIED = 0;
    HOST                = 1;     // bound via RuntimeBindings::AddFunction
    FOREIGN             = 2;     // user wasm module
    CEL_DEFINED         = 3;     // celwasmc-compiled body
  }
  Backend backend             = 7;
  // The wasm import module name in the emitted expression module.
  // HOST:        always "cel_fn"
  // FOREIGN:     the alias from `<alias>.<fnname>` decls
  // CEL_DEFINED: file's `Module <name>;` directive
  // No .wasm path appears anywhere in the ABI — by design.  The
  // path is purely a runtime-side concern.
  string  module_name         = 8;
}
```

Self-contained ABI rule still holds: a reader of the .wasm bytes
can answer every question in [cel-host-surface.md §1.2](cel-host-surface.md#12-self-contained-abi)
about custom fns too.

## 5. Dispatch design: tradeoffs

The user asked for the pros / cons of routing custom fns through
the existing `OverloadTable` (which `Slice F` of M12 just shipped)
vs through a separate `cel_fn.*` namespace + parallel dispatch
path.  Both options were prototyped on paper below; the
recommendation is at §5.3.

### 5.1 Option A — Extend `OverloadTable` (one path for both)

`OverloadTableBuilder::RegisterCustom` already exists (see
`compiler/codegen/overload_table.cc:547`).  The builder stores
custom entries in the same `impls_` vector as built-ins,
deduplicated by overload-id, with a stable interned id.  Codegen
emits `(call $<helper_name>)` via `InternOverloadId` regardless of
whether the row is a built-in or a custom; `InstallOverloadImports`
walks the same table to emit imports.

Required deltas:

  - Add `ImportModule::kCelFnHost` (helper module `"cel_fn"`) for
    `@host.`-prefixed declarations.
  - For each foreign alias seen in the file (and the file's own
    `Module foo;` for CEL-defined fns), the OverloadTable needs
    to name a distinct wasm import module — so the enum-of-2
    becomes a `(kind, module_name)` tagged value (see §5.3).
  - `InstallOverloadImport` dispatches the import module name on
    the new variant.
  - Helper arity is no longer inferred from the helper_name suffix
    (`_at_v` / `_at_vv` / …); it's known from the declared
    `arg_types[]`.  Either:
       - bypass `OverloadHelperArity` for custom rows (look up by id,
         read arity from the ABI entry directly), or
       - stamp the synthesised helper name with the `_at_v…` suffix
         so arity inference still works.  The latter keeps codegen
         uniform but leaks the `_at_v…` convention into a user-
         visible import name.

Pros:

  - **One code path.**  cel-cpp resolves the call → `reference_map`
    stamps the overload_id → `ResolvePass` interns it → codegen
    emits `(call $helper)`.  Identical for built-ins and customs.
  - **Receiver-style and free-form fall out for free.**  cel-cpp
    already routes `s.upper()` and `upper(s)` through the same
    resolver if both decls register the same overload-id.  No
    custom dispatch arm in `expr_lower.cc`.
  - **3VL absorption already wired.**  Every call through the
    table is wrapped by the same `cel_and` / `cel_or` machinery.
    A custom fn returning `UNKNOWN` / `ERROR` propagates without
    new codegen work.
  - **Reuses Slice F's coverage tests.**  `overload_table_test.cc`
    already exercises `RegisterCustom`; the new tests bolt onto
    the existing fixture rather than duplicating it.

Cons:

  - **The `OverloadTable`'s `ImportModule` enum was sized for built-ins.**
    Going from 2 variants (`kCelRuntime`, `kCelHost`) to 2 + N
    means either an N-variant enum (ugly) or a refactor to
    `(kind, module_name)` (more code, but the right shape).
  - **`OverloadHelperArity` is name-suffix-driven.**  Today, helper
    arity is inferred from the helper name (`_at_v` → 2 args, `_at_vv`
    → 3, …).  Custom helper names won't follow that convention.
    Either we expose arity through the `OverloadImpl` struct (cleanest,
    small refactor) or force the synthesised helper names to carry
    the suffix (leaks naming into user-visible imports).
  - **Coupling**.  The `kBuiltinSeeds` invariant tests (e.g.
    `OverloadTableIsExplicitlyUnimplemented`) currently classify
    every cel-cpp `StandardOverloadIds::k*` value.  Custom ids
    don't fit that taxonomy and would need an opt-out path.

### 5.2 Option B — Separate `cel_fn.*` namespace + parallel dispatch

A second registry (`CustomFnTable`) lives alongside `OverloadTable`;
the kCallExpr arm of `expr_lower.cc` first checks the standard
overload table, then falls through to `CustomFnTable` on miss.
The wasm import module for custom fns is always `cel_fn` (host) or
`<module-stem>` (module backend); built-ins stay in `cel` / `cel_host`.

Pros:

  - **Strong namespace separation.**  Built-in trampolines and
    user-owned trampolines never share a directory.  Easier to
    reason about ABI evolution: changing a built-in helper sig
    never breaks a user import and vice versa.
  - **Custom-only ABI can move faster.**  If we later want a
    different calling convention for customs (e.g. "pass an extra
    `EnvHandle` for token-aware fns"), we change `CustomFnTable`
    + `cel_fn.*` import shape without touching `OverloadTable`.
  - **`OverloadTable` stays purely built-in.**  `kBuiltinSeeds`,
    `OverloadTableIsExplicitlyUnimplemented`, the
    `CoverageTripwire` test all keep their narrow meaning.

Cons:

  - **Two parallel dispatch arms in codegen.**  Receiver-style
    customs (`s.upper()`) and free-form customs (`upper(s)`) need
    their own dispatch logic in `expr_lower.cc`.  cel-cpp still
    resolves both to a `kCallExpr` with a `function` name and an
    optional `target`, but the codegen-side fallthrough adds an
    extra `if`.
  - **3VL absorption re-implementation risk.**  The custom-fn arm
    needs to apply the same `cel_and` / `cel_or` wrapping the
    standard arm applies.  If the implementations drift, a custom
    fn's absorption could silently differ from a built-in's —
    exactly the silent-miscompile failure mode `CLAUDE.md` warns
    about.
  - **Two registries to keep in sync.**  Collision detection
    (custom can't shadow built-in) now requires cross-checking
    two tables.  Slice F already has this collision logic for
    `RegisterCustom`; we'd duplicate it.

### 5.3 Recommendation: Option A with a small refactor

Net: extending the `OverloadTable` is the right answer, but the
deltas needed (refactor `ImportModule` from enum-of-2 to
`(kind, module_name)`; lift arity out of helper-name suffix into
the `OverloadImpl` struct) are non-trivial.  Specifically:

  - **`ImportModule` becomes a tagged value.**

    ```cpp
    struct ImportModule {
      enum class Kind : uint8_t {
        kCelRuntime,  // import module "cel" — built-in runtime helpers
        kCelHost,     // import module "cel_host" — built-in host trampolines
        kCelFn,       // import module "cel_fn"  — custom host-backed fns
        kUserModule,  // import module "<module_name>" — custom module-backed fns
      };
      Kind kind;
      absl::string_view module_name;  // only populated for kUserModule
    };
    ```

    `ImportModuleName(mod)` returns the bound name (`"cel"`,
    `"cel_host"`, `"cel_fn"`, or `mod.module_name`).

  - **`OverloadImpl` gains explicit arity.**  `uint8_t num_args`,
    populated from the FunctionDecl for customs and from the
    helper-name suffix for built-ins (until we mass-stamp every
    built-in too — separate cleanup).  `OverloadHelperArity` shrinks
    to "infer for built-ins still using the suffix convention,
    otherwise read from `OverloadImpl`."

  - **`InstallOverloadImports` switches on `ImportModule::kind` for
    the name and on `OverloadImpl::num_args` for the param shape.**
    The `_at_v…` suffix table goes away for new code; only the
    built-in seeds that haven't been migrated keep using it.

Why Option A despite the refactor:

  - **Avoiding two dispatch arms is worth more than the enum
    refactor.**  The 3VL absorption duplication risk (Cons §5.2)
    is the load-bearing cost of Option B — a silent absorption bug
    in custom fns is the kind of compiler miscompile this codebase
    explicitly aims to prevent ([CLAUDE.md "Compilers fail
    silently"](../../../CLAUDE.md)).
  - **`OverloadTable` was designed for customs from day one.**  See
    `overload_table.h:71-95`: the docstring literally talks through
    `MakeOverloadDecl("my_upper_string", …)` registration.  Slice F
    wired the half a real impl needs (lookup, intern, collision
    detection).  Skipping it now would be a step backward.
  - **The refactor is local.**  `ImportModule` is used in three
    places (`overload_table.{h,cc}` + `compile.cc`); arity inference
    in one (`compile.cc`); collision detection in one (the builder).
    Slice-able into a small landing PR that touches no codegen
    semantics.

If the user disagrees and wants Option B, the rest of this doc
adapts mechanically — substitute `CustomFnTable::Lookup` for
`OverloadTable::Lookup` in the codegen plan (§7).  Everything
else (the IDL, the ABI entry, the testing matrix) is the same.

## 6. Frontend integration — cel-cpp checker hookup

Per-call, cel-cpp wants:

```cpp
cel::FunctionDecl decl;
decl.set_name("upper");
ASSIGN_OR_RETURN(auto overload,
    cel::MakeOverloadDecl(
        /*id=*/"upper_string",          // overload_id
        /*result=*/cel::StringType(),   // return_type
        /*args=*/{cel::StringType()})); // arg_types
overload.set_member_function(/*is_receiver=*/true);
RETURN_IF_ERROR(decl.AddOverload(std::move(overload)));
RETURN_IF_ERROR(builder.AddFunction(std::move(decl)));
```

The CLI wiring:

```cpp
// compiler_v2/cli/celwasmc.cc additions
absl::StatusOr<Compiler> BuildCompiler(...) {
  Compiler::Builder cb;
  cb.AddStandardDeclarations();
  if (!opts.functions_path.empty()) {
    ASSIGN_OR_RETURN(auto celfn,
        ParseCelfnSource(ReadFileOrDie(opts.functions_path)));
    for (auto& decl : celfn.decls) {
      cb.RegisterFunction(std::move(decl));
    }
    // Also stash backends for ResolvePass to consume.
    cb.SetCustomFnBackends(std::move(celfn.backends));
  }
  return std::move(cb).Build();
}
```

`Compiler::Builder::SetCustomFnBackends` is new — it stages the
`(overload_id, backend, module_path)` triples for the Resolver to
hand to `OverloadTableBuilder::RegisterCustom`.

For the `RuntimeBindings` side, [cel-host-surface.md §2.4](cel-host-surface.md#24-celruntimebindings)
already has `AddFunction(overload_id, FunctionImpl)`.  We add:

```cpp
RuntimeBindings& AddModule(
    absl::string_view module_name,
    wasmtime::Instance instance);
```

This stages a wasmtime `Instance*` keyed by `module_name`; at
`Plan` time, the linker iterates `cel.abi.functions.host_custom_imports[]`
and for each `MODULE`-backed entry, asserts the named module is
bound and registers the user's export under `<module_name>.<helper_name>`.

## 7. Codegen plan

`expr_lower.cc::LowerCall` already routes through the OverloadTable:

```cpp
const uint32_t interned = overload_table_.InternOverloadId(
    annotations.overload_id);
if (interned == 0) {
  return absl::UnimplementedError(...);
}
// emit (call $<helper_name>) with `arg_count` arg slots + out slot
```

After the §5.3 refactor, this is unchanged — `interned` resolves to
a row whose `ImportModule` correctly names the wasm import module
and whose `num_args` drives the call shape.  Receiver-style calls
land at the same arm: cel-cpp lowers `s.upper()` to a kCallExpr
with `target=s, function="upper"`; the checker resolves to
`upper_string`; the lowering emits the receiver as `arg0`.

### 7.1 cel.abi emission

`cel_abi_emit.cc` walks the OverloadTable's custom subset and
populates `cel.abi.functions.host_custom_imports[]`.  Today the
field is empty in every emitted module; the M13 slice fills it.

### 7.2 Backend-agnostic codegen at the call site

A `<alias>.`-prefixed foreign custom is byte-identical at the
wasm-instruction level to an `@host.`-prefixed host custom —
only the import module name
differs.  This is the whole point of routing both through one
OverloadTable: the codegen arm has zero awareness of which backend
is in play.  Backend dispatch happens at link time in wasmtime,
not at codegen time.

## 8. CLI + tooling

### 8.1 `celwasmc` flag

```
celwasmc -e "<expr>" --functions=<path>.celfn [--schema=...] [--check ...]
```

Multiple `--functions` flags concatenate into one IDL stream
(deterministic ordering — left-to-right).  No `.pb.bin` / proto
form in v1 (the IDL is the only entry point); easy to add later
since the in-memory rep is already proto-shaped.

### 8.2 `celfnc` cross-language stub generator

`celfnc` takes a `.celfn` file plus a target language and emits
the boilerplate that wraps a user's typed function into the raw
`(out_slot, *arg_slots) → void` ABI.  Users write typed code in
their language; `celfnc` writes the byte-shuffling.

```
celfnc --in=fns.celfn --target=go|rust|as|c --out=<dir>
```

Per target language (v1 set):

  - **`go`** — emits `<fn>_celfn_stubs.go` containing
    `//export <helper_name>` functions with raw `uint32` args, the
    CelValue decode/encode dance over shared memory, and a forwarding
    call to user-implemented `func <Name>(<typed-args>) (<typed-return>, error)`.
    User compiles with TinyGo:
    `tinygo build -target=wasi -o fns.wasm ./...`.
  - **`rust`** — emits `<fn>_celfn_stubs.rs` with `#[no_mangle]
    extern "C"` exports; user implements typed `pub fn` bodies in
    a sibling file.  User compiles with
    `cargo build --target=wasm32-wasi --release`.
  - **`as`** (AssemblyScript) — emits `<fn>_celfn_stubs.ts` with
    `@external` decorators; user implements typed bodies in a
    sibling file.  Build with `asc`.
  - **`c`** — emits `<fn>_celfn_stubs.c` against `cel_value.h`; user
    builds with wasi-sdk.

Each generated stub emits a `cel.toolchain` custom section header
containing the published toolchain ABI version + the language tag.
`Engine::Instantiate` verifies the section matches; mismatch is a
clear `Plan`-time error.

### 8.3 The Go forcing-function — concrete pipeline

A user wanting "I have CEL code that calls a Go function" runs:

```
$ cat fns.celfn
bool rules.allow(this proto(acme.User) u, string resource);

$ celfnc --in=fns.celfn --target=go --out=./gen/
$ cat ./gen/fns_celfn_stubs.go    # generated boilerplate
$ cat > rules_impl.go <<'EOF'
package main

func Allow(u *acme.User, resource string) (bool, error) {
    return u.Role == "admin" && resource != "", nil
}
EOF

$ tinygo build -target=wasi -o rules.wasm ./...
$ celwasmc -e 'user.allow("/admin")' \
    --functions=fns.celfn --schema=acme.proto \
    -o expr.wasm

$ ./my-embedder expr.wasm rules.wasm   # C++ application
```

The embedder C++ (§2.1) owns the `rules.wasm` path:

```cpp
ASSIGN_OR_RETURN(auto rules_mod, engine.LoadModule("rules.wasm"));
ASSIGN_OR_RETURN(auto rules_inst, engine.Instantiate(rules_mod));

ASSIGN_OR_RETURN(auto instance, engine.Plan(program,
    RuntimeBindings()
        .AddModule("rules", rules_inst)));   // alias matches `rules.*` decls
```

The alias `"rules"` in `AddModule` matches the `rules.allow(...)`
decl in the IDL.  Same alias same wiring regardless of whether the wasm
came from disk, an embedded resource, or an in-memory build
product — the IDL doesn't care.

This pipeline is the **acceptance test** for the foreign-module
backend; until a TinyGo-built `rules.wasm` round-trips
end-to-end through this pipeline, the slice is not done.

Test fixture for this lives at
`e2e/testdata/custom_fns/go_rules/` and is the
canonical "user side" template the M13.E e2e suite (§12) drives.

### 8.4 Other tooling

  - **`cel_value.h`** ships under `include/cel/` as the public C
    ABI header (§4.5).  Versioned; future deltas require a major
    bump.
  - **`e2e/testdata/custom_fns/`** gains one
    sub-fixture per supported language so the e2e suite covers
    every supported target end-to-end (Go + Rust at minimum; AS
    + C as the suite grows).

## 9. WAT-first prototypes

Per [CLAUDE.md "WAT-first for ABI and codegen design"](../../../CLAUDE.md):
before any codegen lands, the wasm shape of each new import is
prototyped in WAT under `doc/implementation-plan/rewrite/wat/`.

Five WATs cover the new ground:

  - `42_custom_host_receiver.wat` — `s.upper()` where `upper` is a
    host-backed receiver-style custom fn.  Single import
    `cel_fn.upper_string (i32, i32) → ()`.  Calling convention:
    `(call $upper_string (i32.const out) (i32.const s))`.
  - `43_custom_host_free.wat` — `is_admin(user)` where `user` is a
    proto message and the fn is free-form host-backed.  Single
    import `cel_fn.is_admin_message_acme_User (i32, i32) → ()`.
    Verifies externref pass-through via the existing message-slot
    convention.
  - `44_custom_foreign.wat` — `user.allow("/admin")` against a
    foreign module.  Two WAT files:
       - `44a_caller.wat` — the expression module importing
         `rules.allow_message_acme_User_string (i32, i32, i32) → ()`.
       - `44b_rules_stub.wat` — a stand-in for the user's foreign
         module exporting that helper + importing `cel.memory` +
         `cel.arena_alloc`.  This is the WAT-level dress rehearsal
         for the TinyGo output; lets us validate the link shape
         before any Go code exists.
  - `45_custom_cel_defined.wat` — `s.is_number()` where `is_number`
    is CEL-defined.  Two WAT files:
       - `45a_caller.wat` — the expression module importing
         `foo.is_number_string`.
       - `45b_foo_module.wat` — what celwasmc emits for the
         `Module foo;` block.  Exports `is_number_string`, imports
         `cel.memory` + `cel.arena_alloc`, and internally calls the
         standard `cel_string_matches_at_vv` overload from the
         runtime to evaluate `s.matches("^[0-9]+$")`.  This WAT
         locks the wasm-export shape of the CEL-defined backend
         before the sub-compiler is written.
  - `46_cross_module_call.wat` — a CEL-defined fn that itself
    calls a host fn.  Two WAT files:
       - `46a_foo_module.wat` — the CEL-defined module exporting
         `greet_message_acme_User` whose body internally calls
         `cel_fn.upper_string` (imported into the CEL-defined
         module, not just the expression module).
       - `46b_caller.wat` — the expression module that calls
         `foo.greet_message_acme_User`.
    This is the load-bearing case for "CEL-defined fns can call
    host fns"; the WAT proves the import-routing decision (every
    backend's module also re-imports its dependencies) holds up
    before any C++ goes in.

`wat-traces.md` gains one section per WAT.  `wat_runner` gains
flags `--register-custom=cel_fn.upper_string=<wat-stub>` and
`--load-foreign=rules:<path-to-44b>.wasm` so each WAT can be run
end-to-end before any codegen C++ exists.

## 10. Open design questions

### 10.1 ~~User-side toolchain~~ — resolved 2026-05-21

Resolved: ship `celfnc` (§8.2) generating per-language boilerplate
that wraps the cross-language ABI (§4.5).  Go via TinyGo is the
forcing-function target (§8.3); Rust + AssemblyScript + C land
alongside.  v1 ships Go + Rust; AS + C follow in M13.E or a
follow-up.

### 10.2 Concurrency / wasm-module instance sharing

`Engine::Plan` instantiates each user module once per `Engine`;
the user-side `Instance` is shared across every CEL `Instance`
planned under the same `RuntimeBindings`.  If the user wants
per-thread isolation, they pass per-thread `RuntimeBindings`.
No automatic sharing of mutable state between the user's module
and ours.

For CEL-defined fns: the same rule applies — the `foo.wasm`
embedded in the `FunctionLibrary` instantiates once and is
shared.  CEL is referentially transparent so this is always safe.

### 10.3 Overload resolution: custom `size(MyType)` vs built-in `size(string)`?

Same as cel-cpp's rule: signatures are disjoint as long as the
arg types are disjoint.  `size(string)` and `size(MyType)` are
distinct overloads of the same `size` function — both register,
the checker picks based on the resolved arg kind.  No user-visible
ambiguity.  Verified by a positive test in §11.

### 10.4 Type evolution and ABI versioning

Three failure modes to guard against:

  - **Signature change.**  Changing a `.celfn` declaration's
    signature changes the generated overload-id (the kind suffix
    changes).  Old compiled `.wasm` modules carrying the old
    overload-id fail to bind at `Plan` time (overload-id not
    present in `RuntimeBindings`).  No silent ABI mismatch.

  - **CelValue layout change.**  Frozen as of M13.A; bumping the
    layout requires bumping `cel.abi.toolchain_abi_version`.
    `Engine::Instantiate` enforces match between the engine's
    version and the user module's `cel.toolchain` custom section
    (§4.5 point 5).  A mismatched user module fails to instantiate
    with a citation to the version delta.

  - **Foreign-module rebuild drift.**  If the user rebuilds
    `rules.wasm` with a new signature but doesn't update
    `fns.celfn`, the `Plan`-time link check (§4.3 point 3) fails
    because the wasmtime `FuncType` won't match.  Loud, explicit.

### 10.5 Memory model: shared linear memory vs Component Model

Resolved 2026-05-21: shared linear memory + `arena_alloc` import
(§4.5).  Matches what TinyGo / Rust / AssemblyScript all support
today without component-model toolchain dependencies.  The
Component Model is the future story but its toolchain support is
partial (wasmtime stable; TinyGo / AS lagging); revisit when the
cross-language story for components stabilizes.

The shared-memory model has one notable consequence: a faulty
user module can scribble over the host's heap.  Mitigations:

  - The user module imports `arena_alloc` rather than allocating
    raw memory; the arena is the only legitimate region to write.
  - The arena bounds-checks every alloc against the engine's
    configured limit (`CompilerOptions::arena_pages`).
  - wasmtime fuel + the existing one-shot `Eval` lifecycle bound
    any runaway loop.

Stronger isolation (multiple memories per instance, or full
component sandboxing) is the kind of thing the Component Model
buys; we accept the shared-memory tradeoff for v1 because the
forcing-function goal demands TinyGo compatibility.

### 10.6 Procedural body syntax — explicitly deferred

The user's sketched syntax for procedural bodies —

```
Bool isNumber(this string str) {
  Let x = str.matches("…");
  Return x;
  Throw CelError("…", code);
}
```

— is a real language design surface (statements, sequence,
explicit error introduction) that exceeds the v1 slice.  The
pure-CEL-expression form (§3.5) covers every example we've
identified so far, with `cel.bind` covering `let`-style binding
and CEL's existing error-value semantics covering `throw`.

If a future use case genuinely requires procedural bodies (e.g.
multi-step logic with intermediate error introduction that
`cel.bind` chains can't express cleanly), the right home is a
sibling `m14-celfn-procedural-bodies.md` doc that designs the
statement grammar, the lowering to existing CEL constructs (or
a new IR layer), and the error semantics.  That doc would
explicitly NOT introduce mutable state or non-determinism into
CEL — it would be sugar over the existing pure-functional
semantics.  Out of scope for M13.

## 11. Testing obligations

Per [per-component-test-coverage.md](per-component-test-coverage.md)
discipline: positive + negative + boundary at every component.
Manual-tagged where the test requires a wasmtime-instantiated module.

### 11.1 `compiler/celfn/celfn_parser_test.cc`

Positive:

  - Every type in the §3.6 catalog appears as an arg and as a
    return type — one TEST_P matrix.
  - `proto(<fqn>)` resolution against a real descriptor pool.
  - `proto(google.protobuf.Int64Value)` wrapper types parse.
  - Keyword shorthand `Duration` / `Timestamp` equivalence with
    the `proto(google.protobuf.Duration / Timestamp)` form.
  - Receiver vs free form.
  - All three declaration shapes in one file: `Module foo;` +
    CEL-defined fn (body), `@host.<name>(...)` signature,
    `<alias>.<name>(...)` signature.
  - Two distinct foreign aliases (`rules.allow(...)` and
    `policy.score(...)`) coexist; `CelfnFile::foreign_aliases`
    surfaces both in first-use order.
  - Aggregate args (`list<int>`, `map<string, bool>`) — boundary
    case for key-type validation (only spec-allowed key kinds).
  - Comments (line + block).
  - Empty file → empty `FunctionLibrary`.
  - File with no `Module` directive and only `@host.` decls
    and/or `<alias>.` decls parses cleanly.

CEL-defined body coverage (sub-suite):

  - Body uses every operator class CEL exposes (arithmetic,
    comparison, logical, indexing, field access, ternary).
  - Body uses `cel.bind` (the `let` substitute).
  - Body uses macros (`has`, `all`, `exists`, `filter`, `map`).
  - Body calls a sibling CEL-defined fn in the same file.
  - Body calls a sibling host fn declared in the same file.
  - Body's inferred type matches declared return type.

Negative:

  - Unknown type name (`Stringg`) → parse error citing line + suggestion.
  - Reserved name (`add`, `size`) → AlreadyExists with offending id.
  - `this` not on first param → parse error.
  - `dyn` as a type → rejected citing `RejectDyn`.
  - `proto(google.protobuf.Any)` → rejected citing the design doc.
  - Bare unqualified identifier where a type is expected (e.g.
    `User user` without `proto(...)`) → parse error suggesting
    `proto(User)`.
  - Two decls with identical signatures → parse error.
  - `@host.<name>` decl that also carries a body → parse error.
  - `<alias>.<name>` decl that carries a body → parse error.
  - Bare-identifier decl without `@host.` / `<alias>.` prefix
    and without a body → parse error.
  - `bool host.foo();` (missing the `@`) → parse error suggesting
    `@host.foo()`.
  - Foreign alias collides with the file's `Module <name>;` →
    parse error.
  - Same `<alias>.<fnname>` declared twice with identical
    signatures → parse error.
  - String literal with unsupported escape → parse error.
  - Disallowed map key kind (`map<list<int>, int>`) → parse error
    citing the spec.
  - CEL-defined fn body whose inferred type ≠ declared return type
    → parse error with both types.
  - CEL-defined fn body referencing undeclared name → parse error.
  - CEL-defined fn cycle (a calls b calls a) → parse error citing
    the cycle.
  - File without `Module foo;` directive but containing a
    CEL-defined fn → parse error.

### 11.2 `compiler/codegen/overload_table_custom_test.cc`

Already partially covered by `overload_table_test.cc` (Slice F).
New cases:

  - `RegisterCustom` with `ImportModule{kCelFn}` lands in the
    `cel_fn` namespace.
  - `RegisterCustom` with `ImportModule{kUserModule, "rules"}`
    lands under `rules.*` at install time.
  - `RegisterCustom` with `ImportModule{kUserModule, "foo"}`
    (CEL-defined backend) lands under `foo.*`.
  - Two `RegisterCustom` calls with the same `helper_name` under
    different module names → distinct imports, both installed.
  - Custom-id collision with a built-in → AlreadyExists.
  - Custom-id collision with another custom → AlreadyExists.

### 11.3 `compiler/codegen/custom_fn_codegen_test.cc`

  - One `@host.`-prefixed fn → expected import + `(call $...)` at
    the use site, byte-matched against a WAT golden.
  - One fn declared as `bool rules.allow(...)` → same but
    importing from `rules.*`.
  - One CEL-defined fn → caller imports from `foo.*`; the
    generated `foo.wasm` exports the helper and uses the
    runtime-`cel`-import for any built-in op the body uses.
  - Receiver-style call lowers identically to free-form call
    (only the cel-cpp `target` differs; codegen output should match).

### 11.4 `compiler/celfn/cel_body_compiler_test.cc`

The CEL-defined-backend sub-compiler.

  - Body using each primitive operator → emitted wasm calls the
    expected runtime helper.
  - Body using `cel.bind` → expands to the correct wasm shape
    (load + reuse).
  - Body using comprehensions → comprehensions_v2 lowering
    applies inside the CEL-defined fn's wasm.
  - CEL-defined fn body calls another CEL-defined fn in the same
    file → emitted wasm uses internal `(call $sibling)`, not a
    cross-module import.
  - CEL-defined fn body calls a host fn declared in the same file
    → the CEL-defined module gains an import to `cel_fn.*`.
  - CEL-defined fn body returns the wrong type for its declared
    `return_type` → compile-time error.

### 11.5 e2e — manual-tagged, exercises wasmtime

  - **A custom host fn returning a scalar** — bind `upper(s) → s.upper`
    impl; assert `"hello".upper()` → `"HELLO"`.
  - **A custom host fn returning a message** — bind `acme.fetch_user(id) → User{...}`;
    assert field reads on the returned message work end-to-end.
  - **A custom host fn returning ERROR** — bind impl that returns
    `Value::Error`; assert the surrounding expression absorbs it
    (e.g. `is_admin(user) && other` short-circuits to `Error`).
  - **A custom host fn returning UNKNOWN** — bind impl that returns
    `Value::Unknown(attr_id)`; assert short-circuit logic absorbs
    it (`is_admin(user) || true` → `true`).
  - **A CEL-defined fn** — compile a `.celfn` with `Module foo;`
    and `bool is_number(this string s) = s.matches(...);`; assert
    `"123".is_number()` → `true` and `"abc".is_number()` → `false`.
  - **A CEL-defined fn calling a host fn** — `string headline(this proto(acme.User) u) = upper(u.first);`
    where `upper` is declared as `string @host.upper(this string s);`; assert end-to-end.
  - **A CEL-defined fn calling another CEL-defined fn** — `is_eligible`
    body calls `is_adult`; assert composition.
  - **A foreign-module fn (the Go forcing function)** — build
    `rules.wasm` via TinyGo from
    `e2e/testdata/custom_fns/go_rules/`, bind via
    `RuntimeBindings::AddModule`, assert end-to-end eval of
    `user.allow("/admin")`.
  - **A foreign-module fn from Rust** — same fixture, Rust-side
    impl, end-to-end.
  - **Mixed**: one expression that calls a host-backed, a
    CEL-defined, and a foreign-backed custom in sequence.
  - **ABI-version mismatch** — hand-modify the `cel.toolchain`
    section in a built `rules.wasm` to claim a newer ABI version;
    `Engine::Instantiate` fails cleanly.

### 11.5 ABI round-trip

  - `cel.abi.functions.host_custom_imports[]` populated for every
    declared fn.
  - Re-reading the .wasm via `cel.abi` resurfaces the same
    `(overload_id, helper_name, is_receiver, arg_types, return_type,
    backend, module_path)` tuple.
  - A bytes-loaded `Program` (cross-process load path) plans
    correctly against the same `RuntimeBindings`.

### 11.6 Testing-checklist rows to flip

  - "Rewrite M13 — custom fns IDL": one row under the rewrite
    section of `doc/implementation-plan/testing-checklist.md`.
  - "Rewrite M13 — host-backed custom fns": ditto.
  - "Rewrite M13 — module-backed custom fns": ditto.

## 12. Slice plan

Slicing keeps PRs reviewable and each slice independently testable.
Tentative order — adjust at slice-start time when reality intervenes:

  - **Slice A** — refactor: `ImportModule` becomes a tagged
    `(kind, module_name)` value (§5.3); arity moves out of
    helper-name-suffix inference into `OverloadImpl`.  No new
    features.  Lands behind `bazel test //compiler_v2/...` green;
    no behaviour change for built-ins.  WAT prototypes 42–46
    written and assembled (no codegen yet).
  - **Slice B** — `.celfn` parser + `FunctionLibrary` rep (without
    CEL bodies yet — parser stashes them as raw text, sub-compile
    is Slice D).  Lives in `compiler/celfn/`.  Parser tests
    (§11.1) land here, except the body-coverage sub-suite.
  - **Slice C** — host backend end-to-end.  Split into four
    sub-slices because the surface ripples across the engine
    side, the compiler side, and the underlying pipeline
    wiring; landing them as one PR would obscure the layering.

    - **Slice C.1** — engine-owned custom state.  Lands
      `Engine::AddModule(alias, bytes)` (foreign-wasm-module
      registration) + `Engine::AddFunction(overload_id,
      num_args, HostCallback)` (raw host-callback registration)
      + `cel_fn.<overload_id>` trampoline + reserved-alias
      validation + 9 unit tests over the registration matrix.
      Lifecycle: a shared_ptr to `WasmtimeEngineState` keeps the
      custom-module + host-callback maps alive for outstanding
      Instances; per-Plan linker definitions read from those
      maps.  **Shipped 2026-05-21.**  Review:
      `m13-reviews/2026-05-21-post-slice-c1.md`.
    - **Slice C.2** — compiler-side embedder surface.  Lands
      `Compiler::Builder::AddLibrary(FunctionLibrary)` +
      `Compiler::Builder::AddFunction(string_view celfn_source)`
      (parser-driven convenience) + cross-library overload-id
      uniqueness check at `Build()` + deferred-status pattern
      (first parse-failure wins, surfaces at `Build()`) + 7
      unit tests.  `Compiler::function_libraries_` accumulates
      libraries but `Compile` does **not** yet read them — see
      C.3.  **Shipped 2026-05-21.**  Review:
      `m13-reviews/2026-05-21-post-slice-c2.md`.
    - **Slice C.3** — pipeline wiring + e2e.  Lands the
      `OverloadTableBuilder::RegisterCustom` calls per decl
      (so codegen emits `cel_fn.<overload_id>` imports) + the
      `cel::TypeCheckerBuilder::AddFunction` calls (so
      call-site resolution succeeds in the checker) + the
      `celwasmc --celfn <path>` CLI flag + the host e2e tests
      (§11.5 host bullets — Compiler → Program → Engine
      (with `AddFunction`) → Plan → Eval → result).  This is
      the slice that turns C.1 + C.2 from "registered" into
      "actually invoked".
    - **Slice C.4** — closeout: per-component-test-coverage
      doc filled for host backend; lint backlog reconciled;
      testing-checklist rows flipped.  Slice C header status
      line moves to "shipped YYYY-MM-DD".
  - **Slice D** — CEL-defined backend: sub-compiler turns each
    body expression into a wasm function; bundles them into the
    `Module foo;` wasm; `Engine::InstantiateLibraryModule` lands;
    `RuntimeBindings::AddModule` lands (used by both this slice
    and slice E).  CEL-body parser-test sub-suite (§11.1) and
    `cel_body_compiler_test.cc` (§11.4) land here.  e2e tests for
    CEL-defined fns + cross-backend composition land here.
  - **Slice E** — foreign-module backend: `Engine::LoadModule` +
    `Engine::Instantiate` land; the `cel.toolchain` custom section
    verification lands; `celfnc --target=go` lands; the
    `e2e/testdata/custom_fns/go_rules/` fixture lands
    and the Go-via-TinyGo e2e test (§11.5) becomes the slice's
    acceptance test.  `celfnc --target=rust` lands alongside if
    schedule permits; otherwise punted to F or a follow-up.
  - **Slice F** — closeout + remaining language targets
    (`celfnc --target=as` / `--target=c` if not landed in E);
    per-component-test-coverage doc filled;
    `lint-backlog.md` + `cleanup-backlog.md` reconciled;
    `testing-checklist.md` rows flipped; this doc's header status
    line moves to "shipped YYYY-MM-DD".

Each slice updates this doc inline (close-out section, deltas if
the as-shipped shape diverges).  Future work surfaced during
execution goes in §13.

## 13. Future work (populated as the slice executes)

Empty until execution starts.
