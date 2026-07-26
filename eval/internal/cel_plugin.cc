#include "eval/internal/cel_plugin.h"

#include <cstring>
#include <string>

#include <vector>
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

#include "eval/internal/cel_host.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "shared/type.h"
#include "wasm.h"
#include "wasmtime/component/val.h"

namespace celwasm {

namespace {

// The kind-mismatch diagnostics spell the declared CEL type via
// CelTypeKindName (shared/type.h) — the one kind-name helper.  The
// codegen-side FuncType validation should have caught any mismatch by
// the time Lift / Lower runs, but defence in depth is the same rule
// as elsewhere in this codebase: a silent miscompile is worse than an
// InvalidArgument with a named expected kind.
absl::Status CelfnVsValueMismatch(const CelType& type, const Value& value) {
  return absl::InvalidArgumentError(absl::StrCat(
      "cel_plugin: Lift expected Value of kind `", CelTypeKindName(type.kind()),
      "`, got `", ::celwasm::ValueKindName(value.kind()), "`"));
}

absl::Status WasmtimeKindMismatch(const CelType& type, uint8_t actual_kind,
                                  absl::string_view expected_kind_name) {
  return absl::InvalidArgumentError(absl::StrCat(
      "cel_plugin: Lower expected wasmtime val of kind `", expected_kind_name,
      "` (for CEL `", CelTypeKindName(type.kind()),
      "`), got valkind=", static_cast<int>(actual_kind)));
}

// ── Lift arms ───────────────────────────────────────────────────────

absl::Status LiftBool(const Value& v, wasmtime_component_val_t* out) {
  auto b = v.AsBool();
  if (!b.ok()) return b.status();
  out->kind = WASMTIME_COMPONENT_BOOL;
  out->of.boolean = *b;
  return absl::OkStatus();
}

absl::Status LiftInt(const Value& v, wasmtime_component_val_t* out) {
  auto x = v.AsInt();
  if (!x.ok()) return x.status();
  out->kind = WASMTIME_COMPONENT_S64;
  out->of.s64 = *x;
  return absl::OkStatus();
}

absl::Status LiftUint(const Value& v, wasmtime_component_val_t* out) {
  auto x = v.AsUint();
  if (!x.ok()) return x.status();
  out->kind = WASMTIME_COMPONENT_U64;
  out->of.u64 = *x;
  return absl::OkStatus();
}

absl::Status LiftDouble(const Value& v, wasmtime_component_val_t* out) {
  auto x = v.AsDouble();
  if (!x.ok()) return x.status();
  out->kind = WASMTIME_COMPONENT_F64;
  // Direct bit copy through the union slot — NaN payload, signed-zero,
  // and ±Inf survive verbatim because there is no intervening
  // arithmetic conversion (langdef "Equality" requires NaN ≠ NaN,
  // which falls out of the bitwise compare downstream).
  out->of.f64 = *x;
  return absl::OkStatus();
}

// CEL `null` encodes as WIT `option<unit>` none — the
// wasmtime_component_val_t carries kind=OPTION + of.option == nullptr
// for none.  m24 §6 documents this as the canonical lowering; the
// symmetric Lower arm interprets nullptr as Null.  Inlined at the
// dispatch site below since the body is two assignments.

absl::Status LiftString(const Value& v, wasmtime_component_val_t* out) {
  auto s = v.AsString();
  if (!s.ok()) return s.status();
  out->kind = WASMTIME_COMPONENT_STRING;
  // wasm_byte_vec_new copies len bytes verbatim — embedded NULs and
  // multi-byte UTF-8 round-trip because the canonical ABI uses the
  // length, not a NUL terminator.  Empty strings are admissible
  // (size=0, data=nullptr after wasm_byte_vec_new with size 0).
  wasm_byte_vec_new(&out->of.string, s->size(), s->data());
  return absl::OkStatus();
}

// Build a 2-field record {seconds: s64, nanos: s32} in *out, using the
// scratch values supplied.  Owned record entries are allocated through
// `wasmtime_component_valrecord_new_uninit` and the entry names are
// allocated via `wasm_name_new_from_string` — both released by
// `wasmtime_component_val_delete` later.  Reused by LiftDuration +
// LiftTimestamp because their wire shape is identical.
void EmitSecondsNanosRecord(int64_t seconds, int32_t nanos,
                            wasmtime_component_val_t* out) {
  out->kind = WASMTIME_COMPONENT_RECORD;
  wasmtime_component_valrecord_new_uninit(&out->of.record, 2);
  // Field 0: seconds (s64).
  wasm_name_new_from_string(&out->of.record.data[0].name, "seconds");
  out->of.record.data[0].val.kind = WASMTIME_COMPONENT_S64;
  out->of.record.data[0].val.of.s64 = seconds;
  // Field 1: nanos (s32).
  wasm_name_new_from_string(&out->of.record.data[1].name, "nanos");
  out->of.record.data[1].val.kind = WASMTIME_COMPONENT_S32;
  out->of.record.data[1].val.of.s32 = nanos;
}

// Decompose an absl::Duration into (seconds, nanos), preserving sign.
// Canonical form has |nanos| < 1e9 and seconds + nanos sharing sign
// (matches google.protobuf.Duration / absl conventions).  Implemented
// via absl::IDivDuration which truncates toward zero — so the
// remainder's sign matches the input's sign.
void DurationToSecondsNanos(absl::Duration d, int64_t* seconds,
                            int32_t* nanos) {
  absl::Duration rem;
  *seconds = absl::IDivDuration(d, absl::Seconds(1), &rem);
  *nanos = static_cast<int32_t>(absl::ToInt64Nanoseconds(rem));
}

absl::Status LiftDuration(const Value& v, wasmtime_component_val_t* out) {
  auto d = v.AsDuration();
  if (!d.ok()) return d.status();
  int64_t seconds;
  int32_t nanos;
  DurationToSecondsNanos(*d, &seconds, &nanos);
  EmitSecondsNanosRecord(seconds, nanos, out);
  return absl::OkStatus();
}

absl::Status LiftTimestamp(const Value& v, wasmtime_component_val_t* out) {
  auto t = v.AsTimestamp();
  if (!t.ok()) return t.status();
  // absl::Time → unix epoch decomposition.  ToUnixSeconds truncates
  // toward -inf; subtract the second-floor to get a non-negative
  // sub-second remainder in [0, 1e9) nanos.  This matches the
  // google.protobuf.Timestamp invariant (nanos ≥ 0).
  const int64_t seconds = absl::ToUnixSeconds(*t);
  const absl::Duration sub = *t - absl::FromUnixSeconds(seconds);
  const int32_t nanos = static_cast<int32_t>(absl::ToInt64Nanoseconds(sub));
  EmitSecondsNanosRecord(seconds, nanos, out);
  return absl::OkStatus();
}

// Forward decl: list / map Lift need to recurse into the public
// `LiftCelToComponent` dispatcher.  Same for the Lower side.
absl::Status LiftList(const CelType& type, const Value& v,
                      const CelComponentContext& ctx,
                      wasmtime_component_val_t* out);
absl::Status LowerList(const CelType& type, const wasmtime_component_val_t& in,
                       const CelComponentContext& ctx, Value* out);

absl::Status LiftBytes(const Value& v, wasmtime_component_val_t* out) {
  auto b = v.AsBytes();
  if (!b.ok()) return b.status();
  out->kind = WASMTIME_COMPONENT_LIST;
  // bytes lifts as list<u8>: each element is a wasmtime_component_val_t
  // tagged U8.  This is per-byte O(n), but bytes payloads in CEL are
  // typically small (<1KB) — if a bench shows the wrap is hot we can
  // add a wasmtime fast-path for byte arrays later.
  wasmtime_component_vallist_new_uninit(&out->of.list, b->size());
  for (size_t i = 0; i < b->size(); ++i) {
    out->of.list.data[i].kind = WASMTIME_COMPONENT_U8;
    out->of.list.data[i].of.u8 = static_cast<uint8_t>((*b)[i]);
  }
  return absl::OkStatus();
}

// ── Lower arms ──────────────────────────────────────────────────────

absl::Status LowerBool(const CelType& type, const wasmtime_component_val_t& in,
                       Value* out) {
  if (in.kind != WASMTIME_COMPONENT_BOOL) {
    return WasmtimeKindMismatch(type, in.kind, "bool");
  }
  *out = Value::Bool(in.of.boolean);
  return absl::OkStatus();
}

absl::Status LowerInt(const CelType& type, const wasmtime_component_val_t& in,
                      Value* out) {
  if (in.kind != WASMTIME_COMPONENT_S64) {
    return WasmtimeKindMismatch(type, in.kind, "s64");
  }
  *out = Value::Int(in.of.s64);
  return absl::OkStatus();
}

absl::Status LowerUint(const CelType& type, const wasmtime_component_val_t& in,
                       Value* out) {
  if (in.kind != WASMTIME_COMPONENT_U64) {
    return WasmtimeKindMismatch(type, in.kind, "u64");
  }
  *out = Value::Uint(in.of.u64);
  return absl::OkStatus();
}

absl::Status LowerDouble(const CelType& type,
                         const wasmtime_component_val_t& in, Value* out) {
  if (in.kind != WASMTIME_COMPONENT_F64) {
    return WasmtimeKindMismatch(type, in.kind, "f64");
  }
  *out = Value::Double(in.of.f64);
  return absl::OkStatus();
}

absl::Status LowerNull(const CelType& type, const wasmtime_component_val_t& in,
                       Value* out) {
  if (in.kind != WASMTIME_COMPONENT_OPTION) {
    return WasmtimeKindMismatch(type, in.kind, "option");
  }
  if (in.of.option != nullptr) {
    return absl::InvalidArgumentError(
        "cel_plugin: Lower for `null` expected option=none, got Some");
  }
  *out = Value::Null();
  return absl::OkStatus();
}

absl::Status LowerString(const CelType& type,
                         const wasmtime_component_val_t& in, Value* out) {
  if (in.kind != WASMTIME_COMPONENT_STRING) {
    return WasmtimeKindMismatch(type, in.kind, "string");
  }
  // Lower copies — the produced Value carries its own std::string and
  // is decoupled from any subsequent wasmtime_component_val_delete.
  // Defensive: a malformed val with size>0 and data==null would
  // dereference null in the std::string ctor.  Empty strings have
  // size==0; data may be null after a none-style ctor but we read it
  // through string(ptr,len) which tolerates len==0.  See
  // cleanup-backlog #37.
  if (in.of.string.size > 0 && in.of.string.data == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: Lower for `string` saw size=", in.of.string.size,
        " with null data pointer (malformed wasmtime_component_val_t)"));
  }
  *out = Value::String(std::string(in.of.string.data, in.of.string.size));
  return absl::OkStatus();
}

