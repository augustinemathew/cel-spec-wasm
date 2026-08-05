// Adversarial matrix for the host→wasm activation-marshal boundary:
// every variable-length Value kind crossing Activation::Bind → Eval,
// swept across the capacity boundaries of the two copy regions:
//
//   - the per-Instance ACTIVATION BUFFER — kString / kBytes payloads
//     are copy-marshaled into a dlmalloc'd buffer inside linear
//     memory at Eval time (eval/instance.cc `EncodeStringOrBytes` /
//     `EnsureActivationBuffer`); the buffer grows on demand, so the
//     graceful-failure shape is `ResourceExhausted` when dlmalloc
//     can't grow linear memory.
//   - the CHAINED, GROW-ON-DEMAND EVAL ARENA (runtime/cel_arena.c) —
//     list / map / message values are HANDLE-PASSED at Bind time
//     (interned into the per-Instance `ExternrefTable` as
//     CEL_LIST_HOST / CEL_MAP_HOST / CEL_MESSAGE; no payload bytes
//     cross), but trampolines that materialize an ELEMENT wasm-side
//     (`cel_list_at`, `cel_map_lookup` via `EncodeSpan` in
//     eval/internal/cel_host.cc) copy string payloads into the arena.
//     The arena seeds a 64 KiB first chunk
//     (`CELWASM_ARENA_CAPACITY_BYTES`) and, when an allocation
//     overflows the current chunk, mallocs a NEW chunk sized to fit
//     it (chained, post-ssp-fix) — so an element far larger than the
//     seed chunk now copies in soundly and yields the correct value.
//     Growth is NOT unbounded: each grow goes through malloc inside
//     the wasm linear memory, so a single element large enough to
//     exhaust dlmalloc / the linear-memory ceiling returns a GRACEFUL
//     non-OK Status — `arena_alloc` returns 0 → EncodeSpan emits
//     `ResourceExhausted("arena OOM in CelMapLookupImpl")`, surfacing
//     as a non-OK Status from Eval, never a process abort.  The
//     empirically-measured boundary (both link modes) is ~64 MiB for
//     a single element: ≤60 MiB succeeds, ≥64 MiB fails gracefully
//     (the `*Graceful*` cells below pin it).
//
// Copy-marshaled at Bind:  string, bytes (and type names).
// Handle-passed at Bind:   list<*>, map<*,*>, proto message.
//   (Proto messages cross as an interned `HostMessageBacking` slot —
//   `EncodeMessage` in eval/instance.cc — with zero payload bytes
//   copied at Bind, so per the boundary-matrix scope they get no
//   size sweep here; field reads route through `cel_get_field`
//   whose arena-copy shape is the same `EncodeSpan` path the
//   list/map element cells below pin.)
//
// Every cell asserts one of:
//   - the correct Eval result (the size fits the region, or no copy
//     happens on that path), or
//   - a graceful non-OK absl::Status (the copy region is exhausted
//     and the failure is reported, not a wild write).
// A cell that instead aborts the process (e.g. a host-side write
// past the shared linear memory → wasmtime `fault.is_none()` panic)
// is the bug this suite exists to catch; such a cell gets pinned
// with GTEST_SKIP naming the verified signature.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// ───────────────────────── size constants ─────────────────────────
// Region boundaries under test (see file header):
//   activation buffer — grows on demand (4 KiB-rounded dlmalloc)
//   eval arena        — chained, grow-on-demand (runtime/cel_arena.c);
//                       64 KiB seed chunk, mallocs follow-on chunks to
//                       fit larger allocations up to the wasm
//                       linear-memory ceiling (~64 MiB single element)
//   initial memory    — 2 pages = 128 KiB (cel_layout.h), dlmalloc
//                       grows past it on demand
constexpr size_t kOneByte = 1;
constexpr size_t kComfort = size_t{1} * 1024;           // ~1 KiB
constexpr size_t kJustUnderArena = size_t{60} * 1024;   // < 64 KiB arena
constexpr size_t kJustOverArena = size_t{70} * 1024;    // > 64 KiB arena
constexpr size_t kJustOverMemory = size_t{150} * 1024;  // > 128 KiB initial
constexpr size_t kGross = size_t{10} * 1024 * 1024;     // 10 MiB
constexpr int kGrossListCount = 100000;                 // ~100K elements

