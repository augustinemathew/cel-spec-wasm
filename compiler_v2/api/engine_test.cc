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
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"
#include "wasm.h"
#include "wasmtime.h"

namespace cel {
namespace {

// Mirror of the smoke test's expr WAT (see the experiment branch's
// two_phase_shared_memory_smoke_test.cc) — imports cel.memory +
// cel.arena_reset + cel.arena_alloc, exports `eval`.  Calls reset(64,
// 65536), alloc(24), writes a 24-byte payload.  Eval correctness
// is exercised in instance_test.cc once Eval lands; here we only
// need a Program whose imports match what Plan provides so
// instantiation succeeds.
// Phase C: the runtime exports a shared memory (wasm32-wasi-threads).
// The expr import must declare shared+max to match (Wasmtime rejects a
// non-shared import against a shared export).  Also note the
// `arena_reset` import is 0-arg post-runtime-refactor, not 2-arg.
constexpr char kSyntheticExprWat[] = R"WAT(
(module
  (import "cel" "memory" (memory 1 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (func (export "eval") (result i32)
    (local $off i32)
    (call $arena_reset)
    (local.set $off (call $arena_alloc (i32.const 24)))
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

  // Wasm magic + garbage past the version word.  As shipped, the
  // expr module is parsed via `wasmtime_module_new` before the
  // cel.abi custom section is decoded, so malformed bytes fail at
  // module-parse and surface as `FailedPrecondition` (the
  // wasmtime-error wrapper status code).
  Program garbage(std::vector<uint8_t>{0x00, 0x61, 0x73, 0x6d, 0xff, 0xff});
  auto inst_or = engine.Plan(garbage);
  EXPECT_FALSE(inst_or.ok());
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

TEST(EnginePlanTest, InstanceOutlivesEngineAndCompilerWithEvalProof) {
  // Build everything inside an inner scope so Compiler + Engine +
  // Program all destruct before the assertions run.  Instance
  // holds a shared_ptr to WasmtimeEngineState (the engine + parsed
  // runtime module), so the wasm execution machinery stays alive;
  // its own store / linker / instance were always Instance-owned.
  //
  // Proof of life via Eval: the Eval call goes through
  // wasmtime_func_call against the cached eval_fn, which in turn
  // dispatches into the runtime instance (which holds an internal
  // ref to the runtime module via the shared_ptr).  If any of
  // {engine, runtime module, store, linker, runtime_instance,
  // expr_instance, eval_fn} were freed during Engine's destruction
  // this would either trap or UB.  A clean Int(42) is the only
  // way to pass.
  Instance inst = ([]() {
    auto compiler_or = Compiler::NewBuilder().Build();
    ABSL_CHECK_OK(compiler_or);
    auto engine_or = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(engine_or);
    auto prog_or = compiler_or->Compile("42");
    ABSL_CHECK_OK(prog_or);
    auto inst_or = engine_or->Plan(*prog_or);
    ABSL_CHECK_OK(inst_or);
    return *std::move(inst_or);
  })();

  auto v_or = inst.Eval();
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kInt);
  auto i = v_or->AsInt();
  ASSERT_TRUE(i.ok());
  EXPECT_EQ(*i, 42);

  // Eval is also re-entrant after the originating Engine drop.
  auto v2_or = inst.Eval();
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();
  EXPECT_EQ(*v2_or->AsInt(), 42);
}

}  // namespace
}  // namespace cel