// Extract a (seconds, nanos) pair from a 2-field record val.  The
// component canonical ABI does not guarantee field order — we look up
// each field by name.  Returns InvalidArgument if either field is
// missing, the wrong kind, or the record has unexpected entries.
absl::Status DecodeSecondsNanosRecord(const wasmtime_component_val_t& in,
                                      absl::string_view ctx_kind,
                                      int64_t* out_seconds,
                                      int32_t* out_nanos) {
  if (in.kind != WASMTIME_COMPONENT_RECORD) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: Lower for `", ctx_kind,
        "` expected wasmtime record, got valkind=", static_cast<int>(in.kind)));
  }
  if (in.of.record.size != 2) {
    return absl::InvalidArgumentError(
        absl::StrCat("cel_plugin: Lower for `", ctx_kind,
                     "` expected record with 2 fields {seconds, nanos}, got ",
                     in.of.record.size));
  }
  // Defensive: size>0 with null data dereferences null in the loop
  // below.  cleanup-backlog #37.
  if (in.of.record.data == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: Lower for `", ctx_kind,
        "` saw record size=", in.of.record.size,
        " with null data pointer (malformed wasmtime_component_val_t)"));
  }
  bool got_seconds = false;
  bool got_nanos = false;
  for (size_t i = 0; i < in.of.record.size; ++i) {
    const auto& entry = in.of.record.data[i];
    const absl::string_view name(entry.name.data, entry.name.size);
    if (name == "seconds") {
      if (entry.val.kind != WASMTIME_COMPONENT_S64) {
        return absl::InvalidArgumentError(
            absl::StrCat("cel_plugin: Lower for `", ctx_kind,
                         "` expected seconds:s64, got valkind=",
                         static_cast<int>(entry.val.kind)));
      }
      *out_seconds = entry.val.of.s64;
      got_seconds = true;
    } else if (name == "nanos") {
      if (entry.val.kind != WASMTIME_COMPONENT_S32) {
        return absl::InvalidArgumentError(
            absl::StrCat("cel_plugin: Lower for `", ctx_kind,
                         "` expected nanos:s32, got valkind=",
                         static_cast<int>(entry.val.kind)));
      }
      *out_nanos = entry.val.of.s32;
      got_nanos = true;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("cel_plugin: Lower for `", ctx_kind,
                       "` saw unexpected record field `", name, "`"));
    }
  }
  if (!got_seconds || !got_nanos) {
    return absl::InvalidArgumentError(
        absl::StrCat("cel_plugin: Lower for `", ctx_kind,
                     "` record missing `seconds` or `nanos`"));
  }
  return absl::OkStatus();
}

