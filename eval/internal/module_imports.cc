#include "eval/internal/module_imports.h"

#include <cstddef>
#include <cstring>

#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

bool ModuleImportsCelNamespace(wasmtime_module_t* module) {
  wasm_importtype_vec_t imports;
  wasmtime_module_imports(module, &imports);
  bool any_cel = false;
  for (size_t i = 0; i < imports.size; ++i) {
    const wasm_name_t* mod = wasm_importtype_module(imports.data[i]);
    if (mod->size == 3 && std::memcmp(mod->data, "cel", 3) == 0) {
      any_cel = true;
      break;
    }
  }
  wasm_importtype_vec_delete(&imports);
  return any_cel;
}

}  // namespace celwasm
