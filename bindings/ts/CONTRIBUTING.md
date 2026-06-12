# Contributing to the cel-wasm TypeScript bindings

These guidelines mirror the repo's C++ discipline, adapted to TypeScript.
They are derived from §A.6 of
`doc/implementation-plan/rewrite/m29-typescript-bindings.md` and the
global TS rules in `~/.claude/CLAUDE.md`. The formatter and linter are
authoritative — `npm run lint` is the gate.

## Layout

This is an npm-workspaces monorepo rooted at `bindings/ts/`:

| Package                 | Role                                                       |
| ----------------------- | ---------------------------------------------------------- |
| `@cel-wasm/eval`        | Pure-TS evaluator + the shared wire-format type contracts. |
| `@cel-wasm/compiler`    | CEL source → portable `Program` (over the C ABI).          |
| `@cel-wasm/conformance` | The corpus harness + monotonic ratchet.                    |
| `@cel-wasm/web`         | The Monaco compile → download → run browser demo.          |

`@cel-wasm/eval/src/types.ts` is the **single source of truth** for the
wire-format types (`CelValue`, `CelInput`, `CelAbi`, the kind /
error-code / offset constants, `MessageBacking`, `HostFunction`,
`Program`). Every other package imports them from there; do not
re-declare them.

## Style

- Follow the
  [Google TypeScript Style Guide](https://google.github.io/styleguide/tsguide.html).
- One logical unit per module. Non-exported surface lives in `internal/`.
- **Named exports only — no default exports** (lint-enforced).
- No milestone references in code comments (same rule as C++). A comment
  describes what the code does and why, in terms that stay true after the
  work item ships. The one carved-out exception is a loud stub message
  (`throw new Error('… is a stub until WI-N.M')`) that is deleted when the
  body lands.

## Strictness

- `tsconfig` runs `strict: true`, plus `noUncheckedIndexedAccess`,
  `exactOptionalPropertyTypes`, and `noImplicitOverride`.
- **No `any`** — it is a lint error. Use `unknown` + narrowing.
- **Explicit return types on exported functions** (lint-enforced).
- `target` ES2022, `module` nodenext, `declaration: true`.

## Lint & format (mandatory before every commit)

```sh
npm run lint     # eslint . --max-warnings 0 && prettier --check .
npm run format   # prettier --write .  (apply formatting)
```

- ESLint runs `@typescript-eslint` **strict-type-checked** +
  `eslint-plugin-import`. **Zero warnings** is the gate
  (`--max-warnings 0`).
- Prettier is the single formatter — never hand-format, never fight it
  with ESLint stylistic rules.

## Tests

- **vitest**, colocated `*.test.ts` next to the module they cover.
- **Strict author order: interface → tests → implementation.** Write the
  `.ts` interface first; then write the tests _fully_ (the complete
  positive + negative + boundary matrix) against that interface; then
  write the implementation. Do not write the implementation before the
  tests — the tests are the spec the implementation satisfies.
- **Every `src/foo.ts` has a `src/foo.test.ts`.** A new module without a
  paired test is incomplete, even if an e2e test happens to cover it.
- A coverage gate runs via `@vitest/coverage-v8`
  (`npm test -- --coverage`).

## Errors — throw, don't return a sentinel

Idiomatic TS throws. There is **no `StatusOr`**. Fallible operations
throw a typed `Error` subclass with a `.code`
(`CelCompileError`, `CelEvalError`); never return an error sentinel out
of band.

**The one exception is a CEL _value_ error.** Per the wire-format law
(below), a CEL spec error (divide-by-zero, no-such-key, …) is a
`CEL_ERROR` _value_ that propagates through evaluation — it is returned
as a `CelError` discriminated-union member, **not thrown**. Only a host
trap (a malformed Program, an instantiate failure) throws.

## The wire format is law

The byte layout the evaluator reads (CelValue offsets, kinds, strides,
the `cel.abi` schema) is **frozen by the C++/runtime side**. Any
byte-layout constant is a named `const` in `types.ts` with a comment
citing `runtime/cel_data.h` (or `abi/cel_abi.proto`), and is pinned by a
round-trip test against a real `Program` produced by the C++ compiler. If
a kind is renumbered or a payload offset moves on the C side, the pin
fails loudly rather than the codec silently misreading bytes.