absl::Status LowerDuration(const CelType& /*type*/,
                           const wasmtime_component_val_t& in, Value* out) {
  int64_t seconds = 0;
  int32_t nanos = 0;
  if (auto s = DecodeSecondsNanosRecord(in, "duration", &seconds, &nanos);
      !s.ok()) {
    return s;
  }
  // Canonical-form check: |nanos| < 1e9.  langdef "Duration" mandates
  // this so the (seconds, nanos) decomposition is unique.
  if (nanos <= -1'000'000'000 || nanos >= 1'000'000'000) {
    return absl::OutOfRangeError(
        absl::StrCat("cel_plugin: duration.nanos out of range ", nanos,
                     " (must satisfy |nanos| < 1e9)"));
  }
  *out = Value::Duration(absl::Seconds(seconds) + absl::Nanoseconds(nanos));
  return absl::OkStatus();
}

absl::Status LowerTimestamp(const CelType& /*type*/,
                            const wasmtime_component_val_t& in, Value* out) {
  int64_t seconds = 0;
  int32_t nanos = 0;
  if (auto s = DecodeSecondsNanosRecord(in, "timestamp", &seconds, &nanos);
      !s.ok()) {
    return s;
  }
  // google.protobuf.Timestamp invariant: nanos ∈ [0, 1e9).
  if (nanos < 0 || nanos >= 1'000'000'000) {
    return absl::OutOfRangeError(
        absl::StrCat("cel_plugin: timestamp.nanos out of range ", nanos,
                     " (must be in [0, 1e9))"));
  }
  *out = Value::Timestamp(absl::FromUnixSeconds(seconds) +
                          absl::Nanoseconds(nanos));
  return absl::OkStatus();
}

