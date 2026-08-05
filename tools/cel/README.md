# `cel` — command-line driver for the CEL→WASM AOT pipeline

Compile, type-check, and evaluate CEL expressions without writing C++.

```bash
bazel build //tools/cel:cel        # once
bazel-bin/tools/cel/cel eval '1 + 2 + 3'
```

Every example below runs against files in this repo, so you can paste
them as-is. `cel` below means `bazel-bin/tools/cel/cel`.

| Command | Argument | Does |
|---|---|---|
| `eval` | `<expr>` | compile **and** evaluate; print the result |
| `check` | `<expr>` | parse + type-check only; print `OK` or the errors |
| `compile` | `<expr>` | emit portable wasm bytes |
| `run` | `<prog.wasm>` | evaluate a precompiled program — no recompile |
| `inspect` | `<prog.wasm>` | print what a program declares and requires |

`cel --help` is the authoritative flag list — per command, required and
optional split out.

## What you can write

```bash
cel eval '1 + 2 + 3'                              # → 6
cel eval '"ada".startsWith("a")'                  # → true
cel eval 'size("hello")'                          # → 5
cel eval '[1, 3, 5].exists(x, x > 4)'             # → true
cel eval '{"us": 1}["us"]'                        # → 1
cel eval 'duration("2s") > duration("1s")'        # → true
cel eval 'n > 5 ? "big" : "small"' --var n:int=9  # → "big"
```

The language itself is [`doc/langdef.md`](../../doc/langdef.md); what
this implementation accepts and rejects is in the
[user guide](../../doc/user-guide/index.md).

## Quick start — compile once, run later

`compile` emits a portable artifact, `run` evaluates it with no
recompile, and `inspect` says what it needs first.

```bash
cel compile 'a * b + 1' --var a:int --var b:int --output /tmp/expr.wasm

cel inspect /tmp/expr.wasm
# vars:       a:int, b:int
# host fns:   none
# link:       static (cel.abi v1, runtime abi v4)

cel run /tmp/expr.wasm --var a=6 --var b=7        # → 43
```

On `run` you bind **values only** — each variable's declared type
travels with the program in its `cel.abi` section, so aggregates and
messages bind the same way scalars do.

## Commands

### `eval`

Compile and evaluate in one shot. Recompiles every time; prefer
`compile` + `run` when the same expression runs repeatedly.

```bash
cel eval 'a * b' --var a:int=6 --var b:int=7      # → 42
```

### `check`

Parse and type-check without generating code — the fast gate for
validating user-supplied expressions.

```bash
cel check 'a * b + 1' --var a:int --var b:int     # → OK
```

### `compile`

Emit wasm bytes. Without `--output` they go to stdout. Use `--O 2` on
a hot path: roughly half the eval time on chain-heavy expressions, at
2–3× the compile cost.

```bash
cel compile 'a * b + 1' --var a:int --var b:int --output /tmp/expr.wasm
```

### `run`

Evaluate a precompiled program. `--var` takes values; the types come
from the artifact.

```bash
cel run /tmp/expr.wasm --var a=6 --var b=7        # → 43
```

### `inspect`

Print what an artifact declares and requires, without running it.
Variable types render in the `--var` grammar, so a line of output
pastes straight back into a binding.

```bash
cel inspect /tmp/expr.wasm
```

## Binding variables — `--var`

**Always quote the whole argument.** Values may legally contain
commas, which the flag parser would otherwise split.

| Type | Example |
|---|---|
| `bool` | `--var "b:bool=true"` |
| `int` | `--var "n:int=-42"` |
| `uint` | `--var "n:uint=42"` (`42u` also accepted) |
| `double` | `--var "f:double=3.14"` (also `1e-3`) |
| `string` | `--var 's:string="Ada"'` (`"…"` or `'…'`; standard escapes) |
| `bytes` | `--var 'p:bytes=b"\x00\x01"'` or `--var "p:bytes=@/tmp/x.bin"` |
| `duration` | `--var 'd:duration="3s"'` |
| `timestamp` | `--var 't:timestamp="2024-01-01T00:00:00Z"'` |
| `list<T>` | `--var "xs:list<int>=[1, 2, 3]"` |
| `map<K,V>` | `--var 'm:map<string,int>={"a": 1, "b": 2}'` |
| proto, inline | `--var 'u:acme.User=txtpb:name: "Ada"'` |
| proto, inline | `--var 'u:acme.User=json:{"name":"Ada"}'` |
| proto, file | `--var "u:acme.User=@/tmp/u.txtpb"` (`.txtpb` / `.json` / `.pb`) |
| declare only | `--var "a:int"` — for `check` / `compile` |
| bind only | `--var "a=6"` — for `run`; the type comes from the artifact |

