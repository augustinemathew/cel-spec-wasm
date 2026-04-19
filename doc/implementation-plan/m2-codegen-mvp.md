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

- [ ] `compiler/runtime/cel_runtime.h` — `CelValue` 24-byte tagged union,
      `CelKind` enum (`CEL_NULL`, `CEL_BOOL`, `CEL_INT`, `CEL_UINT`,
      `CEL_DOUBLE`, `CEL_STRING`, `CEL_BYTES`, `CEL_LIST`, `CEL_MAP`,
      `CEL_MESSAGE`, `CEL_TIMESTAMP`, `CEL_DURATION`, `CEL_TYPE`,
      `CEL_UNKNOWN`, `CEL_ERROR`).  `CelString { const uint8_t* data; i32
      len; }`, `CelList { CelValue* items; i32 len; i32 cap; }`, `CelMap {
      CelValue* keys; CelValue* vals; i32 len; i32 cap; }`.  Constructor
      helpers: `cel_make_{null,bool,int,uint,double,string,bytes,message,
      unknown,error}`.
- [ ] `compiler/runtime/cel_runtime.c` — bump allocator (`cel_alloc`,
      `cel_reset`), constructors, `cel_string_eq`, `cel_bytes_eq`, all
      pure-linear-memory ops.
- [ ] `compiler/runtime/cel_refs.wat` — module-owned `$cel_refs` externref
      table + `cel_wrap_message`, `cel_unwrap_message`, `cel_ref_intern`.
- [ ] Build rule that cross-compiles the C to wasm32 via brew's `clang`
      (Apple clang has no wasm32 target).  Hermetic wrapper lives at
      `compiler/runtime/BUILD.bazel`.

### Codegen (Binaryen C++ API)

- [ ] `compiler/codegen/module.{h,cc}` — thin wrapper around
      `wasm::Module` holding the `$cel_refs` table, the imports table
      (`cel_host.*`, `cel_fn.*`), and the `eval` export.
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
- [ ] `compiler/codegen/expr_lower_test.cc` — per-`ExprKindCase` emission
      tests using Binaryen's module validator.
- [ ] `compiler/runtime/cel_runtime_test.cc` — native C++ tests linking
      `cel_runtime.c` as a host library (separate from the wasm32 build)
      to unit-test the allocator + constructors.
- [ ] End-to-end: `compiler/e2e/eval_test.cc` instantiates the generated
      module with a WASM runtime (wasmtime C API) and evaluates each
      smoke-test expression, comparing the result against a literal.

## Toolchain notes

- Binaryen is installed via brew (`/opt/homebrew/opt/binaryen`).  Use the
  Binaryen C++ API, not WABT.  Wrap headers via `http_archive` in
  `MODULE.bazel` or vendor under `third_party/binaryen`.  Decision pending.
- Cross-compilation uses brew `llvm`'s `clang --target=wasm32-wasi -O2
  -nostartfiles -Wl,--no-entry`.
- Wasmtime C API pulled in as a Bazel dep for e2e tests.
