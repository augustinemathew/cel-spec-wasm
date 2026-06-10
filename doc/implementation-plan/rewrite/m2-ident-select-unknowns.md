# Rewrite M2 — idents, proto field reads, `Activation`, unknowns

Status: **shipped 2026-04-25.**  All slices M2.A–M2.F land
end-to-end; `scripts/run_full_suite.sh` green (default suite +
all manual-tagged targets); `bazel run
//conformance:run_conformance` shows
`pass=203 / skip=1848 / fail=403` (up from M1 snapshot
`178 / 1935 / 341` — +25 PASS from kSelect / has() / PartialEval
graduating).

> **Plan-vs-execution honesty note (2026-04-25).**  An earlier
> commit marked this doc "shipped 2026-04-24," but a routine
> validation that day found M2 was actually half-done: every
> `SelectE2ETest` (12), `HasE2ETest` (6), and `UnknownE2ETest`
> (7) test in `e2e/m2_test.cc` had a fixture-level
> `GTEST_SKIP` ("pending M2.C" / "pending M2.E") that hid the
> gap from `bazel test //...` (the e2e target is
> tagged `manual`).  Today's commit closes the gap by:
>
>   1. Implementing `CelGetFieldImpl` / `CelHasFieldImpl` (M2.C.0b
>      Layer-2 bodies — never landed in the original M2.C.0
>      slice).
>   2. Wiring `RegisterCelHostImports` into `Engine::Plan::InitLinker`
>      and populating `host_env.memory` / `host_env.cel_alloc_fn`
>      / `host_env.bindings` from the decoded ABI in
>      `InstantiateRuntime`.
>   3. Adding `EncodeMessage` to the activation marshaller so
>      `Repr::kMessage`-bound variables intern into the
>      externref table at `Eval(Activation)` time.
>   4. Implementing `Instance::PartialEval` (M2.E) — the same
>      marshal path with `unknown_patterns` populated, and the
>      Layer-2 prelude consulting them via FULL-match against an
>      *effective* attribute (operand attribute ⊕ field name).
>   5. Removing the fixture-level `GTEST_SKIP`s in `m2_test.cc` —
>      `SelectE2ETest` + `HasE2ETest` + `UnknownE2ETest` now all
>      run green against the real wasmtime path.
>
> The follow-up doc `doc/implementation-plan/per-component-test-coverage.md`
> codifies the closeout gate so this can't recur: a milestone
> doesn't close until `scripts/run_full_suite.sh` is green
> (default + manual-tagged), no fixture-level skips remain, and
> the conformance README inventory is refreshed.

What landed vs as-written plan:

  - Ident + select + has + PartialEval end-to-end through the
    `Compiler → Engine::Plan → Instance::Eval` pipeline.
  - Three-layer `cel_host` split (Layer 1 backing semantics in
    `internal/cel_host.{h,cc}`, Layer 2 runtime-agnostic
    trampoline bodies, Layer 3 wasmtime glue in the dedicated
    `internal/cel_host_wasmtime.{h,cc}`).  The as-written plan
    lumped Layer 3 into the same file; splitting it kept
    runtime-agnostic code free of wasmtime headers.
  - `cel.abi` custom section now carries `variables[]`,
    `fields[]`, `attributes[]` (each sentinel-at-0, dense); the
    `types[]` + `patterns[]` sub-sections defer to M3+ as
    planned.
  - Mid-milestone simplifications: `abi_decode` returns proto
    directly (mirror structs deleted), `RegisterMessageType`
    dead API deleted, LE endianness static assertion added,
    `(void)Foo::descriptor()` → `LinkMessageReflection<Foo>()`
    across eight test files (consolidated into file-scope
    `kDescriptorsLinked` static blocks).

Follow-ups surfaced (see §8 Future work):

  - Harness-side `ExprValue` → `cel::Activation` marshalling —
    the gate for graduating `fields` / `namespace` / `macros`
    fixtures in the conformance corpus.
  - Conformance-harness expr-id → attribute-id map so
    `UnknownSet.exprs` matching is id-level, not just kind-level.

Parent: `design.md` (this directory).  Predecessor: `m1-scalar-pipeline.md`
— the M1 skeleton that M2 fills in.  Companion: `cel-host-surface.md`
— authoritative for `cel::Activation`, `cel::Instance::PartialEval`,
`AttributePattern`.  Companion (new): `wat-traces.md` — target wasm
for every codegen arm, written before the C++ (per CLAUDE.md's
WAT-first rule added during M2).

## Progress log

### Landed (2026-04-23)

**Leaf types and constants**

  - `runtime/cel_data.h` — new `CEL_ERR_TYPE_UNSUPPORTED = 14`
    (M2→M6 graduation contract for MAP / REPEATED field reads).
  - `api/error.h,cc` — mirrored `ErrorCode::kTypeUnsupported`.
  - `ir/annotations.h,cc` — new `Origin` enum
    (`kDynamic`/`kArena`/`kHost`) with crash-on-unknown
    `OriginName`.  `NodeAnnotation` extended with `attribute_id`,
    `map_origin`, `list_origin` (forward-compat per §2.6, §2.8).

**Attribute path parsing (part of §2.3)**

  - `api/attribute.h,cc` — `AttributePattern::Parse(dotted)`.
    Supports dotted identifiers (`c.billing_address.city`),
    wildcards (`c.*.city`), bracketed int / uint / bool / quoted-
    string / wildcard qualifiers (`xs[3]`, `xs[3u]`, `m[true]`,
    `m["key"]`, `xs[*]`), mixed forms (`request.messages[3].text`),
    and rejects leading/trailing dots, consecutive dots,
    bracketed roots, empty or unterminated brackets.
  - 24 new parser tests in `api/attribute_test.cc`.