// Deterministic ASCII payload (so size(string) == byte length ==
// codepoint count, immune to the size()-counts-bytes known bug).
std::string AsciiPayload(size_t n) {
  static constexpr absl::string_view kCycle = "0123456789abcdef";
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(kCycle[i % kCycle.size()]);
  }
  return s;
}

std::vector<Value> IntElements(int n) {
  std::vector<Value> v;
  v.reserve(n);
  for (int i = 0; i < n; ++i) {
    v.push_back(Value::Int(i));
  }
  return v;
}

// Full pipeline with declared + bound variables, returning the raw
// StatusOr so graceful-failure cells can assert on the Status.
absl::StatusOr<Value> EvalBound(
    absl::string_view source,
    const std::vector<std::pair<std::string, CelType>>& decls,
    const std::vector<std::pair<std::string, Value>>& binds) {
  Compiler::Builder b;
  for (const auto& [name, type] : decls) {
    b.DeclareVariable(name, type);
  }
  auto compiler = std::move(b).Build();
  if (!compiler.ok()) return compiler.status();
  auto program = compiler->Compile(source, e2e::DefaultOpts());
  if (!program.ok()) return program.status();
  auto instance = e2e::GlobalEngine().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a;
  for (const auto& [name, value] : binds) {
    a.Bind(name, value);
  }
  return instance->Eval(a);
}

// Assert the cell evaluated to exactly `want` (int result).
void ExpectIntCell(absl::string_view source,
                   const std::vector<std::pair<std::string, CelType>>& decls,
                   const std::vector<std::pair<std::string, Value>>& binds,
                   int64_t want) {
  auto v = EvalBound(source, decls, binds);
  ASSERT_THAT(v, IsOk()) << source;
  ASSERT_EQ(v->kind(), Value::Kind::kInt)
      << source << " kind=" << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), want) << source;
}

// Assert the cell evaluated to exactly `want` (bool result).
void ExpectBoolCell(absl::string_view source,
                    const std::vector<std::pair<std::string, CelType>>& decls,
                    const std::vector<std::pair<std::string, Value>>& binds,
                    bool want) {
  auto v = EvalBound(source, decls, binds);
  ASSERT_THAT(v, IsOk()) << source;
  ASSERT_EQ(v->kind(), Value::Kind::kBool)
      << source << " kind=" << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsBool(), want) << source;
}

// Assert the cell failed GRACEFULLY: a non-OK Status (NOT a process
// abort, NOT a silent wrong value).  The copy region is exhausted
// and the failure is reported through the Status channel.  With the
// chained grow-on-demand arena (post-ssp-fix) this only fires when an
// allocation exceeds the wasm linear-memory ceiling (the arena's
// malloc fails → arena_alloc returns 0).  Observed signature for the
// over-ceiling cells below (both link modes):
//   Eval (func_call): error while executing at wasm backtrace:
//   ...  arena OOM in CelMapLookupImpl
// (EncodeSpan's ResourceExhausted surfacing through
// wasmtime_func_call's error channel).
void ExpectGracefulCell(
    absl::string_view source,
    const std::vector<std::pair<std::string, CelType>>& decls,
    const std::vector<std::pair<std::string, Value>>& binds) {
  auto v = EvalBound(source, decls, binds);
  EXPECT_FALSE(v.ok()) << source << " unexpectedly produced a value of kind "
                       << (v.ok() ? static_cast<int>(v->kind()) : -1);
}

// ══════════════════════════════════════════════════════════════════
// string — COPY-MARSHALED at Bind into the activation buffer
// (eval/instance.cc EncodeStringOrBytes).  The buffer is malloc'd /
// grown per Eval, so every size below is expected to land:
// `size(s)` reads the slot's (ptr, len) without further copies.
// ══════════════════════════════════════════════════════════════════

class StringBindBoundary : public ::testing::Test {
 protected:
  static std::vector<std::pair<std::string, CelType>> Decl() {
    return {{"s", CelType::String()}};
  }
  static void ExpectSize(size_t n) {
    ExpectIntCell("size(s)", Decl(), {{"s", Value::String(AsciiPayload(n))}},
                  static_cast<int64_t>(n));
  }
};

