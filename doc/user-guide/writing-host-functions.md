# Writing host functions (`@host.`)

> Part of the [CEL → WebAssembly user guide](index.md). This is the deep-dive
> page for **host functions** — CEL functions whose body is your C++, called
> back at eval time. Host callbacks are the only custom-function mechanism:
> they run as native code in your process, not in the wasm sandbox
> ([security model](security-model.md)). The declaration side (the `.celfn`
> IDL, function libraries) is on [Custom functions](custom-functions.md).

A host function is declared with the `@host.` prefix, type-checked at compile
time, and backed by a C++ callback registered on the `Engine`. Three
registration surfaces, highest-level first:

| API | You work in | Use when | Status |
|---|---|---|---|
| **Declaration-first** — `BindFunction` | the `.celfn` decl string + a plain typed lambda | **start here** — same decl string both sides, no overload-id spelling, signature validated at registration | ✅ shipped |
| **Typed** — `AddTypedFunction` | plain C++ types + a hand-spelled overload id | you already have the synthesized overload id | ✅ shipped |
| **Context** — `HostCallContext&` | typed accessors (`ctx.ArgInt(0)`, `ctx.ReturnProto(...)`) | you need per-arg control or dynamic arity | ✅ shipped |
| **Raw** — `HostCallback` | n/a — there is no raw callback anymore | — | removed |

Status legend (as in the index): ✅ shipped + tested; 🟡 designed, not yet
wired; ⛔ not started. Every `@host` callback receives a typed context — there
is no `memcpy`-the-slots form.

---

## 1. The typed API — `BindFunction` / `AddTypedFunction` (recommended)

You write a normal C++ lambda over **canonical CEL types**; the binding
decodes each argument, calls you, and encodes the result. No slots, no
`memcpy`, no kind-checking by hand.

**Declaration-first (`BindFunction`)** is the recommended shape: one `.celfn`
string drives both sides, and the lambda's signature is validated against the
declaration at registration — an arity or type mismatch fails right there
with the parameter index and both types named, never at eval:

```cpp
constexpr absl::string_view kDecl = "int @host.double_it(int x);";

// Declare it (compile side) so call sites type-check:
auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("x", celwasm::CelType::Int());
b.AddFunction(kDecl);
auto compiler = std::move(b).Build();
auto program  = compiler->Compile("double_it(x)");

// Register the impl (engine side) with the SAME declaration:
auto engine = celwasm::Engine::NewBuilder().Build();
engine->BindFunction(kDecl,
    [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; });

auto instance = engine->Plan(*program);
celwasm::Activation act;  act.Bind("x", celwasm::Value::Int(21));
auto v = instance->Eval(act);                       // → 42
```

