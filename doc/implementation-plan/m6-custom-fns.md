# M6 — User-defined custom functions

Status: **planned.**  Blocked on M5 (three-valued logic) — custom
functions return the same `OK / UNKNOWN / ERROR` tri-state as built-in
overloads.

## Scope

Let users extend the compiler with their own functions, declared
statically as a `FunctionSet` proto or a `.celfn` IDL file.  The
compiler emits a host import per custom function; the host is
expected to provide the implementation at instantiation time.

Post-M6:

  - `celwasmc -e "is_admin(user)" --functions=my_fns.pb.bin` compiles.
  - The emitted module imports `cel_fn.is_admin(externref) → CelValue*`.
  - A wrapper generator (`celfnc`) produces host-language stubs for
    C, C++, Go — the user fills in the body.

Out of scope:
  - Macros (the spec reserves macros and forbids user-defined ones).
  - Pure-WASM custom functions authored in Rust/C — **M7** extension.

## Deliverables

### IDL + config

- [ ] `compiler/functions/function_set.proto` — a `FunctionSet`
      message with repeated function declarations
      (`name`, `receiver_type` or unset for free, `param_types`,
      `return_type`, `description`).  Mirror cel-cpp's own
      FunctionSet where possible.
- [ ] `.celfn` IDL — a shorthand text form for small projects.
      Grammar: one `fn name(args): ret;` per line.  Parser hand-rolled
      (it's tiny); converts to `FunctionSet` proto internally.

### Codegen

- [ ] For each declared custom function, emit a `cel_fn.<name>` host
      import with the checked signature.  Externref for messages, the
      scalar ABI for primitives, `i32` (`CelValue*`) for
      strings/bytes/lists/maps.
- [ ] `kCallExpr` for a custom function name resolves against the
      FunctionSet and lowers to a `cel_host` call just like a built-in
      overload.  The checker's `reference_map` entry already points
      at the resolved function; codegen consumes the id.

### Tooling

- [ ] `celfnc` binary — reads a `FunctionSet` + target language and
      emits a stub file.  C target: a header declaring each function
      + a skeleton .c calling `cel_unknown` for every body.  Go
      target: a wasmtime host binding that maps each import to a
      user-registered callback.
- [ ] `celwasmc --functions=<path>` flag — accepts `.pb.bin` (proto)
      or `.celfn` (IDL).

## Testing obligations

- [ ] `functions/function_set_test.cc` — parses every primitive +
      message + collection return type; rejects unknown type names.
- [ ] `functions/celfn_parser_test.cc` — positive + negative cases
      for every grammar production.
- [ ] `codegen/custom_fn_test.cc` — a module with one custom fn
      emits the expected import and the `cel_fn.<name>` call.
- [ ] e2e: a custom fn returning a scalar, a custom fn returning a
      message (verifies externref round-trip), a custom fn returning
      ERROR / UNKNOWN (verifies tri-state plumbing).
- [ ] `celfnc_test.sh` — runs the generator on a fixture IDL and
      diffs the output against a golden file (stored under
      `compiler/functions/testdata/goldens/`).

## Open design questions

1. **Overload resolution.** A custom fn named `size` that accepts
   a custom type — does it win over the built-in `size(string)`?
   cel-cpp's rule is "more specific wins"; document it here once
   verified.
2. **Host-side type bindings.** For each language, how do we keep
   the generated stub and the compiler in sync as types evolve?
   Probably a version field in the `cel.abi.function_set` payload
   that the host stub checks.