absl::Status LiftList(const CelType& type, const Value& v,
                      const CelComponentContext& ctx,
                      wasmtime_component_val_t* out) {
  if (v.kind() != Value::Kind::kList) {
    return CelfnVsValueMismatch(type, v);
  }
  auto backing_or = v.ListBacking();
  if (!backing_or.ok()) return backing_or.status();
  const HostListBacking* backing = *backing_or;
  const size_t n = backing->Size();
  const CelType& elem_type = type.list_element();

  out->kind = WASMTIME_COMPONENT_LIST;
  wasmtime_component_vallist_new_uninit(&out->of.list, n);
  // Initialise each slot to a default-cleanup-safe state before
  // recursing — if an element Lift fails mid-loop we still need the
  // outer wasmtime_component_val_delete to walk every slot without
  // tripping on a partial-init.  BOOL is a no-allocation arm so the
  // delete on an aborted element is a no-op.
  for (size_t i = 0; i < n; ++i) {
    out->of.list.data[i].kind = WASMTIME_COMPONENT_BOOL;
    out->of.list.data[i].of.boolean = false;
  }
  // HostListBacking::At needs a CelType witness for "expected element
  // type" — the field is informational per cel_host.h:241
  // ("M4 ignores it; no implicit coercion") and existing callers in
  // cel_host.cc pass `CelType::Int()` as a placeholder.  Match that
  // convention; the recursion's real type check happens inside the
  // nested LiftCelToComponent call against `elem_type`.
  const CelType placeholder_type = CelType::Int();
  absl::Status loop_status = absl::OkStatus();
  for (size_t i = 0; i < n; ++i) {
    auto elem = backing->At(i, placeholder_type);
    if (!elem.ok()) {
      loop_status = absl::Status(elem.status().code(),
                                 absl::StrCat("cel_plugin: list element ", i,
                                              ": ", elem.status().message()));
      break;
    }
    if (auto s =
            LiftCelToComponent(elem_type, *elem, ctx, &out->of.list.data[i]);
        !s.ok()) {
      loop_status = absl::Status(
          s.code(),
          absl::StrCat("cel_plugin: list element ", i, ": ", s.message()));
      break;
    }
  }
  return loop_status;
}

