# `cel` — command-line driver for the CEL→WASM AOT pipeline

One-shot CLI for compiling, type-checking, and evaluating CEL expressions.

```bash
cel eval "1 + 2 + 3"                                  # → 6
cel eval "a * b" --var "a:int=6" --var "b:int=7"      # → 42
cel eval 'd > duration("1s")' --var 'd:duration="2s"' # → true
cel eval "[1, 3, 5, 7].exists(x, x > 5)"              # → true
cel check "u.name" --proto user.proto --var "u:acme.User"   # → OK
cel compile "a * b + 1" --var "a:int" --var "b:int" --output expr.wasm
cel inspect expr.wasm                                 # what does it declare?
cel run expr.wasm --var "a=6" --var "b=7"              # → 43 (no recompile)
```

```
cel eval     <expr> [flags...]  compile + evaluate; print the result
cel check    <expr> [flags...]  parse + type-check; print OK / errors
cel compile  <expr> [flags...]  emit wasm bytes (--output PATH or stdout)
cel run      <prog.wasm> [flags...]
                                evaluate a precompiled program; no recompile
cel inspect  <prog.wasm>        print what the program declares
cel generate --idl PATH --out_dir DIR
                                emit custom-function bindings (fns.wit,
                                codec.h, generated_stub.cc, user_fns.h)
                                from a .idl file
cel embed-decls --plugin PATH --idl PATH --out PATH
                                embed the .idl declaration text into a
                                Component-Model plugin as its cel.fns
                                custom section (the cel_wasm_plugin
                                macro's final step)
```

## Exit codes

| Code | Meaning |
|------|---------|
| `0`  | Success. |
| `1`  | **The expression or program failed** — parse/check diagnostics, a compile failure, a non-OK evaluation, or a result that is a CEL **error** (`1/0`, missing key, overflow) or **unknown**. |
| `2`  | **Usage** — unknown subcommand, unrecognized or invalid flag, malformed `--var` / `--format`, wrong positional count, unreadable input. |

Diagnostics go to **stderr**; only a successful result body goes to
stdout. That split is what makes the CLI safe to script:

```bash
if result=$(cel eval "$expr" --var "n:int=$n"); then
  echo "ok: $result"        # only reached for a real value
else                        # $? is 1 (CEL said no) or 2 (bad invocation)
  echo "failed: $?" >&2
fi
```

A CEL error is a legitimate *library* result — `Instance::Eval` returns
it as a `Value` your C++ code can catch with `||` or `?:` — but at the
process boundary it means the expression produced no result, so `cel`
reports it on stderr and exits `1`.

Binary: `bazel-bin/tools/cel/cel` (built via
`bazel build //tools/cel:cel`).  Note: under `bazel run //tools/cel:cel`,
a relative `--output` path is resolved inside the runfiles tree, not your
shell's working directory — pass an absolute path (e.g.
`--output /tmp/expr.wasm`).

## Compile once, run later

`compile` emits a portable `.wasm`; `run` evaluates it with no
recompile, and `inspect` tells you what it needs first.

```bash
cel compile "a * b + 1" --var "a:int" --var "b:int" --output expr.wasm

cel inspect expr.wasm
# vars:       a:int, b:int
# plugin fns: none
# host fns:   none
# link:       static (cel.abi v1, runtime abi v4)

cel run expr.wasm --var "a=6" --var "b=7"    # → 43
```

On `run`, `--var` supplies **values only** — the type comes from the
program's `cel.abi` section, so you never re-declare it. Binding a
name the program doesn't declare is a usage error that lists the ones
it does, and leaving a declared variable unbound is caught before
evaluation rather than partway through it.

Aggregates and messages bind the same way — `cel.abi` carries each
variable's full declared type, so the element, key/value, and message
FQN all travel with the artifact:

```bash
cel run list.wasm --var "xs=[1, 2, 3]"               # list<int>
cel run req.wasm  --proto req.proto \
                  --var 'r=json:{"user":"ada"}'      # acme.Request
```

`inspect` prints the same spellings, in the `--var` grammar, so a line
of its output pastes straight into a binding:

```
vars:       xs:list<int>, m:map<string,int>, r:acme.Request
```

Artifacts compiled before that field existed carry only the kind. Those
still bind for scalars; an aggregate needs the explicit
`--var name:Type=value` form, and the error says so.

**What `run` cannot do:** evaluate a program that calls `@host` custom
functions. Those are C++ in *your* process, and no generic binary can
supply them. The program declares its own requirements in `cel.abi`, so
`run` refuses up front and names them:

```
ERROR: this program requires @host function(s) the CLI cannot supply:
       string upper(this string)
  @host implementations are C++ in your process — evaluate via the C++
  API (Engine::AddFunction / Engine::Use), or redefine the function
  with a @plugin backend so it travels with the artifact
```

