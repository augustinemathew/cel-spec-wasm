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
  - `kComprehensionExpr` × 4 variants + nested shadowing — comprehensions follow-on milestone (post-M5).
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
      link.  Locked in `compiler_v2/runtime/cel_arith_test.cc` (23
      tests) + `compiler_v2/runtime/cel_compare_test.cc` (initial
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
      across `compiler_v2/runtime/cel_compare_test.cc` (28 total) +
      `compiler_v2/runtime/cel_string_ops_test.cc` (27 total).
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
      `compiler_v2/runtime/cel_string_ops_test.cc` (initial 18 tests
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
      `compiler_v2/runtime/cel_aggregate_arena_test.cc` (21 tests)
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
      in `compiler_v2/codegen/overload_table_test.cc`.
- [x] **Rewrite M5.F** — general kCall arm (`EmitGeneralCall`)
      wires `OverloadTable::Lookup(ann.overload_id)` into
      `expr_lower.cc`.  Arithmetic / same-kind comparison /
      string ops / receiver-form ops compile end-to-end through
      `Compile → Plan → Eval`.  M5.G control-flow operators
      (`_&&_` / `_||_` / `_?_:_` / `!_`) and M5.D-step-2 pending
      dispatchers (`size_list` / `size_map` / `add_list` / `in_list` /
      `in_map`) surface as Unimplemented.  Conformance: 207 → 391
      PASS.  Locked in `compiler_v2/codegen/expr_lower_test.cc`
      (9 new lowering tests) + `compiler_v2/e2e/m5_test.cc`
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
      `compiler_v2/codegen/expr_lower_test.cc` (3 re-pick tests),
      `compiler_v2/runtime/cel_aggregate_arena_test.cc` (8
      polymorphic membership tests), `compiler_v2/e2e/m5_test.cc`
      (`CrossNumericOrderingE2ETest` — 152 tests across the full
      operand-kind × operator × dyn-position matrix + boundary +
      NaN matrix + membership matrix + same-kind regression
      guards).

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
v1 stays in its own sections; v2 (everything under `compiler_v2/`)
is tracked here.

  - [x] Static-literal lowering × bool —
        `compiler_v2/api/instance_test.cc::EvalsBoolLiteralTrue/False`
  - [x] Static-literal lowering × int —
        `compiler_v2/api/instance_test.cc::EvalsIntLiteral`,
        `EvalsNegIntLiteral`
  - [x] Static-literal lowering × uint —
        `compiler_v2/api/instance_test.cc::EvalsUintLiteral`
  - [x] Static-literal lowering × double —
        `compiler_v2/api/instance_test.cc::EvalsDoubleLiteral`
  - [x] Static-literal lowering × null —
        `compiler_v2/api/instance_test.cc::EvalsNullLiteral`
  - [x] Static-literal lowering × string —
        `compiler_v2/api/instance_test.cc::EvalsStringLiteral`
  - [x] Static-literal lowering × bytes —
        `compiler_v2/api/instance_test.cc::EvalsBytesLiteral`
  - [x] Two-phase instantiation × fresh memory per `Engine::Plan` —
        `compiler_v2/api/engine_test.cc::PlanCalledTwiceProducesIndependentInstances`,
        `compiler_v2/api/instance_test.cc::TwoInstancesEvaluateIndependently`
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
under `compiler_v2/`) is tracked here.

**Slice M2.A — `cel::Activation`**

  - [x] `Bind` / `BindLazy` / `Find` per scalar kind —
        `compiler_v2/api/activation_test.cc`
  - [x] `Find` on unbound → `NotFoundError`,
        `BindLazy` memoises across `Find` —
        `activation_test.cc`

**Slice M2.B — `kIdent` lowering + `Eval(Activation)` +
`cel.abi.variables[]`**

  - [x] `kIdent` emits `local.get` (workspace slot) —
        `compiler_v2/codegen/expr_lower_test.cc`
  - [x] `Instance::Eval(Activation)` per scalar kind —
        `compiler_v2/e2e/m2_test.cc::IdentE2ETest::{Bool,Int,Uint,
        Double,String,Bytes}`
  - [x] Unbound declared variable → `FailedPrecondition` —
        `m2_test.cc::IdentE2ETest::UnboundDeclaredVariableFailsPrecondition`
  - [x] Back-to-back Eval rebinds ident cleanly —
        `m2_test.cc::IdentE2ETest::BackToBackEvalRebindsIdent`
  - [x] `cel.abi.variables[]` serialised with name /
        local_index / slot_offset / repr —
        `compiler_v2/abi/cel_abi_emit_test.cc`
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
        `compiler_v2/api/instance_test.cc::InstanceSelectEvalTest::{
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
        `compiler_v2/api/attribute_test.cc` (24 parse tests:
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
        empty pattern set — `compiler_v2/conformance/runner.{h,cc}`.
  - [x] `run_conformance` shows no regressions vs the M1
        snapshot (`total=2454 · pass=178 · skip=1935 · fail=341`)
        — `compiler_v2/conformance/README.md` inventory table
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
        `compiler_v2/conformance/README.md` inventory updated.

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
  - [x] M4.J e2e suite — `compiler_v2/e2e/m4_test.cc` (16 tests
        across `ListLiteralE2ETest`, `ProtoRepeatedE2ETest`,
        `ProtoRepeatedHostMsg3E2ETest`).  `Customer` proto
        gained `repeated string tags = 12` for the kHost-list
        e2e flows.  `map-list-dispatch.md §11` reconciliation
        checklist fully ticked; header flipped to "fully
        reconciled into design.md 2026-04-25".
        `scripts/run_full_suite.sh` MANUAL_TARGETS += `//compiler_v2/e2e:m4_test`.

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

`compiler_v2/api/instance.cc::EncodeBoundValue` now routes
`Repr::kString` and `Repr::kBytes` to a new `EncodeStringOrBytes`
arm.  Payload bytes land in a host-managed arena above
`arena_limit` (codegen's `cel_reset` ceiling), grown via
`wasmtime_memory_grow` on demand — bypasses the `cel_reset` rewind
that previously stomped any pre-eval `cel_alloc` allocations.  See
`doc/implementation-plan/rewrite/conformance-unlock-plan.md` Slice 0.

  - [x] String round-trip through Activation::Bind →
        `Instance::Eval(Activation)` →
        `compiler_v2/api/instance_test.cc::InstanceActivationStringEncoderTest::
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
        `compiler_v2/e2e/m5_test.cc::StringBytesActivationE2ETest::
        {BindStringPlusLiteral,BindBytesSize,BindEmptyString,
        BindEmbeddedNul,BindMultibyteUtf8,BindTwoStringsConcat,
        RebindAcrossEvalsRewindsArena,BindBytesWithNul}`
  - [x] Conformance harness flips kString-blocked SKIPs to live
        Eval — `namespace.textproto` 3 → 4 PASS; total 486 → 490
        (full per-fixture deltas in
        `compiler_v2/conformance/README.md` post-Slice-0 row).

### Rewrite M7 — proto message literals (slices A–E shipped 2026-04-25)

`m7-proto-literals.md` slices A–E delivered `kStructExpr` codegen
+ `cel_make_message` / `cel_set_field` host primitives + per-cpp_type
scalar/repeated/map/oneof/enum/nested-message dispatch +
`InlineConstantReferences` rewrite for cel-cpp's enum-name-as-constant
resolution.  Conformance: **+131 PASS** (700 → 831).  Plan-vs-execution
delta and remaining unblockers captured in `m7-proto-literals.md` §9.

**M7.A — `kStructExpr` admission + `cel_make_message`**

  - [x] `cel.abi.types[]` ABI table — `compiler_v2/abi/cel_abi.proto`
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
        `cel_set_field` (3-arg) imports.  `compiler_v2/compile.cc`.
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
  - [x] E2E: `compiler_v2/e2e/m7_test.cc::ProtoLiteralEmptyE2ETest`
        — 9/9 PASS (proto3 zero/explicit-default/null-submessage,
        proto2 explicit-default×3, Customer empty).

**M7.B — `cel_set_field` for scalar fields**

  - [x] `kCelHostSetFieldInternalName` constant + 3-arg `cel_set_field`
        import.  `codegen/expr_lower.h`, `compiler_v2/compile.cc`.
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

  - [x] `compiler_v2/conformance/README.md` refreshed: headline
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
        verification done via `bazel test //compiler_v2/... --test_output=errors
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
        ±Inf → "+Inf" / "-Inf"; ±0 → "0"; integer-valued doubles
        in safe-cast range → exact via int path; mid-magnitude
        → integer + "." + fractional digits; very large / very
        small magnitudes → scientific with normalized mantissa.
        Byte-exact match against cel-cpp's `to_chars` is NOT a
        contract — round-trip safety is.  Grisu/Ryu body swap
        deferred to §9 if a conformance row demands it.
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
        //compiler_v2/conformance:run_conformance` →
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

## How to update

When you add a test, flip the box to `[x]` and include the test's path in
the adjacent cell *if* the mapping isn't obvious.  When a new AST variant or
type lands, add a new row; never silently drop a row.
