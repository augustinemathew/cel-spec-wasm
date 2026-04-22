# DRAFT — cel_host surface (compiler_v2)

**Status:** DRAFT for discussion, 2026-04-22. Not a spec, not scheduled.
Supersedes the v1 of this draft (deleted); keeps nothing from it except
the file location. Complements `design.md` §4.6/§4.7/§9; where this
draft and `design.md` disagree, `design.md` wins until this draft is
promoted.

This doc scopes the entire host surface a developer touches when
embedding a compiled CEL expression. It covers:

- The **user-facing abstractions** (`Env` / `Program` / `Instance` /
  `Value` / `Activation`) — the ten-line path from source → answer.
- The **`cel.abi` custom section** — v1's self-contained contract,
  extended, not trimmed.
- **3VL as a first-class citizen** — `UNKNOWN` and `ERROR` with real
  payloads, absorbed per spec, producible by host callbacks and
  custom functions.
- The **internal host adapter** — the proto-backed implementation
  that sits behind the `cel_host.*` wasm imports. Today it is the
  only implementation; it is a private class, not a user-facing
  knob.

## 0. Why this draft exists

The prior draft got four things wrong:

1. **Conflated user config with adapter plumbing.** It put
   `SetHostAdapter` on `Env::Builder` and spun up `JsonHostAdapter`
   / `MapHostAdapter` as shipped alternatives. The adapter is an
   implementation detail — today we only support proto, and the
   proto adapter handles externref unwrapping + field resolution
   internally. Users bind `Value`s through `Activation`; they do
   not pick adapters. v1's ABI deliberately kept
   `(field_number, field_name)` as a pair because the ABI is
   future-proof for other backings — but that does not belong in
   the public surface today.
2. **Trimmed the ABI.** It retired `FunctionSet.required_imports`
   and treated `CheckedExpr` as "maybe we drop it". v1's ABI is
   self-contained — a fresh host can see, from bytes alone, every
   external function the module needs and the full type-checked
   expression. That property is load-bearing, not decorative. Keep
   everything.
3. **Demoted 3VL.** It marked unknown-attribute support as "deferred
   / M4 E2a.1" and said errors are "written to out on failure". CEL
   is a 3VL language; `UNKNOWN` and `ERROR` are values, not error
   paths. Every host surface must handle them, and partial eval is a
   first-class feature.
4. **Skipped ergonomics.** It ended at "trampolines + wasmtime
   bindings". A developer's first question isn't "how is the
   trampoline bound"; it's "how do I evaluate `x + 1`". The
   primary API is `Env` / `Program` / `Instance`, mirroring
   cel-cpp.

## 1. Guiding principles

These are non-negotiable for v2. They drive every shape below.

### 1.1 The adapter is internal; `Value` is the user-facing polymorphism

Users interact with `Env` / `Program` / `Instance` / `Value` /
`Activation`. They bind `Value`s to names; they read `Value`s out
of `Eval`. They never see an adapter.

Behind the scenes, the wasm module's `cel_host.*` imports are
wired to a single adapter implementation that understands proto
messages. The adapter owns the externref-to-`Message*` mapping,
resolves field reads/sets/equality, and handles message
construction. It is one class, one file, no interface hierarchy
above it.

**Why not expose the adapter?** Because picking between "proto" vs
"JSON" is not a user-level decision today — today we only support
proto. When the second backing lands (if it does), we will abstract
at that point from one known-good impl, not from speculation. The
ABI already carries `(field_number, field_name)` so the wire
format is future-proof; the C++ surface adapts when the need is
real.

**User data shapes.** A `Value` can wrap a proto `Message`, a
primitive (int/uint/double/bool/string/bytes), a list, a map, a
duration, a timestamp, `UNKNOWN`, or `ERROR`. That is the
polymorphism users care about. If a user has JSON on hand, they
convert it to a `Value::Map` / `Value::List` / primitives on their
side before calling `Activation::Bind`; CEL never sees JSON.

### 1.2 Self-contained ABI

Every external dependency a module has — runtime helper imports,
host helper imports, custom-function imports, required types,
declared variables, the original CEL source, the full CheckedExpr —
lives in `cel.abi`. A tool that reads only the `.wasm` bytes can
answer:

- What CEL expression is this?
- Which variables does it need, at what types?
- Which functions must the host provide?
- Which types must the descriptor pool contain?
- Which fields of which messages does it access?
- What attribute patterns does it permit as unknown?

No out-of-band metadata. No side channel. If it affects evaluation,
it's in the ABI.

### 1.3 Everything in the ABI

Including the original CEL source string, `CheckedExpr` (cel-cpp's
native typed-AST form), the full function-imports list, the full
attribute table, and the full field table. Trimming for size is a
non-goal. Cross-process replay, debugging, static analysis, and
re-compilation with a different codegen all depend on the ABI
being complete.

### 1.4 3VL first-class

`UNKNOWN` and `ERROR` are CEL values with payloads:

- `UNKNOWN` carries an `AttributeId` — a pointer into the ABI's
  attribute table naming which input attribute was unknown (e.g.
  `request.auth.claims`). Partial-eval uses this to tell callers
  "I need this specific input to make progress".
- `ERROR` carries an error code, a human-readable message, and the
  source-expression id (resolving via `CheckedExpr.source_info` to
  a line/col) so diagnostics can point at the offending subexpression.

Every host-callable function (fixed host imports, custom functions,
runtime helpers) absorbs `UNKNOWN` and `ERROR` per `langdef.md` §3VL:

- Strict operators: `UNKNOWN ⊕ x = UNKNOWN`, `ERROR ⊕ x = ERROR`,
  `ERROR` dominates `UNKNOWN`.
- Short-circuit logical ops: `true || UNKNOWN = true`,
  `false && UNKNOWN = false` (preserves progress under partial
  information).
- Host adapters can *produce* `UNKNOWN` and `ERROR` from `ReadField`
  / `HasField` / `Equals`; partial eval is the primary caller.

This is not a compile-time flag or a special mode. It's the runtime.

### 1.5 Intern-ergonomic surface

Ten-line sniff test:

