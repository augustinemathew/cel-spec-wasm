# cel_host surface (compiler_v2)

**Status:** Committed 2026-04-22. Authoritative for the public user
API, the `cel.abi` schema, and every wasm callback signature into
the host. Complements `design.md`, which owns the runtime/codegen
internals; where the two touch (ABI fields, callback shapes, slice
plan for the api/ tier), this doc wins and `design.md` defers.

This doc scopes the entire host surface a developer touches when
embedding a compiled CEL expression. It covers:

- The **user-facing abstractions** (`Compiler` / `Program` /
  `Instance` / `Value` / `Activation` + `RuntimeBindings`) —
  the ten-line path from source → answer.
- The **`cel.abi` custom section** — v1's self-contained contract,
  extended.
- **`Value::Unknown` and `Value::Error` at the surface** — first-
  class values with real payloads. (Absorption semantics are the
  runtime's job, in `design.md` §4; this doc exposes only the
  representation.)
- The **internal host adapter** — the proto-backed implementation
  that sits behind the `cel_host.*` wasm imports. One impl today;
  private class; not a user-facing knob.

## 1. Guiding principles

These are non-negotiable for v2. They drive every shape below.

### 1.1 The adapter is internal; `Value` is the user-facing polymorphism

Users interact with `Compiler` / `Program` / `Instance` /
`Value` / `Activation` (and `RuntimeBindings` when they have
custom functions). They bind `Value`s to names; they read
`Value`s out of `Eval`. They never see an adapter.

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
ASSIGN_OR_RETURN(auto compiler, Cel::Compiler::NewBuilder()
    .AddStandardDeclarations()
    .DeclareVariable("x", CelType::Int())
    .Build());
ASSIGN_OR_RETURN(auto engine, Cel::Engine::NewBuilder().Build());
ASSIGN_OR_RETURN(auto program, compiler.Compile("x + 1"));
ASSIGN_OR_RETURN(auto instance, engine.Plan(program));    // M5+ adds bindings

Activation act;
act.Bind("x", Value::Int(41));
ASSIGN_OR_RETURN(auto result, instance.Eval(act));
std::cout << result.AsInt().value();    // 42
```

An intern should grasp `Compiler` / `Program` / `Instance` in
one sitting. Anything fancier (partial eval, custom functions)
is opt-in.

## 2. User-facing abstractions

Four classes, one struct, two value leaves.  The split is
role-based: compile-time concerns (declarations, type checking,
codegen) live on `Compiler`; runtime concerns (wasm execution,
instances, evaluation) live on `Engine`.  `Program` is the
serialization boundary between them.

```
COMPILE-TIME                        RUNTIME
────────────                        ───────
Cel::Compiler                       Cel::Engine
   │                                   │
   │ Compile(source, opts)             │ Plan(program, bindings)
   ▼                                   ▼
Cel::Program  ──────────────────►  Cel::Instance
(bytes + ABI; serializable)            │
                                       └──Eval(Activation)──► Value
```

Lifecycle:

- `Compiler` — compile-time configuration. Holds function
  **declarations** (signatures, no impls), variable
  declarations, and the compile-time descriptor pool. Immutable
  after `Build()`. One `Compiler` can produce many `Program`s
  in parallel; it holds no wasmtime state and no per-eval state.
  A Compiler can run in a build server that never executes
  anything.
- `Program` — the compiled artifact. Holds wasm bytes + parsed
  ABI; **no wasmtime engine reference**. Serializable (persist to
  disk, re-load later, ship to another process).  An `Engine` is
  required to evaluate it; a Compiler is not.  Decoupled from
  impls — you can ship a `Program` to another process that has
  different impls bound to the same overload ids.
- `Engine` — process-shared wasmtime fixture. Owns the
  `wasm_engine_t` and the parsed `cel_runtime.wasm` module
  (cached once at `Build` time, reused across every `Plan`).
  Thread-safe; one `Engine` per process or per tenant. Bench-
  justified: caching the parsed runtime gives a ~34× per-Plan
  speedup; sharing the engine gives another ~64× cold-to-hot.
- `RuntimeBindings` — runtime-side configuration. Holds function
  **impls** (keyed by overload id) and the runtime descriptor
  pool. Supplied to `Engine::Plan(program, bindings)` when
  constructing an `Instance`. A plain struct, not a class; build
  as a literal at the call site.
- `Instance` — the live evaluator. Holds the wasmtime store +
  instance + per-instance state (mutable-message registry,
  externref table) plus a `shared_ptr` back to the Engine's
  WasmtimeEngineState — so an Instance can outlive the Engine
  handle that built it. Thread-owned; reused across `Eval` calls
  with automatic `cel_reset` between. One `Program` × one
  `Engine` can yield many `Instance`s (multi-threaded use; swap
  bindings between Plans).

### 2.1 `Cel::Compiler`

Compile-time configuration. Contains **no runtime state** —
declarations, types, and the compile-time descriptor pool only.

```cpp
// compiler_v2/api/compiler.h
namespace cel {  // public top-level namespace; internal machinery
                 // stays in `celwasm::`.

class Compiler {
 public:
  class Builder;
  static Builder NewBuilder();

