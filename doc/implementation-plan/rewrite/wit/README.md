# `wit/` — Component Model interface for foreign CEL custom functions

Design surface for the **Regime B** (untrusted / polyglot) custom-function
ABI: a separately-compiled, isolated foreign module that the host links
against the CEL evaluator via the WebAssembly Component Model. See
[`../m23-foreign-fn-component-abi.md`](../m23-foreign-fn-component-abi.md)
for the full rationale, the cost measurements, and the per-type
representation rule.

  - **`cel.wit`** — the complete CEL value model in WIT. Every CEL type;
    arbitrary nesting via a `value` **resource** (WIT forbids recursive
    value types, so aggregates nest through handles); proto messages
    cross as serialized `bytes`. Validates with
    `wasm-tools component wit cel.wit`.
  - **`bench/`** — throwaway benchmarks that produced the boundary-cost
    numbers (cross-component call ~410 ns; every value-handle op
    ~500 ns; by-value scalars free). External toolchain, not in the
    bazel build. See `bench/REPRODUCE.md`.

This is a research/design artifact, not a shipped interface. No
`compiler/`, `eval/`, or `runtime/` code depends on it yet.