```cpp
ASSIGN_OR_RETURN(auto env, Env::NewBuilder()
    .AddStandardDeclarations()
    .DeclareVariable("x", CelType::Int())
    .Build());
ASSIGN_OR_RETURN(auto program, env.Compile("x + 1"));
ASSIGN_OR_RETURN(auto instance, program.Plan());

Activation act;
act.Bind("x", Value::Int(41));
ASSIGN_OR_RETURN(auto result, instance.Eval(act));
std::cout << result.AsInt().value();    // 42
```

An intern should grasp `Env` / `Program` / `Instance` in one sitting.
Anything fancier (adapters, partial eval, custom functions) is
opt-in.

## 2. User-facing abstractions

Three objects, one builder, two leaves.

```
Env ────Compile(source)──► Program ────Plan()──► Instance
                                                   │
                                                   └──Eval(Activation)──► Value
```

Lifecycle:

- `Env` — built once per process (or per long-lived config). Holds
  function registry, descriptor pool, type declarations, host
  adapter, compiler options. Immutable after `Build()`.
- `Program` — built from `(Env, source)`. Holds wasm bytes + parsed
  ABI. Serializable (persist to disk, re-load later). One `Env` can
  produce many `Program`s.
- `Instance` — built from `Program.Plan()`. Holds the live wasmtime
  store + instance + per-instance state (mutable-message registry,
  externref table). Thread-owned; reused across `Eval` calls with
  automatic `cel_reset` between. One `Program` can yield many
  `Instance`s (multi-threaded use).

### 2.1 `Env`

```cpp
// compiler_v2/api/env.h
namespace celwasm {

class Env {
 public:
  class Builder;
  static Builder NewBuilder();

  // Compile a CEL source string to a Program.  Runs parse → check
  // → lower → assemble; returns a Program that can be evaluated or
  // serialized.  The Env is captured by reference, so it must
  // outlive all Programs it compiles.
  absl::StatusOr<Program> Compile(absl::string_view source) const;

  // Load a Program from previously-serialized wasm bytes.  Used for
  // cross-process / cached workflows.  The Env must be compatible
  // with the Program's ABI — CheckCompatible does the audit.
  absl::StatusOr<Program> LoadFromWasm(absl::string_view wasm_bytes) const;

  // Introspection.
  const FunctionRegistry& functions() const;
  const google::protobuf::DescriptorPool& descriptor_pool() const;
  absl::Span<const VariableDecl> declared_variables() const;
};

class Env::Builder {
 public:
  // Seed with the CEL standard library + the Messages/Durations/
  // Timestamps type set.  Almost every caller starts here.
  Builder& AddStandardDeclarations();

  // Variable declarations — what free variables expressions can use.
  Builder& DeclareVariable(std::string name, CelType type);

  // Custom function registration.  FunctionDecl carries signature +
  // implementation; see §5.
  Builder& RegisterFunction(FunctionDecl decl);

  // Type registration — proto descriptors the compiler may reference.
  Builder& RegisterMessageType(const google::protobuf::Descriptor* desc);

  // Compiler tunables (arena size, debug layout, overload-allowlist,
  // etc.) — NOT declarations.  Variables / functions / types come
  // from DeclareVariable / RegisterFunction / RegisterMessageType
  // above, and feed both the checker AND the ABI emitter.
  Builder& SetCompilerOptions(CompilerOptions opts);

  absl::StatusOr<Env> Build() &&;
};

}  // namespace celwasm
```

### 2.2 `Program`

```cpp
// compiler_v2/api/program.h
namespace celwasm {

class Program {
 public:
  // Introspection — what does this program need to evaluate?
  absl::string_view source() const;         // original CEL expression
  absl::Span<const uint8_t> wasm() const;   // raw .wasm bytes
  const Abi& abi() const;                   // parsed cel.abi

  // Declared variables in bind order (matches $eval param order).
  absl::Span<const VariableDecl> declared_variables() const;

  // Required host-side functions (fixed host imports + declared
  // customs).  A pure introspection call — doesn't touch wasmtime.
  absl::Span<const FunctionImportDecl> required_imports() const;

  // Build an Instance — allocates a wasmtime store + instantiates
  // the expr + runtime modules.  Can be called many times; each
  // Instance is independent (different thread, different state).
  absl::StatusOr<Instance> Plan() const;

  // Check that a (possibly different) Env has everything this
  // Program needs — useful when loading a serialized Program
  // against an Env that might have drifted.
  absl::Status CheckCompatible(const Env& other) const;
};

}  // namespace celwasm
```

### 2.3 `Instance`

```cpp
// compiler_v2/api/instance.h
namespace celwasm {

class Instance {
 public:
  // Full evaluation.  All declared variables must be bound in act;
  // unbound variables are an error (not UNKNOWN).
  absl::StatusOr<Value> Eval(const Activation& act);

  // Partial evaluation — some attributes are declared unknown.
  // Returns Value::Unknown(...) if evaluation can't make progress
  // without resolving at least one of the declared unknowns; returns
  // a concrete Value if short-circuiting let it avoid them.
  absl::StatusOr<Value> PartialEval(
      const Activation& act,
      absl::Span<const AttributePattern> unknowns);

  // Back-reference to the Program.
  const Program& program() const;

  // Explicit reset — normally automatic between Eval calls.
  void Reset();
};

}  // namespace celwasm
```

### 2.4 `Value`

The user-facing counterpart to the 24-byte `CelValue` wire struct.
Construct, inspect, extract.