  // Compile a CEL source string to a Program.  Runs parse → check
  // → lower → assemble; returns a Program that can be evaluated or
  // serialized.  The Compiler is captured by reference, so it must
  // outlive all Programs it compiles.  `opts` are per-compilation
  // tunables (arena size, debug layout, stdlib-overload allowlist,
  // …) — NOT declarations.  See §5.4.
  absl::StatusOr<Program> Compile(
      absl::string_view source,
      CompilerOptions opts = {}) const;

  // Introspection — declarations visible to this Compiler.
  absl::Span<const FunctionDecl> declared_functions() const;
  absl::Span<const VariableDecl> declared_variables() const;
  const google::protobuf::DescriptorPool& descriptor_pool() const;
};

class Compiler::Builder {
 public:
  // Seed with the CEL standard library + the Messages/Durations/
  // Timestamps type set.  Almost every caller starts here.
  Builder& AddStandardDeclarations();

  // Variable declarations — what free variables expressions can use.
  Builder& DeclareVariable(std::string name, CelType type);

  // Custom function declaration.  Signature-only: name, overload_id,
  // is_receiver, arg_types, return_type.  NO impl — impls live in
  // RuntimeBindings and are supplied at Plan time.  See §5.1.
  Builder& RegisterFunction(FunctionDecl decl);

  // Type registration — proto descriptors the compiler may reference.
  Builder& RegisterMessageType(const google::protobuf::Descriptor* desc);

  absl::StatusOr<Compiler> Build() &&;
};

// Compiler tunables — per-compilation, NOT per-Compiler.  Passed
// to compiler.Compile(source, opts).  Declarations (variables /
// functions / types) live on Compiler; opts only tunes how a
// specific expression is lowered.  See §5.4 for the flow.
struct CompilerOptions {
  uint32_t arena_pages = 16;                 // initial linear-memory
                                             //   pages for the arena.
  bool debug_layout = false;                 // disable slot reuse so
                                             //   per-expr values stay
                                             //   distinct in memory.
  std::vector<std::string> allowed_overloads;  // if non-empty, stdlib
                                             //   overloads outside this
                                             //   set fail with
                                             //   Unimplemented at
                                             //   compile time.
  // (More tunables land here over time.  Keep decls out.)
};

}  // namespace cel
```

### 2.2 `Cel::Program`

The compiled artifact. Pure data: wasm bytes + (future) parsed
ABI. Serializable; **no wasmtime engine reference**; safe to copy
across process boundaries.

A `Program` is constructed by `Compiler::Compile(source)` (the
standard path) or by `Program(wasm_bytes)` (the cross-process
load path — bytes shipped from elsewhere, e.g. a cache).
Construction is pure-data — it does **not** parse the wasm
through wasmtime.  An `Engine` is required to evaluate; the
wasmtime parse happens inside `Engine::Plan(program)`.

```cpp
// compiler_v2/api/program.h
namespace cel {

class Program {
 public:
  // Construct a Program directly from wasm bytes.  Used by both
  // `Compiler::Compile` (compiled in-process) and the cross-
  // process load path (bytes shipped from elsewhere).  No
  // validation of the bytes; the wasmtime parse happens later in
  // `Engine::Plan`.
  explicit Program(std::vector<uint8_t> wasm_bytes);

  // Pure-data type: copyable + movable.  No external resources.

  // Introspection — what does this program need to evaluate?
  absl::Span<const uint8_t> wasm_bytes() const;
  // (Future fields — parsed Abi for declared_variables /
  // required_imports / source — land with the milestones that
  // populate them.)
};

}  // namespace cel
```

### 2.2.5 `Cel::Engine`

Runtime fixture.  Owns the `wasm_engine_t` and parsed
`cel_runtime.wasm` module — both wasmtime-thread-safe per upstream
docs and shareable across all `Plan` calls.  Process-shared
typically; per-tenant in multi-tenant hosts.

```cpp
// compiler_v2/api/engine.h
namespace cel {

class Engine {
 public:
  class Builder;
  static Builder NewBuilder();

