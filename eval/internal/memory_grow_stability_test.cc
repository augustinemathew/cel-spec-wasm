// Eval-level regression for the linear-memory base-stability
// contract documented on `InstanceImpl::memory`: the host reads wasm
// linear memory through wasmtime's shared-memory API, which returns
// a STABLE base pointer across `memory.grow` (shared memories
// reserve their declared maximum up front; grow only commits pages).
// Host-side artifacts that depend on it:
//
//   - `absl::string_view`s lifted over linear memory (map-key /
//     span decode in the cel_host trampolines);
//   - the cached base pointer in `CelHostCallbackEnv::mem_base`;
//   - the per-Eval size snapshot that `WasmtimeMemoryView` refreshes
//     on a bounds miss (a stale snapshot under-approximates after a
//     mid-Eval grow; refresh-then-retest keeps freshly grown pages
//     addressable without falsely rejecting them).
//
// The tests force a real `memory.grow` MID-$eval: the bump arena is
// seeded at CELWASM_ARENA_CAPACITY_BYTES (64 KiB), and a string
// concat chain over a 256 KiB bound variable allocates ~8.75 MiB of
// intermediates through arena_alloc → dlmalloc → memory.grow.  The
// activation marshal itself allocates only the ~256 KiB activation
// buffer, so a multi-MiB size delta proves the grow happened inside
// $eval, not just during marshal.
//
// API-level counterparts (raw `wasmtime_sharedmemory_grow` + the
// view refresh semantics) live in `wasmtime_memory_view_e2e_test.cc`.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "bazel/link_mode_test_helpers.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/internal/instance_impl.h"
#include "eval/internal/instance_test_peer.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// Force generated-pool registration of the Customer descriptor the
// trampoline test reads through.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      return 0;
    }();

// 8-term concat: with `s` bound to kBoundLen bytes the left-assoc
// chain allocates 2+3+...+8 = 35 × kBoundLen of arena intermediates
// (~8.75 MiB at 256 KiB) and produces an 8 × kBoundLen result.
constexpr absl::string_view kConcat8 = "s + s + s + s + s + s + s + s";
constexpr size_t kBoundLen = size_t{256} * 1024;
constexpr size_t kConcatTerms = 8;

// Lower bound on the data_size delta one Eval of kConcat8 must
// produce.  The marshal accounts for at most the 4K-rounded
// activation buffer (~260 KiB) plus dlmalloc bookkeeping; anything
// past kMarshalSlackBytes can only come from in-$eval arena_alloc —
// i.e. the grow demonstrably happened mid-$eval.
constexpr size_t kMarshalSlackBytes = size_t{4} * 1024 * 1024;

Instance PlanGrowExpr(Engine& engine, absl::string_view expr,
                      bool declare_customer) {
  auto builder = Compiler::NewBuilder();
  builder.DeclareVariable("s", CelType::String());
  if (declare_customer) {
    builder.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  }
  auto compiler_or = std::move(builder).Build();
  ABSL_CHECK_OK(compiler_or);
  CompilerOptions opts;
  opts.link_mode = kTestLinkMode;
  auto prog_or = compiler_or->Compile(expr, opts);
  ABSL_CHECK_OK(prog_or) << expr;
  auto inst_or = engine.Plan(*prog_or);
  ABSL_CHECK_OK(inst_or) << expr;
  return *std::move(inst_or);
}

// (1) record base + size, (2) Eval an expression whose arena
// allocations force memory.grow mid-$eval, (3) assert the base is
// IDENTICAL, the size increased, and the result is correct.
TEST(MemoryGrowStabilityTest, BasePointerStableAcrossMidEvalGrow) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst = PlanGrowExpr(*engine_or, kConcat8,
                               /*declare_customer=*/false);
  wasmtime_sharedmemory_t* mem = InstanceTestPeer::impl(inst).memory;
  ASSERT_NE(mem, nullptr);

  const uint8_t* base_before = wasmtime_sharedmemory_data(mem);
  const size_t size_before = wasmtime_sharedmemory_data_size(mem);
  ASSERT_NE(base_before, nullptr);

  Activation a;
  a.Bind("s", Value::String(std::string(kBoundLen, 'a')));
  auto v_or = inst.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kString);
  EXPECT_EQ(*v_or->AsString(), std::string(kConcatTerms * kBoundLen, 'a'));

  // The load-bearing contract: same base pointer after the grow.
  EXPECT_EQ(wasmtime_sharedmemory_data(mem), base_before);
  const size_t size_after = wasmtime_sharedmemory_data_size(mem);
  EXPECT_GT(size_after, size_before);
  // Multi-MiB delta ⇒ the grow happened inside $eval (the marshal
  // alone cannot account for it — see kMarshalSlackBytes).
  EXPECT_GT(size_after - size_before, kMarshalSlackBytes);

  // Re-Eval on the grown memory: still correct, base still stable
  // (the arena keeps its chunks across resets, so this exercises
  // the already-grown steady state).
  auto v2_or = inst.Eval(a);
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();
  EXPECT_EQ(*v2_or->AsString(), std::string(kConcatTerms * kBoundLen, 'a'));
  EXPECT_EQ(wasmtime_sharedmemory_data(mem), base_before);
}

// Refresh-on-miss, end-to-end: the map-lookup key is built by the
// concat chain, so its span lives in pages grown mid-$eval — AFTER
// the per-Eval size snapshot was taken.  The cel_map_lookup
// trampoline must read that span through the host view (DecodeKey →
// ReadSpan); a stale size snapshot that falsely rejected it would
// decode an empty key and miss the map entry.  Correct result here
// pins the refresh-then-retest story.
TEST(MemoryGrowStabilityTest, TrampolineReadsSpanFromPagesGrownMidEval) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Instance inst =
      PlanGrowExpr(*engine_or, "c.metadata[s + s + s + s + s + s + s + s]",
                   /*declare_customer=*/true);
  wasmtime_sharedmemory_t* mem = InstanceTestPeer::impl(inst).memory;
  ASSERT_NE(mem, nullptr);
  const uint8_t* base_before = wasmtime_sharedmemory_data(mem);

  celwasm::testdata::Customer c;
  (*c.mutable_metadata())[std::string(kConcatTerms * kBoundLen, 'k')] = "hit";

  Activation a;
  a.Bind("s", Value::String(std::string(kBoundLen, 'k')));
  a.Bind("c", Value::Message(c));
  auto v_or = inst.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kString);
  EXPECT_EQ(*v_or->AsString(), "hit");
  EXPECT_EQ(wasmtime_sharedmemory_data(mem), base_before);
}

}  // namespace
}  // namespace celwasm