// The activation buffer is malloc'd inside linear memory and grown per
// Eval.  A single string too large for dlmalloc to carve out fails that
// growth, surfacing as a non-OK Status from `instance.cc`'s
// `Activation[<var>]: malloc returned NULL (needed <n> bytes)` arm —
// not a crash, hang or abort.  Same graceful-ceiling contract the
// list-element case pins at ElementOverLinearMemoryGraceful; this is
// the scalar-binding twin and the only route to that arm.
//
// NOT the sibling arm just above it, which reports a wasmtime error
// from the malloc CALL rather than a NULL return: that one needs a
// trap, not an allocation failure, and stays uncovered.
TEST_F(StringBindBoundary, OverLinearMemoryGraceful) {
  ExpectGracefulCell(
      "size(s)", Decl(),
      {{"s", Value::String(AsciiPayload(size_t{64} * 1024 * 1024))}});
}

TEST_F(StringBindBoundary, Empty) {
  ExpectSize(0);
}
TEST_F(StringBindBoundary, OneByte) {
  ExpectSize(kOneByte);
}
TEST_F(StringBindBoundary, Comfort1KiB) {
  ExpectSize(kComfort);
}
TEST_F(StringBindBoundary, JustUnderArena60KiB) {
  // The activation buffer is independent of the 64 KiB eval arena;
  // this size is a tripwire for any accidental rerouting through it.
  ExpectSize(kJustUnderArena);
}
TEST_F(StringBindBoundary, JustOverArena70KiB) {
  ExpectSize(kJustOverArena);
}
TEST_F(StringBindBoundary, JustOverInitialMemory150KiB) {
  // > 128 KiB initial memory: dlmalloc must memory.grow to satisfy
  // the activation-buffer malloc.  Expected to succeed.
  ExpectSize(kJustOverMemory);
}
TEST_F(StringBindBoundary, Gross10MiB) {
  ExpectSize(kGross);
}

TEST_F(StringBindBoundary, EqualsSelfGross10MiB) {
  // Forces the full 10 MiB through the wasm-side byte compare.
  const std::string payload = AsciiPayload(kGross);
  ExpectBoolCell("s == s", Decl(), {{"s", Value::String(payload)}}, true);
}

TEST_F(StringBindBoundary, ConcatOfBoundFitsGrownArena) {
  // `s + s` materializes the 66 KiB concat RESULT in the eval arena
  // (cel_string_ops.c).  66 KiB overflows the 64 KiB SEED chunk, but
  // the arena grows to fit (chained arena, post-ssp-fix): arena_alloc
  // mallocs a follow-on chunk sized for the 66 KiB result, so the
  // concat succeeds and `size(s + s)` == 2 × 33 KiB.
  const size_t half = size_t{33} * 1024;
  const std::string payload = AsciiPayload(half);
  ExpectIntCell("size(s + s)", Decl(), {{"s", Value::String(payload)}},
                static_cast<int64_t>(half) * 2);
}

TEST_F(StringBindBoundary, EmbeddedNulRoundTrip) {
  // ~1 KiB payload with NUL bytes sprinkled through it; the bare
  // `s` expression round-trips Bind-marshal → slot → result decode.
  std::string payload = AsciiPayload(kComfort);
  for (size_t i = 0; i < payload.size(); i += 64) {
    payload[i] = '\0';
  }
  auto v = EvalBound("s", Decl(), {{"s", Value::String(payload)}});
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), Value::Kind::kString);
  EXPECT_EQ(*v->AsString(), payload);
}

TEST_F(StringBindBoundary, MultiByteUtf8AtBufferBoundary) {
  // Multi-byte UTF-8 sequences ending exactly at the payload edge.
  // Round-trip + self-equality; size() is avoided (known bytes-vs-
  // codepoints divergence, see known_bugs_test.cc).
  std::string payload = AsciiPayload(kComfort - 8);
  payload += "\xC3\xA9";          // é (2 bytes)
  payload += "\xE2\x82\xAC";      // € (3 bytes)
  payload += "\xF0\x9F\x98\x80";  // 😀 (4 bytes) — last byte at edge
  auto v = EvalBound("s", Decl(), {{"s", Value::String(payload)}});
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), Value::Kind::kString);
  EXPECT_EQ(*v->AsString(), payload);
  ExpectBoolCell("s == s", Decl(), {{"s", Value::String(payload)}}, true);
}

// ══════════════════════════════════════════════════════════════════
// bytes — COPY-MARSHALED at Bind, same activation-buffer path as
// string (EncodeStringOrBytes with Repr::kBytes).
// ══════════════════════════════════════════════════════════════════

