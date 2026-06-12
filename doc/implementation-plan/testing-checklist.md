# Testing checklist

Compilers fail silently.  The grid below is the minimum coverage the project
must keep green; tick items off as the corresponding `cc_test` lands.
Negative tests are as important as positive ones — every row needs both a
"this works" and a "this fails with a good message".

Conventions:
  - `compiler/<path>_test.cc` is Google Test (`@com_google_googletest//:gtest_main`).
  - End-to-end (wasm-executing) tests live under `compiler/e2e/`.
  - Each box is `[ ]` when pending, `[x]` when a committed test covers it.

## Gap summary (updated 2026-04-19)

The detailed grids below have the authoritative per-cell state; this
block is the triage view a reviewer can read in ten seconds.

**Closed by M2:**
  - `kConstant` + `kCallExpr` (global) across parser / checker /
    annotations / RejectDyn / codegen / e2e for `bool` / `int` /
    `uint` / `double`.
  - Short-circuit `&&` / `||` / `?:` — codegen + e2e.
  - Runtime allocator + every scalar `cel_make_*` constructor.
  - The CLI (`--emit_wasm` positive + negative).
  - Per-type `RejectDyn` acceptance for every primitive / wrapper /
    `null_type` / `timestamp` / `duration` / `any`
    (`static_subset_test::{AcceptsEveryPrimitiveAtRoot,
    AcceptsEveryPrimitiveWrapperAtRoot,
    AcceptsNullTimestampDurationAny}`).
  - `kSelectExpr (test_only, from has())` — parser / checker /
    annotations row closed via
    `parse_and_check_test::HasMacroLowersToTestOnlySelectExpr`; the
    `RejectDyn` row closed via
    `static_subset_test::{TestOnlySelectExprIsAcceptedWhenOperandTyped,
    DynOperandInTestOnlySelectIsRejected}`.  Codegen + e2e remain
    deferred until M3 wires up proto fields and strings.
  - Codegen negative tests now assert `HasSubstr` on the diagnostic
    message so a regression that collapses distinct Unimplemented
    strings into one generic blurb fails
    (`expr_lower_test::{ListExprIsUnimplementedWithListRepr,
    MapExprIsUnimplementedWithMapRepr,
    UnsupportedVariableReprFailsWithSpecName}`).  The
    `StringConstantIsUnimplementedWithStringRepr` and
    `IdentifierIsUnimplementedWithKindAndId` entries from M2 are gone —
    both surfaces ship positive lowerings as of M3 slices A and C.
  - Per-op e2e comparison coverage for `uint` and `double`, plus
    `double` negate (`eval_test::{UintComparisons, DoubleComparisons,
    DoubleNegate}`).

**Still open inside M2** (the gating items for calling M2 done):
  - `cel_ref_intern` / `cel_ref_get` / `cel_refs_reset` — closed via
    `compiler/codegen/cel_refs.{h,cc}`, emitted from Binaryen C API.
    Covered by `cel_refs_test.cc` (structure + export shape) and
    `compiler/e2e/cel_refs_e2e_test.cc` (wasmtime round-trip:
    intern → get returns host payload, second intern advances slot,
    reset rewinds, slot 0 is null sentinel).
  - wasm32 cross-compile of `cel_runtime.c` — closed via two genrules
    in `compiler/runtime/BUILD.bazel`: one invokes brew's clang
    (`--target=wasm32 -ffreestanding -nostdlib`) to produce
    `cel_runtime.wasm`; the second embeds the bytes as
    `kCelRuntimeWasmBytes` / `kCelRuntimeWasmBytesSize`.  Coverage:
    `compiler/codegen/runtime_link_test.cc`
    (`BinaryenModuleReadWithFeatures` + validator + full
    `cel_make_*` + `memory` export-set check + Allocate-and-Write
    round-trip).  Both genrules and the test are tagged `manual`
    because `/opt/homebrew/opt/llvm/bin/clang` is non-hermetic.
  - `cel_unwrap_message` / `cel_wrap_message` — **closed by M3
    slice G1 (2026-04-19)**.  Emitted as Binaryen IR in
    `compiler/codegen/cel_refs.cc::AddMessageWrapHelpers` (not WAT —
    externref has no wasm32-C source representation).  Pulled into
    every eval module that declares a `Repr::kMessage` variable.
    Covered by `cel_refs_test::{AddMessageWrapHelpersValidates,
    EmitsWrapAndUnwrapMessageFunctions, ExportsWrapAndUnwrapMessage}`
    and by the "needs the table" check in
    `expr_lower_test::MessageVariablePullsInCelRefsTableAndWrappers`.
    Full runtime round-trip deferred to G2 where `cel_host.get_field`
    is the first caller that actually interns a host externref.
  - List / map growth + iteration runtime tests — still open.  The
    wasm32 cross-compile is in place but no codegen caller
    constructs a list or map yet (first caller lands in M5).  When
    it does, add a test that instantiates the merged module under
    wasmtime, grows a list through the allocator, and walks it from
    the host.
  - `cel.abi` custom section — closed via
    `compiler/codegen/abi.{h,cc}` (writer using libprotobuf +
    cel-cpp's `AstToCheckedExpr`) and `cel_abi.proto` (schema
    matching design Appendix A).  `abi_test.cc` covers the proto
    round-trip through a serialized `.wasm`; the CLI attaches the
    section on every `--emit_wasm` invocation.  M2 populates
    `version` / `cel_source` / `checked` / `layout` / empty
    `function_set`; `types` / `attributes` / `patterns` /
    `error_msgs` fill in as M3–M5 introduce features that
    reference them.

**M3 slice G2 (2026-04-19): field-number resolution.**
`SelectExpr` nodes now carry their resolved proto field number in
`NodeAnnotation::field_number`.  Resolution happens during
`PopulateAnnotations` in `compiler/ir/typed_ast.cc`: a
`FieldNumberVisitor` walks every `SelectExpr`, looks up its operand's
message type in the descriptor pool, and writes the field number into
the node annotation.  `ParseAndCheck` passes the still-live descriptor
pool through so the resolver runs on the real generated + user-schema
pool before the bundle goes out of scope.  Zero is the sentinel for
"not a SelectExpr" / "could not be resolved" — proto field numbers
start at 1, so no legitimate value collides with it.  This is Option B
from `m3-proto-and-strings.md` (chosen over A and C for
side-table-by-expr-id shape that matches the planned `attribute_id` /
`pattern_id` interning in `annotations.h`).  Coverage:
  - `typed_ast_test::G2ResolvesEveryProtoFieldKind` — a single
    table-driven test over a synthetic `celwasm.testg2.G2Msg`
    descriptor whose fields span every proto wire type (int64 / bool
    / string / int32 / message / uint32 / float / double / bytes /
    uint64 / sint32 / fixed32 / repeated_int32 / enum / fixed64 /
    sfixed32 / sfixed64 / sint64) with deliberately non-contiguous
    numbers so an off-by-one bug can't pass.  The G2PoolFixture is
    split into `AddKindEnum` / `AddScalarFields` / `AddKindField`
    helpers so the constructor stays below the function-size gate.
  - `typed_ast_test::{TestOnlySelectAlsoResolvesFieldNumber,
    UnknownFieldLeavesAnnotationAtZero,
    UnknownMessageTypeLeavesAnnotationAtZero,
    NonMessageOperandLeavesAnnotationAtZero,
    NestedSelectChainResolvesEachHop,
    NullPoolSkipsFieldNumberResolution}` — edge cases.
  - `parse_and_check_test::{SelectExprAnnotationCarriesFieldNumber,
    NestedSelectExprResolvesEachHop,
    HasMacroSelectExprCarriesFieldNumber,
    RepeatedFieldSelectResolvesFieldNumber,
    HasOnMapKeyLeavesFieldNumberZero,
    RejectsSelectOfUnknownFieldAtCheckTime}` — integration tests
    driving the real parser + checker + `PopulateAnnotations` against
    `google.protobuf.DescriptorProto` from the generated pool.

**M3 slice G2 (2026-04-19): `kSelectExpr` codegen + e2e.**  With
field numbers in `NodeAnnotation`, codegen lowers a non-`test_only`
`SelectExpr` on a `Repr::kMessage` operand to
`cel_alloc(24) → cel_host.get_field(msg, field_number, out_cv) →`
payload-load by `Repr`.  Host imports (`get_field`, `has_field`,
`message_eq`) are emitted unconditionally under the `"cel_host"`
namespace and backed by the pure-host logic in
`compiler/host/cel_host.{h,cc}` + the wasmtime trampolines in
`compiler/host/cel_host_wasmtime.{h,cc}`.  A realistic fixture —
`celwasm.testdata.Customer` in `compiler/testdata/e2e_fixture.proto`
— gives one scalar per CEL-relevant wire type so every payload-load
branch in `LoadSelectPayload` has an e2e assertion.  Coverage:
`cel_host_test::*` (table-driven over every wire type),
`expr_lower_test::SelectLowersToHostGetField` family, and
`eval_test::SelectProto{StringField{Eq,Neq,DefaultIsEmpty,PassThrough},
Int32FieldIsCelInt,Int64FieldCarriesLargeValue,
Uint64FieldIsUnsigned,Uint32FieldIsCelUint,DoubleField,BoolField,
BytesFieldRoundTrips}` — 10 wasmtime-backed scenarios flipping the
`kSelectExpr (field)` row across codegen and e2e columns.

The `test_only` e2e row is still `[ ]` — deferred to G3, which lowers
`has(msg.field)` to `cel_host.has_field`.

**M3 slice G3 (2026-04-19): `has(msg.field)` codegen + e2e.**  The
`test_only=true` branch of `SelectExpr` (what `has(msg.field)` parses
to after cel-cpp macro expansion) now lowers to a direct
`cel_host.has_field(externref, i32) → i32` call instead of the G2
`get_field → payload-load` dance.  The `has_field` trampoline was
already registered in G2; G3 is pure codegen.  `LowerSelect` now
dispatches on `test_only` and factors out `LowerSelectOperand` so
field-number resolution, operand `Repr::kMessage` check, and operand
lowering stay shared — each leg clears the function-size gate
independently.  Proto3 presence semantics (scalar-default = not
present, submessage explicit-set = present) are delegated to the host
via `google::protobuf::Reflection::HasField` in
`compiler/host/cel_host.cc`.  Coverage:
`eval_test::{HasProtoStringFieldSetAndUnset,
HasProtoInt32FieldSetAndUnset, HasProtoBoolFieldSetToFalseIsFalse,
HasProtoBytesFieldEmptyIsFalse,
HasProtoMessageFieldRespectsExplicitPresence,
HasComposesWithLogicalNot, HasAndFieldCompareTernary}` — seven
wasmtime-backed scenarios flipping the `kSelectExpr (test_only)` row
across codegen and e2e columns.  `HasProtoBoolFieldSetToFalseIsFalse`
specifically pins the proto3 "scalar at default value isn't present"
surprise; `HasProtoMessageFieldRespectsExplicitPresence` pins the
complementary "submessage explicitly set is present even if empty"
rule; `HasAndFieldCompareTernary` composes a G2 field read and a G3
`has()` in a single ternary to catch scratch-local aliasing between
the two select paths.

**M3 richer e2e coverage (2026-04-19, landed).**  Seven additional
`compiler/e2e/eval_test.cc` cases composing the G2/G3/G4 codegen
across stages and multi-param shapes that each individual slice
covered in isolation: `MultiParamTwoMessagesConcatenateNames`,
`MultiParamMessagePlusScalarUintComparison`,
`MultiParamTwoMessagesConditional`, `SizeOfProtoStringField` (pins
§1110 UTF-8 codepoint count on a field-read payload),
`ProtoStringFieldStartsWith` (G2 field + slice E member call),
`ProtoIntFieldArithmetic`, `NestedProtoStringFieldConcatWithLiteral`
(G4 nested select + slice D concat).  The `kCallExpr (member)` row's
codegen + e2e cells flip to `[x]` since
`ProtoStringFieldStartsWith` is the first e2e test that composes
`.startsWith()` with a G2 field read (the slice-E suite only
exercised literal receivers).

**M3 slice G4 (2026-04-19): nested message select + message equality.**
Two codegen extensions composing on top of G2: `LoadSelectPayload`
handles `Repr::kMessage` by calling `cel_unwrap_message(cv)` on the
scratch CelValue, so a SelectExpr that returns a submessage produces
an externref the rest of codegen can feed back into another Select or
into a comparison.  `LowerComparison` grew a `Repr::kMessage` arm that
calls `cel_host.message_eq` (already declared by `DeclareHostImports`
in G2) and wraps the result with `i32.eqz` for `!=`.  On the host side
`host_loader.cc` now invokes `CelHostEnv::BindEvalInterner` after
`wasmtime_linker_instantiate`, so `cel_host.get_field` can intern
submessages into the eval module's `$cel_refs` table — without that
bind step `InternMessageViaRefIntern` tripped a "wrong store" panic.
Coverage: 6 new `compiler/e2e/eval_test.cc` cases:
`NestedSelectReadsInnerField`,
`NestedSelectThroughUnsetSubmessageReadsDefault`,
`MessageEqualityTrueForStructurallyEqual`,
`MessageEqualityFalseForDifferentFields`,
`MessageInequalityInvertsMessageEq`,
`NestedMessageEqualityComposesSelectAndEq`.

**M3 slice G1 (2026-04-19): message params as externref.**
`Repr::kMessage` variables now lower to an `externref` param on the
eval function.  `WasmTypeFor(kMessage)` is `BinaryenTypeExternref()`.
When the eval module declares at least one message variable,
`LowerToEvalFunction` pulls in the private `$cel_refs` externref
table (16 initial slots, slot 0 reserved as the null sentinel) plus
the helper functions `cel_ref_intern` / `cel_ref_get` /
`cel_refs_reset` via `AddCelRefsTableAndHelpers`, and the CelValue-
shaped wrappers `cel_wrap_message(externref) → i32` /
`cel_unwrap_message(i32) → externref` via `AddMessageWrapHelpers`.
The wrap/unwrap pair is emitted from `compiler/codegen/cel_refs.cc`
(not WAT) because externref has no wasm32-C representation; it
depends on the runtime's `cel_make_message` import (new in this
slice) and the existing `cel_mem_base` export.  `kMsgSlotOffset = 8`
is the `payload.msg_slot` byte offset inside `CelValue`, pinned by a
`_Static_assert` in `cel_runtime.h`.  Codegen-only at G1 — full
runtime+wasmtime e2e deferred to slice G2, where `cel_host.get_field`
becomes the first caller that actually interns a host externref.
The "proto message" row of the per-type grid and the `kSelectExpr`
rows remain `[ ]` in the codegen/e2e columns because nothing reads a
field yet; the table/wrapper plumbing is the prerequisite, not the
feature.  Coverage:
`expr_lower_test::{MessageVariableLowersAsExternrefParam,
MessageVariablePullsInCelRefsTableAndWrappers,
NoMessageVariableMeansNoCelRefsTable}`;
`cel_refs_test::{AddMessageWrapHelpersValidates,
EmitsWrapAndUnwrapMessageFunctions,
ExportsWrapAndUnwrapMessage}`.

**M3 slice F (2026-04-19): bytes constants + operators end-to-end.**
`b'...'` literals lower through a generalised `LowerSpanLiteral` that
takes the constructor helper name, so the string path routes through
`cel_make_string_view` and the bytes path through `cel_make_bytes_view`
with no duplicated store-per-byte loop.  `Repr::kBytes + ADD` routes
to a new `cel_bytes_concat` runtime helper; `size(bytes)` routes to a
new `cel_bytes_size` helper.  `cel_bytes_size` is a direct payload-
length read (not a code-point count) — `size(bytes)` is byte count
per CEL §1110, and swapping the two dispatches would return the wrong
answer for multi-byte UTF-8 sequences.  The runtime side factors
`cel_string_concat` and `cel_bytes_concat` through a shared
`span_concat(kind)` so the allocation + copy logic has one home.
Bytes equality already worked via the `Repr::kBytes` branch of
`LowerComparison` → `cel_bytes_eq`, so slice F just added e2e
coverage for it rather than changing codegen.  The `bytes` row in the
per-type grid flips to `[x]` in the codegen and e2e columns.
Coverage: `expr_lower_test::{BytesConstantReturnsI32, EmptyBytesLowers,
BytesConcatLowersToRuntimeCall, BytesEqualityLowersToRuntimeCall,
SizeBytesLowersToRuntimeCall}` plus the two new imports added to
`EvalModuleDeclaresRuntimeFunctionImports`;
`cel_runtime_test::{BytesConcat{JoinsPayloads,LeftEmpty,RightEmpty,
BothEmpty,RejectsZeroOffsets,RejectsNonBytesOperands},
BytesSize{CountsBytes,Empty,RejectsZeroOffset,RejectsNonBytes}}`;
`eval_test::{BytesLiteralRoundTripsThroughMemory,EmptyBytesRoundTrips,
BytesLiteralPreservesHighBits,BytesConcatenationProducesJoinedBytes,
BytesConcatenationEmptyLhs,BytesEqualityPositiveAndNegative,
BytesInequalityInvertsEquality,SizeOfBytesIsByteCount,
SizeOfEmptyBytesIsZero,SizeOfConcatenatedBytes}`.

**M3 slice E (2026-04-19): string member calls end-to-end.**
`'x'.startsWith('y')`, `'x'.endsWith('y')`, and `'x'.contains('y')` now
lower to calls to new runtime helpers `cel_string_starts_with` /
`cel_string_ends_with` / `cel_string_contains`.  The checker hands
codegen a `CallExpr` with `target` set, `function` being the bare
method name, and a single arg; a new `LowerStringMemberCall` helper
factors out the dispatch so `LowerCall` stays within review size.
Each helper returns i32 0/1 per the existing
`cel_string_eq`-style ABI; spec edge cases (empty needle / prefix /
suffix is always true; a longer needle than haystack is false) are
enforced in the runtime rather than codegen, which keeps the emitted
eval modules a single call each.  Coverage:
`expr_lower_test::{StartsWithLowersToRuntimeCall,
EndsWithLowersToRuntimeCall, ContainsLowersToRuntimeCall}`;
`cel_runtime_test::{StartsWith{True,False,EmptyPrefixIsTrue,
LongerPrefixIsFalse,FullMatch,RejectsZeroOffsets,RejectsNonString},
EndsWith{True,False,EmptySuffixIsTrue,LongerSuffixIsFalse,FullMatch,
RejectsZeroOffsets}, Contains{TrueMiddle,TruePrefix,TrueSuffix,False,
EmptyNeedleIsTrue,LongerNeedleIsFalse,FullMatch,RejectsZeroOffsets,
RejectsNonString}}`; `eval_test::{StartsWithPositiveAndNegative,
StartsWithEmptyPrefixIsTrue,StartsWithLongerPrefixIsFalse,
EndsWithPositiveAndNegative,EndsWithEmptySuffixIsTrue,
ContainsPositivePrefixMiddleSuffix,ContainsNegative,
ContainsEmptyNeedleIsTrue,ContainsLongerNeedleIsFalse}`.

**M3 slice D (2026-04-19): string operators end-to-end.**
`'a' + 'b'`, `'a' == 'b'`, `'a' != 'b'`, and `size('a')` now lower to
calls to the runtime helpers `cel_string_concat`, `cel_string_eq`, and
`cel_string_size` that M3 slice A already pre-declared as imports.
`_!=_` wraps the equality call in `i32.eqz` rather than reusing a
native `ne` opcode because the helper returns an i32 0/1 sentinel.
Along the way this slice fixed a latent bug in `LowerStringLiteral`:
`cel_alloc` returns an offset relative to the runtime's `g_memory`
array but `i32.store8` expects an absolute linear-memory offset, so
the previous code wrote literal bytes into an unrelated stretch of
the data section.  The fix imports the existing `cel_mem_base` runtime
export and caches `base + scratch` in a local used for every store;
`cel_make_string_view` still takes the arena-relative offset.  The
old `StringLiteralReturnsCelValueOffset` e2e test passed only because
it asserted `offset > 0` without decoding the bytes — the new
`StringLiteralRoundTripsThroughMemory` test pulls the `CelValue` plus
its span back out of wasmtime memory (via `cel_mem_base`) and verifies
the bytes match.  Coverage:
`expr_lower_test::{StringConcatLowersToRuntimeCall,
StringEqualityLowersToRuntimeCall,
StringInequalityInvertsEqualityCall,
SizeStringLowersToRuntimeCall}` and
`eval_test::{StringLiteralRoundTripsThroughMemory,
EmptyStringRoundTrips, StringConcatenationProducesJoinedBytes,
StringConcatenationEmptyLhs, StringEqualityPositiveAndNegative,
StringInequalityInvertsEquality, SizeOfAsciiStringIsByteCount,
SizeOfEmptyStringIsZero, SizeOfUtf8StringCountsCodepointsNotBytes,
SizeOfConcatenatedString}`.

**M3 slice C (2026-04-19): `kIdentExpr` reads scalar variables.**
Variables declared in `CheckOptions::variable_specs` now become typed
parameters on the eval function, in declaration order.  `kIdentExpr`
lowers to `local.get N` against that parameter slot.  `TypedAst::
variables()` carries each `{name, Repr}` pair so codegen can size the
signature even for variables the expression doesn't reference (the
host's ABI signature must be deterministic).  `LoadedEval::CallEval`
generalises the old `CallNullaryEval` to accept per-variable
`wasmtime_val_t` args.  Declaring a variable whose Repr has no scalar
ABI (list/map/message) fails loudly in codegen with the variable name
in the diagnostic.  Coverage:
`expr_lower_test::{IntIdentLowersToLocalGetWithI64Param,
MultipleVarsGetParamsInDeclarationOrder, IdentsOfAllScalarReprs,
UnsupportedVariableReprFailsWithSpecName}` and
`eval_test::{IntVariableIsReadFromFirstParam, BoolVariableInTernary,
DoubleVariableArithmetic, UintVariableUnsignedComparison,
TwoVariablesReadInDeclarationOrder,
UnreferencedVariableStillOccupiesParamSlot}`.  The `kIdentExpr` row's
`codegen` and `e2e` cells flip to `[x]`.

