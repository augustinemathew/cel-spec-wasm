// HostCallContext — the typed, kind-checked face of one `@host`
// function invocation.  It turns the raw 24-byte CelValue slot ABI a
// host callback sees (`runtime/cel_data.h`) into C++ accessors that
// validate the slot kind before touching the tagged-union payload and
// return `absl::StatusOr<T>` so a kind mismatch surfaces as an error
// status, never a garbage / out-of-bounds read.
//
// Design: `doc/implementation-plan/rewrite/m21-host-call-adapter.md`.
//
// A callback registered via `Engine::AddFunction` receives a
// `HostCallContext&`; the engine trampoline builds it (over the
// per-Eval linear memory, externref table, and arena allocator) and
// auto-propagates any unknown / error argument BEFORE invoking the
// callback — so a callback body only ever sees all-known args.  To
// *emit* an unknown a function calls `ReturnUnknown()` (see
// `kFunctionUnknownSentinel` in `eval/attribute.h`).
//
// Accessors are bounds- and kind-checked:
//   - arg index out of `[0, NumArgs())`  → OutOfRange
//   - slot kind != the accessor's kind   → InvalidArgument
//   - dangling externref slot            → FailedPrecondition
//
// `HostListView` / `HostMapView` read elements lazily (no eager decode
// of the whole aggregate); `ArgValue` is the eager escape hatch that
// returns the fully-decoded `Value` tree.

#ifndef CELWASM_EVAL_HOST_CALL_CONTEXT_H_
#define CELWASM_EVAL_HOST_CALL_CONTEXT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "eval/value.h"

namespace google::protobuf {
class Message;
}  // namespace google::protobuf

namespace celwasm {

// Trampoline primitives — full definitions in `eval/internal/cel_host.h`.
// Forward-declared here so the public header doesn't pull the internal
// surface into every includer.
class MemoryView;
class ExternrefTable;
class ArenaAllocator;
class HostListBacking;
class HostMapBacking;

// Lazy view over a CEL list argument — either an externref-backed
// `HostListBacking` (`CEL_LIST_HOST`) or an arena-resident header
// (`CEL_LIST_ARENA`), uniformly.  `At(i)` decodes one element on
// demand; the view is valid only for the callback's duration.
class HostListView {
 public:
  size_t Size() const;
  // Index in `[0, Size())`.  Out-of-range → the spec index-out-of-bounds
  // error Value (host backing) / InvalidArgument (arena).  A nested
  // message / list / map element decodes recursively.
  absl::StatusOr<Value> At(size_t index) const;

 private:
  friend class HostCallContext;
  explicit HostListView(const HostListBacking* absl_nonnull backing)
      : backing_(backing) {}
  HostListView(const MemoryView* absl_nonnull mem,
               const ExternrefTable* absl_nonnull refs, uint32_t count,
               uint32_t elements_offset)
      : mem_(mem),
        refs_(refs),
        count_(count),
        elements_offset_(elements_offset) {}

  // Exactly one representation is live: `backing_ != nullptr` ⇒ host;
  // otherwise arena (uses `mem_` / `refs_` / `count_` /
  // `elements_offset_`).
  const HostListBacking* absl_nullable backing_ = nullptr;
  const MemoryView* absl_nullable mem_ = nullptr;
  const ExternrefTable* absl_nullable refs_ = nullptr;
  uint32_t count_ = 0;
  uint32_t elements_offset_ = 0;
};

// Lazy view over a CEL map argument — externref-backed (`CEL_MAP_HOST`)
// or arena-resident (`CEL_MAP_ARENA`), uniformly.  Keys are compared
// per langdef map-key equality (cross-type numeric for int/uint,
// structural for bool/string).
class HostMapView {
 public:
  size_t Size() const;
  // Missing key → the spec no-such-key error Value.  A nested aggregate
  // value decodes recursively.
  absl::StatusOr<Value> Get(const Value& key) const;
  bool ContainsKey(const Value& key) const;

 private:
  friend class HostCallContext;
  explicit HostMapView(const HostMapBacking* absl_nonnull backing)
      : backing_(backing) {}
  HostMapView(const MemoryView* absl_nonnull mem,
              const ExternrefTable* absl_nonnull refs, uint32_t count,
              uint32_t entries_offset)
      : mem_(mem), refs_(refs), count_(count), entries_offset_(entries_offset) {}

  const HostMapBacking* absl_nullable backing_ = nullptr;
  const MemoryView* absl_nullable mem_ = nullptr;
  const ExternrefTable* absl_nullable refs_ = nullptr;
  uint32_t count_ = 0;
  uint32_t entries_offset_ = 0;
};

class HostCallContext {
 public:
  // Constructed by the engine trampoline (and by tests over fake
  // primitives).  Not embedder-constructible in practice: `MemoryView`
  // / `ExternrefTable` / `ArenaAllocator` live in an internal-visibility
  // header, so an external consumer cannot obtain the references.
  HostCallContext(MemoryView& mem, ExternrefTable& refs, ArenaAllocator& arena,
                  uint32_t out_slot, absl::Span<const uint32_t> arg_slots)
      : mem_(mem),
        refs_(refs),
        arena_(arena),
        out_slot_(out_slot),
        arg_slots_(arg_slots) {}