```cpp
// compiler_v2/api/value.h
namespace celwasm {

class Value {
 public:
  enum Kind {
    kNull, kBool, kInt, kUint, kDouble, kString, kBytes,
    kList, kMap, kMessage, kDuration, kTimestamp,
    kUnknown, kError,
  };

  // ————————— Builders —————————
  static Value Null();
  static Value Bool(bool);
  static Value Int(int64_t);
  static Value Uint(uint64_t);
  static Value Double(double);
  static Value String(std::string);
  static Value Bytes(std::string);
  static Value Duration(absl::Duration);
  static Value Timestamp(absl::Time);
  static Value List(std::vector<Value>);
  static Value Map(std::vector<std::pair<Value, Value>>);

  // Messages — either borrows (caller guarantees lifetime) or owns
  // (Value keeps the unique_ptr).
  static Value Message(const google::protobuf::Message&);
  static Value OwnedMessage(std::unique_ptr<google::protobuf::Message>);

  // First-class 3VL values.
  static Value Unknown(AttributeId);
  static Value Error(ErrorPayload);

  // ————————— Inspection —————————
  Kind kind() const;
  bool IsUnknown() const { return kind() == kUnknown; }
  bool IsError() const { return kind() == kError; }
  bool IsTruthy() const;  // bool false / null / empty list/map → false;
                          // UNKNOWN / ERROR throw — callers should
                          // check first.

  // Typed accessors — StatusOr so a type mismatch is a user error,
  // not a crash.  Mirrors cel-cpp's CelValue interface.
  absl::StatusOr<bool> AsBool() const;
  absl::StatusOr<int64_t> AsInt() const;
  absl::StatusOr<uint64_t> AsUint() const;
  absl::StatusOr<double> AsDouble() const;
  absl::StatusOr<absl::string_view> AsString() const;
  absl::StatusOr<absl::string_view> AsBytes() const;
  absl::StatusOr<absl::Duration> AsDuration() const;
  absl::StatusOr<absl::Time> AsTimestamp() const;
  absl::StatusOr<const ListView*> AsList() const;
  absl::StatusOr<const MapView*> AsMap() const;
  absl::StatusOr<const google::protobuf::Message*> AsMessage() const;
  absl::StatusOr<AttributeId> UnknownAttribute() const;
  absl::StatusOr<const ErrorPayload*> ErrorInfo() const;

  // CEL equality — absorbs UNKNOWN/ERROR per spec.  Returns a Value
  // (not bool) precisely so absorbing is possible.
  Value CelEquals(const Value& other) const;

  // Structural equality — for host test code.
  bool StructurallyEquals(const Value& other) const;
};

}  // namespace celwasm
```

`ListView` / `MapView` are small C++ wrappers over the wire list/map
structure — they defer materialisation (list elements fetched on
access, not eagerly copied) so a user iterating a 1M-element list
doesn't pay to unbox each element through the C++ surface.

### 2.5 `Activation`

```cpp
// compiler_v2/api/activation.h
namespace celwasm {

class Activation {
 public:
  // Direct binding.
  void Bind(std::string name, Value value);

  // Lazy binding — fn is called only if the expression references
  // the variable.  Avoids boxing expensive values up front.
  void BindLazy(std::string name,
                absl::AnyInvocable<absl::StatusOr<Value>() const>);

  // Per-call function override — rare, but cel-cpp supports it.
  void OverrideFunction(std::string name, FunctionImpl fn);

  // Lookup (used by Instance at eval start).
  absl::StatusOr<const Value*> Find(absl::string_view name) const;
};

}  // namespace celwasm
```

## 3. The host adapter (internal)

This section describes machinery users never touch. It lives at
`compiler_v2/host/cel_host.{h,cc}` — the same file the wasmtime
trampolines call into. One concrete implementation today, proto-
backed; described here so reviewers can see the full picture.

### 3.1 What it does

When the wasm module does `x.name`, codegen emits
`cel_host.cel_get_field(out_slot, msg_slot, field_ref_id)`, where
`field_ref_id` is a dense intern id assigned at compile time and
recorded in `cel.abi.fields[]` (§6). The trampoline fires; the
host adapter owns what happens next:

1. **Look up the precomputed entry.** At `LoadEval`, the host
   walked `cel.abi.fields[]` and built a vector
   `FieldRef[field_ref_id] = {FieldDescriptor*, CelType
   result_type}` by resolving each entry against the descriptor
   pool once. The trampoline does one array lookup; no
   per-call descriptor search.
2. **Unwrap `msg_slot`** — a `CelValue{kind: CEL_MESSAGE,
   payload.msg_slot: <externref slot>}` at `msg_slot` points into
   the expr module's `cel_refs` externref table. The adapter
   resolves the slot to a `const google::protobuf::Message*` that
   it (or the activation layer) registered earlier.
3. **Read the field** — via `Reflection::Get*` per the
   precomputed `FieldDescriptor*`.
4. **Materialise the result as a `CelValue` at `out_slot`**, per
   the precomputed `result_type`. Scalars write in place.
   String/bytes copy into the arena via a reentrant `cel_alloc`
   call. Submessages intern via the expr module's
   `cel_ref_intern` export and write
   `{kind: CEL_MESSAGE, payload.msg_slot: <new slot>}`.
   Aggregate returns (list/map fields) materialise the wire
   container per `result_type.list_element` /
   `result_type.map_key/value`.

`cel_has_field`, `cel_message_eq`, `cel_set_field`, and
`cel_make_message` follow the same intern-id pattern — the
adapter is single-owned, file-local, and handles externref
bookkeeping + proto descriptor resolution end-to-end.

### 3.2 Why this is not an interface today

A `HostAdapter` virtual interface with multiple implementations is
not needed until we have a real second backing. "Future-proof" is
not a motivation for interface abstraction — real requirements are.
Until then, `cel_host.cc` is a concrete proto-backed file, reviewed
as such.

If / when a second backing arrives:

1. The concrete proto impl extracts into `adapters/proto.{h,cc}`.
2. A `HostAdapter` interface is carved from its public methods.
3. A second impl joins under `adapters/`.
4. `Env` gains an opt-in hook to override the default.

Each of those four steps is cheap and well-localised. Doing them
preemptively without a second impl is speculative architecture,
and speculative architecture is the specific thing that killed the
previous draft.

### 3.3 Why the ABI still carries both `field_number` and `field_name`

The ABI is a wire contract — durable, cross-process, cross-version.
Trimming it to proto-only today means new backings force a version
bump tomorrow. The cost of including the name string is a handful
of bytes per field reference; the value of *not* re-cutting the ABI
in six months is very real.

The proto adapter uses `field_number` (descriptor lookup by index,
O(1)); `field_name` is carried but unused. When a second backing
arrives, it uses whichever field the checker populated for that
type.

