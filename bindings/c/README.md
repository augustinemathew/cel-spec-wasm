# C bindings — work in progress

**Status: not yet functional.** These headers are the drafted API
contract for the C bindings; the implementation has not landed. Nothing
here compiles into a usable library yet, and the surface may still
change before it ships.

What exists today:

- Header drafts specifying the intended API shape (opaque handles,
  `cel_status*` returns where `NULL` means OK, builder-style
  construction, pointer+length spans).
- The design doc for the API: `doc/implementation-plan/rewrite/m34-c-api.md`.

If you need to embed cel-wasm today, use the C++ API
([user guide](https://augustinemathew.github.io/cel-wasm/user-guide/)) —
`Compiler`/`Program` on the compile side, `Engine`/`Instance`/
`Activation`/`Value` on the eval side.
