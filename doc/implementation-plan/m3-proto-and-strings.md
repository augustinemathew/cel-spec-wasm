# M3 — Proto field reads + string ops

Status: **in progress** (started 2026-04-19; M2 closed).  Work is
sliced thin to land incremental e2e coverage:

- **Slice A+B** (2026-04-19, landed) — two-module host loader
  (`compiler/host/host_loader.{h,cc}`) + eval-module-side import
  declarations for the runtime's exports + string literals + string
  equality.
- **Slice C** (2026-04-19, landed) — scalar `kIdentExpr` lowering
  against typed `eval()` params; variable declaration via
  `CheckOptions::variable_specs` wired end-to-end; codegen reads
  `TypedAst::variables()` at lowering time.
- **Slice D** (2026-04-19, landed) — string operators: `_+_` on string
  routes to `cel_string_concat`; `_==_` / `_!=_` route to
  `cel_string_eq` / `cel_bytes_eq`; `size(string)` routes to
  `cel_string_size` (UTF-8 codepoint count per spec §1110).  Also
  fixed a latent string-literal bug (bytes were being written at
  arena-relative offsets treated as absolute); the fix introduced
  `cel_mem_base` as a runtime export and documented the
  arena-relative offset ABI in `doc/wasm-compiler-design.md` §7.1 /
  §7.4 / §8.1 / §10.1 / Appendix B.

- **Slice E** (2026-04-19, landed) — string member calls routed to
  three new runtime helpers (`cel_string_starts_with`, `…_ends_with`,
  `…_contains`); new `LowerStringMemberCall` isolates the member-call
  dispatch from `LowerCall`.  Spec edge cases enforced runtime-side.
- **Slice F** (2026-04-19, landed) — bytes constants + operators end
  to end.  `LowerStringLiteral` renamed to `LowerSpanLiteral` and now
  takes the constructor helper name; `LowerConstant` routes
  `ConstantKindCase::kBytes` through `cel_make_bytes_view`.  Bytes `+`
  lowers to a new `cel_bytes_concat` (the runtime helper factors out
  of `cel_string_concat` via a shared `span_concat`); `size(bytes)`
  lowers to a new `cel_bytes_size` (byte count per CEL §1110, not
  code-point count).  Bytes equality already worked via the existing
  `Repr::kBytes` branch of `LowerComparison` → `cel_bytes_eq`.
- **Slice G1** (2026-04-19, landed) — message params as externref
  + externref-table plumbing.  `Repr::kMessage` variables now lower
  to an `externref` param on the eval function (`WasmTypeFor(kMessage)
  == BinaryenTypeExternref()`).  When the eval module has at least
  one message variable, `LowerToEvalFunction` pulls in the
  `$cel_refs` table (16 initial slots, slot 0 reserved as null) and
  the three helper functions via `AddCelRefsTableAndHelpers`, plus
  `cel_wrap_message(externref) → i32` and
  `cel_unwrap_message(i32) → externref` via `AddMessageWrapHelpers`.
  The wrap/unwrap pair lives in `cel_refs.cc` so all externref-bearing
  IR is colocated; it takes a runtime-import dependency on
  `cel_make_message` and `cel_mem_base` (declared via
  `DeclareRuntimeImports` — `cel_make_message` added in this slice).
  The `kMsgSlotOffset = 8` byte offset of `payload.msg_slot` inside
  `CelValue` is pinned by a `_Static_assert` in the runtime, so
  `cel_unwrap_message` can do the load inline without a dedicated
  accessor.  Codegen coverage only at G1 — full e2e round-trip is
  deferred to G2 where `cel_host.get_field` is the first caller that
  actually exercises wrap/unwrap through a wasmtime instantiation.
  Coverage: `expr_lower_test::{MessageVariableLowersAsExternrefParam,
  MessageVariablePullsInCelRefsTableAndWrappers,
  NoMessageVariableMeansNoCelRefsTable}`;
  `cel_refs_test::{AddMessageWrapHelpersValidates,
  EmitsWrapAndUnwrapMessageFunctions, ExportsWrapAndUnwrapMessage}`.

Remaining slices before M3 closes:
- **Slice G2** — `kSelectExpr` (scalar + span fields) lowering via a
  unified `cel_host.get_field(externref msg, i32 field_number,
  i32 out_cv) → void` import.  Codegen pre-allocates a 24-byte
  `CelValue` in the arena via `cel_alloc(24)`, passes the
  arena-relative offset to the host as an out-param, and the host
  writes `{kind, payload}` in place.  **First G2 implementation
  decision (open):** how does codegen resolve a `SelectExpr`'s field
  *name* to a proto *field number* at lower time?  Research
  (2026-04-19): cel-cpp's `ast.reference_map()` only populates entries
  for `IdentExpr` / `CallExpr` / `StructExpr` — `SelectExpr` is
  **not** in the map, so the field number is not reachable from the
  checked AST alone.  The descriptor pool is loaded by
  `parse_and_check.cc::LoadDescriptorPool` but is currently *dropped*
  when `ParseAndCheck` returns (the `DescriptorPoolBundle` goes out
  of scope).  Three viable shapes:
    - **A. Retain the bundle in `TypedAst`.** Codegen gets a
      `const DescriptorPool*` accessor and does
      `pool->FindMessageTypeByName(fqn)->FindFieldByName(name)->number()`
      per SelectExpr.  Minimal change to existing APIs; pool is
      already computed.  Requires `TypedAst` to own the pool so its
      lifetime matches the AST.
    - **B. Pre-compute `{expr_id -> field_number}` during annotation
      seeding.** Walk `SelectExpr`s in `PopulateAnnotations`, resolve
      each through the pool while the pool is live, store in
      `NodeAnnotation` alongside `repr`.  Codegen becomes a pure
      lookup.  Matches the TODO at `annotations.h:41–45`
      (`attribute_id` / `pattern_id`).
    - **C. Thread the pool through `LowerToEvalFunction`.**
      Requires signature changes at every call site.
  Pick one before writing codegen.  Leaning B (scales naturally to
  attribute_id interning later), but A is the smaller step if G2
  needs to land fast.  For string/bytes fields, the
  host additionally allocates the span payload bytes via an
  imported-back `cel_alloc(len)` and fills
  `payload.s.{ptr,len}`; for message fields the host writes
  `kind=CEL_MESSAGE` + an interned slot into `payload.msg_slot`.
  `CEL_UNKNOWN` and `CEL_ERROR` travel through the same slot.
  Proto field numbers are emitted as codegen immediates (no new
  interning table — the externref already carries the descriptor).
  See "Open design questions" §1 for the rationale.