### 3.4 Every callback site is typed

A wasm callback is useless to the host without two facts:

1. What handle / arg kinds the host is receiving.
2. What CelValue kind the host must write back.

The runtime cannot derive either from the `CelValue.kind` bits
alone — `kind` is the dynamic shape; the *expected static type*
is what the trampoline uses to marshal args in and results out.
That expected type lives in the ABI, and every callback site has
exactly one typed entry:

| Callback | Typed ABI entry | Type info carried |
|---|---|---|
| `cel_get_field` | `FieldEntry[field_ref_id]` | owning `type_id`, `result_type` |
| `cel_has_field` | `FieldEntry[field_ref_id]` | `result_type` always `BOOL` (structural) |
| `cel_set_field` | `FieldEntry[field_ref_id]` | expected value `CelType` to coerce from |
| `cel_make_message` | `TypeEntry[type_id]` | descriptor resolution key |
| `cel_message_eq`   | `TypeEntry` of either operand | — (operand types drive Differencer) |
| Custom call        | `CustomFunctionEntry[helper_name]` | `(function_name, overload_id, is_receiver, arg_types[], return_type)` |
| Root variable read | `VariableEntry[param_index]` | declared type; trampoline boxes accordingly |

**Consequence.** The `cel_host` import signatures stay narrow — a
small fixed number of i32s per call — and the full type story
lives in typed ABI entries the host precomputes at `LoadEval`.
No string comparisons at runtime, no descriptor walks per call,
no per-kind dispatch ladders in the trampoline.

**Consequence for custom functions.** A custom function's
trampoline knows, from the ABI, exactly how to unbox each arg
CelValue into a typed C++ `Value` (and which `Value` constructor
to call for the result). The user's `FunctionImpl` never sees raw
offsets; it sees `absl::Span<const Value>` and returns a `Value`.
If the return's kind doesn't match the declared `return_type`,
the trampoline writes an `ERROR` CelValue — a compile-time-typed
function that returns the wrong runtime kind is a user bug, not
a wasm trap.

**Consequence for future typed UNKNOWN.** `AttributeEntry` also
carries the attribute's declared type (§6), so `Value::Unknown`
produced by partial-eval can carry the type it *would have had* if
resolved. Relevant when a user's custom fn inspects an UNKNOWN
input and wants to type-check statically.

## 4. 3VL as a first-class citizen

### 4.1 The four kinds that matter

| Kind | Payload | Produced by | Consumed by |
|---|---|---|---|
| `BOOL` / `INT` / `STRING` / … | primitive | literals, arithmetic, host reads | every operator |
| `UNKNOWN` | `AttributeId` | host adapters (for unknown attrs), partial eval | every operator (absorbs) |
| `ERROR` | `(code, message, expr_id)` | overflow, div-by-zero, type mismatch, host errors | every operator (absorbs, dominates UNKNOWN) |

`ERROR` dominating `UNKNOWN` matches `langdef.md` and cel-cpp:
`ERROR + UNKNOWN = ERROR` (the certainty of failure beats the
uncertainty of missing data).

### 4.2 Absorption rules (per-operator)

Codified in every runtime helper, every host trampoline, every
custom function the host wires in:

- **Strict operators** (`+`, `-`, `*`, `/`, `<`, `>`, `==`, `!=`,
  field reads, `has`, `size`, custom fns by default):
  - any arg `ERROR` → return `ERROR` (first one wins, left-to-right)
  - any arg `UNKNOWN` → return `UNKNOWN` (merge attribute ids if
    multiple)
  - else compute
- **Short-circuit operators** (`&&`, `||`, `?:`):
  - `false && x = false` regardless of `x`
  - `true || x = true` regardless of `x`
  - ternary picks the chosen branch; the unchosen branch's `ERROR`/
    `UNKNOWN` is discarded
- **Negation** (`!`):
  - `!UNKNOWN = UNKNOWN`
  - `!ERROR = ERROR`
  - `!bool = negated bool`

### 4.3 `AttributeId` and `AttributePattern`

```cpp
struct AttributeId {
  // Index into Abi.attributes.  Resolves to (variable, qualifiers[])
  // — e.g. "request.auth.claims".
  uint32_t id;
};

struct AttributePattern {
  // Textual pattern like "request.*" or "user.profile.email".
  // Parsed by ParseAttributePattern(); matched against AttributeIds
  // during partial eval.
  std::string pattern;
};
```

Instance::PartialEval takes a list of patterns; the host adapter
and the boxed root-variable layer both consult this list to decide
whether to return `UNKNOWN` vs a concrete value. A field read whose
fully-qualified attribute path matches a declared-unknown pattern
returns `Value::Unknown(AttributeId{...})`; propagation does the
rest.

### 4.4 `ErrorPayload`

```cpp
struct ErrorPayload {
  enum Code {
    kOverflow, kDivideByZero, kModulusByZero, kTypeMismatch,
    kFieldNotFound, kKeyNotFound, kIndexOutOfBounds,
    kUnknownType, kCustomFnFailed, kHostAdapterError, kTimeout, …
  };
  Code code;
  std::string message;
  uint32_t expr_id;             // index into CheckedExpr.source_info
                                //   for source position
};
```

On-wire representation fits in the 16-byte `CelValue.payload`:
`{code: u16, _pad: u16, expr_id: u32, msg_off: u32, msg_len: u32}`.
The message bytes live in the arena (allocated via `cel_alloc`).

### 4.5 Host contract for 3VL

The `cel_host.*` trampolines (and any custom function trampoline):

- Absorb `UNKNOWN` / `ERROR` args *before* dispatching to adapter
  logic. If any arg at the slot offsets is `UNKNOWN` or `ERROR`,
  the trampoline writes the absorbed value to `out_slot` and
  returns without touching the adapter.
- Allow the adapter (or a custom function) to *produce* `UNKNOWN`
  with an `AttributeId` that's registered in the ABI — used for
  partial eval.
- Allow the adapter (or a custom function) to *produce* `ERROR`
  with a recognised code (or `kHostAdapterError` for embedder-
  specific codes) and a message.

## 5. Custom functions

