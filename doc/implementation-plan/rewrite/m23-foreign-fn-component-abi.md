# M23 — Foreign custom-function ABI: WebAssembly Component Model

Status: research / design exploration — drafted 2026-06-03, not yet
started. No code in `compiler/`, `eval/`, or `runtime/` changes here;
this doc + the validated WIT contract under `abi/wit/` + the throwaway
benchmarks under `bench/foreign_component/` capture the investigation
so a future custom-fn milestone starts from measured ground, not a
guess. (The WIT tree originally lived alongside this doc under
`doc/implementation-plan/rewrite/wit/`; the split-by-role placement
landed when m24 began wiring it in.)

## 0. TL;DR

For **foreign / polyglot / untrusted** custom functions (a CEL custom fn
implemented in Go, Rust, or a separately-compiled C++ unit), the
WebAssembly **Component Model** is the right linking substrate: it is the
standardized form of the host-brokered, private-memory, copy-at-the-
boundary ABI we would otherwise hand-roll. This doc:

  - records why module-to-module CEL value passing reduces to "scalars
    cross function calls; everything else is copied by a broker" (§2);
  - gives a **complete CEL value model in WIT** (`abi/wit/cel.wit`) — every
    CEL type, arbitrary nesting, proto as serialized bytes (§4);
  - measures the boundary cost empirically (§5): a cross-component call
    is **~410 ns fixed**, scalar args are free, and **every typed
    `value`-handle operation is ~500 ns** (so the handle model is ~5x
    the by-value path for a 3-arg call);
  - lands on a **per-type representation rule** and a **two-regime
    split** (§6): trusted customs stay the shared-memory thin-guest
    (single-digit-ns slot-out, the existing `cel_runtime` + expr
    shape); only untrusted/polyglot customs pay the Component Model.

## 1. Problem

A custom function extends CEL with embedder-provided behavior
(`myfunc(x)`). We want to support custom fns that are **compiled
separately, possibly in another language, possibly untrusted** — i.e. a
foreign wasm module the host links against the CEL evaluator. The
question is the **ABI**: how does a CEL value cross from the evaluator
into the foreign module and back?

The constraint that makes this non-trivial: a foreign module compiled as
a normal wasm program (wasi-libc, its own allocator) **owns its own
linear memory**. It cannot share the evaluator's memory without
colliding (two wasi runtimes both place their stack pointer / heap base
at the same offsets — empirically, two independently-compiled modules
both initialise `__stack_pointer` to `66560`). So foreign modules are
isolated by construction, and the only channels between two isolated
wasm instances are:

  - **function calls** carrying `i32/i64/f32/f64` (and references), and
  - a **broker** (the native host) that can read/write both memories.

A pointer never crosses — an `i32` "pointer" is an offset into the
sender's memory, meaningless in the receiver's. So every aggregate
(string, list, map, message) has to be **copied** by the host broker, or
**referenced by an opaque handle** the receiver calls back to read.

## 2. First-principles findings (re-derived, then recognised)

Working the constraint forward yields exactly the contract below — at
which point it becomes clear this *is* the WebAssembly Component Model /
WASI Preview 2 canonical ABI, re-derived. The mapping:

| Hand-rolled contract | Component Model name |
| --- | --- |
| private memory per module | components are isolated by definition |
| host brokers the byte copy | the Canonical ABI (lift / lower) |
| callee-exported `alloc` + reset | `cabi_realloc` + `post_return` |
| position-independent inline blob | the canonical record / list encoding |
| opaque handle + pull accessors | `resource` types |
| ABI version check | WIT package version |
| scalars by value | flattened core params |

The practical consequence: rather than author a bespoke marshalling
layer per language, define the boundary in **WIT** and let
`wit-bindgen` / `wasm-tools` / `wac` generate the lift/lower glue. None
of the foreign-fn marshalling code is hand-written.

## 3. Why not just share memory? (the two regimes)

Sharing one linear memory is *cheaper* (no copy; the existing slot-out
call is single-digit-ns) but only works when the second module is a
**thin, co-designed, trusted guest** that brings no libc/allocator/stack
of its own — which is exactly what the per-expression expr module is
today (it imports `cel.memory` + `cel_*` and borrows the runtime's
allocator; see `wasi/DESIGN.md`). Two *independent* wasi runtimes cannot
share a memory without the `66560` collision.

