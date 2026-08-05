# 05 — The custom-function subsystem (`.celfn`)

Status: current — authored 2026-06-10 from the design-rebuild notes;
rewritten 2026-08-04 for the host-only surface after the wasm
Component-Model plugin backend (`@plugin.`) and the parse-only
`@native.` stub were removed (m39-component-removal.md; the removed
backend is archived on branch `component-functions-archive`).
Supersedes doc/implementation-plan/rewrite/m13-custom-fns.md.

A `.celfn` declaration is parsed in `compiler/celfn/`, stamped by the
cel-cpp checker, lowered by codegen as a wasm import, and bound by the
evaluator at Plan to a **native C++ callback in the embedder's
process** — the only custom-function mechanism. Evaluator-side
dispatch internals live in 02-evaluator.md §3–§4; cited, not repeated.

## 1. The decl model

One struct describes every custom function: `CelfnDecl`
(`compiler/celfn/function_library.h`) — name, return type (a
`shared/CelType`, the one C++ type vocabulary since the 2026-07-25
unification, cleanup-backlog #53), params (each `{is_this, type,
name}`), synthesized `overload_id`, `num_args`, and a `Backend`
discriminator that is **host-only**: `Backend::kHost` (`@host.`) is
the sole enumerator, kept as an enum so the wire and the validation
funnel stay explicit about backend rather than implying it. A decl
has no body — the implementation is a C++ callable registered on the
`Engine` (`BindFunction` / `AddTypedFunction` / `AddFunction`,
`eval/engine.h`; §4).

Codegen's view: every custom call lowers to a wasm import under the
import module `"cel_fn"` with the `(out_slot, arg_slots...)` calling
convention over 24-byte CelValue slots — the same OverloadTable row
shape as a built-in (`BuildOverloadTable` /
`InstallOverloadImportsExport`, `compiler/internal/compile.cc`). Who
binds `cel_fn.<overload_id>` at Plan is the Engine's business.

Rejected alternative: a separate dispatch table for customs. Extending
the one OverloadTable lets customs inherit every codegen invariant
(import dedup, link-mode skip-if-defined) for free
(compile.cc `InstallOverloadImportsExport`).

## 2. The overload-id identity chain

Overload ids are **synthesized, never user-chosen**:
`overload_id = <fn_name>_<argkind>...` (`SynthesiseOverloadId`,
function_library.cc). `Argkind()` renders each param type as
a slug — `int`, `list_int`, `map_string_int`, `message_acme_User`
(dots → underscores, **case preserved**), `optional_int`, `type`.

That one string is the subsystem's identity at four stations, which
must stay equal:

| # | Station | Where |
|---|---|---|
| 1 | Checker stamp — `overload.set_id(decl.overload_id)` on every resolved call node | `RegisterCustomFunctionsOnChecker`, `compiler/frontend/parse_and_check.cc` |
| 2 | OverloadTable key — `RegisterCustom(..., helper_name = overload_id, num_args)` | compile.cc `BuildOverloadTable` |
| 3 | Wasm import field name — `(import "cel_fn" "<overload_id>" (param i32 × num_args))`, all-void returns, out_slot first | `InstallOverloadImportsExport`, compile.cc |
| 4 | Engine callback key — bound as the `cel_fn.<overload_id>` linker func at Plan; `BindFunction` re-derives the id from the same `.celfn` string, so binding cannot diverge from import name | 02-evaluator.md §3 |

Uniqueness: cross-library at `Compiler::Builder::Build`
(compiler.cc); per-library at `FunctionLibrary::Builder::Build` (§3);
engine-side, `AddFunction` conflict-checks ids against everything
previously registered (02-evaluator.md §3).

## 3. The IDL & the Builder validation funnel

### 3.1 Grammar

`compiler/celfn/Celfn.g4` (single file). Productions: `moduleDirective`
(`Module foo;` — an optional library label; every declaration
dispatches through the `"cel_fn"` import module regardless),
`hostFnDecl` (`type '@' 'host' '.' Id '(' params? ')' ';'`), and
`bareHostDecl` — a **diagnostic-only** production matching
`host.foo(...)` without `@`, converted to a curated InvalidArgument
("use `@host.`") by the visitor (function_library.cc). `@host.` is the
only backend prefix the grammar admits; any other prefix (including
the removed `@plugin.` / `@native.`) is a parse error
(`mismatched input '<word>' expecting 'host'`).

Type syntax: `bool|int|uint|double|string|bytes`, `Duration`,
`Timestamp`, `list<T>`, `map<K,V>` (keys grammar-restricted to
`bool|int|uint|string`), `proto(fqn)`, `null`. **No `type` or
`optional<T>` syntax** — those kinds are constructible only
programmatically (§5). Comments (`//`, `/* … */`) are skipped by the
lexer and not captured — `CelfnDecl` carries no description field.

### 3.2 One funnel for file and programmatic paths

`ParseCelfnSource` (function_library.cc) is a thin driver: ANTLR parse
(errors → InvalidArgument with line:col), then a tree walk into the
same `FunctionLibrary::Builder` the programmatic API (`AddHost`)
exposes. The Builder is the **single validation funnel** —
grammar-bypassing embedders hit identical gates, with the offending
decl named.

`Build()` gates, in order, first failure wins (function_library.cc):

1. `this` only on the first param (`ValidateThisPlacement`);
   `is_receiver` is true iff the first param carries `this`.
