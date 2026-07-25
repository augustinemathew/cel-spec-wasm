// `cel embed-decls` — embed .idl declaration text into a plugin's
// `cel.fns` custom section.

#include "tools/cel/run_embed_decls.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "abi/plugin_validate.h"
#include "abi/wasm_binary.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"

namespace celwasm::tools::cel {
namespace {

constexpr absl::string_view kCelFnsSection = "cel.fns";
constexpr absl::string_view kErrorPrefix = "cel embed-decls: ";

// Validation 1: the input is a Component-Model component (a core
// module gets a distinct message — the macro's step-3 output is
// already a component; a core module means the wrong artifact was
// fed in).
absl::Status ValidateComponent(absl::Span<const uint8_t> plugin_bytes) {
  return RequireComponentLayer(
      plugin_bytes,
      absl::StrCat(
          kErrorPrefix,
          "--plugin is a core wasm module, not a Component-Model component — "
          "plugins are components; feed the wasm32-wasip2 build output"),
      absl::StrCat(kErrorPrefix,
                   "--plugin is not a wasm binary (bad preamble)"));
}

// Validations 2 + 3: the idl parses (line+col preserved) and every
// decl is `@plugin.`-backed.
absl::Status ValidateIdl(absl::string_view idl_text) {
  absl::StatusOr<FunctionLibrary> lib = ParseCelfnSource(idl_text);
  if (!lib.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat(kErrorPrefix, "--idl: ", lib.status().message()));
  }
  return RequireAllPluginBacked(*lib, kErrorPrefix, "embedded in a plugin");
}

// Validation 4: no pre-existing `cel.fns` section (NotFound is the
// good case; any framing error also rejects).
absl::Status ValidateNoExistingSection(
    absl::Span<const uint8_t> plugin_bytes) {
  const auto section = FindCustomSection(plugin_bytes, kCelFnsSection);
  if (section.ok()) {
    return absl::InvalidArgumentError(absl::StrCat(
        kErrorPrefix,
        "--plugin already carries a cel.fns section; embed-decls must run "
        "on the pre-embed build output"));
  }
  if (!absl::IsNotFound(section.status())) {
    return absl::InvalidArgumentError(
        absl::StrCat(kErrorPrefix, "--plugin is malformed: ",
                     section.status().message()));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<uint8_t>> ReadFileBytes(const std::string& path,
                                                   absl::string_view flag) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat(kErrorPrefix, "cannot open ", flag, " file: ", path));
  }
  std::string buf((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  const auto* data = reinterpret_cast<const uint8_t*>(buf.data());
  return std::vector<uint8_t>(data, data + buf.size());
}

absl::Status WriteFileBytes(const std::string& path,
                            absl::Span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return absl::PermissionDeniedError(
        absl::StrCat(kErrorPrefix, "cannot open --out file: ", path));
  }
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    return absl::DataLossError(
        absl::StrCat(kErrorPrefix, "write failed: ", path));
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<std::vector<uint8_t>> EmbedDecls(
    absl::Span<const uint8_t> plugin_bytes, absl::string_view idl_text) {
  if (absl::Status s = ValidateComponent(plugin_bytes); !s.ok()) return s;
  if (absl::Status s = ValidateIdl(idl_text); !s.ok()) return s;
  if (absl::Status s = ValidateNoExistingSection(plugin_bytes); !s.ok()) {
    return s;
  }
  absl::StatusOr<std::vector<uint8_t>> out = AppendCustomSection(
      plugin_bytes, kCelFnsSection,
      absl::Span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(idl_text.data()), idl_text.size()));
  if (!out.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat(kErrorPrefix, out.status().message()));
  }
  return out;
}

int RunEmbedDecls(const EmbedDeclsOptions& opts) {
  if (opts.plugin_path.empty()) {
    std::cerr << "ERROR: " << kErrorPrefix << "--plugin is required\n";
    return 2;
  }
  if (opts.idl_path.empty()) {
    std::cerr << "ERROR: " << kErrorPrefix << "--idl is required\n";
    return 2;
  }
  if (opts.out_path.empty()) {
    std::cerr << "ERROR: " << kErrorPrefix << "--out is required\n";
    return 2;
  }
  auto plugin_bytes = ReadFileBytes(opts.plugin_path, "--plugin");
  if (!plugin_bytes.ok()) {
    std::cerr << "ERROR: " << plugin_bytes.status().message() << "\n";
    return 1;
  }
  auto idl_bytes = ReadFileBytes(opts.idl_path, "--idl");
  if (!idl_bytes.ok()) {
    std::cerr << "ERROR: " << idl_bytes.status().message() << "\n";
    return 1;
  }
  const absl::string_view idl_text(
      reinterpret_cast<const char*>(idl_bytes->data()), idl_bytes->size());
  auto out = EmbedDecls(*plugin_bytes, idl_text);
  if (!out.ok()) {
    std::cerr << "ERROR: " << out.status().message() << "\n";
    return 1;
  }
  if (absl::Status s = WriteFileBytes(opts.out_path, *out); !s.ok()) {
    std::cerr << "ERROR: " << s.message() << "\n";
    return 1;
  }
  return 0;
}

}  // namespace celwasm::tools::cel
