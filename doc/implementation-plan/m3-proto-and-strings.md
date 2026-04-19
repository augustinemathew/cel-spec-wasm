# M3 — Proto field reads + string ops

Status: **planned.**  Unblocked by M2 closing (`cel_refs.wat`, wasm32
cross-compile, `cel.abi` custom section).

## Scope

Turn the MVP eval module from "a scalar calculator" into "a CEL
expression evaluator that can read data from a host-provided proto
message and compare / concatenate strings."  This is the smallest slice
that forces us to exercise **linear-memory-resident `CelValue`s**,
**the externref table**, and **host imports** together, which in turn
surfaces every piece of the ABI.

Concretely, after M3 the following CEL expressions must round-trip
through `celwasmc --emit_wasm` and evaluate correctly under wasmtime:

  - `request.path == "/login"` — two string equality against a proto
    field read.
  - `has(request.user)` — `SelectExpr.test_only` / `has()` macro.
  - `request.user.name + "!"` — nested field select + string
    concatenation (design §10.1).
  - `size(request.path) > 0` — `size()` overload for string.
  - `"hello".startsWith("he")` — member-call string ops (design §11).

Out of scope (later milestones):
  - list / map / struct literals and comprehensions — **M4**.
  - substring / format / regex — **M7**.
  - overflow, divide-by-zero, unknown — **M5**.

## Deliverables

### Runtime (linear-memory side, wasm32-compiled C)

- [ ] Wire the wasm32-cross-compiled `cel_runtime.wasm` (landed in
      M2) into the **two-module runtime/eval architecture** decided
      2026-04-19 (see `doc/wasm-compiler-design.md` §7.0).  The
      runtime is **not** merged into per-expression modules; it is
      instantiated once by the host and its exports become imports
      of every eval module under the `"cel"` namespace.  Scope of
      this deliverable:
        - Codegen emits `BinaryenAddFunctionImport` /
          `BinaryenAddMemoryImport` / `BinaryenAddTableImport`
          calls for exactly the runtime surface the expression
          uses (walk the IR once, dedupe, emit).
        - A host loader in `compiler/runtime/host_loader.{h,cc}`
          that owns the wasmtime-specific "instantiate runtime,
          hand exports to linker, instantiate eval" sequence so
          embedders get a one-call API.
        - The e2e harness in `compiler/e2e/` switches from
          "instantiate one module" to "instantiate runtime + eval
          against a shared linker".
- [ ] String / bytes constructors + equality are already authored;
      add **concatenation** (`cel_string_concat`) and **length**
      (`cel_string_size` — UTF-8 code-point count, per spec §1110
      which defines `size(string)` as code-point count not byte count).
- [ ] `cel_bool_from_value(CelValue*)` — the inverse of
      `cel_make_bool` — pulls the i32 back out.  Needed for the
      codegen pattern where we lower a `SelectExpr.test_only` to a
      host call returning a `CelValue*` that we then need to test.

### Runtime (externref side, WAT / Binaryen)

- [ ] `cel_refs.wat` — the three helpers `cel_wrap_message`,
      `cel_unwrap_message`, `cel_ref_intern` against the private
      `$cel_refs` externref table.  This is technically an M2 carryover
      (see m2-codegen-mvp.md §Remaining for M2); repeated here because
      field reads can't happen without it.

### Host imports (declared by the module, implemented by the host)

The design doc §12 fixes these; M3 is where we first implement them.

- [ ] `cel_host.get_field(externref msg, i32 field_id) → externref |
      CelValue*` — reads a field.  Field numbering is stable and
      interned in the `cel.abi` attribute table.  Return shape depends
      on the field type: proto-message → externref (wrapped with
      `cel_wrap_message` on the module side); scalar → a `CelValue*`
      into linear memory (which the host allocates via an import-back
      call to `cel_alloc`, OR — cleaner — the host returns just the
      raw scalar and codegen constructs the `CelValue` on the module
      side).  **Decide between these two ABIs before implementing**;
      today the design doc leans toward "host returns scalar, module
      constructs"; flip the decision in both `../wasm-compiler-design.md`
      §12 and here when it's made.
- [ ] `cel_host.has_field(externref msg, i32 field_id) → i32` — the
      backing for `has()` on a proto field.  Returns 0/1.  Note:
      `has()` on a map is different (checks key presence) and lands
      in M4 alongside maps.
- [ ] `cel_host.message_eq(externref a, externref b) → i32` — message
      equality (delegated to host because the spec requires descriptor
      awareness for unknown fields).

### Codegen

- [x] `kIdentExpr` — lookup against the eval function's parameter
      list.  Slice C (2026-04-19) wires variables declared in
      `CheckOptions::variable_specs` as typed params on the eval
      function in declaration order; `kIdentExpr` lowers to
      `local.get N` against that slot.  Scalar-ABI variables (bool,
      int, uint, double, string, bytes as offsets) are supported.
      `message(T)` → externref still pending and gated on the
      externref-bearing codegen that lands alongside proto field
      reads.  The `cel.abi` table's role is still just to encode the
      final ABI — codegen derives the param layout straight from
      `TypedAst::variables()` at lowering time.
- [ ] `kSelectExpr` (operand, field, not test_only) — lowers to a
      `cel_host.get_field` call followed by whatever unwrap or
      constructor is needed to match the checked field type.
