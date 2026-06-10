# Reproducing the component-model foreign-fn benchmarks

These two throwaway benchmarks measure the WebAssembly **Component Model**
boundary cost for a CEL custom-function ABI: how much a cross-component
call costs, how that scales with argument type/count, and how the typed
`value`-resource (handle) model compares to passing primitives by value.

They are **not** part of the bazel build — they use the external
component-model toolchain (`wit-bindgen`, `wasm-tools`, `wac`,
`wasmtime`), not the in-repo wasi-sdk pipeline. They are kept here as the
empirical backing for `../../m23-foreign-fn-component-abi.md`; treat them
as reproducible probes, not regression tests.

## Toolchain (all from GitHub releases)

```
wasm-tools 1.251.0   github.com/bytecodealliance/wasm-tools
wit-bindgen 0.57.1   github.com/bytecodealliance/wit-bindgen      (the `c` generator)
wac 0.10.0           github.com/bytecodealliance/wac              (`wac plug` composer)
wasmtime 45.0.0      github.com/bytecodealliance/wasmtime         (CLI + P1 adapters)
wasi-sdk 25 (clang 19) for the wasm32-wasi compile
```

The two P1->P2 adapter modules (`wasi_snapshot_preview1.reactor.wasm`,
`wasi_snapshot_preview1.command.wasm`) are release assets of wasmtime.

## arg-cost/ — per-argument-type boundary cost

A C++ `engine` component (exports an interface with `noop` / `i64 x{3,5}`
/ `str x{1,3,5}` / `bytes` / string-return functions, trivial bodies) and
a C++ `runner` command that times each shape in a tight loop.

```bash
WIT=arg-cost
wit-bindgen c ./$WIT --world engine-world --out-dir gen-engine
wit-bindgen c ./$WIT --world runner-world --out-dir gen-runner
# compile bindings as C, impls as C++, link (engine = reactor, runner = command)
clang     --target=wasm32-wasi --sysroot=$SR -O2 -c gen-engine/engine_world.c -o eb.o
clang++   --target=wasm32-wasi --sysroot=$SR -O2 -I gen-engine -c arg-cost/engine_impl.cc -o ei.o
clang++   --target=wasm32-wasi --sysroot=$SR -O2 -mexec-model=reactor ei.o eb.o gen-engine/engine_world_component_type.o -o engine.core.wasm
# ... runner symmetrically (no -mexec-model; it has main()) ...
wasm-tools component new engine.core.wasm --adapt wasi_snapshot_preview1=...reactor.wasm -o engine.component.wasm
wasm-tools component new runner.core.wasm --adapt wasi_snapshot_preview1=...command.wasm -o runner.component.wasm
wac plug runner.component.wasm --plug engine.component.wasm -o app.wasm
wasmtime run app.wasm
```

## typed-fn/ — typed CEL custom-fn: by-value vs handle

A C++ `provider` that owns a `value` resource and implements
`invoke(list<value>)` (handles) + `invoke-prim(list<primitive>)`
(by value); a C++ `driver` command that times `invoke-prim`,
`as-primitive` (pull), `of-primitive + drop`, and the full handle path.
Same compile/componentize/compose pipeline (provider = reactor exporting
two interfaces, driver = command importing them; `wac plug driver --plug
provider`).

## Observed numbers (wasmtime 45, default Cranelift JIT, shared 4-core cloud box)

Absolute ns are noisy (~10-15% run-to-run on this host, occasional 2x
outliers); the **ratios** and the per-operation unit are the durable
findings.

### arg-cost (median of clean runs, ns/call)

```
noop                ~410     the cross-component CALL itself (the fixed tax)
i64 x3 / x5         ~410     scalars are essentially free (~1-2 ns each)
str x1              ~485     one string (20-50 B)
str x3              ~648     ~80 ns / string
str x5              ~795     ~77 ns / string
bytes x1            ~505     list<u8> ~= string at this size
str x3 -> str       ~759     +~110 ns for the returned string
str length sweep (x3):  L=20 ~620   L=35 ~680   L=50 ~675   => ~0.6 ns/byte
```

Takeaway: **the call is the cost (~410 ns fixed); arguments are cheap;
string length in the 20-50 B band barely matters (~0.6 ns/byte).**

### typed-fn (ns/call)

```
invoke_prim(3 ints, by value)        ~525    1 boundary call  (RECOMMENDED)
as_primitive (pull 1 value)          ~543    1 boundary call  (the pull)
of_primitive + drop (1 handle)      ~1000    2 boundary calls (construct + drop)
invoke(3 handles): 3x build + call  ~2500    4 boundary calls (~5x the by-value path)
```

Takeaway: **every resource operation (construct / pull / drop / call) is
one ~500 ns crossing.** Passing primitives by value is ~1 crossing;
passing the same 3 args as `value` handles is ~4 crossings (~5x).