  // Build an Instance from a Program, ready for evaluation.  Hot
  // path: store + memory + linker + bind cel.memory + instantiate
  // runtime + bind runtime exports + parse expr bytes via
  // wasmtime_module_new + instantiate expr + lookup eval.  Plan
  // re-parses the expr bytes per call for M1; an Engine-side
  // expr-module cache is the named follow-up if profiles demand
  // it.
  //
  // Safe to call concurrently from multiple threads — each call
  // creates a fresh store + linker + memory; only the engine +
  // parsed runtime module are shared (and both are documented
  // thread-safe).
  absl::StatusOr<Instance> Plan(const Program& program) const;
};

class Engine::Builder {
 public:
  // Materialises the Engine.  Allocates the wasm engine and
  // parses cel_runtime.wasm into a wasmtime_module_t.
  absl::StatusOr<Engine> Build() &&;
};

}  // namespace cel
```

Future `Plan(const Program&, const RuntimeBindings&)` overload
adds custom-impl binding when M5+ user functions land.  M1's
single-arg form is forward-compatible with that.

### 2.3 `Cel::Instance`

The live evaluator. Not serializable. Single-threaded (bind one
per thread for concurrency).

```cpp
// compiler_v2/api/instance.h
namespace cel {

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

  // Back-reference to the Program that built this Instance.
  const Program& program() const;

  // Explicit reset — normally automatic between Eval calls.
  void Reset();
};

}  // namespace cel
```

### 2.4 `Cel::RuntimeBindings`

Runtime-side config. Built with chainable methods and supplied to
`Engine::Plan(program, bindings)`.

```cpp
// compiler_v2/api/runtime_bindings.h
namespace cel {

class RuntimeBindings {
 public:
  // Fresh empty bindings.  Descriptor pool defaults to the
  // generated pool (the process's statically-linked proto set);
  // override with SetDescriptorPool if the runtime uses a
  // different pool than the one the Compiler saw.
  RuntimeBindings();

  // —— Chainable mutators (fluent construction) ——
  RuntimeBindings& SetDescriptorPool(
      const google::protobuf::DescriptorPool* pool);

  // Bind a custom overload's impl.  `overload_id` must match the
  // one declared on the Compiler (§5.1) and recorded in
  // cel.abi.functions.host_custom_imports[].overload_id.  Redundant
  // Binds under the same overload_id → AlreadyExists; extras not
  // referenced by the Program → ignored at Plan time.
  RuntimeBindings& AddFunction(
      std::string overload_id, FunctionImpl impl);

  // Seed impls for every host-backed stdlib function (e.g. the
  // time-zone-aware timestamp helpers, regex helpers) that the
  // runtime module expects on the host side.  Most stdlib lives in
  // the runtime .wasm itself and needs no binding; this helper
  // covers the minority that does.  Named so users don't reinvent
  // the list.
  RuntimeBindings& AddStdlibFunctions();

  // —— Introspection ——
  const google::protobuf::DescriptorPool* descriptor_pool() const;
  const FunctionImpl* absl_nullable Find(
      absl::string_view overload_id) const;
  std::vector<absl::string_view> BoundOverloads() const;

 private:
  const google::protobuf::DescriptorPool* descriptor_pool_;
  absl::flat_hash_map<std::string, FunctionImpl> function_impls_;
};

}  // namespace cel
```

Usage is fluent at the `Plan` call site:

```cpp
ASSIGN_OR_RETURN(auto instance, engine.Plan(program,
    RuntimeBindings()
        .SetDescriptorPool(my_pool)
        .AddFunction("my_upper_string", upper_impl)
        .AddFunction("my_lower_string", lower_impl)));
```

…or retained for reuse across multiple `Plan` calls (different
`Instance`s under the same bindings, typically one per thread):

```cpp
RuntimeBindings bindings;
bindings.SetDescriptorPool(pool)
        .AddFunction("my_upper_string", upper_impl);