class BytesBindBoundary : public ::testing::Test {
 protected:
  static std::vector<std::pair<std::string, CelType>> Decl() {
    return {{"b", CelType::Bytes()}};
  }
  static void ExpectSize(size_t n) {
    ExpectIntCell("size(b)", Decl(), {{"b", Value::Bytes(AsciiPayload(n))}},
                  static_cast<int64_t>(n));
  }
};

TEST_F(BytesBindBoundary, Empty) {
  ExpectSize(0);
}
TEST_F(BytesBindBoundary, OneByte) {
  ExpectSize(kOneByte);
}
TEST_F(BytesBindBoundary, Comfort1KiB) {
  ExpectSize(kComfort);
}
TEST_F(BytesBindBoundary, JustUnderArena60KiB) {
  ExpectSize(kJustUnderArena);
}
TEST_F(BytesBindBoundary, JustOverArena70KiB) {
  ExpectSize(kJustOverArena);
}
TEST_F(BytesBindBoundary, JustOverInitialMemory150KiB) {
  ExpectSize(kJustOverMemory);
}
TEST_F(BytesBindBoundary, Gross10MiB) {
  ExpectSize(kGross);
}

TEST_F(BytesBindBoundary, EmbeddedNulRoundTrip) {
  std::string payload = AsciiPayload(kComfort);
  for (size_t i = 0; i < payload.size(); i += 64) {
    payload[i] = '\0';
  }
  auto v = EvalBound("b", Decl(), {{"b", Value::Bytes(payload)}});
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), Value::Kind::kBytes);
  EXPECT_EQ(*v->AsBytes(), payload);
}

// ══════════════════════════════════════════════════════════════════
// list<int> — HANDLE-PASSED at Bind (EncodeList interns the
// HostListBacking; CEL_LIST_HOST ref_slot).  `size(xs)` and
// `_ in xs` stay host-side, so element count never touches the
// arena on these paths — every count is expected to succeed.
// ══════════════════════════════════════════════════════════════════

class ListIntBindBoundary : public ::testing::Test {
 protected:
  static std::vector<std::pair<std::string, CelType>> Decl() {
    return {{"xs", CelType::List(CelType::Int())}};
  }
  static void ExpectSize(int n) {
    ExpectIntCell("size(xs)", Decl(), {{"xs", Value::List(IntElements(n))}}, n);
  }
};

TEST_F(ListIntBindBoundary, Empty) {
  ExpectSize(0);
}
TEST_F(ListIntBindBoundary, OneElement) {
  ExpectSize(1);
}
TEST_F(ListIntBindBoundary, Comfort43Elements) {
  ExpectSize(43);
}  // ~1 KiB
TEST_F(ListIntBindBoundary, JustUnderArena2560Elements) {
  ExpectSize(2560);  // 2560 × 24 B ≈ 60 KiB if it were arena-copied
}
TEST_F(ListIntBindBoundary, JustOverArena2990Elements) {
  ExpectSize(2990);  // 2990 × 24 B ≈ 70 KiB if it were arena-copied
}
TEST_F(ListIntBindBoundary, JustOverMemory6400Elements) {
  ExpectSize(6400);  // 6400 × 24 B ≈ 150 KiB if it were arena-copied
}
TEST_F(ListIntBindBoundary, Gross100KElements) {
  ExpectSize(kGrossListCount);
}

TEST_F(ListIntBindBoundary, MembershipGross100KElements) {
  // `_ in xs` walks the backing host-side (CelListInImpl); 42 is
  // present (elements are 0..99999).
  ExpectBoolCell("42 in xs", Decl(),
                 {{"xs", Value::List(IntElements(kGrossListCount))}}, true);
}

TEST_F(ListIntBindBoundary, IndexSingleElement) {
  // `xs[0]` routes through cel_list_at; the int element marshals
  // inline into the 24-byte out slot (no arena payload).
  ExpectIntCell("xs[0]", Decl(), {{"xs", Value::List({Value::Int(7)})}}, 7);
}

// ══════════════════════════════════════════════════════════════════
// list<string> — HANDLE-PASSED at Bind; the ELEMENT copy happens at
// `xs[0]` (cel_list_at → EncodeSpan → arena_alloc).  The arena grows
// on demand (chained, post-ssp-fix), so element sizes far over the
// 64 KiB seed chunk now copy in soundly and yield the correct value.
// Only an element large enough to exhaust the wasm linear memory
// (empirically ~64 MiB) fails — and that failure is GRACEFUL
// (arena_alloc returns 0 → non-OK Status), never a process abort.
// ══════════════════════════════════════════════════════════════════

