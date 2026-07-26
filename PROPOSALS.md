# PROPOSALS — changes requiring API/ABI/infrastructure decisions

Status: live — created 2026-06-09 by the professionalization pass.
Each entry is logged here instead of being made, because it changes a
public signature, observable behavior, or repo infrastructure. Owner
decides; nothing here blocks the cleanup queue in CLEANUP_PLAN.md.

## API changes

1. **`Engine::AddPlugin(bytes, FunctionLibrary)` takes an
   internal-visibility type** (cleanup-backlog #32). A public method
   whose parameter type (`//compiler/celfn:function_library`,
   `//:internal`) external code cannot legally depend on. Options:
   promote `function_library` to public (smallest), or add a public
   declaration-string overload mirroring `BindFunction`. Affected:
   `eval/engine.h`, root `BUILD.bazel` examples carve-out.
2. **Host-fn `ErrorPayload.message` does not cross the wasm boundary**
   (cleanup-backlog #31). `Value::Error({code, "msg"})` from a callback
   decodes as `"runtime error code N"`. Behavior change: wire the
   message through the error slot (ABI addition) or document the
   code-only contract permanently. Affected: `cel_host_error.*`,
   runtime error encoding, `examples/08` (currently asserts the honest
   broken behavior).
3. **`HostMapView` has no key-enumeration API** (Size/Get/ContainsKey
   only). Add `Keys()` or an iterator; docs currently state the
   limitation honestly. Affected: `eval/host_call_context.h`.
4. **No `Value::AsProto<T>()`** — typed proto extraction requires
   `MessageBacking()` + an internal header. Add a public typed
   accessor. Affected: `eval/value.h`.
5. **Graceful arena OOM** (cleanup-backlog #16, P0). `arena_alloc`
   exhaustion panics the wasmtime store (host-visible crash) instead
   of returning a resource-exhausted error. Behavioral fix + Compile/
   Plan-time capacity pre-flight. Affected: `runtime/cel_arena.c`, all
   arena consumers, `eval/instance.cc` trap mapping.

## Infrastructure

6. **CI**: no `.github/workflows/`; `cloudbuild.yaml` builds only (no
   tests, no `fetch_cel_cpp.sh` documentation of the pre-fetch
   assumption). Add: build + `bazel test //...` + manual-target list +
   conformance monotonic gate + `examples_smoke_test` +
   `check_doc_drift.sh`, Linux + macOS, badge in README.
7. **Versioning**: `MODULE.bazel` `module()` has no `version`; no git
   tags; no CHANGELOG.md. Adopters cannot pin. Propose `0.x` semver +
   CHANGELOG seeded from the milestone history.
8. **Root governance files**: CONTRIBUTING.md (re-home or stub-link
   doc/contributing.md), SECURITY.md (link
   doc/user-guide/security-model.md + a disclosure contact).
9. **Generated API reference**: no Doxygen config. After the Phase 2
   header pass the comments will be reference-grade; wire
   Doxygen (or standardese-lite) into CI.
10. **`cel run` subcommand** — the CLI can compile a `.wasm` it cannot
    execute; design exists (cel-cli-design.md). Closes the
    compile-once-run-anywhere proof loop.

## Style-guide conflicts requiring signature changes

(None found yet — naming, explicit ctors, and pointer-vs-reference
conventions already conform. Entries will be added as the P2/P3 header
passes uncover them.)
