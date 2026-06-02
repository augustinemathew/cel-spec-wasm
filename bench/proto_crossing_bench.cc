// Proto crossing benches — copy-vs-pull cost for moving a proto
// message across a boundary.
//
// This suite quantifies the decision a host faces when a proto value
// has to reach code on the far side of an isolation boundary (a
// foreign wasm module with its own private linear memory, or any
// consumer that can't share the proto's host-side storage):
//
//   - COPY-BY-VALUE: serialize the message to protobuf wire bytes,
//     hand the bytes across, parse them on the far side into a fresh
//     object.  Cost = serialize + parse, paid in full regardless of
//     how many fields the consumer actually reads.  The `ProtoSerialize`
//     / `ProtoParse` / `ProtoRoundTrip` benches below are the raw
//     library cost of that path.
//
//   - PULL: leave the message in host storage and read only the
//     fields the expression touches, through the field-read
//     trampoline (`cel_get_field` → `ProtoBacking::ReadField` →
//     arena-encode).  The `Pull_*` benches measure this end-to-end
//     (`Instance::Eval` on a select), so the number includes the real
//     in-system trampoline + wasm-eval overhead a foreign caller pays
//     per field, not just the reflection read.
//
// Reading the two halves together is the point: a `ProtoRoundTrip`
// number is what you pay to cross the *whole* message once; a `Pull_*`
// number is what you pay per *field*.  Multiply the per-field pull by
// the field count an expression actually touches and compare — that
// crossover is what decides copy vs pull for a given access pattern.
//
// Message shapes (built once per fixture, outside every loop):
//   - Small  — name + a handful of scalars.
//   - Large  — all scalars + a ~64-entry metadata map + ~64 tags.
//   - Nested — billing_address + the two well-known-time fields set.
//
// Run (always -c opt; debug builds inflate timings ~10x):
//
//   bazel run -c opt //bench:proto_crossing_bench

#include <cstdint>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "benchmark/benchmark.h"
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "compiler/program.h"
#include "shared/type.h"
#include "eval/value.h"
#include "testdata/e2e_fixture.pb.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/message.h"

