// Runtime kernel microbenches — deliverable A of the post-M10 bench
// suite.  Each BENCHMARK function micro-measures one load-bearing kernel
// out of `compiler_v2/runtime/cel_*.c`, with operands staged once outside
// the hot loop and the arena pre-reset so the timed window is just the
// kernel call.
//
// Coverage (matches the milestone scope listed in `bench/README.md`):
//
//   - Arithmetic     — cel_int_add / mul / div, cel_double_add.
//   - Comparison     — cel_int_eq, cel_numeric_eq (cross-type ladder),
//                      cel_string_eq, cel_bytes_eq.
//   - Aggregate      — cel_map_lookup_arena, cel_list_at_arena,
//                      cel_list_eq, cel_map_eq.
//   - 3VL            — cel_and, cel_or, cel_unknown_merge.
//   - Conversion     — cel_uint_to_int_at_v, cel_double_to_int_at_v,
//                      cel_string_to_int_at_v (sized), cel_int_to_string_at_v,
//                      cel_double_to_string_at_v.
//   - String / bytes — cel_string_concat_at_vv (sized),
//                      cel_string_contains_at_vv.
//
// The arena is reset once per benchmark `state` so that an operand
// staged before the loop stays valid for the duration of the loop — the
// kernels under test either (a) don't allocate, or (b) allocate in the
// arena (concat / unknown_merge / int_to_string / double_to_string).
// For allocating kernels, the arena is large enough (linear memory is
// statically sized in the native build — see cel_memory.c) that the
// bumps don't OOM in normal bench iteration counts.  If a future
// regression makes a kernel allocate dramatically more per call, the
// bench will surface it as a slowdown via arena pressure / cache miss
// rather than crashing.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "benchmark/benchmark.h"
#include "compiler_v2/runtime/cel_3vl.h"
#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_arith.h"
#include "compiler_v2/runtime/cel_compare.h"
#include "compiler_v2/runtime/cel_convert.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_list.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_map.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "compiler_v2/runtime/cel_string_ops.h"