Per `design.md` §4.6, customs land as per-function wasm imports
under `"cel_host"`. The surface-facing shape is higher-level:

### 5.1 `FunctionDecl`

One `FunctionDecl` describes **one overload**. cel-cpp's
`FunctionDecl` groups multiple `OverloadDecl`s under one name;
we flatten so each row maps 1:1 to an `OverloadTable` entry and
to one `CustomFunctionEntry` in the ABI.

```cpp
// compiler_v2/api/function.h
struct FunctionDecl {
  // CEL-level call name.  What appears in source: "my.upper",
  // "size", "_+_" (the canonical form of the `+` operator).
  std::string name;

  // Globally-unique overload id, matches cel-cpp's OverloadDecl
  // id.  e.g. "my_upper_string", "add_int_int".  Same overload
  // id registered twice → AlreadyExists.
  std::string overload_id;

  // Receiver style?  x.foo(y) vs foo(x, y).  When true,
  // arg_types[0] is the receiver's type.  Checker resolves
  // receiver-style calls against member overloads only; the
  // runtime call site is identical in either case.
  bool is_receiver = false;

  // Total arity including receiver.  Ordered: receiver first
  // (if is_receiver), then formal args left-to-right.
  std::vector<CelType> arg_types;
  CelType return_type;

  // Impl runs at eval time, inside the host, once per wasm call.
  // Receives boxed Values in arg_types order; returns a boxed
  // Value.  Can return Value::Unknown / Value::Error for 3VL
  // semantics (rarely needed — see §5.3).
  FunctionImpl impl;
};

using FunctionImpl = absl::AnyInvocable<
    Value(absl::Span<const Value> args) const>;
```

The `FunctionImpl` takes boxed `Value`s, not raw i32 offsets. The
trampoline (auto-generated by `RegisterCelHost`) handles boxing /
unboxing. Embedders write natural C++:

```cpp
// Global: my.upper("abc") → "ABC"
env_builder.RegisterFunction(FunctionDecl{
    .name = "my.upper",
    .overload_id = "my_upper_string",
    .is_receiver = false,
    .arg_types = {CelType::String()},
    .return_type = CelType::String(),
    .impl = [](absl::Span<const Value> args) -> Value {
      auto s = args[0].AsString();
      if (!s.ok()) return Value::Error({
          ErrorPayload::kTypeMismatch,
          std::string(s.status().message()), 0});
      std::string upper(s->begin(), s->end());
      absl::AsciiStrToUpper(&upper);
      return Value::String(std::move(upper));
    }});

// Receiver-style: x.upper() → "ABC" when x == "abc"
env_builder.RegisterFunction(FunctionDecl{
    .name = "upper",
    .overload_id = "string_upper",
    .is_receiver = true,
    .arg_types = {CelType::String()},    // receiver == arg 0
    .return_type = CelType::String(),
    .impl = /* same body */,
});
```

### 5.2 Overloading

`FunctionDecl` describes one overload. Multiple registrations
under the same `name` with different `overload_id`s are legal
and resolved by the checker (matches cel-cpp's
`FunctionRegistry`). Duplicate `overload_id` → `AlreadyExists`.

### 5.3 3VL in custom fns

CEL's spec treats every user-defined function as strict: the
trampoline absorbs `UNKNOWN` / `ERROR` args *before* calling
`impl`, period. There is no opt-out — short-circuit behaviour is
hardcoded at the operator level (`&&`, `||`, `?:`) and is not
extensible to customs. A custom fn therefore only ever sees
concrete, non-3VL args, and needs no special handling for
absorption.

A custom `impl` may still *produce* `Value::Unknown` or
`Value::Error` — those are first-class return values and
propagate through the rest of the expression under the normal
absorption rules (§4.2).

### 5.4 Compile-time flow — where the ABI row comes from

Function decls flow from the public API through the checker
into the ABI in one direction, with no separate
`CompileOptions::functions`:

```
Env::Builder::RegisterFunction(FunctionDecl d)
    │
    ▼  (d moved into Env's registry; d.impl retained for LoadEval)
Env::functions()  ◄──── frozen at Builder::Build()
    │
    ▼  (env.Compile(source) runs)
CompilerInternal::FrozenDeclView
    │      (signature-only view over Env::functions();
    │       impl intentionally stripped — compile never
    │       needs it)
    │
    ├──► cel-cpp TypeCheckerBuilder::AddFunction
    │       (checker resolves "my.upper(x)" → overload id
    │        "my_upper_string", is_receiver = false)
    │
    └──► Codegen / ABI emitter
            │
            ▼  UsedImports(used_overload_ids)
                — filters to the subset this expression touches
            │
            ▼
        cel.abi.functions.host_custom_imports[]:
          CustomFunctionEntry {
            function_name = d.name,
            overload_id   = d.overload_id,
            is_receiver   = d.is_receiver,
            helper_name   = d.helper_name  (defaults to overload_id),
            arg_types     = d.arg_types,
            return_type   = d.return_type,
          }
```

**`CompilerOptions` holds only tunables** — arena size, debug
layout, stdlib-overload allowlist, and similar knobs that affect
lowering or runtime behaviour but are not name/type/signature
declarations. If a knob needs to be cross-checked at LoadEval
(e.g. arena page count), it goes into `cel.abi.layout`, not into
`CompilerOptions`.

**Impls are only used at LoadEval.** A `FunctionDecl` carries
an `impl` so the Env can bind it when an `Instance` is planned
— but the impl is invisible to compile. This means one `Env`
can `Compile` many programs in parallel without risking impl
state sharing, and an `Env` with stubbed impls (e.g. all
returning `Unimplemented`) can still produce a valid compiled
`Program` — useful for cross-compile / deploy-then-bind flows.

### 5.5 Cross-check at `LoadEval`

At `LoadEval`, the host walks `cel.abi.functions.host_custom_imports[]`
and for each `CustomFunctionEntry e`:

1. Look up `env.functions()` for a registered `FunctionDecl d`
   matching **every** of `(e.function_name, e.overload_id,
   e.is_receiver)`. Missing → `FailedPrecondition` citing which
   key was missing.
