# Runtime-catalogue generation — single source of truth

Status: shipped 2026-05-26.

## Problem

The pure-wasm runtime helper set (`cel`-module exports of
`cel_runtime.wasm`) was described in **three** places that had to be
hand-kept in sync:

  1. `runtime/cel_*.{h,c}` — the actual C declarations/definitions
     (the helpers' signatures).
  2. `runtime/wasm_exports.txt` `[codegen-helpers]` section — the
     linker keep-list (which symbols survive `--gc-sections`).
  3. `abi/runtime_catalogue.cc` `kCelRuntimeHelpersArr` — a
     hand-written `AbiHelper{name, module, arity, returns_i32}` array
     codegen reads to emit imports.

(3) was pure duplication of (1)+(2); (2) carried the irreducible
membership fact (which C symbols are codegen helpers vs host-only —
NOT derivable from signatures, since e.g. `cel_int_add_at_vv`
(codegen) and `cel_string_eq_at_vv` (host-only) have identical
signatures).

## Decision

**The C source is the single source of truth, via a per-function
marker.** Each codegen-helper declaration carries a
`// cel:codegen-export` marker on its own line directly above it (213
header decls in `runtime/cel_*.h`; 4 definitions in `cel_runtime.c`
that have no header decl — `cel_equals_at_vv`, `cel_not_equals_at_vv`,
`cel_list_arena_view`, `cel_map_count`). The marker is the authority
for **both** membership and signature:

  - `arity` = the C parameter count;
  - `returns_i32` = return type `uint32_t` (vs `void`).

`//bazel:gen_runtime_catalogue` parses the marked decls and emits the
catalogue; the `[codegen-helpers]` section of `wasm_exports.txt` is
derived from the same markers. Adding a helper = write the C function
+ one marker line; nothing else to touch.

**The `AbiHelper` POD struct is deleted.** The generated proto message
`celwasm.abi.CelRuntimeFunction` (in `abi/runtime_catalogue.proto`) is
the type used throughout — `CelRuntimeHelpers()` / `CelHostFunctions()`
/ `CelEnvFunctions()` return `absl::Span<const CelRuntimeFunction>`,
`FindBuiltinHelper()` returns `const CelRuntimeFunction*`, and consumers
(`compiler/codegen/overload_table.cc`, `eval/internal/cel_host_wasmtime.cc`,
`compiler/internal/compile.cc`, `eval/engine.cc`) use proto accessors.

## Mechanism

```
runtime/cel_*.{h,c}  ──(markers)──>  //bazel:gen_runtime_catalogue
   │                                        │
   │                          ┌─────────────┴─────────────┐
   │                          ▼                           ▼
   │            runtime_catalogue.textproto      wasm_exports.txt
   │            (genrule)                         [codegen-helpers]
   │                          │
   │            runtime_catalogue_textproto_cc (genrule: od → .cc
   │            string literal) + runtime_catalogue_textproto.h
   │            (stable accessor decl)
   │                          ▼
   └──>  runtime_catalogue.cc  parses the embedded textproto once at
         first use into a CelRuntimeCatalogue (process-lifetime).
```

The catalogue is the single source of truth for every built-in but
`cel_fn` (user customs, registered at runtime, never catalogued).  The
`cel_host` / `cel_env` import sets (host trampolines — NOT exports of
`cel_runtime.wasm`, so nothing to derive them from) live in a
**committed, commented textproto** (`abi/runtime_host_env.textproto`) —
human-readable, reviewable, with each function's semantics documented.
The genrule prepends that file verbatim and **appends** the derived
`cel` rows, so the generator is a pure composer (it holds no function
data), the host/env source of truth is the textproto, and the `cel`
source of truth is the C markers.  `runtime_catalogue.cc` serves each
module by filtering the one parsed catalogue.  The startup cross-check
in `cel_host_wasmtime.cc` (registered trampolines vs `CelHostFunctions()`)
guards the host rows against reality.

## Consistency test

`abi/runtime_catalogue_consistency_test.cc` (the old
catalogue ⇔ `wasm_exports.txt` name-set check) was **removed**: with
both the catalogue and the keep-list derived from the same markers, the
check is tautological. `runtime_catalogue_test.cc` still pins the
catalogue invariants (no dup names per namespace, arity bounds,
module↔namespace, `FindBuiltinHelper` resolves every entry, ABI-version
cases).

## Verification

Generated catalogue has **217** `cel`-module entries — identical to the
former hand-written array (0-diff on name/arity/returns_i32). The wasm
links all exports; `//abi/... //runtime/... //compiler/... //eval/...`
build and test green.

## Future work

  - No check that the markers match what `cel_runtime.wasm` *actually*
    exports — a helper added without a marker is silently absent from
    both the catalogue and the linker keep-list (and would fail at
    codegen/instantiate, not at PR time). If that ever bites, add a
    test parsing the built wasm's export section against the catalogue.
  - The `cel_host`/`cel_env` sets are hand-maintained in
    `abi/runtime_host_env.textproto` rather than derived from the
    trampoline registration sites; the startup cross-check keeps them
    honest, but deriving them would remove the last hand-maintained
    list.
