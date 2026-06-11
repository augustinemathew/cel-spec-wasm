# M29 — TypeScript bindings + the browser killer-demo

Status: **shipped 2026-06-11.** This doc is both the design and the
**parallel-build work-breakdown**: Part A is the design; Part B
decomposes it into phases → independently-farmable work items, each with
the file ownership, dependencies, spec pointers, and definition-of-done
an agent needs to execute it without reading any other work item.

> **What landed (2026-06-11).** The full bindings monorepo under
> `bindings/ts/` (npm workspaces: `@cel-wasm/{eval,compiler,conformance,web}`)
> plus the `bindings/c` C ABI, built by parallel agents in four waves and
> integrated commit-by-commit. The **pure-TS evaluator works end-to-end**:
> all 27 golden fixtures evaluate through `Engine.create → plan → eval`
> (`marshal` mirrors C++ `Instance::Eval` — `arena_init` seed, string/bytes
> above `__heap_base`, externref reset). The **compiler binding** compiles
> in-scope CEL to byte-identical Programs via a subprocess backend over the
> native `cel` CLI (N-API/emscripten deferred behind the same
> `CompileBackend` interface — emscripten is a written conditional-GO
> spike, `m29-wi24-emscripten-spike.md`). The **Monaco demo** (`bindings/ts/web`)
> does compile → download → run with client-side eval and inline
> diagnostics. Gate: `lint` + `build` + `typecheck` + `test` green —
> **781 tests pass / 13 reasoned skips** across 41 files. Conformance:
> **1451 pass / 336 fail** under a monotonic ratchet (the 336 are tracked,
> not categorized — see Future work). As-shipped deltas from the plan:
> WI-2.2 N-API replaced by the subprocess backend (works tonight; N-API a
> follow-up); `src/kinds.ts` folded into the shared `types.ts`; the demo
> physically lives at `bindings/ts/web` (npm-workspace), named
> `@cel-wasm/web`. Per-WI "Shipped"/"Status" notes are inline below.

---

## Part A — Design

### A.1 Goal & the killer demo

Ship TypeScript bindings for the cel-wasm **compiler** and **evaluator**,
and a browser demo that proves the architecture's whole thesis:

> Type a CEL expression in a Monaco editor → **compile** it to a portable
> `.wasm` Program → **download** that artifact → **run** it right there in
> the browser and see the result. Compile errors surface inline in Monaco.

The demo is the point: it shows "compile once, run anywhere" literally —
the same `.wasm` a server would run, executing in a browser tab with no
server round-trip at eval time.

### A.2 The architecture decision

A compiled `Program` is **just wasm + a `cel.abi` descriptor**, and every
JS host already has a WebAssembly engine. So:

- **Eval is pure TypeScript.** The TS evaluator instantiates the Program
  (a `WebAssembly.Module`), provides the `cel_host.*` host imports as JS
  functions, marshals the activation into the module's linear memory,
  calls `$eval`, and decodes the result CelValue — **no wasmtime, no
  C++**. This runs identically in Node and the browser. This is the
  novel, high-value core.

- **Compile needs the C++ compiler** (it links cel-cpp + Binaryen). The
  clean seam is a **C ABI** (`bindings/c`) — an `extern "C"` surface over
  the C++ `Compiler`. The TS compiler binding wraps that C ABI through
  **two interchangeable backends behind one TS interface**:
  1. **N-API native addon** (Node, the default) — fast, reliable, ships now.
  2. **emscripten `compiler.wasm`** (browser, the stretch) — the C ABI +
     compiler + cel-cpp + Binaryen cross-compiled to wasm, so the demo
     compiles fully client-side. High-risk; the demo works without it via
     a thin local compile endpoint, and gains "no server" once it lands.

```
            bindings/c  (extern "C" over C++ Compiler)
                 │
        ┌────────┴─────────┐
        ▼                  ▼
   N-API addon       emscripten compiler.wasm        (two backends,
   (Node, now)       (browser, stretch)               one TS interface)
        │                  │
        └────────┬─────────┘
                 ▼
        bindings/ts/compiler   ──►  Program (.wasm + cel.abi bytes)
                                          │
                 ┌────────────────────────┘
                 ▼
        bindings/ts/eval   (pure TS: instantiate, marshal, host fns,
                            decode — runs in Node AND the browser)
                 │
                 ▼
        bindings/web   (Vite + Monaco: compile → download → run)
```

### A.3 Scope

**In:**
- Eval of the full static-subset CEL surface: scalars, strings, bytes,
  lists, maps, the arithmetic/comparison/logic/string/conversion
  operators, comprehensions (`map`/`filter`/`exists`/`all`), the
  timestamp/duration ops.
- **Protobuf** — proto-message *field reads*, *presence* (`has`),
  *message construction* (proto literals in expressions), well-known-type
  unwrap, and message equality. Descriptor-backed via **protobufjs**
  (§A.4.6). This makes the `cel_host.*` proto/WKT trampolines first-class,
  not stubs.
- **JSON objects *and* protos in the activation.** A variable declared as
  a message type can be bound as a plain JS object (coerced to the message
  via its descriptor, protobuf-JSON-mapping rules) **or** as a protobufjs
  message instance; both read fields with proto semantics. A variable
  declared `map<...>` binds a JS object/`Map` as a host map. (§A.4.6, §A.5.)