So there are two distinct regimes, and they want different ABIs:

  - **Regime A — trusted, co-compiled customs.** Keep the shared-memory
    thin-guest shape: the custom-fn module imports `cel.memory` +
    `cel_alloc` and is called via the existing slot-out convention.
    Fast, no copy. The custom author uses our SDK (no libc, speaks
    `CelValue`). This is the natural extension of `cel_runtime` + expr
    to an Nth guest and needs no Component Model.
  - **Regime B — untrusted / polyglot customs.** Separate memory,
    host-brokered copies, typed WIT boundary = the Component Model.
    Any language, real isolation, at the per-call cost measured in §5.

This doc is about Regime B. Regime A is a separate (and cheaper)
milestone.

## 4. The complete CEL value model in WIT (`abi/wit/cel.wit`)

Validated with `wasm-tools component wit` and exercised with
`wit-bindgen c` (all `of-*` constructors and `as-*` accessors generate).

### 4.1 The recursion problem — and why `value` is a `resource`

WIT **forbids recursive value types**. The natural encoding is rejected:

```wit
variant value { int(s64), list(list<value>) }   // error: type `value` depends on itself
```

(confirmed empirically with `wasm-tools`). A CEL value is recursive by
nature (`map<string, list<map<...>>>`), so it **cannot** be a single
self-referential variant. The idiomatic escape is a **`resource`** (a
nominal, opaque handle, which *may* be cyclic): the nestable value is a
resource whose aggregate accessors return **more handles**. That makes
`list<list>`, `map<string, list<map>>`, arbitrarily deep trees all
expressible — the nesting lives in the handles, not in a recursive type.

### 4.2 Coverage — every CEL type

| CEL type | WIT modeling |
| --- | --- |
| null | `null-value` / `kind() == null` |
| bool, int, uint, double | `of-bool`/`as-bool`, ... (`s64`/`u64`/`f64`) |
| string, bytes | `of-string`/`as-string`; `of-bytes`/`as-bytes` (`list<u8>`) |
| list | `of-list(list<value>)` / `list-get(i) -> value`, `list-len` |
| map | `of-map(list<tuple<map-key,value>>)` / `map-get`, `map-keys`, `map-size` |
| type | `of-type(name)` / `as-type` |
| duration, timestamp | `record { seconds: s64, nanos: s32 }` |
| message (proto) | `record message { type-name: string, wire: list<u8> }` — **serialized bytes**, deserialized by the far side |
| optional | `of-optional(option<value>)` / `as-optional() -> option<option<value>>` |
| error | `of-error(msg)` / `as-error` (eval-time) |
| unknown | `as-unknown() -> option<list<string>>` (the unknown attribute trail) |

Map keys are constrained to the spec-allowed scalar kinds via
`variant map-key { bool, int, uint, string }`.

### 4.3 Proto messages cross as bytes

A proto value is **not** walked field-by-field across the boundary (that
would be a handle pull-storm — §5). It crosses as
`record message { type-name, wire: list<u8> }`: the evaluator serializes
the `Message`, the bytes are copied once, and the foreign module
deserializes with its own proto library. Same reasoning applies to any
large fully-read aggregate (§6).

### 4.4 The function interface + worlds

```wit
interface custom-fn {
  use types.{value};
  variant eval-error { no-such-overload(string), invalid-argument(string),
                       divide-by-zero, no-such-field(string), no-such-key(string), range }
  invoke: func(name: string, args: list<value>) -> result<value, eval-error>;
}
world cel-runtime     { export types; import custom-fn; }   // host owns values, calls customs
world custom-provider { import types; export custom-fn; }   // foreign module uses values, impls customs
```

The wiring is **bidirectional**: the runtime exports the `value`
resource and imports `custom-fn`; the provider mirrors it. In a real
deployment the host (native, acyclic) implements `types` and links the
single provider; the benchmark in `bench/foreign_component/typed_fn` flips ownership
(provider owns `value`) to keep the two-component composition acyclic.