`inspect` shows the same split ahead of time: `plugin fns:` are
satisfiable with a wasm artifact, `host fns:` are not.

## Scalar arithmetic

```bash
cel eval "1 + 2 + 3"             # → 6
cel eval "10 / 3"                # → 3
cel eval "10.0 / 3.0"            # → 3.33333
cel eval "1 + uint(2)"           # ERROR: no matching overload for '_+_'
                                 # applied to '(int, uint)' — no implicit
                                 # numeric coercion in CEL.
```

## Bound variables

```bash
cel eval "a * b" --var "a:int=6" --var "b:int=7"          # → 42
cel eval 'name + "!"' --var 'name:string="hello"'         # → "hello!"
cel eval "n < 0" --var "n:int=-5"                         # → true
cel eval 'd > duration("1s")' --var 'd:duration="2s"'     # → true
cel eval 't > timestamp("2024-01-01T00:00:00Z")' \
        --var 't:timestamp="2025-06-01T00:00:00Z"'        # → true
cel eval 's + " (" + string(n) + ")"' \
        --var 's:string="Ada"' --var "n:int=36"           # → "Ada (36)"
```

The parser is **type-directed** — same literal, different declared type,
different result:

```bash
cel eval "x" --var "x:int=42"      # → 42
cel eval "x" --var "x:uint=42"     # → 42u
cel eval "x" --var "x:double=42"   # → 42
cel eval "x" --var "x:int=3.14"    # ERROR: unexpected trailing characters
                                   # at offset 1 in value: 3.14
```

Bytes accept either inline `b"\xHH..."` literals or `@path.bin`:

```bash
cel eval "p" --var 'p:bytes=b"\x00\x01\x02"'          # → b"\x00\x01\x02"
printf '\x00\x01\x02' > /tmp/blob.bin
cel eval "size(p)" --var "p:bytes=@/tmp/blob.bin"     # → 3
```

## Lists and maps

Literal in source:

```bash
cel eval "[1, 3, 5, 7].exists(x, x > 5)"      # → true
cel eval "[1, 2, 3].map(x, x * 2)"            # → [2, 4, 6]
cel eval '{"us": 1, "ca": 2}["us"]'           # → 1
cel eval '"ca" in {"us": 1, "ca": 2}'         # → true
```

Bound via `--var` — values contain commas, so quote the whole flag:

```bash
cel eval "xs.exists(x, x > 5)" \
        --var "xs:list<int>=[1, 3, 5, 7]"             # → true

cel eval 'm["us"]' \
        --var 'm:map<string,int>={"us": 1, "ca": 2}'  # → 1

cel eval "size(xs)" \
        --var 'xs:list<string>=["a", "b", "c"]'       # → 3
```

## Proto messages — `--proto`

