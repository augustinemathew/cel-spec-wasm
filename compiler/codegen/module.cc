#include "compiler/codegen/module.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "binaryen-c.h"

namespace celwasm {
namespace {

// Binaryen's C API uses `BinaryenIndex` (uint32) for table / memory
// maxima and encodes "no maximum" as (uint32_t)-1.  This constant
// documents the convention at the one place where we rely on it.
constexpr BinaryenIndex kNoMaximum =
    std::numeric_limits<BinaryenIndex>::max();

// Convenience: many Binaryen entry points take `const char*` and we
// hold `absl::string_view`.  Passing `.data()` directly is unsafe
// because string_view is not null-terminated; copy through std::string
// instead.
std::string Cstr(absl::string_view s) { return std::string(s); }

}  // namespace

BinaryenType TupleType(absl::Span<const BinaryenType> parts) {
  if (parts.empty()) return BinaryenTypeNone();
  if (parts.size() == 1) return parts[0];
  // BinaryenTypeCreate takes a non-const pointer but does not mutate;
  // copy to a local buffer to keep the span's constness honest.
  std::vector<BinaryenType> buf(parts.begin(), parts.end());
  return BinaryenTypeCreate(buf.data(),
                            static_cast<BinaryenIndex>(buf.size()));
}

WasmModule::WasmModule() : module_(BinaryenModuleCreate()) {
  // The design (see doc/wasm-compiler-design.md §1) requires the
  // reference-types and multi-value proposals.  Binaryen's validator
  // defaults to MVP-only features, so turn the relevant ones on up
  // front — otherwise externref tables and host imports that return
  // tuples would fail BinaryenModuleValidate.  GC is enabled only to
  // satisfy Binaryen's "tables with initializer expressions require
  // --enable-gc" check; we never emit GC instructions.
  BinaryenModuleSetFeatures(module_,
                            BinaryenFeatureReferenceTypes() |
                                BinaryenFeatureMultivalue() |
                                BinaryenFeatureBulkMemory() |
                                BinaryenFeatureSignExt() |
                                BinaryenFeatureMutableGlobals() |
                                BinaryenFeatureGC());
}

WasmModule::~WasmModule() {
  if (module_ != nullptr) BinaryenModuleDispose(module_);
}

WasmModule::WasmModule(WasmModule&& other) noexcept : module_(other.module_) {
  other.module_ = nullptr;
}

WasmModule& WasmModule::operator=(WasmModule&& other) noexcept {
  if (this != &other) {
    if (module_ != nullptr) BinaryenModuleDispose(module_);
    module_ = other.module_;
    other.module_ = nullptr;
  }
  return *this;
}

absl::Status WasmModule::SetMemory(uint32_t initial_pages,
                                   std::optional<uint32_t> max_pages,
                                   absl::string_view export_name) {
  if (BinaryenHasMemory(module_)) {
    return absl::FailedPreconditionError(
        "WasmModule::SetMemory called twice; Binaryen only supports a "
        "single memory per module.");
  }
  if (max_pages.has_value() && *max_pages < initial_pages) {
    return absl::InvalidArgumentError(
        absl::StrCat("memory max (", *max_pages,
                     ") is less than initial (", initial_pages, ")"));
  }
  const std::string export_c(export_name);
  BinaryenSetMemory(module_,
                    static_cast<BinaryenIndex>(initial_pages),
                    max_pages.has_value()
                        ? static_cast<BinaryenIndex>(*max_pages)
                        : kNoMaximum,
                    export_name.empty() ? nullptr : export_c.c_str(),
                    /*segmentNames=*/nullptr,
                    /*segmentDatas=*/nullptr,
                    /*segmentPassives=*/nullptr,
                    /*segmentOffsets=*/nullptr,
                    /*segmentSizes=*/nullptr,
                    /*numSegments=*/0,
                    /*shared=*/false,
                    /*memory64=*/false,
                    /*name=*/"memory");
  return absl::OkStatus();
}

absl::Status WasmModule::AddCelRefsTable(
    absl::string_view name,
    uint32_t initial_slots,
    std::optional<uint32_t> max_slots) {
  if (max_slots.has_value() && *max_slots < initial_slots) {
    return absl::InvalidArgumentError(
        absl::StrCat("table max (", *max_slots,
                     ") is less than initial (", initial_slots, ")"));
  }
  // Binaryen's `BinaryenAddTable` takes an initializer expression that
  // is used for every slot.  `ref.null externref` is the natural
  // all-zeros choice for an externref table.
  BinaryenExpressionRef init =
      BinaryenRefNull(module_, BinaryenTypeExternref());
  const std::string name_c(name);
  BinaryenAddTable(module_,
                   name_c.c_str(),
                   static_cast<BinaryenIndex>(initial_slots),
                   max_slots.has_value()
                       ? static_cast<BinaryenIndex>(*max_slots)
                       : kNoMaximum,
                   BinaryenTypeExternref(),
                   init);
  return absl::OkStatus();
}

void WasmModule::AddFunctionImport(absl::string_view internal_name,
                                   absl::string_view external_module,
                                   absl::string_view external_base,
                                   absl::Span<const BinaryenType> params,
                                   BinaryenType result) {
  const std::string internal_c = Cstr(internal_name);
  const std::string ext_mod_c = Cstr(external_module);
  const std::string ext_base_c = Cstr(external_base);
  BinaryenAddFunctionImport(module_,
                            internal_c.c_str(),
                            ext_mod_c.c_str(),
                            ext_base_c.c_str(),
                            TupleType(params),
                            result);
}

void WasmModule::AddFunction(absl::string_view internal_name,
                             absl::Span<const BinaryenType> params,
                             BinaryenType result,
                             absl::Span<const BinaryenType> local_types,
                             BinaryenExpressionRef body) {
  const std::string internal_c = Cstr(internal_name);
  std::vector<BinaryenType> var_buf(local_types.begin(), local_types.end());
  BinaryenAddFunction(module_,
                      internal_c.c_str(),
                      TupleType(params),
                      result,
                      var_buf.empty() ? nullptr : var_buf.data(),
                      static_cast<BinaryenIndex>(var_buf.size()),
                      body);
}

void WasmModule::ExportFunction(absl::string_view internal_name,
                                absl::string_view external_name) {
  const std::string a = Cstr(internal_name);
  const std::string b = Cstr(external_name);
  BinaryenAddFunctionExport(module_, a.c_str(), b.c_str());
}

void WasmModule::ExportMemory(absl::string_view internal_name,
                              absl::string_view external_name) {
  const std::string a = Cstr(internal_name);
  const std::string b = Cstr(external_name);
  BinaryenAddMemoryExport(module_, a.c_str(), b.c_str());
}

void WasmModule::ExportTable(absl::string_view internal_name,
                             absl::string_view external_name) {
  const std::string a = Cstr(internal_name);
  const std::string b = Cstr(external_name);
  BinaryenAddTableExport(module_, a.c_str(), b.c_str());
}

absl::Status WasmModule::Validate() const {
  if (!BinaryenModuleValidate(module_)) {
    return absl::FailedPreconditionError(
        "BinaryenModuleValidate rejected the module "
        "(see stderr for Binaryen diagnostics)");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<uint8_t>> WasmModule::Serialize() const {
  BinaryenModuleAllocateAndWriteResult written =
      BinaryenModuleAllocateAndWrite(module_, /*sourceMapUrl=*/nullptr);
  if (written.binary == nullptr || written.binaryBytes == 0) {
    if (written.binary != nullptr) std::free(written.binary);
    if (written.sourceMap != nullptr) std::free(written.sourceMap);
    return absl::InternalError("BinaryenModuleAllocateAndWrite returned empty");
  }
  const auto* bytes = static_cast<const uint8_t*>(written.binary);
  std::vector<uint8_t> out(bytes, bytes + written.binaryBytes);
  std::free(written.binary);
  if (written.sourceMap != nullptr) std::free(written.sourceMap);
  return out;
}

void WasmModule::PrintToStderr() const { BinaryenModulePrint(module_); }

}  // namespace celwasm