**Compiler surface (part of §2.1)**

  - `api/compiler.h,cc` — `Builder::DeclareVariable(name, CelType)`
    and `Builder::RegisterMessageType(const Descriptor*)` with
    ref-qualified `&` / `&&` overloads so both inline and
    multi-statement fluent chains work.  New `VariableDeclaration`
    struct (renamed from the plan's `VariableDecl` to avoid a
    fatal ODR clash with cel-cpp's `cel::VariableDecl`).
  - `Compile` translates declarations into `CheckOptions::
    variable_specs` via a `CelTypeToSpec` converter.  M5/M6-only
    kinds (unknown, unimplemented) fail loud via `ABSL_CHECK`
    rather than silently falling through.
  - 8 new tests in `compiler_test.cc` covering scalar + container
    + message declarations, duplicate-name rejection, empty-FQN
    rejection, flow-through to checker.

**ResolvePass (part of §5.2 / §2.6, slice M2.B)**

  - `codegen/resolve_pass.h,cc` — `IdentResolver` visitor interns
    every kIdent name (first-seen order), assigns dense
    `local_index`, populates `NodeAnnotation::local_index`, and
    fills `ResolveOutput::variables: vector<ResolvedVariable>`.
  - Two invariant CHECKs: (1) kIdent with `Repr::kUnknown` means
    the checker dropped a type_map entry, (2) same name with
    mismatched Repr across references means the checker produced
    inconsistent types.
  - 50 tests in `resolve_pass_test.cc`, grouped into 8 suites —
    parameterised Repr mapping for every declarable ident type
    (scalars, containers, messages), same-slot shape tests for
    every nesting shape (`x+x`, `l[0]+l[1]`, `arr[arr[0]]`,
    `c.rep_i32[c.i32+10]`, etc.), mixed-shape tests with multiple
    distinct variables at different AST depths.

**LayoutPass (part of §6.1, slice M2.B)**

  - `codegen/layout_pass.h,cc` — new `LaidOutVariable`
    `{name, local_index, repr, slot_offset}`; `StaticLayout`
    exposes `variables` (one entry per referenced variable).
    LayoutPass reserves a 24-byte CelValue workspace slot per
    variable at `workspace_base + local_index * 24`.
  - `IdentStorageVisitor` writes `{StorageKind::kLocal,
    local_index}` on every kIdent annotation — following the plan's
    §2.6 choice of wasm locals for all idents (free variable,
    comprehension iter, comprehension accu) with one uniform
    `expr_lower` arm.
  - 9 new variable-slot tests in `layout_pass_test.cc`.

**Data-model cleanup (§5.1 delta)**

  - Dropped `std::vector<BinaryenType> local_types` from both
    `ResolveOutput` and `StaticLayout`.  Every wasm local is
    uniformly i32 (an offset) and `variables.size()` already
    carries the count; the vector carried no information.  The
    emission-time type array is built inline at `AddFunction`
    time.  `resolve_pass` and `layout_pass` targets no longer
    depend on `@binaryen//:binaryen`.
  - Plan §5.1 (line 1307) and §6.1 (line 1368) reference
    `local_types` — treat those as outdated; the scalar count on
    `variables.size()` is authoritative going forward.

**Failing e2e test suite (milestone gate)**

  - `e2e/m2_test.cc` — 41 e2e tests over the real
    `Customer` / `HostMsg3` fixtures, covering every cell of the
    §6.2 matrix plus the envelope-boundary row.  Currently does
    not build (references Instance::Eval(Activation),
    PartialEval, Value::Message unstub, … — all landing in
    M2.B.3 / M2.C / M2.E).  Target for M2 "done": suite compiles
    and every test passes.

**WAT-first prototyping infrastructure (new during M2)**

  - `doc/implementation-plan/rewrite/wat-traces.md` — walkthrough
    doc: for every planned codegen arm, the target wasm is
    written as WAT first, assembled with `wasm-as`, and locked
    against byte-identical output from `expr_lower` once
    implemented.  Covers literal, ident, two-idents, select,
    comprehension (M5 forward-look), array index (M6
    forward-look).
  - `doc/implementation-plan/rewrite/wat/*.wat` — five checked-in
    WAT files, all assembling.
  - `tools/wat_runner/` — harness that executes WAT
    end-to-end through wasmtime + the real
    `cel_runtime.wasm` (NOT mocked), with optional stub impls
    for `cel_host.cel_get_field` / `cel_has_field` and
    pre-populated memory.  Seven tests exercise the runtime's
    `cel_reset` + `cel_alloc` and prototype the four-arg
    cel_host ABI with UNKNOWN absorption — all before the real
    Layer-2/3 trampoline is written.
  - CLAUDE.md — new section **"WAT-first for ABI and codegen
    design"** making this workflow mandatory for every new
    codegen arm and host-ABI surface.  Cross-cuts future
    milestones (M3 arithmetic, M5 comprehensions, M6
    lists/maps).

### Plan-vs-execution deltas

> **Delta 1: `VariableDecl` → `VariableDeclaration`.**  Plan §5.1
> reference and `cel-host-surface.md` §2.1 show a public
> `cel::VariableDecl`.  cel-cpp's `common/decl.h` already defines
> `cel::VariableDecl` as a final class; using the same name in our
> header causes a silent ODR collision that crashes during
> standard-library declaration setup.  The v2 struct is renamed to
> `VariableDeclaration`; future docs should use the new name.

> **Delta 2: `local_types` dropped.**  Plan §5.1 / §6.1 declare
> `std::vector<BinaryenType> local_types` on both `ResolveOutput`
> and `StaticLayout`.  Every wasm local our codegen declares is
> uniformly i32 (a linear-memory offset).  The vector carried no
> information beyond `variables.size()`; emission-time expansion
> to `vector<BinaryenType>(N, BinaryenTypeInt32())` is one line at
> the `AddFunction` call site.  Dropped the field on both structs.

> **Delta 3: Engine::Plan wires cel_host imports (not just the
> plan's implication).**  Plan §5 Slice M2.C reads "Engine::Plan
> adds a prelude" (of wasm-side work); the actual Engine::Plan
> change is additive linker wiring (`RegisterCelHostImports`) at
> the C++ level, not a codegen change.  The "prelude" terminology
> is reserved for the ident-materialisation instructions at the
> top of `$eval`.

### Landed (2026-04-24)

High-level per-slice summary.  The milestone doc's detailed
close-out pass (as-shipped API walkthroughs, section-by-section
plan-vs-execution deltas, checklist ticks) is a follow-up; this
entry is a progress beacon so readers know the milestone is
code-complete.

  - **M2.A — `cel::Activation`.**  `api/activation.{h,cc}` +
    tests; `Bind` / `BindLazy` / `Find` with lazy-once
    memoisation.
  - **M2.B — `kIdent` lowering + `Instance::Eval(Activation)` +
    `cel.abi.variables[]`.**  Variable prelude emission,
    workspace slots, ABI section decode at `Engine::Plan`.
  - **M2.C.0a — Layer 1 `HostMessageBacking` / `ProtoBacking`.**
    Pure backing semantics, arena-allocator aware.
  - **M2.C.0b — Layer 2 `CelGetFieldImpl` / `CelHasFieldImpl`.**
    Runtime-agnostic trampoline body shared across get/has.
  - **M2.C.1 — `ResolvePass` preserves `field_number` +
    `attribute_id`.**  Dense per-variable `local_index` + interned
    attribute paths on ident + select chains.
  - **M2.C.2 — `LayoutPass` workspace slots for `kSelect`.**
    24-byte CelValue per select node at `workspace_base + n*24`.
  - **M2.C.3 — `expr_lower kSelect` arm.**  Emits
    `call $cel_host.cel_get_field` with operand + field-ref-id +
    attribute-id + out-slot argument ordering.
  - **M2.C.4 — `cel.abi.fields[]` serialisation.**  Dense
    sentinel-at-0 field-ref table; `BuildCelAbi` takes
    `absl::Span<const FieldRefRow>`.
  - **M2.C.5 — Layer 3 wasmtime glue.**  New
    `api/internal/cel_host_wasmtime.{h,cc}`; `CelHostCallbackEnv`
    on `InstanceImpl`; `Engine::Plan` reorders to
    decode-ABI → find-cel_alloc → build-bindings →
    register-imports → instantiate.
  - **M2.D — `has()` dispatch via `test_only`.**  Shared
    `LowerSelectOperand` helper between get/has; `has()` returns
    BOOL per `CelHasFieldImpl`.
  - **M2.E — `AttributePattern` + `Instance::PartialEval`.**
    `ResolvePass` interns `AttributeEntryRow` rows keyed by
    path; `cel.abi.attributes[]` serialised; trampoline
    consults `unknown_patterns` and returns
    `CelValue{kind:CEL_UNKNOWN, payload:attribute_id}` on match.
    `MarshalActivation` decodes unknowns back through
    `Value::Unknown(AttributeId)`.
  - **M2.F — Conformance harness envelope for unknowns.**
    `runner::IsM1Eligible` → `IsInM2Envelope`; envelope now
    admits `unknown` / `any_unknowns` matchers; `RunOne` routes
    them to `Instance::PartialEval`; new `CompareUnknown`.
    Headline unchanged from M1 snapshot
    (`total=2454 · pass=178 · skip=1935 · fail=341`) — the
    corpus has no tests using the unknown matchers, so the
    widening is structural only.  Harness-side `bindings:` →
    `cel::Activation` marshalling is a follow-up that would
    graduate ~100 tests in `fields` / `namespace` / `macros`.

**Bonus cleanups (off-plan, landed during M2.C–E):**

  - `abi_decode.h` refactored to return `celwasm::abi::CelAbi`
    directly — mirror structs (`DecodedCelAbi` / `DecodedVariable`
    / `DecodedField` / `by_name`) dropped; ~120 LOC deleted.
  - LE endianness static assertion in `runtime/cel_data.h` at
    the `sizeof(CelValue) == 24` site (wasm is always LE;
    host must match for the memcpy-based CelValue transfer).
  - `Compiler::Builder::RegisterMessageType` deleted — dead API,
    never read by any codepath.  `ProtoBacking` uses
    `msg->GetDescriptor()` directly.
  - Test-file sweep: `(void)Foo::descriptor()` →
    `google::protobuf::LinkMessageReflection<Foo>()` consolidated
    into file-scope `kDescriptorsLinked` static blocks across
    eight test files.

### Open slices

M2 is code-complete.  Detailed close-out (header status flip,
section-by-section doc reconciliation, `testing-checklist.md`
row ticks, `design.md` invariant verification) is the next
follow-up — see `§7. Exit criteria` for the full list.

## 0. Why one milestone, not three

M2 bundles three capabilities that might look separable:

  1. `kIdent` lowering + `cel::Activation` on the public API.
  2. `kSelect` lowering + `cel_host` trampolines for proto field reads.
  3. Unknown propagation + `Instance::PartialEval` + `AttributePattern`.

Bundling is correct because all three share the same new surfaces:

  - `Activation` is the natural home for idents (the bindings the
    expression reads) *and* for the name/pattern pairs that declare
    which attributes should evaluate as `UNKNOWN`.  Splitting
    idents from unknowns duplicates its design.
  - `cel_host.cel_get_field` is the sole dispatch site where the
    unknown-pattern set is consulted at runtime — if a select's
    rooted `AttributeId` matches an unknown pattern, the trampoline
    returns `CelValue{kind: CEL_UNKNOWN}` instead of the concrete
    field value.  Splitting selects from unknowns means shipping
    `cel_get_field` twice, once without the gate and once with.
  - `ResolvePass` writes `local_index` (for `kIdent`) and
    `field_number` (for `kSelect`) in the same walk; it also assigns
    the `AttributeId`s that `AttributePattern::Matches` consumes.
    Splitting the pass two ways leaves ResolvePass without a
    coherent contract.

M1 shipped the skeleton and proved literal-only codegen
end-to-end.  M2 is the first milestone that produces *non-trivial*
wasm — bodies that read bindings, traverse proto graphs, and
short-circuit on UNKNOWN.  It also graduates the conformance
harness's envelope filter from "scalar matcher only" to "scalar
or `unknown` matcher" (see
`conformance/README.md` — forecast unlocks ~100
tests, plus whatever fraction of the corpus uses `unknown:` /
`any_unknowns:` matchers).

## 1. Scope

### 1.1 What works end-to-end after M2

```
$ bazel run //tools/cel:celwasmc_v2 -- -e "x" \
    -V x:int -bind x=42
42

$ bazel run //tools/cel:celwasmc_v2 -- -e "c.name" \
    --schema e2e/testdata/customer.proto \
    -V c:celwasm.testdata.Customer -bind c=<serialised customer>
"Alice"

$ bazel run //tools/cel:celwasmc_v2 -- -e "has(c.order)" \
    ... -bind c=<customer with no order>
false

$ bazel run //tools/cel:celwasmc_v2 -- -e "c.billing_address.city" \
    ... -bind c=<customer>
"Seattle"

$ bazel run //tools/cel:celwasmc_v2 -- -e "c.name" \
    ... --unknown_attrs "c.name"
UNKNOWN(c.name)
```

Every one of these goes through:

```
parse → check → ResolvePass → LayoutPass → expr_lower → Binaryen
  → Engine::Plan → Instance::Eval(activation)       [concrete path]
  → Engine::Plan → Instance::PartialEval(activation, [patterns])
                                                    [unknown path]
```

### 1.2 Out of scope (deferred to M3+)

| Capability | Milestone |
|---|---|
| `kCall` (arithmetic, comparisons, string ops) | M3 |
| `size()` / `contains()` / `startsWith()` etc. | M3 |
| 3VL (`&&`/`||`/`!` with ERROR absorption) | M4 |
| `eval_error` / `any_eval_errors` matcher tests in conformance | M4 |
| `cel_message_eq` (proto-to-proto equality) | M4 |
| Custom functions | M5 |
| Comprehension macros (`exists`/`all`/`map`/`filter`) | M5 |
| List + map literals | M6 |
| Proto message construction (`Customer{name: …}`) | M7 |

M2 deliberately stops short of `kCall` — the overload table seed
stays empty, and any expression that compiles today beyond a
scalar literal fails at M1 with `Unimplemented`.  That boundary
holds for M2 + 1 node kind (`kSelect`) + `has()` (which cel-cpp's
macro expander rewrites to an explicit `Select(..., test_only=true)`
node, not to a `kCall`).

## 2. Surfaces introduced in M2 (long-lasting shapes)

Like the M1 skeleton surfaces, these are **frozen at M2** —
subsequent milestones populate their internals but do not reshape
them.

### 2.1 `api/activation.{h,cc}` — `cel::Activation`

Per `cel-host-surface.md` §2.6.  Signature-final:

```cpp
namespace cel {

class Activation {
 public:
  Activation();
  ~Activation();

  // Direct binding — copies the Value in by value.
  void Bind(std::string name, Value value);

  // Lazy binding — fn is called only if the expression references
  // the variable.  fn may return InvalidArgument (convert the
  // host value failed), propagated to Eval's StatusOr.
  void BindLazy(std::string name,
                absl::AnyInvocable<absl::StatusOr<Value>() const> fn);

  // Lookup — used by Instance at eval start.  Const-ref to stored
  // Value (or fresh Value materialised from BindLazy).
  absl::StatusOr<const Value*> Find(absl::string_view name) const;

 private:
  // pImpl; internals in api/internal/activation_impl.{h,cc}.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cel
```

`OverrideFunction` (per-call impl override from
`cel-host-surface.md`) lands with customs in M5; M2 leaves it off
the Activation API entirely rather than shipping a stubbed arm.

#### 2.1.1 `Value::HostMessage` / `HostMap` / `HostList` — backing-polymorphic constructors

To support non-proto backings (JSON, XML, struct-of-structs) the
`cel::Value` surface grows three new builders in `api/value.h`.
Only `HostMessage` is exercised at M2; `HostMap` and `HostList`
are signature-final stubs reserving the shape for M6 when
map/list support ships — see `map-list-dispatch.md` for the
full map/list dispatch design.

```cpp
class Value {
 public:
  // … existing builders …

  // M2 — fully functional.  Carries a caller-supplied backing
  // through Activation::Bind to cel_host's read path.  The
  // built-in `ProtoBacking` covers the proto case
  // (`Value::Message(msg)` forwards to this under the hood);
  // embedders with non-proto data shapes provide their own
  // HostMessageBacking subclass.  See §2.4.1 for the interface
  // and `api/internal/cel_host.h` for `ProtoBacking`.
  static Value HostMessage(std::shared_ptr<HostMessageBacking>);

  // M2 — signature-final stub.  Body `ABSL_CHECK(false) << "M6"`.
  // Shipped at M2 so callers wiring up JSON-map backings can
  // see the target shape without waiting for M6.  See
  // `map-list-dispatch.md §4.3` and `§9` for the interface
  // + milestone fit.
  static Value HostMap(std::shared_ptr<HostMapBacking>);

  // M2 — signature-final stub.  Body `ABSL_CHECK(false) << "M6"`.
  static Value HostList(std::shared_ptr<HostListBacking>);
};
```

The existing `Value::Message(const google::protobuf::Message&)`
builder (already on the M1 api surface as a signature-final stub)
is unstubbed in M2 to wrap in a fresh `ProtoBacking` and forward
to `HostMessage`.  Callers with a proto keep using `Message`;
callers with JSON / XML / anything else use `HostMessage`
directly with their own backing.  `Value::Map` / `Value::List`
(also signature-final stubs on the M1 surface) remain stubbed
at M2 — their bodies land in M6.

### 2.2 `api/instance.h` — `PartialEval` second entry point

Two `Instance` methods:

```cpp
absl::StatusOr<Value> Eval(const Activation& act);
absl::StatusOr<Value> PartialEval(
    const Activation& act,
    absl::Span<const AttributePattern> unknowns);
```

`Eval` is the happy path — every declared variable has a concrete
`Bind` / `BindLazy`; unbound → `FailedPrecondition`.
`PartialEval` additionally accepts a list of attribute patterns;
any select/ident whose resolved `AttributeId` matches a pattern
short-circuits to `Value::Unknown(AttributeId)` at runtime.

The conformance harness distinguishes the two in
`runner.cc::RunOne` by checking the test's `result_matcher`:
`value:` → `Eval`, `unknown:` / `any_unknowns:` → `PartialEval`
with the test's declared unknown attributes as patterns.

### 2.3 `api/attribute.h` — `AttributePattern`

Already exists (shipped in M1 for `Value::Unknown(AttributeId)`
carrying).  M2 adds:

```cpp
namespace cel {

class AttributePattern {
 public:
  // Parse a dotted-path pattern: "c.name", "c.order.*", etc.
  // `*` matches any qualifier at that position.
  static absl::StatusOr<AttributePattern> Parse(absl::string_view);

  // Matches against a resolver-produced AttributeId.
  bool Matches(AttributeId attr) const;

 private:
  // Segments: each is either a concrete name (string view into the
  // owned buffer) or the wildcard sentinel.  ~2 segments typical.
  std::string buffer_;
  std::vector<Segment> segments_;
};

}  // namespace cel
```

Parse is the only new entry point — the rest is consumed by the
runtime dispatcher.

### 2.4 `api/internal/cel_host.{h,cc}` — three-layer host adapter

The single most critical M2 interface.  Shipped as a
**three-layer split** so the middle layer (the trampoline
semantics) is pure-C++ testable without wasmtime — see §6.1.1
for the smoke test harness that exercises layers 1 + 2 + the
ABI round-trip in one shot.

Path: `eval/internal/cel_host.{h,cc,_test.cc}`.
Namespace `celwasm`; header guard
`CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_H_`.  Public
consumer: `Engine::Plan` (the only call site that registers
the imports on the wasmtime linker).

```
┌───────────────────────────────────────────────────────────┐
│  Layer 3: wasmtime adapter — thin glue, M2 production    │
│    RegisterCelHostImports(linker, &bindings)              │
│    ↳ wraps wasmtime_memory_t / wasmtime_func_t behind     │
│      the Layer-2 abstractions, forwards i32 args          │
├───────────────────────────────────────────────────────────┤
│  Layer 2: pure trampoline semantics — runtime-agnostic    │
│    CelGetFieldImpl(out_slot, msg_slot, field_ref_id,      │
│                    attribute_id, bindings, mem&,          │
│                    refs&, alloc&)                         │
│    CelHasFieldImpl(...)                                   │
│    ↳ absorbs UNKNOWN/ERROR, checks unknown patterns,      │
│      dispatches into Layer 1, marshals out slot           │
├───────────────────────────────────────────────────────────┤
│  Layer 1: pure CEL semantics — per-backing                │
│    HostMessageBacking (interface)                         │
│      ↳ ProtoBacking      (built-in, proto messages)       │
│      ↳ embedder impls    (JSON, XML, custom — §2.4.1)     │
│    ReadField(msg, field_number, field_name, …)            │
│    HasField(msg, field_number, field_name)                │
└───────────────────────────────────────────────────────────┘
```

Each layer has one reason to change: Layer 1 when CEL semantics
change (new proto cpp_type, new well-known-type rule); Layer 2
when the wasm ABI changes (new arg, new absorption rule); Layer
3 when wasmtime's API changes.  No mixing.

#### 2.4.1 Layer 1 interfaces

```cpp
// Polymorphism hook — the answer to "how do I plug in JSON /
// XML / struct-of-structs as a CEL message backing?"  One
// shipping impl at M2 (ProtoBacking).  Embedders subclass to
// add their own backings; Activation::Bind(Value::HostMessage
// (backing)) carries it through to eval.
class HostMessageBacking {
 public:
  virtual ~HostMessageBacking() = default;

  // Read one field.  `field_number` is 0 for non-proto-backed
  // backings (name-only resolution); always non-zero for
  // ProtoBacking.  `field_name` is always populated.
  // `expected_type` is what the checker said the field should
  // be — backings consult it to pick the return shape (e.g.
  // return Value::HostMessage(sub_backing) for a MESSAGE-typed
  // field, Value::String for a STRING-typed field).
  virtual absl::StatusOr<Value> ReadField(
      int field_number,
      absl::string_view field_name,
      const CelType& expected_type) = 0;

  // Returns has(msg.field) per langdef.md's proto2/proto3 rules
  // for the native backing.  Embedders decide what "has" means
  // for non-proto backings (typical: "the key exists in the
  // underlying tree and its value is not null-equivalent").
  virtual bool HasField(int field_number,
                        absl::string_view field_name) = 0;
};

// Built-in proto adapter.  ~200 LOC, transcribed from v1
// cel_host.cc's ReadField / HasField free-function bodies.
// At M2, ReadField on a MAP / REPEATED field returns
// Value::Error(CEL_ERR_TYPE_UNSUPPORTED) — the envelope
// boundary.  M6 flips these to Value::HostMap(ProtoMapBacking)
// / Value::HostList(ProtoRepeatedBacking) per
// `map-list-dispatch.md §9`.
class ProtoBacking : public HostMessageBacking {
 public:
  explicit ProtoBacking(const google::protobuf::Message* msg)
      : msg_(msg) {}
  absl::StatusOr<Value> ReadField(int, absl::string_view,
                                  const CelType&) override;
  bool HasField(int, absl::string_view) override;

 private:
  const google::protobuf::Message* msg_;  // non-owning
};

// ————— Map / List backings (signature-final stubs at M2) ————
// Bodies `ABSL_CHECK(false) << "M6"` per the CLAUDE.md
// unimplemented-features rule.  Declared here so M6 is an
// additive slice — the api surface, the Value::HostMap /
// HostList builders (§2.1.1), and the Layer-2 trampoline
// signatures (§2.4.2) are all frozen now.  Full design in
// `map-list-dispatch.md`.

class HostMapBacking {
 public:
  virtual ~HostMapBacking() = default;
  virtual size_t Size() const = 0;
  virtual absl::StatusOr<Value> Get(
      const Value& key,
      const CelType& expected_value_type) = 0;
  virtual bool ContainsKey(const Value& key) const = 0;
  virtual void ForEach(
      absl::FunctionRef<void(const Value&, const Value&)>) const = 0;
};

class HostListBacking {
 public:
  virtual ~HostListBacking() = default;
  virtual size_t Size() const = 0;
  virtual absl::StatusOr<Value> At(
      size_t index,
      const CelType& expected_element_type) = 0;
  virtual void ForEach(
      absl::FunctionRef<void(const Value&)>) const = 0;
};
```

`Value::HostMessage(std::shared_ptr<HostMessageBacking>)` is the
constructor on the public `cel::Value` (api/value.h) that carries
a backing through `Activation::Bind`.  Added in §2.1-adjacent
extension.

#### 2.4.2 Layer 2 interfaces

```cpp
// ————— Abstracted primitives Layer 2 depends on ——————————

// Read/write a CelValue (24B) at a linear-memory offset.
// Implemented twice: real wasmtime-backed adapter (prod) +
// std::vector<uint8_t>-backed fake (test).
class MemoryView {
 public:
  virtual ~MemoryView() = default;
  virtual CelValue ReadCelValue(uint32_t offset) const = 0;
  virtual void WriteCelValue(uint32_t offset,
                             const CelValue& v) = 0;
  // Span payloads dereference through the same memory.
  virtual absl::string_view ReadSpan(uint32_t ptr,
                                     uint32_t len) const = 0;
};

// Lookup externref-stored backings by densely-assigned slot.
// cel_refs in prod; std::vector<shared_ptr<HostMessageBacking>>
// in test.  Intern is only needed by the return-value marshal
// step (Layer 2 writes a CelValue{CEL_MESSAGE, msg_slot=K}
// after ReadField returns a HostMessage).
class ExternrefTable {
 public:
  virtual ~ExternrefTable() = default;
  virtual uint32_t Intern(
      std::shared_ptr<HostMessageBacking> backing) = 0;
  virtual HostMessageBacking* Lookup(uint32_t slot) const = 0;
  virtual void Reset() = 0;   // cleared on each cel_reset
};

// Variable-length payload allocator (strings, bytes).  Routes
// to cel_alloc in prod; bump-pointer within a vector in test.
class ArenaAllocator {
 public:
  virtual ~ArenaAllocator() = default;
  // Allocates len bytes, returns host-addressable pointer + the
  // wasm-side offset the CelSpan should carry.  nullptr on OOM.
  virtual uint8_t* Alloc(size_t len,
                         uint32_t* out_offset) = 0;
};

// ————— Layer-2 pure trampoline functions ——————————————————

// §4.7.6.4 body — runtime-agnostic.  Calls Layer 1 only through
// HostMessageBacking; calls Layer 2 abstractions for memory /
// refs / alloc.  No wasmtime, no direct proto calls.
void CelGetFieldImpl(uint32_t out_slot, uint32_t msg_slot,
                     uint32_t field_ref_id,
                     uint32_t attribute_id,
                     const CelHostBindings& bindings,
                     MemoryView& mem,
                     ExternrefTable& refs,
                     ArenaAllocator& alloc);

void CelHasFieldImpl(uint32_t out_slot, uint32_t msg_slot,
                     uint32_t field_ref_id,
                     uint32_t attribute_id,
                     const CelHostBindings& bindings,
                     MemoryView& mem,
                     ExternrefTable& refs,
                     ArenaAllocator& alloc);

// Per-Plan state read by the Layer-2 impls.  Owned by
// InstanceImpl; borrowed as callback data.
struct FieldRefEntry {
  uint32_t field_number;      // 0 = "not proto-resolvable"
  std::string field_name;     // always populated
};

struct AttributeEntry {
  std::string root_variable;              // "" = non-ident-rooted
  std::vector<std::string> qualifiers;    // e.g. ["billing_address", "city"]
};

struct CelHostBindings {
  absl::Span<const FieldRefEntry> field_refs;     // from cel.abi.fields[]
  absl::Span<const AttributeEntry> attributes;    // from cel.abi.attributes[]
  absl::Span<const AttributePattern> unknown_patterns;  // PartialEval only
};
```

#### 2.4.3 Layer 3 interface

```cpp
// Wires Layer 2 onto wasmtime.  Unwraps the 4 i32 args from
// the wasmtime_caller_t, constructs real MemoryView /
// ExternrefTable / ArenaAllocator impls backed by the store's
// memory + the runtime module's cel_alloc + the expr module's
// cel_refs, and calls CelGetFieldImpl / CelHasFieldImpl.
ABSL_MUST_USE_RESULT absl::Status RegisterCelHostImports(
    wasmtime_linker_t* absl_nonnull linker,
    CelHostBindings* absl_nonnull bindings,   // borrowed; outlives linker
    wasmtime_context_t* absl_nonnull ctx,
    wasmtime_memory_t memory,
    wasmtime_func_t cel_alloc,
    wasmtime_func_t cel_ref_intern);
```

#### 2.4.4 Wire ABI (canonical)

| Import | wasm signature |
|---|---|
| `cel_host.cel_get_field` | `(out_slot i32, msg_slot i32, field_ref_id i32, attribute_id i32) -> ()` |
| `cel_host.cel_has_field` | `(out_slot i32, msg_slot i32, field_ref_id i32, attribute_id i32) -> ()` |

Four i32s, not three — the `attribute_id` is needed for the
unknown-pattern check.  See `design.md §4.7.1` for the full
ABI table and `§4.7.6` for the trampoline body pseudocode.

#### 2.4.5 ABI decoder

```cpp
// api/internal/abi_decode.h
// Decodes a cel.abi.CelAbi proto (read from the wasm module's
// custom section) into the runtime lookup tables the Layer-2
// trampolines consume.  Separate from cel_host.h so the smoke
// test can exercise the round-trip cel.abi.CelAbi → wire bytes
// → CelAbi → CelHostBindings in pure C++ — see §6.1.1.
ABSL_MUST_USE_RESULT absl::StatusOr<CelHostBindings>
BuildBindingsFromAbi(
    const celwasm::abi::CelAbi& abi,
    const google::protobuf::DescriptorPool& pool,
    // Storage for the decoded vectors.  Caller owns; spans in
    // the returned CelHostBindings borrow from it.
    std::vector<FieldRefEntry>* absl_nonnull field_refs_storage,
    std::vector<AttributeEntry>* absl_nonnull attributes_storage);
```

### 2.5 Runtime wire-through — `cel_unknown_merge`

Already exists in `runtime/cel_runtime.{h,c}` from v1 M4 Slice A.
M2 adds no new runtime helpers.  What M2 *does* is wire
`cel_unknown_merge` onto the expr-module side: every downstream
op that consumes a `CelValue` must call `cel_unknown_merge` on
its inputs before doing work, so `UNKNOWN` propagates correctly
through the partially-lowered expression.

At M2 this only affects the `kSelect` chain (`x.a.b.c` where any
hop is unknown absorbs into UNKNOWN) — `kCall` lands in M3 and is
where the bulk of unknown-merge sites appear.  M2 documents the
contract; M3 exercises it broadly.

### 2.6 `NodeAnnotation` population

Two new fields this milestone: `map_origin` and `list_origin`
(both `Origin` enum — `kArena` / `kHost` / `kDynamic`, default
`kDynamic`).  The rest are fields M1 reserved and M2 fills in.

  - `local_index` — written by `ResolvePass` for every `kIdent`
    node.  The parameter binding is installed at `$eval` entry by
    codegen (`BinaryenLocalSet(local_index, <offset into
    Activation-materialised arena>)`).
  - `field_number` — written for every `kSelect` node.  Ported
    from v1 M3 G2's ResolvePass logic.
  - `attribute_id` — already declared on the M1 skeleton; M2
    populates for `kIdent` + `kSelect` chains so
    `AttributePattern::Matches` has something to consult.
  - `map_origin` — new this milestone.  M2 only populates on
    `kSelect` nodes whose `result_type.kind == MAP` (value came
    from a proto map field read → always `kHost`) and on
    `kIdent` nodes whose declared type is `map<K,V>` (value
    came from `Activation::Bind` via the prelude → always
    `kHost`).  Every other map-producing kind
    (`kCreateMap`, `kComprehension`, `kCall` returning map,
    mixed-origin `?:`) lands in M5/M6; ResolvePass leaves
    `map_origin = kDynamic` on those until the milestone that
    ships them.  See `map-list-dispatch.md §2` for the full
    inference rule set.
  - `list_origin` — new this milestone; same rule — only
    `kSelect` / `kIdent` of list type get `kHost` at M2, rest
    stay `kDynamic` until their kind ships.

M2 doesn't emit any map/list ops (the envelope boundary is that
`ProtoBacking::ReadField` returns `CEL_ERROR` for MAP/REPEATED
fields), so populating `map_origin` / `list_origin` at M2 is
forward-looking — it lets M6 flip on without touching
ResolvePass for the common cases.