ASSIGN_OR_RETURN(auto instance_a, engine.Plan(program, bindings));
ASSIGN_OR_RETURN(auto instance_b, engine.Plan(program, bindings));
```

### 2.5 `Value`

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

### 2.6 `Activation`

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
4. `RuntimeBindings` gains an opt-in hook to override the
   default (an adapter pointer alongside `function_impls`).

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

## 4. `Value::Unknown` / `Value::Error` in the user surface

**Absorption is not this layer's concern.** The compiler emits
wasm that applies the spec's 3VL absorption rules (`langdef.md`
§3VL) at every operator, short-circuit site, and `has()`. A custom
function's trampoline only ever hands it concrete, non-3VL args
(absorbed before dispatch); a custom function can *produce*
`UNKNOWN` / `ERROR`, and the surrounding emitted code absorbs it
through the rest of the expression. None of that logic is
configurable from this doc's surface — it ships in the runtime.

What *does* live at this layer is the representation of the two
3VL kinds as first-class `Value`s.

### 4.1 `UNKNOWN` — payload is `AttributeId`

```cpp
// compiler_v2/api/attribute.h
namespace cel {

struct AttributeId {
  // Dense index into cel.abi.attributes[].  Resolves to
  // (variable, qualifiers[]) — e.g. "request.auth.claims".
  uint32_t id;
};

struct AttributePattern {
  // Textual pattern like "request.*" or "user.profile.email".
  // Parsed by ParseAttributePattern; matched against
  // AttributeIds during partial eval.
  std::string pattern;
};

}  // namespace cel
```

`Instance::PartialEval` takes a list of `AttributePattern`s; the
runtime compares each root-variable / field-read attribute path
against them and returns `Value::Unknown(AttributeId{…})` when
matched. From that point on, the compiler-emitted code handles
propagation.

Custom functions may also produce `Value::Unknown(...)` for
their own reasons (e.g. "this value needs a DB lookup I haven't
done yet"). That behaves identically.

### 4.2 `ERROR` — payload is `(code, message, expr_id)`

```cpp
// compiler_v2/api/error.h
namespace cel {

struct ErrorPayload {
  enum Code {
    kOverflow, kDivideByZero, kModulusByZero, kTypeMismatch,
    kFieldNotFound, kKeyNotFound, kIndexOutOfBounds,
    kUnknownType, kCustomFnFailed, kHostAdapterError, kTimeout,
    // …
  };
  Code code;
  std::string message;
  uint32_t expr_id;             // index into CheckedExpr.source_info
                                //   for the originating subexpression.
};

}  // namespace cel
```

On-wire representation fits in the 16-byte `CelValue.payload`:
`{code: u16, _pad: u16, expr_id: u32, msg_off: u32, msg_len: u32}`.
The message bytes live in the arena (allocated via `cel_alloc`).

### 4.3 Producing / inspecting 3VL in user code

A custom function's `impl` can freely return a 3VL `Value`:

```cpp
RuntimeBindings()
    .AddFunction("my_find_user_string",
        [](absl::Span<const Value> args) -> Value {
          auto id = args[0].AsString();
          if (!id.ok()) return Value::Error({
              ErrorPayload::kTypeMismatch,
              std::string(id.status().message()),
              /*expr_id=*/0});
          if (!UserCached(*id)) {
              // Tell partial eval we need this input fetched.
              return Value::Unknown(
                  AttributeId{/*id=*/SomeAttrIdFor("user")});
          }
          return Value::Message(LoadUser(*id));
        });
```

At the call site, callers inspect `Value`:

```cpp
if (result.IsError()) {
  auto info = result.ErrorInfo().value();
  LOG(ERROR) << "cel failed: " << info->message
             << " at expr " << info->expr_id;
} else if (result.IsUnknown()) {
  // Partial eval path: fetch the attribute, re-Eval.
}
```

**That is the whole host-surface story for 3VL.** The how-it-
propagates-through-operators table belongs in
`doc/implementation-plan/rewrite/design.md` §4 (runtime helpers),
not here.

## 5. Custom functions

Per `design.md` §4.6, customs land as per-function wasm imports
under `"cel_host"`. The surface-facing shape is higher-level:

### 5.1 `FunctionDecl` (signature only)

One `FunctionDecl` describes **one overload signature**. cel-cpp's
`FunctionDecl` groups multiple `OverloadDecl`s under one name;
we flatten so each row maps 1:1 to an `OverloadTable` entry and
to one `CustomFunctionEntry` in the ABI. **No impl** — impls
live in `RuntimeBindings` (§2.4), keyed by `overload_id`.

```cpp
// compiler_v2/api/function.h
namespace cel {

struct FunctionDecl {
  // CEL-level call name.  What appears in source: "my.upper",
  // "size", "_+_" (the canonical form of the `+` operator).
  std::string name;