- **Host functions** (`@host`) — custom functions implemented as JS
  callbacks. The only custom-fn mechanism in scope.
- Compile via the C ABI (N-API now; emscripten stretch).
- Conformance run over the upstream corpus (now incl. the proto rows);
  a TS conformance ratchet.
- Ported e2e behaviors as a vitest suite.
- The browser demo.

**Out (explicitly):**
- **Unknowns / partial evaluation** — no `PartialEval`, no
  unknown-attribute patterns, no `CEL_UNKNOWN` handling beyond
  pass-through. Drops the AttributeEntry table and a whole API surface.
- **`@component` and `@native` custom functions** — host functions only.
- **`Any` / dynamic-message resolution beyond the supplied descriptor
  set** — the eval binding resolves messages only against descriptors the
  caller provides (§A.4.6); no runtime type-registry discovery.
- **Signed/notarized Program artifacts.**

### A.4 The wire formats a TS evaluator re-implements

These are the load-bearing specs. They are frozen by the C++/runtime
side; the TS binding mirrors them byte-for-byte. Citations are to the
authoritative headers.

#### A.4.1 CelValue — 24 bytes, little-endian (`runtime/cel_data.h:31-271`)

```
 offset  size  field
   0      4    kind        (u32, CelKind)
   4      4    _pad
   8     16    payload     (union, by kind below)
 ─────────────────────────  total = 24 bytes
```

`CelKind` (stable, append-only):

| # | kind | payload @ offset 8 |
|---|---|---|
| 0 | NULL | — |
| 1 | BOOL | i32 `b` |
| 2 | INT | i64 `i` (→ JS `bigint`) |
| 3 | UINT | u64 `u` (→ JS `bigint`) |
| 4 | DOUBLE | f64 `d` |
| 5 | STRING | span `{ptr:u32@8, len:u32@12}` (utf-8 in linear mem) |
| 6 | BYTES | span (raw bytes) |
| 7 | LIST_ARENA | `header_ptr:u32@8` → arena list header |
| 8 | MAP_ARENA | `header_ptr:u32@8` → arena map header |
| 9 | MAP_HOST | `ref_slot:u32@8` (externref table) |
| 10 | MESSAGE | `msg_slot:u32@8` (externref table) |
| 11 | TYPE | span (type name string) |
| 12 | DURATION | `{seconds:i64@8, nanos:i32@16}` |
| 13 | TIMESTAMP | `{seconds:i64@8, nanos:i32@16}` |
| 14 | OPTIONAL | `payload_off:u32@8` (out of scope) |
| 15 | UNKNOWN | `desc_off:u32@8` (out of scope — pass through as error-ish) |
| 16 | ERROR | `code:u32@8` (CelErrorCode) |
| 17 | LIST_HOST | `ref_slot:u32@8` (externref table) |
| 18 | IP | `net_ref:u32@8` (arena) |
| 19 | CIDR | `net_ref:u32@8` (arena) |

Arena **list header** (16 B): `{count:u32, capacity:u32, elements_off:u32,
_pad:u32}`; elements are `count` × 24-byte CelValues at `elements_off`.

Arena **map header** (16 B): `{count:u32, capacity:u32, entries_off:u32,
_pad:u32}`; entries are `count` × 48 bytes (24 B key CelValue + 24 B value
CelValue) at `entries_off`.

Error codes (`cel_data.h:218-271`): OVERFLOW=10, DIVIDE_BY_ZERO=11,
MODULUS_BY_ZERO=12, TYPE_MISMATCH=13, NO_SUCH_KEY=15, INDEX_OOB=17,
INVALID_ARGUMENT=18, FIELD_NOT_FOUND=20 (TS surfaces these as a
`CelError` with `.code` + a synthesized message — message text is not on
the wire, `cleanup-backlog #31`).

#### A.4.2 Memory map (`runtime/cel_layout.h:15-56`)

Page = 64 KiB; initial 2 pages (128 KiB). The low **8192 bytes**
(`CELWASM_RESERVED_LOW_MEMORY_BYTES`) hold the expr module's rodata +
workspace slots (where the marshal writes variables). The arena default
is 64 KiB. The TS marshal writes variable CelValues at their
`slot_offset` (from the ABI), and string/bytes payloads via the runtime's
`arena_alloc` export (NOT into the reserved window).

#### A.4.3 The `cel.abi` descriptor (`abi/cel_abi.proto`, `abi_decode.cc`)

Embedded as a wasm **custom section named `cel.abi`** (locate via the
wasm magic `00 61 73 6d`, version 1, then a LEB128 section walk;
`abi_decode.cc:46-163`). Payload is a `CelAbi` protobuf:

```proto
CelAbi {
  uint32 version;
  repeated VariableEntry variables;   // {name, local_index, slot_offset, repr}
  repeated FieldEntry    fields;      // {id, field_number, name, owner_fqn}
  repeated AttributeEntry attributes; // OUT OF SCOPE (unknowns)
  repeated TypeEntry     types;       // {id, fully_qualified_name}
  uint32 runtime_abi_version;
  LinkMode link_mode;                 // DYNAMIC=0, STATIC=1
}
```

