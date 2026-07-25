#include "abi/plugin.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "abi/internal/sha256.h"
#include "abi/wasm_binary.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"

namespace celwasm {
namespace {

constexpr absl::string_view kCelFnsSection = "cel.fns";

// True iff `bytes` is well-formed UTF-8 (RFC 3629): rejects stray /
// missing continuation bytes, overlong encodings, surrogates, and
// code points past U+10FFFF.
bool IsValidUtf8(absl::Span<const uint8_t> bytes) {
  size_t i = 0;
  while (i < bytes.size()) {
    const uint8_t b0 = bytes[i];
    if (b0 < 0x80) {
      ++i;
      continue;
    }
    size_t cont = 0;   // continuation-byte count
    uint32_t cp = 0;   // decoded code point
    uint32_t min = 0;  // smallest code point this length may encode
    if ((b0 & 0xE0) == 0xC0) {
      cont = 1, cp = b0 & 0x1FU, min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
      cont = 2, cp = b0 & 0x0FU, min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
      cont = 3, cp = b0 & 0x07U, min = 0x10000;
    } else {
      return false;  // stray continuation byte or invalid lead
    }
    if (i + cont >= bytes.size()) return false;  // truncated sequence
    for (size_t k = 1; k <= cont; ++k) {
      const uint8_t b = bytes[i + k];
      if ((b & 0xC0) != 0x80) return false;
      cp = (cp << 6U) | (b & 0x3FU);
    }
    const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    if (cp < min || cp > 0x10FFFF || surrogate) return false;
    i += cont + 1;
  }
  return true;
}

// The `@<backend>.` spelling of a decl's backend, for error messages.
absl::string_view BackendPrefix(CelfnDecl::Backend backend) {
  switch (backend) {
    case CelfnDecl::Backend::kHost:
      return "@host.";
    case CelfnDecl::Backend::kCelDefined:
      return "@native.";
    case CelfnDecl::Backend::kPlugin:
      return "@plugin.";
  }
  ABSL_CHECK(false) << "unknown CelfnDecl::Backend "
                    << static_cast<int>(backend);
  return "";
}

// Classifies the binary and extracts the `cel.fns` payload as
// validated UTF-8 text.
absl::StatusOr<std::string> ExtractCelfnSource(
    absl::Span<const uint8_t> plugin_bytes) {
  const std::optional<WasmLayer> layer = ClassifyWasmBinary(plugin_bytes);
  if (layer == WasmLayer::kCoreModule) {
    return absl::InvalidArgumentError(
        "Plugin::Load: bytes are a core wasm module, not a Component-Model "
        "component — plugins are components; build with cel_wasm_plugin");
  }
  if (layer != WasmLayer::kComponent) {
    return absl::InvalidArgumentError(
        "Plugin::Load: not a wasm binary (bad preamble)");
  }
  const auto section = FindCustomSection(plugin_bytes, kCelFnsSection);
  if (absl::IsNotFound(section.status())) {
    return absl::InvalidArgumentError(
        "Plugin::Load: no cel.fns section — rebuild with cel_wasm_plugin or "
        "run `cel embed-decls`");
  }
  if (!section.ok()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Plugin::Load: malformed plugin binary: ",
        section.status().message()));
  }
  if (!IsValidUtf8(*section)) {
    return absl::InvalidArgumentError(
        "Plugin::Load: cel.fns section payload is not valid UTF-8");
  }
  return std::string(reinterpret_cast<const char*>(section->data()),
                     section->size());
}

// Every decl must be `@plugin.`-backed, and there must be at least
// one.
absl::Status ValidatePluginDecls(const FunctionLibrary& lib) {
  if (lib.decls().empty()) {
    return absl::InvalidArgumentError(
        "Plugin::Load: cel.fns declares no functions — a plugin must "
        "declare at least one @plugin. function");
  }
  for (const CelfnDecl& d : lib.decls()) {
    if (d.backend != CelfnDecl::Backend::kPlugin) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Plugin::Load: decl `", d.fn_name, "` is ",
          BackendPrefix(d.backend),
          "-backed — every declaration in a plugin's cel.fns must be "
          "@plugin."));
    }
  }
  return absl::OkStatus();
}

// Rebuilds the parsed library with the derived WIT interface stamped
// on it (ParseCelfnSource carries no wit_interface; the derivation is
// always `cel:<module>/fns@0.1.0`, fallback module `customfn`).
absl::StatusOr<FunctionLibrary> RebuildWithWitInterface(
    const FunctionLibrary& parsed) {
  FunctionLibrary::Builder b;
  if (!parsed.module_name().empty()) {
    b.SetModuleName(parsed.module_name());
  }
  b.SetWitInterface(DeriveWitInterface(parsed.module_name()));
  for (const CelfnDecl& d : parsed.decls()) {
    b.AddPlugin(d.fn_name, d.return_type, d.params);
  }
  return std::move(b).Build();
}

std::array<uint8_t, kSha256DigestSize> HashPlugin(
    absl::Span<const uint8_t> bytes, absl::string_view source) {
  std::vector<uint8_t> buf;
  buf.reserve(bytes.size() + source.size());
  buf.insert(buf.end(), bytes.begin(), bytes.end());
  const auto* src = reinterpret_cast<const uint8_t*>(source.data());
  buf.insert(buf.end(), src, src + source.size());
  return Sha256(buf);
}

}  // namespace

absl::StatusOr<Plugin> Plugin::Load(absl::Span<const uint8_t> plugin_bytes) {
  if (plugin_bytes.empty()) {
    return absl::InvalidArgumentError(
        "Plugin::Load: plugin bytes are empty");
  }
  absl::StatusOr<std::string> source = ExtractCelfnSource(plugin_bytes);
  if (!source.ok()) return source.status();

  absl::StatusOr<FunctionLibrary> parsed = ParseCelfnSource(*source);
  if (!parsed.ok()) {
    // ParseCelfnSource reports `line N:C` positions — preserve them.
    return absl::InvalidArgumentError(absl::StrCat(
        "Plugin::Load: cel.fns declaration text: ",
        parsed.status().message()));
  }
  if (absl::Status s = ValidatePluginDecls(*parsed); !s.ok()) return s;

  absl::StatusOr<FunctionLibrary> lib = RebuildWithWitInterface(*parsed);
  if (!lib.ok()) return lib.status();

  Plugin plugin;
  plugin.bytes_.assign(plugin_bytes.begin(), plugin_bytes.end());
  plugin.celfn_source_ = *std::move(source);
  plugin.library_ = *std::move(lib);
  plugin.hash_ = HashPlugin(plugin.bytes_, plugin.celfn_source_);
  return plugin;
}

std::string Plugin::hash_hex() const {
  return absl::BytesToHexString(absl::string_view(
      reinterpret_cast<const char*>(hash_.data()), hash_.size()));
}

}  // namespace celwasm