2. Verify `d.arg_types.size() == e.arg_types.size()` and each
   position matches. Mismatch → `FailedPrecondition` printing
   both signatures.
3. Verify `d.return_type == e.return_type`.
4. Bind the wasm import `cel_host.<e.helper_name>` to a
   per-decl trampoline that boxes args per `e.arg_types` and
   writes the result per `e.return_type`.

Rejection at step 1 catches registry drift (embedder updated the
function signature since the module was compiled). Rejection at
steps 2–3 catches the same kind of drift at a finer granularity
— the embedder changed arity or types. All rejection paths
surface at load, not at first `Eval`, so a deployment error
fails visibly rather than mid-request.

**Standard functions (stdlib) analogous but simpler.** Standard
overloads have fixed `overload_id`s (per cel-cpp's
`common/standard_definitions.h`); the runtime module declares
them via `FunctionSet.runtime_imports[]` by id. A stdlib overload
drift — a runtime build mismatch — surfaces the same way, but
the mismatch implies a build bug rather than a registry bug.

## 6. `cel.abi` — the self-contained contract

v2 schema. Preserves v1 and extends. Lives at
`compiler_v2/host/cel_abi.proto`.

```proto
syntax = "proto3";
package celwasm.abi;

import "cel/expr/checked.proto";

message CelAbi {
  uint32 version = 1;                        // current = 2
  string cel_source = 2;                     // kept (was in v1)
  cel.expr.CheckedExpr checked = 3;          // kept (was in v1)

  repeated FieldEntry fields = 4;            // kept + extended
  repeated AttributeEntry attributes = 5;    // kept, now first class
  FunctionSet functions = 6;                 // kept + restructured
  MemoryLayout layout = 7;                   // kept

  repeated TypeEntry types = 8;              // new
  repeated VariableEntry variables = 9;      // new
}

// A field-access call site the module makes.  Indexed densely by
// `field_ref_id`; the wasm call passes only that id (§3.4), and
// the host precomputes (FieldDescriptor*, marshal info) at
// LoadEval from the rest of the row.  Both number and name are
// always populated — adapters pick which to honour (§3.3).
message FieldEntry {
  uint32 field_ref_id = 1;    // dense, 0-indexed; wasm call-site id
  uint32 type_id = 2;         // owning container type (→ TypeEntry)
  uint32 field_number = 3;    // 0 if the type isn't proto-backed
  string field_name = 4;
  CelType result_type = 5;    // static type of the field-access
                              //   result.  Drives how the trampoline
                              //   marshals the read/set value and
                              //   how aggregates (list/map) are
                              //   materialised.
}

// An attribute path the module may reference as an "unknown"
// input for partial eval.  Indexed by id so UNKNOWN CelValues can
// carry a single i32.
message AttributeEntry {
  uint32 id = 1;
  string variable = 2;                   // "request"
  repeated string qualifiers = 3;        // ["auth", "claims"]
  CelType declared_type = 4;             // type the attribute would
                                         //   have if resolved — lets
                                         //   typed UNKNOWN travel
                                         //   through typed customs
                                         //   (§3.4).
}

// Every external function the module needs to evaluate.  Self-
// contained: a host reading only this block knows exactly which
// imports to bind.  Grouped by module for clarity; the wasm-side
// import shapes are derivable from (module, name, kind).
message FunctionSet {
  // cel.* runtime helpers (arithmetic, string ops, 3VL, arena).
  // Host doesn't bind these — the runtime module does — but we
  // record them so a re-linker sees the full graph.
  repeated string runtime_imports = 1;

  // cel_host.* fixed host imports (field read/has/set/make/eq +
  // log).  Host binds these.
  repeated string host_fixed_imports = 2;

  // cel_host.* custom imports (registered by the embedder).  Host
  // binds these.  Each entry carries signature info so mismatches
  // are caught at load time.
  repeated CustomFunctionEntry host_custom_imports = 3;
}

// A single custom overload the module references.  Mirrors
// cel-cpp's (FunctionDecl, OverloadDecl) pair — the `function_name`
// is the CEL-level call name (what appears in source); the
// `overload_id` is cel-cpp's globally-unique key for this
// specific overload variant.  The host registry is keyed by both
// at LoadEval (§5.4 cross-check).
message CustomFunctionEntry {
  string function_name = 1;    // CEL-level name, e.g. "my.upper"
                               //   or "_+_" for operator overloads.
  string overload_id = 2;      // cel-cpp overload id, globally
                               //   unique, e.g. "my_upper_string"
                               //   or "add_int_int".
  bool is_receiver = 3;        // true if called as x.foo(...);
                               //   when true, arg_types[0] is the
                               //   receiver type (matches cel-cpp's
                               //   OverloadDecl::member_function).
  string helper_name = 4;      // wasm import name under "cel_host".
                               //   By convention == overload_id,
                               //   but embedders may override.
  repeated CelType arg_types = 5;  // total arity including receiver.
  CelType return_type = 6;
}

message TypeEntry {
  uint32 type_id = 1;
  string name = 2;             // proto FQN today (e.g.
                               //   "com.example.Customer").
                               // A future non-proto backing may
                               //   use an embedder-chosen name.
}

message VariableEntry {
  uint32 param_index = 1;      // wasm-param index in $eval
  string name = 2;
  CelType type = 3;            // embedded (not a type_id ref) —
                               //   simple vars are flat
}

message CelType {
  enum Kind {
    UNKNOWN = 0;
    BOOL = 1;
    INT = 2;
    UINT = 3;
    DOUBLE = 4;
    STRING = 5;
    BYTES = 6;
    LIST = 7;
    MAP = 8;
    MESSAGE = 9;
    DURATION = 11;
    TIMESTAMP = 12;
  }
  Kind kind = 1;
  uint32 message_type_id = 2;  // for MESSAGE → TypeEntry
  CelType list_element = 3;    // for LIST
  CelType map_key = 4;
  CelType map_value = 5;
}

message MemoryLayout {
  uint32 initial_pages = 1;
  uint32 max_pages = 2;
}
```

### 6.1 What stays from v1

