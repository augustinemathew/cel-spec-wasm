// Google Benchmark harness for proto field / message operations.
//
// Each fixture pre-compiles a single expression and wraps one
// `Customer` message as an externref; the measurement loop re-passes
// that externref to `CallEval`.  Re-allocating the externref per call
// would add ~1µs per iteration (wasmtime_externref_new + finalizer
// registration), which would drown out the field-read path we care
// about here.  The trade-off: the message contents are identical
// across iterations, so branch-predictable compare paths will report
// best-case numbers.  If worst-case field-read cost becomes
// interesting, we'll add a randomised variant later.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "compiler/bench/bench_fixture.h"
#include "compiler/host/host_loader.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "wasmtime.h"

namespace celwasm::bench {
namespace {

constexpr absl::string_view kCustomerSpec = "c:celwasm.testdata.Customer";

struct ProtoFixture {
  PrecompiledEval eval;
  celwasm::testdata::Customer msg;
};

ProtoFixture MakeFixture(absl::string_view source, benchmark::State& state) {
  ProtoFixture fx;
  auto loaded = Precompile(source, {std::string(kCustomerSpec)});
  if (!loaded.ok()) {
    state.SkipWithError(loaded.status().ToString());
    return fx;
  }
  fx.eval.loaded = *std::move(loaded);
  fx.msg.set_name("Ada Lovelace");
  fx.msg.set_age(36);
  fx.msg.set_user_id(1815);
  fx.msg.set_priority(3);
  fx.msg.set_balance_cents(4200);
  fx.msg.set_credit_score(0.78);
  fx.msg.set_is_premium(true);
  fx.msg.set_session_token("token-abc-123");
  fx.msg.mutable_billing_address()->set_city("London");
  fx.msg.mutable_billing_address()->set_country("UK");
  fx.eval.args = {MessageAsExternref(fx.eval.loaded, fx.msg)};
  return fx;
}

void RunProto(benchmark::State& state, ProtoFixture& fx) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(_);
    auto r = fx.eval.loaded.CallEval(fx.eval.args);
    benchmark::DoNotOptimize(r);
    if (!r.ok()) {
      state.SkipWithError(r.status().ToString());
      return;
    }
  }
}

void BM_SelectStringField(benchmark::State& state) {
  auto fx = MakeFixture("c.name", state);
  RunProto(state, fx);
}
BENCHMARK(BM_SelectStringField);

void BM_SelectBoolField(benchmark::State& state) {
  auto fx = MakeFixture("c.is_premium", state);
  RunProto(state, fx);
}
BENCHMARK(BM_SelectBoolField);

void BM_SelectInt32Field(benchmark::State& state) {
  auto fx = MakeFixture("c.age", state);
  RunProto(state, fx);
}
BENCHMARK(BM_SelectInt32Field);

void BM_SelectInt64Field(benchmark::State& state) {
  auto fx = MakeFixture("c.user_id", state);
  RunProto(state, fx);
}
BENCHMARK(BM_SelectInt64Field);

void BM_SelectDoubleField(benchmark::State& state) {
  auto fx = MakeFixture("c.credit_score", state);
  RunProto(state, fx);
}
BENCHMARK(BM_SelectDoubleField);

void BM_SelectBytesField(benchmark::State& state) {
  auto fx = MakeFixture("c.session_token", state);
  RunProto(state, fx);
}
BENCHMARK(BM_SelectBytesField);

void BM_NestedSelectStringField(benchmark::State& state) {
  auto fx = MakeFixture("c.billing_address.city", state);
  RunProto(state, fx);
}
BENCHMARK(BM_NestedSelectStringField);

void BM_StringFieldEq(benchmark::State& state) {
  auto fx = MakeFixture("c.name == 'Ada Lovelace'", state);
  RunProto(state, fx);
}
BENCHMARK(BM_StringFieldEq);

void BM_HasScalarField(benchmark::State& state) {
  auto fx = MakeFixture("has(c.name)", state);
  RunProto(state, fx);
}
BENCHMARK(BM_HasScalarField);

void BM_HasSubMessage(benchmark::State& state) {
  auto fx = MakeFixture("has(c.billing_address)", state);
  RunProto(state, fx);
}
BENCHMARK(BM_HasSubMessage);

void BM_CompoundFieldPredicate(benchmark::State& state) {
  auto fx = MakeFixture(
      "c.age > 18 && c.is_premium && c.name != '' && c.balance_cents > 0u",
      state);
  RunProto(state, fx);
}
BENCHMARK(BM_CompoundFieldPredicate);

}  // namespace
}  // namespace celwasm::bench
