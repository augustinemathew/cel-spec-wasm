# Writing component functions (`@component`)

Component functions live in a **separate WebAssembly Component-Model
component** instantiated alongside the CEL expression. The component
runs in its own linear memory; values cross the boundary through the
canonical ABI (lifted into the component's memory on the way in, lowered
back out on the way out).

The `cel_wasm_component` Bazel macro takes a `.celfn` IDL and your
implementation source, runs the whole compile pipeline (`cel generate` →
`wit-bindgen c` → `wasm32-wasip2` `cc_binary`), and produces a single
`.wasm` component you load via `Engine::AddComponent`.

> **Status:** the C++ pipeline ships end-to-end for the scalar /
> int / bool round trip — that's what the
> [`demo_component`](../../e2e/foreign_component_fixtures/cel_wasm_component_demo/)
> fixture exercises in CI. String returns from the component side
> currently route through a libc++ code path that we'd like to skip
> rather than fully support — see [Performance follow-ups](#9-performance-follow-ups).
> The Go path (TinyGo) is designed; the `--language=go` arm is planned.

---

## 1. The pipeline at a glance

```
fns.idl       ┐
user_fns.cc   ├─► cel_wasm_component  ─►  demo_component.wasm
acme/user.proto ┘   (cel generate +              (CM component
                     wit-bindgen +                that exports
                     wasi-sdk cc_binary)         cel:customfn/fns@0.1.0)
```

The macro emits four intermediate files into the package's gen tree —
`fns.wit`, `codec.h`, `generated_stub.cc`, `user_fns.h` — then compiles
your `user_fns.cc` against them under `wasm32-wasip2`. The output is a
preamble-`0x1000d` Component-Model component; `wasm-tools component wit`
on it shows `export cel:customfn/fns@0.1.0`.

---

## 2. Quick start — C++ ✅

Three input files plus one Bazel macro call.

### 2.1 The IDL — `fns.idl`

```celfn
Module customfn;

int    @component.add(int a, int b);
string @component.greet(string name, int age);
```

The `Module` directive becomes the WIT package name
(`cel:<module>/fns@0.1.0` by default; override with `package = ...` on
the macro). Each `@component.<fn>` decl becomes a typed export inside
that interface.

### 2.2 The implementation — `user_fns.cc`

You write one C++ function per IDL decl. Signatures come from a
generated `user_fns.h` (you don't write that header — the macro emits
it). Types follow the canonical mapping (see §5).

```cpp
#include "user_fns.h"

#include <string>
#include <string_view>

namespace customfn {

int64_t Add(int64_t a, int64_t b) {
  return a + b;
}

std::string Greet(std::string_view name, int64_t age) {
  return std::string("Hello, ") + std::string(name) + " (age " +
         std::to_string(age) + ")";
}

}  // namespace customfn
```

The `customfn` namespace matches the IDL's `Module customfn;`. The C++
function name is the CamelCase of the IDL fn name (`add` → `Add`,
`is_admin` → `IsAdmin`, `allow_user` → `AllowUser`).

### 2.3 The Bazel target — `BUILD.bazel`

```python
load("//bazel:cel_wasm_component.bzl", "cel_wasm_component")

cel_wasm_component(
    name = "demo_component",
    idl = "fns.idl",
    user_fns = ["user_fns.cc"],
)
```

`bazel build :demo_component` produces `bazel-bin/.../demo_component.wasm` —
a real Component-Model component (~54 KB for the example above).
Inspect with:

```bash
bazel run //third_party/wasm_tools:wasm-tools -- \
    component wit bazel-bin/.../demo_component.wasm
```

### 2.4 Loading it from C++

This mirrors the working fixture test
(`e2e/foreign_component_fixtures/cel_wasm_component_demo/demo_component_e2e_test.cc`):

```cpp
#include <fstream>
#include <utility>

#include "compiler/celfn/function_library.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "shared/type.h"

// CelfnType is a plain struct; a tiny helper keeps scalar decls terse.
celwasm::CelfnType Prim(celwasm::CelfnType::Kind k) {
  celwasm::CelfnType t;
  t.kind = k;
  return t;
}

// Load the .wasm bytes (via bazel runfiles in a test, or wherever you
// ship them):
std::ifstream f("path/to/demo_component.wasm", std::ios::binary);
std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());

// Mirror the IDL's decls on the C++ side so the engine knows what to
// bind to which export.  SetWitInterface tells the engine where to
// look — the macro's default world is `cel:<module>/fns@0.1.0`.
auto lib_or =
    celwasm::FunctionLibrary::Builder()
        .SetWitInterface("cel:customfn/fns@0.1.0")
        .AddForeignComponent(
            "add", Prim(celwasm::CelfnType::Kind::kInt),
            {celwasm::CelfnParam{false, Prim(celwasm::CelfnType::Kind::kInt), "a"},
             celwasm::CelfnParam{false, Prim(celwasm::CelfnType::Kind::kInt), "b"}})
        .Build();
ABSL_CHECK_OK(lib_or);
const celwasm::FunctionLibrary lib = *std::move(lib_or);

auto engine = celwasm::Engine::NewBuilder().Build();
ABSL_CHECK_OK(engine);
ABSL_CHECK_OK(engine->AddComponent(bytes, lib));

// Compile and evaluate as usual — `add(...)` is now a callable export.
// Note `Compiler::Builder::Build()` is rvalue-qualified: chain setters
// on a named builder, then `std::move(b).Build()`.
auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("a", celwasm::CelType::Int())
    .DeclareVariable("b", celwasm::CelType::Int())
    .AddLibrary(lib);
auto compiler = std::move(b).Build();
ABSL_CHECK_OK(compiler);

auto program = compiler->Compile("add(a, b)");
ABSL_CHECK_OK(program);
auto instance = engine->Plan(*program);
ABSL_CHECK_OK(instance);

celwasm::Activation act;
act.Bind("a", celwasm::Value::Int(40));
act.Bind("b", celwasm::Value::Int(2));
auto v = instance->Eval(act);
ABSL_CHECK_OK(v);
// *v->AsInt() == 42
```

Working e2e test fixture:
[`e2e/foreign_component_fixtures/cel_wasm_component_demo/`](../../e2e/foreign_component_fixtures/cel_wasm_component_demo/).

---

## 3. Proto messages

The IDL admits `proto(<fully.qualified.name>)` for any
Component-Model-backed function. Proto values **cross the boundary as
serialized bytes** (m24 §8): the host serializes the message, the
component deserializes, and vice versa for returns. The user code on
both sides sees the natural language-native message type.

```celfn
// In fns.idl
Module customfn;

bool             @component.is_admin(proto(acme.User) u);
proto(acme.User) @component.capitalize(proto(acme.User) u);
```

```cpp
// In user_fns.cc — pull the generated header through the macro's
// `extra_includes` argument so user_fns.h #include resolves.
#include "acme/user.pb.h"

namespace customfn {
bool IsAdult(const acme::User& u) {
  return u.age() >= 18;
}
acme::User Capitalize(const acme::User& u) {
  acme::User r = u;
  // … mutate r …
  return r;
}
}  // namespace customfn
```

```python
# In BUILD.bazel — feed the proto cc_library as a wasm-cross dep so the
# wasi-sdk cc_binary can compile your `.pb.h`/`.pb.cc`.
cel_wasm_component(
    name = "demo_component_proto",
    idl = "fns_proto.idl",
    user_fns = ["user_fns_proto.cc"],
    deps = ["//acme:user_cc_proto"],
    extra_includes = ["acme/user.pb.h"],
    tags = ["manual"],   # libprotobuf-cpp cross-build is slow; opt-in
)
```

> The proto path drags **libprotobuf-cpp** into the wasm32-wasip2
> cross-compile, which is a multi-minute cold-cache build. Tag the
> target `manual` so default `bazel build //...` doesn't pay the cost;
> build the proto target explicitly when you exercise that path.

---

## 4. Go via TinyGo ⛔ (designed, not implemented)

The intended Go authoring shape — for parity with C++:

```go
package main

//celfn:export add
func Add(a, b int64) int64 { return a + b }

//celfn:export greet
func Greet(name string, age int64) string {
    return fmt.Sprintf("Hello, %s (age %d)", name, age)
}

func main() {}  // required for a reactor module
```

The planned macro extension would emit Go bindings instead of C++:

```python
cel_wasm_component(
    name = "demo_component",
    idl = "fns.idl",
    language = "go",                  # planned
    user_fns = ["user_fns.go"],
)
```

Behind the scenes the pipeline would be:

```
fns.idl  ─► cel generate --language=go  ─► fns.wit + customfn_bindings.go
                                            +  user_fns.go (yours)
                ↓
            tinygo build -target=wasip2 -buildmode=c-shared -o core.wasm
                ↓
            (the wasip2 TinyGo target emits a CM component directly,
             same as the C++ path)
```

**Why it's not available yet:** today only `--language=cpp` is wired
through `cel generate`. The Go emitter (mirror of `cpp_codec_emitter` /
`cpp_stub_emitter`) hasn't been written, and the macro doesn't yet
accept `language = "go"`.

**Why we picked TinyGo over stock Go** for the Go path: TinyGo's wasip2
output is ~118 KB for a scalar/string fn vs stock Go's ~1.6 MB, and
needs ~2 WASI imports vs ~17. The trade is no proto support — TinyGo's
incomplete reflection traps in `proto.Unmarshal` — so a proto-bearing
Go fn would use stock Go and pay the size cost. This is empirically
confirmed; see `doc/implementation-plan/rewrite/foreign-go-bindgen-findings.md`.

---

## 5. Type matrix

Every CEL type the foreign-component decl surface admits, with the
canonical C++ container the codec lifts to / lowers from:

| CEL type | WIT type | C++ container |
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

Aggregates compose recursively — `list<map<string, list<int>>>` is a
valid type. The codec emits per-type `lift` / `lower` overloads on
demand; only types actually used in the IDL are pulled into the
component, so a string-only IDL doesn't link the map machinery.

`optional<T>` and `type` are **permanently rejected** at the component
boundary (m24 §14) — they don't compose with the canonical ABI.

---

## 6. Backend distinction

There are three function-IDL backends; only one routes through the
Component Model:

| Prefix | Where the body lives | Registration |
| --- | --- | --- |
| `@host.fn` | embedder C++ | `Engine::AddFunction` (or `AddTypedFunction`) |
| `@component.fn` | a wasm component | `Engine::AddComponent(bytes, lib)` |
| `@native.fn = expr` | CEL expression body | — (codegen path unshipped; see index §7) |

This document covers `@component.` exclusively. For host functions see
[Writing host functions](writing-host-functions.md).

---

## 7. Performance + size

Per-call overhead, M-series Mac, `-c opt` (re-measured 2026-07-22;
reproduce with `bazel run -c opt
//benchmark/component:foreign_component_bench`):

| Path | Time / call |
| --- | ---: |
| `int+int → int` via component (this macro) | ~450 ns |
| `int+int → int` via host C++ callback | ~110 ns |
| 256 KiB string `len()` via host C++ callback | 3.6 µs @ 68 GiB/s |

So the **canonical-ABI hop costs ~340 ns over a native host callback**
on the scalar shape. That's the price of process-like isolation: the
component runs in its own linear memory, has no access to the host's
state, and can be swapped at runtime without recompiling the policy.

For workloads where you'd otherwise reach for a `cel-cpp` plugin in
your host process, this is usually a worthwhile trade — the component
is sandboxed, portable across hosts, and produced by one Bazel target
from one IDL.

If the per-call cost matters in your workload, the host-function path
(`@host.` — see [Writing host functions](writing-host-functions.md))
is the direct alternative; it gives up the isolation but stays in the
same process / memory.

---

## 8. Performance follow-ups (open invitation)

A handful of optimisations are in the design queue but not yet wired —
all of them target the per-call boundary cost:

- **AOT-cache the component instance.** Today every `Engine` rebuilds
  the wasmtime component instance from bytes at `AddComponent` time.
  Caching the cwasm machine code (`wasmtime::Module::serialize`) on
  disk and skipping Cranelift on the warm path would amortise the
  ~240-300 µs Plan cost across processes — the same lever already
  documented for the expression module
  (`engine.h:24`).
- **Component-side allocator pool.** `cabi_realloc` is called once
  per argument lift; a small per-call slab inside the component would
  cut that to one bump-pointer touch.
- **Skip the canonical-ABI hop for compute-only components.** If
  the component declares it has no host-visible state across calls
  (a `pure` annotation in the IDL would say so), the engine could
  cache the lifted args across repeated Evals with the same activation
  — common in batch-eval loops.
- **A non-libc++ build mode** that compiles user_fns against a tiny
  freestanding string/vector implementation. The wasm32-wasip2
  toolchain pulls all of libc++ as soon as you `#include <string>`;
  a `-std=cel` flag in the macro would build against an in-tree
  header-only replacement and strip the import surface.

If you have a workload where any of these would matter, the
[bench harness](../../benchmark/component/foreign_component_bench.cc) is the
right place to add a row and measure.

## 9. Where to look next

- **Working demo**:
  [`e2e/foreign_component_fixtures/cel_wasm_component_demo/`](../../e2e/foreign_component_fixtures/cel_wasm_component_demo/)
  — `fns.idl` + `user_fns.cc` + `BUILD.bazel`, end-to-end `cc_test`
  that round-trips `add(a, b)` through `Engine::AddComponent`.
- **Macro source**:
  [`bazel/cel_wasm_component.bzl`](../../bazel/cel_wasm_component.bzl).
- **Design doc**:
  [`doc/implementation-plan/rewrite/m26-celfnc-and-component-build.md`](../implementation-plan/rewrite/m26-celfnc-and-component-build.md).
- **Type matrix detail / canonical ABI**:
  [`doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md`](../implementation-plan/rewrite/m24-foreign-fn-component-backend.md).
- **TinyGo / Go probe findings**:
  `doc/implementation-plan/rewrite/foreign-go-bindgen-findings.md`.
