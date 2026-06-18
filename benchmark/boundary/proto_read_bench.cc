// Host-side proto field-read cost breakdown (no wasm boundary).
//
// The wasm->host trampoline is ~3 ns (unchecked ABI, see
// wasmtime_call_bench.cc::BM_WasmToHost_Unchecked).  The end-to-end
// proto field read is ~29 ns/field (benchmark/eval celwasm_bench
// reads5/reads10 slope), so ~26 ns is HOST-SIDE work.  This bench
// cracks that open: descriptor lookup (cached in production),
// reflection Get (paid per read), and the 24-byte CelValue write.
//
//   bazel run -c opt //benchmark/boundary:proto_read_bench

#include <cstdint>
#include <string>

#include "benchmark/benchmark.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "testdata/e2e_fixture.pb.h"

namespace {

using ::celwasm::testdata::Customer;

Customer MakeCustomer() {
  Customer c;
  c.set_name("Ada Lovelace");
  c.set_age(38);
  c.set_user_id(1234567890123LL);
  c.set_priority(7);
  c.set_balance_cents(999999u);
  c.set_credit_score(812.5);
  c.set_is_premium(true);
  return c;
}

// 24-byte CelValue mirror (runtime/cel_data.h layout) so the encode+write
// cost matches what the trampoline does into linear memory.
struct CelValueBytes {
  uint32_t kind;
  uint32_t pad;
  int64_t payload;
  int64_t slack;
};

// (1) Descriptor lookup — FindFieldByNumber.  Production CACHES this per
// access site, so this is the cost the cache SAVES, not a per-read cost.
void BM_FindFieldByNumber(benchmark::State& state) {
  const google::protobuf::Descriptor* desc = Customer::GetDescriptor();
  for ([[maybe_unused]] auto _ : state) {
    const google::protobuf::FieldDescriptor* f = desc->FindFieldByNumber(2);
    benchmark::DoNotOptimize(f);
  }
}
BENCHMARK(BM_FindFieldByNumber)->Unit(benchmark::kNanosecond);

// (2) GetReflection() — fetched per ReadField call in production.
void BM_GetReflection(benchmark::State& state) {
  Customer c = MakeCustomer();
  const google::protobuf::Message& m = c;
  for ([[maybe_unused]] auto _ : state) {
    const google::protobuf::Reflection* r = m.GetReflection();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_GetReflection)->Unit(benchmark::kNanosecond);

// (3) Raw reflection scalar read (int32 age) — descriptor pre-resolved.
//     This is the irreducible per-read reflection cost.
void BM_ReflectGetInt32(benchmark::State& state) {
  Customer c = MakeCustomer();
  const google::protobuf::Message& m = c;
  const google::protobuf::FieldDescriptor* f =
      m.GetDescriptor()->FindFieldByNumber(2);
  for ([[maybe_unused]] auto _ : state) {
    int32_t v = m.GetReflection()->GetInt32(m, f);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_ReflectGetInt32)->Unit(benchmark::kNanosecond);

// (4) Full per-read production shape: GetReflection + Get + build the
//     24-byte CelValue + write it (the trampoline's host work, minus
//     the cached descriptor lookup and the externref msg_slot lookup).
void BM_ReflectGetInt32_Encode(benchmark::State& state) {
  Customer c = MakeCustomer();
  const google::protobuf::Message& m = c;
  const google::protobuf::FieldDescriptor* f =
      m.GetDescriptor()->FindFieldByNumber(2);
  CelValueBytes out{};
  for ([[maybe_unused]] auto _ : state) {
    int32_t v = m.GetReflection()->GetInt32(m, f);
    out.kind = 2;  // CEL_INT
    out.payload = v;
    benchmark::DoNotOptimize(&out);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_ReflectGetInt32_Encode)->Unit(benchmark::kNanosecond);

// (5) String field read — GetStringReference returns a view (no copy),
//     mirroring production's view-based string reads.
void BM_ReflectGetStringView(benchmark::State& state) {
  Customer c = MakeCustomer();
  const google::protobuf::Message& m = c;
  const google::protobuf::FieldDescriptor* f =
      m.GetDescriptor()->FindFieldByNumber(1);
  std::string scratch;
  for ([[maybe_unused]] auto _ : state) {
    const std::string& s =
        m.GetReflection()->GetStringReference(m, f, &scratch);
    benchmark::DoNotOptimize(s.data());
  }
}
BENCHMARK(BM_ReflectGetStringView)->Unit(benchmark::kNanosecond);

}  // namespace

BENCHMARK_MAIN();
