# Writing plugins (`@plugin`)

A **plugin** is a self-describing, sandboxed WebAssembly artifact that
implements CEL functions. It runs in its own linear memory, cannot read
the embedder's memory, cannot syscall, cannot do I/O — which is why it
is the path for function bodies you didn't write. And it *describes
itself*: the plugin's function declarations travel inside the `.wasm`,
so the artifact is the single source of truth on both the compile side
and the eval side.

The whole lifecycle:

```
 scorer.idl ┐
 scorer.cc  ├─► cel_wasm_plugin ─► scorer_plugin.wasm ─► Plugin::Load(bytes)
 (protos)   ┘   (one macro call)    (self-describing:         │
                                     decls embedded in a      ├─► Compiler::Builder::Use(plugin)
                                     `cel.fns` section)       │     call sites type-check
                                                              └─► Engine::Use(plugin)
                                                                    static export check, then
                                                                    Plan verifies signatures →
                                                                    sandboxed call at Eval
```

> **Status:** the C++ authoring pipeline ships end-to-end for scalar /
> int / bool round trips — what the
> [`demo_plugin`](https://github.com/augustinemathew/cel-wasm/tree/master/e2e/plugin_fixtures/cel_wasm_plugin_demo)
> fixture exercises in CI. Proto arguments/returns work via the
> manual-tagged `demo_plugin_proto` fixture (§5). String *returns* from
> the plugin side currently trap in a libc++ code path (skipped
> `GreetRoundTripsString`); string *arguments* are fine. The Go path is
> designed, not implemented (§7).

---

## 1. Why self-describing?

Registering a plugin function used to require the same declaration
three times: in the `.idl` the plugin was built from, as a hand-written
C++ `FunctionLibrary` mirror, and threaded into both the compiler and
the engine. The C++ mirror was the bug farm — it could silently drift
from the `.idl`, and the resulting failure surfaced at Plan or eval,
far from the drift.

The current model ends that: the `cel_wasm_plugin` build macro embeds
the `.idl` declaration text **verbatim** in a `cel.fns` custom section
inside the artifact, and `Plugin::Load` is the only way to construct a
`Plugin` — every `Plugin` is self-describing by construction. One noun
carries the wasm bytes, the parsed declarations, and a content hash;
the same object registers on any number of compilers and engines, from
any thread (it is immutable after `Load`).

---

## 2. Quick start — C++ ✅

Write the declarations once, in an `.idl`:

```c
// scorer.idl
Module scorer;

bool             @plugin.is_adult(proto(acme.User) u);
proto(acme.User) @plugin.capitalize(proto(acme.User) u);
```

The `Module` directive names the plugin (it seeds the plugin's
interface name, §3). Each `@plugin.<fn>` decl becomes a typed export.

### 2.1 The implementation

One C++ function per decl. Signatures come from a generated
`user_fns.h` (the macro emits it); the function name is the CamelCase
of the IDL name (`add` → `Add`, `is_adult` → `IsAdult`), the namespace
matches the `Module` directive. Protos cross the sandbox boundary as
serialized bytes; the generated codec deserializes before calling you:

```cpp
// scorer_fns.cc — implements the generated user_fns.h
#include "user_fns.h"
#include "acme/user.pb.h"

namespace scorer {
bool IsAdult(const acme::User& u) { return u.age() >= 18; }
acme::User Capitalize(const acme::User& u) { /* ... */ }
}  // namespace scorer
```

### 2.2 The build

```python
load("//bazel:cel_wasm_plugin.bzl", "cel_wasm_plugin")

cel_wasm_plugin(
    name = "scorer_plugin",
    idl  = "scorer.idl",
    user_fns = ["scorer_fns.cc"],          # implements user_fns.h
    deps = [":user_cc_proto"],             # acme.User C++ proto
    extra_includes = ["acme/user.pb.h"],
)
```

`bazel build :scorer_plugin` produces `scorer_plugin.wasm` — the one
artifact you ship. The macro compiles your implementation to a
sandboxed Component-Model wasm **and embeds the declarations verbatim
in a `cel.fns` custom section** — the artifact describes itself.

??? note "Under the hood (implementation detail)"
    The macro chains `cel generate` → `wit-bindgen c` → a
    `wasm32-wasip2` `cc_binary` → `cel embed-decls`, emitting
    `fns.wit`, `codec.h`, `generated_stub.cc`, and `user_fns.h` into
    the package's gen tree. The output is packaged as a WASI
    Component-Model component (preamble `0x1000d`), exporting the
    interface `cel:<module>/fns@0.1.0` — always derived from the
    IDL's `Module` directive (fallback `customfn`); there is no
    override. None of this surfaces in the API — the engine resolves
    your declarations against the plugin's exports by itself.

!!! note
    A proto-typed plugin drags **libprotobuf-cpp** into the
    wasm32-wasip2 cross-compile — a multi-minute cold-cache build. Tag
    such targets `manual` so default `bazel build //...` doesn't pay
    the cost (that is how the in-tree `demo_plugin_proto` fixture is
    built). Scalar/string plugins build in seconds (~54 KB artifacts).

### 2.3 Load it — one noun carries everything

```cpp
#include "abi/plugin.h"

auto plugin = celwasm::Plugin::Load(scorer_bytes).value();
// plugin.decls()      — the parsed declarations
// plugin.hash_hex()   — SHA-256 over (bytes ‖ declarations)
```

`Load` validates the artifact up front: a core wasm module instead of a
component, a missing `cel.fns` section, unparseable declarations, or a
non-`@plugin.` decl are all a clean `InvalidArgument` right here. An
artifact without an embedded section fails with:

```
Plugin::Load: no cel.fns section — rebuild with cel_wasm_plugin or
run `cel embed-decls`
```

(§6 covers `cel embed-decls`, for artifacts built outside the macro.)

### 2.4 Compile side — declarations flow to the type-checker

```cpp
auto builder = celwasm::Compiler::NewBuilder();
builder.DeclareVariable("user", celwasm::CelType::Message("acme.User"))
       .Use(*plugin);
auto compiler = std::move(builder).Build().value();
auto program = compiler.Compile("is_adult(user) && user.age < 120").value();
// The call site type-checks against the .idl signature —
// `is_adult(user.age)` fails HERE, at compile.  The Program records
// every custom function it calls (name + full signature) in its
// cel.abi, for Plan to verify.
```

No mirror: `Use(plugin)` is exactly
`DeclareFunctions(plugin.library())` — the declarations the checker
sees provably came out of the deployed artifact.

### 2.5 Eval side — possibly a different process

```cpp
auto engine = celwasm::Engine::NewBuilder().Build().value();
CHECK_OK(engine.Use(*plugin));
// Fail-fast registration: overload-id collisions, byte parse, and a
// static check that the plugin actually exports every declared
// function — a bad plugin upload is rejected HERE, not at traffic
// time.  No instantiation happens yet.

auto instance = engine.Plan(program).value();
// Plan verifies every function the program requires exists in the
// registry with an EXACTLY matching signature (protos compare by
// fully-qualified name), then instantiates ONLY the plugins the
// program actually calls — each into its own sandbox with its own
// linear memory.

acme::User u;  u.set_name("ada");  u.set_age(30);
celwasm::Activation act;
act.Bind("user", celwasm::Value::Message(u));
auto result = instance.Eval(act);      // -> Value::Bool(true)
```

The runnable, CI-exercised version of this flow (scalar shape) is
[`examples/09_plugin_functions.cc`](https://github.com/augustinemathew/cel-wasm/blob/master/examples/09_plugin_functions.cc).

### 2.6 What failure looks like

Forgot to register the plugin before `Plan`:

```
FailedPrecondition: Engine::Plan: program requires plugin function
`is_adult_message_acme_User` (`bool is_adult(proto(acme.User))`) but
no registered plugin declares it; register the providing plugin with
Engine::Use before Plan
```

— and a plugin update that changed a signature out from under a
compiled program:

```
FailedPrecondition: Engine::Plan: program requires plugin function
`is_adult_message_acme_Person` with signature
`bool is_adult(proto(acme.User))` but the registered plugin
(hash 3f9a2c1b04de) declares `bool is_adult(proto(acme.Person))`;
signatures must match exactly — recompile the program or rebuild the
plugin
```

Both fire at `Plan` — before any traffic, naming the function, the
signature, and (for a mismatch) the registered plugin's content hash.
A plugin upload that doesn't export what it declares fails even
earlier, at `Engine::Use`:

```
Engine::Use: plugin does not export `is-adult-message-acme-user` under
interface `cel:scorer/fns@0.1.0` (CEL overload-id
`is_adult_message_acme_User`)
```

---

## 3. How it works

Four pieces make the flow verifiable end to end (wire-level detail:
[ABI wire format](../design/08-abi-wire-format.md)):

- **The `cel.fns` section.** The macro's last step appends the `.idl`
  declaration text verbatim (UTF-8) as a top-level custom section named
  `cel.fns` on the component. `Plugin::Load` parses it back into a
  `FunctionLibrary`; the plugin's interface name is always derivable
  from the text's `Module` directive (`cel:<module>/fns@0.1.0`), so no
  side channel is needed.
- **The content hash.** `Plugin::hash()` is SHA-256 over
  (wasm bytes ‖ declaration text) — a stable identity for embedder
  bookkeeping, surfaced in Plan-time mismatch diagnostics. It is *not*
  enforced anywhere (§8).
- **Plan-time signature verification.** `Compile` records every custom
  function the emitted wasm imports — name, backend, and full recursive
  signature — in the Program's `cel.abi` (`required_functions`).
  `Engine::Plan` checks each entry against its registry before any
  linking: existence, receiver-ness, arity, each parameter type, and
  the return type must match exactly (protos by fully-qualified name).
  `@host` functions are covered too — a forgotten `BindFunction` is the
  same clean `FailedPrecondition` instead of an opaque wasmtime link
  error.
- **Selective instantiation.** Because Plan knows exactly which
  functions the program needs, it instantiates **only** the registered
  plugins that provide them. Register ten plugins; a program calling
  one instantiates one, and a program calling none instantiates zero.
  (Programs compiled before `required_functions` existed carry no
  table; for those, Plan behaves as before — no check, instantiate
  all.)

---

## 4. The sharing model — where plugin state lives

What is shared and what is not:

- **Shared per engine:** the parsed component (compiled once at `Use`)
  and the declarations. One registration serves every Program the
  engine ever plans. A `Plugin` value itself is immutable —
  registerable on many compilers/engines from many threads.
- **Fresh per Plan:** the plugin *instance*. Each Plan instantiates
  into its own store with its own linear memory — Instances never see
  each other's plugin state.
- **Persistent per Instance:** the plugin instance lives as long as
  the Instance, so state a plugin fn keeps in its linear memory (a
  cache, a counter) survives across `Eval` calls on the SAME Instance
  and resets on a fresh Plan. Write plugin fns as pure functions
  unless per-Instance memoization is the intent.
- **Registration is startup-only** (not thread-safe, like the rest of
  the registration family); `Plan` is concurrent-safe.

### 4.1 The lifecycle, made observable

Consider a plugin function with internal state, declared and
implemented in a plugin:

```c
// counter.idl
Module counter;

int @plugin.invocation_id();
```

```cpp
// counter_fns.cc — the author's implementation
namespace counter {
int InvocationId() {
  static int i = 0;   // lives in the PLUGIN INSTANCE's linear memory
  return i++;
}
}  // namespace counter
```

The embedder compiles a CEL expression that calls it — the call site
type-checks against the plugin's declaration like any other function:

```cpp
auto counter_plugin = Plugin::Load(counter_bytes).value();

auto b = Compiler::NewBuilder();
b.Use(counter_plugin);
auto compiler = std::move(b).Build().value();
auto program = compiler.Compile("invocation_id()").value();
// `program` is wasm that calls the plugin fn on every Eval.
```

Now run that ONE Program several ways and watch where the counter
state actually lives:

```cpp
auto engine = Engine::NewBuilder().Build().value();
CHECK_OK(engine.Use(counter_plugin));      // registered; compiled once, run never

auto a = engine.Plan(program).value();     // plugin instance A created
auto b = engine.Plan(program).value();     // plugin instance B created

Activation act;                            // no variables in this expr
a.Eval(act);   // -> 0     A's counter: i was 0, now 1
a.Eval(act);   // -> 1     same Instance, same linear memory — persists
b.Eval(act);   // -> 0     B is the SAME program on the SAME engine,
               //          but its own sandbox — not 2
a.Eval(act);   // -> 2     A unaffected by B

auto a2 = engine.Plan(program).value();    // re-plan the same program
a2.Eval(act);  // -> 0     a fresh Plan is a fresh sandbox — reset
```

The rules this pins:

- `static` / global state in a plugin fn is **per-Instance**, not
  per-engine and not per-process. Two Instances of the same Program on
  the same engine each start from zero.
- The same holds for heap allocations, lazily-built caches, and
  library init inside the plugin — each Instance pays its own init and
  keeps its own copy.
- A re-plan is a state reset. Anything a deployment does that re-plans
  (rollout, config reload) silently zeroes plugin-internal state —
  never park state a correctness property depends on inside a plugin.
- Corollary for CEL semantics: an expression calling such a function
  is not referentially transparent across Evals. That is the
  embedder's choice to make, but the intended model is pure functions;
  treat in-plugin state as an optimization (memoization) whose loss is
  always safe.

---

## 5. Type matrix

Every CEL type the plugin decl surface admits, with the canonical
C++ container the codec lifts to / lowers from:

| CEL type | wire type (internal) | C++ container |
| --- | --- | --- |
| `bool` | `bool` | `bool` |
| `int` | `s64` | `int64_t` |
| `uint` | `u64` | `uint64_t` |
| `double` | `f64` | `double` |
| `null` | `option<u8>` | `std::monostate` |
| `string` | `string` | `std::string_view` in, `std::string` out |
| `bytes` | `list<u8>` | `std::vector<uint8_t>` |
| `Duration` | `record { seconds: s64, nanos: s32 }` | `::google::protobuf::Duration` |
| `Timestamp` | `record { seconds: s64, nanos: s32 }` | `::google::protobuf::Timestamp` |
| `list<T>` | `list<T>` | `std::vector<T>` |
| `map<K,V>` | `list<tuple<K, V>>` | `std::map<K, V>` |
| `proto(...)` | `list<u8>` | the message type (e.g. `const acme::User&`) |

Aggregates compose recursively — `list<map<string, list<int>>>` is valid. The
codec emits per-type `lift` / `lower` overloads on demand; only types actually
used in the IDL are pulled into the plugin, so a string-only IDL doesn't
link the map machinery.

Proto values cross the boundary as **serialized bytes**: the host
serializes, the plugin deserializes (and vice versa for returns) — a
per-call cost inherent to the plugin-memory boundary; user code on both
sides sees the natural message type. Both sides need the proto
generated from the same `.proto`.

`optional<T>` and `type` are **permanently rejected** at the plugin
boundary — they don't compose with the canonical ABI.

Known envelope limits, each pinned by a test or tracked entry: plugin
functions that *return* a `string` currently trap in libc++ under
wasm32-wasip2 (skipped `GreetRoundTripsString` in the demo fixture;
string arguments are unaffected), and a map-*returning* decl hits a
codec-emitter gap.

---

## 6. Plugins built outside the macro — `cel embed-decls`

If you build a plugin artifact by other means (another build system,
another toolchain), stamp its declarations in yourself:

```
cel embed-decls --plugin=<in.wasm> --idl=<file.idl> --out=<out.wasm>
```

It validates the same things the macro's build step does — the input is
a Component-Model component, the `.idl` parses, **every decl is
`@plugin.`**, and no `cel.fns` section already exists — then appends
the verbatim `.idl` bytes as the section and writes the result. It is
deterministic (a pure function of its inputs), so re-running it in a
build is safe. The output loads with `Plugin::Load` like any
macro-built artifact.

**The legacy escape hatch.** For artifacts you cannot re-embed
(hand-built or pure-WAT fixtures with top-level exports),
`Engine::AddPlugin(plugin_bytes, lib)` keeps explicit-decls
registration alive: you supply the bytes *and* a hand-built
`FunctionLibrary`. It validates less than `Use` — no static export
check (a missing export surfaces at `Plan`, not registration) — and
reintroduces the decl-drift risk the self-describing flow was built to
end. Prefer `cel embed-decls` + `Plugin::Load` whenever the artifact is
yours to modify.

---

## 7. Go authoring ⛔ (designed, not implemented)

The intended Go authoring shape — for parity with C++:

```go
package main

//celfn:export add
func Add(a, b int64) int64 { return a + b }

func main() {}  // required for a reactor module
```

The planned macro extension would emit Go bindings instead of C++
(`language = "go"`), building with TinyGo's wasip2 target — which
emits a Component-Model binary directly, same as the C++ path.

**Why it's not available yet:** only `--language=cpp` is wired through
`cel generate`; the Go emitter (mirror of `cpp_codec_emitter` /
`cpp_stub_emitter`) hasn't been written, and the macro doesn't accept
`language = "go"`.

**Why TinyGo over stock Go:** TinyGo's output is ~118 KB for a
scalar/string fn vs stock Go's ~1.6 MB, and needs ~2 WASI imports vs
~17. The trade is no proto support — TinyGo's incomplete reflection
traps in `proto.Unmarshal` — so a proto-bearing Go fn would use stock
Go and pay the size cost. Empirically confirmed; see
`doc/implementation-plan/rewrite/foreign-go-bindgen-findings.md`.

---

## 8. Performance + size

Per-call overhead, M-series Mac, `-c opt` (re-measured 2026-07-22; reproduce
with `bazel run -c opt //benchmark/plugin:plugin_bench`):

| Path | Time / call |
| --- | ---: |
| `int+int → int` via plugin (this macro) | ~450 ns |
| `int+int → int` via host C++ callback | ~110 ns |
| 256 KiB string `len()` via host C++ callback | 3.6 µs @ 68 GiB/s |

The **canonical-ABI hop costs ~340 ns over a native host callback** on the
scalar shape — the price of process-like isolation: own linear memory, no
access to host state, swappable without recompiling the policy.
Where you'd otherwise reach for a `cel-cpp` extension in your host
process, this is usually a worthwhile trade. If the per-call cost
matters, the host-function path (`@host.` —
[Writing host functions](writing-host-functions.md)) is the direct
alternative: same process / memory, no isolation.

What is deliberately **not** checked, stated honestly: program↔plugin
*hash* agreement (`Plugin::hash()` is exposed for embedder bookkeeping;
enforcement is future work), and the WIT-level `FuncType` of exports
(the wasmtime C API's type introspection is too thin; unreachable for
macro-built plugins, where WIT and decls derive from one `.idl`).

---

## 9. Where to look next

- **Canonical example**:
  [`examples/09_plugin_functions.cc`](https://github.com/augustinemathew/cel-wasm/blob/master/examples/09_plugin_functions.cc)
  — the one-noun flow end to end (`bazel run //examples:09_plugin_functions`).
- **Working e2e fixture**:
  [`e2e/plugin_fixtures/cel_wasm_plugin_demo/`](https://github.com/augustinemathew/cel-wasm/tree/master/e2e/plugin_fixtures/cel_wasm_plugin_demo)
  — `fns.idl` + `user_fns.cc` + `BUILD.bazel`, end-to-end tests.
- **Macro source**:
  [`bazel/cel_wasm_plugin.bzl`](https://github.com/augustinemathew/cel-wasm/blob/master/bazel/cel_wasm_plugin.bzl).
- **The `Plugin` API**: `abi/plugin.h`; wire detail:
  [ABI wire format](../design/08-abi-wire-format.md).
- **Security guarantees**: [security model](security-model.md).
- **TinyGo / Go probe findings**:
  `doc/implementation-plan/rewrite/foreign-go-bindgen-findings.md`.