`--proto` parses **one** `.proto` source file in-process via
`google::protobuf::compiler::Parser` (NOT `protoc`'s `Importer`). It
records `import` statements in `FileDescriptorProto.dependency[]` but
does NOT recursively load them. Imports are resolved through the
**generated `DescriptorPool`** — the one the binary statically links
via cel-cpp and libprotobuf.

**What works with `--proto`:**

- A single self-contained `.proto` file with no user imports.
- A `.proto` that imports only well-known types
  (`google/protobuf/{timestamp,duration,empty,wrappers,any,struct}.proto`,
  …) — these are linked into the binary and resolve out of
  `DescriptorPool::generated_pool()` automatically.

```bash
# testdata/e2e_fixture.proto imports timestamp + duration
# (both WKTs) — resolves cleanly.
cel eval "u.name" \
  --proto testdata/e2e_fixture.proto \
  --var 'u:celwasm.testdata.Customer=txtpb:name: "Ada" user_id: 42'
# → "Ada"

cel eval "u.age" \
  --proto testdata/e2e_fixture.proto \
  --var 'u:celwasm.testdata.Customer=json:{"name":"Ada","age":36}'
# → 36

# Or from a file — recognised by extension (.txtpb, .json, .pb binary).
printf 'name: "Ada"\nage: 36\n' > /tmp/ada.txtpb
cel eval "u.name" \
  --proto testdata/e2e_fixture.proto \
  --var "u:celwasm.testdata.Customer=@/tmp/ada.txtpb"
# → "Ada"
```

**What does NOT work:** user-file imports. If `user.proto` declares
`import "address.proto"`, the descriptor pool rejects the **entire
file atomically** — even fields that don't touch the broken import
(e.g. a plain `string name`) become unresolvable.

```bash
# /tmp/user.proto:
#   syntax = "proto3"; package demo;
#   import "address.proto";
#   message User { string name = 1; Address address = 2; }
#
# /tmp/address.proto:
#   syntax = "proto3"; package demo;
#   message Address { string city = 1; string country = 2; }

cel eval "u.name" --proto /tmp/user.proto \
  --var 'u:demo.User=txtpb:name: "Ada"'
# E0000 ... descriptor.cc:5261] Invalid proto descriptor for file "/tmp/user.proto":
# E0000 ... descriptor.cc:5264]   address.proto: Import "address.proto" was not found or had errors.
# E0000 ... descriptor.cc:5264]   demo.User.address: "Address" is not defined.
# ERROR: --var u: message type `demo.User` not found in descriptor pool
#        — did you pass --proto or --descriptor_set?
```

If you see `Import "<file>" was not found or had errors` followed by a
cascade of `"<Type>" is not defined` lines, you need
`--descriptor_set` instead.

## Proto messages — `--descriptor_set`

For multi-file projects, pre-compile a `FileDescriptorSet` with `protoc`
and pass it via `--descriptor_set`. The `--include_imports` flag is
load-bearing — without it the same import cascade fires.

```bash
# Build the FDS once (any non-WKT import must be included).
protoc --include_imports \
       --descriptor_set_out=/tmp/schema.fds \
       /tmp/user.proto /tmp/address.proto

# Then point cel at the FDS.
cel eval "u.address.city" \
  --descriptor_set /tmp/schema.fds \
  --var 'u:demo.User=txtpb:name: "Ada" address { city: "Berlin" }'
# → "Berlin"
```

The CLI reads the FDS as a binary `FileDescriptorSet` proto and adds
each file to a `SimpleDescriptorDatabase`, then merges with the
generated pool. Common failure modes:

```bash
cel eval "1+1" --descriptor_set /tmp/missing.fds
# ERROR: cannot open --descriptor_set file: /tmp/missing.fds

cel eval "1+1" --descriptor_set /tmp/not-a-proto.txt
# ERROR: --descriptor_set /tmp/not-a-proto.txt is not a valid FileDescriptorSet

# duplicate file name across the set:
# ERROR: duplicate file `user.proto` in --descriptor_set
```

`--proto` and `--descriptor_set` are mutually exclusive:

```bash
cel eval "1+1" --proto a.proto --descriptor_set b.fds
# ERROR: --proto and --descriptor_set are mutually exclusive
```

## Container-qualified names

CEL name resolution mirrors CEL-Go's `container` option: idents are
looked up by stripping the container's segments one at a time.
`--container` affects idents inside the source expression (literal
construction, function lookup); it does NOT shorten the type name in
`--var`, which is always fully qualified.

```bash
cel check "Customer{name: 'Ada'}.name" \
  --proto testdata/e2e_fixture.proto \
  --container celwasm.testdata
# → OK   (Customer resolves as celwasm.testdata.Customer)

cel check "Customer{name: 'Ada'}.name" \
  --proto testdata/e2e_fixture.proto
# ERROR: :1:9: undeclared reference to 'Customer' (in container '')
```

## Multi-format output

`cel eval` prints non-message results as a single tagged line.
Message results default to textproto; `--format` is repeatable.

```bash
cel eval "u" \
  --proto testdata/e2e_fixture.proto \
  --var 'u:celwasm.testdata.Customer=txtpb:name: "Ada" age: 36'
# name: "Ada"
# age: 36

cel eval "u" \
  --proto testdata/e2e_fixture.proto \
  --var 'u:celwasm.testdata.Customer=txtpb:name: "Ada" age: 36' \
  --format=json
# {"name":"Ada","age":36}

cel eval "u" \
  --proto testdata/e2e_fixture.proto \
  --var 'u:celwasm.testdata.Customer=txtpb:name: "Ada" age: 36' \
  --format=cel
# celwasm.testdata.Customer{name: "Ada", age: 36}

cel eval "u" \
  --proto testdata/e2e_fixture.proto \
  --var 'u:celwasm.testdata.Customer=txtpb:name: "Ada" age: 36' \
  --format=textproto --format=json --format=cel
# --- textproto ---
# name: "Ada"
# age: 36
#
# --- json ---
# {"name":"Ada","age":36}
#
# --- cel ---
# celwasm.testdata.Customer{name: "Ada", age: 36}
```

## Compile to wasm

```bash
cel compile "a * b + 1" \
  --var "a:int" --var "b:int" \
  --output /tmp/expr.wasm
# wrote 4900 bytes to /tmp/expr.wasm

# Inspect with binaryen:
wasm-dis /tmp/expr.wasm | less
```

`--O 0..3` is Binaryen's optimizer level (default `0`, recommended
`2` for hot-path use). With no `--output`, wasm bytes go to stdout.

## Just type-check

`cel check` runs parse + check; useful in CI to catch typos early.