### 2.7 `cel.abi` custom-section growth

M1 ships the section empty.  M2 adds:

```
variables[]: one entry per declared variable:
  { name: string, type: CelType, local_index: u32 }

fields[]:   one entry per unique (message_type, field_number) pair
            referenced in the expression:
  { type_id: u32, field_number: u32, field_name: string,
    result_type: CelType, field_ref_id: u32 }

attributes[]: one entry per unique AttributeId produced by
              ResolvePass:
  { attribute_id: u32, root_variable: string, qualifiers: [string] }
```

All three are consumed by `Engine::Plan` to build per-Instance
lookup tables (`FieldRefEntry[]`, the attribute-pattern matcher's
target set).

### 2.8 Forward-compat hooks for maps and lists (M6 preview)

M2 does not ship map or list evaluation — the envelope boundary
is that any `kSelect` returning a map / repeated field lowers
normally (via `cel_host.cel_get_field`) but `ProtoBacking::Read
Field` writes a `Value::Error(CEL_ERR_TYPE_UNSUPPORTED)` until
M6 fills the body in.  What *does* land in M2 is the surface
shape so M6 is additive:

| Surface | M2 shape | M6 body |
|---|---|---|
| `HostMapBacking` / `HostListBacking` (api/internal/cel_host.h) | Declared interfaces; no built-in impl yet. | `ProtoMapBacking` + `ProtoRepeatedBacking` implementations. |
| `Value::HostMap` / `Value::HostList` (api/value.h) | Signature-final stubs; body `ABSL_CHECK(false) << "M6"`. | Bodies filled in; `Value::Map` / `Value::List` also unstubbed. |
| `NodeAnnotation.map_origin` / `list_origin` | Enum field reserved; populated only on `kSelect` / `kIdent` of map/list type (always `kHost`). | Populated on `kCreateMap` / `kCreateList` / `kComprehension` as `kArena`; mixed-branch ternary as `kDynamic`. |
| `CelKind` enum (runtime/cel_data.h) | Add `CEL_MAP_ARENA` / `CEL_MAP_HOST` / `CEL_LIST_ARENA` / `CEL_LIST_HOST` placeholders (runtime never produces them at M2, but the codec paths are named). | Runtime + codegen + cel_host produce and consume them. |

