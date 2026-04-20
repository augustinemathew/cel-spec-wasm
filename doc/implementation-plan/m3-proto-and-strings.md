# M3 — Proto field reads + string ops

Status: **done** (started 2026-04-19; closed 2026-04-19).  Work was
sliced thin to land incremental e2e coverage; all slices A through
G4 plus CLI schema integration + richer e2e compose tests landed:

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
- **Slice G2 — field-number plumbing (2026-04-19, landed).**  Option B
  from the decision below shipped: `PopulateAnnotations` now walks
  every `SelectExpr` in `compiler/ir/typed_ast.cc::FieldNumberVisitor`
  and writes the resolved proto field number into
  `NodeAnnotation::field_number` while the descriptor pool is still
  live.  `ParseAndCheck` passes its `DescriptorPoolBundle::pool` into
  `PopulateAnnotations` (`compiler/frontend/parse_and_check.cc:279`)
  so the resolver runs on the real generated+user-schema pool before
  the bundle goes out of scope.  Field-number 0 is the sentinel for
  "not a SelectExpr" / "could not be resolved" (proto field numbers
  start at 1).  Codegen becomes a pure lookup: `annotations.at(id)
  .field_number`.  Coverage:
    - `typed_ast_test::G2ResolvesEveryProtoFieldKind` — table-driven
      unit test on a synthetic `celwasm.testg2.G2Msg` descriptor
      covering every proto wire type (int64, bool, string, int32,
      message, uint32, float, double, bytes, uint64, sint32,
      fixed32, repeated_int32, enum, fixed64, sfixed32, sfixed64,
      sint64).  Field numbers are deliberately non-contiguous so an
      off-by-one bug in the resolver cannot silently pass.
    - `typed_ast_test` edge cases: `test_only` SelectExprs still get
      numbered; unknown field leaves zero; unknown message type
      leaves zero; non-message operand (map) leaves zero; nested
      chain numbers every hop; null pool leaves every annotation at
      zero.
    - `parse_and_check_test::{SelectExprAnnotationCarriesFieldNumber,
      NestedSelectExprResolvesEachHop, HasMacroSelectExprCarriesFieldNumber,
      RepeatedFieldSelectResolvesFieldNumber,
      HasOnMapKeyLeavesFieldNumberZero,
      RejectsSelectOfUnknownFieldAtCheckTime}` —
      integration tests driving the real parser + checker +
      `PopulateAnnotations` against `google.protobuf.DescriptorProto`
      from the generated pool, so the table-driven unit coverage is
      matched by a round-trip through the production pipeline.

  **Coverage gap intentionally deferred:** no e2e (wasmtime) test yet,
  because the caller of `field_number` is slice G2's `cel_host.get_field`
  codegen, which has not landed.  When that codegen ships, add eval-
  level coverage that evaluates `msg.scalar_field == literal` under a
  wasmtime host stub; the field-number plumbing only becomes visibly
  correct / incorrect at eval time, so the e2e is the real end of the
  slice.

