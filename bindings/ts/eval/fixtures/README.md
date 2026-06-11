# Golden compiled-Program fixtures

Each `<name>.wasm` here is a **self-contained static `Program`** — a CEL
expression compiled to wasm with the runtime kernel bundled in — produced
by the native `cel` CLI. They let the pure-TS eval binding develop and
test against real compiler output **without** the compiler binding (or
bazel) existing in the TS toolchain: the eval tests load these bytes,
instantiate them, marshal the fixture's `activation`, and assert the
decoded result equals the fixture's `expected`.

- **`manifest.json` is the source of truth.** It lists every fixture with
  its `expr`, the variable declarations to compile with (`compileVars`),
  the JS-natural `activation` to bind at eval time (tagged), the
  `expected` decoded `CelValue` (tagged), and a `cliCheck` string the
  generator cross-checks against `cel eval`.
- **Regenerate with `bindings/ts/scripts/build-fixtures.sh`** (only when
  the fixture set or the C++ wire format intentionally changes). It
  compiles each `expr` into `<name>.wasm` and fails loudly if the
  compiler's actual result disagrees with the manifest.

Tagged value forms used by `activation` / `expected`:

```
{ "kind": "null" }
{ "kind": "bool",   "value": true }
{ "kind": "int",    "value": "42" }        // decimal string -> bigint
{ "kind": "uint",   "value": "42" }        // decimal string -> bigint
{ "kind": "double", "value": 4.0 }
{ "kind": "string", "value": "hello" }
{ "kind": "bytes",  "bytes": [1, 2, 3] }
{ "kind": "list",   "elements": [ <tagged>, ... ] }
{ "kind": "map",    "entries": [ [ <tagged-key>, <tagged-val> ], ... ] }
{ "kind": "error",  "code": 11 }           // CelErrorCode (cel_data.h)
```

The fixtures span the static-subset matrix: integer / uint / double /
string / bool / bytes / null scalars, the `map`/`filter`/`exists`/`all`
comprehensions, list & map indexing and membership, `size`, string
methods, unicode `size`, a divide-by-zero error value, and several
variable-bound expressions (the marshal path) including a realistic
age/country policy in both its true and false cases.

## `dynamic/` — DYNAMIC-link twins

`dynamic/<name>.wasm` is the **DYNAMIC-link** compilation of the static
fixture with the same `<name>` (and the same `manifest.json` `expr` +
`compileVars`): a thin (~6 KB) expression module that imports the runtime
helpers from the `cel` namespace (incl. the shared `cel.memory`) instead
of bundling them. It is linked at plan time against the standalone
`../runtime/cel_runtime.wasm`.

The subset (`int_add`, `var_int_add`, `list_map_double`, `map_index`,
`string_concat` — one per value shape) is what `eval/src/dynamic.test.ts`
uses to prove (a) import-introspection routing and (b) that a dynamic
Program evaluates to the **same** `CelValue` as its static twin. Regenerate
with `bindings/ts/scripts/gen-dynamic-fixtures.mjs` (needs
`bazel build //bindings/c:compiler_wasm` first).