// Map keys are restricted to {bool, int, uint, string} per langdef
// (no double/timestamp/duration/list/map/proto keys).  The validation
// happens at FunctionLibrary::Builder::Build time today only for
// proto/optional; map_key_kind check belongs here because it's
// invariant of the marshaling wire (a different decl source could
// produce a map<double, V> shape).
bool IsLegalMapKeyKind(CelType::Kind k) {
  return k == CelType::Kind::kBool || k == CelType::Kind::kInt ||
         k == CelType::Kind::kUint || k == CelType::Kind::kString;
}

absl::Status LiftMap(const CelType& type, const Value& v,
                     const CelComponentContext& ctx,
                     wasmtime_component_val_t* out) {
  const CelType& key_type = type.map_key();
  const CelType& val_type = type.map_value();
  if (!IsLegalMapKeyKind(key_type.kind())) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: map key kind `", CelTypeKindName(key_type.kind()),
        "` is not allowed (langdef: bool|int|uint|string only)"));
  }
  if (v.kind() != Value::Kind::kMap) {
    return CelfnVsValueMismatch(type, v);
  }
  auto backing_or = v.MapBacking();
  if (!backing_or.ok()) return backing_or.status();
  const HostMapBacking* backing = *backing_or;
  const size_t n = backing->Size();

  // Map → list<tuple<K, V>> per m24 §6 (WIT has no native map; the
  // shape is the §6 canonical encoding).  Each outer-list element is
  // a 2-tuple of key + value lifted vals.
  out->kind = WASMTIME_COMPONENT_LIST;
  wasmtime_component_vallist_new_uninit(&out->of.list, n);
  for (size_t i = 0; i < n; ++i) {
    out->of.list.data[i].kind = WASMTIME_COMPONENT_BOOL;
    out->of.list.data[i].of.boolean = false;
  }

  size_t idx = 0;
  absl::Status err = absl::OkStatus();
  backing->ForEach([&](const Value& k, const Value& vv) {
    if (!err.ok()) return;
    if (idx >= n) {
      err = absl::InternalError(
          "cel_plugin: map ForEach visited more entries than Size()");
      return;
    }
    auto* slot = &out->of.list.data[idx];
    slot->kind = WASMTIME_COMPONENT_TUPLE;
    wasmtime_component_valtuple_new_uninit(&slot->of.tuple, 2);
    // Pre-init both tuple slots to no-alloc BOOL for delete-safety on
    // a mid-loop error.
    slot->of.tuple.data[0].kind = WASMTIME_COMPONENT_BOOL;
    slot->of.tuple.data[0].of.boolean = false;
    slot->of.tuple.data[1].kind = WASMTIME_COMPONENT_BOOL;
    slot->of.tuple.data[1].of.boolean = false;
    if (auto s = LiftCelToComponent(key_type, k, ctx, &slot->of.tuple.data[0]);
        !s.ok()) {
      err = absl::Status(s.code(), absl::StrCat("cel_plugin: map entry ", idx,
                                                " key: ", s.message()));
      return;
    }
    if (auto s = LiftCelToComponent(val_type, vv, ctx, &slot->of.tuple.data[1]);
        !s.ok()) {
      err = absl::Status(s.code(), absl::StrCat("cel_plugin: map entry ", idx,
                                                " value: ", s.message()));
      return;
    }
    ++idx;
  });
  return err;
}

