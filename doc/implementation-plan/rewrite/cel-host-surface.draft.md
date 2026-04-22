# DRAFT — cel_host surface (compiler_v2)

**Status:** DRAFT for discussion, 2026-04-21. Not a spec, not scheduled.
Supersedes the v1 of this draft (deleted); keeps nothing from it except
the file location. Complements `design.md` §4.6/§4.7/§9; where this
draft and `design.md` disagree, `design.md` wins until this draft is
promoted.

This doc scopes the entire host surface a developer touches when
embedding a compiled CEL expression. It covers:

- The **user-facing abstractions** (`Env` / `Program` / `Instance` /
  `Value` / `Activation`) — the ten-line path from source → answer.
- The **polyglot `HostAdapter`** — how the runtime reads fields from
  proto, JSON, maps, or any other backing without the wasm module
  knowing or caring.
- The **`cel.abi` custom section** — v1's self-contained contract,
  extended, not trimmed.
- **3VL as a first-class citizen** — `UNKNOWN` and `ERROR` with real
  payloads, absorbed per spec, producible by host adapters and
  custom functions.

## 0. Why this draft exists

The prior draft got four things wrong:

1. **Dropped polyglot-ness.** It hard-coded proto in the cel_host
   surface (`cel_host.cel_get_field(msg_slot, field_id)` with the
   host immediately unwrapping a `Message*`). v1 deliberately
   kept `(field_number, field_name)` as a pair so JSON / map / user-
   struct backings are equally first-class. The runtime should not
   know whether it's talking to a proto.
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

### 1.1 Polyglot runtime

The wasm module knows nothing about proto. `cel_host.*` imports
accept and return opaque container handles (`i32` externref slots);
they never decode a `Descriptor*`. The *adapter* does that — and the
adapter is a pluggable C++ interface with at least three concrete
implementations shipped in-tree (proto, JSON, map).

**Consequence:** a host embedding CEL over a JSON document can bind
the same `cel_host.cel_get_field` import without recompiling the
runtime, as long as it provides a `JsonHostAdapter`. The ABI's field
table carries both `field_number` and `field_name`; the adapter
picks whichever it understands.

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
  const HostAdapter& host_adapter() const;
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

  // Host adapter — how proto/json/map backings are decoded.  Default
  // is ProtoHostAdapter.  Pass a JsonHostAdapter or MapHostAdapter
  // (both in-tree) or a custom impl.
  Builder& SetHostAdapter(std::unique_ptr<HostAdapter>);

  // Compiler options — arena size, debug layout, etc.
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

## 3. The polyglot `HostAdapter`

The runtime calls out to the host whenever an expression touches
something the wasm module can't resolve locally: a field access on a
message, a `has()` test, a message-equality check, a message-literal
construction. None of those calls assume proto.

### 3.1 Interface

```cpp
// compiler_v2/host/host_adapter.h
namespace celwasm {

// A type-erased handle to an "opaque container" — the adapter
// interprets it.  For ProtoHostAdapter, it wraps a Message*.  For
// JsonHostAdapter, it wraps a JSON object ptr.  For MapHostAdapter,
// it wraps a map<string, Value>*.
class ContainerRef {
 public:
  template <class T> const T& As() const;
  template <class T> T& AsMut();
  uint32_t type_id() const;   // → Abi.types
};

class HostAdapter {
 public:
  virtual ~HostAdapter() = default;

  // —— Read side ——
  // `field_number` is provided (non-zero) when the checker resolved
  // a proto field number; adapters that don't understand numbers
  // should use `field_name` instead.  Both are always populated in
  // the ABI; adapters pick which they honour.
  //
  // Returns any Value kind, INCLUDING Unknown / Error.  A missing
  // field, a forbidden access, a lazy-fetch that hasn't resolved —
  // all are first-class return values here.
  virtual Value ReadField(ContainerRef container,
                          absl::string_view field_name,
                          uint32_t field_number) = 0;

  // `has(msg.field)` — returns a bool Value OR Unknown/Error.
  virtual Value HasField(ContainerRef container,
                         absl::string_view field_name,
                         uint32_t field_number) = 0;

  virtual Value Equals(ContainerRef a, ContainerRef b) = 0;

  // —— Write side (message-literal construction) ——
  // Optional — adapters that only support read may return
  // Unimplemented.  The checker prevents construction of types the
  // adapter doesn't support, so this path is dead for read-only
  // adapters in well-typed code.
  virtual absl::StatusOr<ContainerHandle> MakeContainer(
      uint32_t type_id) = 0;

  virtual absl::Status SetField(ContainerHandle& container,
                                absl::string_view field_name,
                                uint32_t field_number, Value value) = 0;

  // —— Type metadata ——
  // Drives Env's type-registration: "which types do I understand?"
  virtual absl::Span<const AdapterTypeInfo> DeclaredTypes() const = 0;
};

}  // namespace celwasm
```

