// Runtime kernel microbenches — deliverable A of the post-M10 bench
// suite.  Each BENCHMARK function micro-measures one load-bearing kernel
// out of `runtime/cel_*.c`, with operands staged once outside
// the hot loop and the arena pre-reset so the timed window is just the
// kernel call.
//
// Coverage (one BM per runtime kernel family):
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
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "benchmark/benchmark.h"
#include "runtime/cel_3vl.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_arith.h"
#include "runtime/cel_compare.h"
#include "runtime/cel_convert.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_list.h"
#include "runtime/cel_make.h"
#include "runtime/cel_map.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_ops.h"

// NOLINTBEGIN(clang-analyzer-deadcode.DeadStores)
// `for (auto _ : state)` is the standard Google Benchmark loop
// idiom — the loop variable is intentionally unused, the loop
// body's side effects (the kernel under test) are the
// observation.  The clang-static-analyzer's dead-store warning
// fires on every benchmark in the file; suppressed file-level
// here since the idiom is uniform across the suite.

namespace celwasm {
namespace {

// Reset the arena to its default capacity.  Mirrors the SetUp()
// shape every runtime *_test.cc uses post-M5 — arena_init is
// idempotent for same-cap; arena_reset rewinds the cursor.
void ResetArena() {
  arena_init(CELWASM_ARENA_CAPACITY_BYTES);
  arena_reset();
}

// Allocate a fresh out-slot CelValue inside the arena.
uint32_t AllocSlot() {
  return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
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
  cel_list_create(l, /*capacity=*/8);
  for (int i = 0; i < 8; ++i) {
    uint32_t v = cel_make_int(static_cast<int64_t>(i) * 10);
    cel_list_append_at(l, v);
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
  cel_list_create(a, /*capacity=*/4);
  cel_list_create(b, /*capacity=*/4);
  for (int i = 0; i < 4; ++i) {
    uint32_t va = cel_make_int(i);
    uint32_t vb = cel_make_int(i);
    cel_list_append_at(a, va);
    cel_list_append_at(b, vb);
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
// SwissTable map-index crossover sweep (cel_map_index.c).
//
// Race the index probe against the linear scan over a map-size sweep.
// state.range(0) = entry count N; state.range(1) = MapPath (0=linear,
// 1=swiss).  The crossover N* is the smallest swept N where the swiss
// arm beats the linear arm; read it off the linear/swiss ratio down
// each N column.  Pins kCelMapIndexThreshold (design §10); see
// benchmark/m32-swisstable-bench-plan.md §2.
//
// A/B seam (plan §3.1 / gap G1): `cel_map_create` + `cel_map_insert`
// builds a dense-entries map with `index_offset == 0` (every keyed
// kernel linear-scans).  The kSwiss arm calls the TERMINAL
// `cel_map_index_build`; the kLinear arm skips it.  `cel_map_index_build`
// itself gates on `count >= kCelMapIndexThreshold` (cel_map_index.c),
// so below the threshold the swiss arm equals the linear arm (no index
// is published) — that gate IS the production behaviour, and the sweep
// measures it as-shipped rather than forcing a sub-threshold index.
// ============================================================

enum class MapPath : uint8_t { kLinear = 0, kSwiss = 1 };

// Build an N-entry int-keyed arena map, keys 0..N-1 in insertion order.
// kSwiss → call cel_map_index_build (gated at kCelMapIndexThreshold);
// kLinear → leave index_offset == 0.  Returns the map slot.
uint32_t BuildIntMap(int64_t n, MapPath path) {
  uint32_t m = AllocSlot();
  cel_map_create(m, static_cast<uint32_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    uint32_t k = cel_make_int(i);
    uint32_t v = cel_make_int(i * 10);
    cel_map_insert(m, k, v);
  }
  if (path == MapPath::kSwiss) cel_map_index_build(m);
  return m;
}

// N-entry string-keyed map, keys "key0000".."keyNNNN" (7 bytes each,
// distinct).  Same A/B seam as BuildIntMap.
uint32_t BuildStrMap(int64_t n, MapPath path) {
  uint32_t m = AllocSlot();
  cel_map_create(m, static_cast<uint32_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    char key_buf[8];
    std::snprintf(key_buf, sizeof(key_buf), "key%04d",
                  static_cast<int>(i));  // 7 chars + NUL
    uint32_t k = cel_make_string(key_buf, 7u);
    uint32_t v = cel_make_int(i * 10);
    cel_map_insert(m, k, v);
  }
  if (path == MapPath::kSwiss) cel_map_index_build(m);
  return m;
}

// The size sweep covers below / at / above every candidate threshold
// out to the multi-group-probe regime.  Plan §1.1 lists N up to 1024,
// but the native bench arena is fixed at CELWASM_ARENA_CAPACITY_BYTES
// (64 KiB; cel_layout.h): an N=1024 int map alone needs ~98 KiB
// (2N CelValue keys+values + an N×48 B entries run) and OOMs, and the
// equality benches build two maps.  256 is the largest N that fits both
// a single map and a two-map equality pair, and it spans the crossover
// + asymptotic regimes the threshold decision needs.
#define CEL_IDX_SWEEP ->ArgsProduct({{2, 4, 8, 16, 32, 64, 128, 256}, {0, 1}})

// ── lookup: hit at the middle of the insertion run (avg linear cost) ──
void BM_MapLookupArenaHit_Int(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  uint32_t m = BuildIntMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t key = cel_make_int(n / 2);  // middle-of-run hit
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_lookup_arena(out, m, key);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapLookupArenaHit_Int) CEL_IDX_SWEEP;

void BM_MapLookupArenaHit_Str(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  uint32_t m = BuildStrMap(n, static_cast<MapPath>(state.range(1)));
  char mid[8];
  std::snprintf(mid, sizeof(mid), "key%04d", static_cast<int>(n / 2));
  uint32_t key = cel_make_string(mid, 7u);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_lookup_arena(out, m, key);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapLookupArenaHit_Str) CEL_IDX_SWEEP;

// ── lookup miss: linear-scan worst case (full N compares) → widest gap ─
void BM_MapLookupArenaMiss_Int(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  uint32_t m = BuildIntMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t key = cel_make_int(n + 1);  // never present
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_lookup_arena(out, m, key);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapLookupArenaMiss_Int) CEL_IDX_SWEEP;

void BM_MapLookupArenaMiss_Str(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  uint32_t m = BuildStrMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t key = cel_make_string("zzzzzzz", 7u);  // never present
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_lookup_arena(out, m, key);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapLookupArenaMiss_Str) CEL_IDX_SWEEP;

// ── membership (`k in m`): same index path as lookup ──
void BM_MapInArenaHit_Int(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  uint32_t m = BuildIntMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t key = cel_make_int(n / 2);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_in_arena(out, key, m);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapInArenaHit_Int) CEL_IDX_SWEEP;

void BM_MapInArenaMiss_Int(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  uint32_t m = BuildIntMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t key = cel_make_int(n + 1);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_in_arena(out, key, m);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapInArenaMiss_Int) CEL_IDX_SWEEP;

// ── map equality: O(n^2) linear inner match → O(n) indexed.  The bend
// from quadratic to linear is the marquee result.  Operand `a` is the
// outer walk (never indexed); operand `b` is the inner-matched side
// whose index the kSwiss arm toggles. ──
void BM_MapEqArena_Int(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  auto path = static_cast<MapPath>(state.range(1));
  uint32_t a = BuildIntMap(n, MapPath::kLinear);  // outer walk
  uint32_t b = BuildIntMap(n, path);              // inner-matched
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_eq_arena(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapEqArena_Int) CEL_IDX_SWEEP;

void BM_MapEqArena_Str(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  auto path = static_cast<MapPath>(state.range(1));
  uint32_t a = BuildStrMap(n, MapPath::kLinear);
  uint32_t b = BuildStrMap(n, path);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_eq_arena(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MapEqArena_Str) CEL_IDX_SWEEP;

// ── N-lookups loop: the comprehension proxy with no compile-path
// dependency (plan gap G3).  One timed iteration probes all N keys —
// O(N^2) over a linear map, O(N) over an indexed one.  The inner loop
// mints N keys per outer iteration; rewind the arena cursor each
// iteration (same trick as BM_UnknownMerge) so the mints don't
// accumulate and OOM. ──
void BM_MapLookupLoop_Int(benchmark::State& state) {
  ResetArena();
  const int64_t n = state.range(0);
  uint32_t m = BuildIntMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t out = AllocSlot();
  uint32_t rewind = *reinterpret_cast<uint32_t*>(cel_mem_base() + 8);
  for (auto _ : state) {
    *reinterpret_cast<uint32_t*>(cel_mem_base() + 8) = rewind;
    for (int64_t i = 0; i < n; ++i) {
      cel_map_lookup_arena(out, m, cel_make_int(i));
      benchmark::DoNotOptimize(out);
    }
  }
}
BENCHMARK(BM_MapLookupLoop_Int)
    ->ArgsProduct({{8, 16, 32, 64, 128, 256}, {0, 1}});

#undef CEL_IDX_SWEEP

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
    uint32_t ids_off = arena_alloc(sizeof(uint32_t));
    *reinterpret_cast<uint32_t*>(cel_mem_base() + ids_off) = id;
    uint32_t desc_off = arena_alloc(2 * sizeof(uint32_t));
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
  // allocation gets reclaimed.  arena_reset takes (base, limit) — we
  // re-bump from the post-stage cursor.  Since arena_alloc reads the
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
BENCHMARK(BM_StringContains)->Arg(8)->Arg(64)->Arg(4096)->Arg(16384);

// Adversarial anchor-common shape: haystack is all 'f', needle "foo"
// is absent — EVERY byte is a false anchor candidate, so this
// measures the verify-loop overhead per candidate rather than raw
// scan throughput (BM_StringContains above, where the anchor byte is
// absent until the tail, measures the opposite extreme).  A search
// rewrite that wins the scan but regresses candidate handling shows
// up here and not there.
//
// Cross-target caveat: this binary links the NATIVE runtime, where
// the pre-SWAR baseline was the host libc's vectorized memchr — so
// native numbers can move differently from the production wasm
// kernel (pinned by the eval-level `long_strings/containsLong_N`
// cells, where wasm32 musl memchr was the baseline).
void BM_StringContainsAnchorCommon(benchmark::State& state) {
  const int64_t n = state.range(0);
  ResetArena();
  std::string hay(static_cast<size_t>(n), 'f');
  uint32_t a = cel_make_string(hay.data(), static_cast<uint32_t>(n));
  uint32_t needle = cel_make_string("foo", 3u);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_string_contains_at_vv(out, a, needle);
    benchmark::DoNotOptimize(out);
  }
  state.SetBytesProcessed(state.iterations() * n);
}
BENCHMARK(BM_StringContainsAnchorCommon)->Arg(64)->Arg(4096)->Arg(16384);

// ============================================================
// M7B duration / timestamp microbenches.
//
// Coverage (per `doc/implementation-plan/rewrite/m7b-duration-timestamp.md`
// §11):
//
//   - Arithmetic     — BM_DurationAdd, BM_DurationSub,
//                      BM_TimestampSubTimestamp, BM_TimestampAddDuration.
//                      Compare against BM_IntAdd above; duration arith
//                      should land within ~2× of int arith (one extra
//                      carry + overflow check).
//   - Accessor       — BM_TimestampYearUtc, BM_TimestampYearUtcLangdefMax,
//                      BM_TimestampDayOfWeekUtc, BM_DurationHours.
//                      Pure-wasm civil-calendar walk; the hot-path
//                      perf budget for any expr that chains
//                      `.getYear()` / `.getMonth()` etc.  Expected
//                      cost: ~5-10× a numeric kernel call because of
//                      the integer-divide cascade in
//                      cel_civil_from_seconds.
//   - Host trampoline — BM_TimestampParseHost.  Belongs in
//                      pipeline_bench.cc shape (Compile + Plan + Eval).
//                      Sketch only — pipeline scaffold lands when
//                      M7B.D ships.
//
// Today (M7B not yet shipped) the kernels are guarded behind
// `CELWASM_M7B_SHIPPED`.  The bench file compiles green so the BUILD
// target validates; the actual benches turn on when the helpers land.
// ============================================================

// Stage a CEL_DURATION CelValue into the arena, returning its offset.
// Returns 0 today (no slot allocated) when M7B not shipped — the
// benches that consume this are themselves guarded.
[[maybe_unused]] uint32_t MakeDuration(int64_t seconds, int32_t nanos) {
#ifdef CELWASM_M7B_SHIPPED
  uint32_t off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* v = reinterpret_cast<CelValue*>(
      reinterpret_cast<uint8_t*>(cel_mem_base()) + off);
  v->kind = CEL_DURATION;
  v->_pad = 0;
  v->payload.dur.seconds = seconds;
  v->payload.dur.nanos = nanos;
  v->payload.dur._pad = 0;
  return off;
#else
  (void)seconds;
  (void)nanos;
  return 0u;
#endif
}

[[maybe_unused]] uint32_t MakeTimestamp(int64_t seconds, int32_t nanos) {
#ifdef CELWASM_M7B_SHIPPED
  uint32_t off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* v = reinterpret_cast<CelValue*>(
      reinterpret_cast<uint8_t*>(cel_mem_base()) + off);
  v->kind = CEL_TIMESTAMP;
  v->_pad = 0;
  v->payload.ts.seconds = seconds;
  v->payload.ts.nanos = nanos;
  v->payload.ts._pad = 0;
  return off;
#else
  (void)seconds;
  (void)nanos;
  return 0u;
#endif
}

#ifdef CELWASM_M7B_SHIPPED

void BM_DurationAdd(benchmark::State& state) {
  ResetArena();
  uint32_t a = MakeDuration(60, 500'000'000);
  uint32_t b = MakeDuration(120, 750'000'000);  // exercises nanos-carry
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_dur_add_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_DurationAdd);

void BM_DurationSub(benchmark::State& state) {
  ResetArena();
  uint32_t a = MakeDuration(120, 0);
  uint32_t b = MakeDuration(60, 1);  // exercises borrow
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_dur_sub_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_DurationSub);

void BM_TimestampSubTimestamp(benchmark::State& state) {
  ResetArena();
  uint32_t a = MakeTimestamp(1234567950, 0);
  uint32_t b = MakeTimestamp(1234567890, 0);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_ts_ts_sub_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_TimestampSubTimestamp);

void BM_TimestampAddDuration(benchmark::State& state) {
  ResetArena();
  uint32_t a = MakeTimestamp(1234567890, 0);
  uint32_t b = MakeDuration(60, 0);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_ts_dur_add_at_vv(out, a, b);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_TimestampAddDuration);

void BM_TimestampYearUtc(benchmark::State& state) {
  ResetArena();
  uint32_t ts = MakeTimestamp(1234567890, 0);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_ts_year_utc(out, ts);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_TimestampYearUtc);

void BM_TimestampYearUtcLangdefMax(benchmark::State& state) {
  ResetArena();
  uint32_t ts = MakeTimestamp(253402300799LL, 0);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_ts_year_utc(out, ts);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_TimestampYearUtcLangdefMax);

void BM_TimestampDayOfWeekUtc(benchmark::State& state) {
  ResetArena();
  uint32_t ts = MakeTimestamp(1234567890, 0);
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_ts_day_of_week_utc(out, ts);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_TimestampDayOfWeekUtc);

void BM_DurationHours(benchmark::State& state) {
  ResetArena();
  uint32_t d = MakeDuration(36000, 0);  // 10h
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_dur_hours(out, d);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_DurationHours);

#endif  // CELWASM_M7B_SHIPPED

// BM_TimestampParseHost belongs in pipeline_bench.cc — host trampolines
// only reach through wasmtime, so the bench needs Compile+Plan+Eval
// pre-staged.  Sketched in m7b-duration-timestamp.md §11; lands when
// M7B.D ships.

// ============================================================
// M7-A google.protobuf.Any pack/unpack/equality scaffolds.
//
// Every BM_AnyPack_* / BM_AnyUnpack_* / BM_AnyEq_* below is gated by
// `kM7aShipped = false` — they `state.SkipWithError` until M7-A.A/B/C
// ship.  The doc (m7a-any.md §11.3) lists expected baselines vs
// deltas; flip the flag and the suite produces real numbers.  The
// BM_AnyTypeUrlParse_* benches are pure kernels that run today (they
// only exercise the string-slice rfind('/') logic the unpack path
// uses).
//
// Rationale for collocating with the other kernel microbenches:
// every BM_* here measures a single tight call to a runtime/host
// kernel.  Pipeline-shaped Any benches (Compile+Plan+Eval of an
// expression that pack-unpacks) belong in pipeline_bench.cc when
// M7-A ships.
// ============================================================

constexpr bool kM7aShipped = false;
constexpr const char* kAnyNotShippedMsg =
    "M7-A not yet shipped — scaffold bench, see m7a-any.md §11.3.";

void BM_AnyPack_SingularField_Reflection(benchmark::State& state) {
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  // When M7-A.A ships: stage a default outer + `Bar{x:42}` source,
  // call CelSetFieldImpl pack op (SerializeAsString + 2x SetString).
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyPack_SingularField_Reflection);

void BM_AnyPack_SingularField_TypedCast(benchmark::State& state) {
  // Comparand: typed Any::PackFrom path.  Skips the per-call
  // FindFieldByName.  Probe A finding (m7a-any.md §10.1): identical
  // output bytes; perf delta is what this bench will reveal.
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyPack_SingularField_TypedCast);

void BM_AnyPack_SingularField_BaselineCopyFrom(benchmark::State& state) {
  // Baseline: M7-shipped CopyFrom on a non-Any message field.  Delta
  // against BM_AnyPack_SingularField_Reflection = pack overhead.
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyPack_SingularField_BaselineCopyFrom);

void BM_AnyUnpack_SingularRead(benchmark::State& state) {
  // ProtoBacking::ReadField on a pre-packed Any field.  Hot ops:
  // FindMessageTypeByName + GetPrototype + ParseFromString.
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyUnpack_SingularRead);

void BM_AnyUnpack_SingularRead_BaselineNonAny(benchmark::State& state) {
  // Baseline: M2.C-shipped ReadField on a non-Any message field.
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyUnpack_SingularRead_BaselineNonAny);

void BM_AnyUnpack_RepeatedAnyForEach(benchmark::State& state) {
  // Walk a 10-element repeated-of-Any; each element unwraps to a
  // different typed message.
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyUnpack_RepeatedAnyForEach);

void BM_AnyEq_AnyVsTyped(benchmark::State& state) {
  // cel_message_eq with one Any operand — single-side peel.
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyEq_AnyVsTyped);

void BM_AnyEq_AnyVsAny(benchmark::State& state) {
  // Both-side peel.  Expected ~2x unpack cost + 1x MessageDifferencer.
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyEq_AnyVsAny);

void BM_AnyEq_BaselineNonAny(benchmark::State& state) {
  // Baseline: M5.B step 2b's cel_message_eq on non-Any operands.
  if (!kM7aShipped) {
    state.SkipWithError(kAnyNotShippedMsg);
    return;
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_AnyEq_BaselineNonAny);

// TypeUrl parse — pure kernels that run today (no production-code
// dependency).  Probe D in m7a-any.md §10.4 reports 10.6 ns
// happy-path / 3.64 ns no-slash on opt build.
absl::string_view AnyTypeUrlFqn(absl::string_view type_url) {
  auto slash = type_url.rfind('/');
  if (slash == absl::string_view::npos) return type_url;
  return type_url.substr(slash + 1);
}

void BM_AnyTypeUrlParse_HappyPath(benchmark::State& state) {
  const std::string url =
      "type.googleapis.com/cel.expr.conformance.proto3.TestAllTypes";
  for (auto _ : state) {
    benchmark::DoNotOptimize(AnyTypeUrlFqn(url));
  }
}
BENCHMARK(BM_AnyTypeUrlParse_HappyPath);

void BM_AnyTypeUrlParse_NoSlash(benchmark::State& state) {
  const std::string url = "TestAllTypes";
  for (auto _ : state) {
    benchmark::DoNotOptimize(AnyTypeUrlFqn(url));
  }
}
BENCHMARK(BM_AnyTypeUrlParse_NoSlash);

}  // namespace
}  // namespace celwasm
// NOLINTEND(clang-analyzer-deadcode.DeadStores)

BENCHMARK_MAIN();
