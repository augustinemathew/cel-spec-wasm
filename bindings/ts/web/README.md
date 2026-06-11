# @cel-wasm/web — the compile → download → run playground

A Vite + Monaco single-page app that proves the cel-wasm thesis end to end:

> Type a CEL expression → **Compile** it to a portable `.wasm` Program →
> **Download** that artifact → **Run** it right there in the browser and
> see the result.

The app is **fully static** — no server at any step. Both halves of the
loop run client-side, so the built `dist/` is plain files servable by any
static host (it is published to GitHub Pages alongside the docs site).

**Live:** <https://augustinemathew.github.io/cel-spec-wasm/playground/>

## Architecture: compile and eval both run in the browser

- **Compile** runs `compiler.wasm` — the real cel-cpp parser +
  type-checker + Binaryen codegen, cross-compiled to wasm32-wasi —
  **client-side** via `@cel-wasm/compiler`'s `WasmCompileBackend` (a
  hand-written WASI shim, no `node:wasi`). `src/internal/compile-client.ts`
  lazy-`fetch`es `${BASE_URL}compiler.wasm` on the first compile, caches
  the instantiated backend, then calls `backend.compile({ source, vars })`.
  The compiler wasm is ~54 MB raw / ~6 MB gzipped, so the UI shows a
  one-time _"Loading compiler (~6 MB)…"_ state during that first fetch.

- **Run** is pure client-side. `src/internal/run.ts` imports
  `@cel-wasm/eval` and does
  `Engine.create()` → `engine.plan(program)` → `instance.eval(activation)`
  in the page. No network hop. The `cel.abi` descriptor is decoded from
  the wasm bytes client-side (`decodeAbi`).

- **Download** saves `program.wasm` (a `Blob`) locally — the literal
  portable artifact (`src/internal/download.ts`).

```
 Monaco editor ──Compile──► compiler.wasm (client-side) ──► Program wasm bytes
       │                                                              │
       │                                        (decodeAbi, client-side)
       ▼                                                              ▼
  inline error panel  ◄── CelCompileError       Program { wasm, abi } ──Download──► program.wasm
                                                          │
                                                      Engine.plan
                                                          │
                                                 instance.eval(activation)
                                                          ▼
                                                   result (client-side)
```

### Known limitation — coarse diagnostics in the browser

The stock wasi-sdk `compiler.wasm` has no C++ exception runtime, so an
**invalid** expression cannot recover cel-cpp's line/column diagnostic —
the backend throws a single generic `CelCompileError`. The error panel
shows the message plus a note that precise line/col diagnostics need the
native (or a future emscripten) backend. Valid expressions compile
byte-identically to the native compiler.

## Running the demo

From `bindings/ts/` once (`npm install`), then from `bindings/ts/web/`:

```sh
npm run dev      # Vite dev server, base = / (static, no endpoint)
npm run build    # tsc typecheck then a static vite build → dist/web/
npm run preview  # serve the built static bundle
```

No Bazel and no native CLI are needed at any point — the demo uses the
committed `public/compiler.wasm` asset plus the pure-TypeScript packages.

### Base path

`npm run dev` serves from `/`. `npm run build` (and `npm run preview`)
emit asset URLs under the Pages subpath `/cel-spec-wasm/playground/` so
the static bundle works under GitHub Pages. Override with the `VITE_BASE`
env var if the site moves (the Pages CI sets it explicitly).

## Manual browser walkthrough (the parts only a browser can verify)

`npm run dev`, open the printed URL, then for each seeded example:

1. **Access check** — `age >= 18 && country in ["US", "CA"]` with
   `age:int=25` / `country:string=US`. Press **Compile** → first time, a
   _"Loading compiler…"_ status appears while `compiler.wasm` downloads;
   then the Program panel shows the wasm size + ABI variables. Press
   **Run** → `true`. Change `age:int=16` and **Run** again → `false`.
2. **List comprehension** — `[1, 2, 3].map(x, x * 2)` → `[2, 4, 6]`.
3. **String builtin** — `"hello".size()` → `5`.
4. **Divide by zero** — `1 / 0` → an _error value_
   (`error: divide by zero (code 11)`), not a crash — proving CEL spec
   errors propagate as values.

Also confirm:

- **Coarse error path** — type `1 +` and **Compile**: the error panel
  shows the generic _"Invalid CEL expression"_ message plus the
  native-backend note. (No red squiggle/column — the browser build can't
  recover the location; see the limitation above.) Compiling a valid
  expression afterward must still work — the backend recovers from the
  exception-escape by replacing its instance.
- **Download** — press **Download .wasm** after a successful compile; the
  browser saves `program.wasm`. (Re-uploading it to any wasm engine runs
  the same Program — that's the artifact's whole point.)
- **Fully client-side** — open the network panel: after the one-time
  `compiler.wasm` fetch, neither **Compile** nor **Run** issues any
  request.

## Headless verification (what CI / this environment checks)

These run without a browser and gate the package:

- `npm run build` — `tsc` (src + `vite.config.ts`) is clean, and
  `vite build` produces a static bundle under `dist/web/` (including the
  copied `compiler.wasm`). No server, no Bazel.
- `npx vitest run web` — the unit + integration suites:
  - `src/internal/variables.test.ts` — the `name:type=value` parser matrix
    (every scalar type, boundary values, the reject set).
  - `src/internal/render.test.ts` — the `CelValue` display projection over
    the whole union (scalars, bigint, bytes, list, map, message,
    timestamp, duration, error).
  - `src/internal/compile-client.test.ts` — the lazy-fetch, backend-cache,
    and load-hook transport plus error shaping (backend + `decodeAbi`
    stubbed).
  - `src/run.test.ts` — **the end-to-end run-path proof**: compile through
    the committed `compiler.wasm` via the same `WasmCompileBackend` the SPA
    uses, then evaluate through the exact client-side path (`runProgram` →
    `@cel-wasm/eval`), asserting the result for every seeded example and
    the invalid-expression `CelCompileError` + recovery.

The Monaco integration itself (`controller.ts`, `monaco-cel.ts`,
`index.ts`, `main.ts`) can only run in a real browser — Monaco's entry
touches browser-only globals and cannot be imported under node/jsdom — so
those are verified by `vite build` plus the manual walkthrough above. The
compile → run wiring _beneath_ the editor is fully exercised headlessly by
`run.test.ts`.

## How it ships to GitHub Pages

`.github/workflows/pages.yml` builds the mkdocs site, then (with Node, no
Bazel) runs `npm ci` + `npm run build -w @cel-wasm/{eval,compiler,web}`
with `VITE_BASE=/cel-spec-wasm/playground/`, and copies
`bindings/ts/web/dist/web/` (including `compiler.wasm`) into
`site/playground/`. The published docs page `doc/playground.md` links to
it.
