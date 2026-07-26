// M12 — format directive parser + per-Eval dispatcher.
//
// The parser walks `fmt` once and produces a `DirectiveOp[]`.  Each
// `%` starts a directive; everything else accumulates into a literal
// run.  Literal runs are coalesced — every contiguous non-`%` byte
// span becomes a single `kLiteral` op, NOT one op per byte.  `%%`
// resolves to a single literal `%` byte (the second `%`); the
// leading `%` byte is consumed but not emitted, so the coalescer
// can't span the gap.
//
// Diagnostic strings mirror cel-cpp's `extensions/formatting.cc`
// (`ParsePrecision` / `Format` / `ParseAndFormatClause`) so error
// messages are line-for-line equivalent — keeps conformance failure
// output diff-clean against upstream.
//
// Cache: a single-slot most-recent-format cache (per-Instance
// module-static state) avoids reparsing on the common
// `list.exists(x, fmt.format([x.a, x.b]))` workload.  Mirrors
// `cel_matches.cc`'s `CachedInitialized` flag — without it, the
// first call with an empty format string would spuriously hit the
// default-constructed empty string.  The cache stores the result
// status too, so a malformed format string sticks without
// re-reporting.

#include "runtime/cel_string_format.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_list.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_format_internal.h"

namespace celwasm::string_format_internal {

namespace {

// Append a literal byte at `byte_off` to the current op list,
// coalescing with the previous op if it is itself a literal that
// ends at `byte_off`.  This keeps literal runs as one op even when
// they're produced one byte at a time.
void AppendLiteralByte(std::vector<DirectiveOp>* ops, uint32_t byte_off) {
  if (!ops->empty() && ops->back().kind == DirectiveKind::kLiteral &&
      ops->back().byte_off + ops->back().len == byte_off) {
    ++ops->back().len;
    return;
  }
  ops->push_back(
      DirectiveOp{DirectiveKind::kLiteral, byte_off, 1, kPrecisionDefault});
}

// Parse the precision portion of a directive, starting at `fmt[i]`.
// Diagnostic strings track cel-cpp `ParsePrecision`.
absl::StatusOr<std::pair<size_t, int>> ParsePrecision(absl::string_view fmt,
                                                      size_t i) {
  if (i >= fmt.size()) {
    return absl::InvalidArgumentError("unexpected end of format string");
  }
  if (fmt[i] != '.') {
    return std::pair<size_t, int>{i, kPrecisionDefault};
  }
  const size_t digits_begin = i + 1;
  size_t j = digits_begin;
  while (j < fmt.size() && absl::ascii_isdigit(fmt[j])) {
    ++j;
  }
  if (j == digits_begin || j == fmt.size()) {
    return absl::InvalidArgumentError(
        "unable to find end of precision specifier");
  }
  int precision = 0;
  if (!absl::SimpleAtoi(fmt.substr(digits_begin, j - digits_begin),
                        &precision)) {
    return absl::InvalidArgumentError(
        "unable to convert precision specifier to integer");
  }
  if (precision > kMaxPrecision) {
    return absl::InvalidArgumentError(
        absl::StrCat("precision specifier exceeds maximum of ", kMaxPrecision));
  }
  return std::pair<size_t, int>{j, precision};
}

DirectiveKind ClassifyType(char c) {
  switch (c) {
    case 's':
      return DirectiveKind::kSubstring;
    case 'd':
      return DirectiveKind::kDecimal;
    case 'f':
      return DirectiveKind::kFixed;
    case 'e':
      return DirectiveKind::kScientific;
    case 'b':
      return DirectiveKind::kBinary;
    case 'o':
      return DirectiveKind::kOctal;
    case 'x':
      return DirectiveKind::kHexLower;
    case 'X':
      return DirectiveKind::kHexUpper;
    default:
      return DirectiveKind::kLiteral;
  }
}

absl::StatusOr<std::pair<size_t, DirectiveOp>> ParseDirective(
    absl::string_view fmt, size_t start) {
  auto precision_res = ParsePrecision(fmt, start);
  if (!precision_res.ok()) return precision_res.status();
  auto [type_idx, precision] = *precision_res;
  const char type_byte = fmt[type_idx];
  const DirectiveKind kind = ClassifyType(type_byte);
  if (kind == DirectiveKind::kLiteral) {
    return absl::InvalidArgumentError(absl::StrCat(
        "unrecognized formatting clause \"", std::string(1, type_byte), "\""));
  }
  DirectiveOp op{};
  op.kind = kind;
  op.byte_off = static_cast<uint32_t>(start - 1);
  op.len = static_cast<uint32_t>(type_idx + 1 - (start - 1));
  op.precision =
      (kind == DirectiveKind::kFixed || kind == DirectiveKind::kScientific)
          ? precision
          : kPrecisionDefault;
  return std::pair<size_t, DirectiveOp>{type_idx + 1, op};
}

}  // namespace

absl::StatusOr<std::vector<DirectiveOp>> ParseFormat(absl::string_view fmt) {
  std::vector<DirectiveOp> ops;
  size_t i = 0;
  while (i < fmt.size()) {
    if (fmt[i] != '%') {
      AppendLiteralByte(&ops, static_cast<uint32_t>(i));
      ++i;
      continue;
    }
    if (i + 1 >= fmt.size()) {
      return absl::InvalidArgumentError("unexpected end of format string");
    }
    if (fmt[i + 1] == '%') {
      AppendLiteralByte(&ops, static_cast<uint32_t>(i + 1));
      i += 2;
      continue;
    }
    auto dir_res = ParseDirective(fmt, i + 1);
    if (!dir_res.ok()) return dir_res.status();
    auto& [next_i, op] = *dir_res;
    ops.push_back(op);
    i = next_i;
  }
  return ops;
}

}  // namespace celwasm::string_format_internal