class ListStringBindBoundary : public ::testing::Test {
 protected:
  static std::vector<std::pair<std::string, CelType>> Decl() {
    return {{"xs", CelType::List(CelType::String())}};
  }
  static Value OneElementList(size_t elem_bytes) {
    return Value::List({Value::String(AsciiPayload(elem_bytes))});
  }
  static void ExpectElementSize(size_t elem_bytes) {
    ExpectIntCell("size(xs[0])", Decl(), {{"xs", OneElementList(elem_bytes)}},
                  static_cast<int64_t>(elem_bytes));
  }
};

TEST_F(ListStringBindBoundary, SizeEmpty) {
  ExpectIntCell("size(xs)", Decl(), {{"xs", Value::List({})}}, 0);
}
TEST_F(ListStringBindBoundary, SizeThousandSmallElements) {
  std::vector<Value> elems;
  elems.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    elems.push_back(Value::String("e"));
  }
  ExpectIntCell("size(xs)", Decl(), {{"xs", Value::List(std::move(elems))}},
                1000);
}

TEST_F(ListStringBindBoundary, ElementEmpty) {
  ExpectElementSize(0);
}
TEST_F(ListStringBindBoundary, ElementOneByte) {
  ExpectElementSize(kOneByte);
}
TEST_F(ListStringBindBoundary, ElementComfort1KiB) {
  ExpectElementSize(kComfort);
}
TEST_F(ListStringBindBoundary, ElementJustUnderArena60KiB) {
  // 60 KiB element + small expression intermediates fit the 64 KiB
  // arena — correct value expected.
  ExpectElementSize(kJustUnderArena);
}
TEST_F(ListStringBindBoundary, ElementJustOverArena70KiB) {
  // 70 KiB element overflows the 64 KiB seed chunk; the arena grows
  // to fit (chained arena, post-ssp-fix), so the element copies in
  // soundly and `size(xs[0])` == 70 KiB.
  ExpectElementSize(kJustOverArena);
}
TEST_F(ListStringBindBoundary, ElementJustOverMemory150KiB) {
  // 150 KiB element: arena grows to fit (chained arena, post-ssp-fix).
  ExpectElementSize(kJustOverMemory);
}
TEST_F(ListStringBindBoundary, ElementGross10MiB) {
  // 10 MiB element: arena grows to fit (chained arena, post-ssp-fix);
  // a single follow-on chunk is sized for the whole 10 MiB element.
  ExpectElementSize(kGross);
}
TEST_F(ListStringBindBoundary, Element60MiBStillFits) {
  // 60 MiB single element still fits below the wasm linear-memory
  // ceiling (chained arena, post-ssp-fix) — correct value.
  ExpectElementSize(size_t{60} * 1024 * 1024);
}
TEST_F(ListStringBindBoundary, ElementOverLinearMemoryGraceful) {
  // NEW arena failure boundary (post-ssp-fix).  A 64 MiB single
  // element exceeds what dlmalloc can carve out of the wasm linear
  // memory, so the growable arena's malloc fails: arena_alloc returns
  // 0 → EncodeSpan emits ResourceExhausted("arena OOM ...") → non-OK
  // Status.  This is the graceful ceiling — NOT a crash/hang/abort.
  // Empirically (both link modes): ≤60 MiB succeeds, ≥64 MiB fails
  // gracefully here.
  ExpectGracefulCell("size(xs[0])", Decl(),
                     {{"xs", OneElementList(size_t{64} * 1024 * 1024)}});
}

TEST_F(ListStringBindBoundary, MembershipOfBoundNeedle) {
  // Needle string crosses via the activation buffer; the haystack
  // list is handle-passed; comparison is host-side.
  const std::string needle = AsciiPayload(kComfort);
  ExpectBoolCell(
      "y in xs",
      {{"xs", CelType::List(CelType::String())}, {"y", CelType::String()}},
      {{"xs", Value::List({Value::String("a"), Value::String(needle)})},
       {"y", Value::String(needle)}},
      true);
}

// ══════════════════════════════════════════════════════════════════
// list<list<int>> — HANDLE-PASSED at Bind; nested aggregate elements
// exercise the cel_list_at marshal of a LIST element.
// ══════════════════════════════════════════════════════════════════