Everything in this subsection tracks the design in
`map-list-dispatch.md` — that doc is the source of truth for
why these particular shapes; this subsection enumerates only
what M2 actually lands.  Reconciliation into `design.md` happens
alongside M5/M6 planning.

## 3. Source layout (M2 deliverables)

New / extended files under `compiler/`:

```
compiler/
├── api/
│   ├── value.{h,cc}                            # +Value::HostMessage (§2.1.1) — functional
│   │                                           # +Value::HostMap / HostList stubs (§2.1.1) — M6 bodies
│   │                                           #  unstub Value::Message → ProtoBacking
│   ├── activation.{h,cc,_test.cc}              # §2.1 — NEW
│   ├── attribute.{h,cc,_test.cc}               # M1; §2.3 adds Parse/Matches
│   ├── instance.h                              # +PartialEval (§2.2)
│   ├── instance.cc                             # +PartialEval impl
│   └── internal/
│       ├── activation_impl.{h,cc}              # pImpl payload — NEW
│       ├── cel_host.{h,cc,_test.cc}            # §2.4 three-layer split — NEW
│       │                                       #   • HostMessageBacking + ProtoBacking (functional)
│       │                                       #   • HostMapBacking / HostListBacking (§2.4.1 stubs)
│       │                                       #   • MemoryView / ExternrefTable /
│       │                                       #     ArenaAllocator (abstractions)
│       │                                       #   • CelGetFieldImpl / CelHasFieldImpl
│       │                                       #     (pure Layer 2)
│       │                                       #   • RegisterCelHostImports (Layer 3)
│       └── abi_decode.{h,cc,_test.cc}          # §2.4.5 BuildBindingsFromAbi — NEW
├── ir/
│   └── annotations.h                           # +Origin enum; +map_origin / list_origin
│                                               #  fields on NodeAnnotation (§2.6, §2.8)
├── codegen/
│   ├── resolve_pass.cc                         # +local_index / field_number / attribute_id
│   │                                           # +map_origin / list_origin on kSelect / kIdent of
│   │                                           #  map/list type (always kHost at M2 — §2.6)
│   ├── expr_lower.cc                           # +kIdent / kSelect / has() arms
│   └── module.cc                               # +cel.abi.{variables,fields,attributes}[]
├── runtime/
│   └── cel_data.h                              # +CEL_MAP_ARENA / CEL_MAP_HOST /
│                                               #  CEL_LIST_ARENA / CEL_LIST_HOST kind enums
│                                               #  (placeholders — runtime produces none at M2)
├── e2e/
│   └── eval_test.cc                            # +ident / select / has / unknown cases
└── conformance/
    └── runner.cc                               # +PartialEval dispatch for unknown matchers
```