absl::Status LowerMap(const CelType& type, const wasmtime_component_val_t& in,
                      const CelComponentContext& ctx, Value* out) {
  const CelType& key_type = type.map_key();
  const CelType& val_type = type.map_value();
  if (!IsLegalMapKeyKind(key_type.kind())) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: map key kind `", CelTypeKindName(key_type.kind()),
        "` is not allowed (langdef: bool|int|uint|string only)"));
  }
  if (in.kind != WASMTIME_COMPONENT_LIST) {
    return WasmtimeKindMismatch(type, in.kind, "list<tuple<K,V>>");
  }
  std::vector<std::pair<Value, Value>> entries;
  entries.reserve(in.of.list.size);
  for (size_t i = 0; i < in.of.list.size; ++i) {
    const auto& elem = in.of.list.data[i];
    if (elem.kind != WASMTIME_COMPONENT_TUPLE) {
      return absl::InvalidArgumentError(absl::StrCat(
          "cel_plugin: map entry ", i,
          " is not a tuple (valkind=", static_cast<int>(elem.kind), ")"));
    }
    if (elem.of.tuple.size != 2) {
      return absl::InvalidArgumentError(
          absl::StrCat("cel_plugin: map entry ", i, " tuple has arity ",
                       elem.of.tuple.size, ", expected 2"));
    }
    Value k, vv;
    if (auto s = LowerComponentToCel(key_type, elem.of.tuple.data[0], ctx, &k);
        !s.ok()) {
      return absl::Status(s.code(), absl::StrCat("cel_plugin: map entry ", i,
                                                 " key: ", s.message()));
    }
    if (auto s = LowerComponentToCel(val_type, elem.of.tuple.data[1], ctx, &vv);
        !s.ok()) {
      return absl::Status(s.code(), absl::StrCat("cel_plugin: map entry ", i,
                                                 " value: ", s.message()));
    }
    entries.emplace_back(std::move(k), std::move(vv));
  }
  *out = Value::Map(std::move(entries));
  return absl::OkStatus();
}

// ── proto(fqn) ──────────────────────────────────────────────────────
//
// Lift: serialize the host-side proto message to wire bytes, emit as
// list<u8>.  Lower: receive list<u8>, look up the descriptor in the
// supplied pool, materialise a new Message via the generated
// MessageFactory, then ParseFromString.
//
// This is the m24 §8 escape hatch — proto messages can't cross as
// externrefs, but the bytes _can_ cross, and the codec on the
// component side decodes them with the proto runtime.

absl::Status LiftProto(const CelType& type, const Value& v,
                       wasmtime_component_val_t* out) {
  if (v.kind() != Value::Kind::kMessage) {
    return CelfnVsValueMismatch(type, v);
  }
  auto backing_or = v.MessageBacking();
  if (!backing_or.ok()) return backing_or.status();
  const google::protobuf::Message* msg = (*backing_or)->message();
  if (msg == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: Lift for proto(", type.message_fully_qualified_name(),
        ") cannot serialise a non-proto-backed message (custom "
        "HostMessageBacking returned message()==nullptr)"));
  }
  // SerializePartialToString admits messages with unset required
  // fields — matches cel-cpp's tolerance.  The full-form
  // SerializeToString would fail-fast on missing required, which
  // CEL semantics don't promise.
  std::string bytes;
  if (!msg->SerializePartialToString(&bytes)) {
    return absl::InternalError(
        absl::StrCat("cel_plugin: SerializePartialToString failed for `",
                     type.message_fully_qualified_name(), "`"));
  }
  out->kind = WASMTIME_COMPONENT_LIST;
  wasmtime_component_vallist_new_uninit(&out->of.list, bytes.size());
  for (size_t i = 0; i < bytes.size(); ++i) {
    out->of.list.data[i].kind = WASMTIME_COMPONENT_U8;
    out->of.list.data[i].of.u8 = static_cast<uint8_t>(bytes[i]);
  }
  return absl::OkStatus();
}

absl::Status LowerProto(const CelType& type, const wasmtime_component_val_t& in,
                        const CelComponentContext& ctx, Value* out) {
  if (in.kind != WASMTIME_COMPONENT_LIST) {
    return WasmtimeKindMismatch(type, in.kind, "list<u8>");
  }
  if (ctx.pool == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: Lower for proto(", type.message_fully_qualified_name(),
        ") requires CelComponentContext::pool to materialise the message"));
  }
  const google::protobuf::Descriptor* desc =
      ctx.pool->FindMessageTypeByName(type.message_fully_qualified_name());
  if (desc == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: proto type `", type.message_fully_qualified_name(),
        "` not found in descriptor pool"));
  }
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(desc);
  if (prototype == nullptr) {
    return absl::InternalError(
        absl::StrCat("cel_plugin: generated_factory has no prototype for `",
                     type.message_fully_qualified_name(),
                     "` (descriptor not registered with the generated "
                     "pool — link the cc_proto_library into the test binary)"));
  }
  // Collect bytes from the list<u8>.
  std::string bytes;
  bytes.reserve(in.of.list.size);
  for (size_t i = 0; i < in.of.list.size; ++i) {
    const auto& el = in.of.list.data[i];
    if (el.kind != WASMTIME_COMPONENT_U8) {
      return absl::InvalidArgumentError(absl::StrCat(
          "cel_plugin: Lower for proto(", type.message_fully_qualified_name(),
          ") saw non-u8 element at index ", i,
          " (valkind=", static_cast<int>(el.kind), ")"));
    }
    bytes.push_back(static_cast<char>(el.of.u8));
  }
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  if (!msg->ParseFromString(bytes)) {
    return absl::InvalidArgumentError(
        absl::StrCat("cel_plugin: failed to parse proto(",
                     type.message_fully_qualified_name(), ") from ",
                     bytes.size(), " bytes"));
  }
  *out = Value::OwnedMessage(std::move(msg));
  return absl::OkStatus();
}