The TS binding parses this with `protobufjs` from the proto schema (or a
hand-rolled decoder — `VariableEntry` is all it strictly needs for
host-fn-only programs). `variables[i].{name, slot_offset, repr}` is the
marshal table: bind `name`, write its CelValue at `slot_offset`.

#### A.4.4 Runtime imports the TS host must provide

A **static** Program (the compiler default, and what the binding targets)
bundles the runtime (`cel.*`) but still **imports**:

- `cel_host.*` — the host trampolines (§A.4.5), provided by TS.
- `cel_fn.<overload_id>` — registered host functions, provided by TS.
- `cel_env.cel_log` — a debug log callback (TS: a no-op or console).
- WASI preview1 — provided as empty/trap stubs (the engine wires an empty
  WASI context; TS mirrors that with minimal stubs).

Instantiation: `WebAssembly.instantiate(programBytes, importObject)` where
`importObject` supplies the four groups above; the Program exports `eval`
(nullary `() -> i32`) and its `memory`.

#### A.4.5 The `cel_host.*` trampolines (the bulk of eval work)

Re-implemented in TS as the `cel_host` import group. Each takes i32 slot
offsets, reads operands **before** writing the out slot, **absorbs
UNKNOWN/ERROR** on inputs (copy to out, skip), and writes a CelValue
(spec errors are CEL_ERROR *values*, never thrown). Grounded in
`eval/internal/cel_host.h:520-790`:

| trampoline | signature (i32 slots) | what TS does |
|---|---|---|
| `cel_get_field` | (out, msg, field_ref, attr) | read field from the externref message backing via its descriptor (`fields[field_ref]` → number/name); proto presence + WKT peel; arena-copy string/bytes |
| `cel_has_field` | (out, msg, field_ref, attr) | proto2/proto3 presence → BOOL |
| `cel_map_lookup` | (out, map, key) | host map: `externref[map].get(key)` → value or NO_SUCH_KEY |
| `cel_map_in` | (out, key, map) | key ∈ map.keys → BOOL |
| `cel_map_size` | (out, map) | size → INT |
| `cel_list_at` | (out, list, idx) | host list: `externref[list][idx]` or INDEX_OOB |
| `cel_list_in` | (out, val, list) | val ∈ list → BOOL (scalar equality) |
| `cel_list_size` | (out, list) | size → INT |
| `cel_list_eq` / `cel_map_eq` | (out, a, b) | structural equality |
| `cel_list_iter_open` | (out, list) | snapshot host list → arena LIST_ARENA for comprehension |
| `cel_map_iter_open` | (state, map) | snapshot host map → arena entries for comprehension |
| `cel_make_message` / `cel_set_field` | … | construct a protobufjs message from `types[type_id]`, set fields, intern in the message externref table |
| `cel_wkt_unwrap_time` / `cel_wkt_unwrap_wrapper` | … | peel `Timestamp`/`Duration`/wrapper messages to scalars |
| `cel_message_eq` / `cel_message_is_zero` / `cel_resolve_message_type_name` | … | message equality / emptiness / FQN-as-type |
| `cel_timestamp_tz_accessor` | (out, ts, tz, kind) | civil-time projection (Luxon/`Temporal`/`Intl` for the IANA zone) |

All trampolines are in scope. The **map/list/size/in/eq/iter_open** set
covers scalar/aggregate programs; the **proto/WKT/message** set is
descriptor-backed via protobufjs (§A.4.6). The only remaining stubs are
`cel_env.cel_log` (a sink) and the WASI preview1 imports (empty stubs).

**Externref tables** (`eval/internal/cel_host.h`): three independent JS
arrays (message / map / list), slot 0 = null sentinel, reset between
Evals. A host-backed list/map variable is a JS array/object stored in the
table; its CelValue carries the slot index. The TS marshal interns bound
JS aggregates here.

#### A.4.6 Descriptors, protos, and JSON-object activation

Proto support needs **descriptors** in TS. The eval binding takes them
from the caller (a serialized `FileDescriptorSet`, or a loaded protobufjs
`Root`) at `Engine.create`/`plan` time, and resolves message types
against that set only (no global registry discovery — out of scope). The
compiler binding may additionally bundle the descriptor set it compiled
against, so a `Program` can be self-describing; the eval binding prefers a
caller-supplied set and falls back to the bundled one.

A **message backing** (the thing an externref message slot points at) is a
protobufjs `Message` + its `Type` (descriptor). It supports `readField`
(by number or name, from `cel.abi.fields[]`), `hasField` (proto2/proto3
presence), and — for proto literals — mutable construction. The
`cel_host` proto/WKT trampolines (§A.4.5) operate over this backing.

**Activation values** are JS-natural, and the *declared type* of the
variable decides how a bound value is interpreted:

| Bound JS value | Declared as | Interpreted as |
|---|---|---|
| number / `bigint` / boolean / string / `Uint8Array` | scalar | inline CelValue |
| `Array` | `list<T>` | host list (externref) |
| `Map` / plain object | `map<K,V>` | host map (externref) |
| **plain JS object** | a **message type** | **coerced to that message** via protobufjs `fromObject` (protobuf-JSON mapping), interned as a message backing |
| **protobufjs message** | a message type | used directly as the message backing |

