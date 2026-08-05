#include "eval/internal/cel_host_common.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "eval/internal/cel_host_error.h"
#include "eval/value.h"
#include "runtime/cel_data.h"

namespace celwasm {

namespace {

// Encode the (string|bytes) span via the per-eval ArenaAllocator.
absl::Status EncodeSpan(const celwasm::Value& v, CelValue* out,
                        ArenaAllocator& alloc) {
  using K = celwasm::Value::Kind;
  absl::string_view s = v.kind() == K::kString ? *v.AsString() : *v.AsBytes();
  uint32_t off = 0;
  uint8_t* p = alloc.Alloc(s.size(), &off);
  if (p == nullptr && !s.empty()) {
    return absl::ResourceExhaustedError("arena OOM in CelMapLookupImpl");
  }
  if (p != nullptr && !s.empty()) std::memcpy(p, s.data(), s.size());
  out->kind = v.kind() == K::kString ? CEL_STRING : CEL_BYTES;
  out->payload.s.ptr = off;
  out->payload.s.len = static_cast<uint32_t>(s.size());
  return absl::OkStatus();
}

// m7b §3.1 / Probe D — `DecomposeAbslDuration` lives in
// `cel_host_common.h` (header-inline) so instance.cc shares it.

absl::Status EncodeDurationValue(const celwasm::Value& v, CelValue* out) {
  auto d_or = v.AsDuration();
  if (!d_or.ok()) return d_or.status();
  out->kind = CEL_DURATION;
  DecomposeAbslDuration(*d_or, &out->payload.dur);
  return absl::OkStatus();
}

absl::Status EncodeTimestampValue(const celwasm::Value& v, CelValue* out) {
  auto t_or = v.AsTimestamp();
  if (!t_or.ok()) return t_or.status();
  out->kind = CEL_TIMESTAMP;
  DecomposeAbslDuration(*t_or - absl::UnixEpoch(), &out->payload.ts);
  return absl::OkStatus();
}

// Encode a celwasm::Value into a CelValue, allocating string/bytes
// payloads through the per-eval ArenaAllocator.  Returns non-OK
// Status on infrastructure failure (arena OOM); spec-level errors
// inside the input Value already encode as `{kind:CEL_ERROR, err:…}`.
// Aborts for a Value kind the inline encoder must never see — the
// caller violated the Layer-1/Layer-2 contract.  Never returns.
[[noreturn]] void EncodeValueUnreachable(const celwasm::Value& v,
                                         const char* reason) {
  ABSL_CHECK(false) << "EncodeValue: " << reason << " (kind "
                    << static_cast<int>(v.kind()) << ")";
}

}  // namespace

absl::Status EncodeValue(const celwasm::Value& v, CelValue* absl_nonnull out,
                         ArenaAllocator& alloc) {
  using K = celwasm::Value::Kind;
  switch (v.kind()) {
    case K::kNull:
      out->kind = CEL_NULL;
      return absl::OkStatus();
    case K::kBool:
      out->kind = CEL_BOOL;
      out->payload.b = *v.AsBool() ? 1 : 0;
      return absl::OkStatus();
    case K::kInt:
      out->kind = CEL_INT;
      out->payload.i = *v.AsInt();
      return absl::OkStatus();
    case K::kUint:
      out->kind = CEL_UINT;
      out->payload.u = *v.AsUint();
      return absl::OkStatus();
    case K::kDouble:
      out->kind = CEL_DOUBLE;
      out->payload.d = *v.AsDouble();
      return absl::OkStatus();
    case K::kString:
    case K::kBytes:
      return EncodeSpan(v, out, alloc);
    case K::kError: {
      const celwasm::ErrorPayload* e = *v.ErrorInfo();
      out->kind = CEL_ERROR;
      out->payload.err = WireErrorCode(e->code);
      return absl::OkStatus();
    }
    case K::kUnknown:
      // Backings don't return unknowns — operand-pair propagation runs
      // before this encoder; PartialEval uses MatchesAnyUnknownPattern.
      EncodeValueUnreachable(v, "kUnknown is unreachable from Layer-1 returns");
    case K::kMessage:
    case K::kMap:
    case K::kList:
      // Aggregate kinds route through EncodeFieldResult /
      // EncodeAggregateIfAny, never the inline path.
      EncodeValueUnreachable(
          v, "aggregate kind must route through EncodeFieldResult");
    case K::kDuration:
      return EncodeDurationValue(v, out);
    case K::kTimestamp:
      return EncodeTimestampValue(v, out);
    case K::kType:
      // type(x) lowers to the cel_type_of_at_v runtime helper, never a
      // Layer-1 backing call (activation side: instance.cc::EncodeType).
      EncodeValueUnreachable(v, "kType is unreachable from Layer-1 backings");
  }
  EncodeValueUnreachable(v, "unhandled kind");
}

// Encode a Layer-1 aggregate (message / map / list) into a wire
// CelValue by interning the backing into the matching externref
// namespace.  Returns true if `v` is an aggregate kind and `*out` has
// been written; false if `v` is a scalar (caller falls through to the
// inline EncodeValue path).
absl::StatusOr<bool> EncodeAggregateToCelValue(const celwasm::Value& v,
                                               const TrampolineContext& ctx,
                                               CelValue* absl_nonnull out) {
  using K = celwasm::Value::Kind;
  CelValue cv{};
  if (v.kind() == K::kMessage) {
    auto sub_or = v.SharedMessageBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = ctx.refs.Intern(*std::move(sub_or));
  } else if (v.kind() == K::kMap) {
    auto sub_or = v.SharedMapBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_MAP_HOST;
    cv.payload.ref_slot = ctx.refs.InternMap(*std::move(sub_or));
  } else if (v.kind() == K::kList) {
    auto sub_or = v.SharedListBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_LIST_HOST;
    cv.payload.ref_slot = ctx.refs.InternList(*std::move(sub_or));
  } else {
    return false;
  }
  *out = cv;
  return true;
}

namespace {

// Slot-writing wrapper of `EncodeAggregateToCelValue`.
absl::StatusOr<bool> EncodeAggregateIfAny(const celwasm::Value& v,
                                          uint32_t out_slot,
                                          const TrampolineContext& ctx) {
  CelValue cv{};
  auto encoded_or = EncodeAggregateToCelValue(v, ctx, &cv);
  if (!encoded_or.ok()) return encoded_or.status();
  if (!*encoded_or) return false;
  ctx.mem.WriteCelValue(out_slot, cv);
  return true;
}

}  // namespace

// Marshal a `celwasm::Value` returned by Layer 1 (ReadField / At / Get)
// into the 24-byte CelValue at `out_slot`.  Scalars + null + error
// encode inline / via arena (`EncodeValue`); aggregate kinds intern
// via `EncodeAggregateIfAny`.  Used by every Layer-2 trampoline so
// the wire shape is consistent across surfaces.
absl::Status EncodeFieldResult(const celwasm::Value& v, uint32_t out_slot,
                               const TrampolineContext& ctx) {
  auto encoded_or = EncodeAggregateIfAny(v, out_slot, ctx);
  if (!encoded_or.ok()) return encoded_or.status();
  if (*encoded_or) return absl::OkStatus();
  CelValue cv{};
  if (auto s = EncodeValue(v, &cv, ctx.alloc); !s.ok()) return s;
  ctx.mem.WriteCelValue(out_slot, cv);
  return absl::OkStatus();
}

absl::Status EncodeValueToSlot(const celwasm::Value& v, uint32_t out_slot,
                               MemoryView& mem, ExternrefTable& refs,
                               ArenaAllocator& alloc) {
  // EncodeFieldResult only ever touches ctx.{mem,refs,alloc}; the
  // bindings span is unused on the encode path, so an empty one is safe.
  const CelHostBindings empty_bindings;
  const TrampolineContext ctx{empty_bindings, mem, refs, alloc};
  return EncodeFieldResult(v, out_slot, ctx);
}

absl::Status EncodeUnknownSet(absl::Span<const uint32_t> ids,
                              ArenaAllocator& alloc,
                              CelValue* absl_nonnull out) {
  out->kind = CEL_UNKNOWN;
  if (ids.empty()) {
    out->payload.unk = 0;  // legal empty UnknownSet, no allocation
    return absl::OkStatus();
  }
  // Canonical id-array form: sorted ascending, deduplicated —
  // the same invariant `cel_unknown_merge`'s merge walk relies on.
  std::vector<uint32_t> sorted(ids.begin(), ids.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  // One allocation: descriptor words at [0, 8), ids at [8, ...).
  const size_t bytes = (2 + sorted.size()) * sizeof(uint32_t);
  uint32_t off = 0;
  uint8_t* p = alloc.Alloc(bytes, &off);
  if (p == nullptr) {
    return absl::ResourceExhaustedError(
        "arena OOM minting an UnknownSet descriptor");
  }
  const uint32_t desc[2] = {off + (2 * static_cast<uint32_t>(sizeof(uint32_t))),
                            static_cast<uint32_t>(sorted.size())};
  std::memcpy(p, desc, sizeof(desc));
  std::memcpy(p + sizeof(desc), sorted.data(),
              sorted.size() * sizeof(uint32_t));
  out->payload.unk = off;
  return absl::OkStatus();
}

}  // namespace celwasm