namespace celwasm {
namespace {

using ::celwasm::testdata::Customer;

// Force generated-pool registration of the descriptor used below.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<Customer>();
      return 0;
    }();

// ============================================================
// Fixture builders.
// ============================================================

Customer MakeSmall() {
  Customer m;
  m.set_name("Ada Lovelace");
  m.set_age(36);
  m.set_user_id(1815);
  m.set_is_premium(true);
  return m;
}

Customer MakeLarge() {
  Customer m = MakeSmall();
  m.set_priority(7);
  m.set_balance_cents(1234567);
  m.set_credit_score(812.5);
  m.set_session_token("0123456789abcdef0123456789abcdef");
  auto& metadata = *m.mutable_metadata();
  auto& quotas = *m.mutable_tier_quotas();
  for (int i = 0; i < 64; ++i) {
    metadata[absl::StrCat("key_", i)] = absl::StrCat("value_", i);
    quotas[i] = i * 10;
    m.add_tags(absl::StrCat("tag_", i));
  }
  return m;
}

Customer MakeNested() {
  Customer m = MakeSmall();
  m.mutable_billing_address()->set_city("London");
  m.mutable_billing_address()->set_country("United Kingdom");
  m.mutable_created_at()->set_seconds(1'600'000'000);
  m.mutable_created_at()->set_nanos(500);
  m.mutable_session_length()->set_seconds(3600);
  return m;
}

// ============================================================
// Shared pipeline helpers (mirror pipeline_bench.cc).
// ============================================================

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

Compiler MakeCustomerCompiler() {
  Compiler::Builder b;
  b.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *std::move(c);
}

Instance PlanOrDie(const Compiler& c, absl::string_view src) {
  auto p = c.Compile(src);
  ABSL_CHECK_OK(p) << src;
  auto i = GlobalEngine().Plan(*p);
  ABSL_CHECK_OK(i);
  return *std::move(i);
}

// ============================================================
// COPY-BY-VALUE — raw protobuf serialize / parse cost.
//
// This is the cost a foreign consumer pays to receive the *whole*
// message by value, independent of how many fields it reads.  Parse
// is split into a heap variant (fresh `Customer`, the common case)
// and an arena variant (`google::protobuf::Arena`) to expose how much
// of parse is allocation — cel-cpp parses into arenas for exactly
// this reason.
// ============================================================

void SerializeBench(benchmark::State& state, const Customer& m) {
  std::string buf;
  for (auto _ : state) {
    buf.clear();
    ABSL_CHECK(m.SerializeToString(&buf));
    benchmark::DoNotOptimize(buf);
  }
  state.SetBytesProcessed(state.iterations() * buf.size());
}

void BM_ProtoSerialize_Small(benchmark::State& state) {
  SerializeBench(state, MakeSmall());
}
BENCHMARK(BM_ProtoSerialize_Small);

void BM_ProtoSerialize_Large(benchmark::State& state) {
  SerializeBench(state, MakeLarge());
}
BENCHMARK(BM_ProtoSerialize_Large);

void BM_ProtoSerialize_Nested(benchmark::State& state) {
  SerializeBench(state, MakeNested());
}
BENCHMARK(BM_ProtoSerialize_Nested);

// Parse into a fresh heap-allocated message (the default cost).
void ParseHeapBench(benchmark::State& state, const Customer& m) {
  std::string buf;
  ABSL_CHECK(m.SerializeToString(&buf));
  for (auto _ : state) {
    Customer parsed;
    ABSL_CHECK(parsed.ParseFromString(buf));
    benchmark::DoNotOptimize(parsed);
  }
  state.SetBytesProcessed(state.iterations() * buf.size());
}

void BM_ProtoParse_Small(benchmark::State& state) {
  ParseHeapBench(state, MakeSmall());
}
BENCHMARK(BM_ProtoParse_Small);

void BM_ProtoParse_Large(benchmark::State& state) {
  ParseHeapBench(state, MakeLarge());
}
BENCHMARK(BM_ProtoParse_Large);

void BM_ProtoParse_Nested(benchmark::State& state) {
  ParseHeapBench(state, MakeNested());
}
BENCHMARK(BM_ProtoParse_Nested);

// Parse into an arena — collapses the per-object malloc churn that
// dominates heap parse into bump allocation.
void ParseArenaBench(benchmark::State& state, const Customer& m) {
  std::string buf;
  ABSL_CHECK(m.SerializeToString(&buf));
  for (auto _ : state) {
    google::protobuf::Arena arena;
    auto* parsed = google::protobuf::Arena::Create<Customer>(&arena);
    ABSL_CHECK(parsed->ParseFromString(buf));
    benchmark::DoNotOptimize(parsed);
  }
  state.SetBytesProcessed(state.iterations() * buf.size());
}

void BM_ProtoParse_Large_Arena(benchmark::State& state) {
  ParseArenaBench(state, MakeLarge());
}
BENCHMARK(BM_ProtoParse_Large_Arena);

// Full cross-by-value cost: serialize on the near side + parse on the
// far side.  This is the apples-to-apples number to compare against
// `Pull_*` × (fields touched).
void RoundTripBench(benchmark::State& state, const Customer& m) {
  std::string buf;
  for (auto _ : state) {
    buf.clear();
    ABSL_CHECK(m.SerializeToString(&buf));
    Customer parsed;
    ABSL_CHECK(parsed.ParseFromString(buf));
    benchmark::DoNotOptimize(parsed);
  }
  state.SetBytesProcessed(state.iterations() * buf.size());
}

void BM_ProtoRoundTrip_Small(benchmark::State& state) {
  RoundTripBench(state, MakeSmall());
}
BENCHMARK(BM_ProtoRoundTrip_Small);

void BM_ProtoRoundTrip_Large(benchmark::State& state) {
  RoundTripBench(state, MakeLarge());
}
BENCHMARK(BM_ProtoRoundTrip_Large);

void BM_ProtoRoundTrip_Nested(benchmark::State& state) {
  RoundTripBench(state, MakeNested());
}
BENCHMARK(BM_ProtoRoundTrip_Nested);

// ============================================================
// PULL — per-field read through the host trampoline, end-to-end.
//
// `Instance::Eval` on a select expression drives the real
// `cel_get_field` path: reflection read + value encode (+ arena copy
// for string/bytes).  The message is bound once; only the loop body
// Evals, matching compile-once-eval-many.  Scalar vs string vs nested
// are separated because their costs differ (a scalar field skips the
// arena copy; a string field pays it; a nested select chains two
// reads).
// ============================================================

void PullBench(benchmark::State& state, absl::string_view src,
               const Customer& m) {
  Compiler c = MakeCustomerCompiler();
  Instance inst = PlanOrDie(c, src);
  Activation a;
  a.Bind("c", Value::Message(m));
  for (auto _ : state) {
    auto v = inst.Eval(a);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}

// One scalar field — the cheapest pull (no arena copy of a payload).
void BM_Pull_ScalarField(benchmark::State& state) {
  PullBench(state, "c.age", MakeSmall());
}
BENCHMARK(BM_Pull_ScalarField);

// One string field — pays the arena materialisation of the bytes.
void BM_Pull_StringField(benchmark::State& state) {
  PullBench(state, "c.name", MakeSmall());
}
BENCHMARK(BM_Pull_StringField);

// Two scalar fields — two trampoline reads in one eval.
void BM_Pull_TwoScalarFields(benchmark::State& state) {
  PullBench(state, "c.age + c.user_id", MakeSmall());
}
BENCHMARK(BM_Pull_TwoScalarFields);

// Nested select — chains two reads (message read, then field read on
// the submessage).
void BM_Pull_NestedField(benchmark::State& state) {
  PullBench(state, "c.billing_address.city", MakeNested());
}
BENCHMARK(BM_Pull_NestedField);

}  // namespace
}  // namespace celwasm
