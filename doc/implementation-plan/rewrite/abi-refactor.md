# ABI refactor — single source of truth for cel_runtime + cel_host + cel_fn imports

Status: **shipped 2026-05-22.**  Slices A, B, C (revised), D, E shipped.
Slice F (custom-fn namespace policy) was rolled into M13 after a
design conflict surfaced — M13 §4.6's per-backend `module_name`
field (HOST → `cel_fn`, FOREIGN → user's alias, CEL_DEFINED →
file's `Module <name>;` directive) supersedes Slice F's original
"every custom fn through `cel_fn`" framing.  Tracking the work
there avoids designing the same surface twice.

Sibling docs: [`m13-custom-fns.md`](m13-custom-fns.md) (owns the
custom-fn ABI surface and Slice F's deferred work),
[`design.md`](design.md) (parent rewrite architecture),
[`cel-host-surface.md`](cel-host-surface.md) (host trampoline ABI
shape).

## What landed (one paragraph)

The catalogue (`abi/runtime_catalogue.{h,cc}`) is the
single authoritative source for codegen-imported helpers, with
arity + return shape.  `engine.cc::BindAllRuntimeExports` iterates
it; `OverloadTableBuilder` consults it instead of sniffing helper
names; the `cel_host` trampoline registry bijection-checks against
it at registration time; `cel_runtime.wasm`'s linker `--export=`
list comes from `runtime/wasm_exports.txt` (whose
`[codegen-helpers]` section is set-identical to the catalogue,
enforced by `runtime_catalogue_consistency_test`); and every
compiled program carries `runtime_abi_version` in its `cel.abi`
section so `Engine::Plan` can reject programs compiled against an
incompatible runtime with a clear FailedPrecondition rather than
an opaque wasmtime trap at first helper call.

## 0. TL;DR

Every wasm import an emitted expr module declares now derives from one
authoritative C++ catalogue: `abi/runtime_catalogue.{h,cc}`.
Three pre-existing hand-maintained surfaces — `kRuntimeExports` in
`engine.cc`, `-Wl,--export=` flags in `runtime/BUILD.bazel`, and
`InferHelperArity`'s 15-entry exception table in `overload_table.cc` —
are gone.  A consistency test parses the BUILD file at test time and
asserts the linker export list ⊇ catalogue's `cel.*` names; an
`OverloadTableBuilder` CHECK trips at construction if any seed names a
helper the catalogue doesn't know about; an `ABI_VERSION` constant lives
in the catalogue header and (Slice E) gets written into every emitted
`cel.abi` custom section.

User-supplied custom functions live in their own namespace, `cel_fn`,
isolated from the catalogue by design — their arity comes from
`Compiler::Builder::AddFunction`, not from a wired-in list — but every
import name they declare is forced into the `cel_fn` namespace at
codegen time and aliased into engine instances at `AddModule` time
(Slice F, coordinated with [`m13-custom-fns.md`](m13-custom-fns.md)).

## 1. Why this exists

A compiler is a fan-in pipeline.  When a single piece of data — "the
runtime helper `cel_int_add_at_vv` takes 3 i32 args, returns nothing,
and is exported by `cel_runtime.wasm`" — has to be restated in four
places (linker flag, engine bind list, codegen import call,
arity-recovery suffix sniff), drift is inevitable and silent.

Three concrete incidents over the M3–M5 timeframe motivated this:

1. **Added a runtime helper, forgot the `--export=` flag.**  Codegen
   emitted the import; wasmtime accepted the module at parse time; the
   instantiate-time linker bind failed with `missing export
   cel_<name>` from inside wasmtime — opaque to the user, hard to map
   back to the offending CL.

2. **Added a runtime helper, forgot `kRuntimeExports[]`.**  Symmetric
   to (1) but in the opposite direction — the wasm was clean, the
   engine's `BindAllRuntimeExports` pass silently skipped the export,
   the import resolved to nothing, and instantiate trapped on first
   call from `$eval`.

3. **`InferHelperArity` sniffed `cel_*_at_v*` suffix and counted v's,
   maintained an exception list for non-suffix helpers (`cel_eq`,
   `cel_size`, the timestamp accessors).**  When a helper grew an arg
   without renaming, the inference returned the wrong arity, the
   codegen-side import declaration didn't match the runtime's actual
   signature, and the failure surfaced as a wasm validation error
   four stages downstream.

Pattern across all three: the fact is "this helper takes N args"; the
representation is replicated in N independent places; the verification
is "everything passes at every stage and then traps at the bottom of
the pipeline."

## 2. What the catalogue holds

```cpp
enum class AbiModule : uint8_t {
  kCelRuntime,  // "cel"      — pure-wasm helpers in cel_runtime.wasm
  kCelHost,     // "cel_host" — wasmtime host trampolines
  kCelEnv,      // "cel_env"  — host env helpers (cel_log, …)
  kCelFn,       // "cel_fn"   — user-supplied custom fns (per-program)
};

struct AbiHelper {
  absl::string_view name;
  AbiModule         module;
  uint8_t           num_args;
  bool              returns_i32;   // false ⇔ void return
};

constexpr uint32_t kRuntimeAbiVersion = 2;

absl::Span<const AbiHelper> CelRuntimeHelpers();
absl::Span<const AbiHelper> CelHostFunctions();
absl::Span<const AbiHelper> CelEnvFunctions();
const AbiHelper* FindBuiltinHelper(AbiModule module, absl::string_view name);
```

Three closed catalogues + one per-program open set:

  - **`cel.*` — closed, in catalogue.**  ~146 entries.  Pure-wasm
    helpers compiled into `cel_runtime.wasm` (arithmetic kernels,
    comparisons, aggregate ops, 3VL, type conversions, comprehension
    iter snapshots, timestamp / duration parse + format, arena
    primitives).
  - **`cel_host.*` — closed, in catalogue.**  20 entries.  Host
    trampolines registered by `cel_host_wasmtime.cc` (proto field
    read/write/has, map / list aggregate dispatch, WKT unwrap,
    message construction, type name resolution).
  - **`cel_env.*` — closed, in catalogue.**  1 entry today
    (`cel_log`).  Reserved for future cross-cutting host environment
    primitives.
  - **`cel_fn.*` — open, per-program.**  Names + arities come from
    `Compiler::Builder::AddFunction`; the catalogue rejects
    `FindBuiltinHelper(kCelFn, ...)` queries by design.  The
    per-program list lives in the emitted module's `cel.abi`
    section (Slice F).

`num_args` is the exact i32 parameter count at the wasm boundary.  The
"out_slot vs operand" calling convention is layered on top — the wasm
signature is just `i32 × N → ()` or `i32 × N → i32`.

`returns_i32` discriminates the two return shapes in practice.  Today
that means: every `_at_v*` kernel + every host trampoline returns void
(results land in `out_slot`); the arena helpers (`arena_alloc`) and a
small set of count / handle helpers return i32.  No i64 returns
anywhere — if a future helper needs one, this field becomes a small
enum and the wire is bumped.

## 3. The three drift surfaces, and how they're closed

| Surface                                  | Pre-refactor                                                  | Post-refactor                                                       | Slice |
|------------------------------------------|---------------------------------------------------------------|---------------------------------------------------------------------|-------|
| Codegen-side arity recovery              | `InferHelperArity` suffix sniff + 15-entry exception list     | `FindBuiltinHelper(module, name)->num_args`; CHECK at Build time if a seed names an unknown helper | A     |
| Engine-side runtime export bind          | 109-line `kRuntimeExports[]` in `engine.cc`                   | `for (const auto& h : CelRuntimeHelpers()) Bind(...)`               | B     |
| Linker `--export=` list in runtime BUILD | 236 lines of hand-maintained `-Wl,--export=...` flags         | Replaced by `runtime/wasm_exports.txt` — single canonical text file with `[codegen-helpers]` + `[host-only]` sections; genrule emits the wasm-ld response file; cc_binary consumes via `-Wl,@<rsp>`.  Drift caught by test below. | C (revised) |
| Host trampoline registry                 | `kEntries[]` in `cel_host_wasmtime.cc`                        | Derived from `CelHostFunctions()` + paired-trampoline registry; bijection check (catalogue ⇄ trampoline pointers) at `RegisterCelHostImports` time | D     |
| ABI version sentinel                     | none                                                          | `kRuntimeAbiVersion` written into `cel.abi.runtime_abi_version`; `CheckRuntimeAbiVersion` at `Plan` time emits a clear FailedPrecondition naming both versions on mismatch | E     |
| Custom-fn import namespace               | engine accepted user wasm with arbitrary export names; codegen had no canonicalisation | DEFERRED — intersects M13 §4.6's per-backend `module_name` (HOST=cel_fn, FOREIGN=alias, CEL_DEFINED=Module name).  Resolution awaiting decision; see Slice F section. | F (deferred to M13) |

## 4. What shipped (Slices A–C)

### Slice A — catalogue + InferHelperArity removal

`abi/runtime_catalogue.{h,cc}` introduced.  Macros
(`K_AT_V`/`VV`/`VVV`/`VVVV`, `RT_VOID`/`RT_I32`, `HOST_VOID`,
`ENV_VOID`) compress the ~85% of entries that follow the
"`out_slot + N value slots, void return`" shape; non-suffix and
non-void helpers are written long-form.

`OverloadTableBuilder` no longer infers arity from name suffix.  Each
seed carries `{cel_id, helper_name, module}`; the builder calls
`FindBuiltinHelper(module, name)` and CHECK-fails at construction if
the lookup misses — a missing catalogue entry now surfaces at process
startup, not at codegen-time or wasm-instantiate-time.

`InferHelperArity` deleted (50 LoC + 15-entry exception table).  The
lone non-OverloadTable caller (`compile.cc::256`, for `cel_copy_slot`)
now calls `abi::FindBuiltinHelper(kCelRuntime, "cel_copy_slot")
  ->num_args` directly.

### Slice B — engine.cc kRuntimeExports → catalogue iteration

`Engine::Plan`'s `BindAllRuntimeExports` is now:

```cpp
for (const auto& h : celwasm::abi::CelRuntimeHelpers()) {
  ABSL_RETURN_IF_ERROR(BindRuntimeExport(
      impl->linker, ctx, impl->helpers_instance,  // renamed from
                                                   // runtime_instance
                                                   // in m28 (2026-06-08)
      std::string(h.name).c_str()));
}
```

The 109-line hand-maintained `kRuntimeExports[]` is gone.  Adding a
runtime helper means: add the catalogue entry, add the linker
`--export=` flag, add the implementation in `cel_runtime.c`.  Three
edits that together comprise "shipping a helper"; nothing else needs
to know.

### Slice C — runtime exports via canonical text file

**Revised after first cut.**  Original design kept the 236 lines of
`-Wl,--export=` flags in `runtime/BUILD.bazel` and added a tripwire
test that parsed BUILD.bazel.  Tripwire detects drift but doesn't
eliminate it — two hand-maintained lists (catalogue C++ and BUILD
flags) stayed in sync only because a test caught divergence.  Dirty.

Replaced with `runtime/wasm_exports.txt` as the single
canonical surface for the symbols `cel_runtime.wasm` exports.  Two
sections:

  - `[codegen-helpers]` — exact mirror of `CelRuntimeHelpers()`.  167
    entries today.
  - `[host-only]` — exports that aren't codegen-imported: system /
    linker (`malloc`, `free`, `__heap_base`), public arena API for
    host reentry (`arena_init`, `arena_capacity`, `arena_cursor`),
    same-kind `_eq`/`_ne` fast paths reached by tail-call from
    `cel_equals_at_vv`, `_arena` tail-call targets for the
    aggregate dispatchers.  26 entries today.

Two consumers:

  1. `//runtime:wasm_export_args` — genrule that
     `awk`-strips comments + section headers and emits a wasm-ld
     response file (one `--export=<name>` per line).  The
     `cel_runtime_wasm.bin` cc_binary consumes it via
     `linkopts = ["-Wl,@$(location :wasm_export_args)"]` +
     `additional_linker_inputs = [":wasm_export_args"]`.  Verified
     end-to-end against `wasm-dis`: all 193 declared exports land
     in the final wasm; the only extra is `memory` (auto-exported
     by wasm-ld).

  2. `runtime_catalogue_consistency_test.cc` — reads the text file
     at test time, parses it with the same grammar the genrule
     uses, and asserts:
     - `catalogue ⊆ [codegen-helpers]` (else linker dead-strips →
       opaque "missing export" at instantiate),
     - `[codegen-helpers] ⊆ catalogue` (else codegen has no
       arity / return-shape metadata),
     - `[host-only]` is non-empty and contains malloc/free/heap_base
       (defensive cross-check).

Net: 236 lines of duplicated BUILD linkopts deleted; one human-edited
text file canonicalises the surface; one genrule + one test cement
the invariants.  Adding a runtime helper now needs three edits — the
catalogue (arity, return shape), the text file (name, in the right
section), and the impl in `cel_runtime/cel_*.c`.

### Coverage shipped

- `runtime_catalogue_test` — 9 invariant tests: kernel-arity canaries,
  no duplicate names within a namespace, expected cross-namespace
  collisions (`cel.cel_list_at` vs `cel_host.cel_list_at`),
  every `cel_host` entry has a matching trampoline (this is the
  Slice D precondition; the current form is a manual reminder, Slice
  D replaces it with a static_assert).
- `runtime_catalogue_consistency_test` — BUILD-list ⊇ catalogue.
- 24/24 `activation_matrix_test` — full activation matrix (lists,
  maps, structs, protos passed in via `--var`).
- 61/61 `m5b_test` — comprehensions over bound aggregates.

## 5. Slices D, E, F

### Slice D — host trampoline registry from catalogue

**Goal.**  `kEntries[]` in `cel_host_wasmtime.cc::RegisterCelHostImports`
becomes a static derivation from `CelHostFunctions()`.  A
`static_assert` (or test-time invariant if static is not feasible
under the abstraction) asserts that every catalogue entry has a paired
trampoline pointer and vice versa.

**Sketch.**

```cpp
// cel_host_wasmtime.cc
constexpr auto kTrampolines = std::array{
  Trampoline{"cel_get_field",   &CelGetFieldTrampoline},
  Trampoline{"cel_has_field",   &CelHasFieldTrampoline},
  ...
};

static_assert(IsBijection(kTrampolines, CelHostFunctions()),
              "cel_host trampoline ↔ catalogue mismatch");

absl::Status RegisterCelHostImports(linker, env) {
  for (const auto& t : kTrampolines) {
    const AbiHelper* h = FindBuiltinHelper(kCelHost, t.name);
    ABSL_CHECK(h != nullptr);
    ABSL_RETURN_IF_ERROR(Define(linker, env, t.name,
                                h->num_args, h->returns_i32, t.fn));
  }
  return absl::OkStatus();
}
```

The bijection check is the load-bearing piece — a new catalogue entry
without a trampoline (or vice versa) is a compile-time error, not a
runtime trap.  `IsBijection` needs to be constexpr-callable; if it
can't be (string-view hash maps aren't), the test target asserts it
at unit-test time instead.

**Edge cases.**

  - **Trampoline arg-count mismatch with catalogue.**  Currently
    detected by wasmtime's `wasmtime_linker_define_func` rejecting a
    signature that doesn't match the import declaration — but the
    error surface is opaque.  Slice D's `Define` helper takes
    `(num_args, returns_i32)` from the catalogue and constructs the
    wasmtime functype from that, so the catalogue is the only thing
    the trampoline registration trusts.
  - **Trampoline binds a name not in catalogue.**  The bijection
    check rejects.  No silent extras.
  - **Catalogue names a helper without a registered trampoline.**
    Symmetric — rejected by the bijection check.  Today this can
    happen if the test target is omitted; Slice D makes it a
    compile-time error.

### Slice E — ABI version in cel.abi + engine instantiate-time check

**Goal.**  Bake `kRuntimeAbiVersion` into every emitted module and
verify it at engine load time.

**Plan.**

1. Add `uint32 runtime_abi_version = 6;` to `CelAbi` in
   `abi/cel_abi.proto` (next free field number; the
   existing `version = 1` is intentionally a separate concept —
   "schema version of the proto message" — and stays).  Field 6
   carries the runtime ABI version the program was compiled against.
2. `compile.cc` sets the field to `abi::kRuntimeAbiVersion` when
   emitting the cel.abi section.
3. `Engine::Plan` (or `Instance::Instantiate`) reads it and compares
   against the runtime's `kRuntimeAbiVersion`.  Mismatch →
   `FailedPrecondition` with both versions and a one-line "the
   program was compiled against ABI v$X but this engine ships v$Y;
   recompile the program against this engine's compiler".
4. `kRuntimeAbiVersion` bumps on **any** of: helper renamed, helper
   removed, helper arity changed, helper return shape changed
   (void ↔ i32), namespace renamed.  Adding a new helper does NOT
   bump — modules compiled against older versions don't reference
   the new helper, so they're still loadable.

**Policy notes.**

  - **Soft vs hard rejection.**  We hard-reject mismatches.  The
    alternative — accept and rely on wasmtime's type-mismatch trap
    at first call — is exactly the failure mode this whole refactor
    is designed to eliminate.
  - **Forward compatibility window.**  None today.  If we later want
    to ship a runtime that accepts programs compiled against an
    older ABI, we add a `MinSupportedAbiVersion` constant alongside
    `kRuntimeAbiVersion`; the check becomes a range, and dropping a
    helper means bumping the floor.

### Slice F — DEFERRED to M13

**Outcome.**  Rolled into [`m13-custom-fns.md`](m13-custom-fns.md).
The refactor closes at 5/6.

**Why.**  Slice F as originally drafted said "force every custom fn
into the `cel_fn` namespace; engine aliases user wasm exports under
`cel_fn` at `AddModule` time."  When the work was about to start,
M13 §4.6 already specified `CustomFunctionEntry` with a per-backend
`module_name` field:

  - HOST backend         → `cel_fn`
  - FOREIGN backend      → user-supplied alias (`rules`, `tax_calc`, …)
  - CEL_DEFINED backend  → file's `Module <name>;` directive

That contradicts Slice F's single-namespace stance.  The foreign-wasm
backend explicitly needs `rules.allow(user, "res")` to land in the
`rules` module — the alias is "implicit by use" and threads through
the IDL, the engine's `RuntimeBindings::AddModule(<alias>, <instance>)`
call, and the user's mental model.  Renaming the wasm boundary to
`cel_fn` would break that ergonomic without buying any
single-source-of-truth invariant the refactor doesn't already have.

**What M13 owns going forward.**

  - The `CustomFunctionEntry` proto and its per-backend `module_name`
    field (§4.6).
  - Per-program enumeration of every custom-fn import in `cel.abi`,
    across all three backends (HOST / FOREIGN / CEL_DEFINED).
  - The engine-side `Plan`-time validator: every entry in the
    enumeration must be satisfied — HOST via `AddFunction`,
    FOREIGN/CEL_DEFINED via `AddModule(<alias>, <instance>)`.  This
    is where Slice F's load-bearing "missing custom function `foo`"
    diagnostic lands.
  - Arity-mismatch diagnostic: declared `N`, user-provided wasm
    export takes `M`; engine rejects with both numbers.

**What the refactor still owns (and shipped).**

  - `cel_fn` as a reserved alias (`IsReservedAlias("cel_fn")` in
    `engine.cc:374`).
  - The HOST-backend bind path: `Engine::AddFunction(overload_id,
    arity, HostCallback)` registers callbacks; `RegisterHostCallbacks`
    defines each one as `cel_fn.<overload_id>` on the linker at Plan
    time (`engine.cc:509`).
  - All of the runtime-side single-source-of-truth invariants that
    custom-fn codegen will rely on (catalogue + version + linker
    response file + bijection checks).

## 6. Edge-case matrix

A line per case + how it's handled.  Failure mode column says where
the failure surfaces; "good" = at PR time / build time / catalogue
construction, "bad" = at instantiate / eval time with an opaque trap.

| #  | Case                                                                  | Handling                                                                                                                  | Failure surface       |
|----|-----------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------|-----------------------|
| 1  | New helper, missing catalogue entry                                   | `OverloadTableBuilder` CHECK at construction (Slice A)                                                                    | process startup       |
| 2  | New helper, missing `--export=` flag                                  | `runtime_catalogue_consistency_test` (Slice C)                                                                            | PR time               |
| 3  | New helper, missing `kRuntimeExports` entry                           | n/a — list deleted (Slice B)                                                                                              | —                     |
| 4  | New helper, missing impl in `cel_runtime.c`                           | wasm-ld unresolved-symbol link error                                                                                      | build time            |
| 5  | New host trampoline, missing catalogue entry                          | Slice D bijection check                                                                                                   | build/test time       |
| 6  | New host trampoline, missing `kTrampolines` registration              | Slice D bijection check (symmetric)                                                                                       | build/test time       |
| 7  | Helper arity changed in `cel_runtime.c`, catalogue stale              | wasmtime instantiate signature mismatch — still bad.  Mitigation: helper bodies' static_assert on arg-count when feasible | instantiate time      |
| 8  | Helper arity changed in catalogue, `cel_runtime.c` stale              | Same as #7 (symmetric)                                                                                                    | instantiate time      |
| 9  | Helper arity changed in catalogue + impl, modules compiled against old version reloaded | Slice E: ABI version mismatch → `FailedPrecondition` with both versions                                  | engine Plan time      |
| 10 | Helper renamed                                                        | bump `kRuntimeAbiVersion`; older modules cleanly rejected                                                                 | engine Plan time      |
| 11 | Helper dropped                                                        | bump `kRuntimeAbiVersion`; older modules cleanly rejected                                                                 | engine Plan time      |
| 12 | Helper added (additive)                                               | No version bump; older modules don't reference it, still load                                                             | n/a                   |
| 13 | `cel.cel_list_at` vs `cel_host.cel_list_at` name overlap              | `FindBuiltinHelper` is `(module, name)`-keyed; per-namespace index maps; `runtime_catalogue_test::ExpectedCrossNamespaceCollisions` pins the set | construction time |
| 14 | Codegen seed names a `cel_host` helper but module is set to `cel`     | `FindBuiltinHelper(kCelRuntime, name)` misses → CHECK                                                                     | process startup       |
| 15 | `arena_alloc` returns i32; codegen treats it as void                  | wasm validation error at compile time (wrong drop count); also detectable by `returns_i32` mismatch at OverloadTable bind | compile time          |
| 16 | New helper returns i64                                                | Catalogue must extend — `bool returns_i32` becomes an enum or pair `(num_i32_args, return_repr)`; this is a wire change → ABI version bump | n/a — design choice deferred until needed |
| 17 | Custom-fn import declared, user didn't `AddModule` / `AddFunction`     | DEFERRED to M13's Plan-time validator (see Slice F section above).  Today: wasmtime rejects at instantiate with an opaque "unknown import" error. | Engine Plan time (post-M13) |
| 18 | Custom-fn user-provided export name doesn't match imported overload-id | DEFERRED to M13                                                                                                            | Engine Plan time (post-M13) |
| 19 | Custom-fn arity mismatch (IDL says N, user wasm export takes M)        | DEFERRED to M13                                                                                                            | Engine Plan time (post-M13) |
| 20 | User-provided custom-fn name collides with built-in (`size`, `string`) | Forbidden at IDL parse + `Compiler::Builder::AddFunction` precondition (M13)                                              | builder API call      |
| 21 | Two custom fns with same overload-id from different libraries          | Forbidden at `Compiler::Builder` — second `AddFunction` with duplicate id returns `AlreadyExists`                          | builder API call      |
| 22 | `cel.abi` section corrupt / truncated                                  | `Engine::Plan`'s proto parse fails; `InvalidArgument` with byte offset.  Pre-existing behaviour, not changed.              | engine Plan time      |
| 23 | Module compiled by a third-party toolchain (no `cel.abi` section)     | Reject at `Plan`.  Already today: `InvalidArgument("missing cel.abi section")`.                                            | engine Plan time      |
| 24 | `runtime_abi_version == 0` (old module from pre-Slice-E build)        | Treat as "unversioned" — reject with `FailedPrecondition("module predates ABI versioning; recompile")`                     | engine Plan time      |
| 25 | Engine with newer runtime, module with older runtime_abi_version      | Reject (today).  When forward compat is added (post-Slice-E followup), accept if within `MinSupportedAbiVersion` window.   | engine Plan time      |
| 26 | Helper used only by checker / static layout, never imported into wasm | Catalogue is the import surface; static helpers stay in `compiler_v2/`, not catalogued.                                    | n/a                   |
| 27 | Catalogue entry with `num_args == 0`                                  | Allowed (`arena_reset()`).  Distinct from pre-refactor "arity 0 means silently skip import" gate — that gate is gone.      | n/a                   |
| 28 | Two helpers sharing the same name in the same namespace               | `runtime_catalogue_test::NoDuplicateNamesWithinNamespace` catches at unit-test time                                        | PR time               |

## 7. Migration / compat story

  - **Modules emitted before this refactor.**  The cel.abi section
    didn't carry `runtime_abi_version`.  After Slice E, those modules
    are rejected by `Engine::Plan` with a clear "predates ABI
    versioning" diagnostic.  The intersection with users in practice
    is empty — there are no published artifacts produced by
    `compiler_v2/` predating this refactor — but the message is
    explicit anyway.
  - **Catalogue additions are additive.**  Until Slice E lands, no
    versioning enforcement; until Slice F lands, no `cel_fn`
    enforcement.  Slices E and F can land independently; D can land
    immediately after C and before E.
  - **Catalogue removals are breaking.**  Any helper removal bumps
    `kRuntimeAbiVersion`.  When the bump lands, sibling docs
    (`testing-checklist.md`, this doc) get the new version recorded.

## 8. Open questions

  - **Multi-version runtime.**  Do we ever want one engine to host
    two `cel_runtime.wasm` versions side by side?  Today no — the
    runtime is process-global.  If multi-tenant ever requires it,
    `Engine::Builder` grows a `WithRuntime(span<const uint8_t>)`
    option and the catalogue carries a runtime-id-keyed index.
    Open: don't design for it unprompted.
  - **Custom-fn ABI subset.**  M13's foreign-wasm backend needs the
    user's module to be able to construct `CelValue` cells —
    meaning it imports a subset of `cel.*` / `cel_host.*`.  Which
    subset?  Today: the same surface the expr module uses, which is
    a superset of what a single custom fn typically needs.  Worth
    revisiting in M13 if the surface ends up large enough to be a
    drag on user-side build size.
  - **i64-returning helpers.**  Not needed today.  Catalogue's
    `bool returns_i32` becomes an enum when needed; design the
    extension at first use, not preemptively.

## 9. References

  - `abi/runtime_catalogue.{h,cc}` — the catalogue itself.
  - `abi/runtime_catalogue_test.cc` — invariant tests.
  - `abi/runtime_catalogue_consistency_test.cc` — BUILD ↔
    catalogue tripwire.
  - `compiler/codegen/overload_table.cc::OverloadTableBuilder` —
    consumer of `FindBuiltinHelper`.
  - `eval/engine.cc::BindAllRuntimeExports` — consumer of
    `CelRuntimeHelpers()`.
  - `eval/internal/cel_host_wasmtime.cc::RegisterCelHostImports`
    — Slice D target.
  - `abi/cel_abi.proto` — Slice E target (add
    `runtime_abi_version` field).
  - `doc/implementation-plan/rewrite/m13-custom-fns.md` — Slice F's
    sibling design.