absl::Status LowerList(const CelType& type, const wasmtime_component_val_t& in,
                       const CelComponentContext& ctx, Value* out) {
  if (in.kind != WASMTIME_COMPONENT_LIST) {
    return WasmtimeKindMismatch(type, in.kind, "list");
  }
  // Defensive: size>0 with null data dereferences null below.
  // cleanup-backlog #37.
  if (in.of.list.size > 0 && in.of.list.data == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: Lower for `list` saw size=", in.of.list.size,
        " with null data pointer (malformed wasmtime_component_val_t)"));
  }
  const CelType& elem_type = type.list_element();
  std::vector<Value> elems;
  elems.reserve(in.of.list.size);
  for (size_t i = 0; i < in.of.list.size; ++i) {
    Value e;
    if (auto s = LowerComponentToCel(elem_type, in.of.list.data[i], ctx, &e);
        !s.ok()) {
      return absl::Status(s.code(), absl::StrCat("cel_plugin: list element ", i,
                                                 ": ", s.message()));
    }
    elems.push_back(std::move(e));
  }
  *out = Value::List(std::move(elems));
  return absl::OkStatus();
}

absl::Status LowerBytes(const CelType& type, const wasmtime_component_val_t& in,
                        Value* out) {
  if (in.kind != WASMTIME_COMPONENT_LIST) {
    return WasmtimeKindMismatch(type, in.kind, "list<u8>");
  }
  // Defensive: size>0 with null data dereferences null below.
  // cleanup-backlog #37.
  if (in.of.list.size > 0 && in.of.list.data == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "cel_plugin: Lower for `bytes` saw size=", in.of.list.size,
        " with null data pointer (malformed wasmtime_component_val_t)"));
  }
  std::string bytes;
  bytes.reserve(in.of.list.size);
  for (size_t i = 0; i < in.of.list.size; ++i) {
    const auto& el = in.of.list.data[i];
    if (el.kind != WASMTIME_COMPONENT_U8) {
      return absl::InvalidArgumentError(absl::StrCat(
          "cel_plugin: Lower for `bytes` saw non-u8 element at index ", i,
          " (valkind=", static_cast<int>(el.kind), ")"));
    }
    bytes.push_back(static_cast<char>(el.of.u8));
  }
  *out = Value::Bytes(std::move(bytes));
  return absl::OkStatus();
}

}  // namespace