**Test fixtures are reused in place, not copied.** Both
`//compiler/testdata:e2e_fixture_cc_proto` (`Customer` + `Address`)
and `//compiler/testdata:host_fixture_proto3_cc_proto` /
`host_fixture_proto2_cc_proto` (`HostMsg3` / `HostMsg2`) already
carry `//compiler_v2:__subpackages__` visibility — see
`compiler/testdata/BUILD.bazel`.  M2's `cel_host_test`,
`abi_decode_test`, and `eval_test` depend on them directly; no
`e2e/testdata/` directory is created.  Rationale:
one proto schema, two milestones — fewer places for the
fixture to drift out of sync.

No deletions at M2.  No files move.  The api/host split
(cel_host at api/internal/ rather than host/) is established here
for the first time — future M7 proto-literal work (`cel_make_message`
/ `cel_set_field`) extends the same cel_host file.

## 4. What gets ported verbatim from v1

  - `compiler/host/cel_host.cc::ReadField` + `HasField` +
    `ReadNumericField` + `WriteSpanPayload` — the per-cpp_type
    switch, span-allocate-copy, and HasField presence rules.
    Transcribed into `eval/internal/cel_host.cc` as
    the body of the new `ProtoBacking` class (§2.4.1); the
    logic doesn't change, only the free-function-vs-method
    shape does.
  - `compiler/codegen/expr_lower.cc::LowerSelect` +
    `LoadSelectPayload` — the select-chain lowering.  Transcribed
    into v2's `expr_lower.cc`; adjusted to use
    `NodeAnnotation::field_number` from `ResolvePass` rather than
    the in-line resolution v1 did.
  - `compiler/codegen/attribute_pool.cc` — the `AttributeId`
    interning logic.  Moves to `compiler/codegen/resolve_pass.cc`
    as the per-node `attribute_id` assignment site.
  - `runtime/cel_runtime.{h,c}` already has `cel_unknown_merge` from
    v1 M4 Slice A; no changes to the runtime source file.
  - **Fixtures reused, not ported:** `//compiler/testdata:e2e_fixture_cc_proto`
    + `:host_fixture_proto{2,3}_cc_proto`.  These are already
    `//compiler_v2:__subpackages__`-visible; M2 targets depend on
    them directly.  No file copy.