- `version`, `cel_source`, `checked` — full source + CheckedExpr.
- `fields` — field table. v1's intern-id design restored as
  `field_ref_id`; schema grows `result_type` (§3.4) so the
  trampoline can marshal without consulting the descriptor pool
  on the hot path.
- `attributes` — attribute table. Grows `declared_type` so typed
  UNKNOWN can flow through customs; otherwise unchanged. Now
  load-bearing (partial-eval first-class, not deferred).
- `FunctionSet` — the self-containment cornerstone. Restructured
  into three arms (runtime vs host-fixed vs host-custom) so each
  arm can be validated against the appropriate registry, but the
  *idea* — a full enumeration of external deps — is preserved.
- `MemoryLayout` — unchanged.

### 6.2 What's new

- `types` — explicit type table. Used today only for proto
  messages, but structured so a later non-proto backing would be
  additive (no schema break).
- `variables` — explicit root-variable declarations with
  param-index ordering. v1 inferred this from wasm-param layout;
  v2 makes it explicit so `CheckCompatible` can validate before
  instantiation rather than failing at first `Eval`.

## 7. End-to-end examples

The section an intern reads first.

### 7.1 Basic

```cpp
#include "compiler_v2/api/env.h"
#include "compiler_v2/api/value.h"

using ::celwasm::Env;
using ::celwasm::Value;
using ::celwasm::Activation;
using ::celwasm::CelType;

absl::StatusOr<int64_t> AddOne(int64_t x) {
  ASSIGN_OR_RETURN(auto env, Env::NewBuilder()
      .AddStandardDeclarations()
      .DeclareVariable("x", CelType::Int())
      .Build());
  ASSIGN_OR_RETURN(auto program, env.Compile("x + 1"));
  ASSIGN_OR_RETURN(auto instance, program.Plan());

  Activation act;
  act.Bind("x", Value::Int(x));
  ASSIGN_OR_RETURN(auto result, instance.Eval(act));
  return result.AsInt();
}
```

### 7.2 Proto message

```cpp
absl::StatusOr<std::string> FullName(const Customer& c) {
  ASSIGN_OR_RETURN(auto env, Env::NewBuilder()
      .AddStandardDeclarations()
      .RegisterMessageType(Customer::GetDescriptor())
      .DeclareVariable("c", CelType::Message("com.example.Customer"))
      .Build());
  ASSIGN_OR_RETURN(auto program,
      env.Compile("c.first_name + ' ' + c.last_name"));
  ASSIGN_OR_RETURN(auto instance, program.Plan());

  Activation act;
  act.Bind("c", Value::Message(c));
  ASSIGN_OR_RETURN(auto result, instance.Eval(act));
  return std::string(result.AsString().value());
}
```

### 7.3 Custom function

```cpp
auto env_builder = Env::NewBuilder()
    .AddStandardDeclarations()
    .DeclareVariable("s", CelType::String());

env_builder.RegisterFunction({
    .name = "my.upper",
    .overload_id = "my_upper_string",
    .is_receiver = false,
    .arg_types = {CelType::String()},
    .return_type = CelType::String(),
    .impl = [](auto args) -> Value {
      auto s = args[0].AsString();
      if (!s.ok()) return Value::Error({ErrorPayload::kTypeMismatch,
                                        std::string(s.status().message()), 0});
      std::string u(s->begin(), s->end());
      absl::AsciiStrToUpper(&u);
      return Value::String(std::move(u));
    },
});

ASSIGN_OR_RETURN(auto env, std::move(env_builder).Build());
ASSIGN_OR_RETURN(auto program, env.Compile("my.upper(s)"));
ASSIGN_OR_RETURN(auto instance, program.Plan());

Activation act;
act.Bind("s", Value::String("hello"));
ASSIGN_OR_RETURN(auto result, instance.Eval(act));      // "HELLO"
```

### 7.4 Partial eval

```cpp
// We haven't fetched the user's email yet — mark it unknown.
Activation act;
act.Bind("user.name", Value::String("Alice"));

ASSIGN_OR_RETURN(auto result, instance.PartialEval(
    act, {AttributePattern::Parse("user.email")}));

if (result.IsUnknown()) {
  // Expression depends on user.email; fetch and re-eval.
  // result.UnknownAttribute() tells us which attribute specifically.
} else {
  // Short-circuit let the expression decide without us.
}
```

### 7.5 Cross-process (serialize / load)

```cpp
// Server-side.
auto wasm_bytes = program.wasm();   // raw .wasm
Persist(wasm_bytes);                // to disk, to cache, to S3

// Later / elsewhere — same Env layout.
auto wasm_bytes = Load();
ASSIGN_OR_RETURN(auto program, env.LoadFromWasm(wasm_bytes));
RETURN_IF_ERROR(program.CheckCompatible(env));
ASSIGN_OR_RETURN(auto instance, program.Plan());
// ... Eval as before.
```

The ABI makes `LoadFromWasm` self-checking — mismatches between
`program.declared_variables()` and `env.declared_variables()` surface
at `CheckCompatible`, not at first `Eval`.

## 8. File layout

```
compiler_v2/
├── api/                            # user-facing public headers
│   ├── BUILD.bazel
│   ├── env.{h,cc,_test.cc}
│   ├── program.{h,cc,_test.cc}
│   ├── instance.{h,cc,_test.cc}
│   ├── value.{h,cc,_test.cc}
│   ├── activation.{h,cc,_test.cc}
│   ├── function.h                  # FunctionDecl / FunctionImpl
│   ├── type.h                      # CelType
│   └── attribute.{h,cc,_test.cc}   # AttributeId / AttributePattern
├── host/
│   ├── BUILD.bazel
│   ├── cel_abi.proto               # §6 schema
│   ├── cel_abi.{h,cc}              # build (codegen side)
│   ├── abi_parse.{h,cc,_test.cc}   # deserialize (host side)
│   ├── cel_host.{h,cc,_test.cc}    # proto-backed adapter +
│   │                               #   fixed host-fn implementations
│   │                               #   (read/has/set/make/eq).
│   │                               #   Internal; no public interface
│   │                               #   class carved out yet (§3.2).
│   ├── cel_host_3vl.{cc,h}         # shared UNKNOWN/ERROR absorption
│   │                               #   used by every trampoline.
│   ├── cel_host_wasmtime.{h,cc}    # RegisterCelHost trampolines
│   ├── custom_registry.{h,cc,_test.cc}
│   ├── host_loader.{h,cc,_test.cc} # two-phase wasmtime instantiation
│   └── cel_log.{h,cc,_test.cc}     # already shipped
└── …
```

