// Import-list inspection over a wasmtime-compiled module.
//
// `Engine::Plan` routes a Program between static and dynamic linking
// based on whether the compiled expr module imports anything from the
// `"cel"` namespace — this header carries that single predicate so it
// can be unit-tested in isolation from the Engine.

#ifndef CELWASM_EVAL_INTERNAL_MODULE_IMPORTS_H_
#define CELWASM_EVAL_INTERNAL_MODULE_IMPORTS_H_

#include "wasmtime.h"

namespace celwasm {

// Does the wasmtime-compiled Program declare any import from module
// `"cel"`?  Static-mode Programs (produced by `Compiler::Compile`
// with `LinkMode::kStatic`) carry runtime helpers as defined functions
// and import nothing from `"cel"`; dynamic-mode Programs import every
// helper plus `cel.memory` + `cel.arena_reset`.  Used by `Engine::Plan`
// to route: skip the standalone `InstantiateRuntime` for static-mode
// Programs.
//
// Implementation note: we ask wasmtime for the import list of the
// already-compiled module rather than walking the raw wasm bytes
// ourselves.  Saves an 85-line hand-rolled section walker that would
// otherwise duplicate wasm-format parsing logic the engine already
// links against.
bool ModuleImportsCelNamespace(wasmtime_module_t* module);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_MODULE_IMPORTS_H_