## 5. Work breakdown (order of authoring)

Each slice should ship as one squashable commit.  Slices within
M2 depend on each other strictly; later slices assume earlier
ones landed.

1. **Slice M2.A — `Activation` surface.** `api/activation.{h,cc}`
   lands with `Bind` / `BindLazy` / `Find`.  `Instance::Eval(const
   Activation&)` added but routes through the M1 eval path
   (activation ignored; no idents compile yet).  Tests assert
   round-trip `Bind(name, value)` + `Find(name)` on every scalar
   kind.

2. **Slice M2.B — `kIdent` lowering.** `ResolvePass` populates
   `local_index`.  `expr_lower.cc` grows `kIdent` arm: emit
   `BinaryenLocalGet(local_index, i32)`.  `Engine::Plan` adds a
   prelude to `$eval` that materialises each declared variable
   from the Activation into a workspace slot and sets the
   corresponding local.  E2E: `-e "x" -V x:int -bind x=42` → `42`.

3. **Slice M2.C.0 — `cel_host` interface + pure-C++ smoke test.**
   The critical de-risking slice.  Lands the three-layer split
   and the ABI round-trip *before* any wasmtime or codegen work
   touches it, so the interface is locked against both proto and
   non-proto backings by a running test.
     - `api/internal/cel_host.h` — `HostMessageBacking`
       (interface, §2.4.1), `ProtoBacking` (built-in),
       `MemoryView` / `ExternrefTable` / `ArenaAllocator`
       (Layer-2 abstractions, §2.4.2), `CelGetFieldImpl` /
       `CelHasFieldImpl` (pure Layer 2), `CelHostBindings` +
       `FieldRefEntry` + `AttributeEntry` data types.
     - `api/internal/cel_host.cc` — `ProtoBacking` body
       (transcribed from v1 `ReadField`/`HasField` bodies) +
       Layer-2 impls (absorption, unknown-pattern check,
       marshal).  **No Layer 3 yet** — `RegisterCelHostImports`
       is a signature-final stub whose body `ABSL_CHECK(false)
       << "M2.C"`.
     - `api/internal/abi_decode.{h,cc}` — `BuildBindingsFromAbi`
       (§2.4.5) turns `cel.abi.CelAbi` into `CelHostBindings`
       at Plan time.  Shipped ahead of Engine::Plan picking it
       up in M2.C.
     - `api/internal/cel_host_test.cc` — the smoke test harness:
       fake `MemoryView` / `ExternrefTable` / `ArenaAllocator`
       backed by `std::vector<uint8_t>` / vector of
       `shared_ptr<HostMessageBacking>` / bump allocator.
       Runs the four-stage pipeline (forge `CelAbi` → serialise
       → decode → `BuildBindingsFromAbi` → dispatch via
       `CelGetFieldImpl`) against both `HostMsg3` (proto) and a
       test-only `JsonBacking` with the same logical shape.
       Deps: `//compiler/testdata:host_fixture_proto{2,3}_cc_proto`
       + `:e2e_fixture_cc_proto`.
   **E2E check.** None yet — this slice ships no wasm.  The
   smoke test is the check.  Every Layer-2 behaviour listed in
   §6.1.1 runs green.
   **Tests.** The smoke-test file IS the test; see §6.1.1.
   **Risk.** Simulation divergence from real wasmtime semantics.
   **Mitigation.** Keep fakes minimal (byte-addressed vector,
   externref-as-shared_ptr-vector); the one cross-check test in
   M2.C confirms the real wasmtime adapter produces byte-
   identical output on the same inputs.
   **Effort.** 1.5 days.

4. **Slice M2.C — `kSelect` + wasmtime Layer 3 + end-to-end
   field reads.**  Adds the wasmtime adapter on top of the
   already-tested Layer 2, plus the codegen arm.
     - `api/internal/cel_host.cc` — fills in
       `RegisterCelHostImports` (Layer 3, §2.4.3).  Thin glue:
       unwrap the 4 i32 args, construct real `MemoryView` /
       `ExternrefTable` / `ArenaAllocator` impls against the
       wasmtime store, call `CelGetFieldImpl` /
       `CelHasFieldImpl`.
     - `eval/engine.cc` — wires
       `BuildBindingsFromAbi` + `RegisterCelHostImports` into
       `Engine::Plan` before instantiating the expr module.
     - `ResolvePass` populates `field_number`.
     - `expr_lower.cc` grows `kSelect` arm (non-`test_only`):
       emit `call $cel_host.cel_get_field(out_slot, msg_slot,
       field_ref_id, attribute_id)` — four args, uniform slot-
       out.
     - Module emission adds `cel.abi.fields[]` +
       `cel.abi.attributes[]`.
     - One cross-check test: the real wasmtime-adapter
       dispatch produces the same `CelValue` at `out_slot` as
       the fake-harness dispatch from Slice M2.C.0 on the same
       inputs.
   **E2E check.** `-e "c.name" --schema e2e_fixture.proto
   -bind c=<Customer>` → the name string.

4. **Slice M2.D — `has(msg.field)`.**  `expr_lower.cc` `kSelect`
   arm dispatches on `test_only` — reads route to
   `cel_get_field`, tests route to `cel_has_field`.  Both
   trampolines live in the same cel_host TU.  E2E: `has(c.order)`
   round-trip.

5. **Slice M2.E — `AttributePattern` + `Instance::PartialEval`.**
   `api/attribute.h` grows `Parse` + `Matches`.  `ResolvePass`
   populates `attribute_id` on `kIdent` + `kSelect` nodes.
   `cel_host.cel_get_field` / `cel_has_field` consult the
   unknown-pattern set and write `CelValue{kind:CEL_UNKNOWN}` on
   match.  `Instance::PartialEval(activation, patterns)` lands.
   Module emission adds `cel.abi.attributes[]`.  E2E:
   `--unknown_attrs "c.name"` produces `UNKNOWN(c.name)`.

6. **Slice M2.F — conformance harness envelope update.**
   `conformance/runner.cc::IsM1Eligible` renamed or
   generalised; the envelope filter accepts `unknown` /
   `any_unknowns` matchers.  `RunOne` routes to `PartialEval`
   when the matcher is unknown-shaped.  Re-run
   `run_conformance`; inventory table in `conformance/README.md`
   updated.

## 6. Test plan