Runnable version:
[`examples/04_host_functions.cc`](https://github.com/augustinemathew/cel-wasm/blob/master/examples/04_host_functions.cc).

`AddTypedFunction` is the same machinery one level down — you pass the
synthesized overload id (`<name>_<arg types>`, e.g. `double_it_int`) yourself:

```cpp
engine->AddTypedFunction("double_it_int",
    [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; });
```

The examples below use `AddTypedFunction` with explicit overload ids (useful
to learn the id scheme); each works identically via
`BindFunction(decl, lambda)`.

The lambda **must** return `absl::StatusOr<R>` — host functions are fallible
by contract. Return an error status to surface a CEL error:

```cpp
engine->AddTypedFunction("reciprocal_double",
    [](double x) -> absl::StatusOr<double> {
      if (x == 0.0) return absl::InvalidArgumentError("division by zero");
      return 1.0 / x;
    });
```

### 1.1 The canonical type table

Each CEL type maps to exactly **one** canonical C++ type on each side:

| CEL type | arg (param) C++ type | return C++ type |
|---|---|---|
| `bool` | `bool` | `bool` |
| `int` | `int64_t` | `int64_t` |
| `uint` | `uint64_t` | `uint64_t` |
| `double` | `double` | `double` |
| `string` | `absl::string_view` | `std::string` |
| `bytes` | `absl::string_view` (shares with `string`) | *(use the context `ReturnBytes` — see §1.3)* |
| `Duration` | `absl::Duration` | `absl::Duration` |
| `Timestamp` | `absl::Time` | `absl::Time` |
| `null` | (use `Value` / context API) | — |
| `proto(M)` | `const M&`, or `const google::protobuf::Message*` (polymorphic) | `std::unique_ptr<M>` |
| `list<T>` | `celwasm::HostListView` | `std::vector<celwasm::Value>` |
| `map<K,V>` | `celwasm::HostMapView` | `std::vector<std::pair<Value,Value>>` |
| any | `celwasm::Value` | `celwasm::Value` |

### 1.2 Only canonical types compile — no silent conversions

The binding accepts **only** the types above and rejects anything else **at
compile time** — there is no implicit conversion:

```cpp
// ✅ compiles — int64_t is the canonical type for CEL int:
engine->AddTypedFunction("f_int",  [](int64_t x) -> absl::StatusOr<int64_t> {...});

// ⛔ DOES NOT COMPILE — `int` is not canonical (would silently narrow):
engine->AddTypedFunction("f_int",  [](int x)     -> absl::StatusOr<int64_t> {...});
//   error: host-fn parameter is not a canonical CEL type. Use exactly:
//   int64_t, uint64_t, double, bool, absl::string_view, absl::Duration,
//   absl::Time, const M& (M:Message), const google::protobuf::Message*, ...

// ⛔ DOES NOT COMPILE — float is not double; char* is not string_view:
engine->AddTypedFunction("g", [](float x)        -> absl::StatusOr<double> {...});
engine->AddTypedFunction("h", [](const char* s)  -> absl::StatusOr<int64_t> {...});
```

Why: if `int64_t`→`int` were allowed, a CEL `int` near `INT64_MAX` would
**silently lose its top 32 bits**. On the error, change the parameter to the
exact canonical type it names — `int64_t` not `int`/`long`, `double` not
`float`.

### 1.3 Strings and bytes

CEL `string` and `bytes` share one C++ arg type: **`absl::string_view`**. A
`string_view` parameter accepts *either* a CEL `string` or a CEL `bytes`:

```cpp
// string (or bytes) s → string (uppercase):
engine->AddTypedFunction("upper_string",
    [](absl::string_view s) -> absl::StatusOr<std::string> {
      return absl::AsciiStrToUpper(s);
    });

// bytes b → int (length):
engine->AddTypedFunction("blen_bytes",
    [](absl::string_view b) -> absl::StatusOr<int64_t> {
      return static_cast<int64_t>(b.size());
    });
```

A returned `std::string` is copied into the program's linear memory by the
arena allocator and encoded as a CEL **`string`**. To return a CEL **`bytes`**
specifically, drop to the context API (§2) and call `ctx.ReturnBytes(...)`.
(There is no `Bytes` wrapper type — plain `absl::string_view` serves both.)

### 1.4 Proto arguments and returns

A `proto(M)` parameter is spelled `const M&` — the binding resolves the
message handle, `dynamic_cast`s to your generated type (a wrong message type
becomes a CEL error automatically), and hands you the live object:

```cpp
// Declared:  int @host.user_age(proto(acme.User) u);
engine->AddTypedFunction("user_age_message_acme_User",
    [](const acme::User& u) -> absl::StatusOr<int64_t> {
      return u.age();
    });
```

If your callback is reflection-generic, take
**`const google::protobuf::Message*`** instead — no `dynamic_cast`:

```cpp
engine->AddTypedFunction("type_name_message_acme_User",
    [](const google::protobuf::Message* m) -> absl::StatusOr<std::string> {
      return std::string(m->GetTypeName());
    });
```

The returned message is **owning** — you hand over a `unique_ptr`, and the
runtime keeps it alive for the rest of the evaluation:

```cpp
// Declared:  proto(acme.User) @host.make_user(string name);
engine->AddTypedFunction("make_user_string",
    [](absl::string_view name) -> absl::StatusOr<std::unique_ptr<acme::User>> {
      auto u = std::make_unique<acme::User>();
      u->set_name(std::string(name));
      return u;                                  // ownership transfers — safe
    });
```

!!! note "Why owning?"
    A returned proto is read by the caller *later* in the same eval; a
    reference to a stack local would dangle the moment your callback returns.
    Handing over a `unique_ptr` makes that impossible. Lists and maps don't
    have this issue — their `Value`s own their storage.

### 1.5 Lists and maps (including nested / complex)

A `list<T>` / `map<K,V>` argument arrives as a lazy view — `HostListView` /
`HostMapView` — that reads elements on demand; each element is itself a
`celwasm::Value`, so **nested aggregates just recurse**:

```cpp
// Declared:  int @host.sum(list<int> xs);
engine->AddTypedFunction("sum_list_int",
    [](celwasm::HostListView xs) -> absl::StatusOr<int64_t> {
      int64_t total = 0;
      for (size_t i = 0; i < xs.Size(); ++i) {
        auto e = xs.At(i);                       // StatusOr<Value>
        if (!e.ok()) return e.status();
        total += *e->AsInt();
      }
      return total;
    });
```

**`list<customer>`** — a list of protos. (A *single* proto argument is
simpler — take `const acme::Customer&` directly, §1.4.) The view yields each
element as a message `Value`; reach the concrete type through the message
backing:

```cpp
// Declared:  int @host.count_active(list<proto(acme.Customer)> custs);
engine->AddTypedFunction("count_active_list_message_acme_Customer",
    [](celwasm::HostListView custs) -> absl::StatusOr<int64_t> {
      int64_t n = 0;
      for (size_t i = 0; i < custs.Size(); ++i) {
        auto e = custs.At(i);                  // StatusOr<Value>
        if (!e.ok()) return e.status();
        auto mb = e->MessageBacking();         // StatusOr<const HostMessageBacking*>
        if (!mb.ok()) return mb.status();
        const auto* c =
            dynamic_cast<const acme::Customer*>((*mb)->message());
        if (c != nullptr && c->active()) ++n;
      }
      return n;
    });
```

**`map<string, list<customer>>`** — composes the same way, no special casing
(the exact shape `e2e/host_fn_test.cc`'s `TypedNestedMapStringListProtoArg`
pins):

```cpp
// Declared: int @host.team_size(
//     map<string, list<proto(acme.Customer)>> teams, string k);
engine->AddTypedFunction(
    "team_size_map_string_list_message_acme_Customer_string",
    [](celwasm::HostMapView teams,
       absl::string_view k) -> absl::StatusOr<int64_t> {
      auto custs = teams.Get(celwasm::Value::String(std::string(k)));
      if (!custs.ok()) return custs.status();
      if (custs->IsError()) return int64_t{0};   // missing key → spec error Value
      auto lb = custs->ListBacking();
      if (!lb.ok()) return lb.status();
      return static_cast<int64_t>((*lb)->Size());
    });
```

One genuine gap: `HostMapView` exposes only `Size()`, `Get(key)`, and
`ContainsKey(key)` — there is **no key-enumeration**, so a callback must
already know which keys it wants (pass them as arguments, or restructure the
data as a list).

Returning aggregates is symmetric — build a `std::vector<Value>` /
`std::vector<pair<Value,Value>>` (or `Value::List(...)` / `Value::Map(...)`);
the runtime interns it and the caller reads it back through the normal
indexing path.

### 1.6 Errors

- **Return an error status** → the call yields a CEL error, which propagates
  per CEL's error semantics (it is *not* a wrong value).

### 1.7 Unknowns and 3-valued logic

CEL is 3-valued: a value can be concrete, an **error**, or an **unknown**
(typically because an input attribute was withheld during `PartialEval`). A
host function participates in two distinct, **distinguishable** ways.

**(1) Unknown / error *arguments* are auto-propagated — you do nothing.** The
trampoline checks every arg before invoking your callback; if any is
unknown/error it writes that value straight to the out-slot — *attribute id
preserved* — and **does not call your function at all**. Your body only ever
runs with all-known arguments; there are **no** `ArgIsUnknown` /
`ArgIsError` accessors and no `PropagateIfUnknown` call to make.

```cpp
// Declared:  int @host.double_it(int x);
engine->AddTypedFunction("double_it_int",
    [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; });
// If `x` is unknown under PartialEval, this lambda is NEVER called;
// the eval result is that same unknown, still attributed to `x`.
```

**(2) Your function may *explicitly return* an unknown — with a sentinel.** A
function with all-known arguments may still decide its result is unknown. It
does **not** mint a fresh attribute id; it stamps the **reserved
function-origin sentinel** — `celwasm::kFunctionUnknownSentinel`
(`= UINT32_MAX`) — distinct from a propagated input unknown
carrying a real attribute id.

```cpp
// Context API — call ReturnUnknown() (no attribute argument):
engine->AddFunction("rate_lookup_string", /*num_args=*/2,
    [](celwasm::HostCallContext& ctx) -> absl::Status {
      auto cur = ctx.ArgString(0);  if (!cur.ok()) return cur.status();
      if (!RateTableLoaded()) return ctx.ReturnUnknown();   // function-origin unknown
      return ctx.ReturnDouble(LookupRate(*cur));
    });

// Typed API — return the sentinel unknown through a StatusOr<Value>:
engine->AddTypedFunction("rate_lookup_string",
    [](absl::string_view cur) -> absl::StatusOr<celwasm::Value> {
      if (!RateTableLoaded()) {
        return celwasm::Value::Unknown(
            celwasm::AttributeId{celwasm::kFunctionUnknownSentinel});
      }
      return celwasm::Value::Double(LookupRate(cur));
    });
```

**Detecting a function-returned unknown (consumer side).** Compare the
unknown's attribute id against the sentinel:

```cpp
celwasm::Value v = *instance->PartialEval(act, patterns);
if (v.IsUnknown() &&
    v.UnknownAttribute()->id == celwasm::kFunctionUnknownSentinel) {
  // a host function explicitly returned unknown (not a withheld input)
}
```

(`Value::UnknownAttribute()` returns `absl::StatusOr<AttributeId>`, hence the
`->id`; `eval/attribute.h`.) `ReturnError` behaves analogously.

> **Status ✅ shipped.** Both mechanisms are live and tested:
> auto-propagation of unknown/error args (error takes precedence over
> unknown), explicit `ctx.ReturnUnknown()` /
> `Value::Unknown(AttributeId{sentinel})`, and the sentinel surviving the
> runtime's 3VL merge — pinned by `host_fn_test.cc`
> (`ReturnUnknownStampsFunctionOriginSentinel`,
> `FunctionOriginUnknownSurvivesOperatorMerge`,
> `UnknownArgAutoPropagatesWithoutInvokingCallback`).

---

## 2. The context API — `HostCallContext&`

For per-argument control (dynamic arity, mixed handling), register a callback
over `HostCallContext`. Every accessor is **kind-checked** and returns
`absl::StatusOr<T>`:

```cpp
engine->AddFunction("clamp_int_int_int", /*num_args=*/4,   // 3 params + out_slot
    [](celwasm::HostCallContext& ctx) -> absl::Status {
      auto v  = ctx.ArgInt(0);  if (!v.ok())  return v.status();
      auto lo = ctx.ArgInt(1);  if (!lo.ok()) return lo.status();
      auto hi = ctx.ArgInt(2);  if (!hi.ok()) return hi.status();
      return ctx.ReturnInt(std::clamp(*v, *lo, *hi));
    });
```

### 2.1 Kind safety — why accessors return `StatusOr`

A CEL value on the wire is a **tagged union**: the integer, the string
`{ptr,len}`, and the externref index all alias the same bytes. Reading a
string's `{ptr,len}` out of an `int`'s bits makes `ptr` a garbage offset — an
**out-of-bounds read** — so every accessor checks the `kind` tag *before*
touching the payload:

```cpp
auto s = ctx.ArgString(0);     // if arg 0 is an int → InvalidArgument, not garbage
if (!s.ok()) return s.status();
```

The full accessor / setter surface (args: `ArgBool/Int/Uint/Double/String/
Bytes/Duration/Timestamp/Proto/List/Map/Value`, plus `ArgIsNull`; returns: the
matching `ReturnXxx`, plus `ReturnUnknown` / `ReturnError`) is specified in
`doc/implementation-plan/rewrite/m21-host-call-adapter.md` §3. There are
**no** `ArgIsUnknown` / `ArgIsError` accessors — an unknown/error argument is
auto-propagated before your callback runs (§1.7).

---

## 3. The old raw API — removed

The original raw form — a `HostCallback` receiving raw linear memory + slot
offsets and `memcpy`ing 24-byte `CelValue` cells by hand — **no longer
exists**. `HostCallback` is now
`std::function<absl::Status(HostCallContext&)>`, so every callback receives
the kind-checked context (§2). Migrating old raw callbacks:
`ctx.ArgString(0)` replaces the read-and-check-`CEL_STRING` block,
`ctx.ReturnInt(...)` replaces the write-to-`out_slot` block.

---

## 4. Receiver (method-style) functions

A leading `this` on the first parameter makes the function dispatch as a
method (`x.fn()` instead of `fn(x)`). The receiver is just the first argument:

```celfn
bool @host.is_admin(this proto(acme.User) u);   // called as: user.is_admin()
```

```cpp
engine->AddTypedFunction("is_admin_message_acme_User",
    [](const acme::User& u) -> absl::StatusOr<bool> {
      return u.role() == "admin";
    });
// expression:  compiler->Compile("user.is_admin()")
```

The `this` is purely call-site sugar — the C++ signature is identical to a
global function taking the same first argument.

---

## 5. Registration reference

- **Overload id.** `AddTypedFunction` / `AddFunction` key on the **overload
  id** the IDL synthesizes: `<fn_name>` + each param's type token, joined by
  `_` (e.g. `length_string`, `is_admin_message_acme_User`, `sum_list_int`).
  It must match the declared signature 1:1.
- **`num_args`** (context `AddFunction` only) = declared params + 1. The
  typed `AddTypedFunction` derives arity from the lambda and checks it.
- **Thread-safety.** Register all functions at startup, *then* `Plan` from
  many threads. `AddFunction` / `AddTypedFunction` are **not**
  concurrent-safe with each other or with `Plan`.
- **Unregistered call** → the expression fails to *evaluate* with a
  missing-import error at `Plan`; an *undeclared* call fails earlier, at
  `Compile`.

---

## 6. Status at a glance

| Capability | Status |
|---|---|
| Typed `AddTypedFunction` — canonical-type lambdas | ✅ |
| `HostCallContext` — kind-checked typed accessors | ✅ |
| proto / list / map args, aggregate / new-string returns | ✅ |
| Owning proto return (`unique_ptr`) | ✅ |
| Polymorphic proto arg (`const google::protobuf::Message*`) | ✅ |
| Explicit unknown return + function-origin sentinel | ✅ |
| Receiver (`this`) dispatch | ✅ |
| Raw 4-arg `HostCallback` (`memcpy` slots) | removed — replaced by `HostCallContext&` |

---

## 7. Test coverage — every example here runs end-to-end

Each capability has a passing test, and the **positive type matrix runs
end-to-end through the real wasmtime pipeline** (compile a CEL expr → `Plan` →
`Eval`), on *both* registration paths. Every `e2e/host_fn_test.cc` test
inlines the real customer flow (`Compiler::NewBuilder()…Build()` → `Compile`
→ `Engine::NewBuilder().Build()` → `AddTypedFunction` / `AddFunction` →
`Plan` → `Eval` / `PartialEval`) — no test-only wrappers.

| Capability | Typed-API e2e (`AddTypedFunction`) | Context-API e2e (`AddFunction`) |
|---|---|---|
| `bool` | `TypedBoolLambdaNegates` | `ContextBoolArgBoolReturn` |
| `int` | `TypedIntLambdaDoubles` | `ContextIntArgIntReturn` + `IntBoundaryTest` |
| `uint` | `TypedUintLambdaIncrements` | `ContextUintArgUintReturn` + `UintBoundaryTest` |
| `double` | `TypedDoubleLambdaHalves` | `ContextDoubleArgDoubleReturn` |
| `string` (arena return) | `TypedStringLambdaAllocatesReturn` | `ContextStringArgStringReturnAllocates` + `StringBoundaryTest` |
| `bytes` arg (via `string_view`) | `TypedBytesLambdaComputesLength` | `ContextBytesArgSumsBytes` |
| `bytes` return | `TypedLambdaReturnsBytesViaValue` (via the `Value` escape hatch — a `std::string` return is `string`-kinded) | `BytesArgWithEmbeddedNulRoundTrips` (`ReturnBytes`) |
| `Duration` | `TypedDurationLambdaDoubles` | `ContextDurationArgDurationReturn` |
| `Timestamp` | `TypedTimestampLambdaRoundTrips` | `ContextTimestampArgTimestampReturn` |
| `proto` arg (concrete) | `TypedConcreteProtoArg` | `ContextProtoArgReadsField` |
| `proto` arg (polymorphic `const Message*`) | `TypedPolymorphicProtoArg` | — |
| `proto` return (owning `unique_ptr<M>`) | `TypedProtoReturn` | `ContextProtoReturnBuildsMessage` |
| `list<T>` (host + arena/literal) | `TypedListViewArg` | `ContextListArgSumsElements`, `ContextListLiteralArgSumsElements` |
| `map<K,V>` (host + literal) | `TypedMapViewArg` | `ContextMapArgLooksUpKey`, `ContextMapLiteralArgLooksUpKey` |
| `map<string, list<proto>>` (deepest) | `TypedNestedMapStringListProtoArg` | `ContextNestedMapStringListProtoArg` |
| unknown return (function-origin sentinel) | `TypedLambdaReturnsUnknown` | `ContextReturnUnknownStampsFunctionOriginSentinel`, `FunctionOriginUnknownSurvivesOperatorMerge` |
| PartialEval — known args dispatch normally | `TypedPartialEvalKnownDispatchesUnknownPropagates` | `PartialEvalKnownArgInvokesHostFn` |
| PartialEval — unknown arg auto-propagates | `TypedPartialEvalKnownDispatchesUnknownPropagates` | `PartialEvalUnknownArgTest` (int/string/proto/list/map) |
| error return (CEL error value) / trapping callback | `TypedLambdaReturnsErrorValue`, `TypedLambdaNonOkStatusTraps` | `ContextReturnErrorPropagates`, `ContextNonOkStatusTraps` |
| receiver (`this`) | `TypedReceiverIsFirstArg` | `ContextReceiverIsFirstArg` |
| composition | `TypedFunctionsCompose` | `ContextResultFeedsComparisonOperator`, `ContextResultFeedsAnotherHostFn` |
| multi-decl library (`ParseCelfnSource`) | — (composition uses two typed decls) | `MultiDeclLibraryBothFire` |

**Component-level (unit, over fake memory / externref table / arena)** — what
the WASM pipeline *cannot* express:

- `eval/host_call_context_test.cc` — the full `ArgXxx`/`ReturnXxx` matrix plus
  the **negatives** only a fabricated slot can force: wrong-kind →
  `InvalidArgument`, out-of-range arg index → `OutOfRange`, dangling externref
  slot → `FailedPrecondition`, boundary values (`INT64_MIN/MAX`, `UINT64_MAX`,
  ±inf/nan/-0.0, empty / embedded-NUL / multi-byte strings), nested
  `list<customer>` / `map<string,list<customer>>`.
- `eval/typed_function_test.cc` — canonical-type round-trips and the
  **compile-time** must-not-compile guarantee (`static_assert` that
  `int`/`float`/`char*`/by-value-proto are rejected).

> The codegen / compiler only ever emit correctly-typed, in-bounds args, and
> the trampoline auto-absorbs unknown/error operands — so a kind-mismatch or
> OOB slot is unreachable through a real CEL expression. Those negatives, and
> the won't-compile guarantees, are verified at the component level by
> construction; everything a CEL expression *can* reach is verified
> end-to-end.

## See also

- [User guide index](index.md) — mental model, quick start, the full API.
- [Custom functions](custom-functions.md) — the `.celfn` IDL, function
  libraries, and Plan-time verification.
- `doc/implementation-plan/rewrite/m21-host-call-adapter.md` — the design
  behind the typed + context APIs (the authoritative spec).