The parser is type-directed and never coerces: `--var "n:int=3.14"` is
an error, not a truncation. Full grammar in
[`var_parser.h`](var_parser.h), exhaustive matrix in
`var_parser_test.cc`.

## Message types — `--proto` / `--descriptor_set`

Message-typed variables need their schema:

```bash
cel eval 'u.name' --proto testdata/e2e_fixture.proto \
    --var 'u:celwasm.testdata.Customer=txtpb:name: "Ada"'      # → "Ada"

cel eval 'has(u.name)' --proto testdata/e2e_fixture.proto \
    --var 'u:celwasm.testdata.Customer=txtpb:name: "Ada"'      # → true
```

**`--proto` does not follow user imports.** It parses one file;
imports resolve only against types linked into the binary (the
well-known types). A `.proto` importing another *user* file is
rejected atomically — even fields that never touch the import become
unresolvable. For those, pre-compile a descriptor set:

```bash
protoc --include_imports --descriptor_set_out=/tmp/schema.fds \
       user.proto address.proto
cel eval 'u.address.city' --descriptor_set /tmp/schema.fds \
    --var 'u:demo.User=@/tmp/user.txtpb'
```

`--include_imports` is load-bearing; without it the same cascade
fires. The two flags are mutually exclusive.

`--container` shortens idents *inside the expression*; it does not
shorten the type name in `--var`, which is always fully qualified:

```bash
cel check "Customer{name: 'Ada'}.name" \
    --proto testdata/e2e_fixture.proto --container celwasm.testdata   # → OK
```

## Custom functions

`@host` functions are C++ callbacks in the embedder's process
(`Engine::AddFunction`), so a program that requires one runs only
through the C++ API — no generic binary can supply the
implementation. `inspect` lists what a program requires, and `cel
run` refuses such a program up front, naming the functions, rather
than surfacing a wasm link error.

## Output — `--format`

Repeatable. Applies only to **message-typed** results; other kinds
(list, map, unknown, error) always render as one tagged line.

| Value | Output |
|---|---|
| `textproto` (default) | `protobuf::TextFormat` |
| `json` | proto3 canonical JSON |
| `cel` | CEL literal — `celwasm.testdata.Customer{name: "Ada"}` |

With one value the body prints bare; with several, each section gets a
`--- <name> ---` header.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Success. |
| `1` | The expression or program failed — diagnostics, a non-OK evaluation, or a result that is a CEL **error** or **unknown**. |
| `2` | Usage — unknown subcommand, bad flag, malformed `--var` / `--format`, wrong positional count, unreadable input. |

Diagnostics go to stderr; only a successful result goes to stdout, so
the CLI is safe to branch on:

```bash
if result=$(cel eval "$expr" --var "n:int=$n"); then
  echo "ok: $result"
else
  echo "failed with $?" >&2      # 1 = CEL said no, 2 = bad invocation
fi
```

## Common errors

| Symptom | Likely cause |
|---|---|
| `--proto and --descriptor_set are mutually exclusive` | pass exactly one |
| `Invalid proto descriptor … Import "X" was not found` | `--proto` does not recurse imports; use `--descriptor_set` |
| `--var u: message type 'X' not found in descriptor pool` | type name misspelt, or schema not passed |
| `--descriptor_set X is not a valid FileDescriptorSet` | not the binary output of `protoc --descriptor_set_out` |
| `--var x: unexpected trailing characters at offset N` | value doesn't match the declared type (e.g. `int=3.14`) |
| `undefined field 'X' not found in struct 'Y'` | field absent from the bound message |
| `found no matching overload for '_+_' applied to '(int, uint)'` | CEL has no implicit numeric coercion |
| `undeclared reference to 'X' (in container '')` | missing `--container`, or an undeclared variable / function |
| `expression's static footprint … exceeds …` | too many constants/slots — simplify |

## Where to look next

- [Getting started](../../doc/user-guide/getting-started.md) — the C++
  embedding path
- [User guide](../../doc/user-guide/index.md) — the full embedder API
- [`var_parser.h`](var_parser.h) / [`value_format.h`](value_format.h) —
  the `--var` and `--format` grammars, with exhaustive shape coverage
  in their `_test.cc` siblings
- `activation_matrix_test.cc` — the full activation matrix (scalars,
  lists, maps, bound messages, proto map / repeated / nested fields)
- `abi/runtime_catalogue.h` — the runtime + host imports a compiled
  wasm depends on; relevant if `eval` traps at instantiate with a
  "missing export" diagnostic

Under `bazel run //tools/cel:cel`, a relative `--output` path resolves
inside the runfiles tree — pass an absolute path.
