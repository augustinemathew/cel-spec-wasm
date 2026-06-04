# M26 — `celfnc` + cel_wasm_component: hermetic component build from a single `.idl`

Status: plan — drafted 2026-06-04, not yet started.

## 0. TL;DR

The m24 author surface ([§5](m24-foreign-fn-component-backend.md)) is
"the author writes only `user_fns.cc`; everything else is generated."
m24 v1 shipped the *runtime side* of that (the marshaling bridge +
`Engine::AddComponent`) and proved the contract with a hand-written
`e2e/foreign_component_fixtures/stub_demo/` fixture. **m26 closes
the build side**: a single `.idl` file fully defines a foreign-fn
component, and `bazel build` produces a `.wasm` component from it —
no out-of-band toolchain, no hand-written WIT, no hand-written codec.

The new surface is **two pieces**:

  1. **A `generate` subcommand on the existing `tools/cel`
     binary** — `cel generate --language={cpp,go} --idl=fns.idl
     --out-dir=./gen` reads a `.idl` and emits `fns.wit` + `codec.h`
     + `generated_stub.cc` + `user_fns.h` (or the Go equivalents).
     Sibling of the existing `eval` / `check` / `compile`
     subcommands. The emitter libraries live under
     `compiler/celfn/celfnc_emit/`; the CLI just links them in.
     Reuses the shipped `ParseCelfnSource`
     (`compiler/celfn/function_library.{h,cc}`) for the front end;
     the back end is a per-CEL-type emitter covering the entire
     m24 §6 type matrix.
  2. **`cel_wasm_component`** — a Starlark macro at
     `bazel/cel_wasm_component.bzl` that wires
     `cel generate` + wit-bindgen + wasi-sdk + wasm-tools into one
     buildable target.

Together they turn this:

```python
cel_wasm_component(
    name = "rules_component",
    idl  = "fns.idl",
    user_fns = ["user_fns.cc"],
)
```

into a `.wasm` component the embedder can pass to
`Engine::AddComponent`. **No external toolchain on the user's
machine**; everything is fetched via `http_archive` and runs through
bazel exec, which means the build is reproducible on
**darwin × linux × arm64 × x86_64** identically.

## 1. Background — what shipped vs what was missing

m24 v1 ([m24 doc closeout](m24-foreign-fn-component-backend.md), 2026-06-04)
shipped:

  - The runtime/marshaling side: `eval/internal/cel_component.{h,cc}`
    lifts/lowers every CEL type the foreign-fn surface admits.
  - `Engine::AddComponent(component_bytes, lib)` instantiates a
    component, validates exports, and binds them as host callbacks.
  - 89 marshaling tests + an e2e dispatch proof using inline WAT
    (settled by a probe: `wasmtime_wat2wasm` accepts component-model
    WAT, so the e2e suite is self-contained).
  - `e2e/foreign_component_fixtures/stub_demo/` — the *author-surface
    contract demonstration*, hand-written: `fns.wit`, `codec.h`,
    `generated_stub.cc`, `user_fns.h`, `user_fns.cc`. Each file
    shows the shape m24 §5 prescribed.

What did NOT ship:

  - `celfnc` — the generator. m24 §14 listed it as future work.
  - A bazel build path for stub_demo (D.1 was rescoped because
    inline WAT was enough for the e2e dispatch test).
  - The TinyGo forcing-fn fixture (D.2 — blocked on toolchain).
  - The production-config bench (D.3 — blocked on a real component
    fixture being buildable).

m26 unblocks all three by delivering celfnc + the build macro.

## 2. The `.idl` — the single source of truth

The format is the one `ParseCelfnSource` already consumes (see
`compiler/celfn/function_library_test.cc::SynthesisesOverloadIdsForAllTypes`).
The decl shape is `<return-type> <module>.<function>(<params>);` —
the `<module>` part is what drives the namespace / package name in
EVERY generated language (C++ namespace, Go package, WIT package
identifier, file paths).

```
Module rules;

bool rules.allow(string user, list<string> roles);
int  rules.score(map<string, int> permissions);
list<string> rules.expand(string template);
```

Three foreign-component decls; one module name (`rules`).
The author runs `bazel build //rules:rules_component` and gets a
deployable `.wasm` component.

### 2.1 How `<module>` flows through every generated file

The `Module` directive (or the alias on each decl prefix) is the
single source of truth for naming the entire generated set:

