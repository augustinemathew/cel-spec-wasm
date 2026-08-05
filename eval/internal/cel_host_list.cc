#include "eval/internal/cel_host_list.h"

#include <cmath>
#include <cstdint>
#include <optional>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "eval/internal/cel_host_common.h"
#include "eval/internal/cel_host_eq.h"
#include "eval/internal/cel_host_error.h"
#include "eval/value.h"
#include "runtime/cel_data.h"
#include "shared/type.h"

namespace celwasm {

namespace {

// Decodes a list-index CelValue to an int64 per langdef §"Indexing":
// the index is int, or — when dyn-typed (e.g. `[1,2,3][dyn(0.0)]`) — a
// CEL_UINT or an integral CEL_DOUBLE, mirroring `cel_list_at_arena` in
// runtime/cel_runtime.c (pinned by oracle `ListIndex{Double,Uint}Agrees`
// and conformance `lists/index/zero_based_{double,uint}`).  On a
// malformed index writes the wire error to `out_slot` and returns
// nullopt; the returned int64 may still be negative (the caller bounds-
// checks it against the backing).
std::optional<int64_t> DecodeListIndex(const CelValue& idx_cv,
                                       uint32_t out_slot,
                                       const TrampolineContext& ctx) {
  if (idx_cv.kind == CEL_INT) {
    return idx_cv.payload.i;
  }
  if (idx_cv.kind == CEL_UINT) {
    if (idx_cv.payload.u > static_cast<uint64_t>(INT64_MAX)) {
      WriteWireError(CEL_ERR_INDEX_OUT_OF_BOUNDS, out_slot, ctx.mem);
      return std::nullopt;
    }
    return static_cast<int64_t>(idx_cv.payload.u);
  }
  if (idx_cv.kind == CEL_DOUBLE) {
    const double d = idx_cv.payload.d;
    if (!std::isfinite(d) || d > 9.2233720368547758e18 ||
        d < -9.2233720368547758e18) {
      WriteWireError(CEL_ERR_INVALID_ARGUMENT, out_slot, ctx.mem);
      return std::nullopt;
    }
    const auto trunc = static_cast<int64_t>(d);
    if (static_cast<double>(trunc) != d) {  // non-integral
      WriteWireError(CEL_ERR_INVALID_ARGUMENT, out_slot, ctx.mem);
      return std::nullopt;
    }
    return trunc;
  }
  WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
  return std::nullopt;
}

// ── Arena-list materialisation (shared) ────────────────────────────
//
// Every trampoline that must *produce* an arena list out of
// host-backed inputs — the comprehension-iter snapshot
// (`CelListIterOpenImpl`, which is also what `cel_list_arena_view`
// routes `list.join()` through) and cross-origin list concat
// (`CelListConcatImpl`) — builds it with these three helpers, so the
// header layout and the element-encoding contract live in one place.

// Byte offset of element `index` inside an arena elements run.
uint32_t ArenaElementSlot(uint32_t elements_off, size_t index) {
  return elements_off + (static_cast<uint32_t>(index) *
                         static_cast<uint32_t>(sizeof(CelValue)));
}

// Allocates a fresh arena list — a 16-byte ArenaListHeader plus a
// `count`×24-byte elements run — and stamps the header (two arena
// allocs, mirroring `cel_list_create`).  `*elements_off` receives the
// run offset; a zero-count list allocates no run and leaves it 0,
// matching `alloc_concat_list` in cel_runtime.c.  Returns nullopt when
// the arena is exhausted or the run would not fit the 32-bit offset
// space; the caller decides how to surface that.
std::optional<uint32_t> AllocArenaList(size_t count,
                                       const TrampolineContext& ctx,
                                       uint32_t* absl_nonnull elements_off) {
  constexpr uint32_t kHeaderBytes = 16u;
  constexpr auto kElemBytes = static_cast<uint32_t>(sizeof(CelValue));
  if (count > UINT32_MAX / kElemBytes) return std::nullopt;
  uint32_t header_off = 0;
  if (ctx.alloc.Alloc(kHeaderBytes, &header_off) == nullptr ||
      header_off == 0) {
    return std::nullopt;
  }
  *elements_off = 0;
  if (count > 0) {
    const uint32_t elements_bytes = static_cast<uint32_t>(count) * kElemBytes;
    if (ctx.alloc.Alloc(elements_bytes, elements_off) == nullptr ||
        *elements_off == 0) {
      return std::nullopt;
    }
  }
  // Header layout mirrors `ArenaListHeader` (cel_data.h): count,
  // capacity, elements_offset, _pad.
  ctx.mem.WriteU32(header_off + 0u, static_cast<uint32_t>(count));
  ctx.mem.WriteU32(header_off + 4u, static_cast<uint32_t>(count));
  ctx.mem.WriteU32(header_off + 8u, *elements_off);
  ctx.mem.WriteU32(header_off + 12u, 0u);
  return header_off;
}

// Writes the wire CelValue naming an arena list built by
// `AllocArenaList`.
void WriteArenaListValue(uint32_t out_slot, uint32_t header_off,
                         const TrampolineContext& ctx) {
  CelValue cv{};
  cv.kind = CEL_LIST_ARENA;
  cv.payload.arena_list.header_ptr = header_off;
  ctx.mem.WriteCelValue(out_slot, cv);
}

// Encodes `n` elements of a host-backed list into the arena elements
// run at `elements_off`, starting at destination element index
// `dst_index`.  Each element goes through the shared marshaller:
// scalars encode inline / into the arena, aggregate elements intern
// into the externref table and land as CEL_MESSAGE / CEL_LIST_HOST /
// CEL_MAP_HOST handles.  Element-read failures propagate as a status.
absl::Status EncodeHostElementsIntoRun(const HostListBacking& backing, size_t n,
                                       uint32_t elements_off, size_t dst_index,
                                       const TrampolineContext& ctx) {
  // `celwasm::CelType::Int()` is informational only (no element-side
  // narrowing); matches the CelListAtImpl call site.
  for (size_t i = 0; i < n; ++i) {
    auto got = backing.At(i, celwasm::CelType::Int());
    if (!got.ok()) return got.status();
    const uint32_t elem_slot = ArenaElementSlot(elements_off, dst_index + i);
    if (auto s = EncodeFieldResult(*got, elem_slot, ctx); !s.ok()) return s;
  }
  return absl::OkStatus();
}

// Materializes a non-empty host list into a fresh arena list at
// `out_slot`.  Returns false on arena OOM so the caller can fall back
// to an empty list; element-read failures propagate as a status.
absl::StatusOr<bool> SnapshotHostListToArena(const HostListBacking& backing,
                                             size_t count, uint32_t out_slot,
                                             const TrampolineContext& ctx) {
  uint32_t elements_off = 0;
  const std::optional<uint32_t> header_off =
      AllocArenaList(count, ctx, &elements_off);
  if (!header_off.has_value()) return false;
  if (auto s = EncodeHostElementsIntoRun(backing, count, elements_off,
                                         /*dst_index=*/0, ctx);
      !s.ok()) {
    return s;
  }
  WriteArenaListValue(out_slot, *header_off, ctx);
  return true;
}

// Writes a zero-count ArenaListHeader at `out_slot` so the comprehension
// prologue's 2-load shape reads count=0 cleanly (its shape walks the
// header pointer at `payload+8`, then reads count at `*header+0` — a zero
// `header_ptr` would dereference into rodata).  Used for non-host
// sources, empty host lists, and OOM fallback.
absl::Status WriteEmptyArenaList(uint32_t out_slot,
                                 const TrampolineContext& ctx) {
  uint32_t elements_off = 0;
  const std::optional<uint32_t> header_off =
      AllocArenaList(/*count=*/0, ctx, &elements_off);
  if (!header_off.has_value()) {
    return absl::ResourceExhaustedError(
        "CelListIterOpenImpl: arena OOM allocating empty header");
  }
  WriteArenaListValue(out_slot, *header_off, ctx);
  return absl::OkStatus();
}

}  // namespace

absl::Status CelListAtImpl(uint32_t out_slot, uint32_t list_slot,
                           uint32_t index_slot, const TrampolineContext& ctx) {
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  CelValue idx_cv = ctx.mem.ReadCelValue(index_slot);

  // 3VL on operands — same path the runtime dispatcher uses.  Index
  // first so an unknown index propagates even if the list is also
  // poisoned (matches the runtime fast-path order in
  // cel_list_at_arena).
  if (idx_cv.kind == CEL_UNKNOWN || idx_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, idx_cv);
    return absl::OkStatus();
  }
  if (list_cv.kind == CEL_UNKNOWN || list_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, list_cv);
    return absl::OkStatus();
  }

  // Codegen calls into us only on the kHost arm; defence-in-depth.
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  std::optional<int64_t> decoded = DecodeListIndex(idx_cv, out_slot, ctx);
  if (!decoded.has_value()) return absl::OkStatus();
  int64_t i = *decoded;

  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListAtImpl: list ref_slot ", list_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }

  if (i < 0) {
    WriteWireError(CEL_ERR_INDEX_OUT_OF_BOUNDS, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // backing->At returns a Value::Error(kIndexOutOfBounds) when i >=
  // Size; the encoder maps that to CEL_ERR_INDEX_OUT_OF_BOUNDS via
  // WireErrorCode.  Single round-trip, no host-side double-check.
  auto got = backing->At(static_cast<size_t>(i), celwasm::CelType::Int());
  if (!got.ok()) return got.status();
  return EncodeFieldResult(*got, out_slot, ctx);
}

absl::Status CelListIterOpenImpl(uint32_t out_slot, uint32_t list_slot,
                                 const TrampolineContext& ctx) {
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);

  if (list_cv.kind == CEL_UNKNOWN || list_cv.kind == CEL_ERROR) {
    // The comprehension prologue's range-absorption guard propagates
    // a poisoned iter_range BEFORE cel_list_arena_view can route it
    // here (expr_lower_comprehension.cc EmitRangeAbsorptionGuard).
    // Reaching this arm means the guard regressed; failing the eval
    // loudly beats silently iterating an empty view — that is the
    // empty-range-identity soundness bug (exists→false, all→true,
    // map/filter→[]) this tripwire pins shut.
    return absl::FailedPreconditionError(
        absl::StrCat("CelListIterOpenImpl: iter_range CelValue is ",
                     list_cv.kind == CEL_UNKNOWN ? "CEL_UNKNOWN" : "CEL_ERROR",
                     " — codegen's comprehension range-absorption guard must "
                     "propagate it before the iterate path runs"));
  }
  if (list_cv.kind != CEL_LIST_HOST) {
    // Codegen contract: cel_list_arena_view only routes us for
    // CEL_LIST_HOST sources.  Defence in depth.
    return WriteEmptyArenaList(out_slot, ctx);
  }
  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListIterOpenImpl: list ref_slot ",
                     list_cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  const size_t count = backing->Size();
  if (count == 0) {
    return WriteEmptyArenaList(out_slot, ctx);
  }
  // Materialize the host list into the arena; OOM falls back to empty.
  auto done = SnapshotHostListToArena(*backing, count, out_slot, ctx);
  if (!done.ok()) return done.status();
  if (!*done) return WriteEmptyArenaList(out_slot, ctx);
  return absl::OkStatus();
}