### 6.1 Unit tests (what each file's `_test.cc` covers)

  - `api/activation_test.cc` — `Bind` / `BindLazy` round-trip per
    scalar kind; `Find` on unbound → `NotFoundError`; `BindLazy`
    fn runs at most once per `Eval` call (lazy memoisation).
  - `api/value_test.cc` (extended) — `Value::HostMessage(backing)`
    round-trip; `Value::Message(proto)` forwards to a fresh
    `ProtoBacking`; `kind()` reports `kMessage` for both.
    `Value::HostMap(backing)` and `Value::HostList(backing)`
    assertion test — calling either aborts with the M6
    `ABSL_CHECK` message (per CLAUDE.md unimplemented-features
    rule); `EXPECT_DEATH` locks the envelope boundary.
  - `codegen/annotations_test.cc` (extended) — `Origin` enum
    round-trip through `NodeAnnotation`; default value is
    `kDynamic`; `map_origin` / `list_origin` zero-sentinel
    stable on nodes where it's irrelevant.
  - `api/attribute_test.cc` (extended) — `Parse` on
    `"a.b.c"`, `"a.*.c"`, `"*"`; `Matches` covers every wildcard
    position; parse errors on leading dot / empty / trailing dot.
  - `api/internal/cel_host_test.cc` — see §6.1.1 (biggest surface
    — the smoke test harness lives here and does most of the
    Layer-1 / Layer-2 work).
  - `api/internal/abi_decode_test.cc` — `CelAbi` wire-round-trip
    on a hand-built fixture; descriptor resolution against two
    distinct pools; sentinel-id handling (`field_ref_id=0`,
    `attribute_id=0`); malformed input → `InvalidArgument`.
  - `codegen/resolve_pass_test.cc` (extended) —
    `local_index` assigned densely per `kIdent`; `field_number`
    matches cel-cpp's `reference_map[id]`; `attribute_id`
    assigned uniquely per rooted path (ident + select chain).
    `map_origin = kHost` on `kSelect` whose `result_type.kind
    == MAP` and on `kIdent` whose declared type is `map<K,V>`;
    `kDynamic` everywhere else at M2 (locks the forward-compat
    hook from §2.8 — M6 flips other kinds to `kArena`).
    Mirror for `list_origin`.
  - `codegen/expr_lower_test.cc` (extended) — `kIdent` emits
    `local.get`; `kSelect` emits `call $cel_host.cel_get_field`
    with correct operand ordering; `has()` dispatches on
    `test_only`.

#### 6.1.1 `cel_host` smoke test (Slice M2.C.0)

The lock on the `cel_host` interface.  Runs entirely in C++, no
wasmtime, no wasm — fakes stand in for `wasmtime_memory_t`,
`cel_refs`, and the runtime's `cel_alloc`.  Every call path a
real expression module would take gets exercised against the
same `CelGetFieldImpl` / `CelHasFieldImpl` that Slice M2.C's
wasmtime adapter dispatches into, so "works under the smoke
test" buys "works in the real ABI" up to the Layer-3 glue.

**Pipeline** — four stages, all in one test:

```
 (1) forge           (2) serialize        (3) decode               (4) dispatch
┌───────────────┐  ┌──────────────┐   ┌─────────────────────┐  ┌───────────────────┐
│ hand-build a  │  │ proto wire   │   │ CelAbi::ParseFrom → │  │ CelGetFieldImpl   │
│ celwasm::abi::│─▶│ bytes        │──▶│ BuildBindingsFromAbi│─▶│ against FakeMem / │
│ CelAbi        │  │              │   │ → CelHostBindings    │  │ FakeRefs / FakeArena│
└───────────────┘  └──────────────┘   └─────────────────────┘  └───────────────────┘
     fake             real             real M2 production          Layer 2, real
```

Stages 2–4 are real M2 production code.  Stage 1 is
hand-constructed until codegen lands; once M2.C ships, the
same harness swaps stage 1 for a call to the real emitter —
the fakes in stages 3–4 retire only after the cross-check
test in M2.C confirms byte-identical output against wasmtime.

**Coverage matrix.**

| Scenario | Fixture | What it locks |
|---|---|---|
| `ReadField` × every proto3 cpp_type | `HostMsg3` (fields 1–18) | Each branch in `ProtoBacking::ReadField`'s switch produces the correct `CelValue.kind` + payload shape — bool/int32/int64/uint32/uint64/sint/fixed/float/double/string/bytes/enum/message. Non-contiguous field numbers catch `field_ref_id` → `field_number` off-by-ones. |
| `ReadField` × repeated / map — **envelope boundary** | `HostMsg3.rep_i32` (repeated) + a proto fixture map field (add to `host_fixture_proto3.proto` if none present) | Result is `CelValue{CEL_ERROR, err=CEL_ERR_TYPE_UNSUPPORTED}`.  This is the M2→M6 graduation contract: M6 changes the same row to expect `CelValue{CEL_LIST_HOST, ref_slot=…}` / `CelValue{CEL_MAP_HOST, ref_slot=…}` and removes the `ERR_TYPE_UNSUPPORTED` expectation. |
| `ReadField` × unresolvable name + number | `HostMsg3` | Unknown field → `CEL_ERROR`, not a descriptor trap. |
| `HasField` proto3 implicit-presence | `HostMsg3` | Singular scalar default vs set; singular message unset vs set. |
| `HasField` proto2 explicit-presence | `HostMsg2` | `optional` set vs unset — the one branch proto3 can't exercise. |
| **Self-recursive nested select** | `HostMsg3.inner.inner.b` | Intern-on-message at depth ≥ 2; each `ReadField` returns `Value::HostMessage(ProtoBacking)`, Layer 2 interns into `FakeRefs` and writes the slot, next `CelGetFieldImpl` looks it up. |
| **Realistic two-hop `c.billing_address.city`** | `Customer` + `Address` | The domain shape M2 ships against.  Uses the four-stage pipeline end-to-end. |
| **ProtoBacking vs JsonBacking parity** | `Customer` (proto) + a test-only `JsonBacking` over a hand-built JSON tree with matching shape | The design proof for the "compile-with-proto, eval-with-JSON" case discussed in §2.4.1.  Both backings run the same `CelAbi`, produce the same decoded `Value`. |
| **Cross-backing transition** | Proto `Customer` whose `bytes session_token` is read by a subclassed `ProtoBacking` that intercepts `"session_token"` and returns a `JsonBacking` | Locks the "tree of heterogeneous backings" story — nothing in Layer 2 prevents it. |
| **Trampoline absorption** | `HostMsg3` with `msg_slot` holding `{CEL_UNKNOWN}` / `{CEL_ERROR}` | Layer 2 absorbs before touching Layer 1; neither `ProtoBacking::ReadField` nor the arena allocator fires. |
| **Unknown-pattern match** | `Customer` with `unknown_patterns = ["c.billing_address"]` evaluating `c.billing_address.city` | First hop matches → writes `{CEL_UNKNOWN, attr_id}`; second hop sees UNKNOWN at `msg_slot` → absorbs.  No allocator call, no interner call, no descriptor walk. |
| **Aliasing** | `msg_slot == out_slot` holding a message | Layer 2's `out_staging` local copy keeps the write safe. |
| **`field_ref_id = 0` sentinel** | `JsonBacking` with `field_number=0` in every decoded `FieldRefEntry` | Name-fallback path works — `ProtoBacking` would reject, `JsonBacking` handles by `field_name`. |
| **Multi-pool descriptor lifetime** | Two `DescriptorPool`s each with their own `Customer`; two `CelHostBindings` built against each | `BuildBindingsFromAbi` + `ProtoBacking` correctly isolate — no cross-pool pointer leakage. |

Fixture deps:

```bzl
# eval/internal/BUILD.bazel  (smoke-test target)
cc_test(
    name = "cel_host_test",
    srcs = ["cel_host_test.cc"],
    deps = [
        ":cel_host",
        ":abi_decode",
        "//compiler/testdata:e2e_fixture_cc_proto",
        "//compiler/testdata:host_fixture_proto3_cc_proto",
        "//compiler/testdata:host_fixture_proto2_cc_proto",
        "//eval:value",
        "@com_google_googletest//:gtest_main",
    ],
)
```

### 6.2 E2E tests (`eval_test.cc`)

  - **Ident × each scalar kind.**  bool / int / uint / double /
    string / bytes / null each bound and retrieved.
  - **Select × every Customer field kind.**  string / int /
    nested-message / repeated (read via size in M3; M2 only
    accesses the message).  `c.name`, `c.age`, `c.order`,
    `c.billing_address.city`.
  - **has() × populated / unpopulated / nested.**
    `has(c.name)`, `has(c.order)` on a customer without order,
    `has(c.billing_address.city)`.
  - **Unknown attribute × leaf + nested.**  `c.name` with
    `--unknown_attrs "c.name"` produces `UNKNOWN(c.name)`.
    `c.billing_address.city` with `--unknown_attrs "c.billing_address"`
    produces `UNKNOWN(c.billing_address)` (chained select
    short-circuits at the first unknown hop).
  - **Wildcard pattern × star mid-path.**
    `c.billing_address.city` with `--unknown_attrs "c.*.city"`
    matches and produces an unknown rooted at `c.*.city`.
  - **Eval vs PartialEval parity.** An expression with no
    unknowns declared evaluates identically under both
    entry points.