The `api/` tier is the public surface. The `host/` tier is machinery.
An embedder should almost never need to `#include` anything under
`host/` — `api/env.h` and `api/value.h` should suffice for 95% of
use cases.

## 9. Per-slice landing plan

Maps onto `design.md` §11.4. One new slice (**S3.5**) carves out the
api/ tier separately so it lands before the first slice that needs
it (S4, first proto reads).

| Slice | Additions |
|---|---|
| **S3.5** | `api/value.{h,cc}` + `api/activation.{h,cc}` + `api/env.{h,cc}` skeleton + `api/program.{h,cc}` + `api/instance.{h,cc}`. Wires the existing (scalar-only) pipeline through the public surface; `Env::Compile` / `Program::Plan` / `Instance::Eval` work for `-e "42"`. |
| **S4** | `cel_host.{h,cc}` proto-backed adapter + fixed host imports (`cel_get_field`, `cel_has_field`). ABI v2 schema lands; `abi_parse` reads everything. Attribute table reserved but not yet populated. Root variables (scalars + proto messages) end-to-end. |
| **S4.5** | `AttributePattern` + partial eval on root variables. Attribute table populated by checker. `Instance::PartialEval` lands. |
| **S6** | `cel_message_eq`. ERROR payload grows `expr_id` field. |
| **S7** | Custom functions — `FunctionDecl` + `RegisterCelHost` customs loop + `FunctionImpl` auto-boxing trampolines. |
| **S8** | List + map literals at the wasm level. `Value::List` / `Value::Map` on the user surface. No new adapters. |
| **S9** | `cel_make_message` + `cel_set_field` in the proto adapter. Message-literal construction wired end-to-end. |

S3.5 and S4.5 are the new slices this draft introduces vs design.md.
Both are single-day and unlock the rest.

## 10. Open questions

### Decided (2026-04-22)

- **Q1. Adapter ownership.** ~~Does `Env` own the adapter, or
  `Instance`?~~ The adapter is internal to `host/` and not part
  of the public surface (§1.1, §3). The `Env` holds the config it
  needs (descriptor pool, declared variables, function registry);
  `Instance` holds the live wasmtime plumbing. The adapter lives
  inside the host-side machinery and is constructed per `Instance`
  — stateful per-eval work (externref table, mutable-message
  registry) belongs there.
- **Q2. `Value::Message(const Message&)` copy vs borrow.**
  **Borrow.** Matches cel-cpp's `CelValue` conventions. Lifetime
  documented on the function. `Value::OwnedMessage(unique_ptr)` is
  the copying alternative.

### Still open

1. **`Activation::BindLazy` — is the value re-fetched across
   multiple `Eval`s on the same `Instance`?** If yes, lazy =
   per-`Find` call; if no, lazy = first `Find` caches. Recommend:
   **per-`Eval` caching, clear on `Reset`.** Matches developer
   intuition ("same eval sees the same value").

2. **Should `Value::List` / `Value::Map` materialise the wire list
   eagerly, or stream?** Eager is simple; streaming avoids
   blowing up on large lists. Recommend: **streaming view
   (`ListView` / `MapView`) in the API, eager copy only when the
   user explicitly asks for `ToVector` / `ToMap`.**

3. **Is `AttributeId` resolved eagerly (attribute_id = compile-time
   int) or lazily (attribute_id = string resolved at partial-eval
   time)?** Eager = fast; requires full attribute table in ABI.
   Lazy = flexible; allows runtime-declared unknowns. Recommend:
   **eager for declared attributes (common case), plus a fallback
   string path for runtime-synthesised attribute ids (rare).**

4. **Does `Instance` always implicitly `Reset` between `Eval`s, or
   does the caller control it?** Implicit is safer, explicit
   allows reuse of arena state across back-to-back evals (rare).
   Recommend: **implicit, with an opt-out
   `Instance::EvalWithoutReset`** for the advanced case.

## 11. Non-goals for this draft

Called out explicitly so reviewers don't ask:

- **Non-proto backings (JSON, map, user-defined structs).** The
  ABI leaves room (§3.3, §6.1); the v2 public surface only binds
  proto. If / when a second backing is real, §3.2 describes the
  four-step path to add it. Not attempting it speculatively.
- **Thread safety of `Env`.** Immutable after `Build`; read-only.
  Thread-safe by construction. Not discussed further.
- **Serialization of `Value` itself.** Users serialize via proto /
  JSON / their own format. `Value` is in-process only.
- **Hot-reload of `Program`.** Re-compile and swap `Instance`s. No
  dynamic patching.
- **Pluggable arena allocators.** The arena is the arena. Size
  configurable via `CompilerOptions`; algorithm is bump.
- **`CompilerOptions` details.** Important but orthogonal; lives in
  a separate doc when we get there.

## 12. What happens next

This draft is for discussion. Suggested review order:

1. Does the `Env` / `Program` / `Instance` triad feel right? It's
   the biggest shape decision; everything else hangs off it.
2. Is the activation → `Value::Bind` → internal-adapter flow the
   right cut between user and runtime? The previous draft exposed
   the adapter; this one hides it. Is anything still leaking?
3. Are the 3VL absorption rules in §4.2 the ones we want, or is
   there a shorter formulation?
>>> The 3VL absorpotion are totally handled in the compiler and emitted code. This layer does not concern itself with it. 
4. Does the ABI schema in §6 preserve the v1 self-contained
   property to your satisfaction, or did we still trim something?
5. Which of the still-open questions in §10 should be decided
   before promotion; which can be deferred?

Once settled, this draft graduates to `cel-host-design.md` (no
`.draft.` infix), slices are added to `design.md` §11, and the
testing checklist gets new rows.
