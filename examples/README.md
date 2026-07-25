# Examples

Nine small programs, in reading order. Each one is a complete,
buildable embed you can copy as a starting point — and each is run by
`:examples_smoke_test` on every test sweep, so what you read here is
what actually executes.

```bash
bazel run //examples:01_hello_world
```

| # | Example | What it shows |
| --- | --- | --- |
| 01 | [`01_hello_world.cc`](01_hello_world.cc) | The whole pipeline in ~30 lines: compile → JIT → eval. |
| 02 | [`02_variables.cc`](02_variables.cc) | Statically-typed variables, `Activation` bindings, one Instance evaluating many inputs. |
| 03 | [`03_compile_once_run_anywhere.cc`](03_compile_once_run_anywhere.cc) | The headline act: save the compiled `.wasm` to disk, reload it in a process that never sees the compiler, evaluate. |
| 04 | [`04_host_functions.cc`](04_host_functions.cc) | Extend CEL with your own C++ function — one line of `.celfn` IDL + one typed lambda. |
| 05 | [`05_partial_eval.cc`](05_partial_eval.cc) | Partial evaluation: mark inputs UNKNOWN, learn whether the policy needs them before fetching data. |
| 06 | [`06_proto_messages.cc`](06_proto_messages.cc) | Protobuf messages as CEL variables — field reads, zero copies. |
| 07 | [`07_error_handling.cc`](07_error_handling.cc) | The three failure layers: compile-time `Status`, CEL error *values*, accessor mismatches. |
| 08 | [`08_function_errors_and_unknowns.cc`](08_function_errors_and_unknowns.cc) | A host function returning a CEL error value, a CEL unknown (absorbed by three-valued logic), or an infrastructure failure — and how each surfaces. |
| 09 | [`09_plugin_functions.cc`](09_plugin_functions.cc) | A **sandboxed** custom function: [`adder.idl`](adder.idl) + [`adder_fns.cc`](adder_fns.cc) → a WebAssembly component with its own linear memory, built by the `cel_wasm_plugin` macro and registered at runtime. |

The plugin path in 09 is deliberately scalar-only — string-returning
plugin functions currently trap (a known, pinned bug; see the skip in
[`demo_plugin_e2e_test.cc`](../e2e/plugin_fixtures/cel_wasm_plugin_demo/demo_plugin_e2e_test.cc)).
The deeper guide is
[writing plugins](../doc/user-guide/writing-plugins.md).
