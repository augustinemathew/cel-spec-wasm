#include "compiler/codegen/module.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
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

// Binaryen's C API encodes "no maximum" for memory / table bounds as
// (uint32_t)-1.  This constant names the convention.
constexpr BinaryenIndex kNoMaximum = std::numeric_limits<BinaryenIndex>::max();

// `absl::string_view` is not null-terminated; every Binaryen entry
// point that takes `const char*` needs a freshly-constructed
// `std::string` that outlives the call.
std::string Cstr(absl::string_view s) {
  return std::string(s);
}

}  // namespace

BinaryenType TupleType(absl::Span<const BinaryenType> parts) {
  if (parts.empty()) return BinaryenTypeNone();
  if (parts.size() == 1) return parts[0];
  // `BinaryenTypeCreate` takes a non-const pointer but does not mutate.
  // Copy to a local buffer so the span's constness is honest.
  std::vector<BinaryenType> buf(parts.begin(), parts.end());
  return BinaryenTypeCreate(buf.data(), static_cast<BinaryenIndex>(buf.size()));
}

WasmModule::WasmModule() : module_(BinaryenModuleCreate()) {
  // Binaryen's validator defaults to MVP-only.  The design requires
  // multi-value (tuple returns for host trampolines) and
  // reference-types (externref table for message handles, M3+).
  // Phase C adds Atomics (threads): the runtime imports a shared
  // memory; importing a shared memory requires the threads feature
  // to be on, even if the expr module itself emits no atomic ops.
  BinaryenModuleSetFeatures(
      module_, BinaryenFeatureReferenceTypes() | BinaryenFeatureMultivalue() |
                   BinaryenFeatureBulkMemory() | BinaryenFeatureSignExt() |
                   BinaryenFeatureMutableGlobals() | BinaryenFeatureGC() |
                   BinaryenFeatureAtomics());
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
                                   absl::string_view export_name,
                                   absl::Span<const DataSegment> segments) {
  if (BinaryenHasMemory(module_)) {
    return absl::FailedPreconditionError(
        "WasmModule::SetMemory called after memory was already set; "
        "Binaryen supports a single memory per module.");
  }
  if (max_pages.has_value() && *max_pages < initial_pages) {
    return absl::InvalidArgumentError(absl::StrCat("memory max (", *max_pages,
                                                   ") is less than initial (",
                                                   initial_pages, ")"));
  }

  // Pack segment info into the parallel-array shape Binaryen wants.  We
  // keep the owned storage alive for the duration of the BinaryenSetMemory
  // call; after that Binaryen has copied what it needs.  Use a plain
  // `uint8_t` vector for the `bool*` arg because `std::vector<bool>` is
  // not guaranteed array-contiguous.
  std::vector<const char*> seg_datas;
  std::vector<uint8_t> seg_passives;
  std::vector<BinaryenExpressionRef> seg_offsets;
  std::vector<BinaryenIndex> seg_sizes;
  seg_datas.reserve(segments.size());
  seg_passives.reserve(segments.size());
  seg_offsets.reserve(segments.size());
  seg_sizes.reserve(segments.size());
  for (const DataSegment& seg : segments) {
    seg_datas.push_back(reinterpret_cast<const char*>(seg.bytes.data()));
    seg_passives.push_back(0);
    seg_offsets.push_back(BinaryenConst(
        module_, BinaryenLiteralInt32(static_cast<int32_t>(seg.offset))));
    seg_sizes.push_back(static_cast<BinaryenIndex>(seg.bytes.size()));
  }

  const std::string export_c(export_name);
  BinaryenSetMemory(
      module_, static_cast<BinaryenIndex>(initial_pages),
      max_pages.has_value() ? static_cast<BinaryenIndex>(*max_pages)
                            : kNoMaximum,
      export_name.empty() ? nullptr : export_c.c_str(),
      /*segmentNames=*/nullptr, seg_datas.empty() ? nullptr : seg_datas.data(),
      seg_passives.empty() ? nullptr
                           : reinterpret_cast<bool*>(seg_passives.data()),
      seg_offsets.empty() ? nullptr : seg_offsets.data(),
      seg_sizes.empty() ? nullptr : seg_sizes.data(),
      static_cast<BinaryenIndex>(segments.size()),
      /*shared=*/false,
      /*memory64=*/false,
      /*name=*/"memory");
  return absl::OkStatus();
}

