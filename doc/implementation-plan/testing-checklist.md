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
  - `kComprehensionExpr` × 4 variants + nested shadowing — M3/M5.
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
| `timestamp`     | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `duration`      | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `list<T>`       | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `map<K,V>`      | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| proto message   | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| enum            | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| wrapper (Int64Value …) | [ ] | [ ]  | [x]         | [x]       | [ ]     | [ ]      |
| `any`           | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
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
| `kStructExpr` (proto ctor) | [x] | [ ] | [x]      | [x]       | [ ]     | [ ] |
| `kMapExpr`          | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (exists) | [ ] | [ ] | [ ]  | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (all)    | [ ] | [ ] | [ ]  | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (filter) | [ ] | [ ] | [ ]  | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (map)    | [ ] | [ ] | [ ]  | [x]       | [ ]     | [ ] |
| nested comprehensions with shadowing | [ ] | [ ] | [ ] | [ ]   | [ ]     | [ ] |

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
      (`ThreeValuedAbsorptionNaNCompareAbsorbed`).  Remaining
      DISABLED shells: row 8 (ternary result — step 6), 14 / 15 /
      16 / 17 / 21 (string / bytes / size / msg-eq absorption —
      steps 4–5), 7 / 19 (ternary simplify — step 6).

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

## How to update

When you add a test, flip the box to `[x]` and include the test's path in
the adjacent cell *if* the mapping isn't obvious.  When a new AST variant or
type lands, add a new row; never silently drop a row.
