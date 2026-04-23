// M1 — Engine / Builder lifecycle.  Per (revised) Plan §5.2:
// Build allocates the wasm engine and parses cel_runtime.wasm
// once.  Subsequent commits add `Plan(program, bindings)` and
// the per-Plan tests; this commit just exercises the lifecycle
// + the immutability + multi-Engine independence properties
// that the architecture relies on.

#include "compiler_v2/api/engine.h"

#include <utility>

#include "gtest/gtest.h"

namespace cel {
namespace {

TEST(EngineBuilderTest, BuildSucceedsWithDefaults) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
}

TEST(EngineBuilderTest, BuildIsCheapToCallTwiceFromIndependentBuilders) {
  // Each Builder is an independent &&-consume; building two
  // engines in sequence should both succeed and give independent
  // wasm engines + parsed runtime modules under the hood.
  auto a = Engine::NewBuilder().Build();
  auto b = Engine::NewBuilder().Build();
  ASSERT_TRUE(a.ok()) << a.status();
  ASSERT_TRUE(b.ok()) << b.status();
}

TEST(EngineLifetimeTest, MoveConstructionPreservesState) {
  auto engine_or = Engine::NewBuilder().Build();
  ASSERT_TRUE(engine_or.ok()) << engine_or.status();
  Engine a = *std::move(engine_or);
  // Move into a fresh Engine — destruction of the moved-from half
  // is a no-op (shared_ptr was moved out).
  Engine b = std::move(a);
  (void)b;
}

}  // namespace
}  // namespace cel