absl::Status WasmModule::AddMemoryImport(absl::string_view external_module,
                                         absl::string_view external_base,
                                         uint32_t initial_pages,
                                         std::optional<uint32_t> max_pages,
                                         absl::Span<const DataSegment> segments,
                                         bool shared) {
  if (BinaryenHasMemory(module_)) {
    return absl::FailedPreconditionError(
        "WasmModule::AddMemoryImport: module already has a memory.");
  }
  if (max_pages.has_value() && *max_pages < initial_pages) {
    return absl::InvalidArgumentError(
        absl::StrCat("memory import max (", *max_pages,
                     ") is less than initial (", initial_pages, ")"));
  }
  if (shared && !max_pages.has_value()) {
    return absl::InvalidArgumentError(
        "AddMemoryImport: shared memory must specify max_pages.");
  }
  // Pack segment info into the parallel-array shape Binaryen wants —
  // identical to the SetMemory path; we keep the owned storage alive
  // for the duration of the BinaryenSetMemory call.  Same `std::vector
  // <uint8_t>` trick for the `bool*` passives arg (vector<bool> isn't
  // contiguous).
  std::vector<const char*> seg_datas;
  std::vector<uint8_t> seg_passives;
  std::vector<BinaryenExpressionRef> seg_offsets;
  std::vector<BinaryenIndex> seg_sizes;
  seg_datas.reserve(segments.size());
  seg_passives.reserve(segments.size());
  seg_offsets.reserve(segments.size());
  seg_sizes.reserve(segments.size());
  for (const DataSegment& seg : segments) {
    seg_datas.push_back(reinterpret_cast<const char*>(seg.bytes.data()));
    seg_passives.push_back(0);
    seg_offsets.push_back(BinaryenConst(
        module_, BinaryenLiteralInt32(static_cast<int32_t>(seg.offset))));
    seg_sizes.push_back(static_cast<BinaryenIndex>(seg.bytes.size()));
  }

  // `BinaryenSetMemory` wipes any existing import info on the memory
  // with the matching internal name, so install the memory shape first
  // and then mark it as imported.  Reversing these two calls silently
  // emits a non-imported memory (confirmed against binaryen 129).
  const std::string ext_mod_c = Cstr(external_module);
  const std::string ext_base_c = Cstr(external_base);
  BinaryenSetMemory(
      module_, static_cast<BinaryenIndex>(initial_pages),
      max_pages.has_value() ? static_cast<BinaryenIndex>(*max_pages)
                            : kNoMaximum,
      /*exportName=*/nullptr,
      /*segmentNames=*/nullptr, seg_datas.empty() ? nullptr : seg_datas.data(),
      seg_passives.empty() ? nullptr
                           : reinterpret_cast<bool*>(seg_passives.data()),
      seg_offsets.empty() ? nullptr : seg_offsets.data(),
      seg_sizes.empty() ? nullptr : seg_sizes.data(),
      static_cast<BinaryenIndex>(segments.size()), shared,
      /*memory64=*/false,
      /*name=*/"memory");
  BinaryenAddMemoryImport(module_,
                          /*internalName=*/"memory", ext_mod_c.c_str(),
                          ext_base_c.c_str(),
                          /*shared=*/shared ? 1 : 0);
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
  BinaryenAddFunctionImport(module_, internal_c.c_str(), ext_mod_c.c_str(),
                            ext_base_c.c_str(), TupleType(params), result);
}

void WasmModule::AddFunction(absl::string_view internal_name,
                             absl::Span<const BinaryenType> params,
                             BinaryenType result,
                             absl::Span<const BinaryenType> local_types,
                             BinaryenExpressionRef body) {
  const std::string internal_c = Cstr(internal_name);
  std::vector<BinaryenType> var_buf(local_types.begin(), local_types.end());
  BinaryenAddFunction(module_, internal_c.c_str(), TupleType(params), result,
                      var_buf.empty() ? nullptr : var_buf.data(),
                      static_cast<BinaryenIndex>(var_buf.size()), body);
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

void WasmModule::AddCustomSection(absl::string_view name,
                                  absl::Span<const uint8_t> bytes) {
  const std::string name_c = Cstr(name);
  BinaryenAddCustomSection(module_, name_c.c_str(),
                           reinterpret_cast<const char*>(bytes.data()),
                           static_cast<BinaryenIndex>(bytes.size()));
}

absl::Status WasmModule::Validate() const {
  if (!BinaryenModuleValidate(module_)) {
    return absl::FailedPreconditionError(
        "BinaryenModuleValidate rejected the module "
        "(see stderr for Binaryen diagnostics)");
  }
  return absl::OkStatus();
}

absl::Status WasmModule::Optimize(int level) {
  if (level < 0 || level > 3) {
    return absl::InvalidArgumentError(
        absl::StrCat("WasmModule::Optimize: level out of range; got ", level,
                     ", want 0..3"));
  }
  if (level == 0) {
    // Caller asked for no-op explicitly; preserve byte-for-byte output
    // (existing codegen golden tests depend on level-0 being a no-op).
    return absl::OkStatus();
  }
  // ShrinkLevel = 0: perf-only, do not trade speed for size.  The expr
  // module is Cranelift's input — its serialized size is irrelevant in
  // a JIT-instantiate flow; what matters is the native code Cranelift
  // produces.  Smaller-but-slower wasm is the wrong trade-off here.
  BinaryenSetOptimizeLevel(level);
  BinaryenSetShrinkLevel(0);
  BinaryenModuleOptimize(module_);
  return absl::OkStatus();
}

absl::StatusOr<std::vector<uint8_t>> WasmModule::Serialize() const {
  BinaryenModuleAllocateAndWriteResult written =
      BinaryenModuleAllocateAndWrite(module_, /*sourceMapUrl=*/nullptr);
  // Binaryen hands us malloc'd buffers we must free ourselves; wrap them
  // in unique_ptrs so the free happens automatically on every exit path.
  using MallocPtr = std::unique_ptr<void, decltype(&std::free)>;
  MallocPtr binary(written.binary, &std::free);
  MallocPtr source_map(written.sourceMap, &std::free);
  if (written.binary == nullptr || written.binaryBytes == 0) {
    return absl::InternalError("BinaryenModuleAllocateAndWrite returned empty");
  }
  const auto* bytes = static_cast<const uint8_t*>(written.binary);
  return std::vector<uint8_t>(bytes, bytes + written.binaryBytes);
}

void WasmModule::PrintToStderr() const {
  BinaryenModulePrint(module_);
}

}  // namespace celwasm