- **Slice G3** — `kSelectExpr` with `test_only = true` lowers to a
  separate `cel_host.has_field(externref, i32) → i32` import.
- **Slice G4** — nested-message select (compose G2 with
  `cel_unwrap_message`) + `_==_` on `Repr::kMessage` dispatching to
  `cel_host.message_eq(externref, externref) → i32`.
- **Checker integration** — pass a `--schema` file on the CLI,
  round-trip through a proto fixture under `compiler/e2e/testdata/`.

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

- [x] `cel_refs.wat` — the three helpers `cel_wrap_message`,
      `cel_unwrap_message`, `cel_ref_intern` against the private
      `$cel_refs` externref table.  Landed as Binaryen-IR emitters
      in `compiler/codegen/cel_refs.{h,cc}` (not WAT — `externref` has
      no C-source representation via the wasm32 cross-compile path).
      `AddCelRefsTableAndHelpers` emits the table plus
      `cel_ref_intern` / `cel_ref_get` / `cel_refs_reset`;
      `AddMessageWrapHelpers` emits `cel_wrap_message` /
      `cel_unwrap_message` on top.  Pulled in by `LowerToEvalFunction`
      only when the expression has at least one message variable
      (Slice G1).

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
      Slice G1 (2026-04-19) extends this to `message(T) → externref`
      params; reading a message ident is just `local.get N` at the
      `externref` type.  The `cel.abi` table's role is still just to
      encode the final ABI — codegen derives the param layout
      straight from `TypedAst::variables()` at lowering time.
- [ ] `kSelectExpr` (operand, field, not test_only) — lowers to a
      `cel_host.get_field` call followed by whatever unwrap or
      constructor is needed to match the checked field type.
- [ ] `kSelectExpr` with `test_only = true` — lowers to
      `cel_host.has_field`.  Macro expansion already turned
      `has(x.y)` into this shape during parsing (cel-cpp), so the
      checker sees exactly the right AST here.
- [x] `kCallExpr` for string operators (slices D + E, 2026-04-19):
      - `_+_` on string (concat) — `cel_string_concat`.
      - `size(string)` — `cel_string_size`.
      - `_==_` / `_!=_` on string — `cel_string_eq` (inverted with
        `i32.eqz` for `_!=_`).
      - `_.startsWith(_)`, `_.endsWith(_)`, `_.contains(_)` — member
        calls, routed to `cel_string_starts_with` /
        `cel_string_ends_with` / `cel_string_contains` via the new
        `LowerStringMemberCall` helper.  Empty-needle / longer-needle
        edge cases enforced runtime-side.
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

1. **Scalar field return ABI.** **Decided 2026-04-19 (ahead of
   Slice G2).**  Neither of the two original options.  Converged on
   a uniform out-parameter shape:
     - The module pre-allocates a 24-byte `CelValue` in the arena
       via `cel_alloc(24)` and passes its arena-relative offset
       (`out_cv: i32`) to `cel_host.get_field(externref msg,
       i32 field_number, i32 out_cv) → void`.
     - The host writes `kind` + the appropriate `payload` bytes
       in place.  For string/bytes fields the host additionally
       allocates the span payload bytes via an imported-back
       `cel_alloc(len)` and fills `payload.s.{ptr,len}`.  For
       message fields the host writes
       `{kind=CEL_MESSAGE, payload.msg_slot = intern(ref)}` into
       the slot and the module pulls the externref back out via
       `cel_unwrap_message`.
     - UNKNOWN and ERROR propagate through the same slot by
       writing `kind=CEL_UNKNOWN` / `CEL_ERROR`.  This is the
       **reason** neither of the original options works: both
       assumed the return shape is known statically from the field
       type, but unknown / error must be reachable at every field
       read.
     - Field numbers are emitted as codegen immediates; no new
       interning table (the externref carries the descriptor
       pool).  `has_field` and `message_eq` remain separate
       imports (slices G3 and G4).
   Design doc §8.2 still describes the pre-decision split ABI;
   that rewrite lands with Slice G2.
2. **Interned string pool layout.** All string constants live in the
   `data` segment, but the layout that makes `size()` cheap (a
   leading u32 length) costs a byte per unique string vs. passing
   `(ptr, len)` at every call site.  Cost analysis goes in design §9.
3. **Host-table injection.** The sh_test already sets `--var` and
   `--schema`; does `celwasmc` grow a `--emit-host-stub` flag that
   generates the host-side boilerplate in C/C++/Go?  Defer to M6
   unless an M3 user hits friction.
