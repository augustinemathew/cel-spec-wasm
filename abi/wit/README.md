# `abi/wit/` — WIT vocabulary for the cross-component CEL boundary

ABI contract surface for the **foreign-component custom-function**
boundary (m24 Regime B). Parallel to `abi/cel_abi.proto` (the host /
guest wire contract): `cel.wit` is the cross-component-boundary type
vocabulary the Component-Model-backed foreign-function path uses.

  - **`cel.wit`** — the complete CEL value model in WIT. Every CEL type;
    arbitrary nesting via a `value` **resource** (WIT forbids recursive
    value types, so aggregates nest through handles); proto messages
    cross as serialized `bytes`. Validates with
    `wasm-tools component wit cel.wit`.

The `cel.wit` value model is reserved for the **dynamic / variadic**
path (m24 §4); the common case — concretely-typed custom fns — goes
through per-fn typed WIT that `celfnc` generates, not through this
shared `value` resource. See
[`../../doc/implementation-plan/rewrite/m23-foreign-fn-component-abi.md`](../../doc/implementation-plan/rewrite/m23-foreign-fn-component-abi.md)
for the cost rationale and per-type representation rule.

No first-party `compiler/` / `eval/` / `runtime/` code consumes this
yet — m24 is the milestone that wires it in.
