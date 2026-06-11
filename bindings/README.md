# cel-wasm bindings

Language bindings for the cel-wasm AOT compiler + evaluator, plus a
browser demo. The thesis: a compiled CEL `Program` is **just wasm + a
`cel.abi` descriptor**, and every JS host already has a WebAssembly
engine — so evaluation is pure TypeScript and runs identically in Node
and the browser.

```
            bindings/c  (extern "C" over the C++ Compiler)
                 │
        ┌────────┴─────────┐
        ▼                  ▼
   N-API addon       emscripten compiler.wasm     (two backends,
   (Node, now)       (browser, stretch)            one TS interface)
        │                  │
        └────────┬─────────┘
                 ▼
   @cel-wasm/compiler  ──►  Program (.wasm + cel.abi bytes)
                                 │
        ┌────────────────────────┘
        ▼
   @cel-wasm/eval   (pure TS: instantiate, marshal, host fns, decode —
                     runs in Node AND the browser)
```

## Layout

| Path                       | What                                                                   |
| -------------------------- | --------------------------------------------------------------------- |
| `bindings/c/`              | The C ABI (`extern "C"`) over the C++ `Compiler` — the compile seam.   |
| `bindings/ts/`             | The npm-workspaces monorepo (see below).                              |
| `bindings/ts/eval/`        | `@cel-wasm/eval` — pure-TS evaluator + the shared wire-format types.   |
| `bindings/ts/compiler/`    | `@cel-wasm/compiler` — CEL source → portable `Program`.               |
| `bindings/ts/conformance/` | `@cel-wasm/conformance` — the corpus harness + monotonic ratchet.     |
| `bindings/ts/web/`         | `@cel-wasm/web` — the Monaco compile → download → run browser demo.   |

The shared wire-format type contracts (`CelValue`, `CelInput`, `CelAbi`,
the kind / error-code / offset constants, `MessageBacking`,
`HostFunction`, `Program`) live in `bindings/ts/eval/src/types.ts` and
are the single source of truth, mirroring `runtime/cel_data.h` and
`abi/cel_abi.proto` byte-for-byte.

> The C ABI (`bindings/c/`) and the N-API / emscripten backends are
> built by later work items; today only the TypeScript scaffold + the
> shared types are populated.

## Develop (TypeScript)

From `bindings/ts/`:

```sh
npm install
npm run lint     # eslint (strict-type-checked) + prettier --check
npm run build    # tsc --build across all packages
npm test         # vitest
```

See [`bindings/ts/CONTRIBUTING.md`](ts/CONTRIBUTING.md) for the full
style / strictness / testing guidelines.