namespace celwasm {
namespace {

// Reset the arena to a known base / limit.  Mirrors the SetUp() shape
// every runtime *_test.cc uses; the constants come from the parent
// design's linear-memory layout (bytes 0..16 are reserved; bumping
// starts at 16).
void ResetArena() {
  cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
}

// Allocate a fresh out-slot CelValue inside the arena.
uint32_t AllocSlot() {
  return cel_alloc(static_cast<uint32_t>(sizeof(CelValue)));
}

// ============================================================
// Arithmetic kernels (cel_arith.c).
// ============================================================

void BM_IntAdd(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_int(123);
  uint32_t b = cel_make_int(456);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_int_add_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_IntAdd);

void BM_IntMul(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_int(0x1234);
  uint32_t b = cel_make_int(0x5678);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_int_mul_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_IntMul);

void BM_IntDiv(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_int(987654321);
  uint32_t b = cel_make_int(13);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_int_div_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_IntDiv);

// Divide-by-zero short-circuit — kernel writes the error envelope into
// the out slot without touching `a`.  Locks the fast-reject cost.
void BM_IntDivByZero(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_int(42);
  uint32_t b = cel_make_int(0);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_int_div_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_IntDivByZero);

void BM_DoubleAdd(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_double(3.14159);
  uint32_t b = cel_make_double(2.71828);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_double_add_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_DoubleAdd);

// ============================================================
// Comparison kernels (cel_compare.c).
// ============================================================

void BM_IntEq(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_int(42);
  uint32_t b = cel_make_int(42);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_int_eq_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_IntEq);

// Cross-type ladder dispatcher — `1 == 1u` shape.  The interesting
// number here is the cost of the kind-pair branch ladder vs the same-
// kind path above (BM_IntEq).
void BM_NumericEqIntUint(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_int(42);
  uint32_t b = cel_make_uint(42u);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_numeric_eq_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_NumericEqIntUint);

void BM_NumericEqIntDouble(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_int(42);
  uint32_t b = cel_make_double(42.0);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_numeric_eq_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_NumericEqIntDouble);

// String equality at the size buckets we expect in real workloads.
// Bench arg is the operand byte count; uses two equal-content strings
// so the kernel walks the full length (the worst case).
void BM_StringEq(benchmark::State& state) {
  const int64_t n = state.range(0);
  ResetArena();
  std::string s(static_cast<size_t>(n), 'a');
  uint32_t a = cel_make_string(s.data(), static_cast<uint32_t>(n));
  uint32_t b = cel_make_string(s.data(), static_cast<uint32_t>(n));
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_string_eq_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
  state.SetBytesProcessed(state.iterations() * n);
}
BENCHMARK(BM_StringEq)->Arg(8)->Arg(64)->Arg(4096);

void BM_BytesEq(benchmark::State& state) {
  const int64_t n = state.range(0);
  ResetArena();
  std::vector<uint8_t> buf(static_cast<size_t>(n), 0xAB);
  uint32_t a = cel_make_bytes(buf.data(), static_cast<uint32_t>(n));
  uint32_t b = cel_make_bytes(buf.data(), static_cast<uint32_t>(n));
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_bytes_eq_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
  state.SetBytesProcessed(state.iterations() * n);
}
BENCHMARK(BM_BytesEq)->Arg(8)->Arg(64)->Arg(4096);

// ============================================================
// Aggregate kernels (cel_runtime.c — still in the umbrella TU
// after the P9 punt; see split-plan §Future work).
// ============================================================

// Build a small string-keyed map of size N once and measure a single
// hot-key lookup.  Mirrors the `m["k"]` literal shape codegen emits.
void BM_MapLookupArenaHit(benchmark::State& state) {
  ResetArena();
  // 4-entry map with string keys "k0".."k3"; lookup hits "k0".
  uint32_t m = AllocSlot();
  cel_map_create(m, /*capacity=*/4);
  for (int i = 0; i < 4; ++i) {
    char key_buf[4] = {'k', static_cast<char>('0' + i), '\0', '\0'};
    uint32_t k = cel_make_string(key_buf, 2u);
    uint32_t v = cel_make_int(100 + i);
    cel_map_insert(m, k, v);
  }
  uint32_t key = cel_make_string("k0", 2u);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_lookup_arena(out, m, key);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapLookupArenaHit);

void BM_MapLookupArenaMiss(benchmark::State& state) {
  ResetArena();
  uint32_t m = AllocSlot();
  cel_map_create(m, /*capacity=*/4);
  for (int i = 0; i < 4; ++i) {
    char key_buf[4] = {'k', static_cast<char>('0' + i), '\0', '\0'};
    uint32_t k = cel_make_string(key_buf, 2u);
    uint32_t v = cel_make_int(100 + i);
    cel_map_insert(m, k, v);
  }
  uint32_t key = cel_make_string("zz", 2u);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_lookup_arena(out, m, key);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapLookupArenaMiss);

void BM_ListAtArena(benchmark::State& state) {
  ResetArena();
  uint32_t l = AllocSlot();
  cel_list_create(l, /*count=*/8);
  for (int i = 0; i < 8; ++i) {
    uint32_t v = cel_make_int(static_cast<int64_t>(i) * 10);
    cel_list_set(l, static_cast<uint32_t>(i), v);
  }
  uint32_t idx = cel_make_int(3);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_list_at_arena(out, l, idx);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_ListAtArena);

// Dispatcher path — sees CEL_LIST_ARENA, tail-calls the arena arm.
// Headline: how much overhead the kDynamic branch adds vs the direct
// arena call above.
void BM_ListEqDispatchArena(benchmark::State& state) {
  ResetArena();
  uint32_t a = AllocSlot();
  uint32_t b = AllocSlot();
  cel_list_create(a, /*count=*/4);
  cel_list_create(b, /*count=*/4);
  for (int i = 0; i < 4; ++i) {
    uint32_t va = cel_make_int(i);
    uint32_t vb = cel_make_int(i);
    cel_list_set(a, static_cast<uint32_t>(i), va);
    cel_list_set(b, static_cast<uint32_t>(i), vb);
  }
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_list_eq(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_ListEqDispatchArena);

void BM_MapEqDispatchArena(benchmark::State& state) {
  ResetArena();
  uint32_t a = AllocSlot();
  uint32_t b = AllocSlot();
  cel_map_create(a, /*capacity=*/4);
  cel_map_create(b, /*capacity=*/4);
  for (int i = 0; i < 4; ++i) {
    char key_buf[4] = {'k', static_cast<char>('0' + i), '\0', '\0'};
    uint32_t ka = cel_make_string(key_buf, 2u);
    uint32_t va = cel_make_int(100 + i);
    cel_map_insert(a, ka, va);
    uint32_t kb = cel_make_string(key_buf, 2u);
    uint32_t vb = cel_make_int(100 + i);
    cel_map_insert(b, kb, vb);
  }
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_eq(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapEqDispatchArena);

// ============================================================
// 3VL kernels (cel_3vl.c).
// ============================================================

void BM_AndBoolBool(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_bool(1);
  uint32_t b = cel_make_bool(0);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_and(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_AndBoolBool);

void BM_OrBoolBool(benchmark::State& state) {
  ResetArena();
  uint32_t a = cel_make_bool(0);
  uint32_t b = cel_make_bool(1);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_or(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_OrBoolBool);

// `cel_unknown_merge` allocates a fresh descriptor + ids array per
// call.  Per-call arena bumps are what we're really measuring; reset
// the arena every iteration so the test never OOMs (bump cursor moves
// otherwise).  This makes the timing include the arena bump cost —
// which is the right thing for this kernel, the merge IS arena
// allocation in practice.
void BM_UnknownMerge(benchmark::State& state) {
  // Build the two operand UnknownSets once.  They live before the
  // per-iteration reset cursor, so re-pointing arena_base preserves
  // them.
  ResetArena();
  // Mint two single-id UnknownSets.  Wire shape per cel_3vl.h:
  // payload.unk is a u32 byte-offset to {ids_off, len}; ids_off points
  // at a contiguous u32 array.
  auto mint_unk = [](uint32_t id) -> uint32_t {
    uint32_t ids_off = cel_alloc(sizeof(uint32_t));
    *reinterpret_cast<uint32_t*>(cel_mem_base() + ids_off) = id;
    uint32_t desc_off = cel_alloc(2 * sizeof(uint32_t));
    auto* desc = reinterpret_cast<uint32_t*>(cel_mem_base() + desc_off);
    desc[0] = ids_off;
    desc[1] = 1;
    uint32_t slot = AllocSlot();
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_UNKNOWN;
    v->payload.unk = desc_off;
    return slot;
  };
  uint32_t a = mint_unk(7);
  uint32_t b = mint_unk(11);
  // Snapshot the arena cursor AFTER staging operands; each iteration
  // rewinds back to here, so operand bytes stay valid but the merge
  // allocation gets reclaimed.  cel_reset takes (base, limit) — we
  // re-bump from the post-stage cursor.  Since cel_alloc reads the
  // cursor from bytes 8..12, we capture it directly.
  uint32_t post_stage_cursor = *reinterpret_cast<uint32_t*>(cel_mem_base() + 8);
  uint32_t out = AllocSlot();
  // After OUT alloc the cursor moves again — capture that as the
  // rewind point so OUT stays valid across iterations.
  uint32_t rewind_cursor = *reinterpret_cast<uint32_t*>(cel_mem_base() + 8);
  (void)post_stage_cursor;
  for (auto _ : state) {
    *reinterpret_cast<uint32_t*>(cel_mem_base() + 8) = rewind_cursor;
    cel_unknown_merge(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_UnknownMerge);

// ============================================================
// Conversion kernels (cel_convert.c — shipped M10).
// ============================================================

void BM_UintToInt(benchmark::State& state) {
  ResetArena();
  uint32_t in = cel_make_uint(42u);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_uint_to_int_at_v(out, in);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_UintToInt);

void BM_DoubleToInt(benchmark::State& state) {
  ResetArena();
  uint32_t in = cel_make_double(3.14);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_double_to_int_at_v(out, in);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_DoubleToInt);

// String-parse hot path.  Bench arg = digit count; the parser walks
// the input byte-by-byte so cost scales with N.  1 / 5 / 19 digit
// buckets cover (a) the trivial single-digit case, (b) the medium
// case the conformance corpus hits most, (c) the worst-case
// INT64_MAX-width parse the langdef admits.
void BM_StringToInt(benchmark::State& state) {
  const int64_t n = state.range(0);
  ResetArena();
  std::string digits(static_cast<size_t>(n), '1');
  // Make the input parseable: high digit is non-zero (no leading-zero
  // reject), the rest are filler.  19 digits = `1111111111111111111`
  // which fits in int64.
  uint32_t in = cel_make_string(digits.data(), static_cast<uint32_t>(n));
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_string_to_int_at_v(out, in);
    benchmark::DoNotOptimize(out);
  }
  state.SetBytesProcessed(state.iterations() * n);
}
BENCHMARK(BM_StringToInt)->Arg(1)->Arg(5)->Arg(19);

void BM_IntToString(benchmark::State& state) {
  // itoa + arena alloc per call.  Rewind the cursor every iteration
  // (same trick as BM_UnknownMerge) so the arena doesn't fill.
  ResetArena();
  uint32_t in = cel_make_int(123456789);
  uint32_t out = AllocSlot();
  uint32_t rewind = *reinterpret_cast<uint32_t*>(cel_mem_base() + 8);
  for (auto _ : state) {
    *reinterpret_cast<uint32_t*>(cel_mem_base() + 8) = rewind;
    cel_int_to_string_at_v(out, in);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_IntToString);

void BM_DoubleToString(benchmark::State& state) {
  ResetArena();
  uint32_t in = cel_make_double(3.14159265358979);
  uint32_t out = AllocSlot();
  uint32_t rewind = *reinterpret_cast<uint32_t*>(cel_mem_base() + 8);
  for (auto _ : state) {
    *reinterpret_cast<uint32_t*>(cel_mem_base() + 8) = rewind;
    cel_double_to_string_at_v(out, in);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_DoubleToString);

// ============================================================
// String ops (cel_string_ops.c).
// ============================================================

void BM_StringConcat(benchmark::State& state) {
  const int64_t n = state.range(0);
  ResetArena();
  std::string s(static_cast<size_t>(n), 'x');
  uint32_t a = cel_make_string(s.data(), static_cast<uint32_t>(n));
  uint32_t b = cel_make_string(s.data(), static_cast<uint32_t>(n));
  uint32_t out = AllocSlot();
  uint32_t rewind = *reinterpret_cast<uint32_t*>(cel_mem_base() + 8);
  for (auto _ : state) {
    *reinterpret_cast<uint32_t*>(cel_mem_base() + 8) = rewind;
    cel_string_concat_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
  state.SetBytesProcessed(state.iterations() * 2 * n);
}
BENCHMARK(BM_StringConcat)->Arg(8)->Arg(64)->Arg(4096);

// Substring search — small haystack with a 3-byte needle near the
// end is a typical workload shape (`s.contains("foo")` on a label
// string).  Bench arg = haystack size; needle is fixed length.
void BM_StringContains(benchmark::State& state) {
  const int64_t n = state.range(0);
  ResetArena();
  std::string hay(static_cast<size_t>(n), 'a');
  // Plant the needle at the tail so the scan walks the whole string
  // before hitting.  Worst-case search cost.
  if (n >= 3) {
    hay[static_cast<size_t>(n) - 3] = 'f';
    hay[static_cast<size_t>(n) - 2] = 'o';
    hay[static_cast<size_t>(n) - 1] = 'o';
  }
  uint32_t a = cel_make_string(hay.data(), static_cast<uint32_t>(n));
  uint32_t needle = cel_make_string("foo", 3u);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_string_contains_at_vv(out, a, needle);
    benchmark::DoNotOptimize(out);
  }
  state.SetBytesProcessed(state.iterations() * n);
}
BENCHMARK(BM_StringContains)->Arg(8)->Arg(64)->Arg(4096);

}  // namespace
}  // namespace celwasm

BENCHMARK_MAIN();