// ───────────────────────────────────────────────────────────────
// Per-Instance most-recent-format cache.  Same shape as
// `cel_matches.cc`'s pattern cache.  `cached_initialized`
// distinguishes "no format has been cached yet" from "the empty
// format is cached" — without the flag, the first call with an
// empty format would spuriously hit and poison `out` via the cached
// (empty) ops vector.
// ───────────────────────────────────────────────────────────────

namespace {

using ::celwasm::string_format_internal::DirectiveKind;
using ::celwasm::string_format_internal::DirectiveOp;
using ::celwasm::string_format_internal::ParseFormat;
using ::celwasm::string_format_internal::RenderBinary;
using ::celwasm::string_format_internal::RenderDecimal;
using ::celwasm::string_format_internal::RenderFixed;
using ::celwasm::string_format_internal::RenderHex;
using ::celwasm::string_format_internal::RenderOctal;
using ::celwasm::string_format_internal::RenderScientific;
using ::celwasm::string_format_internal::RenderString;

bool& CachedInitialized() {
  static bool b = false;
  return b;
}
std::string& CachedFormat() {
  static auto* s = new std::string();
  return *s;
}
std::vector<DirectiveOp>& CachedOps() {
  static auto* v = new std::vector<DirectiveOp>();
  return *v;
}
bool& CachedParseOk() {
  static bool b = false;
  return b;
}

inline void Poison(CelValue* out, uint32_t err_code) {
  out->kind = CEL_ERROR;
  out->payload.err = err_code;
}

inline bool Absorb3vlUnary(CelValue* out, const CelValue* in) {
  if (in->kind == CEL_ERROR || in->kind == CEL_UNKNOWN) {
    *out = *in;
    return true;
  }
  return false;
}

inline absl::string_view BorrowSpan(const CelSpan& s) {
  return {reinterpret_cast<const char*>(cel_mem_base()) + s.ptr, s.len};
}

// Run a single directive against `arg`, appending to `buf`.
// Returns false on kind-mismatch (caller poisons out).
bool DispatchDirective(std::string& buf, const DirectiveOp& op,
                       const CelValue* arg) {
  switch (op.kind) {
    case DirectiveKind::kSubstring:
      return RenderString(buf, arg);
    case DirectiveKind::kDecimal:
      return RenderDecimal(buf, arg);
    case DirectiveKind::kFixed:
      return RenderFixed(buf, arg, op.precision);
    case DirectiveKind::kScientific:
      return RenderScientific(buf, arg, op.precision);
    case DirectiveKind::kBinary:
      return RenderBinary(buf, arg);
    case DirectiveKind::kOctal:
      return RenderOctal(buf, arg);
    case DirectiveKind::kHexLower:
      return RenderHex(buf, arg, /*upper=*/false);
    case DirectiveKind::kHexUpper:
      return RenderHex(buf, arg, /*upper=*/true);
    case DirectiveKind::kLiteral:
      // Caller routes literal ops via the byte-range copy path
      // rather than dispatching here; reaching this arm means a
      // bug in the dispatcher loop.
      ABSL_CHECK(false) << "DispatchDirective called on kLiteral op";
      return false;
  }
  return false;
}

// Copy a literal byte range of the format string to `buf`.  The
// source view is re-derived from `fmt_span` on every call: rendering
// an aggregate argument can lift a host-backed operand into the
// arena, and `arena_alloc` may `memory.grow` and relocate the
// linear-memory base out from under a cached `string_view`.
void EmitLiteral(std::string& buf, const CelSpan& fmt_span,
                 const DirectiveOp& op) {
  buf.append(BorrowSpan(fmt_span).data() + op.byte_off, op.len);
}

// Element count / element slot of an arena list, addressed by header
// OFFSET rather than pointer so both survive a base relocation.
// A zero header offset is the header-less empty shape (offset 0 is
// never a real allocation) — count 0, never dereferenced.
uint32_t ArenaListCountAt(uint32_t header_ptr) {
  if (header_ptr == 0) return 0;
  return reinterpret_cast<const ArenaListHeader*>(cel_mem_base() + header_ptr)
      ->count;
}

uint32_t ArenaListElementSlot(uint32_t header_ptr, uint32_t i) {
  const auto* hdr =
      reinterpret_cast<const ArenaListHeader*>(cel_mem_base() + header_ptr);
  return hdr->elements_offset +
         (static_cast<uint32_t>(kCelListEntryStride) * i);
}

// Walk `ops` × args list to produce the formatted output in `buf`.
// Returns true on success; on any mismatch (arg-count, kind), sets
// `*err` to a non-zero CEL error code and returns false.
//
// Every linear-memory reference is held as an OFFSET and re-derived
// per iteration — `%s` on an aggregate argument can allocate (see
// EmitLiteral above).
//
// Arg-level ERROR / UNKNOWN absorb is handled upstream by the
// kernel's pre-scan (see `cel_string_format_at_vv`), so this loop
// can assume every dispatched arg is a regular value.
bool RunFormat(std::string& buf, const CelSpan& fmt_span,
               const std::vector<DirectiveOp>& ops, uint32_t args_header_ptr,
               uint32_t* err) {
  uint32_t arg_index = 0;
  for (const auto& op : ops) {
    if (op.kind == DirectiveKind::kLiteral) {
      EmitLiteral(buf, fmt_span, op);
      continue;
    }
    if (arg_index >= ArenaListCountAt(args_header_ptr)) {
      *err = CEL_ERR_INVALID_ARGUMENT;
      return false;
    }
    const uint32_t arg_slot =
        ArenaListElementSlot(args_header_ptr, arg_index++);
    if (!DispatchDirective(buf, op, cel_value_at(arg_slot))) {
      *err = CEL_ERR_INVALID_ARGUMENT;
      return false;
    }
  }
  return true;
}

// Refresh the cache on a miss.  Returns true if the cached parse
// succeeded; false if the parse errored (the caller poisons `out`
// with CEL_ERR_INVALID_ARGUMENT).
bool RefreshCache(absl::string_view fmt) {
  if (CachedInitialized() && CachedFormat() == fmt) return CachedParseOk();
  auto res = ParseFormat(fmt);
  if (!res.ok()) {
    CachedOps().clear();
    CachedParseOk() = false;
  } else {
    CachedOps() = std::move(*res);
    CachedParseOk() = true;
  }
  CachedFormat().assign(fmt);
  CachedInitialized() = true;
  return CachedParseOk();
}

// Copy `buf` into the arena and write CEL_STRING into `out_slot`.
// Takes the SLOT, not a pointer: `arena_alloc` may relocate the
// linear-memory base, so the destination pointer is derived after.
void CommitResult(uint32_t out_slot, const std::string& buf) {
  if (buf.empty()) {
    CelValue* out = cel_value_at(out_slot);
    out->kind = CEL_STRING;
    out->payload.s.ptr = 0;
    out->payload.s.len = 0;
    return;
  }
  const uint32_t off = arena_alloc(static_cast<uint32_t>(buf.size()));
  if (off == 0) {
    Poison(cel_value_at(out_slot), CEL_ERR_OVERFLOW);
    return;
  }
  std::memcpy(cel_mem_base() + off, buf.data(), buf.size());
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_STRING;
  out->payload.s.ptr = off;
  out->payload.s.len = static_cast<uint32_t>(buf.size());
}

// Resolve the args operand to an arena list header offset, lifting a
// host-backed args list (`"%s".format(boundList)`) into the arena via
// `cel_list_arena_view` — the same normalisation `list.join()` and
// the comprehension prologue take.  Returns false after poisoning
// `out_slot` when the operand is not list-shaped.
bool ResolveArgsHeader(uint32_t out_slot, uint32_t args_slot,
                       uint32_t* header_ptr) {
  const uint32_t kind = cel_value_at(args_slot)->kind;
  if (kind != CEL_LIST_ARENA && kind != CEL_LIST_HOST) {
    Poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
    return false;
  }
  const uint32_t view_slot = cel_list_arena_view(args_slot);
  *header_ptr = cel_value_at(view_slot)->payload.arena_list.header_ptr;
  return true;
}

}  // namespace