## 5. The cost — measured (`bench/foreign_component/`, full numbers + repro in REPRODUCE.md)

Built for real: C++ components, separate memories, composed with `wac`,
run on wasmtime 45. Absolute ns are noisy on the shared host; the ratios
and the per-operation unit are the durable findings.

**Per argument type** (`bench/foreign_component/arg_cost`):

  - **The cross-component CALL is the cost: ~410 ns, fixed**, independent
    of arguments (~100x a same-module wasm call).
  - **Scalars are free** (~1-2 ns each).
  - **A string (20-50 B) adds ~80 ns**, almost all fixed-per-argument:
    the length sweep is ~0.6 ns/byte, so 20 vs 50 bytes differs by
    ~20 ns. The bytes are not the cost; the per-arg ABI work is.

**Typed value model** (`bench/foreign_component/typed_fn`):

  - `invoke-prim(3 ints, by value)`  — **~525 ns** (1 crossing).
  - `as-primitive` (pull one value)  — **~543 ns** (1 crossing).
  - `of-primitive` + `drop`          — **~1000 ns** (2 crossings).
  - `invoke(3 handles)`              — **~2500 ns** (~4 crossings, ~5x).

The single load-bearing fact: **every `value`-resource operation
(construct / pull / drop / the call) is one ~500 ns crossing.** Handles
do not amortize. A value built then read once costs `construct + pull`
~= 1 µs before any work happens.

## 6. Design recommendation

**Choose the representation per value at the call site**, driven by the
§5 costs:

  - **Scalars (null/bool/int/uint/double/string/bytes/duration/timestamp)
    -> pass by value** as a `primitive` variant. ~525 ns/call, args
    free. The 90% path.
  - **Nested aggregates read sparsely -> `value` handles.** Correct and
    arbitrarily deep, but ~500 ns per node touched — fine for reading a
    few fields, a pull-storm for walking a whole tree.
  - **Proto messages, and any large aggregate read in full -> serialized
    `bytes`.** One copy, decode locally; never a ~500 ns x N handle
    walk.

And the **regime split** from §3: trusted customs stay Regime A
(shared-memory thin-guest, single-digit-ns slot-out); only
untrusted/polyglot customs pay Regime B's ~410 ns + per-arg cost. The
Component Model's typed elegance costs ~500 ns per value operation, which
is precisely why it is the wrong tool for the hot path and the right tool
for the isolation boundary.

## 7. Reproduce

`abi/wit/cel.wit` validates with `wasm-tools component wit abi/wit/cel.wit`.
The benchmarks + the exact toolchain versions and build/compose/run
commands are in `bench/foreign_component/REPRODUCE.md`. The toolchain
(`wasm-tools`, `wit-bindgen`, `wac`, `wasmtime`) is external to the
bazel build — these are disposable probes, not regression tests.

## 8. Future work / open questions

  - **Compiler-side integration** of this ABI into the cel-spec-wasm
    pipeline (frontend / overload table / codegen / eval bridge) is
    designed in [`m24-foreign-fn-component-backend.md`](m24-foreign-fn-component-backend.md)
    — it provides the foreign backend `m13-custom-fns.md` deferred,
    dispatched as a host callback over the wasmtime component API.
  - **Regime A design** (trusted shared-memory custom-fn guest) is the
    cheaper, more common case and is unscoped here — its own milestone.
  - **Host-implemented `types`**: the real deployment has the native
    host implement the `value` resource (acyclic) rather than a peer
    component; needs the wasmtime component host API (Rust `bindgen!` or
    the C API). The benchmark used a provider-owned resource to dodge
    the instantiation cycle; a host-owned measurement would pin the
    realistic pull direction.
  - **`-c opt`-grade numbers**: §5 is wasmtime-45 default JIT on a noisy
    shared host; component-call overhead is an active upstream
    optimization target, so re-measure before treating the ~410 ns
    baseline as fixed.
  - **One-copy aggregate path**: a `bytes`-blob `invoke` variant
    (the TLV encoding) was not benchmarked head-to-head against the
    handle pull-storm; that table would make the §6 "bytes for full
    reads" rule quantitative.