**M3 slice A+B (2026-04-19): string literals execute end-to-end.**
String constants now lower to a block that calls `cel_alloc`, stores
bytes into the runtime's shared memory, and calls
`cel_make_string_view`.  `WasmTypeFor(kString)` is `i32` (the offset of
a `CelValue` in the runtime's arena).  The eval module declares the
full runtime import set unconditionally; a new two-module host
(`//compiler/host:host_loader`) instantiates runtime + eval under a
shared `wasmtime_linker_t` and exposes `CallNullaryEval()`.  Coverage:
`expr_lower_test::{StringConstantReturnsI32, EmptyStringLowers,
EvalModuleImportsSharedMemoryFromRuntime,
EvalModuleDeclaresRuntimeFunctionImports}`,
`host_loader_test::{LoadsAndCallsNullaryEval, ResolvesRuntimeImports,
CanBeCalledRepeatedly, ReturnsScalarReprsUnchanged,
RejectsNonWasmBytes, RejectsEvalModuleWithUnsatisfiedImports}`, and
`eval_test::StringLiteralReturnsCelValueOffset`.  The `string` row in
the per-type grid is now ticked for `codegen` and `e2e eval`.

**M3 entry — front-end gaps closed (2026-04-19).**
The per-`ExprKindCase` grid had claimed parser / checker /
annotations `[x]` for `kIdentExpr`, `kSelectExpr` (field), and
`kCallExpr` (member), but the ticks were implicit — no dedicated
test pinned the AST shape for those variants.  Closed via three
tests in `parse_and_check_test.cc`:
  - [x] `kIdentExpr` — `IdentExprParsesToIdentNode` asserts
        `root.kind_case() == kIdentExpr` and the ident name round-trips.
  - [x] `kSelectExpr (field)` — `SelectExprParsesToFieldAccessOnProtoMessage`
        parses `d.name` against `google.protobuf.DescriptorProto` and
        asserts `!test_only` + field name + annotated as `kString`.
  - [x] `kCallExpr (member)` — `MemberCallExprParsesWithTarget` parses
        `"hi".startsWith("h")` and asserts `has_target()` + function
        name + constant-literal target.

**M3 entry — unpinned front-end semantics.** The grids do not cover
these (see §"Front-end semantic coverage" below).  None are blocking
for M3 codegen start, but the member-access chain row and the
string-escape row become load-bearing once M3 lowers
`request.user.name` and string constants, so prioritize those.

**Waiting on later milestones** (don't try to close in M2):
  - `kListExpr`, `kMapExpr`, `kStructExpr` — codegen + e2e rows
    `[ ]`.  Unblocked by M5 (collections).
  - `kComprehensionExpr` × 4 variants + nested shadowing — **shipped 2026-05-17 at the M5.B comprehensions follow-on milestone**; per-shape grid rows ticked below.
  - Arithmetic-overflow / divide-by-zero / NaN-unordered / string
    coercion / unknown-propagation e2e — M4 (three-valued logic).
  - Partial-eval commutativity for `unknown && false → false` — M4.
  - Enum and `Any`-unwrap rejection — M7 stdlib or earlier if a
    user expression forces the question.
  - Conformance suite from `tests/simple/testdata/` — M8.

A bullet in "Still open inside M2" should NEVER outlive M2.  A bullet
in "Waiting on later milestones" SHOULD outlive M2 and flip during
the milestone that introduces the feature.

## Per CEL type

For every type T, we need a positive test and at least one negative test in
each stage of the pipeline where T can appear.

`checker` = `parse_and_check_test.cc` accepts an expression that produces
the type. `annotations` = `typed_ast_test::ReprOfTest` maps the TypeSpec
variant to the right `Repr`. `RejectDyn` tests live in
`static_subset_test.cc`.

| Type            | parser | checker | annotations | RejectDyn | codegen | e2e eval |
| --------------- | :----: | :-----: | :---------: | :-------: | :-----: | :------: |
| `bool`          | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `int`           | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `uint`          | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `double`        | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `string`        | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `bytes`         | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `null_type`     | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `timestamp`     | [x]    | [x]     | [x]         | [x]       | [ ]     | [x]      |
| `duration`      | [x]    | [x]     | [x]         | [x]       | [ ]     | [x]      |
| `list<T>`       | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `map<K,V>`      | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| proto message   | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| enum            | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| wrapper (Int64Value …) | [x] | [x]  | [x]         | [x]       | [x]     | [x]      |
| `any`           | [x]    | [x]     | [x]         | [x]       | [ ]     | [x]      |
| `dyn` (rejected)| —      | —       | —           | [x]       | —       | —        |
| `error`         | —      | —       | —           | [x]       | —       | —        |

## Per `ExprKindCase`

| Variant             | parser | checker | annotations | RejectDyn | codegen | e2e |
| ------------------- | :----: | :-----: | :---------: | :-------: | :-----: | :-: |
| `kConstant`         | [x]    | [x]     | [x]         | [x]       | [x]     | [x] |
| `kIdentExpr`        | [x]    | [x]     | [x]         | [x]       | [x]     | [x] |
| `kSelectExpr` (field) | [x]  | [x]     | [x]         | [x]       | [x]     | [x] |
| `kSelectExpr` (`test_only`, from `has()`) | [x] | [x] | [x]  | [x] | [x] | [x] |
| `kCallExpr` (global) | [x]   | [x]     | [x]         | [x]       | [x]     | [x] |
| `kCallExpr` (member) | [x]   | [x]     | [x]         | [x]       | [x]     | [x] |
| `kCallExpr` (short-circuit `&&` / `||` / `?:`) | [ ] | [ ] | [ ] | [ ] | [x] | [x] |
| `kListExpr` (empty + non-empty) | [x] | [x] | [x]  | [x]       | [ ]     | [ ] |
| `kStructExpr` (proto ctor) | [x] | [ ] | [x]      | [x]       | [ ]     | [x] |
| `kMapExpr`          | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (exists) | [x] | [x] | [x]  | [x]       | [x]     | [x] |
| `kComprehensionExpr` (all)    | [x] | [x] | [x]  | [x]       | [x]     | [x] |
| `kComprehensionExpr` (filter) | [x] | [x] | [x]  | [x]       | [x]     | [x] |
| `kComprehensionExpr` (map)    | [x] | [x] | [x]  | [x]       | [x]     | [x] |
| `kComprehensionExpr` (exists_one)            | [x] | [x] | [x] | [x] | [x] | [x] |
| `kComprehensionExpr` (transformList v2 3-arg)| [x] | [x] | [x] | [x] | [x] | [x] |
| `kComprehensionExpr` (transformList v2 4-arg)| [x] | [x] | [x] | [x] | [x] | [x] |
| `kComprehensionExpr` (transformMap v2 3-arg) | [x] | [x] | [x] | [x] | [x] | [x] |
| `kComprehensionExpr` (transformMap v2 4-arg) | [x] | [x] | [x] | [x] | [x] | [x] |
| `kComprehensionExpr` (transformMapEntry 3-arg, single-key entry) | [x] | [x] | [x] | [x] | [x] | [x] |
| `kComprehensionExpr` (transformMapEntry 3-arg, multi-key entry)  | [x] | [x] | [x] | [x] | [x] | [x] |
| `kComprehensionExpr` (transformMapEntry 4-arg, single-key entry) | [x] | [x] | [x] | [x] | [x] | [x] |
| `kComprehensionExpr` (cel.bind via Shape-C)  | [x] | [x] | [x] | [x] | [x] | [x] |
| nested comprehensions with shadowing | [x] | [x] | [x] | [x]  | [x]     | [x] |

## Front-end helpers

### Variable-spec parser (`compiler/frontend/parse_and_check.cc`)

- [x] Every primitive by name (`bool`, `int`, `uint`, `double`, `string`,
      `bytes`, `null_type`).  (`parse_and_check_test::PrimitiveVariableSpecs`)
- [x] Every well-known (`timestamp`, `duration`, `any`).
- [x] `list<T>` with primitive and nested-list `T`.
      *(message-element list pending until schema fixture lands.)*
- [x] `map<K,V>` with string key, int value.  *(Full key-type matrix still
      pending — `map<list<int>,int>` rejection not yet asserted.)*
- [x] Proto message by FQN (`google.protobuf.Empty` from the generated
      pool).  *(Custom-schema FQNs pending until e2e fixtures land.)*
- [x] Errors: missing `:`, empty name, unknown type, unbalanced `<>`,
      trailing garbage after the type.

### `RejectDyn`

- [x] DYN at root (missing root type).
- [x] DYN nested inside call, select, list, struct, map, comprehension
      subtrees.
- [x] `ErrorTypeSpec`, `FunctionTypeSpec`, `ParamTypeSpec`, `UnsetTypeSpec`
      each rejected with the correct label.
- [x] Checked expression with no DYN returns OK.
- [x] **Rewrite M5.A** — implicit dyn from container element types
      rejected: `[]` (typed `list<dyn>`), `[1, "two"]` (heterogeneous
      list), bare `{}` (`map<dyn, dyn>`).  Locked via
      `m4_test.cc::ListRejectionE2ETest::{BareEmptyListLiteralRejected,
      HeterogeneousListRejected}` and `compile_test::CompileMapTest::
      EmptyMapLiteralRejected`.  Recursion covers `list_type`,
      `map_type` (key + value), and `abstract_type.parameter_types`.
- [x] **Rewrite M5.B step 1** — same-kind arithmetic + comparison
      runtime helpers.  `cel_arith.h` (int / uint / double × add /
      sub / mul / div / mod + neg for int / double) + `cel_compare.h`
      (per-kind eq / ne / lt / le / gt / ge + bool eq / ne + null eq).
      Bodies in `cel_runtime.c` with cel-cpp parity citations.
      `uint64_mul_overflows` / `int64_mul_overflows` use a split
      32×32→64 partial-product shape to avoid `__multi3` (compiler-rt
      128-bit multiply) which the wasm32 freestanding build doesn't
      link.  Locked in `runtime/cel_arith_test.cc` (23
      tests) + `runtime/cel_compare_test.cc` (initial
      same-kind matrix; cross-type added in step 2).  WATs 16
      (`arith_int_add`) + 17 (`compare_int_eq`) lock the slot-out
      helper ABI shape end-to-end through `wat_runner`.  47 helper
      exports added to `cel_runtime.wasm` and the `wat_runner`
      `kRuntimeExports` array.
- [x] **Rewrite M5.B step 2** — cross-type numeric ladder + bool /
      string / bytes ordering tail.  16 helpers added: 6 cross-type
      numeric (`cel_numeric_{eq,ne,lt,le,gt,ge}_at_vv`) implementing
      the int↔uint↔double ladder per langdef §"Equality" /
      §"Comparison"; 4 bool ordering (`cel_bool_{lt,le,gt,ge}_at_vv`)
      via `DEFINE_CMP_VV`; 6 string / bytes ordering tail
      (`cel_string_{le,gt,ge}_at_vv` + `cel_bytes_{le,gt,ge}_at_vv`)
      via a new `DEFINE_SPAN_CMP_VV` macro.  34 ids moved from
      `kExplicitlyUnimplementedIds` to `kBuiltinSeeds`; seed count
      46 → 80; unimplemented count 120 → 86.  64 new test cases
      across `runtime/cel_compare_test.cc` (28 total) +
      `runtime/cel_string_ops_test.cc` (27 total).
      One cel-cpp parity edge case mirrored verbatim:
      `kGreaterEqualsUintDouble` (typo missing `64`) at
      `third_party/cel-cpp/common/standard_definitions.h:212`.
      Polymorphic `equals` / `not_equals` deferred to step 2b after
      M5.D step 2 ships aggregate eq + `cel_message_eq`.
- [x] **Rewrite M5.C** — string / bytes ops runtime helpers.
      `cel_string_ops.h` — string concat / size / eq / lt / contains /
      startsWith / endsWith + bytes concat / size / eq / lt (no
      contains / startsWith / endsWith on bytes per langdef).  Bodies
      in `cel_runtime.c` share `span_eq` / `span_lt` / `span_contains` /
      `span_match_at` so each per-helper body is two lines.  WAT 18
      (`string_concat`) locks the arena-alloc slot-out shape; concat
      is the only M5.C helper that calls `cel_alloc`.  Locked in
      `runtime/cel_string_ops_test.cc` (initial 18 tests
      covering happy path, empty / multi-byte UTF-8 / embedded-NUL,
      byte-vs-string lt unsigned ordering, 3VL / type-mismatch
      envelope; step 2 added 9 more for the le/gt/ge tail).  11 helper
      exports added; regex `matches` deferred per
      `m5-kcall-comprehensions.md §1.2`.
- [x] **Rewrite M5.D step 1** — aggregate-op kArena fast paths.
      7 helpers (`cel_list_size_arena`, `cel_list_in_arena`,
      `cel_list_eq_arena`, `cel_list_concat_arena`,
      `cel_map_size_arena`, `cel_map_in_arena`, `cel_map_eq_arena`);
      decls split between `cel_list.h` and `cel_map.h`, bodies in
      `cel_runtime.c`.  Shared scalar matcher `cel_value_eq` builds
      on the existing `map_keys_equal` ladder (int↔uint cross-type,
      bool / string), adds same-kind double / bytes / null branches.
      Cross-type numeric involving double defers to M5.B step 2's
      `cel_numeric_*` ladder.  WATs 21 (`size_list`) + 22 (`in_list`)
      lock the 1-operand and 2-operand slot-out shapes end-to-end
      through `wat_runner`.  Locked in
      `runtime/cel_aggregate_arena_test.cc` (21 tests)
      covering per-helper happy path × empty boundary × cross-type
      numeric `in` × map order-irrelevance × 3VL absorption × type
      mismatch.  kDynamic dispatchers + 7 kHost trampolines +
      `cel_message_eq` deferred to M5.D step 2.
- [x] **Rewrite M5.E** — `OverloadTable::kBuiltinSeeds` populated
      (46 entries: arithmetic same-kind / concat / same-kind ordering
      / container size / container `in` / string ops) +
      `kExplicitlyUnimplementedIds` populated (120 entries: special-
      cased in `expr_lower`, deferred-to-M5.B-step-2 cross-type
      numeric ladder + bool/string/bytes ordering tail, timestamp /
      duration arithmetic + ordering, regex `matches`, timestamp /
      duration accessors, type conversions).  Coverage tripwire
      `CoverageTripwireClassifiesEveryStandardId` asserts every
      cel-cpp `StandardOverloadIds::k*` is either resolvable via
      `InternOverloadId` or in `kExplicitlyUnimplementedIds` —
      forcing-function for the next vendoring of cel-cpp.  Locked
      in `compiler/codegen/overload_table_test.cc`.
- [x] **Rewrite M5.F** — general kCall arm (`EmitGeneralCall`)
      wires `OverloadTable::Lookup(ann.overload_id)` into
      `expr_lower.cc`.  Arithmetic / same-kind comparison /
      string ops / receiver-form ops compile end-to-end through
      `Compile → Plan → Eval`.  M5.G control-flow operators
      (`_&&_` / `_||_` / `_?_:_` / `!_`) and M5.D-step-2 pending
      dispatchers (`size_list` / `size_map` / `add_list` / `in_list` /
      `in_map`) surface as Unimplemented.  Conformance: 207 → 391
      PASS.  Locked in `compiler/codegen/expr_lower_test.cc`
      (9 new lowering tests) + `e2e/m5_test.cc`
      (32 e2e tests across arithmetic, comparison, string ops,
      bytes, bound vars, proto fields, pending guards).

- [x] **Rewrite Slice 1.6** — cross-numeric ordering / membership
      ladder.  `EmitGeneralCall` re-picks the cross-numeric overload
      id (`less_int64_uint64`, etc.) when operand Reprs span numeric
      kinds — cel-cpp's reference_map only lists the same-kind
      overload of the non-dyn operand, so the re-pick is mandatory
      for `dyn(int) < uint` to route to `cel_numeric_lt_at_vv` rather
      than `cel_uint_lt_at_vv` (which would type-mismatch).
      `cel_value_eq_polymorphic` extracts a polymorphic element-
      equality matcher used by `cel_list_in_arena` /
      `cel_map_in_arena`, and `map_keys_equal` widens to consult
      `numeric_compare_kernel` for any numeric pair.  Conformance:
      562 → 664 PASS (+102); `comparisons.textproto` 189 → 287,
      `lists.textproto` 23 → 27.  Locked in
      `compiler/codegen/expr_lower_test.cc` (3 re-pick tests),
      `runtime/cel_aggregate_arena_test.cc` (8
      polymorphic membership tests), `e2e/m5_test.cc`
      (`CrossNumericOrderingE2ETest` — 152 tests across the full
      operand-kind × operator × dyn-position matrix + boundary +
      NaN matrix + membership matrix + same-kind regression
      guards).

- [x] **Rewrite M5.B comprehensions follow-on** — cel.bind + the
      five standard / v2 comprehension macros (exists / all /
      exists_one / map / filter / transformList / transformMap /
      transformMapEntry) over both list and map sources.  Slices
      A–J landed 2026-05-17 (commits `c218552`..`c8a4c56`):
        - **A**: ResolvePass scope handler.  Comprehensions
          admitted (was: rejected by `ComprehensionDetector`).
        - **B**: LayoutPass scope-aware slot allocation; ABI
          filter excludes comp vars.
        - **C** (+ nested closeout): `kComprehensionExpr`
          codegen for exists/all/exists_one over list literals;
          per-comp binding indices + unique loop labels for
          nested-same-name accu_var safety.
        - **D**: map/filter codegen + `cel_list_append_at` /
          `cel_list_append_at_if_bool` runtime helpers; 3VL pred.
        - **E**: map-source comprehensions via
          `cel_map_iter_init/next/key_at/value_at` runtime
          helpers; iter_var binds to current key per design §3.10.
        - **F**: two-iter-var (`comprehensions_v2`) — list +
          map source; synthesized index counter for list
          two-iter case.
        - **G**: `transformMap` + `cel_map_insert_at` runtime
          helper; ComprehensionsV2CheckerLibrary registered for
          `cel.@mapInsert` overload type-check.
        - **H**: `transformMapEntry` — generalized over
          `entry.size()` (0 = no-op, 1 = transformMap-equivalent,
          N>1 = N sequential inserts); pre-sizing multiplier
          via `PerIterEntryCount`.
        - **I**: `cel.bind` parser-library registration +
          Shape-C fast path.
        - **Consolidation**: pre-sized list + map accumulators
          (capacity = iter_range.count × per-iter); list
          runtime API collapsed (`cel_list_create(out,
          capacity)` + universal `cel_list_append_at`,
          `cel_list_set` deleted); growth branches replaced
          with `__builtin_trap` invariant; lint PCH fix
          (bazel build superset to keep external symlinks live
          for clang-tidy).
        - **Slice H + 3VL pred + ext-symbol SKIP**:
          `cel_map_insert_at_if_bool` for conditional
          transformMap predicate error propagation; runner
          gained "undeclared reference to 'cel'" → SKIP
          classifier for unregistered extensions.
        - **J**: closeout — docs reconcile + conformance README
          headline / per-fixture / forecast updates.
      Conformance: 1287 → **1373 PASS / 2454** (+86 net).
      Per-fixture: macros 0→38 PASS, macros2 0→39 PASS,
      bindings_ext 0→7 PASS, namespace 4→6 PASS, block_ext
      37 FAIL → 25 SKIP + 12 FAIL.  Locked in
      `e2e/m5b_test.cc` (54 tests across 9
      fixture classes — exists/all/exists_one, map/filter,
      map-iter, two-iter-var, cel.bind, transformMap,
      transformMapEntry, nested, consumer; per-fixture
      SKIPs at `RejectDyn`-on-empty-literal cases documented
      with cel_map_test.cc runtime equivalents).  Doc:
      `doc/implementation-plan/rewrite/m5-comprehensions-followon.md`.

## Front-end semantic coverage (beyond the grids)

The per-type and per-`ExprKindCase` grids above cover *which shapes
are accepted and annotated*, but say nothing about *how the source
string decomposes into those shapes*.  Most of the unpinned items
below delegate to the vendored cel-cpp parser and would rarely
regress from our side — but a wrong expectation (e.g. we assume
left-assoc when the parser is right-assoc, or a codegen pass that
collapses two SelectExprs into one) would silently miscompile.
Each bullet is one cheap regression test.

### Operator precedence & associativity

**Pinned** by a test that inspects an inner result:
  - `*` vs `+`: `1 + 2 * 3 == 7` (`eval_test::IntArithmeticPrecedence`).
  - Explicit parens override: `(1 + 2) * 3 == 9`
    (`eval_test::MixedArithAndConditional`).

**Unpinned** — no test asserts either the parse tree or the value:
  - [ ] `!` vs `&&` — `!true && false` must parse as `(!true) && false`
        and evaluate to `false`.
  - [ ] `&&` vs `||` — `true && false || true` must parse as
        `(true && false) || true` and evaluate to `true`.
  - [ ] `==` vs `&&` — `1 == 1 && 2 == 2` evaluates to `true` (i.e.
        `&&` does not bind tighter than `==`).
  - [ ] Unary vs binary minus — `1 - -2 == 3` (the `-2` is a unary
        negate of a literal, not a malformed token).
  - [ ] Ternary right-associativity — `true ? 1 : false ? 2 : 3 == 1`
        and `false ? 1 : true ? 2 : 3 == 2`.
  - [ ] Relational vs logical — `1 < 2 && 3 < 4` evaluates to `true`
        (relationals bind tighter than `&&`).

Any of the above can be a ~three-line `eval_test` addition — a cheap
insurance policy for parser upgrades.

### Literal forms

**String / bytes** — pinned: `"abc"`, `'hello'`, `b"ok"`.
  - [ ] Escape sequences: `"\n"`, `"\t"`, `"\\"`, `"\""`,
        `"\x41"` (hex byte), `"\u2603"` (BMP codepoint).  Each must
        round-trip through `size()` / equality unchanged.
  - [ ] Raw strings: `r"\n"` → four codepoints, not one newline.
  - [ ] Triple-quoted: `"""hello"""` parses; internal quotes allowed.
  - [ ] Bytes with high bytes: `b"\xff\x00"` has size 2.

**Integer / double** — pinned: `42`, `1u`, `1.5`,
`18446744073709551615u`.
  - [ ] Hex integer: `0xff == 255` and `0xFFu == 255u`.
  - [ ] Scientific double: `1e2 == 100.0`, `1.5e-1 == 0.15`.
  - [ ] Rejection: `-1u` must fail the checker (uint can't be
        negative; the parser accepts the token, the checker must
        flag).

### Member-access & call chains

M3's `request.user.name` and `"hi".startsWith("h")` exercise these
shapes for the first time; today nothing pins them.
  - [ ] `a.b.c` parses as nested `SelectExpr(SelectExpr(Ident "a",
        "b"), "c")`, left-associative.
  - [ ] `a.b().c` parses as `SelectExpr(CallExpr(target=Ident "a",
        fn="b"), "c")` — call-then-member.
  - [ ] `"a".b().c` — literal receiver works the same way.
  - [ ] Chain-level `has()`: `has(a.b.c)` lowers to `test_only`
        SelectExpr at the outermost level only; the inner
        `SelectExpr(a, "b")` stays a normal (non-test_only) select.

### Miscellaneous front-end behaviour

  - [ ] Line comments: `1 + 2 // trailing` parses and evaluates to 3.
  - [ ] Reserved-word rejection: bare `has`, `in`, `as`, `type`
        as an identifier is flagged by the checker, not silently
        treated as a user variable.
  - [ ] Qualified enum-ish name resolution: `com.google.protobuf.Empty`
        parses as a **type reference**, not as three chained selects
        on an ident `com`.

### Scope of this section

These rows do **not** belong in the per-type or per-`ExprKindCase`
grids — those axes are about "can the type system produce this
shape?".  The rows above are about "does the parser produce the
shape we think it produces?".  The distinction matters because a
grid tick can be true (shape is accepted and annotated) while the
source→shape mapping still has an unpinned surprise.

## Runtime (native-compiled for unit tests)

- [x] Bump allocator: alignment, reset semantics, out-of-memory.
      (`compiler/runtime/cel_runtime_test.cc`)
- [x] `cel_make_*` constructors populate the right tag + payload.
      (covers null/bool singletons, int/uint/double, string/bytes copy +
       view, message, type, duration, timestamp, optional some/none,
       unknown, error)
- [x] `cel_string_eq` / `cel_bytes_eq` on empty, equal, unequal-length,
      different-content inputs.
- [x] Three-valued logic helpers: `cel_unknown_merge` (sorted-dedup'd
      union + determinism), `cel_not` (bool flip + ERROR/UNKNOWN
      passthrough), `cel_and` / `cel_or` full 5×5 truth tables over
      {TRUE, FALSE, ERROR, UnknownA, UnknownB} via `TEST_P`, and
      `cel_status_either` (ERROR > UNKNOWN > OK dominance, left-wins
      determinism).  Type-error rejection for every helper.
      (M4 slice A — `cel_runtime_test.cc`)
- [ ] List / map growth, iteration.
- [ ] `cel_ref_intern` dedup + `cel_unwrap_message` round-trip.

## End-to-end

Each e2e test instantiates the generated module against a host stub, calls
`eval`, and asserts the returned `CelValue`.  Track one row per smoke
expression from `m1-type-checker.md`, plus:

- [x] Arithmetic overflow error (int + int overflows to `ERROR`).
      **M4 slice B (2026-04-19) + slice C/3b1 (2026-04-20):** codegen
      routes `_+_` / `_-_` / `_*_` on int/uint through runtime
      helpers that write a CEL_ERROR{code} into the scratch slot on
      overflow.  Slice C/3b1 replaced the `unreachable` trap with a
      `cel_copy_celvalue_at(sret, scratch); return` early-exit so the
      host sees an observable CelValue ERROR rather than a wasm trap.
      Covered by `eval_test::{IntAddOverflowIsCelError,
      IntSubOverflowIsCelError, IntMulOverflowIsCelError,
      UintAddOverflowIsCelError, UintSubUnderflowIsCelError,
      UintMulOverflowIsCelError}` plus the `INT64_MIN / -1` corner
      case `eval_test::IntDivMinByNegOneIsCelError`.  `CallEval` in
      `host_loader.cc` decodes the slot and surfaces CEL_ERROR as
      `absl::InternalError("CallEval: result is ERROR")` — the
      `CelErrorStatus` matcher guards on that message.
- [x] Division by zero (`int / 0` and `double / 0`).  **M4 slice B
      (2026-04-19) + slice C/3b1 (2026-04-20):** `int / 0`, `uint / 0`,
      and `int % 0` write CEL_ERROR to the slot, the eval fn early-
      returns, and the host decodes it as an observable ERROR —
      covered by `eval_test::{IntDivByZeroIsCelError,
      UintDivByZeroIsCelError, IntModByZeroIsCelError}`.
      `INT64_MIN % -1` is defined as 0 per IEEE-style cel-go precedent
      (`IntModMinByNegOneIsZeroNotTrap`).  `double / 0` produces
      +/-Inf per IEEE 754 per §langdef, which is OK not ERROR;
      negative-coverage still pending once the double-op-coverage
      checklist row moves.  Regression cases
      `IntMulHappyPathStillWorksAfterRetrofit`,
      `SignedMulNegativeResultRoundTrips`, and
      `SignedMulIntMinByOneRoundTrips` guard against the refactor
      breaking the non-overflow path.
- [x] NaN-unordered compare produces ERROR for `<` / `<=` / `>` / `>=`,
      OK(false) for `==` / `!=`.  **M4 slice D (2026-04-19) + slice
      C/3b1 (2026-04-20):** codegen routes every ordered double
      compare through `LowerDoubleOrderedCompare`, which now emits
      `cel_set_error_at(sret, CEL_ERR_TYPE_MISMATCH); return;` when
      either operand is NaN (IEEE 754 `x != x`).  Equality stays as
      a plain `f64.eq` / `f64.ne`, which IEEE 754 defines to return
      false for any NaN input — no guard needed.  Covered by
      `eval_test::{DoubleLessNaNOnLeftIsCelError,
      DoubleLessNaNOnRightIsCelError, DoubleLessEqNaNIsCelError,
      DoubleGreaterNaNIsCelError, DoubleGreaterEqNaNIsCelError,
      DoubleEqualityWithNaNReturnsFalseNotTrap,
      DoubleOrderedCompareNonNaNStillWorks,
      DoubleDivZeroProducesInfNotTrap}`.
- [ ] String coercion errors where the spec forbids them.
- [x] `unknown` propagation through `&&` / `||` (M4).  **Slice C/3b2
      (2026-04-20)** wired `cel_and` / `cel_or` / `cel_not` into
      codegen; **Slice E2a.1 (2026-04-20)** added the first non-
      synthetic UNKNOWN producer.  `eval_test::EvalE2EUnknownTest`
      flips this row by running end-to-end expressions where a host-
      marked attribute flows into `&&` / `||` via `c.is_premium`.
- [x] Partial-eval: `unknown && false → false` commutatively (M4).
      Covered by `cel_runtime_test` (25×2 helper truth table) plus
      `EvalE2EUnknownTest::PartialMatchDoesNotShortCircuit` and
      related e2e tests that run the absorber end-to-end on a real
      host-emitted UNKNOWN.
- [x] **UNKNOWN producer via `cel_host.get_field`.**  Slice E2a.1
      (2026-04-20).  `EvalE2EUnknownTest` — 9 passing tests across
      full-match, partial-match, no-match, wildcards, bare-variable
      patterns, multi-pattern dispatch, and `SetUnknownPatterns`
      idempotency.  `AttributePattern::IsMatch` unit coverage lives
      in `compiler/host/attribute_test.cc` (18 cases over
      `{NONE, PARTIAL, FULL}` × wildcards × parse errors), and the
      codegen→ABI invariant (pre-order attr_id assignment) is
      pinned by `abi_test::BuildPopulatesAttributeTable…`.
- [ ] **3VL absorption in non-absorbing ops (Slice F).**  Every
      expression where an ERROR / UNKNOWN subtree flows through a
      non-absorbing intermediate op (arithmetic, comparison,
      NaN-compare, string op, message equality) into a 3VL
      absorber (`&&` / `||` / `?:`).  Full enumeration (22 rows) in
      `m4-slice-f-3vl-absorption.md`.  **F1 shipped 2026-04-20**:
      ERROR-source rows 1, 2, 3, 5 and UNKNOWN-source rows 9, 10,
      11, 12, 13, 20 are enabled in `eval_test.cc` and pass against
      the new `cel_cmp_*` 3VL boxed comparison helpers.
      **Uniform-boxed Step 2 shipped 2026-04-20**: arithmetic now
      takes and returns CelValue offsets via `cel_int_*_at_vv` /
      `cel_uint_*_at_vv` helpers, so ERROR / UNKNOWN operands
      absorb into the sret slot via `cel_status_either` instead of
      early-returning from `$eval`; rows 4, 18, 22 enabled
      (`ThreeValuedAbsorptionErrorArithThenCompareAbsorbed`,
      `UnknownThroughArithThenCompareAbsorbed`,
      `UnknownAndErrorInArithSubtreeErrorDominates`).
      **Uniform-boxed Step 3 shipped 2026-04-20**: dropped the
      `HasNonOkProducer` gate; every scalar / bool comparison now
      goes through `LowerBoxedComparison`, and the scalar-compare
      emitters (`ScalarEqualityOp`, `OrderedIntOp`/`Uint`/`Double`/
      `CompareOp`, `LowerDoubleOrderedCompare`, `UnboxBool`) are
      deleted — NaN-in-ordered-compare → ERROR is now handled in
      the runtime by `cel_cmp_double_{lt,le,gt,ge}`.  Row 6 enabled
      (`ThreeValuedAbsorptionNaNCompareAbsorbed`).
      **Uniform-boxed Step 4 shipped 2026-04-20**: string / bytes /
      size helpers now absorb non-OK and return CelValue offsets via
      the 9 new `cel_{string,bytes}_{eq,concat,starts_with,ends_with,
      contains,size}_v` helpers; `LowerSpanConcat` /
      `LowerSpanEquality` / `LowerStringMemberCall` swapped to the
      `_v` variants, and `!=` on strings / bytes now emits
      `cel_not(...eq_v(...))` so UNKNOWN / ERROR ride through instead
      of collapsing via `i32.eqz`.  Rows 15 / 16 / 17 / 21 enabled
      (`UnknownThroughStringEqAbsorbedByOr`,
      `UnknownThroughStartsWithAbsorbedByAndFalse`,
      `UnknownThroughBytesEqAbsorbedByOr`,
      `UnknownThroughSizeThenCompareAbsorbed`).  Runtime: 37 new
      unit tests over happy path, left / right / both absorption,
      ERROR-dominates-UNKNOWN, kind mismatch, and zero-offset.
      **Uniform-boxed Step 5 shipped 2026-04-20**: message equality
      now absorbs UNKNOWN / ERROR sub-messages before invoking the
      host's descriptor-aware `message_eq`.  Added runtime helper
      `cel_message_eq_prologue_v(a, b)` (0 on OK-OK, dominant non-OK
      via `cel_status_either` otherwise, TYPE_MISMATCH on non-message
      kind / zero offset) + codegen `LowerMessageEqualityBoxed` which
      sets a/b/p locals, emits `BinaryenIf` over the prologue result,
      and on the OK path calls
      `cel_make_bool(message_eq(cel_unwrap_message(a),
      cel_unwrap_message(b)))` (with `i32.eqz` wrap for `_!=_`).
      `LowerExprBoxed` kMessage branch wraps ident-sourced externrefs
      via `cel_wrap_message` and routes selects through
      `LowerSelectFieldBoxed`.  Row 14 enabled
      (`UnknownThroughMessageEqAbsorbedByOr`) over the
      `billing_address` sub-message.  Runtime coverage: 8 new tests
      over happy OK-OK, left-/right-UNKNOWN, both-UNKNOWN-merge,
      ERROR-dominates-UNKNOWN, non-message left / right, zero offset.
      Codegen shape tests:
      `MessageEqualityLowersThroughPrologueAndHostCall` +
      `MessageInequalityInvertsEqCallOnOkBranch`.  The pre-step-5
      `LowerMessageEquality` emitter is left in-tree for the step 7
      sweep (no remaining in-tree callers).
      **Uniform-boxed Step 6 shipped 2026-04-20**: ternary simplify.
      Added `LowerConditionalBoxed` that returns a CelValue offset (no
      `$eval` early-return): cond lowered via `LowerExprBoxed`, kind
      byte probed against `CEL_UNKNOWN` — non-OK branch returns the
      cond offset verbatim, OK branch unboxes via `cel_bool_from_value`
      and selects t/f via `BinaryenIf`.  `LowerExprBoxed` dispatches
      any `CONDITIONAL` call to it regardless of Repr; `LowerShortCircuit`
      and `LowerLogicalNot` now lower operands via `LowerExprBoxed` so
      a nested ternary flows through the boxed form.  Root-ternary
      `LowerConditional` (scalar path with sret early-return) stays
      for the eval-root case where no absorber wraps the ternary.  Row
      8 enabled (`ThreeValuedAbsorptionTernaryResultAbsorbedByOr`);
      added row 19 (`UnknownInTernaryCondAbsorbedByOr`) over the
      Customer fixture's `c.age > 0 ? 1 : 2` under `== 1 || true`; row
      7 remains spec-correct (root ternary bubbles ERROR-in-cond).
      Codegen shape test: `TernaryUnderAbsorberLowersThroughBoxedForm`
      asserts `cel_bool_from_value` + `cel_mem_base` + `cel_or` are
      all present.  No DISABLED rows remain for Slice F; step 8 (doc
      sweep) is next.
      **Uniform-boxed Step 7 shipped 2026-04-20**: dead-code sweep.
      Caller-grep audit (macOS Apple clang blocks
      `llvm-cov --show-functions`) identified the post-Slice-F
      orphaned public ABI.  Deletions: `cel_string_eq`, `cel_bytes_eq`,
      `cel_string_concat`, `cel_bytes_concat`, `cel_int_neg_at_i`,
      `cel_int_neg_at_v`, and the `LowerMessageEquality` scalar C++
      emitter.  Static-ified (still referenced by their `_v` siblings
      as single-source-of-truth): `cel_string_starts_with`,
      `cel_string_ends_with`, `cel_string_contains`,
      `cel_string_size`, `cel_bytes_size`.  `LowerSizeCall` now emits
      `cel_int_from_value(cel_*_size_v(...))` so there's one code
      path for size() regardless of absorber context.  Test-suite:
      40+ `RuntimeTest` cases removed (the `_v` sibling tests cover
      the same semantics); `expr_lower_test.cc` and
      `runtime_link_test.cc` import/export exhaustive lists trimmed;
      `Size{String,Bytes}LowersToRuntimeCall` now asserts the
      `cel_int_from_value` outer + `cel_*_size_v` inner compose.

**M4 slice C commit 3b2 (2026-04-20): bool-as-CelValue + 3VL in
`&&` / `||` / `!` / `?:`.**  Bool values now travel as CelValue
offsets across the whole codegen pipeline — literals via
`cel_make_bool`, every bool-producing call site (`has(msg.f)`,
`starts_with` / `ends_with` / `contains`, ordered / equality
comparisons) wraps its i32 result in `cel_make_bool`, and bool
`$eval` params are auto-boxed at the prologue.  `&&` / `||` /
`!` dispatch to the slice A 3VL helpers (`cel_and` / `cel_or` /
`cel_not`) on boxed operands.  `?:` unboxes its condition via
`cel_bool_from_value` (two-valued stopgap — tracked in the
design-doc §10.2.1 until the 3VL ternary decision lands).
Coverage: the existing e2e suite exercises the new shape end-to-
end — `eval_test::{BoolLiteralTrue, BoolLiteralFalse,
LogicalAndShortCircuit, LogicalOrShortCircuit, LogicalNot,
Ternary, MixedArithAndConditional, BoolVariableInTernary,
HasComposesWithLogicalNot, HasAndFieldCompareTernary,
MultiParamTwoMessagesConditional}` all pass against the new
`cel_make_bool` + `cel_and` / `cel_or` / `cel_not` codegen.
Non-obvious coverage holes still open: the 3VL truth-table for
`cel_and` / `cel_or` (25 × 2 cases) and the `unknown`-in-`?:`
semantics — both deferred to later M4 slices.  Dead-runtime
audit in the same commit: `cel_box_bool`, `cel_int_*` /
`cel_uint_*` non-sret, their `_ii` / `_uu` scalar variants,
boxed `_at` variants, `cel_int_neg*`, and the
`propagate_status_at` / `*_binop_prelude` helpers were all
removed — the `cel_runtime_test.cc` cases that covered them went
with them, and the sret `_at_ii` / `_at_uu` / `_neg_at_i`
coverage (from slice C commit 2) stands unchanged.

**M4 slice E2a.2 (2026-04-20): `celwasmc-eval` CLI + `--unknown_attrs`.**
New cc_binary at `compiler/cli:celwasmc-eval` that compiles a CEL
expression and runs it under wasmtime in-process.  Adds
`--unknown_attrs=var.q[,...]` — each pattern is parsed via
`ParseUnknownAttributePattern` and installed through
`LoadedEval::SetUnknownPatterns`.  Platform-gated to darwin-arm64
alongside every other `host_loader` consumer.  Variables are bound
to zero / null-externref arguments (null externref uses the
`wasmtime_val_t{}`-zero-init convention that
`wasmtime_externref_is_null` treats as null); this is sufficient
for partial-eval testing because a pattern-covered access never
dereferences the null handle.  Shell coverage in
`compiler/cli:celwasmc_eval_test` (8 cases):
nullary-int `1 + 2 * 3 → 7`, nullary-double `1.5 + 2.5 → 4`, bool
`&&` / `||`, partial-eval exact `c.age / --unknown_attrs=c.age →
unknown`, wildcard `c.*`, multi-pattern dispatch, malformed-pattern
diagnostic (must mention `--unknown_attrs`), missing-`-e` usage.
Out of scope: decoding string / bytes / message results (the slot
offset is printed as an i32 today); flag for non-null variable
values (deferred — add `--var_value=name=json` if / when needed).

## Bench harness (2026-04-20)

Google Benchmark suite under `compiler/bench/`.  Not coverage; a
regression tripwire for the eval + compile hot paths.  Three
`cc_binary` targets: `eval_bench`, `compile_bench`, `proto_bench`.

Run with `bazel run -c opt //compiler/bench:eval_bench -- \
--benchmark_filter=...`.  `-c opt` is load-bearing — fastbuild numbers
are 100× noisier.  CI builds the targets but does not execute them;
numbers are captured ad-hoc (no baseline checked in).

Matrix:
  - Arithmetic / logic depth (AddChain, MulChain, AndChain, OrChain,
    TernaryLadder) — Arg sweep over N.  N capped at 28 for chains and
    24 for ternaries because cel-cpp's parser `max_recursion_depth`
    defaults to 32; bumping it would measure a different thing than
    shipping.
  - String / bytes length L ∈ {10, 100, 1k, 10k} for literal pass-
    through, `size(...)`, `== 'lit'`, `+` concat.  Both "match" and
    "mismatch-at-front" for `==` so early-exit is a separate number.
  - Proto field reads against the Customer fixture: every scalar
    kind, nested select, `has(…)`, compound predicates.
  - Compile path times ParseAndCheck + Lower + Serialize + LoadEval as
    one unit (users of the library pay all four).

First-run findings (2026-04-20, darwin-arm64, `-c opt`):
  - `CallEval` floor ≈ 420 ns — wasmtime boundary + `cel_reset` +
    enter/exit wasm, same whether the expression returns `42` or a
    deep chain.  This dominates anything with <28 ops.
  - Per-op cost ≈ 3–4 ns for arithmetic and `&&` / `||` on the
    CelValue-offset bool path.
  - String literals scale O(L) per call (pass-through: 2.0 µs @ 10k,
    size: 4.6 µs @ 10k) — not because `size`/pass-through walk the
    bytes, but because `cel_reset` wipes the arena between calls, so
    each iteration re-materialises the literal.  If short-string
    eval cost becomes a bottleneck, caching the literal arena
    prefix across `cel_reset` is the lever.
  - `cel_string_eq` short-circuits correctly: mismatch-at-front at
    L=10000 is ~4 µs faster than full-match, matching the 9999-byte
    memcmp we skip.

Updating the suite: add a new `BENCHMARK(...)` in the matching file,
rebuild with `-c opt`, re-run to confirm the new case isn't an
outlier.  No gate on the numbers — this is a reference, not a
regression test.

## Tooling — cel_log host import (shipped 2026-04-20)

Not a CEL-language feature, so no type-×-stage row applies.  Listed
here so the "feature → test" audit is still exhaustive.

  - [x] Runtime-side macro (`CEL_LOG` + `CEL_LOG_STR`/`_INT`/`_UINT`/
    `_F64`/`_BOOL`/`_V`) in `compiler/runtime/cel_runtime.h`;
    `CEL_LOG_DISABLED` compile-out switch.  Import symbol
    `cel_env.cel_log` asserted present on runtime.wasm by
    `compiler/codegen/runtime_link_test.cc::ImportsCelLogFromCelEnv`.
  - [x] Host decoder + sink (`compiler/host/cel_log.{h,cc}`, target
    `//compiler/host:cel_log`).  Format directives `%s %d %u %f %b
    %v %%`; `%v` pretty-prints every CelKind.  Coverage:
    `compiler/host/cel_log_test.cc` (33 tests) — every directive,
    every CelKind for `%v`, edge cases (empty fmt, argc short, OOB
    string / argv pointer, trailing `%`, unknown directive).
  - [x] Wasmtime trampoline `RegisterCelLog` binds the import on the
    two-module linker.  `host_loader.cc` creates the linker
    up-front and uses `wasmtime_linker_instantiate` for both
    modules.  E2E coverage:
    `compiler/host/host_loader_test.cc::CelLogSinkCapturesRuntimeEnterLines`
    asserts a capture sink receives `cel_runtime.c … enter` lines
    from a real `'hello'` eval.
  - [x] Codegen declares `cel_log` alongside every other runtime
    import so eval modules see a consistent import set.  Coverage:
    `compiler/codegen/expr_lower_test.cc`'s exhaustive
    `EvalModuleDeclaresRuntimeFunctionImports` list.
  - [x] Every public (non-`static`) helper in `cel_runtime.c` opens
    with `CEL_LOG("enter")`.  Consumed by dead-code sweeps (F-Step 7
    pattern) — an unseen `enter` after the full test suite flags a
    delete candidate.

## Rewrite M1 (shipped 2026-04-22)

The runtime-isolation slice (8 commits on `master`,
`825f2e3..5085f46`) closed M1.  Per
`rewrite/m1-scalar-pipeline.md §6.3`, M1 flips the rows below.
This block is the rewrite-tier counterpart to the v1 grids above —
v1 stays in its own sections; v2 (everything under `compiler/`)
is tracked here.

  - [x] Static-literal lowering × bool —
        `eval/instance_test.cc::EvalsBoolLiteralTrue/False`
  - [x] Static-literal lowering × int —
        `eval/instance_test.cc::EvalsIntLiteral`,
        `EvalsNegIntLiteral`
  - [x] Static-literal lowering × uint —
        `eval/instance_test.cc::EvalsUintLiteral`
  - [x] Static-literal lowering × double —
        `eval/instance_test.cc::EvalsDoubleLiteral`
  - [x] Static-literal lowering × null —
        `eval/instance_test.cc::EvalsNullLiteral`
  - [x] Static-literal lowering × string —
        `eval/instance_test.cc::EvalsStringLiteral`
  - [x] Static-literal lowering × bytes —
        `eval/instance_test.cc::EvalsBytesLiteral`
  - [x] Two-phase instantiation × fresh memory per `Engine::Plan` —
        `eval/engine_test.cc::PlanCalledTwiceProducesIndependentInstances`,
        `eval/instance_test.cc::TwoInstancesEvaluateIndependently`
  - [x] No-`cel_alloc` × static-only eval — implicit by
        `EvalsNullLiteral` / `EvalsBoolLiteral` succeeding without
        binding `cel_alloc` to anything bigger than its 2-page
        host-owned memory; explicit counting-trampoline test is
        a follow-up (see `m1-scalar-pipeline.md §11`).

Bench harness for the M1 surface:
  - [x] `cel_pipeline_bench` — per-stage cost across
        Compiler::Build / Engine::Build / Compile / Plan / Eval +
        4 composite end-to-end shapes, parameterised over the 5
        scalar inputs.

Architectural deltas vs as-written M1 plan (see
`rewrite/two-phase-runtime-isolation.md §4-5`):
  - [x] Compiler / Program / Engine / Instance role split (vs the
        as-written 3-class plan that pinned wasmtime to Compiler).
  - [x] Host-allocated `cel.memory` imported by both expr +
        runtime (vs the as-written "expr defines memory; runtime
        imports it").
  - [x] `host_loader.{h,cc}` deleted; role split across
        `api/engine` + `api/instance`.

## Rewrite M2 (shipped 2026-04-24)

The ident / select / has / unknowns slice closed M2.  Per
`rewrite/m2-ident-select-unknowns.md §6.3 / §6.4`, M2 flips the
rows below.  v1 grids above are untouched; v2 coverage (everything
under `compiler/`) is tracked here.

**Slice M2.A — `cel::Activation`**

  - [x] `Bind` / `BindLazy` / `Find` per scalar kind —
        `eval/activation_test.cc`
  - [x] `Find` on unbound → `NotFoundError`,
        `BindLazy` memoises across `Find` —
        `activation_test.cc`

**Slice M2.B — `kIdent` lowering + `Eval(Activation)` +
`cel.abi.variables[]`**

  - [x] `kIdent` emits `local.get` (workspace slot) —
        `compiler/codegen/expr_lower_test.cc`
  - [x] `Instance::Eval(Activation)` per scalar kind —
        `e2e/m2_test.cc::IdentE2ETest::{Bool,Int,Uint,
        Double,String,Bytes}`
  - [x] Unbound declared variable → `FailedPrecondition` —
        `m2_test.cc::IdentE2ETest::UnboundDeclaredVariableFailsPrecondition`
  - [x] Back-to-back Eval rebinds ident cleanly —
        `m2_test.cc::IdentE2ETest::BackToBackEvalRebindsIdent`
  - [x] `cel.abi.variables[]` serialised with name /
        local_index / slot_offset / repr —
        `abi/cel_abi_emit_test.cc`
  - [x] ABI wire round-trip through proto serialization —
        `cel_abi_emit_test::EmittedProtoSerializesAndRoundTripsThroughProtoParse`

**Slice M2.C — `kSelect` + host-ABI trampoline + `cel.abi.fields[]`**

  - [x] `kSelect` lowering × every proto scalar field kind
        (string / int32 / int64 / uint32 / uint64 / double / bool
        / bytes) —
        `m2_test.cc::SelectE2ETest::Select{String,Int32,Int64,
        Uint32,Uint64,Double,Bool,Bytes}`
  - [x] Nested select (message → message → scalar) —
        `m2_test.cc::SelectE2ETest::SelectNestedMessageField`,
        `SelectSelfRecursiveInnerField`,
        `SelectThreeHopSelfRecursive`
  - [x] Proto3 unset-scalar reads back as zero-default —
        `m2_test.cc::SelectE2ETest::SelectUnsetProto3StringReturnsDefault`
  - [x] Back-to-back Eval with different message bindings —
        `m2_test.cc::SelectE2ETest::BackToBackEvalWithDifferentMessages`
  - [x] Unit kSelect lowering shape —
        `eval/instance_test.cc::InstanceSelectEvalTest::{
        IntFieldOnMessageRoundTrips, BoolFieldOnMessageRoundTrips,
        NestedSelectReadsSubBackingString}`
  - [x] `cel.abi.fields[]` dense with sentinel at 0 —
        `cel_abi_emit_test::FieldRefsEmittedDenselyWithSentinelAtZero`
  - [x] Envelope boundary: `kSelect` on REPEATED field returns
        `CEL_ERR_TYPE_UNSUPPORTED` —
        `m2_test.cc::EnvelopeBoundaryE2ETest::SelectRepeatedFieldReturnsUnsupportedError`

**Slice M2.D — `has()` dispatch via `test_only`**

  - [x] `has()` × populated scalar / message field —
        `m2_test.cc::HasE2ETest::{StringFieldSetReturnsTrue,
        NestedMessageSetReturnsTrue}`
  - [x] `has()` × unset field returns false —
        `m2_test.cc::HasE2ETest::{StringFieldUnsetReturnsFalse,
        NestedMessageUnsetReturnsFalse}`
  - [x] `has()` on nested / two-hop paths —
        `m2_test.cc::HasE2ETest::{TwoHopHasSet,
        TwoHopHasUnsetLeafReturnsFalse}`
  - [x] Unit `has()` dispatch shape —
        `instance_test::InstanceSelectEvalTest::{HasMessageFieldSetReturnsTrue,
        HasMessageFieldUnsetReturnsFalse}`

**Slice M2.E — `AttributePattern` + `Instance::PartialEval`**

  - [x] `AttributePattern::Parse` × each wildcard position +
        every rejection case —
        `eval/attribute_test.cc` (24 parse tests:
        single-segment, dotted, wildcard-mid / trailing, array /
        map keys, leading / trailing / consecutive dot rejection)
  - [x] `PartialEval` × leaf unknown short-circuit —
        `m2_test.cc::UnknownE2ETest::LeafUnknownShortCircuits`
  - [x] `PartialEval` × nested-chain absorbs at first unknown
        hop —
        `m2_test.cc::UnknownE2ETest::NestedChainAbsorbsAtFirstUnknownHop`
  - [x] `PartialEval` × wildcard mid-path matches —
        `m2_test.cc::UnknownE2ETest::WildcardMidPathMatches`
  - [x] `PartialEval` × non-matching pattern passes through to
        real value —
        `m2_test.cc::UnknownE2ETest::NonMatchingPatternsPassThrough`
  - [x] Eval vs PartialEval parity with empty pattern set —
        `m2_test.cc::UnknownE2ETest::EvalVsPartialEvalParityWithNoPatterns`
  - [x] `has()` absorbs UNKNOWN at target —
        `m2_test.cc::UnknownE2ETest::HasAbsorbsUnknownAtTarget`
  - [x] Root-ident unknown short-circuits `kSelect` —
        `m2_test.cc::UnknownE2ETest::RootIdentUnknownShortCircuitsSelect`
  - [x] Unit `PartialEval` behaviour —
        `instance_test::InstancePartialEvalTest::{MatchingPatternAbsorbsSelectToUnknown,
        NonMatchingPatternFallsThroughToRealValue,
        WildcardPatternMatchesAnyFieldUnderRoot,
        EmptyPatternSetBehavesLikeEval}`
  - [x] `cel.abi.attributes[]` interned densely with sentinel
        at 0 — `cel_abi_emit_test::EmptyWhenNoVariablesReferenced`
        (sentinel check) + `resolve_pass_test` (dense interning
        per rooted path)

**Slice M2.F — conformance harness envelope for unknowns**

  - [x] `IsInM2Envelope` admits `unknown:` / `any_unknowns:`
        matchers; `RunOne` routes them to `PartialEval` with
        empty pattern set — `conformance/runner.{h,cc}`.
  - [x] `run_conformance` shows no regressions vs the M1
        snapshot (`total=2454 · pass=178 · skip=1935 · fail=341`)
        — `conformance/README.md` inventory table
        refreshed.

Architectural deltas vs as-written M2 plan (see
`rewrite/m2-ident-select-unknowns.md §Plan-vs-execution deltas`):

  - [x] `VariableDecl` → `VariableDeclaration` to avoid ODR
        collision with cel-cpp's `cel::VariableDecl`.
  - [x] `local_types` dropped from `ResolveOutput` /
        `StaticLayout` (no information beyond `variables.size()`).
  - [x] Layer-3 wasmtime glue moved to a dedicated
        `api/internal/cel_host_wasmtime.{h,cc}` (vs the as-
        written single-file `cel_host` plan); decouples
        runtime-agnostic Layer 2 from wasmtime-specific Layer 3.
  - [x] `api/internal/abi_decode` refactored mid-milestone to
        return `celwasm::abi::CelAbi` directly — mirror structs
        (`DecodedCelAbi` / `DecodedVariable` / `DecodedField`)
        deleted.
  - [x] `Compiler::Builder::RegisterMessageType` deleted — dead
        API, never read. `ProtoBacking` uses
        `msg->GetDescriptor()` directly.
  - [x] LE endianness static assertion added at the
        `sizeof(CelValue) == 24` site in `runtime/cel_data.h`.

### Rewrite M3 — map literals + indexing (shipped 2026-04-24)

**Slices M3.A–G — runtime + codegen + host surface**

  - [x] `CelKind` split (`CEL_MAP_ARENA = 8`, `CEL_MAP_HOST = 9`,
        kinds renumbered down) + `ArenaMapHeader` (16 B) —
        `runtime/cel_data.h`; size invariants in
        `runtime/cel_data_test.cc`.
  - [x] Arena map primitives — `cel_map_create` / `cel_map_insert`
        / `cel_map_lookup_arena` covered by
        `runtime/cel_map_test.cc` parameterized over every scalar
        key kind (bool/int/uint/string), boundary values
        (`INT64_MIN/MAX`, `UINT64_MAX`, embedded NUL, multi-byte
        UTF-8), and disallowed key kinds (every CelKind that
        isn't a valid map key).
  - [x] `__attribute__((musttail))` kDynamic dispatcher
        (`cel_map_lookup` arming `cel_map_lookup_arena` /
        `cel_host_cel_map_lookup`) — `runtime/cel_runtime.c`;
        wasmtime tail-call config flipped on in `api/engine.cc`
        + `tools/wat_runner/wat_runner.cc`.
  - [x] `HostMap` + `ProtoMap` concrete `HostMapBacking` impls —
        `api/internal/cel_host.{h,cc}`; behaviour parity covered
        by `host_map_test.cc` (vector-backed) +
        `proto_map_test.cc` (proto-reflection-backed).
  - [x] Layer-2 trampoline `CelMapLookupImpl` — virtual call to
        `HostMapBacking::Get`, span+message marshal back into
        `out_slot`; `cel_map_lookup_impl_test.cc` exhaustive over
        absorption (`UNKNOWN` / `ERROR` on either operand),
        invalid slot, slot type mismatch, message-value
        ExternrefTable interning.
  - [x] `Value::HostMap` / `Value::Map` factories +
        `StructurallyEquals` kMap arm (pointer-identity at M3) —
        `value.h` / `cel_host.cc::Value::Map`.
  - [x] Resolve pass `MapOriginVisitor` (kCreateMap → kArena;
        kIdent/kSelect with kMap repr → kHost) — `resolve_pass.cc`;
        unit coverage in `resolve_pass_test.cc`.
  - [x] Layout pass slot allocation for kCreateMap +
        kCallExpr(`_[_]`) — `layout_pass.cc`; coverage in
        `layout_pass_test.cc`.
  - [x] expr_lower kCreateMap arm + kCall(`_[_]`) three-origin
        dispatch (kArena → `cel_map_lookup_arena`, kHost →
        `cel_host.cel_map_lookup`, kDynamic → `cel_map_lookup`) —
        `expr_lower.cc`; `expr_lower_test.cc` asserts the right
        target call shows up in the emitted body.
  - [x] `ProtoBacking::ReadField` on MAP fields wraps in
        `Value::HostMap(std::make_shared<ProtoMap>(…))` — covered
        by `proto_map_test.cc::ProtoBackingMapTest::*`.

**Slice M3.H — conformance harness envelope bump for maps**

  - [x] `IsInM2Envelope` → `IsInM3Envelope`; admits
        `value:{ map_value: … }` matchers in addition to scalar
        + unknown.  `runner.{h,cc}`.
  - [x] `CompareValue` grows a `kMapValue` arm dispatching to a
        new `CompareMap` (order-agnostic — sizes match, every
        `entries[].key` decodes via `binding_marshal::ValueFromProto`
        and is found in the cel-side `HostMapBacking` via
        structural-equality scan, value compare recurses through
        `CompareValue`).  `runner.cc`.
  - [x] `Instance::Eval` decoder grows a `CEL_MAP_ARENA` arm:
        reads `ArenaMapHeader`, walks `count` × 48-byte entries,
        recursively decodes (key, value) CelValue pairs, wraps
        in vector-backed `Value::Map(...)`. `api/instance.cc`.
  - [x] Layer-3 wasmtime glue grows a third trampoline —
        `cel_host.cel_map_lookup` (3-arg, distinct ABI from the
        4-arg field trampolines) — and `Engine::Plan` registers
        all three on the linker so the M3 runtime module
        instantiates regardless of whether the program reaches
        the host arm.  `api/internal/cel_host_wasmtime.cc`,
        `api/engine.cc::InitLinker`.
  - [x] `BindRuntimeExport` loop covers every runtime export
        the expr module may import (`cel_reset` / `cel_alloc` /
        `cel_map_create` / `cel_map_insert` / `cel_map_lookup_arena`
        / `cel_map_lookup`) — no lazy import tracking, per the
        repo "always link the runtime fully" rule.
        `api/engine.cc::InstantiateRuntime`.
  - [x] `run_conformance` snapshot refreshed — headline
        `total=2454 · pass=203 · skip=1873 · fail=378` (up
        from `186 · 1903 · 365`).  Movements: `fields/map_fields/*`
        graduates 13 tests via M3 arena dispatch; `basic.{}` /
        `basic.{"k":"v"}` pass via `CompareMap`; `parse.repeat/map_literal`
        + `plumbing.eval_results/eval_map_results` graduate now
        that the runtime instantiates with map imports bound.
        `conformance/README.md` inventory updated.

### Rewrite M4 — list literals + indexing (shipped 2026-04-25)

Plan-vs-execution delta: shipped `cel_list_create(out, count)` +
`cel_list_set(list, index, elem)` (fixed-length, no append/grow) per
direct user direction.  Past-count `set` poisons.  See
`m4-list-literals.md` Slice progress table for full slice status.

**Slices M4.A–E + G — runtime + host backings + Layer 2/3 + envelope flip**

  - [x] `CelKind` split (`CEL_LIST_ARENA = 7` re-using the slot,
        `CEL_LIST_HOST = 17` appended) + `ArenaListHeader` (16 B) +
        `kCelListEntryStride = 24` + `CEL_ERR_INDEX_OUT_OF_BOUNDS = 17` —
        `runtime/cel_data.h`; size + offset invariants in
        `runtime/cel_data_test.cc::ArenaListHeaderLayout` /
        `ArenaListPayloadAliasesCorrectOffset` /
        `HostListPayloadAliasesRefSlot`.
  - [x] Arena list primitives — `cel_list_create` /
        `cel_list_set` / `cel_list_at_arena` covered by
        `runtime/cel_list_test.cc` parameterized over every
        scalar element kind plus null + boundary indices
        (negative, `== count`, multi-probe linear-scan), 3VL
        absorption on operand + index, set-past-count poison,
        set-duplicate-index overwrite, ForEach in order.
  - [x] `__attribute__((musttail))` kDynamic dispatcher
        (`cel_list_at` arming `cel_list_at_arena` /
        `cel_host_cel_list_at`) — `runtime/cel_runtime.c`; same
        toolchain config as the map dispatcher.
  - [x] `HostList` + `ProtoList` concrete `HostListBacking` impls
        — `api/internal/cel_host.{h,cc}`; behaviour parity covered
        by `host_list_test.cc` (vector-backed, every element
        kind + StructurallyEquals identity) +
        `proto_list_test.cc` (proto-reflection-backed against
        `rep_i32` / `rep_s` / `rep_b` / `rep_f64` / `rep_msg`).
  - [x] Layer-2 trampoline `CelListAtImpl` — virtual call to
        `HostListBacking::At`, scalar/aggregate marshal back into
        `out_slot` via `EncodeFieldResult`; `cel_list_at_impl_test.cc`
        exhaustive over absorption (`UNKNOWN` / `ERROR` on either
        operand), non-int index, negative + OOB indices,
        non-CEL_LIST_HOST operand, missing ref_slot, ProtoList
        backing dispatch, nested message + nested list element
        intern.  `EncodeFieldResult` factored to handle every
        aggregate kind uniformly via `EncodeAggregateIfAny`.
  - [x] `Value::List` / `Value::HostList` factories +
        `Value::ListBacking` / `SharedListBacking` accessors +
        `StructurallyEquals` kList arm (pointer-identity at M4) —
        `value.h` / `cel_host.cc`; positive coverage in
        `host_list_test.cc::ValueListTest::*`.
  - [x] Layer-3 wasmtime glue grows a `cel_host.cel_list_at`
        trampoline; `HostThreeArgTrampoline<Impl>` template
        extracted to share with `CelMapLookupTrampoline`.
        `HostExternrefTable` gains the third namespace
        (`InternList` / `LookupList` + `list_backings_` vector +
        slot-0 sentinel).  `api/internal/cel_host_wasmtime.{h,cc}`.
  - [x] Test fakes deduplicated into shared
        `cel_host_test_fakes.h` — three Layer-2 unit tests
        previously each re-implemented `FakeMemoryView` /
        `FakeExternrefTable` / `FakeArenaAllocator` and drifted
        as new namespaces (M3 maps, M4 lists) landed; now
        centralised so adding the next namespace touches one
        file.  `api/BUILD.bazel` adds the `cel_host_test_fakes`
        library; `cel_host_test.cc` + `cel_map_lookup_impl_test.cc`
        replace their inline fakes with `using` aliases.
  - [x] `BindRuntimeExport` loop covers every runtime export
        the expr module may import — added `cel_list_create` /
        `cel_list_set` / `cel_list_at_arena` / `cel_list_at`.
        `api/engine.cc::InstantiateRuntime`.  Manual-test linker
        setups (`cel_runtime_wasm_test.cc`, `wat_runner.cc`)
        also bind no-op `cel_host.cel_list_at` so the runtime
        module instantiates.
  - [x] M4.G envelope flip: `ProtoBacking::ReadField` on
        REPEATED (non-map) fields returns
        `Value::HostList(ProtoList{...})` — was
        `Value::Error(kTypeUnsupported)`.  Coverage flipped in
        `cel_host_test.cc::RepeatedReturnsHostList` +
        `Layer2DispatchTest::RepeatedFieldSurfacesAsHostList`.
        `host_fixture_proto3.proto` extended with
        `rep_s/rep_b/rep_f64/rep_msg` so per-cpp_type element
        reads can be asserted.
  - [x] M4.F codegen: `ListOriginVisitor` (`resolve_pass.cc`),
        `AggregateStorageVisitor::PostVisitList` slot allocation
        (`layout_pass.cc`), `EmitKListExpr` (`expr_lower.cc`)
        lowering kCreateList → `cel_list_create` + per-element
        `cel_list_set`, and the kCallExpr(`_[_]`) arm dispatching
        on operand `repr` (kMap → MapLookupCallTarget, kList →
        new `ListAtCallTarget`).  Tests in
        `resolve_pass_test::ResolvePassListOriginTest::*` (4),
        `layout_pass_test::LayoutPassListTest::*` (5),
        `expr_lower_test::ExprLowerListTest::*` (4 — empty,
        scalar literal create+set with index pinning, arena
        fast-path, host-bound trampoline).  WAT traces 11–15
        authored and run through `wat_runner_test` (the kDynamic
        dispatcher trace SKIPs end-to-end on a wasmtime c-api
        panic; production paths cover that arm).
  - [x] M4.G e2e completion: `m2_test::SelectRepeatedFieldReturnsHostList`
        flipped from SKIP to a green `customer.tags[0] == "tag0"`
        end-to-end test.
  - [x] M4.H activation marshaller + Eval decoder:
        `EncodeList` (interns via `ExternrefTable::InternList`,
        writes `{CEL_LIST_HOST, payload.ref_slot}`) +
        `DecodeArenaListAt` (walks `ArenaListHeader` + `count` ×
        24 B elements, recurses through `DecodeCelValueAt`).
        `instance_test.cc` adds three TESTs (empty-list literal,
        scalar list round-trip with order-aware ForEach, literal
        indexing).
  - [x] M4.I conformance harness — `IsInM3Envelope` →
        `IsInM4Envelope`, `IsAggregateMatcherKindForM4` admits
        `kListValue`, new `CompareList` (order-aware).  Conformance
        moved 203 → 212 PASSes (`lists.textproto` 0 → 4,
        `parse.textproto` 148 → 150, `fields.textproto` 13 → 14).
        Unrelated guard added: `ResolvePass` early-rejects
        `kComprehensionExpr`-bearing programs (the comprehensions
        follow-on milestone will replace with scope-aware
        resolution; M5 keeps the gate in place); fixes a crash that
        previously aborted the conformance binary on
        comprehension-bearing list_value tests.
  - [x] M4.J e2e suite — `e2e/m4_test.cc` (16 tests
        across `ListLiteralE2ETest`, `ProtoRepeatedE2ETest`,
        `ProtoRepeatedHostMsg3E2ETest`).  `Customer` proto
        gained `repeated string tags = 12` for the kHost-list
        e2e flows.  `map-list-dispatch.md §11` reconciliation
        checklist fully ticked; header flipped to "fully
        reconciled into design.md 2026-04-25".
        `scripts/run_full_suite.sh` MANUAL_TARGETS += `//e2e:m4_test`.

**M2.C.0b interim** (interleaved with M3 work)

  - [x] `CelGetFieldImpl` / `CelHasFieldImpl` shipped as
        Unimplemented-returning Layer-2 stubs (rather than
        `ABSL_CHECK(false)`) — the only callers are the wasmtime
        Layer-3 trampoline (production path crashes at the
        compile-time gate before reaching the trampoline) and
        the conformance harness, where graceful Unimplemented
        is the load-bearing behaviour.  Real bodies land in
        their own slice; unit tests in
        `cel_host_test.cc::Layer2{Absorption,Dispatch,Aliasing,UnknownPattern,HasField,CrossBacking}`
        currently FAIL against the stub and will graduate when
        the bodies land.

### Conformance unlock — Slice 0 (kString / kBytes activation encoder, shipped 2026-04-25)

`eval/instance.cc::EncodeBoundValue` now routes
`Repr::kString` and `Repr::kBytes` to a new `EncodeStringOrBytes`
arm.  Payload bytes land in a host-managed arena above
`arena_limit` (codegen's `cel_reset` ceiling), grown via
`wasmtime_memory_grow` on demand — bypasses the `cel_reset` rewind
that previously stomped any pre-eval `cel_alloc` allocations.  See
`doc/implementation-plan/rewrite/conformance-unlock-plan.md` Slice 0.

  - [x] String round-trip through Activation::Bind →
        `Instance::Eval(Activation)` →
        `eval/instance_test.cc::InstanceActivationStringEncoderTest::
        {NonEmptyStringRoundTrips,EmptyStringRoundTrips,
        EmbeddedNulSurvives,MultibyteUtf8RoundTrips,
        ArenaRewindsBetweenEvals}`
  - [x] Bytes round-trip (parallel of the string path) →
        `instance_test.cc::InstanceActivationBytesEncoderTest::
        {NonEmptyBytesRoundTrips,EmptyBytesRoundTrips}`
  - [x] Kind-mismatch (declared string, bound int) → InvalidArgument →
        `instance_test.cc::InstanceActivationStringEncoderTest::
        KindMismatchRejected`
  - [x] E2E composing string-bound activation with `+` / `size()` →
        `e2e/m5_test.cc::StringBytesActivationE2ETest::
        {BindStringPlusLiteral,BindBytesSize,BindEmptyString,
        BindEmbeddedNul,BindMultibyteUtf8,BindTwoStringsConcat,
        RebindAcrossEvalsRewindsArena,BindBytesWithNul}`
  - [x] Conformance harness flips kString-blocked SKIPs to live
        Eval — `namespace.textproto` 3 → 4 PASS; total 486 → 490
        (full per-fixture deltas in
        `conformance/README.md` post-Slice-0 row).

### Rewrite M7 — proto message literals (slices A–E shipped 2026-04-25)

`m7-proto-literals.md` slices A–E delivered `kStructExpr` codegen
+ `cel_make_message` / `cel_set_field` host primitives + per-cpp_type
scalar/repeated/map/oneof/enum/nested-message dispatch +
`InlineConstantReferences` rewrite for cel-cpp's enum-name-as-constant
resolution.  Conformance: **+131 PASS** (700 → 831).  Plan-vs-execution
delta and remaining unblockers captured in `m7-proto-literals.md` §9.

**M7.A — `kStructExpr` admission + `cel_make_message`**

  - [x] `cel.abi.types[]` ABI table — `abi/cel_abi.proto`
        `TypeEntry { id, fully_qualified_name }` (FQN-only, descriptor
        resolved at Plan time, mirroring `FieldEntry.owner_fqn`).
        Serialized in `cel_abi_emit.cc::BuildCelAbi`.
  - [x] `NodeAnnotation::message_type_id` (M7.A) — `ir/annotations.h`.
  - [x] `MessageTypeIdVisitor` ResolvePass — walks kStructExpr post-order,
        interns FQN, stamps id; sentinel id 0 pushed before traversal.
        `codegen/resolve_pass.cc`.
  - [x] `AggregateStorageVisitor::PostVisitStruct` allocates a
        workspace slot per kStructExpr; releases entry-value slots
        for the M7.B layered set-field calls.  `codegen/layout_pass.cc`.
  - [x] `EmitKStructExpr` codegen arm — emits
        `cel_host.cel_make_message(type_id, out_slot)` followed by
        per-entry `cel_host.cel_set_field` calls.
        `codegen/expr_lower.cc`.
  - [x] `InstallStructImports` adds `cel_make_message` (2-arg) +
        `cel_set_field` (3-arg) imports.  `compiler/internal/compile.cc`.
  - [x] `OwnedProtoBacking` (composes `ProtoBacking` for read-side;
        owns `unique_ptr<Message>`) + virtual `HostMessageBacking
        ::message()` so `cel_message_eq` works for both M7-built and
        M2.C-bound messages.  `api/internal/cel_host.{h,cc}`.
  - [x] `CelMakeMessageImpl` Layer-2 — resolves type_id → Descriptor,
        `MessageFactory::generated_factory()->GetPrototype(desc)
        ->New()`, intern, write CEL_MESSAGE.
        `api/internal/cel_host.cc`.
  - [x] `CelMakeMessageTrampoline` Layer-3 (2-arg) registered in
        `RegisterCelHostImports`.  `api/internal/cel_host_wasmtime.cc`.
  - [x] `BuildCelHostBindings` resolves FQN → Descriptor* via
        `DescriptorPool::generated_pool()` for every `cel.abi.types[]`
        row.  `api/internal/cel_host_wasmtime.cc`.
  - [x] M2 read-side null fix: `ProtoBacking::ReadField` on unset
        singular message returns `Value::Null()` per langdef
        §"Field Selection" (was returning default-instance backing).
        `api/internal/cel_host.cc`.
  - [x] `wat/40_kstruct_make_message.wat` + `wat-traces.md` §40.
  - [x] E2E: `e2e/m7_test.cc::ProtoLiteralEmptyE2ETest`
        — 9/9 PASS (proto3 zero/explicit-default/null-submessage,
        proto2 explicit-default×3, Customer empty).

**M7.B — `cel_set_field` for scalar fields**

  - [x] `kCelHostSetFieldInternalName` constant + 3-arg `cel_set_field`
        import.  `codegen/expr_lower.h`, `compiler/internal/compile.cc`.
  - [x] `EmitCelSetFieldCall` helper + per-entry emit loop in
        `EmitKStructExpr`; field_ref_id interns per entry with
        `field_number=0` + `name` + `owner_fqn=s.name()`.
        `codegen/expr_lower.cc`.
  - [x] `CelSetFieldImpl` Layer-2 — resolves OwnedProtoBacking via
        `dynamic_cast` (host-bound `ProtoBacking` rejected — read-only
        invariant), per-cpp_type dispatch via `SetScalarField`:
        BOOL/INT32/INT64/UINT32/UINT64/FLOAT/DOUBLE/STRING (TYPE_STRING
        vs TYPE_BYTES)/ENUM.  `api/internal/cel_host.cc`.
  - [x] `CelSetFieldTrampoline` Layer-3 (3-arg, reuses
        `HostThreeArgTrampoline`).  `api/internal/cel_host_wasmtime.cc`.
  - [x] `wat/41_kstruct_set_scalar.wat` + `wat-traces.md` §41.
  - [x] E2E: `m7_test.cc::ProtoLiteralScalarE2ETest` — parameterized
        scalar matrix (10 cpp_types × boundary values) + sint/fixed/
        sfixed wire variants + ident/computed-expr source operands +
        multi-entry literals; all PASS.

**M7.C — repeated + map fields**

  - [x] `ForEachArenaListElement` / `ForEachArenaMapEntry` walkers
        (read `ArenaListHeader` / `ArenaMapHeader` + per-element
        CelValue read via MemoryView).  `api/internal/cel_host.cc`.
  - [x] `AppendRepeatedFromCelValue` (arena source) +
        `AppendRepeatedFromHostListValue` (host source) — per-cpp_type
        `Add...` dispatch incl. CPPTYPE_MESSAGE via
        `AddMessage(...)` + `CopyFrom`.
  - [x] `InsertArenaMapEntry` + `InsertHostMapEntry` — build a fresh
        map-entry submessage via `Reflection::AddMessage` then populate
        key + value via shared `SetScalarField` (arena) or per-cpp_type
        (host) ladder.
  - [x] `SetMapField` + `SetRepeatedField` route on `value_cv.kind`
        (`CEL_LIST_ARENA`/`CEL_LIST_HOST`/`CEL_MAP_ARENA`/`CEL_MAP_HOST`).
  - [x] `CelSetFieldImpl` calls `is_map()` then `is_repeated()`
        before scalar dispatch (proto map fields are also `is_repeated()`
        per descriptor.proto).
  - [x] Descriptor-mismatch guard at every CopyFrom site so
        `TestAllTypes{single_any: BoolValue{...}}`-shape rows fail
        per-row instead of CHECK-aborting the conformance run.
  - [x] E2E: `m7_test.cc::ProtoLiteralRepeatedE2ETest` (8/8 PASS) +
        `ProtoLiteralMapE2ETest` (7/7 PASS) +
        `ProtoLiteralOneofE2ETest` (proto2 + proto3 oneof clear-on-set,
        all 6 PASS) + `ProtoLiteralActivationE2ETest`
        (list-of-message binding flowing into construction).

**M7.D — enum literals**

  - [x] `parse_and_check.cc::InlineConstantReferences` — walks AST
        in place after checker returns; replaces each `kIdentExpr`
        whose `reference_map` entry has a Constant value
        (cel-cpp's enum-name-as-constant resolution) with a
        `kConstantExpr` carrying that value.  Idempotent; runs
        before `RejectDyn` and `PopulateAnnotations`.
  - [x] cel-cpp checker probe-spike resolved (m7-proto-literals.md
        §3.3 R4): the checker emits a `Reference` with `value()`
        carrying the int constant, NOT a Constant in-place.
        Inline rewrite is the simplest correct path (cf. cel-cpp's
        runtime/reference_resolver — same approach).
  - [x] E2E: `m7_test.cc::ProtoLiteralEnumE2ETest` (4/4 PASS) —
        `Foo.Kind.KIND_SEVEN` standalone, RHS-of-field-set, unset
        reads as 0, int round-trip via `Foo{kind: 7}.kind == 7`.

**M7.E — singular message field nesting**

  - [x] `SetScalarField` CPPTYPE_MESSAGE arm — looks up source
        backing via `ExternrefTable`, asserts descriptor matches
        field's `message_type()`, `MutableMessage(msg, field)
        ->CopyFrom(*src_msg)`.  `api/internal/cel_host.cc`.
  - [x] Threaded `ExternrefTable* refs` parameter through
        `SetScalarField` + every call site (singular dispatch +
        `InsertArenaMapEntry`).
  - [x] E2E: `m7_test.cc::ProtoLiteralNestedE2ETest` (4/5 PASS;
        1 SKIP for the M8-blocked `Int32Value{value:5}.value`
        wrapper-typed-expression-in-scalar-context test).
  - [x] Equality unblock: `m7_test.cc::ProtoLiteralEqualityE2ETest`
        passes (the cohort the M5.B `cel_message_eq` kernel was
        waiting for, blocked since 2026-04-24 on M7.A construction).

**M7.F — closeout (in progress)**

  - [x] `conformance/README.md` refreshed: headline
        700 → 831 PASS; per-fixture inventory rows for `comparisons`
        / `dynamic` / `enums` / `proto2` / `proto3` / `wrappers`;
        forecast table reordered with §4.5 encoder polish + M8
        wrappers + chained-null + Any as the next four unblockers;
        plan-vs-execution delta callout added.
  - [x] `m7-proto-literals.md` status flipped to `shipped 2026-04-25
        (slices A–E)`; "What landed" / "What didn't land" callout +
        Future-work §9 captures every unblocking work item with
        rows-graduated estimates.
  - [ ] Plan-doc reconciliation across siblings (`design.md` §4.7.1
        / §4.7.5 / §11.5; `cel-host-surface.md` §6 for `cel.abi.types[]`)
        — pending.
  - [ ] `scripts/run_full_suite.sh` closeout gate run — pending; manual
        verification done via `bazel test //... --test_output=errors
        --build_tests_only` (44/45 PASS — the 45th is `m7_test`'s 1
        deliberate SKIP for M8 wrapper).

### Rewrite M10 — type conversions (slices A–E shipped 2026-05-14)

`m10-conversions.md` slices A–E delivered the type-conversion
overload surface: `bool` / `int` / `uint` / `double` / `string` /
`bytes` inter-conversions plus identity arms, all pure-runtime
helpers (no host trampolines).  `m10_test.cc` 87/87 PASS;
conformance `975 → 1058 PASS` (+83).

**M10.A — identity overloads (6 seeds)**

  - [x] `bool_to_bool` / `int64_to_int64` / `uint64_to_uint64` /
        `double_to_double` / `string_to_string` / `bytes_to_bytes`
        graduated from `kExplicitlyUnimplementedIds` to
        `kBuiltinSeeds`, all pointing at `cel_copy_slot` (the
        M5.G Slice 2 helper — its `(dst, src) → void` ABI is
        bit-identical to the unary conversion shape, and the
        24-byte CelValue memcpy preserves CEL_UNKNOWN / CEL_ERROR
        absorbing semantics for free).
  - [x] `InstallOverloadImports` bug fix — `cel_copy_slot`
        doesn't match the `_at_v` suffix convention, so the
        arity-lookup loop never declared the import.  Added it to
        `kDispatchers` with arity 2.
  - [x] `m10_test.cc::IdentityE2ETest` 6/6 PASS.

**M10.B — numeric inter-conversions (6 kernels)**

  - [x] `cel_uint_to_int_at_v` / `cel_double_to_int_at_v` /
        `cel_int_to_uint_at_v` / `cel_double_to_uint_at_v` /
        `cel_int_to_double_at_v` / `cel_uint_to_double_at_v` —
        each with `absorb_3vl_unary` prelude + kind-check +
        per-conversion body + write_*; overflow / NaN /
        negative-source poisons `CEL_ERR_OVERFLOW`.
  - [x] Double bounds use exact-representable boundaries
        `-2^63` / `2^63` / `2^64`; NaN check via `v != v` (no
        `<math.h>` in freestanding wasm32).
  - [x] BUILD exports (6) + engine.cc binds (6) + OverloadTable
        seeds (6).
  - [x] `m10_test.cc::IntFamilyE2ETest` / `UintFamilyE2ETest` /
        `DoubleFamilyE2ETest` — admit + reject (overflow / NaN /
        Inf / negative-into-uint).
  - [x] `overload_table_test::UsedImportsSilentlySkipsUnknownIds`
        — magic-number 99 replaced with
        `kBuiltinSeedCount + 1000` (the test used to hardcode 99
        as "unknown" but M10.B's six new seeds made 99 real).

**M10.C — string parsing (4 kernels + 3 subroutines)**

  - [x] `cel_string_to_int_at_v` / `cel_string_to_uint_at_v` /
        `cel_string_to_double_at_v` / `cel_string_to_bool_at_v`.
  - [x] Subroutines: `parse_int64_str` (manual overflow check —
        `__builtin_mul_overflow` on 64-bit needs `__multi3` which
        the freestanding wasm32 build doesn't link, per the M5.B
        precedent), `parse_uint64_str` (rejects leading `-`),
        `parse_double_str` (admits
        `[+-]?digits(.digits)?([eE][+-]?digits)?` plus `inf` /
        `infinity` / `nan` case-insensitive), `parse_bool_str`
        (exact-byte match against cel-cpp's 10-row truth table).
  - [x] BUILD exports (4) + engine.cc binds (4) + OverloadTable
        seeds (4).
  - [x] `m10_test.cc::StringParseE2ETest` (~20 admit + reject)
        + `StringParseBoolE2ETest` (parameterized truth-table).

**M10.D — number / bool → string formatting (4 kernels)**

  - [x] `cel_int_to_string_at_v` / `cel_uint_to_string_at_v` /
        `cel_bool_to_string_at_v` / `cel_double_to_string_at_v`.
        Outputs arena-allocated via `cel_alloc(n)` and stamped
        as `{CEL_STRING, payload.s}` — same lifetime model as
        M9.B `cel_type_of_at_v`.
  - [x] Subroutines: `write_uint_decimal` (no-leading-zero itoa
        via reverse-then-reverse buffer), `write_int_decimal`
        (handles INT64_MIN via uint64 promotion),
        `append_double_fraction` (digit-by-digit fractional
        decomposition + trailing-zero trim), `stamp_string`
        (common arena-alloc + memcpy + CelValue stamp).
  - [x] `cel_double_to_string_at_v` per §4.4: NaN → "nan";
        ±Inf → "+Inf" / "-Inf"; ±0 → "0"; for finite non-zero
        values the kernel delegates to `std::to_chars(buf, end, v,
        chars_format::general)` in `cel_convert_double_format.cc`
        (sibling C++ TU, fixed 2026-06-06 per cleanup-backlog #38) —
        shortest-round-trip representation, mirroring cel-cpp's
        `FormatDouble`
        (`runtime/standard/type_conversion_functions.cc:56`).
        Coverage matrix in `runtime/cel_convert_test.cc`:
        `DoubleFormatRoundTripTest` (corpus rows + edge cases,
        parameterised) asserts literal shortest-form pin AND
        `strtod(result) == input` round-trip safety; per-case
        `TEST_F`s for `1.0/3.0`, `min()`, `max()`, `denorm_min()`.
        E2E regression pins in `e2e/known_bugs_test.cc`:
        `DoubleToStringShortestRoundTrip` (`string(123.456)`) and
        `DoubleToStringExponentForm` (`string(1e10)`).
  - [x] BUILD exports (4) + engine.cc binds (4) + OverloadTable
        seeds (4).
  - [x] `m10_test.cc::NumberFormatE2ETest` 10/10 PASS.

**M10.E — bytes ↔ string + UTF-8 validation (2 kernels)**

  - [x] `cel_string_to_bytes_at_v` / `cel_bytes_to_string_at_v`
        — both share the source's `payload.s` span (no arena
        copy) and flip the kind tag.  Aliased slots
        (`out_slot == in_slot`) handled by reading the span
        into a local before writing.
  - [x] `utf8_valid` RFC3629 byte-wise validator — rejects
        orphan continuation bytes, overlong encodings (2/3/4
        byte), truncated sequences, UTF-16 surrogates
        (`0xED 0xA0..0xBF`), code points beyond U+10FFFF
        (`0xF4 0x90..0xBF +`, `0xF5..0xFF` leader).
  - [x] BUILD exports (2) + engine.cc binds (2) + OverloadTable
        seeds (2).
  - [x] `m10_test.cc::BytesFamilyE2ETest` 8/8 PASS (ascii /
        empty / UTF-8 round-trip + invalid-leading / orphan-cont
        / truncated / surrogate / overlong-NUL rejection).
  - [x] Two stale rows from `IntFamilyE2ETest`'s parameterized
        table — `int(true) == 1` / `int(false) == 0` — dropped:
        cel-cpp's runtime registers `bool → int` but its checker
        does NOT, so the rows check-fail.  Tracked in §9.

**M10.F — closeout**

  - [x] `m9_test.cc` regression in `cel_runtime_wasm_test` /
        `wat_runner_test` (the M9.B
        `cel_host::resolve_message_type_name` import wasn't
        declared in the C-API test harnesses) cleared by adding
        a 2-arg no-op stub in each.
  - [x] `m10-conversions.md` status flipped to `shipped
        2026-05-14` with as-shipped summary + plan-vs-execution
        deltas.
  - [x] `testing-checklist.md` — this section.
  - [x] `scripts/run_full_suite.sh --quick`: 8/8 PASS.
  - [x] Conformance: `bazel run
        //conformance:run_conformance` →
        `pass=1058 / skip=693 / fail=703` (was `975 / 781 / 698`
        pre-M10; +83 PASS / −88 SKIP / +5 FAIL).  README refresh
        + Slice 3 classifier-tightening pass land separately.
  - [ ] `cel_runtime.c` split (now ~3300 lines) — `cel-runtime-c-split-plan.md`
        is the authoritative plan; dispatched to a subagent as
        a separate closeout slice (does not block M10 ship).

### Rewrite M9 — type subsystem (slices A–F shipped 2026-05-14)

`m9-type-subsystem.md` slices A–F delivered the `CEL_TYPE` value-of-types
end-to-end: runtime helper `cel_type_of_at_v` + 12-row primitive
type-name table, `InlineTypeIdentifierReferences` frontend rewrite,
`Repr::kType` packing path, host trampoline for `type(<message>)`,
polymorphic `cel_equals` arm, plus the runner's `kTypeValue` matcher
and `typed_result:` harness routing.  Greens the `m9_test.cc`
capability matrix and unlocks the `type_value:` envelope cohort
(largest scope-not-yet-shipped bucket pre-M9 per §1 of the plan).

**M9.A — `CEL_TYPE` payload + `Value::Type` + activation roundtrip**

  - [x] `Value::Kind::kType = 13` + `Value::Type(name)` factory +
        `Value::AsType()` accessor + `TypeTag` ctor sharing the
        `std::string` Payload alternative with kString / kBytes;
        `StructurallyEquals` byte-compare arm; `ValueKindName(kType)
        → "type"`.  `api/value.{h,cc}`.
  - [x] `CelType::Kind::kType = 13` + `CelType::Type()` factory +
        `CelTypeKindName(kType) → "type"`.  `api/type.{h,cc}`.
  - [x] `CelTypeToSpec(kType) → "type"` for the public Compiler spec.
        `api/compiler.cc`.
  - [x] `cel_data.h`: removed unused `payload.type_id` field; CEL_TYPE
        values now reuse `payload.s` (CelSpan into linear memory).
  - [x] `instance.cc::DecodeCelValueAt` CEL_TYPE arm — read
        `payload.s`, copy into owned `std::string`, return
        `Value::Type(name)`.
  - [x] `instance.cc::EncodeType` + `Repr::kType` arm in
        `EncodeBoundValue` + `TotalHostStringBytes` widened to
        include kType binding sizes.  Bytes land in the host string
        arena (same lifetime as kString / kBytes).
  - [x] `binding_marshal::ValueFromProto kTypeValue` arm decodes
        `proto.type_value` to `Value::Type(name)`.
  - [x] `binding_marshal::TypeSpecFragment` + `CelTypeFromProtoType`
        admit `ProtoType::kType` (`"type"` spec keyword;
        `CelType::Type()`).
  - [x] `parse_and_check.cc::ParsePrimitiveType("type") →
        cel::TypeType{}` — `Activation::Bind("t", Value::Type(...))`
        round-trips through the variable_specs CelType.
  - [x] `ArgIsAdmissibleScalar` admits `t.has_type()` (so
        `dyn(type-value)` passes the static-subset gate).
  - [x] `cel_log.cc::FormatSpanPayload` widened to print
        `type(<name>)` from `payload.s`; old `type(id=N)`
        formatting removed.
  - [x] Unit: `binding_marshal_test::TypeProducesValueType`.
  - [x] Unit: `cel_log_test::ValueTypeKind` updated for the new
        `type(<name>)` format (in-memory bytes path).

**M9.B — `type(x)` codegen + primitive table**

  - [x] `cel_runtime.c::cel_type_of_at_v` — 18-slot
        `kPrimitiveTypeName[]` indexed by CelKind (null_type / bool
        / int / uint / double / string / bytes / list / map / type /
        google.protobuf.Duration / google.protobuf.Timestamp);
        CEL_MESSAGE arm dispatches to the host trampoline; absorbs
        3VL (CEL_UNKNOWN / CEL_ERROR); arena-allocates the name
        bytes per call.  Declared in `cel_type.h` (new file).
  - [x] `OverloadTable::kBuiltinSeeds` 85 → 86 (added
        `Seed{"type", cel_type_of_at_v}`);
        `kExplicitlyUnimplementedIds` 81 → 80 (removed `"type"`).
        `codegen/overload_table.cc`; `overload_table_test.cc`
        `kBuiltinSeedCount` bumped.
  - [x] Runtime export list: `cel_type_of_at_v` exported from
        `cel_runtime.wasm` (BUILD.bazel `--export=` + bound in
        `engine.cc::BindAllRuntimeExports`).
  - [x] `wasm_imports.txt` — `cel_host_resolve_message_type_name`
        declared so the wasm module instantiates cleanly.

**M9.C — `type(message)` + type-ident kConstant rewrite**

  - [x] `parse_and_check.cc::InlineTypeIdentifierReferences` —
        rewrites bare `kIdentExpr` nodes (whose Reference has no
        `value()` AND whose TypeSpec has `has_type()`) to
        `kConstantExpr` with `string_value = <spec type-name>`;
        runs after `InlineConstantReferences` (M7.D), before
        `RejectDyn`.  Covers primitive / wrapper / well-known /
        null / message-FQN / list / map / type-param inner kinds.
  - [x] `StaticMemoryBuilder::AllocateType` — same payload layout
        as `AllocateString` but stamps `kind = CEL_TYPE` on the
        rodata CelValue.
  - [x] `LayoutPass::ConstLayoutVisitor::Pack` dispatches on the
        annotation's `Repr::kType` before the proto-oneof ladder,
        invariant-checks that the constant has `string_value`.
  - [x] `CelResolveMessageTypeNameImpl` Layer-2 + Layer-3
        trampoline (`HostTwoArgTrampoline`) registered as
        `cel_host.resolve_message_type_name` (2 args).  Reads the
        CEL_MESSAGE backing via `ExternrefTable`, walks
        `GetDescriptor()->full_name()`, arena-copies the FQN,
        stamps `{CEL_TYPE, payload.s}`.  Defends against
        non-proto / unmapped backings with
        `CEL_ERR_HOST_ADAPTER_ERROR`.

**M9.D — CEL_TYPE equality kernel**

  - [x] `cel_runtime.c::equality_kernel` CEL_TYPE × CEL_TYPE arm
        — memcmp on `payload.s` bytes (length check, then byte
        loop reading through `cel_memory_base_()`).  Cross-kind
        (`CEL_TYPE == CEL_INT`) short-circuits to `false` via
        the existing kind-mismatch path.

**M9.E — `type(null)` / `type(list)` / `type(map)` polish**

  - [x] Names land verbatim in `kPrimitiveTypeName[]`:
        `null_type` / `list` / `map`.  Timestamp / duration
        names also pinned in the table for once construction is
        unblocked.

**M9.F — `typed_result:` matcher (harness)**

  - [x] `runner.cc::IsAggregateOrObjectMatcherKind` admits
        `kTypeValue`; `CompareValue` routes to new `CompareType`
        (kind check + byte-equal on the type-name string).
  - [x] `runner.cc::RunOne` routes `SimpleTest::kTypedResult` to
        the value-comparison path using
        `t.typed_result().result()`; rejects rows lacking a
        `result` value with a clean Unsupported reason (the
        deduced-type-only branch is a follow-up harness slice).

**M9.G — closeout**

  - [x] `m9-type-subsystem.md` status header flipped to `shipped
        2026-05-14` with as-shipped summary.
  - [x] `testing-checklist.md` — this section.
  - [x] Default suite + manual-tagged closeout gate via
        `scripts/run_full_suite.sh --quick`: 46/47 PASS (the
        47th is the M10 failing-by-design placeholder per the
        plan-doc precedent set by 7c61682).
  - [ ] Sibling-doc reconciliation (`design.md` §"Shipping
        snapshot" row for M9; `cel-host-surface.md` for the
        new `resolve_message_type_name` trampoline) — deferred
        to next closeout sweep (does not block M9 ship).

### Runtime carving (post-M10) — split-plan P1-P8 shipped 2026-05-14

Source: `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md`.

Coverage delta (each carved TU now has its own per-topic test file
where one did not already exist; existing per-topic tests stay green):

  - [x] `cel_log.c` carved (P1) — covered by host-side overrides in
        `host/cel_log_test.cc` (pre-existing).
  - [x] `cel_memory.c` carved (P2) — covered by
        `runtime/cel_memory_test.cc` (pre-existing).
  - [x] `cel_arena.c` carved (P3) — covered by
        `runtime/cel_arena_test.cc` (pre-existing).
  - [x] `cel_make.c` carved (P4) — covered by
        `runtime/cel_make_test.cc` (pre-existing).
  - [x] `cel_3vl.c` carved (P5) — covered by
        `runtime/cel_3vl_test.cc` (pre-existing).
  - [x] `cel_type.c` carved (new, M9.B body) — covered by
        new `runtime/cel_type_test.cc` (15 cases: 12 primitive
        type-name rows + 3VL absorbs + CEL_OPTIONAL reject +
        CEL_MESSAGE host-stub fall-through).
  - [x] `cel_convert.c` carved (new, M10.B/C/D/E bodies) —
        covered by new `runtime/cel_convert_test.cc` (~60 cases:
        numeric inter-conversion matrix incl. INT64_MIN/MAX +
        UINT64_MAX + NaN + ±Inf + ±0 + overflow boundaries;
        string parse matrix incl. 10-row parameterized bool truth
        table + sign / scientific / trailing-garbage rejects;
        number→string formatter matrix incl. INT64_MIN edge +
        scientific/mixed/integer paths; UTF-8 RFC3629 reject
        matrix incl. orphan continuation / overlong / truncated /
        surrogate / >U+10FFFF / invalid leader + valid 2/3/4-byte
        code points).
  - [x] `cel_arith.c` carved (P6) — covered by
        `runtime/cel_arith_test.cc` (pre-existing).
  - [x] `cel_string_ops.c` carved (P7) — covered by
        `runtime/cel_string_ops_test.cc` (pre-existing).
  - [x] `cel_compare.c` carved (P8) — covered by
        `runtime/cel_compare_test.cc` (pre-existing).
  - [ ] `cel_list.c` + `cel_map.c` (P9) — punted.  See
        split-plan §"Future work".

`cel_internal.h` introduced as the shared umbrella for cross-TU
static-inline helpers (`poison`, `absorb_3vl_*`, `require_kinds`,
`write_*`, `spans_equal`, `cv_at`, wasm-side `memcpy`/`memset`) plus
the small set of internal-extern decls (`cel_memory_base_`,
`cel_memory_size_`, `numeric_compare_kernel`, `is_numeric_kind`,
`cel_value_eq`, `map_keys_equal`).  No tests directly target the
header — it's exercised indirectly by every runtime test.

### Bench (post-M10) — first baseline shipped 2026-05-14

`manual`-tagged Google Benchmark binaries, now under
`benchmark/`.  Out of the default test suite — run explicitly
via `bazel run -c opt //benchmark/kernel:kernel_bench` /
`//benchmark/compiler:pipeline_bench`.  The 2026-05 baseline
numbers are archived in
`doc/implementation-plan/rewrite/archive/bench-tree-readme.md`.

  - [x] `kernel_bench.cc` — runtime kernel microbenches covering
        arithmetic (`cel_int_add` / `_mul` / `_div`, `cel_double_add`),
        comparison (`cel_int_eq`, cross-type ladder
        `cel_numeric_eq_at_vv`, `cel_string_eq`, `cel_bytes_eq`),
        aggregate (`cel_map_lookup_arena` hit/miss, `cel_list_at_arena`,
        kDynamic `cel_list_eq` / `cel_map_eq` dispatchers), 3VL
        (`cel_and`, `cel_or`, `cel_unknown_merge`), conversion
        (`cel_uint_to_int`, `cel_double_to_int`, `cel_string_to_int`
        sized × 3, `cel_int_to_string`, `cel_double_to_string`), and
        string ops (`cel_string_concat` sized × 3,
        `cel_string_contains` sized × 3).
  - [x] `pipeline_bench.cc` — end-to-end pipeline benches against the
        public `cel::Compiler::Compile` / `cel::Engine::Plan` /
        `cel::Instance::Eval` surface.  Caches Compiler across Compile
        iterations; pre-stages Program + Instance for Eval-only
        steady-state.  Covers `kConstantExpr`, `kSelectExpr`,
        `kCallExpr` (3-term arith, 20-term compare chain,
        `type(x) == int`, `int(string(123))`), `kCreateList`,
        `kCreateMap`, `kStructExpr`, plus the arena-vs-proto crossover
        for list indexing (`[..][2]` vs `c.tags[2]`) and map lookup
        (`{..}[k]` vs `c.metadata[k]`).
  - [ ] wasm32-side bench — Google Benchmark doesn't link
        freestanding; native-host numbers are the only baseline today.

**Optimization-lever pass (2026-05-15) — second baseline.**

Four levers landed in three commits (e0826ed / 99fd27c / 754bcaf
+ closeout doc refresh):

  - [x] **Native cc_library `-O3 + -flto`** — kernel µbench numbers
        dropped 60–80% across the leaf kernels (LTO inlines
        cel_internal.h's static-inline helpers and lets per-topic
        .c files cross-inline).  Latent-bug fix: `cel_memory.c`'s
        `g_memory` buffer got `_Alignas(8)` after -O3 surfaced a
        previously-accidental linker alignment.
  - [x] **wasm32 genrule `-O3 + -flto`** — pipeline Eval numbers
        mostly flat (Cranelift was already producing efficient
        native code from unoptimized wasm); the real pipeline
        lever turned out to be Binaryen, not wasm32 build flags.
  - [x] **`WasmModule::Optimize(level)` + `CompileOptions::
        optimize_level`** — wraps `BinaryenModuleOptimize` between
        Validate and Serialize.  Default 0 preserves byte-identical
        output; opt=2 trade is **+121–159% Compile / −52% Eval**
        on the 20-term-compare chain.  Crossover ~163 Evals — well
        below any production amortisation window for a reused
        Program.
  - [x] **`module_test::WasmModuleOptimizeTest`** — three rows:
        `LevelZeroIsByteIdentical` (locks the default-off
        invariant), `LevelTwoStillValidatesAndShrinksDeadCode`
        (proves the pass list actually runs), and
        `LevelOutOfRangeIsInvalidArgument` (closed-range
        contract).
  - [x] **`e2e/optimize_test`** (manual-tagged) — 21
        representative expressions run at both opt=0 and opt=2
        with CelValue-identity asserted.  The "Binaryen
        miscompile" gate; 9/9 manual targets now green in
        `scripts/run_full_suite.sh`.
  - [x] **`pipeline_bench` opt2 pairs** —
        `BM_{Compile,Eval}_{ThreeTermArith,TwentyTermCompare}_Opt2`
        capturing the compile-up / eval-down trade-off in the
        bench README.
  - [x] **`bench/README.md` refresh** — adds
        Build-configuration overview, How-JIT-compilation-fits-in
        section, side-by-side `-O2` vs `-O3 + -flto` columns for
        every kernel row, three-column pipeline table including
        the opt=2 variants, and Cranelift / pre-compile-cache
        notes.
  - [ ] CI bench lane — bench numbers are still captured one-off
        via `bazel run -c opt`.  A stable-environment CI lane
        (or `--benchmark_repetitions`-driven daily run) is the
        natural next step when perf becomes load-bearing.
        Future work; not a milestone blocker (host trampoline cost
        dominates pipeline numbers per the README table).
  - [ ] Comprehension benches — slated for M11 alongside the
        comprehension AST kind.

### Rewrite M7-A — google.protobuf.Any pack/unpack/equality (shipped 2026-05-16)

  - [x] **M7-A.A pack arm** — `WriteMessageOrPack` helper in
        `eval/internal/cel_host.cc` threaded through
        4 cpp_type-MESSAGE call sites (singular set, repeated
        arena append, repeated host append, host-map insert).
        One helper, three shapes (CopyFrom / Any reflection-pack /
        M8 wrapper-mismatch).
  - [x] **Fixture extension** — `compiler/testdata/host_fixture_proto3.proto`
        gains `single_any = 30`, `repeated_any = 31`,
        `map_str_to_any = 32`; `BUILD.bazel` depends on
        `@com_google_protobuf//:any_proto`.
  - [x] **M7-A.B read-side unwrap** — `UnpackAnyToValue` in
        `eval/internal/cel_host.cc` parses type_url +
        value against the Any descriptor's own pool; wired into
        `ProtoBacking::ReadField`'s CPPTYPE_MESSAGE arm.  Error
        envelope per probe B: empty type_url → null, FQN
        unresolved → `kFieldNotFound`, corrupt bytes →
        `kTypeMismatch`.
  - [x] **Frontend §3.5.A carve-out** — `IsSelectThroughAny` in
        `compiler/frontend/parse_and_check.cc` admits SelectExpr
        nodes whose operand types as `google.protobuf.Any`,
        recursive so chained selects through Any land too.  This
        is the dyn-gate concession that makes Any usable for
        customers (cel-cpp parity).
  - [x] **M7-A.C equality peel** — `PeelAnyForEq` in
        `CelMessageEqImpl` unpacks Any operands before
        `MessageDifferencer::Equals`; handles Any-vs-typed,
        typed-vs-Any (symmetric), Any-vs-Any cross-descriptor.
  - [x] **Layer-2 byte-level tests** — 8 cases in
        `cel_host_test.cc::CelSetFieldAnyPackTest` (round-trip,
        empty payload, cross-syntax, CopyFrom branch, M8 mismatch
        rejection, null-clear ordering, repeated-host, map-host).
  - [x] **E2E coverage** — `e2e/m7a_test.cc`:
        AnyPackShape (4 parameterised + 1 TEST_F), AnyUnpack (6),
        AnyTypeOf (2), AnyReject (5 — probe-B error envelope),
        AnyEquality (9 — Any-vs-typed, symmetric, two-field-reads,
        outer-outer, unset, direct-literal-peel, two-Any-literals,
        different-type_url-unequal), AnyNullClear (3),
        AnyLiteralRoundTrip (2).
  - [x] **Conformance unlock** — 1058 → 1065 (+7 PASS).
        `wrappers.textproto :: */to_any` rows remain FAIL — they
        require M8's wrapper auto-unwrap (`Int32Value → int`)
        which is the inner step after M7-A.B unwraps Any to
        `Int32Value`.

### Rewrite M7B — Timestamp / Duration (slices A–F shipped 2026-05-16)

  - [x] **CelKind × pipeline stage** — `kDuration` × {ResolvePass,
        LayoutPass, codegen, runtime helper, activation marshal,
        decoder} all lit; same for `kTimestamp`.
  - [x] **Activation marshalling** — `EncodeBoundValue` /
        `DecodeCelValueAt` arms for `Repr::kDuration` /
        `kTimestamp` (instance.cc).  Equivalent arms in
        `EncodeValue` (cel_host.cc).  Sign-correlated `(seconds,
        nanos)` decomposition shared via `DecomposeAbslDuration`.
  - [x] **Field-read normaliser** —
        `UnpackWellKnownTimeMessage` peels singular
        `google.protobuf.Timestamp` / `Duration`-typed fields into
        `Value::Timestamp` / `Value::Duration` (cel_host.cc).
        Bridges cross-form read with `Timestamp{...}` proto-
        literal construction.
  - [x] **Arithmetic helpers (6)** — `cel_dur_add_at_vv`,
        `cel_dur_sub_at_vv`, `cel_ts_dur_add_at_vv`,
        `cel_dur_ts_add_at_vv`, `cel_ts_dur_sub_at_vv`,
        `cel_ts_ts_sub_at_vv`.  Shared `dur_combine` body with
        `__builtin_{add,sub}_overflow` + sign-correlated
        normalisation + langdef-range check on timestamp
        results + proto-Duration range check on duration
        results.
  - [x] **Ordering helpers (8)** — `cel_{dur,ts}_{lt,le,gt,ge}_at_vv`
        via shared lexicographic compare on the
        `CelDurTs` payload.  Equality / inequality routed
        through `equality_kernel`'s new
        `case CEL_DURATION:` / `CEL_TIMESTAMP:` arms.
  - [x] **Civil calendar + UTC accessors (10 ts + 4 dur)** —
        `cel_civil_from_seconds` via documented Hinnant
        `civil_from_days`.  Validated against
        `absl::ToCivilSecond(UTCTimeZone())` for the §6.4
        quirk grid (Probe A).  Accessors:
        `cel_ts_{year,month,day_of_month_1,day_of_month,
        day_of_year,day_of_week,hours,minutes,seconds,
        milliseconds}_utc_at_v` + `cel_dur_{hours,minutes,
        seconds,milliseconds}_at_v`.
  - [x] **Parse / format trampolines (4)** —
        `CelTimestampParseImpl` / `CelDurationParseImpl` (Probe B
        + C post-validation against absl/CEL admit-set drift) +
        `CelTimestampFormatImpl` / `CelDurationFormatImpl`
        (proto-Duration text format with multiple-of-3 trailing-
        zero trim).
  - [x] **Int conversions (4) + identities (2)** —
        `cel_{ts,dur}_to_int_at_v` (extract seconds field),
        `cel_int_to_{ts,dur}_at_v` (range-check on both); identity
        ids route through `cel_copy_slot`.
  - [x] **With-TZ accessor dispatch (M7B.E)** — single 4-arg
        `cel_host.cel_timestamp_tz_accessor(out, ts, tz, kind)`
        trampoline + 10 pure-wasm shim helpers.  IANA names via
        `absl::LoadTimeZone`; fixed offsets via inline parse
        (`ResolveTimeZone` helper); weekday convention reordered
        from absl's monday=0 to cel-cpp's sunday=0.
  - [x] **Wire ABI** — `CEL_ERR_INVALID_ARGUMENT = 18` added
        (parse failures); `CelDurTs` payload arm reused for both
        kinds; 28 OverloadTable seeds added (kBuiltinSeeds:
        108 → 156).  `InstallOverloadImports` learned `kCelHost`.
  - [x] **E2E coverage** — `e2e/m7b_test.cc`:
        176 / 180 rows passing.  Round-trip × §6.1 boundary grid,
        arithmetic × §6.3 grid, ordering × LexCompareGrid,
        accessors × §6.4 quirk grid (41 rows), parse admit/reject
        (28 rows), format/convert (10), with-TZ (10), reject
        matrix (8).  4 SKIPs surfaced future work: 3 cross-form
        equivalence rows + 1 descriptor-mismatch hard-to-exercise
        defence-in-depth row.
  - [x] **Conformance unlock** — 1058 → 1137 (**+79 PASS**;
        `timestamps.textproto` 0/76 → 69/76).  7 remaining FAILs
        are proto-Duration boundary corners cel-cpp considers
        overflow at a slightly tighter bound than our int64
        check; defer to a polish pass.

## Rewrite M8 — wrapper types (shipped 2026-05-17)

  - [x] **Per CEL type — wrapper row.**  Flipped from `[ ] [ ] [x]
        [x] [ ] [ ]` to all `[x]`.  `google.protobuf.{Bool,Int32,
        Int64,UInt32,UInt64,Float,Double,String,Bytes}Value` flow
        end-to-end across the three boundaries (construction-side
        auto-wrap, read-side auto-peel + Any-chain, kStructExpr
        tail-unwrap).  All 9 wrapper kinds covered.
  - [x] **e2e — 86-test matrix in `e2e/m8_test.cc`.**
        80 PASS / 6 SKIP (skipped rows are reject-matrix cases that
        sit outside M8's scope per §6.3 of the plan).  Sections:
        `WrapperLiteralUnwrapE2ETest` (M8.C — 29 tests covering
        per-kind set / set-to-zero / empty-construct / cross-form),
        `WrapperFieldReadE2ETest` (M8.B — unset-reads-null × proto2
        + proto3, `has()` rows, set-to-default-still-scalar),
        `WrapperAnyChainE2ETest` (M8.B — 6 wrapper-of-Any rows +
        non-wrapper-stays-message regression),
        `WrapperConstructionE2ETest` (M8.A — auto-wrap vs explicit-
        wrapper per kind, null-into-wrapper-clears, boundary values),
        `WrapperActivationBindE2ETest` (M8.A — scalar-bind × 9 kinds,
        null-bind, wrong-kind reject),
        `WrapperRoundTripE2ETest` (A+B+C end-to-end including
        explicit-wrapper round-trip).
  - [x] **Layer-2 unit tests at `cel_host_test.cc`.**  Existing
        `CelSetFieldAnyPackTest::DescriptorMismatchOnSingularMessageInvalidArg`
        updated to expect `InvalidArgument` (post-M8.A status; was
        `Unimplemented` pre-M8).  Wrapper-peel via the read-side
        path is covered by the e2e suite.
  - [x] **WAT trace 56 (`56_wrapper_kstruct_unwrap.wat`)**
        documents the kStructExpr tail-unwrap codegen shape;
        `wat_runner` stub + test (`WrapperKStructTailUnwrapProducesCelInt`)
        in `tools/wat_runner/wat_runner_test.cc` pin
        the trampoline ABI (3-arg `(out_slot, msg_slot,
        wrapper_kind)`).
  - [x] **Conformance: 1144 → 1287 (+143 PASS, ~95% of the +151
        target).**  `wrappers.textproto` 9/36 → 18/36 (`to_any`
        rows × 9 unlocked); `comparisons.eq_wrapper/*` 18/45 →
        45/45 (`eq_X / eq_X_empty / eq_X_proto2_null × 9` unlocked);
        `dynamic.textproto` wrapper rows 0/95 → 95/95;
        `proto2.textproto :: literal_wellknown × 9` + `empty_field/
        wkt` × 1 = 10/10 unlocked; `proto3.textproto :: literal_
        wellknown × 9` unlocked.  The 8-row shortfall vs +151 is
        likely classification noise from rows credited to one arm
        but actually unlocked by another; tracked in m8 plan §9
        "Future work".
  - [x] **Throwaway empirical probe** at
        `former compiler_v2/throwaway/m8_wrapper_probe.cc` (branch
        `throwaway/m8-wrapper-probe`, PR #4).  Pinned cel-cpp's
        `RuntimeOptions::enable_empty_wrapper_null_unboxing`
        toggle — default `false` peels unset wrapper fields to
        scalar zero; conformance corpus is generated with `true`
        (null), which our implementation matches.

## Phase C — runtime self-hosting (in flight)

### C3 — regex `matches` kernel (shipped 2026-05-19)

  - [x] **`cel_matches_at_vv` kernel** — RE2-backed PartialMatch,
        self-hosted in `runtime/cel_matches.cc`.
        Per-Instance single-slot most-recent-pattern cache
        (the common `list.exists(x, x.matches(pat))` shape hits
        the cache every iteration past the first; multi-pattern
        sites recompile each switch at RE2-compile cost ~µs).
  - [x] **Unit tests** at `runtime/cel_matches_test.cc` —
        focused TEST_F for 3VL absorb (error/unknown × text/pat),
        kind-mismatch (non-string × text/pat), pattern-compile
        failure + sticky-error cache; parameterised TEST_P over
        the 9 `string.textproto::matches/*` rows verbatim;
        cache-behaviour (warm-path 1000× + alternating-thrash 50×);
        boundary (embedded NUL, anchors, empty-empty,
        invalid-UTF-8 no-crash, 4 KiB text).
  - [x] **Overload-table seeds** — `matches` + `matches_string`
        seeds added in `compiler/codegen/overload_table.cc`
        pointing at `cel_matches_at_vv`; both removed from
        `kExplicitlyUnimplementedIds`.  Seed count 156 → 158.
  - [x] **Runtime BUILD wiring** — `cel_runtime` cc_binary
        exports `cel_matches_at_vv`; `cel_matches` cc_library
        depends on `@re2` + `absl::strings`.
  - [x] **Host export binding** — `engine.cc::kRuntimeExports`
        adds `cel_matches_at_vv`; runtime-test instantiation
        verified.
  - [x] **Conformance: `string.textproto::matches/*` 0/9 → 9/9
        PASS** (overall 1373 → 1382, +9).  Caught a real bug
        on first run: a first-call-with-empty-pattern would
        spuriously poison because the cache's
        `CachedPattern() == ""` matched the default-constructed
        empty pattern and skipped the compile path, leaving
        `CachedRe()` null.  Added a `CachedInitialized` flag
        + a kernel-layer regression test
        (`EmptyPatternAfterNonEmptyPattern`) so the cold-cache
        path is locked outside the lexicographic-first
        parameterized row.

## Rewrite M11 — `cel_host` refactor (in flight)

### M11 Slice A + E — Any-of-Any P0 fix + `cel_host_error` extraction (shipped 2026-05-19)

  - [x] **Any-of-Any iterative unwrap (Slice A — P0).**
        `UnpackAnyToValue` now loops over nested Any layers
        instead of peeling exactly one.  Mirror of cel-cpp's
        `AdaptAny` (`internal/well_known_types.cc:1943-2007`).
        Depth-bounded by `ABSL_CHECK(depth < 1024)` per the
        m7a-any.md §R3 design (belt-and-suspenders against
        malformed Any chains that wire-size can't itself bound).
        Tests at `cel_host_test.cc::AnyOfAnyTest` — depth 1/2/3/4
        round-trip, depth-2 P0 regression, non-WKT inner →
        kMessage, malformed payload → kTypeMismatch, unknown FQN
        → kFieldNotFound, empty type_url → null.
  - [x] **Strict Any URL prefix (Slice A).**
        `ExtractAnyFqn` accepts only `type.googleapis.com/` and
        `type.googleprod.com/` per cel-cpp; anything else surfaces
        as a clean `kFieldNotFound` error.  Pre-M11 accepted any
        string-with-slash, a quiet divergence from cel-cpp.
        Tests at `AnyOfAnyTest::StrictUrlPrefixRejectsNonStandardPrefix`
        + `UrlWithoutSlashRejected` + `GprodPrefixAccepted`.
  - [x] **Any-of-9-wrappers round-trip (Slice A).**
        Each of the 9 wrapper kinds (Bool/Int32/Int64/UInt32/
        UInt64/Float/Double/String/Bytes Value) unwraps via
        `Any<Wrapper>` to the corresponding CEL scalar.  Locked
        in `AnyOfWrapperKindsTest::*UnwrapsTo*` (9 tests).
  - [x] **Any-of-WKT-time (Slice A).**  `Any<Timestamp>` →
        kTimestamp, `Any<Duration>` → kDuration.  Locked in
        `AnyOfWktTimeTest::*UnwrapsTo*`.
  - [x] **`cel_host_error` TU (Slice E).**  Wire-error helpers
        + 3VL absorbers extracted out of `cel_host.cc` into a
        new leaf-level TU at `eval/internal/cel_host_error.{cc,h}`.
        New `cc_library` + `cc_test` targets in `eval/BUILD.bazel`.
        Slice-E delta vs the plan: also introduced
        `:cel_host_hdrs` headers-only target to break the dep
        cycle that would otherwise arise as helpers extract out;
        every future cel_host_* TU will depend on it.
  - [x] **`cel_host_error` direct unit tests** at
        `eval/internal/cel_host_error_test.cc` —
        positive + negative + boundary coverage per CLAUDE.md
        testing principles.  ~17 tests covering: cel::Value
        error factories per pinned code, `WireErrorCode`
        exhaustive per `cel::ErrorCode` enumerator, slot writers
        with INT64_MIN/MAX boundary, 3VL absorber propagation
        rules (first-non-normal-wins).  Before Slice E these
        helpers were only exercised transitively via the
        Compile → Plan → Eval e2e suite.

### Rewrite M12 — string_ext extension (slices A–F shipped 2026-05-20)

`m12-string-ext.md` slices A-F land the cel-cpp `strings`
extension library: 13 functions × 19 overloads + the
printf-style `format` directive, all self-hosted inside
`cel_runtime.wasm`.  Conformance: `string_ext.textproto` 0/216
→ 94/216, total 1382 → 1476 PASS (+94, hit §5.2 target
exactly).  Self-hosted runtime kernels in
`runtime/cel_string_ext_*.cc` (Slices A-D) +
`cel_string_format*.cc` (Slices D-E) + checker library
registration + 19 overload-table seeds + wasm exports + linker
bindings (Slice F).

  - [x] **Slice A — code-point iterator + simple kernels**
        (charAt, lowerAscii, upperAscii, trim, reverse).
        `cel_string_ext_codepoint.cc` + `cel_string_ext_internal.h`
        shared UTF-8 helpers + 38 unit tests covering boundary
        + multi-byte UTF-8 + Unicode-whitespace matrix.
  - [x] **Slice B — search/extract family** (indexOf ×2 +
        lastIndexOf ×2 + substring ×2 + replace ×2).  Boundary
        matrix in `cel_string_ext_search_test.cc`: negative pos,
        end<start, empty needle, n=0/<0 semantics.  cel-cpp
        parity verified row-by-row against
        `common/values/string_value.cc`.
  - [x] **Slice C — list-bridging family** (split ×2 + join ×2).
        `cel_string_ext_list.cc` with direct CelValue stamping
        into arena list element array.  Final-piece rule for
        split locked verbatim.  CEL_LIST_HOST deferred (per §10).
  - [x] **Slice D — quote + format directive parser**
        (`strings.quote` + `cel_string_format_at_vv` parser).
        Diagnostic strings byte-identical to cel-cpp's
        `extensions/formatting.cc`.  27 parser tests across
        happy-path matrix + every malformed-input reject path.
  - [x] **Slice E — format renderer + per-CelKind dispatch.**
        7 Render* arms in `cel_string_format_render.cc` across
        the full directive × CelKind matrix; per-Instance
        single-slot format cache mirroring `cel_matches.cc`'s
        shape (CachedInitialized flag locks the empty-pattern
        bug closed at write time).  49 dispatcher tests including
        cache regression coverage.
  - [x] **Slice F — overload-table wiring + conformance lock.**
        StringsCheckerLibrary registration; 19 overload IDs
        seeded (158 → 177); `_at_vvv` (arity 4) + `_at_vvvv`
        (arity 5) recognised by `compile.cc::OverloadHelperArity`;
        wasm exports + `engine.cc::kRuntimeExports` table
        extensions; literal-list `format` arg static-subset
        admission.  34 e2e tests in
        `e2e/m12_test.cc`.  Conformance baseline
        bumped 1382 → 1476.

## Rewrite M16 — math_ext extension (shipped 2026-05-24)

cel-cpp `math` extension: 17 functions.  `greatest`/`least` expand
(parser macros) to `math.@min`/`@max`; the rest are plain global
calls.  20 self-hosted kernels in a single `runtime/
cel_math_ext.c`; no new codegen (generic kCall).  Conformance:
`math_ext.textproto` 0 → 194/199 PASS (5 SKIP dyn-error rows, 0
FAIL); corpus-wide +194 (1576 → 1770 after merging M14).

  - [x] **Slice 0 — WAT-first.** AST-shape probe
        (`compiler/probes/math/ast_shape_probe_test.cc`) +
        two traces (`wat/m16_math_min_list.wat`,
        `wat/m16_math_bit_shift.wat`) assembled via `wasm-as`.
  - [x] **Slice A — scalar.** ceil/floor/round/trunc, isInf/isNaN/
        isFinite, abs/sign/sqrt (kind-dispatch); positive +
        negative + boundary matrix in `cel_math_ext_test.cc`
        (abs(INT64_MIN)→overflow, sign(±0/NaN), round half-away,
        sqrt(neg)→NaN).
  - [x] **Slice B — bitwise.** bitAnd/Or/Xor/Not/ShiftLeft/
        ShiftRight; negative-offset→INVALID_ARGUMENT, count>63→0,
        logical right-shift on int (no sign extension).
  - [x] **Slice C — min/max.** binary + list folds via the
        cross-type numeric ladder; NaN keeps the first operand;
        mixed-list returns the winner's runtime kind.
  - [x] **Slice D — wiring + conformance.** 58 overload seeds
        (177 → 235); `MathCheckerLibrary()` + math macros in
        `parse_and_check.cc`; 20 wasm exports + catalogue entries;
        targeted static-subset admission for `dyn`-typed cross-type
        / mixed-list `math.@min`/`@max`.  67-case e2e in
        `e2e/m16_test.cc`.  Baseline → 1770 (post-M14 merge).
### Rewrite M14 — CEL `optional<T>` (shipped 2026-05-22)

`m14-optionals.md` shipped `optional<T>` end-to-end:
runtime kernels + IR `Repr::kOptional` + codegen Select/Index/
Struct branches + frontend `OptionalCheckerLibrary` wiring +
`{?key: opt_v}` / `[?elem]` / `{?field: opt_v}` literal entries
+ `optMap`/`optFlatMap` macros riding Shape-C with zero new
comprehension codegen.  Conformance: `optionals.textproto`
0/70 → 22/70 PASS (4 FAIL, 44 SKIP); corpus-wide 1476 → 1576
PASS (+100).  Slice C unlock was 0 (every literal-entry /
`optMap` row in the corpus has a `dyn`-typed sub-expression
that `RejectDyn` filters upstream — see m14-optionals.md §4
Slice C delta 1).  Slice E unlocked +4 by lifting the
proto-`?field:` gate; one previously-SKIP'd row newly FAILs
on the CEL_MESSAGE zero-predicate trap (filed for follow-up).

  - [x] **Slice 0 — WAT-first ABI lock.**  Six WAT files under
        `doc/.../wat/m14_optional_*.wat` lock the OptionalCell
        layout (32-byte arena-allocated `{present, _pad, inner}`),
        the kernel ABIs (`cel_optional_*_at_*`,
        `cel_select_optional_field_at_vv`), and the
        immutability + absent-key contracts.  All six
        assemble and run end-to-end through `wat_runner` with
        byte-exact memory assertions.
  - [x] **Slice A — runtime kernels + parser flip.**  8 new
        kernels in `cel_optional.{h,c}` (`none_at`, `of_at_v`,
        `of_non_zero_at_v`, `has_value_at_v`, `value_at_v`,
        `or_at_vv`, `or_value_at_vv`,
        `select_optional_field_at_vv`) + 32 per-TU unit tests in
        `cel_optional_test.cc` + 4 3VL-absorption tests (B3
        follow-up).  `kPrimitiveTypeName[14]` filled with
        `"optional_type"`.  `EnableOptionalSyntax = true` +
        `AddLibrary(OptionalCheckerLibrary())` in
        `parse_and_check.cc`.  14 OverloadTable seeds (the 7
        value-level overloads + 7 chained-index overloads all
        routed through `cel_select_optional_field_at_vv`).
  - [x] **Slice B — codegen + static-subset gate.**
        `Repr::kOptional` stamped by both `ReprOf` overloads
        (TypeSpec `AbstractType{"optional_type"}` + cel::Type
        `OptionalType`).  `LayoutPass::SelectKeyRodataVisitor`
        lifts kSelect-on-optional field names into rodata.
        `EmitKSelect` / `EmitKIndexCall` optional branches
        route to `cel_select_optional_field_at_vv`.  test_only
        Select on optional chains through
        `cel_optional_has_value_at_v` on the same slot.
        `CheckSubsetStruct` rejects `Foo{?field: ...}` proto-
        literal entries.  Codegen test matrix:
        `optional<map>.field`, `optional<list>[i]`,
        `optional<list>[?i]`, `m[?k]`, `[1,2][?1]`,
        `has(opt.x)`, Select on typed-None.  e2e test matrix:
        the same plus orValue chains and the verbatim
        conformance row sources.
  - [x] **Slice C — `optMap` / `optFlatMap` macros + optional
        entries in literals.**  Two new runtime kernels
        (`cel_map_insert_at_if_present` / `cel_list_append_at_if_present`)
        + 12 per-TU unit tests in `cel_optional_test.cc` covering
        Some/None/error/unknown/wrong-kind + mixed-entry shapes.
        Two new WAT traces (§M14.7 / §M14.8) with byte-exact
        `WatRunner` assertions on the post-eval
        `ArenaListHeader.count` / `ArenaMapHeader.count`.
        `EmitKMapExpr` / `EmitKListExpr` honour
        `MapExprEntry.optional()` / `ListExprElement.optional()`;
        6 new codegen tests in `expr_lower_test.cc` cover
        all-optional / mixed / regression-only-plain patterns.
        12 new e2e tests in `m14_test.cc` cover literal-entry
        materialisation/omission plus `optMap`/`optFlatMap`
        Some/None branches — confirming Shape-C cel.bind
        detector admits the macros with zero new comprehension
        codegen (per probe Q5).
  - [x] **Slice E — proto `?field:` literal entries.**  New
        runtime kernel `cel_set_field_at_if_present` in
        `cel_optional.c` reuses the `absorb_optional_predicate`
        helper from Slice C, then delegates to
        `cel_host.cel_set_field` on Some.  Pure wasm; no new host
        trampoline (the design pull-in identified by the
        pressure-test in `design-pressure-test-prompt.md` worked
        example 1).  7 new per-TU unit tests in
        `cel_optional_test.cc` including a load-bearing
        short-circuit assertion via a strong override of
        `cel_host_cel_set_field` that counts invocations (proves
        None-path doesn't reach the host).  WAT §M14.9 with the
        same short-circuit assertion via a wat_runner stub.
        Frontend gate lifted (`CheckSubsetStruct`), codegen
        branched on `f.optional()` in `EmitKStructExpr`.  3 new
        e2e tests in `m14_test.cc` covering Some-materialises,
        None-leaves-unset, and mixed-entry shapes.
  - [x] **Slice D — closeout.**  `.baseline` bumped 1572 → 1576.
        Status flipped to shipped in `m14-optionals.md` §0.
        `is_zero_value` CEL_MESSAGE trap filed as M14 follow-up
        in `cleanup-backlog.md` (only known remaining gap; one
        previously-SKIP'd corpus row newly FAILs on it).

### Rewrite M17 — `encoders` extension (base64) (shipped 2026-05-24)

`m17-encoders-ext.md` shipped `base64.encode(bytes)->string` +
`base64.decode(string)->bytes` self-hosted in `cel_runtime.wasm`.
Conformance: `encoders_ext.textproto` 0/4 → 4/4; corpus-wide
1770 → 1774 (+4, no fail regression; landed by rebasing onto M16).
Semantics confirmed
against `third_party/cel-cpp/extensions/encoders.cc` (overload
ids, `absl::Base64{Escape,Unescape}`, `"invalid base64 data"`).

  - [x] **Slice 0 — WAT-first.**  `m17_base64_{encode,decode}.wat`
        lock the unary `(out, arg)` slot-out ABI; both run
        end-to-end through `wat_runner` (`WatRunnerEncodersTest`,
        2 cases) asserting `b'hello'`→`"aGVsbG8="` and unpadded
        `'aGVsbG8'`→`b'hello'`.
  - [x] **Slice A — runtime kernels + unit tests.**
        `cel_base64_ext.{h,cc}` (`cel_base64_encode_at_v` /
        `cel_base64_decode_at_v`), absl wrappers bridged via
        `cel_string_ext_internal.h` + a local `WriteBytesFromBytes`.
        `cel_base64_ext_test.cc` — 18 tests (happy paths, padding
        shapes, unpadded decode, invalid input, 256-byte round
        trip, 3VL + kind-mismatch envelope).  `MakeBytes` /
        `BytesAt` / `ExpectBytes` added to
        `string_ext_test_helpers.h`.
  - [x] **Slice B — pipeline wiring + conformance lock.**
        `EncodersCheckerLibrary()` registered in
        `parse_and_check.cc`; 2 overload seeds in
        `overload_table.cc` (249 → 251); 2 `K_AT_V` catalogue
        entries; 2 `wasm_exports.txt` lines; `:cel_base64_ext`
        wired into `cel_runtime_wasm.bin`.  `m17_test.cc` e2e
        (9 tests).  `.baseline` 1770 → 1774.

### Rewrite M20 — enum/scalar field-assignment range errors (shipped 2026-05-25)

`m20-enum-field-range.md` made out-of-range scalar/enum proto field
assignments produce a CEL error VALUE (matching cel-cpp) via a
poison-on-error `cel_set_field` contract — no ABI/codegen change.
Conformance: corpus-wide 1890 → 1898 (+8, no fail regression):
`enums.textproto` legacy_proto{2,3} `assign_standalone_int_too_{big,neg}`
(4) + `dynamic.textproto` int32/uint32 `field_assign_proto{2,3}_range`
(4).  Strong enum types remain descoped (cel-cpp itself decays enums to
int — validated by the now-deleted strong-enum probe).

  - [x] **Slice 0 — WAT-first.**  `m20_set_field_poison.wat` locks the
        poison contract (poison on overflow + no-op on already-poisoned
        slot); runs through `wat_runner`
        (`SetFieldPoisonsOnOutOfRangeAndPropagates`).  `wat-traces.md`
        §M20.1.
  - [x] **Slice A — cel-cpp differential oracle.**
        `testdata/cel_cpp_oracle.{h,cc}` (namespace-isolated, links
        `@cel-cpp//runtime`, emits `cel.expr.Value`); a non-OK
        `Evaluate()` status folds into `is_error` to match cel-cpp's
        conformance harness.
  - [x] **Slice B — poison-on-error `cel_set_field`.**  `CelSetFieldImpl`
        early-outs on a CEL_ERROR `msg_slot`, classifies `OutOfRange`
        field writes → `CEL_ERROR{CEL_ERR_OVERFLOW}` poison in place
        (else trap).  Unit tests: `cel_host_test.cc::CelSetFieldPoisonTest`
        (5 cases — int32 over/underflow, enum overflow, no-op-on-poison,
        in-range control).
  - [x] **Slice C — enum range checks.**  `CheckInt32Range` added to all
        four enum write arms (singular / repeated / host-list /
        map-entry); int32/uint32 arms were already checked.
  - [x] **Slice D — differential suite + un-skips.**
        `testdata/cel_cpp_oracle_test.cc` (20 cases: smoke + the 8 M20
        rows + INT32 boundary matrix, our-pipeline-vs-oracle).  The four
        `wkt_field_set_test.cc` `*_range` cases un-skipped (assert a CEL
        error VALUE).  `.baseline` 1890 → 1898; cleanup-backlog #11
        closed.

### Rewrite M21 — host-call adapter (typed `HostCallContext`) (shipped 2026-05-26)

`m21-host-call-adapter.md` turned the raw 4-arg `@host` callback ABI
into a typed, kind-checked `HostCallContext` (Layer 1) + a
canonical-types-only `BindTypedFunction` / `Engine::AddTypedFunction`
(Layer 2), and widened the host-call trampoline env to share the
per-Instance externref table + arena (Layer 0) — the prerequisite that
makes proto / list / map arguments and newly-allocated string /
aggregate returns work for user host fns.  No wire-format / runtime
`.wasm` / codegen change.

  - [x] **Layer 1 — `HostCallContext` unit matrix** at
        `eval/host_call_context_test.cc`.  Every `ArgXxx` /
        `ReturnXxx` over fake `MemoryView` / `ExternrefTable` /
        `ArenaAllocator`: positive decode/encode, negative wrong-kind →
        `InvalidArgument` (the non-bypassable kind-tag check), OOB arg
        index → `OutOfRange`, dangling externref slot →
        `FailedPrecondition`.  Boundary ints/uints, ±inf/nan/-0.0,
        empty / embedded-NUL / multi-byte strings & bytes.  Complex:
        `list<customer>` (host + arena-of-messages),
        `map<string,list<customer>>`, int-keyed maps, nested arena
        decode.  Aggregate returns intern host backings; `ReturnUnknown`
        stamps `kFunctionUnknownSentinel`; `ReturnValue(Unknown)`
        preserves a propagated input attribute id.
  - [x] **Layer 2 — `BindTypedFunction`** at
        `eval/typed_function_test.cc`.  Canonical param/return types
        round-trip through typed lambdas over the fake context; arity =
        params + 1; concrete `const M&` (dynamic_cast, wrong-type →
        error) and polymorphic `const Message*`; function-pointer bind;
        wrong-kind arg / non-OK lambda surface as error status.
        Must-not-compile set covered by `static_assert` on the exposed
        `kIsCanonicalHostArg` / `kIsCanonicalHostReturn` predicate
        (`int`, `unsigned`, `float`, `char*`, `std::string` arg,
        by-value proto rejected; `int`/`float`/`string_view` return
        rejected).  **Also exercised end-to-end through real wasmtime**
        (`host_fn_test.cc` `Typed*` — the full kind matrix incl. arena
        `string` return, concrete + polymorphic proto arg, owning proto
        return, `HostListView`/`HostMapView` args, and `bytes` / unknown
        / error returns via the `Value` escape hatch — all via
        `Engine::AddTypedFunction`), so the typed sugar is proven over
        the full pipeline, not just the fake context.
  - [x] **Layer 0 + integration — e2e through real wasmtime** at
        `e2e/host_fn_test.cc` (67 cases).  Every test inlines the real
        customer flow (no test-only wrappers); the full kind matrix runs
        on **both** the typed (`AddTypedFunction`) and context
        (`AddFunction`) registration paths — incl. Duration / Timestamp,
        concrete + polymorphic proto arg, owning proto return,
        `list`/`map` (bound + literal) and the nested
        `map<string,list<proto>>`, arena `ReturnString`, and `bytes` /
        unknown / error returns.  `instance_test.cc` + `engine_test.cc`
        callbacks use `HostCallContext`.
  - [x] **3VL — PartialEval dimension** (e2e): host fns dispatch
        normally on the PartialEval path with known args
        (`PartialEvalKnownArgInvokesHostFn`); an arg marked unknown by an
        `AttributePattern` is absorbed by the trampoline before the
        callback runs (callback NOT invoked, asserted via a flag) and
        propagates with its real attribute id (≠ sentinel) — verified
        across int / string / proto / list / map
        (`UnknownArgAutoPropagatesWithoutInvokingCallback` +
        `PartialEvalUnknownArgTest`), the five wire-representation
        classes (the absorption check is kind-independent in the
        trampoline).  A function-origin `ReturnUnknown()` stays
        distinguishable and survives an operator merge
        (`FunctionOriginUnknownSurvivesOperatorMerge`).  Error args
        propagate (error precedence over unknown).

### Rewrite M24 — foreign custom functions via Component Model (shipped 2026-06-04)

`m24-foreign-fn-component-backend.md` adds **Regime B** — a foreign
custom-fn backend that dispatches a normal isolated WebAssembly
*component* (any language toolchain, own memory, protos cross as
bytes) over the shipped `kCelFn` host-callback path.  The compiler,
checker, overload table, codegen, and 3VL absorption are
**unchanged**; the new surface is one Eval-side entry-point
(`Engine::AddComponent`) and one marshaling bridge
(`eval/internal/cel_component.{h,cc}`) that lifts/lowers
`CelfnType` × `Value` ↔ `wasmtime_component_val_t`.

  - [x] **A.1–A.5 — IDL + Builder gates** at
        `compiler/celfn/function_library.cc`,
        `compiler/celfn/function_library_test.cc` (+12 cases) and
        `compiler/compiler_test.cc` (+4 cases).
        `Builder::AddForeignComponent(fn_name, return_type, params)`
        registers as `kCelFn` (so codegen routing is invisible to the
        compiler); `Builder::Build()` rejects (a) `kForeignComponent`
        decls whose return or any param mentions `optional<T>` (v1
        decision: optional dropped, named in the error) and (b) **any**
        backend whose return contains `map<K, V>` for a K outside
        `{bool, int, uint, string}` (langdef rule).
  - [x] **B.1–B.7 + B.9 — typed marshaling at the canonical-ABI
        boundary** at `eval/internal/cel_component_test.cc`.  The full
        type matrix from m24 §6 (minus the dropped `optional<T>` row)
        round-trips through `LiftCelToComponent` / `LowerComponentToCel`:
        `bool` / `int` / `uint` / `double` / `null` / `string` / `bytes`
        / `duration` / `timestamp` / `type` / `list<T>` / `map<K,V>` /
        `proto(fqn)`.  Boundary discipline covers every scalar
        extremum (`INT64_MIN/MAX`, `UINT64_MAX`, `±0.0`, `NaN`, `±Inf`,
        nanos 0 / 999_999_999 / out-of-range), every string/bytes
        edge (empty, embedded NUL, multi-byte UTF-8, KB-scale), every
        aggregate shape (empty / single / ragged-nested), every legal
        map-key kind and the `double`-key rejection, the missing-pool
        / unknown-fqn / bad-bytes proto-decode failures, and a
        cross-kind defence-in-depth row per scalar.  **Plus the
        large-payload boundary block** (F.1) at the bottom of the
        file: `LargeStringRoundTripsAtMiBScale` (256 KiB), `…Bytes…`
        (256 KiB), `LargeListIntRoundTripsAt100kElements` (10⁵
        scalars), `LargeListOfLargeStringsLiftsAtMiBScale` (1 K × 1 KiB),
        `LargeNestedListLiftsAt10kLeafCells` (100 × 100),
        `LargeMapStringIntRoundTripsAt10kEntries` (10⁴ entries),
        `LargeMapStringListIntLiftsAt10kLeafCells` (10⁴ leaves),
        `LargeProtoLargeStringFieldLiftsAtMiBScale` (256 KiB UTF-8 in
        `Customer.name`), `LargeProtoLargeBytesFieldLiftsAtMiBScale`
        (256 KiB binary in `Customer.session_token`), and
        `LargeProtoRepeatedStringLiftsAt10kEntries` (10⁴
        `Customer.tags`).
  - [x] **C.1–C.4 — `Engine::AddComponent(component_bytes, lib)`** at
        `eval/engine.{h,cc}` (+`_test.cc`).  Per-Plan: parses the
        component via `wasmtime_component_new`, instantiates it,
        translates each declared overload-id from snake to **kebab**
        for the component-export lookup (Component-Model spec rejects
        `_`), validates the export shape (positive — used by every
        e2e case below; negative — overload-id conflict + unknown
        export, two passing tests).  Each export is bound as a
        `HostCallback` whose body marshals args via the cel_component
        bridge, calls `wasmtime_component_func_call`, and lowers the
        result.  3VL absorption fires upstream of the callback
        (covered by m21's `UnknownArgAutoPropagatesWithoutInvokingCallback`).
  - [x] **D.1 / F.2 — e2e dispatch path proof** at
        `e2e/foreign_component_dispatch_test.cc` (4 cases).  Each
        test goes through the full pipeline: `AddForeignComponent` →
        `Compile` → `AddComponent` (parses inline component-model WAT
        via `wasmtime_wat2wasm`) → `Plan` → `Eval` against an
        Activation.  Cases: `IntAddRoundTripsBoundaryValues` (scalar
        path + `INT64_MIN`); `BoolPassthroughRoundTrips` (scalar bool);
        `LargeStringTransportsAtMiBScale` (256 KiB string into a
        component with `(canon lift … (realloc …))`); and
        `LargeListIntTransportsAt100kElements` (10⁵ list<int> summed
        inside the component).  Pins the *transport* size, not just
        the value range — the place a bug in canonical-ABI handoff or
        the eval-side memcpy / HostCallContext::ArgString path would
        surface.

**As-of-v1 deferrals (tracked in m24 §14 Future Work).**  D.2 TinyGo
forcing-fn fixture, D.3 production-config `bench/foreign_component`
(`AddComponent` invoke cost vs `kUserModule` slot-out vs native
`AddFunction`), E.1 `celfnc` codec generator + IDL `type` keyword,
and `optional<T>` re-introduction at the typed path.  None gate v1
because the contract is pinned today by the wasmtime component
runtime (language-agnostic by construction) and the dispatch path
is proven byte-exact against `INT64_MIN` and MiB-scale payloads.

## Rewrite M28 — Configurable linking (prototype shipped 2026-06-08)

The prototype lands `LinkMode::kStatic` end-to-end through Compile →
Plan → Eval with 51/51 e2e green in both modes.  Full design in
`doc/implementation-plan/rewrite/m28-configurable-linking.md`; review
in `doc/implementation-plan/rewrite/reviews/2026-06-08-m28-prototype.md`.

**Shipped:**

  - [x] `CompilerOptions::link_mode = kStatic` compiles for scalar /
        arith / concat / ternary expression cells
        (`compiler/compiler_test.cc::CompilerLinkModeTest` +
        `compiler/internal/compile_test.cc`).
  - [x] `kStatic` Program plans + evaluates through `Engine::Plan` —
        unified Plan body with two `if (is_static)` branches, no
        separate `PlanStatic` (`eval/engine.cc:751-779`).
  - [x] Parity (kDynamic vs kStatic) verified on the
        `e2e/m28_static_link_test.cc` matrix (const int / string /
        bool, 1+2 int, "a"+"b", ternary).
  - [x] `InstanceImpl::runtime_instance` → `helpers_instance` rename
        complete across `eval/engine.cc` + `eval/internal/instance_impl.h`
        (6 reader sites; doc references in
        `doc/implementation-plan/rewrite/{design,abi-refactor,wasi/DESIGN,wasi/README,wasi/milestones/M6-M8}.md`
        reconciled 2026-06-08).
  - [x] Strip tool + `cel_runtime_stripped_wasm_bytes` build target
        with three byte-shape invariants pinned
        (`runtime/cel_runtime_stripped_wasm_bytes_test.cc`).
  - [x] Default `CompilerOptions::link_mode` is `kStatic` — flipped
        in the 2026-06-08 simplification pass
        (`compiler/compiler.h:144`, `compiler/internal/compile.h:102`);
        51/51 e2e green under either default.
  - [x] `Engine::Plan` routes via wasmtime's own import-introspection
        API — `ModuleImportsCelNamespace(wasmtime_module_t*)` in
        `eval/engine.cc:378`, ~15 lines using `wasmtime_module_imports`.
        Replaces the original ~85-line hand-rolled ULEB128 + section
        walker `HasCelImports(bytes)`.
  - [x] `InstallStructImports` folded into `InstallListImports` —
        the helper's single remaining import
        (`cel.cel_set_field_at_if_present`) now ships alongside the
        sibling `*_at_if_present` predicates
        (`compiler/internal/compile.cc:159`).
  - [x] Dual-mode e2e infrastructure landed (2026-06-09) —
        `e2e/link_mode_e2e_helpers.h` selects `kE2ELinkMode` from a
        compile-time macro; `e2e/link_mode_e2e_test.bzl` defines
        `link_mode_e2e_cc_test` which emits `<name>_dynamic` +
        `<name>_static` cc_test pairs per source.  Replaces 14
        per-file `CompilePlan` duplicates.  See
        `doc/implementation-plan/rewrite/m28-configurable-linking.md` §5.5.
  - [x] 22 e2e source files run in both kDynamic and kStatic
        (2026-06-09) — `mvp_concat_test`, `known_bugs_test`,
        `host_fn_test`, `optimize_test`, `program_roundtrip_test`,
        `wkt_field_set_test`, `m2_test`, `m2_partial_eval_test`,
        `m4_test`, `m5_test`, `m5b_test`, `m7_test`, `m7a_test`,
        `m7b_test`, `m8_test`, `m9_test`, `m10_test`, `m12_test`,
        `m14_test`, `m16_test`, `m17_test`, `m18_test`.  **45/45
        targets pass** with bit-identical results between modes.
        Covers variable-bearing cells, comprehensions, `has(msg.field)`,
        list / map literals, host-fn calls, and program-roundtrip.
        The bespoke `m28_static_link_test` remains explicitly
        mode-scoped.
  - [x] cctz + absl format-spec paths verified identical in both
        modes (2026-06-09) — `e2e/cctz_doubles_test.cc`, 14 cells
        covering `timestamp(RFC3339)` parse, timezone-aware
        accessor (`cel_host.cel_timestamp_tz_accessor`),
        `duration(string)` parse, `timestamp + duration` and
        `timestamp - timestamp` arithmetic, `string(<double>)`
        (full IEEE 754 precision via absl `str_format`),
        `double(<string>)` parse, and cross-type double
        arithmetic.  Settles the previously-feared silent-corruption
        gap empirically — see m28 §10.1 invariant 9 for the
        framing correction this enabled.
  - [x] Dual-mode bench harness backported (2026-06-09) —
        `benchmark/eval/celwasm_bench` takes `--link_mode=dynamic|static`
        (default dynamic, consumed before Google Benchmark arg
        parsing); `run.sh` runs the celwasm side once per mode and
        emits two `report.sh` tables against the single `celcpp_bench`
        run.  The prototype's hardcoded merged-wasm cells (which
        loaded artifacts from the deleted
        `wasm_compilation_experiments/` dir) were removed — the same
        measurement is now `--link_mode=static` over the regular
        corpus.  Loader coverage: `benchmark/eval/corpus_loader_test.cc`.
  - [x] `Engine::Builder::EnableJitPerfMap` (2026-06-09) — opt-in
        wasmtime perfmap profiling strategy for JIT symbolication
        under `samply` / `perf`; positive (enabled engine builds,
        plans, evals `40 + 2` → 42, chained-rvalue builder form) +
        explicit-false cases in
        `eval/engine_test.cc::EngineBuilderJitPerfMapTest`, both
        link modes.

**Post-merge closeout additions (2026-06-09, late pass):**

  - [x] Component-Model custom fns × link mode —
        `e2e/foreign_component_dispatch_test` converted to
        `link_mode_e2e_cc_test` + 3 new cases (string arg+return via
        the canonical-ABI return-pointer arm, missing-export →
        FailedPrecondition at Plan naming the kebab-case export,
        trapping component fn → clean Eval error); 7/7 in BOTH modes.
        `demo_component_e2e_test` likewise dual-mode.
  - [x] Hollow dual-mode targets fixed — `host_fn_test` plus 13 more
        e2e sources emitted `_dynamic`/`_static` pairs whose sources
        never selected the mode (both emissions silently ran the
        kStatic default).  All bare `Compile` sites now route through
        `e2e::DefaultOpts()` / `kE2ELinkMode`;
        `host_fn_type_matrix_test` converted to the dual macro.
        Post-fix fleet: 47/47 e2e targets green, genuinely dual.
  - [x] Plan link-mode label tripwire —
        `eval/engine_test.cc::EnginePlanLinkModeTripwireTest`:
        correctly-labeled both modes, mislabeled-static (byte-patched
        section) and mislabeled-dynamic (hand-framed section on
        synthetic WAT) → FailedPrecondition naming both signals,
        unknown future enum value → no validation, abi-less module →
        no validation.  Plan flow rework verified by 29/29 dual-mode
        eval+e2e targets; static mode no longer installs dead cel.*
        linker bindings.

**P1 follow-ups — ALL CLOSED 2026-06-09 (evening pass):**

  - [x] Strip-tool no-merge invariant pinned (P1-2; invariant 1) —
        `runtime/cel_runtime_stripped_wasm_bytes_test.cc::
        CatalogueExportsTargetDistinctFunctions`: catalogue membership
        + pairwise-distinct internal targets.
  - [x] Rodata budget enforced (P1-3; invariant 6) —
        `InstallExprRodataSegment` returns ResourceExhausted past
        `CELWASM_RESERVED_LOW_MEMORY_BYTES`; red-first negative test
        (9000-char literal previously compiled silently), 4 KiB
        boundary positive, dynamic-mode control
        (`compiler/internal/compile_test.cc`).
  - [x] Conformance parameterized over `LinkMode`; both modes green
        (P1-4) — full corpus 1899/463/92 each, byte-identical down to
        FAIL detail text; gate script runs both modes with per-mode
        baselines (`conformance/.baseline_static` = 1899).
  - [x] Matrix coverage in static mode — variable-bearing cells,
        comprehension, `has(msg.field)`, list / map literals
        (P1-5, originally tracked against `m28_static_link_test.cc`).
        Satisfied 2026-06-09 via the dual-mode e2e sweep over 22
        source files; bit-identical results between modes.
        *(Remaining: a 1000-term arith chain cell — defer with the
        bench gate.)*
  - [x] `ModuleImportsCelNamespace` unit test (P1-6; invariant 8) —
        helper extracted to `eval/internal/module_imports.{h,cc}`
        with `module_imports_test.cc`: synthetic-WAT matrix (no
        imports / wasi-only / cel_host-only / cel.* / mixed /
        multiple) plus the module-name boundary cases ("celx", "ce")
        pinning the size==3 + memcmp comparison.  2026-06-09.
  - [x] `WasmModule::Adopt` + `AddActiveDataSegment` unit tests in
        `compiler/codegen/module_test.cc` (P1-7) — adopt round-trip,
        feature-set UNION (MVP-featured module gains DefaultFeatures;
        invariant 5), ownership/destruction, segments on `"memory"`
        and `"0"`-named memories, empty-span boundary.  2026-06-09.
  - [x] Codegen-side guard against future `BinaryenLoad(..., "memory")`
        (P1-8; invariant 2) — structural: `CodegenLoad`/`CodegenStore`
        wrappers with no memory-name parameter replaced all 11 direct
        sites; emitted wasm byte-identical (golden + dual-mode e2e).
        2026-06-09.
  - [x] Bench cells run under both modes; production three-way data
        in `rewrite/m28-bench-results.md`.  The §11.4 ±10%
        reproduction FAILED (17–22× measured vs ~31× claimed) — the
        results doc records the corrected headline and causes.
        2026-06-09.
  - [x] `cel.abi` `LinkMode` proto field added (field 7); legacy
        bytes decode as `LINK_MODE_DYNAMIC` (hand-rolled legacy-wire
        test), dynamic sections stay byte-identical to pre-field
        Programs (structural pin), unknown future values pass through
        (`abi/cel_abi_emit_test.cc`); both compile arms stamp their
        mode (`compiler/internal/compile_test.cc`).  2026-06-09.

**P2 follow-ups (reframed from P1; defense-in-depth, not gate items):**

  - [ ] Defense-in-depth `CallInit` helper at `Engine::Plan` time —
        look up `__wasm_call_ctors` on `helpers_instance`, invoke
        once if present, skip silently if absent.  (Originally
        tracked as P1 against the design's promised
        `CallInit(_initialize | __wasm_call_ctors)` and a cctz-
        touching e2e.)  *(Reframed 2026-06-09; see
        `m28-configurable-linking.md` §10.1 invariant 9 and
        §13 P2.  The strip tool DCEs `__wasm_call_ctors`
        entirely from the stripped runtime, and the dual-mode
        e2e sweep — including `e2e/cctz_doubles_test.cc` —
        showed every tested cctz / absl path is bit-identical
        between modes.  Helper remains useful as a tripwire for
        future surfaces, e.g. RE2-driven regex or a new absl
        format-spec.)*

### Conformance burndown — Round 3 (2026-06-05)

  - [x] **proto2 extension field look-up** at
        `eval/internal/cel_host.cc::ResolveFieldDescriptor` +
        `eval/internal/cel_host_test.cc::ProtoBackingExtensionTest`
        (4 cases: read by full name, has-true, has-false,
        unknown-ext-name).  Closes 16 of 18 rows in
        `proto2/extensions_has` + `proto2/extensions_get` (the two
        remaining `*_repeated_test_all_types` `extensions_get`
        rows are a separate list-equality surface — tracked under
        cleanup-backlog #40 follow-up note).

## Rewrite M27 — property-based testing machinery (Slices A+B shipped 2026-06-05; Slice C harness 2026-06-09)

Coverage that shipped with the PBT milestone (design in
`doc/implementation-plan/rewrite/m27-pbt-cel-generator.md`; section
added in the 2026-06-10 review sweep — the milestone landed without
flipping any checklist rows):

  - [x] fuzztest framework wired and smoke-tested
        (`e2e/fuzz/fuzz_smoke_test.cc`).
  - [x] grammar structural validation (L1), per-production checker
        round-trip (L2), generated-expression type agreement with
        cel-cpp (L3) — `e2e/fuzz/grammar_test.cc` over
        `grammar.{h,cc}` + the catalogs (renamed 2026-06-11 from
        `grammar_slice_b/c.{h,cc}` to `grammar_scalars.{h,cc}` +
        `grammar_aggregates.{h,cc}`).
  - [x] type-driven generator unit coverage
        (`e2e/fuzz/generator_test.cc`).
  - [x] oracle property — generated expressions evaluate equal under
        cel-wasm and the real cel-cpp pipeline
        (`e2e/fuzz/cel_oracle_property_test.cc` via
        `oracle_harness.{h,cc}`; divergence mining in
        `mine_divergences.cc`, sample dumper in `dump_samples.cc`).
  - [x] remaining Slice C/D scope (additional productions, depth-8
        budget) — shipped under Rewrite M30 (below).

## Rewrite M30 — differential fuzzing to the full CEL dialect (A/B/C-nested/D-string/F/G shipped 2026-06-11)

Coverage that shipped extending the M27 machinery to the full static
subset (design + slice status in
`doc/implementation-plan/rewrite/m30-fuzz-full-dialect.md`; per-overload
grid in `e2e/fuzz/COVERAGE.md`; session journal in
`e2e/fuzz/SESSIONS.md`):

  - [x] adversarial leaf set — INT64/UINT64 boundaries, 2^53±1, ±0.0,
        denormal, 1e308, embedded-NUL, multi-byte / invalid UTF-8
        (`grammar_scalars.cc`).
  - [x] error-producing arithmetic + error-ness as a compared
        dimension (`GenAndEvalStatus`; `oracle_harness.cc`).
  - [x] nested aggregates — `list<list<T>>`, `list<map<K,V>>`,
        `map<string,list<int>>` leaves + constructors + size +
        container-iter_var comprehensions (`grammar_aggregates.cc`).
  - [x] string functions (contains/startsWith/.../substring/replace/
        split/quote), `string.format`, base64 encoders, math_ext (28),
        temporal accessors incl `_with_tz`, cross-numeric + string/
        bytes/bool conversions (`grammar_scalars.cc`).
  - [x] CI-gateable miner (exit code = divergence count) + `scripts/
        fuzz.sh` runner + nightly `.github/workflows/fuzz.yml`.
  - [x] every PBT-found divergence pinned in `e2e/known_bugs_test.cc`
        as a `Pbt*` test (inventory in `COVERAGE.md`).
  - [ ] open slices (tracked in the milestone doc): conversion
        remainder (numeric-string leaves, duration/timestamp parse),
        two-arg pos/limit string forms, temporal arithmetic, list/map
        `FUZZ_TEST` registrations, net_ext + optionals (blocked on a
        `shared/type.h` type-vocabulary extension).

## Expression-depth gate (cleanup-backlog #45 interim fix, 2026-06-10)

Compile-time bound on AST nesting depth (`kMaxExpressionNestingDepth`
= 2048, `compiler/frontend/parse_and_check.{h,cc}`); over-deep
expressions reject with ResourceExhausted instead of overflowing the
native stack at lowering / wasmtime Plan time:

  - [x] shallow expression admitted
        (`parse_and_check_test.cc::DepthGateAdmitsShallowExpression`).
  - [x] chain at exactly the limit admitted (boundary positive)
        (`DepthGateAdmitsChainExactlyAtLimit`).
  - [x] chain at limit+1 rejected, message names measured depth +
        limit (`DepthGateRejectsChainOneOverLimit`).
  - [x] nested-aggregate shape (`[[[…]]]`) at limit+1 rejected
        (`DepthGateRejectsNestedListOneOverLimit`).
  - [x] parser-recursion-limit backstop normalised to the same
        ResourceExhausted (`DepthGateParserBackstopIsResourceExhausted`).
  - [x] e2e: pre-fix SIGSEGV boundary N=4654 now rejects gracefully;
        1000-term headline bench chain still compiles + evals — both
        link modes (`e2e/known_bugs_test.cc::
        DeepArithChainFormerSegvBoundaryRejectedAtCompile` /
        `LongArith1000TermsStillEvalsUnderDepthGate`).

## Error/unknown wire-contract fork resolution (V3 + V4, 2026-06-10)

Settled the two error-side forks of `doc/design/03-abi-and-memory.md`
§8 oracle-first; the §8.2 unknown fork (V2) stays open.

  - [x] oracle pin: strict binary op over (unknown, error), BOTH
        orders → ERROR wins (`testdata/cel_cpp_oracle_test.cc::
        PartialEvalOracle.{UnknownPlusErrorIsError,
        ErrorPlusUnknownIsError}`; cel-cpp `NoOverloadResult`,
        eval/eval/function_step.cc:202-223).
  - [x] host `AbsorbBinary` aligned to error-dominant; both orderings
        pinned (`eval/internal/cel_host_error_test.cc::
        AbsorbBinaryTest.{SecondOperandErrorBeatsFirstOperandUnknown,
        FirstOperandErrorBeatsSecondOperandUnknown}`).
  - [x] kernel `absorb_3vl_binary` orderings pinned through
        `cel_int_add_at_vv` (`runtime/cel_arith_test.cc::
        {UnknownLeftErrorRightPropagatesError,
        ErrorLeftUnknownRightPropagatesError}`).
  - [x] `DecodeCelError` gains the missing `kInvalidArgument` arm;
        exhaustive wire round-trip — all 14 named `ErrorCode`s + an
        out-of-enum byte — through a host fn returning each code
        (`eval/instance_test.cc::ErrorCodeRoundTripTest`, both link
        modes).
  - [x] encode-side `WireErrorCode` maps every named code (was
        collapsing kDuplicateKey/kUnknownType/kCustomFnFailed/
        kTimeout to TYPE_MISMATCH) and passes out-of-enum numerics
        through (`eval/internal/cel_host_error_test.cc::
        WireErrorCodeTest.EveryHostCodeMapsToWireConstant`; the
        out-of-enum pass-through is pinned e2e in
        `instance_test.cc::UnrecognizedWireCodeDegradesGracefully`).
  - [x] `%v` error formatter reads the production bare-code wire
        (`eval/host/cel_log_test.cc::ValueErrorKindBareCode`; dead
        descriptor-shape fixture removed).
  - [x] example 08 + `examples_smoke_test.sh` updated to the fixed
        decode (`error value: invalid_argument`).
  - [x] conformance gate held at 1966/1966 (static + dynamic).

## Host-backed `ofNonZeroValue` zero predicate (cleanup-backlog #10, 2026-06-10)

Replaced the three `__builtin_trap()` arms of
`runtime/cel_optional.c::is_zero_value` (CEL_MESSAGE / CEL_LIST_HOST
/ CEL_MAP_HOST) with host probes; new host import
`cel_host.cel_message_is_zero(out_slot, msg_slot)` (cel-cpp parity:
`ParsedMessageValue::IsZeroValue()`, parsed_message_value.cc:78).
Conformance: 1972 → 1973 PASS, 1 → 0 FAIL, both link modes — the
corpus' last FAIL row
(`optionals/optional_ofNonZeroValue_struct_optional_ofNonZeroValue_map_optindex_field`)
now passes.

  - [x] oracle pins added BEFORE the fix (the oracle gained
        optional-type support: `OptionalCompilerLibrary` +
        `extensions::EnableOptionalTypes` in
        `testdata/cel_cpp_oracle.cc`):
        `cel_cpp_oracle_test.cc::OptionalOfNonZeroValueMessage.
        {ConformanceRowOracleIsFalse,ZeroMessageOracleIsFalse,
        NonZeroMessageOracleIsTrue}` + the three `*Agrees`
        differentials.
  - [x] WAT-first: `rewrite/wat/69_optional_of_non_zero_message.wat`
        (`wat-traces.md` §69) run end-to-end through `wat_runner`
        with a scripted 2-arg stub, both verdicts
        (`wat_runner_test.cc::WatRunnerM14Test.
        OptionalOfNonZeroOn{Zero,NonZero}MessageProduces{None,Some}`);
        wat_runner grew the generic `CelHostTwoArgStub` surface.
  - [x] kernel matrix: `runtime/cel_optional_test.cc` — every
        CelKind arm now covered (scalars zero+non-zero incl. -0.0,
        string/bytes empty+non-empty, arena list/map empty+non-empty,
        duration/timestamp zero+non-zero, type, IP/CIDR never-zero,
        ERROR/UNKNOWN propagation, host list/map/message via
        strong-override probes incl. poison-result → Some and the
        inner-cell-offset recursion pin).
  - [x] trampoline matrix: `eval/internal/cel_host_test.cc::
        CelMessageIsZeroTest` (11 cases: proto3/proto2 zero, set
        field, field-set-to-default stays zero, only-unknown-fields
        non-zero, OwnedProtoBacking, UNKNOWN/ERROR propagation,
        non-message kTypeMismatch, unmapped slot + non-proto backing
        kHostAdapterError).
  - [x] catalogue row in `abi/runtime_host_env.textproto` +
        trampoline in `cel_host_wasmtime.cc` (bijection CHECKs keep
        them locked); runtime-wasm instantiation stub in
        `cel_runtime_wasm_test.cc`.
  - [x] e2e, both link modes: `e2e/m14_test.cc::
        ProtoOptionalFieldE2ETest.{OfNonZeroValueOnNonZeroMessageHasValue
        (un-skipped known-bug case),OfNonZeroValueOnZeroMessageIsNone,
        OptionalOfNonZeroValueStructOptionalOfNonZeroValueMapOptindexField}`.

## Comprehension range absorption (cleanup-backlog #14, 2026-06-10)

A comprehension whose `iter_range` is UNKNOWN / ERROR now propagates
that value instead of returning the empty-range identity
(exists→false, all→true, exists_one→false, map/filter→[]) — the
prologue emits a 3VL guard on the range value before any iterate-path
setup (`expr_lower_comprehension.cc::EmitRangeAbsorptionGuard`;
cel-cpp parity: `comprehension_step.cc:165-169` / `:350-354`,
`result = std::move(range)`).  Conformance held 1973/0 per mode (no
corpus rows exercise unknown ranges).

  - [x] oracle pins added BEFORE the fix — new oracle-only TU
        `testdata/cel_cpp_oracle_comprehension_test.cc`:
        `ComprehensionUnknownRangeOracle` (exists / all / exists_one
        / map / filter + map-range exists / transform-map →
        UNKNOWN), `ComprehensionErrorRangeOracle` (same 7 shapes →
        ERROR), `ErrorRangeDominatesUnknownBody`, concrete-range +
        unknown-/error-BODY controls.
  - [x] WAT-first: `rewrite/wat/70_comprehension_unknown_range.wat`
        (`wat-traces.md` §70) run end-to-end through `wat_runner`
        against the real kernel, no stubs
        (`WatRunnerComprehensionTest.UnknownRangeAbsorbsIntoResult`);
        wat_runner export list grew `cel_list_arena_view`.
  - [x] codegen: prologue + loop wrapped in a named
        `comp_absorb_<expr_id>` block; guard copies the poison into
        the accu slot and brs past the loop; accu local set hoisted
        before the guard so `@result` resolves on both paths.
  - [x] trampoline tripwire: `CelListIterOpenImpl` /
        `CelMapIterOpenImpl` return FailedPrecondition on
        CEL_UNKNOWN / CEL_ERROR inputs (guard-regression canary) —
        new `eval/internal/cel_iter_open_impl_test.cc` (snapshot
        happy paths, empty source, poison → loud failure, other
        non-host kinds → defensive empty, missing ref_slot).
  - [x] e2e, both link modes (`e2e/m2_partial_eval_test.cc`): the 2
        pinned GTEST_SKIPs deleted
        (`ComprehensionOverUnknownListIsUnknown`,
        `ShadowedRangeVarUnknownIsUnknown`); new
        `ComprehensionUnknownRangeE2E` (7 macro shapes × unknown
        range), `ComprehensionErrorRangeE2E` (7 × error range),
        `ErrorRangeDominatesUnknownBody`, `ConcreteRangeControls`
        (per-macro happy path), and the accumulator-3VL negative
        controls (`ConcreteRange{UnknownBodyIsUnknown,
        ErrorBodyIsError}`).

## Kernel deep equality + OOM poison + overflow guards (cleanup-backlog #40/#46, 2026-06-10)

Three kernel-correctness fixes in `runtime/cel_runtime.c` /
`runtime/cel_arena.c`: (1) arena+arena aggregate equality routes
non-scalar element pairs (CEL_MESSAGE, nested lists/maps, CEL_TYPE,
CEL_OPTIONAL) through the polymorphic equality kernel instead of the
scalar-only matcher (`[Msg{x:1}] == [Msg{x:1}]` was silently false);
(2) OOM / poisoned-source comprehension prologues vend a poison
view/iteration instead of degrading to an empty walk, backed by a
per-Instance emergency block (`arena_oom_block`); (3) wasm32
multiply/add overflow guards at every alloc-size math site (reject →
CEL_ERR_OVERFLOW, never wrap).  Conformance held 1973/0 per mode.

  - [x] kernel deep-eq matrix — `runtime/cel_deep_eq_test.cc`:
        message lists {equal, unequal-value, length-mismatch,
        empty×nonempty}, message-valued maps {equal, unequal,
        order-independent}, nested aggregates (list-in-list,
        map-in-list, list-in-map, two levels), scalar fast-path
        controls (no host trip), equivalence with direct
        `msg == msg`, trampoline wire contract (element cell
        offsets), `in` with message / list needles, nested
        host-origin elements route to host trampolines,
        cross-kind false, not-comparable pair unequal-not-error,
        CEL_TYPE elements.
  - [x] OOM/poison vends — `runtime/cel_oom_poison_test.cc`:
        emergency-block reservation survives reset + exhaustion;
        `cel_list_arena_view` {arena pass-through, host-snapshot
        OOM → OVERFLOW view, error/unknown source → verbatim
        poison view, wrong kind → TYPE_MISMATCH view, poison view
        at true OOM}; `cel_map_iter_init` {state OOM → OVERFLOW
        iteration, error source → verbatim, wrong kind →
        TYPE_MISMATCH, empty-map 0-handle control};
        `cel_map_insert` strict-construction 3VL (value error,
        key error before kind gate); deep-eq scratch OOM →
        OVERFLOW, never false.
  - [x] overflow guards — `runtime/cel_oom_poison_test.cc`
        (list/map create adversarial capacities, byte-ceiling
        boundary, concat count-add wrap, concat stride-multiply),
        `runtime/cel_arena_test.cc` (unalignable
        `(UINT32_MAX-7, UINT32_MAX]` tail rejected; OOM block
        outside arena accounting, never overlapped),
        `runtime/cel_string_ops_test.cc` (string concat length-add
        wrap poisons OVERFLOW).
  - [x] flipped pin — `runtime/cel_map_test.cc`
        `PoisonedMapInitReturnsZeroHandle` →
        `PoisonedMapInitVendsOneErrorIteration`.
  - [x] e2e, both link modes —
        `e2e/arena_message_aggregate_eq_test.cc`:
        `ArenaMessageAggregateEqE2ETest` (message lists/maps,
        nested aggregates, `in`, equivalence with direct message
        eq, scalar controls) +
        `PoisonedComprehensionSourceE2ETest` (`[1/0].map/exists`,
        error-element filter, `{'a': 1/0}` map literal /
        size / exists, error-keyed literal, healthy controls).

## Unknown-payload descriptor contract + logic-op 3VL precedence (V2 fix, 2026-06-10)

The `payload.unk` wire is the UnknownSet descriptor everywhere
(`doc/design/03-abi-and-memory.md` §8.2 — host writers mint
descriptors, decoders dereference, `Value::Unknown` carries the
attribute-id set, `absorb_3vl_binary` merges both-unknown), and the
logic ops carry the oracle-confirmed UNKNOWN-over-ERROR precedence
(§8.3 scope note).  Conformance held 1973/0 per mode.

  - [x] oracle pins — `testdata/cel_cpp_oracle_unknown_payload_test.cc`:
        merged-set provenance (and/or/add × dotted/bare, dedup,
        single-attr baseline) + logic-op precedence (`unknown && error`
        → unknown in both orders, both ops; absorbing-bool controls
        `false && error` → false, `true || error` → true,
        `unknown && false` → false, `unknown || true` → true).
  - [x] kernel — `runtime/cel_3vl_test.cc`: 4×4 cel_and/cel_or
        matrices flipped to UNKNOWN > ERROR (E_U/U_E rows) +
        unknown-vs-error id-set preservation both orders;
        `runtime/cel_arith_test.cc::BothUnknownMergesAttributeIdSets`
        (strict-op both-unknown merge through `cel_int_add_at_vv`).
  - [x] Value set surface — `eval/value_test.cc`: multi-id set
        (sorted), dedup, `UnknownAttribute()` FailedPrecondition on
        merged sets, wrong-kind accessors, empty set, set equality.
  - [x] host writers/decoders — `eval/host_call_context_test.cc`
        (ReturnUnknown mints sentinel descriptor; ReturnValue
        preserves 1-element + merged sets; arg decoder surfaces
        every descriptor id; empty-set payload; OOB descriptor →
        InvalidArgument); `eval/internal/cel_host_test.cc`
        (trampoline mints 1-element descriptor on pattern match);
        `eval/typed_function_test.cc` (sentinel travels in
        descriptor).
  - [x] e2e, both link modes —
        `e2e/m2_partial_eval_test.cc::MergedUnknownProvenanceTest`
        (`a && b` / `a || b` / `a + b` both-unknown decode BOTH
        identities; dotted `a.age && b.age`; same-attr dedup;
        single-unknown regression);
        `e2e/m5_test.cc::ControlFlowUnknownErrorPrecedenceE2ETest`
        (unknown-over-error, both orders, both ops).

## How to update

When you add a test, flip the box to `[x]` and include the test's path in
the adjacent cell *if* the mapping isn't obvious.  When a new AST variant or
type lands, add a new row; never silently drop a row.