class NestedListBindBoundary : public ::testing::Test {
 protected:
  static std::vector<std::pair<std::string, CelType>> Decl() {
    return {{"xs", CelType::List(CelType::List(CelType::Int()))}};
  }
  static Value Nested(int outer, int inner) {
    std::vector<Value> rows;
    rows.reserve(outer);
    for (int i = 0; i < outer; ++i) {
      rows.push_back(Value::List(IntElements(inner)));
    }
    return Value::List(std::move(rows));
  }
};

TEST_F(NestedListBindBoundary, SizeEmpty) {
  ExpectIntCell("size(xs)", Decl(), {{"xs", Value::List({})}}, 0);
}
TEST_F(NestedListBindBoundary, SizeOneRow) {
  ExpectIntCell("size(xs)", Decl(), {{"xs", Nested(1, 3)}}, 1);
}
TEST_F(NestedListBindBoundary, SizeThousandRows) {
  ExpectIntCell("size(xs)", Decl(), {{"xs", Nested(1000, 10)}}, 1000);
}
TEST_F(NestedListBindBoundary, InnerSizeSmall) {
  ExpectIntCell("size(xs[0])", Decl(), {{"xs", Nested(1, 100)}}, 100);
}
TEST_F(NestedListBindBoundary, InnerSizeJustUnderArenaIfSnapshotted) {
  // 2000 × 24 B = 48 KiB if the inner list is arena-snapshotted at
  // xs[0]; fits either way — correct value expected.
  ExpectIntCell("size(xs[0])", Decl(), {{"xs", Nested(1, 2000)}}, 2000);
}
TEST_F(NestedListBindBoundary, InnerSizeOverArenaIfSnapshotted) {
  // 5000 × 24 B = 120 KiB would exceed the 64 KiB arena IF the
  // element marshal snapshotted — verified (both link modes) that it
  // does NOT: the inner-list element re-interns as a fresh
  // CEL_LIST_HOST handle at `xs[0]`, so the correct value comes back
  // with zero arena footprint for the elements.
  ExpectIntCell("size(xs[0])", Decl(), {{"xs", Nested(1, 5000)}}, 5000);
}

// ══════════════════════════════════════════════════════════════════
// map<string,int> — HANDLE-PASSED at Bind (EncodeMap interns the
// HostMapBacking; CEL_MAP_HOST ref_slot).  size / lookup / `in`
// stay host-side for the entry-count axis.
// ══════════════════════════════════════════════════════════════════

class MapStringIntBindBoundary : public ::testing::Test {
 protected:
  static std::vector<std::pair<std::string, CelType>> Decl() {
    return {{"m", CelType::Map(CelType::String(), CelType::Int())}};
  }
  static Value MapOf(int n) {
    std::vector<std::pair<Value, Value>> entries;
    entries.reserve(n);
    for (int i = 0; i < n; ++i) {
      entries.emplace_back(Value::String("k" + std::to_string(i)),
                           Value::Int(i));
    }
    return Value::Map(std::move(entries));
  }
  static void ExpectSize(int n) {
    ExpectIntCell("size(m)", Decl(), {{"m", MapOf(n)}}, n);
  }
};

TEST_F(MapStringIntBindBoundary, Empty) {
  ExpectSize(0);
}
TEST_F(MapStringIntBindBoundary, OneEntry) {
  ExpectSize(1);
}
TEST_F(MapStringIntBindBoundary, Comfort21Entries) {
  ExpectSize(21);
}
TEST_F(MapStringIntBindBoundary, JustUnderArena1280Entries) {
  ExpectSize(1280);  // 1280 × 48 B ≈ 60 KiB if it were arena-copied
}
TEST_F(MapStringIntBindBoundary, JustOverArena1500Entries) {
  ExpectSize(1500);  // 1500 × 48 B ≈ 70 KiB if it were arena-copied
}
TEST_F(MapStringIntBindBoundary, JustOverMemory3200Entries) {
  ExpectSize(3200);  // 3200 × 48 B ≈ 150 KiB if it were arena-copied
}
TEST_F(MapStringIntBindBoundary, Gross100KEntries) {
  ExpectSize(kGrossListCount);
}

TEST_F(MapStringIntBindBoundary, LookupSmall) {
  ExpectIntCell("m['k7']", Decl(), {{"m", MapOf(10)}}, 7);
}

