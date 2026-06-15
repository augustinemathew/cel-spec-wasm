// Throwaway probe: per-field proto read cost, generated-getter ("native")
// vs reflection, across three field shapes — scalar (literal), repeated
// (array element), and map lookup.  Pure in-process protobuf; no wasm,
// no cel-cpp.  Measures the *reflection-removal* component of a
// monomorphized access (the boundary + direct-wasm-load components need
// the eval/wat harness, not this).
//
//   bazel run -c opt //benchmark/proto_read:proto_read_bench

#include <cstdint>
#include <string>

#include "benchmark/benchmark.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/map_field.h"
#include "google/protobuf/message.h"
#include "testdata/host_fixture_proto3.pb.h"

namespace {

using ::celwasm::testdata::HostMsg3;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::MapKey;
using ::google::protobuf::MapValueConstRef;
using ::google::protobuf::Reflection;

constexpr int kN = 64;       // elements in the repeated / map fields
constexpr int kIdx = 32;     // element / key we read
const char* const kKey = "k32";

HostMsg3 MakeMsg() {
  HostMsg3 m;
  m.set_i64(42);
  for (int i = 0; i < kN; ++i) {
    m.add_rep_i32(i);
    (*m.mutable_str_to_i32())["k" + std::to_string(i)] = i;
  }
  return m;
}

// ---- scalar (literal) -----------------------------------------------------
void BM_Scalar_Native(benchmark::State& s) {
  HostMsg3 m = MakeMsg();
  for (auto _ : s) benchmark::DoNotOptimize(m.i64());
}
BENCHMARK(BM_Scalar_Native);

void BM_Scalar_Reflection(benchmark::State& s) {
  HostMsg3 m = MakeMsg();
  const Reflection* r = m.GetReflection();
  const FieldDescriptor* fd = m.GetDescriptor()->FindFieldByName("i64");
  for (auto _ : s) benchmark::DoNotOptimize(r->GetInt64(m, fd));
}
BENCHMARK(BM_Scalar_Reflection);

// ---- repeated element (array) --------------------------------------------
void BM_RepeatedElem_Native(benchmark::State& s) {
  HostMsg3 m = MakeMsg();
  for (auto _ : s) benchmark::DoNotOptimize(m.rep_i32(kIdx));
}
BENCHMARK(BM_RepeatedElem_Native);

void BM_RepeatedElem_Reflection(benchmark::State& s) {
  HostMsg3 m = MakeMsg();
  const Reflection* r = m.GetReflection();
  const FieldDescriptor* fd = m.GetDescriptor()->FindFieldByName("rep_i32");
  for (auto _ : s) benchmark::DoNotOptimize(r->GetRepeatedInt32(m, fd, kIdx));
}
BENCHMARK(BM_RepeatedElem_Reflection);

// ---- map lookup -----------------------------------------------------------
void BM_MapLookup_Native(benchmark::State& s) {
  HostMsg3 m = MakeMsg();
  const auto& map = m.str_to_i32();
  const std::string key = kKey;
  for (auto _ : s) {
    auto it = map.find(key);
    benchmark::DoNotOptimize(it->second);
  }
}
BENCHMARK(BM_MapLookup_Native);

void BM_MapLookup_Reflection(benchmark::State& s) {
  HostMsg3 m = MakeMsg();
  const Reflection* r = m.GetReflection();
  const FieldDescriptor* fd = m.GetDescriptor()->FindFieldByName("str_to_i32");
  MapKey key;
  key.SetStringValue(kKey);
  for (auto _ : s) {
    MapValueConstRef val;
    bool ok = r->LookupMapValue(m, fd, key, &val);
    benchmark::DoNotOptimize(ok ? val.GetInt32Value() : -1);
  }
}
BENCHMARK(BM_MapLookup_Reflection);

}  // namespace

BENCHMARK_MAIN();
