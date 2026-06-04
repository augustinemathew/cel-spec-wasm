# `bench/foreign_component/` — boundary-cost benches for the m24 backend

Throwaway component-model benches that produced the boundary-cost
numbers cited in m23 §5 (cross-component call ~410 ns; every
value-handle op ~500 ns; by-value scalars free). External toolchain
(`cargo`, `wasm-tools`, `wac`); **not in the bazel build** — they exist
to capture the cost story before m24 commits to a per-fn typed-WIT
default.

  - **`arg_cost/`** — sweeps argument shapes (scalar, list, nested) and
    measures the per-call cost as a function of payload size.
  - **`typed_fn/`** — measures the typed-WIT path (m24 §4 default),
    which crosses the whole aggregate once via canonical-ABI copy and
    deserializes locally — the alternative to N value-resource handle
    crossings.
  - **`REPRODUCE.md`** — toolchain pins + exact commands.

See
[`../../doc/implementation-plan/rewrite/m23-foreign-fn-component-abi.md`](../../doc/implementation-plan/rewrite/m23-foreign-fn-component-abi.md)
for the design context and what each measurement settled.

When the m24 backend ships with native-wasmtime-component benches in
`bench/` proper, these go away — they were the investigation, not the
production target.
