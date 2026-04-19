# M0 — Parser CLI

Status: **done** (2026-04).

## Scope

Stand up the Bazel build, vendor `cel-cpp` (parser only), and produce a
`celwasmc -e "<cel>"` binary that parses an expression and prints its
`cel::expr::ParsedExpr` textproto.  This proves the front-end plumbing end to
end and gives us a reproducible entry point for every later milestone.

## Checklist

- [x] Root `MODULE.bazel` declares `abseil-cpp` 20260107.0 + `protobuf` 33.4 +
      `cel-cpp` (local_path_override → `third_party/cel-cpp`).
- [x] `compiler/cli:celwasmc` depends on `@cel-cpp//parser` +
      `//proto/cel/expr:syntax_cc_proto` only.
- [x] `-e` flag parses a CEL expression and prints the textproto to stdout;
      parse errors go to stderr with exit code 1.
- [x] Smoke-tested on literals, identifiers, binary ops, and standard macros
      (`exists`, `all`) including nested comprehensions.

## Follow-ups rolled into later milestones

- Integrating the type checker → M1.
- Driving codegen from a checked AST → M2.