TEST_F(MapStringIntBindBoundary, MembershipOfGrossBoundKey) {
  // A 10 MiB string KEY: the key crosses via the activation buffer
  // (string bind path — grows); the map is handle-passed; the `in`
  // check decodes the key host-side.  No 64 KiB-arena copy anywhere
  // on this path — correct value expected.
  const std::string big_key = AsciiPayload(kGross);
  ExpectBoolCell("y in m",
                 {{"m", CelType::Map(CelType::String(), CelType::Int())},
                  {"y", CelType::String()}},
                 {{"m", Value::Map({{Value::String(big_key), Value::Int(1)}})},
                  {"y", Value::String(big_key)}},
                 true);
}

// ══════════════════════════════════════════════════════════════════
// map<int,string> — HANDLE-PASSED at Bind; the VALUE copy happens at
// `m[0]` (cel_map_lookup → EncodeSpan → arena_alloc), through the
// chained grow-on-demand eval arena, mirroring the list<string>
// element cells: values far over the 64 KiB seed chunk copy in
// soundly; only a value past the wasm linear-memory ceiling
// (~64 MiB) fails, and gracefully.
// ══════════════════════════════════════════════════════════════════

class MapIntStringBindBoundary : public ::testing::Test {
 protected:
  static std::vector<std::pair<std::string, CelType>> Decl() {
    return {{"m", CelType::Map(CelType::Int(), CelType::String())}};
  }
  static Value OneEntryMap(size_t value_bytes) {
    return Value::Map(
        {{Value::Int(0), Value::String(AsciiPayload(value_bytes))}});
  }
  static void ExpectValueSize(size_t value_bytes) {
    ExpectIntCell("size(m[0])", Decl(), {{"m", OneEntryMap(value_bytes)}},
                  static_cast<int64_t>(value_bytes));
  }
};

TEST_F(MapIntStringBindBoundary, SizeEmpty) {
  ExpectIntCell("size(m)", Decl(), {{"m", Value::Map({})}}, 0);
}
TEST_F(MapIntStringBindBoundary, SizeThousandSmallEntries) {
  std::vector<std::pair<Value, Value>> entries;
  entries.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    entries.emplace_back(Value::Int(i), Value::String("v"));
  }
  ExpectIntCell("size(m)", Decl(), {{"m", Value::Map(std::move(entries))}},
                1000);
}

TEST_F(MapIntStringBindBoundary, ValueEmpty) {
  ExpectValueSize(0);
}
TEST_F(MapIntStringBindBoundary, ValueOneByte) {
  ExpectValueSize(kOneByte);
}
TEST_F(MapIntStringBindBoundary, ValueComfort1KiB) {
  ExpectValueSize(kComfort);
}
TEST_F(MapIntStringBindBoundary, ValueJustUnderArena60KiB) {
  ExpectValueSize(kJustUnderArena);
}
TEST_F(MapIntStringBindBoundary, ValueJustOverArena70KiB) {
  // 70 KiB value overflows the 64 KiB seed chunk; the arena grows to
  // fit (chained arena, post-ssp-fix) — correct value.
  ExpectValueSize(kJustOverArena);
}
TEST_F(MapIntStringBindBoundary, ValueJustOverMemory150KiB) {
  // 150 KiB value: arena grows to fit (chained arena, post-ssp-fix).
  ExpectValueSize(kJustOverMemory);
}
TEST_F(MapIntStringBindBoundary, ValueGross10MiB) {
  // 10 MiB value: arena grows to fit (chained arena, post-ssp-fix).
  ExpectValueSize(kGross);
}
TEST_F(MapIntStringBindBoundary, Value60MiBStillFits) {
  // 60 MiB value still fits below the wasm linear-memory ceiling
  // (chained arena, post-ssp-fix) — correct value.
  ExpectValueSize(size_t{60} * 1024 * 1024);
}
TEST_F(MapIntStringBindBoundary, ValueOverLinearMemoryGraceful) {
  // NEW arena failure boundary (post-ssp-fix), mirroring the
  // list<string> ElementOverLinearMemoryGraceful cell.  A 64 MiB
  // value exceeds the wasm linear memory: arena_alloc returns 0 →
  // EncodeSpan emits ResourceExhausted → non-OK Status.  Graceful
  // ceiling, not a crash.  Empirically (both link modes): ≤60 MiB
  // succeeds, ≥64 MiB fails gracefully here.
  ExpectGracefulCell("size(m[0])", Decl(),
                     {{"m", OneEntryMap(size_t{64} * 1024 * 1024)}});
}

}  // namespace
}  // namespace celwasm
