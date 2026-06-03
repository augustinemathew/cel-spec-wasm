# Stub-demo — the foreign custom-fn author experience (validated)

A working, end-to-end demonstration of the developer experience designed
in [`../../m25-foreign-fn-dx.md`](../../m25-foreign-fn-dx.md): a custom-fn
author writes **only native C++** (`int64_t`, `std::string`,
`std::vector`, `std::map`, nested) and a **generated codec** does all the
marshaling. Validated on wasmtime 45: **17/17 e2e assertions pass**,
including boundary cases.

| File | Role | Who writes it |
| --- | --- | --- |
| `fns.wit` | one typed WIT function per custom fn (CEL types mapped concretely) | generator (from the CEL decls) |
| `user_fns.h` / `user_fns.cc` | the custom functions, **native C++ signatures only** | **the author** |
| `codec.h` | lift/lower wit-bindgen `{ptr,len}`/`{f0,f1}` structs <-> `std::` containers | generator |
| `generated_stub.cc` | implements the wit exports: codec-in -> user fn -> codec-out | generator |
| `driver_main.cc` | e2e test harness (the host side): builds CEL-shaped args, asserts results + boundaries | test suite |

The split is the whole point: **`user_fns.cc` is the entire author
surface.** Everything else is mechanical and generated.

## Reproduce

Toolchain + commands as in [`../bench/REPRODUCE.md`](../bench/REPRODUCE.md)
(wasi-sdk clang, `wit-bindgen c`, `wasm-tools component new`, `wac plug`,
`wasmtime run`). Sketch:

```bash
wit-bindgen c ./wit --world author --out-dir gauthor
wit-bindgen c ./wit --world host   --out-dir ghost
# author component (reactor): native impls compiled with -fno-exceptions
clang++ ... -fno-exceptions -fno-rtti -I gauthor user_fns.cc generated_stub.cc gauthor/author.c \
        gauthor/author_component_type.o -mexec-model=reactor -o author.core.wasm
# driver component (command)
clang++ ... -I ghost driver_main.cc ghost/host.c ghost/host_component_type.o -o driver.core.wasm
wasm-tools component new author.core.wasm --adapt ...reactor.wasm -o author.component.wasm
wasm-tools component new driver.core.wasm --adapt ...command.wasm -o driver.component.wasm
wac plug driver.component.wasm --plug author.component.wasm -o app.wasm
wasmtime run app.wasm     # => "17 passed, 0 failed"
```

`-fno-exceptions -fno-rtti` is required: `std::map`/`std::string` throw
paths otherwise reference `__cxa_throw`, unresolved in this link.

This is a research artifact, not part of the bazel build. The real
generator is `celfnc` (m25 §5 / m13 §8.2).