- **Slice G2 — `kSelectExpr` codegen (2026-04-19, landed).**  With
  field numbers in `NodeAnnotation`, codegen lowers a non-`test_only`
  `SelectExpr` on a `Repr::kMessage` operand to
  `cel_alloc(24)` → `cel_host.get_field(msg, field_number, out_cv) → void`
  → a per-Repr payload load.  The host writes `{kind, payload}` in
  place.  For string/bytes fields the host allocates the span payload
  bytes via `cel_alloc(len)` and fills `payload.s.{ptr,len}`; for
  message fields the host writes `kind=CEL_MESSAGE` + an interned slot
  into `payload.msg_slot`.  `CEL_UNKNOWN` and `CEL_ERROR` travel
  through the same slot.  Field numbers are emitted as immediates; no
  new interning table (the externref already carries the descriptor).
  Plumbing pieces:
    - `compiler/host/cel_host.{h,cc}` — runtime-agnostic
      `ReadField`/`HasField`/`MessageEq` over a
      `google::protobuf::Message*`.
    - `compiler/host/cel_host_wasmtime.{h,cc}` — wasmtime-specific
      trampolines that unwrap externref / i32 / memory and register
      three functions on a `wasmtime_linker_t` under the `"cel_host"`
      namespace: `get_field`, `has_field`, `message_eq`.
    - `compiler/codegen/expr_lower.cc` — `DeclareHostImports` emits
      the three imports unconditionally (per the "don't gate cel_*
      imports on AST inspection" rule).  `LowerSelect` produces the
      3-child block; `LoadSelectPayload` dispatches on `Repr` for the
      payload load (string/bytes pass through the scratch CelValue*;
      numeric/bool load from `cel_mem_base + scratch + 8`).
    - `compiler/host/host_loader.cc` — holds a
      `std::unique_ptr<CelHostEnv>` per `LoadedEval`; unique_ptr
      because `CelHostEnv` is non-movable (address captured as
      callback data).
    - `compiler/testdata/e2e_fixture.proto` — realistic `Customer`
      fixture with one scalar per CEL-relevant wire type
      (`string`, `int32`, `int64`, `uint32`, `uint64`, `double`,
      `bool`, `bytes`), plus an `Address` submessage staged for G4.
  **Coverage landed in this slice:**
    - `compiler/host/cel_host_test.cc` — table-driven unit tests over
      every wire type; exercises default/absent values too.
    - `compiler/e2e/eval_test.cc` — 10 proto e2e cases against
      `Customer`, running the full pipeline under wasmtime:
      `SelectProtoStringFieldEq`, `SelectProtoStringFieldNeq`,
      `SelectProtoStringFieldDefaultIsEmpty`,
      `SelectProtoStringFieldPassThrough`,
      `SelectProtoInt32FieldIsCelInt`,
      `SelectProtoInt64FieldCarriesLargeValue`,
      `SelectProtoUint64FieldIsUnsigned`,
      `SelectProtoUint32FieldIsCelUint`,
      `SelectProtoDoubleField`, `SelectProtoBoolField`,
      `SelectProtoBytesFieldRoundTrips`.  Nested-message select and
      `CEL_MESSAGE` payload dispatch are deferred to G4 — LowerSelect
      rejects non-`kMessage` operands with `Unimplemented` today.
- **Slice G3 — `has(msg.field)` codegen (2026-04-19, landed).**  CEL
  macro expansion lowers `has(msg.field)` to a `SelectExpr` with
  `test_only = true`; codegen now routes that to
  `cel_host.has_field(externref, i32) → i32` instead of the
  `get_field`/payload-load sequence used for plain field reads.
  `LowerSelect` in `compiler/codegen/expr_lower.cc` dispatches on
  `test_only`: the `true` branch emits a direct `has_field` call, the
  `false` branch keeps the G2 3-child block.  Both paths share a
  `LowerSelectOperand` helper for field-number resolution, operand
  `Repr::kMessage` check, and operand lowering — the refactor keeps
  each leg under the function-size gate without duplicating the
  validation.  The `has_field` trampoline was already registered by
  `cel_host_wasmtime.cc` during G2; G3 is pure codegen + tests.
  Proto3 presence semantics are delegated to the host
  (`google::protobuf::Reflection::HasField` for submessages;
  scalar-non-default for singular scalars).
  **Coverage landed in this slice:**
    - `compiler/e2e/eval_test.cc` — 7 new `has()` cases against
      `Customer`:
      `HasProtoStringFieldSetAndUnset`,
      `HasProtoInt32FieldSetAndUnset`,
      `HasProtoBoolFieldSetToFalseIsFalse` (pins the proto3
      scalar-at-default = not-present surprise),
      `HasProtoBytesFieldEmptyIsFalse`,
      `HasProtoMessageFieldRespectsExplicitPresence` (distinguishes
      submessage explicit-presence from scalar default),
      `HasComposesWithLogicalNot`,
      `HasAndFieldCompareTernary` (composes G2 field read + G3 has()
      through the ternary lowering to catch scratch-local aliasing
      between the two select paths).
    - `compiler/host/cel_host_test.cc` — existing `HasField*` table
      coverage still applies; no new unit tests needed because G3 is
      a codegen-only addition over an already-tested host primitive.
- **Slice G4 — nested-message select + message equality (2026-04-19, landed).**
  Two small extensions to G2 codegen that together let nested SelectExprs
  round-trip and `_==_` accept two `Repr::kMessage` operands:
    1. `LoadSelectPayload` gained a `Repr::kMessage` arm that calls
       `cel_unwrap_message(cv)` on the scratch CelValue pointer, turning
       the host-written `payload.msg_slot` back into the externref the
       rest of codegen expects for message values.  The scratch offset
       is passed through verbatim — `cel_unwrap_message` does the
       `cel_mem_base + cv + 8` msg_slot load itself, so pre-loading the
       slot at the call site and handing it in would treat the slot
       index as a CelValue address and walk off the arena.  Caught in
       the first run of the G4 e2e suite (`got=0 expected=1` on every
       nested test); fixed before commit.
    2. `LowerComparison` grew a `Repr::kMessage` branch that emits
       `message_eq(lhs, rhs)` for `==` and wraps the result in
       `i32.eqz` for `!=`.  The existing `DeclareHostImports` already
       declared `message_eq` in Slice G2, so no import plumbing was
       needed.
  Host plumbing: `host_loader.cc` now calls
  `CelHostEnv::BindEvalInterner(eval_instance)` after
  `wasmtime_linker_instantiate`, tolerant of `absl::NotFound` for
  scalar-only evals that never pulled in `AddCelRefsTableAndHelpers`.
  Without that bind step the `cel_ref_intern` func handle on
  `CelHostEnv` stayed zero-initialised and any `get_field` call that
  produced a submessage tripped a wasmtime "object used with the wrong
  store" panic inside `InternMessageViaRefIntern`.
  **Coverage landed in this slice:**
    - `compiler/e2e/eval_test.cc` — 6 new cases against `Customer`:
      `NestedSelectReadsInnerField` (`c.billing_address.city ==
      "Seattle"`), `NestedSelectThroughUnsetSubmessageReadsDefault`
      (default-of-unset composes with the inner select),
      `MessageEqualityTrueForStructurallyEqual`,
      `MessageEqualityFalseForDifferentFields`,
      `MessageInequalityInvertsMessageEq`,
      `NestedMessageEqualityComposesSelectAndEq`
      (`a.billing_address == b.billing_address`).
    - `cel_host_test.cc` — `MessageEq` table coverage from Slice G1
      still applies; the G4 add is codegen-only over that host
      primitive.
- **Richer e2e coverage (2026-04-19, landed).**  Pure test-only slice
  over the already-shipped G2/G3/G4 codegen, filling out the
  multi-param + mixed-stage matrix that the per-slice suites each
  covered in isolation but no single test exercised together.  Seven
  new cases in `compiler/e2e/eval_test.cc`:
    - `MultiParamTwoMessagesConcatenateNames` — two `Customer` externref
      params composed through string concat (`a.name + " & " + b.name`).
    - `MultiParamMessagePlusScalarUintComparison` — Customer message +
      scalar uint param together (`c.balance_cents > threshold`);
      asserts both branches.
    - `MultiParamTwoMessagesConditional` — ternary selecting between
      two message params' string fields (`a.is_premium ? a.name :
      b.name`); both branches.
    - `SizeOfProtoStringField` — `size(c.name)` with UTF-8 payload
      `"héllo"` asserting 5 codepoints (pins G2 string-payload + §1110
      codepoint semantics together).
    - `ProtoStringFieldStartsWith` — `c.name.startsWith("Ad")` (G2
      field read + slice E member call composed).
    - `ProtoIntFieldArithmetic` — `c.user_id + 100` with int32 field
      (pins scalar-from-proto-field promotion into a plain i64 add).
    - `NestedProtoStringFieldConcatWithLiteral` — G4 nested select
      composed with a string literal (`c.billing_address.city + ", " +
      c.billing_address.country`).
  A test attempting to thread a host-side C string as an eval param
  (`"Hello, " + c.name`) was dropped during this slice: `CallEval`
  invokes `cel_reset` internally so any host-side `cel_alloc` before
  the call is wiped.  Passing host strings as params is a host SDK
  design problem (Slice H / task #41), not an M3 e2e concern.
- **Checker integration (2026-04-19, landed).** Two CLI entry points
  for supplying message definitions, both funnelling into the same
  `DescriptorPool` + checker type env via `CheckOptions`:
    - `--schema <file.proto>` — textual `.proto` source.  Parsed
      in-process with `google::protobuf::compiler::Parser`.  Imports
      other than CEL well-known types are not resolved at parse time
      — the resulting `FileDescriptorProto` is handed to a
      `SimpleDescriptorDatabase` which is merged with the generated
      pool, so references to WKTs (duration, timestamp, wrappers)
      still resolve, but non-WKT imports surface as "unknown type"
      further down.  That's the point where the user should switch
      to `--schema_descriptorset` for multi-file schemas.
    - `--schema_descriptorset <file.pb>` — pre-compiled
      `google.protobuf.FileDescriptorSet` (output of
      `protoc --descriptor_set_out=...`).  Preferred for multi-file
      schemas or when the caller already runs `protoc` in their
      build.
  Flag names use underscores to match the rest of the absl-flags CLI
  (`--emit_wasm`, etc.).  At most one of the two may be set;
  `CheckOptions` surfaces a dedicated `InvalidArgument` error
  otherwise.  `CheckOptions::schema_path` was replaced by
  `schema_proto_path` + `schema_descriptor_set_path` — the only
  in-tree caller (the CLI) and the one test that exercised it were
  updated in the same change.
  **Coverage landed in this slice:**
    - `parse_and_check_test.cc` — 4 new cases:
      `RejectsSchemaDescriptorSetNotFound`,
      `RejectsProtoSourceNotFound`, `RejectsBothSchemaFlags`,
      `ProtoSourceSchemaRegistersMessage`,
      `ProtoSourceSchemaResolvesNestedMessageField`,
      `ProtoSourceSchemaRejectsSyntaxError`.
    - `cli/emit_wasm_test.sh` — positive case running the CLI with
      `--schema=e2e_fixture.proto --var=c:celwasm.testdata.Customer
      --check -e 'c.name'`, and a negative case asserting that
      setting both `--schema` and `--schema_descriptorset` is
      rejected at the flag plumbing boundary.

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
  - list / map / struct literals and comprehensions — **M5**.
  - substring / format / regex — **M7**.
  - overflow, divide-by-zero, unknown — **M4**.

## Deliverables

### Runtime (linear-memory side, wasm32-compiled C)

- [x] Wire the wasm32-cross-compiled `cel_runtime.wasm` (landed in
      M2) into the **two-module runtime/eval architecture** decided
      2026-04-19 (see `doc/wasm-compiler-design.md` §7.0).  The
      runtime is **not** merged into per-expression modules; it is
      instantiated once by the host and its exports become imports
      of every eval module under the `"cel"` namespace.  Scope of
      this deliverable (landed in Slice A+B, 2026-04-19):
        - Codegen emits `BinaryenAddFunctionImport` /
          `BinaryenAddMemoryImport` / `BinaryenAddTableImport`
          calls for exactly the runtime surface the expression
          uses.  Note: the imports are emitted **unconditionally**
          (not AST-gated) — see the "no lazy tracking of runtime
          imports" feedback entry.
        - Host loader shipped at `compiler/host/host_loader.{h,cc}`
          (not `compiler/runtime/`; co-located with the cel_host
          trampolines for G2).  Owns the wasmtime "instantiate
          runtime, hand exports to linker, instantiate eval"
          sequence plus `BindEvalInterner` (added in G4).
        - The e2e harness in `compiler/e2e/` runs every case through
          the two-module shared-linker path.
- [x] String / bytes constructors + equality authored in M2; slice D
      added **concat** (`cel_string_concat`) and **size**
      (`cel_string_size` — UTF-8 codepoint count per spec §1110),
      slice F added the bytes counterparts (`cel_bytes_concat`,
      `cel_bytes_size` — byte count, not codepoint count — factored
      through a shared `span_concat`).
- [~] `cel_bool_from_value(CelValue*)` — **not shipped, not needed.**
      The original design assumed `has()` would lower to a host call
      returning `CelValue*`, then the module would unbox the bool.
      The shipped G3 ABI has `cel_host.has_field` return `i32` 0/1
      directly, so there is no CelValue to unbox.  Other bool-
      consumers (ternary cond, `&&` / `||`) also operate on `i32`
      directly today.  If a future slice adds a host import that
      returns a `CelValue*` of kind `CEL_BOOL`, add this helper then;
      not before.

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

The design doc §8.2 fixes these; M3 is where we first implement them.
**All three landed; decision history retained below for posterity.**

- [x] `cel_host.get_field(externref msg, i32 field_number, i32 out_cv)
      → void` — reads a field.  Decision: unified out-parameter (the
      "Open design questions" entry at the bottom of this doc captures
      the reasoning — neither "externref | CelValue*" nor
      "host returns scalar, module constructs" survived contact with
      the UNKNOWN / ERROR requirement).  Module pre-allocates a 24-byte
      CelValue via `cel_alloc(24)`, passes the arena-relative offset,
      host writes `kind + payload` in place.  Landed in Slice G2
      (2026-04-19).
- [x] `cel_host.has_field(externref msg, i32 field_number) → i32` —
      returns 0/1 directly.  Landed in Slice G3 (2026-04-19); proto3
      presence semantics (scalar-at-default = not-present,
      submessage-explicitly-set = present) delegated to
      `google::protobuf::Reflection::HasField`.  `has()` on a map is
      a different operation (map-key presence) and remains deferred
      to M5.
- [x] `cel_host.message_eq(externref a, externref b) → i32` — message
      equality.  Landed in Slice G4 (2026-04-19); delegated to host
      `google::protobuf::util::MessageDifferencer`-style equality per
      spec §1110.

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
- [x] `kSelectExpr` (operand, field, not test_only) — landed in
      Slice G2 (scalar + string/bytes payload loads) and Slice G4
      (message payload load via `cel_unwrap_message` for nested
      selects).  Per-`Repr` dispatch lives in `LoadSelectPayload`.
- [x] `kSelectExpr` with `test_only = true` — lowers to
      `cel_host.has_field`.  Macro expansion already turned
      `has(x.y)` into this shape during parsing (cel-cpp), so the
      checker sees exactly the right AST here.  Landed in Slice G3
      (2026-04-19).
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
- [x] `kConstant` for **string** and **bytes** constants — Slice A+B
      landed the string path via `LowerStringLiteral` (now
      `LowerSpanLiteral`); Slice F landed bytes by generalising the
      same helper to take the constructor name
      (`cel_make_string_view` vs `cel_make_bytes_view`).  Literal
      bytes are stored through a `cel_mem_base + scratch` absolute
      pointer while the view constructor takes the arena-relative
      offset (the string-literal absolute/relative-offset bug that
      this approach fixes is documented in the testing-checklist.md
      Slice D block).
- [x] Eval-function signature revision: Slice C generalised
      `CallNullaryEval` to `CallEval(args)` and made the return
      type follow the top-level expression's `Repr` — scalar reprs
      return their native wasm type (i32 / i64 / f64), string /
      bytes / message reprs return `i32` (arena-relative CelValue*)
      or `externref` for messages.  `cel.abi.MemoryLayout` carries
      the per-param wasm type and return shape so hosts can
      typecheck the call; M2's `abi_test` round-trips it.

### CLI

- [x] `celwasmc --emit_wasm` already accepts `--var` and `--schema`
      from M1; this milestone exercises those for the first time in
      codegen.  As of 2026-04-19 `--schema` takes a textual `.proto`
      source file (parsed in-process via
      `google::protobuf::compiler::Parser`) and a new
      `--schema_descriptorset` takes a binary
      `google.protobuf.FileDescriptorSet` (the output of
      `protoc --descriptor_set_out=...`).  Both land the same
      messages in the same `DescriptorPool` and are mutually
      exclusive.  sh_test coverage: positive case with `--schema`
      + `--var c:celwasm.testdata.Customer -e 'c.name'`; negative
      case asserting that passing both flags is rejected.
      **Follow-up (not blocking M3):** add a sh_test that emits a
      .wasm with `--var request:celwasm.testdata.Customer` and
      inspects the imports list for the expected `cel_host.*`
      entries — and add a cross-equivalence test that `--schema x.proto`
      and `--schema_descriptorset x.pb` produce identical bytes
      for the same expression.

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
| multi-param eval (proto+proto, proto+scalar) | [x] | [x] |

New e2e test cases (all under `compiler/e2e/eval_test.cc`):

- [x] **String constant round-trip** — landed in Slice A+B as
      `StringLiteralRoundTripsThroughMemory` + `EmptyStringRoundTrips`
      (reads the `CelValue` + span back out of wasmtime memory
      via `cel_mem_base`).
- [x] **String concatenation** — landed in Slice D as
      `StringConcatenationProducesJoinedBytes` +
      `StringConcatenationEmptyLhs`.
- [x] **String equality (positive + negative)** — landed in Slice D
      as `StringEqualityPositiveAndNegative` +
      `StringInequalityInvertsEquality`.
- [x] **Proto field read (scalar)** — landed in Slice G2 across 10
      `SelectProto*Field*` cases covering every CEL-relevant wire
      type on the `Customer` fixture.
- [x] **Proto field read (message)** — landed in Slice G4 as
      `NestedSelectReadsInnerField` and
      `NestedSelectThroughUnsetSubmessageReadsDefault`; richer
      compose-test `NestedProtoStringFieldConcatWithLiteral`
      landed in task #40.
- [x] **`has()` positive + negative** — against both set and unset
      fields.  Landed in Slice G3 (2026-04-19); see the 7 `Has*` cases
      in `compiler/e2e/eval_test.cc`.
- [x] **Message-equality via host** — landed in Slice G4 as
      `MessageEqualityTrueForStructurallyEqual`,
      `MessageEqualityFalseForDifferentFields`,
      `MessageInequalityInvertsMessageEq`, and
      `NestedMessageEqualityComposesSelectAndEq`.

Negative tests that must land — **deferred to follow-up slice.**
Each is a thin sh_test against the CLI; nothing unblocked by
waiting, but the current suite is entirely positive and does not
exercise the "codegen exits cleanly on an unsupported shape"
invariant.  Track these in task #42 when created; do not gate
M3 closure on them.

- [ ] `kSelectExpr` on a non-message receiver fails in the checker
      (already checked; assert here that M3's codegen never sees this
      AST so an invariant is preserved).
- [ ] String concat of a non-string operand — checker error; verify
      celwasmc exits with the checker's diagnostic, not a codegen
      ICE.
- [ ] A field whose type is unsupported in M3 (e.g. a `repeated`
      field, since lists are M5) surfaces a **codegen** `Unimplemented`
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
   Design doc §8.2 rewrite landed 2026-04-19 alongside the M3 docs
   sweep — it now documents the unified out-parameter shape as the
   normative ABI.
2. **Interned string pool layout.** All string constants live in the
   `data` segment, but the layout that makes `size()` cheap (a
   leading u32 length) costs a byte per unique string vs. passing
   `(ptr, len)` at every call site.  Cost analysis goes in design §9.
3. **Host-table injection.** The sh_test already sets `--var` and
   `--schema`; does `celwasmc` grow a `--emit-host-stub` flag that
   generates the host-side boilerplate in C/C++/Go?  Defer to M6
   unless an M3 user hits friction.
