// Header-only fakes shared by cel_host's Layer-2 unit tests
// (cel_host_test, cel_map_lookup_impl_test, cel_list_at_impl_test,
// host_list_test, proto_list_test).  Three abstract Layer-2 inputs —
// MemoryView / ExternrefTable / ArenaAllocator — get one minimal
// vector-backed concrete each, plus a `Layer2Fixture` that bundles
// them with a default `TrampolineContext`.
//
// Centralising these fakes keeps the externref-namespace surface
// (Intern/Lookup × Message/Map/List) in one place; previously each
// test re-implemented the same vector + slot-0 sentinel pattern and
// drifted independently when new namespaces (M3 maps, M4 lists)
// landed.

#ifndef CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_TEST_FAKES_H_
#define CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_TEST_FAKES_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/internal/cel_host.h"
#include "compiler_v2/runtime/cel_data.h"

namespace celwasm {
namespace test {

// 64 KiB byte-buffer; CelValue read/write copies the 24-byte struct
// at the requested offset; ReadSpan returns a view into the buffer.
class FakeMemoryView final : public MemoryView {
 public:
  explicit FakeMemoryView(size_t size = size_t{64u} * 1024u)
      : mem_(size, 0) {}

  CelValue ReadCelValue(uint32_t offset) const override {
    CelValue cv{};
    std::memcpy(&cv, mem_.data() + offset, sizeof(cv));
    return cv;
  }
  void WriteCelValue(uint32_t offset, const CelValue& v) override {
    std::memcpy(mem_.data() + offset, &v, sizeof(v));
  }
  void WriteU32(uint32_t offset, uint32_t value) override {
    std::memcpy(mem_.data() + offset, &value, sizeof(value));
  }
  absl::string_view ReadSpan(uint32_t ptr, uint32_t len) const override {
    return {reinterpret_cast<const char*>(mem_.data() + ptr), len};
  }

  uint8_t* absl_nonnull data() { return mem_.data(); }

  // Convenience: stage a CelValue at a slot, return the slot offset.
  uint32_t Place(uint32_t slot, const CelValue& v) {
    WriteCelValue(slot, v);
    return slot;
  }

 private:
  std::vector<uint8_t> mem_;
};

// Vector-backed ExternrefTable with three independent namespaces
// (messages / maps / lists), each with a slot-0 sentinel.
class FakeExternrefTable final : public ExternrefTable {
 public:
  FakeExternrefTable() {
    msgs_.push_back(nullptr);
    maps_.push_back(nullptr);
    lists_.push_back(nullptr);
  }

  uint32_t Intern(std::shared_ptr<const HostMessageBacking> b) override {
    msgs_.push_back(std::move(b));
    return static_cast<uint32_t>(msgs_.size() - 1);
  }
  const HostMessageBacking* absl_nullable Lookup(uint32_t slot) const override {
    return slot < msgs_.size() ? msgs_[slot].get() : nullptr;
  }

  uint32_t InternMap(std::shared_ptr<const HostMapBacking> b) override {
    maps_.push_back(std::move(b));
    return static_cast<uint32_t>(maps_.size() - 1);
  }
  const HostMapBacking* absl_nullable LookupMap(uint32_t slot) const override {
    return slot < maps_.size() ? maps_[slot].get() : nullptr;
  }

  uint32_t InternList(std::shared_ptr<const HostListBacking> b) override {
    lists_.push_back(std::move(b));
    return static_cast<uint32_t>(lists_.size() - 1);
  }
  const HostListBacking* absl_nullable LookupList(
      uint32_t slot) const override {
    return slot < lists_.size() ? lists_[slot].get() : nullptr;
  }

  void Reset() override {
    msgs_.clear();
    msgs_.push_back(nullptr);
    maps_.clear();
    maps_.push_back(nullptr);
    lists_.clear();
    lists_.push_back(nullptr);
  }

 private:
  std::vector<std::shared_ptr<const HostMessageBacking>> msgs_;
  std::vector<std::shared_ptr<const HostMapBacking>> maps_;
  std::vector<std::shared_ptr<const HostListBacking>> lists_;
};

// Bump allocator over a region inside a FakeMemoryView, sized at
// construction.  `base_offset` is the wasm-side offset that
// `out_offset` reflects; `cursor_` advances 8-byte-aligned to mirror
// the production arena's behaviour.
class FakeArenaAllocator final : public ArenaAllocator {
 public:
  FakeArenaAllocator(FakeMemoryView* absl_nonnull mem, uint32_t base_offset,
                     size_t capacity)
      : mem_(mem), base_offset_(base_offset), capacity_(capacity) {}

  uint8_t* absl_nullable Alloc(size_t len,
                               uint32_t* absl_nonnull out_offset) override {
    const size_t need = (len + 7u) & ~size_t{7u};
    if (cursor_ + need > capacity_) return nullptr;
    *out_offset = base_offset_ + static_cast<uint32_t>(cursor_);
    uint8_t* dst = mem_->data() + *out_offset;
    cursor_ += need;
    return dst;
  }

 private:
  FakeMemoryView* absl_nonnull mem_;
  uint32_t base_offset_;
  size_t capacity_;
  size_t cursor_ = 0;
};

}  // namespace test
}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_TEST_FAKES_H_