absl::Status LiftCelToComponent(const CelType& type, const Value& value,
                                const CelComponentContext& ctx,
                                wasmtime_component_val_t* out) {
  switch (type.kind()) {
    case CelType::Kind::kBool:
      if (value.kind() != Value::Kind::kBool) {
        return CelfnVsValueMismatch(type, value);
      }
      return LiftBool(value, out);
    case CelType::Kind::kInt:
      if (value.kind() != Value::Kind::kInt) {
        return CelfnVsValueMismatch(type, value);
      }
      return LiftInt(value, out);
    case CelType::Kind::kUint:
      if (value.kind() != Value::Kind::kUint) {
        return CelfnVsValueMismatch(type, value);
      }
      return LiftUint(value, out);
    case CelType::Kind::kDouble:
      if (value.kind() != Value::Kind::kDouble) {
        return CelfnVsValueMismatch(type, value);
      }
      return LiftDouble(value, out);
    case CelType::Kind::kNull:
      if (!value.IsNull()) return CelfnVsValueMismatch(type, value);
      // Bypass the broken LiftNull dispatch above — write directly.
      out->kind = WASMTIME_COMPONENT_OPTION;
      out->of.option = nullptr;
      return absl::OkStatus();
    case CelType::Kind::kString:
      if (value.kind() != Value::Kind::kString) {
        return CelfnVsValueMismatch(type, value);
      }
      return LiftString(value, out);
    case CelType::Kind::kBytes:
      if (value.kind() != Value::Kind::kBytes) {
        return CelfnVsValueMismatch(type, value);
      }
      return LiftBytes(value, out);
    case CelType::Kind::kDuration:
      if (value.kind() != Value::Kind::kDuration) {
        return CelfnVsValueMismatch(type, value);
      }
      return LiftDuration(value, out);
    case CelType::Kind::kTimestamp:
      if (value.kind() != Value::Kind::kTimestamp) {
        return CelfnVsValueMismatch(type, value);
      }
      return LiftTimestamp(value, out);
    case CelType::Kind::kList:
      return LiftList(type, value, ctx, out);
    case CelType::Kind::kMap:
      return LiftMap(type, value, ctx, out);
    case CelType::Kind::kOptional:
      // Dropped from v1 (user direction 2026-06-03): plugin
      // fns may not declare `optional<T>` arg / return shapes.  The
      // wire `option<unit>` used for CEL `null` is encoded inside the
      // null arm (above); a kOptional decl type is the
      // embedder asking for a feature this regime doesn't provide.
      return absl::InvalidArgumentError(
          "cel_plugin: optional<T> is not a supported argument or "
          "return shape for plugin fns (m24 v1)");
    case CelType::Kind::kMessage:
      return LiftProto(type, value, out);
    case CelType::Kind::kUnknown:
      // A Builder-finalised decl can never carry kUnknown (ArgkindSlug
      // CHECKs at Add* time); fall through to the invariant-break
      // return below.
      break;
    case CelType::Kind::kType:
      if (value.kind() != Value::Kind::kType) {
        return CelfnVsValueMismatch(type, value);
      }
      // CEL `type` lifts as WIT string carrying the type-name.  Reuses
      // LiftString via a synthetic Value::String copy of the type-name
      // — the wire shape is identical.
      {
        auto name = value.AsType();
        if (!name.ok()) return name.status();
        out->kind = WASMTIME_COMPONENT_STRING;
        wasm_byte_vec_new(&out->of.string, name->size(), name->data());
        return absl::OkStatus();
      }
  }
  // Closed-set switch: any unhandled value is an invariant break, not
  // a recoverable input — match the codebase's unreachable-default rule.
  return absl::InternalError(absl::StrCat(
      "cel_plugin: unhandled CelType::Kind = ", static_cast<int>(type.kind())));
}

absl::Status LowerComponentToCel(const CelType& type,
                                 const wasmtime_component_val_t& in,
                                 const CelComponentContext& ctx, Value* out) {
  switch (type.kind()) {
    case CelType::Kind::kBool:
      return LowerBool(type, in, out);
    case CelType::Kind::kInt:
      return LowerInt(type, in, out);
    case CelType::Kind::kUint:
      return LowerUint(type, in, out);
    case CelType::Kind::kDouble:
      return LowerDouble(type, in, out);
    case CelType::Kind::kNull:
      return LowerNull(type, in, out);
    case CelType::Kind::kString:
      return LowerString(type, in, out);
    case CelType::Kind::kBytes:
      return LowerBytes(type, in, out);
    case CelType::Kind::kDuration:
      return LowerDuration(type, in, out);
    case CelType::Kind::kTimestamp:
      return LowerTimestamp(type, in, out);
    case CelType::Kind::kList:
      return LowerList(type, in, ctx, out);
    case CelType::Kind::kMap:
      return LowerMap(type, in, ctx, out);
    case CelType::Kind::kOptional:
      return absl::InvalidArgumentError(
          "cel_plugin: optional<T> is not a supported return shape "
          "for plugin fns");
    case CelType::Kind::kMessage:
      return LowerProto(type, in, ctx, out);
    case CelType::Kind::kType:
      return absl::UnimplementedError(
          "cel_plugin: type-of-types is not a supported return shape "
          "for plugin fns (cleanup-backlog #44)");
    case CelType::Kind::kUnknown:
      // See the Lift-side kUnknown arm — invariant break below.
      break;
  }
  return absl::InternalError(absl::StrCat(
      "cel_plugin: unhandled CelType::Kind = ", static_cast<int>(type.kind())));
}

}  // namespace celwasm