  // Globally-unique overload id, matches cel-cpp's OverloadDecl
  // id.  e.g. "my_upper_string", "add_int_int".  Same overload
  // id declared twice → AlreadyExists.
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
};

// Impl provided separately at Plan time via RuntimeBindings.
// Receives boxed Values in arg_types order; returns a boxed
// Value.  Can return Value::Unknown / Value::Error for 3VL
// semantics (rarely needed — see §5.3).
using FunctionImpl = absl::AnyInvocable<
    Value(absl::Span<const Value> args) const>;

}  // namespace cel
```

The `FunctionImpl` takes boxed `Value`s, not raw i32 offsets. The
trampoline (auto-generated by `RegisterCelHost`) handles boxing /
unboxing. Two-step: declare at compile time, bind at Plan time.

```cpp
// Step 1 — compile-time declaration on the Compiler.
compiler_builder.RegisterFunction(FunctionDecl{
    .name = "my.upper",
    .overload_id = "my_upper_string",
    .is_receiver = false,
    .arg_types = {CelType::String()},
    .return_type = CelType::String(),
});

// Step 2 — runtime impl, supplied to Plan.
RuntimeBindings bindings;
bindings.AddFunction("my_upper_string",
    [](absl::Span<const Value> args) -> Value {
      auto s = args[0].AsString();
      if (!s.ok()) return Value::Error({
          ErrorPayload::kTypeMismatch,
          std::string(s.status().message()), 0});
      std::string upper(s->begin(), s->end());
      absl::AsciiStrToUpper(&upper);
      return Value::String(std::move(upper));
    });

// Receiver-style declaration looks identical:
compiler_builder.RegisterFunction(FunctionDecl{
    .name = "upper",
    .overload_id = "string_upper",
    .is_receiver = true,
    .arg_types = {CelType::String()},    // receiver == arg 0
    .return_type = CelType::String(),
});
bindings.AddFunction("string_upper", /* same impl */);
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

Two inputs flow into `compiler.Compile(source, opts)`:
**declarations from the `Compiler`** (functions, variables,
types) and **`CompilerOptions` from the call site** (tunables
only, no declarations). Impls are not involved at compile time
at all — they live on `RuntimeBindings` and enter the system
at `Plan`.

```
Compiler::Builder::RegisterFunction(FunctionDecl d)
    │                                  (signature only — no impl)
    ▼
Compiler::declared_functions()  ◄──── frozen at Builder::Build()
    │
    │          ┌─────────────────────────────────────────────────┐
    │          │  compiler.Compile(source, CompilerOptions opts) │
    │          └─────────────────────────────────────────────────┘
    ▼                                  ▼
FrozenDeclView (already                opts (arena_pages,
    │     signature-only — no impl            debug_layout,
    │     field even exists)                    allowed_overloads, …)
    │                                          │
    ├──► cel-cpp TypeCheckerBuilder  ◄─────────┤ (filters stdlib
    │       .AddFunction                       │   overloads,
    │    (resolves "my.upper(x)" →             │   etc.)
    │     overload id "my_upper_string",       │
    │     is_receiver = false)                 │
    │                                          │
    └──► Codegen / ABI emitter       ◄─────────┘ (picks arena
            │                                     layout, slot
            ▼  UsedImports(used_overload_ids)     reuse policy)
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

**`CompilerOptions` lives at the call site, not on `Compiler`.**
Two `Compile()` calls against the same `Compiler` may pass
different options (e.g. production vs debug-layout); the
`Compiler` doesn't care, and users aren't forced to rebuild
it just to flip a knob. Declarations are stable; tunables are
per-compilation.

**Cross-checked tunables go into the ABI, not into
`CompilerOptions`.** If a knob needs to be honoured at
LoadEval (e.g. arena page count, which the host must match
when instantiating memory), codegen writes it into
`cel.abi.layout` from the `CompilerOptions` input. The host
reads the ABI; it never sees `CompilerOptions` directly.

**Impls never reach the Compiler.** A `FunctionDecl` is a pure
signature. Impls enter the system at `Engine::Plan(program, bindings)`,
where `RuntimeBindings::function_impls` maps overload_id →
`FunctionImpl`. This means one `Compiler` can `Compile` many
programs in parallel without any per-eval state, and a
serialized `Program` can travel to a different process that
wires its own impls at load time.

### 5.5 Cross-check at `Plan`

When `engine.Plan(program, bindings)` runs, the host walks
`cel.abi.functions.host_custom_imports[]` and for each
`CustomFunctionEntry e`:

1. Look up `bindings.function_impls[e.overload_id]`. Missing →
   `FailedPrecondition("no impl bound for overload_id '<id>' "
   "(required by function '<function_name>')")`.
2. (The ABI already carries `function_name` / `is_receiver` /
   `arg_types` / `return_type` from compile time, so there's
   no separate "signature check against the registry" step —
   bindings hold impls only. Signature correctness was proven
   at compile time; the impl is opaque.)
3. Bind the wasm import `cel_host.<e.helper_name>` to a
   per-overload trampoline that boxes args per `e.arg_types`
   and writes the result per `e.return_type`, invoking
   `bindings.function_impls[e.overload_id]`.

`Program::CheckCompatible(bindings)` runs just step 1 for every
entry without touching wasmtime — useful when you want to fail
fast in a deployment pipeline before trying to instantiate.

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
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/value.h"

using ::cel::Compiler;
using ::cel::RuntimeBindings;
using ::cel::Value;
using ::cel::Activation;
using ::cel::CelType;

absl::StatusOr<int64_t> AddOne(int64_t x) {
  ASSIGN_OR_RETURN(auto compiler, Compiler::NewBuilder()
      .AddStandardDeclarations()
      .DeclareVariable("x", CelType::Int())
      .Build());
  ASSIGN_OR_RETURN(auto program, compiler.Compile("x + 1"));
  ASSIGN_OR_RETURN(auto instance, engine.Plan(program));  // no customs

  Activation act;
  act.Bind("x", Value::Int(x));
  ASSIGN_OR_RETURN(auto result, instance.Eval(act));
  return result.AsInt();
}
```

