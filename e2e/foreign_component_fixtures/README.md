# `e2e/foreign_component_fixtures/` — fixture components for the m24 e2e suite

Per-section component fixtures that the m24 e2e suite
(`e2e/foreign_fn_type_matrix_test.cc`) loads via `Engine::AddComponent`.
A fixture is a self-contained Component-Model component:
author-supplied native C++ (or TinyGo, m24 §11 forcing function) +
`celfnc`-emitted codec + stub, built to `.wasm`.

  - **`stub_demo/`** — the validated 17-case author component from m24
    §5 (`user_fns.cc`, `codec.h`, `generated_stub.cc`, `fns.wit`,
    `driver_main.cc`). Originally landed under
    `doc/implementation-plan/rewrite/wit/stub-demo/` as the design-
    surface proof; relocated here when the m24 e2e suite started
    referencing it.

Each fixture builds to a `.wasm` artifact the e2e harness loads. The
build wiring (`cc_binary` with the wasi-sdk component-model toolchain,
`wasm-tools component` post-step) lands alongside `Engine::AddComponent`
in the m24 §3.5 slice; until then these are reference sources, not
bazel targets.

See
[`../../doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md`](../../doc/implementation-plan/rewrite/m24-foreign-fn-component-backend.md)
for the design.
