// m24 §13 open question: "Component API in eval/: wasmtime's component
// host API is a new eval dependency; verify the vendored build exposes
// it (the C API's component surface is thinner than Rust's — may force
// a shim)."
//
// This probe settles the question definitively by exercising every
// component-API entry point Engine::AddComponent (m24 §3.5) will need.
// Two layers of risk to settle:
//
//   1. **Compile**: the vendored `wasmtime/component.h` and its subtree
//      gate every typedef and function under
//      `#ifdef WASMTIME_FEATURE_COMPONENT_MODEL`.  The vendored
//      `conf.h` does NOT define this macro (verified — see the
//      output of `grep -c WASMTIME_FEATURE_COMPONENT_MODEL conf.h`).
//      So a probe `#include`ing the component headers without
//      force-defining the macro sees zero declarations.  The BUILD
//      rule below sets `-DWASMTIME_FEATURE_COMPONENT_MODEL` to force
//      the declarations visible at preprocess time.
//
//   2. **Link**: the static archive may not contain the symbol bodies
//      if the wasmtime Rust→C library was built without the
//      `component-model` cargo feature.  The decisive test is whether
//      `wasmtime_component_new` and friends resolve at link time.
//      If link succeeds, the archive carries the bodies — m24 §3.5
//      can be written natively against the C API, NO Rust shim
//      required.  If link fails with undefined references, the
//      archive is feature-stripped and we need either a different
//      vendored build or a Rust shim crate that calls the
//      `wasmtime::component::*` Rust API and exposes a small C
//      surface.
//
// This is a CLAUDE.md "throwaway probe" — disposable; kept while m24
// is in flight, deleted at milestone closeout.  The fact it confirmed
// (and the file:line it confirmed against) gets recorded in m24's
// design doc as a dated callout.
//
// Run:
//
//   bazel run //eval/probes/m24:wasmtime_component_api_probe
//
// Expected stdout on success (every symbol resolves):
//
//   probe[m24]: wasmtime_component_api: ALL SYMBOLS RESOLVED
//   probe[m24]: wasmtime_component_new(malformed bytes) errored as expected
//   probe[m24]: -> Engine::AddComponent (m24 §3.5) can be written natively
//
// Expected build failure on link-time miss (the diagnostic that
// answers the §13 question the other way):
//
//   ld: Undefined symbols:
//     _wasmtime_component_new, referenced from: ...
//   probe[m24]: archive is component-feature-stripped; Rust shim required.

#include <cstdint>
#include <cstdio>

#include "wasm.h"
#include "wasmtime/component.h"
#include "wasmtime/error.h"

extern "C" {

// Touch every component-API entry point Engine::AddComponent will use,
// in the order the design (m24 §3.5) calls them, with arguments shaped
// so the call body either succeeds-trivially or returns a well-formed
// error.  We DO NOT need to feed a real component — we only need each
// symbol to resolve at link time and execute one step at runtime.

int ProbeWasmtimeComponentApi(void) {  // NOLINT(misc-use-internal-linkage)
  wasm_engine_t* engine = wasm_engine_new();
  if (engine == nullptr) {
    std::fputs("probe[m24]: wasm_engine_new returned null\n", stderr);
    return 1;
  }

  // 1. wasmtime_component_new — parse component bytes.  Feeding 4
  // garbage bytes triggers a parse error; the symbol resolution is
  // what matters.  A successful link + non-null error returned
  // proves the body is present and reachable.
  const std::uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef};
  wasmtime_component_t* component = nullptr;
  wasmtime_error_t* err = wasmtime_component_new(
      engine, garbage, sizeof(garbage), &component);
  if (err == nullptr) {
    std::fputs(
        "probe[m24]: wasmtime_component_new(garbage) unexpectedly succeeded\n",
        stderr);
    if (component != nullptr) wasmtime_component_delete(component);
    wasm_engine_delete(engine);
    return 1;
  }
  // Free the error — we just needed to confirm the call returned one.
  wasmtime_error_delete(err);
  std::fputs(
      "probe[m24]: wasmtime_component_new(malformed bytes) errored as "
      "expected\n",
      stdout);

  // 2. wasmtime_component_linker_new — instantiation-side linker.
  // Constructible without a real component.  This proves the second
  // load-bearing entry point links.
  wasmtime_component_linker_t* linker = wasmtime_component_linker_new(engine);
  if (linker == nullptr) {
    std::fputs("probe[m24]: wasmtime_component_linker_new returned null\n",
               stderr);
    wasm_engine_delete(engine);
    return 1;
  }
  wasmtime_component_linker_delete(linker);

  // We do NOT exercise wasmtime_component_linker_instantiate,
  // wasmtime_component_instance_get_func, or wasmtime_component_func_call
  // here — those need a real instantiated component.  But link-time
  // resolution is settled by referencing them (the addresses below
  // force the linker to keep the symbols live).  The volatile sink
  // prevents -O3 from dead-stripping the references.
  volatile void* sink = nullptr;
  sink = (void*)&wasmtime_component_linker_instantiate;
  sink = (void*)&wasmtime_component_instance_get_func;
  sink = (void*)&wasmtime_component_func_call;
  sink = (void*)&wasmtime_component_linker_instance_add_func;
  (void)sink;

  wasm_engine_delete(engine);

  std::fputs("probe[m24]: wasmtime_component_api: ALL SYMBOLS RESOLVED\n",
             stdout);
  std::fputs(
      "probe[m24]: -> Engine::AddComponent (m24 §3.5) can be written "
      "natively, no Rust shim required.\n",
      stdout);
  return 0;
}

}  // extern "C"

int main(void) {
  return ProbeWasmtimeComponentApi();
}
