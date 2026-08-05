// Shared Layer-1 / Layer-2 test harness for the cel_host family
// test TUs: the non-proto JsonLikeBacking, the Layer2Fixture that
// bundles fake MemoryView / ExternrefTable / ArenaAllocator +
// bindings, and small read-side helpers.  Split out of the former
// monolithic cel_host_test.cc alongside the per-family TU split.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_TEST_HARNESS_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_TEST_HARNESS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "eval/error.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"
#include "shared/type.h"

namespace celwasm::test {

using ::absl_testing::IsOk;

// Dummy — ProtoBacking reads via reflection, not the hint.
inline const celwasm::CelType& IgnoredType() {
  static const auto& kAny = *new celwasm::CelType(celwasm::CelType::Int());
  return kAny;
}

inline const HostMessageBacking* BackingFromValue(const celwasm::Value& v) {
  auto b = v.MessageBacking();
  EXPECT_TRUE(b.ok()) << b.status();
  return b.ok() ? *b : nullptr;
}

// ═══════════ JSON-ish backing (non-proto Layer 1) ═══════════

class JsonLikeBacking : public HostMessageBacking {
 public:
  // `fields_` IS initialized below; pro-type-member-init false-fires on
  // the flat_hash_map member when clang-tidy can't see its default ctor.
  // A redundant `fields_{}` would instead trip readability-redundant-
  // member-init, so suppress here.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  explicit JsonLikeBacking(absl::flat_hash_map<std::string, int64_t> fields)
      : fields_(std::move(fields)) {}

  absl::StatusOr<celwasm::Value> ReadField(
      int, absl::string_view name, const celwasm::CelType&) const override {
    auto it = fields_.find(std::string(name));
    if (it == fields_.end()) {
      return celwasm::Value::Error(celwasm::ErrorPayload{
          celwasm::ErrorCode::kFieldNotFound, std::string(name), 0});
    }
    return celwasm::Value::Int(it->second);
  }

  bool HasField(int, absl::string_view name) const override {
    return fields_.contains(std::string(name));
  }

 private:
  absl::flat_hash_map<std::string, int64_t> fields_;
};

// Fixture bundling mem/refs/alloc/bindings + a `Get`/`Has` helper
// that handles slot wiring.  Tests that exercise the trampoline
// instantiate one and call the helper — boilerplate stays here.
struct Layer2Fixture {
  static constexpr uint32_t kMsgSlot = 16;
  static constexpr uint32_t kOutSlot = 64;
  static constexpr uint32_t kArenaBase = 2048;

  FakeMemoryView mem{4096};
  FakeExternrefTable refs;
  FakeArenaAllocator alloc{&mem, kArenaBase, /*capacity=*/2048};
  std::vector<FieldRefEntry> field_refs{FieldRefEntry{}};  // index 0 sentinel
  std::vector<AttributeEntry> attributes;
  std::vector<celwasm::AttributePattern> unknown_patterns;
  CelHostBindings bindings_scratch;  // outlives TrampolineContext ref

  TrampolineContext Ctx() {
    bindings_scratch = CelHostBindings{absl::MakeConstSpan(field_refs),
                                       absl::MakeConstSpan(attributes),
                                       absl::MakeConstSpan(unknown_patterns)};
    return TrampolineContext{bindings_scratch, mem, refs, alloc};
  }

  // Intern `backing`, write a CEL_MESSAGE CelValue at kMsgSlot, and
  // register `(field_number, field_name)` at field_ref_id = 1.
  uint32_t BindMessage(const std::shared_ptr<HostMessageBacking>& backing,
                       uint32_t field_number, absl::string_view field_name) {
    const uint32_t slot = refs.Intern(backing);
    CelValue cv{};
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = slot;
    mem.WriteCelValue(kMsgSlot, cv);
    field_refs.push_back(FieldRefEntry{field_number, std::string(field_name)});
    return slot;
  }

  // Default dispatch: out_slot=64, msg_slot=16, field_ref_id=1,
  // attribute_id=0.  Tests that need aliasing / non-zero
  // attribute_id call CelGetFieldImpl directly.
  CelValue Get(uint32_t attribute_id = 0) {
    auto status = CelGetFieldImpl(kOutSlot, kMsgSlot, /*field_ref_id=*/1,
                                  attribute_id, Ctx());
    EXPECT_THAT(status, IsOk());
    return mem.ReadCelValue(kOutSlot);
  }
  CelValue Has() {
    auto status =
        CelHasFieldImpl(kOutSlot, kMsgSlot, /*field_ref_id=*/1, 0, Ctx());
    EXPECT_THAT(status, IsOk());
    return mem.ReadCelValue(kOutSlot);
  }
};

}  // namespace celwasm::test

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_TEST_HARNESS_H_