`ContainerHandle` is the owning variant — `Instance` keeps a registry
of handles constructed during eval and drops them at `Reset`.
`ContainerRef` is the borrowed view.

### 3.2 Shipped adapters

Three in-tree, all single-file:

- **`ProtoHostAdapter`** (`compiler_v2/host/adapters/proto.h`) —
  reads `google::protobuf::Message*` via `Reflection`. Transcribed
  from v1 `cel_host.cc`.
- **`JsonHostAdapter`** (`compiler_v2/host/adapters/json.h`) — reads
  a small internal `JsonValue` (or the embedder's JSON lib of
  choice via a thin shim). `field_number` is ignored; lookup by
  name. Write side supported via mutation.
- **`MapHostAdapter`** (`compiler_v2/host/adapters/map.h`) —
  `absl::flat_hash_map<std::string, Value>` as a container.
  Minimal, useful in tests, illustrates the interface.

Embedders register their own with `Env::Builder::SetHostAdapter(...)`.

### 3.3 Why `field_name` AND `field_number`

v1 gets this right. The checker knows field numbers for proto-typed
expressions and emits them into the AST; name is the fallback for
non-proto backings (JSON) or forward-compat (field renames). The ABI
stores both per field reference. The wasm-level import signature
carries both i32s; a 0-valued number signals "resolve by name".

**Performance.** Proto adapter uses number (fast path, descriptor
lookup by index). JSON adapter uses name. Both paths are O(1) in
the common case (proto: array lookup; JSON: hash). The other
argument in each call is untouched — small cost for a big
polyglot win.

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

Every `HostAdapter` implementation MUST:

- Accept `ContainerRef`s representing `UNKNOWN` / `ERROR` values and
  return them unchanged (pass-through absorption). Typically this
  is handled by the fixed trampolines *before* the adapter is
  called — so adapter authors don't need to implement it — but the
  interface allows adapters to override for weird cases (e.g. an
  adapter that wants `UNKNOWN + x` to materialise).
- Be allowed to return `UNKNOWN` from any method, naming an
  `AttributeId` that's registered in the ABI.
- Be allowed to return `ERROR` from any method, with a code the
  host runtime recognises (or `kHostAdapterError` for embedder-
  specific codes).

## 5. Custom functions

Per `design.md` §4.6, customs land as per-function wasm imports
under `"cel_host"`. The surface-facing shape is higher-level:

### 5.1 `FunctionDecl`

```cpp
// compiler_v2/api/function.h
struct FunctionDecl {
  std::string name;                           // "my.upper"
  std::vector<CelType> arg_types;
  CelType return_type;

  // Impl runs at eval time, inside the host, once per wasm call.
  // Receives boxed Values; returns a boxed Value.  Can return
  // Value::Unknown / Value::Error for 3VL semantics.
  FunctionImpl impl;
};

using FunctionImpl = absl::AnyInvocable<
    Value(absl::Span<const Value> args) const>;
```

The `FunctionImpl` takes boxed `Value`s, not raw i32 offsets. The
trampoline (auto-generated by `RegisterCelHost`) handles boxing /
unboxing. Embedders write natural C++:

```cpp
env_builder.RegisterFunction(FunctionDecl{
    .name = "my.upper",
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
```

### 5.2 Overloading

