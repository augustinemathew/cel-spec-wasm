# Playground — try cel-wasm in your browser

The **[cel-wasm playground :material-open-in-new:](playground/){target=_blank}**
is the whole pipeline running in a single browser tab — no install, no
server, nothing to trust beyond the page you already loaded.

[Open the playground :material-rocket-launch:](playground/){ .md-button .md-button--primary target=_blank }

## What it does

Type a CEL expression, declare its free variables, and the page:

1. **Compiles** it to a portable `.wasm` Program — running the *same*
   cel-cpp parser + type-checker + Binaryen codegen the native compiler
   uses, cross-compiled to WebAssembly and executed **client-side**. No
   `POST` to a build server: the compiler itself is wasm.
2. Lets you **download** that `.wasm` artifact — the exact bytes you'd
   ship to any host.
3. **Runs** it right there, fully client-side, via the pure-TypeScript
   [`@cel-wasm/eval`](https://github.com/augustinemathew/cel-spec-wasm/tree/master/bindings/ts/eval)
   evaluator — `Engine.create()` → `plan(program)` → `instance.eval(activation)`.

That round-trip is the architecture's thesis made tangible: **a compiled
CEL Program is just wasm + a `cel.abi` descriptor, and every browser
already has a WebAssembly engine** — so evaluation is pure client-side
code that runs identically in Node and the browser.

!!! note "First compile downloads the compiler (~6 MB gzipped)"

    The in-browser compiler (`compiler.wasm`) is ~54 MB raw / ~6 MB
    gzipped. The page lazy-fetches it on your **first** compile and shows
    a *"Loading compiler…"* state; every compile after that is instant.
    Evaluation never downloads anything — it is already in the page.

!!! warning "Diagnostics are coarse in the browser build"

    The stock wasi-sdk build of the compiler has no C++ exception
    runtime, so an **invalid** expression surfaces a single generic
    *"Invalid CEL expression"* message rather than cel-cpp's precise
    line/column diagnostic. The native CLI (and a future emscripten
    build) report the full diagnostic; use those when you need exact
    error locations.

## Examples to try

The playground seeds a few; a good first one:

```text
age >= 18 && country in ["US", "CA"]
```

with variables

```text
age:int=25
country:string=US
```

evaluates to `true` — compiled and run without ever leaving your tab.

## How it's built

The playground is a static single-page app (Vite + Monaco) built from the
[`bindings/ts/web`](https://github.com/augustinemathew/cel-spec-wasm/tree/master/bindings/ts/web)
package and published alongside these docs. It needs no backend and no
Bazel at serve time — just the committed `compiler.wasm` asset and the
pure-TypeScript `@cel-wasm/compiler` / `@cel-wasm/eval` packages.
