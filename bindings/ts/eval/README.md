# `@celwasm/eval` — TypeScript eval host for compiled CEL

Run a CEL expression that was **compiled to WebAssembly** by the C++
`cel compile` pipeline, in Node or the browser. There is no compiler here:
you ship the compiled `Program` (wasm bytes + `cel.abi` section) across the
boundary and this library evaluates it — instantiating `cel_runtime.wasm`,
marshalling your inputs, calling `$eval`, and decoding the result.

It mirrors the C++ embedder API (`doc/user-guide.md`): `Engine` →
`Program` → `Instance`, with an `Activation` of bound `Value`s in and a
`Value` out.

## Install

```jsonc
// the only runtime dependency
"dependencies": { "@bufbuild/protobuf": "^2" }
```

You need two build artifacts from the C++ side:

- `cel_runtime.wasm` — `bazel build //compiler_v2/runtime:cel_runtime_wasm`
- a compiled `Program` — `cel compile "<expr>" --var ... -o expr.wasm`

## 60-second example

```ts
import { readFileSync } from 'node:fs';
import { Engine, Program, Activation, Value, asInt } from '@celwasm/eval';

// 1. Compile the runtime ONCE; reuse the Engine across many programs.
const engine = await Engine.create(readFileSync('cel_runtime.wasm'));

// 2. Load a compiled Program  (cel compile "a * b + 1" --var a:int --var b:int)
const program = Program.fromBytes(readFileSync('expr.wasm'));

// 3. Plan → Instance (compile-once, eval-many).
const instance = await engine.plan(program);

// 4. Bind inputs and evaluate. int/uint are bigint (64-bit, no precision loss).
const result = instance.eval(
  new Activation().bind('a', Value.int(6n)).bind('b', Value.int(7n)),
);

console.log(asInt(result)); // 43n
```

`Engine.create` and `engine.plan` are `async` (they
`WebAssembly.compile`/`instantiate`); `instance.eval` is synchronous, so
the hot eval loop has no awaits.

## Values in and out

`Value` is one discriminated union covering every CEL type. Factories
build them; `asX` accessors read them back (throwing `ValueError` on a
kind mismatch).

```ts
Value.null();  Value.bool(true);  Value.int(42n);  Value.uint(7n);
Value.double(1.5);  Value.string('hi');  Value.bytes(new Uint8Array([1, 2]));

asBool(v); asInt(v); asUint(v); asDouble(v); asString(v); asBytes(v);
isNull(v); isError(v); isUnknown(v);
```

CEL-level **errors and unknowns come back as a `Value`** (`isError(r)` /
`isUnknown(r)`) — you inspect them, you don't catch them. Host-level
failures (unbound variable, kind mismatch, malformed program) **throw**
(`EvalError` / `ValueError` / `EngineError`).

## Messages, lists, and maps

Aggregates can be bound and returned in two backings:

### Plain JS objects (mirror a proto / JSON)

No descriptors needed. A JS object is a message, an array a list, a `Map`
a map. (`bigint`→int, `number`→double, `Uint8Array`→bytes.)

```ts
const u = Value.object({ name: 'Ann', age: 30n, tags: ['gold', 'vip'] });
const xs = Value.list([Value.int(10n), Value.int(20n)]);
const m = Value.map([[Value.string('k'), Value.int(7n)]]);

instance.eval(new Activation().bind('u', u)); // e.g. compiled `u.name` → "Ann"
```

### Real protobuf-es messages (full type fidelity)

Supply a `TypeRegistry` so the host has the descriptors. This is the only
thing that needs one — scalars / JS-object messages / lists / maps don't.

```ts
import { create } from '@bufbuild/protobuf';
import { TypeRegistry } from '@celwasm/eval';

const registry = TypeRegistry.fromDescriptorSet(readFileSync('schema.fds.bin'));
const engine = await Engine.create(readFileSync('cel_runtime.wasm'), { registry });

const desc = registry.getMessage('acme.Customer')!;
const customer = create(desc, { name: 'Ann', age: 30 });

const r = instance.eval(
  new Activation().bind('u', Value.message(registry.message(customer))),
);
```

### Binding a JSON object to a **proto-typed** expression