### 7.2 Proto message

```cpp
absl::StatusOr<std::string> FullName(const Customer& c) {
  ASSIGN_OR_RETURN(auto compiler, Compiler::NewBuilder()
      .AddStandardDeclarations()
      .RegisterMessageType(Customer::GetDescriptor())
      .DeclareVariable("c", CelType::Message("com.example.Customer"))
      .Build());
  ASSIGN_OR_RETURN(auto program,
      compiler.Compile("c.first_name + ' ' + c.last_name"));

  ASSIGN_OR_RETURN(auto instance, engine.Plan(program,
      RuntimeBindings().SetDescriptorPool(
          google::protobuf::DescriptorPool::generated_pool())));

  Activation act;
  act.Bind("c", Value::Message(c));
  ASSIGN_OR_RETURN(auto result, instance.Eval(act));
  return std::string(result.AsString().value());
}
```

### 7.3 Custom function

```cpp
// Compile-time: declare the signature.
ASSIGN_OR_RETURN(auto compiler, Compiler::NewBuilder()
    .AddStandardDeclarations()
    .DeclareVariable("s", CelType::String())
    .RegisterFunction({
        .name = "my.upper",
        .overload_id = "my_upper_string",
        .is_receiver = false,
        .arg_types = {CelType::String()},
        .return_type = CelType::String(),
    })
    .Build());

ASSIGN_OR_RETURN(auto program, compiler.Compile("my.upper(s)"));

// Runtime: bind the impl.
ASSIGN_OR_RETURN(auto instance, engine.Plan(program,
    RuntimeBindings().AddFunction("my_upper_string",
        [](absl::Span<const Value> args) -> Value {
          auto s = args[0].AsString();
          if (!s.ok()) return Value::Error({
              ErrorPayload::kTypeMismatch,
              std::string(s.status().message()), 0});
          std::string u(s->begin(), s->end());
          absl::AsciiStrToUpper(&u);
          return Value::String(std::move(u));
        })));

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
// Server-side (compiler process).  Ship the raw .wasm bytes.
auto wasm_bytes = program.wasm();
Persist(wasm_bytes);                // to disk, to cache, to S3

// Later / elsewhere (runner process — no Compiler needed; only an
// Engine is required to evaluate).
auto wasm_bytes = Load();
cel::Program program(std::move(wasm_bytes));
ASSIGN_OR_RETURN(auto engine, cel::Engine::NewBuilder().Build());

// Wire impls for whatever custom overloads the ABI declares.
RuntimeBindings bindings;
bindings.SetDescriptorPool(MyPool());
for (const auto& fn : program.abi().functions().host_custom_imports) {
  bindings.AddFunction(fn.overload_id, LookUpMyImpl(fn.overload_id));
}
RETURN_IF_ERROR(engine.CheckCompatible(program, bindings));

ASSIGN_OR_RETURN(auto instance, engine.Plan(program, bindings));
// ... Eval as before.
```