So `instance.eval({ req: { user: { id: 7 }, country: "US" } })` works when
`req` is declared as a message type and its descriptor is loaded — the
plain object is coerced to the message, and `req.user.id` reads through
`cel_get_field`. Decode mirrors this: a returned `CEL_MESSAGE` decodes to
a plain JS object (protobufjs `toObject`) unless the caller asks for the
message instance.

### A.5 The canonical TypeScript API

Idiomatic TS — **throw** on error (no `StatusOr`), `Uint8Array` for
bytes, `bigint` for int64/uint64, `Promise` for the async wasm
instantiate, structural value types. Not a transliteration of the C++.

```ts
// ── @cel-wasm/compiler ──────────────────────────────────────────────
export interface CompileOptions { container?: string; optimizeLevel?: 0|1|2|3; }
export interface VariableDecl { name: string; type: CelType; }

export function compile(
  source: string,
  vars?: VariableDecl[],
  opts?: CompileOptions,
): Promise<Program>;          // throws CelCompileError (with .diagnostics) on failure

export interface Program {
  readonly wasm: Uint8Array;  // the portable artifact (downloadable)
  readonly abi: CelAbi;       // decoded descriptor
}

// ── @cel-wasm/eval ──────────────────────────────────────────────────
export interface EngineOptions {
  // proto descriptors: a serialized FileDescriptorSet or a protobufjs Root.
  // Required only to evaluate message-typed variables / proto literals.
  descriptors?: Uint8Array | protobuf.Root;
}
export class Engine {
  static create(opts?: EngineOptions): Promise<Engine>;
  plan(program: Program): Promise<Instance>;
  // host functions: declared in .celfn IDL, implemented as a JS callback
  defineFunction(decl: string, impl: (...args: CelValue[]) => CelValue): void;
}

export class Instance {
  // activation values are JS-natural; a plain object bound to a
  // message-typed variable is coerced to that proto (§A.4.6).
  eval(activation?: Record<string, CelInput>): CelValue;   // throws on trap
}

// What you bind IN (objects/protos accepted for message-typed vars):
export type CelInput =
  | null | boolean | bigint | number | string | Uint8Array
  | CelInput[] | Map<CelInput, CelInput> | { [field: string]: CelInput }
  | protobuf.Message;

// What you get OUT — a discriminated union over JS-natural shapes:
export type CelValue =
  | null | boolean | bigint | number | string | Uint8Array
  | CelValue[] | Map<CelValue, CelValue>
  | { [field: string]: CelValue }                          // a message
  | { kind: 'timestamp'; epochSeconds: bigint; nanos: number }
  | { kind: 'duration';  seconds: bigint; nanos: number }
  | { kind: 'error'; code: number; message: string };
```

```ts
// usage — the demo's core loop:
const program  = await compile('age >= 18 && country in ["US","CA"]',
                               [{name:'age', type:'int'}, {name:'country', type:'string'}]);
const engine   = await Engine.create();
const instance = await engine.plan(program);
const allowed  = instance.eval({ age: 25n, country: 'US' });   // => true
// program.wasm is the downloadable artifact.
```

### A.6 TypeScript coding & linting guidelines

Mirrors the repo's C++ discipline, adapted to TS (and the global
`~/.claude/CLAUDE.md` TS rules):

