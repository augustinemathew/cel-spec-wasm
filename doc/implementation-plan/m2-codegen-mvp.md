# M2 — WASM codegen MVP

Status: **in progress** (started 2026-04).  The scalar slice is green
end-to-end; the module still lacks non-scalar payload plumbing, the ABI
custom section, and the wasm32 cross-compile target.  See
[Remaining for M2](#remaining-for-m2) below for the exact punch list.

## Scope

Lower a statically-typed `TypedAst` to a self-contained `.wasm` module.  The
MVP target is enough of the ABI and codegen to evaluate pure arithmetic,
boolean logic, string equality, and scalar proto field reads — i.e. the
smallest slice that exercises linear memory + externref + host imports
together.

Out of scope for M2 (later milestones pick these up):
  - comprehensions (M3) — but the scope-frame plumbing is stubbed in M2 so
    M3 only has to fill in codegen.
  - advanced string ops + format directives (M4).
  - three-valued error/unknown propagation (M5).
  - user-defined functions (M6).

## Deliverables

### Runtime (authored in C, compiled to a wasm object)

- [x] `compiler/runtime/cel_runtime.h` — `CelKind` enum, 24-byte
      `CelValue` tagged union (static-asserted), `CelSpan`/`CelArray`/
      `CelMap`/`CelDurTs` payload layouts, full set of `cel_make_*`
      constructors.
- [x] `compiler/runtime/cel_runtime.c` — bump allocator (`cel_alloc`,
      `cel_reset`) over a 64 KiB static-backed linear-memory buffer,
      singleton storage for null/true/false/optional-none, all
      constructors, `cel_string_eq` + `cel_bytes_eq`.
- [x] `compiler/codegen/cel_refs.{h,cc}` — emits an externref table
      + the three pure ref-table helpers (`cel_ref_intern`,
      `cel_ref_get`, `cel_refs_reset`) via the Binaryen C API.
      Authored as a standalone module for M2 unit + e2e coverage
      (`compiler/codegen/cel_refs_test.cc` — validator + export
      shape + i32 next-slot global init; `compiler/e2e/cel_refs_e2e_test.cc`
      — wasmtime round-trip: intern → get returns the host payload,
      slot numbers increment, `cel_refs_reset` rewinds, slot 0 is
      the null sentinel).  **M3 relocates this logic into the
      runtime module** (see `doc/wasm-compiler-design.md` §7.0 —
      runtime owns memory + `$cel_refs` table + helpers; eval
      modules import them).  The emitter and its tests stay
      useful either as-is or as a reference for how to author the
      same shape in C against clang's `__externref_t` extension.
      `cel_wrap_message` / `cel_unwrap_message` still defer until
      M3 because they need `cel_alloc`.
- [x] Build rule that cross-compiles the C to wasm32 via brew's `clang`
      (Apple clang has no wasm32 target).  Genrule in
      `compiler/runtime/BUILD.bazel` drives `/opt/homebrew/opt/llvm/bin/clang`
      with `--target=wasm32 -ffreestanding -nostdlib -Wl,--no-entry
      -Wl,--export-all`; a second genrule embeds the resulting
      `cel_runtime.wasm` as a C++ byte array
      (`//compiler/runtime:cel_runtime_wasm_bytes`).  Coverage:
      `compiler/codegen/runtime_link_test.cc` loads the bytes through
      `BinaryenModuleReadWithFeatures`, validates, asserts every
      `cel_*` export is present, and round-trips through
      `BinaryenModuleAllocateAndWrite`.  The codegen-side merge onto
      the eval module is deferred to M3: the scalar slice has no
      runtime dependency yet and the first caller will be string
      constants.  Freestanding wasm has no libc, so the C source
      conditionally provides inline `memcpy` / `memset` / `memcmp`
      behind `#if defined(__wasm__)`.  Both the genrule and the
      embedded-bytes target are tagged `manual` because they depend on
      absolute brew paths (non-darwin machines won't have them).

### Codegen (Binaryen C API — see design §10)

- [x] Binaryen vendoring via `MODULE.bazel` `http_archive` pinned to
      version_129 (SHA256 verified) + `third_party/binaryen/BUILD.external.bazel`
      `cmake()` rule (via `rules_foreign_cc`) driving Binaryen's own
      CMakeLists.txt.  Exposes `@binaryen//:binaryen` with `libbinaryen.a`
      + `binaryen-c.h`.
- [x] `compiler/codegen/module.{h,cc}` — thin RAII wrapper over
      `BinaryenModuleRef` holding memory + `$cel_refs` externref table
      + imports + exports + functions + validate + serialize.
      `BinaryenModuleSetFeatures` turns on reference-types, multivalue,
      bulk-memory, sign-ext, mutable-globals, and GC (the last only to
      allow `ref.null externref` table initializers; we emit no GC
      instructions).
- [x] `compiler/codegen/expr_lower.{h,cc}` — MVP: lowers constants
      (int/uint/double/bool), arithmetic (`+ - * / %`), unary negate,
      logical (`&& || !`), comparisons (`== != < <= > >=`), and the
      ternary `_?_:_` to Binaryen expressions on the scalar Reprs
      {bool, int, uint, double}.  `LowerToEvalFunction` emits a
      nullary function whose return type is the root expression's
      scalar ABI lowering.  Identifiers, selects, lists, maps,
      structs, comprehensions, strings, bytes return `Unimplemented`.
      Error-propagation semantics (overflow, divide-by-zero, NaN,
      unknown) deferred to M5.
- [x] `compiler/codegen/abi.{h,cc}` + `cel_abi.proto` — writer for the
      `cel.abi` custom section.  Uses cel-cpp's `AstToCheckedExpr` to
      embed a full `cel.expr.CheckedExpr`; M2 leaves the interning
      tables (`types` / `attributes` / `patterns` / `error_msgs`)
      empty because the codegen MVP never emits references to them.
      The CLI attaches the section on every `--emit_wasm` invocation.
      Coverage: `abi_test.cc` round-trips the payload through a
      serialized `.wasm` by walking the wasm section list directly
      (no Binaryen reader dependency).
- [x] CLI: `celwasmc -e "<expr>" --emit_wasm=out.wasm [--check …]`
      writes a complete module.  The flag implies `--check` (lowering a
      ParsedExpr is meaningless without type info) and reuses the same
      pipeline as the e2e test: `ParseAndCheck → LowerToEvalFunction →
      Validate → Serialize → write`.  Non-scalar root expressions
      surface as a `codegen error: …` diagnostic on stderr with exit
      status 1; no partial output file is written.  The flag uses an
      underscore (`--emit_wasm`) because absl::ParseCommandLine's
      default name resolution only accepts the underscore form.
      Coverage: `compiler/cli/emit_wasm_test.sh` (sh_test) — positive
      case (int expr produces a `\0asm\x01` file, boolean expr does
      too), negative case (string constant fails with a codegen
      diagnostic and writes no output file).

### Tests (google-style `cc_test`)

- [x] `compiler/ir/annotations_test.cc` — one test per `Repr` value +
      `ReprName` coverage.  *(M1 backfill — landed before M2 started
      but the box here was stale; flipped 2026-04.)*
- [x] `compiler/ir/typed_ast_test.cc` — covers every `TypeSpec` variant in
      `ReprOf()` (primitive × 6, wrapper × 6, well-known × 3, null, list,
      map, message, type, dyn, error).  *(M1 backfill — see note above.)*
- [x] `compiler/ir/static_subset_test.cc` — covers every `ExprKindCase` in
      both "all typed" and "some node DYN" configurations.  *(M1 backfill.)*
- [x] `compiler/frontend/parse_and_check_test.cc` — one test per primitive,
      list, map, and message spec parse; negative cases for bad spec, bad
      type name, trailing garbage.  *(M1 backfill.)*
- [x] `compiler/cli/emit_wasm_test.sh` — sh_test for the CLI's
      `--emit_wasm` flag (positive: int + bool expressions produce a
      file with the `\0asm\x01` preamble; negative: string-constant
      root surfaces a `codegen error:` on stderr with no output file).
- [x] `compiler/codegen/binaryen_smoke_test.cc` — proves the Binaryen
      integration is reachable from Bazel: `BinaryenModuleCreate`,
      add a function returning `i32.const 42`, `BinaryenModuleAllocateAndWrite`,
      assert bytes start with `\0asm\x01\x00\x00\x00` and that
      `BinaryenModuleValidate` reports OK.
- [x] `compiler/codegen/module_test.cc` — 16 tests: empty-module
      preamble, move construct/assign, SetMemory (export, twice-fails,
      max<initial), AddCelRefsTable (externref type, max<initial),
      function import is callable, locals declared, Export
      Function/Table, full eval-module shape validates; plus 3 cases
      on the `TupleType` helper (empty → None, single → passthrough,
      N → interned tuple).
- [x] `compiler/codegen/expr_lower_test.cc` — 23 tests covering:
      constants (int/uint/double/bool) return the right `BinaryenType`
      and the validator accepts; arithmetic for int (signed),
      uint (unsigned opcodes), double; unary negate for int (`0 - x`
      shape) and double (`f64.neg`); logical not (`i32.eqz`);
      comparisons that pick signed vs unsigned vs f64 opcodes; `&&`
      and `||` lower to `BinaryenIf` short-circuit shape; `?:` lowers
      to `BinaryenIf`; a mixed expression; and negative tests that
      ListExpr, MapExpr, string constants, and IdentExpr all cleanly
      return `Unimplemented`.  Plus `WasmTypeFor` scalar coverage.
- [x] `compiler/runtime/cel_runtime_test.cc` — 38 gtest cases covering
      struct size invariant, allocator alignment / reset / OOM, every
      `cel_make_*` constructor (singletons + per-kind payload), and
      string/bytes equality on empty/equal/differing-length/differing-
      content/cross-kind/zero-offset inputs.
- [x] End-to-end: `compiler/e2e/eval_test.cc` — 17 tests that instantiate
      each generated module under the wasmtime C API, call the exported
      `eval` function, and compare the returned `wasmtime_val_t` against a
      literal.  Covers int/uint/double constants, arithmetic (with
      precedence + unary negate + modulo), unsigned division, all six
      comparison ops on ints (including negative-operand signed check) and
      doubles, bool literals, `&&` / `||` / `!`, ternary, and a mixed
      expression.  This is the first place in the test suite that proves
      the emitted `.wasm` actually executes correctly — validator passes
      + inspected IR shape (what `expr_lower_test` / `module_test` check)
      only prove the module is well-formed, not that it means what we
      think it means.  Wasmtime v43.0.1 is pinned in `MODULE.bazel` as a
      prebuilt darwin-arm64 archive under `@wasmtime_darwin_arm64`; other
      platforms can gain a matching archive + `select()` later.

## Remaining for M2

The remaining unchecked boxes above are the closing work for M2; none
of them is a blocker for scalar e2e but all three gate the start of M3.

1. **`compiler/codegen/cel_refs.{h,cc}`** ✅ *landed 2026-04-18.*
   Emits the `$cel_refs` externref table plus the three helper
   functions (`cel_ref_intern`, `cel_ref_get`, `cel_refs_reset`) and
   exports them under their internal names.  A mutable i32 global
   `cel_refs_next` starts at 1 (slot 0 is the null sentinel) and
   grows monotonically; `cel_refs_reset` rewinds it to 1.  Release-
   per-slot is out of scope for M2 — the expression's externref
   lifetime matches the arena's.  The `CelValue*`-shaped helpers
   (`cel_wrap_message`, `cel_unwrap_message`) still need
   `cel_alloc`, so they land with the wasm32 cross-compile step
   below.  Coverage: `compiler/codegen/cel_refs_test.cc` (validator,
   global init, export shape) + `compiler/e2e/cel_refs_e2e_test.cc`
   (wasmtime round-trip: intern→get returns host payload, slot
   numbers increment, reset rewinds, slot 0 returns null).

   Non-obvious wasmtime fact worth keeping visible: the module
   Binaryen emits declares the `$cel_refs` table with a typed
   `(ref.null externref)` slot initializer, which only parses under
   wasmtime when `wasm_reference_types` + `wasm_function_references`
   + `wasm_gc` are all enabled in the engine config.  The e2e test
   sets all three.

2. **wasm32 cross-compile rule for the C runtime.** ✅ *landed 2026-04-19.*
   Apple clang ships without a wasm32 backend, so
   `//compiler/runtime:cel_runtime_wasm_file` is a `genrule` that shells
   out to `/opt/homebrew/opt/llvm/bin/clang` (brew's 22.x) with
   `--target=wasm32 -ffreestanding -nostdlib -O2 -Wl,--no-entry
   -Wl,--export-all`.  A second genrule
   (`:cel_runtime_wasm_bytes_cc`) converts the resulting module into a
   C++ byte array via `od -An -tu1 -v`; a tiny `cc_library`
   (`:cel_runtime_wasm_bytes`) exposes `kCelRuntimeWasmBytes` +
   `kCelRuntimeWasmBytesSize` to codegen.  Both targets are tagged
   `manual` because the absolute brew path is non-hermetic — CI on a
   darwin-arm64 box with brew `llvm` + `lld` installed runs them
   explicitly, other machines skip them.
   -
   Freestanding wasm has no libc, so `cel_runtime.c` conditionally
   supplies inline `memcpy` / `memset` / `memcmp` behind
   `#if defined(__wasm__)` — without this wasm-ld reports unresolved
   symbols that clang autogenerated from struct copies.  The native
   build (`cc_library :cel_runtime`) still includes `<string.h>`
   through the same preprocessor switch.
   -
   Coverage: `compiler/codegen/runtime_link_test.cc`
   `BinaryenModuleReadWithFeatures`-es the embedded bytes, runs the
   validator, walks `BinaryenGetExportByIndex` and asserts every
   `cel_*` constructor + `memory` is present, then serializes via
   `BinaryenModuleAllocateAndWrite` and re-reads the output to
   confirm round-trip equivalence.  Failure mode the test targets:
   a clang dead-strip or link flag change that silently drops a
   constructor export.
   -
   The codegen path does **not** merge the runtime into the eval
   module.  Instead (decided 2026-04-19 — see
   `doc/wasm-compiler-design.md` §7.0) every eval module declares
   imports from a `"cel"` namespace and the host wires those
   imports to the runtime's exports at instantiation time.  The
   cross-compile artefact above is what the host instantiates; the
   codegen-side work lands in M3 as "emit imports against the
   expected runtime shape, not `BinaryenModuleRead` + append".
   Concretely, M3 adds:
     - A walk over the IR that collects the set of `cel_*`
       functions the expression actually calls (don't import the
       full 24-constructor surface for `1 + 2`).
     - `BinaryenAddFunctionImport` + `BinaryenAddMemoryImport` +
       `BinaryenAddTableImport` calls in `expr_lower` so the eval
       module's imports header is well-formed.
     - A host loader in `compiler/runtime/host_loader.{h,cc}` that
       owns the instantiate-runtime-once / wire-imports dance so
       embedders (and the e2e tests) don't reinvent it.
     - An e2e test where the root expression is a string constant
       that constructs a `CelValue` via `cel_make_string` and
       asserts the returned `i32` pointer reads back as the
       expected kind + payload from the runtime's linear memory
       — a completely new class of e2e assertion.

3. **`compiler/codegen/abi.{h,cc}` + `cel_abi.proto`** ✅ *landed 2026-04-18.*
   Emits the `cel.abi` custom section holding a serialized
   `celwasm.CelAbi` proto.  For M2 the payload is `version` /
   `cel_source` / `checked` (full `cel.expr.CheckedExpr` via
   cel-cpp's `AstToCheckedExpr`) / empty `function_set` / `layout`
   = {initial_pages=1, max_pages=0}.  The interning tables
   (`types` / `attributes` / `patterns` / `error_msgs`) stay empty
   because the codegen MVP never references them; they populate as
   M3 (proto fields, strings), M4 (collections), and M5
   (three-valued logic) introduce features that need them.  The
   CLI's `--emit_wasm` flow attaches the section on every emitted
   module.  Round-trip coverage in `abi_test.cc` walks the wasm
   section list directly (small hand-rolled parser — Binaryen
   lacks a section reader) so a schema drift or an accidental
   section-name typo fires the test.

### Testing gaps still open in M2

Even for the scalar slice that is "done", the coverage checklist has
unchecked boxes that a vigilant reader should close before calling M2
complete:

- ~~**`RejectDyn`** per-type backfill~~ — closed 2026-04-18 by
  `static_subset_test::{AcceptsEveryPrimitiveAtRoot,
  AcceptsEveryPrimitiveWrapperAtRoot, AcceptsNullTimestampDurationAny}`.
- ~~**`kSelectExpr` (test_only from `has()`)**~~ — parser / checker /
  annotations / RejectDyn rows closed 2026-04-18 via
  `parse_and_check_test::HasMacroLowersToTestOnlySelectExpr` and
  `static_subset_test::{TestOnlySelectExprIsAcceptedWhenOperandTyped,
  DynOperandInTestOnlySelectIsRejected}`.  Codegen + e2e intentionally
  remain deferred until M3 wires up proto-field reads and strings.
- ~~**`e2e` column for uint/double per-op comparisons + double
  negate**~~ — closed 2026-04-18 by `eval_test::{UintComparisons,
  DoubleComparisons, DoubleNegate}`.
- ~~**Negative codegen tests with a good-message assertion**~~ —
  closed 2026-04-18; `expr_lower_test::{ListExprIsUnimplementedWithListRepr,
  MapExprIsUnimplementedWithMapRepr,
  StringConstantIsUnimplementedWithStringRepr,
  IdentifierIsUnimplementedWithKindAndId}` all assert `HasSubstr` on
  the diagnostic so a future refactor that collapses distinct strings
  into one generic blurb will trip these tests.
- **Custom-section / ABI**: not written yet, so no tests either.  M2
  deliverable #3 above closes this.
- **Linux build portability**: the wasm32 cross-compile genrule in
  `compiler/runtime/BUILD.bazel` hardcodes the darwin-arm64 brew path
  `/opt/homebrew/opt/llvm/bin/clang`, so `bazel build
  //compiler/runtime:cel_runtime_wasm_file` fails on Linux (and on
  intel macs where brew lives at `/usr/local/opt/llvm`).  The test
  `//compiler/codegen:runtime_link_test` and the embedded-bytes
  `cc_library` are both tagged `manual` today, but any machine that
  wants to exercise the runtime on top of codegen — i.e. any CI box
  from M3 onward — needs a working cross-compile.  Authoring plan:
    - Detect the toolchain via a `select()` on `@platforms//os`
      (darwin → `/opt/homebrew/opt/llvm/bin/clang` *or*
      `/usr/local/opt/llvm/bin/clang`; linux → `/usr/bin/clang-17`
      from apt's `llvm-17` package, plus the matching `lld`), or
      register a proper `cc_toolchain` for wasm32 so the rule no
      longer shells out at all.
    - Drop `--export-all` in favour of an explicit `--export=cel_*`
      whitelist so a toolchain that auto-inserts synthetic symbols
      does not leak them into the emitted module.
    - CI: add a linux job that runs
      `bazel build //compiler/runtime:cel_runtime_wasm_file &&
       bazel test //compiler/codegen:runtime_link_test` so the
      cross-compile path is covered on both platforms.  Do *not*
      ship M3 until this is green on Linux — the merge step will
      make the cross-compile a hard dependency of every emitted
      module, not the opt-in it is today.

These are listed here (and not in `testing-checklist.md`) because they
belong to the "M2 is *really* done" bar rather than to ongoing
coverage.  Once closed, flip them in both places.

## Toolchain notes

- Binaryen is vendored as a tarball under `third_party/binaryen/`
  (bootstrapped via `third_party/fetch_binaryen.sh`) and built through
  its own CMakeLists.txt via `rules_foreign_cc`'s `cmake` rule.  We
  consume `libbinaryen.a` + `binaryen-c.h` — the officially stable
  public surface.  Neither Binaryen's C++ headers nor wasm-opt are
  linked into the compiler; the C API covers everything codegen needs.
- Cross-compilation uses brew `llvm`'s `clang --target=wasm32
  -ffreestanding -nostdlib -O2 -Wl,--no-entry -Wl,--export-all`
  (freestanding; the runtime is a pure-C library, not a WASI
  command).  `wasm-ld` is shipped in brew's separate `lld` formula,
  not bundled with `llvm`, so both are required on the build host.
- Wasmtime C API pulled in as a Bazel dep for e2e tests.
