#include "abi/plugin_validate.h"

#include <cstdint>
#include <optional>

#include "abi/wasm_binary.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"

namespace celwasm {

// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::Status RequireComponentLayer(absl::Span<const uint8_t> bytes,
                                   absl::string_view core_module_message,
                                   absl::string_view bad_preamble_message) {
  const std::optional<WasmLayer> layer = ClassifyWasmBinary(bytes);
  if (layer == WasmLayer::kCoreModule) {
    return absl::InvalidArgumentError(core_module_message);
  }
  if (layer != WasmLayer::kComponent) {
    return absl::InvalidArgumentError(bad_preamble_message);
  }
  return absl::OkStatus();
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
absl::Status RequireAllPluginBacked(const FunctionLibrary& lib,
                                    absl::string_view prefix,
                                    absl::string_view clause) {
  for (const CelfnDecl& d : lib.decls()) {
    if (d.backend != CelfnDecl::Backend::kPlugin) {
      return absl::InvalidArgumentError(absl::StrCat(
          prefix, "decl `", d.fn_name, "` is ", BackendPrefix(d.backend),
          "-backed — every declaration ", clause, " must be @plugin."));
    }
  }
  return absl::OkStatus();
}

}  // namespace celwasm