- [ ] `kSelectExpr` with `test_only = true` — lowers to
      `cel_host.has_field`.  Macro expansion already turned
      `has(x.y)` into this shape during parsing (cel-cpp), so the
      checker sees exactly the right AST here.
- [x] `kCallExpr` for string operators (slice D, 2026-04-19):
      - `_+_` on string (concat) — `cel_string_concat`.
      - `size(string)` — `cel_string_size`.
      - `_==_` / `_!=_` on string — `cel_string_eq` (inverted with
        `i32.eqz` for `_!=_`).
      - `_.startsWith(_)`, `_.endsWith(_)`, `_.contains(_)` — member
        calls; deferred to slice E.
- [ ] `kConstant` for **string** and **bytes** constants — emit a
      data segment entry + a `cel_make_string_view` (or
      `cel_make_bytes_view`) call against the interned offset and
      length.
- [ ] Eval-function signature revision: the return type is no longer
      guaranteed scalar.  The function now returns either a scalar
      (backwards-compatible) or an `i32` (the `CelValue*` in linear
      memory).  The `cel.abi.MemoryLayout.return_shape` field tells
      the host which to expect — document the enum value names in
      the design doc when this lands.

### CLI

- [ ] `celwasmc --emit_wasm` already accepts `--var` and `--schema`
      from M1; this milestone exercises those for the first time in
      codegen.  Regression-test with a schema whose message has nested
      messages and repeated scalar fields.  No new flag work expected,
      but the sh_test gains a case that passes `--var request:my.pkg.Request`
      and inspects the emitted module's imports list.

## Testing obligations

Before M3 is marked done, the following rows in `testing-checklist.md`
must flip to `[x]`:

| Type            | codegen | e2e eval |
| --------------- | :-----: | :------: |
| `string`        | [x]     | [x]      |
| `bytes`         | [x]     | [x]      |
| proto message   | [x]     | [x]      |

| Variant         | codegen | e2e |
| --------------- | :-----: | :-: |
| `kIdentExpr`    | [x]     | [x] |
| `kSelectExpr` (field) | [x] | [x] |
| `kSelectExpr` (test_only, from `has()`) | [x] | [x] |
| `kCallExpr` (member) | [x] | [x] |

New e2e test cases (all under `compiler/e2e/eval_test.cc`):

- [ ] **String constant round-trip** — `Evaluate("\"hi\"")` returns a
      pointer; the test reads the `CelValue` struct and its `CelSpan`
      payload out of wasmtime-exported memory, asserts
      `kind == CEL_STRING`, span length = 2, bytes = `"hi"`.
- [ ] **String concatenation** — `Evaluate("\"a\" + \"b\"")` reads
      back as `"ab"`.
- [ ] **String equality (positive + negative)** — `"hi" == "hi"` → 1,
      `"hi" == "bye"` → 0.
- [ ] **Proto field read (scalar)** — constructs a test-only message
      on the host side, passes it as an externref, evaluates
      `msg.some_int == 42`, asserts `1`.
- [ ] **Proto field read (message)** — nested select
      `msg.user.name == "alice"`.
- [ ] **`has()` positive + negative** — against both set and unset
      fields.
- [ ] **Message-equality via host** — `msg1 == msg2` with the same
      underlying proto bytes.

Negative tests that must land:

- [ ] `kSelectExpr` on a non-message receiver fails in the checker
      (already checked; assert here that M3's codegen never sees this
      AST so an invariant is preserved).
- [ ] String concat of a non-string operand — checker error; verify
      celwasmc exits with the checker's diagnostic, not a codegen
      ICE.
- [ ] A field whose type is unsupported in M3 (e.g. a `repeated`
      field, since lists are M4) surfaces a **codegen** `Unimplemented`
      error mentioning the field FQN.  This is the "fails with a good
      message" counterpart for the field-read path.

## Toolchain / dep notes

- The wasm32 runtime link is the first time the compiler actually
  merges two Binaryen modules.  Evaluate `BinaryenModuleAdd*` vs. a
  custom link step in `codegen/link.{h,cc}`; document the decision in
  the design doc §10 when made.
- The e2e test gains a minimal proto schema under
  `compiler/e2e/testdata/`.  Generate it from a committed `.proto`
  file via `proto_library` + `cc_proto_library`; do not hand-pack
  the bytes.  This fixture is used both to run the checker
  (`--schema=...`) and to build messages on the host side.

## Open design questions

1. **Scalar field return ABI.** Host returns the scalar, module
   wraps into a `CelValue` — OR — host allocates a `CelValue` via
   imported `cel_alloc` and returns the pointer.  Current lean:
   former.  Decide before authoring `cel_host.get_field`.
2. **Interned string pool layout.** All string constants live in the
   `data` segment, but the layout that makes `size()` cheap (a
   leading u32 length) costs a byte per unique string vs. passing
   `(ptr, len)` at every call site.  Cost analysis goes in design §9.
3. **Host-table injection.** The sh_test already sets `--var` and
   `--schema`; does `celwasmc` grow a `--emit-host-stub` flag that
   generates the host-side boilerplate in C/C++/Go?  Defer to M6
   unless an M3 user hits friction.