  HostCallContext(const HostCallContext&) = delete;
  HostCallContext& operator=(const HostCallContext&) = delete;

  // ── arity ──
  int NumArgs() const {
    return static_cast<int>(arg_slots_.size());
  }

  // ── scalar args ──
  absl::StatusOr<bool> ArgBool(int i) const;
  absl::StatusOr<int64_t> ArgInt(int i) const;
  absl::StatusOr<uint64_t> ArgUint(int i) const;
  absl::StatusOr<double> ArgDouble(int i) const;

  // ── string / bytes args — slice straight into linear memory ──
  absl::StatusOr<absl::string_view> ArgString(int i) const;
  absl::StatusOr<absl::string_view> ArgBytes(int i) const;

  // ── temporal args ──
  absl::StatusOr<absl::Duration> ArgDuration(int i) const;
  absl::StatusOr<absl::Time> ArgTimestamp(int i) const;

  // ── null ──
  // True iff arg `i` is a present `null` value.  Returns false (not an
  // error) for an out-of-range index or a non-null kind.
  bool ArgIsNull(int i) const;

  // NOTE: there are no ArgIsUnknown / ArgIsError accessors.  Unknown /
  // error args are auto-propagated by the trampoline BEFORE the callback
  // runs, so a callback body only ever sees all-known args.  To *emit*
  // an unknown, see ReturnUnknown below.

  // ── proto arg — resolves msg_slot via the externref table ──
  // Returns the live host-interned message (zero-copy); valid for the
  // callback's duration.  Non-message kind → InvalidArgument; dangling
  // slot / non-proto backing → FailedPrecondition.
  absl::StatusOr<const google::protobuf::Message*> ArgProto(int i) const;

  // ── aggregate args — arena- OR externref-backed, uniformly ──
  absl::StatusOr<HostListView> ArgList(int i) const;
  absl::StatusOr<HostMapView> ArgMap(int i) const;

  // ── escape hatch: the fully-decoded Value (any kind) ──
  absl::StatusOr<Value> ArgValue(int i) const;

  // ── return setters — write the out_slot ──
  // Scalars / null / duration / timestamp encode inline; string / bytes
  // arena-allocate a copy; message / list / map intern a backing into
  // the externref table and write the CEL_*_HOST handle.
  ABSL_MUST_USE_RESULT absl::Status ReturnBool(bool v);
  ABSL_MUST_USE_RESULT absl::Status ReturnInt(int64_t v);
  ABSL_MUST_USE_RESULT absl::Status ReturnUint(uint64_t v);
  ABSL_MUST_USE_RESULT absl::Status ReturnDouble(double v);
  ABSL_MUST_USE_RESULT absl::Status ReturnString(absl::string_view v);
  ABSL_MUST_USE_RESULT absl::Status ReturnBytes(absl::string_view v);
  ABSL_MUST_USE_RESULT absl::Status ReturnDuration(absl::Duration v);
  ABSL_MUST_USE_RESULT absl::Status ReturnTimestamp(absl::Time v);
  ABSL_MUST_USE_RESULT absl::Status ReturnNull();
  // Proto: OWNING — the backing must outlive the call (it lives in the
  // per-eval externref table until Reset()).  Borrowing a stack/temp
  // message would dangle.
  ABSL_MUST_USE_RESULT absl::Status ReturnProto(
      std::unique_ptr<google::protobuf::Message> m);
  ABSL_MUST_USE_RESULT absl::Status ReturnList(absl::Span<const Value> elems);
  ABSL_MUST_USE_RESULT absl::Status ReturnMap(
      absl::Span<const std::pair<Value, Value>> entries);
  // Explicit unknown / error RETURN (3VL).  ReturnUnknown stamps
  // {CEL_UNKNOWN, payload.unk = kFunctionUnknownSentinel}, marking the
  // unknown as function-returned (distinct from a propagated input
  // unknown, which carries a real attribute id).
  ABSL_MUST_USE_RESULT absl::Status ReturnUnknown();
  ABSL_MUST_USE_RESULT absl::Status ReturnError(ErrorPayload payload);
  // Any kind, including Unknown (the attribute id is preserved verbatim,
  // so a function-origin sentinel round-trips unchanged).
  ABSL_MUST_USE_RESULT absl::Status ReturnValue(const Value& v);

 private:
  MemoryView& mem_;
  ExternrefTable& refs_;
  ArenaAllocator& arena_;
  uint32_t out_slot_;
  absl::Span<const uint32_t> arg_slots_;
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_HOST_CALL_CONTEXT_H_