### 6.3 Conformance unlock

  - Envelope filter accepts `result_matcher: unknown` /
    `any_unknowns`.
  - `RunOne` routes unknown matchers to `PartialEval` with the
    test's declared attribute patterns parsed into
    `AttributePattern`s.
  - Per the forecast in `conformance/README.md`:
    `fields` (60), `namespace` (14), `has()` slice of `macros`,
    pieces of `enums` unlock via the M2 capability; cross-fixture
    `unknown:` matcher tests unlock via the M2 harness change.
    Expect ~100 + unknowns PASSes.

### 6.4 Lint + testing-checklist rows

Flip these rows on `doc/implementation-plan/testing-checklist.md`
§"Rewrite M2":

  - [ ] `kIdent` lowering × each scalar kind
  - [ ] `kSelect` lowering × each `Customer` field kind
  - [ ] `has()` × populated / unpopulated / nested
  - [ ] `AttributePattern::Matches` × each wildcard position
  - [ ] `Instance::PartialEval` × leaf unknown vs nested unknown
  - [ ] `cel.abi.variables[]` + `fields[]` + `attributes[]`
        populated + decoded by `Engine::Plan`

## 7. Exit criteria

  - [ ] `bazel test //...` green.
  - [ ] `bazel run //conformance:run_conformance` shows
        no `kFail` regressions vs the M1 snapshot.  New PASSes
        appear for `fields.textproto`, `namespace.textproto`, the
        `has()` slice of `macros.textproto`, and unknown-matcher
        tests scattered across the corpus.
  - [ ] `conformance/README.md` inventory table
        refreshed with the new headline (`total`, `pass`, `skip`,
        `fail`) and per-fixture row updates.
  - [ ] `doc/implementation-plan/testing-checklist.md` §"Rewrite M2"
        rows all ticked.
  - [ ] This doc's header flipped to `Status: shipped <date>`,
        with a one-paragraph "what landed" summary if the
        as-shipped shape differs from this plan.
  - [ ] `design.md` §10.1 invariants verified:
          - codegen stays oblivious to partial eval;
          - `cel_reset` semantics survive.
        The "no new `NodeAnnotation` fields" invariant is
        **relaxed** for this milestone: M2 adds `map_origin` +
        `list_origin` (§2.6) as forward-compat hooks for M6.
        The invariant's intent (ident + select + unknown
        handling fits in existing fields) still holds — no
        *idents / selects / unknowns* need new fields; the new
        fields are strictly for map/list origin inference that
        will be exercised by M6 codegen, not M2 codegen.
        Reconcile into `design.md §10.1.3` on that milestone's
        doc-update pass.
  - [ ] `map-list-dispatch.md`'s M2-row in `§9` verified:
        interface stubs land; `Value::HostMap` / `Value::Host
        List` builders added as `ABSL_CHECK` stubs;
        `ProtoBacking::ReadField` on MAP/REPEATED returns
        `CEL_ERR_TYPE_UNSUPPORTED`; `CelKind` enum has the
        four new values declared (but unused by M2 codegen).

## 8. What M2 explicitly proves

  1. **The `Activation` surface is sufficient for idents and
     unknowns both.** No separate "unknowns activation" type; no
     `Activation::BindUnknown` method.  `PartialEval` takes
     patterns as a second argument, `Activation` stays focused
     on concrete binding.
  2. **Codegen stays oblivious to partial eval.** The same
     lowered wasm runs under `Eval` and `PartialEval`; the fork
     lives inside `cel_host.cel_get_field` at runtime.  This is
     load-bearing for the `design.md` §10.1.3 invariant.
  3. **`WasmAnnotations` is the unified symbol table.**
     `local_index`, `field_number`, `attribute_id` populate on
     top of M1's schema without a field addition.  The
     `CLAUDE.md` symbol-table debt is cleared in practice (and
     already cleared in the doc).
  4. **`api/internal/` is the right home for wasmtime glue.**
     `cel_host.cc` joins `instance_impl.cc` +
     `wasmtime_engine_state.cc` as internal-only implementation;
     no user code reaches for it.  If future work (custom
     functions in M5) needs a similar trampoline family, the
     precedent is set.
  5. **The polymorphic-backing pattern generalises from messages
     to maps and lists.** `HostMessageBacking` (functional) +
     `HostMapBacking` / `HostListBacking` (stubs) share the same
     shape.  The fact that M6 can add map/list support as a
     purely additive fill-in (interface shape already declared,
     `Value::HostMap` builder already in place, `map_origin` /
     `list_origin` already inferred on every M2-reachable node
     that produces a map/list) is concrete evidence the pattern
     scales.  Full design in `map-list-dispatch.md`.

## 9. Risk register (M2-specific)

  - **Descriptor pool lifetime.** `field_number` must be resolved
    against the `DescriptorPool` that's live when the *field
    reference* is decoded, not when `Compile` runs.  v1 M3 G2 hit
    this once already — the fix there (pool reachable from
    `Engine::Plan` bindings) transcribes forward.  Mitigation:
    `cel_host_test.cc` has a test that builds two Customers
    against two distinct pools and ensures each `field_ref_id`
    resolves against the right pool.
  - **`cel_unknown_merge` wire-through completeness.** At M2
    the only node kind that consumes unknowns is `kSelect` (the
    chain `x.a.b.c` must absorb at the first unknown hop).
    `kCall` lands in M3 and is where the bulk of merge sites
    live.  Mitigation: M2's `cel_host.cel_get_field` does the
    merge internally (reads msg's CelValue; if kind=UNKNOWN,
    writes UNKNOWN to out without touching descriptors).  An
    M3 regression test will cover the broader propagation once
    `kCall` ships.
  - **AttributeId stability across recompiles.** Two
    compilations of the same source must produce the same
    `AttributeId`s, or cached compiled modules can't be safely
    re-invoked under the same unknown-pattern set.  Mitigation:
    deterministic interning order (ResolvePass visits nodes in
    `expr_id` order; ids grow densely from 1); unit test locks
    the invariant.
  - **`BindLazy` memoisation per Eval call.** Spec is ambiguous;
    cel-cpp memoises.  Mitigation: follow cel-cpp; test locks
    that the fn runs at most once per `Eval`, even if the
    expression references the variable multiple times.

## 8. Future work

Surfaced during M2 execution; not in scope of this milestone.

  - **Conformance harness `ExprValue` → `cel::Activation`
    marshalling.**  The envelope filter still rejects tests
    with `bindings` / `type_env`.  Adding the scalar-ExprValue
    path would graduate ~100 tests in
    `fields.textproto` / `namespace.textproto` /
    `macros.textproto::has()` without any further compiler
    work — the runtime side already lands.
  - **Conformance: expr-id → attribute-id mapping.**  `UnknownSet`
    matchers carry AST expression IDs but our runtime-interned
    `AttributeId` is opaque to the harness.  `CompareUnknown`
    only locks the kind today.  A per-run side table built from
    `ResolvePass` annotations would allow exact id-level
    matching.
  - **Harness: `typed_result:` matcher.**  `type_deduction.textproto`
    (47 tests) is `check_only:true` with a deduced-type matcher;
    envelope drops them today.  Running the checker and
    comparing against `typed_result.deduced_type` unlocks all 47
    without a milestone dependency.
  - **Counting-trampoline test for `cel_alloc`-free fast path.**
    M1's §11 deferred it; M2 didn't close it either.  Explicit
    test that variable-free eval makes no `cel_alloc` call is
    still useful as a perf-regression guard.
  - **Descriptor pool indirection.**  `Compiler` accepts an
    explicit pool via `--schema` / `--schema_descriptorset`;
    fallback is the generated pool.  A future slice could make
    the pool a first-class `Compiler::Builder` argument instead
    of two mutually-exclusive CLI flags.
  - **WAT coverage regression guard.**  `wat_runner_test.cc`
    re-assembles every `.wat` under `doc/.../wat/`; M2's
    kSelect / has / PartialEval WATs now live under that
    directory.  Add byte-level disassembly equivalence tests
    between emitted wasm and authored WAT (planned in CLAUDE.md
    "WAT-first" but not ticketed as a specific test row).

## 10. After M2

Next milestones (from `m1-scalar-pipeline.md §10`, updated for
the unknowns-move-to-M2 scope change):

  - **M3** — `kCall` + built-in overload set; arithmetic /
    comparison / string ops land; `OverloadTable::kBuiltinSeeds`
    populates.  The big unlock for conformance (`comparisons`,
    `conversions`, `integer_math`, `fp_math`, `logic`,
    `string`).
  - **M4** — 3VL + error surface (`eval_error` / `any_eval_errors`
    matchers); `cel_message_eq`.
  - **M5+** — customs, comprehensions, map/list literals, proto
    literals, perf slice.

Each gets its own `m<n>-*.md` plan doc following this template.
