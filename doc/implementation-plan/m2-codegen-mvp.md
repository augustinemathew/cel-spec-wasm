# M2 — WASM codegen MVP

Status: **in progress** (started 2026-04).

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

- [ ] `third_party/fetch_binaryen.sh` + `third_party/binaryen.sha` —
      bootstraps a pinned Binaryen release tarball under
      `third_party/binaryen/`.  Mirrors the cel-cpp vendoring pattern.
- [ ] `third_party/binaryen/BUILD.bazel` — `cmake()` rule (via
      `rules_foreign_cc`) that drives Binaryen's own CMakeLists.txt and
      exports a `cc_library` with `libbinaryen.a` + `binaryen-c.h`.
- [ ] `compiler/codegen/module.{h,cc}` — thin wrapper over
      `BinaryenModuleRef` holding the `$cel_refs` table, the imports
      table (`cel_host.*`, `cel_fn.*`), and the `eval` export.
- [ ] `compiler/codegen/expr_lower.{h,cc}` — dispatches on
      `cel::ExprKindCase` and `Repr` to emit expression WASM.
- [ ] `compiler/codegen/abi.{h,cc}` — emits the `cel.abi` custom section
      (type-id / attribute-id / pattern-id interning).
- [ ] CLI: `celwasmc --emit-wasm out.wasm -e "<expr>" [--check …]` writes a
      complete module.

### Tests (google-style `cc_test`)

- [ ] `compiler/ir/annotations_test.cc` — one test per `Repr` value +
      `ReprName` coverage.
- [ ] `compiler/ir/typed_ast_test.cc` — cover every `TypeSpec` variant in
      `ReprOf()` (primitive × 6, wrapper × 6, well-known × 3, null, list,
      map, message, type, dyn, error).
- [ ] `compiler/ir/static_subset_test.cc` — cover every `ExprKindCase` in
      both "all typed" and "some node DYN" configurations.
- [ ] `compiler/frontend/parse_and_check_test.cc` — one test per primitive,
      list, map, and message spec parse; negative cases for bad spec, bad
      type name, trailing garbage.
- [ ] `compiler/codegen/binaryen_smoke_test.cc` — proves the Binaryen
      integration is reachable from Bazel: `BinaryenModuleCreate`,
      add a function returning `i32.const 0`, `BinaryenModuleWrite`,
      assert bytes start with `\0asm\x01\x00\x00\x00` and that
      `BinaryenModuleValidate` reports OK.
- [ ] `compiler/codegen/module_test.cc` — `cc_library` wrapper builds
      the expected shape of a real eval module (memory + cel_refs
      table + imports + exports) and `BinaryenModuleValidate` is OK.
- [ ] `compiler/codegen/expr_lower_test.cc` — per-`ExprKindCase`
      emission tests: each lowered function round-trips through
      `BinaryenModuleValidate`, and instruction shapes match a
      hand-written golden (via `BinaryenModulePrint` textproto).
- [x] `compiler/runtime/cel_runtime_test.cc` — 38 gtest cases covering
      struct size invariant, allocator alignment / reset / OOM, every
      `cel_make_*` constructor (singletons + per-kind payload), and
      string/bytes equality on empty/equal/differing-length/differing-
      content/cross-kind/zero-offset inputs.
- [ ] End-to-end: `compiler/e2e/eval_test.cc` instantiates the generated
      module with a WASM runtime (wasmtime C API) and evaluates each
      smoke-test expression, comparing the result against a literal.

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
