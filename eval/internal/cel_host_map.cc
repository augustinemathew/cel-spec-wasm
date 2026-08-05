#include "eval/internal/cel_host_map.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "eval/internal/cel_host_common.h"
#include "eval/internal/cel_host_eq.h"
#include "eval/internal/cel_host_error.h"
#include "eval/value.h"
#include "runtime/cel_data.h"
#include "shared/type.h"

namespace celwasm {

// ══════════════════════════════════════════════════════════════════
// CelMapLookupImpl — Layer 2 entry for `cel_host.cel_map_lookup`.
// Reads map_slot's ref_slot, dereferences to a HostMapBacking,
// decodes key, calls Get(), marshals result back.
// ══════════════════════════════════════════════════════════════════

namespace {

// Decode a scalar CelValue into a celwasm::Value.  Map-key kinds only
// (bool/int/uint/string) — every other kind returns nullopt and the
// caller surfaces a TYPE_MISMATCH error to the wasm side.  Strings
// stay a non-owning view over linear memory: the shared-memory base
// pointer is stable (wasmtime shared memories never remap on grow)
// and the key Value is consumed by `Get` / `ContainsKey` before the
// trampoline returns — no backing stores it past the call.
std::optional<celwasm::Value> DecodeKey(const CelValue& cv,
                                        const MemoryView& mem) {
  switch (cv.kind) {
    case CEL_BOOL:
      return celwasm::Value::Bool(cv.payload.b != 0);
    case CEL_INT:
      return celwasm::Value::Int(cv.payload.i);
    case CEL_UINT:
      return celwasm::Value::Uint(cv.payload.u);
    case CEL_STRING:
      return celwasm::Value::StringView(
          mem.ReadSpan(cv.payload.s.ptr, cv.payload.s.len));
    default:
      return std::nullopt;
  }
}

}  // namespace

absl::Status CelMapLookupImpl(uint32_t out_slot, uint32_t map_slot,
                              uint32_t key_slot, const TrampolineContext& ctx) {
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  CelValue key_cv = ctx.mem.ReadCelValue(key_slot);

  // 3VL on operands — same path the runtime dispatcher uses.
  if (key_cv.kind == CEL_UNKNOWN || key_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, key_cv);
    return absl::OkStatus();
  }
  if (map_cv.kind == CEL_UNKNOWN || map_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, map_cv);
    return absl::OkStatus();
  }

  // Codegen calls into us only on the kHost arm; defence-in-depth
  // for codegen drift.
  if (map_cv.kind != CEL_MAP_HOST) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }

  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapLookupImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }

  std::optional<celwasm::Value> key = DecodeKey(key_cv, ctx.mem);
  if (!key.has_value()) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }

  // expected_value_type is informational at M3 (no implicit
  // coercion); HostMap ignores it.  Pass an arbitrary scalar — the
  // type catalogue exposes no `Dyn` factory yet, and any choice
  // here is observed only by future backings that opt into typed
  // narrowing.
  auto got = backing->Get(*key, celwasm::CelType::Int());
  if (!got.ok()) return got.status();
  // EncodeFieldResult handles scalar + every aggregate kind
  // uniformly — nested map/list/message values from Get land
  // through the matching externref namespace.
  return EncodeFieldResult(*got, out_slot, ctx);
}

namespace {

// MapIterState field offsets (mirror the struct in cel_runtime.c).
constexpr uint32_t kMapIterKindOff = 0u;
constexpr uint32_t kMapIterCursorOff = 4u;
constexpr uint32_t kMapIterPayloadOff = 8u;
constexpr uint32_t kMapIterCountOff = 12u;
constexpr uint32_t kMapIterHostKind = 1u;  // MAP_ITER_KIND_HOST

// Stamp an empty (count=0) host map-iter state.  The runtime's
// `cel_map_iter_init` reads count and collapses count=0 to the empty
// iter handle.
void WriteEmptyMapIterState(uint32_t state_offset,
                            const TrampolineContext& ctx) {
  ctx.mem.WriteU32(state_offset + kMapIterKindOff, kMapIterHostKind);
  ctx.mem.WriteU32(state_offset + kMapIterCursorOff, 0u);
  ctx.mem.WriteU32(state_offset + kMapIterPayloadOff, 0u);
  ctx.mem.WriteU32(state_offset + kMapIterCountOff, 0u);
}

// Allocate a `count * 48` byte arena snapshot (key 24B immediately
// followed by value 24B — the stride `cel_map_iter_{key,value}_at`
// index by), encode each entry pair via `EncodeFieldResult`, and stamp
// the iter state to point at it.  Returns false on arena OOM so the
// caller can fall back to an empty state.
absl::StatusOr<bool> SnapshotMapEntriesToArena(
    const std::vector<std::pair<celwasm::Value, celwasm::Value>>& entries,
    uint32_t state_offset, const TrampolineContext& ctx) {
  constexpr uint32_t kPerEntry = 2u * sizeof(CelValue);
  const uint32_t snapshot_bytes =
      static_cast<uint32_t>(entries.size()) * kPerEntry;
  uint32_t snapshot_off = 0;
  ctx.alloc.Alloc(snapshot_bytes, &snapshot_off);
  if (snapshot_off == 0) return false;
  for (size_t i = 0; i < entries.size(); ++i) {
    const uint32_t key_off =
        snapshot_off + (static_cast<uint32_t>(i) * kPerEntry);
    const uint32_t val_off = key_off + sizeof(CelValue);
    if (auto s = EncodeFieldResult(entries[i].first, key_off, ctx); !s.ok()) {
      return s;
    }
    if (auto s = EncodeFieldResult(entries[i].second, val_off, ctx); !s.ok()) {
      return s;
    }
  }
  ctx.mem.WriteU32(state_offset + kMapIterKindOff, kMapIterHostKind);
  ctx.mem.WriteU32(state_offset + kMapIterCursorOff, 0u);
  ctx.mem.WriteU32(state_offset + kMapIterPayloadOff, snapshot_off);
  ctx.mem.WriteU32(state_offset + kMapIterCountOff,
                   static_cast<uint32_t>(entries.size()));
  return true;
}

}  // namespace