```bash
cel check "a * b + 1" --var "a:int" --var "b:int"
# OK

cel check "u.name.endsWith('@acme.com')" \
  --proto testdata/e2e_fixture.proto \
  --var "u:celwasm.testdata.Customer"
# OK

cel check "u.unknown_field" \
  --proto testdata/e2e_fixture.proto \
  --var "u:celwasm.testdata.Customer"
# ERROR: :1:2: undefined field 'unknown_field' not found in struct
# 'celwasm.testdata.Customer'
```

## `--var` syntax reference

| Type            | Example                                                       |
|-----------------|---------------------------------------------------------------|
| `bool`          | `--var "b:bool=true"`                                         |
| `int`           | `--var "n:int=-42"`                                           |
| `uint`          | `--var "n:uint=42"` (`42u` also accepted)                     |
| `double`        | `--var "f:double=3.14"` (also `1e-3`)                         |
| `string`        | `--var 's:string="Ada"'` (`"…"` or `'…'`; standard escapes)   |
| `bytes`         | `--var 'p:bytes=b"\x00\x01"'` or `--var "p:bytes=@/tmp/x.bin"`|
| `duration`      | `--var 'd:duration="3s"'`                                     |
| `timestamp`     | `--var 't:timestamp="2024-01-01T00:00:00Z"'`                  |
| `list<T>`       | `--var "xs:list<int>=[1, 2, 3]"`                              |
| `map<K,V>`      | `--var 'm:map<string,int>={"a": 1, "b": 2}'`                  |
| `<F.Q.N>` proto | `--var 'u:acme.User=txtpb:name: "Ada"'`                       |
|                 | `--var 'u:acme.User=json:{"name":"Ada"}'`                     |
|                 | `--var "u:acme.User=@/tmp/u.{txtpb,json,pb}"` (by extension)  |
| declaration     | `--var "a:int"` (declare without binding — for `check`/`compile`)|

The parser is type-directed; there is no silent coercion. See
`var_parser.h` for the full grammar, `var_parser_test.cc` for the
exhaustive test matrix.

`--var` and `--format` carry values that may legally contain commas
and absl's default repeatable-flag parser comma-splits them. The CLI
extracts both from argv before absl parses (see `ExtractRepeated` in
`cel.cc`); consequence — **always quote the whole `--var` argument**.

## `--format` reference

`--format` is meaningful only for `eval`, and only when the result is
a message-typed value. Aggregate non-message kinds (list, map,
unknown, error) always render as a single tagged line.

| Value       | Output                                              |
|-------------|-----------------------------------------------------|
| `textproto` | TextFormat as produced by `protobuf::TextFormat`    |
| `json`      | proto3 canonical JSON via `MessageToJsonString`     |
| `cel`       | CEL literal form (`pkg.Type{field: value, ...}`)    |

With one entry, the body is printed bare; with multiple, each
section is preceded by a `--- <name> ---` header.

## Common errors

| Symptom                                                                  | Likely cause                                              |
|--------------------------------------------------------------------------|-----------------------------------------------------------|
| `ERROR: --proto and --descriptor_set are mutually exclusive`             | pass exactly one of the two                               |
| `descriptor.cc:5261 Invalid proto descriptor … Import "X" was not found` | `--proto` does not recurse imports; use `--descriptor_set` |
| `ERROR: --var u: message type 'X' not found in descriptor pool`          | type name misspelt, or schema file not passed             |
| `ERROR: --descriptor_set X is not a valid FileDescriptorSet`             | file is not the binary output of `protoc --descriptor_set_out` |
| `ERROR: --var x: unexpected trailing characters at offset N in value`    | value text doesn't match declared type (e.g. `int=3.14`)  |
| `ERROR: undefined field 'X' not found in struct 'Y'`                     | field absent from the bound message                       |
| `ERROR: found no matching overload for '_+_' applied to '(int, uint)'`   | no implicit numeric coercion in CEL                       |
| `ERROR: undeclared reference to 'X' (in container '')`                   | missing `--container PKG` for a short name                |
| `ERROR: expression's static footprint ... exceeds ...`                   | too many constants/slots — simplify the expression        |

## Where to look next

- `var_parser.h` / `var_parser_test.cc` — full `--var` grammar +
  exhaustive shape coverage (bytes-from-file, embedded-NUL strings,
  surrogate-pair escapes).
- `value_format.h` / `value_format_test.cc` — `--format` semantics
  for every CEL value kind.
- `activation_matrix_test.cc` — the full activation matrix the CLI
  was built to surface: scalars + lists + maps + bound messages +
  proto map / repeated / nested submessage fields.
- `cel_smoke_test.sh` — fast end-to-end smoke against a built `cel`
  binary; run as `bazel test //tools/cel:cel_smoke_test`.
- `abi/runtime_catalogue.h` — single source of truth for
  the runtime + host imports the compiled wasm depends on; relevant
  if a `cel eval` traps at instantiate with a "missing export"
  diagnostic.