| Where the module name appears | How |
| --- | --- |
| WIT package identifier | `package <module>:fns@<ver>;` (e.g. `rules:fns@0.1.0`). Hence the wit-bindgen-emitted prefix `exports_<module>_fns_*` and the canonical export attribute `<module>:fns@<ver>#<fn-kebab>` follow. |
| C++ namespace for `user_fns` | `namespace <module> { ... }` in both `user_fns.h` (skeleton) and `user_fns.cc` (author bodies). Generated `codec.h` and `generated_stub.cc` reference `<module>::Fn(...)`. |
| C++ output file paths | `gen/<module>/fns.wit`, `gen/<module>/codec.h`, `gen/<module>/generated_stub.cc`, `gen/<module>/user_fns.h`. Keeps multi-component projects from colliding. |
| Go package | `package <module>` at the top of `user_fns.go`, `codec.go`, `generated_stub.go`. |
| Bazel target | `<module>_component.wasm` by convention. |

The grammar lives at `compiler/celfn/CelfnLexer.g4` +
`CelfnParser.g4`. m26 does **not** extend the grammar — the
foreign-component decl shape that `ParseCelfnSource` already accepts
(see m24 §A.4/A.5 Builder gates) is sufficient. `optional<T>` and
`type` remain permanently rejected as foreign-component shapes
([m24 §14](m24-foreign-fn-component-backend.md#permanent-scope)); the
`celfnc` generator inherits those gates by routing through
`FunctionLibrary::Builder::Build()` and propagating the rejection.

CEL `null` (`kNull`) stays supported as a declarable shape.

## 3. `cel generate` output — four files per IDL

```
                  cel generate --language=cpp
fns.idl ────────────────────────────────────▶ ┌─ fns.wit              ← WIT interface, fed to wit-bindgen
                                              ├─ codec.h              ← lift/lower std:: ↔ wit-bindgen author_* structs
                                              ├─ generated_stub.cc    ← WIT exports calling user::fn(codec::lift(...))
                                              └─ user_fns.h           ← author skeleton (one decl per fn, native types)
```

`cel generate --language=go <idl>` emits the analogous Go set
(`fns.wit`, `codec.go`, `generated_stub.go`, `user_fns.go`). The
WIT file is language-agnostic; the codec + stub layers reshape to
match the target language. This is what makes the SAME `.idl`
buildable into BOTH a C++ component (D.1) and a Go component
(D.2 forcing-fn fixture).

### 3.1 `fns.wit`

One typed WIT function per IDL decl. Identifier rule: snake_case
`overload_id` (e.g. `allow_string_list_string`) maps to kebab-case
`allow-string-list-string` per the Component-Model spec
(`Engine::AddComponent` does the snake↔kebab translation at the
export-lookup site — see m24 closeout deltas).

```wit
package cel:customfn@0.1.0;

interface fns {
  allow-string-list-string: func(user: string, roles: list<string>) -> bool;
  score-map-string-int: func(permissions: list<tuple<string, s64>>) -> s64;
  expand-string: func(t: string) -> list<string>;
}

world author { export fns; }
world host   { import fns; }
```

### 3.2 `codec.h`

Per-type lift (`author_<wit_t>` → `std::<T>`) and lower (back). The
generator emits ONLY the conversions needed by the IDL (avoids
template instantiation bloat). The pattern matches the shipped
`stub_demo/codec.h`.

### 3.3 `generated_stub.cc`

One `exports_cel_customfn_fns_<fn>` per IDL decl. Body:

```cpp
bool exports_cel_customfn_fns_allow_string_list_string(
    author_string_t* user,
    author_list_string_t* roles) {
  return user::Allow(codec::lift(*user), codec::lift(*roles));
}
```

The author writes the `user::Allow(...)` body in `user_fns.cc`;
everything else is generated.

### 3.4 `user_fns.h` (skeleton)

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace user {
bool Allow(std::string_view user, const std::vector<std::string>& roles);
int64_t Score(const std::map<std::string, int64_t>& permissions);
std::vector<std::string> Expand(std::string_view t);
}  // namespace user
```

The author fills the bodies in `user_fns.cc`. On a fresh project,
`celfnc --emit-skeleton` writes a stub `user_fns.cc` too; on
re-runs it does NOT overwrite (the author's bodies are sacred).

## 3.5 What wit-bindgen actually emits — empirical probe (2026-06-04)

A real run of `wit-bindgen c --world author fns.wit` (probe at
`/tmp/witgen` during m26 design; not committed) settled three
shapes that diverged from the initial design assumptions and that
celfnc's codec/stub emitters must match exactly:

  - **Records get an interface-qualified prefix, not `author_`.**
    `record duration { … }` inside `interface fns` becomes
    `exports_cel_customfn_fns_duration_t` in `author.h`.  The prefix
    is `exports_<package_normalized>_<interface_normalized>_`.
    Collections (`list<T>`, `tuple<>`) stay on the `author_` prefix
    (`author_list_s64_t`, `author_tuple2_string_s64_t`,
    `author_list_tuple2_string_s64_t`).
  - **Returns clean up via `cabi_post_*`, NOT via the author calling
    `_free`.** wit-bindgen emits a `__wasm_export_..._post_return`
    per export that returns a heap-owned type; the canonical-ABI
    runtime calls it after the host has copied the value out.  The
    author's `generated_stub.cc` populates `*ret` once via
    `author_string_dup_n(ret, data, len)` (for strings) or by
    direct `ret->ptr = cabi_realloc(NULL, 0, align, n)` +
    `ret->len = n` (for lists), and the cleanup is automatic.
    The author NEVER calls `author_list_*_free` on a return value —
    those helpers exist for cleanup of INPUT values when the author
    chooses to take ownership.
  - **`option<unit>` is not accepted by wit-bindgen's C generator.**
    A celfn `null` argument has to use `option<u8>` (or some other
    concrete one-byte carrier).  The canonical-ABI value is "none"
    when the CEL value is null; the payload byte exists but is
    never read.  Documented + tested in `wit_emitter_test::
    NullPrimitiveBecomesOptionUnit`.

The probe binary + the synthesised `fns.wit` covering every m24
§6 row + the generated `author.h` and `author.c` are at
`/tmp/witgen/` (working-tree only; not committed per CLAUDE.md
probe discipline).

### 3.5.1 Wire-level naming the codec emitter must produce

The codec emitter takes these as ground truth (driven directly by
the wit-bindgen probe):

| WIT shape | wit-bindgen C type |
| --- | --- |
| `bool` / `s64` / `u64` / `f64` | `bool` / `int64_t` / `uint64_t` / `double` |
| `string` | `author_string_t { uint8_t* ptr; size_t len; }` |
| `list<u8>` | `author_list_u8_t { uint8_t* ptr; size_t len; }` |
| `list<s64>` | `author_list_s64_t { int64_t* ptr; size_t len; }` |
| `list<string>` | `author_list_string_t { author_string_t* ptr; size_t len; }` |
| `list<list<s64>>` | `author_list_list_s64_t { author_list_s64_t* ptr; size_t len; }` |
| `tuple<string, s64>` | `author_tuple2_string_s64_t { author_string_t f0; int64_t f1; }` |
| `list<tuple<string, s64>>` (= map) | `author_list_tuple2_string_s64_t { author_tuple2_string_s64_t* ptr; size_t len; }` |
| `option<u8>` | `author_option_u8_t { bool is_some; uint8_t val; }` (but the **export signature** uses `uint8_t *maybe_x` — pointer-as-maybe) |
| `record duration` (inside interface) | `exports_<pkg>_<iface>_duration_t { int64_t seconds; int32_t nanos; }` |

Helpers the codec emitter relies on, all from `author.h`:

  - `author_string_dup_n(ret, data, len)` — copy bytes into a freshly
    `cabi_realloc`'d buffer; populates `ret->ptr` + `ret->len`.  Used
    by every string-return path.
  - `cabi_realloc(NULL, 0, align, n)` — the canonical-ABI allocator;
    used for list-of-T returns (no `author_list_*_dup_n` exists).
  - `author_<type>_free(ptr)` — frees an **input** list / tuple / option
    if the author chose to take ownership.  Returns clean up via
    `cabi_post_*`, not these.

## 4. CEL type ↔ WIT ↔ C++ — the full m24 §6 matrix, in code

Every CEL type the m24 foreign-component surface admits has three
emitter functions in celfnc:

| CEL type | WIT emission | codec lift emission | codec lower emission |
| --- | --- | --- | --- |
| `bool` | `bool` | `bool` (pass-through) | `bool` |
| `int` | `s64` | `int64_t` | `int64_t` |
| `uint` | `u64` | `uint64_t` | `uint64_t` |
| `double` | `f64` | `double` | `double` |
| `null` | `option<unit>` | `std::monostate{}` from `none` | `none` (no `some` case) |
| `string` | `string` | `std::string_view{author_string_t.ptr, .len}` (in) / `std::string` (out) | `author_string_dup_n(ret, s.data(), s.size())` |
| `bytes` | `list<u8>` | `std::vector<uint8_t>{l.ptr, l.ptr + l.len}` | `author_list_u8_dup(ret, v.data(), v.size())` |
| `duration` | `record { seconds: s64, nanos: s32 }` | `absl::Seconds(r.seconds) + absl::Nanoseconds(r.nanos)` | `{seconds: ToInt64Seconds, nanos: ToInt64Nanoseconds(d - Seconds(...))}` |
| `timestamp` | `record { seconds: s64, nanos: s32 }` | `absl::FromUnixSeconds(r.seconds) + absl::Nanoseconds(r.nanos)` | mirror duration |
| `list<T>` | `list<wit T>` | `std::vector<C++ T>` via per-element `codec::lift(l.ptr[i])` | per-element lower into a freshly-allocated `author_list_<T>` |
| `map<K, V>` | `list<tuple<wit K, wit V>>` | `std::map<C++ K, C++ V>` via per-entry lift | per-entry lower into a freshly-allocated `author_list_tuple2_<K>_<V>` |
| `proto(fqn)` | `list<u8>` | `T msg; msg.ParseFromArray(l.ptr, l.len); return msg;` | `out_bytes = msg.SerializeAsString(); lower as list<u8>` |
| `type` | — | — | — (rejected at celfnc-IDL-parse time; see m24 §14) |
| `optional<T>` | — | — | — (rejected at celfnc-IDL-parse time; see m24 §14) |

Recursion is by **concrete expansion** (m24 §6 / §A nested-decl
tests). Every type-emitter is a small switch in celfnc; the
recursion happens by calling the inner-type emitter from `kList` and
`kMap`. **Arbitrary nesting depth supported** —
`list<map<string, list<int>>>` round-trips identically to a flat
`int` from the IDL through to the canonical ABI.

### 4.0 Return-ownership rule — automatic via `cabi_post_*`

The codec emitter does NOT call `_free` on return values.  The
author writes:

```cpp
void exports_cel_customfn_fns_ret_string(author_string_t* x,
                                         author_string_t* ret) {
  // codec::lift(*x) → std::string_view (no copy, ref to caller mem)
  // user::Shout(...) → std::string (owned, temporary)
  // codec::lower writes into *ret via author_string_dup_n
  codec::lower(ret, user::Shout(codec::lift(*x)));
}
```

`author_string_dup_n` calls `cabi_realloc(NULL, 0, 1, len)`
internally; the canonical-ABI runtime owns the buffer once Lift
copies it back to the host, and the wit-bindgen-emitted
`__wasm_export_..._ret_string_post_return` does the `free()` after
the host call returns.  The author does not have to think about
the cleanup at all.

The same pattern applies to every list / map / record return — the
codec emitter writes into the `*ret` out-param directly; cleanup
is automatic via `cabi_post_*`.

### 4.1 Proto-on-foreign — already gated by m24 §A

`m13 §4.5.1` bars `proto(...)` on a `kForeign` (Regime A) decl;
`m24 §8` lifts that ban for `kForeignComponent` (Regime B) by
crossing protos as serialized bytes. celfnc inherits the existing
rejection: `FunctionLibrary::Builder::Build()` already rejects
`MentionsProto` on `kForeign` decls and accepts on `kForeignComponent`.
The proto codec is the only emitter that needs a runtime dep on the
author's `<acme.User>.pb.h`; the generated `user_fns.h` adds the
`#include` automatically.

### 4.2 Cross-language reuse — Go via TinyGo

`cel generate --language=go <idl>` emits the Go-flavored versions
of the same four logical outputs: `fns.wit` (unchanged, the
language-agnostic contract), Go `codec.go`, Go `generated_stub.go`,
and a `user_fns.go` skeleton. The author writes the Go bodies;
TinyGo compiles to a wasip2 component. This unblocks D.2 — the
*same `.idl`* builds both a C++ and a Go component and both
register cleanly through `Engine::AddComponent`.

## 5. Toolchain integration — hermetic bazel for darwin × linux × arm64 × x86_64

Four new tools enter the build graph as `http_archive` deps in
`MODULE.bazel`. Each is registered once per host platform, mirroring
the existing `wasmtime_*` and `wasi_sdk_*` patterns
(`MODULE.bazel:~64` and `~100`). Bazel toolchain resolution picks
the matching one per `exec_compatible_with`.

| Tool | Version | URL pattern | Used for |
| --- | --- | --- | --- |
| `wasm-tools` | v1.251.0 | `wasm-tools-1.251.0-{aarch64,x86_64}-{macos,linux}.tar.gz` | `wasm-tools component new` (core wasm → component); also called internally by `tinygo build -target=wasip2` |
| `wit-bindgen` | latest stable | `wit-bindgen-<v>-{aarch64,x86_64}-{macos,linux}.tar.gz` | Generates `author.h` (the `{ptr,len}/{f0,f1}` structs) from `fns.wit` |
| `tinygo` | v0.41.1 | `tinygo<v>.<linux,darwin>-{amd64,arm64}.tar.gz` | Compiles `user_fns.go` + generated Go stub → wasip2 core module |
| `wasi-sdk` | (existing) | already in `MODULE.bazel` | Compiles `user_fns.cc` + generated_stub.cc → wasm32 core module |

Per CLAUDE.md "`bazel/` vs `third_party/`":

  - **`third_party/wasm_tools/`** — http_archive + a thin
    `BUILD.external.bazel` exposing the binary as a runnable target
    (`//third_party/wasm_tools:wasm_tools`).
  - **`third_party/wit_bindgen/`** — same shape.
  - **`third_party/tinygo/`** — http_archive + Go runtime files; a
    bit larger because tinygo distributes a sysroot too.
  - **`bazel/cel_wasm_component.bzl`** — the user-facing macro.
    Lives in `bazel/` (first-party Starlark) per the
    "What goes in `bazel/`" rule.

### 5.1 Cross-platform expectations (set explicitly)

  - **darwin-arm64** (Apple Silicon): primary dev host. All four
    archives have official prebuilt darwin-arm64 binaries.
  - **darwin-x86_64** (Intel Mac): supported same way.
  - **linux-arm64**: supported (matters for ARM CI workers + cheap
    Graviton runners).
  - **linux-x86_64**: supported (canonical CI host).

Each `third_party/<tool>/BUILD.bazel` (the aggregator) does a
`select()` on `@platforms//{os,cpu}` to pick the right
`@<tool>_<host>` repo. Same pattern as `third_party/wasmtime:wasmtime`
today.

No tool requires a tool not in the bazel graph. **No
`brew install`, no `apt install`, no developer-local toolchain
state.** A fresh checkout `git clone && bazel build //...` builds
every component end-to-end on a CI worker.

### 5.2 wasi-sdk extension (small)

The existing `third_party/wasi_sdk/` toolchain compiles the
`cel_runtime.wasm` kernel. m26 adds a second use: it also compiles
the author's `user_fns.cc` + `generated_stub.cc` into the
foreign-fn component. The toolchain is already cross-platform; the
only addition is an `-fno-exceptions` flag on the foreign-fn TU
(component-model wasm leaf code can't carry the exception runtime —
m24 §13 future-work item, now closed).

## 6. `cel_wasm_component` Starlark macro — input/output

```python
load("//bazel:cel_wasm_component.bzl", "cel_wasm_component")

cel_wasm_component(
    name = "rules_component",
    idl  = "fns.idl",
    user_fns = ["user_fns.cc"],
    deps = ["//acme/proto:user_cc_proto"],   # optional, for proto-typed fns
)
```

Pipeline (each step a `genrule` or `cc_binary` action):

1. **`//tools/cel:cel generate --language=cpp --idl=<idl>
   --out-dir=<gen>`** →
   `gen/fns.wit`, `gen/codec.h`, `gen/generated_stub.cc`,
   `gen/user_fns.h`.
2. **`wit-bindgen c <gen>/fns.wit --world author --out-dir <gen>`** →
   `gen/author.h`, `gen/author_component_type.o`.
3. **wasi-sdk `cc_binary`** on `[user_fns.cc, gen/generated_stub.cc,
   gen/author_component_type.o]` with `deps` from the macro
   args and `gen/` on the include path. Output:
   `core_<name>.wasm`.
4. **`wasm-tools component new core_<name>.wasm -o <name>.wasm`** →
   the final component.

For `language = "go"`, step 1 emits Go files and steps 3-4 swap to
the TinyGo path (`tinygo build -target=wasip2`, which calls
`wasm-tools component embed` + `wasm-tools component new` internally).

A `filegroup` for `<name>.wasm` is the public output. Consumers
(`cc_test`, the bench) take it as a `data` dep and load it via
`Engine::AddComponent` from `runfiles`.

### 6.1 Reusing the macro from a `cc_test`

```python
cel_wasm_component(name = "demo_component", idl = "demo.idl", user_fns = ["demo_user_fns.cc"])

cc_test(
    name = "demo_e2e_test",
    srcs = ["demo_e2e_test.cc"],
    data = [":demo_component"],
    deps = ["//eval:engine", "//compiler:compiler", "//eval:instance", "@com_google_googletest//:gtest_main"],
)
```

The test loads `bazel-bin/.../demo_component.wasm` via the bazel
runfiles library, hands it to `Engine::AddComponent`, and exercises
the same `Compile → Plan → Eval` path the m24 e2e dispatch test pins.

## 7. File-by-file pipeline mapping

  - `tools/cel/cel.cc` (MODIFIED) — adds `generate` to the
    subcommand gate (`eval` / `check` / `compile` / **`generate`**)
    and dispatches to `RunGenerate(...)`.
  - `tools/cel/run_generate.{h,cc}` (NEW) — `RunGenerate(...)`
    argument parsing (`--idl`, `--language`, `--out-dir`), IDL load
    via `ParseCelfnSource`, dispatch to the C++ or Go backend in
    `compiler/celfn/celfnc_emit/`.
  - `compiler/celfn/celfnc_emit/wit_emitter.{h,cc}` (NEW) — emits
    `fns.wit`. Pure function: `CelfnType → WIT type string`.
    Language-agnostic.
  - `compiler/celfn/celfnc_emit/cpp_codec_emitter.{h,cc}` (NEW) —
    emits C++ `codec.h`. Per-type lift/lower fns; recursion via
    inner type.
  - `compiler/celfn/celfnc_emit/cpp_stub_emitter.{h,cc}` (NEW) —
    emits C++ `generated_stub.cc`. One export per decl, body wires
    `codec::lift` → `user::Fn(...)` → return.
  - `compiler/celfn/celfnc_emit/cpp_skeleton_emitter.{h,cc}` (NEW) —
    emits `user_fns.h` and (on first run) a stub `user_fns.cc`.
  - `compiler/celfn/celfnc_emit/go_emitter.{h,cc}` (NEW) — Go-language
    emitter (D.2). Mirrors the C++ emitter triplet but in Go syntax.
  - `compiler/celfn/celfnc_emit/celfnc_emit_test.cc` (NEW) —
    unit-tests every emitter against the full m24 §6 type matrix
    plus nested cases. Plus an end-to-end golden test: a fixed
    `.idl` → fixed output files; if a generator drifts, the diff
    fails.
  - `tools/cel/run_generate_test.cc` (NEW) — CLI-level test of the
    `cel generate` subcommand surface (argv parsing, error paths,
    `--language` validation).
  - `bazel/cel_wasm_component.bzl` (NEW) — the macro. Lives in
    first-party Starlark per CLAUDE.md "`bazel/` vs `third_party/`".
  - `third_party/wasm_tools/{BUILD.bazel, BUILD.external.bazel}` (NEW)
  - `third_party/wit_bindgen/{BUILD.bazel, BUILD.external.bazel}` (NEW)
  - `third_party/tinygo/{BUILD.bazel, BUILD.external.bazel}` (NEW)
  - `MODULE.bazel` — http_archive entries for each tool × each host.
  - `e2e/foreign_component_fixtures/stub_demo/BUILD.bazel` (NEW) —
    closes D.1. Builds the stub_demo as a `cel_wasm_component`
    target with the macro.
  - `e2e/foreign_component_fixtures/tinygo_demo/` (NEW) — D.2
    forcing-fn. Same IDL as stub_demo, Go bodies.
  - `bench/foreign_component_bench.cc` (existing, rewritten) — D.3.
    Drops the inline WAT, uses a `cel_wasm_component(language="cpp")`
    fixture for the cost measurement.

## 8. Testing plan

### 8.0 Generated-code compile gate — the user's per-build regression pin

User direction (2026-06-04, during m26 §3.5 design): *"Make sure the
codec generated has bazel cc_binary or cc_library to ensure that it
compiles right. A test I mean. This way when the code changes we can
validate it is compilable."*

The pattern, per-emitter, is a **three-target bazel triplet**:

  1. **`<lang>_<emitter>_dumper`** (`cc_binary`) — a tiny C++ `main`
     that constructs a `FunctionLibrary` covering every type-row
     the emitter handles today, calls the emitter, and writes the
     result to stdout. Inputs are hard-coded; this is a code-gen
     test, not a CLI integration test.
  2. **`generated_<output>` `genrule`** — runs the dumper and
     captures stdout into the output file
     (`bazel-bin/.../generated_codec.h` or analogous).
  3. **`<emitter>_compile_check` `cc_library`** — includes the
     genrule-emitted file alongside `fixtures/author.h` (a
     hand-curated mirror of the wit-bindgen 0.57 C output) and a
     small `.cc` that *references every public symbol* the emitter
     promises to produce. **If the codec emitter ever emits text
     the C++ compiler rejects, `bazel build` fails here** —
     surfacing the regression at the closest possible point
     instead of allowing it to slip into a downstream consumer.

Why a `cc_library` (not a `cc_test`): the failure mode we care about
is *compile* failure, not runtime failure. A `cc_library` that
fails to compile breaks every dependent target, which is exactly
the gating behavior we want at build time.

Concrete example as-shipped — `compiler/celfn/celfnc_emit/`:

  - `codec_dumper.cc` → `cc_binary` (the dumper).
  - `generated_codec_h` `genrule` → `generated_codec.h`.
  - `codec_compile_check` `cc_library` includes `generated_codec.h`
    + `fixtures/author.h` and references every lift/lower overload
    via small `[[maybe_unused]]` helpers (`RefStringCodec`,
    `RefListIntCodec`, …).

When `H.1` lands (wit-bindgen in the bazel graph), `fixtures/author.h`
goes away and the cc_library depends on a wit-bindgen-emitted
`author.h` directly. Until then, the fixture file IS the contract
the codec emitter promises to match — its struct shapes are
byte-exact mirrors of what `wit-bindgen c --world author` produced
during the m26 §3.5 probe.

The same gate gets cloned for the stub emitter (H.2.c) and skeleton
emitter (H.2.d). Each gets its own `_dumper` + `_genrule` +
`_compile_check` triplet. Five lines of BUILD per emitter; pays for
itself the first time a downstream silent break would have shipped.

### 8.1 Emitter unit tests (`celfnc_emit_test.cc`)

Per CLAUDE.md "interface → tests → implementation":

  - Every CEL type × every emitter → assert exact emitted text.
    Cases for every row in §4's matrix.
  - Nested cases: `list<list<int>>`, `list<map<string, list<int>>>`,
    `map<int, list<bytes>>`, etc. — at least 6 nestings.
  - Negative: `optional<T>` and `type` decls → emit fails with the
    m24 §14 rejection message (re-uses `Builder::Build()`'s gates).
  - Negative: illegal map key (`map<double, int>`) → emit fails
    with the m24 §A.5 rejection message.
  - Go-target rows (D.2 prereq): every type emits valid Go too.

### 8.1.1 CLI-level test (`run_generate_test.cc`)

  - `cel generate` with no `--idl` → exit non-zero, named error.
  - `cel generate --language=ocaml` → "unknown language" error
    (only `cpp` and `go` are valid in v1).
  - `cel generate --language=cpp --idl=fixtures/empty.idl
    --out-dir=$tmp` → produces four files; each is non-empty and
    syntactically valid (the C++ `generated_stub.cc` compiles
    against the matching `author.h`).
  - Round-trip: re-run `cel generate` → idempotent, no churn on
    second invocation.

### 8.2 cel_wasm_component integration tests

  - **Empty IDL** → emits empty fns.wit + empty stub; `wasm-tools`
    builds a valid (trivial) component. Pins the build pipeline
    works at the floor.
  - **Single scalar decl** (`int add(int, int)`) → component
    builds; `Engine::AddComponent` accepts; eval returns the right
    answer.
  - **Every §4 row at top level** → one IDL with one decl per type;
    component builds; round-trip in `e2e/cel_wasm_component_matrix_test.cc`.
  - **Nested decls** → at least one `list<map<string, list<int>>>`
    fn; build + eval.
  - **Proto decl** → an IDL using `proto(acme.User)`; macro picks
    up the `deps` proto and builds.
  - **Stub_demo rebuilt under the macro** → original `driver_main.cc`
    (17 cases) re-runs green against the macro-built component.
  - **TinyGo equivalent** (D.2) → same IDL, `--target=go`, TinyGo
    builds, e2e test exercises identical fn calls. Pins that the
    contract is genuinely language-agnostic.

### 8.3 Toolchain platform matrix

CI matrix (post-m26):

  - darwin-arm64 — primary; every test passes.
  - darwin-x86_64 — every test passes.
  - linux-arm64 — every test passes.
  - linux-x86_64 — every test passes.

A small `bazel test //bazel:platform_smoke_test` target builds a
trivial cel_wasm_component on every host and asserts the .wasm is
non-empty.

### 8.4 D.3 bench numbers (closing the "How is perf looking?" loop)

After m26, the bench in `bench/foreign_component_bench.cc` switches
from inline-WAT components to macro-built C++ components. Cases:

  - `BM_Eval_ForeignComponent_AddIntInt` — scalar, minimum dispatch.
  - `BM_Eval_HostFn_AddIntInt` — same shape via `AddTypedFunction`
    (the baseline).
  - `BM_Eval_ForeignComponent_LenString_256KiB` — large-payload.
  - `BM_Eval_HostFn_LenString_256KiB` — baseline.
  - `BM_Eval_ForeignComponent_SumListInt_100k` — large list.

Run under `-c opt` per CLAUDE.md "Benchmark configuration"; report
the foreign-component-vs-host-fn delta as the m24 dispatch overhead.

## 9. Closeout of m24 deferrals

| m24 §14 item | m26 status |
| --- | --- |
| TinyGo forcing-fn fixture (D.2) | **closed** by `--target=go` in celfnc + tinygo_wasm_component macro |
| `bench/foreign_component` (D.3) | **closed** by macro-built fixture + bench rewrite |
| `celfnc` codec generator | **delivered** by m26 §3 + §7 |
| Broader large-payload e2e | natural extension on top of macro fixtures |

## 10. Sequencing — phases

  1. **Phase A — Toolchain** (~3 hr): wasm-tools + wit-bindgen + tinygo
     into bazel as http_archive. Smoke test: `bazel run
     //third_party/wasm_tools:wasm_tools -- --version` works on
     darwin-arm64.
  2. **Phase B — celfnc minimum** (~4 hr): emitters for the type
     matrix needed by stub_demo (the existing fixture). Each
     emitter has unit tests up front (interface→tests→impl).
  3. **Phase C — Macro v1** (~2 hr): cel_wasm_component supports
     the C++ path end-to-end. stub_demo builds under bazel,
     closing D.1.
  4. **Phase D — TinyGo path** (~3 hr): celfnc --target=go,
     tinygo_wasm_component macro, D.2 fixture, e2e test.
  5. **Phase E — Bench rewrite** (~1 hr): D.3 uses macro-built
     components; report `-c opt` numbers.
  6. **Phase F — Full type matrix in celfnc** (~3 hr): every row
     in §4 wired + matrix test.

Total: ~16 hr of focused work.

## 11. Out of scope (will NOT do in m26)

  - **`wac` for component composition.** v1 components have one
    interface each; multi-interface composition lands later.
  - **Rust author surface.** Same shape as Go would work (Rust
    has the most mature wit-bindgen support), but no current
    user demand and adds another toolchain. Future workstream.
  - **Streaming WIT types.** `resource`, `stream`, `future`,
    `error-context` — none in m24 §6, none here.
  - **Multi-module composition.** A `.idl` declares one module's
    worth of fns; combining modules via `wac` is a follow-up.
  - **Author-side `null` handling beyond `option<unit>`.** m24
    keeps null as a wire detail; m26 surfaces it as `std::monostate`
    in C++ and `*Nullable` in Go but doesn't extend the IDL.
  - **`celfnc` re-emitting `user_fns.cc` after the first run.** The
    skeleton is written once on first run; subsequent runs do NOT
    overwrite the author's bodies.

## 12. Risks + mitigations

  - **Tool version skew on the m26 toolchain.** Pinned via
    `http_archive` sha256; bazel will refuse to build if a fetch
    returns a different artifact.
  - **WIT semantic drift between wit-bindgen versions.** Pin
    wit-bindgen to a specific minor; review the diff in
    `gen/author_component_type.o` between version bumps.
  - **TinyGo wasip2 maturity.** `//go:wasmexport` + `wasip2`
    target shipped in TinyGo 0.31 and is stable as of 0.41.
    If TinyGo regresses, falls back to TinyGo 0.41.1 (pinned).
  - **`celfnc` and the existing `function_library` Builder gates
    drifting apart.** Mitigation: `celfnc` parses the IDL via
    `ParseCelfnSource` + calls `FunctionLibrary::Builder::Build()`
    before emitting; if a rejection rule grows, both sides see it.

## 13. Future work (after m26 lands)

  - Rust author surface (`--target=rust`).
  - WIT `resource` (dynamic / variadic fns; the m23 §value-resource
    path, currently parked).
  - `wac` for multi-component composition.
  - `bench:foreign_component_bench` paired with cel-cpp's native
    eval cost for a "what does foreign-fn cost vs cel-cpp's
    in-process call?" reference point.
  - Hot-reload of a foreign component (recompile + AddComponent
    again without restarting the engine).
