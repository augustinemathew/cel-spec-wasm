# Repo rules for Claude (cel-wasm TypeScript bindings)

This directory is the **TypeScript bindings** for the cel-wasm AOT
compiler + evaluator, plus a browser demo. The thesis the bindings
exist to prove: a compiled CEL `Program` is **just wasm + a `cel.abi`
descriptor**, and every JS host already has a WebAssembly engine — so
**evaluation is pure TypeScript** and runs identically in Node and the
browser, with no wasmtime and no C++ at eval time.

These rules are TypeScript-specific and **layer on top of the root
[`CLAUDE.md`](../../CLAUDE.md)** (the compiler's repo-wide rules) and the
global TS rules in `~/.claude/CLAUDE.md`. Where this file is silent, the
root rules still apply — design-doc-first, test-first, no unprompted
later-milestone work, honest reporting.

## Layout — npm workspaces, organised by binding role

```
bindings/
  c/compiler/            extern "C" over the C++ Compiler — the compile seam
  ts/                    the npm-workspaces monorepo (this dir)
    eval/        @cel-wasm/eval        pure-TS evaluator + the shared wire types
    compiler/    @cel-wasm/compiler    CEL source → portable Program (via bindings/c/compiler)
    conformance/ @cel-wasm/conformance the corpus harness + monotonic ratchet
    web/         @cel-wasm/web         Monaco compile → download → run demo
```

The dependency direction is **one-directional**, mirroring the C++ side:

- `@cel-wasm/eval` depends on nothing first-party — it is the pure-TS
  core (instantiate, marshal, host trampolines, decode). It owns the
  **shared wire-format type contracts**.
- `@cel-wasm/compiler` depends on `@cel-wasm/eval` for the `Program` /
  `CelAbi` types only, and wraps the C ABI (`bindings/c/compiler`) through two
  interchangeable backends (N-API now, emscripten later).
- `@cel-wasm/conformance` and `@cel-wasm/web` are leaves that consume
  both.

**`eval/src/types.ts` is the single source of truth.** `CelValue`,
`CelInput`, `CelAbi`, `MessageBacking`, `HostFunction`, `Program`, and
**every** wire-layout constant (kinds, error codes, byte offsets,
strides) live there and are re-exported from each package. Never
re-declare a wire constant or a public value type anywhere else — import
it. The layout mirrors `runtime/cel_data.h` and `abi/cel_abi.proto`
byte-for-byte; each constant carries a comment citing the C header line.

## Authoritative docs

Read these before non-trivial changes; update them in the same commit as
the code if a change invalidates them.

- `doc/implementation-plan/rewrite/m29-typescript-bindings.md` — the
  design + the parallel work-breakdown. **§A.4** is the frozen wire
  spec the binding re-implements; **§A.5** is the canonical API;
  **§A.6** is the coding/lint contract this file operationalises;
  **Part B** is the work items + dependency graph. Tick its
  definition-of-done items as they land.
- `CONTRIBUTING.md` (this dir) — the long-form style/strictness/test
  guidelines; this CLAUDE.md is the enforced summary.
- `runtime/cel_data.h`, `runtime/cel_layout.h`, `abi/cel_abi.proto`,
  `eval/internal/abi_decode.cc`, `eval/internal/cel_host.h` — the
  C++/runtime sources the wire formats are frozen by. When in doubt
  about a byte, read the header, don't guess.

## Scope (what these bindings do and do NOT do)

**In:** the full static-subset CEL surface (scalars, strings, bytes,
lists, maps, arithmetic/comparison/logic/string/conversion operators,
comprehensions, timestamp/duration); **protobuf** (field reads,
presence, construction, WKT unwrap, message equality — descriptor-backed
via protobufjs); **JSON objects _and_ protos in the activation**; **host
functions** (`@host`) as JS callbacks; compile via the C ABI;
conformance + e2e; the browser demo.

**Out — do not build these unprompted:** unknowns / partial evaluation
(no `AttributeEntry`, no unknown patterns); `@component` / `@native`
custom functions; `Any` / dynamic-message resolution beyond the supplied
descriptors; signed Program artifacts. These are out of scope by design
— a binding that references them is wrong, not incomplete.

## TypeScript style

Follow the [Google TypeScript Style Guide](https://google.github.io/styleguide/tsguide.html).
ESLint (`eslint.config.mjs`) and Prettier (`.prettierrc`) are
**authoritative** — match them, never `// eslint-disable` around a
finding without an explicit, justified reason.

- **Strict, no escape hatches.** `tsconfig.base.json` is the floor:
  `strict`, `noUncheckedIndexedAccess`, `exactOptionalPropertyTypes`,
  `noImplicitOverride`, `noUnusedLocals`, `noUnusedParameters`,
  `verbatimModuleSyntax`, `isolatedModules`. Do not loosen any of
  these to make code compile — fix the code.
- **No `any`** (lint-error). Use `unknown` + narrowing. No
  non-null `!` assertions to dodge `noUncheckedIndexedAccess`; narrow
  or guard instead.
- **Explicit return types on every exported function.** Exported
  boundaries are a contract; let inference stay internal.
- **Named exports only** — no default exports (lint-enforced). One
  logical unit per module; non-exported surface goes under
  `internal/`, the readability pair to "not in the package's public
  `index.ts`".
- **Errors throw, never return sentinels.** The idiomatic-TS API
  surface throws typed `Error` subclasses (`CelCompileError`,
  `CelEvalError`) carrying a `.code` — **not** a C++-shaped `StatusOr`.
  The one exception is **CEL spec errors inside eval**: those are
  `CelError` _values_ on the wire (e.g. divide-by-zero), absorbed and
  propagated as values per §A.4.5 — they are returned, never thrown.
- **Idiomatic value types:** `bigint` for int64/uint64, `Uint8Array`
  for bytes, `Promise` for the async wasm instantiate, discriminated
  unions for `CelValue`. Never transliterate the C++ shapes.
- **No milestone/work-item refs in code comments** (same rule as C++).
  Comments describe what the code does and why, in terms that stay
  true after the WI closes. Cite a doc _path_ + a C-header _line_ for
  a non-obvious wire invariant; never cite "WI-1.4c". (The sole
  carve-out: a `throw new Error('… is a stub until WI-N')` in a
  signature-final stub, deleted when the WI lands.)

## The wire format is law

Every byte-layout constant (offset, kind number, error code, stride) is
a named `const` in `eval/src/types.ts` with a comment citing the
`runtime/cel_data.h` (or `cel_layout.h` / `cel_abi.proto`) line it
mirrors — **and pinned by a round-trip test against a real Program
produced by the C++ compiler** (the golden fixtures). A wire constant
without both a citation and a fixture-backed test is a latent
miscompile: if the C++ side renumbers a kind or moves a payload offset,
the test must fail loudly rather than the codec silently misreading
bytes. Treat the fixtures (`eval/fixtures/`) as the contract the codec
satisfies.

## Testing is mandatory

Same discipline as the C++ side — a binding that marshals bytes fails
silently when an unhandled shape slips through.

- **Strict author order: interface → tests → implementation.** Write
  the `.ts` interface (the exported types/signatures) first; then write
  the **full** positive + negative + boundary test matrix against that
  interface (bodies present, `it.skip` / `it.todo` where the impl
  doesn't exist yet) **before** the implementation; then implement,
  un-skipping each case as its path goes green. The tests are the
  spec the implementation satisfies — writing them first is what stops
  the impl from defining its own wrong contract.
- **Every `src/foo.ts` has a colocated `src/foo.test.ts`.** A new
  module without a paired test is incomplete, even if an e2e test
  happens to cover it.
- **Exhaust the matrix — this is a codec/compiler.** For every value
  kind: every allowed type (each scalar, each aggregate, each map-key
  kind), every boundary (`INT64_MIN/MAX`, `UINT64_MAX`, `0`, `-1`,
  empty string, embedded NUL, multi-byte UTF-8, empty list/map), and
  every disallowed shape that must reject. Negative coverage is the
  load-bearing half.
- **`doc/langdef.md` is the source of truth** for CEL semantics (3VL,
  coercion, comprehension, equality). Cite the spec section in the
  test when behaviour is spec-mandated; do not assert whatever shape
  the impl happens to produce.
- **vitest**, `vitest run` for CI. Parameterize structurally-identical
  cases with `it.each` once the matrix is settled, but draft them
  longhand first and keep distinct-story cases (a specific bug, a
  specific spec citation) as their own `it`.
- **Regression tests ship in the same commit as the fix**, named after
  the bug / conformance row so the test↔row mapping is greppable. A
  behaviour gap that can't pass yet is an `it.skip('…', …)` carrying
  the assertion it _will_ make and a **verified** reason naming the
  concrete blocker — never a silent omission.

## Development flow

From `bindings/ts/` (npm workspaces; Node ≥ 20):

```sh
npm install            # restore workspace deps (protobufjs, the toolchain)

# Inner loop — run the package you're touching:
npm run build -w @cel-wasm/eval      # tsc --build for one package
npm test    -w @cel-wasm/eval        # vitest for one package
npx vitest watch eval/src/celvalue   # focused watch while iterating

# Gate — before every commit, run all three across all packages:
npm run lint           # eslint (strict-type-checked) + prettier --check  → 0 warnings
npm run build          # tsc --build across the workspace project graph
npm test               # vitest run, all packages
npm run format         # prettier --write (fix formatting, then re-lint)
```

The commit gate is **`lint` + `build` + `test` all green across every
package** — the TS mirror of the C++ `scripts/lint.sh --branch` +
`bazel test` gate. Run the touched package in the loop; run the full
three at the gate. Regenerate the golden fixtures with
`scripts/build-fixtures.sh` only when the C++ wire format intentionally
changes (it shells out to the `cel` CLI + bazel) — fixtures are
committed, not rebuilt per run.

Update `m29-typescript-bindings.md` (tick the WI / definition-of-done)
and `doc/implementation-plan/testing-checklist.md` in the same commit as
the feature.

## What not to do

- **Don't introduce dynamic typing or unknowns.** The binding accepts
  only the static subset; there is no `dyn`, no partial eval. An
  `UNKNOWN`/`OPTIONAL` CelValue is out of scope (pass through as
  error-ish), never a supported path.
- **Don't transliterate the C++ API.** No `StatusOr`, no out-params,
  no `absl`-shaped names. Throw, return `bigint`/`Uint8Array`, use
  `Promise` and discriminated unions (§A.5).
- **Don't re-declare wire constants or public value types** outside
  `eval/src/types.ts` — import them.
- **Don't loosen `tsconfig` strictness or add `any`/`!`** to make code
  compile. Fix the code or narrow the type.
- **Don't ship a `src/*.ts` without its `src/*.test.ts`**, and don't
  `it.skip` a whole suite for "this feature isn't done yet" — skip the
  individual case with a verified blocker reason.
- **Don't reimplement parse / type-check / codegen in TS.** Those live
  in the C++ compiler reached through `bindings/c/compiler`; the TS compiler
  binding is a thin wrapper, and the TS eval binding only re-implements
  the **runtime ABI** (marshal/decode + host trampolines), not the
  language.
- **Don't edit vendored / generated output** (`node_modules/`,
  `dist/`, `*.tsbuildinfo`) — all are git-ignored and regenerated.