A common case: the expression was compiled and **type-checked against a
proto** (in C++), but at eval time your data is a plain JS object — say it
arrived as JSON over the wire. That works directly, and it needs **no
`TypeRegistry`**: the descriptors were only needed at *compile* time; at
eval time the object backing just reads fields by name.

```bash
# Compiled & type-checked against the proto (in C++). `u.age` is an int32,
# `u.is_premium` a bool — the checker enforces that here.
cel compile 'u.is_premium && u.age > 18' \
    --var u:acme.Customer --descriptor_set schema.fds.bin -o policy.wasm
```

```ts
// No registry — Engine.create takes just the runtime bytes.
const engine = await Engine.create(readFileSync('cel_runtime.wasm'));
const instance = await engine.plan(
  Program.fromBytes(readFileSync('policy.wasm')),
);

// Bind a plain JS object. Field NAMES must match the proto fields; use
// `bigint` for int/uint fields and `number` for double fields (the object
// backing maps bigint→int, number→double).
const u = Value.object({ name: 'Ann', age: 30n, is_premium: true });

asBool(instance.eval(new Activation().bind('u', u))); // → true
//                                age 30 > 18 && premium
```

The same `policy.wasm` accepts a real protobuf-es message too
(`Value.message(registry.message(customer))`) — the Program is
backing-agnostic. Use the object form for JSON/struct data, the message
form when you already hold a typed protobuf-es message.

> Caveat: a plain object can't distinguish `uint` from `int` (a `bigint`
> is `int`), so a `uint`-typed field read through the object backing comes
> back as CEL `int`. If you need exact `uint` fidelity, use a real proto
> message.

### Reading aggregates out

A returned message / list / map is also a `Value`:

```ts
asMessage(r).getField('name');     // read any backing's field generically
registry.toMessage(asMessage(r));  // …or materialize back to a typed proto
asList(r).map(asString);           // ['gold', 'vip']
asMap(r);                          // [[key, value], ...]  (each a Value)
```

## Correctness notes

- **`int` / `uint` are 64-bit → always `bigint`.** `number` silently
  corrupts past 2^53.
- **Bytes are `Uint8Array`.** Strings decode as UTF-8.
- The 24-byte wire `CelValue` is little-endian; the codec handles that —
  you only ever touch `Value`.

## Where things live

| Module | Mirrors (C++) | Role |
|---|---|---|
| `engine.ts` | `engine.cc` | compile runtime, `plan` a Program |
| `instance.ts` | `instance.cc` | marshal activation, `$eval`, decode result |
| `program.ts` | `program.h` | wasm bytes + decoded `cel.abi` |
| `value.ts` | `value.h` | the `Value` union + factories + `asX` |
| `type-registry.ts` | `generated_pool()` | protobuf-es descriptors |
| `abi.ts` | `abi_decode.cc` | `cel.abi` custom-section decode |
| `host/*.ts` | `cel_host.cc` | `cel_host.*` trampolines + backings |

## Running the tests

From the `ts/` directory:

```bash
npm test                  # hermetic unit suite (100% coverage gate)
npm run test:integration  # real-wasm e2e — instantiates cel_runtime.wasm
                          #   and evals compiled Program fixtures
npm run check             # one-shot gate: proto-drift + typecheck + lint
                          #   + format + coverage
```

The e2e (`eval/test/integration/*.e2e.test.ts`) needs the runtime wasm. It
resolves `bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm` by default, or
set `CEL_RUNTIME_WASM` to an explicit path:

```bash
bazel build //compiler_v2/runtime:cel_runtime_wasm   # build it first
npm run test:integration

# or point at a prebuilt copy:
CEL_RUNTIME_WASM=/path/to/cel_runtime.wasm npm run test:integration
```

The Program fixtures under `eval/test/testdata/*.wasm` are regenerated by
`eval/test/testdata/gen_fixtures.sh` (which runs the C++ `cel compile`), and
`fixtures.json` records the CEL source each was compiled from — so a test
title like `u.age → 30` names the exact expression it ran.

See `doc/implementation-plan/rewrite/m19-api-split-ts-eval-host.md` §5.7.1
for the full canonical API and what's still in flight (host functions,
partial-eval/unknowns, arena-backed list/map results).