No `Compiler` at the runner side — `Program(bytes)` reconstructs
the artifact from raw bytes, `Engine::CheckCompatible(program,
bindings)` proves the bindings cover every declared custom, and
`Engine::Plan(program, bindings)` wires the trampolines.  An
`Engine` is required (one per process), but a Compiler is not.
A runner that doesn't share source or declarations with the
compiler can still evaluate.

## 8. File layout

```
compiler_v2/
├── api/                            # user-facing public headers
│   ├── BUILD.bazel
│   ├── compiler.{h,cc,_test.cc}    # Cel::Compiler + Builder +
│   │                               #   CompilerOptions (no wasmtime)
│   ├── program.{h,_test.cc}        # Cel::Program (bytes + ABI;
│   │                               #   header-only, Program(bytes) ctor)
│   ├── engine.{h,cc,_test.cc}      # Cel::Engine + Builder; owns
│   │                               #   wasm_engine_t + parsed
│   │                               #   cel_runtime.wasm; Plan(program)
│   │                               #   → Instance
│   ├── instance.{h,cc,_test.cc}    # Cel::Instance + Eval
│   ├── value.{h,cc,_test.cc}
│   ├── activation.{h,cc,_test.cc}
│   ├── type.{h,cc,_test.cc}        # CelType
│   ├── attribute.{h,cc,_test.cc}   # AttributeId / AttributePattern
│   ├── error.{h,cc,_test.cc}       # ErrorPayload + ErrorCode
│   ├── cel_pipeline_bench.cc       # per-stage cost benches
│   └── internal/
│       ├── wasmtime_engine_state.{h,cc}  # engine + parsed runtime
│       └── instance_impl.{h,cc}          # per-Plan wasmtime handles
│
│   Planned but not yet shipped:
│     - runtime_bindings.{h,cc,_test.cc}  # RuntimeBindings struct
│       + RegisterCustomImports hook on Engine::Plan (M5+)
│     - function.h                        # FunctionDecl / FunctionImpl
│       (M5+ custom function declaration / impl types)
├── host/
│   ├── BUILD.bazel
│   └── cel_log.{h,cc,_test.cc}     # already shipped — host-side
│                                   #   cel_env.cel_log trampoline +
│                                   #   format-string decoder
└── …

Files **planned but not yet shipped** under `host/` (named here so
the future shape is visible; each lands with the milestone that
needs it):

  - `cel_abi.proto` + `cel_abi.{h,cc}` — §6 schema, build-side
  - `abi_parse.{h,cc,_test.cc}` — runtime-side deserialize
  - `cel_host.{h,cc,_test.cc}` + `cel_host_3vl.{cc,h}` +
    `cel_host_wasmtime.{h,cc}` — proto-backed adapter +
    UNKNOWN/ERROR absorption + `RegisterCelHost` trampolines
  - `custom_registry.{h,cc,_test.cc}` — M5+ user-function impl
    registry, walked by Engine::Plan when wiring custom imports

`host/host_loader.{h,cc}` was removed in the runtime-isolation
work — its role split across `api/engine` + `api/instance`.
```

The `api/` tier is the public surface. The `host/` tier is machinery.
An embedder should almost never need to `#include` anything under
`host/` — `api/compiler.h`, `api/program.h`, `api/instance.h`,
`api/runtime_bindings.h`, `api/value.h`, `api/activation.h` should
suffice for 95% of use cases.

## 9. Per-slice landing plan

Maps onto `design.md` §11.4. One new slice (**S3.5**) carves out the
api/ tier separately so it lands before the first slice that needs
it (S4, first proto reads).

| Slice | Additions |
|---|---|
| **S3.5** | `api/value.{h,cc}` + `api/activation.{h,cc}` + `api/compiler.{h,cc}` skeleton + `api/program.{h,cc}` + `api/runtime_bindings.{h,cc}` + `api/instance.{h,cc}`. Wires the existing (scalar-only) pipeline through the public surface; `Compiler::Compile` / `Engine::Plan(program, bindings)` / `Instance::Eval` work for `-e "42"`. |
| **S4** | `cel_host.{h,cc}` proto-backed adapter + fixed host imports (`cel_get_field`, `cel_has_field`). ABI v2 schema lands; `abi_parse` reads everything. Attribute table reserved but not yet populated. Root variables (scalars + proto messages) end-to-end. |
| **S4.5** | `AttributePattern` + partial eval on root variables. Attribute table populated by checker. `Instance::PartialEval` lands. |
| **S6** | `cel_message_eq`. ERROR payload grows `expr_id` field. |
| **S7** | Custom functions — `FunctionDecl` + `RegisterCelHost` customs loop + `FunctionImpl` auto-boxing trampolines. |
| **S8** | List + map literals at the wasm level. `Value::List` / `Value::Map` on the user surface. No new adapters. |
| **S9** | `cel_make_message` + `cel_set_field` in the proto adapter. Message-literal construction wired end-to-end. |

