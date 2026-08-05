#include "eval/internal/cel_host_eq.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "eval/internal/cel_host_backing.h"
#include "eval/internal/cel_host_common.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/util/message_differencer.h"
#include "runtime/cel_internal.h"  // numeric_compare_kernel, cel_numeric_key_eq
#include "shared/type.h"

namespace celwasm {

namespace {

// Scalar-equality matcher mirroring `cel_runtime.c::cel_value_eq`:
// cross-type numeric per langdef §"Equality" for int/uint/double;
// structural for bool / string / bytes / null.  Map KEYS use
// `HostMapKeyEq` below instead — see its comment.
// SCALARS ONLY — every aggregate / message operand routes through
// `WireValueEq` before reaching here, so the `default:` arm is for
// wire kinds with no scalar identity (error / unknown / type) and
// for drift, both of which compare unequal rather than trap: `kind`
// is a wire field read out of linear memory, not a closed enum.
// Span operands ReadSpan via the MemoryView since both arena spans
// (literal-built) and encoded backing-element spans (allocated via
// the trampoline's ArenaAllocator) live in linear memory.
bool HostScalarSpanEq(const CelValue& a, const CelValue& b,
                      const MemoryView& mem) {
  if (a.payload.s.len != b.payload.s.len) return false;
  absl::string_view sa = mem.ReadSpan(a.payload.s.ptr, a.payload.s.len);
  absl::string_view sb = mem.ReadSpan(b.payload.s.ptr, b.payload.s.len);
  return sa == sb;
}

bool HostScalarSameKindEq(const CelValue& a, const CelValue& b,
                          const MemoryView& mem) {
  switch (a.kind) {
    case CEL_NULL:
      return true;
    case CEL_BOOL:
      return a.payload.b == b.payload.b;
    case CEL_INT:
      return a.payload.i == b.payload.i;
    case CEL_UINT:
      return a.payload.u == b.payload.u;
    case CEL_DOUBLE:
      return a.payload.d == b.payload.d;
    case CEL_STRING:
    case CEL_BYTES:
      return HostScalarSpanEq(a, b, mem);
    case CEL_DURATION:
      return a.payload.dur.seconds == b.payload.dur.seconds &&
             a.payload.dur.nanos == b.payload.dur.nanos;
    case CEL_TIMESTAMP:
      return a.payload.ts.seconds == b.payload.ts.seconds &&
             a.payload.ts.nanos == b.payload.ts.nanos;
    default:
      return false;
  }
}

bool HostNumericCrossEq(const CelValue& a, const CelValue& b) {
  // langdef §"Equality": int/uint/double compare by mathematical value
  // across the type ladder.  Delegates to the runtime's own ladder
  // (`numeric_compare_kernel`, runtime/cel_compare.c) rather than
  // widening both sides to double: the int-vs-uint arm must be EXACT,
  // and a double widening folds distinct >2^53 magnitudes together.
  // Sharing the kernel is also what keeps a host-origin operand
  // answering identically to an arena-built one.  A non-numeric operand
  // reaches the kernel's default arm and compares unequal.
  return numeric_compare_kernel(&a, &b) == kCmpEqual;
}

bool HostScalarValueEq(const CelValue& a, const CelValue& b,
                       const MemoryView& mem) {
  if (a.kind == b.kind) return HostScalarSameKindEq(a, b, mem);
  return HostNumericCrossEq(a, b);
}

// Map-KEY equality for wire values — the LOSSLESS rule, not the `==`
// rule that `HostScalarValueEq` implements.  Above 2^53 the two
// disagree: one double is the rounded image of a range of int64s, and a
// key lookup converts losslessly or not at all (see `cel_map_key_eq` in
// runtime/cel_internal.h for the cel-cpp citations).
//
// Only the NUMERIC half is shared with the runtime.  `cel_map_key_eq`
// itself reads spans through `cel_memory_base_()`, which on this side
// is the host's own buffer, not the wasm instance's linear memory — so
// span-bearing keys stay on `HostScalarValueEq`, which reads them
// through the MemoryView.
bool HostMapKeyEq(const CelValue& a, const CelValue& b, const MemoryView& mem) {
  if (is_numeric_kind(a.kind) && is_numeric_kind(b.kind)) {
    return cel_numeric_key_eq(&a, &b) != 0;
  }
  return HostScalarValueEq(a, b, mem);
}

uint32_t ReadArenaListCount(const CelValue& cv, const MemoryView& mem) {
  ArenaListHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_list.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  return hdr.count;
}

// Encode a backing-returned celwasm::Value into a wire CelValue for
// the equality / membership walks.  Scalars encode inline or into the
// arena; aggregate kinds intern into the externref table and land as
// CEL_MESSAGE / CEL_LIST_HOST / CEL_MAP_HOST handles that
// `WireValueEq` can recurse into.  (Encoding aggregates to a
// CEL_ERROR placeholder is what made two identical nested host lists
// compare unequal — two placeholders are never equal to each other.)
absl::StatusOr<CelValue> EncodeBackingElement(const celwasm::Value& v,
                                              const TrampolineContext& ctx) {
  CelValue cv{};
  auto encoded_or = EncodeAggregateToCelValue(v, ctx, &cv);
  if (!encoded_or.ok()) return encoded_or.status();
  if (*encoded_or) return cv;
  if (auto s = EncodeValue(v, &cv, ctx.alloc); !s.ok()) return s;
  return cv;
}

}  // namespace

CelValue ReadArenaListElement(const CelValue& cv, uint32_t i,
                              const MemoryView& mem) {
  ArenaListHeader hdr{};
  // ReadCelValue is also what reads ArenaListHeader-shaped runs —
  // it's just memcpy(24) which truncates to the 16-byte header.
  // Use a typed memcpy through ReadSpan for clarity.
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_list.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  return mem.ReadCelValue(hdr.elements_offset +
                          (i * static_cast<uint32_t>(kCelListEntryStride)));
}

// `bv.kind()`.  Same-kind compares hit the direct payload path;
// cross-kind routes through `HostNumericCrossEq` against a
// synthesised prototype so the langdef §"Equality" mathematical-value
// rule holds (`1 in [1u]`).
static bool NumericBackingEqualsQuery(const celwasm::Value& bv,
                                      uint32_t same_kind,
                                      const CelValue& query_cv) {
  CelValue proto{};
  proto.kind = same_kind;
  switch (same_kind) {
    case CEL_INT:
      proto.payload.i = *bv.AsInt();
      break;
    case CEL_UINT:
      proto.payload.u = *bv.AsUint();
      break;
    default:  // CEL_DOUBLE
      proto.payload.d = *bv.AsDouble();
      break;
  }
  if (query_cv.kind != same_kind) return HostNumericCrossEq(proto, query_cv);
  switch (same_kind) {
    case CEL_INT:
      return query_cv.payload.i == proto.payload.i;
    case CEL_UINT:
      return query_cv.payload.u == proto.payload.u;
    default:  // CEL_DOUBLE
      return query_cv.payload.d == proto.payload.d;
  }
}

// Compare a string/bytes backing scalar against a wire query CelValue
// of the matching wire kind, reading the query bytes from linear
// memory.  Returns false for any non-matching query kind.  Single
// call site per arm so it inlines back into the caller.
static bool SpanBackingEqualsQuery(absl::string_view backing,
                                   uint32_t want_kind, const CelValue& query_cv,
                                   const MemoryView& mem) {
  if (query_cv.kind != want_kind) return false;
  if (query_cv.payload.s.len != backing.size()) return false;
  absl::string_view q =
      mem.ReadSpan(query_cv.payload.s.ptr, query_cv.payload.s.len);
  return q == backing;
}

// Compare a temporal backing scalar (duration / timestamp) against a
// wire query CelValue.  Split out of `BackingValueEqualsQuery` so
// that function stays inside the function-size gate once the
// aggregate arms land.
static bool TemporalBackingEqualsQuery(const celwasm::Value& bv,
                                       const CelValue& query_cv) {
  if (bv.kind() == celwasm::Value::Kind::kDuration) {
    if (query_cv.kind != CEL_DURATION) return false;
    const absl::Duration d = *bv.AsDuration();
    const int64_t sec = absl::ToInt64Seconds(d);
    return sec == query_cv.payload.dur.seconds &&
           absl::ToInt64Nanoseconds(d - absl::Seconds(sec)) ==
               query_cv.payload.dur.nanos;
  }
  if (query_cv.kind != CEL_TIMESTAMP) return false;
  const absl::Time t = *bv.AsTimestamp();
  const int64_t sec = absl::ToUnixSeconds(t);
  return sec == query_cv.payload.ts.seconds &&
         absl::ToInt64Nanoseconds(t - absl::FromUnixSeconds(sec)) ==
             query_cv.payload.ts.nanos;
}

absl::StatusOr<bool> BackingValueEqualsQuery(const celwasm::Value& bv,
                                             const CelValue& query_cv,
                                             const TrampolineContext& ctx) {
  using K = celwasm::Value::Kind;
  const MemoryView& mem = ctx.mem;
  switch (bv.kind()) {
    case K::kBool:
      if (query_cv.kind != CEL_BOOL) return false;
      return *bv.AsBool() == (query_cv.payload.b != 0);
    case K::kInt:
      return NumericBackingEqualsQuery(bv, CEL_INT, query_cv);
    case K::kUint:
      return NumericBackingEqualsQuery(bv, CEL_UINT, query_cv);
    case K::kDouble:
      return NumericBackingEqualsQuery(bv, CEL_DOUBLE, query_cv);
    case K::kString:
      return SpanBackingEqualsQuery(*bv.AsString(), CEL_STRING, query_cv, mem);
    case K::kBytes:
      return SpanBackingEqualsQuery(*bv.AsBytes(), CEL_BYTES, query_cv, mem);
    case K::kNull:
      return query_cv.kind == CEL_NULL;
    case K::kDuration:
    case K::kTimestamp:
      return TemporalBackingEqualsQuery(bv, query_cv);
    case K::kMessage:
    case K::kList:
    case K::kMap: {
      // Aggregate element: materialise a wire handle for it and
      // compare deeply.  Answering `false` here is what made
      // `[1, 2] in xss` — and `msg in x.repeated_msgs` — a permanent
      // `false` regardless of contents.
      auto encoded_or = EncodeBackingElement(bv, ctx);
      if (!encoded_or.ok()) return encoded_or.status();
      return WireValueEq(*encoded_or, query_cv, ctx);
    }
    case K::kError:
    case K::kUnknown:
    case K::kType:
      // No value identity to match a query against: an error /
      // unknown element is a 3VL marker, and `type` values never
      // reach a list backing.  No `default:` — a new Value::Kind must
      // break this switch at compile time rather than fall into a
      // silent `false`.
      return false;
  }
  ABSL_CHECK(false) << "BackingValueEqualsQuery: out-of-enum Value::Kind "
                    << static_cast<int>(bv.kind());
  return false;
}

absl::StatusOr<size_t> ListLength(const CelValue& cv,
                                  const TrampolineContext& ctx) {
  if (cv.kind == CEL_LIST_ARENA) {
    return static_cast<size_t>(ReadArenaListCount(cv, ctx.mem));
  }
  const HostListBacking* backing = ctx.refs.LookupList(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "list ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  return backing->Size();
}

namespace {

// One list element normalized across origins for the equality walk —
// the same normalizing-accessor bridge as SnapshotMapEntries below
// uses for maps.  Non-message elements carry a wire CelValue (nested
// lists / maps as CEL_LIST_HOST / CEL_MAP_HOST handles, which
// `WireValueEq` recurses into); message elements resolve to the
// underlying proto so the compare goes through the same Any-peel +
// MessageDifferencer core as `cel_host.cel_message_eq`.
// `keepalive` pins the host-side backing (ProtoList::At returns a
// fresh ProtoBacking per call) so `msg` stays valid for the compare.
struct ListEqElement {
  CelValue wire{};
  bool is_message = false;
  const google::protobuf::Message* absl_nullable msg = nullptr;
  std::shared_ptr<const HostMessageBacking> keepalive;
};

// Reads the i-th element of an arena-origin list into the normalized
// form.  CEL_MESSAGE wire values must reference an interned
// msg_slot; a dangling slot is interner/codegen drift → non-OK.
absl::StatusOr<ListEqElement> ReadArenaListEqElement(
    const CelValue& cv, size_t i, const TrampolineContext& ctx) {
  ListEqElement e;
  e.wire = ReadArenaListElement(cv, static_cast<uint32_t>(i), ctx.mem);
  if (e.wire.kind == CEL_MESSAGE) {
    const HostMessageBacking* backing =
        ctx.refs.Lookup(e.wire.payload.msg_slot);
    if (backing == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("list element msg_slot ", e.wire.payload.msg_slot,
                       " not found in ExternrefTable"));
    }
    e.is_message = true;
    e.msg = backing->message();  // null for non-proto custom backings
  }
  return e;
}

// Reads the i-th element of a host-origin list into the normalized
// form.  Caller has verified `cv.kind == CEL_LIST_HOST`.
absl::StatusOr<ListEqElement> ReadHostListEqElement(
    const CelValue& cv, size_t i, const TrampolineContext& ctx) {
  const HostListBacking* backing = ctx.refs.LookupList(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "list ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  auto got = backing->At(i, celwasm::CelType::Int());
  if (!got.ok()) return got.status();
  ListEqElement e;
  if (got->kind() == celwasm::Value::Kind::kMessage) {
    auto shared_or = got->SharedMessageBacking();
    if (!shared_or.ok()) return shared_or.status();
    e.is_message = true;
    e.keepalive = *std::move(shared_or);
    e.msg = e.keepalive->message();  // null for non-proto custom backings
    return e;
  }
  auto enc_or = EncodeBackingElement(*got, ctx);
  if (!enc_or.ok()) return enc_or.status();
  e.wire = *enc_or;
  return e;
}

absl::StatusOr<ListEqElement> ReadListEqElementAt(
    const CelValue& cv, size_t i, const TrampolineContext& ctx) {
  if (cv.kind == CEL_LIST_ARENA) {
    return ReadArenaListEqElement(cv, i, ctx);
  }
  return ReadHostListEqElement(cv, i, ctx);
}

// Equality of two normalized elements.  Message pairs route through
// CompareProtoMessages — kNotComparable (non-proto backing,
// Any-unpack failure) compares UNEQUAL, matching the walk's
// established contract; message-vs-non-message is the langdef
// cross-kind `false`, not an error.  Everything else takes the deep
// wire compare, which recurses for nested lists / maps.
absl::StatusOr<bool> ListEqElementEquals(const ListEqElement& a,
                                         const ListEqElement& b,
                                         const TrampolineContext& ctx) {
  if (a.is_message != b.is_message) return false;
  if (a.is_message) {
    return CompareProtoMessages(a.msg, b.msg) == ProtoMessageEqOutcome::kEqual;
  }
  return WireValueEq(a.wire, b.wire, ctx);
}

// Element-wise equality walk for two list operands of any origin
// pair (arena+arena, arena+host, host+host).  Caller has already
// verified both kinds are list-shaped and verified equal lengths.
// Returns OkStatus + `*equal` set; non-OK Status only on
// infrastructure failure (bad ref_slot, backing->At error).
absl::Status WalkListEq(const CelValue& a_cv, const CelValue& b_cv, size_t n,
                        const TrampolineContext& ctx, bool* equal) {
  for (size_t i = 0; i < n; ++i) {
    auto ea_or = ReadListEqElementAt(a_cv, i, ctx);
    if (!ea_or.ok()) return ea_or.status();
    auto eb_or = ReadListEqElementAt(b_cv, i, ctx);
    if (!eb_or.ok()) return eb_or.status();
    auto same_or = ListEqElementEquals(*ea_or, *eb_or, ctx);
    if (!same_or.ok()) return same_or.status();
    if (!*same_or) {
      *equal = false;
      return absl::OkStatus();
    }
  }
  *equal = true;
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<bool> ListsEqual(const CelValue& a_cv, const CelValue& b_cv,
                                const TrampolineContext& ctx) {
  auto na_or = ListLength(a_cv, ctx);
  if (!na_or.ok()) return na_or.status();
  auto nb_or = ListLength(b_cv, ctx);
  if (!nb_or.ok()) return nb_or.status();
  if (*na_or != *nb_or) return false;
  bool equal = true;
  if (auto s = WalkListEq(a_cv, b_cv, *na_or, ctx, &equal); !s.ok()) return s;
  return equal;
}

namespace {

// Read the ArenaMapHeader of a CEL_MAP_ARENA operand via the
// MemoryView (mirrors ReadArenaListCount above).  `cv` must be
// CEL_MAP_ARENA; caller has verified.
ArenaMapHeader ReadArenaMapHeader(const CelValue& cv, const MemoryView& mem) {
  ArenaMapHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_map.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  return hdr;
}

// Returns the entry count of `cv` whether arena or host.  Caller has
// already verified kind ∈ {CEL_MAP_ARENA, CEL_MAP_HOST}.
absl::StatusOr<size_t> MapEntryCount(const CelValue& cv,
                                     const TrampolineContext& ctx) {
  if (cv.kind == CEL_MAP_ARENA) {
    return static_cast<size_t>(ReadArenaMapHeader(cv, ctx.mem).count);
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "map ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  return backing->Size();
}

// Snapshot every entry of `cv` as wire-format (key, value) CelValue
// pairs, regardless of origin — the normalizing accessor that lets
// the equality walk below treat arena and host operands uniformly
// (same bridge shape as ReadListEqElementAt for lists).  Arena entries
// are read straight out of linear memory; host entries are encoded
// via EncodeBackingElement, so an aggregate value lands as a
// CEL_LIST_HOST / CEL_MAP_HOST / CEL_MESSAGE handle that
// `WireValueEq` recurses into.
absl::Status SnapshotMapEntries(
    const CelValue& cv, const TrampolineContext& ctx,
    std::vector<std::pair<CelValue, CelValue>>* out) {
  if (cv.kind == CEL_MAP_ARENA) {
    const ArenaMapHeader hdr = ReadArenaMapHeader(cv, ctx.mem);
    out->reserve(hdr.count);
    for (uint32_t i = 0; i < hdr.count; ++i) {
      const uint32_t entry_off =
          hdr.entries_offset + (i * static_cast<uint32_t>(kCelMapEntryStride));
      out->emplace_back(
          ctx.mem.ReadCelValue(entry_off),
          ctx.mem.ReadCelValue(entry_off +
                               static_cast<uint32_t>(sizeof(CelValue))));
    }
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "map ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  absl::Status status = absl::OkStatus();
  out->reserve(backing->Size());
  backing->ForEach([&](const celwasm::Value& k, const celwasm::Value& v) {
    if (!status.ok()) return;
    auto ek_or = EncodeBackingElement(k, ctx);
    if (!ek_or.ok()) {
      status = ek_or.status();
      return;
    }
    auto ev_or = EncodeBackingElement(v, ctx);
    if (!ev_or.ok()) {
      status = ev_or.status();
      return;
    }
    out->emplace_back(*ek_or, *ev_or);
  });
  return status;
}

// Returns true iff some entry of `b` has a key equal to `entry`'s
// key AND a value equal to `entry`'s value.  Key equality goes through
// `HostMapKeyEq` — the LOSSLESS map-key rule, matching the arena
// kernel's `cel_map_key_eq`, so a cross-origin comparison answers the
// same as an arena-only one.  Keys are scalar by construction (langdef
// restricts map keys to bool / int / uint / string), so the scalar
// matcher is complete for them.  VALUES may be aggregates and take the
// deep compare.  Mirrors `cel_runtime.c::arena_map_entry_matches`.
absl::StatusOr<bool> NormalizedMapEntryMatches(
    const std::pair<CelValue, CelValue>& entry,
    const std::vector<std::pair<CelValue, CelValue>>& b,
    const TrampolineContext& ctx) {
  for (const auto& [kb, vb] : b) {
    if (HostMapKeyEq(entry.first, kb, ctx.mem)) {
      return WireValueEq(entry.second, vb, ctx);
    }
  }
  return false;
}

}  // namespace

absl::StatusOr<bool> NormalizedMapEq(const CelValue& a_cv, const CelValue& b_cv,
                                     const TrampolineContext& ctx) {
  auto na_or = MapEntryCount(a_cv, ctx);
  if (!na_or.ok()) return na_or.status();
  auto nb_or = MapEntryCount(b_cv, ctx);
  if (!nb_or.ok()) return nb_or.status();
  if (*na_or != *nb_or) return false;
  std::vector<std::pair<CelValue, CelValue>> a_entries;
  std::vector<std::pair<CelValue, CelValue>> b_entries;
  if (auto s = SnapshotMapEntries(a_cv, ctx, &a_entries); !s.ok()) return s;
  if (auto s = SnapshotMapEntries(b_cv, ctx, &b_entries); !s.ok()) return s;
  for (const auto& ea : a_entries) {
    auto matched_or = NormalizedMapEntryMatches(ea, b_entries, ctx);
    if (!matched_or.ok()) return matched_or.status();
    if (!*matched_or) return false;
  }
  return true;
}

namespace {

// ── Origin-agnostic deep value equality ───────────────────────────
//
// The single entry point every host-side element / value compare
// funnels through.  Declared at the top of this trampoline block;
// defined here because it recurses into both the list walk
// (`ListsEqual`) and the map walk (`NormalizedMapEq`).  Recursion
// depth is bounded by the operand's static CEL type nesting, which
// the checker has already accepted.

bool IsListKind(uint32_t kind) {
  return kind == CEL_LIST_ARENA || kind == CEL_LIST_HOST;
}

bool IsMapKind(uint32_t kind) {
  return kind == CEL_MAP_ARENA || kind == CEL_MAP_HOST;
}

// Underlying proto of a CEL_MESSAGE wire value, or nullptr for an
// unmapped slot / a non-proto custom backing — either way the pair is
// kNotComparable, which the list-element contract treats as unequal.
const google::protobuf::Message* absl_nullable ResolveWireMessage(
    const CelValue& cv, const TrampolineContext& ctx) {
  const HostMessageBacking* backing = ctx.refs.Lookup(cv.payload.msg_slot);
  return backing == nullptr ? nullptr : backing->message();
}

}  // namespace

absl::StatusOr<bool> WireValueEq(const CelValue& a, const CelValue& b,
                                 const TrampolineContext& ctx) {
  // Cross-kind aggregate comparisons are langdef `false`, not an
  // error ("comparing incompatible types is not an error").
  if (IsListKind(a.kind) || IsListKind(b.kind)) {
    if (!IsListKind(a.kind) || !IsListKind(b.kind)) return false;
    return ListsEqual(a, b, ctx);
  }
  if (IsMapKind(a.kind) || IsMapKind(b.kind)) {
    if (!IsMapKind(a.kind) || !IsMapKind(b.kind)) return false;
    return NormalizedMapEq(a, b, ctx);
  }
  if (a.kind == CEL_MESSAGE || b.kind == CEL_MESSAGE) {
    if (a.kind != CEL_MESSAGE || b.kind != CEL_MESSAGE) return false;
    return CompareProtoMessages(ResolveWireMessage(a, ctx),
                                ResolveWireMessage(b, ctx)) ==
           ProtoMessageEqOutcome::kEqual;
  }
  return HostScalarValueEq(a, b, ctx.mem);
}

// If `m` is a google.protobuf.Any, unpack its type_url + value
// against the Any descriptor's own pool and return a fresh
// typed-message clone (stashed in `owner` so the caller can keep it
// alive).  Returns the original `m` when it's not an Any; returns
// nullptr on Any-unpack failure (malformed type_url / unknown FQN /
// corrupt bytes).  Used by CelMessageEqImpl to compare an Any
// against either a typed message or another Any uniformly.
static const google::protobuf::Message* absl_nullable PeelAnyForEq(
    const google::protobuf::Message* m,
    std::unique_ptr<google::protobuf::Message>& owner) {
  if (m->GetDescriptor() == nullptr ||
      m->GetDescriptor()->full_name() != "google.protobuf.Any") {
    return m;
  }
  celwasm::Value peeled =
      UnpackAnyToValue(*m, m->GetDescriptor()->file()->pool());
  if (peeled.kind() != celwasm::Value::Kind::kMessage) return nullptr;
  auto backing_or = peeled.MessageBacking();
  if (!backing_or.ok() || (*backing_or)->message() == nullptr) return nullptr;
  const google::protobuf::Message* src = (*backing_or)->message();
  owner.reset(src->New());
  owner->CopyFrom(*src);
  return owner.get();
}

// Proto-message equality core, shared by `cel_host.cel_message_eq`
// and the list-equality element walk (declared above ListLength).
// Peels google.protobuf.Any operands, then compares by descriptor +
// MessageDifferencer.  Null inputs (non-proto custom backings) and
// Any-unpack failures are kNotComparable — the caller decides
// whether that surfaces as a type error (direct `msg == msg`) or as
// unequal (list element walk).
ProtoMessageEqOutcome CompareProtoMessages(
    const google::protobuf::Message* absl_nullable a,
    const google::protobuf::Message* absl_nullable b) {
  if (a == nullptr || b == nullptr) {
    return ProtoMessageEqOutcome::kNotComparable;
  }
  // Peel either operand if it's a google.protobuf.Any (typical
  // shape: a direct `Any{...}` literal that didn't pass through
  // ProtoBacking::ReadField's unwrap arm).  The peeled owners live
  // for the duration of this call.
  std::unique_ptr<google::protobuf::Message> a_owner;
  std::unique_ptr<google::protobuf::Message> b_owner;
  const google::protobuf::Message* a_cmp = PeelAnyForEq(a, a_owner);
  const google::protobuf::Message* b_cmp = PeelAnyForEq(b, b_owner);
  if (a_cmp == nullptr || b_cmp == nullptr) {
    // Any with malformed type_url / unknown FQN / corrupt bytes —
    // equality is undefined.
    return ProtoMessageEqOutcome::kNotComparable;
  }
  if (a_cmp->GetDescriptor() != b_cmp->GetDescriptor()) {
    // Cross-descriptor mismatch after peel → unequal, not error.
    return ProtoMessageEqOutcome::kUnequal;
  }
  return google::protobuf::util::MessageDifferencer::Equals(*a_cmp, *b_cmp)
             ? ProtoMessageEqOutcome::kEqual
             : ProtoMessageEqOutcome::kUnequal;
}

}  // namespace celwasm