`FunctionDecl` describes a single overload. Multiple registrations
under the same `name` with different `arg_types` are legal and
resolved by the checker (matches cel-cpp's `FunctionRegistry`).

### 5.3 3VL in custom fns

The trampoline absorbs `UNKNOWN` / `ERROR` *before* calling `impl`
by default (matches strict-operator semantics). Custom fns that
want to see `UNKNOWN` / `ERROR` directly (e.g. a logical-merge fn)
set `FunctionDecl::strict = false` and receive the raw args.

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

// A field reference the module makes.  Both number and name are
// always populated — adapters pick which to honour (§3.3).
message FieldEntry {
  uint32 type_id = 1;         // TypeEntry.type_id the field lives on
  uint32 field_number = 2;    // 0 if the type isn't proto-backed
  string field_name = 3;
}

// An attribute path the module may reference as an "unknown"
// input for partial eval.  Indexed by id so UNKNOWN CelValues can
// carry a single i32.
message AttributeEntry {
  uint32 id = 1;
  string variable = 2;                   // "request"
  repeated string qualifiers = 3;        // ["auth", "claims"]
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

message CustomFunctionEntry {
  string helper_name = 1;      // wasm import name under "cel_host"
  CelType return_type = 2;
  repeated CelType arg_types = 3;
  bool strict = 4;             // §5.3
}

message TypeEntry {
  uint32 type_id = 1;
  string name = 2;             // "com.example.Customer" (proto FQN
                               //   or embedder-chosen name)
  enum BackingHint {
    UNSPECIFIED = 0;
    PROTO = 1;
    JSON = 2;
    MAP = 3;
    OPAQUE = 4;
  }
  BackingHint backing = 3;     // adapter may ignore
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
- `fields` — field table. Changed schema (field_number is the
  dispatch key, not an intern-id) but the purpose is identical.
- `attributes` — attribute table. Unchanged schema, but now load-
  bearing (partial-eval first-class, not deferred).
- `FunctionSet` — the self-containment cornerstone. Restructured
  into three arms (runtime vs host-fixed vs host-custom) so each
  arm can be validated against the appropriate registry, but the
  *idea* — a full enumeration of external deps — is preserved.
- `MemoryLayout` — unchanged.

### 6.2 What's new

- `types` — explicit type table so multi-backing (proto / JSON /
  map) is representable.
- `variables` — explicit root-variable declarations with
  param-index ordering. v1 inferred this from wasm-param layout;
  v2 makes it explicit so multi-adapter hosts can validate before
  instantiation.

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

### 7.5 Polyglot (JSON backing)

```cpp
ASSIGN_OR_RETURN(auto env, Env::NewBuilder()
    .AddStandardDeclarations()
    .SetHostAdapter(std::make_unique<JsonHostAdapter>())
    .DeclareVariable("doc", CelType::Json("Document"))   // JSON-backed type
    .Build());

ASSIGN_OR_RETURN(auto program, env.Compile("doc.title"));
ASSIGN_OR_RETURN(auto instance, program.Plan());

Activation act;
act.Bind("doc", Value::Json(ParseJson(R"({"title": "Hello"})")));
ASSIGN_OR_RETURN(auto result, instance.Eval(act));      // "Hello"
```

Same expression. Same `Env::NewBuilder().Compile()` path. Different
adapter. No wasm recompile across adapters — the module sees only
`cel_host.cel_get_field(out, msg_slot, field_name, field_number)`;
what's behind `msg_slot` is the adapter's problem.

### 7.6 Cross-process (serialize / load)

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
│   ├── host_adapter.h              # interface
│   ├── host_adapter_3vl.{cc,h}     # shared UNKNOWN/ERROR absorption
│   ├── adapters/
│   │   ├── proto.{h,cc,_test.cc}
│   │   ├── json.{h,cc,_test.cc}
│   │   └── map.{h,cc,_test.cc}
│   ├── cel_abi.proto               # §6 schema
│   ├── cel_abi.{h,cc}              # build (codegen side)
│   ├── abi_parse.{h,cc,_test.cc}   # deserialize (host side)
│   ├── cel_host.{h,cc,_test.cc}    # fixed host-fn implementations
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
| **S4** | `host_adapter.h` interface + `ProtoHostAdapter` + fixed host imports (`cel_get_field`, `cel_has_field`). ABI v2 schema lands; `abi_parse` reads everything. Attribute table reserved but not yet populated. Root variables (non-proto scalars + proto messages) end-to-end. |
| **S4.5** | `AttributePattern` + partial eval on root variables. Attribute table populated by checker. `Instance::PartialEval` lands. |
| **S6** | `cel_message_eq`. ERROR payload grows `expr_id` field. |
| **S7** | Custom functions — `FunctionDecl` + `RegisterCelHost` customs loop + `FunctionImpl` auto-boxing trampolines. |
| **S8** | `JsonHostAdapter` + `MapHostAdapter`. List + map literals at the wasm level. `Value::List` / `Value::Map`. |
| **S9** | `cel_make_message` + `cel_set_field`. Message-literal construction; adapter `MakeContainer` / `SetField`. |

S3.5 and S4.5 are the new slices this draft introduces vs design.md.
Both are single-day and unlock the rest.

## 10. Open questions

1. **Does `Env` own the `HostAdapter`, or does `Instance`?**
   If `Env` owns, all `Instance`s share one adapter — good for
   stateless adapters (proto), awkward for stateful ones (a JSON
   cache keyed by request). Recommend: **Env owns by default, with
   an `Instance`-level override hook** for stateful adapters.

2. **Should `Value::Message(const Message&)` copy, or borrow?**
   cel-cpp's `CelValue` borrows. That forces the caller to keep
   the message alive for the duration of `Eval`. Easy to get wrong.
   Alternative: copy defensively, accept the cost. Recommend:
   **borrow; document the lifetime; provide `Value::OwnedMessage`
   for the copying case**. Matches cel-cpp, predictable.

3. **`Activation::BindLazy` — is the value re-fetched across
   multiple `Eval`s on the same `Instance`?** If yes, lazy =
   per-`Find` call; if no, lazy = first `Find` caches. Recommend:
   **per-`Eval` caching, clear on `Reset`.** Matches developer
   intuition ("same eval sees the same value").

4. **Should `Value::List` / `Value::Map` materialise the wire list
   eagerly, or stream?** Eager is simple; streaming avoids
   blowing up on large lists. Recommend: **streaming view
   (`ListView` / `MapView`) in the API, eager copy only when the
   user explicitly asks for `ToVector` / `ToMap`.**

5. **Does `JsonHostAdapter` own a JSON implementation, or abstract?**
   Owning picks winners (nlohmann vs rapidjson). Abstract makes it
   one more interface. Recommend: **abstract — ship one concrete
   subclass per common library under `adapters/json_*.{h,cc}`**.
   Embedders pick the one matching their existing JSON lib.

6. **Is `AttributeId` resolved eagerly (attribute_id = compile-time
   int) or lazily (attribute_id = string resolved at partial-eval
   time)?** Eager = fast; requires full attribute table in ABI.
   Lazy = flexible; allows runtime-declared unknowns. Recommend:
   **eager for declared attributes (common case), plus a fallback
   string path for runtime-synthesised attribute ids (rare).**

7. **Does `Instance` always implicitly `Reset` between `Eval`s, or
   does the caller control it?** Implicit is safer, explicit
   allows reuse of arena state across back-to-back evals (rare).
   Recommend: **implicit, with an opt-out
   `Instance::EvalWithoutReset`** for the advanced case.

## 11. Non-goals for this draft

Called out explicitly so reviewers don't ask:

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
2. Is the `HostAdapter` interface the right cut between runtime
   and embedder? Or should the runtime know more?
3. Are the 3VL absorption rules in §4.2 the ones we want, or is
   there a shorter formulation?
4. Does the ABI schema in §6 preserve the v1 self-contained
   property to your satisfaction, or did we still trim something?
5. Which of the open questions in §10 should be decided before
   promotion; which can be deferred?

Once settled, this draft graduates to `cel-host-design.md` (no
`.draft.` infix), slices are added to `design.md` §11, and the
testing checklist gets new rows.