S3.5 and S4.5 are the new slices this draft introduces vs design.md.
Both are single-day and unlock the rest.

## 10. Decisions

All resolved 2026-04-22.

- **Q1. Adapter ownership.** The adapter is internal to `host/`
  and not part of the public surface (§1.1, §3). The `Compiler`
  holds compile-time declarations (variables, functions, types,
  descriptor pool); the `Instance` holds the live wasmtime plumbing
  plus the adapter. Stateful per-eval work (externref table,
  mutable-message registry) lives on the `Instance`. Impls enter
  via `RuntimeBindings` at `Plan` time.
- **Q2. `Value::Message(const Message&)` copy vs borrow.**
  **Borrow.** Matches cel-cpp's `CelValue` conventions. Lifetime
  documented on the function. `Value::OwnedMessage(unique_ptr)` is
  the copying alternative.
- **Q3. `Activation::BindLazy` caching.** **Per-`Eval` caching,
  cleared on `Reset`.** Same `Eval` call sees the same value;
  subsequent `Eval`s re-invoke the lazy binder. Matches developer
  intuition and keeps surprises out of multi-call Instance reuse.
- **Q4. `Value::List` / `Value::Map` materialisation.**
  **Streaming view.** `Value::AsList()` / `AsMap()` return
  `ListView` / `MapView` handles that fetch elements on demand.
  Explicit `ToVector` / `ToMap` methods materialise eagerly when
  the user asks. Avoids O(N) boxing on every access to large
  aggregates.
- **Q5. `AttributeId` resolution.** **Eager.** Every attribute
  referenced by the expression is interned into
  `cel.abi.attributes[]` at compile time with a dense `uint32_t`
  id. `Value::Unknown` payload is an `AttributeId`, not a string.
  Runtime-synthesised (embedder-declared) attributes are a
  non-goal; if a custom wants to signal UNKNOWN on an attribute
  the checker didn't see, it returns an `AttributeId` pointing at
  a reserved "opaque host-chosen" id. Kept simple; add a string
  path later if real usage forces it.
- **Q6. Implicit `Reset` between `Eval`s.** **Always reset.** No
  `EvalWithoutReset`. Back-to-back `Eval`s on the same `Instance`
  each start with a fresh arena + externref table. Simpler
  lifetime model; no subtle bugs from arena carry-over.

## 11. Non-goals for this draft

Called out explicitly so reviewers don't ask:

- **Non-proto backings (JSON, map, user-defined structs).** The
  ABI leaves room (§3.3, §6.1); the v2 public surface only binds
  proto. If / when a second backing is real, §3.2 describes the
  four-step path to add it. Not attempting it speculatively.
- **Thread safety of `Compiler`.** Immutable after `Build`;
  read-only. Thread-safe by construction. `RuntimeBindings` is
  a value type, copyable per-thread. Only `Instance` is
  single-threaded. Not discussed further.
- **Serialization of `Value` itself.** Users serialize via proto /
  JSON / their own format. `Value` is in-process only.
- **Hot-reload of `Program`.** Re-compile and swap `Instance`s. No
  dynamic patching.
- **Pluggable arena allocators.** The arena is the arena. Size
  configurable via `CompilerOptions`; algorithm is bump.
- **`CompilerOptions` details.** Important but orthogonal; lives in
  a separate doc when we get there.

## 12. Pointers

- `design.md` — runtime / codegen internals, memory model,
  ResolvePass + LayoutPass, runtime helper catalogue. This doc's
  §4.1 "absorption rules" intentionally absent — see `design.md`
  §4 for the runtime's 3VL machinery.
- `testing-checklist.md` — per-slice test rows. When a slice from
  §9 ships, the matching rows flip.
- `third_party/cel-cpp/common/standard_definitions.h` — canonical
  source of `overload_id` constants for stdlib functions.