absl::Status CelListSizeImpl(uint32_t out_slot, uint32_t list_slot,
                             const TrampolineContext& ctx) {
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  if (AbsorbUnary(list_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListSizeImpl: list ref_slot ",
                     list_cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  WriteWireInt(static_cast<int64_t>(backing->Size()), out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelListInImpl(uint32_t out_slot, uint32_t value_slot,
                           uint32_t list_slot, const TrampolineContext& ctx) {
  CelValue value_cv = ctx.mem.ReadCelValue(value_slot);
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  if (AbsorbBinary(value_cv, list_cv, out_slot, ctx.mem)) {
    return absl::OkStatus();
  }
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListInImpl: list ref_slot ", list_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  // Use ForEach + a direct compare against the pre-decoded wire query
  // CelValue.  Avoids the per-element `At()` -> `StatusOr<Value>` and,
  // for scalar elements, any arena allocation.  We track `found` to
  // short-circuit; `ForEach` itself can't break, so the post-found
  // iterations just skip the compare.  `status` carries out an
  // infrastructure failure from an aggregate element's deep compare,
  // which `ForEach` has no way to signal itself.
  bool found = false;
  absl::Status status = absl::OkStatus();
  backing->ForEach([&](const celwasm::Value& v) {
    if (found || !status.ok()) return;
    auto equal_or = BackingValueEqualsQuery(v, value_cv, ctx);
    if (!equal_or.ok()) {
      status = equal_or.status();
      return;
    }
    if (*equal_or) found = true;
  });
  if (!status.ok()) return status;
  WriteWireBool(found, out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelListEqImpl(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
                           const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  const bool a_ok = (a_cv.kind == CEL_LIST_ARENA || a_cv.kind == CEL_LIST_HOST);
  const bool b_ok = (b_cv.kind == CEL_LIST_ARENA || b_cv.kind == CEL_LIST_HOST);
  if (!a_ok || !b_ok) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  auto equal_or = ListsEqual(a_cv, b_cv, ctx);
  if (!equal_or.ok()) return equal_or.status();
  WriteWireBool(*equal_or, out_slot, ctx.mem);
  return absl::OkStatus();
}

namespace {

// Copies the `n` elements of an arena-origin operand into the
// destination run starting at element index `dst_index`.  Arena
// elements are already wire CelValues — scalars inline, aggregates as
// interned msg_slot / ref_slot handles — so the copy is verbatim.
void CopyArenaElementsIntoRun(const CelValue& cv, size_t n,
                              uint32_t elements_off, size_t dst_index,
                              const TrampolineContext& ctx) {
  for (size_t i = 0; i < n; ++i) {
    const CelValue e =
        ReadArenaListElement(cv, static_cast<uint32_t>(i), ctx.mem);
    ctx.mem.WriteCelValue(ArenaElementSlot(elements_off, dst_index + i), e);
  }
}

// Copies the `n` elements of a list operand of EITHER origin into the
// destination run starting at element index `dst_index`.  Caller has
// verified `cv.kind` is list-shaped and that `n` is its length.
absl::Status CopyListElementsIntoRun(const CelValue& cv, size_t n,
                                     uint32_t elements_off, size_t dst_index,
                                     const TrampolineContext& ctx) {
  if (cv.kind == CEL_LIST_ARENA) {
    CopyArenaElementsIntoRun(cv, n, elements_off, dst_index, ctx);
    return absl::OkStatus();
  }
  const HostListBacking* backing = ctx.refs.LookupList(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListConcatImpl: list ref_slot ", cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  return EncodeHostElementsIntoRun(*backing, n, elements_off, dst_index, ctx);
}

}  // namespace

absl::Status CelListConcatImpl(uint32_t out_slot, uint32_t a_slot,
                               uint32_t b_slot, const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  const bool a_ok = (a_cv.kind == CEL_LIST_ARENA || a_cv.kind == CEL_LIST_HOST);
  const bool b_ok = (b_cv.kind == CEL_LIST_ARENA || b_cv.kind == CEL_LIST_HOST);
  if (!a_ok || !b_ok) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  auto na_or = ListLength(a_cv, ctx);
  if (!na_or.ok()) return na_or.status();
  auto nb_or = ListLength(b_cv, ctx);
  if (!nb_or.ok()) return nb_or.status();
  // Lift both operands into ONE fresh arena list: the result is
  // observably a CEL_LIST_ARENA, which keeps every downstream reader
  // (indexing, `in`, equality, `join`, a further concat) on the arena
  // fast path.  Arena OOM poisons CEL_ERR_OVERFLOW, matching
  // `cel_list_concat_arena`'s contract for the same failure.
  uint32_t elements_off = 0;
  const std::optional<uint32_t> header_off =
      AllocArenaList(*na_or + *nb_or, ctx, &elements_off);
  if (!header_off.has_value()) {
    WriteWireError(CEL_ERR_OVERFLOW, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  if (auto s = CopyListElementsIntoRun(a_cv, *na_or, elements_off,
                                       /*dst_index=*/0, ctx);
      !s.ok()) {
    return s;
  }
  if (auto s = CopyListElementsIntoRun(b_cv, *nb_or, elements_off,
                                       /*dst_index=*/*na_or, ctx);
      !s.ok()) {
    return s;
  }
  // Written last: `out_slot` may alias an operand slot, and the copy
  // loops above read the operands' headers out of it.
  WriteArenaListValue(out_slot, *header_off, ctx);
  return absl::OkStatus();
}

}  // namespace celwasm