extern "C" void cel_string_format_at_vv(uint32_t out_slot, uint32_t s_slot,
                                        uint32_t args_slot) {
  if (Absorb3vlUnary(cel_value_at(out_slot), cel_value_at(s_slot))) return;
  if (Absorb3vlUnary(cel_value_at(out_slot), cel_value_at(args_slot))) return;
  if (cel_value_at(s_slot)->kind != CEL_STRING) {
    Poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Normalise args origin FIRST: the lift allocates, and everything
  // below holds linear-memory references.
  uint32_t args_header_ptr = 0;
  if (!ResolveArgsHeader(out_slot, args_slot, &args_header_ptr)) return;
  const CelSpan fmt_span = cel_value_at(s_slot)->payload.s;
  if (!RefreshCache(BorrowSpan(fmt_span))) {
    Poison(cel_value_at(out_slot), CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  // Pre-scan args for ERROR / UNKNOWN; cel-cpp's Format
  // short-circuits on the first ERROR / UNKNOWN argument it
  // dispatches, but spec semantics are "any arg-level ERROR /
  // UNKNOWN absorbs".  We absorb upfront so the cache hit on a
  // failed parse doesn't dominate the absorb path.
  for (uint32_t k = 0; k < ArenaListCountAt(args_header_ptr); ++k) {
    const CelValue* a = cel_value_at(ArenaListElementSlot(args_header_ptr, k));
    if (a->kind == CEL_ERROR || a->kind == CEL_UNKNOWN) {
      const CelValue absorbed = *a;
      *cel_value_at(out_slot) = absorbed;
      return;
    }
  }
  std::string buf;
  buf.reserve(fmt_span.len);
  uint32_t err = 0;
  if (!RunFormat(buf, fmt_span, CachedOps(), args_header_ptr, &err)) {
    Poison(cel_value_at(out_slot), err != 0 ? err : CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  // cel-cpp doesn't error on extra args (it silently ignores
  // unconsumed list tail).  Mirror that — no check on
  // `arg_index < args count` after the loop.
  CommitResult(out_slot, buf);
}