absl::Status CelMapIterOpenImpl(uint32_t state_offset, uint32_t map_slot,
                                const TrampolineContext& ctx) {
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  if (map_cv.kind == CEL_UNKNOWN || map_cv.kind == CEL_ERROR) {
    // Same tripwire as CelListIterOpenImpl: the comprehension
    // prologue's range-absorption guard propagates poisoned ranges
    // before any iterate path runs; an empty iter here would be the
    // silent empty-range-identity wrong answer.
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapIterOpenImpl: iter_range CelValue is ",
                     map_cv.kind == CEL_UNKNOWN ? "CEL_UNKNOWN" : "CEL_ERROR",
                     " — codegen's comprehension range-absorption guard must "
                     "propagate it before the iterate path runs"));
  }
  if (map_cv.kind != CEL_MAP_HOST) {
    // Codegen contract: cel_map_iter_init only calls us for
    // CEL_MAP_HOST sources.  Defence in depth — leave empty.
    WriteEmptyMapIterState(state_offset, ctx);
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapIterOpenImpl: map ref_slot ",
                     map_cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  // `ForEach` is the only positional-agnostic accessor on
  // `HostMapBacking`; iter callers need by-index lookup, so snapshot the
  // entries once up front (the snapshot also lives in the arena —
  // count*48B — for the iter's lifetime; a streaming variant is future
  // work if huge-host-map comprehensions become hot).
  std::vector<std::pair<celwasm::Value, celwasm::Value>> entries;
  entries.reserve(backing->Size());
  backing->ForEach([&](const celwasm::Value& k, const celwasm::Value& v) {
    entries.emplace_back(k, v);
  });
  if (entries.empty()) {
    WriteEmptyMapIterState(state_offset, ctx);
    return absl::OkStatus();
  }
  auto done = SnapshotMapEntriesToArena(entries, state_offset, ctx);
  if (!done.ok()) return done.status();
  if (!*done) WriteEmptyMapIterState(state_offset, ctx);
  return absl::OkStatus();
}

absl::Status CelMapSizeImpl(uint32_t out_slot, uint32_t map_slot,
                            const TrampolineContext& ctx) {
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  if (AbsorbUnary(map_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (map_cv.kind != CEL_MAP_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapSizeImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  WriteWireInt(static_cast<int64_t>(backing->Size()), out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelMapInImpl(uint32_t out_slot, uint32_t key_slot,
                          uint32_t map_slot, const TrampolineContext& ctx) {
  CelValue key_cv = ctx.mem.ReadCelValue(key_slot);
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  if (AbsorbBinary(key_cv, map_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (map_cv.kind != CEL_MAP_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapInImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  std::optional<celwasm::Value> key = DecodeKey(key_cv, ctx.mem);
  if (!key.has_value()) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  WriteWireBool(backing->ContainsKey(*key), out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelMapEqImpl(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
                          const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  // Any origin pair is admitted (host+host, host+arena, arena+host;
  // arena+arena normally short-circuits in the runtime's arena fast
  // path but compares correctly here too).  Both operands are
  // snapshotted through the same normalizing accessor, then compared
  // with a set-equality walk.
  const bool a_ok = (a_cv.kind == CEL_MAP_ARENA || a_cv.kind == CEL_MAP_HOST);
  const bool b_ok = (b_cv.kind == CEL_MAP_ARENA || b_cv.kind == CEL_MAP_HOST);
  if (!a_ok || !b_ok) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  auto eq_or = NormalizedMapEq(a_cv, b_cv, ctx);
  if (!eq_or.ok()) return eq_or.status();
  WriteWireBool(*eq_or, out_slot, ctx.mem);
  return absl::OkStatus();
}

}  // namespace celwasm
