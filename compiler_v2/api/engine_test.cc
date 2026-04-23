// M1 — Engine lifecycle + Engine::Plan basic wiring.
//
// Plan tests use a hand-written minimal WAT module instead of a
// real Compile()'d Program because the production codegen still
// emits expr-defines-memory; Plan §5.6 (revised) flips that in a
// follow-up commit, after which engine_test.cc can switch to using
// `Compiler::Compile`.  Synthetic WAT here ensures Plan's wasmtime
// wiring is tested in isolation and the test doesn't regress when
// the codegen flip lands.

#include "compiler_v2/api/engine.h"

#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/program.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace cel {
namespace {

// Mirror of the smoke test's expr WAT (see the experiment branch's
// two_phase_shared_memory_smoke_test.cc) — imports cel.memory +
// cel.cel_reset + cel.cel_alloc, exports `eval`.  Calls reset(64,
// 65536), alloc(24), writes a 24-byte payload.  Eval correctness
// is exercised in instance_test.cc once Eval lands; here we only
// need a Program whose imports match what Plan provides so
// instantiation succeeds.
constexpr char kSyntheticExprWat[] = R"WAT(
(module
  (import "cel" "memory" (memory 1))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (func (export "eval") (result i32)
    (local $off i32)
    (call $cel_reset (i32.const 64) (i32.const 65536))
    (local.set $off (call $cel_alloc (i32.const 24)))
    (i32.store (local.get $off) (i32.const 1))
    (i64.store offset=8 (local.get $off) (i64.const 42))
    (local.get $off)))
)WAT";

std::vector<uint8_t> Wat2Wasm(absl::string_view wat) {
  wasm_byte_vec_t out;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
  ABSL_CHECK(err == nullptr);
  std::vector<uint8_t> bytes(out.data, out.data + out.size);
  wasm_byte_vec_delete(&out);
  return bytes;
}

Program SyntheticProgram() {
  return Program(Wat2Wasm(kSyntheticExprWat));
}

TEST(EngineBuilderTest, BuildSucceedsWithDefaults) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
}

TEST(EngineBuilderTest, BuildIsCheapToCallTwiceFromIndependentBuilders) {
  auto a = Engine::NewBuilder().Build();
  auto b = Engine::NewBuilder().Build();
  ASSERT_TRUE(a.ok()) << a.status();
  ASSERT_TRUE(b.ok()) << b.status();
}

TEST(EngineLifetimeTest, MoveConstructionPreservesState) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine a = *std::move(engine_or);
  Engine b = std::move(a);
  (void)b;
}

TEST(EnginePlanTest, PlanSucceedsOnSyntheticProgram) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);

  Program program = SyntheticProgram();
  auto inst_or = engine.Plan(program);
  ASSERT_TRUE(inst_or.ok()) << inst_or.status();
}

TEST(EnginePlanTest, PlanCalledTwiceProducesIndependentInstances) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);

  Program program = SyntheticProgram();
  auto a = engine.Plan(program);
  auto b = engine.Plan(program);
  ASSERT_TRUE(a.ok()) << a.status();
  ASSERT_TRUE(b.ok()) << b.status();
  // Both Instances are live independently; dropping one shouldn't
  // affect the other.  Just letting the scope close exercises that.
}

TEST(EnginePlanTest, PlanFailsOnMalformedBytes) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);

  // Wasm magic + garbage past the version word.
  Program garbage(std::vector<uint8_t>{0x00, 0x61, 0x73, 0x6d, 0xff, 0xff});
  auto inst_or = engine.Plan(garbage);
  EXPECT_EQ(inst_or.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(EnginePlanThreadingTest, ConcurrentPlanCallsAllSucceed) {
  // Threading invariant from the plan + cel-host-surface design:
  // Engine + parsed runtime module are wasmtime-thread-safe;
  // Plan() creates a fresh store / linker / memory per call,
  // sharing only the engine + runtime module.  Multiple threads
  // can call Plan concurrently against the same Engine.
  //
  // Test: N threads each call Plan; collect their resulting
  // Instances; verify all are valid via memory_size_bytes().  If
  // wasmtime races during concurrent module instantiation, this
  // either crashes (TSan would see it) or produces a Plan that
  // returns an unhealthy Instance.
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine engine = *std::move(engine_or);
  Program program = SyntheticProgram();

  constexpr int kNumThreads = 8;
  constexpr int kPlansPerThread = 4;
  std::vector<std::thread> threads;
  std::vector<std::vector<Instance>> per_thread(kNumThreads);
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&, t]() {
      per_thread[t].reserve(kPlansPerThread);
      for (int i = 0; i < kPlansPerThread; ++i) {
        auto inst_or = engine.Plan(program);
        ABSL_CHECK_OK(inst_or)
            << "thread " << t << " Plan iter " << i << ": " << inst_or.status();
        per_thread[t].push_back(*std::move(inst_or));
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  // Every Instance is healthy: store + memory still alive,
  // memory_size_bytes() returns the expected 2-page count.
  for (int t = 0; t < kNumThreads; ++t) {
    for (int i = 0; i < kPlansPerThread; ++i) {
      EXPECT_GT(per_thread[t][i].memory_size_bytes(), 0u)
          << "thread " << t << " plan " << i;
    }
  }
}

TEST(EnginePlanTest, InstanceOutlivesEngineThatBuiltIt) {
  // Plan in an inner scope with the Engine; let the Engine handle
  // die.  The Instance holds a shared_ptr to the WasmtimeEngineState
  // so the wasm engine + parsed runtime stay alive; the Instance's
  // own store/linker/instances were always Instance-owned.  Verify
  // by reading memory_size_bytes() AFTER the Engine drops — that
  // call goes through wasmtime_store_context + wasmtime_memory_data_size,
  // both of which would crash / UB on freed handles.
  std::size_t expected_size = 0;
  Instance inst = ([&]() {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    auto i = e->Plan(SyntheticProgram());
    ABSL_CHECK_OK(i);
    expected_size = i->memory_size_bytes();
    return *std::move(i);
  })();
  // Engine destroyed.  If Instance's wasmtime resources weren't
  // intact, this read would crash.
  EXPECT_GT(expected_size, 0u);
  EXPECT_EQ(inst.memory_size_bytes(), expected_size);
}

}  // namespace
}  // namespace cel