2. Per-library overload-id uniqueness.
3. Universal map-key gate: `FirstIllegalMapKey` recurses through
   list/map/optional carriers; an illegal key kind *anywhere* ⇒
   InvalidArgument naming decl + param — matching the kernel's
   map-key contract (pinned: function_library_test.cc).

### 3.3 The arity-cap hole

There is **no params-count cap at any layer**; each seam fails
differently:

- Library: `num_args = uint8_t(params.size()) + 1` (+1 = out_slot,
  function_library.cc) — 255 params silently wraps `num_args` to 0.
- Codegen: `InstallOverloadImport` installs imports only for
  `num_args` ∈ [1, 5]; anything else hits `default: return false` — a
  *silent skip*, so a 5-value-arg decl emits a module whose call
  target was never imported (compile.cc).
- Engine: `AddFunction` rejects only arity 0 (02-evaluator.md §3).

> **Open question (V9):** the cap must live at **one** layer — the
> Builder — with downstream layers CHECKing rather than silently
> skipping. Probe the three failure shapes above first, then extend
> `Build()` with the cap and the all-backend type gate (§5) in one
> change.

### 3.4 Library → compile flow

`Compiler::Builder::DeclareFunctions` / `AddFunction(celfn_source)`
accumulate libraries (compiler.cc); `Compile()` forwards them to
*both* the checker (`CheckOptions.function_libraries`, §2 station 1)
and codegen (`CompileOptions.function_libraries`, stations 2-3).
`DeclTypeToCheckerType` (parse_and_check.cc) maps decl `CelType`s to
`cel::Type`; kType/kOptional hit an `ABSL_CHECK` stub (§5) behind the
`ValidateDeclTypesMappable` gate in compiler.cc. `Compile` also
records every `cel_fn` import the final wasm carries — name, backend,
full recursive signature — as `required_functions` rows in the
Program's `cel.abi` (08-abi-wire-format.md §1.1 field 8), which is
what Plan verifies against the Engine registry and what lets
`cel run` refuse a program the CLI provably cannot supply.

## 4. `@host` end-to-end

The shipped, recommended path is **declaration-first**: one `.celfn`
string is the single source of truth, used verbatim on both sides
(`examples/04_host_functions.cc`):

```cpp
constexpr absl::string_view kDecl =
    "int @host.discount_pct(string tier);";
builder.AddFunction(kDecl);                      // compile side
engine.BindFunction(kDecl, [](absl::string_view tier)
                        -> absl::StatusOr<int64_t> { ... });
```

Compile side: the decl makes `discount_pct` type-check like a builtin
(§2 station 1) and lowers to `(call $cel_fn.discount_pct_string
(out_slot, arg_slot))`. Eval side: `BindFunction` re-parses the same
string (`ParseSingleHostDecl`, engine.cc; exactly one `@host.` decl
required), validates the lambda's `param_kinds` positionally against
the declared CEL types (`CppParamMatchesDeclType`, engine.cc —
`Value` matches anything, `string_view` serves string|bytes,
`null`/`type`/`optional` only via `Value`), and registers under the
synthesized id. A signature mismatch is rejected at registration, not
at eval. Pinned by `EngineBindFunctionTest` (engine_test.cc) and
`e2e/host_fn_test.cc` (`BindFunctionDeclFirstRoundTrip`).

The lower-level surfaces (`AddFunction`, `AddTypedFunction`) and the
L0/L1/L2 dispatch stack are the evaluator's story: **02-evaluator.md
§3 and §4**. This subsystem's contract: the callback is keyed by
overload-id, receives `(out_slot, args...)` CelValue slots, and never
sees Error/Unknown args (absorbed at L0).

The trust posture is stated in the user-facing docs and repeated here
because it is a design decision, not an accident: the callback is
native code in the embedder's process. A sandboxed backend for
function bodies the embedder does not trust was built (wasm
Component-Model plugins), measured, and removed — the component
boundary restricted the type surface to a strict subset of ours,
required a second toolchain, and imposed import restrictions that had
to be stubbed around. The evidence and the alternatives menu for any
future sandboxed backend live in
`doc/implementation-plan/rewrite/m39-component-removal.md` (§1, §10);
the removed implementation is archived on
`component-functions-archive`.

## 5. Type-surface policy

- **CEL `null` is a distinct kind** (kNull ≠ kOptional; pinned
  function_library_test.cc).
- **Map keys are bool|int|uint|string, any nesting depth** (§3.2
  gate 3) — matching the kernel's map-key contract.
- **`optional<T>` and `type` have no `.celfn` syntax** and are
  constructible only programmatically; at the callback boundary they
  are reachable only through the `Value` escape hatch (§4).
- **The gate asymmetry is a live crash bug**: a programmatic
  `AddHost("f", CelType::Type(), ...)` passes `Build()` and crashes
  at the `ABSL_CHECK` stub in `DeclTypeToCheckerType`
  (parse_and_check.cc) — embedder input must never crash the process.
  The fix rides V9 (§3.3).
- **Protos cross the callback boundary as live message objects** —
  `const M&` / `const google::protobuf::Message*` in, owning
  `std::unique_ptr<M>` out (02-evaluator.md §4) — no serialization at
  the host boundary.

## 6. Future work

- The arity cap at the Builder (V9, §3.3) + the kType/kOptional
  `Build()` gate (§5), one change with the three failure-shape probes
  first.
- Host-fn error-message carriage: the `ErrorPayload`'s free-text
  message does not survive the wasm round-trip (PROPOSALS.md "Host-fn
  error message carriage") — tracked, out of scope here.
