# @cel-wasm/web — the compile → download → run demo

A Vite + Monaco single-page app that proves the cel-wasm thesis end to end:

> Type a CEL expression → **Compile** it to a portable `.wasm` Program →
> **Download** that artifact → **Run** it right there in the browser and
> see the result.

Compile errors surface inline in the Monaco editor; the run happens
**fully client-side** (pure TypeScript, no server hop at eval time) — the
same `.wasm` a server would run, executing in a browser tab.

## Architecture: compile at a dev endpoint, eval in the browser

The browser cannot subprocess the native `cel` CLI, so the two halves of
the loop run in different places:

- **Compile** is a tiny dev-server endpoint. `vite.config.ts` mounts a
  `POST /api/compile` middleware (`dev-server/compile-handler.ts`) that
  runs in Node, drives the native `cel` CLI, and returns
  `{ ok, wasmBase64, byteLength }` (success) or
  `{ ok: false, error, diagnostics }` (a parse / type-check failure).
  This is the design's documented fallback until the emscripten
  `compiler.wasm` lands; the UI swaps to a client-side compile by changing
  only `src/internal/compile-client.ts`, nothing above the fetch.

- **Run** is pure client-side. `src/internal/run.ts` imports
  `@cel-wasm/eval`, decodes the wasm bytes, and does
  `Engine.create()` → `engine.plan(program)` → `instance.eval(activation)`
  in the page. No network hop. The `cel.abi` descriptor is decoded from
  the wasm bytes client-side (`decodeAbi`), so the Node endpoint stays
  dependency-light (no protobufjs).

- **Download** saves `program.wasm` (a `Blob`) locally — the literal
  portable artifact (`src/internal/download.ts`).

```
 Monaco editor ──Compile──► POST /api/compile ──► cel CLI (Node) ──► wasm bytes
       │                                                                  │
       │                                            (decodeAbi, client-side)
       ▼                                                                  ▼
  inline error markers  ◄── diagnostics            Program { wasm, abi } ──Download──► program.wasm
                                                              │
                                                          Engine.plan
                                                              │
                                                     instance.eval(activation)
                                                              ▼
                                                       result (client-side)
```

## Running the demo

From `bindings/ts/` once (`npm install`), then from `bindings/ts/web/`:

```sh
npm run dev      # Vite dev server with the /api/compile endpoint
npm run build    # tsc typecheck (src + dev-server) then a static vite build
npm run preview  # serve the built static bundle (no compile endpoint)
```

The compile endpoint needs the native `cel` CLI. Build it once from the
repo root:

```sh
bazel build //tools/cel:cel
```

or point the endpoint at a binary with `CEL_CLI=/path/to/cel npm run dev`.

## Manual browser walkthrough (the parts only a browser can verify)

`npm run dev`, open the printed URL, then for each seeded example:

1. **Access check** — `age >= 18 && country in ["US", "CA"]` with
   `age:int=25` / `country:string=US`. Press **Compile** → the Program
   panel shows the wasm size + ABI variables. Press **Run** → `true`.
   Change `age:int=16` and **Run** again → `false`.
2. **List comprehension** — `[1, 2, 3].map(x, x * 2)` → `[2, 4, 6]`.
3. **String builtin** — `"hello".size()` → `5`.
4. **Divide by zero** — `1 / 0` → an _error value_
   (`error: divide by zero (code 11)`), not a crash — proving CEL spec
   errors propagate as values.

Also confirm:

- **Inline diagnostics** — type `1 +` and **Compile**: a red squiggle
  appears in Monaco at the error column and the error panel lists the
  located diagnostic.
- **Download** — press **Download .wasm** after a successful compile; the
  browser saves `program.wasm`. (Re-uploading it to any wasm engine runs
  the same Program — that's the artifact's whole point.)
- **Client-side eval** — open the network panel: **Run** issues **no**
  request. Only **Compile** hits `/api/compile`.

## Headless verification (what CI / this environment checks)

These run without a browser and gate the package:

- `npm run build` — `tsc` (src + dev-server + `vite.config.ts`) is clean,
  and `vite build` produces a static bundle under `dist/web/`.
- `npx vitest run web` — the unit + integration suites:
  - `src/internal/variables.test.ts` — the `name:type=value` parser matrix
    (every scalar type, boundary values, the reject set).
  - `src/internal/render.test.ts` — the `CelValue` display projection over
    the whole union (scalars, bigint, bytes, list, map, message,
    timestamp, duration, error).
  - `src/internal/compile-client.test.ts` — the `/api/compile` transport +
    error shaping (base64 decode, diagnostics, request body).
  - `dev-server/compile-handler.test.ts` — request validation + a real
    compile through the native CLI (skips if the CLI is not built).
  - `src/run.test.ts` — **the end-to-end run-path proof**: compile via the
    same handler the browser calls, then evaluate through the exact
    client-side path (`runProgram` → `@cel-wasm/eval`), asserting the
    result for every seeded example.

The Monaco integration itself (`controller.ts`, `monaco-cel.ts`,
`index.ts`, `main.ts`) can only run in a real browser — Monaco's entry
touches browser-only globals and cannot be imported under node/jsdom — so
those are verified by `vite build` plus the manual walkthrough above. The
compile → run wiring _beneath_ the editor is fully exercised headlessly by
`run.test.ts`.
