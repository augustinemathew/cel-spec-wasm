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
- [ ] `compiler/runtime/cel_refs.wat` — module-owned `$cel_refs` externref
      table + `cel_wrap_message`, `cel_unwrap_message`, `cel_ref_intern`.
- [ ] Build rule that cross-compiles the C to wasm32 via brew's `clang`
      (Apple clang has no wasm32 target).  Hermetic wrapper lives at
      `compiler/runtime/BUILD.bazel`.  *(Host `cc_library` is already
      wired up; wasm32 target follows once codegen starts consuming it.)*

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
- [ ] `compiler/codegen/abi.{h,cc}` — emits the `cel.abi` custom section
      (type-id / attribute-id / pattern-id interning).
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

The three unchecked boxes above are the closing work for M2; none of them
is a blocker for scalar e2e but all three gate the start of M3.

1. **`compiler/runtime/cel_refs.wat`** — the module-owned `$cel_refs`
   externref table and its three helpers (`cel_wrap_message`,
   `cel_unwrap_message`, `cel_ref_intern`) are the load-bearing piece
   that lets a proto message cross the host boundary without a copy.
   Today `WasmModule::AddCelRefsTable` declares the table; no function
   has been authored that actually reads or writes it.  Authoring plan:
     - Write the three helpers in WAT, link them into the emitted module
       the same way the C runtime will be (currently both sides only
       exist on the host — no cross-compile yet).
     - Tests: positive — `cel_wrap_message(ref)` + `cel_unwrap_message`
       round-trip returns the identity externref; `cel_ref_intern` on
       two equal refs returns the same slot id (pointer-equality
       dedup).  Negative — intern-table overflow returns a sentinel,
       `cel_unwrap_message` on the null slot traps (or returns the
       null externref, TBD — document whichever we pick).
     - Every box in the "Runtime" gap list of
       `testing-checklist.md` that mentions `cel_ref_intern` /
       `cel_unwrap_message` flips off the back of this deliverable.

2. **wasm32 cross-compile rule for the C runtime.** Apple clang ships
   without the `wasm32-wasi` target, so the Bazel target
   `//compiler/runtime:cel_runtime_wasm` has to drive brew's `llvm`.
   Today only the native `cc_library` exists, which is enough for
   `cel_runtime_test` to run on the host but doesn't put any runtime
   bytes in the `.wasm` the compiler emits.  The generated module
   therefore has *no* dependency on the runtime yet — that changes as
   soon as `expr_lower` starts constructing `CelValue`s (first use is
   string constants in M3).  Authoring plan:
     - `genrule` or `rules_cc` toolchain that invokes
       `clang --target=wasm32-wasi -O2 -nostartfiles -Wl,--no-entry -c`
       over `cel_runtime.c`, producing `cel_runtime.wasm`.
     - `expr_lower` or a new `codegen/link.{h,cc}` merges the runtime
       module with the per-expression module via Binaryen's
       `BinaryenModuleAddFunctionImport` + symbol rewiring, OR we
       embed the runtime as a Binaryen-authored module built from the
       C-translated IR.  *(The merge strategy is one of the open
       design questions that needs a decision before M3.)*
     - Tests: the e2e test gains a case where the root expression
       constructs a non-trivial `CelValue` (e.g. a string constant) and
       asserts the returned pointer reads back as the expected kind +
       payload from linear memory.  This is a completely new class of
       e2e assertion — today's scalar tests only inspect
       `wasmtime_val_t.of.i64` etc.

3. **`compiler/codegen/abi.{h,cc}`** — emits the `cel.abi` custom
   section (design §9, appendix A) holding the serialized
   `CheckedExpr`, the type / attribute / pattern interning tables, and
   `MemoryLayout`.  Hosts MUST read this before instantiation so the
   custom section is on the critical path for every production host,
   but the e2e test today just looks at the `eval` return value and
   doesn't care about metadata.  Authoring plan:
     - A `CelAbi` proto message (mirror of appendix A).  Serialize it
       and call `BinaryenAddCustomSection(mod.raw(), "cel.abi", …)`.
     - The CLI grows a `--dump-abi` companion flag that pretty-prints
       the parsed `CelAbi` from a module.
     - Tests: `abi_test.cc` builds a module with interned types and
       attributes, reads the custom section back out, round-trips it,
       and asserts byte equality with a golden proto.  E2E gains a
       test that host-side `wasmtime_module_imports` / custom-section
       lookup finds `cel.abi` and it decodes without error.

### Testing gaps still open in M2

Even for the scalar slice that is "done", the coverage checklist has
unchecked boxes that a vigilant reader should close before calling M2
complete:

- **`RejectDyn`**: `uint`, `double`, `string`, `bytes`, `null_type`,
  `timestamp`, `duration`, wrapper rows in the per-type matrix are all
  `[ ]`.  The rejection logic in `static_subset.cc` is the same for
  every type — the tests just don't enumerate them.  Cheap backfill.
- **`kSelectExpr` (test_only from `has()`)**: no stage has a test row.
  Once M3 starts, at least one checker/annotation test per stage must
  land before codegen lowering is added.
- **`e2e` column for comparisons, negate, and `!`**: the rows exist in
  `testing-checklist.md` but the `e2e` box for `uint/double` specific
  comparisons is still `[ ]` — `IntComparisons` covers int; double
  covers `<` and `==`; `uint` has only `10u/3u` as a witness.  Add
  per-op cases to close the matrix.
- **Negative codegen tests with a good-message assertion**: the
  current negative tests in `expr_lower_test` only check the status
  code, not the error message.  CLAUDE.md is explicit: "rejected with
  a good message."  Add `testing::HasSubstr` assertions on the status
  `.message()` for each Unimplemented path so a future refactor that
  collapses distinct error strings into one generic one can't pass.
- **Custom-section / ABI**: not written yet, so no tests either.  M2
  deliverable #3 above closes this.

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
- Cross-compilation uses brew `llvm`'s `clang --target=wasm32-wasi -O2
  -nostartfiles -Wl,--no-entry`.
- Wasmtime C API pulled in as a Bazel dep for e2e tests.