- **Style:** [Google TypeScript Style Guide](https://google.github.io/styleguide/tsguide.html).
- **Strictness:** `tsconfig` `strict: true`, `noUncheckedIndexedAccess`,
  `exactOptionalPropertyTypes`, `noImplicitOverride`. No `any` (lint-error);
  use `unknown` + narrowing. Explicit return types on exported functions.
- **Lint/format:** ESLint with `@typescript-eslint` **strict-type-checked**
  + `eslint-plugin-import`; Prettier as the single formatter. CI gate +
  pre-commit: `eslint . --max-warnings 0 && prettier --check .`.
- **Tests:** **vitest**, colocated `*.test.ts`. Strict author order matches
  the repo: interface → tests (full positive/negative/boundary matrix) →
  implementation. Every `src/foo.ts` has a `foo.test.ts`. Coverage gate.
- **Naming/structure:** one logical unit per module; `internal/` for
  non-exported surface; no default exports; named exports only.
- **No milestone refs in code comments** (same rule as C++).
- **Errors:** throw typed `Error` subclasses (`CelCompileError`,
  `CelEvalError`) with a `.code`; never return error sentinels.
- **The wire format is law:** any byte-layout constant (offsets, kinds,
  strides) is a named `const` with a comment citing
  `runtime/cel_data.h` — and pinned by a round-trip test against a real
  Program produced by the C++ compiler.

### A.7 Conformance & e2e

- **Conformance:** load `spec/tests/simple/testdata/*.textproto` (31 files,
  2454 rows; parse textproto → `SimpleTest`), compile each `expr` (compiler
  binding) + eval (TS eval) + compare the decoded `CelValue` to the row's
  expected value/error. A **TS ratchet** (`bindings/ts/conformance/.baseline`)
  mirrors the C++ monotonic gate. Skip categories carried over (out-of-scope
  rows: unknowns, proto-construction, components).
- **e2e:** port the behavior-pinning e2e suites (`e2e/*_test.cc`) to vitest
  — host fns, lists/maps, comprehensions, the operator matrices, string ext.
  Each ported file cites the C++ original.

---

## Part B — Implementation plan (phases → farmable work items)

Each work item (WI) lists: **Owns** (files/dirs it exclusively writes —
chosen so concurrent WIs never touch the same file), **Depends** (WIs that
must finish first), **Deliverable**, **Done-when**, and **Spec** (the Part A
section + code citations it needs). A WI with no unmet dependency can be
farmed to an independent agent immediately.

### Phase 0 — Foundation (serial; everything depends on it)

**WI-0.1 — Monorepo scaffold + toolchain.**
- *Owns:* `bindings/` top-level: `bindings/ts/package.json` (npm workspaces
  for `compiler`, `eval`, `conformance`, `web`), root `tsconfig.base.json`,
  `.eslintrc.cjs` (strict-type-checked), `.prettierrc`, `vitest.config.ts`,
  `bindings/ts/CONTRIBUTING.md` (the §A.6 guidelines), `.gitignore` updates,
  a `bindings/README.md`. Stub package dirs with empty `src/index.ts` +
  `tsconfig.json` per package.
- *Depends:* none.
- *Done-when:* `npm install && npm run lint && npm run build && npm test`
  all pass on the empty scaffold (one trivial passing test per package).
- *Spec:* §A.2 (dirs), §A.5 (package names), §A.6 (toolchain).

**WI-0.2 — Build the runtime + a fixtures pipeline.**
- *Owns:* `bindings/ts/eval/fixtures/` + a script
  `bindings/ts/scripts/build-fixtures.sh` that runs the **C++** `cel`
  CLI / bazel to emit (a) `cel_runtime.wasm` bytes the eval binding loads,
  and (b) a handful of golden compiled Programs (`.wasm` + expected JSON
  results) for known expressions, so eval WIs can develop **without** the
  compiler binding existing yet.
- *Depends:* none (uses the existing C++ build).
- *Done-when:* `build-fixtures.sh` produces the runtime wasm + ≥10 golden
  Programs with expected results, committed as fixtures.
- *Spec:* §A.4.4; `tools/cel` CLI; `runtime/BUILD.bazel`.

### Phase 1 — The pure-TS eval binding (highly parallel)

All Phase-1 WIs depend only on **WI-0.1 + WI-0.2** and own disjoint files,
so they run **fully in parallel**.

**WI-1.1 — ABI decoder.**
- *Owns:* `bindings/ts/eval/src/abi.ts` + `abi.test.ts`.
- *Deliverable:* `decodeAbi(wasm: Uint8Array): CelAbi` — find the `cel.abi`
  custom section, parse the `CelAbi` proto (variables/fields/types/
  link_mode/runtime_abi_version). Skip attributes (out of scope).
- *Done-when:* decodes the golden Programs (WI-0.2) to the expected
  variable tables; rejects a non-cel wasm; round-trips link mode.
- *Spec:* §A.4.3; `abi/cel_abi.proto`; `abi_decode.cc:46-163`.

**WI-1.2 — CelValue codec.**
- *Owns:* `bindings/ts/eval/src/celvalue.ts` + test, `src/kinds.ts`
  (the kind/error-code constants).
- *Deliverable:* read/write a CelValue at a `(DataView, offset)`:
  `readCelValue`, `writeScalar*`, span read (string/bytes via memory +
  utf-8), arena list/map header read, error decode → `CelError`. Plus the
  `CelValue` discriminated-union type (§A.5).
- *Done-when:* round-trips every in-scope kind against bytes from the
  golden Programs; bigint for i64/u64; little-endian verified.
- *Spec:* §A.4.1; `runtime/cel_data.h:31-271`.

**WI-1.3 — Externref table + JS backings.**
- *Owns:* `bindings/ts/eval/src/externref.ts` + test.
- *Deliverable:* three independent handle namespaces (message/map/list),
  slot 0 = null, `intern(jsValue) -> slot`, `lookup(slot)`, `reset()`.
- *Done-when:* intern/lookup/reset round-trips; reset clears all three.
- *Spec:* §A.4.5 (externref tables).

**WI-1.4a — Host trampolines: list + map.**
- *Owns:* `bindings/ts/eval/src/host/aggregates.ts` + test.
- *Deliverable:* TS impls of `cel_map_lookup`, `cel_map_in`, `cel_map_size`,
  `cel_list_at`, `cel_list_in`, `cel_list_size`, `cel_list_eq`,
  `cel_map_eq`, `cel_list_iter_open`, `cel_map_iter_open` — reading slots
  (WI-1.2) and externref backings (WI-1.3), 3VL-absorbing, error-as-value.
- *Depends:* WI-1.2, WI-1.3.
- *Done-when:* unit-tested against hand-built memory + backings for each;
  NO_SUCH_KEY / INDEX_OOB produce error values; iter_open snapshots to arena.
- *Spec:* §A.4.5; `cel_host.h:542-684`.

**WI-1.4b — Proto backing + descriptor loading (protobufjs).**
- *Owns:* `bindings/ts/eval/src/proto/backing.ts`, `proto/descriptors.ts`
  + tests.
- *Deliverable:* load descriptors (a `FileDescriptorSet` `Uint8Array` or a
  protobufjs `Root`) → a resolver `messageType(fqn) -> protobuf.Type`; a
  `MessageBacking` over a protobufjs `Message` + its `Type`: `readField(by
  number|name) -> CelValue`, `hasField`, mutable `setField`, and the
  `fromObject`/`toObject` coercion (§A.4.6). Pure protobufjs — no wasm.
- *Depends:* WI-1.2 (for the `CelValue` type only — use the interface,
  not the impl, so this runs concurrently).
- *Done-when:* loads a real descriptor set; reads/sets a field; coerces a
  plain object ↔ message; WKT peel helpers (`Timestamp`/`Duration`/
  wrappers) covered.
- *Spec:* §A.4.6; `cel_host.h` Layer-1 backing semantics.

**WI-1.4c — Host trampolines: proto / WKT / message.**
- *Owns:* `bindings/ts/eval/src/host/proto.ts` + test.
- *Deliverable:* TS impls of `cel_get_field`, `cel_has_field`,
  `cel_make_message`, `cel_set_field`, `cel_wkt_unwrap_time`,
  `cel_wkt_unwrap_wrapper`, `cel_message_eq`, `cel_message_is_zero`,
  `cel_resolve_message_type_name` over the WI-1.4b backing + WI-1.3
  externref message table + WI-1.2 slot codec; 3VL-absorbing,
  error-as-value, arena-copy string payloads.
- *Depends:* WI-1.2, WI-1.3, WI-1.4b.
- *Done-when:* field read/has/construct/set round-trip against a real
  descriptor + message; WKT peel produces scalars; unset proto3 → Null.
- *Spec:* §A.4.5, §A.4.6; `cel_host.h:520-790`.

**WI-1.4d — Host trampolines: cel_env + WASI stubs.**
- *Owns:* `bindings/ts/eval/src/host/stubs.ts` + test.
- *Deliverable:* `cel_env.cel_log` as a sink (optional console), the WASI
  preview1 minimal stubs that let a static Program instantiate. (The
  timezone accessor `cel_timestamp_tz_accessor` lives here, backed by
  `Intl`/`Temporal` — or returns INVALID_ARGUMENT until wired.)
- *Depends:* WI-1.2.
- *Done-when:* WASI stubs let a module instantiate; cel_log is a no-op sink.
- *Spec:* §A.4.4; `cel_host.h:520-790`; engine WASI stub set.

**WI-1.5 — Engine / Instance / marshal (the assembly).**
- *Owns:* `bindings/ts/eval/src/engine.ts`, `instance.ts`,
  `activation.ts`, `marshal.ts`, `src/index.ts` (public API) + tests.
- *Deliverable:* `Engine.create(opts)` (loads descriptors via WI-1.4b),
  `engine.plan(program)` (instantiate the static Program with the host
  import object assembled from WI-1.4a/c/d + the runtime),
  `instance.eval(activation)` — the **marshal** writes each variable's
  CelValue at its `slot_offset` (WI-1.2), interning host aggregates /
  coercing JS objects+protos to message backings (WI-1.3 + WI-1.4b per
  the variable's declared `repr`/type, §A.4.6) — then calls `$eval`,
  decodes the result, resets externrefs. `defineFunction` registers a JS
  host fn as a `cel_fn.*` import. The canonical API of §A.5.
- *Depends:* WI-1.1, 1.2, 1.3, 1.4a, 1.4b, 1.4c, 1.4d.
- *Done-when:* evaluates the golden Programs (WI-0.2); a registered host
  fn round-trips; a JS-object-bound message-typed var reads its fields; a
  proto literal constructs.
- *Spec:* §A.4.4, §A.4.6, §A.5.
- *Shipped:* `engine.ts` / `instance.ts` / `marshal.ts` / `activation.ts`
  / internal `resolving-codec.ts` + `src/index.ts`, with colocated tests.
  All 27 committed `eval/fixtures/` Programs evaluate correctly through
  `Engine.create → plan → eval` (`instance.test.ts`, fixture-driven off
  `manifest.json`).  The eval sequence mirrors C++ `Instance::Eval`
  (`eval/instance.cc`): reset externrefs → marshal each variable into its
  `slot_offset` (string/bytes payloads into a `malloc`'d activation buffer
  ABOVE the bump arena, since `$eval`'s prelude `arena_reset`s) → seed the
  arena once per Instance with `arena_init(CELWASM_ARENA_CAPACITY_BYTES)`
  → call `eval` → decode the result slot resolving externref kinds.  The
  host-fn `cel_fn.*` trampoline and the message-var marshal/coercion path
  are unit-pinned (`buildCelFnImports` round-trip; `marshal.test.ts`
  message coercion); the e2e paths driving them from inside a compiled
  Program are a follow-up gated on a compiled `@host` / message-typed
  fixture (no such fixture is reachable in the TS test toolchain yet).

### Phase 2 — The C ABI + compiler binding

**WI-2.1 — C ABI over the C++ compiler.**
- *Owns:* `bindings/c/` — `cel_capi.h`, `cel_capi.cc`, `BUILD.bazel`,
  `cel_capi_test.cc`.
- *Deliverable:* `extern "C"`: `cel_compile(const char* source, const
  CelCompileOpts*, uint8_t** out_wasm, size_t* out_len, char** out_err)`,
  `cel_free`, variable-decl + host-fn-decl builders. Thin shim over
  `compiler/compiler.h`; maps `StatusOr` → return code + `out_err` string.
- *Depends:* WI-0.1 (for the repo conventions only; otherwise independent).
- *Done-when:* `cel_capi_test` compiles `1+2` and a variable expr to wasm
  bytes that the **WI-1.5 eval binding** then evaluates correctly (the
  cross-check); error path returns a diagnostic string.
- *Spec:* §A.2; `compiler/compiler.h:148-210`; §A.3 (simplifications).

**WI-2.2 — N-API addon.**
- *Owns:* `bindings/ts/compiler/native/` (`binding.gyp` or `CMakeLists`,
  `addon.cc`), build wiring.
- *Deliverable:* a Node native module exporting `compileNative(source,
  vars, opts) -> { wasm: Buffer, error?: string }` over WI-2.1's C ABI.
- *Depends:* WI-2.1.
- *Done-when:* `require()`-able from Node; compiles `1+2`; prebuild script
  documented.
- *Spec:* §A.2.

**WI-2.3 — TS compiler API.**
- *Owns:* `bindings/ts/compiler/src/index.ts`, `errors.ts` + tests.
- *Deliverable:* `compile(source, vars?, opts?): Promise<Program>` (§A.5)
  over WI-2.2, decoding the ABI via WI-1.1; `CelCompileError` with parsed
  diagnostics (line/col/message) from the compiler's error string.
- *Depends:* WI-2.2, WI-1.1.
- *Done-when:* compiles to a `Program` the eval binding runs; a bad
  expression throws `CelCompileError` with a usable diagnostic.
- *Spec:* §A.5.

**WI-2.4 — (stretch) emscripten `compiler.wasm`.**
- *Owns:* `bindings/c/emscripten/` (build config), a `compiler.wasm`
  artifact + a `compileWasm()` backend in `bindings/ts/compiler`.
- *Deliverable:* the C ABI + compiler + cel-cpp + Binaryen built to wasm;
  the TS compiler API gains a browser backend behind the same interface.
- *Depends:* WI-2.1; **high-risk, time-boxed** — if it doesn't converge,
  the demo uses the local-endpoint fallback (WI-4.x) and this stays open.
- *Done-when:* `compiler.wasm` compiles `1+2` in a headless browser; OR a
  written go/no-go with the blocker, and the fallback documented.
- *Spec:* §A.2; CLAUDE.md layering rule.

### Phase 3 — Conformance & e2e (parallel, after Phase 1 + 2.3)

**WI-3.1 — Conformance harness + ratchet.**
- *Owns:* `bindings/ts/conformance/` (runner, textproto loader, `.baseline`,
  a `run` script) + tests.
- *Deliverable:* load the corpus, compile+eval+compare each row, classify
  pass/skip/fail with carried-over skip categories; a monotonic baseline
  gate mirroring `scripts/check_conformance_monotonic.sh`.
- *Depends:* WI-1.5, WI-2.3.
- *Done-when:* runs the full corpus, reports pass/skip/fail with 0 fail,
  writes a baseline; one fixture file pinned in CI.
- *Spec:* §A.7; `conformance/runner.h`; `spec/tests/simple/testdata/`.

**WI-3.2 — e2e behavior ports.**
- *Owns:* `bindings/ts/eval/e2e/*.test.ts` (disjoint files per area).
- *Deliverable:* vitest ports of the behavior-pinning suites (host fns,
  lists/maps, comprehensions, operator matrices, string ext), each citing
  its C++ original.
- *Depends:* WI-1.5, WI-2.3.
- *Done-when:* the ported suites pass; coverage gate met.
- *Spec:* §A.7; the e2e inventory in this doc's recon.

### Phase 4 — The browser demo

**WI-4.1 — Monaco compile/download/run demo.**
- *Owns:* `bindings/web/` (Vite app, `index.html`, `src/`, the Monaco
  integration, a tiny `dev-server` compile endpoint using WI-2.3 until
  WI-2.4 lands).
- *Deliverable:* a pretty single-page app: Monaco editor with CEL syntax,
  a **Compile** action (→ diagnostics inline in Monaco on error, or a
  `Program`), a **Download .wasm** button, a **Run** panel (variables form
  + the TS eval binding → result), example expressions.
- *Depends:* WI-1.5, WI-2.3 (WI-2.4 optional upgrade to client-side compile).
- *Done-when:* `npm run dev` serves it; compile→download→run works
  end-to-end for the example expressions; compile errors render in Monaco.
- *Spec:* §A.1, §A.5.
- *Status: shipped.* `bindings/ts/web/` is a Vite + Monaco app.
  **Compile** posts to a Vite-plugin `POST /api/compile` endpoint
  (`dev-server/compile-handler.ts`, drives the native `cel` CLI, returns
  base64 wasm or parsed diagnostics); the `cel.abi` is decoded
  client-side so the Node endpoint needs no protobufjs. **Run** is pure
  client-side (`src/internal/run.ts` → `@cel-wasm/eval` Engine/Instance,
  no network hop). **Download** saves `program.wasm`. Diagnostics render
  inline via `setModelMarkers` plus an error panel. Seeded examples:
  the access check (bound `age`/`country`), `[1,2,3].map(x, x*2)`,
  `"hello".size()`, and `1 / 0` (error-value path). Headless verification:
  `npm run build` clean, the endpoint serves wasm + diagnostics (live
  curl), and `src/run.test.ts` proves the compile→eval wiring for every
  example. The Monaco glue (`controller.ts`/`monaco-cel.ts`) is
  browser-only (Monaco's entry can't load under node/jsdom) — verified by
  `vite build` + the manual steps in `web/README.md`.

### Dependency graph (what can start when)

```
  WI-0.1 ─┬─► WI-0.2 ─┬─► WI-1.1 ───────────────────┐
          │           ├─► WI-1.2 ─┬─► WI-1.4a ──────┤
          │           ├─► WI-1.3 ─┤                 │
          │           ├─► WI-1.4b ┴─► WI-1.4c ──────┼─► WI-1.5 ─┬─► WI-3.1
          │           └─► WI-1.4d ──────────────────┘           ├─► WI-3.2
          └─► WI-2.1 ─► WI-2.2 ─► WI-2.3 ───────────────────────┴─► WI-4.1
                    └─► WI-2.4 (stretch, parallel)
```

**Parallelize maximally.** Each WI owns disjoint files and depends only on
prior WIs' *interfaces* (TypeScript types), not their implementations — so
a downstream WI can be written against a stubbed type the moment the
interface is named, not when the impl lands. Concretely:

- **Wave A (after Phase 0): up to 7 agents at once** —
  WI-1.1, WI-1.2, WI-1.3, WI-1.4b, WI-1.4d, WI-2.1, and WI-2.4 (stretch,
  independent). None share a file; none block each other.
- **Wave B: WI-1.4a, WI-1.4c, WI-2.2** (each needs one Wave-A output).
- **Wave C: WI-1.5, WI-2.3** (the two assemblies).
- **Wave D: WI-3.1, WI-3.2, WI-4.1** (conformance, e2e, demo — parallel).

To unblock even Wave A→C overlap, Phase 0 emits the **shared type
contracts first** (a `bindings/ts/eval/src/types.ts` with `CelValue`,
`CelInput`, `CelAbi`, the host-fn signature, the `MessageBacking`
interface), committed by WI-0.1 so every WI imports stable types from day
one and integration in WI-1.5 is wiring, not redesign.

### Definition of done (the milestone gate) — as shipped

1. ✅ `bindings/ts`: `lint` + `build` + `typecheck` + `test` green across
   all packages — **781 tests pass / 13 reasoned skips** (41 files).
2. ✅ The eval binding evaluates the golden Programs (27/27) and the e2e
   ports (200 assertions, 8 suites).
3. ✅ The compiler binding compiles arbitrary in-scope CEL → a Program the
   eval binding runs (byte-identical to the golden fixtures).
4. ⚠️ The conformance harness runs the corpus under a committed monotonic
   ratchet — **but at 1451 pass / 336 fail, NOT "0 fail."** The 336 fails
   are tracked (the ratchet forbids regressing the pass count or growing
   the fail count) but not yet each converted to a reasoned skip; that
   categorization is Future work. (The full-corpus run also OOMs near
   ~1400 rows in a single process — the harness needs streaming/batching
   to scale; the vitest pins a subset + the unit suites.)
5. ✅ The demo compiles → downloads → runs in the browser, errors inline
   in Monaco (headless-verified: `vite build`, the compile endpoint, and
   the `run.test.ts` compile→eval wiring; the Monaco glue itself is
   browser-only, verified via build + the `web/README.md` manual steps).
6. ✅ `compiler.wasm` (WI-2.4) has a written conditional-GO go/no-go
   (`m29-wi24-emscripten-spike.md`); the demo runs on the subprocess/
   local-endpoint fallback.

### Future work (surfaced during execution)

- **Categorize the 336 conformance fails** into fixes vs reasoned skips
  (the one honest gap vs the "0 fail" aspiration). Make the full-corpus
  run streaming/batched so it doesn't OOM (~1400 rows in one process).
- **N-API and emscripten compile backends** behind the existing
  `CompileBackend` interface (subprocess is v1; emscripten is the
  conditional-GO spike — protobuf-to-wasm is the ~3–5 day long pole).
- **Host-fn and message-var e2e fixtures** — the `cel_fn.*` and
  message-typed-variable paths are unit-pinned but not yet driven from a
  compiled `@host` / message Program (the CLI backend has no `@host`-decl
  flag; a compiled message-typed fixture is needed).
- The `typecheck` gate (`tsc --noEmit` incl tests, added this milestone)
  catches the test-only type-error class the build/lint/vitest gate
  missed — keep it in CI.

### Risks

- **emscripten compiler.wasm** (WI-2.4) is the one genuinely hard,
  possibly-multi-day piece — time-boxed, with a fallback so it never
  blocks the demo.
- **textproto parsing in TS** (WI-3.1) — the corpus is textproto; need a
  parser (protobufjs + the SimpleTest schema, or a focused reader).
- **Proto-bearing conformance rows** are out of scope v1 → large skip
  count; that's expected and tracked by category, not a regression.
